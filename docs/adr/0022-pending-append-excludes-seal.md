---
id: 0022
title: bm25_pending_append_multi takes the seal singleton in ShareLock mode
date: 2026-07-29
status: Accepted
summary: The pending-list append acquires LockPage(BM25_METAPAGE_BLKNO, ShareLock) so appends and seals genuinely exclude each other, making the publish record's unconditional pending-anchor reset safe for the first time.
---

# 0022. `bm25_pending_append_multi` takes the seal singleton in ShareLock mode

## Context

The seal holds `LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock)` across the
entire drain + build + commit, and `bm25_segment_build_and_commit` resets
`pending_head`/`tail`/`tail_free`/`npages`/`ndocs` **unconditionally** in the
publish record. The comment justifying that reset said no append could land
between the drain snapshot and the publish.

It could. `bm25_pending_append_multi` serialized only on the metapage **buffer
content lock**. `LockPage` is the lmgr heavyweight `LOCKTAG_PAGE` lock and
`LockBuffer` is an LWLock — different lock managers, no conflict between them —
so the singleton did not exclude an appender at all. Worse, the production
`aminsert` path appends *and then* tries `ConditionalLockPage`
(`bm25_build.c`), so the append ran entirely outside the singleton by
construction.

A document appended during a seal was therefore published by nobody, and then
had its page reset to `Invalid` and recycled by `bm25_pending_truncate`:
committed, heap-visible, in no segment, on no chain, and findable through the
index only after a `REINDEX` (review ref C9, issue #40).

The window is not the drain. Appends land on the **tail** page and the drain
walks head → tail, so it reaches the tail last; the exposure is the stretch
between the drain reading the tail and the publish committing — that is, the
segment **build**, which for a large pending list takes seconds.

Reproduced by hand with two concurrent `psql` sessions (40000-document pending
list, explicit `bm25_seal()` in one session, a stream of inserts in the other):
on the pre-fix build it does not merely lose rows, **it crashed the backend** —
reproduced twice. The same workload on the fixed build is clean, twice.

## Decision

`bm25_pending_append_multi` takes `LockPage(index, BM25_METAPAGE_BLKNO,
ShareLock)` before the metapage buffer lock and releases it once the document is
durably on the chain and counted.

`ShareLock`, not `ExclusiveLock`, is the point: it conflicts with the sealer's
`ExclusiveLock`, so appends and seals genuinely exclude each other, while
`ShareLock` does not conflict with itself, so concurrent inserters still append
in parallel. Heavyweight before buffer, as everywhere else in this AM. The lock
goes inside the append function rather than at its call sites so every caller —
`aminsert` and `bm25_debug_pending_append` — is covered.

## Alternatives considered

- **Make the publish reset the anchor to what the drain actually consumed**
  (the report's second option, and closer to GIN's `ginInsertCleanup`
  discipline). Rejected as materially harder to get right: the drain consumes
  whole pages, so a document appended into a **partially drained tail page**
  must be neither re-drained (BM25 scores are additive — the single-record-seal
  invariant says a re-drain double-scores and double-counts global stats) nor
  truncated. Getting that right needs per-document drain granularity, i.e. a new
  protocol, to avoid a per-insert lock. This fix instead makes an invariant the
  code already claimed actually true.
- **Take `ExclusiveLock` on the append side** — correct but serializes every
  concurrent inserter against every other, for no benefit.
- **Leave it and document the loss** — silent permanent data loss from two
  ordinary concurrent INSERTs is not a documentable limitation.

## Consequences

- **An INSERT now blocks for the duration of a concurrent seal.** Previously it
  did not — it proceeded and silently lost the row. This is the correct
  trade and it is a real latency change: sealing a large pending list takes
  seconds, and inserts wait. The opportunistic `aminsert` seal still uses
  `ConditionalLockPage`, so an inserter never *starts* a seal it would block on.
- One lmgr acquire/release per inserted row. Cheap next to the tokenizing the
  same function already does. If it ever appears in an insert-heavy profile, the
  upgrade path is the partial-reset protocol above — noted in a `ponytail:`
  comment at the lock site.
- Three comments that asserted the false invariant are corrected in place
  (`bm25_seal_index`, the publish record, and `bm25_debug_pending_append`, whose
  comment showed the authors believed the `aminsert` path took the singleton —
  it did not).
- Covered by `t/015_pending_append_race.pl`, following this repo's existing
  `background_psql` + `query_until` idiom. It is a stress test, so it is not
  guaranteed to open the window on every CI run; its post-fix assertions are
  exact regardless of interleaving, so it cannot false-fail. Sized (40000-row
  bulk load as one statement, 2000 racer rows) to keep the cassert+UBSan job
  affordable.

## Addendum (2026-09-29, merge and upgrade hold the same lock)

The Consequences above say an INSERT blocks for the duration of a concurrent seal.
The same holds, for their whole hold, for the other writers that take the singleton
in ExclusiveLock mode, because the appender's ShareLock conflicts with every one of
them:

- **Merge, forced or opportunistic.** `bm25_merge_maybe` took the singleton and its
  header comment said inserts continue. They do not: every insert that adds a
  document takes the lock in ShareLock mode, so each merge pass blocks them until it
  finishes. The opportunistic path's `ConditionalLockPage` only keeps the merger from
  queueing behind another holder or waiter; once it wins the lock it holds it for the
  whole pass, and it does not shorten that hold. The comment was corrected in the
  hygiene change for #277.
- **`bm25_upgrade`.** It seals the pending list first, then, when it rewrites
  segments, takes the singleton again inside `bm25_merge_rewrite_all`. `bm25_merge`
  does the same (a seal, then `bm25_merge_maybe(index, true)`). Each call therefore
  holds the lock twice, releasing it between the seal and the second phase, and
  inserts wait during both holds.

The opportunistic seal an `aminsert` triggers is unchanged and still skips rather
than waits. The lock's other consequences (a VACUUM index pass, and cleanup's seal and
reclaims) are in ADR 0102's addendum.

## Addendum (2026-10-05)

The appender's mode is unchanged, but the set of holders it waits for changed (#300).

- The orphan sweep holds the singleton in ShareLock, so an INSERT never waits for it
  (ADR 0117). `bm25_bulkdelete` already held Share.
- What still blocks an insert is an Exclusive holder: a seal (O(pending)), one
  `bm25_reclaim_retired` chunk, one merge pass or upgrade rewrite (ADR 0118). Reclaim no longer
  holds across the whole horizon-cleared list, and a forced merge releases between passes.
  VACUUM's opportunistic merge pass is a single pass and is unbounded.
- The merge entry points open the heap before taking the singleton, the order the seal uses
  (#300).
- The append that needs a fresh page is one WAL record (ADR 0116).
