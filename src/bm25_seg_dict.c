/* bm25_seg_dict.c -- the sealed-segment DICT reader: one term's lookup, the
 * forward DICT iterator the merge feed reads, and wildcard expansion.
 *
 * Split out of bm25_seg_read.c (#228, ADR 0101) as a pure code move; nothing here
 * changed behaviour. What lives here:
 *   - bm25_seg_dict_lookup: a term's (post_root, post_off, df) in one segment, by a
 *     linear walk of the sorted DICT chain with a bm25_term_cmp early-out;
 *   - the BM25DictIter forward iterator (bm25_seg_dict_iter_*), which hands the
 *     merge feed every entry in stored order;
 *   - bm25_dict_expand_wildcard and the private dedup table it fills
 *     (BM25WildKey/BM25WildEnt/BM25WildAccum), over every segment's DICT chain and
 *     the pending list;
 *   - bm25_dictentry_validate, the decode-boundary check every DICT walker runs,
 *     with its debug probe beside it.
 *
 * The page, block-number and extent validators these walkers call are in
 * bm25_seg_read.c. Decoding the postings a lookup points at is
 * bm25_seg_scan_postings, in bm25_seg_chain.c.
 */
#include "postgres.h"

#include "bm25.h"
#include "bm25_query.h"         /* bm25_glob_match (wildcard dict-entry matcher) */
#include "common/hashfn.h"      /* hash_bytes -- dedup HTAB over variable-length term keys */
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "storage/bufmgr.h"

/* Compare two byte strings lexicographically, shorter-is-smaller on equal prefix.
 * Same ordering as accum_term_cmp (bm25_accum.c), the qsort comparator the builder
 * sorts the dict with; that one is tied to its struct layout, so this raw-bytes
 * variant is separate. Exported (issue #303) for the DICT order checks below and the
 * debug dumps that run them. */
int
bm25_term_cmp(const char *a, int alen, const char *b, int blen)
{
    int n = Min(alen, blen);
    int c = memcmp(a, b, n);

    if (c != 0)
        return c;
    return alen - blen;
}

/* ---- DICT chain order (issue #303) ----
 *
 * What ends a cycle through DICT pages, and what catches an unsorted page boundary.
 * The builder writes every term of a segment exactly once, in bm25_term_cmp order,
 * whole entries only, and opens a DICT page only for an entry about to be written
 * (bm25_seg_build.c: the DICT pass and chain_ensure). So on every well-formed chain:
 *   - every page holds at least one entry;
 *   - a page's first term sorts strictly after the previous page's last term;
 *   - within a page, every term sorts strictly after the one before;
 *   - the chain holds exactly the segment header's nterms entries (df == 0 sentinel
 *     entries included: the builder writes one per accumulated term, and stamps
 *     h->nterms with the same count).
 * A cycle through DICT pages revisits a page whose first term is at or below a term the
 * walk has already passed, so the cross-page rule stops it at its first revisit -- the
 * one-entry self-loop included, which strictness catches. An empty page has no first
 * term to compare, so the first rule is what keeps a cycle through empty pages from
 * running to the walker's cap. Before these rules a lookup for a term sorting after
 * every entry on a cycle spun until cancelled, the merge feed re-fed the cycle's terms
 * into a new segment, and a page whose first term sorted before its predecessor's last
 * made the lookup's early stop report a present term absent.
 *
 * Who checks what: every DICT walker checks the per-page rules. The merge iterator and
 * the dumps also check within-page order and the count; the lookup and the wildcard
 * expander do not, because they are a linear scan over every entry before the key and a
 * second compare per entry would double it (a residual, see BM25SegWalk). Since every
 * merge runs the full check, an unsorted page is never laundered into a new segment. */
void
bm25_dict_order_init(BM25DictOrder *o)
{
    o->lastlen = -1;
    o->nentries = 0;
}

/* [cur, end) is the page's entry span. Validates the first entry's bounds before its
 * term is read; the caller's loop validates it again, which is one compare. */
void
bm25_dict_page_first(const BM25DictOrder *o, const char *cur, const char *end,
                     BlockNumber blk)
{
    const BM25DictEntry *e = (const BM25DictEntry *) cur;

    if (cur >= end)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment DICT page at block %u holds no entries", blk),
                 errdetail("The segment builder writes at least one entry on every "
                           "dictionary page."),
                 errhint("REINDEX the index.")));
    (void) bm25_dictentry_validate(cur, end);
    if (o->lastlen >= 0 &&
        bm25_term_cmp(cur + sizeof(BM25DictEntry), e->termlen, o->last, o->lastlen) <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment DICT page at block %u does not start after the "
                        "previous page's last term", blk),
                 errdetail("A dictionary chain is sorted across its pages; this link is "
                           "out of order or leads back into the chain."),
                 errhint("REINDEX the index.")));
}

/* Called at a page's end with its last term. One memcpy per page; the term is bounded
 * by its page (bm25_dictentry_validate), and so by `last`. */
void
bm25_dict_page_last(BM25DictOrder *o, const char *term, int termlen)
{
    Assert(termlen >= 0 && termlen <= BLCKSZ);  /* checked: bm25_dictentry_validate */
    memcpy(o->last, term, termlen);
    o->lastlen = termlen;
}

void
bm25_dict_entry_order_validate(const char *prev, int prevlen, const char *term,
                               int termlen, BlockNumber blk)
{
    if (bm25_term_cmp(term, termlen, prev, prevlen) <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment DICT terms out of order on block %u", blk),
                 errhint("REINDEX the index.")));
}

void
bm25_dict_count_validate(uint64 nentries, uint32 nterms)
{
    if (nentries != nterms)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: segment DICT chain holds " UINT64_FORMAT " entries, but the "
                        "segment header counts %u", nentries, nterms),
                 errhint("REINDEX the index.")));
}

/*
 * bm25_dictentry_validate -- the decode boundary for a DICT entry.
 *
 * Every dict walker (the sorted-chain lookup, the merge/debug iterator, and the
 * bare bm25_debug_segterms dump) memcpy's/derefs a BM25DictEntry straight off a
 * shared-buffer page and then uses its termlen -- a raw on-page uint16 -- as a
 * memcmp/text length AND as the stride to the next entry. bm25_seg_page_validate
 * does not cover this (seg_gen only), and a corrupt termlen otherwise carries
 * bm25_term_cmp / cstring_to_text_with_len past the page and then strides the
 * cursor off into the void on the next iteration. Bounding both the fixed
 * header and the MAXALIGN'd term span here, in the one place all three walkers
 * now share, closes all of them at once. Returns the entry's total on-page
 * length (header + MAXALIGN'd term) so the caller can advance without
 * recomputing it.
 *
 * SPAN ONLY, deliberately: this validates where the entry ENDS, never what its
 * pointer fields mean. post_root/pos_post_root are checked where they are followed
 * (bm25_seg_scan_postings' entry guard and wand_cursor_load_block, both via
 * bm25_seg_blkno_validate), because only a follower knows whether InvalidBlockNumber
 * is corruption or the builder's legitimate "no chain here" sentinel -- it is the
 * latter for a df == 0 term and for a term with no positions. Said here because the
 * gap read as an oversight once already: a df > 0 term with post_root Invalid used to
 * walk zero postings in silence, and the reason was looked for in this function.
 */
Size
bm25_dictentry_validate(const char *cur, const char *end)
{
    const BM25DictEntry *e;
    Size                  entry_len;

    if (cur + sizeof(BM25DictEntry) > end)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: dictionary entry header overruns the page")));

    e = (const BM25DictEntry *) cur;
    entry_len = MAXALIGN(sizeof(BM25DictEntry) + e->termlen);
    if (cur + entry_len > end)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: dictionary entry term of length %u overruns the page",
                        e->termlen)));
    return entry_len;
}

/* Decode-boundary probe (trust-boundary review, 2026-08). bm25_dictentry_validate
 * only fires on an already-corrupt DICT page, which a regression suite cannot
 * produce, so this runs the SAME validator over a caller-chosen termlen/avail
 * pair. `termlen` fills a REAL BM25DictEntry on the C stack (a struct assignment,
 * not a raw bytea, so the suite stays host-endian/padding independent -- same
 * reasoning as bm25_debug_block_validate's header-by-fields approach); `avail`
 * stands in for (end - cur), the notional page bytes left from this entry
 * onward. &hdr itself is the "page position" -- only the pointer arithmetic is
 * examined, exactly like bm25_debug_block_validate's `&base`. TEST-ONLY: a pure
 * function of its scalar arguments, no relation touched, covered by the install
 * script's REVOKE loop like every other bm25_debug_* function. */
PG_FUNCTION_INFO_V1(bm25_debug_dictentry_validate);
Datum
bm25_debug_dictentry_validate(PG_FUNCTION_ARGS)
{
    int32          termlen = PG_GETARG_INT32(0);
    int32          avail   = PG_GETARG_INT32(1);
    BM25DictEntry  hdr;

    if (termlen < 0 || termlen > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_dictentry_validate: termlen out of uint16 range")));

    memset(&hdr, 0, sizeof(hdr));
    hdr.termlen = (uint16) termlen;

    PG_RETURN_INT64((int64) bm25_dictentry_validate((char *) &hdr, (char *) &hdr + avail));
}

/* Look up a term within one segment's sorted DICT chain. Entries are globally
 * sorted by term across the whole chain, so the search is monotone: within a page
 * we scan entries in order and stop as soon as an entry's term exceeds the key
 * (c > 0) -- the key cannot appear later on this page nor on any later page (the
 * chain is sorted), so we return false immediately. If we exhaust a page without
 * passing the key we move to the next page. (Variable-length MAXALIGN-strided
 * entries preclude a binary search within a page, and no page carries a min/max
 * key, so nothing here skips a page: this is a LINEAR scan of every entry sorting
 * before the key -- O(dict) worst case, the same ceiling bm25_dict_expand_wildcard
 * documents below for its own unseeked walk -- and the c > 0 early-out only bounds
 * the tail. Upgrade path if this ever dominates: a per-page first-key header, to
 * make page-granularity skipping possible at all.)
 * Returns the term's postings location + df. */
bool
bm25_seg_dict_lookup(Relation index, BM25SegmentHeader *h, const char *term,
                     int termlen, BlockNumber *post_root, uint16 *post_off, uint32 *df,
                     BlockNumber *pos_post_root, uint16 *pos_post_off)
{
    BlockNumber     blk = h->dict_root;
    BM25SegWalk     w;
    BM25DictOrder   order;

    /* SEGREAD-11 (issue #154): one lseek per (term, segment) lookup, taken here
     * exactly like bm25_dict_expand_wildcard's. This walk is per TERM, not per
     * posting, so the cost 0071 refused for bm25_seg_blkno_validate does not arise --
     * see bm25_seg_chain_extent_validate. The walker errors rather than ending the
     * walk at a bad link, because stopping would return "term absent from this
     * segment" for a term that may well be present -- a silently smaller result set. */
    bm25_seg_walk_init(&w, index, RelationGetNumberOfBlocks(index), h->gen,
                       BM25_PAGE_DICT, false, "segment DICT chain");
    bm25_dict_order_init(&order);

    while (blk != InvalidBlockNumber)
    {
        Buffer  buf;
        Page    pg;
        char   *cur, *end;
        BlockNumber next;
        Size    pagebytes;
        const BM25DictEntry *last = NULL;

        CHECK_FOR_INTERRUPTS();

        /* Block 0, extent, revisit, cap, then -- under the lock -- the content bound,
         * gen and kind (BM25SegWalk). The content bound matters here in particular: a
         * bare pd_lower read below SizeOfPageHeaderData would read as "page has zero
         * entries" and move on, so a term that only exists on the corrupt page would
         * come back "not found" instead of erroring. */
        buf = bm25_seg_walk_read(&w, blk, &pagebytes);
        pg = BufferGetPage(buf);
        cur = (char *) PageGetContents(pg);
        end = cur + pagebytes;
        next = BM25PageGetOpaque(pg)->nextblk;
        /* A page with entries, starting after the previous page's last term (issue
         * #303): what ends a cycle here, and what keeps an unsorted page boundary from
         * turning the c > 0 early stop below into "absent" for a present term. */
        bm25_dict_page_first(&order, cur, end, blk);
        while (cur < end)
        {
            BM25DictEntry *e = (BM25DictEntry *) cur;
            const char    *eterm = cur + sizeof(BM25DictEntry);
            Size           entry_len = bm25_dictentry_validate(cur, end);
            int            c = bm25_term_cmp(eterm, e->termlen, term, termlen);

            if (c == 0)
            {
                *post_root = e->post_root;
                *post_off = e->post_off;
                *df = e->df;
                /* M4: hand back the term's POS-chain entry (Invalid for a pre-M4
                 * segment or a term only in positions-off fields). Nullable. */
                if (pos_post_root != NULL)
                    *pos_post_root = e->pos_post_root;
                if (pos_post_off != NULL)
                    *pos_post_off = e->pos_post_off;
                UnlockReleaseBuffer(buf);
                return true;
            }
            if (c > 0)
            {
                /* passed the key on this sorted page: term is absent everywhere */
                UnlockReleaseBuffer(buf);
                return false;   /* sorted chain: cannot appear on a later page */
            }
            last = e;
            cur += entry_len;
        }
        /* The whole page sorted before the key: keep its last term for the next
         * page's order check. bm25_dict_page_first guaranteed an entry. */
        Assert(last != NULL);   /* checked: bm25_dict_page_first */
        bm25_dict_page_last(&order, (const char *) last + sizeof(BM25DictEntry),
                            last->termlen);
        UnlockReleaseBuffer(buf);
        blk = next;
    }
    return false;
}

/* ---- DICT chain iterator (Phase-4 merge feed, Task 23) ----
 *
 * A forward cursor over one segment's sorted DICT chain, yielding every entry's
 * (term, termlen, post_root, post_off, df) in stored order. It generalizes the
 * page-walk in bm25_seg_dict_lookup: instead of comparing against a search key it
 * simply hands back each entry.
 *
 * Correction (adversarial review, 2026-08): this used to say the source segment
 * "has already been swapped out of the live catalog by the time the merge reads
 * it" -- false. bm25_accum_from_segments (bm25_merge.c), the only production
 * caller, reads its source segments' DICT chains to BUILD the merged
 * accumulator; the atomic catalog swap that retires them happens strictly
 * afterward, in bm25_segcat_publish_swap. The source segment is live,
 * in the catalog, and readable by ordinary scans for the entire time this
 * iterator walks it. What actually keeps its DICT pages immutable and safe from
 * concurrent reuse is that the merge caller holds the metapage singleton
 * (LockPage(BM25_METAPAGE_BLKNO, ExclusiveLock)) for the whole accumulate+swap
 * pass, which blocks the only thing that could ever free/reuse them -- Phase-3/4
 * orphan/retired reclaim, which needs the same singleton before it can act. This
 * is the identical argument bm25_seg_dict_iter_unlock/_relock (below) rest on for
 * dropping the lock mid-term without losing that safety.
 *
 * A SHARE lock on the current page is held between begin/next/end of that page
 * UNLESS the caller explicitly drops it via bm25_seg_dict_iter_unlock (the merge
 * loop does, around each term's postings replay) -- the term pointer returned by
 * _next() points into the buffer's page copy and is therefore valid only until
 * the next _next() call OR an _unlock call, whichever comes first. The merge
 * loop copies term/termlen into its own buffer before calling _unlock for
 * exactly this reason; any other caller needing to retain a term past either
 * event must do the same.
 *
 * The gen: its ONE production caller -- the merge feed, bm25_accum_from_segments in
 * bm25_merge.c -- holds the metapage singleton for the whole iteration, which blocks
 * the only thing that could free and reuse this segment's pages (Phase-3/4
 * orphan/retired reclaim), so gen validation is no concurrency guard here, and this
 * iterator ran with expected_gen 0 until issue #303. It now passes the segment's real
 * gen, as a corruption backstop: a link into another segment's DICT page is the right
 * kind, and under the singleton a mismatch is XX002 outright (bm25_seg_gen_mismatch).
 * The debug probes that also drive it hold no singleton, and get the catalog check.
 *
 * Earlier wordings of this paragraph named two further caller groups and were
 * wrong about both (corrected in issue #145). Three debug SRFs -- bm25_debug_terms
 * / bm25_debug_postings (bm25_segment.c) and bm25_debug_segterms
 * (bm25_seg_debug.c) -- have never used this iterator; they open-code their own
 * DICT page walk. (Two other probes in bm25_seg_debug.c do drive it, the
 * dict_iter case of bm25_debug_seg_chain_extent and bm25_debug_seg_postings_count,
 * which is the "debug probes" sentence above.) And
 * the wildcard expander, which did use it, was moved off it precisely because
 * "holds an ordinary MVCC snapshot" is not a sound substitute for the singleton
 * on a hot standby without feedback (see bm25_seg_dict_iter_unlock's header).
 * All four of those walks copy each DICT page under its SHARE lock and decode the
 * copy, which lets them carry the segment's REAL gen. The iterator carries it too
 * since issue #303, but only re-checks it at page crossings, not after a relock, so
 * do not "consolidate" a reader without the singleton onto it.
 *
 * The OTHER backstop this paragraph used to claim the iterator lacked -- the
 * `blk < nblocks` extent bound -- it now has (SEGREAD-11, issue #154): nblocks is
 * captured once in _begin, pos_cursor_open style, and every page is checked against
 * it in _next before ReadBuffer. It errors rather than stopping quietly, because
 * stopping quietly here means the merge feed reads a short dictionary and writes a
 * permanently incomplete segment. */
typedef struct BM25DictIter
{
    BM25SegWalk     walk;           /* block 0, extent, revisit, cap, content, kind */
    BlockNumber     blk;            /* current page, or Invalid when exhausted */
    Buffer          buf;            /* SHARE-locked current page, or InvalidBuffer */
    char           *cur;            /* cursor into the current page's contents */
    char           *end;            /* pd_lower boundary of the current page */
    /* Issue #303: the full order check the merge feed needs (see "DICT chain order"
     * above). prev is the entry returned last, on the current page; the previous
     * page's last term is in order.last. */
    const char     *prev;
    int             prevlen;
    uint32          nterms;         /* the segment header's count, checked at the end */
    BM25DictOrder   order;
} BM25DictIter;

void *
bm25_seg_dict_iter_begin(Relation index, BM25SegmentHeader *h)
{
    BM25DictIter *it = (BM25DictIter *) palloc0(sizeof(BM25DictIter));

    /* One lseek per iterator, not per page -- pos_cursor_open's shape, which is the
     * one ADR 0071 names for a walk that wants an extent bound without putting a
     * syscall in its loop. The real gen (issue #303): see the type comment above. */
    bm25_seg_walk_init(&it->walk, index, RelationGetNumberOfBlocks(index), h->gen,
                       BM25_PAGE_DICT, false, "segment DICT chain");
    it->blk = h->dict_root;
    it->buf = InvalidBuffer;
    it->cur = NULL;
    it->end = NULL;
    it->prev = NULL;
    it->prevlen = 0;
    it->nterms = h->nterms;
    bm25_dict_order_init(&it->order);
    return it;
}

bool
bm25_seg_dict_iter_next(void *iter, char **term, int *termlen,
                        BlockNumber *post_root, uint16 *post_off, uint32 *df,
                        BlockNumber *pos_post_root, uint16 *pos_post_off)
{
    BM25DictIter   *it = (BM25DictIter *) iter;

    for (;;)
    {
        /* Need a fresh page? Release the previous one and SHARE-lock the next. */
        if (it->buf == InvalidBuffer)
        {
            Page    pg;
            Size    pagebytes;

            if (it->blk == InvalidBlockNumber)
            {
                /* Chain exhausted. A merge feed that read fewer or more entries than
                 * the segment counts would write a short or doubled dictionary into a
                 * new segment (issue #303). Checked on every call that reports the end,
                 * which is once per drained iterator. */
                bm25_dict_count_validate(it->order.nentries, it->nterms);
                return false;
            }

            /* Maintenance-interrupts pass: this was, at the time, the ONE
             * genuinely lock-free instant in this iterator's whole lifetime --
             * the previous page (if any) was already released in the "page
             * drained" branch below before looping back here, and the next
             * page's lock has not been acquired yet. A check anywhere else in
             * this function, or (at the time) in a caller's per-entry loop
             * around bm25_seg_dict_iter_next, would have been dead: this
             * iterator holds it->buf SHARE-locked across every entry returned
             * from one page (LWLockAcquire calls HOLD_INTERRUPTS, so
             * CHECK_FOR_INTERRUPTS is a silent no-op the whole time a buffer
             * content lock is held). Placed here, it gives every caller a
             * per-DICT-page floor on cancellability.
             *
             * Correction (fix 5, adversarial review, 2026-08): the merge feed
             * (bm25_accum_from_segments, bm25_merge.c) is no longer covered
             * ONLY here -- it now calls bm25_seg_dict_iter_unlock before each
             * term's postings replay (the fix for this iterator's DICT lock
             * nesting under bm25_seg_scan_postings' own POST/POS locks), which
             * created a SECOND genuinely lock-free instant this iterator did
             * not have before, and the merge loop places its own
             * CHECK_FOR_INTERRUPTS there -- see its call site. That makes a
             * large merge cancellable per-term instead of only per-DICT-page,
             * directly serving ADR 0041's "loud/cancellable beats silently
             * uncancellable" goal for this specific loop.
             *
             * Correction (XCUT-03, issue #145): an older wording of this comment
             * said the check here "still covers wildcard dict expansion and the
             * debug SRFs". NEITHER was true, and neither is a caller now. The
             * debug SRFs (bm25_debug_terms/bm25_debug_postings in bm25_segment.c,
             * and bm25_debug_segterms in bm25_seg_debug.c) open-code their own
             * DICT page walk and never went through this iterator at all; the
             * wildcard expander did, and was moved off it onto a per-page copy (see
             * bm25_seg_dict_iter_unlock's header for why the unlock/relock pair
             * is unsound for a reader on a standby). All four now copy each page
             * and check per ENTRY on the copy. This check is therefore the merge
             * feed's per-page floor, underneath the per-term one that
             * bm25_seg_dict_iter_unlock opens at its call site. */
            CHECK_FOR_INTERRUPTS();

            /* The walker (issue #303), in the same lock-free instant as the check
             * above: block 0, the extent bound (SEGREAD-11), the revisit test and the
             * cap before ReadBuffer, then the content bound and the page kind under
             * the lock. All LOUD, because this iterator's one production caller is
             * the merge feed: terminating the chain quietly here would make _next
             * return false as if the dictionary ended, and merge would write a new
             * segment missing every term from the bad link onward.
             *
             * H6: the page-kind half was this walker's only page validation once,
             * when it carried no gen (see the type comment above). A corrupt
             * dict_root or a stray nextblk would otherwise be decoded as BM25DictEntry
             * records with nothing but bm25_dictentry_validate's memory-safety bound
             * in the way -- and this iterator feeds the MERGE, so the product of a bad
             * decode is a newly written, permanently wrong segment. The content bound
             * is recomputed on EVERY fresh page for the same reason: a corrupt pd_lower
             * read the bare way made the page look empty, and _next returned false as
             * if the chain ended there. */
            it->buf = bm25_seg_walk_read(&it->walk, it->blk, &pagebytes);
            pg = BufferGetPage(it->buf);
            it->cur = (char *) PageGetContents(pg);
            it->end = it->cur + pagebytes;
            /* A page with entries, the first after the previous page's last term. */
            bm25_dict_page_first(&it->order, it->cur, it->end, it->blk);
            it->prev = NULL;
        }

        if (it->cur < it->end)
        {
            BM25DictEntry *e = (BM25DictEntry *) it->cur;
            Size           entry_len = bm25_dictentry_validate(it->cur, it->end);

            /* Within-page order, one compare per entry; the first entry was checked
             * against the previous page by bm25_dict_page_first. */
            if (it->prev != NULL)
                bm25_dict_entry_order_validate(it->prev, it->prevlen,
                                               it->cur + sizeof(BM25DictEntry),
                                               e->termlen, it->blk);
            it->prev = it->cur + sizeof(BM25DictEntry);
            it->prevlen = e->termlen;
            it->order.nentries++;

            *term     = it->cur + sizeof(BM25DictEntry);
            *termlen  = e->termlen;
            *post_root = e->post_root;
            *post_off = e->post_off;
            *df       = e->df;
            /* M4: the term's POS-chain frame start; nullable so bag-of-words iter
             * callers (bm25_debug_segterms) need not thread them. On a pre-M4
             * segment these are the Invalid/0 sentinels the builder stamped. */
            if (pos_post_root != NULL)
                *pos_post_root = e->pos_post_root;
            if (pos_post_off != NULL)
                *pos_post_off = e->pos_post_off;
            it->cur += entry_len;
            return true;
        }

        /* Page drained: keep its last term for the next page's order check, then
         * advance, releasing this buffer first. */
        {
            Page    pg = BufferGetPage(it->buf);
            BlockNumber next = BM25PageGetOpaque(pg)->nextblk;

            Assert(it->prev != NULL);   /* checked: bm25_dict_page_first */
            bm25_dict_page_last(&it->order, it->prev, it->prevlen);
            UnlockReleaseBuffer(it->buf);
            it->buf = InvalidBuffer;
            it->blk = next;
        }
    }
}

/*
 * bm25_seg_dict_iter_unlock / bm25_seg_dict_iter_relock -- MINIMAL fix (2026-08,
 * crash/replica-safety pass) for a nested-lock shape in the merge feed
 * (bm25_accum_from_segments, bm25_merge.c): bm25_seg_dict_iter_next holds its
 * current DICT page SHARE-locked across every entry returned from that page,
 * and the merge loop calls bm25_seg_scan_postings once per entry, which itself
 * acquires POST/POS page SHARE locks and holds THEM across its own callback
 * (which allocates in the destination accumulator) -- so the DICT page stayed
 * locked across an entire POST/POS chain replay, for every term on that page,
 * the whole time the callback underneath was free to palloc/hash_search. A
 * full fix (never holding a content lock across a NESTED lock acquisition
 * anywhere in the reader) would restructure bm25_seg_scan_postings' own
 * contract across three files and is OUT OF SCOPE here (see the merge loop's
 * call site for the full reasoning). This pair lets the merge loop drop just
 * the OUTER (DICT) lock around the inner POST/POS replay and re-take it after,
 * on the SAME pinned buffer -- the buffer is only unlocked, never released, so
 * it cannot be evicted or reused, and a live segment's DICT pages are never
 * modified in place (the merge caller holds the metapage singleton for its
 * whole duration, which blocks the one thing -- Phase-3 orphan/retired reclaim
 * -- that could ever free/reuse this segment's pages), so the content is
 * unchanged when re-locked and it->cur/it->end need no re-derivation.
 *
 * PRECONDITION -- THE METAPAGE SINGLETON, AND NOTHING WEAKER (XCUT-03, issue
 * #145). This pair is safe only for a caller holding
 * LockPage(BM25_METAPAGE_BLKNO, ExclusiveLock) for the whole iteration, which
 * today means the merge feed and only the merge feed. Resuming after _relock does
 * NO revalidation of any kind -- not the gen check and not the page-kind check it
 * runs at page crossings --
 * so it->cur/it->end are replayed against whatever bytes the page now holds.
 *
 * DO NOT substitute "the caller holds an ordinary MVCC snapshot including the
 * segment". A draft of #145 added the wildcard expander as a second caller on
 * exactly that reasoning and it was rejected in review: the snapshot argument
 * rests on GlobalVisCheckRemovableFullXid, which is evaluated on the PRIMARY, so
 * on a hot standby WITHOUT hot_standby_feedback it restrains nothing. Generic WAL
 * redo takes an ordinary exclusive content lock and carries no recovery-conflict
 * information, so the startup process can replay retire->stamp->reuse over this
 * pinned page inside the unlocked window and the resumed cursor would decode
 * rewritten bytes as dict entries. Memory-safe, silently wrong. The expander now
 * copies each DICT page instead (see bm25_dict_expand_wildcard below), which is
 * the pattern any new caller should reach for first.
 *
 * The singleton does hold on a standby too, vacuously: nothing replays a reuse
 * that the primary could not have performed, and the primary cannot have
 * performed one while a merge holds the singleton. Immutability of a LIVE
 * segment's pages is separately unconditional -- sealed pages are never rewritten
 * in place and retire itself resets no links (D-RETIRE/M7) -- so under the
 * singleton the content is unchanged across unlock/relock and the cursor still
 * points where it did.
 *
 * This does NOT make it safe to read *term (returned by the preceding
 * bm25_seg_dict_iter_next call) while unlocked: that pointer aims directly
 * into this page's buffer, and touching page bytes without the content lock
 * breaks the "always read under a lock" discipline every other reader in this
 * file follows, argument-for-this-one-case-being-safe or not. The caller MUST
 * copy term/termlen into its own buffer before calling _unlock.
 */
void
bm25_seg_dict_iter_unlock(void *iter)
{
    BM25DictIter *it = (BM25DictIter *) iter;

    if (it->buf != InvalidBuffer)
        LockBuffer(it->buf, BUFFER_LOCK_UNLOCK);
}

void
bm25_seg_dict_iter_relock(void *iter)
{
    BM25DictIter *it = (BM25DictIter *) iter;

    if (it->buf != InvalidBuffer)
        LockBuffer(it->buf, BUFFER_LOCK_SHARE);
}

void
bm25_seg_dict_iter_end(void *iter)
{
    BM25DictIter   *it = (BM25DictIter *) iter;

    if (it->buf != InvalidBuffer)
        UnlockReleaseBuffer(it->buf);
    pfree(it);
}

/* ---- M6 Task 7: wildcard expansion (bm25_dict_expand_wildcard) ----
 *
 * Dedup set over variable-length term bytes. dynahash keys are fixed-size, so a
 * term of arbitrary length is keyed by a (ptr,len) pair whose bytes live in the
 * caller's context (stable for the HTAB's lifetime); custom hash/match/keycopy
 * callbacks hash and compare the pointed-to bytes, not the pointer. This dedups
 * by term CONTENT with no arbitrary length cap (a fixed inline key buffer would
 * have one, and truncating it would false-merge two distinct long terms and drop
 * a real match). The HTAB stores membership only (key, no payload). */
typedef struct BM25WildKey
{
    const char *ptr;            /* into a cxt-owned copy of the term bytes */
    int         len;
} BM25WildKey;

typedef struct BM25WildEnt
{
    BM25WildKey key;
} BM25WildEnt;

static uint32
bm25_wildkey_hash(const void *key, Size keysize)
{
    const BM25WildKey *k = (const BM25WildKey *) key;

    return hash_bytes((const unsigned char *) k->ptr, k->len);
}

static int
bm25_wildkey_match(const void *a, const void *b, Size keysize)
{
    const BM25WildKey *ka = (const BM25WildKey *) a;
    const BM25WildKey *kb = (const BM25WildKey *) b;

    if (ka->len != kb->len)
        return 1;               /* unequal length => not equal (nonzero) */
    return memcmp(ka->ptr, kb->ptr, ka->len);
}

static void *
bm25_wildkey_copy(void *dst, const void *src, Size keysize)
{
    /* Copy the (ptr,len) struct, NOT the bytes: the bytes are cxt-owned and stay
     * valid for the HTAB's life, so the stored pointer keeps pointing at them. */
    memcpy(dst, src, sizeof(BM25WildKey));
    return (char *) dst + sizeof(BM25WildKey);
}

/* Accumulator threaded through the segment + pending passes: the dedup set, the
 * growable output token array (all copies palloc'd in cxt), and the running
 * distinct count for the cap check. */
typedef struct BM25WildAccum
{
    HTAB          *seen;
    MemoryContext  cxt;
    BM25Token     *out;
    int            n;
    int            capacity;
    const char    *pattern;     /* original (un-folded) pattern + its length, for the cap
                                 * ERROR only -- echoing the caller's own bytes back. Never
                                 * used for matching; the folded pattern has its own length
                                 * (patfoldlen), since the fold can change it. */
    int            patlen;
} BM25WildAccum;

/* Add one matched term to the distinct set. Copies the bytes into cxt FIRST (both
 * callers hand it a pointer into a caller-owned scratch copy -- a termbuf on the
 * segment side, a PGAlignedBlock page image on the pending side -- neither of
 * which outlives the loop iteration), then HASH_ENTERs keyed on the stable copy.
 *
 * CALLER PRECONDITION (XCUT-03, issue #145): NO buffer content lock may be held
 * across this call. It does a MemoryContextAlloc that can OOM-ERROR and a
 * hash_search(HASH_ENTER) that can trigger an unbounded dynahash expansion, and
 * it can ereport the max_expansions cap -- and it runs once per candidate term,
 * so under a lock it turns an interruptible scan into an uncancellable one
 * (LWLockAcquire holds interrupts off) and stalls every writer needing the page.
 * Both call sites therefore copy out and unlock first. A term
 * already present (another segment or pending) is a no-op -- the ONE-distinct-set
 * invariant: a term in N sources must be scored ONCE under the leaf bit, else it
 * double-counts. The cap is enforced HERE, during accumulation (tighter than the
 * plan's "check after the full union is materialized" ceiling -- this bounds the
 * set to max_expansions+1 entries instead of materializing an unbounded union),
 * and it ERRORs, never truncates: a truncated OR set silently drops matches.
 *
 * On a duplicate hit, HASH_ENTER's keycopy callback (bm25_wildkey_copy) does NOT
 * run -- dynahash only invokes keycopy for a newly created entry, so an existing
 * entry's (ptr,len) still points at whatever EARLIER call's cxt allocation first
 * won this term. This call's `copy` is therefore unreferenced by the table on
 * the found path and must be freed here: left alone, every duplicate hit across
 * segments and the pending list strands a term-sized allocation in cxt for the
 * life of the scan. Not a correctness bug (cxt is torn down with the scan either
 * way) but unbounded in proportion to corpus width x segment count, and free to
 * fix since nothing downstream ever sees this call's copy. */
static void
bm25_wild_add(BM25WildAccum *a, const char *term, int termlen)
{
    char        *copy;
    bool         found;
    BM25WildKey  probe;

    copy = MemoryContextAlloc(a->cxt, Max(termlen, 1));
    memcpy(copy, term, termlen);
    probe.ptr = copy;
    probe.len = termlen;
    (void) hash_search(a->seen, &probe, HASH_ENTER, &found);
    if (found)
    {
        pfree(copy);            /* HTAB still references an earlier call's copy; ours is unreferenced */
        return;                 /* already in the set: dedup across segments + pending */
    }

    if (a->n >= bm25_wildcard_max_expansions)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: wildcard \"%.*s\" expands to more than %d terms; "
                        "narrow it or raise bm25_native.wildcard_max_expansions",
                        a->patlen, a->pattern, bm25_wildcard_max_expansions)));

    if (a->n == a->capacity)
    {
        a->capacity *= 2;
        a->out = repalloc(a->out, sizeof(BM25Token) * a->capacity);
    }
    a->out[a->n].ptr     = copy;
    a->out[a->n].len     = termlen;
    a->out[a->n].pos     = 0;    /* inert: the scorer reads only ptr/len */
    a->out[a->n].src_off = -1;
    a->out[a->n].src_len = 0;
    a->n++;
}

/*
 * bm25_dict_expand_wildcard -- expand a WILDCARD leaf's glob PATTERN into the
 * distinct set of dictionary terms it matches, across every live segment plus
 * the pending list, deduplicated into ONE set. Returns the term count; *out_terms
 * is a cxt-palloc'd BM25Token array, each entry's ptr/len pointing at a cxt-owned
 * copy of the term bytes (the rest of BM25Token is inert -- the boolean scorer
 * reads only ptr/len, feeding each term into the SAME per-term OR loop a MATCH
 * leaf's tokens use, all under the wildcard leaf's one presence bit).
 *
 * D8 stemmed-dict semantics (subtle, silent-wrong-prone): the dictionary stores
 * STEMMED, lowercased terms (the index stems at build). The pattern is matched
 * RAW -- ASCII-lowercased but NOT stemmed -- against those stored bytes. So `judg*`
 * matches the stem `judg` (from "judge"), `judgment`, `judgement`, but NOT `judo`;
 * `wom*n` matches `woman`/`women` but not `wombat`. The caller MUST NOT pre-stem
 * the pattern (that is why the wildcard path bypasses bm25_analyze). Which stems
 * a corpus produces is english_stem's business, not ours -- the tests verify the
 * expected set with bm25_debug_tokenize rather than hand-computing it.
 *
 * Field scope is NOT applied here: the dict is not field-partitioned, so we expand
 * over the whole dict; the leaf's field_id gates each returned term at SCORING
 * time (per-field df via the RLE), exactly as a MATCH leaf is scoped.
 *
 * Algorithm: split the (lowercased) pattern at its first '*' into a literal prefix.
 * Per segment (byte-sorted dict, bm25_term_cmp order): skip entries sorting before
 * the prefix, accept while an entry still starts with the prefix (a pure-prefix
 * pattern -- '*' last -- accepts unconditionally; an embedded-glob pattern re-checks
 * bm25_glob_match), and stop once an entry sorts past the prefix range (the sorted
 * chain guarantees no later entry can match). Pending has no sorted dict, so it
 * gets one linear bm25_glob_match pass; its matches dedup against the sealed set.
 *
 * ponytail: known ceiling, acceptable at M6 (bounded by the guardrails). The walk
 * starts at dict_root with NO seek, so the "prefix range" is a LINEAR skip of
 * every dict entry < prefix per segment (O(dict)); the early-stop only bounds the
 * tail. Upgrade path if this ever dominates: binary-search the sorted chain to the
 * first entry >= prefix.
 *
 * Page validation (corrected in issue #145): this walk NOW runs the option-(d)
 * seg_gen check with the segment's real gen, exactly as bm25_seg_dict_lookup
 * does. It previously ran none, because it went through bm25_seg_dict_iter_*,
 * which then passed expected_gen = 0 -- and the note that used to sit here ("a term
 * from a torn read would simply score to df==0 in the validated lookups the
 * scorer runs, so it cannot mis-score, only waste a lookup") was a statement
 * about the harmlessness of a bogus TERM, not a defense of decoding a page that
 * had been reclaimed and reused underneath the walk. With the gen check in place
 * that case raises ERRCODE_T_R_SERIALIZATION_FAILURE, which
 * bm25_scan_build_ranking's bounded subtransaction retry turns into a re-run
 * against a fresh snapshot -- the same treatment every other segment read gets.
 */
int
bm25_dict_expand_wildcard(Relation index, const BM25ScanSnapshot *snap,
                          const char *pattern, int patlen,
                          MemoryContext cxt, BM25Token **out_terms)
{
    char           *pat;
    int             patfoldlen;
    const char     *star;
    int             prefixlen;
    bool            pure_prefix;
    HASHCTL         ctl;
    BM25WildAccum   acc;
    uint32          si;
    BlockNumber     blk;
    /* Per-document state for the PENDING pass; see its header below. `acc_tid` is
     * the tid of the document currently open, so a continuation belonging to SOMEONE
     * ELSE (or to nobody) is dropped rather than expanded. */
    ItemPointerData acc_tid;
    bool            acc_active = false;
    /* XCUT-03: destination of BOTH passes' copy-then-unlock. One buffer is enough
     * because the passes are strictly sequential -- the segment walk is finished
     * before the pending walk starts -- and each page is fully consumed before the
     * next overwrites it. */
    PGAlignedBlock  pagecopy;
    /* Extent bound for the SEALED DICT chain walk below, which ERRORs rather than
     * stopping on a past-the-extent link (see that check for why a query path must
     * be loud where a debug SRF or a sweep may terminate). Derived ONCE here: it is
     * an lseek-class call, and since nothing in this extension ever shrinks the
     * relation it stays a valid lower bound on the extent for the whole call.
     *
     * The PENDING walk does not use this sample. A LIVE pending chain can
     * legitimately grow past a bound read at entry, because a concurrent appender
     * extends the relation and links a new tail while this walk is in progress, so
     * a fixed bound would reject a valid page. Since #291 that walk goes through
     * bm25_pending_walk_read, like every scan-side pending walker, whose extent
     * check re-samples on a would-be violation (bm25_blk_in_extent) instead. */
    BlockNumber     nblocks = RelationGetNumberOfBlocks(index);
    BM25PendingWalk pwalk;      /* #291: the validated pending walk below */
    /* Issue #303: the sealed walk's cross-page order state. palloc'd, not on the stack
     * beside pagecopy: it carries a BLCKSZ term buffer of its own. */
    BM25DictOrder  *order = palloc(sizeof(BM25DictOrder));

    /* Lowercase the pattern into scratch (D8): the dict bytes were lowercased at
     * build, so a raw-cased pattern would miss. bm25_fold_term is the SAME fold the
     * analyzer applies to the terms it writes (ADR 0046); the byte-wise tolower this
     * replaced agreed with the analyzer for ASCII and nowhere else, so an accented
     * prefix could not reach a dictionary entry that had been folded properly.
     *
     * Folding parity is all this buys, and it is worth being precise about the limit:
     * the dictionary holds STEMMED terms and this pattern is not stemmed (the deliberate
     * M6 trade-off documented in ARCHITECTURE), so a stemmer that rewrites the
     * characters a prefix is made of still defeats it -- german folds and transliterates
     * "A-diaeresis rger" to "arg", which no accented prefix matches. That is the
     * wildcard/stemmer trade-off, unchanged here, not a folding defect.
     *
     * The fold is not byte-length preserving, so every length below must be derived
     * from the FOLDED pattern; patlen describes the caller's bytes and is used only to
     * echo the pattern back in errors. */
    pat = bm25_fold_term(pattern, patlen, &patfoldlen);

    /* Re-validate the LENGTH cap against the folded pattern (QRY-06).
     *
     * The comment above says every length below must come from the folded pattern, and
     * min_prefix does -- but the max-LENGTH cap is enforced once, at parse time, on the
     * RAW bytes, and never again. bm25_glob_match then runs on `pat`/`patfoldlen`, so a
     * fold that lengthens its input lets the matcher exceed a guardrail whose whole
     * purpose is to bound its cost.
     *
     * DO NOT try to bound the expansion ratio and skip this check on that basis. An
     * earlier version of this comment did exactly that -- "ICU-only, at most 1.5x,
     * because U+0130 is Unicode's sole expanding lowercase mapping" -- and both halves
     * are false. towlower is 1:1 per WIDE CHARACTER, not per byte, so a libc provider
     * expands too: U+023A folds to U+2C65, 2 bytes to 3. And the ratio is not 1.5x:
     * under a Turkish ICU collation 'I' (1 byte) folds to U+0131 (2 bytes), i.e. 2.0x.
     * Adversarial review supplied both counterexamples.
     *
     * The check below is provider-independent and needs no such argument, which is the
     * point: measure the quantity the matcher will actually consume rather than reason
     * about how far it can drift from the one parse time measured.
     *
     * Same shape as the min_prefix note below: the folded length is a different quantity
     * from the one parse time measured, so a fold in either direction can make a
     * parse-accepted pattern error here. That is a loud rejection of an exotic pattern,
     * never a silent wrong answer. */
    if (patfoldlen > bm25_wildcard_max_pattern_length)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: wildcard pattern folds to %d bytes, exceeding "
                        "bm25_native.wildcard_max_pattern_length (%d)",
                        patfoldlen, bm25_wildcard_max_pattern_length),
                 errdetail("Case folding is not length-preserving; the matcher runs on "
                           "the folded pattern.")));

    star = memchr(pat, '*', (size_t) patfoldlen);
    if (star == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: wildcard pattern \"%.*s\" has no '*'",
                        patlen, pattern)));
    prefixlen = (int) (star - pat);
    /* Belt-and-suspenders: parse-time validate_wildcard_pattern already enforced
     * both of these, so a wildcard reaching the expander has a valid prefix.
     *
     * Measured against the FOLDED prefix, which is deliberately not the same quantity
     * parse time measured: min_prefix exists to bound the dictionary scan, and dict
     * bytes are folded, so the folded length is the one that matters. A fold that
     * SHORTENS its input (Kelvin sign to 'k' under ICU) can therefore make a
     * parse-accepted pattern error here instead. That is a loud, clean rejection of an
     * exotic pattern, never a silent wrong answer, so it is left as is. */
    if (prefixlen < bm25_wildcard_min_prefix)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: wildcard prefix too short (min %d)",
                        bm25_wildcard_min_prefix)));
    /* Pure prefix iff the first '*' is the last pattern byte: no glob tail to match,
     * so any dict entry starting with the prefix is accepted without bm25_glob_match. */
    pure_prefix = (star == pat + patfoldlen - 1);

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(BM25WildKey);
    ctl.entrysize = sizeof(BM25WildEnt);
    ctl.hash      = bm25_wildkey_hash;
    ctl.match     = bm25_wildkey_match;
    ctl.keycopy   = bm25_wildkey_copy;
    ctl.hcxt      = CurrentMemoryContext;   /* scratch: freed with the caller's scratch context */
    acc.seen = hash_create("bm25 wildcard dedup", 256, &ctl,
                           HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_KEYCOPY |
                           HASH_CONTEXT);
    acc.cxt      = cxt;
    acc.capacity = 16;
    acc.out      = (BM25Token *) MemoryContextAlloc(cxt, sizeof(BM25Token) * acc.capacity);
    acc.n        = 0;
    acc.pattern  = pattern;
    acc.patlen   = patlen;

    /* (1) Each live segment's sorted dict: prefix-range scan + early stop.
     *
     * XCUT-03 (issue #145) -- WHY THIS OPEN-CODES THE DICT PAGE WALK INSTEAD OF
     * USING bm25_seg_dict_iter_*, which it did until this pass.
     *
     * The defect being fixed: the prefix compare, bm25_glob_match, and
     * bm25_wild_add's MemoryContextAlloc + dynahash HASH_ENTER (rehash-capable,
     * OOM-ERROR-capable) all ran with the DICT page SHARE-locked, once per entry,
     * for every entry on the page. A buffer content lock is an LWLock and
     * LWLockAcquire does HOLD_INTERRUPTS(), so the per-entry CHECK_FOR_INTERRUPTS
     * was a silent no-op and a wildcard over a wide dictionary ignored
     * pg_cancel_backend and statement_timeout for a DICT page at a time, while
     * blocking any writer that needed the page.
     *
     * The first fix kept the iterator and used bm25_seg_dict_iter_unlock/_relock
     * per term, as the merge feed does. Adversarial review rejected that, and the
     * reason is worth keeping: it is NOT sound on a hot standby. The merge feed's
     * safety rests on holding the metapage singleton, which no reader has. The
     * substitute offered for a reader was "an ordinary MVCC snapshot holds
     * GlobalVisCheckRemovableFullXid back" -- true on the PRIMARY, and worth
     * nothing on a standby WITHOUT hot_standby_feedback, where that horizon is
     * evaluated on the primary and this backend's snapshot restrains nothing.
     * Generic WAL redo takes an ordinary exclusive content lock and carries no
     * recovery-conflict information, so during the unlocked window the startup
     * process can replay retire->stamp->reuse over the pinned page, and _relock
     * would resume it->cur/it->end against rewritten bytes with NO revalidation of
     * any kind -- not even the page-kind check the iterator runs at page
     * crossings. Memory-safe, but a silently wrong expansion set.
     *
     * So this walk does what the other four sites in this pass do and what ADR
     * 0063 blesses: COPY the page under its SHARE lock, release, and decode the
     * private copy. That dissolves the hazard rather than documenting it -- there
     * is no window in which a page can change under a live cursor, because the
     * cursor addresses caller memory. It is also cheaper than per-term lock
     * cycling (one memcpy per ~254 entries instead of two LWLock operations per
     * entry), and it lets this walk carry the two backstops the iterator
     * structurally cannot:
     *
     *   - REAL gen validation. The iterator had none then (it has since issue
     *     #303, checked at page crossings only); here h.gen is in hand from the snapshot, so a page that was
     *     reclaimed and reused is caught and raises
     *     ERRCODE_T_R_SERIALIZATION_FAILURE. That is precisely the retryable error
     *     bm25_scan_build_ranking's bounded subtransaction retry exists for, and
     *     this function is reached only from bm25_scan_build_ranking_exhaustive
     *     underneath it -- so the standby substitution case now ends in a retry
     *     with a fresh snapshot instead of a wrong answer. This REPLACES the older
     *     "no validation, but a torn read only wastes a lookup" reasoning in this
     *     function's header, which was true only because nothing downstream
     *     trusted the term; it was never a defense of reading a reused page.
     *   - The `blk < nblocks` extent bound the house pattern requires
     *     (bm25_debug_terms in bm25_segment.c documents it as the house pattern).
     *     The iterator lacked one when this was written and has one now
     *     (SEGREAD-11, issue #154), in the ERRORING form this walk uses rather than
     *     the debug dumps' quiet terminate; the gen argument above is what still
     *     keeps this walk off it.
     *
     * bm25_seg_dict_iter_unlock/_relock consequently return to ONE caller, the
     * merge feed, whose metapage-singleton precondition is genuine and
     * primary-only. Do not add a second caller without re-reading that pair's
     * header. */
    for (si = 0; si < snap->nsegs; si++)
    {
        BM25SegmentHeader   h;
        BlockNumber         dblk;
        bool                past_prefix = false;
        BM25SegWalk         w;

        CHECK_FOR_INTERRUPTS();

        bm25_seg_header_read(index, snap->segs[si].header_blkno,
                             snap->segs[si].gen, &h);
        bm25_seg_walk_init(&w, index, nblocks, h.gen, BM25_PAGE_DICT, false,
                           "segment DICT chain");
        bm25_dict_order_init(order);

        dblk = h.dict_root;
        while (dblk != InvalidBlockNumber && !past_prefix)
        {
            Buffer  buf;
            Page    pg;
            char   *cur, *end;
            Size    pagebytes;
            const BM25DictEntry *last = NULL;

            CHECK_FOR_INTERRUPTS();

            /* Past-the-extent link: ERROR, do not stop quietly.
             *
             * The `blk < nblocks` loop-condition form used by the debug SRFs and
             * the VACUUM chain walks would be WRONG here, and the difference is
             * the answer being produced, not the walk. Those callers have a sound
             * fallback -- a debug dump reports what it could reach, a sweep frees
             * what it could reach -- so terminating early degrades gracefully. A
             * QUERY RESULT SET has no such fallback: ending the walk here silently
             * shrinks the wildcard's expansion term set, which silently drops
             * matching rows. That is exactly the outcome the pd_lower bound a few
             * lines below exists to prevent, and exactly why this function's
             * PENDING pass takes no extent bound at all.
             *
             * It also keeps this walk consistent with bm25_seg_dict_lookup, which
             * walks these SAME chains on this SAME query path and errors on a corrupt
             * link -- since SEGREAD-11 (issue #154) through the same named check
             * rather than incidentally out of ReadBuffer. Without this, one corrupt
             * DICT chain would raise loudly for a plain term and silently under-answer
             * a wildcard over the identical pages.
             *
             * Unambiguously corruption, never a legitimate stale pointer: nothing
             * in this extension shrinks the index relation (no RelationTruncate /
             * smgrtruncate anywhere in src/ -- see bm25_segment_blkno_validate in
             * bm25_fsm.c, whose identical check on the VACUUM path is the
             * precedent for this one), so `nblocks` read once at entry is a lower
             * bound on the extent for this whole call, and a sealed segment's DICT
             * pages were all written before the scan's snapshot.
             *
             * TEST COVERAGE, stated exactly. The PREDICATE (blkno >= nblocks =>
             * ERRCODE_INDEX_CORRUPTED) is already pinned by
             * bm25_debug_segment_blkno_bounds in sql/78_trust_boundary_bounds,
             * which drives bm25_segment_blkno_validate directly; this site is not
             * given a fourth probe of the same predicate. The CALL-SITE WIRING is
             * not asserted anywhere, because no debug lever can write a corrupt
             * DICT nextblk -- the same gap sql/95_segment_pointer_bounds' header
             * states for every other block-pointer guard, recorded here rather
             * than papered over.
             *
             * Issue #303: the check now runs inside the walker (BM25SegWalk), with
             * block 0, the revisit test and the cap, and the content bound and
             * gen/kind under the lock -- read bare, a corrupt pd_lower would make
             * this page look empty and silently truncate the expansion set. Its call
             * site is asserted by sql/139_seg_walk (a DICT link to block 0). */
            buf = bm25_seg_walk_read(&w, dblk, &pagebytes);
            memcpy(pagecopy.data, BufferGetPage(buf), BLCKSZ);
            UnlockReleaseBuffer(buf);

            pg  = (Page) pagecopy.data;
            cur = (char *) PageGetContents(pg);
            end = cur + pagebytes;
            /* A page with entries, starting after the previous page's last term
             * (issue #303): the early stop below trusts the chain's order, and a
             * cycle through DICT pages ends here at its first revisit. */
            bm25_dict_page_first(order, cur, end, dblk);
            while (cur < end)
            {
                BM25DictEntry *e = (BM25DictEntry *) cur;
                /* Page-bounds trust boundary: bounds the header and the MAXALIGN'd
                 * term span inside [cur, end) before termlen is used, and returns
                 * the stride. Same shared helper every DICT walker uses. */
                Size           entry_len = bm25_dictentry_validate(cur, end);
                const char    *term      = cur + sizeof(BM25DictEntry);
                int            termlen   = e->termlen;
                bool           starts;

                /* Live: no content lock is held anywhere in this loop, so this
                 * fires per DICT ENTRY where the pre-fix version's check was dead
                 * for every entry after the first on the page. */
                CHECK_FOR_INTERRUPTS();

                starts = (termlen >= prefixlen &&
                          memcmp(term, pat, prefixlen) == 0);
                if (starts)
                {
                    if (pure_prefix || bm25_glob_match(pat, patfoldlen, term, termlen))
                        bm25_wild_add(&acc, term, termlen);   /* copies into cxt */
                }
                /* Not a prefix hit. In bm25_term_cmp order everything sorting after
                 * the prefix range is > prefix and cannot match, so stop -- the
                 * sorted chain guarantees no later entry, on this page or a later
                 * one, can match. (c == 0 implies starts, handled above.) */
                else if (bm25_term_cmp(term, termlen, pat, prefixlen) > 0)
                {
                    past_prefix = true;
                    break;
                }
                last = e;
                cur += entry_len;
            }
            if (!past_prefix)
            {
                Assert(last != NULL);   /* checked: bm25_dict_page_first */
                bm25_dict_page_last(order, (const char *) last + sizeof(BM25DictEntry),
                                    last->termlen);
            }
            dblk = BM25PageGetOpaque(pg)->nextblk;   /* off the copy */
        }
    }

    /* (2) Pending: no sorted dict, so glob-match every live doc's terms. Matches
     * dedup against the sealed set (a term in both sources is scored once).
     *
     * STRANDED CONTINUATIONS (issue #197, ADR 0069). This walk carries the same
     * per-document tid state pending_df / pending_df_by_field do (bm25_stats.c), and
     * for the same reason: an ordinary CANCELLED VACUUM leaves a document's part 0
     * invalidated and its later parts intact, each with a valid tid and
     * BM25_PENDING_DOC_CONT set. Gating on ItemPointerIsValid alone therefore admits
     * a fragment of a document VACUUM was removing.
     *
     * This walker was the LAST of the class to be corrected, because it is the only
     * one whose output is neither a TID nor filtered downstream. The TID-emitting
     * walkers (the @@@ collector, the AND mask, the phrase stash, bm25_load_if_needed)
     * are corrected for free -- a fragment's TID belongs to a heap tuple VACUUM was
     * removing, so the heap recheck drops it. pending_global_stats skips CONT records
     * outright. This one emits a term SET; nothing downstream can filter it, so the
     * fragment's terms enter the expansion. Post-#196 they carry df 0 and are dropped
     * from scoring, which leaves exactly one observable: they consume
     * bm25_native.wildcard_max_expansions, so a wildcard can hit the cap and ERROR
     * where it otherwise would not, until the next seal drops the fragment.
     *
     * DO NOT "simplify" this to skipping every CONT record. #196 measured that
     * variant and it is wrong in the opposite direction: a document's distinct
     * (field, term) entries are PARTITIONED across its parts
     * (bm25_pending_append_multi sets hdr.ndocterms = j - jstart over entries
     * [jstart, j)), so a term often lives ONLY in a continuation and skipping
     * continuations silently shrinks the expansion set -- which silently drops
     * matching rows, the very outcome the sealed pass's extent ERROR above exists to
     * prevent. sql/92 PART FOUR pins both directions.
     *
     * State spans the PAGE loop, not just the iterator: parts are sized greedily
     * against a full page, so every part of a MULTI-PART document starts on a fresh
     * one and a document's head and its continuations are never on the same page.
     * (A small single-part record does share a page with its neighbours -- it just
     * has no continuation to be separated from.) */
    blk = snap->pending_head;
    ItemPointerSetInvalid(&acc_tid);
    bm25_pending_walk_init(&pwalk, index, snap->next_gen);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter pit;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&pwalk, blk); /* SHARE-locked */

        /* XCUT-03 (issue #145): copy the page out under the SHARE lock and unlock
         * immediately, before any glob matching or accumulation. This loop used to
         * run bm25_glob_match plus bm25_wild_add's MemoryContextAlloc + dynahash
         * HASH_ENTER (rehash-capable, OOM-ERROR-capable) for every term of every
         * pending doc on the page, with the page still locked -- against the tail
         * page a concurrent inserter is appending to. A single fixed-size BLCKSZ
         * memcpy is cheap and bounded; everything below now reads the caller-owned
         * copy and never touches the shared buffer again. Verbatim the pattern
         * pending_phrase_stash / pending_and_stash use (bm25_scan_match.c); like them, it
         * also reads nextblk off the copy. bm25_pending_iter_begin runs its
         * page-kind and pd_lower gates against the copy, unchanged.
         *
         * TWO CONSEQUENCES THIS SHARES WITH THOSE WALKERS, both accepted there and
         * neither new in kind. (1) The nextblk snapshot is now taken BEFORE the
         * page's glob-match pass rather than immediately before the unlock, so the
         * window between reading a successor's block number and ReadBuffer'ing it
         * widens from ~0 to one page's decode. On the primary what stands between
         * that and a recycled successor is the drained-page retire horizon (issue
         * #135), held back by this scan's own snapshot; on a standby, which holds
         * no horizon back, it is the chain-epoch check in bm25_pending_walk_read
         * (#291), which turns a recycled successor into a 40001. (2) A
         * corrupt termlen/pos_bytes now walks off the end of a caller-stack
         * PGAlignedBlock instead of off a shared buffer -- read-only either way,
         * and bounded by the same bm25_pending_iter_next/term_entry_span checks as
         * before. */
        memcpy(pagecopy.data, BufferGetPage(buf), BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg = (Page) pagecopy.data;
        bm25_pending_iter_begin(&pit, pg);
        while (bm25_pending_iter_next(&pit))
        {
            BM25PendingDocHeader *dh = pit.cur;
            char   *p = (char *) dh + bm25_pending_doc_entries_off(dh);
            uint32  k;

            if (!ItemPointerIsValid(&dh->tid))
                continue;       /* tombstoned pending doc */

            if ((dh->flags & BM25_PENDING_DOC_CONT) == 0)
            {
                acc_active = true;
                acc_tid = dh->tid;
            }
            else if (!acc_active || !ItemPointerEquals(&acc_tid, &dh->tid))
                continue;       /* stranded continuation (ADR 0069) -- see the header */

            for (k = 0; k < dh->ndocterms; k++)
            {
                BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                const char           *tterm = p + sizeof(BM25PendingTermEntry);

                if (bm25_glob_match(pat, patfoldlen, tterm, te->termlen))
                    bm25_wild_add(&acc, tterm, te->termlen);
                /* SEGREAD-13 (issue #154): the shared stride rule, not a second copy
                 * of the MAXALIGN expression this used to open-code. `pit.end_ptr` is
                 * the same bound bm25_pending_iter_next already measured all
                 * ndocterms entries of THIS doc against before returning true, so the
                 * check here is a re-check and not a newly load-bearing one -- the
                 * identical arrangement drain_doc_add_part uses, and for the same
                 * reason: a second walk re-derives the bound rather than trusting the
                 * iterator's earlier pass blindly. */
                p += bm25_pending_term_entry_span(p, pit.end_ptr);
            }
        }
        bm25_pending_iter_end(&pit);
        next = BM25PageGetOpaque(pg)->nextblk;    /* off the copy: buf is long gone */
        blk = next;
    }

    hash_destroy(acc.seen);     /* the term copies it referenced live on in cxt */
    pfree(pat);
    pfree(order);
    *out_terms = acc.out;
    return acc.n;
}
