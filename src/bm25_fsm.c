/* bm25_fsm.c -- page reclamation. Phase 3: orphan (crashed-seal / unreferenced)
 * page reclamation via a mark-and-sweep over the page space, recording
 * unreachable pages to the FSM (the bloom blvacuum.c pattern). Phase 4 extends
 * this file with the XID-horizon retired-segment reclamation
 * (bm25_retire_segment / bm25_reclaim_retired). This file owns only the STAMP +
 * FREE half of stamp-and-gate; the horizon-gated reuse allocator -- the GATE half
 * -- is bm25_page_alloc, which lives in bm25_meta.c. Hot-Standby reuse safety is
 * option (d) (per-page seg_gen stamping + reader validation), not a custom
 * reuse-conflict WAL record (unbuildable under Generic-WAL-only -- see Phase 4's
 * reuse-safety note).
 *
 * The sweep READ-locks chains to mark reachability, then STAMPS each orphan
 * BM25_PAGE_DELETED (stamp-and-gate: a crash-unsafe FSM hint can hand back a
 * reused-and-now-live page, so the allocator reuses only on-page-marked pages)
 * before RecordFreeIndexPage + IndexFreeSpaceMapVacuum. The stamp carries
 * InvalidFullTransactionId (immediate reuse) for every orphan kind EXCEPT
 * BM25_PAGE_PENDING, which gets a real horizon -- see the sweep-loop comment on
 * that asymmetry (issue #135).
 * The FSM is a hint; the on-page DELETED mark is the source of truth the allocator
 * gates on, and the CALLER of bm25_page_alloc still FPI-re-inits the returned page
 * so stale contents never leak into a new segment. */
#include "postgres.h"

#include "bm25.h"
#include "storage/indexfsm.h"   /* RecordFreeIndexPage / IndexFreeSpaceMapVacuum */
#include "storage/bufmgr.h"
#include "storage/lmgr.h"       /* LockPage / UnlockPage (merge singleton, reclaim) */
#include "storage/procarray.h"  /* GetOldestNonRemovableTransactionId (horizon refresh) */
#include "utils/snapmgr.h"      /* GlobalVisCheckRemovableFullXid (per-entry reuse gate) */
#include "access/transam.h"     /* FullTransactionId (retired-list entries) */
#include "access/xact.h"        /* BeginInternalSubTransaction (bm25_crash_epoch) */
#include "access/htup_details.h"   /* heap_form_tuple (bm25_debug_sweep_evidence) */
#include "funcapi.h"            /* get_call_result_type (bm25_debug_sweep_evidence) */
#include "miscadmin.h"          /* IsUnderPostmaster, MyProcPid */
#include "storage/dsm_registry.h"   /* GetNamedDSMSegment (bm25_crash_epoch) */
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"    /* GetCurrentTimestamp (crash-epoch fallback seed) */

/* Forward-declared: defined below (near bm25_reclaim_retired, which they were
 * introduced for) but bm25_reclaim_orphans's Phase 4 -- earlier in this file --
 * now shares them too, so both retired-list page readers run the identical
 * on-page-kind and entry-count checks. */
static void bm25_retired_page_flags_validate(uint16 flags, BlockNumber blk);
static void bm25_retired_count_validate(int n, BlockNumber blk);
static void bm25_retired_cycle_cap_validate(uint64 visited, BlockNumber nblocks,
                                            BlockNumber blk);
static void bm25_segment_blkno_validate(BlockNumber blkno, BlockNumber nblocks,
                                        const char *src);

/* Stamp a page BM25_PAGE_DELETED + retire_xid in its opaque, under Generic WAL.
 * This is the stamp half of stamp-and-gate: the index FSM fork is not crash-safe,
 * so after recovery GetFreeIndexPage can hand back a page that was freed, reused,
 * and is LIVE again. We therefore mark every freed page on-page BEFORE recording it
 * free; bm25_page_alloc reuses ONLY marked (or brand-new) pages and rejects unmarked
 * (= live) ones. The opaque sits in the special area, OUTSIDE the [pd_lower,pd_upper)
 * page hole Generic WAL compresses, so a plain delta record suffices -- no
 * GENERIC_XLOG_FULL_IMAGE. Caller holds buf EXCLUSIVE-locked and guarantees
 * !PageIsNew(page) (a zero page has no valid special area to stamp). retire_xid =
 * InvalidFullTransactionId => reusable now (a gen-validated orphan, whose stale
 * reader is caught by bm25_seg_page_validate); a valid retire_xid => reuse gated on
 * the cluster horizon (retired-segment reclaim, AND every pending-chain page,
 * issue #135: a pending page's seg_gen is its chain epoch (#291), which a scan can
 * only bound, not match, so on the primary the horizon is what keeps a scan's walk
 * from ever meeting a reused page; the epoch check covers the standby, which holds
 * no horizon back). */
void
bm25_page_mark_deleted(Relation index, Buffer buf, FullTransactionId retire_xid)
{
    GenericXLogState   *st;
    Page                pg;
    BM25PageOpaque     *op;

    /* Issue #302.D: with pd_special past BM25_PAGE_SPECIAL_OFF the stamp below would
     * land beyond the registered image (into GenericXLogState's next slot), never
     * reach the page or the WAL, and the page would be recorded free unmarked. Checked
     * on the shared page before the window opens, so the ERROR unwinds cleanly. */
    bm25_page_special_validate(BufferGetPage(buf), BufferGetBlockNumber(buf));
    st = GenericXLogStart(index);
    pg = GenericXLogRegisterBuffer(st, buf, 0);
    op = BM25PageGetOpaque(pg);

    op->flags |= BM25_PAGE_DELETED;
    op->retire_xid = retire_xid;
    GenericXLogFinish(st);
}

/* The sweep's reachable set is ONE BIT per block, not a bool (#67). nblocks is a
 * BlockNumber, and palloc refuses anything over MaxAllocSize, so a bool-per-block
 * array capped the sweep at about 1G blocks (~8 TB at BLCKSZ 8192): VACUUM on a
 * larger index failed with a bare "invalid memory alloc request size" instead of
 * reclaiming anything. At a bit per block the entire BlockNumber range needs 512 MB,
 * inside the cap, so no relation PostgreSQL can address is out of reach. */
static inline Size
reach_bytes(BlockNumber nblocks)
{
    /* Not ((Size) nblocks + 7) / 8: Size is 32 bits on a 32-bit build, where that
     * addition wraps for the top few BlockNumbers and would size a tiny array. */
    return (Size) nblocks / 8 + 1;
}

static inline bool
reach_test(const uint8 *reachable, BlockNumber blk)
{
    return (reachable[blk / 8] & (1 << (blk % 8))) != 0;
}

static inline void
reach_set(uint8 *reachable, BlockNumber blk)
{
    reachable[blk / 8] |= (uint8) (1 << (blk % 8));
}

/* Mark every block reachable from one BM25_PAGE_* chain (follow opaque->nextblk).
 * Read-lock only. The reachable bitmap doubles as the cycle guard: a corrupt or
 * self-referential nextblk terminates the walk instead of looping forever.
 *
 * A never-initialized page on a chain is an ERROR (issue #302 F). Every live chain's
 * links are written in or after the record that initializes the page they name (a
 * pending append's init and link share one record since #300; a segment's chains are
 * unreachable until its publish record), so a zero page here is corruption. Without the
 * check its opaque is read from the page header: a cassert build fails
 * PageGetSpecialPointer's assertion, and a production build reads nextblk 0 and stops
 * only because block 0 is already marked. ERROR rather than stop, because the sweep frees
 * whatever this leaves unmarked. */
static void
mark_chain(Relation index, BlockNumber start, uint8 *reachable, BlockNumber nblocks)
{
    BlockNumber blk = start;

    while (blk != InvalidBlockNumber && blk < nblocks)
    {
        Buffer          buf;
        Page            pg;
        BM25PageOpaque *op;

        /* mark_chain's only caller, bm25_reclaim_orphans, runs exclusively on the
         * VACUUM path (amvacuumcleanup), so this is the throttling form, not a
         * bare check -- matching bm25_bulkdelete (ADR 0025). Placed at the top of
         * the loop body, before the buffer is touched: the previous iteration
         * always released its buffer before looping back (UnlockReleaseBuffer
         * below runs on every path through this loop), so this is a genuinely
         * lock-free instant, unlike the ChainWriter/BM25DictIter loops elsewhere
         * in this pass that hold a buffer across iterations by design. */
        BM25_VACUUM_DELAY_POINT();

        if (reach_test(reachable, blk))
            break;              /* already visited (defends against cycles) */
        reach_set(reachable, blk);
        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        if (PageIsNew(pg))
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: chain from block %u links uninitialized block %u",
                            start, blk)));
        /* #302.D: nextblk sits at pd_special. Past the block, the walk would follow a
         * neighbour's bytes and stop short, and the full-extent pass, which skips
         * every page marked here, would then free the rest of a live chain. */
        bm25_page_special_validate(pg, blk);
        /* The page entry check, before the opaque read: it bounds pd_lower and
         * pd_upper (pd_lower >= the header, pd_lower <= pd_upper <= BLCKSZ) and
         * checks pd_special again (the check above already did, naming the block), so a
         * page whose header is garbage is an ERROR rather than a nextblk read from it. */
        (void) bm25_page_content_bytes(pg);
        op = BM25PageGetOpaque(pg);
        blk = op->nextblk;
        UnlockReleaseBuffer(buf);
    }
}

/* ---- orphan-sweep gate (issue #300) ---- */

/* Sweeps this backend has run past the gate. Test-only observability
 * (bm25_debug_orphan_sweeps); process-local, like bm25_debug_work_units. */
static int64 bm25_orphan_sweeps_run = 0;

/* The crash epoch: a nonzero 64-bit value that changes every time the server's shared
 * memory is (re)initialised -- a start, a restart, AND a crash-restart under
 * restart_after_crash, where the postmaster itself keeps running. The orphan sweep
 * records it on completion (meta.swept_epoch) and runs again whenever it differs:
 * a crash can strand a P_NEW zero page that no WAL record mentions, and the index FSM
 * is not WAL-logged, so a crash (or a standby's promotion) can lose free entries that
 * only a sweep re-records.
 *
 * Not the postmaster start time: PgStartTime is assigned once, in PostmasterMain, and
 * a crash-restart re-creates shared memory without touching it, so a backend crash
 * between two sweeps would go unnoticed. A named DSM segment lives exactly as long as
 * shared memory does: the registry that names it is itself re-created, empty, by
 * DSMRegistryShmemInit on every shared-memory init. GetNamedDSMSegment needs no
 * shared_preload_libraries, and is in every major this extension supports (17+).
 *
 * FROZEN: the segment's name and size must never change. A binary that asked for the
 * same name with another size would ERROR against a segment an older binary created,
 * for the life of the server. */
#define BM25_CRASH_EPOCH_SEGMENT    "bm25_native.crash_epoch"
#define BM25_CRASH_EPOCH_SIZE       sizeof(uint64)

static bool     bm25_crash_epoch_cached = false;
static uint64   bm25_crash_epoch_value = 0;

#if PG_VERSION_NUM >= 190000
static void
bm25_crash_epoch_init(void *ptr, void *arg)
#else
static void
bm25_crash_epoch_init(void *ptr)
#endif
{
    uint64      v = 0;

    if (!pg_strong_random(&v, sizeof(v)))
        v = (uint64) GetCurrentTimestamp() ^ ((uint64) MyProcPid << 32);
    if (v == 0)
        v = 1;                  /* 0 is reserved for "unknown" / "never swept" */
    *(uint64 *) ptr = v;
}

/* This server lifetime's crash epoch, or 0 when it cannot be had -- a standalone
 * backend, or a GetNamedDSMSegment failure (out of DSM segments, a full /dev/shm, or
 * a PG 17 minor whose registry keeps a half-initialised entry after one failed
 * create). 0 means "unknown": the gate then always sweeps, and the completion record
 * stores 0, so the next VACUUM sweeps too. That is the only safe reading, and it makes
 * a DSM problem cost VACUUM time rather than fail every cleanup of every bm25 index.
 *
 * The failure is caught in an internal subtransaction, the only safe way to recover
 * from an ERROR thrown while the registry's LWLocks were held: its abort releases them
 * and any half-made DSM mapping. A query cancel is re-thrown, never swallowed. Call
 * this with no buffer lock held and no Generic WAL window open; bm25_reclaim_orphans
 * calls it first thing. Cached per backend once known: a crash-restart kills every
 * backend, so no cached value outlives the epoch it describes. */
uint64
bm25_crash_epoch(void)
{
    MemoryContext       oldcxt = CurrentMemoryContext;
    ResourceOwner       oldowner = CurrentResourceOwner;
    volatile uint64     v = 0;

    if (bm25_crash_epoch_cached)
        return bm25_crash_epoch_value;
    if (!IsUnderPostmaster)
        return 0;

    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(oldcxt);
    PG_TRY();
    {
        bool        found;
        uint64     *p;

#if PG_VERSION_NUM >= 190000
        p = (uint64 *) GetNamedDSMSegment(BM25_CRASH_EPOCH_SEGMENT, BM25_CRASH_EPOCH_SIZE,
                                          bm25_crash_epoch_init, &found, NULL);
#else
        p = (uint64 *) GetNamedDSMSegment(BM25_CRASH_EPOCH_SEGMENT, BM25_CRASH_EPOCH_SIZE,
                                          bm25_crash_epoch_init, &found);
#endif
        v = *p;
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcxt);
        CurrentResourceOwner = oldowner;
    }
    PG_CATCH();
    {
        ErrorData  *edata;

        MemoryContextSwitchTo(oldcxt);
        edata = CopyErrorData();
        FlushErrorState();
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(oldcxt);
        CurrentResourceOwner = oldowner;
        if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED)
            ReThrowError(edata);
        ereport(LOG,
                (errmsg("bm25: could not attach the crash-epoch segment, so VACUUM will run the orphan sweep: %s",
                        edata->message)));
        FreeErrorData(edata);
        v = 0;
    }
    PG_END_TRY();

    if (v != 0)
    {
        bm25_crash_epoch_value = v;
        bm25_crash_epoch_cached = true;
    }
    return v;
}

/* The gate: does this index need its orphan sweep? See bm25_reclaim_orphans's
 * "GATED, AND SHARE-MODE" for the invariant this decides. epoch == 0 (unknown) always
 * sweeps; swept_epoch == 0 (never swept by a binary keeping this evidence: a new
 * index, or one written only by older binaries) differs from every real epoch. */
static bool
bm25_orphan_sweep_needed(const BM25MetaPageData *meta, uint64 epoch)
{
    return epoch == 0 ||
           meta->swept_epoch != epoch ||
           meta->orphan_ops_begun != meta->orphan_ops_done;
}

/* The pending epoch floor for the sweep's per-page rule: min(next_gen, head's epoch,
 * tail's epoch), from the metapage copy the caller read under the singleton. The tail
 * is read without any bound by the caller's sampled nblocks -- an appender may have
 * extended the relation since -- because it is the page every later append copies its
 * epoch from; the metapage names it, so it exists. A head or tail that is not a PENDING page means
 * the metapage is corrupt, which a scan reports loudly; here it only drops the floor
 * to 0, so the sweep frees no pending page at all, the conservative answer. */
static uint32
bm25_pending_epoch_floor(Relation index, const BM25MetaPageData *meta)
{
    uint32      floor = meta->next_gen;
    BlockNumber ends[2];
    int         i;

    ends[0] = meta->pending_head;
    ends[1] = meta->pending_tail;
    for (i = 0; i < 2; i++)
    {
        Buffer      buf;
        Page        pg;

        if (ends[i] == InvalidBlockNumber)
            continue;
        if (ends[i] == BM25_METAPAGE_BLKNO)
        {
            floor = 0;          /* corrupt: see above */
            continue;
        }
        buf = ReadBuffer(index, ends[i]);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        if (!PageIsNew(pg))
            bm25_page_special_validate(pg, ends[i]);      /* #302.D, before the opaque */
        if (PageIsNew(pg) || !(BM25PageGetOpaque(pg)->flags & BM25_PAGE_PENDING))
            floor = 0;
        else
            floor = Min(floor, BM25PageGetOpaque(pg)->seg_gen);
        UnlockReleaseBuffer(buf);
    }
    return floor;
}

/* ---- Phase 4: retired-segment list (D-RETIRE/R4) ---- *
 *
 * A retired-list entry is a per-SEGMENT RANGE descriptor, not a per-page record.
 * It captures the dropped segment's header + chain roots (already in hand in the
 * dropped BM25SegCatEntry / its BM25SegmentHeader) plus the single retire_xid
 * horizon. bm25_reclaim_retired (Task 25) re-walks the chains later to free each
 * page. Storing the range (not every page) keeps the in-swap append to ONE
 * retired-list-tail page, so the swap's single Generic WAL record stays within
 * the 4-buffer cap.
 *
 * BM25RetiredEntry and BM25_RETIRED_PER_PAGE moved to bm25_format.h (the on-disk
 * home) so a non-merge swap caller (bm25_merge_rewrite_all) can guard on the same
 * single-retired-page cap the swap's in-window append relies on. They remain
 * available here unchanged via the bm25.h include. */

/* bm25_reclaim_orphans -- mark-and-sweep the page space, freeing true orphans.
 *
 * A page is REACHABLE if it is the metapage, on the pending chain, on the
 * segment-catalog chain, on the retired-free chain, on the field-config chain, or
 * transitively linked from a live segment header. For each live segment we mark its
 * header block plus its SEVEN chain roots -- dict_root, norms_root, livedocs_root,
 * docmap_root, posts_root, pos_root, keymap_root, i.e. every BlockNumber root in
 * BM25SegmentHeader. The postings live in ONE segment-wide chain rooted at
 * h.posts_root (D-POST), so a single mark_chain(posts_root) collects every
 * term's blocks in one walk -- there is no per-dict-term postings walk (that would
 * contradict D-POST and re-scan the same chain once per term).
 *
 * Everything unmarked in [1, nblocks) is an orphan -- a crashed-seal leftover or a
 * just-truncated pending page that the publish record already unlinked -- and is
 * recorded free. This is idempotent with bm25_pending_truncate's own
 * RecordFreeIndexPage (re-recording a free page is harmless). Live segment pages
 * are marked and never freed.
 *
 * "Unmarked => true orphan" holds ONLY while nothing is building or swapping, which
 * is why this pass takes the seal/merge singleton for its whole duration (see the
 * LockPage note in the body). The comment that used to stand here -- "Phase 3 has no
 * merge/catalog-swap, so every unmarked page is a true orphan and no scan snapshot
 * can reference one" -- was true when written and went stale the moment the merge
 * landed and the opportunistic aminsert seal started building segments from any
 * backend.
 *
 * PHASE 4 HAZARD (NOW HANDLED): a merged-away segment's pages leave the live
 * catalog but are NOT yet free -- they sit on the retired list as a RANGE
 * descriptor + retire_xid and must not be reused until the XID horizon clears
 * (bm25_reclaim_retired, Task 25). Retire does NO per-page work (D-RETIRE): those
 * pages are neither on the retired_head chain (only the BM25_PAGE_RETIRED
 * *descriptor* pages are, so mark_chain(retired_head) marks just the descriptors)
 * nor flagged BM25_PAGE_DELETED at swap time. To naive reachability they would look
 * like orphans and be freed prematurely -- the exact use-after-free the retired list
 * exists to prevent. This sweep now closes that hole: after marking the live-catalog
 * chains it walks the retired list and, for every BM25RetiredEntry, marks the
 * descriptor's header block + every chain root the entry carries (dict, norms,
 * livedocs, docmap, posts, keymap, pos -- all seven) REACHABLE (the retired-range
 * marking pass below). So retired segment pages are kept out of the free set here;
 * their actual freeing is DEFERRED to bm25_reclaim_retired once retire_xid clears the
 * horizon. The sweep's BM25_PAGE_DELETED guard remains as defense-in-depth (it covers
 * a page once reclaim_retired has begun stamping it), but it is no longer the sole
 * protection for a freshly-retired-but-unstamped page -- the RANGE marking is.
 *
 * GATED, AND SHARE-MODE (issue #300). The pass costs O(index) -- every live chain
 * marked, then every block visited, throttled -- and it used to run on every VACUUM
 * holding the singleton EXCLUSIVE, which every document-adding INSERT needs in
 * ShareLock (ADR 0022). Each insert waited for the whole pass; under autovacuum the
 * waiting insert's deadlock check cancelled the worker after deadlock_timeout, so on
 * an insert-busy table autovacuum was cancelled over and over and its heap work
 * (freeze horizons, reltuples) never landed. Two changes:
 *
 * 1. It runs only on durable evidence that an orphan, or a free page the FSM has
 *    lost, can exist. INVARIANT: whenever no singleton-EXCLUSIVE op is running,
 *    meta.orphan_ops_begun != meta.orphan_ops_done exactly when some op that can
 *    leave orphans began durably and did not reach its end, or ended by design
 *    with orphans behind it (a merge or upgrade swap, which orphans the old catalog
 *    chain); and every other orphan or lost FSM entry predates either a crash or
 *    restart (meta.swept_epoch differs from this server lifetime's
 *    bm25_crash_epoch) or the first sweep by a binary that keeps this evidence
 *    (swept_epoch == 0). The sources and their brackets: the seal's
 *    drain-build-publish-truncate (bm25_seal_pending_locked), each merge pass and
 *    bm25_merge_rewrite_all (never closed, see above), bm25_reclaim_retired's
 *    compact-then-free and splice-then-free, and the pending append -- which is no
 *    source at all since its new page, link and metapage update became one WAL
 *    record. What remains uncovered is an ERROR inside bm25_page_alloc after
 *    GetFreeIndexPage popped a candidate, or after a P_NEW extension: one free page
 *    drops out of the FSM, or a zero page is stranded, until the next sweep -- a
 *    space leak, never corruption, accepted rather than bracketing every
 *    allocation.
 *
 * 2. It holds the singleton in ShareLock, not ExclusiveLock. That still excludes
 *    every seal, merge, swap, upgrade rewrite and reclaim (all ExclusiveLock), which
 *    is everything ADR 0019's "unreachable is only a proxy for orphan" depends on;
 *    what it now admits is pending appenders. Their pages are the one thing the
 *    reachable[] snapshot can be wrong about, and the per-page rule in the sweep
 *    loop below is what keeps a page an appender owns from ever being stamped.
 *
 * What share mode does NOT fix (issue #300 residual, the ADR 0102 shape): a
 * BLOCKING bm25_seal() / bm25_merge() / bm25_upgrade() that arrives during the
 * sweep queues for ExclusiveLock, and every later appender's ShareLock request
 * then queues behind that waiter for the rest of the pass; the waiter, hard-blocked
 * by an autovacuum holder, also cancels the autovacuum after deadlock_timeout, and
 * the evidence stays set until a sweep completes. aminsert's opportunistic seal
 * takes the singleton conditionally and never queues.
 */
void
bm25_reclaim_orphans(Relation index)
{
    BM25MetaPageData    meta;
    BlockNumber         nblocks;
    uint8              *reachable;
    BM25SegCatEntry    *segs;
    uint32              nsegs,
                        s;
    BlockNumber         blk;
    FullTransactionId   pending_retire;
    uint64              epoch;
    uint32              e_floor;

    /* This server lifetime's crash epoch, fetched before any lock or buffer is
     * touched: it may create a DSM segment, and its failure handling runs a
     * subtransaction (bm25_crash_epoch). Cached per backend after the first call. */
    epoch = bm25_crash_epoch();

    if (RelationGetNumberOfBlocks(index) == 0)
        return;                 /* no metapage to read: nothing to sweep */

    /* The gate, first WITHOUT the singleton (issue #300 F9). A stale "needed" only
     * costs the locked re-check below. A stale "not needed" means an op opened its
     * bracket after this read -- and that op's own evidence sends a later VACUUM. So
     * an idle index's VACUUM never queues on the singleton at all, not even behind a
     * waiting bm25_seal(). */
    bm25_meta_read(index, &meta);
    if (!bm25_orphan_sweep_needed(&meta, epoch))
        return;

    /* Serialize against seals and catalog swaps -- the SAME singleton every other
     * page-lifecycle operation takes (seal, merge swap, bm25_reclaim_retired
     * below), in ShareLock: see "GATED, AND SHARE-MODE" above. On error the
     * heavyweight lock is released by transaction abort.
     *
     * Without it this whole mark-and-sweep ran unlocked, and "unreachable" is only
     * a safe proxy for "orphan" while nothing is BUILDING. A segment under
     * construction in another backend exists solely as unreachable orphan pages
     * until its publish record commits -- and chain_flush releases each page before
     * allocating the next (the 4-buffer cap forbids holding them), so the sweep
     * could take BUFFER_LOCK_EXCLUSIVE on a live in-flight page uncontended, stamp
     * it BM25_PAGE_DELETED with an invalid retire_xid, and hand it to the FSM. The
     * next bm25_page_alloc then returns it and the caller FPI-re-inits it,
     * destroying a live segment's dictionary or postings. VACUUM holds only
     * ShareUpdateExclusiveLock, which does not conflict with the RowExclusiveLock
     * an INSERT holds, so the concurrent builder is an ordinary INSERT that crossed
     * the seal threshold. The in-file premise "Phase 3 has no merge/catalog-swap,
     * so every unmarked page is a true orphan" went stale when the merge landed.
     *
     * Holding it also makes the metapage read below and bm25_segcat_read's OWN
     * bm25_meta_read one snapshot. Unlocked they were two: a swap landing between
     * them left the sweep marking the OLD catalog chain while enumerating the NEW
     * one, so the new chain's pages were swept and freed while live. */
    LockPage(index, BM25_METAPAGE_BLKNO, ShareLock);

    /* Read the extent under the singleton: nothing that builds can extend the
     * relation now, and pages appenders add past it are simply not this pass's
     * business. */
    nblocks = RelationGetNumberOfBlocks(index);

    /* The gate again, under the singleton: no bracket can open or close while we hold
     * it (every bracketed op takes it EXCLUSIVE), so the evidence read here is what
     * the completion record below may retire. */
    bm25_meta_read(index, &meta);
    if (nblocks == 0 || !bm25_orphan_sweep_needed(&meta, epoch))
    {
        UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
        return;
    }
    bm25_orphan_sweeps_run++;

    /* Test lever: park a VACUUM here, singleton held and the sweep committed to
     * running, nothing marked, so a test can cancel the sweep and check what cleanup
     * had already done before it (t/027), or insert past it (t/030). */
    bm25_debug_pause_point("orphan_sweep_start");

    /* The pending epoch floor for the per-page rule in the sweep loop, read under
     * the singleton from the SAME metapage read the marking below uses. While we hold
     * the singleton nothing can drain the chain or start a new one (a new chain needs
     * an empty one), so every page an appender adds from here on copies its epoch off
     * the chain's tail: by induction, the tail's epoch now, or 0 once an older binary
     * has written a 0 page into the chain. The head's epoch joins the min because a
     * chain is one epoch only while one binary generation wrote it; with no chain,
     * the floor is next_gen, which any chain started later draws at or above.
     * bm25_pending_epoch_floor. */
    e_floor = bm25_pending_epoch_floor(index, &meta);

    reachable = (uint8 *) palloc0(reach_bytes(nblocks));
    reach_set(reachable, BM25_METAPAGE_BLKNO);

    mark_chain(index, meta.pending_head, reachable, nblocks);
    mark_chain(index, meta.segcat_root, reachable, nblocks);
    mark_chain(index, meta.retired_head, reachable, nblocks);
    /* v4: the per-index field-config chain (BM25_PAGE_FIELDCFG, seg_gen = 0) is
     * rooted ONLY at meta->field_config_blkno -- it hangs off no segment header and
     * is not on the pending/segcat/retired chains. Mark it reachable here so the
     * sweep never frees it; it is written once at CREATE INDEX and read-only
     * thereafter (Contract section D). InvalidBlockNumber (a v3-equivalent single default
     * field with no page) makes mark_chain a no-op. The chain follows
     * BM25PageOpaque.nextblk like every other chain, so a future multi-page config
     * (M5, up to BM25_MAX_FIELDS) is covered without change. */
    mark_chain(index, meta.field_config_blkno, reachable, nblocks);

    bm25_segcat_read_locked(index, &segs, &nsegs);
    for (s = 0; s < nsegs; s++)
    {
        BM25SegmentHeader   h;

        /* BEFORE the bitmap write, not after. header_blkno arrives by raw copy
         * off the segment catalog and the bitmap is sized to nblocks, so an
         * unbounded write here lands at an arbitrary offset past a palloc'd
         * array. Note the ordering that made this reachable at all: the
         * bm25_seg_header_read on the next line is what would have rejected the
         * block (its ReadBuffer errors past the extent), so a corrupt entry
         * always failed -- one statement too late, after the out-of-bounds
         * store. Validating first therefore changes no successful behaviour. */
        bm25_segment_blkno_validate(segs[s].header_blkno, nblocks,
                                    "segment catalog entry");
        reach_set(reachable, segs[s].header_blkno);
        bm25_seg_header_read(index, segs[s].header_blkno, segs[s].gen, &h);
        mark_chain(index, h.dict_root, reachable, nblocks);
        mark_chain(index, h.norms_root, reachable, nblocks);
        mark_chain(index, h.livedocs_root, reachable, nblocks);
        mark_chain(index, h.docmap_root, reachable, nblocks);
        /* D-POST: the whole segment's postings are one chain -- mark it once. */
        mark_chain(index, h.posts_root, reachable, nblocks);
        /* These per-segment chains hang off the segment header exactly like the five
         * v3 roots, so they must be marked here or a sweep would free live pages.
         * pos_root is a REAL live root under M4 (C-POS-BUILD) when the index stores
         * positions (Invalid otherwise), keymap_root a REAL live root under M5 when the
         * index has key_field (Invalid otherwise) -- marking each keeps a live segment's
         * POS / KEYMAP pages out of the orphan free set; an Invalid root is a no-op. */
        mark_chain(index, h.pos_root, reachable, nblocks);
        mark_chain(index, h.keymap_root, reachable, nblocks);
    }

    /* PHASE 4: mark the SEGMENT pages each retired RANGE descriptor points at as
     * reachable. mark_chain(meta.retired_head) above marks only the descriptor
     * (BM25_PAGE_RETIRED) pages themselves; the dropped segments' DATA pages are
     * NOT on that chain and carry no per-page DELETED flag at swap time
     * (D-RETIRE), so without this they would look like orphans and be freed
     * prematurely -- the use-after-free the retired list exists to prevent. We
     * therefore walk the retired list, and for each BM25RetiredEntry mark its
     * header block + all seven chain roots (dict, norms, livedocs, docmap, posts,
     * keymap, pos -- every root in BM25RetiredEntry) reachable. Those pages are
     * freed LATER by bm25_reclaim_retired once retire_xid clears the horizon
     * (Task 25), never by this orphan sweep. The retired descriptors carry the
     * chain roots directly (no header re-read needed). */
    {
        BlockNumber rblk = meta.retired_head;
        uint64      rvisited = 0;

        while (rblk != InvalidBlockNumber && rblk < nblocks)
        {
            Buffer              buf;
            Page                pg;
            BM25RetiredEntry   *arr;
            BlockNumber         next;
            int                 nent;
            int                 k;

            /* VACUUM-only path (see mark_chain above); throttling form. Lock-free
             * here too -- buf is always released before the next iteration. */
            BM25_VACUUM_DELAY_POINT();

            /* mark_chain(meta.retired_head) above stops at a cycle; this walk would
             * not. Unreachable from VACUUM today, whose bm25_reclaim_retired walks the
             * same list first under the same cap, so this one is defense in depth for
             * a caller without that order. */
            bm25_retired_cycle_cap_validate(rvisited, nblocks, rblk);
            rvisited++;

            buf = ReadBuffer(index, rblk);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            pg = BufferGetPage(buf);
            /* Unlike mark_chain's walk (any block, cycle-guarded by reachable[]),
             * this chain is followed straight from meta.retired_head, which by
             * construction only ever links BM25_PAGE_RETIRED pages -- same
             * reasoning as bm25_reclaim_retired's identical check. A corrupt or
             * stray nextblk landing on an unrelated live page would otherwise be
             * read straight through as a BM25RetiredEntry array.
             *
             * entries packed contiguously from PageGetContents up to pd_lower;
             * bm25_page_content_bytes bounds pd_lower before it drives this
             * division -- a corrupt pd_lower below SizeOfPageHeaderData would
             * otherwise underflow the bare subtraction this used to be, and the
             * signed/unsigned conversion in the division that followed could hand
             * nent an effectively arbitrary value instead of erroring. It runs
             * BEFORE the opaque read: it is the page entry check, and it rejects a
             * never-initialized page whose opaque would be its header. */
            nent = bm25_page_content_bytes(pg) / sizeof(BM25RetiredEntry);
            bm25_retired_page_flags_validate(BM25PageGetOpaque(pg)->flags, rblk);
            arr = (BM25RetiredEntry *) PageGetContents(pg);
            /* nent does not size a fixed stack array here (unlike
             * bm25_reclaim_retired's keep[]/drop[]), but an overstated count would
             * still read arr[] past this page's real entries into page slack and
             * chase whatever garbage chain-root block numbers happen to be there,
             * marking arbitrary pages reachable. Bound it the same way
             * bm25_reclaim_retired does. (The per-entry header_blkno is bounded
             * separately, inside the loop -- an in-range nent still carries
             * arbitrary block numbers when the page itself is corrupt.) */
            bm25_retired_count_validate(nent, rblk);
            for (k = 0; k < nent; k++)
            {
                /* Was a silent `if (... < nblocks)` skip. Now the same loud
                 * validator the segment-catalog loop above uses -- the two read
                 * the same field into the same array and must not drift. Skipping
                 * quietly is the worse option of the two: it leaves the sweep
                 * freeing pages on the strength of a descriptor it has just
                 * decided not to trust. Under the page's SHARE lock, like the two
                 * validators above it; transaction abort releases it. */
                bm25_segment_blkno_validate(arr[k].header_blkno, nblocks,
                                            "retired segment descriptor");
                reach_set(reachable, arr[k].header_blkno);
                mark_chain(index, arr[k].dict_root, reachable, nblocks);
                mark_chain(index, arr[k].norms_root, reachable, nblocks);
                mark_chain(index, arr[k].livedocs_root, reachable, nblocks);
                mark_chain(index, arr[k].docmap_root, reachable, nblocks);
                /* D-POST: one shared postings chain per retired segment. */
                mark_chain(index, arr[k].posts_root, reachable, nblocks);
                /* M5: keep the retired segment's KEYMAP chain out of the orphan free
                 * set too -- same premature-free hazard as the other roots. Invalid
                 * (no key_field) => mark_chain no-op. Freed later by reclaim_one_range
                 * once retire_xid clears the horizon. */
                mark_chain(index, arr[k].keymap_root, reachable, nblocks);
                /* M4 (C-POS-MERGE): twin the KEYMAP mark for the retired segment's POS
                 * chain -- without this a dropped position-bearing segment's POS pages
                 * look like orphans and get freed while still referenced. Invalid (no
                 * positions) => no-op. Freed later by reclaim_one_range. */
                mark_chain(index, arr[k].pos_root, reachable, nblocks);
            }
            next = BM25PageGetOpaque(pg)->nextblk;
            UnlockReleaseBuffer(buf);
            rblk = next;
        }
    }

    /* Test lever (t/031): marking is done; an INSERT parked here lands on pages the
     * reachable[] snapshot calls unreachable, which is what the per-page rule below
     * is for. */
    bm25_debug_pause_point("orphan_sweep_marked");

    /* Sweep: STAMP each unreachable orphan BM25_PAGE_DELETED and record it free.
     * Stamp-and-gate (this file's header): the crash-unsafe FSM
     * hint alone cannot prove a returned page is free, so the gated allocator
     * (bm25_page_alloc) reuses ONLY pages carrying the on-page DELETED mark; an orphan
     * recorded free WITHOUT the mark would be rejected as "live" and the relation would
     * grow unboundedly (the 18_vacuum_reclaim regression). We therefore re-lock each
     * orphan EXCLUSIVE and stamp it before RecordFreeIndexPage.
     *
     * BM25_PAGE_DELETED guard (Task-18 sweep spec; plan D-RETIRE): a page in a
     * Phase-4 retired segment range carries BM25_PAGE_DELETED + a retire_xid whose
     * horizon (bm25_reclaim_retired), NOT this sweep, owns its lifetime -- a pre-swap
     * scan snapshot may still be walking it, so re-stamping or re-recording it here is
     * pointless work but harmless (RecordFreeIndexPage is idempotent and the
     * already-set retire_xid is preserved by NOT re-stamping). We thus skip the stamp
     * on an already-DELETED page, but still record it free idempotently. This guard
     * is NECESSARY but NOT sufficient for Phase 4: the in-swap retire helper stamps no
     * per-page DELETED (4-buffer Generic WAL cap, D-RETIRE), so a freshly-retired-but-
     * unstamped page is protected from premature freeing by the retired-RANGE marking
     * above (it is reachable[] there), not by this flag.
     *
     * PageIsNew guard: an unreachable block can be a never-initialized zero page.
     * bm25_page_alloc physically extends the relation (P_NEW) before the caller
     * PageInits it under Generic WAL, so a crash in that window leaves a zero-filled
     * block in [1, nblocks). Its pd_special is 0, which would trip
     * PageGetSpecialPointer's (pd_special >= SizeOfPageHeaderData) assertion inside
     * BM25PageGetOpaque on an --enable-cassert build, and it has no special area to
     * stamp -- so we DO NOT stamp a zero page; we record it free directly. The
     * allocator accepts a PageIsNew page without a mark (it has no stale live data),
     * so leaving it unstamped is correct. This is the only site that dereferences the
     * opaque of an ARBITRARY, possibly-uninitialized block, so the guard lives here.
     *
     * UNKNOWN-KIND guard (BM25_PAGE_ALL_KNOWN, bm25_format.h): the root set above is
     * hardcoded at compile time, so this sweep's reachability is CLOSED-WORLD -- it can
     * only ever reach page kinds this build knows. ADR 0009 classifies "a new page type
     * old binaries never follow a link to" as ADDITIVE, meaning the floor gate lets THIS
     * (older) binary accept and write an index whose newer chains it cannot walk. Those
     * pages are unreachable here purely out of ignorance, not because they are dead, and
     * freeing one hands a live chain to the next bm25_page_alloc (a non-pending orphan
     * stamp carries InvalidFullTransactionId => immediate reuse, no horizon wait). We
     * therefore LEAK any page carrying a flag bit outside the mask rather than free it:
     * the additive contract is only true because of this check. A binary that knows the
     * kind reaches it from its own root set and reclaims it normally.
     *
     * HORIZON ASYMMETRY -- PENDING orphans, and only those, get a real retire_xid
     * (issue #135). This sweep is the SECOND way a drained pending page reaches the
     * FSM: bm25_pending_truncate is the opportunistic recycler, but it is skipped or
     * unwound whenever the seal errors, crashes, or loses the opportunistic
     * ConditionalLockPage race -- and then the detached chain sits here as an orphan
     * until VACUUM. So fixing only the truncate stamp would leave the identical
     * use-after-free reachable through VACUUM instead. The metapage singleton this
     * sweep holds does NOT close it: scans never take that lock, and a scan holds a
     * pre-seal pending_head across an arbitrarily long walk (bm25_scan_snapshot
     * releases the metapage immediately and reads the chain page by page afterwards).
     *
     * Every OTHER orphan kind keeps the immediate-reuse stamp, each for its own
     * reason. Segment pages carry a per-page seg_gen that bm25_seg_page_validate
     * checks on every read, so a stale reader that lands on a recycled-and-re-inited
     * segment page fails loudly instead of decoding garbage. A pending page's seg_gen
     * is only its chain's epoch (#291): a scan rejects a page re-inited after its
     * snapshot with a 40001, which is right for a standby but must never fire on the
     * primary, so pending orphans keep the horizon. Catalog pages carry seg_gen = 0,
     * so they have no such check either; what protects them is that no production reader walks an old
     * catalog chain into this sweep (issue #270). bm25_scan_snapshot and
     * bm25_segcat_first_entry read the catalog under the metapage SHARE that the
     * swap's EXCLUSIVE excludes, while bm25_segcat_read_locked's callers and
     * bm25_segcat_locate_entry's (bm25_livedocs_clear, under bm25_bulkdelete's
     * ShareLock) hold the singleton across their walk. Holding it, they may run
     * beside this share-mode sweep, but they took it after the swap that orphaned
     * any chain this sweep frees (the swap holds it EXCLUSIVE), so they read the new
     * root. The unlocked debug SRFs are the exception (bm25.h).
     *
     * THE PER-PAGE RULE (issue #300): an unreachable PENDING page that is not DELETED
     * is SKIPPED, not freed, when its seg_gen is 0 or at least e_floor. In share mode
     * the reachable[] snapshot is exact for everything except the pending chain: an
     * appender (ShareLock, compatible) can pop a page that was DELETED and unreachable
     * when we marked, re-init it and link it, or extend past a mark_chain that
     * stopped at nblocks and link FSM pages after that. Every page an appender inits
     * while we hold the singleton is PENDING, stamped with an epoch at or above
     * e_floor -- the current chain's, or a new chain's drawn from next_gen -- or 0
     * once an older binary has put a 0 page into the chain and its successors copy
     * it; see the e_floor computation above. Every DEAD pending page has a smaller,
     * nonzero epoch: epochs are drawn from next_gen and bumped per chain, so an
     * earlier chain's are below every later chain's, and a dead page belongs to a
     * chain that was drained (a truncate leftover). The rule therefore never stamps a
     * page an appender owns, and frees every dead pending page except a legacy
     * epoch-0 one, which leaks. We decide under the page's EXCLUSIVE content lock,
     * and bm25_page_alloc hands the appender its page EXCLUSIVE-locked and the
     * appender keeps that lock through its one WAL record (bm25_pending_append_multi),
     * so we see either the old DELETED or zero page or the finished new one, never a
     * half-written page.
     *
     * What the rule does NOT prevent is the harmless half of the race: we can lock a
     * DELETED (or zero) page between an appender's GetFreeIndexPage pop and its
     * ConditionalLockBuffer, and re-record it free just before it becomes live. That
     * FSM entry is then a stale hint, which stamp-and-gate already tolerates (the FSM
     * is not crash-safe either): bm25_page_alloc hands out only DELETED or zero pages
     * and drops anything else it pops, so a later pop of the now-live page is
     * rejected, never re-handed out.
     *
     * One horizon for the whole sweep (read before the
     * loop, outside every buffer lock -- ReadNextFullTransactionId takes
     * XidGenLock): these orphans are all being declared dead at one instant. */
    pending_retire = ReadNextFullTransactionId();

    for (blk = 1; blk < nblocks; blk++)
    {
        Buffer  buf;
        Page    page;

        /* The full-extent sweep: this is where the real per-page I/O and the
         * bulk of the reclaim's cost is, so it gets the same throttling form as
         * bm25_bulkdelete (ADR 0025), placed before the reachable[] skip so a
         * relation that is mostly-live still makes cancellable, throttled
         * progress across the whole extent rather than free-running through
         * every skip. Unconditionally lock-free: no buffer is held between one
         * iteration's UnlockReleaseBuffer/continue and the next iteration's top. */
        BM25_VACUUM_DELAY_POINT();

        if (reach_test(reachable, blk))
            continue;

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        page = BufferGetPage(buf);
        if (!PageIsNew(page))
        {
            uint16 flags;

            /* #302.D: the flags below, and the stamp, sit at pd_special. */
            bm25_page_special_validate(page, blk);
            flags = BM25PageGetOpaque(page)->flags;

            if (flags & ~((uint16) BM25_PAGE_ALL_KNOWN))
            {
                /* A newer binary's page kind -- leak, never free. */
                UnlockReleaseBuffer(buf);
                continue;
            }
            if ((flags & (BM25_PAGE_PENDING | BM25_PAGE_DELETED)) == BM25_PAGE_PENDING)
            {
                uint32 page_epoch = BM25PageGetOpaque(page)->seg_gen;

                /* Possibly a concurrent appender's live page: the per-page rule
                 * above. Leak it; a later sweep frees it if it is dead. */
                if (page_epoch == 0 || page_epoch >= e_floor)
                {
                    UnlockReleaseBuffer(buf);
                    continue;
                }
            }
            if (!(flags & BM25_PAGE_DELETED))
                bm25_page_mark_deleted(index, buf,
                                       (flags & BM25_PAGE_PENDING)
                                       ? pending_retire
                                       : InvalidFullTransactionId);
        }
        UnlockReleaseBuffer(buf);
        RecordFreeIndexPage(index, blk);
    }
    pfree(reachable);

    /* No IndexFreeSpaceMapVacuum here: bm25_vacuumcleanup, this function's only
     * caller, vacuums the whole map after it on every run, swept or not. */

    /* Retire the evidence this pass was run for. Whatever brackets were open when we
     * read the metapage under the singleton belong to ops that died, or to a swap's
     * deliberately unclosed bracket, and the sweep just freed what they left; none
     * can have opened since, because each takes the singleton EXCLUSIVE. So done may
     * catch up to begun, and swept_epoch record this server lifetime. In place under
     * the metapage EXCLUSIVE content lock, like every metapage field writer that can
     * run beside an appender (whose own read-modify-write of the struct holds that
     * lock throughout). */
    {
        Buffer              metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
        GenericXLogState   *st;
        BM25MetaPageData   *m;
        Page                mp;

        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        st = GenericXLogStart(index);
        mp = GenericXLogRegisterBuffer(st, metabuf, 0);
        m = BM25PageGetMeta(mp);
        Assert(m->orphan_ops_begun == meta.orphan_ops_begun);   /* checked: the singleton */
        m->orphan_ops_done = m->orphan_ops_begun;
        m->swept_epoch = epoch;
        bm25_meta_set_pd_lower(mp);
        GenericXLogFinish(st);
        UnlockReleaseBuffer(metabuf);
    }

    UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
}

/* bm25_debug_sweep_evidence(index regclass) -> (ops_begun, ops_done, swept_this_epoch)
 * -- TEST-ONLY. The orphan-sweep gate's inputs (issue #300): the metapage bracket
 * counters, and whether the last completed sweep ran in this server lifetime. The raw
 * epoch is not shown -- it is random, so no expected output could hold it, and every
 * question a suite asks is "does it match". */
PG_FUNCTION_INFO_V1(bm25_debug_sweep_evidence);
Datum
bm25_debug_sweep_evidence(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index;
    BM25MetaPageData    meta;
    uint64              epoch;
    TupleDesc           tupdesc;
    Datum               values[3];
    bool                nulls[3] = {false, false, false};

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "bm25_debug_sweep_evidence: return type must be a row type");
    epoch = bm25_crash_epoch();     /* before any lock: see its header */
    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_meta_read(index, &meta);
    index_close(index, AccessShareLock);

    values[0] = Int64GetDatum((int64) meta.orphan_ops_begun);
    values[1] = Int64GetDatum((int64) meta.orphan_ops_done);
    values[2] = BoolGetDatum(epoch != 0 && meta.swept_epoch == epoch);
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc),
                                                      values, nulls)));
}

/* bm25_debug_orphan_sweeps() -> bigint -- TEST-ONLY. How many orphan sweeps THIS
 * backend has run past the gate (issue #300): a VACUUM that sweeps moves it by one, a
 * gated-off one does not. Process-local, so it is read in the session that ran the
 * VACUUM. */
PG_FUNCTION_INFO_V1(bm25_debug_orphan_sweeps);
Datum
bm25_debug_orphan_sweeps(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64(bm25_orphan_sweeps_run);
}

/* bm25_debug_set_pending_tail_epoch(index regclass, epoch bigint) -> bigint --
 * TEST-ONLY, MUTATING. Overwrite the pending tail page's seg_gen (its #291 chain
 * epoch) and return the old value, or NULL when the chain is empty. Emulates an older
 * binary that appended a page stamped 0 to a chain a newer binary started: every
 * later append then copies that 0 off the tail, which the orphan sweep's per-page rule
 * must cover (issue #300). Holds the seal singleton in ShareLock, as an appender does,
 * so no seal can drain the chain under it. Owner-only (bm25_index_open_owned). */
PG_FUNCTION_INFO_V1(bm25_debug_set_pending_tail_epoch);
Datum
bm25_debug_set_pending_tail_epoch(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int64               epoch = PG_GETARG_INT64(1);
    Relation            index = bm25_index_open_owned(relid, RowExclusiveLock);
    BM25MetaPageData    meta;
    Buffer              buf;
    GenericXLogState   *st;
    Page                pg;
    int64               old;

    if (epoch < 0 || epoch > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_set_pending_tail_epoch: epoch %lld out of range",
                        (long long) epoch)));
    LockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
    bm25_meta_read(index, &meta);
    if (meta.pending_tail == InvalidBlockNumber)
    {
        UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
        index_close(index, RowExclusiveLock);
        PG_RETURN_NULL();
    }
    buf = ReadBuffer(index, meta.pending_tail);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    if (!(BM25PageGetOpaque(BufferGetPage(buf))->flags & BM25_PAGE_PENDING))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25_debug_set_pending_tail_epoch: block %u is not a pending page",
                        meta.pending_tail)));
    st = GenericXLogStart(index);
    pg = GenericXLogRegisterBuffer(st, buf, 0);
    old = (int64) BM25PageGetOpaque(pg)->seg_gen;
    BM25PageGetOpaque(pg)->seg_gen = (uint32) epoch;
    GenericXLogFinish(st);
    UnlockReleaseBuffer(buf);
    UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
    index_close(index, RowExclusiveLock);
    PG_RETURN_INT64(old);
}

/* -------------------------------------------------------------------------
 * bm25_debug_alloc_unknown_page(index regclass) -> int -- TEST-ONLY.
 * Emulates a future additive (v7-style) NEW PAGE KIND that THIS build cannot reach:
 * allocates one page and inits it with a synthetic flag bit (1 << 13) that no
 * BM25_PAGE_* define claims and BM25_PAGE_ALL_KNOWN therefore excludes. The page is
 * deliberately linked from NOTHING -- which is exactly how a v7 chain looks to a v6
 * binary, since v6 has no root to reach it from and its sweep is closed-world.
 *
 * Paired with bm25_debug_page_flags, this is the discriminating probe for the
 * unknown-kind guard in bm25_reclaim_orphans: with the guard, VACUUM must leave the
 * page's flags untouched; without it, the sweep stamps BM25_PAGE_DELETED and hands
 * the page to the next allocator caller -- the silent corruption ADR 0009's additive
 * contract depends on this not happening. Never reachable from the query path.
 * -------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(bm25_debug_alloc_unknown_page);
Datum
bm25_debug_alloc_unknown_page(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index = bm25_index_open_owned(relid, RowExclusiveLock);
    Buffer              buf;
    Page                pg;
    GenericXLogState   *st;
    BlockNumber         blk;

    buf = bm25_page_alloc(index, NULL);     /* EXCL-locked */
    st  = GenericXLogStart(index);
    pg  = GenericXLogRegisterBuffer(st, buf, GENERIC_XLOG_FULL_IMAGE);
    bm25_page_init(pg, (uint16) (1 << 13)); /* outside BM25_PAGE_ALL_KNOWN */
    blk = BufferGetBlockNumber(buf);
    GenericXLogFinish(st);
    UnlockReleaseBuffer(buf);

    index_close(index, RowExclusiveLock);
    PG_RETURN_INT32((int32) blk);
}

/* bm25_debug_page_flags(index regclass, blkno int) -> int -- TEST-ONLY.
 * The raw BM25PageOpaque.flags of one page. Lets the sweep suites assert what a
 * VACUUM did (or did not do) to a specific page without pageinspect, which the
 * gating cassert+UBSan CI job lacks (it builds PostgreSQL from source without
 * contrib). */
PG_FUNCTION_INFO_V1(bm25_debug_page_flags);
Datum
bm25_debug_page_flags(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    int32       blkno = PG_GETARG_INT32(1);
    Relation    index = bm25_index_open_readable(relid, AccessShareLock);
    Buffer      buf;
    Page        pg;
    int32       flags;

    if (blkno < 0 || (BlockNumber) blkno >= RelationGetNumberOfBlocks(index))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_page_flags: block %d out of range", blkno)));

    buf = ReadBuffer(index, (BlockNumber) blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    flags = PageIsNew(pg) ? 0 : (int32) BM25PageGetOpaque(pg)->flags;
    UnlockReleaseBuffer(buf);

    index_close(index, AccessShareLock);
    PG_RETURN_INT32(flags);
}

/* bm25_debug_page_seg_gen(index regclass, blkno int) -> bigint -- TEST-ONLY.
 * The raw BM25PageOpaque.seg_gen of one page: a segment page's generation, a pending
 * page's chain epoch (#291), 0 for the rest. bigint because seg_gen is a uint32. Same
 * no-pageinspect motivation as bm25_debug_page_flags; a never-initialized page
 * reports 0. */
PG_FUNCTION_INFO_V1(bm25_debug_page_seg_gen);
Datum
bm25_debug_page_seg_gen(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    int32       blkno = PG_GETARG_INT32(1);
    Relation    index = bm25_index_open_readable(relid, AccessShareLock);
    Buffer      buf;
    Page        pg;
    int64       gen;

    if (blkno < 0 || (BlockNumber) blkno >= RelationGetNumberOfBlocks(index))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_page_seg_gen: block %d out of range", blkno)));

    buf = ReadBuffer(index, (BlockNumber) blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    gen = PageIsNew(pg) ? 0 : (int64) BM25PageGetOpaque(pg)->seg_gen;
    UnlockReleaseBuffer(buf);

    index_close(index, AccessShareLock);
    PG_RETURN_INT64(gen);
}

/* bm25_debug_page_retire_xid_valid(index regclass, blkno int) -> bool -- TEST-ONLY.
 * Whether one page's BM25PageOpaque.retire_xid is a valid FullTransactionId, i.e.
 * whether its reuse is horizon-GATED (true) or immediate (false). Deliberately a
 * bool and not the raw xid8: the actual value is whatever the cluster's XID counter
 * happened to be, so it could never appear in expected output, and every question the
 * suites ask is about the gate, not the number.
 *
 * This is the only way to observe issue #135's fix from SQL. A drained pending page
 * keeps BM25_PAGE_PENDING when bm25_page_mark_deleted ORs in BM25_PAGE_DELETED, so
 * (flags & (DELETED|PENDING)) identifies exactly the recycled pending pages and this
 * function then asserts each one is horizon-gated -- a timing-free, exact assertion.
 * Same no-pageinspect motivation as bm25_debug_page_flags above. */
PG_FUNCTION_INFO_V1(bm25_debug_page_retire_xid_valid);
Datum
bm25_debug_page_retire_xid_valid(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    int32       blkno = PG_GETARG_INT32(1);
    Relation    index = bm25_index_open_readable(relid, AccessShareLock);
    Buffer      buf;
    Page        pg;
    bool        valid;

    if (blkno < 0 || (BlockNumber) blkno >= RelationGetNumberOfBlocks(index))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_page_retire_xid_valid: block %d out of range",
                        blkno)));

    buf = ReadBuffer(index, (BlockNumber) blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    /* A never-initialized zero page has no special area to read (and would trip
     * PageGetSpecialPointer's assertion under cassert) -- report it ungated, the
     * same way bm25_debug_page_flags reports flags 0 for it. */
    valid = PageIsNew(pg)
        ? false
        : FullTransactionIdIsValid(BM25PageGetOpaque(pg)->retire_xid);
    UnlockReleaseBuffer(buf);

    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(valid);
}

/* bm25_debug_pending_head(index regclass) -> bigint -- TEST-ONLY.
 * The metapage's pending_head, or NULL for InvalidBlockNumber (an empty/detached
 * pending list). bm25_stats() reports pending_ndocs but not the anchor itself, and
 * pending_ndocs is useless for the assertion issue #131 needs: VACUUM's pending
 * sweep has already decremented it to 0 by the time the defective seal runs, so it
 * reads the same before and after the fix. The anchor block number is the state
 * that actually latches, so it is the state the suite has to see. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_head);
Datum
bm25_debug_pending_head(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index = bm25_index_open_readable(relid, AccessShareLock);
    BlockNumber         head;

    bm25_meta_read_pending_head(index, &head);
    index_close(index, AccessShareLock);

    if (head == InvalidBlockNumber)
        PG_RETURN_NULL();
    PG_RETURN_INT64((int64) head);
}

/* bm25_debug_npages(index regclass) -> RelationGetNumberOfBlocks. The
 * pg_freespacemap-free probe the VACUUM-reclaim suite uses to assert the relation
 * does not grow unboundedly across repeated seal/reclaim cycles. */
PG_FUNCTION_INFO_V1(bm25_debug_npages);
Datum
bm25_debug_npages(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    Relation    index = bm25_index_open_readable(relid, AccessShareLock);
    int64       n = (int64) RelationGetNumberOfBlocks(index);

    index_close(index, AccessShareLock);
    PG_RETURN_INT64(n);
}

/* bm25_retire_segment -- the IN-SWAP range-append helper (D-RETIRE/R4).
 *
 * This is NOT a standalone post-swap pass. It is invoked by
 * bm25_segcat_publish_swap WHILE the swap's single GenericXLog is open,
 * once per dropped segment, with the retired-list TAIL page already registered in
 * that record (the caller opened/extended it once and registers the metapage too),
 * and `retire_xid` already captured INSIDE the metapage exclusive-lock window (that
 * lock is what orders the capture -- neither the caller nor this helper opens a
 * critical section of its own; the only one in play is GenericXLogFinish's). It
 * writes ONE BM25RetiredEntry RANGE record for the dropped segment into the
 * caller-supplied tail page (`rpage`) at slot `*pn`, then bumps `*pn` and the
 * page's pd_lower.
 *
 * PURE MEMORY -- NO buffer access. The swap's Generic WAL window must be throw-free
 * (m2a.md:6692: no ReadBuffer/LockBuffer/palloc/ereport between GenericXLogStart and
 * GenericXLogFinish). So the segment's chain roots are NOT read here; the CALLER
 * pre-reads each dropped segment's header BEFORE opening the window and passes the
 * harvested roots in via `roots` (a BM25SegmentHeader the caller already populated).
 * This helper only copies those roots + seg->gen + retire_xid into the registered
 * page. It does NOT:
 *   - read any buffer (header pre-read by the caller, outside the window);
 *   - walk the segment's pages (reclaim does that, after the horizon passes);
 *   - set op->nextblk = Invalid on any page (D-RETIRE/M7: a pre-swap scan may
 *     still be walking the chain; links are reset only by the FPI re-init in
 *     bm25_page_alloc on eventual reuse);
 *   - per-page-stamp BM25_PAGE_DELETED at swap time (a whole segment's pages
 *     cannot fit the 4-buffer cap, and it is unnecessary -- the RANGE entry is the
 *     source of truth, and option-(d) seg_gen re-stamping protects stale readers).
 *
 * NOTE: no GenericXLogFinish here -- the caller's swap record finishes it, making
 * the retire crash-atomic with the catalog flip (R4). */
void
bm25_retire_segment(Page rpage, int *pn, BM25SegCatEntry *seg,
                    const BM25SegmentHeader *roots, FullTransactionId retire_xid)
{
    BM25RetiredEntry   *arr = (BM25RetiredEntry *) PageGetContents(rpage);
    int                 n = *pn;

    /* checked: every caller caps its drop count at BM25_RETIRED_PER_PAGE before
     * opening the window -- the merge path implicitly via BM25_MERGE_MAX_INPUTS, and
     * bm25_merge_rewrite_all explicitly (bm25_merge.c's nsegs ereport). It stays an
     * Assert and not an ereport because this runs INSIDE the swap's Generic WAL
     * window, where throwing is what the throw-free rule forbids; the enforcement
     * therefore has to live at the callers, which is where it does live. */
    Assert(n < BM25_RETIRED_PER_PAGE);  /* checked: the callers' drop-count caps */

    /* Zero the slot before filling it (PEND-22). BM25RetiredEntry has a 4-byte hole at
     * offset 36 -- uint32 gen at 32, then FullTransactionId retire_xid needing
     * 8-alignment -- that the ten assignments below never write.
     *
     * This is HARDENING, not a live leak, and the distinction is worth stating so the
     * next reader does not go looking for a disclosure that is not there. The only
     * caller passes a page that bm25_page_init zeroed on its REGISTERED copy inside the
     * same Generic WAL window, and this fill is IN PLACE, so today those four bytes are
     * page-derived zeros rather than stack residue. What is wrong with relying on that
     * is the shape of the reliance: correctness-by-remote-invariant, where the invariant
     * lives in a different file and nothing connects the two. The compaction path below
     * leans on something weaker still -- `arr[w] = keep[w]` is a whole-struct assignment,
     * and C does not specify that struct assignment copies padding at all.
     *
     * Cost is a 48-byte memset per retired segment, on the merge path. */
    memset(&arr[n], 0, sizeof(arr[n]));
    arr[n].header_blkno  = seg->header_blkno;
    arr[n].dict_root     = roots->dict_root;
    arr[n].norms_root    = roots->norms_root;
    arr[n].livedocs_root = roots->livedocs_root;
    arr[n].docmap_root   = roots->docmap_root;
    arr[n].posts_root    = roots->posts_root;
    /* M5: the dropped segment's KEYMAP chain root. Real (not Invalid) when the index
     * has key_field; reclaim_one_range frees this chain once retire_xid clears the
     * horizon. Harvested from roots (a pre-window bm25_seg_header_read, which memcpy's
     * the whole header incl. keymap_root), so copying it here stays pure-memory -- the
     * swap's throw-free Generic WAL window is preserved. */
    arr[n].keymap_root   = roots->keymap_root;
    /* M4 (C-POS-MERGE): copy the dropped segment's POS chain root (harvested from the
     * same pre-window bm25_seg_header_read into `roots`, so this stays pure-memory and
     * the swap's throw-free WAL window is preserved). Invalid when the segment carries
     * no positions; reclaim_one_range frees the chain once retire_xid clears. */
    arr[n].pos_root      = roots->pos_root;
    arr[n].gen           = seg->gen;
    arr[n].retire_xid    = retire_xid;
    *pn = n + 1;
    ((PageHeader) rpage)->pd_lower =
        (((char *) arr) + sizeof(BM25RetiredEntry) * (*pn)) - (char *) rpage;
}

/* The page kind each root of BM25RetiredEntry must reach, in reclaim_one_range's
 * roots[] order, and its name for the error message. Every chain of one segment carries
 * the same seg_gen, so the kind is what tells a root aimed at a sibling chain of the
 * same segment from the right one. */
static const uint16 reclaim_root_kind[8] = {
    BM25_PAGE_SEGCAT, BM25_PAGE_DICT, BM25_PAGE_NORMS, BM25_PAGE_LIVE,
    BM25_PAGE_DOCMAP, BM25_PAGE_POST, BM25_PAGE_KEYMAP, BM25_PAGE_POS
};
static const char *const reclaim_root_name[8] = {
    "header", "dictionary", "norms", "live-docs", "doc-map", "postings", "key-map",
    "positions"
};

/* The decode boundary for one page reclaim_one_range is about to free (issue #302 C).
 * Runs under the page's EXCLUSIVE content lock, before bm25_page_mark_deleted opens its
 * WAL window, so an ERROR here writes nothing to the page. A retired descriptor's roots
 * are copied from the segment header at swap time, and nothing re-reads them before this
 * walk, so a corrupt root or link would otherwise free block 0 or a live chain: the
 * stamped page clears the horizon gate at once and bm25_page_alloc re-inits it.
 *
 *   content bytes  bm25_page_content_bytes is the page entry check every opaque reader
 *                  runs first. It rejects a never-initialized page, which the old code
 *                  recorded free unstamped: a published segment's pages are all
 *                  initialized before its publish record, so a range reaching a zero
 *                  page is corrupt.
 *   kind           the chain's own kind (reclaim_root_kind). Unlike the read paths'
 *                  `(flags & want)`, DELETED ORed in is refused too, below.
 *   seg_gen        must equal the descriptor's gen. INDEX_CORRUPTED, not the
 *                  retryable 40001 bm25_seg_page_validate_kind raises: the range is past
 *                  the horizon and out of every catalog, so no race explains a mismatch.
 *   not DELETED    compact-then-free (ADR 0032) walks no range twice, and nothing else
 *                  stamps a retired range's pages, so a DELETED page here is either
 *                  corruption or this walk's own earlier stamp. The second case is what
 *                  makes this the cycle guard for the walk (issue #302 E): any in-range
 *                  cycle comes back to a page this call stamped, so the walk needs no
 *                  visit counter. */
static void
reclaim_page_validate(Page pg, BlockNumber blk, const BM25RetiredEntry *e, int c)
{
    uint16  flags;
    uint32  gen;

    (void) bm25_page_content_bytes(pg);
    flags = BM25PageGetOpaque(pg)->flags;
    gen = BM25PageGetOpaque(pg)->seg_gen;

    if ((flags & reclaim_root_kind[c]) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: retired segment %u reaches block %u, which is not a %s page",
                        e->gen, blk, reclaim_root_name[c]),
                 errdetail("Page flags are 0x%x; a %s page carries 0x%x.",
                           flags, reclaim_root_name[c], reclaim_root_kind[c])));
    if (gen != e->gen)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: retired segment %u reaches block %u, which belongs to "
                        "segment %u",
                        e->gen, blk, gen)));
    if (flags & BM25_PAGE_DELETED)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: retired segment %u reaches block %u, which is already freed",
                        e->gen, blk),
                 errdetail("The %s chain revisits a page this walk freed, or links to "
                           "one freed earlier.",
                           reclaim_root_name[c])));
}

/* Free every page of one retired segment: stamp each BM25_PAGE_DELETED + retire_xid
 * (so the allocator can later PROVE it reusable via stamp-and-gate) then
 * RecordFreeIndexPage. Reads one page at a time, EXCL-locked, releasing the page
 * lock BEFORE RecordFreeIndexPage (FSM fork only -- never nest the FREED PAGE'S OWN
 * lock with the FSM update that publishes it; PEND-19, issue #145: this rule governs
 * the FREE side only, and does not forbid the allocate-side FSM traffic that
 * bm25_page_alloc deliberately runs under a held metapage lock -- see
 * bm25_pending_append_multi). D-POST/M1: posts_root is the single shared POSTINGS chain, walked
 * once, so no page is double-freed. The segment is past the horizon (caller gated on
 * GlobalVisCheckRemovableFullXid) and out of every live catalog, so SHARE/EXCL access
 * is uncontended. A gen mismatch here is therefore never the concurrent-reclaim race
 * that option (d) retries on a read path: every page is checked by
 * reclaim_page_validate above, and any failure is corruption (issue #302 C).
 *
 * WHERE THE ERROR FIRES (issue #302 D3): here, page by page, AFTER the caller has
 * compacted the descriptor. Index WAL is physical, so the compaction record survives
 * the abort: the error fires once, the next VACUUM finds the range gone and proceeds,
 * and the pages not yet freed -- this range's remainder and every later range of the
 * same chunk -- are left to the orphan sweep, whose evidence the caller's open bracket
 * keeps set. ADR 0032 accepts that leak. Validating before compaction instead would fail
 * every VACUUM, and so every merge and orphan sweep behind it (ADR 0118's order), until
 * REINDEX.
 *
 * The seven chain roots (dict, norms, livedocs, docmap, posts, keymap, pos -- every
 * root in BM25RetiredEntry) each walk opaque->nextblk to the chain end;
 * header_blkno is a SINGLE SEGCAT-kind page (its nextblk was set Invalid at
 * build), so c==0 frees just it and never follows a link. We carry retire_xid (not Invalid) into the stamp so a
 * crash mid-reclaim leaves the not-yet-freed tail still horizon-gated, not prematurely
 * reusable. nextblk links survived retire (D-RETIRE/M7: retire reset no links).
 *
 * `nblocks` is threaded in by the caller (same shape as mark_chain's bound) rather
 * than re-derived here: bm25_reclaim_retired already computed it once under the
 * metapage singleton for its own chain bound, and this function is called once per
 * dropped range from inside that same locked pass, so the extent cannot have moved. */
static void
reclaim_one_range(Relation index, const BM25RetiredEntry *e, BlockNumber nblocks)
{
    BlockNumber roots[8];
    int         c;

    roots[0] = e->header_blkno;
    roots[1] = e->dict_root;
    roots[2] = e->norms_root;
    roots[3] = e->livedocs_root;
    roots[4] = e->docmap_root;
    roots[5] = e->posts_root;
    /* M5: the KEYMAP chain (docid->key). Invalid when the index has no key_field,
     * making the walk below a no-op -- back-compat with a keyless index. It is a real
     * opaque->nextblk chain like norms/docmap, so the generic while-loop frees it
     * (follows next, not treated as a single page like header_blkno at c==0). */
    roots[6] = e->keymap_root;
    /* M4 (C-POS-MERGE): the POS chain (BM25_PAGE_POS frames). Invalid when the segment
     * carries no positions => the walk is a no-op. A real opaque->nextblk chain like the
     * others, so the generic loop frees it. The loop bound is bumped 7->8 for this slot;
     * a missed bump would silently leak every merged segment's POS pages. */
    roots[7] = e->pos_root;
    for (c = 0; c < 8; c++)
    {
        BlockNumber blk = roots[c];

        /* Same out-of-extent backstop as mark_chain and the retired-list walks
         * in bm25_reclaim_orphans and bm25_reclaim_retired (each bounds its walk
         * with `blk < nblocks`): a corrupt nextblk cannot send this loop past the
         * relation's own extent; such a link ends the walk and leaks the rest of
         * the chain to the orphan sweep. An in-extent cycle is stopped by
         * reclaim_page_validate's DELETED test, not by this bound. */
        while (blk != InvalidBlockNumber && blk < nblocks)
        {
            Buffer      buf;
            BlockNumber next;
            Page        pg;

            CHECK_FOR_INTERRUPTS();

            /* Before ReadBuffer: the metapage is never part of a segment, and this
             * walk would otherwise stamp it DELETED (issue #302 C). */
            if (blk == BM25_METAPAGE_BLKNO)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: retired segment %u names the metapage as a %s page",
                                e->gen, reclaim_root_name[c])));

            buf = ReadBuffer(index, blk);
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            pg = BufferGetPage(buf);
            reclaim_page_validate(pg, blk, e, c);
            next = BM25PageGetOpaque(pg)->nextblk;
            bm25_page_mark_deleted(index, buf, e->retire_xid);
            UnlockReleaseBuffer(buf);
            RecordFreeIndexPage(index, blk);
            blk = (c == 0) ? InvalidBlockNumber : next; /* header is one page, not a chain */
        }
    }
}

/* The decode boundary for one page reached via the retired-list chain: it must
 * carry BM25_PAGE_RETIRED (this chain, unlike the orphan sweep, has no
 * independent root set to fall back on -- see the comment on the call site
 * below), and its declared entry count must not exceed what the fixed
 * keep[]/drop[] stack arrays (BM25_RETIRED_PER_PAGE wide) can hold. Extracted
 * so the debug probe below exercises the identical checks bm25_reclaim_retired
 * runs against a real page's opaque flags and decoded entry count. */
static void
bm25_retired_page_flags_validate(uint16 flags, BlockNumber blk)
{
    if (!(flags & BM25_PAGE_RETIRED))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: retired-list page %u is not a retired-list page (flags 0x%x)",
                        blk, flags)));
    /* Issue #302 E. bm25_reclaim_retired splices a drained descriptor out of the list
     * before it stamps it DELETED, so no page on the list is ever DELETED. Reaching one
     * means a link into a freed descriptor, and bm25_reclaim_retired would unlink and
     * free it again -- once per chunk, without end, if the link closes a cycle. */
    if (flags & BM25_PAGE_DELETED)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: retired-list page %u is already freed (flags 0x%x)",
                        blk, flags)));
}

static void
bm25_retired_count_validate(int n, BlockNumber blk)
{
    if (n > BM25_RETIRED_PER_PAGE)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: retired-list page %u claims %d entries, maximum is %d",
                        blk, n, (int) BM25_RETIRED_PER_PAGE)));
}

/* The visit cap every retired-list walk runs before reading a page (issue #302 E),
 * shaped like bm25_pending_cycle_cap_validate: `visited` is the number of pages the
 * walk has already read. The `blk < nblocks` bound on each walk stops a link out of the
 * extent but not an in-extent cycle, and bm25_reclaim_retired walks this list holding
 * the singleton ExclusiveLock, which every inserter that adds a document waits for. A
 * cycle there was only cancellable, and autovacuum retries it every naptime. The cap
 * covers one walk; bm25_reclaim_retired restarts its walk per chunk, so it also needs
 * its head-revisit test and the DELETED test above.
 *
 * A count, not a reachable[] bitmap, because nblocks bounds the distinct pages a walk
 * can legally read, and block 0 is never on the list: nblocks - 1 pages, plus the one
 * page bm25_reclaim_retired re-reads after opening its orphan bracket. So `visited >=
 * nblocks` before a read proves a repeat. */
static void
bm25_retired_cycle_cap_validate(uint64 visited, BlockNumber nblocks, BlockNumber blk)
{
    if (visited >= (uint64) nblocks)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: retired list revisits blocks; stopped at block %u after "
                        UINT64_FORMAT " pages (relation has %u)",
                        blk, visited, nblocks)));
}

/* A segment header block number taken off the segment catalog or a retired
 * descriptor, bounded before it indexes the orphan sweep's reachable[] bitmap.
 * Same trust class as the two validators above -- a raw on-disk value driving a
 * memory access -- so it gets the same treatment.
 *
 * ERROR rather than a silent skip, deliberately, and applied to BOTH loops so
 * they cannot drift apart again. The sweep FREES every page this bitmap leaves
 * unmarked, so an entry it cannot trust is not one it may quietly drop from the
 * reachable set: that is precisely how a live page reaches the FSM and gets
 * re-inited under a running scan. Erroring also matches the sibling checks in
 * the same loop (bm25_retired_page_flags_validate, bm25_retired_count_validate),
 * which already abort a VACUUM on a corrupt retired page.
 *
 * A block past the extent is unambiguously corruption here, never a legitimate
 * stale pointer: nothing in this extension ever shrinks the index relation
 * (there is no RelationTruncate/smgrtruncate anywhere in src/), so the extent
 * only grows and a descriptor written when the index was smaller still points
 * inside it. */
static void
bm25_segment_blkno_validate(BlockNumber blkno, BlockNumber nblocks, const char *src)
{
    if (blkno >= nblocks)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s names segment header block %u, past the index "
                        "extent of %u blocks",
                        src, blkno, nblocks)));
}

/* bm25_debug_segment_blkno_bounds(blkno bigint, nblocks bigint) -- TEST-ONLY.
 * Runs the SAME bm25_segment_blkno_validate the orphan sweep's two reachable[]
 * writers call, over caller-supplied values. Same rationale as
 * bm25_debug_retired_page_bounds beside it: the real path needs a corrupt
 * segment catalog or retired descriptor, which a regression suite cannot
 * produce, so the check is driven directly instead. Returns blkno on success so
 * the accept cases have something to assert on. bigint arguments so a caller can
 * pass a value outside uint32 and get the C-level range complaint rather than a
 * SQL cast error masking which guard fired -- the shape sql/79 established. */
PG_FUNCTION_INFO_V1(bm25_debug_segment_blkno_bounds);
Datum
bm25_debug_segment_blkno_bounds(PG_FUNCTION_ARGS)
{
    int64   blkno   = PG_GETARG_INT64(0);
    int64   nblocks = PG_GETARG_INT64(1);

    if (blkno < 0 || blkno > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_segment_blkno_bounds: blkno out of uint32 range")));
    if (nblocks < 0 || nblocks > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_segment_blkno_bounds: nblocks out of uint32 range")));

    bm25_segment_blkno_validate((BlockNumber) blkno, (BlockNumber) nblocks,
                                "segment catalog entry");
    PG_RETURN_INT64(blkno);
}

/* bm25_reclaim_retired -- XID-horizon reclamation of retired SEGMENTS into the FSM.
 *
 * GATED reclaim (stamp-and-gate, Task 25): force-refresh the local visibility
 * horizon once, then for each retired RANGE entry whose retire_xid is removable
 * cluster-wide (GlobalVisCheckRemovableFullXid) stamp + free all the segment's pages
 * and drop the entry; keep not-yet-removable entries. Because the gate is the cluster
 * horizon, the FIRST VACUUM right after a merge typically frees nothing (the horizon
 * still covers the just-captured retire_xid) and a LATER VACUUM frees the segment --
 * exactly what 20_merge_reclaim's double-VACUUM exercises. Leaving an entry un-freed
 * is always SAFE (defers space reuse); the dangerous direction is freeing too early,
 * which the horizon gate forbids.
 *
 * heaprel == NULL has no horizon source, so defer everything (the legacy M1 builder
 * passes NULL; VACUUM and the merge path thread a real heap).
 *
 * Generic WAL discipline: the descriptor page is rewritten inside its own GenericXLog
 * window which is CLOSED (Finish + UnlockReleaseBuffer) BEFORE reclaim_one_range
 * acquires any segment buffer -- never nest a second buffer acquisition inside an open
 * window. We therefore copy survivors/drops to local arrays under the descriptor lock,
 * close the window, then free. The local arrays are sized BM25_RETIRED_PER_PAGE (a
 * compile-time BLCKSZ/sizeof constant, low hundreds -- stack-safe and -Werror=vla-clean).
 * That rewrite happens whenever anything is dropped, even for a page we are about to
 * unlink and free: a descriptor must stop listing a range BEFORE the range is freed, or
 * an interruption leaves entries aliasing reallocated pages (H11 -- see the note at the
 * compaction site).
 *
 * Descriptor-page reclamation: the swap prepends ONE fresh descriptor page per merge,
 * so without freeing emptied descriptors the retired chain would grow one ~8 KB page
 * per merge forever (mark_chain(retired_head) keeps them reachable). We therefore
 * UNLINK + free any descriptor page that drains to zero entries, EXCEPT the head:
 * unlinking the head would need a metapage write, so an emptied head is left linked and
 * reclaimed on a later pass once the next merge prepends a fresh head before it -- which
 * bounds the live chain to O(1) empty pages with no metapage churn. The unlink rewrites
 * only the predecessor's nextblk.
 *
 * Concurrency: we hold the metapage SINGLETON (the same heavyweight LockPage the seal
 * and the merge swap take) across each descriptor page we work on. Without it, this
 * reclaim could race another backend's reclaim (or a concurrent swap prepending to
 * retired_head) and corrupt the singly-linked chain mid-unlink. It is re-entrant, so a
 * caller that already holds it is fine, but then nothing below releases it.
 *
 * BOUNDED HOLD (issue #300): one CHUNK per descriptor page that has work. A chunk takes
 * the singleton, re-reads the metapage and walks from retired_head to the first page
 * with something to drop or unlink, compacts it, frees every range it dropped (and, if
 * it emptied, splices and frees it), and releases. Every insert that adds a document
 * waits for each hold, so the wait is one descriptor page's ranges -- one merge's inputs
 * -- rather than the whole horizon-cleared list. The release comes only AFTER the
 * chunk's frees: the descriptor is compacted before its ranges are freed, so a release
 * in between would leave ranges no descriptor lists, which a concurrent orphan sweep may
 * free and an allocator re-init before the resumed reclaim_one_range reaches them. Its
 * per-page checks would then stop at the first such page (a freed page is DELETED, a
 * re-inited one carries another gen) and fail the VACUUM. And every chunk restarts from
 * retired_head rather than carrying
 * prev_blk across the gap, because a swap in the gap prepends a new head and a stale
 * predecessor would splice the wrong page. */
void
bm25_reclaim_retired(Relation index, Relation heaprel)
{
    BM25MetaPageData meta;
    BlockNumber      blk,
                     prev_blk,
                     nblocks;
    uint64           visited;
    bool             any_freed = false;
    bool             bracket_open;
    bool             chunk_worked;

    if (heaprel == NULL)
        return;                 /* no horizon source: defer (always safe) */

    for (;;)                    /* one chunk per descriptor page with work: BOUNDED HOLD */
    {
        prev_blk = InvalidBlockNumber;
        bracket_open = false;
        chunk_worked = false;

        /* Serialize against swaps and other reclaims (re-entrant -- see header note).
         * On error the heavyweight lock is released by transaction abort. */
        LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);

        /* Read under the singleton, same as bm25_reclaim_orphans's mark_chain bound,
         * so it reflects a quiesced relation for the whole chunk (including the
         * reclaim_one_range calls below, which reuse this same value). */
        nblocks = RelationGetNumberOfBlocks(index);

        (void) GetOldestNonRemovableTransactionId(heaprel); /* unstale the local horizon */

        bm25_meta_read(index, &meta);
        blk = meta.retired_head;
        visited = 0;

        /* blk < nblocks backstops a corrupt retired_head chain that points past the
         * relation's own extent, same pattern as mark_chain and the orphan sweep's
         * own retired-list walk in bm25_reclaim_orphans. An in-extent cycle is
         * stopped by the visit cap at the top of the body, so a corrupt list cannot
         * hold the singleton for longer than one pass over the extent. */
        while (blk != InvalidBlockNumber && blk < nblocks)
        {
            Buffer              buf;
            BlockNumber         next;
            BM25RetiredEntry    keep[BM25_RETIRED_PER_PAGE];
            BM25RetiredEntry    drop[BM25_RETIRED_PER_PAGE];
            int                 nkeep = 0,
                                ndrop = 0;
            int                 n,
                                k;
            bool                will_unlink;
            Page                pg;
            BM25RetiredEntry   *arr;

            CHECK_FOR_INTERRUPTS();

            bm25_retired_cycle_cap_validate(visited, nblocks, blk);
            visited++;

            buf = ReadBuffer(index, blk);
            LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
            pg = BufferGetPage(buf);
            /* This chain is walked straight from meta.retired_head, which by
             * construction only ever links BM25_PAGE_RETIRED pages -- unlike the
             * orphan sweep in bm25_reclaim_orphans, there is no independent root set
             * to fall back on here, so a page of any other kind reached via this
             * chain means a corrupt/stray nextblk landed on an unrelated live page.
             * Reading it as a BM25RetiredEntry array would misinterpret that page's
             * real content as retired-segment descriptors.
             *
             * bm25_page_content_bytes bounds pd_lower before it drives this division
             * -- same reasoning as bm25_reclaim_orphans's Phase 4 (this file): a bare
             * pd_lower read here would let a corrupt pd_lower underflow the
             * subtraction this used to be, and the signed/unsigned conversion in the
             * division that followed could hand n an effectively arbitrary value
             * BEFORE bm25_retired_count_validate ever sees it. First, before the
             * opaque read, as the page entry check (same note as Phase 4). */
            n = bm25_page_content_bytes(pg) / sizeof(BM25RetiredEntry);
            bm25_retired_page_flags_validate(BM25PageGetOpaque(pg)->flags, blk);
            next = BM25PageGetOpaque(pg)->nextblk;
            arr = (BM25RetiredEntry *) PageGetContents(pg);
            /* n drives the fixed keep[]/drop[] stack arrays declared above, both
             * sized BM25_RETIRED_PER_PAGE (the construction-time max entries that
             * fit on one page). A corrupt pd_lower overstating the entry count would
             * otherwise let the partition loop below write past both arrays. */
            bm25_retired_count_validate(n, blk);

            /* Partition entries into survivors (horizon not yet cleared) and removable
             * drops. Pure reads under the page lock; no buffer acquisition, no WAL window
             * yet (reclaim_one_range, which DOES acquire buffers, runs only after we close
             * the rewrite window below). */
            for (k = 0; k < n; k++)
            {
                if (GlobalVisCheckRemovableFullXid(heaprel, arr[k].retire_xid))
                    drop[ndrop++] = arr[k];
                else
                    /* memcpy for the same reason as the copy-back below: keep[] is an
                     * uninitialized stack array, and a struct assignment that skips padding
                     * would leave its holes unwritten for the memcpy at the end to publish. */
                    memcpy(&keep[nkeep++], &arr[k], sizeof(BM25RetiredEntry));
            }

            /* An emptied NON-head page is removed from the chain entirely below; an emptied
             * head (or a page that keeps survivors) stays linked and is rewritten in place. */
            will_unlink = (nkeep == 0 && prev_blk != InvalidBlockNumber);

            /* Issue #302 E: a cycle back to the head. The visit cap cannot see it: this
             * walk would unlink and free the head -- a page the metapage still names --
             * and end the chunk, and the next chunk would restart with a fresh count and
             * do it again. This is the only unlink a revisit can reach: an emptied page is
             * passed over without work only as the head (a non-head one is unlinked on
             * its first visit), and a page with drops ends the chunk on its first visit.
             * Unlinked pages are DELETED, which bm25_retired_page_flags_validate refuses,
             * so a cycle that skips the head cannot free one twice either; one with no
             * work anywhere on it is what the visit cap stops. */
            if (will_unlink && blk == meta.retired_head)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: retired list links back to its head, block %u", blk)));

            /* Orphan bracket (issue #300), opened lazily: only a pass that is about to
             * compact or splice can leave orphans (see "Compacting first" below: an
             * interruption between the compaction and the frees leaks the dropped
             * ranges, one between the splice and the free leaks the descriptor), and a
             * pass with nothing to reclaim -- most VACUUMs -- must write no WAL. The begin
             * record has to reach the WAL before that first compaction, and it is a
             * metapage write, so drop this page's lock first and come back: the singleton
             * keeps the retired list exactly as it was, and re-partitioning only lets an
             * entry whose horizon cleared meanwhile be dropped too. */
            if ((ndrop > 0 || will_unlink) && !bracket_open)
            {
                UnlockReleaseBuffer(buf);
                bm25_orphan_op_begin(index);
                bracket_open = true;
                continue;           /* same blk: re-read and re-partition it */
            }

            /* Drop the entries from the descriptor BEFORE freeing anything they describe --
             * on EVERY path, including will_unlink, where the page is about to be freed
             * anyway. This costs one extra page image per drained descriptor and buys the
             * only safe failure mode (H11).
             *
             * reclaim_one_range below stamps each dropped segment's pages DELETED and records
             * them free, and their retire_xid has already cleared the horizon, so
             * bm25_page_alloc accepts them IMMEDIATELY. No allocator races this pass: every
             * allocator that can run against a live index (the pending append included, in
             * ShareLock) holds the metapage singleton, which this pass holds ExclusiveLock.
             * The order matters for what happens AFTER an interrupted pass, when the next
             * allocator may re-hand a freed page. (ambuild and the test-only
             * bm25_debug_alloc_unknown_page hold no singleton and are not live-index
             * allocators.) Generic WAL page writes are physical: GenericXLogFinish applies
             * and logs them, and transaction abort does not undo them. So if we freed the
             * ranges while the descriptor still listed them and then failed before the
             * splice below (a ReadBuffer I/O error, a palloc failure, backend or server
             * death), the surviving entries would outlive the free and point at pages the
             * allocator has already re-handed to a live structure. Before #302 the next
             * pass walked that structure's CURRENT nextblk links and freed it under its
             * readers. Now reclaim_page_validate stops at the first stale page (a freed
             * page is DELETED, a re-inited one carries another gen), but the entry is
             * still listed, so EVERY later VACUUM fails on it -- and the merge and orphan
             * sweep that follow it in cleanup order never run -- until REINDEX.
             *
             * Compacting first inverts that: an interruption leaves an empty-but-linked
             * descriptor page (the next pass takes the will_unlink branch and frees it) and
             * leaks the not-yet-freed data pages, which the closed-world mark-and-sweep in
             * bm25_reclaim_orphans recovers. Deferred space beats a VACUUM that fails until
             * REINDEX (and, before #302's per-page checks, beat aliased pages). */
            if (ndrop > 0)
            {
                GenericXLogState   *st;
                int                 w;

                /* Rewrite the descriptor page keeping only survivors (compact in place),
                 * THEN -- after closing this window -- free the dropped segments. */
                st = GenericXLogStart(index);
                pg = GenericXLogRegisterBuffer(st, buf, 0);
                arr = (BM25RetiredEntry *) PageGetContents(pg);
                /* memcpy, not struct assignment (PEND-22). These bytes go onto a
                 * WAL-registered page, and BM25RetiredEntry has a 4-byte hole at offset 36.
                 * `arr[w] = keep[w]` leaves it to the compiler whether padding is copied at
                 * all -- unspecified in C, and the difference is observable here because the
                 * result is written to disk and replayed on a standby. memcpy makes the
                 * whole 48 bytes, hole included, part of the contract. keep[] itself is an
                 * uninitialized stack array, but every slot read back was filled by a
                 * whole-struct copy off the page (which bm25_retire_segment now memsets), so
                 * the hole carries the page's zeros rather than stack residue. */
                for (w = 0; w < nkeep; w++)
                    memcpy(&arr[w], &keep[w], sizeof(BM25RetiredEntry));
                ((PageHeader) pg)->pd_lower =
                    (((char *) arr) + sizeof(BM25RetiredEntry) * nkeep) - (char *) pg;
                GenericXLogFinish(st);
                UnlockReleaseBuffer(buf);
                /* Test lever (t/029): the descriptor no longer lists the dropped ranges
                 * and none of their pages is freed yet -- the state an ERROR here leaves,
                 * which only the orphan sweep recovers. No buffer is held. */
                bm25_debug_pause_point("reclaim_retired_compacted");
            }
            else
                UnlockReleaseBuffer(buf);

            /* Free the dropped segments' data pages (independent buffers; outside any
             * descriptor WAL window). */
            for (k = 0; k < ndrop; k++)
                reclaim_one_range(index, &drop[k], nblocks);
            if (ndrop > 0)
                any_freed = true;

            if (will_unlink)
            {
                GenericXLogState   *st;
                Buffer              prevbuf = ReadBuffer(index, prev_blk);
                Buffer              dbuf;

                /* Splice blk out of the chain: predecessor now points past it. */
                LockBuffer(prevbuf, BUFFER_LOCK_EXCLUSIVE);
                st = GenericXLogStart(index);
                BM25PageGetOpaque(GenericXLogRegisterBuffer(st, prevbuf, 0))->nextblk = next;
                GenericXLogFinish(st);
                UnlockReleaseBuffer(prevbuf);

                /* Now unreachable from the chain: stamp DELETED (stamp-and-gate) + free.
                 * A crash between the splice and here leaves blk as a plain orphan the
                 * next bm25_reclaim_orphans sweep recovers -- no leak, no double-free. */
                dbuf = ReadBuffer(index, blk);
                LockBuffer(dbuf, BUFFER_LOCK_EXCLUSIVE);
                if (!PageIsNew(BufferGetPage(dbuf)))
                    bm25_page_mark_deleted(index, dbuf, InvalidFullTransactionId);
                UnlockReleaseBuffer(dbuf);
                RecordFreeIndexPage(index, blk);
                any_freed = true;
                /* prev_blk UNCHANGED -- its successor is now `next`. */
            }
            else
                prev_blk = blk;     /* page stays linked (kept survivors, or empty head) */

            /* This page had work and all of it is done: end the chunk here. */
            if (ndrop > 0 || will_unlink)
            {
                chunk_worked = true;
                break;
            }
            blk = next;
        }

        /* Every compaction this chunk made has had its ranges freed and every splice its
         * descriptor freed: nothing left behind. */
        if (bracket_open)
            bm25_orphan_op_end(index);

        UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
        if (!chunk_worked)
            break;                  /* a walk found nothing left to do */
        /* Test lever (t/032): between two chunks, the singleton released (unless a
         * caller holds it too). */
        bm25_debug_pause_point("reclaim_retired_between_chunks");
    }

    /* Outside the singleton: the FSM locks its own pages. */
    if (any_freed)
        IndexFreeSpaceMapVacuum(index);
}

/* Decode-boundary probe (trust-boundary review, 2026-08). The two validators
 * above only fire on an already-corrupt retired-list page (wrong page kind, or
 * a pd_lower overstating the entry count), which a regression suite cannot
 * produce, so this runs BOTH checks, in the SAME order bm25_reclaim_retired
 * runs them (page kind first, then entry count), over caller-chosen values.
 * `blk` in the resulting error messages is always 0 -- the probe has no real
 * page, only the checks are under test. TEST-ONLY: a pure function of its
 * scalar arguments, no relation touched. Returns n on success. */
PG_FUNCTION_INFO_V1(bm25_debug_retired_page_bounds);
Datum
bm25_debug_retired_page_bounds(PG_FUNCTION_ARGS)
{
    int32   flags = PG_GETARG_INT32(0);
    int32   n     = PG_GETARG_INT32(1);

    if (flags < 0 || flags > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_retired_page_bounds: flags out of uint16 range")));

    bm25_retired_page_flags_validate((uint16) flags, 0);
    bm25_retired_count_validate(n, 0);
    PG_RETURN_INT32(n);
}

/* bm25_debug_retired_count -- counts RANGE entries on the retired list (retired
 * SEGMENTS awaiting horizon reclamation, one entry per dropped segment; NOT
 * individual pages). Debug/regression only. */
PG_FUNCTION_INFO_V1(bm25_debug_retired_count);
Datum
bm25_debug_retired_count(PG_FUNCTION_ARGS)
{
    Oid              relid = PG_GETARG_OID(0);
    Relation         index = bm25_index_open_readable(relid, AccessShareLock);
    BM25MetaPageData meta;
    int64            total = 0;
    uint64           visited = 0;
    BlockNumber      blk;
    BlockNumber      nblocks = RelationGetNumberOfBlocks(index);

    bm25_meta_read(index, &meta);
    blk = meta.retired_head;
    /* Debug SRF, but it walks the same on-disk chain shape as bm25_reclaim_retired
     * and gets the same corruption backstops (the `blk < nblocks` extent bound
     * mark_chain and the two production retired-list walkers use, and their visit
     * cap) and cancellability. */
    while (blk != InvalidBlockNumber && blk < nblocks)
    {
        Buffer          buf;
        Page            pg;
        BM25PageOpaque *op;
        int             n;

        CHECK_FOR_INTERRUPTS();
        bm25_retired_cycle_cap_validate(visited++, nblocks, blk);

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        /* Uniform with bm25_reclaim_retired / bm25_reclaim_orphans's Phase 4 /
         * bm25_debug_retired_page_bounds: bound pd_lower (via
         * bm25_page_content_bytes, the page entry check, so before the opaque read)
         * before it drives this division, and validate the page is actually a
         * retired-list page before trusting its content as a BM25RetiredEntry
         * array. Debug-only, but reachable by anyone who can
         * already call a debug SRF on this index (H13, ADR 0039), so it gets the
         * same trust boundary as the three production/probe walkers. */
        n = bm25_page_content_bytes(pg) / sizeof(BM25RetiredEntry);
        op = BM25PageGetOpaque(pg);
        bm25_retired_page_flags_validate(op->flags, blk);
        bm25_retired_count_validate(n, blk);
        total += n;
        blk = op->nextblk;
        UnlockReleaseBuffer(buf);
    }
    index_close(index, AccessShareLock);
    PG_RETURN_INT64(total);
}

/* bm25_debug_retired_entry_bytes(index regclass, n int) -- the RAW bytes of retired
 * descriptor n, padding hole included. Companion to bm25_debug_segcat_entry_bytes; see
 * that function for why the #144 class needs a byte-level probe at all. The hole here
 * is at offset [36,40), between uint32 gen and the 8-aligned FullTransactionId.
 * Development and regression testing only. */
PG_FUNCTION_INFO_V1(bm25_debug_retired_entry_bytes);
Datum
bm25_debug_retired_entry_bytes(PG_FUNCTION_ARGS)
{
    Oid              relid = PG_GETARG_OID(0);
    int32            want  = PG_GETARG_INT32(1);
    Relation         index = bm25_index_open_readable(relid, AccessShareLock);
    BM25MetaPageData meta;
    BlockNumber      blk;
    BlockNumber      nblocks = RelationGetNumberOfBlocks(index);
    int64            seen = 0;
    uint64           visited = 0;
    bytea           *out = NULL;

    if (want < 0)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_retired_entry_bytes: entry index must be non-negative")));
    }

    bm25_meta_read(index, &meta);
    blk = meta.retired_head;
    /* Same chain shape, page-kind validation, extent bound and visit cap as the
     * sibling walkers. */
    while (blk != InvalidBlockNumber && blk < nblocks && out == NULL)
    {
        Buffer              buf;
        Page                pg;
        BM25PageOpaque     *op;
        BM25RetiredEntry   *arr;
        int                 n;

        CHECK_FOR_INTERRUPTS();
        bm25_retired_cycle_cap_validate(visited++, nblocks, blk);

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        n = bm25_page_content_bytes(pg) / sizeof(BM25RetiredEntry);
        op = BM25PageGetOpaque(pg);
        bm25_retired_page_flags_validate(op->flags, blk);
        bm25_retired_count_validate(n, blk);
        arr = (BM25RetiredEntry *) PageGetContents(pg);

        if (want < seen + n)
        {
            out = (bytea *) palloc(VARHDRSZ + sizeof(BM25RetiredEntry));
            SET_VARSIZE(out, VARHDRSZ + sizeof(BM25RetiredEntry));
            memcpy(VARDATA(out), &arr[want - seen], sizeof(BM25RetiredEntry));
        }
        seen += n;
        blk = op->nextblk;
        UnlockReleaseBuffer(buf);
    }
    index_close(index, AccessShareLock);

    if (out == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_retired_entry_bytes: entry %d out of range (" INT64_FORMAT " present)",
                        want, seen)));

    PG_RETURN_BYTEA_P(out);
}

/* bm25_debug_retired_pages -- counts PAGES on the retired-list chain (BM25_PAGE_RETIRED
 * descriptor pages, not entries). Distinct from bm25_debug_retired_count (entries):
 * this is the metric that detects the "one leaked descriptor page per merge" bug -- it
 * stays bounded (O(1)) once bm25_reclaim_retired unlinks emptied non-head descriptors,
 * and grows ~1 per merge without that. Debug/regression only. */
PG_FUNCTION_INFO_V1(bm25_debug_retired_pages);
Datum
bm25_debug_retired_pages(PG_FUNCTION_ARGS)
{
    Oid              relid = PG_GETARG_OID(0);
    Relation         index = bm25_index_open_readable(relid, AccessShareLock);
    BM25MetaPageData meta;
    int64            pages = 0;
    BlockNumber      blk;
    BlockNumber      nblocks = RelationGetNumberOfBlocks(index);

    bm25_meta_read(index, &meta);
    blk = meta.retired_head;
    /* Same bound, visit cap, page-kind check and cancellability as
     * bm25_debug_retired_count above. The kind check was missing here (issue #302 E):
     * a link onto another chain was counted as a descriptor page and followed. */
    while (blk != InvalidBlockNumber && blk < nblocks)
    {
        Buffer          buf;
        BM25PageOpaque *op;

        CHECK_FOR_INTERRUPTS();
        bm25_retired_cycle_cap_validate((uint64) pages, nblocks, blk);

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        (void) bm25_page_content_bytes(BufferGetPage(buf));    /* before the opaque */
        op = BM25PageGetOpaque(BufferGetPage(buf));
        bm25_retired_page_flags_validate(op->flags, blk);
        pages++;
        blk = op->nextblk;
        UnlockReleaseBuffer(buf);
    }
    index_close(index, AccessShareLock);
    PG_RETURN_INT64(pages);
}
