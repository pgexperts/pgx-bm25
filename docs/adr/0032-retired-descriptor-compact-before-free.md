---
id: 0032
title: A retired descriptor stops listing a range before the range is freed
date: 2026-07-29
status: Accepted
summary: bm25_reclaim_retired skipped the descriptor rewrite on the will_unlink path, so reclaim_one_range freed and DELETED-stamped a segment's pages while the descriptor still listed them; the rewrite is now unconditional whenever anything is dropped, making an interruption leak pages instead of aliasing reallocated ones.
---

# 0032. A retired descriptor stops listing a range before the range is freed

## Context

`bm25_reclaim_retired` walks the retired-list descriptor pages and, for each
`BM25RetiredEntry` whose `retire_xid` has cleared the cluster horizon, frees every page
of the segment it describes. A descriptor page that drains to zero entries and is not
the head is unlinked from the chain and freed too.

The two outcomes were sequenced differently, and one of them was wrong (review ref H11,
issue #50). `will_unlink` was computed first, and the in-place compaction was then gated
on `ndrop > 0 && !will_unlink` — the reasoning being that there is no point compacting a
page that is about to disappear. So on the unlink path the descriptor page was released
with all `ndrop` entries intact, `reclaim_one_range` freed the segments, and only then
was the page spliced out of the chain and freed.

That ordering is unsafe because the free is immediately effective and the splice is not
guaranteed to follow:

- `reclaim_one_range` stamps each page `BM25_PAGE_DELETED` with `retire_xid` and calls
  `RecordFreeIndexPage`. That `retire_xid` has *already* passed
  `GlobalVisCheckRemovableFullXid`, so `bm25_page_alloc` accepts those pages at once.
- Generic WAL page changes are physical. `GenericXLogFinish` applies and logs them, and
  transaction abort does not undo them. The DELETED stamps and the FSM entries survive an
  error.
- The pass holds the metapage singleton, but the pending-append allocator does not take
  it, so another backend can consume the freed pages during the window.

An error or crash between the free and the splice — `ReadBuffer(prev_blk)` raising on a
short read, `GenericXLogStart`'s palloc failing, the backend dying — therefore leaves
live entries describing pages the allocator has already re-handed to a live structure. A
later pass re-reads those entries and `reclaim_one_range` walks the *new* structure's
current `nextblk` links (it deliberately does no `seg_gen` validation, by its own header
contract) and hands them to the FSM page by page. Silent index corruption.

The non-unlink branch already had the correct order, so this was an intra-function
inconsistency rather than a missing concept.

## Decision

**Drop the `!will_unlink` condition: compact whenever anything is dropped.** The
descriptor page is rewritten to keep only survivors — zero of them, on the unlink path —
inside its own closed Generic WAL window before `reclaim_one_range` touches a single
segment buffer. The splice and the descriptor page's own free still happen afterwards.

This inverts the failure mode. An interruption now leaves an empty-but-still-linked
descriptor page, which the next pass sees as `nkeep == 0` and unlinks by the ordinary
`will_unlink` path, plus some not-yet-freed data pages that are unreachable from the live
catalog and from the retired list. `bm25_reclaim_orphans` is a closed-world
mark-and-sweep, so it recovers exactly those pages ([0019](0019-orphan-sweep-singleton.md)).
Deferred space beats aliased pages.

The cost is one extra page image per drained descriptor page — roughly one per merge that
fully clears a descriptor, on the VACUUM path.

## Alternatives considered

- **Move the splice (and the descriptor free) above `reclaim_one_range`.** The report's
  primary suggestion, and it achieves the same invariant. Rejected as the larger diff: it
  reorders three buffer acquisitions across two Generic WAL windows in a function whose
  header documents "never nest a second buffer acquisition inside an open window", to buy
  nothing over the one-condition change. The empty-but-linked intermediate state that
  compaction leaves behind is already a state the function handles.
- **Validate `seg_gen` in `reclaim_one_range` so a stale entry cannot free a live
  structure.** Defense in depth against this whole bug class, and worth having, but it is
  a different decision: the header explicitly declines gen validation because the segment
  is past the horizon and out of every live catalog, which is true whenever the ordering
  invariant holds. Fixing the ordering keeps that premise true.
- **Leave the free-then-splice order and rely on the orphan sweep to notice.** It cannot.
  The sweep marks every retired entry's chain roots REACHABLE precisely so that
  freshly-retired pages are not freed early, so a surviving entry actively *protects* the
  aliased pages from being noticed as orphans.

## Consequences

- The window in which a descriptor entry can outlive the free of the range it describes
  is closed. Both branches now share one rule: the descriptor stops listing a range
  before the range is freed.
- No behavioral change on the success path. The end state after a completed pass is
  byte-identical, which is why this carries no new regression test: the defect is only
  observable by interrupting the function between two statements, and the repo has no
  fault-injection facility (PG 17+ `INJECTION_POINT` would need
  `--enable-injection-points` in the server build and in CI — its own change).
  `sql/20_merge_reclaim`, `sql/21_retired_descriptors` and `sql/62_segcat_chain` cover
  that the drain, the unlink and the space reuse still work; `t/004_crash_orphan.pl`
  covers the orphan sweep this fix now leans on.
- One additional Generic WAL record per drained descriptor page on the reclaim path.

## Addendum (2026-10-04)

The fresh-eyes review of this date found the original H11 rationale (review ref H11, issue #50, not the later #300) stale on one point: pending appends now take the singleton in ShareLock mode, which conflicts with reclaim's ExclusiveLock, so the 'allocator does not take it' premise is gone, though compact-before-free is still needed for abort and crash safety. It also found that `reclaim_one_range` stamps and frees every page a retired chain names with no kind, gen or block-0 check, so one corrupt root can free the metapage. Tracked in #302 (validation) and #312 (comment).

## Addendum (2026-10-05, PRs #331-#350)

- **Per-page validation in `reclaim_one_range`** (#302.C, PR #342). Each page is checked
  before it is stamped DELETED: not block 0, page content bounds (which include
  `pd_special`), the chain's own kind, `seg_gen` equal to the descriptor's gen, and not
  already DELETED. Anything else is `ERRCODE_INDEX_CORRUPTED`. The DELETED test doubles as the
  walk's cycle guard, since compact-before-free means no range is legitimately walked twice.
  Where it fires was decided by the user (D3): after the descriptor is compacted, so it fires
  once. Index WAL is not transactional, so the compaction record survives the abort, the next
  VACUUM proceeds, and its orphan sweep recovers the pages the failed walk left (the leak this
  record already accepts). The alternative, refusing before compaction, would fail every
  VACUUM, and with it merge and the orphan sweep (ADR 0118's order), until REINDEX.
- `bm25_segheader_validate` rejects a chain root of 0, so a corrupt live header can no longer
  be harvested into a retired descriptor at merge time.
- The 2026-10-04 addendum's stale "allocator does not take the singleton" comment was
  rewritten (#312 META-05, PR #349): every allocator that can run against a live index,
  the pending append included, holds the metapage singleton, which this pass holds
  ExclusiveLock.
- `t/037_reclaim_unlink_order` now witnesses this record's order on the will_unlink path: a
  second merge's reclaim is parked after compacting a non-head descriptor page and cancelled;
  the page lists nothing, stays linked and has freed nothing. With #50 reverted it fails, and
  `t/029`-`t/031` stay green (PR #347).
