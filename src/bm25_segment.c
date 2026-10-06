/* bm25_segment.c -- SQL-callable debug SRFs that walk the on-page segment format
 * (bm25_debug_terms and bm25_debug_postings; the read-path helpers themselves live
 * in bm25_seg_read.c, bm25_seg_dict.c and bm25_seg_chain.c).
 *
 * On-page layout (sealed segments, written by bm25_seg_build.c):
 *   DICT page chain: sorted BM25DictEntry records with inline term bytes, laid
 *              from PageGetContents up to pd_lower, each MAXALIGN'd.
 *   POST page chain (one shared chain per segment, D-POST, not one per term):
 *              block-encoded postings from the orphan builder, every term's
 *              blocks back to back; a DICT entry's (post_root, post_off, df)
 *              names where a term's run starts and how long it is.
 *
 * The M0/M1 in-place single-segment builder (bm25_builder_begin/add_doc/flush
 * and bm25_segment_add_doc/new_term/append_posting) was removed; it is
 * superseded by the M2a pending-list + bm25_seg_build.c orphan builder.
 *
 * READ DISCIPLINE (SEGREAD-10, issue #145, ADR 0083): both SRFs below validate a
 * DICT page under its SHARE lock, memcpy it into a caller-owned PGAlignedBlock,
 * release the buffer, and decode the COPY. Nothing that allocates, hashes, reads
 * another page, or writes a tuplestore row runs with a content lock held -- which
 * is also what makes their per-entry CHECK_FOR_INTERRUPTS fire at all
 * (LWLockAcquire holds interrupts off for as long as the lock is held).
 */
#include "postgres.h"

#include "bm25.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"      /* df aggregation across segments (bm25_debug_terms) */
#include "utils/tuplestore.h"   /* tuplestore_begin_heap/putvalues -- no longer pulled
                                 * in transitively via funcapi.h as of PG19 */

/* -------------------------------------------------------------------------
 * bm25_debug_terms -- SRF: return (term text, df int), summing df per term
 * across EVERY live segment.
 * -------------------------------------------------------------------------
 * Re-pointed in Task 7 off the removed M1 meta.seg_root onto the published
 * catalog + the Task-5 segment reader: iterate every segment in bm25_segcat_read,
 * walk each segment's chained DICT, and sum df per term into a dynahash (written
 * catalog-wide so the multi-segment build that landed later needed no second
 * re-point). SEGMENTS-ONLY, and deliberately still so: the SCAN path gained
 * read-your-writes over unsealed pending (sql/16_pending_ryw), but this probe never
 * reads the pending list, so callers must seal before probing.
 * Materialize-mode SRF; regression introspection only. */
PG_FUNCTION_INFO_V1(bm25_debug_terms);
Datum
bm25_debug_terms(PG_FUNCTION_ARGS)
{
    /* DfEnt: dynahash entry -- fixed-width term key + summed df. The key is a
     * NUL-padded fixed buffer so HASH_BLOBS hashes the whole termlen+bytes.
     *
     * Issue #313 SEGREAD-06: the key was 64 bytes, so any term of 64 bytes or more
     * raised "term too long for debug aggregation" though the index stores terms up
     * to BM25_MAX_TERM_BYTES (bm25_debug_postings handled the same index). It is now
     * sized for the longest storable term plus the NUL the emit loop's strlen needs.
     * A debug SRF, so 2 KB per distinct term is an acceptable price for not
     * re-keying the hash. df is summed across segments, so it accumulates in int64
     * and is range-checked on the way out to the int column. */
    typedef struct
    {
        char    term[BM25_MAX_TERM_BYTES + 1];
        int64   df;
    } DfEnt;

    ReturnSetInfo    *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid               relid = PG_GETARG_OID(0);
    Relation          index;
    BM25SegCatEntry  *segs;
    uint32            nsegs, s;
    Tuplestorestate  *ts;
    TupleDesc         tupdesc;
    MemoryContext     oldctx;
    HTAB             *agg;
    HASHCTL           ctl;
    HASH_SEQ_STATUS   seq;
    DfEnt            *de;
    /* SEGREAD-10 (issue #145): destination of the per-DICT-page copy-then-unlock
     * below. One BLCKSZ buffer reused across every page of every segment. */
    PGAlignedBlock    pagecopy;
    /* Issue #303: the chain-order state, palloc'd for its BLCKSZ term buffer. */
    BM25DictOrder    *order = palloc(sizeof(BM25DictOrder));

    /* Standard Materialize-mode SRF guard. */
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context"
                        " that cannot accept a set")));

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(((DfEnt *) 0)->term);
    ctl.entrysize = sizeof(DfEnt);
    ctl.hcxt      = CurrentMemoryContext;
    agg = hash_create("bm25 debug_terms df", 256, &ctl,
                      HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    bm25_segcat_read(index, &segs, &nsegs);
    for (s = 0; s < nsegs; s++)
    {
        BM25SegmentHeader h;
        BlockNumber       blk;
        BM25SegWalk       w;

        bm25_seg_header_read(index, segs[s].header_blkno, segs[s].gen, &h);
        blk = h.dict_root;
        /* A QUIET walk (issue #303): a link past the extent ends this segment's dump,
         * which is what separates this walker from the reader chains in
         * bm25_seg_dict.c and bm25_seg_chain.c, whose walks ERROR there, because a
         * lookup that stops early reports a term absent and a reader that stops early
         * returns a default -- both silently wrong answers. A debug dump reporting
         * what it could reach is a defensible answer, so this one terminates. Every
         * other check the walker runs (block 0, revisit, the visit cap, content, gen,
         * kind) and the DICT order rules ERROR here as everywhere: the dump used to
         * stop only at the extent, so a cycle spun until cancelled.
         *
         * nblocks 0: the walker samples the extent at its first read, which is after
         * bm25_segcat_read above. The sample used to be taken before the catalog
         * read, so a segment sealed in between had its DICT pages past the stale
         * sample and was quietly left out (SEGREAD-07). */
        bm25_seg_walk_init(&w, index, 0, h.gen, BM25_PAGE_DICT, true,
                           "segment DICT chain");
        bm25_dict_order_init(order);
        while (blk != InvalidBlockNumber)
        {
            Buffer  buf;
            Page    pg;
            char   *cur, *end;
            BlockNumber next;
            Size    pagebytes;
            const BM25DictEntry *prev = NULL;

            CHECK_FOR_INTERRUPTS();

            /* SEGREAD-10 (issue #145): validate under the SHARE lock, copy the page
             * out, unlock, decode the copy. The entry loop below runs a
             * hash_search(HASH_ENTER) per term -- an unbounded dynahash expansion,
             * and an OOM-ERROR site -- which used to execute with this DICT page
             * still locked, for every entry on the page. A buffer content lock is
             * an LWLock and LWLockAcquire does HOLD_INTERRUPTS(), so the whole
             * window was also uncancellable. One fixed BLCKSZ memcpy is cheap and
             * bounded; the copy is caller-owned and nothing below touches the
             * shared buffer again (pattern: pending_phrase_stash, bm25_scan_match.c).
             *
             * Deliberately NOT converted onto bm25_seg_dict_iter_*: that iterator
             * carries expected_gen = 0 by design, so routing this walk through it
             * would DROP the gen check this SRF has. (It would no longer drop an
             * extent bound -- the iterator gained one in SEGREAD-11 -- but the
             * iterator's ERRORS where this SRF terminates, which is the wrong
             * behaviour for a dump.) Copy-then-unlock keeps both and still gets the
             * lock-free entry loop.
             *
             * The walker takes the SHARE lock and runs the content bound (pd_lower:
             * read bare, a corrupt one made this page look empty and the loop moved
             * on silently) and gen/kind under it. */
            buf = bm25_seg_walk_read(&w, blk, &pagebytes);
            if (!BufferIsValid(buf))
                break;          /* quiet: a link past the extent ends the dump */
            memcpy(pagecopy.data, BufferGetPage(buf), BLCKSZ);
            UnlockReleaseBuffer(buf);

            pg = (Page) pagecopy.data;
            cur = (char *) PageGetContents(pg);
            end = cur + pagebytes;
            bm25_dict_page_first(order, cur, end, blk);     /* issue #303 */
            while (cur < end)
            {
                BM25DictEntry *e = (BM25DictEntry *) cur;
                /* Page-bounds trust boundary: validates the header and the
                 * MAXALIGN'd term span fit [cur, end) before e->termlen is used
                 * for anything, and returns the stride to advance by. */
                Size           entry_len = bm25_dictentry_validate(cur, end);
                char           key[sizeof(((DfEnt *) 0)->term)];
                bool           found;

                if (prev != NULL)
                    bm25_dict_entry_order_validate((const char *) prev +
                                                   sizeof(BM25DictEntry),
                                                   prev->termlen,
                                                   cur + sizeof(BM25DictEntry),
                                                   e->termlen, blk);
                prev = e;
                order->nentries++;

                /* Live now that no content lock is held: one check per DICT ENTRY,
                 * where this loop previously had one per DICT PAGE and it was dead
                 * for every entry after the first (SEGREAD-10). */
                CHECK_FOR_INTERRUPTS();

                /* Distinct from the bounds check above: a page-bounded termlen can
                 * still exceed `key`. No stored term can (the tokenizer caps terms
                 * at BM25_MAX_TERM_BYTES), so a longer one is corruption. */
                if (e->termlen >= (int) sizeof(key))
                    ereport(ERROR,
                            (errcode(ERRCODE_INDEX_CORRUPTED),
                             errmsg("bm25_debug_terms: dictionary term of %d bytes exceeds "
                                    "the %d-byte term limit",
                                    (int) e->termlen, BM25_MAX_TERM_BYTES)));
                MemSet(key, 0, sizeof(key));
                memcpy(key, cur + sizeof(BM25DictEntry), e->termlen);
                de = (DfEnt *) hash_search(agg, key, HASH_ENTER, &found);
                if (!found)
                    de->df = 0;
                de->df += (int64) e->df;
                cur += entry_len;
            }
            Assert(prev != NULL);   /* checked: bm25_dict_page_first */
            bm25_dict_page_last(order, (const char *) prev + sizeof(BM25DictEntry),
                                prev->termlen);
            next = BM25PageGetOpaque(pg)->nextblk;    /* off the copy: buf is released */
            blk = next;
        }
        /* The count only for a walk that reached the chain's end, not one a quiet
         * extent stop cut short. */
        if (blk == InvalidBlockNumber)
            bm25_dict_count_validate(order->nentries, h.nterms);
    }
    pfree(order);

    /* Emit (term, summed df). strlen on the NUL-padded fixed key recovers the
     * term length (test terms have no embedded NULs). */
    hash_seq_init(&seq, agg);
    while ((de = (DfEnt *) hash_seq_search(&seq)) != NULL)
    {
        Datum  vals[2];
        bool   nulls[2] = {false, false};

        if (de->df > PG_INT32_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                     errmsg("bm25_debug_terms: df " INT64_FORMAT " of term \"%s\" does not "
                            "fit the int result column", de->df, de->term)));
        vals[0] = PointerGetDatum(cstring_to_text_with_len(de->term, strlen(de->term)));
        vals[1] = Int32GetDatum((int32) de->df);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    hash_destroy(agg);

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* -------------------------------------------------------------------------
 * bm25_debug_postings -- SRF: return (term, tf, doclen) for every posting,
 * catalog-wide across every live segment.
 * -------------------------------------------------------------------------
 * Re-pointed in Task 7 off the removed M1 meta.seg_root onto the published catalog
 * + the Task-5 reader: for each segment, for each DICT entry, decode the term's
 * block postings (bm25_seg_scan_postings) and emit (term, tf, doclen) per posting,
 * reading doclen from the NORMS chain through a per-segment BM25SegReader
 * (bm25_seg_reader_doclen). SEGMENTS-ONLY, like
 * bm25_debug_terms above: it never reads the pending list even though the scan path
 * does, so callers must seal before probing.
 * Materialize-mode SRF; regression introspection only. */
typedef struct
{
    /* The segment's reader, opened once per segment in bm25_debug_postings' segment
     * loop; it carries the index and the segment header, which is why the context no
     * longer does. This callback runs PER POSTING, and it was the last per-posting
     * caller of the one-shot bm25_seg_doclen (issue #225): every posting re-walked
     * NORMS from the root and paid one RelationGetNumberOfBlocks, the cost ADR 0095
     * removed from every other per-posting path. A term's postings arrive in ascending
     * docid order, so the cursor resumes forward within a term and restarts from the
     * root only at the next term's first posting. */
    BM25SegReader      *rdr;
    const char         *term;
    int                 termlen;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
} DbgPostCtx;

static void
dbg_post_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    DbgPostCtx *c = (DbgPostCtx *) state;
    uint32      doclen = bm25_seg_reader_doclen(c->rdr, local_docid);
    Datum       vals[3];
    bool        nulls[3] = {false, false, false};

    (void) field_id;    /* (term,tf,doclen) is field-agnostic; whole-doc doclen. */

    vals[0] = PointerGetDatum(cstring_to_text_with_len(c->term, c->termlen));
    vals[1] = Int32GetDatum((int32) tf);
    vals[2] = Int32GetDatum((int32) doclen);
    tuplestore_putvalues(c->ts, c->tupdesc, vals, nulls);
}

PG_FUNCTION_INFO_V1(bm25_debug_postings);
Datum
bm25_debug_postings(PG_FUNCTION_ARGS)
{
    ReturnSetInfo    *rsi    = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid               relid  = PG_GETARG_OID(0);
    Relation          index;
    BM25SegCatEntry  *segs;
    uint32            nsegs, s;
    Tuplestorestate  *ts;
    TupleDesc         tupdesc;
    MemoryContext     oldctx;
    /* SEGREAD-10 (issue #145): destination of the per-DICT-page copy-then-unlock. */
    PGAlignedBlock    pagecopy;
    /* Issue #303: as in bm25_debug_terms above. */
    BM25DictOrder    *order = palloc(sizeof(BM25DictOrder));

    /* Standard Materialize-mode SRF guard. */
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context"
                        " that cannot accept a set")));

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);

    bm25_segcat_read(index, &segs, &nsegs);
    for (s = 0; s < nsegs; s++)
    {
        BM25SegmentHeader h;
        BM25SegReader     rdr;      /* scoped with h: cannot outlive its segment */
        BlockNumber       blk;
        BM25SegWalk       w;

        bm25_seg_header_read(index, segs[s].header_blkno, segs[s].gen, &h);
        bm25_seg_reader_init(&rdr, index, &h);

        /* Walk the DICT chain; for each entry decode its postings via the reader.
         *
         * SEGREAD-10 (issue #145): the reader's POST/NORMS SHARE locks used to nest
         * under a HELD DICT SHARE lock, and the old comment here only argued that
         * that could not SELF-DEADLOCK (true: all SHARE, distinct page kinds). It
         * never addressed the cost, which is the actual defect. An entire nested
         * postings replay -- every POST page of the term, every NORMS lookup, a
         * palloc'd term copy, and a tuplestore_putvalues per posting that can spill
         * to a temp file -- ran per DICT ENTRY with the DICT page still locked. And
         * because LWLockAcquire does HOLD_INTERRUPTS(), bm25_seg_scan_postings' and
         * chain_read_at's own CHECK_FOR_INTERRUPTS were dead for that whole span:
         * this SRF over a large segment ignored cancellation for a DICT page's worth
         * of terms at a time.
         *
         * Fixed by copying each DICT page under its lock and releasing before the
         * entry loop -- a debug SRF has no cross-page consistency requirement that
         * forbids it, and the nested replay reads unrelated blocks. See
         * bm25_debug_terms above for why this is NOT routed through
         * bm25_seg_dict_iter_* (that would drop the gen and extent backstops).
         *
         * The walk is the same quiet BM25SegWalk, with the same DICT order rules and
         * the same lazy extent sample, as bm25_debug_terms above (issue #303). */
        bm25_seg_walk_init(&w, index, 0, h.gen, BM25_PAGE_DICT, true,
                           "segment DICT chain");
        bm25_dict_order_init(order);
        blk = h.dict_root;
        while (blk != InvalidBlockNumber)
        {
            Buffer  buf;
            Page    pg;
            char   *cur, *end;
            BlockNumber next;
            Size    pagebytes;
            const BM25DictEntry *prev = NULL;

            CHECK_FOR_INTERRUPTS();

            buf = bm25_seg_walk_read(&w, blk, &pagebytes);
            if (!BufferIsValid(buf))
                break;          /* quiet: a link past the extent ends the dump */
            memcpy(pagecopy.data, BufferGetPage(buf), BLCKSZ);
            UnlockReleaseBuffer(buf);

            pg = (Page) pagecopy.data;
            cur = (char *) PageGetContents(pg);
            end = cur + pagebytes;
            bm25_dict_page_first(order, cur, end, blk);
            while (cur < end)
            {
                BM25DictEntry *e = (BM25DictEntry *) cur;
                /* Page-bounds trust boundary (see bm25_debug_terms above): bounds
                 * e->termlen before it drives the palloc/memcpy size below and
                 * hands back the stride to advance by. */
                Size           entry_len = bm25_dictentry_validate(cur, end);
                DbgPostCtx     dc;
                char          *termcopy;

                if (prev != NULL)
                    bm25_dict_entry_order_validate((const char *) prev +
                                                   sizeof(BM25DictEntry),
                                                   prev->termlen,
                                                   cur + sizeof(BM25DictEntry),
                                                   e->termlen, blk);
                prev = e;
                order->nentries++;

                /* Live now that no content lock is held: one check per term, in
                 * addition to the ones inside bm25_seg_scan_postings that this
                 * loop's held DICT lock used to render dead (SEGREAD-10).
                 *
                 * One unit of cancellable work per dictionary entry replayed (#156).
                 * This is the granularity sql/80_maintenance_interrupts' SEGREAD-10
                 * assertion measures -- pre-fix the whole DICT page's worth of entries
                 * replayed inside one lock window, so the count ran to the full
                 * dictionary. See BM25_WORK_UNIT() in bm25.h. sql/80 injects its
                 * cancel at the pause point, ahead of the check. */
                bm25_debug_pause_point("debug_dict_entry");
                BM25_WORK_UNIT();
                CHECK_FOR_INTERRUPTS();

                termcopy = palloc(e->termlen);
                memcpy(termcopy, cur + sizeof(BM25DictEntry), e->termlen);
                dc.rdr     = &rdr;
                dc.term    = termcopy;
                dc.termlen = e->termlen;
                dc.ts      = ts;
                dc.tupdesc = tupdesc;
                /* e->df bounds the decode to this term's run within the shared,
                 * undelimited segment-wide postings chain (D-POST). */
                bm25_seg_scan_postings(index, e->post_root, e->post_off, e->df,
                                       h.gen, dbg_post_cb, &dc,
                                       InvalidBlockNumber, 0, NULL, NULL, NULL, 0);
                pfree(termcopy);
                cur += entry_len;
            }
            Assert(prev != NULL);   /* checked: bm25_dict_page_first */
            bm25_dict_page_last(order, (const char *) prev + sizeof(BM25DictEntry),
                                prev->termlen);
            next = BM25PageGetOpaque(pg)->nextblk;    /* off the copy: buf is released */
            blk = next;
        }
        if (blk == InvalidBlockNumber)
            bm25_dict_count_validate(order->nentries, h.nterms);
    }
    pfree(order);

    index_close(index, AccessShareLock);
    return (Datum) 0;
}
