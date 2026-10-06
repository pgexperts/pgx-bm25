---
id: 0118
title: Cleanup runs seal, reclaim, merge, then the sweep, and the reclaim and forced-merge holds are bounded under one singleton
date: 2026-10-05
status: Accepted
summary: VACUUM cleanup runs seal, reclaim_retired, merge_maybe(false), orphan sweep and a whole-map FSM vacuum; the heap lock precedes the singleton; reclaim_retired works one descriptor page per hold and a forced merge releases between passes; a second lock splitting the singleton was rejected.
---

# 0118. Cleanup runs seal, reclaim, merge, then the sweep, and the reclaim and forced-merge holds are bounded under one singleton

## Context

Issue #300 had four parts beyond gating the sweep (ADR 0116, ADR 0117), landed as part A (the
reorder and two small fixes) and part B (the holds).

**Order (part A).** `bm25_vacuumcleanup` ran seal, orphan sweep, `merge_maybe(false)`, then
`bm25_reclaim_retired`, each under the singleton. The sweep is the one pass whose hold scales
with the whole index on every VACUUM, so it is the one most likely to be cancelled, by the
user or, for an autovacuum, by an inserter's deadlock check after `deadlock_timeout`. A
cancelled sweep also skipped the merge and the retired reclaim, which are the only automatic
merge and the only automatic retired reclaim. On an insert-busy table whose autovacuum kept
being cancelled, segments and retired-but-unfreed ranges grew without bound.

**Heap lock order (XCUT-11).** `bm25_merge_maybe` and `bm25_merge_rewrite_all` took the
singleton and then opened the heap. `bm25_merge`'s seal drops its heap lock on the way out,
so a DDL lock queued on the heap in that gap left the merge holding the singleton while it
waited. Every other singleton taker opens the heap first.

**FSM vacuum (PEND-07).** `bm25_pending_truncate` ended with `IndexFreeSpaceMapVacuum`, a
vacuum of the whole map, under the singleton on the insert path.

**Holds (part B).** After gating and share mode, two maintenance paths still held the
singleton in ExclusiveLock across work that grows with the index. `bm25_reclaim_retired`
freed every horizon-cleared retired range in one hold. A forced `bm25_merge_maybe` ran its
whole ladder in one hold. Every document-adding INSERT waits for each (ADR 0022). The seal's
hold scales with the pending list, and VACUUM's single opportunistic merge pass holds for
its inputs.

## Decision

**Cleanup order.** seal, `bm25_reclaim_retired`, `bm25_merge_maybe(info->index, false)`,
`bm25_reclaim_orphans` (gated, ADR 0116), then `IndexFreeSpaceMapVacuum` on every run. Each
pass still takes and releases the singleton itself, so the sweep still runs with nothing
building or swapping (ADR 0019's argument, carried by ADR 0117). Every interleaving the new
order produces was already reachable across two VACUUMs, or a `bm25_merge()` followed by a
VACUUM. Pages `reclaim_retired` frees are stamped `DELETED` before they reach the FSM, so a
later sweep finds them unreachable and re-records them free without re-stamping; one an
appender re-allocated in between is either linked into the pending chain before the sweep
marks, or a `PENDING` page carrying the chain's epoch, which the sweep's per-page rule never
stamps. A merge's freshly retired segments are on the retired list when the sweep marks, so
their range marking keeps them out of the free set. The reclaim runs before the merge so a
cancelled merge cannot starve it, and so the merge can reuse what it frees. The cost runs
the other way: an ERROR in `reclaim_retired` or the merge now also skips this VACUUM's
sweep, where it used to skip only what followed it. The sweep is idempotent and its orphans
wait for the next VACUUM, and the two earlier passes are bounded by the retired list and the
merge inputs, the sweep by the whole index. The merge also runs before the sweep so the same
cleanup frees the catalog chain a merge's swap orphans (ADR 0116).

**Heap before singleton.** `bm25_merge_maybe` and `bm25_merge_rewrite_all` open the heap
first.

**Truncate vacuums a range.** `bm25_pending_truncate` calls `FreeSpaceMapVacuumRange` over the
blocks it freed. The whole-map vacuum moved to the end of every cleanup (ADR 0116).

**`bm25_reclaim_retired` works in chunks of one descriptor page.** A chunk takes the
singleton, re-reads the metapage and walks from `retired_head` to the first page with
something to drop or unlink, compacts it, frees every range it dropped and, if the page
emptied, splices and frees it, then releases. The wait an INSERT sees is one descriptor
page's ranges, which is one merge's inputs, not the whole horizon-cleared list.
- The release comes only after the chunk's frees. The descriptor is compacted before its
  ranges are freed (ADR 0032), so a release in between would leave ranges no descriptor
  lists, which a concurrent sweep may free, an allocator re-initialize, and the resumed
  `reclaim_one_range` (no `seg_gen` check) free again while live.
- Every chunk restarts from `retired_head` and carries no `prev_blk` across the gap, because
  a swap in the gap prepends a new head and a stale predecessor would splice the wrong page.
- Each chunk carries its own orphan bracket, opened lazily before the first compaction
  (ADR 0116). The FSM vacuum runs once, after the last release.

**A forced `bm25_merge_maybe` releases the singleton between ladder passes.** Each pass has
already swapped, and the next re-reads the metapage and the catalog. The heap stays open, so
the order stays heap then singleton. A `bm25_bulkdelete` that takes its whole-pass
ShareLock (ADR 0102) in the gap is waited for on re-acquire, so it never sees a swap
mid-pass. The merge's trailing `bm25_reclaim_retired` runs after the release, so its chunks
are not nested inside the merge's hold.

**One singleton.** The user decided on 2026-10-05 to keep the single metapage singleton and
bound the holds by chunking and releasing. The lock split below was rejected.

**XCUT-08 is not done.** The five bare `CHECK_FOR_INTERRUPTS` sites on the maintenance path
that could be `BM25_VACUUM_DELAY_POINT()` stay as they are: `bm25_fsm.c` (two),
`bm25_merge.c` (two) and `bm25_seg_build.c` (one). The issue said to land throttling with or
after hold-bounding and never before, because throttling a loop lengthens an
insert-blocking hold. Under this design every one of those sites is still inside such a hold,
so converting any of them would lengthen one.

**Test levers and tests.** `bm25_native.debug_pause` accepts a comma-separated list, so one
backend can park twice. The pause points are now fourteen: `orphan_sweep_start` (8),
`merge_start` (9), `pending_append_alloc` (10), `orphan_sweep_marked` (11),
`reclaim_retired_compacted` (12), `reclaim_retired_between_chunks` (13) and
`merge_between_passes` (14) were added in this work, appended so existing keys are unchanged.
`t/027_cleanup_order.pl` (a VACUUM cancelled in its sweep has already merged and reclaimed; one
cancelled in its merge has already reclaimed; `bm25_merge` holds no singleton while it waits
for the heap), `sql/133_pending_truncate_fsm` (pages recycled by an insert-path seal are found
by the allocator with no VACUUM between) and `t/032_bounded_maintenance_holds.pl` (parks at
points 13 and 14 and checks that an INSERT with a short `lock_timeout` succeeds while no
index lock is held).

## Alternatives considered

- **A second lock splitting the singleton ("4-A").** The design pass recommended it. A new
  lock M, a lock tag on `InvalidBlockNumber` that can never be a real page, would cover
  structure (builds, swaps, reclaims, the sweep, bulkdelete), and the metapage lock would be
  narrowed to the pending anchor, with appenders taking it in RowExclusive (self-compatible,
  conflicting with the Share every older binary uses, so mixed binaries stall each other
  without interleaving unsafely). Merges and reclaims would stop blocking inserts, which
  leaves only a seal blocking them. Its costs were: a lock-protocol change that supersedes
  ADR 0019's mode, the 0066 and 0102 cancel trade and ADR 0107's choice of lock; the
  extension lock (`ReadBufferExtended(P_NEW)` passes `EB_SKIP_EXTENSION_LOCK`, safe today only
  because the singleton and the metapage serialize every allocator, so `bm25_page_alloc`'s
  extend and the swap's retired tail would move to `ExtendBufferedRel`); and a larger set of
  mixed-binary arguments, one of which (the sweep floor) review found wrong. The user
  chose the one-singleton option as the smaller change: the gate and the share-mode sweep
  already take the defect's primary hold, the O(index) orphan sweep, off the insert path,
  and chunking bounds reclaim and forced merges without superseding the lock protocol that
  ADRs 0019, 0066, 0102 and 0107 rest on. The opportunistic merge pass staying a single hold
  was accepted as the residual of that choice.
- **Under one singleton, no chunking: leave reclaim and forced merge as single holds.** They
  grow with the index; chunking bounds them to one unit.
- **Bound the opportunistic merge pass.** A pass is the unit that must not be split, and its
  build must stay hidden from the orphan sweep (ADR 0019), so it stays one hold.
- **Convert the XCUT-08 sites to `BM25_VACUUM_DELAY_POINT()`.** Each would lengthen a hold
  that blocks inserts; not done (above). ADR 0041 had kept plain `CHECK_FOR_INTERRUPTS` at
  the reclaim loops on the ground that they are not exclusively VACUUM-path; its addendum
  records that this does not change under one singleton.
- **ADR 0022's anchor-reset to bound the seal.** Rejected there as materially harder; the
  seal hold stays a residual.

## Consequences

- **Residual: VACUUM's opportunistic merge pass is a single pass and holds the singleton for
  its whole length.** On a large index with a steady insert rate, an autovacuum whose merge
  pass outlasts `deadlock_timeout` while an insert waits is cancelled (`canceling autovacuum
  task`, context `while cleaning up index`), losing that run's freeze progress and
  statistics; a manual VACUUM or `bm25_merge()` in a low-insert window lets it finish. Running
  the merge before the sweep means a cancelled merge loses only itself.
- **Residual: the seal's hold is O(pending),** and pending can grow while a longer hold
  (the sweep in share mode, ADR 0117) makes the opportunistic seal skip.
- Inserts wait for each reclaim chunk and each forced merge pass, but not for the whole reclaim
  or the whole ladder.
- A cleanup that reaches the seal or a reclaim while an explicit seal, merge or upgrade holds
  the singleton still waits, as before; only `merge_maybe(false)` is conditional.
- ADR 0022's statement that inserts wait for a seal, a merge and the other holders is
  narrowed to a seal, a reclaim chunk and a merge pass (its addendum).
- Release note: restart the server after installing, because an older backend neither
  records the evidence nor shares the protocol (ADR 0116).
