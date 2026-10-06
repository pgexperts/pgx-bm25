/* bm25_seg_read.c -- sealed-segment reader (Phase 1 minimal).
 *
 * Role in the system: the read side of the immutable segment format the builder
 * (bm25_seg_build.c) lays down. The scorer (Task 7+) and the merge rebuild path
 * (Phase 4) call into this; this file provides the catalog snapshot, the header
 * read, the LIVEDOCS tombstone path and the shared validators. The DICT lookup,
 * the block-postings decode, the live-docs test and docid->TID live in
 * bm25_seg_dict.c and bm25_seg_chain.c (below).
 *
 * Lock hand-off (lock next before releasing current) is used wherever a chain is
 * walked, per the Conventions.
 *
 * Option (d) reuse-safety: after locking each followed segment page, the reader
 * validates its stamped seg_gen against the expected (catalog/header) generation;
 * a mismatch means the page was reclaimed-and-reused concurrently (gens never
 * repeat => no ABA), so the scan aborts cleanly via bm25_seg_page_validate rather
 * than read recycled bytes.
 *
 * The reader is four files (#228, ADR 0101); this one keeps the catalog and the
 * validation layer the other three stand on:
 *   - bm25_seg_read.c (this file): the SEGCAT snapshot and catalog reads, with
 *     their extent bound; the page-kind, page-offset, block-number, content-bytes
 *     and chain-extent validators every walker calls, with the debug probes that
 *     wrap four of them;
 *     the segment header readers; and the LIVEDOCS bitmap -- the VACUUM tombstone
 *     path (bm25_livedocs_locate/_clear) and the one-pass all-set check behind
 *     bm25_seg_reader_init_checked, which share one page-geometry helper.
 *   - bm25_seg_dict.c: DICT lookup, the DICT iterator, wildcard expansion.
 *   - bm25_seg_chain.c: the POST/POS decode, chain_read_at and the dense per-docid
 *     readers, and BM25SegReader.
 *   - bm25_seg_debug.c: the debug SRFs that need only the reader's API. Probes
 *     that call one of this file's statics (bm25_debug_segcat_walk,
 *     bm25_debug_seg_lenfields) stay here so those statics keep internal linkage. */
#include "postgres.h"

#include "bm25.h"
#include "funcapi.h"
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "storage/bufmgr.h"
#include "storage/lock.h"   /* LockHeldByMe: segcat_singleton_held */
#include "utils/builtins.h"
#include "utils/tuplestore.h"   /* tuplestore_begin_heap/putvalues -- no longer pulled
                                 * in transitively via funcapi.h as of PG19 */

/* ---- SEGCAT extent bound (issue #225) ----
 *
 * The four SEGCAT walkers below (bm25_scan_snapshot, bm25_segcat_read,
 * bm25_segcat_find_entry, which is test/debug-only, and bm25_segcat_locate_entry) are
 * the only chain walks in this file whose `nblocks` is sampled BEFORE the chain's root
 * is read, and that ordering is forced: bm25_scan_snapshot reads segcat_root under the
 * metapage SHARE and walks the whole chain under it, and an smgr call is kept off that
 * lock on the common path.
 *
 * A sample taken before the root can be STALE-LOW, and on a healthy index. A seal or
 * merge allocates its new SEGCAT page(s) with bm25_page_alloc -- which extends the
 * relation when the FSM has nothing -- and only then takes the metapage EXCLUSIVE and
 * flips segcat_root (bm25_segcat_publish_append prepends one page;
 * bm25_segcat_build_orphan_chain + bm25_segcat_publish_swap build a whole new chain).
 * A walker that samples, loses the CPU across that allocate-and-flip, and then reads
 * the metapage is handed a root at or past its own sample. So the bound
 * bm25_seg_chain_extent_validate's header argues for -- "a captured value is a lower
 * bound and can never reject a block that legitimately exists" -- does not hold here
 * as stated: that argument assumes the capture FOLLOWS the chain's publication, which
 * is true of every other walker in the segment reader and false of these four.
 * bm25_segcat_first_entry (issue #270) reads only the root, under the metapage SHARE,
 * and samples the same way for the same reason, so the same holds for it.
 *
 * Hence the re-sample. On a would-be violation, read the extent once more. That second
 * read happens after the root was read, and the writer's extension precedes its flip,
 * which precedes the metapage lock this walker's root came through -- so the fresh
 * extent covers every page of the chain this walker can reach, and a block still at or
 * past it is out of the index for real. Healthy walks never reach the re-sample outside
 * that race window, so the steady-state cost stays one lseek per walk, taken before the
 * lock. When the re-sample does run inside bm25_scan_snapshot it runs under the metapage
 * SHARE; one lseek there is cheaper than the ReadBuffer I/O that walk already performs
 * under the same lock, and it takes no lock of its own.
 *
 * Callers holding the seal/merge singleton (bm25_reclaim_orphans, bm25_bulkdelete --
 * ShareLock for its whole pass, snapshot through pending sweep -- bm25_merge_execute
 * via bm25_merge_maybe, bm25_merge_rewrite_all) cannot see the race at all -- no
 * publisher can run -- but other walkers (e.g. the
 * bm25_debug_merge_plan / _budget_plan probes) read without it, and the helper does
 * not need to know which callers are which.
 *
 * Exported (bm25.h) because the pending-chain walkers in bm25_pending.c (#243) need
 * the identical re-sample, for a different reason: appends keep linking new pages
 * while a sweep walks. Their argument is the "Pending extent bound" note there. The
 * helper states only the mechanism; each caller's note is what makes it sound. */
bool
bm25_blk_in_extent(Relation index, BlockNumber blk, BlockNumber *nblocks)
{
    if (blk < *nblocks)
        return true;
    *nblocks = RelationGetNumberOfBlocks(index);
    return blk < *nblocks;
}

/* The ERROR form, for the two walkers whose truncated walk is a wrong answer: a scan
 * snapshot or a catalog copy that stops early drops live segments (the aggregate
 * `filled != nsegs` check downstream would also fire, but as "has N entries, expected
 * M", which names no pointer). Raised before ReadBuffer, so an out-of-extent link never
 * reaches the buffer manager's own short-read error. */
static void
segcat_extent_validate(Relation index, BlockNumber blk, BlockNumber *nblocks)
{
    if (!bm25_blk_in_extent(index, blk, nblocks))
        bm25_seg_chain_extent_validate(blk, *nblocks, "segment catalog chain");
}

/* ---- SEGCAT link shape (issue #244) ----
 *
 * The extent bound above says where a link may point, and nothing about what it points
 * at. An in-extent nextblk that is corrupt had three outcomes, all on-disk corruption:
 *   (a) a cycle through entry-bearing pages copied the same entries again until the
 *       count was satisfied, so the two copying walkers returned duplicated segments and
 *       the aggregate `filled != nsegs` check passed -- a scan summed them silently;
 *   (b) a link to a page of another kind (block 0, a data page) had that page's bytes
 *       copied as BM25SegCatEntry records;
 *   (c) a cycle through pages with no entries never advanced the count. In
 *       bm25_scan_snapshot that spin runs under the metapage SHARE content lock, whose
 *       LWLockAcquire holds interrupts, so the backend could not be cancelled and every
 *       writer needing the metapage EXCLUSIVE queued behind it.
 * find_entry and locate_entry have no entry count at all, so any in-extent cycle kept
 * them walking until cancelled.
 *
 * Three guards, each for a shape the others miss:
 *   - segcat_page_nentries: the page kind, on every page a walker reads (b), and a
 *     zero-entry page that links onward (c, caught at the first such page).
 *   - segcat_visit_validate: at most nsegs + 1 pages per walk, checked before
 *     ReadBuffer. This is what bounds find/locate on a cycle through entry-bearing
 *     pages.
 *   - segcat_entries_unique_validate: no generation twice in a copied catalog (a, for
 *     the two copying walkers). Neither check above sees a 7-entry root that links to
 *     itself in a 210-entry catalog: that fills all 210 slots in 30 visits.
 *
 * The packing invariant the zero-entry check and the cap rest on. Every page a writer
 * links into the live chain carries at least one entry, with one exception.
 * bm25_segcat_build_orphan_chain fills each page before starting the next and stops
 * after the page that takes the last entry, so its pages are non-empty unless n == 0;
 * n == 0 (a merge whose inputs were all tombstoned) yields ONE empty page, the whole
 * catalog, with nextblk Invalid. bm25_segcat_publish_append writes n > 0 entries onto a
 * fresh or existing root, or prepends a chain built with n > 0. Tombstones rewrite
 * entries in place, and nothing removes one outside a swap, which rebuilds the chain.
 * So the empty page is the lone root of an nsegs == 0 catalog or, once a later publish
 * prepends a chain in front of it, the chain's LAST page -- next Invalid either way. An
 * empty page with a successor is therefore never legitimate, and a legitimate chain has
 * at most nsegs non-empty pages plus that one empty tail: nsegs + 1. The copying walkers
 * stop at nsegs entries and never reach the empty tail, so for them the cap is
 * unreachable while the zero-entry check stands; they share it anyway, because it is
 * one compare and it keeps the bound from resting on a single check.
 *
 * The cap reads nsegs from the same metapage copy as segcat_root, so a concurrent
 * publish cannot outgrow it: an in-place append adds entries and no page, and a prepend
 * or a swap starts a chain this walk never enters.
 *
 * A link to a segment HEADER page, which is BM25_PAGE_SEGCAT too, used to pass the kind
 * check (#276). segcat_page_nentries now also requires the catalog role, seg_gen == 0
 * (bm25_segcat_page_validate), which a header page, stamped with its gen (>= 1), fails.
 *
 * Not covered (documented residual, #276):
 *   - a corrupt meta.nsegs. Every guard above takes nsegs from the metapage on trust:
 *     the cap, the copying walkers' entry count and their copy size. Too small, and
 *     the copying walkers stop early and return a short catalog (silently), while
 *     find/locate can hit the cap on a healthy chain. Too large, and the copying
 *     walkers first palloc nsegs entries: above about 26.8M (MaxAllocSize over the
 *     40-byte entry) that fails with "invalid memory alloc request size", and below it
 *     can still ask for up to about 1 GB -- in bm25_scan_snapshot, under the metapage
 *     SHARE lock. Past the allocation they raise "has N entries, expected M", and the
 *     cap loosens with nsegs. */
static uint32
segcat_page_nentries(Page pg, BlockNumber blk)
{
    uint32      n;
    BlockNumber next;

    /* Content bounds, kind and catalog role (seg_gen == 0, issue #276). The content
     * bound also matters before pd_lower becomes a count: a corrupt pd_lower below
     * SizeOfPageHeaderData would otherwise underflow into a huge one, or, clamped,
     * make a corrupt page look like an empty one. */
    n = (uint32) (bm25_segcat_page_validate(pg, BM25_SEGCAT_ROLE_CATALOG, blk)
                  / MAXALIGN(sizeof(BM25SegCatEntry)));
    next = BM25PageGetOpaque(pg)->nextblk;
    if (n == 0 && next != InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment catalog page has no entries but is not the end of the chain"),
                 errdetail("It links to block %u.", next)));
    return n;
}

/* Issue #303.C: a copied catalog entry's gen is the expected_gen every reader of its
 * segment then passes, and 0 is bm25_seg_page_validate_kind's "don't validate"
 * sentinel. A zero gen would therefore switch off reuse detection for every page of
 * that segment, and leave its header_blkno free to name a catalog page (#276).
 * bm25_meta_fill starts next_gen at 1, so no segment is ever given gen 0. Checked on
 * each entry as it is copied, by the three walkers that hand entries to callers. */
static void
segcat_entry_gen_validate(const BM25SegCatEntry *e, BlockNumber blk)
{
    if (e->gen == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment catalog page %u holds an entry with generation 0",
                        blk),
                 errdetail("The entry names segment header block %u; generations start "
                           "at 1.", e->header_blkno)));
}

/* `visited` is the number of pages this walk has already read. Called before the next
 * ReadBuffer, so bm25_scan_snapshot raises holding only the metapage. */
static void
segcat_visit_validate(uint32 visited, uint32 nsegs)
{
    if (visited > nsegs)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment catalog chain is longer than its %u entries allow", nsegs),
                 errdetail("%u pages were read without reaching the end of the chain.",
                           visited)));
}

static int
segcat_gen_cmp(const void *a, const void *b)
{
    uint32  x = *(const uint32 *) a;
    uint32  y = *(const uint32 *) b;

    return (x > y) - (x < y);
}

/* Generations are unique across the live catalog (bm25_next_gen gives each segment its
 * own and never repeats one), so a repeat means the walk copied a page twice. gen alone
 * is enough: a revisit duplicates whole entries, so every key repeats together. A
 * sorted copy rather than a pairwise scan, because nothing bounds the catalog between
 * merges (bm25_segcat_publish_append's header). */
static void
segcat_entries_unique_validate(const BM25SegCatEntry *segs, uint32 n)
{
    uint32     *gens;
    uint32      i;

    if (n < 2)
        return;
    gens = palloc(sizeof(uint32) * n);
    for (i = 0; i < n; i++)
        gens[i] = segs[i].gen;
    qsort(gens, n, sizeof(uint32), segcat_gen_cmp);
    for (i = 1; i < n; i++)
        if (gens[i] == gens[i - 1])
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: segment catalog lists generation %u more than once",
                            gens[i]),
                     errdetail("A catalog page link probably forms a cycle.")));
    pfree(gens);
}

/* D-SNAP / C3 (Model A): the ONE consistent scan-start snapshot. Captures
 * pending_head, segcat_root, nsegs, and the global stats (ndocs/total_len/k1/b),
 * AND copies the WHOLE live catalog into out->segs -- all WHILE STILL HOLDING the
 * metapage BUFFER_LOCK_SHARE. Holding the metapage share across the catalog walk
 * is what makes the snapshot atomic against a concurrent seal/merge: a seal cannot
 * flip segcat_root + advance pending_head (it needs the metapage EXCLUSIVE) while
 * we hold SHARE, so pending_head and the catalog we copy are guaranteed mutually
 * consistent. Nothing bounds the catalog between merges (see the duplicate-gen
 * check above), but the copy is a validation and a memcpy per entry, so the
 * copy-under-lock is accepted as cheap rather than guaranteed small. After this
 * returns the scan never re-reads a catalog page, so OLD catalog pages from a later
 * swap need no horizon-gating (only the large segment DATA pages do). Callers MUST
 * use this instead of pairing
 * bm25_meta_read with bm25_segcat_read under two separate metapage locks.
 *
 * `nblocks` is the extent sampled by the caller BEFORE the metapage lock -- see
 * bm25_blk_in_extent above for why that order is forced and what it costs. The
 * public entry point takes the sample itself; bm25_debug_segcat_walk passes a stale
 * one to drive the re-sample path. */
static void
scan_snapshot_sampled(Relation index, BM25ScanSnapshot *out, BlockNumber nblocks)
{
    Buffer              metabuf;
    Page                metapage;
    BM25MetaPageData   *meta;
    uint32              filled = 0;
    BlockNumber         blk;

    metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    metapage = BufferGetPage(metabuf);
    meta = BM25PageGetMeta(metapage);

    /* Enforce the v6 two-directional gate here: this call is the scan's very
     * first (and, per the snapshot-once discipline above, its ONLY) metapage
     * touch, so nothing upstream has already validated the version the way an
     * introspection/build/maintenance caller of bm25_meta_read would have.
     * Skipping this would let a too-old/too-new index be scanned silently --
     * exactly the defect the gate exists to prevent. */
    bm25_meta_validate(meta);

    /* one atomic capture of everything the scan linearizes against */
    out->pending_head = meta->pending_head;
    /* #291: under the SAME lock as pending_head -- the pair is what makes the epoch
     * bound sound (see BM25ScanSnapshot.next_gen and bm25_pending_append_multi). */
    out->next_gen     = meta->next_gen;
    out->segcat_root  = meta->segcat_root;
    out->nsegs        = meta->nsegs;
    out->ndocs        = meta->ndocs;
    out->total_len    = meta->total_len;
    out->k1           = meta->k1;
    out->b            = meta->b;

    /* v4: capture the analyzer fingerprint + field_count under the SAME metapage
     * SHARE lock (D-SNAP/C3) so the scan-start gate compares against a value that
     * is mutually consistent with the catalog/stats this snapshot saw. */
    out->analyzer_fingerprint = meta->analyzer_fingerprint;
    out->field_count          = meta->field_count;
    out->field_config_blkno   = meta->field_config_blkno;   /* M5: BM25F per-field cfg root */

    /* Model A: copy ALL live BM25SegCatEntry into a palloc'd array WHILE STILL
     * holding the metapage SHARE lock. Each SEGCAT page is pinned SHARE, copied,
     * and released before the next is pinned (sequential pin-and-release, NOT
     * crab-locking -- no two SEGCAT page locks are ever held at once). The metapage
     * SHARE held throughout is the atomicity anchor: a seal/merge needs the metapage
     * EXCLUSIVE to swap segcat_root, so it cannot move the chain under this copy. */
    if (out->nsegs == 0)
        out->segs = NULL;
    else
    {
        uint32  visited = 0;

        out->segs = (BM25SegCatEntry *) palloc(sizeof(BM25SegCatEntry) * out->nsegs);
        blk = out->segcat_root;
        while (blk != InvalidBlockNumber && filled < out->nsegs)
        {
            Buffer          buf;
            Page            page;
            char           *cur;
            uint32          n,
                            i;
            BlockNumber     next;

            /* No CHECK_FOR_INTERRUPTS: the metapage content lock holds interrupts, so
             * one here would be dead and would only pad the CI interrupt-check floor
             * (issue #313 XCUT-09, ADR 0041). That is why every check below must END
             * the walk rather than lean on a cancel. */

            /* Issue #225: the entry COUNT above bounds how many entries are copied and
             * nothing about where nextblk points. Raised with the metapage still held;
             * transaction abort releases it, as it does for every other ERROR under
             * this lock (bm25_meta_validate above included). Issue #244's page cap
             * comes first for the same reason: both fire before ReadBuffer. */
            segcat_visit_validate(visited++, out->nsegs);
            segcat_extent_validate(index, blk, &nblocks);
            /* A link to the metapage must be refused BEFORE ReadBuffer, not by the kind
             * check after it: this walk already holds that buffer's content lock, and
             * locking it a second time is not something the buffer manager supports. PG
             * 18's LWLock-based content locks happen to let a second SHARE through; PG 19
             * reworked buffer locking and asserts (bufmgr.c, "entry->data.lockmode ==
             * BUFFER_LOCK_UNLOCK"), and nothing promises a production build more than
             * self-deadlock. Same message as the kind check, since the metapage is simply
             * the wrong kind, so the error does not depend on the major. The other three
             * walkers hold no lock across iterations and reach the kind check safely. */
            if (blk == BM25_METAPAGE_BLKNO)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: segment page is not of the expected kind"),
                         errdetail("The segment catalog chain links to the metapage.")));
            buf = ReadBuffer(index, blk);
            LockBuffer(buf, BUFFER_LOCK_SHARE);
            page = BufferGetPage(buf);
            cur = (char *) PageGetContents(page);
            /* Kind, content bound, and the zero-entry-with-successor shape that used
             * to spin here uncancellably (issue #244). Validating at the point of read
             * rather than leaning on the aggregate count below is the discipline every
             * chain reader in the segment reader follows. */
            n = segcat_page_nentries(page, blk);
            for (i = 0; i < n && filled < out->nsegs; i++)
            {
                memcpy(&out->segs[filled], cur, sizeof(BM25SegCatEntry));
                segcat_entry_gen_validate(&out->segs[filled], blk);
                filled++;
                cur += MAXALIGN(sizeof(BM25SegCatEntry));
            }
            next = BM25PageGetOpaque(page)->nextblk;
            UnlockReleaseBuffer(buf);
            blk = next;
        }
        if (filled != out->nsegs)
        {
            UnlockReleaseBuffer(metabuf);
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: segment catalog has %u entries, expected %u",
                            filled, out->nsegs)));
        }
    }

    UnlockReleaseBuffer(metabuf);

    /* Issue #244, shape (a): run on the private copy AFTER the metapage is released.
     * Nothing here needs the lock, and sorting the catalog under it would lengthen the
     * hold every writer waits on. */
    segcat_entries_unique_validate(out->segs, out->nsegs);     /* returns at once for n < 2 */
}

void
bm25_scan_snapshot(Relation index, BM25ScanSnapshot *out)
{
    /* Sampled here, before scan_snapshot_sampled takes the metapage SHARE. */
    scan_snapshot_sampled(index, out, RelationGetNumberOfBlocks(index));
}

/* Same sample-before-root contract as scan_snapshot_sampled: `nblocks` is taken before
 * bm25_meta_read hands back segcat_root, so it gets the same re-sampling bound. */
static void
segcat_read_sampled(Relation index, BM25SegCatEntry **out, uint32 *nsegs,
                    BlockNumber nblocks)
{
    BM25MetaPageData    meta;
    BM25SegCatEntry    *arr;
    uint32              n = 0;
    uint32              visited = 0;
    BlockNumber         blk;

    bm25_meta_read(index, &meta);
    *nsegs = meta.nsegs;
    if (meta.nsegs == 0)
    {
        *out = NULL;
        return;
    }
    arr = palloc(sizeof(BM25SegCatEntry) * meta.nsegs);

    blk = meta.segcat_root;
    while (blk != InvalidBlockNumber && n < meta.nsegs)
    {
        Buffer  buf;
        Page    pg;
        char   *cur;
        uint32  pn,
                i;
        BlockNumber next;

        CHECK_FOR_INTERRUPTS();

        segcat_visit_validate(visited++, meta.nsegs);  /* issue #244 */
        segcat_extent_validate(index, blk, &nblocks);  /* issue #225, as above */
        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        cur = (char *) PageGetContents(pg);
        pn = segcat_page_nentries(pg, blk);              /* issue #244 */
        for (i = 0; i < pn && n < meta.nsegs; i++)
        {
            memcpy(&arr[n], cur, sizeof(BM25SegCatEntry));
            segcat_entry_gen_validate(&arr[n++], blk);
            cur += MAXALIGN(sizeof(BM25SegCatEntry));
        }
        next = BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
        blk = next;
    }
    /* Fix (2026-08, crash/replica-safety pass): this used to fall straight through
     * to `*nsegs = n` here with no check, silently handing every one of this
     * function's many callers (including bm25_reclaim_orphans, which uses the
     * result to mark live segments' pages reachable, and bm25_bulkdelete, which
     * tombstones docs by walking it) an UNDERCOUNTED catalog on a short/truncated
     * chain -- a corrupt pd_lower, an out-of-extent nextblk, or a chain that ends
     * (InvalidBlockNumber) before contributing meta.nsegs entries all looked
     * identical to "here are the n segments that exist". For bm25_reclaim_orphans
     * that is the premature-free hazard Phase 4 exists to prevent, reached from
     * the other direction; for bm25_bulkdelete it is VACUUM silently skipping
     * live segments. Mirrors bm25_scan_snapshot's identical aggregate check
     * immediately above in this file -- loud beats silently wrong (ADR 0040). */
    if (n != meta.nsegs)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment catalog has %u entries, expected %u",
                        n, meta.nsegs)));
    /* Issue #244, shape (a). The count above is satisfied by a cycle that copies one
     * page's entries repeatedly, and every segment it crowded out is then missing from
     * the result -- for bm25_reclaim_orphans, the same premature-free hazard the check
     * above exists for. */
    segcat_entries_unique_validate(arr, n);
    *out = arr;
    *nsegs = n;
}

/* The unasserted copy of the catalog: for the debug SRFs, which read an index nobody is
 * guaranteed to be quiescing and accept that a concurrent merge-then-VACUUM can make
 * them fail loudly (bm25.h). Production code calls bm25_segcat_read_locked. */
void
bm25_segcat_read(Relation index, BM25SegCatEntry **out, uint32 *nsegs)
{
    segcat_read_sampled(index, out, nsegs, RelationGetNumberOfBlocks(index));
}

/* Whether this backend holds the seal/merge singleton -- the heavyweight page lock on
 * the metapage, LockPage's tag -- in ShareLock or any stronger mode. "Or stronger" is
 * by lock-mode number, so it admits the ExclusiveLock the seal, merge and reclaim take
 * as well as bulkdelete's ShareLock. The three-argument form is the same on 17, 18
 * and 19. Production code since issue #303 (bm25_seg_gen_mismatch); it was cassert-only
 * before. */
static bool
segcat_singleton_held(Relation index)
{
    LOCKTAG     tag;

    SET_LOCKTAG_PAGE(tag, index->rd_lockInfo.lockRelId.dbId,
                     index->rd_lockInfo.lockRelId.relId, BM25_METAPAGE_BLKNO);
    return LockHeldByMe(&tag, ShareLock, true);
}

/* Issue #270: the copy for production callers, which must hold the singleton. The
 * walk releases the metapage after reading segcat_root, and a catalog page orphaned by
 * a swap carries no retire_xid: the next bm25_reclaim_orphans frees it for immediate
 * reuse. Only the singleton keeps the swap and that reclaim out of the walk, so a
 * caller without it can read a page that has since become something else. The check is
 * cassert-only. Every caller today holds the singleton: bm25_bulkdelete (ShareLock),
 * bm25_reclaim_orphans, bm25_merge_execute (under bm25_merge_maybe) and
 * bm25_merge_rewrite_all (ExclusiveLock), and bm25_segcat_publish_swap, which only the
 * two merge paths call. */
void
bm25_segcat_read_locked(Relation index, BM25SegCatEntry **out, uint32 *nsegs)
{
    Assert(segcat_singleton_held(index));   /* checked: every caller takes LockPage first */
    bm25_segcat_read(index, out, nsegs);
}

/* Issue #270: entry 0 of the live catalog, for bm25_validate_key_config_for_insert,
 * which runs once per INSERT row and holds no singleton. It needs one entry, not the
 * catalog, because every segment carries the same key config (ADR 0067's induction).
 *
 * The root is read and copied under the metapage SHARE buffer lock, as
 * bm25_scan_snapshot does, and metapage-then-catalog is the index-wide lock order (ADR
 * 0018). That is what makes the copy safe without the singleton: a swap needs the
 * metapage EXCLUSIVE to orphan this root, so the root cannot be orphaned, reclaimed and
 * reused while this reads it. The same checks as the walkers apply: the format gate,
 * which since #302.A also refuses a segcat_root of 0 (this holds the metapage's content
 * lock, so ReadBuffer on block 0 is the PG 19 double lock scan_snapshot_sampled
 * describes), the extent bound with its re-sample, the kind, role and zero-entry
 * checks, and the entry's nonzero gen (#303.C). One page is read, so the visit cap
 * and the duplicate check, which bound multi-page walks, have nothing to bound.
 *
 * What the caller reads next is safe after the release, for a different reason. The
 * entry names a segment header, and a merge that drops that segment puts its pages on
 * the retired list with retire_xid = the next xid at the swap (bm25_segcat_publish_swap).
 * The inserter's active snapshot was taken before it read this entry, which was before
 * the swap, so its advertised xmin is older than that retire_xid, and
 * GlobalVisCheckRemovableFullXid keeps the pages out of reuse until the insert ends --
 * the bound bm25_scan_build_ranking states for scans. bm25_seg_header_read and
 * bm25_seg_keymeta also check seg_gen before kind, so a reuse the horizon did not
 * prevent raises 40001, which a client can retry, not XX002.
 *
 * `nblocks` is sampled before the metapage lock, as for scan_snapshot_sampled; the
 * public entry point takes the sample, bm25_debug_segcat_walk passes a stale one. */
static bool
first_entry_sampled(Relation index, BM25SegCatEntry *out, BlockNumber nblocks)
{
    Buffer              metabuf;
    Buffer              buf;
    BM25MetaPageData   *meta;
    BlockNumber         root;
    uint32              nsegs;
    Page                page;

    metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    meta = BM25PageGetMeta(BufferGetPage(metabuf));
    bm25_meta_validate(meta);
    nsegs = meta->nsegs;
    root = meta->segcat_root;
    if (nsegs == 0)
    {
        UnlockReleaseBuffer(metabuf);
        return false;
    }

    /* Errors below are raised with the metapage held; abort releases it, as for the
     * same checks in scan_snapshot_sampled. Same messages as the copying walkers, so a
     * corrupt catalog reads the same whichever path meets it first. */
    if (root == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment catalog has %u entries, expected %u", 0, nsegs)));
    segcat_extent_validate(index, root, &nblocks);
    /* No block-0 test here: bm25_meta_validate above refuses a segcat_root of 0
     * (issue #302.A), which is what keeps this ReadBuffer off the metapage buffer
     * this backend holds. */
    buf = ReadBuffer(index, root);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    /* A legitimate empty page is only ever the last page of a chain, so an empty root
     * means a catalog of zero entries, which nsegs > 0 contradicts. */
    if (segcat_page_nentries(page, root) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment catalog has %u entries, expected %u", 0, nsegs)));
    memcpy(out, PageGetContents(page), sizeof(BM25SegCatEntry));
    segcat_entry_gen_validate(out, root);
    UnlockReleaseBuffer(buf);
    UnlockReleaseBuffer(metabuf);
    return true;
}

bool
bm25_segcat_first_entry(Relation index, BM25SegCatEntry *out)
{
    return first_entry_sampled(index, out, RelationGetNumberOfBlocks(index));
}

/* Option (d) + page-kind (H6): validate a followed segment page's stamped
 * generation AND its stamped page kind. Call while holding the page's content
 * lock (so neither field can be read torn against the replay-time FPI re-init).
 *
 * gen: a mismatch means a reclaimed-and-reused page (gens never repeat => no
 * ABA); abort the scan cleanly rather than read recycled bytes.
 * expected_gen == 0 means "don't validate" (non-segment caller, or a caller that
 * carries no gen -- see bm25_seg_dict_iter_next and bm25_livedocs_clear).
 *
 * want_kind: the BM25_PAGE_* bit (or OR of bits) this chain is allowed to reach.
 * seg_gen alone CANNOT separate the chains WITHIN one segment -- DICT, POST,
 * NORMS, LIVE, DOCMAP, KEYMAP, POS and the header all carry the SAME seg_gen --
 * so a corrupt BM25SegmentHeader.dict_root aimed at this segment's own NORMS
 * chain passed gen validation and was decoded as BM25DictEntry records. The
 * result was a silently wrong dictionary, not the loud ERRCODE_INDEX_CORRUPTED
 * the rest of the trust boundary promises. want_kind == 0 means "don't validate"
 * (the bm25_seg_page_validate wrapper below).
 *
 * ORDER IS LOAD-BEARING -- gen first, kind second. The two failures are not
 * interchangeable: bm25_scan_build_ranking CATCHES
 * ERRCODE_T_R_SERIALIZATION_FAILURE and retries the scan up to three times with
 * a fresh snapshot, which is exactly right for a concurrently reclaimed segment.
 * A reclaimed page that has ALREADY been re-initialized as some other kind fails
 * BOTH checks; running the kind check first would report that benign
 * merge/reclaim race as non-retryable corruption and break the retry. With gen
 * first, the kind check only ever fires on a page whose gen MATCHES -- i.e. the
 * genuine SEGREAD-06 case, a wrong chain inside the right segment -- which is
 * corruption, is not retryable, and must propagate.
 *
 * The kind test is `(flags & want_kind) != 0`, never equality:
 * bm25_page_mark_deleted ORs BM25_PAGE_DELETED onto a live kind, and a page can
 * legitimately be read while carrying both bits. */
void
bm25_seg_page_validate_kind(Page page, uint32 expected_gen, uint16 want_kind)
{
    uint16 flags;

    if (expected_gen != 0 &&
        BM25PageGetOpaque(page)->seg_gen != expected_gen)
        ereport(ERROR,
                (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                 errmsg("bm25: segment reclaimed concurrently; retry")));

    if (want_kind == 0)
        return;

    flags = BM25PageGetOpaque(page)->flags;
    if ((flags & want_kind) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment page is not of the expected kind"),
                 errdetail("The page's flags are 0x%x; expected one of 0x%x.",
                           flags, want_kind)));
}

/* The gen arm (issue #303, D1). bm25_seg_page_validate_kind reads every gen mismatch as
 * a concurrent reclaim and raises 40001, which the ranking build retries and a client
 * may retry forever. That is right for the race and wrong for a corrupt link into
 * another segment's page, which is the same mismatch. What tells them apart is whether
 * the walk's own segment is still live: retirement removes a segment's catalog entry
 * before any of its pages can be reclaimed, WAL replay keeps that order on a standby,
 * and gens never repeat -- so a live segment never owns a page carrying another gen.
 *
 * How the catalog is read matters (design check R1). bm25_segcat_find_entry walks it
 * with no metapage lock, which ADR 0107 forbids outside the singleton: a catalog chain
 * orphaned by a swap is freed for immediate reuse, so that walk can meet a reused page
 * and raise XX002 itself -- turning the legitimate 40001 this arm exists to keep into a
 * spurious corruption error, on exactly the standby where the race happens.
 * bm25_scan_snapshot holds the metapage SHARE across its walk, so the chain cannot be
 * orphaned under it (a swap needs the metapage EXCLUSIVE; on a standby, the swap's redo
 * waits on the same buffer lock). It runs only on this error path, so its cost does not
 * matter; the caller holds no content lock, so it adds no lock-order edge. A backend
 * holding the singleton cannot see its segments retired at all, so it needs no read. */
void
bm25_seg_gen_mismatch(Relation index, BlockNumber blk, uint32 page_gen, uint32 want_gen,
                      const char *what)
{
    bool        live;

    if (segcat_singleton_held(index))
        live = true;
    else
    {
        BM25ScanSnapshot snap;
        uint32      i;

        bm25_scan_snapshot(index, &snap);
        live = false;
        for (i = 0; i < snap.nsegs && !live; i++)
            live = (snap.segs[i].gen == want_gen);
        if (snap.segs != NULL)
            pfree(snap.segs);
    }

    if (live)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s reaches block %u, a page of segment gen %u, not %u",
                        what, blk, page_gen, want_gen),
                 errdetail("Segment gen %u is still live, so none of its pages can have "
                           "been reclaimed.", want_gen),
                 errhint("REINDEX the index.")));
    ereport(ERROR,
            (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
             errmsg("bm25: segment reclaimed concurrently; retry")));
}

/* Gen-only check. The segment header readers (bm25_seg_header_read and _lens) call it
 * first so a reused page raises the retryable 40001 before bm25_segcat_page_validate
 * checks the kind and role. */
void
bm25_seg_page_validate(Page page, uint32 expected_gen)
{
    bm25_seg_page_validate_kind(page, expected_gen, 0);
}

/* Decode-boundary probe for the page-kind half of bm25_seg_page_validate_kind.
 * No bm25_debug_* lever can aim a real chain at a real wrong-kind page (that
 * needs a corrupt root pointer, and the roots live inside the segment header),
 * so -- exactly like bm25_debug_retired_page_bounds and every other pure probe
 * in this family -- this drives the SAME check over caller-chosen values.
 * expected_gen is fixed at 0 so only the kind half is under test; the gen half
 * is unreachable from SQL by construction and is covered by the real
 * concurrent-reclaim paths. TEST-ONLY: pure function of its scalar arguments,
 * no relation touched. Returns want_kind on success. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_page_kind_validate);
Datum
bm25_debug_seg_page_kind_validate(PG_FUNCTION_ARGS)
{
    int32           flags     = PG_GETARG_INT32(0);
    int32           want_kind = PG_GETARG_INT32(1);
    PGAlignedBlock  page;

    if (flags < 0 || flags > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_page_kind_validate: flags out of uint16 range")));
    if (want_kind < 0 || want_kind > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_page_kind_validate: want_kind out of uint16 range")));

    /* A real, fully-initialized page rather than a bare stack PageHeaderData
     * (the shape bm25_debug_page_content_bytes gets away with): the function
     * under test reaches the opaque through PageGetSpecialPointer, which asserts
     * on pd_special in a cassert build. bm25_page_init leaves seg_gen = 0, which
     * is what the expected_gen = 0 below pairs with. */
    bm25_page_init((Page) page.data, (uint16) flags);

    bm25_seg_page_validate_kind((Page) page.data, 0, (uint16) want_kind);
    PG_RETURN_INT32(want_kind);
}

/*
 * bm25_page_content_bytes -- the decode boundary for a page's content byte count.
 *
 * Every reader that derives a length from pd_lower (a chain page's content span in
 * chain_read_at, a KEYMAP page's key[] span in seg_key_cur, ...) computes it as
 * `pd_lower - SizeOfPageHeaderData`. PageIsVerifiedExtended enforces
 * pd_lower <= pd_upper <= pd_special <= BLCKSZ but NOT a lower bound on pd_lower --
 * PageIsEmpty and an uninitialized page are both pd_lower <= SizeOfPageHeaderData
 * by design, down to and including 0 -- so a torn or hostile page can carry a
 * pd_lower small enough to underflow that subtraction to just under SIZE_MAX,
 * which every downstream length/offset comparison would then read as "plenty of
 * room" instead of corruption.
 *
 * Callers (every read-side derivation of a page's content length in the tree, as
 * of the 2026-08 trust-boundary sweep): chain_read_at (bm25_seg_chain.c) and the
 * DICT-chain walkers bm25_seg_dict_lookup / bm25_seg_dict_iter_next
 * (bm25_seg_dict.c) / bm25_debug_segterms (bm25_seg_debug.c); seg_key_cur
 * (bm25_keymap.c); the pending-list iterator and POS cursor (bm25_pending.c /
 * bm25_seg_chain.c); the SEGCAT walkers bm25_scan_snapshot,
 * bm25_segcat_read, bm25_segcat_find_entry, bm25_segcat_locate_entry, and
 * bm25_segcat_first_entry's root read (this file); the field-config walker
 * bm25_fieldcfg_read (bm25_analyzer.c);
 * bm25_debug_terms / bm25_debug_postings (bm25_segment.c); and all four
 * retired-list walkers -- bm25_reclaim_orphans's Phase 4, bm25_reclaim_retired,
 * bm25_debug_retired_page_bounds (a caller-argument probe, not a page read), and
 * bm25_debug_retired_count (bm25_fsm.c).
 *
 * The POST-block readers (bm25_seg_scan_postings and bm25_seg_block_header_read
 * in bm25_seg_chain.c, wand_cursor_load_block in bm25_wand.c) route through this
 * function too.
 * They were the last holdouts: each already had an explicit
 * `cur + sizeof(BM25BlockHeader) > pend` check that a corrupt pd_lower below
 * SizeOfPageHeaderData trips unconditionally (cur is always >=
 * PageGetContents(pg), so it is past a pend that underflowed toward pg itself),
 * which made them MEMORY-safe -- but the guard's outcome there is "no blocks on
 * this page", so the term's postings vanished SILENTLY instead of erroring. That
 * is the same silent-wrong-answer shape this sweep closed in the DICT walkers,
 * and on the WAND path it is an ordinary ranked query, so they are bounded here
 * rather than left to a guard that only happens to be safe.
 *
 * The catalog appender's capacity check (bm25_segcat_publish_append's
 * `pd_lower + n * entry > pd_upper`) reads back an EXISTING segcat root page; since
 * issue #302.A it runs after bm25_segcat_page_validate, which calls this function.
 *
 * Deliberately NOT routed through this function, because they are a different
 * case, not an oversight: the KEYMAP/segment-builder WRITE paths (chain_write,
 * chain_write_stream, bm25_keymap_write, bm25_fieldcfg_write, and the rest of
 * bm25_seg_build.c) read pd_lower back only on a page THIS SAME CALL allocated and
 * bm25_page_init'd moments earlier -- not on-disk trust boundaries, since nothing
 * untrusted has touched the page yet.
 *
 * Returns the content byte count so callers do not recompute the subtraction.
 *
 * Issue #302.D: it also checks pd_special, because every caller reads the page's
 * BM25PageOpaque next and BM25PageGetOpaque is a bare page + pd_special. Core's
 * PageIsVerified accepts pd_special == BLCKSZ, which puts the 24-byte opaque wholly
 * past the block: flags, nextblk and seg_gen would come from the neighbouring buffer,
 * and a stamp would land there. bm25 writes BM25_PAGE_SPECIAL_OFF on every page
 * (bm25_page_init), so anything else is corruption. Checked after the pd_lower test,
 * so an all-zero page keeps its existing message. Being here rather than in
 * BM25PageGetOpaque is deliberate: that accessor also runs on registered copies inside
 * WAL windows, where nothing may throw (ADR 0083).
 */
Size
bm25_page_content_bytes(Page pg)
{
    uint32 pd_lower = ((PageHeader) pg)->pd_lower;
    uint32 pd_upper = ((PageHeader) pg)->pd_upper;

    if (pd_lower < SizeOfPageHeaderData || pd_lower > pd_upper || pd_upper > BLCKSZ)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment page has an invalid content boundary"),
                 errdetail("The page has pd_lower %u and pd_upper %u; its header needs %zu bytes.",
                           pd_lower, pd_upper, SizeOfPageHeaderData)));
    bm25_page_special_validate(pg, InvalidBlockNumber);
    return (Size) (pd_lower - SizeOfPageHeaderData);
}

/* The pd_special half of bm25_page_content_bytes, for the opaque readers that do not
 * derive a content length (bm25_page_mark_deleted, the orphan sweep). blk names the
 * page in the message; InvalidBlockNumber for callers holding only a Page. */
void
bm25_page_special_validate(Page pg, BlockNumber blk)
{
    uint32 pd_special = ((PageHeader) pg)->pd_special;

    if (bm25_page_special_ok(pg))
        return;
    if (blk == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: page has an invalid special area"),
                 errdetail("The page's pd_special is %u; every bm25 page has %zu.",
                           pd_special, (Size) BM25_PAGE_SPECIAL_OFF)));
    ereport(ERROR,
            (errcode(ERRCODE_INDEX_CORRUPTED),
             errmsg("bm25: page %u has an invalid special area", blk),
             errdetail("The page's pd_special is %u; every bm25 page has %zu.",
                       pd_special, (Size) BM25_PAGE_SPECIAL_OFF)));
}

/*
 * bm25_segcat_page_validate -- the role check for a BM25_PAGE_SEGCAT page (issues #276,
 * #302.A, #303.C).
 *
 * Scope (the on-disk validation contract, decision D1 of the 2026-10-05 grind): a
 * corrupt bm25 page, checksum-valid and of any origin, must produce
 * ERRCODE_INDEX_CORRUPTED rather than a crash, an out-of-bounds access, an unbounded or
 * uncancellable wait, a write into a page of another kind, or the freeing of a
 * reachable page. A silently wrong answer from in-range values is caught only where the
 * decode boundary has a cheap structural invariant (count, order, kind, gen, span);
 * otherwise it is a documented residual.
 *
 * Catalog pages and segment header pages share the SEGCAT bit (bm25_seg_header_read
 * explains why there is no separate bit), so the kind check alone let a catalog walk or
 * the catalog appender take a header page for a catalog page, and a header read with no
 * expected gen take a catalog page for a header. seg_gen is the cheap invariant that
 * separates them: bm25_segcat_build_orphan_chain and the appender init catalog pages
 * with seg_gen 0, the builder stamps a header with its segment's gen, and gens start at
 * 1 (bm25_meta_fill). Order: content bounds first (which also rejects an all-zero page
 * and a misplaced special area before the opaque read), then the kind, then the role.
 * Returns the content byte count. Callers that pass an expected gen still run their
 * gen check first (bm25_seg_header_read), so a concurrently reused page keeps raising
 * the retryable 40001.
 */
Size
bm25_segcat_page_validate(Page pg, BM25SegcatRole role, BlockNumber blk)
{
    Size        bytes = bm25_page_content_bytes(pg);
    uint32      seg_gen;

    bm25_seg_page_validate_kind(pg, 0, BM25_PAGE_SEGCAT);
    seg_gen = BM25PageGetOpaque(pg)->seg_gen;
    if (role == BM25_SEGCAT_ROLE_CATALOG && seg_gen != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment catalog page %u is a segment header page", blk),
                 errdetail("It carries generation %u; catalog pages carry 0.", seg_gen)));
    if (role == BM25_SEGCAT_ROLE_HEADER && seg_gen == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment header page %u is a segment catalog page", blk),
                 errdetail("It carries generation 0; a segment header carries its "
                           "segment's generation.")));
    return bytes;
}

/* Reject an on-disk BlockNumber before it reaches ReadBuffer (SEGREAD-05, QRY-01).
 *
 * THE VALUE THAT MATTERS IS InvalidBlockNumber, because P_NEW IS InvalidBlockNumber
 * (storage/bufmgr.h). A segment root, chain link or DICT pointer corrupted to 0xFFFFFFFF
 * therefore does not fail the read -- it EXTENDS THE RELATION, from a read-only scan,
 * and hands back a freshly zeroed page. The extension is the damage; what follows is
 * merely confusing. That zero page then reaches bm25_seg_page_validate_kind, whose
 * FIRST test is the gen check, so an all-zero opaque raises
 * ERRCODE_T_R_SERIALIZATION_FAILURE ("segment reclaimed concurrently; retry") -- which
 * bm25_scan_build_ranking CATCHES and retries up to three times. One corrupt block
 * number can thus extend the relation three times per query and then surface a
 * misleading retry error instead of a single ERRCODE_INDEX_CORRUPTED.
 *
 * Block 0 is rejected for the second half of that story rather than for safety: the
 * metapage is a real, readable page, so ReadBuffer succeeds and the gen check fires the
 * same retryable error on a page that is not a segment page at all. Naming it here
 * reports corruption once, correctly, instead of three silent retries.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO is bound blkno against RelationGetNumberOfBlocks.
 * That check reads as the obvious third clause; it is omitted for two reasons, and
 * NEITHER of them is correctness.
 * (a) It is redundant. For any block past EOF, ReadBuffer already raises a loud
 *     ERRCODE_DATA_CORRUPTED short-read error from mdreadv -- out-of-range is the case
 *     the buffer manager handles well. InvalidBlockNumber is precisely the value it
 *     handles by growing the file instead, which is why that one is named above.
 * (b) It is not free. RelationGetNumberOfBlocks reaches mdnblocks, which lseeks on
 *     every call in a normal backend: smgrnblocks_cached consults the cache ONLY under
 *     InRecovery (verified in PG 17 and 18 -- "lack of a shared invalidation mechanism
 *     for changes in file size"). This function is called once per posting-block load,
 *     so a syscall here lands in the innermost loop of every ranked scan.
 * If a future change does want the extent bound, the shape to copy is pos_cursor_load's
 * (bm25_seg_chain.c): capture nblocks ONCE when the cursor opens and compare against
 * the captured value, rather than paying the lseek per block.
 *
 * An earlier draft of this comment justified the omission by claiming a per-backend
 * nblocks cache would go stale under a concurrent merge and produce spurious
 * corruption errors. That is false in both supported versions -- the cache is
 * recovery-only -- and it is recorded here because the wrong reason is more memorable
 * than the right one.
 *
 * `what` names the pointer for the error detail (the caller knows whether this is a
 * segment header, a postings root or a chain link; the validator cannot). */
void
bm25_seg_blkno_validate(BlockNumber blkno, const char *what)
{
    if (blkno == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s is InvalidBlockNumber", what),
                 errdetail("InvalidBlockNumber is P_NEW; reading it would extend the "
                           "relation instead of failing.")));

    if (blkno == BM25_METAPAGE_BLKNO)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s points at the metapage (block %u)",
                        what, BM25_METAPAGE_BLKNO)));
}

/* Loop-level extent backstop for a chain walk (SEGREAD-11, issue #154).
 *
 * NOT the third clause ADR 0071 declined above, and the difference is what `nblocks`
 * costs at each site. bm25_seg_blkno_validate is a PER-POINTER validator called once
 * per posting-block load; giving it an extent bound would put an lseek in that loop,
 * which is what 0071 refused. This is a PER-WALK bound whose caller has already paid
 * for `nblocks` ONCE -- at function entry for a per-term walk, or at cursor open for a
 * per-posting one, exactly the shape 0071 names as the one to copy. Both guards are
 * wanted at the sites that have both: 0071 catches InvalidBlockNumber (P_NEW, which
 * would EXTEND the relation) before any read, this one catches a link that aims past
 * the extent, and the two values are disjoint.
 *
 * LOUD, not a quiet loop-condition terminate. `while (blk != InvalidBlockNumber && blk
 * < nblocks)` is the form bm25_segcat_find_entry, bm25_segcat_locate_entry (through
 * bm25_blk_in_extent, which re-samples first -- see below) and the three debug DICT
 * dumps use, and it is right for them: a catalog probe that reports
 * "gen not live" and a debug dump that reports what it could reach both have a
 * defensible truncated answer. The walkers this helper serves have none -- ADR 0095's
 * four and, since issue #225, the SEGCAT copies and the KEYMAP walk. For example,
 * bm25_seg_dict_lookup would report a term ABSENT (silently dropping matching rows),
 * bm25_seg_dict_iter_next would hand the merge a short dictionary (silently writing a
 * permanently incomplete segment), and chain_read_at would stop at a chain end its
 * callers used to read as "treat as live" / an invalid TID / doclen 0 (silently wrong
 * scores; reaching the chain's end early now ERRORs too, issue #294).
 * This is the same call the query-path wildcard walk in bm25_dict_expand_wildcard
 * makes, for the same reason, and its header states the rule in full.
 *
 * `nblocks` may be captured well before the comparison. That is sound because nothing
 * in this extension shrinks an index relation -- there is no RelationTruncate or
 * smgrtruncate anywhere in src/ -- so a captured value is a LOWER bound on the extent
 * for the whole walk and can never reject a block that legitimately exists.
 *
 * That argument has a precondition the sentence above leaves implicit, and the SEGCAT
 * walkers are the one place it fails: the capture must come AFTER the chain being
 * walked was published. Every walker here satisfies it -- a segment's pages are all
 * allocated before the publish record that makes the segment reachable, and every
 * reader or per-term walk is opened on a segment already in the caller's catalog
 * snapshot. The SEGCAT walkers sample BEFORE they read the root, so they take the
 * re-sampling form, segcat_extent_validate (issue #225).
 *
 * Block 0 (issue #303). The metapage is inside every extent, so the bound above let a
 * chain link to it through: the walker read the metapage, whose seg_gen 0 failed the
 * gen check as ERRCODE_T_R_SERIALIZATION_FAILURE, and the ranking build retried that
 * three times before reporting a "concurrent reclaim" that never happened. No segment
 * chain ever links to block 0, so it is corruption here, and named. The callers that
 * reach this only for a block already out of the extent (the SEGCAT and pending
 * walkers, through bm25_blk_in_extent first) never pass block 0, so the clause cannot
 * fire under a held metapage lock. */
void
bm25_seg_chain_extent_validate(BlockNumber blk, BlockNumber nblocks, const char *what)
{
    if (blk == BM25_METAPAGE_BLKNO)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s links to block %u, the metapage", what,
                        BM25_METAPAGE_BLKNO),
                 errhint("REINDEX the index.")));
    if (blk >= nblocks)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s leaves the index", what),
                 errdetail("Block %u is past the end of the relation, which has %u blocks.",
                           blk, nblocks)));
}

/* Bound a DICT-supplied byte offset before it is added to a page pointer
 * (SEGREAD-04, XCUT-06).
 *
 * bm25_dictentry_validate bounds the entry header and the MAXALIGN'd term span, but
 * it never looks at post_off or pos_post_off -- so both flow raw from the page into
 * `PageGetContents(pg) + off`. off is a uint16, so that is pointer arithmetic up to
 * 65535 bytes into an 8 KB object: undefined behaviour on formation, before anything
 * is dereferenced. In pos_cursor_open the object is a palloc(BLCKSZ) copy (the
 * ADR 0063 copy-then-unlock shape), which makes it a plain heap overrun rather than
 * a page overrun, but it is the same defect.
 *
 * The two sibling readers that first guarded this exact quantity open-coded it
 * (bm25_seg_block_header_read, and wand_cursor_load_block in bm25_wand.c), after the
 * pointer was already formed; this is their check, named, so the readers that lacked
 * it cannot drift again. bm25_seg_block_header_read_lead now calls it before forming
 * the pointer (issue #312); wand_cursor_load_block still tests the formed pointer
 * against the content end.
 *
 * `>` and not `>=`: an offset landing exactly at the content end is a legal
 * one-past-the-end pointer, and the callers' own "does a block header still fit"
 * guards handle the empty tail. Rejecting it here would additionally reject a
 * zero-posting term whose root/offset pair is not yet meaningful (see BUILD-07),
 * which is a different bug with a different fix. */
void
bm25_seg_page_off_validate(uint32 off, Size pagebytes, const char *what)
{
    if ((Size) off > pagebytes)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s offset %u is past the page's %zu content bytes",
                        what, off, pagebytes)));
}

/* Decode-boundary probe for bm25_seg_page_off_validate. Pure function of its two
 * scalar arguments, driven directly for the same reason as its siblings: a corrupt
 * post_off lives inside a DICT entry that no debug lever can write. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_page_off_validate);
Datum
bm25_debug_seg_page_off_validate(PG_FUNCTION_ARGS)
{
    int32 off       = PG_GETARG_INT32(0);
    int32 pagebytes = PG_GETARG_INT32(1);

    if (off < 0 || off > (int32) PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_page_off_validate: off %d is outside uint16", off)));
    if (pagebytes < 0 || pagebytes > BLCKSZ)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_page_off_validate: pagebytes %d is outside [0, %d]",
                        pagebytes, BLCKSZ)));

    bm25_seg_page_off_validate((uint32) off, (Size) pagebytes, "probed");
    PG_RETURN_INT32(off);
}

/* Decode-boundary probe for bm25_seg_blkno_validate. Pure function of its argument,
 * so the suite drives it directly rather than manufacturing a corrupt root pointer
 * (the roots live inside the segment header, which no debug lever can aim). Returns
 * the block number unchanged when it passes, so a test can assert both halves. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_blkno_validate);
Datum
bm25_debug_seg_blkno_validate(PG_FUNCTION_ARGS)
{
    int64 blkno = PG_GETARG_INT64(0);

    if (blkno < 0 || blkno > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_blkno_validate: blkno " INT64_FORMAT
                        " is outside the BlockNumber range", blkno)));

    bm25_seg_blkno_validate((BlockNumber) blkno, "probed block pointer");
    PG_RETURN_INT64(blkno);
}

/* Decode-boundary probe (2026-08 page-content-bytes sweep). bm25_page_content_bytes
 * only fires on an already-corrupt page, which a regression suite cannot produce,
 * so this drives the SAME function over a caller-chosen (pd_lower, pd_upper) pair
 * on a synthetic page header built on the stack -- a struct assignment, not a raw
 * bytea, so the suite stays host-endian/padding independent, same reasoning as
 * bm25_debug_block_validate's header-by-fields approach. pd_special is set to the value
 * every bm25 page carries, so only pd_lower/pd_upper are under test; the rest of the
 * header (and the page body it never reads) is left zeroed. TEST-ONLY: a pure function of its scalar arguments,
 * no relation touched, covered by the install script's REVOKE loop like every other
 * bm25_debug_* function. */
PG_FUNCTION_INFO_V1(bm25_debug_page_content_bytes);
Datum
bm25_debug_page_content_bytes(PG_FUNCTION_ARGS)
{
    int32          pd_lower = PG_GETARG_INT32(0);
    int32          pd_upper = PG_GETARG_INT32(1);
    PageHeaderData hdr;

    if (pd_lower < 0 || pd_lower > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_page_content_bytes: pd_lower out of uint16 range")));
    if (pd_upper < 0 || pd_upper > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_page_content_bytes: pd_upper out of uint16 range")));

    memset(&hdr, 0, sizeof(hdr));
    hdr.pd_lower = (LocationIndex) pd_lower;
    hdr.pd_upper = (LocationIndex) pd_upper;
    hdr.pd_special = (LocationIndex) BM25_PAGE_SPECIAL_OFF;   /* #302.D: as on a real page */

    PG_RETURN_INT64((int64) bm25_page_content_bytes((Page) &hdr));
}

/*
 * bm25_segheader_validate -- the decode boundary for a segment header.
 *
 * field_count arrives by raw memcpy off the page and is then used as a LOOP BOUND
 * over caller arrays documented as ">= BM25_MAX_FIELDS": bm25_segheader_read_lenfields
 * writes field_count uint64s into them, and bm25_field_corpus_stats' consuming loop is
 * bounded by the same on-disk value while its seglen/segndocs are BM25_MAX_FIELDS stack
 * arrays. A corrupted field_count therefore smashes a stack frame on the scored-scan
 * hot path, reached from an ordinary `ORDER BY col &@@ q`.
 *
 * bm25_seg_page_validate does not cover this -- it compares seg_gen only. The
 * identical quantity IS already guarded one file over, in bm25_fieldcfg_read
 * (bm25_analyzer.c); this makes the segment header consistent with that rather
 * than leaving the guard an exception.
 */
/* Confirm the page actually HOLDS a segment header before one is memcpy'd off it
 * (SEGREAD-15).
 *
 * The three readers copy a full sizeof(BM25SegmentHeader) out of PageGetContents
 * without ever consulting pd_lower, and bm25_segheader_read_lenfields then reads
 * field_count * 8 bytes twice more past the struct. Because field_count is bounded to
 * BM25_MAX_FIELDS first, the worst case stays inside the 8 KB page image -- so unlike
 * its siblings this is not an out-of-page overrun. What it is: a page whose content
 * stops short (a truncated write, a wrong-but-same-kind page) is read as though the
 * missing bytes were data, and page SLACK is handed back as ndocs, total_len, chain
 * roots and per-field sumdoclen. Those drive scoring and chain traversal, so the
 * failure is confident wrong answers rather than a crash, which is the harder kind to
 * notice. bm25_seg_key_header_validate (bm25_keymap.c) is the same check for the
 * KEYMAP root; this makes the segment header consistent with it.
 *
 * `need` is passed rather than derived so the caller that also reads the trailing
 * per-field arrays can demand their bytes too, in ONE check, before any of them is
 * touched. */
static void
bm25_segheader_span_validate(Page pg, BlockNumber header_blkno, Size need)
{
    Size pagebytes = bm25_page_content_bytes(pg);

    if (pagebytes < need)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment header page %u holds %zu content bytes, needs %zu",
                        header_blkno, pagebytes, need)));
}

static void
bm25_segheader_validate(const BM25SegmentHeader *hdr, BlockNumber header_blkno)
{
    if (hdr->field_count == 0 || hdr->field_count > BM25_MAX_FIELDS)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment header at block %u has invalid field_count %u "
                        "(maximum is %d)",
                        header_blkno, hdr->field_count, BM25_MAX_FIELDS)));

    /* Issue #313 REGR-06 (HDL-12). Local docids are uint32, so a segment cannot hold
     * more than PG_UINT32_MAX documents, and the per-docid loops (bm25_bulkdelete among
     * them) count in uint32 on that premise. A larger on-disk ndocs is corrupt. */
    if (hdr->ndocs > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment header at block %u has " UINT64_FORMAT
                        " documents (maximum is %u)",
                        header_blkno, hdr->ndocs, PG_UINT32_MAX)));

    /* Issue #294. A segment with documents always has all three dense per-docid
     * chains: the builder writes ndocs DOCMAP cells, ndocs * field_count NORMS cells
     * and ceil(ndocs / 8) LIVE bytes, each through a chain_write that allocates the
     * root. An InvalidBlockNumber root there is a whole chain missing, which every
     * per-docid read would otherwise meet only as "the chain ends at byte 0". */
    if (hdr->ndocs > 0 &&
        (hdr->livedocs_root == InvalidBlockNumber ||
         hdr->docmap_root == InvalidBlockNumber ||
         hdr->norms_root == InvalidBlockNumber))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment header at block %u has " UINT64_FORMAT
                        " documents but no %s chain",
                        header_blkno, hdr->ndocs,
                        hdr->livedocs_root == InvalidBlockNumber ? "live-docs" :
                        hdr->docmap_root == InvalidBlockNumber ? "doc-map" : "norms"),
                 errhint("REINDEX the index.")));

    /* Issue #302 C. No chain starts at the metapage: every root is InvalidBlockNumber
     * or a page bm25_page_alloc returned, which is never block 0. The merge swap copies
     * these roots into the retired descriptor from this read (bm25_retire_segment), and
     * reclaim_one_range later frees what they name, so a zero root caught here never
     * becomes a descriptor that frees the metapage. On the read paths it also turns the
     * retryable gen mismatch block 0 would raise (seg_gen 0) into this error. Roots
     * aimed at other in-extent pages are left to the chain walkers and to
     * reclaim_page_validate (bm25_fsm.c), which check kind and gen per page. */
    {
        static const char *const root_name[] = {
            "dictionary", "norms", "live-docs", "doc-map", "postings", "positions",
            "key-map"
        };
        const BlockNumber roots[] = {
            hdr->dict_root, hdr->norms_root, hdr->livedocs_root, hdr->docmap_root,
            hdr->posts_root, hdr->pos_root, hdr->keymap_root
        };
        int         i;

        StaticAssertStmt(lengthof(root_name) == lengthof(roots),
                         "segment header root names out of step with the roots");
        for (i = 0; i < (int) lengthof(roots); i++)
            if (roots[i] == BM25_METAPAGE_BLKNO)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: segment header at block %u names the metapage as "
                                "its %s chain root",
                                header_blkno, root_name[i]),
                         errhint("REINDEX the index.")));
    }
}

void
bm25_seg_header_read(Relation index, BlockNumber header_blkno,
                     uint32 expected_gen, BM25SegmentHeader *out)
{
    Buffer  buf;
    Page    pg;

    /* header_blkno arrives verbatim from a BM25SegCatEntry memcpy'd off a SEGCAT
     * page; neither bm25_scan_snapshot nor bm25_segcat_read validates any field of
     * that entry. Gate it BEFORE ReadBuffer -- afterwards is too late, the relation
     * has already grown. */
    bm25_seg_blkno_validate(header_blkno, "segment header block");
    buf = ReadBuffer(index, header_blkno);

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    /* The segment HEADER page is bm25_page_init(pg, BM25_PAGE_SEGCAT) -- there is
     * no separate BM25_PAGE_SEGHDR bit, and adding one would need a
     * format-version/upgrade story (ADR 0009), so header and catalog pages share
     * the SEGCAT bit deliberately. seg_gen separates the two: the builder stamps the
     * header with seg_gen = gen while catalog pages keep seg_gen = 0 and gens are
     * always >= 1 (bm25_seg_build.c). The expected-gen check runs first, so a page
     * reused under a scan still raises the retryable 40001; the role check after it
     * (issue #276) covers the caller that passes no gen, bm25_livedocs_clear, which
     * the gen check alone left unable to tell a catalog page from a header. */
    bm25_seg_page_validate(pg, expected_gen);
    (void) bm25_segcat_page_validate(pg, BM25_SEGCAT_ROLE_HEADER, header_blkno);
    bm25_segheader_span_validate(pg, header_blkno, sizeof(BM25SegmentHeader));
    memcpy(out, PageGetContents(pg), sizeof(BM25SegmentHeader));
    bm25_segheader_validate(out, header_blkno);
    /* This recovers the v4 fields (field_count/pos_root/keymap_root) the writer
     * placed in the struct. The length-prefixed total_len_by_field array follows
     * the struct on the page; a caller needing it must call
     * bm25_segheader_read_lenfields(pg, out, header_blkno, ...) BEFORE this UnlockReleaseBuffer
     * (the buffer is gone afterward). The M3 scorer uses out->total_len and does
     * not need the per-field array. */
    UnlockReleaseBuffer(buf);
}

/* Like bm25_seg_header_read, but ALSO copies the length-prefixed per-field arrays
 * that follow the struct on the page, under the SAME SHARE lock (bm25_seg_header_read
 * releases the buffer before a caller could read them). out_lens / out_ndocs must be
 * >= BM25_MAX_FIELDS. The BM25F scorer needs per-field sumdoclen AND N_field together; one
 * lock gives it the struct (field_count, roots) and both arrays atomically. seg_gen
 * validated exactly like bm25_seg_header_read (option (d)). For a single-field
 * segment the page carries no ndocs_by_field[], so bm25_segheader_read_lenfields
 * derives out_ndocs[0] from out->ndocs (see its comment). */
void
bm25_seg_header_read_lens(Relation index, BlockNumber header_blkno,
                          uint32 expected_gen, BM25SegmentHeader *out,
                          uint64 *out_lens, uint64 *out_ndocs)
{
    Buffer  buf;
    Page    pg;

    /* Same untrusted source as bm25_seg_header_read; same gate, same reason. */
    bm25_seg_blkno_validate(header_blkno, "segment header block");
    buf = ReadBuffer(index, header_blkno);

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    /* Header page: gen, then SEGCAT kind and header role, as bm25_seg_header_read. */
    bm25_seg_page_validate(pg, expected_gen);
    (void) bm25_segcat_page_validate(pg, BM25_SEGCAT_ROLE_HEADER, header_blkno);
    bm25_segheader_span_validate(pg, header_blkno, sizeof(BM25SegmentHeader));
    memcpy(out, PageGetContents(pg), sizeof(BM25SegmentHeader));
    bm25_segheader_validate(out, header_blkno);
    bm25_segheader_read_lenfields(pg, out, header_blkno, out_lens, out_ndocs);
    UnlockReleaseBuffer(buf);
}

/* bm25_segheader_read_lenfields -- read the per-field length arrays that follow the
 * BM25SegmentHeader on its page into caller buffers (each sized >= hdr->field_count).
 * The caller must have already populated *hdr (the field_count prefix lives in the
 * struct). Mirrors bm25_segheader_write_lenfields.
 *
 * out_ndocs_by_field may be NULL when the caller only wants sumdoclen. When non-NULL:
 *   field_count == 1: the on-page bytes carry ONLY total_len_by_field[0] (M3/C-BUILD
 *                     back-compat), so N_field[0] is derived from hdr->ndocs (every
 *                     live doc has the one field).
 *   field_count  > 1: ndocs_by_field[] is read from the page, immediately after the
 *                     total_len_by_field[] run. */
void
bm25_segheader_read_lenfields(Page pg, const BM25SegmentHeader *hdr,
                              BlockNumber header_blkno,
                              uint64 *out_total_len_by_field,
                              uint64 *out_ndocs_by_field)
{
    const char *arr = (const char *) PageGetContents(pg) + sizeof(BM25SegmentHeader);
    uint32      i;

    /* Validate HERE, not at the call site. hdr->field_count is a raw on-disk
     * uint32 and it is this function's loop bound over caller arrays documented
     * as ">= BM25_MAX_FIELDS" -- so a corrupt value writes up to 4G x 8 bytes
     * past a 256-byte stack array. bm25_segheader_validate has always existed
     * for exactly this, but it was a CALLER's duty to remember it, and one of
     * the two callers did not (bm25_debug_seg_lenfields, whose tlbf[] is such an
     * array). Folding it in closes the class rather than the one instance: the
     * bound cannot now be skipped by any present or future caller.
     *
     * The remaining explicit calls in bm25_seg_header_read / _read_lens are NOT
     * redundant with this one -- they validate the header struct they hand back
     * to the caller, whose field_count is then used for things other than this
     * array (per-field loop bounds in the scorer). */
    bm25_segheader_validate(hdr, header_blkno);

    /* And bound the page against what those loops are about to read (SEGREAD-15).
     * field_count is capped at BM25_MAX_FIELDS above, so this cannot leave the page
     * image -- but without it a short page is read as data and its slack is returned
     * as per-field sumdoclen and N_field, which feed avgdl and therefore every score.
     * One check for both arrays, before either is touched: a single-field page
     * carries only total_len_by_field[0] (ndocs_by_field is derived below), so the
     * requirement is one array for field_count == 1 and two otherwise. */
    bm25_segheader_span_validate(pg, header_blkno,
                                 sizeof(BM25SegmentHeader) +
                                 (Size) hdr->field_count * sizeof(uint64) *
                                 (hdr->field_count > 1 ? 2 : 1));

    for (i = 0; i < hdr->field_count; i++)
        memcpy(&out_total_len_by_field[i], arr + i * sizeof(uint64), sizeof(uint64));
    arr += (Size) hdr->field_count * sizeof(uint64);

    if (out_ndocs_by_field == NULL)
        return;

    if (hdr->field_count > 1)
    {
        for (i = 0; i < hdr->field_count; i++)
            memcpy(&out_ndocs_by_field[i], arr + i * sizeof(uint64), sizeof(uint64));
    }
    else
    {
        /* Single-field page carries no ndocs_by_field[]; derive it. */
        out_ndocs_by_field[0] = hdr->ndocs;
    }
}

static inline Size bm25_livedocs_bits_per_page(void);

/* True when the first h->ndocs bits of the segment's LIVEDOCS bitmap are all set,
 * read from the bitmap itself: one pass over the chain, one page per ~65k documents,
 * stopping at the first clear bit. A chain that ends before ceil(ndocs/8) bytes, or a
 * page short of the full span with more bitmap still to read, is corruption and
 * ERRORs, as seg_doc_is_live_cur's per-lookup read does (issue #294; ADR 0100 had
 * counted the missing bytes as set). Because bits only ever go from
 * set to clear after seal (bm25_livedocs_clear; only the builder sets them), a bit
 * seen set on any page was also set at every earlier instant, so a true here means
 * every bit was set when the walk BEGAN (the first page's read) -- not at its end: a
 * page read early may be tombstoned while later pages are read. The start of the walk
 * is still after the scan's MVCC snapshot, which is all the safety argument at
 * bm25_seg_reader_init_checked needs. Bits past ndocs are never examined; the builder's
 * padding there is not a document. Same walker (BM25SegWalk) as chain_read_at;
 * nblocks is the extent the caller's reader already captured, to avoid a second
 * RelationGetNumberOfBlocks lseek. */
static bool
seg_livedocs_all_set(Relation index, BM25SegmentHeader *h, BlockNumber nblocks)
{
    Size        whole = (Size) (h->ndocs / 8);     /* bytes that must be 0xFF */
    uint8       tailmask = (uint8) ((1u << (h->ndocs % 8)) - 1);
    Size        total = whole + (tailmask != 0 ? 1 : 0);
    Size        seen = 0;
    BlockNumber blk = h->livedocs_root;
    Size        span = bm25_chain_full_span(BM25_PAGE_LIVE);
    BM25SegWalk w;

    bm25_seg_walk_init(&w, index, nblocks, h->gen, BM25_PAGE_LIVE, false,
                       "segment LIVEDOCS chain");
    while (blk != InvalidBlockNumber && seen < total)
    {
        Buffer          buf;
        Page            pg;
        const uint8    *p;
        Size            pagebytes,
                        n,
                        i;
        BlockNumber     next;
        bool            all = true;

        CHECK_FOR_INTERRUPTS();

        /* Block 0, extent, revisit, cap, content bound, gen and kind (issue #303). */
        buf = bm25_seg_walk_read(&w, blk, &pagebytes);
        pg = BufferGetPage(buf);
        p = (const uint8 *) PageGetContents(pg);
        next = BM25PageGetOpaque(pg)->nextblk;
        /* A page the walk will continue past must carry the full span, or every later
         * byte would be read for the wrong documents (chain_read_at's rule). A short
         * LAST page falls through to the chain-end check below instead. */
        if (next != InvalidBlockNumber && pagebytes < total - seen && pagebytes != span)
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: live-docs chain page at block %u holds %zu content "
                            "bytes, not the %zu every page but the last carries",
                            blk, pagebytes, span),
                     errhint("REINDEX the index.")));
        }
        n = Min(pagebytes, total - seen);
        for (i = 0; i < n; i++)
        {
            uint8 want = (seen + i < whole) ? 0xFF : tailmask;

            if ((p[i] & want) != want)
            {
                all = false;
                break;
            }
        }
        UnlockReleaseBuffer(buf);
        if (!all)
            return false;
        seen += n;
        blk = next;
    }
    if (seen < total)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: live-docs chain ends at %zu bytes, %zu short of the "
                        "bitmap for " UINT64_FORMAT " documents",
                        seen, total - seen, h->ndocs),
                 errdetail("The segment's generation is %u.", h->gen),
                 errhint("REINDEX the index.")));
    return true;
}

/* Why a reader may stop reading LIVEDOCS per lookup (issue #229, ADR 0100).
 *
 * The evidence is the bitmap, checked once here -- never the catalog's live_ndocs.
 * The count is a second copy of the same fact, kept by a different writer protocol:
 * bm25_segcat_publish_swap republishes surviving entries from a copy taken before
 * its flip, and it is correct only because bm25_bulkdelete holds the seal/merge
 * singleton across its whole pass so no tombstone lands inside that window (#241).
 * A reader that trusted the count would return a vacuumed, possibly reused TID the
 * moment any writer broke that protocol, with the entry saying live_ndocs == ndocs
 * over a clear bit; the bits cannot disagree with themselves. The count is not
 * consulted even as a shortcut for segments it says are tombstoned: keeping the bits
 * the only input keeps the check testable on its own (sql/111 tombstones a segment's
 * last document to reach the tail byte).
 *
 * What bounds the check's cost instead is expected_lookups, the caller's upper bound
 * on how many liveness lookups the reader will make (the term's df in the df pass, the
 * summed df of the present terms in the WAND driver). The check runs only when that
 * is at least twice the bitmap's page count, so it can never cost more than half of
 * what it might save -- without the gate a rare term in a 10M-document segment would
 * walk ~150 bitmap pages to save a handful of lookups. Below the gate the reader reads
 * LIVEDOCS per lookup, as before.
 *
 * What changes is WHEN liveness is sampled: at reader init instead of at each lookup.
 * A doc VACUUM tombstones after the check is counted and scored as live -- exactly
 * what already happens to a dead tuple VACUUM has not reached yet -- and the
 * executor's MVCC heap check drops it the same way: its line pointer can be reused
 * only after the tombstone, which is after this check and so after the scan's MVCC
 * snapshot, making any new tuple there invisible to the scan. The ranked paths
 * already build their ranking once and return its TIDs later without re-reading any
 * bit, so no returned TID was ever liveness-checked at return time.
 *
 * Opt-in, per reader: VACUUM's bulkdelete and the merge replay decide what to
 * tombstone or drop from the bits themselves and must keep reading them per doc. */
bool
bm25_seg_reader_init_checked(BM25SegReader *r, Relation index, BM25SegmentHeader *h,
                             uint64 expected_lookups)
{
    uint64      pages = h->ndocs / bm25_livedocs_bits_per_page() + 1;

    bm25_seg_reader_init(r, index, h);
    r->assume_live = (expected_lookups >= 2 * pages &&
                      seg_livedocs_all_set(index, h, r->livedocs.nblocks));
    return r->assume_live;
}

/* For a second reader on the SAME segment, opened in the same call as the one whose
 * bm25_seg_reader_init_checked produced all_live -- the WAND driver hands its result
 * to each term's cursor rather than walking the bitmap once per cursor. */
void
bm25_seg_reader_init_known(BM25SegReader *r, Relation index, BM25SegmentHeader *h,
                           bool all_live)
{
    bm25_seg_reader_init(r, index, h);
    r->assume_live = all_live;
}

/* Read-only catalog lookup by generation (Task-0 contract). Same chain walk as
 * bm25_segcat_locate_entry but keyed on gen, copying the matching entry into *out
 * and returning true (false if the gen is no longer in the live catalog).
 *
 * TEST/DEBUG-ONLY: it has no production caller. The Phase-4 merge re-validation it
 * was reserved for never called it; its only caller is bm25_debug_segcat_walk, whose
 * find_entry / find_absent walkers drive this walk shape (extent re-sample, page cap,
 * kind and zero-entry checks) for the #225 and #244 suites. Kept for them, not as
 * API. */
static bool segcat_find_entry_sampled(Relation index, uint32 gen,
                                      BM25SegCatEntry *out, BlockNumber nblocks);

bool
bm25_segcat_find_entry(Relation index, uint32 gen, BM25SegCatEntry *out)
{
    return segcat_find_entry_sampled(index, gen, out,
                                     RelationGetNumberOfBlocks(index));
}

static bool
segcat_find_entry_sampled(Relation index, uint32 gen, BM25SegCatEntry *out,
                          BlockNumber nblocks)
{
    BM25MetaPageData    meta;
    BlockNumber         blk;
    uint32              visited = 0;

    bm25_meta_read(index, &meta);
    blk = meta.segcat_root;
    /* SEGREAD-07 (#139): nextblk has no acyclicity invariant enforced anywhere, and
     * this walk had NEITHER an interrupt check nor an extent bound -- as, it turned
     * out, neither did bm25_scan_snapshot nor bm25_segcat_read, which this comment
     * used to cite as bounded siblings; they had only the interrupt check until issue
     * #225. `blk < nblocks` is the corruption backstop that bm25_debug_terms
     * (bm25_segment.c) documents as the house pattern; terminating rather than
     * erroring on an out-of-extent pointer turns an unkillable spin into this
     * function's ordinary "gen not in the live catalog" answer.
     *
     * The sample precedes bm25_meta_read, so the test is bm25_blk_in_extent's
     * re-sampling form (issue #225): a bare `blk < nblocks` quietly reported a gen
     * published by a concurrent seal as "not live" whenever the seal had extended the
     * relation for its new catalog page after the sample.
     *
     * Issue #244: the extent bound ends a walk that leaves the index, and nothing ended
     * one that cycles inside it -- this walk has no entry count to run out. The page cap
     * does, and it is enough on its own here, with no duplicate check like the copying
     * walkers': this walk returns at its first match, and when the corruption is a
     * cycle every page it reads before the first revisit is a distinct page of the
     * chain, so a match found before the cap is where the entry really is. A gen the
     * cycle cuts off is never matched, and the walk ends at the cap with an error
     * instead of the "not live" answer. (A link to a stale, orphaned SEGCAT page is a
     * different corruption this walk does not detect; it is a residual.) */
    while (blk != InvalidBlockNumber && bm25_blk_in_extent(index, blk, &nblocks))
    {
        Buffer          buf;
        Page            pg;
        BM25SegCatEntry *ents;
        uint32          n,
                        i;
        BlockNumber     next;

        /* Lock-free instant: the previous iteration released its buffer, so
         * InterruptHoldoffCount is 0 here and the check actually fires. */
        CHECK_FOR_INTERRUPTS();

        segcat_visit_validate(visited++, meta.nsegs);
        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        ents = (BM25SegCatEntry *) PageGetContents(pg);
        /* Same SEGCAT chain, same checks as bm25_scan_snapshot / bm25_segcat_read. The
         * content bound matters for THIS caller in particular: a corrupt page that
         * silently contributed zero entries would make a live generation look like it
         * "is no longer in the live catalog" instead of erroring. */
        n = segcat_page_nentries(pg, blk);
        for (i = 0; i < n; i++)
        {
            if (ents[i].gen == gen)
            {
                memcpy(out, &ents[i], sizeof(BM25SegCatEntry));
                UnlockReleaseBuffer(buf);
                return true;
            }
        }
        next = BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
        blk = next;
    }
    return false;
}

/* Usable bits per LIVE page: the PageGetContents region (page minus the MAXALIGN'd
 * header, minus the opaque), 8 bits/byte. Identical for every LIVE page in a segment.
 *
 * COUPLING: this MUST equal the per-page span the LIVE-bitmap writer fills in
 * bm25_seg_build.c (its `maxspan`, BLCKSZ - SizeOfPageHeaderData - MAXALIGN(opaque)).
 * The two expressions differ only in MAXALIGN(SizeOfPageHeaderData) vs the bare form,
 * which are equal (PageHeaderData is 8-aligned, both == 24), so they yield the same
 * byte count. bm25_livedocs_locate divide-addresses by this constant, which is valid
 * ONLY because the writer packs every LIVE page but the last to exactly this span. */
static inline Size
bm25_livedocs_bits_per_page(void)
{
    Size contents = BLCKSZ - MAXALIGN(SizeOfPageHeaderData)
                          - MAXALIGN(sizeof(BM25PageOpaque));

    /* The readers' full-span check (bm25_chain_full_span, issue #294) states the same
     * span a third time; all three must agree. */
    Assert(contents == bm25_chain_full_span(BM25_PAGE_LIVE));     /* invariant */
    return contents * 8;
}

/* Walk the LIVE page chain to the page holding bit `local_docid`, returning the
 * block and the byte/bit within PageGetContents(). Errors if the chain is too short
 * (corruption -- the bitmap is sized for ndocs at seal). Only SHARE-locks the pages
 * it walks past; the caller re-pins the returned block under EXCL for the mutation. */
BlockNumber
bm25_livedocs_locate(Relation index, BlockNumber livedocs_root, uint32 gen,
                     uint32 local_docid, Size *byte_off, int *bit)
{
    Size        bits_per = bm25_livedocs_bits_per_page();
    uint32      page_ix = (uint32) (local_docid / bits_per);
    uint32      in_page = (uint32) (local_docid % bits_per);
    BlockNumber blk = livedocs_root;
    uint32      i;
    BM25SegWalk w;

    /* H6: this walk feeds bm25_livedocs_clear, which then takes the page it returns
     * EXCLUSIVE and WRITES it. Its caller, bm25_bulkdelete, holds the seal/merge
     * singleton ShareLock for its whole pass, so no swap can retire this segment and
     * no reclaim can reuse its pages under the walk (see bm25_livedocs_clear). The
     * kind and gen checks are therefore corruption backstops on the chain it follows,
     * not concurrency guards -- and the gen (issue #303; this walk carried none
     * before) is what refuses a same-kind link into ANOTHER segment's LIVE chain,
     * which the kind check passes: under the singleton a mismatch is XX002 outright
     * (bm25_seg_gen_mismatch). */
    bm25_seg_walk_init(&w, index, RelationGetNumberOfBlocks(index), gen, BM25_PAGE_LIVE,
                       false, "live-docs chain");
    for (i = 0; i < page_ix; i++)
    {
        Buffer      buf;
        Size        have;

        /* SEGREAD-07 (#139). page_ix already bounds this loop, but each iteration
         * follows an on-disk nextblk, so an out-of-extent pointer would have
         * ReadBuffer EXTEND the relation rather than fail. The walker's checks run
         * before ReadBuffer, in the lock-free instant the previous iteration's
         * UnlockReleaseBuffer opened. */
        CHECK_FOR_INTERRUPTS();

        if (blk == InvalidBlockNumber)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: live-docs chain too short for docid %u",
                            local_docid)));
        buf = bm25_seg_walk_read(&w, blk, &have);
        /* The divide above is sound only if every page walked past is full (issue
         * #294): a short one would have this walk tombstone a different bit than the
         * one chain_read_at reads for the same docid. */
        if (have != bits_per / 8)
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: live-docs chain page at block %u holds %zu content "
                            "bytes, not the %zu every page but the last carries",
                            blk, have, bits_per / 8),
                     errhint("REINDEX the index.")));
        }
        blk = BM25PageGetOpaque(BufferGetPage(buf))->nextblk;
        UnlockReleaseBuffer(buf);
    }
    if (blk == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: live-docs chain too short for docid %u",
                        local_docid)));
    /* The block this function RETURNS is re-pinned EXCLUSIVE and written by
     * bm25_livedocs_clear, so it gets the walker's pre-read checks too (block 0,
     * extent, revisit, cap) -- including the page_ix == 0 case, where the loop above
     * never ran. bm25_livedocs_clear checks what it reads there. */
    (void) bm25_seg_walk_check(&w, blk);
    *byte_off = in_page / 8;
    *bit = (int) (in_page % 8);
    return blk;
}

/* Scan the catalog chain for the entry whose header_blkno matches; return its
 * catalog block + slot so the caller can mutate it under a WAL window. Errors if
 * not found (segment vanished). Distinct from the read-only, test/debug-only
 * bm25_segcat_find_entry (which copies by gen) -- this returns the physical block+slot for an in-place
 * Generic WAL mutation by the tombstone path. */
static void segcat_locate_entry_sampled(Relation index, BlockNumber header_blkno,
                                        BlockNumber *out_blk, int *out_idx,
                                        BlockNumber nblocks);

void
bm25_segcat_locate_entry(Relation index, BlockNumber header_blkno,
                         BlockNumber *out_blk, int *out_idx)
{
    segcat_locate_entry_sampled(index, header_blkno, out_blk, out_idx,
                                RelationGetNumberOfBlocks(index));
}

static void
segcat_locate_entry_sampled(Relation index, BlockNumber header_blkno,
                            BlockNumber *out_blk, int *out_idx, BlockNumber nblocks)
{
    BM25MetaPageData    meta;
    BlockNumber         blk;
    uint32              visited = 0;

    bm25_meta_read(index, &meta);
    blk = meta.segcat_root;
    /* SEGREAD-07 (#139), and the worst-placed instance of it: this walker is on the
     * VACUUM tombstone path (bm25_livedocs_clear below), so a cyclic segcat chain
     * wedged AUTOVACUUM with no way to cancel it. Same house pattern as
     * bm25_segcat_find_entry above -- check first, extent bound in the loop
     * condition. Falling out of the loop lands on the ERROR below, which is the
     * accurate outcome for a chain that does not contain the header.
     *
     * Re-sampling form for the reason find_entry gives (issue #225). On the VACUUM
     * path the race it covers -- a seal extending the relation for a fresh catalog
     * root after the sample -- can no longer happen, because bm25_bulkdelete holds
     * the singleton ShareLock across every call into this function and the seal
     * needs it ExclusiveLock (#239). The re-sample stays because this walker is also
     * driven without that lock (bm25_debug_segcat_walk), and because it is the
     * house pattern for every catalog walker rather than a per-caller judgement.
     *
     * Issue #244: the page cap, kind check and zero-entry check as find_entry has
     * them, and no duplicate check, for find_entry's reason: the first match is where
     * the entry really is. That matters more here, because the block and slot this
     * returns are written EXCLUSIVE by bm25_livedocs_clear. */
    while (blk != InvalidBlockNumber && bm25_blk_in_extent(index, blk, &nblocks))
    {
        Buffer          buf;
        Page            pg;
        BM25PageOpaque *op;
        BM25SegCatEntry *ents;
        int             n,
                        i;

        CHECK_FOR_INTERRUPTS();

        segcat_visit_validate(visited++, meta.nsegs);
        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        op = BM25PageGetOpaque(pg);
        ents = (BM25SegCatEntry *) PageGetContents(pg);
        /* Same SEGCAT chain, same checks as bm25_scan_snapshot / bm25_segcat_read.
         * Without the content bound, a corrupt page here would silently contribute
         * zero entries and this function's "not found" path below would raise a
         * misleading "not found in catalog" error for a segment that actually IS
         * there, instead of the accurate corruption signal. A page holds a few
         * hundred entries, so the count fits an int. */
        n = (int) segcat_page_nentries(pg, blk);
        for (i = 0; i < n; i++)
        {
            if (ents[i].header_blkno == header_blkno)
            {
                *out_blk = blk;
                *out_idx = i;
                UnlockReleaseBuffer(buf);
                return;
            }
        }
        blk = op->nextblk;
        UnlockReleaseBuffer(buf);
    }
    ereport(ERROR,
            (errcode(ERRCODE_INDEX_CORRUPTED),
             errmsg("bm25: segment header %u not found in catalog",
                    header_blkno)));
}

/* Tombstone one document: clear its live bit and decrement live counters in the
 * segment header's catalog entry and the metapage global stats. One Generic WAL
 * record covers the three touched pages (LIVE, segcat, meta); the seg-header page
 * is neither locked nor registered -- see the sumdoclen note below for why it does
 * not have to be. Idempotent at the bit level: if the bit is already clear we still
 * open the WAL window but abort it and make no stat change (so a re-reported dead
 * TID does not double-count).
 *
 * Lock order: meta -> LIVE -> segcat. The index-wide rule is METAPAGE BEFORE
 * SEGMENT-CATALOG PAGE, set by bm25_scan_snapshot, which holds the metapage SHARE
 * across its entire catalog walk (that held lock is precisely what stops a publish
 * moving the chain under the copy, so the reader's direction is the fixed one and
 * writers conform to it). This path used to run LIVE -> segcat -> meta, matching
 * the seal's own then-inverted order; both were corrected together -- see
 * bm25_segment_build_and_commit's publish record and docs/adr/0018.
 *
 * sumdoclen approximation: per-doc length is NOT stored, so we subtract the segment's
 * average live doclen (hdr->total_len / hdr->ndocs) from both the catalog entry's
 * total_len and the metapage total_len. Exact sumdoclen is restored on merge (spec
 * section 7: stats are approximate between merges). The seg-header is immutable after seal,
 * so avg_len is read from the header copy already in hand (hdr_copy) and the header
 * page is NOT locked or registered: the WAL window touches exactly 3 pages -- the
 * LIVE page, the segcat page (live_ndocs/total_len), and the metapage (global stats). */
void
bm25_livedocs_clear(Relation index, BlockNumber header_blkno, uint32 local_docid)
{
    BM25SegmentHeader   hdr_copy;
    BM25SegmentHeader  *h = &hdr_copy;
    BlockNumber         live_blk;
    Size                byte_off;
    int                 bit;
    Buffer              livebuf,
                        catbuf,
                        metabuf;
    GenericXLogState   *state;
    Page                livepg,
                        catpg,
                        metapg;
    BM25MetaPageData   *meta;
    BM25SegCatEntry    *cat;
    BlockNumber         catblk;
    int                 catidx;
    unsigned char      *bits;
    uint64              avg_len;
    uint32              avg_tok;

    /* The Task-0 contract passes header_blkno (BM25SegmentHeader has no self-block
     * field). Read the header struct from that block to find the chain roots. The
     * only production caller, bm25_bulkdelete, holds the seal/merge singleton
     * ShareLock across its whole pass, which excludes every catalog swap -- a merge
     * or bm25_upgrade's bm25_merge_rewrite_all, the only things that retire a
     * segment, both under the singleton ExclusiveLock -- and every reclaim (the only
     * thing that frees its pages for reuse). The segment therefore cannot change
     * identity under this call, so the header read passes expected_gen = 0 (skip the
     * option-(d) check); the read-side scan, which holds no such lock, is where
     * gen-validation matters as a concurrency guard. The LIVE walk and the write below
     * do check the header's gen, as a corruption backstop against a link into another
     * segment (issue #303). The same hold is what makes bm25_segcat_locate_entry's unlocked catalog
     * walk below safe (issue #270), so both rest on the hold this Assert checks. */
    Assert(segcat_singleton_held(index));   /* checked: bm25_bulkdelete's LockPage */
    bm25_seg_header_read(index, header_blkno, 0, &hdr_copy);

    /* Locate the LIVE page (SHARE only to read chain; re-lock EXCL below). */
    live_blk = bm25_livedocs_locate(index, h->livedocs_root, h->gen, local_docid,
                                    &byte_off, &bit);

    /* Find the catalog page + slot for this segment (by header_blkno identity).
     * bm25_segcat_locate_entry is the internal block+slot locator used by the WAL
     * mutation here (distinct from the read-only, test/debug-only
     * bm25_segcat_find_entry, which returns a BM25SegCatEntry copy by gen). */
    bm25_segcat_locate_entry(index, header_blkno, &catblk, &catidx);

    /* Approximate sumdoclen decrement = this segment's average live doclen. The
     * segment header is immutable after seal, so we read avg_len from the copy
     * already in hand (hdr_copy) and do NOT lock/register the header page -- the
     * tombstone touches only 3 pages. */
    avg_len = (h->ndocs > 0) ? (h->total_len / h->ndocs) : 0;
    /* Same treatment for the token count (ADR 0088): the entry's total_tokens must
     * track its total_len through the segment's whole life, or the two stop being
     * comparable and the estimator's Max(stored, total_len) quietly changes meaning.
     * Derived from the SAME immutable header copy, so it is the segment's average
     * tokens-per-document. Reads 0 on a legacy segment (the header's hole was always
     * page-zeroed), which makes the decay below a no-op on an already-zero entry --
     * self-consistent without consulting the trust flag. */
    avg_tok = (h->ndocs > 0) ? (uint32) (h->total_tokens / h->ndocs) : 0;

    /* Lock order: meta, LIVE, segcat -- the index-wide rule is metapage BEFORE
     * segment-catalog page, because bm25_scan_snapshot holds the metapage SHARE
     * across its whole catalog walk and cannot do otherwise (that held lock is
     * what stops a publish moving the chain under the copy). This used to run
     * LIVE -> segcat -> meta, matching the seal's then-inverted order; with
     * buffer content locks being LWLocks -- no deadlock detector, not
     * cancel-interruptible -- a VACUUM here and a concurrent ranked SELECT could
     * wedge both backends until SIGKILL. LIVE stays ahead of segcat, so nothing
     * else about the previous order changed.
     *
     * bm25_segcat_locate_entry above takes and RELEASES its metapage and segcat
     * locks before returning, so it holds nothing across this block and
     * introduces no ordering of its own. */
    metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    livebuf = ReadBuffer(index, live_blk);
    catbuf  = ReadBuffer(index, catblk);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    LockBuffer(livebuf, BUFFER_LOCK_EXCLUSIVE);
    LockBuffer(catbuf,  BUFFER_LOCK_EXCLUSIVE);

    /* H6: until now these three pages were taken EXCLUSIVE and written through
     * Generic WAL with NO kind and NO gen check -- the least-guarded WRITE in the
     * reader. The caller's singleton hold makes a concurrency check unnecessary, so
     * the page kind (all three) and the LIVE page's gen (issue #303) are corruption
     * backstops rather than concurrency ones. Run BEFORE GenericXLogStart so an error
     * here leaves no WAL window open at all.
     *
     * live_blk came off bm25_livedocs_locate's chain walk and catblk off
     * bm25_segcat_locate_entry's catalog walk -- both derived from on-disk
     * pointers, so both are misdirectable; the metapage is BM25_METAPAGE_BLKNO, a
     * compile-time constant that cannot be, and its check is uniformity only.
     * SEGCAT is shared with segment header pages (see bm25_seg_header_read); the
     * catalog role (seg_gen == 0, issue #276) tells this page from a header. */
    bm25_seg_page_validate_kind(BufferGetPage(livebuf), 0, BM25_PAGE_LIVE);
    /* The LIVE page's gen too (issue #303, design check R3). bm25_livedocs_locate reads
     * the pages it walks past, not the one it returns, so a link from this segment's
     * last walked-past page into ANOTHER segment's LIVE chain -- the same kind --
     * reached this write unread, and the tombstone bit landed on the other segment's
     * document. The singleton is held (asserted above), so the segment is live and a
     * mismatch is corruption, raised here with no WAL window open. */
    if (BM25PageGetOpaque(BufferGetPage(livebuf))->seg_gen != h->gen)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: live-docs chain reaches block %u, a page of segment gen "
                        "%u, not %u", live_blk,
                        BM25PageGetOpaque(BufferGetPage(livebuf))->seg_gen, h->gen),
                 errdetail("Segment gen %u is still live, so none of its pages can have "
                           "been reclaimed.", h->gen),
                 errhint("REINDEX the index.")));
    (void) bm25_segcat_page_validate(BufferGetPage(catbuf), BM25_SEGCAT_ROLE_CATALOG,
                                     catblk);
    bm25_seg_page_validate_kind(BufferGetPage(metabuf), 0, BM25_PAGE_META);

    /* The byte this tombstone flips must be bitmap the page actually holds (issue
     * #294). bm25_livedocs_locate addresses it by divide, which bounds it by the full
     * span and not by this page's pd_lower; on a page whose content stops short, the
     * byte would sit in the zeroed hole, read as already clear, and the tombstone would
     * be skipped as a no-op while every reader ERRORs on the same docid. Before
     * GenericXLogStart, like the kind checks. */
    if (byte_off >= bm25_page_content_bytes(BufferGetPage(livebuf)))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: live-docs page at block %u holds %zu content bytes, too "
                        "few for local docid %u",
                        live_blk, bm25_page_content_bytes(BufferGetPage(livebuf)),
                        local_docid),
                 errhint("REINDEX the index.")));

    /* Registration order is the STANDBY's lock order: generic_redo locks every
     * block EXCLUSIVE in block_id order and holds them all until the record is
     * applied, so the metapage must be registered first to honour ADR 0018 on
     * replay too (issue #240). On the primary registration takes no locks; the
     * LockBuffer order above is what governs there. */
    state  = GenericXLogStart(index);
    metapg = GenericXLogRegisterBuffer(state, metabuf, 0);
    livepg = GenericXLogRegisterBuffer(state, livebuf, 0);
    catpg  = GenericXLogRegisterBuffer(state, catbuf, 0);

    bits = (unsigned char *) PageGetContents(livepg) + byte_off;
    if (!(*bits & (1u << bit)))
    {
        /* Already tombstoned -- no stat change; discard the (no-op) WAL window. */
        GenericXLogAbort(state);
        UnlockReleaseBuffer(metabuf);
        UnlockReleaseBuffer(catbuf);
        UnlockReleaseBuffer(livebuf);
        return;
    }
    *bits &= ~(1u << bit);

    cat  = &((BM25SegCatEntry *) PageGetContents(catpg))[catidx];
    meta = BM25PageGetMeta(metapg);

    if (cat->live_ndocs > 0)
        cat->live_ndocs--;
    if (cat->total_len >= avg_len)
        cat->total_len -= avg_len;
    else
        cat->total_len = 0;
    /* Clamped like total_len above. Integer division makes avg_tok a floor, so a
     * fully-tombstoned segment can leave a small positive remainder -- harmless,
     * and the reason nothing may assert total_tokens >= total_len for a DECAYED
     * entry: that inequality is a write-time invariant, not a lifetime one. */
    if (cat->total_tokens >= avg_tok)
        cat->total_tokens -= avg_tok;
    else
        cat->total_tokens = 0;

    if (meta->ndocs > 0)
        meta->ndocs--;
    if (meta->total_len >= avg_len)
        meta->total_len -= avg_len;
    else
        meta->total_len = 0;

    /* pd_lower discipline on the metapage so page-hole compression keeps the struct. */
    bm25_meta_set_pd_lower(metapg);

    GenericXLogFinish(state);
    UnlockReleaseBuffer(metabuf);
    UnlockReleaseBuffer(catbuf);
    UnlockReleaseBuffer(livebuf);
}

/* bm25_debug_segcat_walk(index regclass, walker text, nblocks bigint) RETURNS bigint
 *   -- issue #225 probe for the SEGCAT walkers' pre-lock extent sample.
 *
 * The four SEGCAT walkers, and bm25_segcat_first_entry's root read, sample the extent
 * BEFORE reading segcat_root, so a seal that extends the relation for a fresh catalog
 * page between the sample and the metapage read hands them a healthy root at or past
 * their own sample (bm25_blk_in_extent's header). That window is a few instructions
 * wide and no regression suite can land a concurrent seal inside it. What CAN be
 * driven is its effect, because the only thing the race changes is the value of the
 * sample: this probe runs the real walker with a caller-chosen sample instead of a
 * fresh one. nblocks = 1 (the metapage alone) puts
 * every catalog page past the sample, so the very first link -- the root -- is a
 * would-be violation, exactly as in the race; the re-sample then refreshes the walk's
 * extent, and the links after it are tested against that. A healthy index must still
 * read back in full. Take the re-sample out of bm25_blk_in_extent and every walker
 * here fails on a healthy catalog: the two copies raise "segment catalog chain leaves
 * the index", find_entry answers 0 and locate_entry raises "not found in catalog".
 * That is the mutation this probe exists to catch.
 *
 * nblocks = -1 runs the public entry point, which samples for itself: the control.
 *
 * Returns what the walker produced, so a walk that quietly stopped short cannot pass:
 *   scan_snapshot, segcat_read   the number of entries copied (== nsegs, or it raised)
 *   find_entry                   1 if the catalog's LAST entry's gen was found, else 0
 *   locate_entry                 1 once the LAST entry's header was located (raises if not)
 *   first_entry                  0 for an empty catalog; else 1 if the entry equals
 *                                entry 0 of a full bm25_segcat_read, -1 if it does not
 *   find_absent                  1 if gen 0 (never issued) was found, else 0
 *   locate_absent                1 if header block 0 (the metapage) was located; raises
 *                                "not found in catalog" on a healthy chain
 * "Last" is the final entry in chain order, so both lookups must cross every catalog
 * page to succeed.
 *
 * The two _absent walkers are issue #244's: over a corrupt chain, find_entry and
 * locate_entry would need their target from bm25_segcat_read, which now rejects that
 * chain before the lookup starts. A target that is never present needs no read, and
 * makes the lookup walk until the chain ends -- or, on a cycle, until a bound stops it.
 * TEST-ONLY, read-only; REVOKEd from PUBLIC with every bm25_debug_*. */
PG_FUNCTION_INFO_V1(bm25_debug_segcat_walk);
Datum
bm25_debug_segcat_walk(PG_FUNCTION_ARGS)
{
    Oid                 relid   = PG_GETARG_OID(0);
    char               *walker  = text_to_cstring(PG_GETARG_TEXT_PP(1));
    int64               nblocks = PG_GETARG_INT64(2);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    int64               result;

    /* Arguments before the index is opened (97_debug_probe_arguments). 0 is allowed:
     * it is as stale as a sample can be, and the re-sample must cope with it. */
    if (nblocks < -1 || nblocks > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_segcat_walk: nblocks " INT64_FORMAT
                        " is not a block count", nblocks)));
    if (strcmp(walker, "scan_snapshot") != 0 && strcmp(walker, "segcat_read") != 0 &&
        strcmp(walker, "find_entry") != 0 && strcmp(walker, "locate_entry") != 0 &&
        strcmp(walker, "find_absent") != 0 && strcmp(walker, "locate_absent") != 0 &&
        strcmp(walker, "first_entry") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_segcat_walk: unknown walker \"%s\"", walker),
                 errhint("Use scan_snapshot, segcat_read, first_entry, find_entry, "
                         "locate_entry, find_absent or locate_absent.")));

    index = bm25_index_open_readable(relid, AccessShareLock);

    if (strcmp(walker, "scan_snapshot") == 0)
    {
        BM25ScanSnapshot snap;

        if (nblocks < 0)
            bm25_scan_snapshot(index, &snap);
        else
            scan_snapshot_sampled(index, &snap, (BlockNumber) nblocks);
        result = snap.nsegs;
    }
    else if (strcmp(walker, "segcat_read") == 0)
    {
        if (nblocks < 0)
            bm25_segcat_read(index, &segs, &nsegs);
        else
            segcat_read_sampled(index, &segs, &nsegs, (BlockNumber) nblocks);
        result = nsegs;
    }
    else if (strcmp(walker, "first_entry") == 0)
    {
        BM25SegCatEntry first;
        bool            found;

        if (nblocks < 0)
            found = bm25_segcat_first_entry(index, &first);
        else
            found = first_entry_sampled(index, &first, (BlockNumber) nblocks);
        result = 0;
        if (found)
        {
            /* The oracle is a full read's entry 0. Both copies come from the same page
             * bytes, so comparing the whole struct, padding included, is exact. */
            bm25_segcat_read(index, &segs, &nsegs);
            result = (nsegs > 0 && memcmp(&first, &segs[0], sizeof(first)) == 0) ? 1 : -1;
        }
    }
    else if (strcmp(walker, "find_absent") == 0)
    {
        BM25SegCatEntry got;
        bool            found;

        /* gen 0 is never issued (bm25_meta_init starts next_gen at 1). */
        if (nblocks < 0)
            found = bm25_segcat_find_entry(index, 0, &got);
        else
            found = segcat_find_entry_sampled(index, 0, &got, (BlockNumber) nblocks);
        result = found ? 1 : 0;
    }
    else if (strcmp(walker, "locate_absent") == 0)
    {
        BlockNumber catblk;
        int         catidx;

        /* The metapage is never a segment header, so a whole-chain walk ends in the
         * walker's own "not found in catalog" error; returning 1 means it matched. */
        if (nblocks < 0)
            bm25_segcat_locate_entry(index, BM25_METAPAGE_BLKNO, &catblk, &catidx);
        else
            segcat_locate_entry_sampled(index, BM25_METAPAGE_BLKNO, &catblk, &catidx,
                                        (BlockNumber) nblocks);
        result = 1;
    }
    else
    {
        BM25SegCatEntry last;

        /* The target comes from an ordinary full read, so the lookups below are the
         * only walks that see the substituted sample. */
        bm25_segcat_read(index, &segs, &nsegs);
        if (nsegs == 0)
        {
            index_close(index, AccessShareLock);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25_debug_segcat_walk: index has no segments")));
        }
        last = segs[nsegs - 1];
        if (strcmp(walker, "find_entry") == 0)
        {
            BM25SegCatEntry got;
            bool            found;

            if (nblocks < 0)
                found = bm25_segcat_find_entry(index, last.gen, &got);
            else
                found = segcat_find_entry_sampled(index, last.gen, &got,
                                                  (BlockNumber) nblocks);
            result = found ? 1 : 0;
        }
        else
        {
            BlockNumber catblk;
            int         catidx;

            if (nblocks < 0)
                bm25_segcat_locate_entry(index, last.header_blkno, &catblk, &catidx);
            else
                segcat_locate_entry_sampled(index, last.header_blkno, &catblk, &catidx,
                                            (BlockNumber) nblocks);
            result = 1;
        }
    }

    index_close(index, AccessShareLock);
    PG_RETURN_INT64(result);
}

/* bm25_debug_seg_lenfields(index regclass, seg int) -- read the named segment's
 * header length-prefixed total_len_by_field[] and return (field_id, total_len) per
 * field. The per-field sumdoclen array lives on the header page AFTER the struct, so
 * it must be read under the SAME SHARE lock that reads the header (bm25_seg_header_read
 * releases the buffer before the array can be recovered). Development/regression
 * introspection only; not on the query path. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_lenfields);
Datum
bm25_debug_seg_lenfields(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    int32               segidx = PG_GETARG_INT32(1);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    TupleDesc           tupdesc;
    Tuplestorestate    *ts;
    BM25SegmentHeader   h;
    uint64              tlbf[BM25_MAX_FIELDS];
    Buffer              buf;
    Page                pg;
    uint32              f;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    index = bm25_index_open_readable(relid, AccessShareLock);
    tupdesc = BlessTupleDesc(tupdesc);
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    bm25_segcat_read(index, &segs, &nsegs);
    if (segidx < 0 || (uint32) segidx >= nsegs)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_seg_lenfields: segment %d out of range (%u live)",
                        segidx, nsegs)));

    /* Read header + the trailing length-fields array under ONE SHARE lock. The
     * catalog entry is unvalidated (bm25_segcat_read checks no field), so gate the
     * pointer here exactly as the non-debug readers do. */
    bm25_seg_blkno_validate(segs[segidx].header_blkno, "segment header block");
    buf = ReadBuffer(index, segs[segidx].header_blkno);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    /* Header page: SEGCAT kind, same shared-bit note as bm25_seg_header_read. */
    bm25_seg_page_validate_kind(pg, segs[segidx].gen, BM25_PAGE_SEGCAT);
    bm25_segheader_span_validate(pg, segs[segidx].header_blkno, sizeof(BM25SegmentHeader));
    memcpy(&h, PageGetContents(pg), sizeof(BM25SegmentHeader));
    bm25_segheader_read_lenfields(pg, &h, segs[segidx].header_blkno, tlbf, NULL);
    UnlockReleaseBuffer(buf);

    for (f = 0; f < h.field_count; f++)
    {
        Datum   vals[2];
        bool    nulls[2] = {0};

        vals[0] = Int32GetDatum((int32) f);
        vals[1] = Int64GetDatum((int64) tlbf[f]);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}
