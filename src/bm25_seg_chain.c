/* bm25_seg_chain.c -- the sealed-segment chain readers: the POST/POS decode that
 * turns a DICT entry into postings, and the dense per-docid chains (LIVEDOCS,
 * DOCMAP, NORMS) read through resumable cursors.
 *
 * Split out of bm25_seg_read.c (#228, ADR 0101) as a pure code move; nothing here
 * changed behaviour. What lives here:
 *   - bm25_seg_scan_postings, with the POS byte cursor (PosCursor, pos_cursor_*)
 *     it runs in lockstep for positional callers, and bm25_seg_block_header_read,
 *     the per-block header read the WAND cursor skips with;
 *   - chain_read_at, the bounded read every dense per-docid lookup funnels
 *     through, and its span check bm25_chain_span_validate;
 *   - the chain page images (issue #267): bm25_seg_walk_get and
 *     bm25_chain_page_keep/_done, the page access chain_read_at and the KEYMAP walk share, and the reader opt-in
 *     bm25_seg_reader_cache_pages / bm25_seg_reader_release_pages;
 *   - the one-shot lookups (bm25_seg_doc_is_live, bm25_seg_docid_to_tid,
 *     bm25_seg_doclen_field, bm25_seg_doclen) and BM25SegReader, which carries one
 *     cursor per chain across a caller's loop (bm25_seg_reader_init and the
 *     bm25_seg_reader_* accessors);
 *   - two probes that call file-statics here, bm25_debug_chain_span_validate and
 *     bm25_debug_chain_cursor_crosstalk. They stay beside what they probe so those
 *     functions keep internal linkage and each probe runs the code the reader runs.
 *
 * Not here: bm25_seg_reader_init_checked and bm25_seg_reader_init_known. The first
 * walks the LIVEDOCS bitmap once to decide whether a reader may skip per-lookup
 * liveness reads, using the LIVEDOCS page geometry bm25_livedocs_locate also uses,
 * so both stay in bm25_seg_read.c with the rest of the LIVEDOCS code and call
 * bm25_seg_reader_init here.
 *
 * Every page read here, or served from a page image, is seg_gen- and kind-validated
 * (option (d)) with the validators in bm25_seg_read.c.
 *
 * Also here, first, because everything below reads through it: BM25SegWalk, the
 * validated page reader every sealed-chain walk in the tree uses (issue #303).
 */
#include "postgres.h"

#include "storage/off.h"          /* MaxOffsetNumber: the docid -> TID check */
#include "bm25.h"
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "storage/bufmgr.h"

/* ---- BM25SegWalk: the validated walk of a sealed segment chain (issue #303) ----
 *
 * The segment twin of BM25PendingWalk (bm25_pending.c, #291). Before it, each of the
 * sealed-chain walkers (three DICT readers, three DICT dumps, the POST and POS decodes,
 * chain_read_at, the KEYMAP walk, the LIVE walks) carried its own subset of the
 * per-page checks, and they had drifted: a link to block 0 read the metapage and failed
 * the gen check as a retryable 40001, an in-extent cycle spun until cancelled on every
 * walker but the dense chains, and the debug dumps sampled their extent before the
 * catalog read (SEGREAD-07). ADR 0111 states the validated-walker contract this
 * implements; ADR 0095 the extent half.
 *
 * Per page, in this order:
 *   1. Block 0 (in bm25_seg_chain_extent_validate). The metapage is never a page of a
 *      segment chain, so a link to it is corruption, raised before ReadBuffer.
 *   2. Extent: blk >= nblocks is ERRCODE_INDEX_CORRUPTED (ADR 0095). A quiet walk (the
 *      debug dumps) re-samples first through bm25_blk_in_extent and, if the link is
 *      still past the end, returns InvalidBuffer so the dump stops with what it could
 *      reach. A walk opened with nblocks == 0 takes its sample at the first read.
 *      ADR 0095's precondition holds at every caller: the sample follows the chain's
 *      publication, because it is taken at walk or cursor open, after the caller's
 *      catalog snapshot or singleton, or lazily here.
 *   3. Revisit: a link to the page just read (a self-loop), to the walk's first page,
 *      or to the chain's root is a cycle, because every walk runs forward over a simple
 *      list -- including the ones that start mid-list (a POST run's post_root, a cursor
 *      resume) -- and nothing in a chain links to its root. A walk that may start past
 *      the root says which block it is (BM25SegWalk.root): a cursor resume's first page
 *      is the cursor's, so without it a link back to the root read the root as the
 *      next page. Three compares per page transition and no visited set, so it costs
 *      the per-posting chain_read_at nothing measurable. A longer cycle that passes
 *      through none of the three is not seen here; the family invariants below and
 *      the cap are what end it.
 *   4. Visit cap: visited >= nblocks is a cycle by counting, the
 *      bm25_pending_cycle_cap_validate shape. A chain is a set of distinct blocks in
 *      [1, nblocks), and nothing truncates an index, so a well-formed walk reads fewer
 *      than nblocks pages. Every chain family has a tighter bound of its own (below);
 *      the cap is what keeps "every segment walk is bounded" true independently of them.
 *   5. Content bytes (bm25_page_content_bytes) BEFORE any opaque read, as
 *      bm25_pending_walk_read does: on a never-initialized page (pd_special == 0) the
 *      opaque read trips PageGetSpecialPointer's Assert on a cassert build and reads the
 *      page header as the opaque on a production one. A page reused by a concurrent
 *      reclaim is always initialized, so this order takes no legitimate 40001 away.
 *   6. Gen, then kind (bm25_seg_page_validate_kind's header says why that order). A
 *      gen mismatch goes to bm25_seg_gen_mismatch, which tells a corrupt link into
 *      another segment's page (XX002: the walk's own segment is still live) from the
 *      reclaim race (40001: it has left the catalog).
 * No CHECK_FOR_INTERRUPTS here: every caller runs one at the top of its page loop
 * holding no buffer, as with BM25PendingWalk, so the interrupt-check floors are
 * unchanged.
 *
 * What ends a cycle in each chain family (checked by the callers, on values their
 * decode already has):
 *   - DICT: strict cross-page term order and no empty page (bm25_dict_order_*,
 *     bm25_seg_dict.c), and nentries == nterms at the end of a whole-chain walk.
 *   - POST: every page entered mid-run yields at least one block, and (docid, field)
 *     ascends across blocks (bm25_seg_scan_postings; the WAND sweep, peek and decode).
 *   - LIVE, DOCMAP, NORMS: the full span on every page walked past and the offset or
 *     byte budget (ADR 0111); KEYMAP: its own full span (bm25_keymap_full_span).
 *   - POS: at least one content byte per page, and the frame stream's tf lockstep.
 *
 * Image hits (bm25_seg_walk_get) count as visits: they update start/prev and run the
 * revisit test and the cap, so a self-loop on the page a cursor holds as an image is
 * caught at once rather than walked by offset budget.
 *
 * Residuals (D1's second sentence: a wrong answer from in-range values is caught only
 * where a cheap structural invariant exists):
 *   - 303.A(f), narrowed. In LIVE, DOCMAP, NORMS, KEYMAP and POS, a link to another
 *     page of the same chain that is not the walk's first page, the previous one or the
 *     root -- a backward link into the middle of the chain, or a forward skip, which is
 *     not a cycle at all -- can answer from the wrong page within the walk's offset
 *     budget. Allocation is not monotone, and a per-lookup visited set would cost the
 *     per-posting path. On bm25_livedocs_locate's path the wrong page is WRITTEN: a
 *     tombstone bit on another document of the same segment.
 *   - DICT within-page order is not checked by bm25_seg_dict_lookup or the wildcard
 *     expander: it would double the per-entry compares of an O(dict) linear scan. The
 *     merge iterator and the dumps check it, so a merge never launders an unsorted page.
 *   - POST in-block (docid, field) disorder: a cycle of two or more pages whose blocks
 *     are each internally out of order can pass every cross-block test. Closing it
 *     needs a per-posting compare in the decode loop.
 *   - A KEYMAP root past the extent reaches ReadBuffer's own short-read error
 *     (ERRCODE_DATA_CORRUPTED) in bm25_seg_keymeta, which takes no extent sample so the
 *     per-row INSERT key check stays free of an lseek (ADR 0071's reasoning). */

/* Pre-read checks 1-4 and the bookkeeping. Returns false only for a quiet walk whose
 * link is past the re-sampled extent. Inlined into both readers below; the exported
 * bm25_seg_walk_check is the same body for a caller that does not read the page. */
static inline bool
seg_walk_step(BM25SegWalk *w, BlockNumber blk)
{
    if (unlikely(blk >= w->nblocks || blk == BM25_METAPAGE_BLKNO))
    {
        /* Block 0 never re-samples: it is corruption whatever the extent is. */
        if (blk != BM25_METAPAGE_BLKNO && (w->nblocks == 0 || w->quiet) &&
            bm25_blk_in_extent(w->index, blk, &w->nblocks))
            ;                   /* in the extent after all */
        else if (w->quiet && blk != BM25_METAPAGE_BLKNO)
            return false;       /* a dump reports what it could reach */
        else
            bm25_seg_chain_extent_validate(blk, w->nblocks, w->what);
    }
    if (unlikely(w->visited > 0 &&
                 (blk == w->start || blk == w->prev || blk == w->root)))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s revisits block %u", w->what, blk),
                 errdetail("The chain links back to a page this walk already read, "
                           "after %u pages.", w->visited),
                 errhint("REINDEX the index.")));
    if (unlikely(w->visited >= w->nblocks))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: %s visits more pages than the index holds", w->what),
                 errdetail("%u pages were read; the relation has %u blocks.", w->visited,
                           w->nblocks),
                 errhint("REINDEX the index.")));
    if (w->visited == 0)
        w->start = blk;
    w->prev = blk;
    w->visited++;
    return true;
}

bool
bm25_seg_walk_check(BM25SegWalk *w, BlockNumber blk)
{
    return seg_walk_step(w, blk);
}

/* Checks 5 and 6 on a page the caller holds locked in buf (or on an image, buf
 * Invalid). Error exits other than the gen arm leave the buffer locked; transaction
 * abort releases it, as for every validator here. A gen mismatch releases it first:
 * bm25_seg_gen_mismatch may read the catalog under the metapage SHARE, which must not
 * nest under a segment page's lock (ADR 0018's metapage-first order). An image never
 * mismatches -- bm25_seg_walk_get serves one only at the walk's gen. */
static inline Size
seg_walk_page_validate(BM25SegWalk *w, Buffer buf, Page pg, BlockNumber blk)
{
    Size    pagebytes = bm25_page_content_bytes(pg);
    uint32  page_gen = BM25PageGetOpaque(pg)->seg_gen;

    if (unlikely(w->gen != 0 && page_gen != w->gen))
    {
        if (BufferIsValid(buf))
            UnlockReleaseBuffer(buf);
        bm25_seg_gen_mismatch(w->index, blk, page_gen, w->gen, w->what);
    }
    bm25_seg_page_validate_kind(pg, 0, w->kind);
    return pagebytes;
}

Buffer
bm25_seg_walk_read(BM25SegWalk *w, BlockNumber blk, Size *pagebytes)
{
    Buffer  buf;

    if (!seg_walk_step(w, blk))
        return InvalidBuffer;
    buf = ReadBuffer(w->index, blk);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    *pagebytes = seg_walk_page_validate(w, buf, BufferGetPage(buf), blk);
    return buf;
}

/* ---- M4 POS byte cursor (lockstep with the POST scan) ----
 *
 * A forward byte reader over the SEPARATE BM25_PAGE_POS chain. Unlike the POST
 * decode (which reads whole blocks that never span a page), a position frame -- even
 * a single deltapos varbyte -- can straddle a POS page boundary (the writer streams bytes
 * with chain_write_stream). So this cursor hands out bytes one at a time, following
 * nextblk mid-frame / mid-varbyte, and reads a varbyte across the boundary too
 * (pos_cursor_varbyte). The POST reader below opens it only when pos_cb != NULL.
 *
 * COPY-THEN-UNLOCK, AND WHY IT IS THE WHOLE POINT (SEGREAD-02, issue #139). This
 * cursor used to hold ONE POS page SHARE-locked CONTINUOUSLY from pos_cursor_open
 * to pos_cursor_close -- i.e. across the entire POST decode loop below, since the
 * two run in lockstep. A buffer content lock is an LWLock and LWLockAcquire does
 * HOLD_INTERRUPTS(), so InterruptHoldoffCount was non-zero for that whole span and
 * bm25_seg_scan_postings' per-POST-page CHECK_FOR_INTERRUPTS -- the one its own
 * comment calls "what makes a long scan cancellable at all" -- was a silent no-op
 * on EVERY phrase/proximity query. A phrase on a term with df in the millions
 * ignored pg_cancel_backend, SIGINT and statement_timeout until the whole term's
 * POST+POS replay finished.
 *
 * THE INVARIANT NOW: the cursor holds no buffer -- not pinned, not locked -- between
 * calls. pos_cursor_load takes the page's SHARE lock, validates it, memcpy's the
 * whole BLCKSZ into pc->page, and unlocks before returning; every byte handed out
 * afterwards comes from that private copy. Holdoff is therefore 0 at the POST
 * loop's interrupt check and inside chain_read_at (reached from the callbacks),
 * which is what makes both live.
 *
 * PER PAGE, NOT PER FRAME. Copying just the current frame would be the smaller
 * copy, but there is no sound bound on a frame: it is varbyte(tf) followed by tf
 * deltas, and while the INSERT path caps tf at 65535 ambuild does not (#158), so a
 * frame can reach hundreds of megabytes. BLCKSZ is the only bound that holds for
 * any input. The copy is charged to CurrentMemoryContext, matching the posbuf
 * allocation in the same scan, and freed in pos_cursor_close.
 *
 * Re-validation is not a concern: the cursor is strictly forward and single-pass,
 * so every page is validated exactly once, under its own lock, in pos_cursor_load.
 * Should a rewind ever be added, pos_cursor_load is the single chokepoint it must
 * go through. */
typedef struct PosCursor
{
    BM25SegWalk  walk;              /* extent, revisit, cap, content, gen, kind */
    BlockNumber  blk;               /* block pc->page was copied from */
    BlockNumber  next;              /* nextblk, read off the private copy */
    char        *page;              /* private palloc(BLCKSZ) copy; no buffer held */
    const uint8 *cur;               /* read cursor into pc->page's contents */
    const uint8 *end;               /* content boundary within pc->page */
} PosCursor;

/* The ONE place a POS page is fetched. ORDER IS LOAD-BEARING, top to bottom:
 *
 * 1. bm25_seg_walk_read (issue #303): block 0, the extent bound, the revisit test
 *    and the cycle cap BEFORE the page is touched at all (XCUT-02), then, under the
 *    lock, bm25_page_content_bytes and the gen/kind check. blk comes off an on-disk
 *    nextblk, so a corrupt chain could aim past the relation (ReadBuffer would EXTEND
 *    it), at the metapage, or round a loop. The content bound matters here in
 *    particular: a bare pd_lower read has the same underflow shape as chain_read_at's,
 *    in the OTHER direction -- pd_lower < 24 makes end < cur immediately, so
 *    pos_cursor_byte treats the page as exhausted and skips to nextblk right away,
 *    decoding a term's positions from the WRONG page instead of erroring. Silently
 *    wrong phrase matches, not a crash, which is why it went unnoticed.
 * 2. memcpy the whole page, then UnlockReleaseBuffer -- holdoff back to 0.
 * 3. ONLY THEN derive next/cur/end, all from the private copy.
 *
 * The zero-content rejection (XCUT-02) is deliberately placed after the unlock, so
 * the error propagates with nothing held. It is safe by construction:
 * chain_write_stream gives every reachable POS page at least one content byte, so
 * it cannot fire on a well-formed index -- and it kills the two-page A->B->A spin
 * outright (a zero-content page leaves cur == end, so pos_cursor_byte would recurse
 * into the next page immediately, forever) instead of relying on the cap above to
 * notice. */
static void
pos_cursor_load(PosCursor *pc, BlockNumber blk)
{
    Buffer  buf;
    Page    pg;
    Size    pagebytes;

    buf = bm25_seg_walk_read(&pc->walk, blk, &pagebytes);
    pg = BufferGetPage(buf);
    memcpy(pc->page, pg, BLCKSZ);
    UnlockReleaseBuffer(buf);

    if (pagebytes == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: position chain reaches a page with no content"),
                 errdetail("The empty page is block %u.", blk)));

    pc->blk  = blk;
    pc->next = BM25PageGetOpaque((Page) pc->page)->nextblk;
    pc->cur  = (const uint8 *) PageGetContents((Page) pc->page);
    pc->end  = pc->cur + pagebytes;
}

static void
pos_cursor_open(PosCursor *pc, Relation index, uint32 expected_gen,
                BlockNumber root, uint16 off)
{
    /* One extent sample per cursor, at open (the shape ADR 0071 names), after the
     * caller's snapshot published the segment. */
    bm25_seg_walk_init(&pc->walk, index, RelationGetNumberOfBlocks(index), expected_gen,
                       BM25_PAGE_POS, false, "position chain");
    pc->blk = InvalidBlockNumber;
    pc->next = InvalidBlockNumber;
    pc->page = NULL;
    pc->cur = NULL;
    pc->end = NULL;
    if (root != InvalidBlockNumber)
    {
        pc->page = (char *) palloc(BLCKSZ);
        pos_cursor_load(pc, root);
        /* The root page alone starts at the term's byte offset; every continuation
         * page starts at content top, which is what pos_cursor_load leaves.
         *
         * Bound off first. pos_cursor_load has just set cur to content top and end to
         * content top + pagebytes, so end - cur IS this page's content length; an
         * unbounded off would both form an out-of-range pointer into the palloc'd
         * page copy and leave cur > end, which pos_cursor_byte reads as "page
         * exhausted" and follows to the next page -- decoding the term's positions
         * from the wrong page rather than erroring. */
        bm25_seg_page_off_validate(off, (Size) (pc->end - pc->cur), "position");
        pc->cur = (const uint8 *) PageGetContents((Page) pc->page) + off;
    }
}

static void
pos_cursor_close(PosCursor *pc)
{
    if (pc->page != NULL)
    {
        pfree(pc->page);
        pc->page = NULL;
    }
    pc->cur = NULL;
    pc->end = NULL;
}

/* Advance to the next POS page. Errors if the chain runs dry before the frame
 * stream is done (corruption / desync).
 *
 * The interrupt check here is GENUINELY LIVE -- the cursor holds no buffer at this
 * point, so InterruptHoldoffCount is 0 -- and it is the natural per-POS-page
 * cancellation granularity, complementing the per-POST-page check in
 * bm25_seg_scan_postings. A term whose positions dwarf its postings (high tf) is
 * cancellable through this one; a term whose postings dwarf its positions through
 * that one. */
static void
pos_cursor_next_page(PosCursor *pc)
{
    Assert(pc->page != NULL);   /* invariant */

    /* One unit of cancellable work: a POS page crossing (#156). Counted beside the
     * check, not by it -- see BM25_WORK_UNIT()'s header in bm25.h. This is the
     * granularity sql/66_scan_interrupts' phrase assertion measures: pre-#139 the
     * cursor held a POS page SHARE-locked across the whole POST decode, so the
     * check below was dead and the count would run to the term's full frame stream. */
    BM25_WORK_UNIT();
    CHECK_FOR_INTERRUPTS();

    if (pc->next == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: position chain exhausted mid-frame")));
    pos_cursor_load(pc, pc->next);
}

/* Read ONE byte, crossing to the next page if the current one is spent. */
static inline uint8
pos_cursor_byte(PosCursor *pc)
{
    while (pc->cur >= pc->end)
        pos_cursor_next_page(pc);
    return *pc->cur++;
}

/* Decode one LEB128 varbyte from the cursor, honoring page boundaries mid-value.
 * Unlike bm25_varbyte_decode this cursor is already memory-safe -- pos_cursor_byte
 * errors out itself once the POS chain is exhausted, so there is no run `end` to
 * check here. That relies on pc->end itself being trustworthy: it is derived via
 * bm25_page_content_bytes (pos_cursor_load above -- the ONE place a POS page is
 * fetched), not a
 * bare pd_lower read, so a corrupt pd_lower cannot put end before cur and make
 * this cursor silently skip to the wrong page instead of erroring. What is NOT
 * safe without a bound is `shift`: a corrupt stream whose continuation bit
 * (0x80) never clears would push shift past 31, which is undefined behaviour for
 * a uint32 shift, on the fifth-and-later byte. Bound the byte count the same way
 * bm25_varbyte_decode bounds its own run (reusing BM25_VARBYTE_MAX_BYTES) so a
 * corrupt position stream fails loud instead of invoking UB. */
static uint32
pos_cursor_varbyte(PosCursor *pc)
{
    uint32 result = 0;
    int    shift = 0;
    int    n = 0;
    uint8  b;

    do
    {
        if (n == BM25_VARBYTE_MAX_BYTES)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: varbyte sequence wider than %d bytes",
                            BM25_VARBYTE_MAX_BYTES)));
        b = pos_cursor_byte(pc);
        result |= ((uint32) (b & 0x7F)) << shift;
        shift += 7;
        n++;
    } while (b & 0x80);
    return result;
}

/* Decode exactly `df` postings of one term, invoking
 * cb(local_docid, tf, field_id, state) per posting. Walk the POST chain from
 * (post_root, post_off) block by block, decoding the delta-docid and tf varbyte
 * streams in lockstep.
 *
 * The `df` bound is the ONLY thing that ends a term's run. A segment lays ALL
 * terms' posting blocks into one shared chain (D-POST: posts_root) with NO
 * inter-term delimiter, so the blocks of the next term follow this term's last
 * block with nothing marking the boundary. Decoding `df` postings and stopping is
 * therefore exactly the boundary of this term's run; reading further would bleed
 * into the next term's (and beyond). There is no end-of-blocks sentinel in the
 * builder's layout, so we must not rely on one.
 *
 * Each block delta-encodes its docids from 0 INDEPENDENTLY (the builder resets its
 * running prev per block so blocks stay independently decodable for M2b WAND
 * skipping). The decoder must therefore reset `prev` at every block boundary;
 * carrying it across blocks would inflate docids in block 1+.
 *
 * df is also the run's EXACT length, and the walk holds the chain to it (issue #293,
 * the "validated walker" contract): DICT's df counts postings and the builder writes
 * exactly df of them in blocks of at most BM25_POSTINGS_PER_BLOCK, so a chain that ends, or
 * a block that claims more postings than the run has left, is corruption, never a
 * shorter answer. Stopping quietly there under-answered every non-WAND reader, and
 * the merge replay then wrote the short run into a healthy-looking new segment. */
void
bm25_seg_scan_postings(Relation index, BlockNumber post_root, uint16 post_off,
                       uint32 df, uint32 expected_gen, bm25_post_cb cb, void *state,
                       BlockNumber pos_post_root, uint16 pos_post_off,
                       bm25_pos_cb pos_cb, void *pos_state,
                       const bool *field_store_positions, uint32 field_count)
{
    BlockNumber  blk = post_root;
    uint16       off = post_off;
    uint32       emitted = 0;
    uint32       prev_last = 0;     /* last docid of the previous block of this run */
    uint32       prev_last_field = 0;   /* ... and that posting's field (issue #303) */
    bool         have_block = false;
    BM25SegWalk  w;
    PosCursor    pc;
    bool         want_pos = (pos_cb != NULL && pos_post_root != InvalidBlockNumber);
    uint32      *posbuf = NULL;     /* decoded positions of the current frame */
    uint32       poscap = 0;
    char        *pgcopy = NULL;     /* private BLCKSZ copy of the current POST page */
    /* SEGREAD-11 (issue #154). Hoisted ONCE at entry, not per page: this function runs
     * per (term, segment), the same shape as bm25_seg_dict_lookup's hoist, so one lseek
     * here is amortized over the term's whole df-bounded run. It is the value the loop's
     * bm25_seg_chain_extent_validate compares against below. */
    BlockNumber  nblocks = RelationGetNumberOfBlocks(index);

    /* Issue #303: the walk starts mid-chain, at the term's post_root, and the revisit
     * test's "first page" is that page; nothing in a run links back to it. */
    bm25_seg_walk_init(&w, index, nblocks, expected_gen, BM25_PAGE_POST, false,
                       "segment POST chain");

    /* A df > 0 term whose POST chain cannot be entered at all. post_root arrives raw off
     * a DICT page: bm25_dictentry_validate bounds the entry's header and MAXALIGN'd term
     * span and looks at no pointer field, so nothing upstream has ever examined it.
     *
     * Without this the loop guard below simply never runs -- InvalidBlockNumber ends the
     * walk before its first iteration -- and the term SILENTLY YIELDS ZERO POSTINGS: a
     * term the dictionary says occurs in df documents matches none of them, on the
     * exhaustive ranked path, the @@@ membership path, the phrase pass, and the merge's
     * replay (where it would write the loss into a new segment permanently). Silent
     * under-answering is precisely what bm25_seg_chain_extent_validate's header argues a
     * walker must not do, and the reason the bound below ERRORs rather than stopping.
     *
     * bm25_seg_blkno_validate is the validator ADR 0071 introduced for exactly this
     * value, already on WAND's POST walk via wand_cursor_load_block. This is the same
     * gate on the OTHER POST walk -- the one every non-WAND path uses -- so a corrupt
     * post_root no longer reports itself differently depending on which reader followed
     * it. Gated on df > 0 because InvalidBlockNumber is the builder's deliberate "this
     * term wrote no blocks" sentinel (BUILD-07), which pairs with df == 0; that entry is
     * well-formed and its empty walk is the right answer.
     *
     * Before pos_cursor_open, so a rejected term raises without first pinning a POS
     * page. */
    if (df > 0)
        bm25_seg_blkno_validate(post_root, "postings chain root");

    /* M4: open the lockstep POS cursor only when the caller asked for positions AND
     * the term actually has a POS entry. Bag-of-words scans pass pos_cb == NULL, so
     * the POS chain is never faulted in (the section 3.6 central invariant). */
    if (want_pos)
        pos_cursor_open(&pc, index, expected_gen, pos_post_root, pos_post_off);

    while (blk != InvalidBlockNumber && emitted < df)
    {
        Buffer  buf;
        Page    pg;
        char   *cur, *pend;
        BlockNumber next;
        Size    pagebytes;
        uint32  blocks_here = 0;

        /* Innermost decode loop of every scored and @@@ scan: a check per
         * posting page is what makes a long scan cancellable at all. It is live
         * again as of #139 -- the POS cursor above used to hold a content lock
         * across this whole loop, and HOLD_INTERRUPTS made this a no-op.
         *
         * One unit of cancellable work per POST page (#156), counted independently of
         * the check for the reason BM25_WORK_UNIT()'s header in bm25.h records: if this
         * check goes dead again, the pages are still decoded and the count still
         * climbs, which is what makes the interrupt suites' assertions fail rather
         * than pass vacuously.
         *
         * sql/66 injects its cancel at the pause point, mid-loop, rather than by a
         * timer, whose service time is the runner's (ADR 0070's 2026-10-06 addendum).
         * Deleting this check alone does NOT fail that suite: chain_read_at's
         * per-posting check services the cancel within the same page. Deletion is
         * what ci.yml's grep floor catches; the suite catches the check going DEAD. */
        bm25_debug_pause_point("scan_post_page");
        BM25_WORK_UNIT();
        CHECK_FOR_INTERRUPTS();

        /* SEGREAD-11 and issue #303, before ReadBuffer, inside the walker: block 0,
         * the extent, the revisit test and the cap. `blk` is post_root on the first
         * pass and a page-opaque nextblk after that, and `emitted < df` counts
         * postings, which says nothing about where the chain points. ERRORs rather
         * than ending the walk -- a truncated POST walk is never a defensible answer
         * here (it is the same silent under-answer the post_root guard above
         * describes, just reached mid-chain), and the term's run is bounded by df,
         * not by chain end. */
        if (pgcopy == NULL)
            pgcopy = (char *) palloc(BLCKSZ);

        /* COPY-THEN-UNLOCK, same shape and the same reason as the POS cursor's
         * (see pos_cursor_load). Validate and bound UNDER the lock, copy the page,
         * release it, and decode from the private copy. Three things follow:
         *
         *   - chain_read_at's own CHECK_FOR_INTERRUPTS becomes live. It is only
         *     ever reached from cb/pos_cb, i.e. from inside this loop body, so the
         *     POST lock alone was enough to keep it dead even after the POS cursor
         *     was fixed -- and its worst case is a cursor backward jump re-walking
         *     a long NORMS chain per posting.
         *   - the three nested lock levels (POS, POST, and whatever chain page a
         *     callback reads) collapse to at most one at a time.
         *   - the callbacks' palloc / hash_search / repalloc (bm25_scan_match.c's
         *     phrase_stash_add) no longer run under a buffer content lock, where an
         *     allocation-time ERROR or a context callback would have been a much
         *     worse place to fail from.
         *
         * The copy is allocated once and reused for every page of the run.
         *
         * pend comes from the walker's validated content length, not raw pd_lower: a
         * corrupt pd_lower below SizeOfPageHeaderData leaves pend < cur, which this
         * loop's own guard reads as "no blocks on this page" and emits nothing -- the
         * term's postings vanish silently instead of erroring. */
        buf = bm25_seg_walk_read(&w, blk, &pagebytes);
        pg = BufferGetPage(buf);
        memcpy(pgcopy, pg, BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg = (Page) pgcopy;
        next = BM25PageGetOpaque(pg)->nextblk;
        /* First page starts at off; subsequent pages start at content top. Bound the
         * DICT-supplied offset before it becomes a pointer -- unbounded, a corrupt
         * off both forms an out-of-range pointer and leaves cur > pend, which this
         * loop's own guard reads as "no blocks here" and silently drops the term's
         * postings instead of erroring. */
        if (blk == post_root)
            bm25_seg_page_off_validate(off, pagebytes, "postings");
        cur = (char *) PageGetContents(pg) + (blk == post_root ? off : 0);
        pend = (char *) PageGetContents(pg) + pagebytes;

        while (cur + sizeof(BM25BlockHeader) <= pend && emitted < df)
        {
            BM25BlockHeader hdr;
            const uint8    *dp, *tp;
            const uint8    *dend, *tend;
            uint32          prev = 0;   /* reset per block: blocks decode from 0 */
            uint32          i;
            uint32          blk_fields[BM25_POSTINGS_PER_BLOCK];

            memcpy(&hdr, cur, sizeof(hdr));
            /* Was Assert(hdr.ndocs <= BM25_POSTINGS_PER_BLOCK), justified by "sealed
             * pages are trusted (option-(d) gen-validated)". That justification does
             * not hold: bm25_seg_page_validate compares seg_gen and nothing else, so
             * it says nothing about ndocs. In a production build the Assert is gone
             * and a header claiming 65535 docs wrote 256 KB into the 512-byte
             * blk_fields[] below -- a stack smash on the query hot path. */
            (void) bm25_block_validate(&hdr, cur, pend);
            /* Issue #293. The builder slices a term's df postings into blocks, so every
             * block of a well-formed run fits in what the run has left; a block that
             * claims more is not this term's (or not a block at all). Checked here, per
             * block, because it is also what lets the posting loop below consume the
             * WHOLE block, which the last_docid check after that loop depends on. */
            if (hdr.ndocs > df - emitted)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: posting block claims %u postings but the term's run "
                                "has only %u left",
                                hdr.ndocs, df - emitted),
                         errdetail("The block is on page %u of the postings chain.", blk),
                         errhint("REINDEX the index.")));
            dp = (const uint8 *) (cur + sizeof(BM25BlockHeader));
            dend = dp + hdr.docid_bytes;
            tp = dend;
            tend = tp + hdr.tf_bytes;
            /* Decode the block's field-id RLE ONCE, in lockstep with docid/tf.
             * When field_rle_bytes == 0 (single-field / M3 back-compat page) every
             * posting is field 0 and no RLE bytes exist on the page. */
            if (hdr.field_rle_bytes > 0)
                bm25_field_rle_decode(tp + hdr.tf_bytes, hdr.field_rle_bytes,
                                      blk_fields, hdr.ndocs);
            else
                for (i = 0; i < hdr.ndocs; i++)
                    blk_fields[i] = 0;
            /* Cross-block order, one peeked varbyte per block (prev restarts at 0, so a
             * block's first delta IS its first docid), on (docid, field) taken
             * lexicographically (issue #303). Postings ascend by (docid, field) within
             * a term's run (D-ACCUM; the merge replays segment-then-docid order), and a
             * document can legitimately straddle two blocks in a segment written before
             * the builder cut blocks at document boundaries (issue #289) -- but it
             * continues at a HIGHER field, so the rule is exact for both layouts. The
             * docid-only `<` this replaces admitted a single-document block linked to
             * itself, which emitted the same posting again. A run that steps backwards
             * is a link into some other run. */
            if (have_block)
            {
                uint32  first;

                (void) bm25_varbyte_decode(dp, dend, &first);
                if (first < prev_last ||
                    (first == prev_last && blk_fields[0] <= prev_last_field))
                    ereport(ERROR,
                            (errcode(ERRCODE_INDEX_CORRUPTED),
                             errmsg("bm25: posting block starts at docid %u field %u, not "
                                    "after the previous block's last posting (docid %u "
                                    "field %u)",
                                    first, blk_fields[0], prev_last, prev_last_field),
                             errdetail("The block is on page %u of the postings chain.", blk),
                             errhint("REINDEX the index.")));
            }
            /* No `emitted < df` term: the ndocs check above already holds the block
             * inside the run, so the whole block is always consumed. */
            for (i = 0; i < hdr.ndocs; i++)
            {
                uint32 delta, tf;
                uint32 fid = blk_fields[i];

                dp += bm25_varbyte_decode(dp, dend, &delta);
                tp += bm25_varbyte_decode(tp, tend, &tf);
                prev += delta;
                cb(prev, tf, fid, state);

                /* M4: in lockstep, consume this posting's POS frame -- but ONLY when
                 * the field bears positions (D12 gate). A positions-off field wrote no
                 * frame, so the reader must NOT read one, or the streams desync. The
                 * gate is field_store_positions[fid] (NULL => all fields on). */
                if (want_pos &&
                    (field_store_positions == NULL ||
                     (fid < field_count && field_store_positions[fid])))
                {
                    uint32 tfcount = pos_cursor_varbyte(&pc);
                    uint32 p, pos;

                    /* D2 self-verifying lockstep: the frame's tf-count MUST equal the
                     * posting's tf from the POST stream. A mismatch means the two
                     * chains fell out of step (corruption) -- fail loud, never misparse. */
                    if (tfcount != tf)
                        ereport(ERROR,
                                (errcode(ERRCODE_INDEX_CORRUPTED),
                                 errmsg("bm25: position frame tf-count %u != posting tf %u "
                                        "(local_docid %u field %u): position stream desync",
                                        tfcount, tf, prev, fid)));

                    if (tfcount > poscap)
                    {
                        poscap = tfcount;
                        posbuf = (posbuf == NULL)
                            ? palloc(sizeof(uint32) * poscap)
                            : repalloc(posbuf, sizeof(uint32) * poscap);
                    }
                    pos = 0;
                    for (p = 0; p < tfcount; p++)
                    {
                        uint32 dpos = pos_cursor_varbyte(&pc);   /* first = pos[0] */

                        /* Issue #303.F: a (term, field, doc) list STRICTLY ascends --
                         * position advanced per lexeme before analyzer revision 5 and
                         * is deduplicated within a run since (ADR 0087) -- so after
                         * the first, a zero delta or one that wraps is corruption. The
                         * phrase matcher's binary searches and its slot assignment
                         * assume the order (sdr_assign Asserts it), and this frame
                         * also feeds the merge replay, which would copy a bad list
                         * into a healthy-looking segment. */
                        if ((p > 0 && dpos == 0) || pos + dpos < pos)
                            ereport(ERROR,
                                    (errcode(ERRCODE_INDEX_CORRUPTED),
                                     errmsg("bm25: position list of local_docid %u field %u "
                                            "does not ascend: delta %u after position %u",
                                            prev, fid, dpos, pos),
                                     errdetail("The delta ends on block %u of the positions chain.", pc.blk),
                                     errhint("REINDEX the index.")));
                        pos += dpos;
                        posbuf[p] = pos;
                    }
                    pos_cb(prev, fid, posbuf, tfcount, pos_state);
                }
                emitted++;
            }
            /* After the whole block, the running prev IS the run's last docid, so the
             * header's last_docid can be checked without materializing the run (which
             * is why ADR 0073 left this reader out). The same equality, and message,
             * as bm25_block_last_docid_validate on the WAND decode. */
            if (prev != hdr.last_docid)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: posting block header claims last docid %u but its "
                                "run ends at %u",
                                hdr.last_docid, prev),
                         errdetail("The block is on page %u of the postings chain.", blk),
                         errhint("REINDEX the index.")));
            prev_last = prev;
            prev_last_field = blk_fields[hdr.ndocs - 1];
            have_block = true;
            blocks_here++;
            /* Advance PAST the RLE tail and the v5 impact table too: omitting either
             * would mis-locate the next block's header. impact_bytes is never 0 on a
             * v5 page (bm25_meta_read rejects any earlier format), so this advance is
             * unconditional; the impact table itself is not decoded here -- the scorer
             * (Task 3+) reads it separately given the block's start offset. */
            cur += sizeof(BM25BlockHeader) + hdr.docid_bytes + hdr.tf_bytes +
                   hdr.field_rle_bytes + hdr.impact_bytes;
        }
        /* Progress (issue #303). Every page this loop enters holds at least one block
         * of the run: chain_write places blocks whole and opens a page only for a
         * block that did not fit (bm25_seg_build.c), so (post_root, post_off) names a
         * real block and every continuation page starts with one. A page that yielded
         * none was a link into the middle of nowhere, and a cycle through such pages
         * spun until cancelled -- the walker's revisit test sees only some of them. */
        if (blocks_here == 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: postings chain page at block %u holds none of the "
                            "term's remaining %u postings", blk, df - emitted),
                     errhint("REINDEX the index.")));
        blk = next;
    }

    if (want_pos)
        pos_cursor_close(&pc);
    if (posbuf != NULL)
        pfree(posbuf);
    if (pgcopy != NULL)
        pfree(pgcopy);

    /* Issue #293. The loop above ends at df postings or at the chain's end, and only
     * the first is a finished run: the chain reaching nextblk == InvalidBlockNumber, or
     * running out of blocks on its last page, before df is a truncated run. Every
     * non-WAND reader (the @@@ collector, the exhaustive scorer, the phrase and BM25F
     * df passes, bm25_debug_postings) and the merge replay come through here, so this
     * one check is what keeps a short chain from being a short answer -- and from
     * being laundered by a merge into a new segment no validator could fault. emitted
     * cannot exceed df (the per-block ndocs check). df == 0 with an Invalid root is the
     * builder's empty-term sentinel and passes trivially. */
    if (emitted != df)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: postings chain ends after %u of the term's %u postings",
                        emitted, df),
                 errdetail("The chain starts at block %u, offset %u.", post_root, post_off),
                 errhint("REINDEX the index.")));
}

/* Header-only block reader (M2b Task 3, the WAND fast-path primitive): read one
 * block's BM25BlockHeader + its trailing impact table WITHOUT varbyte-decoding the
 * docid/tf/field-RLE streams, then yield the (blk, off) of the NEXT block in the
 * chain (*next_blk == InvalidBlockNumber at chain end). Follows
 * bm25_seg_scan_postings' page-walk rules -- option-(d) seg_gen validation, the
 * block-pointer and in-page-offset bounds, and the SAME "another block fits on this
 * page -> stay; else nextblk starts at offset 0" rule that walker's outer/inner loop
 * pair encodes -- but this handles exactly ONE block per call instead of looping to
 * df. It is NOT an exact mirror: it holds the page's content lock through the decode
 * (the walker copies the page and unlocks first), takes no chain-extent check and
 * only a self-link test in place of the walker's revisit test, so a caller that walks
 * a chain with it must bound that walk itself. The caller (a block-max skip
 * decision, or bm25_debug_block_impacts in bm25_seg_debug.c) is the one that must
 * track cumulative ndocs against df to know when a TERM's run of blocks ends, since
 * a block carries no end-of-term marker (D-POST: every term's blocks live
 * back-to-back in one shared per-segment chain). */
void
bm25_seg_block_header_read(Relation index, BlockNumber blk, uint16 off,
                            uint32 expected_gen, BM25BlockHeader *hdr,
                            BM25BlockImpact *imp,
                            BlockNumber *next_blk, uint16 *next_off)
{
    bm25_seg_block_header_read_lead(index, blk, off, expected_gen, hdr, imp,
                                    next_blk, next_off, 0, NULL);
}

/* The header-only read above, optionally also yielding the block's LEAD -- its
 * first document's docid and postings (issue #289; NULL skips it). The WAND deep
 * check asks for it, when the next block is on another page, to learn whether and
 * how the resident block's last document continues into this block (match_docid:
 * see bm25_block_lead_decode).
 *
 * Not a block decode: the first docid is stored absolute (encode_block deltas from
 * 0) and the lead is at most BM25_MAX_FIELDS postings. Everything it reads is on
 * THIS page, inside the bytes bm25_block_validate has just bounded: chain_write
 * places a block whole on one page, and the validator checks it against pend. */
void
bm25_seg_block_header_read_lead(Relation index, BlockNumber blk, uint16 off,
                                uint32 expected_gen, BM25BlockHeader *hdr,
                                BM25BlockImpact *imp,
                                BlockNumber *next_blk, uint16 *next_off,
                                uint32 match_docid, BM25BlockLead *lead)
{
    Buffer      buf;
    Page        pg;
    char       *cur, *pend;
    BlockNumber nextpage;
    Size        block_len;
    Size        pagebytes;

    /* The third page-sourced-BlockNumber reader, and the one easiest to overlook
     * because it only PEEKS a header. Its two feeders are untrusted all the same:
     * wand_cursor_sweep_global_ub passes post_root straight off a DICT entry, and
     * next_geq's peek loop passes a page-opaque nextblk. Both loop guards happen to
     * exclude InvalidBlockNumber, so this was never a relation-extension hazard --
     * but block 0 reached ReadBuffer, and the sweep runs at cursor OPEN, before the
     * gated wand_cursor_load_block, so a post_root corrupted to 0 produced exactly
     * the misleading triple-retry this validator exists to eliminate. */
    bm25_seg_blkno_validate(blk, "postings block pointer");
    buf = ReadBuffer(index, blk);

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    /* Gen through the arm (issue #303), released first; then kind. */
    if (BM25PageGetOpaque(pg)->seg_gen != expected_gen)
    {
        uint32  page_gen = BM25PageGetOpaque(pg)->seg_gen;

        UnlockReleaseBuffer(buf);
        bm25_seg_gen_mismatch(index, blk, page_gen, expected_gen, "segment POST chain");
    }
    bm25_seg_page_validate_kind(pg, 0, BM25_PAGE_POST);
    nextpage = BM25PageGetOpaque(pg)->nextblk;
    /* Validated content length, not raw pd_lower -- see bm25_seg_scan_postings. */
    pagebytes = bm25_page_content_bytes(pg);
    /* Bound `off` before it becomes a pointer, as bm25_seg_scan_postings does: it is a
     * uint16 off a DICT entry or the previous block's link, and `contents + off` for a
     * corrupt one points up to 64 KB into an 8 KB page before the test below sees it. */
    bm25_seg_page_off_validate(off, pagebytes, "postings block");
    cur = (char *) PageGetContents(pg) + off;
    pend = (char *) PageGetContents(pg) + pagebytes;

    /* Caller-maintained invariant: (blk, off) always names a real block start --
     * either the term's dict-entry (post_root, post_off) on the first call, or a
     * position this same function handed back on a prior call. Never mid-block,
     * never past pd_lower. `off` comes off a page too, so this is a real check and
     * not only an internal-consistency one. The bound above admits off == pagebytes
     * (a legal one-past-the-end pointer); this test is what rejects it, because no
     * block header fits there. */
    if (cur + sizeof(BM25BlockHeader) > pend)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block offset %u past the end of block %u",
                        off, blk)));

    memcpy(hdr, cur, sizeof(BM25BlockHeader));
    /* Before the impact-table pointer is computed from three on-page uint16s: they
     * sum to at most 196605, so an unvalidated header pointed the decode ~190 KB
     * past an 8 KB page. Validating first also gives us block_len. */
    block_len = bm25_block_validate(hdr, cur, pend);

    bm25_decode_impact_table((const uint8 *) (cur + sizeof(BM25BlockHeader) +
                                              hdr->docid_bytes + hdr->tf_bytes +
                                              hdr->field_rle_bytes),
                             hdr->impact_bytes, imp);

    if (lead != NULL)
        bm25_block_lead_decode(hdr, cur, match_docid, lead);

    cur += block_len;

    if (cur + sizeof(BM25BlockHeader) <= pend)
    {
        /* Another whole block fits on THIS page: stay put. */
        *next_blk = blk;
        *next_off = (uint16) (cur - (char *) PageGetContents(pg));
    }
    else
    {
        /* Page exhausted (< one header's worth of room left, incl. none at all):
         * the chain continues on nextpage, which always starts fresh at content
         * offset 0 (same convention bm25_seg_scan_postings uses for continuation
         * pages). nextpage == InvalidBlockNumber here means true chain end.
         *
         * A page linked to itself (issue #303) would hand back this page's first
         * block again. Only on this branch: staying on the page is the other one. */
        if (nextpage == blk)
        {
            UnlockReleaseBuffer(buf);
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: postings chain page at block %u links to itself", blk),
                     errhint("REINDEX the index.")));
        }
        *next_blk = nextpage;
        *next_off = 0;
    }

    UnlockReleaseBuffer(buf);
}

/* ---------------------------------------------------------------------------
 * Dense per-docid chains: LIVEDOCS (bitmap), DOCMAP (ItemPointerData[]) and NORMS
 * (uint32[] row-major by docid). All three are byte-addressed arrays spread over a
 * nextblk chain, so a lookup is "read len bytes at absolute content offset off".
 *
 * H16: each of the three readers used to re-walk its chain FROM THE ROOT on every
 * call, and all three are called PER SCORED POSTING (seg_posting_cb) as well as
 * per doc per field in the merge replay. A single-field segment with 1M docs has a
 * ~490-page NORMS chain, so a term with df = 500k cost ~500k x 245 average page
 * visits -- ~1.2e8 ReadBuffer + LWLock pairs for one term, growing quadratically
 * with segment size, and the same again twice over for the live check and the TID
 * lookup. No wrong answers, but superlinear on the primary query path with no
 * guardrail and no acknowledging comment.
 *
 * The fix exploits the access pattern rather than building an index over the chain:
 * every hot caller walks docids FORWARD (D-ACCUM's ascending-(docid,field_id)
 * posting order, the merge's `for d in 0..ndocs`, WAND's forward-only skipping), and
 * the NORMS row for one docid is field_count CONTIGUOUS cells. So a caller-held
 * BM25ChainCursor remembers the page a lookup landed on plus the content bytes
 * before it, and the next lookup resumes there instead of at the root. Ascending
 * access is then O(1) amortized -- the cursor advances one page per ~2000 docids
 * instead of re-walking half the chain each time.
 *
 * A cursor is a pure OPTIMIZATION and cannot affect the answer, and chain_read_at's
 * resume test is what makes that so rather than the struct's shape: it resumes only
 * when the cursor names the SAME CHAIN ROOT and the target is at or after the
 * cursor's page, so the cursor only ever supplies a starting point, the walk falls
 * back to the root on a backward jump or a different chain, and EVERY page actually
 * read is still seg_gen- and kind-validated (option (d)) -- pages the walk now skips
 * are pages it no longer touches at all. Passing NULL is always correct and is what
 * the one-shot public wrappers below do.
 *
 * The root clause is SEGREAD-14 (issue #154). Before it the test read blk/seen alone,
 * so the sentence above was true of the CALLERS (each re-inits per segment and keeps
 * one cursor per chain) rather than of the code: a cursor handed a second chain
 * resumed at the first chain's page and byte origin. See chain_read_at's own comment
 * for what that produced.
 * --------------------------------------------------------------------------- */

/* The memcpy span check chain_read_at runs immediately before trusting a bounded
 * read off an already-validated page: `local_off` (this record's byte offset
 * WITHIN the page, i.e. off - seen) plus `len` must not exceed `pagebytes` (the
 * page's total validated content, from bm25_page_content_bytes). Without it, a
 * page whose content was truncated mid-record by a corrupt pd_lower would have
 * the memcpy below read page slack -- uninitialized bytes beyond the last whole
 * record -- into *dst as if it were a real NORMS doclen, DOCMAP tid, or LIVEDOCS
 * bitmap byte, the same shape bm25_seg_key_span_validate (ADR 0039) guards in the
 * KEYMAP walker. Extracted to its own function purely so
 * bm25_debug_chain_span_validate (below) can drive it with caller-chosen values;
 * chain_read_at is still the only production caller. */
static void
bm25_chain_span_validate(Size local_off, Size len, Size pagebytes)
{
    if (local_off + len > pagebytes)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment chain read overruns its page"),
                 errdetail("The read starts at offset %zu with length %zu; the page holds %zu.",
                           local_off, len, pagebytes)));
}

/* Decode-boundary probe (2026-08 page-content-bytes sweep). Same reasoning as
 * bm25_debug_page_content_bytes in bm25_seg_read.c: this fires only on an
 * already-corrupt page, so the probe drives bm25_chain_span_validate directly with caller-chosen
 * values rather than needing a real truncated chain page. Returns local_off on
 * success (mirrors bm25_debug_seg_key_bounds returning its validated width) so a
 * well-formed call demonstrates the accepted offset. */
PG_FUNCTION_INFO_V1(bm25_debug_chain_span_validate);
Datum
bm25_debug_chain_span_validate(PG_FUNCTION_ARGS)
{
    int64 local_off = PG_GETARG_INT64(0);
    int64 len       = PG_GETARG_INT64(1);
    int64 pagebytes = PG_GETARG_INT64(2);

    if (local_off < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_chain_span_validate: local_off must be non-negative")));
    if (len < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_chain_span_validate: len must be non-negative")));
    if (pagebytes < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_chain_span_validate: pagebytes must be non-negative")));

    bm25_chain_span_validate((Size) local_off, (Size) len, (Size) pagebytes);
    PG_RETURN_INT64(local_off);
}

/* ---- Chain page images (issue #267) ----
 *
 * After #264 checked LIVEDOCS once per segment, what a frequent-term ranked build still
 * paid per scored pair was one NORMS ReadBuffer per field posting and one DOCMAP
 * ReadBuffer per candidate, almost all of them for the page the cursor was already on
 * (ADR 0100). A cursor that opted in keeps a copy of the page its last lookup landed on
 * and serves the next same-page lookup from it.
 *
 * Why a copy and not a pin (ADR 0100 named the pin): a pin held between calls has to be
 * released on every exit, including ERROR and the scan callbacks' non-local exits, so
 * every reader's owner would need resource-owner handling, and each lookup would still
 * take the content lock. A palloc'd copy needs neither: the buffer is released before
 * this function returns, as before, and the copy is ordinary memory that goes with its
 * context. Copying a page and reading it unlocked is what bm25_seg_scan_postings and
 * the POS cursor already do (ADR 0063).
 *
 * Why the copy is the page: NORMS, DOCMAP and KEYMAP are written once, as orphan
 * chains, before the publish record makes the segment visible, and nothing writes them
 * in place afterwards; gens never repeat, so a page that validated as (kind, gen) holds
 * those bytes for as long as it belongs to that segment. A lookup served from the copy
 * therefore returns the bytes a fresh read would, at zero buffer accesses. If the page
 * is reclaimed and reused meanwhile, a fresh read would fail the gen check and a
 * ranked build would retry (the @@@ collector, outside the retry wrapper, would
 * error); the copy instead keeps returning the segment's own bytes, which is
 * what the scan's snapshot asked for. A copy is used only for the gen it was taken at
 * (see bm25_seg_walk_get), and the kind check is re-run on it.
 *
 * LIVEDOCS is the exception, enforced twice: bm25_seg_reader_cache_pages never gives a
 * LIVEDOCS cursor a context, and these functions refuse BM25_PAGE_LIVE regardless.
 * bm25_livedocs_clear flips bits in place after seal, so a copy of a bitmap page could
 * miss a tombstone a fresh read sees, and VACUUM's bulkdelete and the merge replay
 * decide what to drop from exactly those reads. */
static inline bool
chain_page_cacheable(const BM25ChainCursor *cur, uint16 want_kind)
{
    return cur != NULL && cur->img_cxt != NULL && want_kind != BM25_PAGE_LIVE;
}

Page
bm25_seg_walk_get(BM25SegWalk *w, BM25ChainCursor *cur, BlockNumber blk, Buffer *buf,
                  Size *pagebytes)
{
    /* The image answers only for the block AND gen it was taken at. A gen mismatch
     * (a cursor handed another segment's chain whose page reuses that block number --
     * no production caller does that) falls through to a fresh read, so the copy can
     * never turn a read that would succeed into an error, or the reverse. The image
     * passed the extent and content checks when it was taken; the revisit test and
     * the cap run on every visit, image or read (see BM25SegWalk). */
    if (chain_page_cacheable(cur, w->kind) && cur->img != NULL && cur->img_blk == blk &&
        BM25PageGetOpaque((Page) cur->img)->seg_gen == w->gen)
    {
        (void) seg_walk_step(w, blk);
        *pagebytes = seg_walk_page_validate(w, InvalidBuffer, (Page) cur->img, blk);
        *buf = InvalidBuffer;
        return (Page) cur->img;
    }

    /* Allocated before the buffer is locked, so an out-of-memory error never fires
     * under a content lock. One allocation per cursor; later pages overwrite it. */
    if (chain_page_cacheable(cur, w->kind) && cur->img == NULL)
        cur->img = (char *) MemoryContextAlloc(cur->img_cxt, BLCKSZ);

    Assert(!w->quiet);          /* invariant: the image readers never stop quietly */
    *buf = bm25_seg_walk_read(w, blk, pagebytes);
    return BufferGetPage(*buf);
}

/* Called on the page a lookup landed on, while its buffer is still share-locked: the
 * copy is taken after bm25_seg_walk_get validated the page and replaces the cursor's
 * previous image. A page served from the image (buf invalid) is already the image. */
void
bm25_chain_page_keep(BM25ChainCursor *cur, BlockNumber blk, Page pg, Buffer buf,
                     uint16 want_kind)
{
    if (!chain_page_cacheable(cur, want_kind) || cur->img == NULL || !BufferIsValid(buf))
        return;
    memcpy(cur->img, pg, BLCKSZ);
    cur->img_blk = blk;
}

void
bm25_chain_page_done(Buffer buf)
{
    if (BufferIsValid(buf))
        UnlockReleaseBuffer(buf);
}

/* Content bytes chain_write (bm25_seg_build.c) leaves on every page of a dense chain
 * but its last. chain_ensure rotates to a new page when the next whole value no longer
 * fits CHAIN_PAGE_CAPACITY (the same expression as `cap` here), so a DOCMAP or NORMS
 * page holds floor(cap / value size) values; the LIVE writer hands chain_write spans
 * of exactly cap bytes, so a LIVE page is full to the byte. The LIVE span is also
 * what bm25_livedocs_bits_per_page (bm25_seg_read.c) divides by. */
Size
bm25_chain_full_span(uint16 kind)
{
    Size    cap = BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BM25PageOpaque));

    switch (kind)
    {
        case BM25_PAGE_LIVE:
            return cap;
        case BM25_PAGE_DOCMAP:
            return cap - cap % sizeof(ItemPointerData);
        case BM25_PAGE_NORMS:
            return cap - cap % sizeof(uint32);
        default:
            elog(ERROR, "bm25: no fixed page span for chain kind 0x%x", kind);
            return 0;           /* keep the compiler quiet */
    }
}

static const char *
chain_kind_name(uint16 kind)
{
    switch (kind)
    {
        case BM25_PAGE_LIVE:
            return "live-docs";
        case BM25_PAGE_DOCMAP:
            return "doc-map";
        case BM25_PAGE_NORMS:
            return "norms";
        default:
            return "segment";
    }
}

/* Read `len` bytes at absolute content offset `off` from the chain rooted at `root`.
 * `cur` may be NULL.
 *
 * Every caller range-checks its docid against h->ndocs first, and the builder writes
 * each chain to exactly the length ndocs implies (bm25_seg_build.c), so `off` is
 * always inside a well-formed chain. Reaching the chain's end first is therefore
 * corruption and ERRORs (issue #294). This used to return false, and the callers
 * turned that into "live", an invalid TID (which the heap fetch reads as P_NEW and
 * extends the heap with) and doclen 0 -- defaults that only ever served a damaged
 * chain, resurrected tombstoned rows, and that the merge replay then made permanent.
 * VACUUM's bm25_livedocs_locate already ERRORed on the same condition.
 *
 * Every page walked PAST must also hold the writer's full span for its kind
 * (bm25_chain_full_span): this walk addresses by cumulative content bytes while
 * bm25_livedocs_locate divides by a fixed span, and a short non-final page would make
 * the two (and this walk and the writer) address different cells for every later
 * docid. The page a lookup lands on needs no such check -- the span check below bounds
 * the read within it, and whether it is short only matters to a later docid, whose
 * walk passes it.
 *
 * A stored value never straddles a page: chain_write only ever writes a value whole,
 * moving to the next page when it does not fit (bm25_seg_build.c), so pd_lower is a
 * whole number of elements and the memcpy below stays inside one page.
 *
 * want_kind (H6): this is the ONE genuinely multi-kind reader in the file -- its
 * three callers below hand it livedocs_root (LIVE), docmap_root (DOCMAP) and
 * norms_root (NORMS). Threading the caller's expected kind through as a parameter
 * keeps the check STRICT per caller; a LIVE|DOCMAP|NORMS bitmask here would accept
 * a docmap_root aimed at the norms chain, which is exactly the SEGREAD-06 shape
 * (all three chains carry the same seg_gen, and all three are addressed by a raw
 * byte offset, so a swapped root reads a plausible-looking value from the wrong
 * chain and silently mis-scores rather than erroring). */
static void
chain_read_at(Relation index, BM25SegmentHeader *h, BlockNumber root,
              BM25ChainCursor *cur, Size off, void *dst, Size len,
              uint16 want_kind)
{
    BlockNumber blk;
    Size        seen;           /* content bytes on the pages BEFORE blk */
    BlockNumber nblocks;        /* extent bound for this walk (SEGREAD-11) */
    BM25SegWalk w;
    bool        w_open = false; /* w initialized: see the fast path below */

    /* Resume from the cursor only when it describes THE SAME CHAIN and the target is
     * at or after its page start; otherwise (a different chain, a backward jump, or
     * an unpositioned cursor) start over.
     *
     * cur->root == root is the SEGREAD-14 clause (issue #154). `seen` is a byte
     * origin within one chain and `blk` a page of that chain, so both are meaningless
     * against any other root: without this test a cursor carried to a second chain
     * resumed at a page of the first one at an offset measured in the first one. What
     * happened next depended on where it landed -- usually the page-kind check firing
     * on a NORMS page presented as DOCMAP, but a same-kind chain (another segment's
     * NORMS) would have been read as if it were this one. No production caller does
     * this today; the test is what keeps "a cursor cannot change an answer" a property
     * of the code rather than of the current call sites.
     *
     * blk == BM25_METAPAGE_BLKNO is also treated as unpositioned. That is not a real
     * chain page (block 0 is the metapage), and rejecting it makes an all-zeroes
     * BM25SegReader safe: InvalidBlockNumber is 0xFFFFFFFF, not 0, so a reader embedded
     * in a MemoryContextAllocZero'd struct whose owner forgot bm25_seg_reader_init would
     * otherwise claim to be positioned at the metapage. The same zero fill leaves
     * root == 0, which is the metapage block too and therefore never equal to a real
     * chain root -- so the new clause cannot be satisfied by accident either; the blk
     * test is what actually carries that case, and this one does not weaken it. */
    if (cur != NULL && cur->blk != InvalidBlockNumber &&
        cur->blk != BM25_METAPAGE_BLKNO && cur->root == root && cur->seen <= off)
    {
        blk  = cur->blk;
        seen = cur->seen;
    }
    else
    {
        blk  = root;
        seen = 0;
    }

    /* SEGREAD-11 (issue #154). Taken from the cursor when it has one, because this
     * function runs PER SCORED POSTING and RelationGetNumberOfBlocks lseeks on every
     * call outside recovery -- ADR 0071's cost objection applies here more sharply
     * than anywhere else in the file, and its prescribed answer is to capture at
     * cursor open and compare against the captured value. nblocks == 0 means the
     * cursor was never opened through bm25_seg_reader_init (a zero fill), so fall
     * back to capturing for this call rather than rejecting every block; the
     * cursor-less one-shot wrappers take the same path and pay one lseek per call,
     * which is a rounding error beside the root-to-target re-walk they already do. */
    if (cur != NULL && cur->nblocks != 0)
        nblocks = cur->nblocks;
    else
        nblocks = RelationGetNumberOfBlocks(index);
    while (blk != InvalidBlockNumber)
    {
        Buffer      buf;
        Page        pg;
        Size        pagebytes;
        BlockNumber next;
        bool        here = false;
        bool        fast = false;

        CHECK_FOR_INTERRUPTS();

        /* The per-posting common case (issue #303's perf A/B): the first page of this
         * call is the one the cursor's image holds, and the lookup lands on it. A walk
         * of one page has nothing for the walker's pre-read checks to find -- the image
         * passed block 0, the extent and the content bound when it was taken, there is
         * no earlier page to revisit, and one visit is under any cap -- so it is served
         * as before the walker existed, with the content bound and the kind check on
         * the image. Measured: initializing and stepping the walker here cost ~6% of a
         * frequent-term WAND top-k. Any other first page, and every later one, goes
         * through the walker, which is initialized on that first use. */
        if (!w_open && chain_page_cacheable(cur, want_kind) && cur->img != NULL &&
            cur->img_blk == blk && BM25PageGetOpaque((Page) cur->img)->seg_gen == h->gen)
        {
            pg = (Page) cur->img;
            buf = InvalidBuffer;
            pagebytes = bm25_page_content_bytes(pg);
            bm25_seg_page_validate_kind(pg, 0, want_kind);
            fast = (off < seen + pagebytes);
        }
        if (!fast)
        {
            /* One walk per call, from wherever this call starts: the revisit test's
             * "first page" is the cursor's page on a resume, which is sound because the
             * walk only ever moves forward from it (see BM25SegWalk). Stores only. */
            if (!w_open)
            {
                bm25_seg_walk_init(&w, index, nblocks, h->gen, want_kind, false,
                                   "segment chain");
                w.root = root;
                w_open = true;
            }
            /* Block 0, the extent, the revisit test and the cap run before any
             * ReadBuffer, inside bm25_seg_walk_get, and error rather than end the walk,
             * for the same reason the chain's end does below. The content bound and the
             * kind/gen check run on every page, image or buffer; pagebytes feeds `seen`,
             * an accumulator every later iteration's offset comparison trusts. */
            pg = bm25_seg_walk_get(&w, cur, blk, &buf, &pagebytes);
        }
        if (off < seen + pagebytes)
        {
            /* `off < seen + pagebytes` only bounds the memcpy's START offset on
             * this page; it says nothing about `len` past that point.
             * bm25_chain_span_validate (above) is the same check, extracted so
             * bm25_debug_chain_span_validate can drive it directly. */
            bm25_chain_span_validate(off - seen, len, pagebytes);
            memcpy(dst, (char *) PageGetContents(pg) + (off - seen), len);
            here = true;
            if (cur != NULL)
            {
                /* All three stamped together: blk and seen are only interpretable
                 * against the chain they were measured in, so leaving root behind
                 * would be worse than not recording it at all. */
                cur->blk  = blk;
                cur->seen = seen;
                cur->root = root;
            }
            bm25_chain_page_keep(cur, blk, pg, buf, want_kind);
        }
        next = BM25PageGetOpaque(pg)->nextblk;
        bm25_chain_page_done(buf);
        if (here)
            return;
        /* Walking past this page: it is not the chain's last as far as `off` is
         * concerned, so it must be full (see the header). Tested after the buffer is
         * released, on values already copied out, and only on a page transition: a
         * lookup that lands on the cursor's page, the per-posting common case, never
         * gets here. */
        if (next != InvalidBlockNumber && pagebytes != bm25_chain_full_span(want_kind))
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: %s chain page at block %u holds %zu content bytes, not "
                            "the %zu every page but the last carries",
                            chain_kind_name(want_kind), blk, pagebytes,
                            bm25_chain_full_span(want_kind)),
                     errhint("REINDEX the index.")));
        seen += pagebytes;
        blk = next;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INDEX_CORRUPTED),
             errmsg("bm25: %s chain ends at %zu content bytes, before offset %zu",
                    chain_kind_name(want_kind), seen, off),
             errdetail("The segment (generation %u) holds " UINT64_FORMAT " documents.",
                       h->gen, h->ndocs),
             errhint("REINDEX the index.")));
}

/* Cursor-taking cores. The public one-shot entry points below pass cur = NULL. */

static bool
seg_doc_is_live_cur(Relation index, BM25SegmentHeader *h, BM25ChainCursor *cur,
                    uint32 local_docid, bool assume_live)
{
    uint8   byte;

    /* local_docid is NOT internally derived -- the comment that used to sit here
     * claiming so was wrong. Every scan-path caller passes the running sum of
     * varbyte deltas decoded off a POST page (`prev += delta` in
     * bm25_seg_scan_postings); bm25_block_validate bounds the block's byte spans
     * and nothing about the doc-ids they encode, so the sum is unbounded and can
     * wrap uint32. The debug SRFs pass a caller-supplied value. Out of range here
     * therefore means a corrupt page, which is exactly the case an Assert cannot
     * serve: it aborts the backend in a cassert build and is compiled out of the
     * production build where corrupt pages actually turn up. Hence the same
     * ERRCODE_INDEX_CORRUPTED every other decode boundary in the segment reader
     * raises.
     *
     * An in-range docid whose LIVEDOCS chain is short is corruption too, and
     * chain_read_at raises it; it used to read as "live" here (issue #294). */
    if (local_docid >= h->ndocs)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: local docid %u in a live-docs lookup exceeds the "
                        "segment's document count " UINT64_FORMAT,
                        local_docid, h->ndocs)));

    /* After the range check, not before: a reader that checked the bitmap at init
     * (bm25_seg_reader_init_checked) still rejects a corrupt docid the same way; it
     * only skips the per-lookup bitmap read. */
    if (assume_live)
        return true;

    chain_read_at(index, h, h->livedocs_root, cur,
                  (Size) local_docid / 8, &byte, sizeof(byte), BM25_PAGE_LIVE);
    return (byte & (uint8) (1u << (local_docid % 8))) != 0;
}

static ItemPointerData
seg_docid_to_tid_cur(Relation index, BM25SegmentHeader *h, BM25ChainCursor *cur,
                     uint32 local_docid)
{
    ItemPointerData out;

    /* Same provenance as seg_doc_is_live_cur's docid -- decoded off a POST page,
     * not derived here -- so the same real check rather than an Assert; see the
     * comment there. A stride of sizeof(ItemPointerData) makes an out-of-range
     * docid land on another document's DOCMAP cell, not outside any buffer. */
    if (local_docid >= h->ndocs)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: local docid %u in a doc-map lookup exceeds the "
                        "segment's document count " UINT64_FORMAT,
                        local_docid, h->ndocs)));
    chain_read_at(index, h, h->docmap_root, cur,
                  (Size) local_docid * sizeof(ItemPointerData),
                  &out, sizeof(ItemPointerData), BM25_PAGE_DOCMAP);

    /* The ONE docid -> TID chokepoint (issue #294): every scan path, the merge replay,
     * VACUUM's bulkdelete callback and the debug SRFs read DOCMAP through here, so the
     * cell is validated once, here, rather than at each caller. A chain check cannot
     * catch a corrupt CELL in an intact chain (a zeroed one, say), and the builder only
     * ever writes heap TIDs (the pending drain skips invalidated ones). What it must
     * not hand on: blkno == InvalidBlockNumber, which the heap fetch's ReadBuffer reads
     * as P_NEW and extends the heap with, from a read-only query, without the extension
     * lock; offset 0, which trips Asserts in the executor and TidStore; and an offset
     * past MaxOffsetNumber, which no page's line-pointer array can hold. The bound is
     * deliberately not MaxHeapTuplesPerPage: bm25 does not restrict the table access
     * method, and a non-heap AM may use offsets a heap page never would. An offset
     * inside the bound but past the page's last item is the table AM's to refuse; it
     * reads as "no such tuple", not as heap growth. ItemPointerIsValid alone is not
     * enough: it tests only that the offset is nonzero, so (InvalidBlockNumber, 1)
     * passes it. */
    if (ItemPointerGetBlockNumberNoCheck(&out) == InvalidBlockNumber ||
        ItemPointerGetOffsetNumberNoCheck(&out) < FirstOffsetNumber ||
        ItemPointerGetOffsetNumberNoCheck(&out) > MaxOffsetNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: doc-map cell for local docid %u holds an invalid heap "
                        "TID (%u,%u)",
                        local_docid, ItemPointerGetBlockNumberNoCheck(&out),
                        (unsigned) ItemPointerGetOffsetNumberNoCheck(&out)),
                 errdetail("The segment's generation is %u.", h->gen),
                 errhint("REINDEX the index.")));
    return out;
}

static uint32
seg_doclen_field_cur(Relation index, BM25SegmentHeader *h, BM25ChainCursor *cur,
                     uint32 local_docid, uint32 field_id)
{
    uint32  out = 0;

    /* BOTH indices arrive from the page; neither is derived here, and the two
     * Asserts that used to stand in for these checks said the opposite.
     *
     * local_docid: the POST-page delta sum, unbounded -- see seg_doc_is_live_cur.
     *
     * field_id: produced by bm25_field_rle_decode, which bounds it to
     * BM25_MAX_FIELDS and NEVER to THIS segment's field_count. A single-field
     * segment can therefore legitimately be handed field_id 7. field_count is the
     * NORMS row stride, so such an id does not read outside the chain -- it
     * addresses a cell belonging to a LATER DOCUMENT's row and returns it as this
     * document's field length. The failure mode is a silently wrong BM25 score,
     * not a buffer overrun, and in a production build (no Assert) it was entirely
     * silent. Loud beats silently wrong, so both are real checks. */
    if (local_docid >= h->ndocs)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: local docid %u in a norms lookup exceeds the "
                        "segment's document count " UINT64_FORMAT,
                        local_docid, h->ndocs)));
    if (field_id >= h->field_count)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field id %u in a norms lookup exceeds the segment's "
                        "field count %u",
                        field_id, h->field_count)));
    chain_read_at(index, h, h->norms_root, cur,
                  ((Size) local_docid * h->field_count + field_id) * sizeof(uint32),
                  &out, sizeof(uint32), BM25_PAGE_NORMS);
    return out;
}

bool
bm25_seg_doc_is_live(Relation index, BM25SegmentHeader *h, uint32 local_docid)
{
    return seg_doc_is_live_cur(index, h, NULL, local_docid, false);
}

ItemPointerData
bm25_seg_docid_to_tid(Relation index, BM25SegmentHeader *h, uint32 local_docid)
{
    return seg_docid_to_tid_cur(index, h, NULL, local_docid);
}

/* Per-(doc,field) length from the per-field NORMS chain. NORMS is a packed uint32
 * array laid row-major by docid: cell [local_docid*field_count + field_id]. C3's
 * BM25F scorer reads doclen_f from here -- via a reader (below), not this one-shot
 * form. */
uint32
bm25_seg_doclen_field(Relation index, BM25SegmentHeader *h, uint32 local_docid,
                      uint32 field_id)
{
    return seg_doclen_field_cur(index, h, NULL, local_docid, field_id);
}

/* ---- BM25SegReader: the per-chain cursors for one segment, held across a loop ----
 *
 * Init once per (caller loop, segment) and use the accessors instead of the one-shot
 * functions. Unless an owner opts in to page images (bm25_seg_reader_cache_pages,
 * issue #267), nothing is allocated and nothing needs freeing: the cursors are plain
 * scalars, so a stack-declared reader is the normal case and its lifetime is the
 * enclosing scope. Correctness does not depend on the reader being fresh, matching
 * the segment, or being reset: a cursor whose root does not match the chain being
 * read is ignored and the walk restarts from that chain's root (SEGREAD-14, see the
 * cursor note above). So without page images the only way to misuse it is to leave
 * performance on the table. With them, an owner can also misuse the memory: name a
 * context that dies before the reader (a dangling image), or re-init in a loop
 * without bm25_seg_reader_release_pages (one image per iteration left behind).
 *
 * ONE RelationGetNumberOfBlocks for all four cursors (SEGREAD-11; KEYMAP joined the
 * other three in issue #225): the extent is a property of the relation, not of a
 * chain, and one lseek per (caller loop, segment) is the budget ADR 0071 allows.
 * Stamping the roots here is not required for correctness -- chain_read_at (and the
 * KEYMAP walk) stamps root whenever it lands on a page, and an unpositioned cursor is
 * rejected on blk before root is consulted -- but a cursor that describes itself from
 * the moment it exists is one fewer invariant to reconstruct when reading a failure. */
void
bm25_seg_reader_init(BM25SegReader *r, Relation index, BM25SegmentHeader *h)
{
    BlockNumber nblocks = RelationGetNumberOfBlocks(index);

    r->index = index;
    r->h     = h;
    /* A re-init forgets any page image the reader held (issue #267) without freeing
     * it: the struct may be fresh stack garbage, so its old pointer cannot be trusted.
     * Owners that re-init in a loop call bm25_seg_reader_release_pages first. */
    bm25_chain_cursor_init(&r->norms, h->norms_root, nblocks);
    bm25_chain_cursor_init(&r->livedocs, h->livedocs_root, nblocks);
    bm25_chain_cursor_init(&r->docmap, h->docmap_root, nblocks);
    bm25_chain_cursor_init(&r->keymap, h->keymap_root, nblocks);
    r->key_size     = 0;
    r->assume_live  = false;
}

/* Opt-in, not part of the init (issue #267), because an image is memory in a context
 * the reader does not own: a reader initialized in one context and used from another
 * (an SRF's per-call context, the finalizers' scanctx) would otherwise allocate where
 * it does not live, and a reader re-initialized per segment would leave one image per
 * segment behind. Each opting-in owner names a context that outlives the reader and
 * bounds the images it holds at once; see the call sites.
 *
 * LIVEDOCS is left out by construction: bm25_livedocs_clear mutates that chain in
 * place, so a copy is not the page (see "Chain page images" above). */
void
bm25_seg_reader_cache_pages(BM25SegReader *r, MemoryContext cxt)
{
    r->norms.img_cxt  = cxt;
    r->docmap.img_cxt = cxt;
    r->keymap.img_cxt = cxt;
}

static void
chain_cursor_release_page(BM25ChainCursor *c)
{
    if (c->img != NULL)
        pfree(c->img);
    c->img     = NULL;
    c->img_blk = InvalidBlockNumber;
}

void
bm25_seg_reader_release_pages(BM25SegReader *r)
{
    chain_cursor_release_page(&r->norms);
    chain_cursor_release_page(&r->docmap);
    chain_cursor_release_page(&r->keymap);
}

bool
bm25_seg_reader_doc_is_live(BM25SegReader *r, uint32 local_docid)
{
    return seg_doc_is_live_cur(r->index, r->h, &r->livedocs, local_docid,
                               r->assume_live);
}

ItemPointerData
bm25_seg_reader_docid_to_tid(BM25SegReader *r, uint32 local_docid)
{
    return seg_docid_to_tid_cur(r->index, r->h, &r->docmap, local_docid);
}

uint32
bm25_seg_reader_doclen_field(BM25SegReader *r, uint32 local_docid, uint32 field_id)
{
    return seg_doclen_field_cur(r->index, r->h, &r->norms, local_docid, field_id);
}

/* Whole-doc length (sum over fields). For a single-field segment this reads the one
 * NORMS cell (field 0); for a multi-field segment it sums every field's cell, in
 * uint32 arithmetic. ONE body for both entry points below, so the one-shot form and
 * the reader form cannot drift apart in what they sum -- sql/110 compares them cell
 * for cell, and that comparison is only meaningful while the summation is shared and
 * the cursors are the only difference.
 *
 * The field_count cells of one docid are CONTIGUOUS, so whichever cursor is passed
 * makes this one page visit for the whole row instead of field_count walks. */
static uint32
seg_doclen_cur(Relation index, BM25SegmentHeader *h, BM25ChainCursor *cur,
               uint32 local_docid)
{
    uint32  f;
    uint32  sum = 0;

    for (f = 0; f < h->field_count; f++)
        sum += seg_doclen_field_cur(index, h, cur, local_docid, f);
    return sum;
}

/* One-shot: a fresh cursor per call, so every call walks NORMS from the root and pays
 * one extent capture. No production caller remains (issue #225 moved the last one,
 * bm25_debug_postings, onto a reader); it is kept as the root-walk reference the
 * reader form is checked against. */
uint32
bm25_seg_doclen(Relation index, BM25SegmentHeader *h, uint32 local_docid)
{
    BM25ChainCursor cur;

    /* Same field init as bm25_seg_reader_init, for the one local cursor this function
     * keeps across its field loop; the extent capture is what keeps the SEGREAD-11
     * bound off the per-field path. */
    bm25_chain_cursor_init(&cur, h->norms_root, RelationGetNumberOfBlocks(index));
    return seg_doclen_cur(index, h, &cur, local_docid);
}

/* Reader form of bm25_seg_doclen: the reader's NORMS cursor carries across calls, so a
 * caller walking docids forward resumes where the previous row ended instead of
 * re-walking from the root, and the extent was captured once at bm25_seg_reader_init.
 * Shares the NORMS cursor with bm25_seg_reader_doclen_field, which is sound -- both
 * read the same chain, and the cursor's root test covers any other use. */
uint32
bm25_seg_reader_doclen(BM25SegReader *r, uint32 local_docid)
{
    return seg_doclen_cur(r->index, r->h, &r->norms, local_docid);
}

/* bm25_debug_chain_cursor_crosstalk(index regclass, seg int, docid bigint) RETURNS tid
 *   -- SEGREAD-14 (issue #154) CALL-SITE probe.
 *
 * Deliberate misuse: ONE BM25ChainCursor is positioned on the segment's NORMS chain
 * and then handed to the DOCMAP read for the same docid. No production caller does
 * this -- bm25_seg_reader_init gives each chain its own cursor and every caller
 * re-inits per segment -- so without a probe the `cur->root == root` clause has no
 * test at all, and the two comments claiming a cursor "cannot affect the answer"
 * would still be assertions about the call sites rather than about the code.
 *
 * Why the NORMS->DOCMAP pairing specifically. The resume test is `cur->seen <= off`,
 * and NORMS cells are 4 bytes against DOCMAP's 6, so the NORMS cursor's byte origin is
 * always at or below the DOCMAP offset for the same docid -- the pre-fix test passes
 * for EVERY docid, including 0, rather than only for a lucky one. What it then resumes
 * on is a NORMS page being read as DOCMAP, which bm25_seg_page_validate_kind rejects;
 * that loud rejection is the pre-fix behaviour this probe records. A same-kind pairing
 * (another segment's NORMS) would instead have returned a plausible wrong number, but
 * it needs two segments and a cursor carried between them, which is a longer setup for
 * a weaker signal.
 *
 * Returns the TID read through the crosstalked cursor, so the suite can compare it
 * against the row's real ctid rather than against another reading of the same bytes.
 * TEST-ONLY and read-only; no page is written. */
PG_FUNCTION_INFO_V1(bm25_debug_chain_cursor_crosstalk);
Datum
bm25_debug_chain_cursor_crosstalk(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               seg   = PG_GETARG_INT32(1);
    int64               docid = PG_GETARG_INT64(2);
    Relation            index = bm25_index_open_readable(relid, AccessShareLock);
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;
    BM25ChainCursor     cur;
    ItemPointer         out;

    bm25_segcat_read(index, &segs, &nsegs);
    if (seg < 0 || (uint32) seg >= nsegs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    }
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);
    if (docid < 0 || (uint64) docid >= h.ndocs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_chain_cursor_crosstalk: docid " INT64_FORMAT
                        " out of range for segment %d (" UINT64_FORMAT " documents)",
                        docid, seg, h.ndocs)));
    }

    /* nblocks left 0 on purpose: it makes chain_read_at capture the extent per call,
     * which is the arm the cursor-less wrappers also take, so this probe does not
     * quietly depend on the reader-init capture. */
    bm25_chain_cursor_init(&cur, InvalidBlockNumber, 0);

    (void) seg_doclen_field_cur(index, &h, &cur, (uint32) docid, 0);   /* NORMS */

    out = (ItemPointer) palloc(sizeof(ItemPointerData));
    *out = seg_docid_to_tid_cur(index, &h, &cur, (uint32) docid);      /* DOCMAP */

    index_close(index, AccessShareLock);
    PG_RETURN_ITEMPOINTER(out);
}
