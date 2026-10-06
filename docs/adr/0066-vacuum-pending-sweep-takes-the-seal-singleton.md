---
id: 0066
title: VACUUM's pending sweep takes the seal singleton in ShareLock mode
date: 2026-08-20
status: Accepted
summary: bm25_pending_mark_dead holds LockPage(BM25_METAPAGE_BLKNO, ShareLock) for its whole walk so it cannot interleave with a concurrent drain and split a multi-part document, while still not blocking concurrent appenders, which hold the same mode.
---

# 0066. VACUUM's pending sweep takes the seal singleton in ShareLock mode

## Context

`bm25_pending_drain` takes its per-page `BUFFER_LOCK_SHARE` under the seal's
`LockPage(BM25_METAPAGE_BLKNO, ExclusiveLock)`. That heavyweight singleton — not
the per-page buffer locks — is what makes a page-at-a-time walk add up to a stable
snapshot of the chain.

`bm25_pending_mark_dead`, VACUUM's pending sweep, took only per-page
`BUFFER_LOCK_EXCLUSIVE` and never asked for the singleton. Both walkers go
head-to-tail releasing each page before reading the next, so they can leapfrog on a
multi-part (spanning) document:

- the drain reads P1 SHARE, buffers part 0 of document D with a valid TID, releases P1;
- the sweep takes P1 EXCLUSIVE and invalidates D's part 0, then P2 and invalidates part 1;
- the drain reads P2, sees part 1 with an invalid TID, `continue`s, and later flushes
  D carrying ONLY part 0's tokens.

D is published with a truncated token list and a wrong per-field `doclen`, which
feed wrong `total_len_by_field[]` / `ndocs_by_field[]` into the segment header. D's
heap tuple is dead so it is never returned, but its corpus-statistics contribution
skews every other document's score until a merge rewrites the segment.

The drain's own comment asserted the opposite — that TID-equality invalidation takes
every part of a multi-part document together, so no continuation can be orphaned.
That is true only if the two passes are serialized, which nothing arranged. Issue
#147 (PEND-06).

## Decision

`bm25_pending_mark_dead` takes `LockPage(BM25_METAPAGE_BLKNO, ShareLock)` for its
entire walk, released at the normal exit; every error path leaves by `ereport`,
where transaction abort releases it — the same discipline
`bm25_pending_append_multi` uses.

ShareLock, not ExclusiveLock: it does not conflict with itself, so the sweep does
not serialize against a concurrent appender (which holds the same mode), but it
DOES conflict with the sealer's ExclusiveLock, which is the whole requirement.

## Alternatives considered

- **ExclusiveLock** — would also serialize the sweep against every appender, turning
  an autovacuum pass into a writer stall for the duration of a whole-chain walk.
  Unnecessary: the sweep's per-page buffer locks already exclude concurrent
  same-page writes, and the only ordering that was missing is sweep-vs-drain.
- **Make the drain tolerate a split document** — rejected: the drain cannot
  distinguish "this continuation's parent was swept a moment ago" from "this chain
  is corrupt", so tolerating it means publishing wrong corpus statistics silently,
  which is the defect.
- **Have the sweep skip multi-part documents** — rejected: it would leave dead
  spanning documents in the chain indefinitely, and they are exactly the large
  documents worth reclaiming.

## Consequences

Two behaviour notes, both the prescribed trade rather than defects.

The sweep now holds ShareLock across a cost-throttled walk (ADR 0025 /
`BM25_VACUUM_DELAY_POINT`, adopted for these loops in the same change), so a
blocking `bm25_seal` or `bm25_merge` caller triggers the standard autovacuum
auto-cancel after `deadlock_timeout`. Aborting the sweep is safe: marking is
idempotent and `pending_ndocs` is administrative.

Symmetrically, autovacuum's drain now sleeps while holding the ExclusiveLock, so
concurrent appends can stall up to `deadlock_timeout` before that auto-cancel
fires — a slightly higher autovacuum-cancellation likelihood on insert-heavy
tables.

This property has no single-session observable — a lock that is taken, and a delay
point inert unless `VacuumCostActive`. `sql/91_vacuum_seal_interaction` therefore
asserts liveness (sweep-then-seal within one VACUUM does not self-deadlock, the
singleton is re-acquirable, a manual seal in the same session proves nothing was
leaked) and says in its header that it cannot do more. A two-session TAP test could
pin the lock ACQUISITION semi-deterministically, but not the corruption
interleaving, which needs injection points standard server builds lack.

## Addendum (2026-09-28)

Two things this record stated are superseded by
[0102](0102-bulkdelete-holds-the-singleton-for-its-whole-pass.md).

First, the stall scope: this record gave only VACUUM's *pending* sweep the
seal/merge singleton in ShareLock mode. 0102 extends that same trade to the
whole of `bm25_bulkdelete` — the catalog snapshot, every segment's tombstone
loop, and the pending sweep in one critical section — because a seal or merge
swapping the catalog mid-loop, not just mid-sweep, was found to lose
tombstones and corrupt statistics (issues #239, #241). The "Consequences"
paragraph's autovacuum-auto-cancel and inserter-stall trade-offs described
here now apply to that whole pass, not only the sweep.

Second, the claim that the corruption interleaving "needs injection points
standard server builds lack" is disproved: 0102 adds
`bm25_native.debug_pause`, a `PGC_SUSET` debug GUC that parks a backend at a
named step behind an ordinary `pg_advisory_lock`, and `t/020_vacuum_merge_race.pl`
uses it to drive the interleaving deterministically on stock builds. This
record's own `sql/91_vacuum_seal_interaction` remains a liveness-only test, as
written, but the corruption interleaving it said could not be pinned without
injection points now has a deterministic test elsewhere.

## Addendum (2026-10-04)

The fresh-eyes review of this date found the autovacuum-cancellation risk this record calls a slightly higher likelihood to be systematic on an insert-busy table once a cleanup hold exceeds deadlock_timeout. The deterministic reproduction (per-table cost-delay settings, a 600-document index) isolated the orphan sweep (ADR 0019) as the long holder, not this record's drain, whose hold scales with the pending list rather than the index. Tracked in #300.

## Addendum (2026-10-05)

The autovacuum-cancel trade this record accepted, and the 2026-10-04 addendum's finding that it
is systematic, no longer applies to the orphan sweep (#300). The sweep holds the singleton in
ShareLock and runs only on evidence (ADRs 0116 and 0117), so it is not the long holder. The
trade still applies to the Exclusive holds that remain: the seal, whose hold scales with the
pending list, one reclaim chunk, and a merge pass, with VACUUM's opportunistic merge pass
unbounded (ADR 0118). `bm25_pending_mark_dead` still takes Share and still excludes a drain.
