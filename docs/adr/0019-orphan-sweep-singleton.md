---
id: 0019
title: bm25_reclaim_orphans runs under the seal/merge singleton
date: 2026-07-29
status: Superseded
superseded_by: 0117
summary: The orphan mark-and-sweep takes LockPage(BM25_METAPAGE_BLKNO, ExclusiveLock) for its whole duration, because "unreachable" is only a safe proxy for "orphan" while nothing is building or swapping.
---

# 0019. `bm25_reclaim_orphans` runs under the seal/merge singleton

## Context

`bm25_reclaim_orphans` performed its entire mark-and-sweep with **no heavyweight
lock**, while every other page-lifecycle operation — the seal, the merge swap,
and its own sibling `bm25_reclaim_retired` in the same file — serializes on
`LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock)`.

The sweep's whole premise is "unreachable from the metapage ⇒ orphan". That is
only safe while nothing is **building**. A segment under construction in another
backend exists solely as unreachable orphan pages until its publish record
commits — that is the two-phase-install design, stated in
`bm25_segment_build_orphans`' own header: *"the CALLER's publish record is the
linearization point."* And `chain_flush` releases each page before allocating the
next, because the 4-buffer Generic WAL cap forbids holding them, so those live
in-flight pages sit unlocked and unreachable, exactly matching the sweep's
definition of garbage.

So the sweep could take `BUFFER_LOCK_EXCLUSIVE` on a live in-flight page
uncontended, OR `BM25_PAGE_DELETED` into its opaque with an invalid `retire_xid`,
and hand it to `RecordFreeIndexPage`. The next `bm25_page_alloc` in any backend
returns it — a DELETED page with an invalid `retire_xid` is precisely what makes
reuse legal — and the caller FPI-re-inits it, destroying a live segment's
dictionary or postings page. Nothing upstream prevents the race: VACUUM holds
only `ShareUpdateExclusiveLock`, which does not conflict with the
`RowExclusiveLock` an INSERT holds, so the concurrent builder is an ordinary
INSERT crossing the seal threshold (review ref C6, issue #37).

A second manifestation needed no allocator at all. The sweep read the metapage at
one point and `bm25_segcat_read` did its **own** `bm25_meta_read` a few lines
later — two independent snapshots. A merge swap landing between them left the
sweep marking the OLD catalog chain while enumerating the NEW one, so the new
chain's pages were swept and freed while live.

The in-file comment justifying the absence of a lock — *"Phase 3 has no
merge/catalog-swap, so every unmarked page is a true orphan"* — was true when
written and went stale when the merge landed and the opportunistic `aminsert`
seal started building segments from any backend.

## Decision

We will take `LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock)` at the top of
`bm25_reclaim_orphans` and release it at the end — the same singleton, acquired
the same way, as `bm25_reclaim_retired` twenty lines down. `RelationGetNumberOfBlocks`
moves under the lock too, so the extent reflects a quiesced relation.

## Alternatives considered

- **Have the sweep skip pages younger than the `nblocks` snapshot** — the
  accidental protection that already existed. It only covers pages the builder
  extends *after* the sweep starts; pages taken from the FSM, and any page the
  builder wrote before the sweep began, are in range. Not a fix.
- **Have builders stamp in-flight pages with a distinguishing flag the sweep
  skips** — a second, parallel liveness mechanism to keep in sync with the
  existing gen/DELETED stamps, and it still would not fix the two-metapage-reads
  catalog-swap variant. The singleton fixes both with one line.
- **Take only the metapage buffer lock across the sweep** — would have to be held
  across every `mark_chain` walk, inverting the order established in
  `docs/adr/0018` and blocking every scan for the duration of a full-relation
  sweep.

## Consequences

- A VACUUM's orphan sweep now blocks a concurrent explicit `bm25_seal()` /
  `bm25_merge()` for its duration, and vice versa. That is the intended
  serialization; the opportunistic `aminsert` seal uses `ConditionalLockPage` and
  simply skips, as it already does when a seal is in progress.
- The lock is re-entrant, so a future caller that already holds it stays correct.
  On error it is released by transaction abort, matching `bm25_reclaim_retired`.
- Heavyweight-before-buffer ordering is preserved: the singleton is taken before
  any buffer lock in the sweep, as in the seal and both merge paths.
- The metapage read and `bm25_segcat_read`'s own internal read are now one
  snapshot, closing the catalog-swap variant.
- **No new regression test.** Both failure modes require a second backend to be
  mid-build or mid-swap inside a window that opens and closes within one
  statement; `isolationtester` interleaves only between statements, so it cannot
  hold a builder there without a fault-injection point. Verified structurally,
  and by the existing sweep coverage — `sql/18_vacuum_reclaim`,
  `sql/20_merge_reclaim`, `sql/21_retired_descriptors` and
  `t/004_crash_orphan.pl` — continuing to pass, which is what proves the added
  serialization did not break the sweep itself.

## Addendum (2026-08-16)

The singleton this record establishes does **not** cover the case it looks like it
covers for one page kind, and the sweep's stamp is no longer uniform. Issue #135.

**What the singleton does not reach.** The Context above argues that "unreachable
⇒ orphan" holds only while nothing is *building*, and closes that with the
metapage `LockPage`. Correct, and unchanged. But there is a second reader class
the singleton never touches: **scans**. A scanner holds only `AccessShareLock` and
takes no part in the seal/merge/reclaim singleton. `bm25_scan_snapshot` captures
`pending_head` under one metapage SHARE lock, releases it, and walks the pending
chain *afterwards* — page by page, each buffer released before the next is read.
So a scan that started before a seal is still holding block numbers out of the
detached chain long after the seal committed, and no amount of serialization among
*writers* observes that.

**Why that was fatal specifically for pending pages.** The sweep's stamp was
`BM25_PAGE_DELETED + InvalidFullTransactionId` for every orphan, which
`bm25_page_alloc` accepts *immediately* — its horizon test short-circuits on an
invalid `retire_xid`. Segment pages tolerate that: every segment page carries a
per-page `seg_gen` that `bm25_seg_page_validate` checks on each read, so a stale
reader landing on a recycled-and-re-inited segment page fails loudly. Pending
pages have no such backstop — `bm25_page_init` leaves `seg_gen = 0` on them, which
makes that validation a documented no-op for exactly this chain. A recycled
pending page therefore gets decoded as a stream of `BM25PendingDocHeader`s
whatever it now actually contains: garbage TIDs and garbage `doclen` folded into
corpus statistics, or a spurious `ERRCODE_INDEX_CORRUPTED` on a healthy index.

**Decision.** A drained pending page is now freed with a real horizon —
`ReadNextFullTransactionId()`, read once outside every buffer lock (it takes
`XidGenLock`) — at **both** sites that can free one:

- `bm25_pending_truncate` (`src/bm25_pending.c`), the opportunistic post-commit
  recycler, one value for the whole chain because the pages were all detached at
  one instant (the publish record);
- `bm25_reclaim_orphans` (this record's function, `src/bm25_fsm.c`), for orphans
  carrying `BM25_PAGE_PENDING` only.

The second site is not optional and is not covered by the issue as filed. The
truncate is *opportunistic*: it is skipped or unwound whenever the seal crashes,
errors, or loses the opportunistic `ConditionalLockPage` race, and the detached
chain then reaches the FSM through this sweep instead. Stamping only the truncate
would have left the identical use-after-free reachable through VACUUM.

Every **other** orphan kind keeps `InvalidFullTransactionId`, because it has the
`seg_gen` backstop that pending pages lack. That asymmetry is deliberate and is
spelled out at the stamp site.

**Corrected cost model.** The issue predicted "one extra VACUUM cycle before the
pages return", and that is wrong. `GlobalVisTestIsRemovableFullXid` self-updates
its boundary (it re-runs `GlobalVisUpdate` and retests rather than trusting a
cached bound), so on a quiet cluster a stamped xid becomes removable after roughly
**one completed transaction** — no VACUUM required. The real cost is elsewhere,
in the allocator:

`GetFreeIndexPage` calls `RecordUsedIndexPage` as a side effect, so within ONE
`bm25_page_alloc` call the free pool only shrinks (that is what makes the loop
terminate — ADR 0042). But rejected candidates are handed back to the FSM after
the loop, so the NEXT call pops the same ones again. With N horizon-blocked pages
resident in the FSM, every allocation walked all N — a buffer read and a
`ConditionalLockBuffer` each — before reaching the extend it was always going to
reach. Tolerable while only retired segments contributed; a drained pending chain
does not stay small. At the default 4 MB seal threshold one seal frees ~512 pages
at once, so k seals under a long-lived snapshot leave ~512k blocked entries and
one seal's own appends cost ~512 x 512k buffer touches — quadratic in k.
`bm25_page_alloc` therefore now stops after `BM25_ALLOC_MAX_REJECTS` (32) rejected
candidates in one call and extends instead. That costs only reuse opportunity,
never correctness: the after-the-loop requeue preserves every rejected candidate
for a later call exactly as before, and extending by one page is what an exhausted
FSM already did. "Consecutive" and "total" coincide, since accepting a candidate
breaks the loop.

**One consequence worth naming, because it surprised the test analysis.** Nothing
in `src/` calls `GetCurrentTransactionId`, so `SELECT bm25_seal(...)`, `SELECT
bm25_merge(...)`, `VACUUM` and any read-only `SELECT` assign **no xid**. A
`ReadNextFullTransactionId()` stamp is by definition an xid that has not been
assigned yet, so a run of those statements cannot clear it — `nextXid` never moves.
Two things follow. Within one `amvacuumcleanup`
(`bm25_seal_index` → `bm25_reclaim_orphans` → `bm25_merge_maybe` →
`bm25_reclaim_retired`, one transaction) the merge can never reuse what the seal or
the sweep just freed in that same VACUUM; it extends instead, and picks those pages
up on a later pass. And any test that measures a footprint across a run of
seal/VACUUM/SELECT statements has to burn an xid deliberately — see ADR 0031's
addendum, where four TAP suites and `18_vacuum_reclaim` did exactly that. Neither
is a correctness problem; both are the deferral behaving as designed, and the
allocator cap above is what keeps the deferred population from costing more than it
saves.

**Wording elsewhere that this narrows.** ADR 0004's stamp-and-gate bullet
("`DELETED` and retire_xid invalid-or-horizon-clear") stays literally true — the
gate is unchanged; only which free path writes which value moved. ADR 0009's
unknown-page-kind argument says "an orphan is stamped `BM25_PAGE_DELETED` with
`InvalidFullTransactionId`, which `bm25_page_alloc` reuses IMMEDIATELY"; that
remains true of the case 0009 is reasoning about (an unknown kind is not
`BM25_PAGE_PENDING`), but it is no longer true of orphans in general. See 0009's
own addendum.

**Test.** `sql/89_pending_recycle_horizon`, on a new probe
`bm25_debug_page_retire_xid_valid(index regclass, blkno int) -> bool` — a bool and
not the raw xid8 so expected output is stable. The assertion needs no VACUUM cycle
and no timing at all: `bm25_page_mark_deleted` **ORs** the DELETED bit in and
leaves `BM25_PAGE_PENDING` set, so a recycled pending page stays identifiable as
`(DELETED | PENDING)` for as long as it is unreused, and every such page must be
horizon-gated. Pre-fix `f`, post-fix `t`. The orphan-arrival half still needs a
crash and stays with `t/004_crash_orphan.pl`; what 88 pins on the sweep side is
that a VACUUM does not *downgrade* an already-gated page.

## Addendum (2026-09-21)

The sweep's reachable set is now one BIT per block (`reach_bytes` / `reach_test` /
`reach_set` in `bm25_fsm.c`); it was a `bool` per block. `nblocks` is a
`BlockNumber` and `palloc` refuses anything over `MaxAllocSize`, so the bool array
capped this sweep at about 1G blocks (~8 TB at BLCKSZ 8192) -- VACUUM on a larger
index failed with a bare "invalid memory alloc request size" and reclaimed nothing
(#67, noted in the `bm25_fsm.c` finding's evidence). A bit per block covers the
whole `BlockNumber` range in 512 MB. Nothing else about the pass changed: the
bitmap is still `mark_chain`'s cycle guard, and bounding each block number before
it is set (the #134 fix) still precedes every write. Negative control: setting the
neighbouring bit instead turns `18_vacuum_reclaim`, `19_merge`,
`20_merge_reclaim`, `89_pending_recycle_horizon`, `91_vacuum_seal_interaction` and
`105_merge_memory_budget` red.

## Addendum (2026-10-04)

The fresh-eyes review of this date measured the cost of holding the singleton ExclusiveLock across the whole mark-and-sweep: an INSERT waiting on that page lock (LOCKTAG_PAGE) triggers PostgreSQL's blocking-autovacuum cancellation after deadlock_timeout, so on an insert-busy table autovacuum index cleanup is cancelled repeatedly, losing the sweep and the only automatic merge. Any fix that releases the lock between mark and sweep must re-validate liveness per page, because the reachable bitmap goes stale; a seg_gen check covers only segment pages (pending and catalog pages carry seg_gen 0 and still need reachability marking), and a release gap reintroduces this record's swap and in-flight-build hazards. Tracked in #300.

## Addendum (2026-10-05)

The Exclusive mode this record chose is superseded by ADR 0117 (#300): the sweep now holds
the singleton in ShareLock, which still excludes every seal, merge, swap, upgrade rewrite and
reclaim and admits only appenders, with a per-page epoch rule protecting a page an appender
owns. The reachability argument of the Context, the single metapage snapshot, and the horizon
asymmetry of the 2026-08-16 addendum all carry over unchanged. What changed around it:

- the sweep runs only on durable evidence that an orphan can exist (ADR 0116), no longer on
  every VACUUM;
- cleanup runs it last, after the seal, `bm25_reclaim_retired` and `merge_maybe(false)`
  (ADR 0118);
- the Consequence that a sweep blocks an explicit `bm25_seal()` or `bm25_merge()` and the
  reverse still holds, because their Exclusive request conflicts with Share. What no longer
  holds is that it blocks inserts.
