---
id: 0121
title: On a hot standby the ranking build runs once, without the retry subtransaction
date: 2026-10-05
status: Accepted
summary: In recovery bm25_scan_build_ranking calls the build directly with no internal subtransaction and no retry, so a recovery conflict is core's ordinary 40001 statement cancel instead of FATAL session termination, and bm25's own reuse 40001 reaches the standby client the same way; the primary keeps the bounded subtransaction retry. This replaces ADR 0004's in-scan retry entry for the recovery case.
---

# 0121. On a hot standby the ranking build runs once, without the retry subtransaction

## Context

ADR 0004's "in-scan retry via subtransaction" entry wraps each ranking-build attempt in
`BeginInternalSubTransaction` + `PG_TRY` so the option-(d) `seg_gen` abort (a page reused
under a scan) is retried with a fresh snapshot and never reaches the user. Every ranked
scan, the delegated `@@@` build, the over-pull tail rebuild and `bm25_debug_rank` go
through it.

Core resolves a recovery conflict (lock, snapshot, buffer pin, tablespace) at a
`CHECK_FOR_INTERRUPTS` with a statement ERROR only outside a subtransaction; inside one,
`ProcessRecoveryConflictInterrupt` terminates the session with FATAL, because a
subtransaction could catch the ERROR and carry on with a conflicting snapshot. This holds
on PG17, 18 and 19. The ranking build is dense with interrupt checks, so on a standby any
conflict that landed mid-build killed the client's connection instead of cancelling its
statement (#307 SCAN-02, found by the 2026-10-04 review and recorded on ADR 0004 and 0005
that day).

A fact that constrains every fix: core's conflict cancel is itself SQLSTATE 40001
("canceling statement due to conflict with recovery"). Any scheme that catches 40001 to
retry would also catch core's, which is exactly what core escalates to FATAL to prevent.

## Decision

Decided by the user as D22. In `bm25_scan_build_ranking`, when `RecoveryInProgress()`, call
`bm25_scan_build_ranking_once` directly: no subtransaction, no `PG_TRY`, no retry. A
recovery conflict is then core's ordinary 40001 statement cancel and the session survives,
and bm25's own reuse 40001 reaches the standby client the same way, as the flat `@@@` path
already reported it (ADR 0110). The primary, where no recovery conflict can be delivered,
keeps the bounded three-attempt retry unchanged. Each build decides afresh, so a promotion
mid-scan needs no handling.

Without the subtransaction the dispatcher's scratch contexts (ADR 0036) are children of the
caller's context rather than the subtransaction's. Every branch deletes them on success,
statement abort reclaims them on error, and everything the build keeps (ranked, scores,
keys, `stats_pin`) is in `so->scanctx` either way.

The flat `@@@` retry that ADR 0110 deferred here (XCUT-04) is closed as unneeded: on the
primary a live snapshot's horizon keeps reuse away, and on a standby every path now reports
the abort the same way.

Pause point 15, `rank_build_attempt`, parks a build at the top of each attempt (inside the
subtransaction on a primary). Landed in PR #331 (#307).

## Alternatives considered

- **Return-code unwind** (the issue's proposal, option A). Replace the `ereport` at about 32
  `bm25_seg_page_validate_kind` sites, the epoch-validate sites and the twelve
  `bm25_pending_walk_read` callers (about 45 in all) with a status that each frame
  propagates, releasing its locked buffer and resetting partial scan state. That is the
  WAND scan-state code, a known recurring bug cluster, and its only gain over this
  decision is that a standby's reuse abort stays invisible.
- **A private ResourceOwner plus `PG_TRY` without a subtransaction.** Rejected by ADR 0110
  as unsound: an error caught that way leaves locks, interrupt holdoff and error state
  unrecovered.
- **Catch the error and retry outside a subtransaction.** Would also catch core's
  conflict 40001, which is what core's FATAL exists to prevent.

## Consequences

- A standby session survives a recovery conflict during a ranked query (`t/033`, with
  `max_standby_streaming_delay = 0` and a lock conflict against a parked scan), and an
  ordinary ERROR inside the build leaves the standby session usable.
- **Cost:** with `hot_standby_feedback = off`, a seal-and-reuse race on the primary that
  the retry used to absorb now reaches a standby client as 40001. Standby clients must
  already retry core's own conflict 40001; README recommends `hot_standby_feedback = on`
  and states the behaviour.
- **Residual:** a conflict signalled just before promotion and serviced after it lands in
  a primary-path subtransaction and is escalated as before; the window is the promotion
  itself.
- ADR 0004's in-scan retry entry now describes the primary only (ADR 0004 addendum). The
  ADR 0036 and ADR 0110 addenda of this date record the context parenting and the
  XCUT-04 closure.
