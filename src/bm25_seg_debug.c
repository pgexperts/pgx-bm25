/* bm25_seg_debug.c -- the SQL-callable debug probes over the sealed-segment
 * reader: bm25_debug_* SRFs that expose segment catalog, dictionary, posting,
 * position, liveness and length internals to the regression suites, plus the
 * callbacks and state types only they use. Two of them,
 * bm25_debug_stamp_seg_field_count, bm25_debug_stamp_chain_next and the four
 * levers after it (issues #293/#294), are test-only corruption levers that WAL-log a
 * forged value onto a real page, to reach decode boundaries a suite cannot otherwise
 * construct.
 *
 * Split out of bm25_seg_read.c (#228, ADR 0101) as a pure code move; the
 * SQL-visible signatures are untouched. Kept apart from bm25_debug.c, the scan-side
 * probes, because these call only the segment reader's API. Like every
 * bm25_debug_* function they are REVOKEd from PUBLIC by the extension script's
 * allowlist loop (sql/63_debug_privileges).
 *
 * Not every segment-reader probe is here. A probe that wraps one validator stays
 * beside it, and a probe that calls a file-static stays in the file that owns it,
 * so the static keeps internal linkage and the probe runs the code the reader
 * runs. Those are: the page-kind, page-offset, block-number and content-bytes
 * probes, bm25_debug_segcat_walk and bm25_debug_seg_lenfields in bm25_seg_read.c;
 * bm25_debug_dictentry_validate in bm25_seg_dict.c; and
 * bm25_debug_chain_span_validate and bm25_debug_chain_cursor_crosstalk in
 * bm25_seg_chain.c.
 */
#include "postgres.h"

#include "bm25.h"
#include "funcapi.h"
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "storage/bufmgr.h"
#include "storage/lmgr.h"      /* LockPage: bm25_debug_livedocs_clear */
#include "utils/builtins.h"
#include "utils/tuplestore.h"   /* tuplestore_begin_heap/putvalues -- no longer pulled
                                 * in transitively via funcapi.h as of PG19 */

/* bm25_debug_seg_doc_live -- SQL-callable: returns true if local_docid in the
 * named segment is live. The livedocs bitmap is all-set at seal; VACUUM clears a
 * doc's bit when it tombstones it (Phase 3), so a clear bit == tombstoned/dead.
 * Before any deletes every doc is live. Range-checks the seg argument.
 *
 * Race note (adversarial review, 2026-08): bm25_segcat_read below runs without the
 * metapage singleton (see bm25_handler.c's bm25_bulkdelete for a caller that DOES
 * take it, and why; bm25_fsm.c's bm25_reclaim_orphans and bm25_merge.c's
 * bm25_merge_rewrite_all hold it across their own bm25_segcat_read too, and
 * bm25_merge_execute inherits it from bm25_merge_maybe -- not an exhaustive list,
 * so do not read it as one). On a healthy index under concurrent merge/reclaim, that
 * now means bm25_segcat_read's own short/truncated-chain ERROR (fix 3) is reachable
 * here where it wasn't before -- a benign snapshot race that used to read as an
 * (silently wrong) undercount now surfaces as ERRCODE_INDEX_CORRUPTED instead. Still
 * a strict improvement (loud beats silently wrong), but a debug SRF that fails
 * transiently under concurrency is worth knowing about if it's ever seen in the wild. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_doc_live);
Datum
bm25_debug_seg_doc_live(PG_FUNCTION_ARGS)
{
    Oid                 relid  = PG_GETARG_OID(0);
    int32               seg    = PG_GETARG_INT32(1);
    int64               docid  = PG_GETARG_INT64(2);
    Relation            index  = bm25_index_open_readable(relid, AccessShareLock);
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;
    bool                live;

    bm25_segcat_read(index, &segs, &nsegs);
    if (seg < 0 || (uint32) seg >= nsegs)
    {
        index_close(index, AccessShareLock);    /* release before erroring (txn abort would too) */
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    }
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);

    /* Range-check the caller's docid (SEGREAD-09). `seg` was always checked; `docid`
     * never was, and the two failures it produced were both wrong in kind rather than
     * dangerous. The callee does now reject an out-of-range local docid, but as
     * ERRCODE_INDEX_CORRUPTED -- so a user typo was reported to an operator as a
     * corrupt index, which is the sort of message that starts an incident. And the
     * `(uint32)` cast below silently truncates, so docid = 2^32 + 5 read document 5 and
     * returned a confidently wrong answer rather than complaining at all.
     *
     * Checked against this segment's own ndocs, which is the bound the callee applies;
     * doing it here means the error names the argument and carries the parameter-value
     * errcode it should have had. */
    if (docid < 0 || (uint64) docid >= h.ndocs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_seg_doc_live: docid " INT64_FORMAT
                        " out of range for segment %d (" UINT64_FORMAT " documents)",
                        docid, seg, h.ndocs)));
    }

    live = bm25_seg_doc_is_live(index, &h, (uint32) docid);
    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(live);
}

/* bm25_debug_seg_chain_extent(index regclass, seg int, walker text, root bigint)
 *   -- SEGREAD-11 (issue #154) CALL-SITE probe.
 *
 * Why this is not another pure-function probe. The convention 78_trust_boundary_bounds
 * and 95_segment_pointer_bounds follow is to extract a validator and drive it with
 * caller-supplied values, because "a regression suite cannot produce a corrupt page".
 * That convention proves a PREDICATE and, as 95's own header says, leaves the WIRING
 * unasserted: delete the call and the suite stays green. For a bound whose entire
 * content IS the call -- `blk >= nblocks` is two comparisons no one doubts -- a
 * predicate-only test would assert nothing about the change. So this probe corrupts
 * the ROOT instead of the page: it reads a real, well-formed segment header into a
 * PRIVATE STACK COPY and substitutes a caller-chosen root before handing it to the
 * walker. Nothing is written, no page is touched until the walker touches it, and the
 * corrupt value takes exactly the path a corrupt on-page root would.
 *
 * It reaches the one shape no other lever in the tree can produce, which is why
 * 95_segment_pointer_bounds could only state the gap: an out-of-extent chain pointer
 * on a real read path. Pre-fix, an out-of-extent root surfaced as the buffer manager's
 * "could not read block N in file ..." (ERRCODE_DATA_CORRUPTED) -- correct, but it
 * names no bm25 chain and reads as a storage fault rather than index corruption.
 *
 * Four walkers, because the bound landed at four call sites with two different
 * `nblocks` sources, and only driving both proves both:
 *   dict_lookup   bm25_seg_dict_lookup      nblocks hoisted at function entry
 *   dict_iter     bm25_seg_dict_iter_next   nblocks captured in _begin
 *   chain         chain_read_at, cur = NULL nblocks captured per call
 *   chain_cursor  chain_read_at, cur != NULL nblocks taken from the cursor
 * The last two are the same `if` in chain_read_at reached down its two arms; a
 * regression that dropped the cursor arm would leave `chain` green on its own.
 *
 * Returns true when the walker ran to completion without raising. The interesting
 * direction is the error, so the true answer is the negative control: a healthy root
 * still walks, i.e. the bound rejects nothing the writer emits. TEST-ONLY, read-only,
 * and revoked from PUBLIC by the install script's loop like every bm25_debug_*. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_chain_extent);
Datum
bm25_debug_seg_chain_extent(PG_FUNCTION_ARGS)
{
    Oid                 relid  = PG_GETARG_OID(0);
    int32               seg    = PG_GETARG_INT32(1);
    char               *walker = text_to_cstring(PG_GETARG_TEXT_PP(2));
    int64               root   = PG_GETARG_INT64(3);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;
    BlockNumber         proot;
    uint16              po;
    uint32              df;

    /* Argument validation before the index is opened, per 97_debug_probe_arguments:
     * a probe's arguments are untrusted input exactly like a query's. The uint32 range
     * is the BlockNumber domain; InvalidBlockNumber itself is allowed through because
     * it is a legal chain terminator and a legitimate thing to hand these walkers.
     *
     * root = -1 means "do not substitute anything, walk the segment's own root". That
     * is the negative control, and it has to be a sentinel rather than a block number
     * typed into the suite: the real roots are not visible from SQL, and a suite that
     * hard-coded them would be pinning this build's page layout instead of the guard. */
    if (root < -1 || root > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_seg_chain_extent: root " INT64_FORMAT
                        " is not a block number", root)));

    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_segcat_read(index, &segs, &nsegs);
    if (seg < 0 || (uint32) seg >= nsegs)
    {
        index_close(index, AccessShareLock);    /* release before erroring */
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    }
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);

    /* From here a walker may raise; the relation is left to transaction abort's
     * resource owner, exactly as it is when bm25_debug_seg_doc_live's callee raises. */
    if (strcmp(walker, "dict_lookup") == 0)
    {
        /* An empty key sorts before every stored term, so the very first entry
         * compares greater and the lookup stops after ONE page: the walk is the point,
         * not the answer, and a one-page walk still has to clear the bound first. */
        if (root >= 0)
            h.dict_root = (BlockNumber) root;
        (void) bm25_seg_dict_lookup(index, &h, "", 0, &proot, &po, &df, NULL, NULL);
    }
    else if (strcmp(walker, "dict_iter") == 0)
    {
        void   *it;
        char   *term;
        int     termlen;

        /* Drained, not stepped once: with a healthy root this crosses every DICT page
         * of the segment, so the negative control covers mid-chain links and not just
         * the root. */
        if (root >= 0)
            h.dict_root = (BlockNumber) root;
        /* No CHECK_FOR_INTERRUPTS in the drain: the iterator holds its DICT page
         * SHARE-locked between entries, which holds interrupts, so a check here was
         * dead (issue #313 XCUT-09). The iterator runs its own per-page check. */
        it = bm25_seg_dict_iter_begin(index, &h);
        while (bm25_seg_dict_iter_next(it, &term, &termlen, &proot, &po, &df,
                                       NULL, NULL))
        {
            /* drained for the walk's own checks; nothing to do per entry */
        }
        bm25_seg_dict_iter_end(it);
    }
    else if (strcmp(walker, "chain") == 0)
    {
        if (root >= 0)
            h.norms_root = (BlockNumber) root;
        (void) bm25_seg_doclen_field(index, &h, 0, 0);       /* cur == NULL */
    }
    else if (strcmp(walker, "chain_cursor") == 0)
    {
        BM25SegReader   r;

        if (root >= 0)
            h.norms_root = (BlockNumber) root;
        bm25_seg_reader_init(&r, index, &h);                 /* captures nblocks */
        (void) bm25_seg_reader_doclen_field(&r, 0, 0);
    }
    else
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_seg_chain_extent: unknown walker \"%s\"", walker),
                 errhint("Use dict_lookup, dict_iter, chain or chain_cursor.")));
    }

    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(true);
}

/* post_cb for bm25_debug_seg_postings_count: count, decode nothing. */
static void
seg_postings_count_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    (void) local_docid;
    (void) tf;
    (void) field_id;
    *(int64 *) state += 1;
}

/* bm25_debug_seg_postings_count(index regclass, seg int, root bigint) RETURNS bigint
 *   -- SEGREAD-11 (issue #154) CALL-SITE probe for the POST chain.
 *
 * Same lever as bm25_debug_seg_chain_extent above -- a real segment, a real walk, a
 * substituted chain root, nothing written -- but it RETURNS A COUNT rather than a bool,
 * and that difference is the whole point. The defect on this walk is not that it raises
 * the wrong error; it is that it raises NO error and returns NOTHING. A probe that only
 * reported "did not raise" would have called the broken build a pass. So this one
 * reports how many postings bm25_seg_scan_postings actually emitted, which makes the
 * silence itself the assertion: pre-fix, root = InvalidBlockNumber returned 0 for a
 * term the dictionary says occurs in df documents.
 *
 * The term is the segment's FIRST dictionary entry, taken through the real iterator, so
 * the probe needs no term argument and pins no layout; its df is separately visible from
 * SQL through bm25_debug_segterms, which is what lets the negative control assert
 * count == df rather than merely count > 0.
 *
 * Three roots, three answers: -1 (the segment's own root) counts df; InvalidBlockNumber
 * is the unreachable-root case above; any block at or past the extent is the chain
 * bound. TEST-ONLY, read-only, REVOKEd from PUBLIC with every other bm25_debug_*. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_postings_count);
Datum
bm25_debug_seg_postings_count(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               seg   = PG_GETARG_INT32(1);
    int64               root  = PG_GETARG_INT64(2);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;
    void               *it;
    char               *term;
    int                 termlen;
    BlockNumber         proot;
    uint16              po;
    uint32              df;
    bool                got;
    int64               emitted = 0;

    /* Arguments validated before the index is opened (97_debug_probe_arguments), and
     * -1 is the same "substitute nothing" sentinel the sibling probe uses. */
    if (root < -1 || root > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_seg_postings_count: root " INT64_FORMAT
                        " is not a block number", root)));

    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_segcat_read(index, &segs, &nsegs);
    if (seg < 0 || (uint32) seg >= nsegs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    }
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);

    /* Closed before the walk, deliberately: the iterator holds its current DICT page
     * SHARE-locked between _next calls, and bm25_seg_scan_postings takes buffer locks of
     * its own. Only the SCALARS survive _end -- `term` points into the page the _end
     * just released, and is not read after this. */
    it  = bm25_seg_dict_iter_begin(index, &h);
    got = bm25_seg_dict_iter_next(it, &term, &termlen, &proot, &po, &df, NULL, NULL);
    bm25_seg_dict_iter_end(it);
    if (!got)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_seg_postings_count: segment %d has no dictionary "
                        "entries", seg)));
    }

    /* post_off is kept from the real entry: substituting the root alone is what a
     * corrupted pointer field looks like, and it keeps the offset a value the walker
     * would genuinely have been handed. */
    if (root >= 0)
        proot = (BlockNumber) root;

    /* From here the walker may raise; the relation is left to transaction abort's
     * resource owner, as in bm25_debug_seg_chain_extent. */
    bm25_seg_scan_postings(index, proot, po, df, h.gen,
                           seg_postings_count_cb, &emitted,
                           InvalidBlockNumber, 0, NULL, NULL, NULL, 0);

    index_close(index, AccessShareLock);
    PG_RETURN_INT64(emitted);
}

/* bm25_debug_segterms(index regclass) -- walk segment 0's dict, emit (term, df).
 * Development/regression introspection only; not on the query path.
 *
 * Race note (adversarial review, 2026-08): bm25_segcat_read below runs without the
 * metapage singleton (see bm25_handler.c's bm25_bulkdelete for a caller that DOES
 * take it, and why; bm25_fsm.c's bm25_reclaim_orphans and bm25_merge.c's
 * bm25_merge_rewrite_all hold it across their own bm25_segcat_read too, and
 * bm25_merge_execute inherits it from bm25_merge_maybe -- not an exhaustive list,
 * so do not read it as one). On a healthy index under concurrent merge/reclaim, that
 * now means bm25_segcat_read's own short/truncated-chain ERROR (fix 3) is reachable
 * here where it wasn't before -- a benign snapshot race that used to read as an
 * (silently wrong) undercount now surfaces as ERRCODE_INDEX_CORRUPTED instead. Still
 * a strict improvement (loud beats silently wrong), but a debug SRF that fails
 * transiently under concurrency is worth knowing about if it's ever seen in the wild. */
PG_FUNCTION_INFO_V1(bm25_debug_segterms);
Datum
bm25_debug_segterms(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid             relid = PG_GETARG_OID(0);
    Relation        index = bm25_index_open_readable(relid, AccessShareLock);
    BM25SegCatEntry *segs;
    uint32          nsegs;
    TupleDesc       tupdesc;
    Tuplestorestate *ts;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    /* C7: the Materialize-mode tuplestore MUST live in the per-query context, not
     * the (short-lived) current context, or PG 18 raises "invalid tuplestore
     * state" when the executor drains it after this call returns. Mirrors
     * bm25_debug_accum in bm25_accum.c. */
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    bm25_segcat_read(index, &segs, &nsegs);
    if (nsegs > 0)
    {
        BM25SegmentHeader h;
        BlockNumber       blk;
        PGAlignedBlock    pagecopy;
        BM25SegWalk       w;
        BM25DictOrder    *order = palloc(sizeof(BM25DictOrder));   /* issue #303 */

        bm25_seg_header_read(index, segs[0].header_blkno, segs[0].gen, &h);
        /* This whole-dictionary walk duplicates the page-chain traversal in
         * bm25_seg_dict_lookup; both should fold into the bm25_seg_dict_iter_*
         * iterator, which has landed and is what the merge feed walks its source
         * dicts with (bm25_accum_from_segments, bm25_merge.c). Neither this dump
         * nor the lookup has been refactored onto it -- and the DICT walkers that
         * needed the lock-hold fix (SEGREAD-10, issue #145: this one and
         * bm25_segment.c's two) took the copy-then-unlock route instead, which
         * KEEPS the gen validation the iterator, by design, does not carry -- and
         * keeps the QUIET form of the extent bound, which since SEGREAD-11 is the
         * only part of the bound the iterator does differently (it errors). */
        blk = h.dict_root;
        /* SEGREAD-07 (#139): this was the only DICT walker in the tree with neither
         * an interrupt check nor an extent bound; it now has both.
         *
         * Precision the original wording lacked, and which the paragraph above now
         * contradicted outright (issue #145): the two properties do NOT travel
         * together across the DICT walkers, so they need separate statements.
         * INTERRUPT CHECKS: every DICT walker has one -- bm25_seg_dict_lookup,
         * bm25_seg_dict_iter_next, bm25_segment.c's two, and this one. EXTENT
         * BOUNDS: every DICT walker has one of those too as of SEGREAD-11 (issue
         * #154), but in one of TWO FORMS, and which form a walker takes follows
         * from whether a truncated walk is a defensible answer for it. QUIET
         * TERMINATE -- bm25_segment.c's two debug SRFs and this one, all dumps that
         * legitimately report what they could reach. ERROR -- bm25_seg_dict_lookup
         * (stopping early reports a term ABSENT), bm25_seg_dict_iter_next (stopping
         * early hands the merge a short dictionary) and the query-path wildcard walk
         * in bm25_dict_expand_wildcard, bm25_seg_dict.c (a query result set has no
         * defensible truncated form). The older wording of this paragraph said the
         * first two had no extent bound at all, which was true when it was written.
         *
         * Issue #303: all of these now read through BM25SegWalk, this dump as a QUIET
         * walk with a lazy extent sample (taken at the first read, after the catalog
         * read above), the error form everywhere else; and every DICT walker checks
         * the chain's order, which ends a cycle the extent bound alone never did. */
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
             * out, unlock, then decode the copy. The entry loop below pallocs a text
             * Datum per term and calls tuplestore_putvalues, which can SPILL TO A
             * TEMP FILE once the store outgrows work_mem -- all of that used to run
             * with this DICT page still locked, and uncancellably so, since
             * LWLockAcquire holds interrupts off. Same pattern and reasoning as
             * bm25_segment.c's two SRFs and pending_phrase_stash (bm25_scan_match.c).
             * Copying does not weaken anything: validate_kind still runs on the
             * shared page under the lock, and the copy is what the decode sees, so
             * the two cannot disagree. The walker runs the content bound under the
             * lock too: a bare pd_lower read would make a corrupt page's entries
             * silently vanish from this dump instead of erroring. */
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
                Size    entry_len = bm25_dictentry_validate(cur, end);
                Datum   vals[2];
                bool    nulls[2] = {0};

                if (prev != NULL)
                    bm25_dict_entry_order_validate((const char *) prev +
                                                   sizeof(BM25DictEntry),
                                                   prev->termlen,
                                                   cur + sizeof(BM25DictEntry),
                                                   e->termlen, blk);
                prev = e;
                order->nentries++;

                /* Live now that nothing is locked: one check per DICT ENTRY, where
                 * this loop previously had one per DICT PAGE and it was dead. */
                CHECK_FOR_INTERRUPTS();

                vals[0] = PointerGetDatum(
                    cstring_to_text_with_len(cur + sizeof(BM25DictEntry), e->termlen));
                vals[1] = Int32GetDatum((int32) e->df);
                tuplestore_putvalues(ts, tupdesc, vals, nulls);
                cur += entry_len;
            }
            Assert(prev != NULL);   /* checked: bm25_dict_page_first */
            bm25_dict_page_last(order, (const char *) prev + sizeof(BM25DictEntry),
                                prev->termlen);
            next = BM25PageGetOpaque(pg)->nextblk;    /* off the copy */
            blk = next;
        }
        if (blk == InvalidBlockNumber)
            bm25_dict_count_validate(order->nentries, h.nterms);
        pfree(order);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_segcat(index regclass) -- introspect the segment catalog: one row
 * per sealed segment with (ndocs, live_ndocs, total_len, nterms, has_positions).
 * has_positions == (segment header pos_root != InvalidBlockNumber): what the WRITER
 * actually built, read straight off the header -- independent of any reader-side
 * store_positions gate. This is the M4 writer-observability witness: a test can
 * assert the writer emitted a POS chain (or, for an all-off / pre-M4 index, did NOT)
 * without inferring it from read-back rows (which the reader gate can force to zero
 * either way). Development/regression introspection only; not on the query path. */
/* bm25_debug_segcat_entry_bytes(index regclass, n int) -- the RAW bytes of catalog
 * entry n, padding hole included.
 *
 * Exists because the #144 class is invisible to every ordinary assertion: uninitialized
 * struct padding changes no query result, so a suite that only checks answers cannot
 * tell a memset-before-fill from its absence. This hands SQL the actual bytes, so a test
 * can assert the hole at offset [4,8) is zero -- a property that holds by construction
 * after the fix and holds only by luck before it.
 *
 * bm25_segcat_read memcpy's each entry off the page at full sizeof, so padding is
 * preserved verbatim on the way here; what SQL sees is what the page holds, which is
 * what Generic WAL shipped. Development and regression testing only.
 */
PG_FUNCTION_INFO_V1(bm25_debug_segcat_entry_bytes);
Datum
bm25_debug_segcat_entry_bytes(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               idx   = PG_GETARG_INT32(1);
    Relation            index;
    BM25SegCatEntry    *segs = NULL;
    uint32              nsegs = 0;
    bytea              *out;

    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_segcat_read(index, &segs, &nsegs);

    if (idx < 0 || (uint32) idx >= nsegs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_segcat_entry_bytes: entry %d out of range (%u live)",
                        idx, nsegs)));
    }

    out = (bytea *) palloc(VARHDRSZ + sizeof(BM25SegCatEntry));
    SET_VARSIZE(out, VARHDRSZ + sizeof(BM25SegCatEntry));
    memcpy(VARDATA(out), &segs[idx], sizeof(BM25SegCatEntry));

    index_close(index, AccessShareLock);
    PG_RETURN_BYTEA_P(out);
}

PG_FUNCTION_INFO_V1(bm25_debug_segcat);
Datum
bm25_debug_segcat(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid             relid = PG_GETARG_OID(0);
    Relation        index = bm25_index_open_readable(relid, AccessShareLock);
    BM25SegCatEntry *segs;
    uint32          nsegs, i;
    Tuplestorestate *ts;
    TupleDesc       tupdesc;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    /* C7: the Materialize-mode tuplestore MUST live in the per-query context, not
     * the (short-lived) current context, or PG 18 raises "invalid tuplestore
     * state" when the executor drains it after this call returns. Mirrors
     * bm25_debug_segterms above and bm25_debug_accum in bm25_accum.c. */
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    bm25_segcat_read(index, &segs, &nsegs);
    for (i = 0; i < nsegs; i++)
    {
        Datum               vals[6];
        bool                nulls[6] = {0};
        BM25SegmentHeader   h;

        /* Read the header for pos_root -- the writer's own record of whether it
         * built a POS chain (validating gen the same way the sibling debug SRFs do). */
        bm25_seg_header_read(index, segs[i].header_blkno, segs[i].gen, &h);

        vals[0] = Int64GetDatum((int64) segs[i].ndocs);
        vals[1] = Int64GetDatum((int64) segs[i].live_ndocs);
        vals[2] = Int64GetDatum((int64) segs[i].total_len);
        vals[3] = Int32GetDatum((int32) segs[i].nterms);
        vals[4] = BoolGetDatum(h.pos_root != InvalidBlockNumber);
        /* The CATALOG entry's copy, not the header's: this is the decayed value the
         * estimator actually reads, so a suite asserting tombstone decay sees it.
         * Reported raw, WITHOUT the BM25_FEAT_SEGCAT_TOKENS gate -- a probe that
         * silently zeroed an untrusted value could not tell "absent" from
         * "present but not trusted", which is exactly what sql/108 must pin. */
        vals[5] = Int64GetDatum((int64) segs[i].total_tokens);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_seg_postings(index regclass, qterm text) -- for each segment, look up
 * `qterm` and decode its postings via the (df-bounded) reader, tagging each row
 * with the segment index. Exercises bm25_seg_dict_lookup + bm25_seg_scan_postings
 * end to end, and the df bound in particular: the postings chain is shared across
 * all terms, so the rows for a term must end exactly at its df.
 * Development/regression introspection only; not on the query path. */
typedef struct
{
    Tuplestorestate *ts;
    TupleDesc        tupdesc;
    uint32           seg;
} SegPostDebugState;

static void
seg_post_debug_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    SegPostDebugState  *st = (SegPostDebugState *) state;
    Datum               vals[3];
    bool                nulls[3] = {0};

    (void) field_id;    /* the 3-col SRF stays M3-shaped; bm25_debug_field_postings
                         * (C2.7) surfaces field_id in its own 4-col SRF. */
    vals[0] = Int32GetDatum((int32) st->seg);
    vals[1] = Int64GetDatum((int64) local_docid);
    vals[2] = Int64GetDatum((int64) tf);
    tuplestore_putvalues(st->ts, st->tupdesc, vals, nulls);
}

PG_FUNCTION_INFO_V1(bm25_debug_seg_postings);
Datum
bm25_debug_seg_postings(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    text               *qterm = PG_GETARG_TEXT_PP(1);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs, i;
    SegPostDebugState   st;

    /* Validate the call context BEFORE opening the index (matches the sibling
     * debug SRFs). */
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &st.tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    index = bm25_index_open_readable(relid, AccessShareLock);
    st.tupdesc = BlessTupleDesc(st.tupdesc);
    /* C7: the Materialize-mode tuplestore MUST live in the per-query context, not
     * the (short-lived) current context, or PG 18 raises "invalid tuplestore
     * state" when the executor drains it after this call returns. Mirrors
     * bm25_debug_segterms / bm25_debug_segcat above. */
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        st.ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = st.ts;
    rsi->setDesc = st.tupdesc;

    bm25_segcat_read(index, &segs, &nsegs);
    for (i = 0; i < nsegs; i++)
    {
        BM25SegmentHeader   h;
        BlockNumber         post_root;
        uint16              post_off;
        uint32              df;

        bm25_seg_header_read(index, segs[i].header_blkno, segs[i].gen, &h);
        st.seg = i;
        if (bm25_seg_dict_lookup(index, &h, VARDATA_ANY(qterm),
                                 VARSIZE_ANY_EXHDR(qterm),
                                 &post_root, &post_off, &df, NULL, NULL))
            bm25_seg_scan_postings(index, post_root, post_off, df, h.gen,
                                   seg_post_debug_cb, &st,
                                   InvalidBlockNumber, 0, NULL, NULL, NULL, 0);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_field_postings(index regclass, qterm text) -- like bm25_debug_seg_postings
 * but surfaces the field_id decoded from the per-block field-id RLE. Exercises the
 * field-aware decode end to end (bm25_seg_scan_postings now emits field_id). Same
 * shared reader; only the callback differs (4 cols incl. field_id). Development/
 * regression introspection only; not on the query path. */
typedef struct
{
    Tuplestorestate *ts;
    TupleDesc        tupdesc;
    uint32           seg;
} SegFieldPostDebugState;

static void
seg_field_post_debug_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    SegFieldPostDebugState *st = (SegFieldPostDebugState *) state;
    Datum   vals[4];
    bool    nulls[4] = {0};

    vals[0] = Int32GetDatum((int32) st->seg);
    vals[1] = Int64GetDatum((int64) local_docid);
    vals[2] = Int64GetDatum((int64) tf);
    vals[3] = Int32GetDatum((int32) field_id);
    tuplestore_putvalues(st->ts, st->tupdesc, vals, nulls);
}

PG_FUNCTION_INFO_V1(bm25_debug_field_postings);
Datum
bm25_debug_field_postings(PG_FUNCTION_ARGS)
{
    ReturnSetInfo          *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                     relid = PG_GETARG_OID(0);
    text                   *qterm = PG_GETARG_TEXT_PP(1);
    Relation                index;
    BM25SegCatEntry        *segs;
    uint32                  nsegs, i;
    SegFieldPostDebugState  st;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &st.tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    index = bm25_index_open_readable(relid, AccessShareLock);
    st.tupdesc = BlessTupleDesc(st.tupdesc);
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        st.ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = st.ts;
    rsi->setDesc = st.tupdesc;

    bm25_segcat_read(index, &segs, &nsegs);
    for (i = 0; i < nsegs; i++)
    {
        BM25SegmentHeader   h;
        BlockNumber         post_root;
        uint16              post_off;
        uint32              df;

        bm25_seg_header_read(index, segs[i].header_blkno, segs[i].gen, &h);
        st.seg = i;
        if (bm25_seg_dict_lookup(index, &h, VARDATA_ANY(qterm),
                                 VARSIZE_ANY_EXHDR(qterm),
                                 &post_root, &post_off, &df, NULL, NULL))
            bm25_seg_scan_postings(index, post_root, post_off, df, h.gen,
                                   seg_field_post_debug_cb, &st,
                                   InvalidBlockNumber, 0, NULL, NULL, NULL, 0);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_block_impacts(index regclass, qterm text) -- M2b Task 3 round-trip
 * proof: for each segment, look up qterm and walk its run of blocks with the
 * header-only reader bm25_seg_block_header_read (never decoding a single
 * posting), emitting one row per (segment, block, field) impact-table entry:
 * (seg, block_no, nfields, field_id, max_tf, min_doclen). block_no is the
 * PHYSICAL page the logical block lives on -- a page holds several of a term's
 * blocks back to back, so block_no can repeat across rows (a different
 * field_id, or a different logical block that happens to share the page).
 * This is the ONLY thing that reads back what encode_block (bm25_seg_build.c)
 * stamped at seal/merge time, so it is what proves that stamping correct.
 * Development/regression introspection only; not on the query path. */
PG_FUNCTION_INFO_V1(bm25_debug_block_impacts);
Datum
bm25_debug_block_impacts(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    text               *qterm = PG_GETARG_TEXT_PP(1);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs, segidx;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;

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
    for (segidx = 0; segidx < nsegs; segidx++)
    {
        BM25SegmentHeader   h;
        BlockNumber         post_root;
        uint16              post_off;
        uint32              df;
        BlockNumber         blk;
        uint16              off;
        uint32              seen = 0;   /* cumulative ndocs read so far for this term */

        bm25_seg_header_read(index, segs[segidx].header_blkno, segs[segidx].gen, &h);
        if (!bm25_seg_dict_lookup(index, &h, VARDATA_ANY(qterm),
                                  VARSIZE_ANY_EXHDR(qterm),
                                  &post_root, &post_off, &df, NULL, NULL))
            continue;      /* term absent from this segment */

        blk = post_root;
        off = post_off;
        /* Stop once `seen` reaches df: a term's blocks carry no end marker (they
         * live back-to-back with the NEXT term's blocks in the one shared D-POST
         * chain), so df is the only thing that bounds this walk. */
        while (blk != InvalidBlockNumber && seen < df)
        {
            BM25BlockHeader hdr;
            BM25BlockImpact imp;
            BlockNumber     next_blk;
            uint16          next_off;
            int             fi;

            /* One per block, as in bm25_debug_block_spans' walk below (#305
             * REGR-05): a term's run can span a whole POST chain. */
            CHECK_FOR_INTERRUPTS();
            bm25_seg_block_header_read(index, blk, off, h.gen,
                                       &hdr, &imp, &next_blk, &next_off);

            for (fi = 0; fi < imp.nfields; fi++)
            {
                Datum vals[6];
                bool  nulls[6] = {0};

                vals[0] = Int32GetDatum((int32) segidx);
                vals[1] = Int64GetDatum((int64) blk);
                vals[2] = Int32GetDatum((int32) imp.nfields);
                vals[3] = Int32GetDatum((int32) imp.fields[fi].field_id);
                vals[4] = Int32GetDatum((int32) imp.fields[fi].max_tf);
                vals[5] = Int32GetDatum((int32) imp.fields[fi].min_doclen);
                tuplestore_putvalues(ts, tupdesc, vals, nulls);
            }

            seen += hdr.ndocs;
            blk = next_blk;
            off = next_off;
        }
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_block_spans(index regclass, qterm text) -- issue #289: for each
 * segment, one row per block of qterm's run, in chain order: (seg, block_ord,
 * ndocs, first_docid, last_docid). ndocs is the block's POSTING count (the header
 * field's historical name). A block whose first_docid equals the previous block's
 * last_docid in the same segment is a straddle: one document's postings for the
 * term split across the two. The writer no longer produces those, and the suite
 * uses this to show both that and that bm25_native.debug_count_slicing still
 * builds the old layout its reader tests need. Header-only reads, like
 * bm25_debug_block_impacts beside it. Development/regression introspection only. */
PG_FUNCTION_INFO_V1(bm25_debug_block_spans);
Datum
bm25_debug_block_spans(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    text               *qterm = PG_GETARG_TEXT_PP(1);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs, segidx;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;

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
    for (segidx = 0; segidx < nsegs; segidx++)
    {
        BM25SegmentHeader   h;
        BlockNumber         post_root;
        uint16              post_off;
        uint32              df;
        BlockNumber         blk;
        uint16              off;
        uint32              seen = 0;   /* df-bounded, as in bm25_debug_block_impacts */
        int32               ord = 0;

        bm25_seg_header_read(index, segs[segidx].header_blkno, segs[segidx].gen, &h);
        if (!bm25_seg_dict_lookup(index, &h, VARDATA_ANY(qterm),
                                  VARSIZE_ANY_EXHDR(qterm),
                                  &post_root, &post_off, &df, NULL, NULL))
            continue;

        blk = post_root;
        off = post_off;
        while (blk != InvalidBlockNumber && seen < df)
        {
            BM25BlockHeader hdr;
            BM25BlockImpact imp;
            BlockNumber     next_blk;
            uint16          next_off;
            BM25BlockLead   lead;
            Datum           vals[5];
            bool            nulls[5] = {0};

            CHECK_FOR_INTERRUPTS();
            bm25_seg_block_header_read_lead(index, blk, off, h.gen, &hdr, &imp,
                                            &next_blk, &next_off, 0, &lead);
            vals[0] = Int32GetDatum((int32) segidx);
            vals[1] = Int32GetDatum(ord++);
            vals[2] = Int32GetDatum((int32) hdr.ndocs);
            vals[3] = Int64GetDatum((int64) lead.docid);
            vals[4] = Int64GetDatum((int64) hdr.last_docid);
            tuplestore_putvalues(ts, tupdesc, vals, nulls);

            seen += hdr.ndocs;
            blk = next_blk;
            off = next_off;
        }
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_seg_positions(index regclass, qterm text) -- M4 position round-trip
 * probe. For each segment, look up qterm and, via the OPTIONAL lockstep POS cursor,
 * decode every position-bearing posting's frame, emitting (seg, local_docid,
 * field_id, position) -- one row per position. Exercises the whole C-POS-BUILD path
 * end to end: dict pos_post_root/off, the separate POS chain, page-spanning frames,
 * and the tf-count == tf self-check. Consults each field's store_positions bit (from
 * the field-config page) so a positions-off field yields NO rows (the reader gates
 * frame-presence identically to the writer). Development/regression only. */
typedef struct
{
    Tuplestorestate *ts;
    TupleDesc        tupdesc;
    uint32           seg;
} SegPosDebugState;

/* pos_cb: one row per decoded position. */
static void
seg_pos_debug_cb(uint32 local_docid, uint32 field_id, const uint32 *positions,
                 uint32 npos, void *state)
{
    SegPosDebugState *st = (SegPosDebugState *) state;
    uint32            k;

    for (k = 0; k < npos; k++)
    {
        Datum   vals[4];
        bool    nulls[4] = {0};

        vals[0] = Int32GetDatum((int32) st->seg);
        vals[1] = Int64GetDatum((int64) local_docid);
        vals[2] = Int32GetDatum((int32) field_id);
        vals[3] = Int32GetDatum((int32) positions[k]);
        tuplestore_putvalues(st->ts, st->tupdesc, vals, nulls);
    }
}

/* post_cb: no-op -- bm25_seg_scan_postings requires a post cb, but the SRF only wants
 * the pos_cb rows. */
static void
seg_pos_debug_post_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    (void) local_docid;
    (void) tf;
    (void) field_id;
    (void) state;
}

PG_FUNCTION_INFO_V1(bm25_debug_seg_positions);
Datum
bm25_debug_seg_positions(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    text               *qterm = PG_GETARG_TEXT_PP(1);
    Relation            index;
    BM25MetaPageData    meta;
    BM25FieldConfigHeader fchdr;
    BM25FieldConfig     fcfg[BM25_MAX_FIELDS];
    uint8               store_pos_bytes[BM25_MAX_FIELDS];
    bool                store_pos[BM25_MAX_FIELDS];
    uint32              fc, f;
    BM25SegCatEntry    *segs;
    uint32              nsegs, i;
    SegPosDebugState    st;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &st.tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    index = bm25_index_open_readable(relid, AccessShareLock);
    st.tupdesc = BlessTupleDesc(st.tupdesc);
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        st.ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = st.ts;
    rsi->setDesc = st.tupdesc;

    /* Resolve the per-field store_positions gate from the field-config page. When the
     * trailing flag array is absent (a pre-M4 index), default every field on -- such
     * an index also has pos_root Invalid, so the gate is never consulted anyway. */
    bm25_meta_read(index, &meta);
    fc = 0;
    if (meta.field_config_blkno != InvalidBlockNumber)
    {
        for (f = 0; f < BM25_MAX_FIELDS; f++)
            store_pos_bytes[f] = 1;             /* default on if flag array absent */
        bm25_fieldcfg_read(index, meta.field_config_blkno, &fchdr, fcfg, store_pos_bytes);
        fc = fchdr.field_count;
    }
    for (f = 0; f < BM25_MAX_FIELDS; f++)
        store_pos[f] = (f < fc) ? (store_pos_bytes[f] != 0) : true;

    bm25_segcat_read(index, &segs, &nsegs);
    for (i = 0; i < nsegs; i++)
    {
        BM25SegmentHeader   h;
        BlockNumber         post_root, pos_post_root;
        uint16              post_off, pos_post_off;
        uint32              df;

        bm25_seg_header_read(index, segs[i].header_blkno, segs[i].gen, &h);
        st.seg = i;
        if (bm25_seg_dict_lookup(index, &h, VARDATA_ANY(qterm),
                                 VARSIZE_ANY_EXHDR(qterm),
                                 &post_root, &post_off, &df,
                                 &pos_post_root, &pos_post_off))
            bm25_seg_scan_postings(index, post_root, post_off, df, h.gen,
                                   seg_pos_debug_post_cb, &st,
                                   pos_post_root, pos_post_off,
                                   seg_pos_debug_cb, &st,
                                   store_pos, h.field_count);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* -------------------------------------------------------------------------
 * bm25_debug_stamp_seg_field_count(index regclass, seg int, field_count int)
 * -- TEST-ONLY corruption lever.
 *
 * WAL-logs an arbitrary field_count onto one sealed segment's header page, so a
 * regression suite can drive the decode boundaries that are ONLY reachable from
 * an already-corrupt page. "A regression suite cannot produce a corrupt page" is
 * the stated reason sql/69, sql/78 and sql/79 test their validators as pure
 * functions over caller-supplied values instead; that works for a validator, but
 * not for the question this lever exists to answer, which is whether a validator
 * is actually CALLED on a given path. bm25_segheader_read_lenfields drives
 * field_count as a loop bound over caller arrays sized BM25_MAX_FIELDS, and the
 * only way to prove it now rejects an out-of-range one is to hand it a page
 * carrying it.
 *
 * A raw poke by design: no bm25_segheader_validate here, or the lever could not
 * write the value under test. Ownership + AM identity are enforced by
 * bm25_index_open_owned, and the install script's name-matching loop REVOKEs it
 * from PUBLIC like every other bm25_debug_* function.
 *
 * The poke leaves the SEGMENT unreadable by construction -- every later read of
 * it raises ERRCODE_INDEX_CORRUPTED, which is the point -- so a caller must drop
 * the table afterwards rather than continue against it. Modifying the struct in
 * place needs no pd_lower change: it sits below pd_lower already, outside the
 * page hole GenericXLogFinish zeroes.
 * -------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(bm25_debug_stamp_seg_field_count);
Datum
bm25_debug_stamp_seg_field_count(PG_FUNCTION_ARGS)
{
    Oid                 relid  = PG_GETARG_OID(0);
    int32               segidx = PG_GETARG_INT32(1);
    int32               fcount = PG_GETARG_INT32(2);
    Relation            index  = bm25_index_open_owned(relid, RowExclusiveLock);
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    Buffer              buf;
    Page                page;
    GenericXLogState   *state;
    BM25SegmentHeader  *h;

    bm25_segcat_read(index, &segs, &nsegs);
    if (segidx < 0 || (uint32) segidx >= nsegs)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_stamp_seg_field_count: segment %d out of range (%u live)",
                        segidx, nsegs)));

    /* This lever takes a page EXCLUSIVE off an unvalidated catalog entry, which makes
     * it the sharpest instance of the shape the non-debug readers now gate -- and a
     * write, not a read. Gate the pointer, and confirm the target really is a segment
     * header page before poking it: the point of this function is to corrupt ONE field
     * of a well-formed header, not to write field_count into whatever page a bad
     * catalog entry happens to name. */
    bm25_seg_blkno_validate(segs[segidx].header_blkno, "segment header block");
    buf = ReadBuffer(index, segs[segidx].header_blkno);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    /* Validated before the window opens (issue #313 SURFACE-12, ADR 0083), as the
     * sibling levers do: the registered copy is taken under the same lock. */
    bm25_seg_page_validate_kind(BufferGetPage(buf), segs[segidx].gen, BM25_PAGE_SEGCAT);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    h = (BM25SegmentHeader *) PageGetContents(page);
    h->field_count = (uint32) fcount;       /* the deliberate corruption this lever exists for */
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
    index_close(index, RowExclusiveLock);
    PG_RETURN_VOID();
}

/* -------------------------------------------------------------------------
 * bm25_debug_stamp_chain_next(index regclass, chain text, seg int, page int,
 *                             nextblk bigint) RETURNS bigint
 * -- TEST-ONLY corruption lever (issue #225).
 *
 * WAL-logs an arbitrary nextblk onto the page-th page (0 = the root) of a chain, and
 * returns the block it stamped. bm25_debug_seg_chain_extent reaches the walkers ADR
 * 0095 bounded by substituting a ROOT in a private header copy, which works because
 * those walkers take their root from a BM25SegmentHeader the probe can copy. The SEGCAT
 * walkers take theirs from the metapage under its own lock, so a root substitution
 * would have to reimplement the walker; and the point of the bound is the LINK a walker
 * follows mid-chain, which only a real page can carry. So this writes one, the same
 * way bm25_debug_stamp_seg_field_count writes a real corrupt header.
 *
 * chain = 'segcat': the catalog chain from the metapage's segcat_root; `seg` must be
 * -1 (the catalog belongs to no segment, and a value here would suggest otherwise).
 * chain = 'keymap': segment `seg`'s KEYMAP chain, walked at that segment's gen.
 * chain = 'pending': the pending list from the metapage's pending_head; `seg` must be
 * -1, as for segcat. Added for issue #243, whose sweep bound had no real corrupt link
 * to meet. Pending pages carry no segment gen, so the kind check runs with gen 0.
 * chain = 'post', 'live', 'docmap' or 'norms': segment `seg`'s shared POST chain or
 * its dense per-docid chains, walked at that segment's gen like 'keymap'. Added for
 * issues #293/#294, whose walkers must ERROR on a chain that ends early.
 *
 * The walk to the target page is itself bounded and kind-checked -- the lever must not
 * write a nextblk into whatever page an already-corrupt chain names. The target page is
 * re-validated under the EXCLUSIVE lock it is written under. The chain it corrupts
 * stays corrupt: the index is unusable for anything that walks it, so the caller drops
 * the table afterwards. Ownership + AM identity via bm25_index_open_owned; REVOKEd from
 * PUBLIC by the install script's loop.
 * -------------------------------------------------------------------------*/
/* The levers' kind check. bm25_seg_page_validate_kind's message says "segment page",
 * which is wrong for the pending chain -- pending pages belong to no segment -- so that
 * chain gets its own wording here. The test is the same `(flags & kind) == 0`, never
 * equality (bm25_page_mark_deleted ORs BM25_PAGE_DELETED onto a live kind), the errcode
 * is the same, and the pending chain is walked with gen 0, so there is no gen half to
 * run. The SEGCAT and KEYMAP chains keep the shared check unchanged. */
static void
stamp_validate_kind(Page page, uint32 gen, uint16 kind)
{
    uint16      flags;

    if (kind != BM25_PAGE_PENDING)
    {
        bm25_seg_page_validate_kind(page, gen, kind);
        return;
    }
    flags = BM25PageGetOpaque(page)->flags;
    if ((flags & kind) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending page is not of the expected kind"),
                 errdetail("The page's flags are 0x%x; expected one of 0x%x.", flags, kind)));
}

/* Chains a lever can address: the two index-wide ones, and the segment-scoped ones,
 * which take a segment number. */
static bool
stamp_chain_is_segment(const char *chain)
{
    return strcmp(chain, "keymap") == 0 || strcmp(chain, "post") == 0 ||
           strcmp(chain, "live") == 0 || strcmp(chain, "docmap") == 0 ||
           strcmp(chain, "norms") == 0;
}

/* Argument checks every chain lever shares, run before the index is opened
 * (97_debug_probe_arguments). */
static void
stamp_chain_args_validate(const char *fn, const char *chain, int32 seg, int32 pageno)
{
    if (pageno < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: page %d is negative", fn, pageno)));
    if (strcmp(chain, "segcat") != 0 && strcmp(chain, "pending") != 0 &&
        !stamp_chain_is_segment(chain))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: unknown chain \"%s\"", fn, chain),
                 errhint("Use segcat, pending, keymap, post, live, docmap or norms.")));
    if (!stamp_chain_is_segment(chain) && seg != -1)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: seg must be -1 for chain %s", fn, chain)));
    if (stamp_chain_is_segment(chain) && seg < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: seg %d is negative", fn, seg)));
}

/* The walk both levers share: from the chain's root to its page-th page, returning that
 * page's block and the gen and kind it must carry. Bounded (extent) and kind-checked at
 * every hop, so a lever never writes into whatever page an already-corrupt chain names.
 * `fn` names the calling lever in the one error this raises of its own. */
static BlockNumber
stamp_chain_target(Relation index, const char *fn, const char *chain, int32 seg,
                   int32 pageno, uint32 *gen, uint16 *kind)
{
    BlockNumber         blk;
    BlockNumber         nblocks = RelationGetNumberOfBlocks(index);
    int32               i;

    *gen = 0;
    if (strcmp(chain, "segcat") == 0)
    {
        BM25MetaPageData meta;

        bm25_meta_read(index, &meta);
        blk = meta.segcat_root;
        *kind = BM25_PAGE_SEGCAT;
    }
    else if (strcmp(chain, "pending") == 0)
    {
        BM25MetaPageData meta;

        bm25_meta_read(index, &meta);
        blk = meta.pending_head;
        *kind = BM25_PAGE_PENDING;
    }
    else
    {
        BM25SegCatEntry    *segs;
        uint32              nsegs;
        BM25SegmentHeader   h;

        bm25_segcat_read(index, &segs, &nsegs);
        if ((uint32) seg >= nsegs)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
        bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);
        *gen = h.gen;
        if (strcmp(chain, "post") == 0)
        {
            blk = h.posts_root;
            *kind = BM25_PAGE_POST;
        }
        else if (strcmp(chain, "live") == 0)
        {
            blk = h.livedocs_root;
            *kind = BM25_PAGE_LIVE;
        }
        else if (strcmp(chain, "docmap") == 0)
        {
            blk = h.docmap_root;
            *kind = BM25_PAGE_DOCMAP;
        }
        else if (strcmp(chain, "norms") == 0)
        {
            blk = h.norms_root;
            *kind = BM25_PAGE_NORMS;
        }
        else
        {
            blk = h.keymap_root;
            *kind = BM25_PAGE_KEYMAP;
        }
    }

    for (i = 0; ; i++)
    {
        Buffer      buf;
        BlockNumber next;

        if (blk == InvalidBlockNumber)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("%s: %s chain has only %d page(s)", fn, chain, i)));
        /* Extent only, not the helper's block-0 clause (issue #303): sql/113 forges a
         * link to the metapage and then aims the lever through it, to reach the kind
         * checks below on block 0. */
        if (blk >= nblocks)
            bm25_seg_chain_extent_validate(blk, nblocks, "chain being stamped");
        if (i == pageno)
            break;
        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        stamp_validate_kind(BufferGetPage(buf), *gen, *kind);
        next = BM25PageGetOpaque(BufferGetPage(buf))->nextblk;
        UnlockReleaseBuffer(buf);
        CHECK_FOR_INTERRUPTS();
        blk = next;
    }
    return blk;
}

PG_FUNCTION_INFO_V1(bm25_debug_stamp_chain_next);
Datum
bm25_debug_stamp_chain_next(PG_FUNCTION_ARGS)
{
    Oid                 relid   = PG_GETARG_OID(0);
    char               *chain   = text_to_cstring(PG_GETARG_TEXT_PP(1));
    int32               seg     = PG_GETARG_INT32(2);
    int32               pageno  = PG_GETARG_INT32(3);
    int64               nextblk = PG_GETARG_INT64(4);
    Relation            index;
    BlockNumber         blk;
    uint16              kind;
    uint32              gen;
    Buffer              buf;
    Page                page;
    GenericXLogState   *state;

    /* Arguments before the index is opened (97_debug_probe_arguments). The full uint32
     * range is allowed for nextblk: InvalidBlockNumber (truncating the chain) is as
     * legitimate a thing to forge as a block past the extent. */
    if (nextblk < 0 || nextblk > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_stamp_chain_next: nextblk " INT64_FORMAT
                        " is not a block number", nextblk)));
    stamp_chain_args_validate("bm25_debug_stamp_chain_next", chain, seg, pageno);

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    blk = stamp_chain_target(index, "bm25_debug_stamp_chain_next", chain, seg, pageno,
                             &gen, &kind);

    buf = ReadBuffer(index, blk);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    /* Validated before the window opens, so a wrong page raises with no record open. */
    stamp_validate_kind(BufferGetPage(buf), gen, kind);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    BM25PageGetOpaque(page)->nextblk = (BlockNumber) nextblk;   /* the deliberate corruption */
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
    index_close(index, RowExclusiveLock);
    PG_RETURN_INT64((int64) blk);
}

/* -------------------------------------------------------------------------
 * bm25_debug_stamp_segcat_empty(index regclass, page int) RETURNS bigint
 * -- TEST-ONLY corruption lever (issue #244).
 *
 * WAL-logs pd_lower back to the page header on the page-th page (0 = the root) of the
 * segment catalog, so the page reads as holding no entries while keeping its nextblk,
 * and returns the block. meta.nsegs is left alone, so the catalog now claims entries it
 * does not have. That is the zero-entry-page shape the SEGCAT walkers must reject when
 * the page links onward; paired with bm25_debug_stamp_chain_next it also forges an
 * empty page that links to itself. No SQL path writes such a page (every published
 * catalog page but a chain's last carries an entry, see segcat_page_nentries in
 * bm25_seg_read.c), which is why this lever exists. Same walk, validation, ownership
 * gate and REVOKE as bm25_debug_stamp_chain_next; drop the table after using it.
 * -------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(bm25_debug_stamp_segcat_empty);
Datum
bm25_debug_stamp_segcat_empty(PG_FUNCTION_ARGS)
{
    Oid                 relid   = PG_GETARG_OID(0);
    int32               pageno  = PG_GETARG_INT32(1);
    Relation            index;
    BlockNumber         blk;
    uint16              kind;
    uint32              gen;
    Buffer              buf;
    Page                page;
    GenericXLogState   *state;

    if (pageno < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_stamp_segcat_empty: page %d is negative", pageno)));

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    blk = stamp_chain_target(index, "bm25_debug_stamp_segcat_empty", "segcat", -1, pageno,
                             &gen, &kind);

    buf = ReadBuffer(index, blk);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    bm25_seg_page_validate_kind(BufferGetPage(buf), gen, kind);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    ((PageHeader) page)->pd_lower = SizeOfPageHeaderData;      /* the deliberate corruption */
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
    index_close(index, RowExclusiveLock);
    PG_RETURN_INT64((int64) blk);
}

/* -------------------------------------------------------------------------
 * Validated-walker levers (issues #293/#294). Each WAL-logs one forged value onto a
 * real page of a segment chain, so the suites can show the POST, LIVE, DOCMAP and NORMS
 * walkers raising on shapes no SQL path writes. Same contract as
 * bm25_debug_stamp_chain_next: arguments checked before the index is opened, the
 * target walk bounded and kind-checked by stamp_chain_target, the target page
 * re-validated under the EXCLUSIVE lock it is written under, index owner only, REVOKEd
 * from PUBLIC; the index is left corrupt, so drop it afterwards.
 * -------------------------------------------------------------------------*/

/* Open the record on the page-th page of `chain` and return the page image to write.
 * The caller writes, then calls stamp_finish. */
static Page
stamp_begin(Relation index, const char *fn, const char *chain, int32 seg, int32 pageno,
            Buffer *buf, BlockNumber *blk, GenericXLogState **state)
{
    uint16      kind;
    uint32      gen;

    *blk = stamp_chain_target(index, fn, chain, seg, pageno, &gen, &kind);
    *buf = ReadBuffer(index, *blk);
    LockBuffer(*buf, BUFFER_LOCK_EXCLUSIVE);
    stamp_validate_kind(BufferGetPage(*buf), gen, kind);
    *state = GenericXLogStart(index);
    return GenericXLogRegisterBuffer(*state, *buf, 0);
}

static void
stamp_finish(Buffer buf, GenericXLogState *state)
{
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
}

/* bm25_debug_stamp_chain_lower(index regclass, chain text, seg int, page int,
 *                              content_bytes int) RETURNS bigint
 * Sets the page-th page's pd_lower so it holds content_bytes content bytes, keeping
 * its nextblk (and zeroing what a shrink cuts off): a short non-final page of a
 * dense chain, or a final page that stops before the bytes ndocs implies. Returns
 * the block stamped. */
PG_FUNCTION_INFO_V1(bm25_debug_stamp_chain_lower);
Datum
bm25_debug_stamp_chain_lower(PG_FUNCTION_ARGS)
{
    Oid                 relid  = PG_GETARG_OID(0);
    char               *chain  = text_to_cstring(PG_GETARG_TEXT_PP(1));
    int32               seg    = PG_GETARG_INT32(2);
    int32               pageno = PG_GETARG_INT32(3);
    int32               nbytes = PG_GETARG_INT32(4);
    const char         *fn = "bm25_debug_stamp_chain_lower";
    Size                cap = BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BM25PageOpaque));
    Relation            index;
    BlockNumber         blk;
    Buffer              buf;
    GenericXLogState   *state;
    Page                page;

    stamp_chain_args_validate(fn, chain, seg, pageno);
    if (nbytes < 0 || (Size) nbytes > cap)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: content_bytes %d is outside [0, %zu]", fn, nbytes, cap)));

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    page = stamp_begin(index, fn, chain, seg, pageno, &buf, &blk, &state);
    /* Shrinking also zeroes the bytes cut off, so the page looks like one whose
     * writer stopped short (the hole a real short page has) rather than one whose
     * bytes are merely hidden. */
    if (SizeOfPageHeaderData + nbytes < ((PageHeader) page)->pd_lower)
        memset((char *) page + SizeOfPageHeaderData + nbytes, 0,
               ((PageHeader) page)->pd_lower - (SizeOfPageHeaderData + nbytes));
    ((PageHeader) page)->pd_lower = SizeOfPageHeaderData + nbytes;  /* the corruption */
    stamp_finish(buf, state);
    index_close(index, RowExclusiveLock);
    PG_RETURN_INT64((int64) blk);
}

/* bm25_debug_stamp_post_block(index regclass, seg int, page int, block int,
 *                             field text, value bigint) RETURNS bigint
 * Overwrites one header field of the block-th posting block (0 = the first) on the
 * page-th page of segment seg's POST chain: field 'ndocs' (1..BM25_POSTINGS_PER_BLOCK,
 * so the header still passes bm25_block_validate's own bounds) or 'last_docid'. The
 * blocks before it are stepped over with bm25_block_validate, the readers' own
 * decode boundary, so the lever never writes into something that is not a block.
 * Returns the block number stamped. */
PG_FUNCTION_INFO_V1(bm25_debug_stamp_post_block);
Datum
bm25_debug_stamp_post_block(PG_FUNCTION_ARGS)
{
    Oid                 relid  = PG_GETARG_OID(0);
    int32               seg    = PG_GETARG_INT32(1);
    int32               pageno = PG_GETARG_INT32(2);
    int32               which  = PG_GETARG_INT32(3);
    char               *field  = text_to_cstring(PG_GETARG_TEXT_PP(4));
    int64               value  = PG_GETARG_INT64(5);
    const char         *fn = "bm25_debug_stamp_post_block";
    Relation            index;
    BlockNumber         blk;
    Buffer              buf;
    GenericXLogState   *state;
    Page                page;
    char               *cur,
                       *pend;
    BM25BlockHeader     hdr;
    int32               i;

    stamp_chain_args_validate(fn, "post", seg, pageno);
    if (which < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: block %d is negative", fn, which)));
    if (strcmp(field, "ndocs") == 0)
    {
        if (value < 1 || value > BM25_POSTINGS_PER_BLOCK)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("%s: ndocs " INT64_FORMAT " is outside [1, %d]",
                            fn, value, BM25_POSTINGS_PER_BLOCK)));
    }
    else if (strcmp(field, "last_docid") == 0)
    {
        if (value < 0 || value > (int64) PG_UINT32_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("%s: last_docid " INT64_FORMAT " is not a docid", fn, value)));
    }
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: unknown field \"%s\"", fn, field),
                 errhint("Use ndocs or last_docid.")));

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    page = stamp_begin(index, fn, "post", seg, pageno, &buf, &blk, &state);
    cur = (char *) PageGetContents(page);
    pend = cur + bm25_page_content_bytes(page);
    for (i = 0; ; i++)
    {
        if (cur + sizeof(BM25BlockHeader) > pend)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("%s: page %d of the postings chain holds only %d block(s)",
                            fn, pageno, i)));
        memcpy(&hdr, cur, sizeof(hdr));
        if (i == which)
            break;
        cur += bm25_block_validate(&hdr, cur, pend);
    }
    if (strcmp(field, "ndocs") == 0)
        hdr.ndocs = (uint16) value;                 /* the corruption */
    else
        hdr.last_docid = (uint32) value;            /* the corruption */
    memcpy(cur, &hdr, sizeof(hdr));
    stamp_finish(buf, state);
    index_close(index, RowExclusiveLock);
    PG_RETURN_INT64((int64) blk);
}

/* bm25_debug_stamp_docmap_tid(index regclass, seg int, docid bigint, tid tid)
 *   RETURNS bigint
 * Overwrites segment seg's DOCMAP cell for docid with an arbitrary TID, through an
 * intact chain: the corrupt-cell shape no chain check can see, which the docid -> TID
 * chokepoint must reject. The cell is located with the writer's full-page span, the
 * same addressing every reader uses on a healthy chain. Returns the block stamped. */
PG_FUNCTION_INFO_V1(bm25_debug_stamp_docmap_tid);
Datum
bm25_debug_stamp_docmap_tid(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               seg   = PG_GETARG_INT32(1);
    int64               docid = PG_GETARG_INT64(2);
    ItemPointer         tid   = PG_GETARG_ITEMPOINTER(3);
    const char         *fn = "bm25_debug_stamp_docmap_tid";
    Size                per = bm25_chain_full_span(BM25_PAGE_DOCMAP) / sizeof(ItemPointerData);
    Size                off;
    Relation            index;
    BlockNumber         blk;
    Buffer              buf;
    GenericXLogState   *state;
    Page                page;

    if (docid < 0 || docid > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: docid " INT64_FORMAT " is not a docid", fn, docid)));
    stamp_chain_args_validate(fn, "docmap", seg, (int32) ((uint64) docid / per));

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    page = stamp_begin(index, fn, "docmap", seg, (int32) ((uint64) docid / per),
                       &buf, &blk, &state);
    off = ((uint64) docid % per) * sizeof(ItemPointerData);
    if (off + sizeof(ItemPointerData) > bm25_page_content_bytes(page))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: docid " INT64_FORMAT " is past the doc-map chain", fn, docid)));
    memcpy((char *) PageGetContents(page) + off, tid, sizeof(ItemPointerData));  /* the corruption */
    stamp_finish(buf, state);
    index_close(index, RowExclusiveLock);
    PG_RETURN_INT64((int64) blk);
}

/* bm25_debug_stamp_seg_root(index regclass, seg int, chain text, root bigint)
 *   RETURNS bigint
 * Overwrites segment seg's header root for chain 'live', 'docmap' or 'norms' and
 * returns the old root. Unlike bm25_debug_seg_chain_extent, which substitutes a root
 * in a private header copy AFTER the header was read, this writes the page, so the
 * forged root meets bm25_segheader_validate on every read. */
PG_FUNCTION_INFO_V1(bm25_debug_stamp_seg_root);
Datum
bm25_debug_stamp_seg_root(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               seg   = PG_GETARG_INT32(1);
    char               *chain = text_to_cstring(PG_GETARG_TEXT_PP(2));
    int64               root  = PG_GETARG_INT64(3);
    const char         *fn = "bm25_debug_stamp_seg_root";
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;
    BM25SegmentHeader  *ph;
    BlockNumber        *field;
    BlockNumber         old;
    Buffer              buf;
    GenericXLogState   *state;
    Page                page;

    if (root < 0 || root > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: root " INT64_FORMAT " is not a block number", fn, root)));
    if (strcmp(chain, "live") != 0 && strcmp(chain, "docmap") != 0 &&
        strcmp(chain, "norms") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: unknown chain \"%s\"", fn, chain),
                 errhint("Use live, docmap or norms.")));
    if (seg < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: seg %d is negative", fn, seg)));

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    bm25_segcat_read(index, &segs, &nsegs);
    if ((uint32) seg >= nsegs)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    /* The validated read first, so the lever only ever writes a real header page. */
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);

    buf = ReadBuffer(index, segs[seg].header_blkno);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    bm25_seg_page_validate_kind(BufferGetPage(buf), segs[seg].gen, BM25_PAGE_SEGCAT);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    ph = (BM25SegmentHeader *) PageGetContents(page);
    field = strcmp(chain, "live") == 0 ? &ph->livedocs_root :
            strcmp(chain, "docmap") == 0 ? &ph->docmap_root : &ph->norms_root;
    old = *field;
    *field = (BlockNumber) root;                    /* the corruption */
    stamp_finish(buf, state);
    index_close(index, RowExclusiveLock);
    PG_RETURN_INT64((int64) old);
}

/* bm25_debug_livedocs_clear(index regclass, seg int, docid bigint) RETURNS void
 * Tombstones one document the way bm25_bulkdelete does -- under the seal/merge
 * singleton ShareLock, through bm25_livedocs_clear -- but WITHOUT bulkdelete's
 * per-document liveness read first. That read goes through chain_read_at, which now
 * rejects a short LIVE chain itself, so bulkdelete never reaches the write path's own
 * checks on one: bm25_livedocs_locate's full-span test on the pages it walks past and
 * bm25_livedocs_clear's byte_off bound (issue #294). They are defence in depth, and
 * this lever is how a suite reaches them. Writes a real tombstone; owner only. */
PG_FUNCTION_INFO_V1(bm25_debug_livedocs_clear);
Datum
bm25_debug_livedocs_clear(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               seg   = PG_GETARG_INT32(1);
    int64               docid = PG_GETARG_INT64(2);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;

    if (seg < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_livedocs_clear: seg %d is negative", seg)));
    if (docid < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_livedocs_clear: docid " INT64_FORMAT " is negative",
                        docid)));

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    LockPage(index, BM25_METAPAGE_BLKNO, ShareLock);   /* bm25_bulkdelete's hold */
    bm25_segcat_read_locked(index, &segs, &nsegs);
    if ((uint32) seg >= nsegs)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);
    if ((uint64) docid >= h.ndocs)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_livedocs_clear: docid " INT64_FORMAT
                        " out of range for segment %d (" UINT64_FORMAT " documents)",
                        docid, seg, h.ndocs)));
    bm25_livedocs_clear(index, segs[seg].header_blkno, (uint32) docid);
    UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
    index_close(index, RowExclusiveLock);
    PG_RETURN_VOID();
}

/* bm25_debug_tombstone(index regclass) -- one row per local doc-id across all
 * sealed segments, tagged with the segment generation and live/dead state. Walks
 * the catalog, reads each segment header (validating its gen via option (d)), and
 * tests every bit via bm25_seg_doc_is_live. Exercises bm25_livedocs_clear's effect:
 * after seal every doc is live; VACUUM tombstones flip bits to dead.
 * Development/regression introspection only; not on the query path. */
PG_FUNCTION_INFO_V1(bm25_debug_tombstone);
Datum
bm25_debug_tombstone(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid              relid = PG_GETARG_OID(0);
    Relation         index;
    Tuplestorestate *ts;
    TupleDesc        tupdesc;
    BM25SegCatEntry *segs;
    uint32           nsegs,
                     s;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    index = bm25_index_open_readable(relid, AccessShareLock);
    tupdesc = BlessTupleDesc(tupdesc);
    /* C7: the Materialize-mode tuplestore MUST live in the per-query context, not
     * the (short-lived) current context, or PG 18 raises "invalid tuplestore
     * state" when the executor drains it after this call returns. Mirrors
     * bm25_debug_segterms / bm25_debug_segcat / bm25_debug_seg_postings above. */
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    bm25_segcat_read(index, &segs, &nsegs);
    for (s = 0; s < nsegs; s++)
    {
        BM25SegmentHeader   h;
        /* Ascending over every local docid, the same shape bm25_bulkdelete's sweep has,
         * so it reads the LIVEDOCS chain the same way: one forward cursor per segment
         * instead of a root re-walk (and a SEGREAD-11 extent capture) per document.
         * Declared inside the segment loop, with h, so it cannot outlive its segment. */
        BM25SegReader       rdr;
        uint64              d;

        bm25_seg_header_read(index, segs[s].header_blkno, segs[s].gen, &h);
        bm25_seg_reader_init(&rdr, index, &h);
        for (d = 0; d < h.ndocs; d++)
        {
            Datum   vals[3];
            bool    nulls[3] = {0};

            /* Per document (#305 SURFACE-11): a segment can hold millions, each
             * a LIVEDOCS read and a tuplestore row that may spill to disk. */
            CHECK_FOR_INTERRUPTS();

            vals[0] = Int32GetDatum((int32) segs[s].gen);
            vals[1] = Int64GetDatum((int64) d);
            vals[2] = BoolGetDatum(bm25_seg_reader_doc_is_live(&rdr, (uint32) d));
            tuplestore_putvalues(ts, tupdesc, vals, nulls);
        }
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}
