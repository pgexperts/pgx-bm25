---
id: 0117
title: The orphan sweep holds the singleton in ShareLock, and a per-page epoch rule keeps it off pages an appender owns
date: 2026-10-05
status: Accepted
supersedes: 0019
summary: bm25_reclaim_orphans takes the seal/merge singleton in ShareLock, which still excludes every builder and swapper, and under each page's EXCLUSIVE lock skips an unreachable PENDING page whose epoch is 0 or at least min(next_gen, head epoch, tail epoch).
---

# 0117. The orphan sweep holds the singleton in ShareLock, and a per-page epoch rule keeps it off pages an appender owns

## Context

ADR 0019 made the orphan sweep take the singleton in ExclusiveLock for its whole duration.
Its argument was that "unreachable" is a safe proxy for "orphan" only while nothing is
building or swapping: a segment under construction is unreachable pages until its publish
record commits, `chain_flush` releases each page before allocating the next, and a merge
swap landing between the sweep's metapage read and its catalog read would leave it marking
the old chain while enumerating the new one. That argument stands, and it is carried here.
What no longer holds is the lock mode. An Exclusive hold across an O(index) pass blocks every
document-adding INSERT (ADR 0022) and gets an autovacuum cancelled after `deadlock_timeout`
(issue #300; ADR 0116). Gating the sweep (ADR 0116) makes it rarer, but it still runs after
every restart and every dead maintenance op. On a large insert-busy table an exclusive sweep
would then be cancelled by every autovacuum attempt and its evidence would never clear: each
cancelled autovacuum loses its heap work, so the starvation loop survives, only rarer to
enter and impossible to leave until a quiet window or an uncancellable anti-wraparound run.
The issue's own success test puts the index in the sweep-needed state first, so that gating
alone cannot pass it.

Shared mode excludes every seal, merge, swap, upgrade rewrite and reclaim, which all take
the singleton Exclusive, and so excludes everything ADR 0019 needed to exclude. What it
admits is pending appenders, which hold it in ShareLock too. The issue named the binding
hazard: an appender can pop an FSM page that is `DELETED` and unreachable in the sweep's
snapshot, re-initialize it and link it, and a share-mode sweep would then double-free it or
free it as an orphan.

The user decided on 2026-10-05 to run the sweep in share mode with a per-page rule, and to
keep one singleton (ADR 0118).

## Decision

**ShareLock.** `bm25_reclaim_orphans` takes `LockPage(index, BM25_METAPAGE_BLKNO, ShareLock)`
after the unlocked gate check, reads the extent and the metapage under it, and releases it
at the end. Marking is as before; non-pending structure cannot change under Share.

**The per-page rule.** In the sweep loop each unreachable block is taken under
`BUFFER_LOCK_EXCLUSIVE`, and an unreachable page of kind `PENDING` that is not `DELETED` is
skipped, not freed, when its `seg_gen` is 0 or at least `e_floor`. A zero page, an unknown
kind, and an already-`DELETED` page keep their previous handling.

**`e_floor = min(next_gen, head epoch, tail epoch)`**, read under the singleton from the same
metapage read the marking uses (`bm25_pending_epoch_floor`). Why:
- While the sweep holds the singleton nothing can drain the chain or start a new one, so
  every page an appender adds copies its epoch from the chain's tail: by induction the
  tail's epoch now, or 0 once an older binary has written a 0 page into the chain and its
  successors copy it.
- With no chain, the floor is `next_gen`, which any chain started later draws at or above.
- A head or tail that is not a `PENDING` page means a corrupt metapage; the floor drops to
  0, so no pending page is freed, the conservative answer.

A dead pending page from a drained chain has a smaller nonzero epoch: epochs are drawn from
`next_gen` and bumped per chain (ADR 0110), so an earlier chain's are below every later
chain's. The rule therefore never stamps a page an appender owns, and frees every such page
except a legacy epoch-0 one, which leaks. One other dead page is skipped rather than freed:
a page an older binary's three-record append initialized off the CURRENT chain's tail and
never linked carries that chain's epoch, so it waits until the chain is drained; the
one-record append cannot produce it.

**Why the decision is exact under the page lock.** `bm25_page_alloc` hands the appender its
page EXCLUSIVE-locked and the appender keeps that lock through its one WAL record (ADR
0116), so the sweep sees either the old `DELETED` or zero page, or the finished new page
with an epoch at or above `e_floor`. It never sees a half-written page.

**The harmless half of the race.** The sweep can lock a `DELETED` or zero page between an
appender's `GetFreeIndexPage` pop and its `ConditionalLockBuffer`, and re-record it free just
before it becomes live. That FSM entry is a stale hint, which stamp-and-gate already
tolerates (the FSM is not crash-safe either): `bm25_page_alloc` hands out only `DELETED` or
zero pages and drops anything else it pops, so a later pop of the now-live page is rejected.
`mark_chain` stops at the first block at or past the sampled `nblocks`, so a chain page
appended after an extension can be left unmarked, but it carries an epoch at or above
`e_floor` and is skipped.

**The `epoch == 0` clause, and the tail in the floor, are needed because of the single
singleton.** An older appender holds the singleton in ShareLock too, so it can run beside
the sweep and write an epoch-0 page into an epoch-e chain. Two cases follow. If a 0 page is
already the tail when the sweep starts, the floor drops to 0 through the tail's epoch and
every pending candidate is skipped. If a 0 page is written after the floor was read, it is
below the floor, and only the `page_epoch == 0` clause protects it and the pages that copy
its epoch. A design-stage version whose floor was the head's epoch alone had the first hole:
an older appender adds a 0 page, new appenders copy the tail's 0, land after the mark, carry
`seg_gen 0` below the head's epoch, and are stamped `DELETED`; once the horizon passes they
are reused and committed rows become unfindable. `t/031` covers both cases.

**No exclusive window.** Crash recovery can leave thousands of build orphans to stamp, and
stamping them inside an insert-blocking window would recreate the cancel.

**What stays.** The horizon asymmetry of ADR 0019's 2026-08-16 addendum is unchanged: pending
orphans are stamped with `ReadNextFullTransactionId()` read once outside every buffer lock,
other kinds with `InvalidFullTransactionId`. Catalog pages carry `seg_gen = 0`, so they have no
generation backstop; no production reader walks an old catalog chain into this sweep (issue
#270): `bm25_scan_snapshot` and `bm25_segcat_first_entry` read the catalog under the metapage
SHARE that a swap's EXCLUSIVE excludes, and `bm25_segcat_read_locked`'s callers hold the
singleton, taken after the swap that orphaned the chain they could reach. The unlocked debug
SRFs are the exception.

**Tests.** `t/030_sweep_insert_concurrency.pl` (INSERTs during a manual and an autovacuum
sweep, with no autovacuum cancel); `t/031_sweep_appender_race.pl` (appenders racing the
sweep: the chain exists at the mark, the chain starts after the mark, and an older binary's
epoch-0 page before and after the mark, using the owner-only
`bm25_debug_set_pending_tail_epoch`); pause points `orphan_sweep_start` and
`orphan_sweep_marked`. Without the per-page rule the first two stamp live chain pages; without
the epoch-0 clause the third does; against an Exclusive sweep every post-mark INSERT times out.

## Alternatives considered

- **Keep the Exclusive sweep and only gate it.** Rejected for the starvation loop above.
- **A re-entrant Exclusive window inside the sweep, or chunk releases between mark and
  sweep.** The window is unnecessary given the per-page rule, and stamping crash orphans
  inside it would recreate the cancel. Releases bring back ADR 0019's swap and in-flight
  build hazards.
- **Let seals run during the sweep.** Catalog pages carry `seg_gen = 0`, so a concurrent
  `publish_append` catalog page is indistinguishable from an orphan.
- **A second lock for the sweep.** See ADR 0118.
- **`ConditionalLockPage` in autovacuum.** The issue had already dropped it.

## Consequences

- INSERT never waits for the sweep.
- **Residual: a blocking `bm25_seal()`, `bm25_merge()` or `bm25_upgrade()` arriving during
  the sweep queues for ExclusiveLock, every later appender's ShareLock request then queues
  behind that waiter for the rest of the pass, and the waiter, hard-blocked by an autovacuum
  holder, cancels the autovacuum after `deadlock_timeout`.** The evidence then stays set until
  a sweep completes. This is the ADR 0102 shape. `aminsert`'s opportunistic seal takes the
  singleton conditionally and never queues.
- During the sweep the opportunistic seal skips, so pending can grow by the insert rate times
  the sweep's length, and the next seal, whose hold scales with the pending list, is larger.
- **Residual: legacy epoch-0 pending orphans leak permanently.** They are bounded by
  pre-upgrade truncate crashes. The rule frees every dead pending page except one with epoch 0
  and, until its chain drains, one an older binary left unlinked off the current tail.
- ADR 0019's Exclusive mode is superseded; its reachability argument and its horizon addendum
  are not. ADR 0066's and ADR 0102's autovacuum-cancel trade no longer applies to the sweep.
- A sweep can free a page between an appender's pop and its lock, leaving a stale FSM hint;
  stamp-and-gate rejects it.

## Addendum (2026-10-05, PRs #331-#350)

Test coverage of the singleton the sweep takes (#309 REGR #37, PR #347's A/B): on every CI
leg only `t/030`'s lock-mode assertion ("the parked sweep holds the singleton in
ShareLock") witnesses it. Behaviourally, a seal racing a sweep that took no singleton is caught
only by the cassert `Assert(segcat_singleton_held)` in `bm25_segcat_read_locked`; with that
Assert also removed, `t/029` and `t/031` pass. No new test was added.
