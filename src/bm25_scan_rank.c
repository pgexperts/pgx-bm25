/* bm25_scan_rank.c -- the ranked-scan builders: the scan-start corpus prologue,
 * the exhaustive multi-source OR-sum scorer, and the dispatcher that chooses
 * between it and the Block-Max WAND driver, behind the bounded retry that is the
 * public entry point (bm25_scan_build_ranking).
 *
 * Split out of bm25_scan.c (#228, ADR 0101) as a pure code move; nothing here
 * changed behaviour. What lives here:
 *   - the corpus prologue every ranked path runs: bm25_scan_corpus_stats, its
 *     static pending walkers, and bm25_field_corpus_stats (bm25_scan_corpus_stats
 *     and bm25_field_corpus_stats are declared in bm25_scan.h because
 *     bm25_debug.c's probes call them);
 *   - bm25_scan_build_ranking_exhaustive with its posting callback
 *     (seg_posting_cb, TermScoreCtx) and the phrase-position gates it consults;
 *   - bm25_scan_build_ranking_once (the D7 dispatch) and bm25_scan_build_ranking
 *     (the subtransaction retry wrapper, declared in bm25.h; a single direct call
 *     on a hot standby, #307);
 *   - bm25_scan_build_filtered (#290), the dispatch's branch for a scan with WHERE
 *     keys of its own to apply: every set from the exhaustive scorer under one
 *     snapshot, composed into the SQL answer.
 * The exhaustive scorer and the D7 dispatch read bm25_qtree_is_multileaf, a static
 * inline in bm25_scan.h that bm25_load_if_needed in bm25_scan.c reads too. The flow of both builders, and the
 * contract they share, is described in bm25_scan.c's header.
 *
 * The phrase/proximity stash and the AND-of-terms fallback the exhaustive scorer
 * drives are in bm25_scan_match.c; the statistics layer it shares with the WAND
 * driver is in bm25_stats.c. Calls run one way: from here into bm25_scan_match.c,
 * bm25_wand.c and bm25_stats.c, and none of those call back into this file.
 */
#include "postgres.h"

#include "bm25.h"
#include "bm25_scan.h"          /* the corpus helpers defined here; the match layer */
#include "bm25_stats.h"         /* the statistics layer shared with bm25_wand.c */
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "utils/hsearch.h"
#include "access/xact.h"        /* BeginInternalSubTransaction / Release / Rollback */
#include "access/xlog.h"        /* RecoveryInProgress (the #307 standby bypass) */
#include "utils/resowner.h"     /* CurrentResourceOwner (restored across the retry) */
#include "bm25_wand.h"          /* M2b Task 4/5: BM25WandCtx/bm25_block_ub, BM25TopK top-k heap */
#include "bm25_query.h"         /* M6: BM25Query AST + bm25_query_flatten/eval */
#include "utils/float.h"        /* get_float8_infinity (#290 unmatched WHERE rows) */

/* -------------------------------------------------------------------------
 * BM25 exhaustive OR-sum scorer
 * -------------------------------------------------------------------------
 * Several of this scorer's types and helpers are in bm25_stats.h (#67.13, ADR
 * 0093). The TID-keyed accumulator entry BM25AccEnt and its upsert
 * bm25_scores_add moved because the WAND driver's pending arm fills the same
 * accumulator. The match-set memory budget from #62.5 (BM25MatchBudget,
 * bm25_match_budget_init, bm25_match_charge) moved because bm25_scores_add
 * charges it. The sort element BM25ExhScored moved because the budget's
 * per-document charge counts one slot of it. The comment explaining why the
 * budget counts bytes, shared across every structure a scan builds, is with its
 * definitions in bm25_stats.c. The two charges that stayed in the scanner are
 * defined beside what they charge for: the leaf-presence hash's in
 * bm25_scan_match.c, the @@@ union collector's in bm25_scan.c.
 */

/* Posting callback used by the multi-source scorer: for each (local_docid, tf) of
 * a query term in a sealed segment, gate on the live-docs bit, dedupe against the
 * pending set, resolve the TID, read the per-doc length from NORMS, and accumulate
 * bm25_termscore into the M1 dynahash. Only the doclen source (segment NORMS), the
 * live-docs gate, and the pending dedupe differ from the M1 inline accumulation.
 *
 * Dedupe (Task 12): if the resolved TID is already in c->pending_tids, the pending
 * list already scored it, so skip the segment copy and score the TID exactly once
 * -- pending wins. What this covers is ONE heap TID reachable from both sources at
 * once: a pending entry plus a not-yet-tombstoned segment posting for that SAME
 * TID. It is not the old and new versions of an UPDATEd row -- an UPDATE writes a
 * new heap TID, so those are different keys and never meet here. See
 * bm25_scan_build_ranking_exhaustive's header for the full argument. This is
 * a no-op when c->pending_tids is NULL (no pending source). */
/* BM25F term-scoring context (C3). The scorer is unified over single- and
 * multi-field: it ALWAYS resolves per-field stats by the posting's field_id.
 *   idf_f[f]   -- per-field idf (bm25_idf(N_field, df_field)); df_field is the
 *                RLE-partitioned per-field df, N_field the header count.
 *   avgdl_f[f] -- total_len_by_field[f] / N_field[f].
 *   fcfg[f]    -- k1/b/boost for field f (BM25FieldConfig from the field-config page).
 * A single-field index collapses exactly to the M3 formula: the caller fills
 * idf_f[0]/avgdl_f[0] from the metapage-cached (tombstone-adjusted) corpus stats
 * and fcfg[0].boost == 1.0, so the contribution is boost*termscore == the old
 * single-idf/single-avgdl term. field_id selects which stats; the math is
 * field-agnostic (bm25_idf/bm25_termscore unchanged). */
typedef struct
{
    Relation                index;
    BM25SegmentHeader      *seg;
    BlockNumber             seg_hdr;    /* M5: header_blkno of *seg, for docid->key */
    const double           *idf_f;      /* [field_count] */
    const double           *avgdl_f;    /* [field_count] */
    const BM25FieldConfig  *fcfg;       /* [field_count]: k1, b, boost */
    HTAB                   *scores;         /* M1 TID-keyed dynahash */
    HTAB                   *pending_tids;   /* TIDs already scored from pending, or NULL */
    BM25MatchBudget        *budget;         /* #62.5 shared materialization total */
    /* H16: forward cursors over this segment's LIVEDOCS/DOCMAP/NORMS chains. Postings
     * arrive in ascending (docid, field_id) order (D-ACCUM), so all three lookups below
     * resume from the page the previous posting landed on instead of re-walking from the
     * root. Lives here by value: the ctx is per (term, segment), the same scope as the
     * header it reads through. */
    BM25SegReader           rdr;
} TermScoreCtx;

static void
seg_posting_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    TermScoreCtx   *c = (TermScoreCtx *) state;
    ItemPointerData tid;
    uint32          doclen_f;
    double          contrib;

    /* field_id is decoded off this segment's posting-block field RLE; it is not
     * derived here, and bm25_seg_page_validate does not constrain it (it compares
     * seg_gen only). The old comment here claimed both the opposite provenance and
     * a memory-safety stake it does not have, so state what is actually true:
     *
     *   - NOT a bounds problem. idf_f, avgdl_f and fcfg are all BM25_MAX_FIELDS
     *     wide at every caller, and bm25_field_rle_decode already rejects an id
     *     >= BM25_MAX_FIELDS, so every read below is in bounds even for a corrupt
     *     stream. There is no OOB read here.
     *   - IS a correctness problem. The decoder never bounds the id against THIS
     *     segment's field_count, so an id in the field_count..BM25_MAX_FIELDS gap
     *     passes it, picks up a field's stats the segment does not have, and (via
     *     bm25_seg_reader_doclen_field, where field_count is the NORMS row stride)
     *     scores this document against ANOTHER DOCUMENT's field length. In a
     *     production build the Assert that was supposed to catch that is compiled
     *     out, so the query just returned a wrong score with no error at all.
     *
     * A real ereport on ERRCODE_INDEX_CORRUPTED, matching the decode boundaries in
     * bm25_seg_chain.c. The accessor (seg_doclen_field_cur) is separately guarded
     * there; this one fires
     * first and names the posting, so the failure points at the decode rather than
     * at the norms read it would otherwise surface from. */
    if (field_id >= c->seg->field_count)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field id %u in a scored posting exceeds the segment's "
                        "field count %u",
                        field_id, c->seg->field_count)));
    if (c->idf_f[field_id] == 0.0)
        return;                     /* term absent from this field: no contribution */
    if (!bm25_seg_reader_doc_is_live(&c->rdr, local_docid))
        return;                                     /* tombstoned: skip */
    tid = bm25_seg_reader_docid_to_tid(&c->rdr, local_docid);

    /* Dedupe-by-TID: pending wins. A TID reachable from both pending and a sealed
     * segment's not-yet-tombstoned postings is scored only from pending. (Not an
     * UPDATE's two versions -- different TIDs; see this callback's header.) */
    if (c->pending_tids != NULL)
    {
        bool found;

        (void) hash_search(c->pending_tids, &tid, HASH_FIND, &found);
        if (found)
            return;
    }

    /* Per-field doclen (NORMS carries doclen per field; for a single field this is
     * exactly the M3 whole-doc doclen). Sum this field's boosted term contribution
     * into the doc's ONE TID-keyed entry -- summing across a doc's fields keeps
     * score-each-doc-once (dedupe is by TID, not by (TID,field)). */
    doclen_f = bm25_seg_reader_doclen_field(&c->rdr, local_docid, field_id);
    contrib  = c->fcfg[field_id].boost *
               bm25_termscore(c->idf_f[field_id], tf, doclen_f,
                              c->avgdl_f[field_id],
                              c->fcfg[field_id].k1, c->fcfg[field_id].b);
    bm25_scores_add(c->scores, &tid, contrib,
                    c->seg_hdr, c->seg->gen, local_docid, c->budget);
}

/* Scan the pending list once, summing live (TID-valid) docs and their doclen, to
 * fold the unsealed corpus into the global IDF/avgdl stats (Critical Decision 2:
 * the scorer's live corpus = meta.ndocs + live pending docs, so read-your-writes
 * scoring is correct before a seal). meta.ndocs/total_len are sealed-only. */
static void
pending_global_stats(Relation index, BlockNumber pending_head, uint32 epoch_bound,
                     uint64 *out_ndocs, uint64 *out_total_len)
{
    BlockNumber blk = pending_head;
    BM25PendingWalk w;

    *out_ndocs = 0;
    *out_total_len = 0;
    bm25_pending_walk_init(&w, index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&w, blk); /* SHARE-locked */

        pg = BufferGetPage(buf);
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            if (!ItemPointerIsValid(&it.cur->tid))
                continue;       /* slot invalidated by a VACUUM sweep */
            /* v7 (#57): a document too large for one page is several records. Count the
             * FIRST part only -- this loop counts DOCUMENTS, and every part repeats the
             * whole document's doclen, so counting parts would inflate both N and sumdoclen
             * by the part count. That lands straight in idf and avgdl: the 4000-term row
             * in sql/77 spans ~21 parts, which made N read 22 for a two-row table and the
             * score come out 15x high until the seal collapsed it. */
            if ((it.cur->flags & BM25_PENDING_DOC_CONT) != 0)
                continue;
            *out_ndocs += 1;
            *out_total_len += it.cur->doclen;
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
        blk = next;
    }
}

/* Per-field pending corpus stats (BM25F, C3): for every live pending doc, add its
 * per-field doclen into out_len[field], and increment out_ndocs[field] for each field
 * the doc HAS (doclen > 0). Both out arrays are >= field_count and pre-zeroed by the
 * caller. Mirrors pending_global_stats, partitioned by field.
 *
 * WHERE THE PER-FIELD DOCLEN COMES FROM. A v8 record stores it outright, so this walk
 * reads it off the document's first part. A pre-v8 record does not, and this
 * reconstructs it as sum-of-tf over the field's term entries -- exact for such a
 * record, because it was written under an analyzer that emitted one token per source
 * run, which makes the token count and the run count the same number. Do NOT
 * "simplify" this back to sum-of-tf unconditionally: doclen is the run count
 * (bm25_accum_add_field_tokens), the seal writes the run count into NORMS, and
 * charging tf here would make avgdl move when a seal runs. */
static void
pending_stats_by_field(Relation index, BlockNumber pending_head, uint32 epoch_bound,
                       uint32 field_count,
                       uint64 *out_len, uint64 *out_ndocs)
{
    BlockNumber blk = pending_head;
    BM25PendingWalk w;
    /* Zeroed at declaration, not only in the head-record branch below (SCAN-08). The
     * per-document reset lives inside `if ((flags & BM25_PENDING_DOC_CONT) == 0)`, so a
     * walk whose FIRST record is a continuation would accumulate into uninitialized
     * stack before any reset runs. That is not hypothetical: ADR 0069 records that an
     * ordinary cancelled VACUUM leaves a stranded BM25_PENDING_DOC_CONT with no head
     * record, and reading uninitialized memory was the defect even though the garbage
     * was then discarded (acc_active is still false when the next head arrives, so
     * nothing was emitted) -- UB and an MSan report rather than a wrong answer.
     *
     * The `!acc_active` arm added below now continues before that read, so this
     * initializer no longer has a reachable path to protect. Keep it: it is the reason
     * the two are independent, so a future rearrangement of the guard cannot silently
     * reintroduce the read. Its cost is one stack memset per call. */
    uint32      len_by_field[BM25_MAX_FIELDS] = {0};   /* per-DOCUMENT scratch, spans its parts */
    bool        acc_active = false;         /* a document is being accumulated */
    /* True once the current document's first part handed us stored doclens; the
     * sum-of-tf fallback below is then skipped entirely, including on its
     * continuations. */
    bool        len_stored = false;
    /* The tid of the document being accumulated, so a continuation that belongs to
     * SOMEONE ELSE is dropped instead of folded into it. See the check at its use. */
    ItemPointerData acc_tid;
    uint32      f2;

    ItemPointerSetInvalid(&acc_tid);

    bm25_pending_walk_init(&w, index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&w, blk); /* SHARE-locked */

        pg = BufferGetPage(buf);
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;
            char   *p = (char *) dh + bm25_pending_doc_entries_off(dh);
            uint32  k;
            uint32  f;

            if (!ItemPointerIsValid(&dh->tid))
                continue;       /* slot invalidated by a VACUUM sweep */

            /* v7 (#57): accumulate across a document's parts and emit ONCE per document.
             * out_len[] would be right either way (each entry is counted once wherever it
             * lands), but out_ndocs[] is a count of DOCUMENTS having the field, so a
             * per-record emit would count a spanning document once per part and inflate
             * N_field -- i.e. deflate that field's idf. tok_by_field is plain counters, so
             * carrying it across a page boundary is safe. */
            if ((dh->flags & BM25_PENDING_DOC_CONT) == 0)
            {
                const uint32 *fl;

                if (acc_active)
                    for (f = 0; f < field_count; f++)
                        if (len_by_field[f] > 0)
                        {
                            out_len[f]   += len_by_field[f];
                            out_ndocs[f] += 1;
                        }
                for (f = 0; f < field_count; f++)
                    len_by_field[f] = 0;
                acc_active = true;
                acc_tid = dh->tid;

                /* v8: take the stored doclens. dh->nfieldlens is the WRITING index's
                 * field_count and this reader's field_count is the same metapage value,
                 * fixed at CREATE INDEX -- so a difference means a corrupt record, and
                 * the safe response is to fall back to the reconstruction below rather
                 * than read a short array. Reading it short would leave the missing
                 * fields at 0, and a per-field doclen of 0 is not a small number to
                 * bm25_termscore, it is the maximum-score one. */
                fl = bm25_pending_doc_fieldlens(dh);
                len_stored = (fl != NULL && dh->nfieldlens == field_count);
                if (len_stored)
                    for (f = 0; f < field_count; f++)
                        len_by_field[f] = fl[f];
            }
            else if (!acc_active || !ItemPointerEquals(&acc_tid, &dh->tid))
                /* A continuation with nothing to continue -- part 0 was invalidated by
                 * a VACUUM sweep that was then cancelled, so it never opened a document
                 * here (ADR 0069; bm25_pending_drain has the same arm, and drops the
                 * fragment for the same reason). Without this check the orphan's term
                 * entries were folded into whatever document happened to precede it,
                 * inflating that document's length and therefore skewing avgdl. The
                 * pre-existing zero-initialization of the scratch covers only the case
                 * where the orphan is the FIRST record the walk sees; this covers the
                 * rest. */
                continue;

            if (!len_stored)
                for (k = 0; k < dh->ndocterms; k++)
                {
                    BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                    if (te->field_id < field_count)
                        len_by_field[te->field_id] += te->tf;
                    p += MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
                }
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
        blk = next;
    }

    /* Last document on the chain. */
    if (acc_active)
        for (f2 = 0; f2 < field_count; f2++)
            if (len_by_field[f2] > 0)
            {
                out_len[f2]   += len_by_field[f2];
                out_ndocs[f2] += 1;
            }
}

/* bm25_field_corpus_stats -- per-field sumdoclen (out_len) and N_field (out_ndocs) summed
 * over the snapshot's live segments + live pending. N_field is the count of live
 * docs that HAVE the field (>=1 token in it): a doc whose field-f column is
 * NULL/empty tokenizes to zero tokens and is NOT counted toward field f's N.
 *
 * Segments contribute their (post-seal) per-field stats from the header
 * (total_len_by_field[] + ndocs_by_field[], the latter derived from hdr->ndocs for
 * a single-field segment). Pending contributes live pending docs. Both arrays are
 * sized >= field_count. Tombstones: like corpus avgdl, per-field avgdl is exact
 * only immediately after seal/merge and drifts approximate under tombstones -- the
 * segment header stats are not decremented per-field per tombstone (R5). */
void                            /* declared in bm25_scan.h (bm25_debug.c) */
bm25_field_corpus_stats(Relation index, const BM25ScanSnapshot *snap,
                        uint64 *out_len /* >= field_count */,
                        uint64 *out_ndocs /* >= field_count */)
{
    uint32 f;
    uint32 si;

    for (f = 0; f < snap->field_count; f++)
    {
        out_len[f]   = 0;
        out_ndocs[f] = 0;
    }
    for (si = 0; si < snap->nsegs; si++)
    {
        BM25SegmentHeader h;
        uint64            seglen[BM25_MAX_FIELDS];
        uint64            segndocs[BM25_MAX_FIELDS];

        CHECK_FOR_INTERRUPTS();

        bm25_seg_header_read_lens(index, snap->segs[si].header_blkno,
                                  snap->segs[si].gen, &h, seglen, segndocs);
        /* Issue #303.G: every segment of an index carries the index's own field
         * count (the builders take it from the metapage, and neither ALTER nor the
         * merge changes it), and the outputs are zeroed and consumed only up to
         * snap->field_count. A header claiming more summed into slots nobody
         * initialized; one claiming fewer silently left a field's corpus stats
         * short, skewing its avgdl in every score. bm25_segheader_validate bounds
         * the value only by BM25_MAX_FIELDS, so the cross-check is here, where both
         * counts are in hand. */
        if (h.field_count != snap->field_count)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: segment header at block %u has field_count %u, but the "
                            "index has %u fields",
                            snap->segs[si].header_blkno, h.field_count, snap->field_count),
                     errhint("REINDEX the index.")));
        for (f = 0; f < h.field_count; f++)
        {
            out_len[f]   += seglen[f];
            out_ndocs[f] += segndocs[f];
        }
    }
    pending_stats_by_field(index, snap->pending_head, snap->next_gen,
                           snap->field_count,
                           out_len, out_ndocs);
}

/* Load the per-field config for a scan and OVERLAY the live-tunable
 * parameters (k1/b/boost) from current reloptions (Task 2). All five ranked-
 * scan entry points come through here so the exhaustive scorer, the WAND
 * builder, and the WAND-bound debug probes all see one consistent,
 * live-resolved set for the scan's lifetime; the query-parse/flatten debug
 * SRFs (field identity only, no ranking) and the meta-based snippet/score
 * readers (reuse a stored result) deliberately do not -- see their own call
 * sites.
 *
 * store_pos mirrors bm25_fieldcfg_read's own contract: NULL when the caller
 * doesn't need per-field position-storage bits, else a caller-zeroed array of
 * size >= BM25_MAX_FIELDS. The no-field-config-page fallback below leaves it
 * untouched (matching every real inline fallback this replaces -- none of
 * them ever turned position storage on for the synthesized single field). */
static void
bm25_scan_load_fieldcfg(Relation index, const BM25ScanSnapshot *snap,
                        BM25FieldConfigHeader *fhdr, BM25FieldConfig *fcfg,
                        uint8 *store_pos)
{
    /* Zero the WHOLE caller array first, on both branches (SCAN-06).
     *
     * The callers declare `BM25FieldConfig fcfg[BM25_MAX_FIELDS]` as plain uninitialized
     * stack. bm25_fieldcfg_read fills only the entries the FIELD-CONFIG PAGE claims
     * (fhdr->field_count), but every consumer downstream indexes fcfg[] by
     * snap->field_count, which is a SEPARATE on-disk value read from the METAPAGE.
     * Nothing makes those two agree -- they are validated independently, each against
     * BM25_MAX_FIELDS -- so when the metapage count is the larger of the two the scorer
     * reads k1, b and boost out of uninitialized stack and produces confidently wrong
     * scores rather than failing.
     *
     * Zeroing does not RECONCILE the two counts; that disagreement is a separate defect
     * with a separate fix. What it does is make the degenerate case deterministic
     * (zeroed config) instead of stack-dependent, which is this issue's remit. The
     * fallback branch below has always done this; the read branch never did.
     *
     * Cost is one ~4 KB memset per scan, not per row. */
    MemSet(fcfg, 0, sizeof(BM25FieldConfig) * BM25_MAX_FIELDS);

    if (snap->field_config_blkno != InvalidBlockNumber)
        bm25_fieldcfg_read(index, snap->field_config_blkno, fhdr, fcfg, store_pos);
    else
    {
        /* No field-config page (defensive): synthesize one field from the
         * metapage snapshot, exactly as the inline fallbacks did. */
        MemSet(fhdr, 0, sizeof(*fhdr));
        fhdr->field_count = 1;
        MemSet(fcfg, 0, sizeof(BM25FieldConfig) * BM25_MAX_FIELDS);
        fcfg[0].field_id = 0;
        fcfg[0].k1       = snap->k1;
        fcfg[0].b        = snap->b;
        fcfg[0].boost    = 1.0;
    }
    bm25_resolve_live_params(index, fcfg, fhdr->field_count);
}

static int
scored_desc(const void *a, const void *b)
{
    double sa = ((const BM25ExhScored *) a)->score;
    double sb = ((const BM25ExhScored *) b)->score;

    /* Descending score; break ties by ascending TID for determinism. */
    if (sa < sb)
        return 1;
    if (sa > sb)
        return -1;
    return ItemPointerCompare((ItemPointer) &((const BM25ExhScored *) a)->tid,
                              (ItemPointer) &((const BM25ExhScored *) b)->tid);
}

/*
 * The section I scan-start gate itself (bm25_fingerprint_gate) moved to
 * bm25_analyzer.c with #188, when bm25_insert became its second caller: both of its
 * operands are that file's, and leaving it here would have made the write path
 * include a scan-private header to reach a predicate about the analyzer. The scan
 * still calls it exactly where it always did, from bm25_scan_corpus_stats below.
 *
 * The query-side fingerprint it is handed is recomputed from the index's OWN
 * reloptions at scan start (M3: query and index share one analyzer, so a match is the
 * norm; the gate is the guardrail for a future query-side analyzer override and for a
 * post-build reloption edit without REINDEX).
 */

/*
 * bm25_scan_corpus_stats -- the shared scan-start prologue every ranked path runs
 * before it scores anything: ONE atomic snapshot, the section I analyzer fingerprint
 * gate, and the global + per-field corpus statistics.
 *
 * Sharing it is not tidiness. WAND's bit-exactness against the exhaustive scorer
 * (D8) rests on both consuming IDENTICAL stats, and the five hand-copied
 * prologues this replaces had already drifted: wand_debug_ctx_and_block never
 * carried the fingerprint gate at all, because the copy that created it dropped
 * the gate and nothing afterwards could notice. That is the failure mode of
 * hand-copied prologues generally, not a one-off slip.
 *
 * (D-SNAP/C3) The snapshot captures pending_head, the whole live catalog
 * (segs[]/nsegs), the global stats (ndocs/total_len) and k1/b together under a
 * single metapage SHARE lock. NEVER pair a bm25_meta_read with a separate
 * bm25_segcat_read here: a seal wedging between the two could double-score or
 * drop a doc. snap->segs is palloc'd in CurrentMemoryContext and freeing it is
 * the caller's job (bm25.h), so every caller must already be in a context whose
 * lifetime it controls. The two scorer paths do that with an explicit scratch
 * context created BEFORE this call, never after (H17); the three debug SRFs
 * instead inherit the SRF's own per-call context, which is bounded for them.
 *
 * (section I) M3 tokenizes the query with the index's OWN analyzer, so *qcfg is
 * resolved here and its fingerprint validated against the snapshot's stored
 * value before any work. Callers reuse *qcfg for bm25_analyze, tokenizing
 * against the very config just gated.
 *
 * (Critical Decision 2) The live corpus for IDF/avgdl is the sealed global stats
 * (segments, tombstone-adjusted) PLUS live pending, so read-your-writes scoring
 * is correct before a seal. The pending walk uses the SAME pending_head the
 * caller's scoring pass will use, so stats and postings stay consistent.
 *
 * (C3) BM25F per-field config + per-field avgdl. The field-config page is read
 * ONCE per scan (m6: never per-term-per-segment) into fcfg[]. Two cases:
 *
 *  - field_count == 1: collapse to the M3 formula byte-for-byte. avgdl_f[0] is
 *    the metapage-cached (tombstone-adjusted) corpus avgdl; k1/b come from
 *    fcfg[0] (== the metapage defaults C1 wrote) and boost == 1.0; the idf still
 *    uses live_ndocs (bm25_term_idf). Crucially this path does NOT use the
 *    segment-header per-field stats, which are NOT tombstone-decremented (R5) --
 *    so a tombstoned single-field index scores identically to M3.
 *
 *  - field_count > 1: avgdl_f[f] = total_len_by_field[f] / N_field[f] from the
 *    segment headers + pending (bm25_field_corpus_stats). Exact post-seal/merge,
 *    approximate under tombstones (R5), exactly like corpus avgdl.
 *
 * store_pos (M4/D12) is the per-field store_positions bit array, zeroed and
 * filled here; pass NULL when the caller does not want the position bits. Only
 * the exhaustive scorer does: they gate its lockstep POS reader and drive the D7
 * phrase degradation check.
 *
 * The corpus-wide avgdl is deliberately not an output -- it exists only to seed
 * avgdl_f[0], and no caller has ever read it again.
 *
 * Everything after the capture is bm25_corpus_stats_from_snapshot, which the #290
 * filtered build calls directly: it captures one snapshot for several sets.
 */
static void bm25_corpus_stats_from_snapshot(Relation index, const BM25ScanSnapshot *snap,
                                            BM25AnalyzerConfig *qcfg, uint8 *store_pos,
                                            uint64 *live_ndocs, BM25FieldConfig *fcfg,
                                            double *avgdl_f, uint64 *fld_ndocs);

void                            /* declared in bm25_scan.h (bm25_debug.c) */
bm25_scan_corpus_stats(Relation index, BM25ScanSnapshot *snap,
                       BM25AnalyzerConfig *qcfg, uint8 *store_pos,
                       uint64 *live_ndocs, BM25FieldConfig *fcfg,
                       double *avgdl_f, uint64 *fld_ndocs)
{
    bm25_scan_snapshot(index, snap);
    bm25_corpus_stats_from_snapshot(index, snap, qcfg, store_pos, live_ndocs,
                                    fcfg, avgdl_f, fld_ndocs);
}

/* bm25_scan_corpus_stats without the capture: everything it derives from a
 * snapshot the caller took, and keeps alive, itself. */
static void
bm25_corpus_stats_from_snapshot(Relation index, const BM25ScanSnapshot *snap,
                                BM25AnalyzerConfig *qcfg, uint8 *store_pos,
                                uint64 *live_ndocs, BM25FieldConfig *fcfg,
                                double *avgdl_f, uint64 *fld_ndocs)
{
    BM25FieldConfigHeader fhdr;
    uint64                pend_ndocs,
                          pend_total_len,
                          ndocs,
                          live_total_len;
    double                avgdl;
    uint32                f;

    bm25_analyzer_config(index, qcfg);
    bm25_fingerprint_gate(index, bm25_analyzer_fingerprint(qcfg),
                          snap->analyzer_fingerprint, BM25_GATE_SCAN);

    pending_global_stats(index, snap->pending_head, snap->next_gen,
                         &pend_ndocs, &pend_total_len);
    ndocs          = snap->ndocs + pend_ndocs;
    live_total_len = snap->total_len + pend_total_len;
    avgdl = (ndocs > 0)
        ? ((double) live_total_len / (double) ndocs)
        : 1.0;
    *live_ndocs = ndocs;

    if (store_pos != NULL)
        for (f = 0; f < BM25_MAX_FIELDS; f++)
            store_pos[f] = 0;       /* bm25_fieldcfg_read wants a zeroed array */
    bm25_scan_load_fieldcfg(index, snap, &fhdr, fcfg, store_pos);

    if (snap->field_count == 1)
    {
        avgdl_f[0]   = avgdl;       /* metapage corpus avgdl: M3-identical, tombstone-adjusted */
        fld_ndocs[0] = ndocs;       /* unused for idf when field_count==1 (uses live_ndocs directly) */
    }
    else
    {
        uint64 fld_total_len[BM25_MAX_FIELDS];

        bm25_field_corpus_stats(index, snap, fld_total_len, fld_ndocs);
        for (f = 0; f < snap->field_count; f++)
            avgdl_f[f] = (fld_ndocs[f] > 0)
                ? ((double) fld_total_len[f] / (double) fld_ndocs[f]) : 1.0;
    }
}

/* M6 boolean scorer work item: one query term carrying its leaf's field scope,
 * query-time boost, presence bit, and negation. The exhaustive scorer's per-term
 * OR loop is generalized to iterate a FLAT list of these (leaves x their tokens),
 * so a plain text query is just its tokens with field so->qfield / boost 1.0 /
 * leaf_bit -1, and a boolean tree is every leaf's tokens with the leaf's own
 * scope/boost/bit. Keeping the list flat leaves the scoring loop a single level
 * deep (the phrase machinery still indexes by the token ordinal qi). */
typedef struct
{
    const char *ptr;            /* stemmed query term (into a per-leaf lowercased buf) */
    int         len;
    int32       field_id;       /* per-leaf scope (replaces the global so->qfield gate) */
    double      boost;          /* query-time multiplier, folded into idf (linear) */
    int         leaf_bit;       /* >=0 => mark presence in and_presence; -1 => text path */
    bool        negated;        /* must_not leaf: presence only, contributes NO score */
    int         phrase_grp;     /* M6 Task 6: >=0 => token belongs to phrase-leaf group
                                 * phrase_grp (positions gathered, presence via adjacency
                                 * recheck, NOT per-term seg_and_cb); -1 => non-phrase */
    int         phrase_qi;      /* within-phrase term index (0..) when phrase_grp >= 0 */
} BM25TermWork;

/* M6 Task 6: one PHRASE leaf inside a boolean tree. Its terms score into acc as OR
 * terms (like a MATCH leaf, above); its leaf_bit is set in and_presence ONLY for docs
 * whose stashed term positions satisfy the leaf's own adjacency (field/slop/ordered)
 * via phrase_recheck_tid. Each phrase leaf owns a SEPARATE stash (keyed by TID, lists
 * sized to that leaf's own token count), so two phrase leaves never alias on cur_qi
 * (the within-phrase term index) the way one shared stash would. */
typedef struct
{
    PhraseStashCtx ctx;         /* this leaf's position stash (ctx.stash keyed by TID) */
    int32          field_id;    /* leaf field scope, passed as qfield to the recheck */
    int            slop;
    bool           ordered;
    int            leaf_bit;    /* presence bit set on a passing recheck */
    uint32         nqterms;     /* leaf token count (== ctx.nq); recheck's nq */
    /* Issue #184: this leaf's own token -> slot grouping. Per leaf, not shared: each
     * phrase leaf analyzes its own text, so its slot boundaries are its own. */
    BM25PhraseSlotMap smap;
} BoolPhraseGrp;

/* -------------------------------------------------------------------------
 * D7/D12 phrase-positionless predicate (shared by the text and jsonb gates)
 * -------------------------------------------------------------------------
 * "Can a phrase be answered from stored positions?" is asked on two query
 * surfaces -- (text,text) and the jsonb boolean tree -- over identical inputs.
 * It used to be written out twice, which is how the two gates came to differ:
 * only the text copy consults phrase_fallback. Sharing the predicate means a
 * future edit to what counts as positionless cannot land on one surface and
 * miss the other, which is the drift this split exists to prevent.
 *
 * Deliberately TWO functions rather than one, because the callers need
 * different call shapes over the same predicate. The text path has a single
 * scoped field; a jsonb boolean tree carries one field scope PER PHRASE LEAF.
 * Folding the segment sweep into the field check would force the jsonb caller
 * to re-read every segment header once per leaf -- O(nleaves * nsegs) header
 * reads for what is a single index-wide property.
 *
 * Both return true when positions ARE available; callers degrade on false and
 * own the ERROR-vs-WARNING policy themselves (see the call sites for why that
 * policy deliberately still differs between the two surfaces).
 */
static bool
bm25_phrase_fields_have_positions(const BM25ScanSnapshot *snap,
                                  const bool *field_store_positions,
                                  int32 scoped_field)
{
    uint32  f;

    /* A field-scoped phrase on an off field cannot be answered from positions.
     * An out-of-range scope is not this predicate's business -- it names no
     * field at all, so it cannot name a positions-off one, and the caller's
     * own field validation reports it. */
    if (scoped_field != BM25_FIELD_ALL)
        return !(scoped_field < (int32) snap->field_count &&
                 !field_store_positions[scoped_field]);

    /* A BARE (unscoped) phrase matches OR-across-fields (D6), so it may land on
     * ANY field. On a MIXED index (some fields store_positions=false), the off
     * field stashes no positions and the recheck would silently drop a doc whose
     * phrase holds only there -- a false negative. So a bare phrase degrades if
     * ANY field is positions-off (D12), exactly like the scoped case. An all-on
     * index has no off field, so this never fires and OR-across-fields works. */
    for (f = 0; f < snap->field_count; f++)
        if (!field_store_positions[f])
            return false;
    return true;
}

static bool
bm25_phrase_segments_have_positions(Relation index, const BM25ScanSnapshot *snap)
{
    uint32  si;

    /* Any live segment without a POS chain is position-less for phrase purposes.
     * (A newly-built all-positions index has pos_root valid on every segment; a
     * pre-M4 or all-off index has it Invalid.) Pending-only docs always carry
     * true posblob positions, so an empty-segment index does not degrade. */
    for (si = 0; si < snap->nsegs; si++)
    {
        BM25SegmentHeader   h;

        CHECK_FOR_INTERRUPTS();

        bm25_seg_header_read(index, snap->segs[si].header_blkno,
                             snap->segs[si].gen, &h);
        if (h.pos_root == InvalidBlockNumber && h.ndocs > 0)
            return false;
    }
    return true;
}

/*
 * bm25_scan_build_ranking_exhaustive -- the multi-source OR-sum scorer (one
 * attempt). Until M2b this WAS bm25_scan_build_ranking_once; Task 9 split the
 * seam in two so a bag-of-words scored scan can instead route through the
 * Block-Max WAND driver (bm25_wand_build_ranking) by default. This function
 * is now reached three ways, always producing the identical contract (fill
 * so->ranked[]/scores[]/nranked in descending score order):
 *   1. bm25_scan_build_ranking_once (the dispatcher, just below) when the D7
 *      gate excludes WAND -- phrase/proximity/AND-fallback queries, whose
 *      positional recheck runs AFTER scoring (D9 of M4) so score-based
 *      pruning could discard a doc the filter would have kept, and
 *      bm25_native.wand_top_k = 0 (WAND disabled).
 *   2. bm25_gettuple's over-pull tail fallback (Task 10): once a WAND-capped
 *      scan's executor pulls past wand_top_k rows, this rebuilds the FULL
 *      ranking so the tail is still exact.
 *   3. bm25_debug_rank / bm25_debug_rank_key (bm25_debug.c), which pass
 *      force_exhaustive=true so they reach this function unconditionally --
 *      proving WAND parity in sql/43_wand_parity.sql needs an uncapped
 *      reference, and one that cannot drift onto the WAND path (#67.4).
 *
 * Builds the full descending-score ranking by unioning EVERY live source the
 * scan's snapshot sees -- the pending list plus ALL sealed segments in the
 * catalog -- deduplicated by heap TID (Task 12). The M1 single-segment math
 * (bm25_idf / bm25_termscore / scored_desc / the BM25AccEnt dynahash) is unchanged;
 * only the posting SOURCES and the dedupe are new.
 *
 * Per query term the algorithm is:
 *   1. Sum df across ALL segments + pending -> ONE corpus-wide idf (the M1
 *      single-idf model, now corpus-wide rather than segment-local).
 *   2. Score the PENDING copies FIRST, registering each scored TID into
 *      pending_tids. Pending must precede segments within the term so the
 *      segment callback can skip TIDs pending already owns (dedupe ordering
 *      invariant; pending wins).
 *   3. Score EACH segment's postings via seg_posting_cb, which skips tombstoned
 *      docs (bm25_seg_doc_is_live) AND TIDs already in pending_tids.
 *
 * Dedupe-by-TID (pending wins) guards the one case the key can actually express:
 * a single heap TID reachable from BOTH sources at once -- a pending entry for
 * that TID plus a segment posting for the SAME TID that is not yet tombstoned
 * (e.g. a heap line pointer reused for a new row). Scoring such a TID once keeps
 * the additive BM25 sum correct (design section 6/section 7). It is a defensive
 * backstop rather than the primary mechanism: VACUUM's bm25_bulkdelete tombstones
 * a dead TID's posting (bm25_livedocs_clear) before its line pointer may be
 * reused -- the ambulkdelete contract, spelled out in bm25_gettuple's non-scoring
 * xs_recheck comment -- so bm25_seg_doc_is_live normally skips that posting first.
 *
 * The two builders define "pending wins" differently for such a TID (#314 SCORE-03,
 * a documented divergence, not a defect). This builder is term-major and registers a
 * pending TID in pending_tids only as it scores each term, so a TID pending for
 * term B but in a segment for term A is scored from the segment for A and from
 * pending for B and summed. The WAND driver (bm25_wand.c) scores pending for ALL
 * terms first, so it would skip that TID's segment posting for A. The two agree on
 * every reachable state because the state is unreachable under the ambulkdelete
 * contract above: a TID is pending only after its old segment posting was
 * tombstoned. No Assert backs that, deliberately -- it would test on-disk state,
 * which a corrupt index can violate and a production build compiles out.
 *
 * It does NOT merge the two versions of an UPDATEd row: an UPDATE writes a NEW
 * heap TID, so old and new version are DIFFERENT keys and a TID-keyed set can
 * never collapse them. A stale pre-UPDATE segment posting survives ranking under
 * its own TID and is removed only by the executor's heap-visibility check.
 *
 * It is NOT a seal-crash window either -- the seal is atomic (D-SEAL/C2), so no
 * snapshot ever sees a doc in both sources from a seal.
 *
 * Consistency comes from ONE bm25_scan_snapshot (D-SNAP/C3): pending_head, the
 * whole live catalog, the global stats, and k1/b are captured atomically under a
 * single metapage SHARE lock. Sources published after this point are invisible.
 * avgdl derives from the snapshot's global stats (sealed + tombstone-adjusted;
 * the sealer keeps it live), matching the corpus the segments belong to.
 *
 * ranked[]/scores[] are written into so->scanctx so they outlive this call; all
 * scratch (the two dynahashes, the token array, the temp BM25ExhScored array) lives
 * in a short-lived child context deleted before return. For bm25_debug_rank's
 * fabricated opaque, so->scanctx is the SRF's per-query context -- also fine.
 *
 * May ereport ERRCODE_T_R_SERIALIZATION_FAILURE (the option-(d) seg_gen abort
 * from bm25_seg_page_validate on a followed segment page); the public
 * bm25_scan_build_ranking wrapper catches exactly that and retries with a fresh
 * snapshot (D-HORIZON/C4), except on a hot standby, where it reaches the client
 * (#307; see the wrapper).
 *
 * The contract (fill so->ranked[0..nranked-1] in descending score order with
 * valid TIDs and so->scores[] in parallel) must not change -- every caller
 * above depends on it being interchangeable with the WAND path's result.
 *
 * pin (#268) is non-NULL only for caller 2, and carries the corpus statistics the
 * capped WAND build scored under. The snapshot, postings, tombstone gates and
 * membership all stay fresh; only the inputs a document's score is computed from
 * (per-field avgdl and k1/b/boost, and each token's idf) are replaced, so a
 * document both builds saw gets the score it was emitted with. bm25_term_idf still
 * runs per term under the pin: its df pass is also the dictionary lookup the
 * segment pass consumes (issue #246), and its false return still means "no live
 * posting for this term in scope", so only its idf OUTPUT is overridden.
 *
 * given_snap (#290) is non-NULL only for the filtered build, which computes several
 * sets -- the ORDER BY ranking and each WHERE key's membership -- and needs them all
 * from ONE snapshot. The scorer then reads that snapshot instead of capturing its
 * own; the caller owns it and keeps its catalog copy alive for the call. Everything
 * else (the gate, the corpus statistics, the scoring) runs exactly as it does
 * without one.
 */
static void
bm25_scan_build_ranking_exhaustive(Relation index, BM25ScanOpaque so,
                                   const char *query, int querylen,
                                   const BM25PinnedStats *pin,
                                   const BM25ScanSnapshot *given_snap)
{
    BM25ScanSnapshot   snap;
    BM25AnalyzerConfig qcfg;
    BM25Token         *qtoks;
    int              nq,
                     qi;
    uint32           si;
    uint64           live_ndocs;
    HTAB            *acc;
    HTAB            *pending_tids;
    HASHCTL          ctl;
    HASH_SEQ_STATUS  seq;
    BM25ExhScored   *arr;
    long             nacc;
    /* #62.5: one running total for the whole build, so structures that each hold
     * an entry per matching document share the budget instead of each spending it. */
    BM25MatchBudget  budget;
    int              idx;
    BM25AccEnt      *e;
    MemoryContext    scratchctx;
    MemoryContext    oldctx;
    /* BM25F per-field config + per-field corpus stats (C3). For field_count == 1
     * these collapse to the single-field metapage stats (see the fill below). */
    BM25FieldConfig       fcfg[BM25_MAX_FIELDS];
    double                avgdl_f[BM25_MAX_FIELDS];
    uint64                fld_ndocs[BM25_MAX_FIELDS];  /* per-field N (BM25F idf denominator) */
    uint32                f;
    uint8                 km_type;  /* M5: index-wide key config for docid->key resolve */
    uint16                km_size;
    BM25SegKeyCache       keycache; /* issue #225: per-segment KEYMAP readers for the finalize */
    BM25KeyReq           *kreqs = NULL;     /* issue #246: rows' key sources, keyed index only */
    BM25TermSegLoc       *seglocs;          /* issue #246: [nsegs] df-pass lookups, per term */
    uint32                nkreq = 0;
    /* M4 phrase (C-MATCH). The stash + its context are built only for a phrase query;
     * field_store_positions[] gates the lockstep POS reader AND drives the D7
     * degradation check (a phrase scoped to / landing on an off field errors). */
    bool                  phrase = so->qphrase;
    HTAB                 *phrase_stash = NULL;
    PhraseStashCtx        pctx;
    /* Issue #184: the text phrase's token -> slot grouping. Read only under the same
     * (phrase && nq > 0) guard that fills it; nslots = 0 keeps it inert otherwise. */
    BM25PhraseSlotMap     qsmap = {0};
    HTAB                 *and_presence = NULL;   /* D7 AND fallback / M6 boolean: TID -> leaf bitmask */
    PhraseAndCtx          actx;
    bool                  field_store_positions[BM25_MAX_FIELDS];
    bool                  fallback_and = false;  /* D7: phrase_fallback='and' downgrade active */
    /* M6 boolean tree (BOOLEAN/BOOST root). Reuses the AND-fallback presence HTAB
     * (and_presence/PhraseAndEnt.mask via phrase_and_mark, cur_qi = leaf_bit) -- no
     * new BM25AccEnt.mask field. Mutually exclusive with phrase/fallback_and (a jsonb
     * tree never sets so->qphrase). */
    bool                  boolean_mode = bm25_qtree_is_multileaf(so->qtree);
    BM25TermWork         *works;
    int                   nworks = 0;
    /* M6 Task 6: per-phrase-leaf position stashes (boolean tree only). npgrp == 0 on
     * every non-boolean path and on a boolean tree with no phrase leaf. */
    BoolPhraseGrp        *pgrps = NULL;
    int                   npgrp = 0;

    /*
     * Do all scratch work in a short-lived context so the persistent scanctx only
     * holds the final ranked[]/scores[] arrays. Parent it under the CURRENT
     * (subtransaction) context, not scanctx: this worker runs inside the retry
     * wrapper's internal subtransaction, so on the retryable seg_gen abort the
     * subtransaction rollback reclaims scratchctx for free (it would otherwise
     * orphan under the longer-lived scanctx until endscan). The success path still
     * deletes it explicitly below -- but only AFTER copying the results into
     * scanctx, which is outside the subtransaction and survives the Release.
     *
     * H17: created HERE, before the corpus-stat prologue, not after it.
     * bm25_scan_corpus_stats takes the snapshot, which palloc's a copy of the
     * whole live catalog in CurrentMemoryContext with freeing it the caller's job
     * (bm25.h), and its pending/field-config/per-field-stat reads allocate here
     * too. With the context created after that call, all of it landed in the
     * subtransaction context and was reclaimed only at end of transaction --
     * unbounded across rescans, which is once per outer row for a parameterized
     * nested loop. snap.segs is last read well before the delete below (the
     * per-segment header sweep), so moving it in is safe.
     */
    scratchctx = AllocSetContextCreate(CurrentMemoryContext,
                                       "bm25 rank scratch",
                                       ALLOCSET_SMALL_SIZES);
    oldctx = MemoryContextSwitchTo(scratchctx);

    /* #62.5: read the budget once, HERE, so every structure this build materializes
     * is judged against one total taken at one instant -- max_match_memory is
     * PGC_USERSET and could otherwise be read at two different values by two
     * structures of the same scan. Reset per build, which is what makes the total a
     * peak rather than a lifetime sum: the over-pull tail rebuild and every rescan
     * start again from zero, matching the scratch context they allocate into. */
    bm25_match_budget_init(&budget);

    /*
     * (1) Shared scan-start prologue: ONE atomic snapshot (D-SNAP/C3), the section I
     * analyzer fingerprint gate, and the global + BM25F per-field corpus stats.
     * bm25_scan_corpus_stats carries the rationale for all three; qcfg comes back
     * resolved AND gated, ready to tokenize the query below against the very
     * analyzer it was just validated against.
     *
     * M4/D12: this is the one call site that wants the per-field store_positions
     * bits. Pre-init field_store_positions all-false; a field's bit comes from the
     * field-config page's trailing flag array only when that array is present (an
     * M5/legacy page has none -> treated as OFF, harmless because those segments
     * also have pos_root Invalid). field_store_positions[] then gates the lockstep
     * POS reader AND is consulted by the phrase degradation check (D7).
     */
    for (f = 0; f < BM25_MAX_FIELDS; f++)
        field_store_positions[f] = false;
    {
        uint8 store_pos[BM25_MAX_FIELDS];

        if (given_snap != NULL)
        {
            /* A struct copy: snap.segs still points at the caller's catalog copy,
             * which nothing below frees. */
            snap = *given_snap;
            bm25_corpus_stats_from_snapshot(index, &snap, &qcfg, store_pos,
                                            &live_ndocs, fcfg, avgdl_f, fld_ndocs);
        }
        else
            bm25_scan_corpus_stats(index, &snap, &qcfg, store_pos,
                                   &live_ndocs, fcfg, avgdl_f, fld_ndocs);
        for (f = 0; f < BM25_MAX_FIELDS; f++)
            field_store_positions[f] = (store_pos[f] != 0);
    }

    /* #268: the over-pull tail scores under the capped build's statistics (see the
     * function header). field_count is fixed at CREATE INDEX, so a mismatch means
     * the pin does not describe this index; refuse rather than score on it. */
    if (pin != NULL)
    {
        if (pin->field_count != snap.field_count)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bm25: over-pull tail rebuild sees %u fields, the build it "
                            "continues saw %u",
                            snap.field_count, pin->field_count)));
        for (f = 0; f < snap.field_count; f++)
        {
            avgdl_f[f]    = pin->avgdl_f[f];
            fcfg[f].k1    = pin->k1[f];
            fcfg[f].b     = pin->b[f];
            fcfg[f].boost = pin->boost[f];
        }
    }

    /* Tokenize the query through the index's analyzer so the query terms stem to the
     * SAME roots the segment stored (build/insert tokenize via bm25_analyze too).
     * Tokenizing un-stemmed here would look up surface forms the index never stored
     * and silently match nothing. Reuses the qcfg resolved + gated above. */
    nq = bm25_analyze(&qcfg, query, querylen, &qtoks);

    /* #268: the pinned idf is indexed by the capped build's token ordinal, which is
     * this build's work index only on the plain OR path (works[] maps qtoks 1:1
     * below) and only if the query analyzed to the same token count. Both hold for
     * every capped scan: the WAND gate excludes phrases and multi-leaf trees. The
     * token count can still move under an open scan, though: the analyzer reloptions
     * take AccessExclusiveLock for ALTER INDEX ... SET only once the backend running
     * the ALTER has registered them (bm25 registers its options lazily, on the first
     * bm25_options call in a backend, and core derives the ALTER's lock level from
     * the options registered in THAT backend, so a cold one takes only
     * ShareUpdateExclusiveLock), and ALTER TEXT SEARCH DICTIONARY takes no index
     * lock at all. So this check is the backstop, not a redundancy: it raises an
     * ERROR rather than score through a misaligned pin, which would be silent.
     *
     * Issue #313 XCUT-12: the SQLSTATE is 55000, not XX000. Concurrent DDL reaches
     * this, so it is not an internal invariant, and a client classifying XX000 as a
     * server bug would be misled (ADR 0092). Not 40001 either: the pin came from the
     * first build of THIS scan, so the retry wrapper would fail identically; a fresh
     * statement is what recovers. The field_count check above stays XX000 -- that
     * one is fixed at CREATE INDEX and is a true invariant. */
    if (pin != NULL && (phrase || boolean_mode || pin->nq != nq))
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bm25: over-pull tail rebuild does not match the build it continues "
                        "(%d query terms, %d pinned; phrase %d, multi-leaf %d)",
                        nq, pin->nq, (int) phrase, (int) boolean_mode)));

    /* M4 phrase degradation (D7) + stash setup. A phrase query needs positions on
     * every segment it touches and on any field it is scoped to. Decide up front:
     *   - ERROR (default 'error') if ANY live segment is position-less (pos_root
     *     Invalid: pre-M4 / all-off index) OR the scoped field (so->qfield, when not
     *     ALL) has store_positions=false -- the positions simply are not there.
     *   - phrase_fallback='and': emit a WARNING and fall back to AND-of-terms (all nq
     *     terms present, no positional test). fallback_and disables the recheck.
     * A phrase longer than the matcher cap is rejected here (parse allows any length;
     * the cap is a matcher-array bound). nq==0 (all-stopword phrase) drops through as
     * a normal empty result. */
    if (phrase && nq > 0)
    {
        bool need_degrade = false;

        if (nq > BM25_PHRASE_MAX_TERMS)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bm25: phrase has too many terms (%d); max %d",
                            nq, BM25_PHRASE_MAX_TERMS)));

        /* Shared D7/D12 predicate; the || short-circuits exactly as the two
         * inlined loops used to, so a field-scope miss still skips the segment
         * sweep entirely. */
        if (!bm25_phrase_fields_have_positions(&snap, field_store_positions,
                                               so->qfield) ||
            !bm25_phrase_segments_have_positions(index, &snap))
            need_degrade = true;

        if (need_degrade)
        {
            if (bm25_phrase_fallback_is_and(index))
            {
                ereport(WARNING,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bm25: positions not present; phrase/proximity query "
                                "downgraded to AND-of-terms (phrase_fallback='and')"),
                         errhint("REINDEX to enable exact phrase/proximity search.")));
                fallback_and = true;
                phrase = false;         /* no positional recheck; AND-filter instead */
            }
            else
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bm25: positions not present in this index; "
                                "REINDEX to enable phrase/proximity search"),
                         errhint("Or set the phrase_fallback='and' reloption to "
                                 "fall back to AND-of-terms with a WARNING.")));
        }
    }

    /* The three query modes are mutually exclusive, and from here on that is load-
     * bearing without being checked anywhere else (#67): the pending-stash dispatch
     * and the survivor filter below are if / else-if chains that take the FIRST true
     * arm, so two modes at once would not error -- the phrase arm would silently win,
     * no leaf presence bit would be marked, bm25_query_eval would never run, and a
     * boolean tree's must_not clauses would be ignored. phrase vs fallback_and is
     * exclusive by construction just above (setting one clears the other). boolean_mode
     * vs both is held only REMOTELY: bm25_rescan runs bm25_rescan_parse_phrase for a
     * text right operand only, so so->qphrase stays false whenever the operand is a
     * jsonb tree -- the only kind that can be multi-leaf. A future jsonb PHRASE root
     * that set qphrase would break that, and a cassert build now traps it here. */
    Assert(!(phrase && fallback_and));   /* invariant */
    Assert(!(boolean_mode && (phrase || fallback_and)));   /* checked: bm25_rescan */

    /* Build the phrase stash (TID -> per-(qi,field) position lists) for a live
     * phrase query. Skipped for bag-of-words and the AND fallback. */
    if (phrase && nq > 0)
    {
        /* Issue #184: group the query tokens into per-source-word slots. The STASH is
         * unchanged -- still one list per (token, field), still filled with
         * cur_qi = qi; the grouping is consulted only by phrase_recheck_tid. Legal
         * here because the cap check above already bounded nq. */
        bm25_phrase_slot_map(qtoks, nq, &qsmap);

        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize   = sizeof(ItemPointerData);
        ctl.entrysize = sizeof(PhraseStashEnt);
        ctl.hcxt      = scratchctx;
        phrase_stash = hash_create("bm25 phrase stash", 256, &ctl,
                                   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
        pctx.index        = index;
        pctx.stash        = phrase_stash;
        pctx.cxt          = scratchctx;
        pctx.nq           = (uint32) nq;
        pctx.field_count  = snap.field_count;
        /* Zeroed, not opened: there is no segment yet, and the pending pass that runs
         * first has none at all. A zero-filled BM25SegReader is the state chain_read_at
         * documents as safely unpositioned (blk 0 is the metapage, so it never claims a
         * position), which makes "no segment" a value the read path recognizes rather
         * than one that depends on nobody using it. */
        MemSet(&pctx.rdr, 0, sizeof(pctx.rdr));
        pctx.pending_tids = NULL;   /* set below once pending_tids exists */
        pctx.cur_qi       = 0;
        pctx.field_store_positions = field_store_positions;
        pctx.budget       = &budget;         /* #62.5 */
    }

    /* TID-keyed score accumulator (M1) + a set of TIDs already scored from
     * pending so segment postings can dedupe against pending (pending wins). */
    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(ItemPointerData);
    ctl.entrysize = sizeof(BM25AccEnt);
    ctl.hcxt      = scratchctx;
    acc = hash_create("bm25 score acc", 256, &ctl,
                      HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(ItemPointerData);
    ctl.entrysize = sizeof(ItemPointerData);    /* set: key only, no payload */
    ctl.hcxt      = scratchctx;
    pending_tids = hash_create("bm25 pending tids", 256, &ctl,
                               HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    /* M4: now that pending_tids exists, finish wiring the phrase stash (segment pos
     * cb dedupes against pending), or build the AND-fallback presence set. */
    if (phrase && nq > 0)
        pctx.pending_tids = pending_tids;
    if (fallback_and || boolean_mode)
    {
        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize   = sizeof(ItemPointerData);
        ctl.entrysize = sizeof(PhraseAndEnt);
        ctl.hcxt      = scratchctx;
        and_presence = hash_create("bm25 leaf presence", 256, &ctl,
                                   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
        actx.index        = index;
        actx.presence     = and_presence;
        MemSet(&actx.rdr, 0, sizeof(actx.rdr));     /* no segment yet, see pctx */
        actx.pending_tids = pending_tids;
        actx.cur_qi       = 0;               /* boolean: overridden per leaf (= leaf_bit) */
        actx.qfield       = so->qfield;      /* boolean: overridden per leaf (= leaf field) */
        actx.budget       = &budget;         /* #62.5 */
    }

    /* M6: flatten the scoring work into ONE flat list of (term, field, boost, bit)
     * so the loop below stays a single level deep. Text/single-leaf scans map
     * qtoks 1:1 (so qi still equals the query TOKEN ordinal the phrase stash keys
     * on); a boolean tree flattens to leaves, tokenizes each leaf, and emits one
     * work entry per token carrying that leaf's field/boost/leaf_bit/negated. */
    if (boolean_mode)
    {
        BM25Query  *leaves[BM25_QUERY_MAX_LEAVES];
        BM25Token  *ltoks[BM25_QUERY_MAX_LEAVES];
        int         lnt[BM25_QUERY_MAX_LEAVES];
        int         grp_of[BM25_QUERY_MAX_LEAVES]; /* phrase-group idx per leaf, -1 if not */
        int         nl,
                    i,
                    total = 0,
                    nphrase = 0;

        nl = bm25_query_flatten(so->qtree, leaves, BM25_QUERY_MAX_LEAVES);
        for (i = 0; i < nl; i++)
        {
            /* MATCH/TERM (Task 4), PHRASE (Task 6), and WILDCARD (Task 7) all reach
             * the scorer. A WILDCARD leaf is EXPANDED (not tokenized): its glob
             * pattern becomes the distinct set of dict terms it matches, each of
             * which then scores as an OR alternative under the leaf's ONE presence
             * bit -- the leaf bit sets iff ANY expansion hits (D8 OR semantics). The
             * expanded terms are the RAW stemmed dict bytes and must NOT be re-stemmed
             * (bm25_analyze), so this path bypasses the analyzer. Field scope/boost
             * come from the leaf, applied in the per-term loop below like any leaf. */
            /* checked: bm25_query_flatten returns leaf nodes only -- the tag below is
             * abbreviated only because this line has no room left for the full name. */
            Assert(leaves[i]->kind == BM25Q_MATCH || leaves[i]->kind == BM25Q_TERM ||
                   leaves[i]->kind == BM25Q_PHRASE ||
                   leaves[i]->kind == BM25Q_WILDCARD);   /* checked: query_flatten */
            if (leaves[i]->kind == BM25Q_WILDCARD)
                lnt[i] = bm25_dict_expand_wildcard(index, &snap,
                                                   leaves[i]->text, leaves[i]->textlen,
                                                   scratchctx, &ltoks[i]);
            else
                lnt[i] = bm25_analyze(&qcfg, leaves[i]->text, leaves[i]->textlen, &ltoks[i]);
            /* A TERM leaf means exactly one term (QRY-05).
             *
             * BM25Q_TERM and BM25Q_MATCH were behaviourally identical: both analyzed
             * their value and OR'd every token it produced. So `bm25_term('body','red
             * car')` matched a document containing only "red" and one containing only
             * "car" -- OR semantics from a builder named for a single term, and
             * README.md:102 documenting it as single-term. A silently wider match set
             * than the name promises is the worst of the three possible behaviours.
             *
             * Enforced HERE and not in bm25_query.c because "how many terms is this?"
             * is an analyzer question, and parse time has no Relation: the same value
             * yields one token or several depending on the index's stemmer, stopwords
             * and tokenizer. Two consequences worth stating rather than discovering:
             * the error is analyzer-dependent (a value that stems to one token is
             * legal, so the same builder call can be legal on one index and not on
             * another), and a value whose only token is a stopword yields ZERO tokens,
             * which is a different case -- left alone, because a term that analyzes
             * away matches nothing, which is already what an empty leaf does. */
            if (leaves[i]->kind == BM25Q_TERM && lnt[i] > 1)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: \"term\" query \"%.*s\" analyzes to %d terms; "
                                "\"term\" accepts exactly one",
                                leaves[i]->textlen, leaves[i]->text, lnt[i]),
                         errhint("Use \"match\" for OR semantics over several terms, "
                                 "or \"phrase\" to require them adjacent.")));

            total += lnt[i];
            grp_of[i] = -1;
            if (leaves[i]->kind == BM25Q_PHRASE)
            {
                /* The recheck's pos[]/npos[] arrays are BM25_PHRASE_MAX_TERMS wide. */
                if (lnt[i] > BM25_PHRASE_MAX_TERMS)
                    ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("bm25: phrase has too many terms (%d); max %d",
                                    lnt[i], BM25_PHRASE_MAX_TERMS)));
                grp_of[i] = nphrase++;
            }
        }

        /* M6 Task 6: a phrase leaf is answerable only from positions. If any live
         * segment is position-less, or a phrase leaf's scoped field stores no
         * positions (or a BARE phrase could land on such a field), raise the same
         * clean ERROR the (text,text) phrase path raises -- never a silent adjacency
         * miss (no positions -> empty stash -> recheck always fails -> rows dropped).
         *
         * The jsonb phrase path does NOT offer the phrase_fallback='and' downgrade,
         * and that is a CAPABILITY limit, not a policy preference -- worth stating
         * because the shared predicate above otherwise makes the two gates look
         * gratuitously different. The AND-of-terms fallback rides PhraseAndEnt's
         * single uint64 mask, whose two uses are mutually exclusive (see its
         * declaration): in fallback_and mode a bit indexes a PHRASE TERM, in
         * boolean_mode it indexes a LEAF. Degrading one leaf of a boolean tree
         * needs per-leaf, per-term presence simultaneously with the leaf bits, so
         * it needs a second presence structure -- a feature, not an argument flip.
         * Until then this surface fails loud rather than silently disagreeing with
         * the text surface about what the same phrase matches. */
        if (nphrase > 0)
        {
            bool positionless = false;

            /* Same D7/D12 predicate as the text gate, applied PER PHRASE LEAF:
             * a boolean tree carries an independent field scope on each leaf, and
             * any one of them landing on a positions-off field is enough. The
             * segment sweep is index-wide, so it runs once after the leaf loop
             * rather than inside it. */
            for (i = 0; i < nl && !positionless; i++)
            {
                if (grp_of[i] < 0)
                    continue;
                if (!bm25_phrase_fields_have_positions(&snap, field_store_positions,
                                                       leaves[i]->field_id))
                    positionless = true;
            }
            if (!positionless &&
                !bm25_phrase_segments_have_positions(index, &snap))
                positionless = true;
            if (positionless)
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bm25: positions not present in this index; REINDEX to "
                                "enable phrase/proximity search in a jsonb query tree"),
                         /* Say so ONLY to the operator whose expectation this
                          * actually violates. Someone who set phrase_fallback='and'
                          * has been told this index degrades phrases instead of
                          * failing, and on the text surface it does -- so the bare
                          * message above reads as the setting being ignored or
                          * broken, with nothing pointing at the real (capability)
                          * reason documented above. On a default index the hint
                          * would be noise about a reloption the user never set. */
                         bm25_phrase_fallback_is_and(index)
                         ? errhint("This index sets phrase_fallback='and', which "
                                   "applies only to a text-RHS phrase query "
                                   "(col @@@ '\"a b\"~2'); a jsonb query tree cannot "
                                   "degrade a phrase to AND-of-terms and fails "
                                   "instead.")
                         : 0));

            /* One stash per phrase leaf, each keyed by TID with lists sized to that
             * leaf's own token count, so distinct phrase leaves never alias cur_qi. */
            pgrps = palloc0(sizeof(BoolPhraseGrp) * nphrase);
            for (i = 0; i < nl; i++)
            {
                BoolPhraseGrp *grp;

                if (grp_of[i] < 0)
                    continue;
                grp = &pgrps[grp_of[i]];
                MemSet(&ctl, 0, sizeof(ctl));
                ctl.keysize   = sizeof(ItemPointerData);
                ctl.entrysize = sizeof(PhraseStashEnt);
                ctl.hcxt      = scratchctx;
                grp->ctx.index        = index;
                grp->ctx.stash        = hash_create("bm25 boolean phrase stash", 256,
                                                    &ctl,
                                                    HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
                grp->ctx.cxt          = scratchctx;
                grp->ctx.nq           = (uint32) lnt[i];
                grp->ctx.field_count  = snap.field_count;
                MemSet(&grp->ctx.rdr, 0, sizeof(grp->ctx.rdr));  /* no segment yet */
                grp->ctx.pending_tids = pending_tids;   /* segment pass dedupes vs pending */
                grp->ctx.cur_qi       = 0;
                grp->ctx.field_store_positions = field_store_positions;
                grp->ctx.budget       = &budget;       /* #62.5 */
                grp->field_id = leaves[i]->field_id;
                grp->slop     = leaves[i]->slop;
                grp->ordered  = leaves[i]->ordered;
                grp->leaf_bit = leaves[i]->leaf_bit;
                grp->nqterms  = (uint32) lnt[i];
                /* Built from THIS leaf's tokens; the cap check above is what lets the
                 * map's fixed-width arrays hold them. */
                bm25_phrase_slot_map(ltoks[i], lnt[i], &grp->smap);
            }
            npgrp = nphrase;
        }

        works = palloc(sizeof(BM25TermWork) * Max(total, 1));
        for (i = 0; i < nl; i++)
        {
            int j;

            for (j = 0; j < lnt[i]; j++)
            {
                works[nworks].ptr        = ltoks[i][j].ptr;
                works[nworks].len        = ltoks[i][j].len;
                works[nworks].field_id   = leaves[i]->field_id;
                works[nworks].boost      = leaves[i]->boost;
                works[nworks].leaf_bit   = leaves[i]->leaf_bit;
                works[nworks].negated    = leaves[i]->negated;
                works[nworks].phrase_grp = grp_of[i];  /* -1 for MATCH/TERM leaves */
                works[nworks].phrase_qi  = j;          /* within-phrase term index */
                nworks++;
            }
        }
    }
    else
    {
        int j;

        works = palloc(sizeof(BM25TermWork) * Max(nq, 1));
        for (j = 0; j < nq; j++)
        {
            works[j].ptr        = qtoks[j].ptr;
            works[j].len        = qtoks[j].len;
            works[j].field_id   = so->qfield;
            works[j].boost      = 1.0;
            works[j].leaf_bit   = -1;
            works[j].negated    = false;
            works[j].phrase_grp = -1;
            works[j].phrase_qi  = 0;
        }
        nworks = nq;
    }

    /* One term's dictionary lookups, filled by bm25_term_idf's df pass and read by
     * the segment pass below, so each segment's dictionary is walked once per term
     * rather than twice (issue #246). Reused term after term. */
    seglocs = (BM25TermSegLoc *) palloc(sizeof(BM25TermSegLoc) * Max(snap.nsegs, 1));

    /* Phase-1 accepted simplifications (a future single-pass optimization,
     * deferred; out of M2a Phase-2 scope):
     *  - Per-(term,segment) re-walk: the df-sum loop reads every segment header and
     *    the score loop re-reads the header of each segment that holds the term,
     *    and pending_df/bm25_pending_score_term
     *    re-walk the whole pending chain per term. (The dictionary walk is no longer
     *    repeated: the df pass hands its lookups over, issue #246.) Buffers are
     *    cached so it is lock churn, not I/O; a single pass could bin postings by
     *    term instead.
     *  - Per-term-ENTRY stride duplication in the pending walkers (see
     *    pending_df/bm25_pending_score_term and the @@@ path). */
    for (qi = 0; qi < nworks; qi++)
    {
        BM25TermWork *w      = &works[qi];
        int32         qfield = w->field_id;    /* M6: per-leaf field scope (was so->qfield) */
        double        idf_f[BM25_MAX_FIELDS];

        CHECK_FOR_INTERRUPTS();

        /* (1) Per-field idf from the shared helper -- see bm25_term_idf for the
         * single- vs multi-field df split, the C4 field-scope forcing and the M6
         * query-time boost (w->boost; 1.0 on the plain text path).
         *
         * A false return means no scored field has a non-zero df, i.e. the term is
         * absent from every field this leaf scores. Nothing to score and nothing to
         * mark, so drop it under OR semantics. That single exit also covers the
         * whole-corpus miss on the single-field path (global_df == 0 leaves
         * df_by_field[0] == 0, hence idf 0, hence false). */
        if (!bm25_term_idf(index, &snap, w->ptr, w->len,
                           live_ndocs, fld_ndocs, qfield, w->boost, idf_f, seglocs))
            continue;
        /* #268: the tail rebuild keeps the lookups above but scores with the idf the
         * capped build used. A zero pinned row (the term matched nothing then) stays
         * zero, so the term scores nothing here either: both scorers skip idf 0. */
        if (pin != NULL)
            memcpy(idf_f, pin->idf + (Size) qi * BM25_MAX_FIELDS, sizeof(idf_f));

        /* (2) PENDING first: score this term's live pending docs and register
         * their TIDs so the segment pass below dedupes against them. A must_not
         * (negated) leaf contributes NO score -- it only marks presence below (D11),
         * so skip scoring but STILL let the segment pass dedupe against whatever a
         * positive leaf already registered. */
        if (!w->negated)
            bm25_pending_score_term(index, snap.pending_head, snap.next_gen,
                                    w->ptr, w->len,
                                    snap.field_count, idf_f, avgdl_f, fcfg,
                                    acc, pending_tids, &budget);

        /* M4: stash this term's TRUE pending positions (read-your-writes, D4) BEFORE
         * the segment pass, so the segment pos cb can dedupe against pending (pending
         * wins). cur_qi is this term's query TOKEN ordinal.
         *
         * M6 boolean: mark this leaf's presence bit on every live pending doc that
         * carries the term (positive OR must_not), scoped to the leaf's own field.
         * cur_qi = leaf_bit; qfield = the leaf field. */
        if (phrase && nq > 0)
        {
            pctx.cur_qi = (uint32) qi;
            pending_phrase_stash(&pctx, snap.pending_head, snap.next_gen,
                                 w->ptr, w->len);
        }
        else if (fallback_and)
        {
            actx.cur_qi = (uint32) qi;
            pending_and_stash(&actx, snap.pending_head, snap.next_gen,
                              w->ptr, w->len);
        }
        else if (boolean_mode)
        {
            if (w->phrase_grp >= 0)
            {
                /* M6 Task 6: a PHRASE leaf's pending positions go into ITS OWN stash
                 * (cur_qi = the within-phrase term index); presence is decided later
                 * by the adjacency recheck, NOT per-term here. */
                PhraseStashCtx *pc = &pgrps[w->phrase_grp].ctx;

                pc->cur_qi = (uint32) w->phrase_qi;
                pending_phrase_stash(pc, snap.pending_head, snap.next_gen,
                                     w->ptr, w->len);
            }
            else
            {
                actx.cur_qi = (uint32) w->leaf_bit;
                actx.qfield = qfield;
                pending_and_stash(&actx, snap.pending_head, snap.next_gen,
                                  w->ptr, w->len);
            }
        }

        /* (3) EACH segment: decode this term's postings, gate on live-docs,
         * dedupe against pending, resolve TID + per-field doclen (NORMS),
         * accumulate the boosted per-field contribution. For a phrase query, ALSO
         * pass the lockstep POS cursor (pos_cb) to stash positions; bag-of-words and
         * the AND fallback pass pos_cb == NULL so the POS chain is never faulted. */
        for (si = 0; si < snap.nsegs; si++)
        {
            BM25SegmentHeader   h;
            /* This term's lookup in this segment, from the df pass above on the same
             * snapshot (issue #246). A segment's dictionary never changes after seal,
             * so it is the answer a second walk would give. */
            const BM25TermSegLoc *loc = &seglocs[si];
            BlockNumber         post_root = loc->post_root;
            uint16              post_off = loc->post_off;
            uint32              seg_df = loc->df;
            BlockNumber         pos_post_root = loc->pos_post_root;
            uint16              pos_post_off = loc->pos_post_off;
            /* Positions are needed for the text phrase path AND for any boolean
             * PHRASE leaf (whose adjacency is rechecked from the stash below). */
            bool                want_pos = (phrase && nq > 0) ||
                                          (boolean_mode && w->phrase_grp >= 0);

            CHECK_FOR_INTERRUPTS();

            /* A segment without the term is skipped before its header is read: the
             * df pass already read and gen-checked that header in this build. */
            if (loc->found)
            {
                TermScoreCtx tc;

                /* Accepted residual (#274, P1): this re-reads the header the df pass
                 * just read for this (term, segment), and on a multi-field index the
                 * init_checked below walks the LIVEDOCS bitmap the df pass's own
                 * init_checked already walked. BM25TermSegLoc does not carry either
                 * answer forward. The repeat is at most one header page plus one
                 * bitmap page per ~65k documents per (term, segment) holding the term;
                 * bounds computed on the bench corpus (bench/wand_global_ub.sh, 53
                 * segments, wand_top_k = 0, LIMIT 10, PG 18.6 cassert): at most 106
                 * of the 2,852 accesses of the rare `w4000` and at most 424 of
                 * 852,401 for `w1 w3 w10 w30`, so a
                 * wider hand-off struct is not worth its coupling yet. If it becomes
                 * worth it, the hand-off must keep ADR 0100's rule that liveness is
                 * sampled once, after the scan's MVCC snapshot. */
                bm25_seg_header_read(index, snap.segs[si].header_blkno,
                                     snap.segs[si].gen, &h);
                tc.index        = index;
                /* &h aliases this loop iteration's stack header; safe because
                 * seg_posting_cb dereferences tc.seg only synchronously inside the
                 * bm25_seg_scan_postings call below, while h is still in scope. */
                tc.seg          = &h;
                /* H16: forward cursors. Liveness (issue #246): checked once per
                 * (term, segment) against the LIVEDOCS bitmap, gated on seg_df as the
                 * df pass is, instead of one LIVEDOCS read per scored posting. ADR
                 * 0100's safety argument covers this reader as it covers WAND's: the
                 * check runs after the scan's MVCC snapshot, and this builder, too,
                 * builds its whole ranking once and returns the TIDs later without
                 * re-reading a bit, so a tombstone landing after the check is the same
                 * case as a dead tuple VACUUM has not reached yet, and the heap
                 * visibility check drops it. That last step is the index-scan path's:
                 * the TEST-ONLY bm25_debug_rank (ranked TIDs) and bm25_debug_rank_key
                 * (decoded keys and scores) probes read the ranking with no heap
                 * fetch, so they can list such a row, as they can any dead row VACUUM
                 * has not tombstoned yet -- they never had a heap check. The phrase stash and the presence pass
                 * below reuse this reader's answer (init_known), so all three gate a
                 * posting on the same liveness sample, as their callbacks' "gated
                 * identically to the scorer" contract asks. */
                (void) bm25_seg_reader_init_checked(&tc.rdr, index, &h, seg_df);
                /* Issue #267: NORMS and DOCMAP page images for the per-posting lookups.
                 * This reader and the phrase/presence readers below are re-opened per
                 * (term, segment), so each releases its images when its scan is done:
                 * at most three live at once (tc.rdr's NORMS and DOCMAP, plus one DOCMAP
                 * for the phrase or presence reader; 8 KB each), however many terms and
                 * segments the query has. In the build's scratch context, so an ERROR
                 * frees whatever was not released. */
                bm25_seg_reader_cache_pages(&tc.rdr, CurrentMemoryContext);
                tc.seg_hdr      = snap.segs[si].header_blkno;   /* M5: for docid->key */
                tc.idf_f        = idf_f;
                tc.avgdl_f      = avgdl_f;
                tc.fcfg         = fcfg;
                tc.scores       = acc;
                tc.pending_tids = pending_tids;
                tc.budget       = &budget;      /* #62.5 */

                if (want_pos)
                {
                    /* Route positions to the right stash: the global pctx on the text
                     * phrase path, or THIS phrase leaf's own stash on the boolean path.
                     * pc->seg aliases h (same synchronous-use lifetime as tc.seg). A
                     * boolean phrase leaf is always positive (must_not phrase is rejected
                     * at parse -- its non-idempotent position stash cannot honor the
                     * pending-wins dedup the way an idempotent presence bit can), so the
                     * POST cursor always scores via seg_posting_cb here. */
                    PhraseStashCtx *pc = (boolean_mode && w->phrase_grp >= 0)
                        ? &pgrps[w->phrase_grp].ctx : &pctx;

                    Assert(!w->negated);   /* checked: parse rejects a must_not PHRASE */
                    /* Re-opened every segment and every term: pc is per-query (or
                     * per-phrase-leaf), so its previous reader names the PREVIOUS
                     * segment's chains and a header that &h has since been refilled
                     * over. &h aliases this iteration's stack header, which is safe
                     * because seg_phrase_pos_cb reads through pc->rdr only synchronously
                     * inside the scan below, while h is still in scope. Same segment,
                     * same call as tc.rdr, so it takes tc.rdr's liveness check rather
                     * than walking the bitmap again (see tc.rdr above). */
                    bm25_seg_reader_init_known(&pc->rdr, index, &h, tc.rdr.assume_live);
                    bm25_seg_reader_cache_pages(&pc->rdr, CurrentMemoryContext);
                    /* seg_df bounds the decode; the lockstep pos cursor stashes each
                     * position-bearing posting's frame (D3). field_store_positions[]
                     * tells the reader which fields carry a frame (D12). */
                    bm25_seg_scan_postings(index, post_root, post_off, seg_df, h.gen,
                                           seg_posting_cb, &tc,
                                           pos_post_root, pos_post_off,
                                           seg_phrase_pos_cb, pc,
                                           field_store_positions, snap.field_count);
                    bm25_seg_reader_release_pages(&pc->rdr);
                }
                else
                {
                    /* seg_df bounds the decode to this term's run within the shared,
                     * undelimited segment-wide postings chain (D-POST). A must_not
                     * (negated) leaf scores nothing (M6 D11) -- skip seg_posting_cb.
                     * The AND fallback AND the M6 boolean path then run a SECOND
                     * decode with seg_and_cb to mark presence (positions untouched);
                     * for boolean, cur_qi/qfield were set to this leaf above. */
                    if (!w->negated)
                        bm25_seg_scan_postings(index, post_root, post_off, seg_df, h.gen,
                                               seg_posting_cb, &tc,
                                               InvalidBlockNumber, 0, NULL, NULL, NULL, 0);
                    if (fallback_and || boolean_mode)
                    {
                        /* Re-opened per (segment, term), see the phrase stash above;
                         * liveness from tc.rdr's check, as there. tc.rdr is checked
                         * even for a negated leaf, which scores nothing: this pass then
                         * makes the lookups the check was gated on. */
                        bm25_seg_reader_init_known(&actx.rdr, index, &h,
                                                   tc.rdr.assume_live);
                        bm25_seg_reader_cache_pages(&actx.rdr, CurrentMemoryContext);
                        bm25_seg_scan_postings(index, post_root, post_off, seg_df, h.gen,
                                               seg_and_cb, &actx,
                                               InvalidBlockNumber, 0, NULL, NULL, NULL, 0);
                        bm25_seg_reader_release_pages(&actx.rdr);
                    }
                }
                bm25_seg_reader_release_pages(&tc.rdr);
            }
        }
    }

    /* M6 Task 6: resolve each boolean PHRASE leaf's presence bit. Its terms already
     * scored into acc as OR terms (above); its leaf_bit is set in and_presence ONLY
     * for docs whose stashed positions satisfy the leaf's own adjacency (its field /
     * ordered / slop, NOT the global so->q*). A doc that scored from the phrase's
     * terms but fails adjacency keeps its score but not the bit -> bm25_query_eval
     * drops it if the leaf sits under a must (and keeps it under a must_not, AND-NOT).
     * Runs BEFORE the drain so the bitmask is complete when bm25_query_eval reads it.
     * Marking a TID that is not in acc is harmless (the drain iterates acc only). */
    if (boolean_mode && npgrp > 0)
    {
        int g;

        for (g = 0; g < npgrp; g++)
        {
            BoolPhraseGrp  *grp = &pgrps[g];
            HASH_SEQ_STATUS pseq;
            PhraseStashEnt *pe;

            actx.cur_qi = (uint32) grp->leaf_bit;
            hash_seq_init(&pseq, grp->ctx.stash);
            while ((pe = (PhraseStashEnt *) hash_seq_search(&pseq)) != NULL)
            {
                CHECK_FOR_INTERRUPTS();

                if (phrase_recheck_tid(pe, grp->nqterms, snap.field_count,
                                       grp->field_id, grp->ordered, grp->slop,
                                       &grp->smap))
                    phrase_and_mark(&actx, &pe->key);
            }
        }
    }

    /* Drain the hash into a flat array (still in scratchctx) and sort.
     *
     * M4 phrase recheck (D4): a phrase / AND-fallback query drops each candidate TID
     * that fails the positional matcher (phrase) or lacks all nq terms (AND). This is
     * a FILTER over the produced TID set -- the score is kept as-is (filter-only, D9),
     * the bm25_scan_build_ranking contract and the WAND seam are untouched. idx counts
     * survivors (<= the accumulator size), and every count below uses idx, not the raw
     * entry count. Bag-of-words keeps every entry (both predicates below are false). */
    nacc = hash_get_num_entries(acc);   /* long: avoid truncation on large sets */
    arr  = palloc(sizeof(BM25ExhScored) * Max(nacc, 1));
    idx  = 0;
    hash_seq_init(&seq, acc);
    while ((e = (BM25AccEnt *) hash_seq_search(&seq)) != NULL)
    {
        CHECK_FOR_INTERRUPTS();

        /* Phrase recheck: keep the TID only if the phrase holds in some field (D6). A
         * candidate absent from the stash (all its postings were on off fields, so no
         * positions) cannot match -- drop it. */
        if (phrase && nq > 0)
        {
            PhraseStashEnt *pe;
            bool            found;

            pe = (PhraseStashEnt *) hash_search(phrase_stash, &e->key,
                                                HASH_FIND, &found);
            if (!found ||
                !phrase_recheck_tid(pe, (uint32) nq, snap.field_count,
                                    so->qfield, so->qphrase_ordered, so->qslop,
                                    &qsmap))
                continue;               /* filtered: fails the positional match */
        }
        else if (fallback_and)
        {
            PhraseAndEnt *ae;
            bool          found;
            uint64        full = (nq >= 64)
                ? ~UINT64CONST(0)
                : ((UINT64CONST(1) << nq) - 1);

            ae = (PhraseAndEnt *) hash_search(and_presence, &e->key,
                                              HASH_FIND, &found);
            if (!found || (ae->mask & full) != full)
                continue;               /* AND fallback: not all nq terms present */
        }
        else if (boolean_mode)
        {
            /* M6: run the boolean formula (must/should/must_not, D5/D11) over this
             * doc's leaf presence bitmask. Only positive leaves put a doc in `acc`,
             * so every candidate here has at least one positive hit; the mask (0 if
             * the doc never reached and_presence -- impossible for a scored doc, but
             * defensive) also carries any must_not leaf bits, which the formula
             * rejects on. Filter-only: survivors keep their accumulated score. */
            PhraseAndEnt *ae;
            bool          found;
            uint64        mask;

            ae = (PhraseAndEnt *) hash_search(and_presence, &e->key,
                                              HASH_FIND, &found);
            mask = found ? ae->mask : 0;
            if (!bm25_query_eval(so->qtree, mask))
                continue;               /* boolean formula rejects this doc */
        }

        arr[idx].tid       = e->key;
        arr[idx].score     = e->score;
        arr[idx].src_hdr   = e->src_hdr;   /* M5: segment source for docid->key */
        arr[idx].src_gen   = e->src_gen;
        arr[idx].src_docid = e->src_docid;
        idx++;
    }
    nacc = idx;                         /* survivors only: drive all downstream counts */
    hash_destroy(acc);
    hash_destroy(pending_tids);
    if (phrase_stash != NULL)
        hash_destroy(phrase_stash);
    if (and_presence != NULL)
        hash_destroy(and_presence);

    qsort(arr, nacc, sizeof(BM25ExhScored), scored_desc);

    /* M5 key_field: the index-wide key config (bm25_ranked_key_config: the #292
     * stamp, else discovered from segments, then pending records). BM25_KEY_NONE =>
     * keyless index, ctid-only path (ranked_keys stays NULL, nothing allocated). Done
     * BEFORE switching to scanctx so the transient header read stays in scratchctx. */
    bm25_ranked_key_config(index, &snap, &km_type, &km_size);

    /* Ranked-row key readers, one per segment the loop below meets (issue #225). In
     * scratchctx: the cache is loop scratch and must not follow the result arrays into
     * scanctx. Only a keyed index resolves keys, so a keyless one skips the hash. */
    if (km_type != BM25_KEY_NONE)
    {
        bm25_seg_key_cache_init(&keycache, index, scratchctx);
        /* The rows' key sources, collected in rank order below and resolved by
         * bm25_seg_key_cache_fill (issue #246). Scratch, like the cache. */
        kreqs = (BM25KeyReq *) palloc(sizeof(BM25KeyReq) * Max(nacc, 1));
    }

    /*
     * Allocate the persistent result arrays in scanctx so they outlive this
     * function and survive across gettuple calls.
     */
    MemoryContextSwitchTo(so->scanctx);

    /* Any lazily-built score index describes the ranking we are about to REPLACE.
     * bm25_build_score_index's own precondition is that so->ranked/scores/
     * ranked_keys are already at their FINAL nranked, so a hash built from an
     * earlier (WAND-capped) array answers NULL for every row past that array's
     * length. Dropped here, at the assignment, rather than at the one caller that
     * happens to rebuild today (bm25_gettuple's over-pull tail), so any future
     * rebuild path is covered by construction. The old hashes live in scanctx and
     * die with the scan, so leaking them for one statement is harmless. */
    so->score_by_tid = NULL;
    so->score_by_key = NULL;

    so->ranked = palloc(sizeof(BM25Posting) * Max(nacc, 1));
    so->scores = palloc(sizeof(double) * Max(nacc, 1));
    /* M5: parallel key array only when the index is keyed. Each ranked row's key is
     * resolved from its winning segment source (src_hdr/gen/docid) through keycache;
     * a pending-only row (Invalid src_hdr) has no key. One header read and one extent
     * capture per distinct SEGMENT, not per row (issue #225), and one KEYMAP lookup
     * per row, made after this loop by bm25_seg_key_cache_fill, which reads each
     * multi-page KEYMAP in one forward pass (issue #246).
     *
     * ranked_key_present is allocated with (never instead of) ranked_keys and marks
     * WHICH slots were actually resolved -- the zero bytes left in an unresolved slot
     * are a valid key for id = 0, so they cannot carry that signal themselves. See
     * the ranked_key_present comment in bm25.h. palloc0 => every slot starts absent
     * and is turned on only by a resolve that succeeded, here or in the pending
     * backfill below. */
    if (km_type != BM25_KEY_NONE)
    {
        so->ranked_keys = palloc0((Size) Max(nacc, 1) * km_size);
        so->ranked_key_present = (bool *) palloc0(sizeof(bool) * Max(nacc, 1));
        so->ranked_key_type = km_type;
        so->ranked_key_size = km_size;
    }
    else
    {
        so->ranked_keys = NULL;
        so->ranked_key_present = NULL;
        so->ranked_key_type = BM25_KEY_NONE;
        so->ranked_key_size = 0;
    }
    for (idx = 0; idx < (int) nacc; idx++)
    {
        CHECK_FOR_INTERRUPTS();

        so->ranked[idx].tid    = arr[idx].tid;
        so->ranked[idx].tf     = 0;     /* unused; only .tid matters here */
        so->ranked[idx].doclen = 0;
        so->scores[idx]        = arr[idx].score;

        if (so->ranked_keys != NULL && arr[idx].src_hdr != InvalidBlockNumber)
        {
            kreqs[nkreq].header_blkno = arr[idx].src_hdr;
            kreqs[nkreq].gen          = arr[idx].src_gen;
            kreqs[nkreq].local_docid  = arr[idx].src_docid;
            kreqs[nkreq].slot         = (uint32) idx;
            nkreq++;
        }
    }
    /* The slot copy (only the bytes the row's segment reported, the rest zeroed) and
     * the present flag (set only on a true return: a segment with no KEYMAP leaves an
     * all-zero slot that is byte-identical to a real id = 0) are the helper's; see its
     * header comment. */
    if (so->ranked_keys != NULL)
        bm25_seg_key_cache_fill(&keycache, kreqs, nkreq, so->ranked_keys,
                                so->ranked_key_present, km_size);
    so->nranked = (uint32) nacc;
    so->rcur    = 0;

    /* Rows the pending scorer owns have no segment to resolve a key from; read it
     * off the pending record instead. Must run AFTER nranked is final -- it walks
     * ranked_key_present[0..nranked). */
    bm25_ranked_keys_fill_from_pending(index, so, snap.pending_head, snap.next_gen);

    /* Drop all scratch allocations (dynahashes, token array, arr, etc.). */
    MemoryContextSwitchTo(oldctx);
    MemoryContextDelete(scratchctx);
}

/* -------------------------------------------------------------------------
 * #290: the filtered build -- a scan that must apply WHERE keys of its own
 * -------------------------------------------------------------------------
 * TID sets here are ascending, duplicate-free ItemPointerData arrays. A ranking
 * holds each TID at most once (the exhaustive scorer accumulates by TID), so the
 * sorted TIDs of one are a set.
 */
static int
tid_asc(const void *a, const void *b)
{
    return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

/* The TIDs of s's ranking as a set, in CurrentMemoryContext. */
static ItemPointerData *
bm25_ranked_tid_set(const BM25ScanOpaque s, uint32 *n)
{
    ItemPointerData *set = palloc(sizeof(ItemPointerData) * Max(s->nranked, 1));
    uint32           i;

    for (i = 0; i < s->nranked; i++)
        set[i] = s->ranked[i].tid;
    qsort(set, s->nranked, sizeof(ItemPointerData), tid_asc);
    *n = s->nranked;
    return set;
}

static bool
bm25_tid_set_contains(const ItemPointerData *set, uint32 n, ItemPointer t)
{
    return bsearch(t, set, n, sizeof(ItemPointerData), tid_asc) != NULL;
}

/* a := a INTERSECT b, in place; returns the new length. */
static uint32
bm25_tid_set_intersect(ItemPointerData *a, uint32 na,
                       const ItemPointerData *b, uint32 nb)
{
    uint32 i = 0,
           j = 0,
           k = 0;

    while (i < na && j < nb)
    {
        int c = ItemPointerCompare(&a[i], (ItemPointer) &b[j]);

        if (c < 0)
            i++;
        else if (c > 0)
            j++;
        else
        {
            a[k++] = a[i];
            i++;
            j++;
        }
    }
    return k;
}

/* Release the result arrays a build left on s, and forget them. */
static void
bm25_ranked_release(BM25ScanOpaque s)
{
    if (s->ranked != NULL)
        pfree(s->ranked);
    if (s->scores != NULL)
        pfree(s->scores);
    if (s->ranked_keys != NULL)
        pfree(s->ranked_keys);
    if (s->ranked_key_present != NULL)
        pfree(s->ranked_key_present);
    s->ranked = NULL;
    s->scores = NULL;
    s->ranked_keys = NULL;
    s->ranked_key_present = NULL;
    s->ranked_key_type = BM25_KEY_NONE;
    s->ranked_key_size = 0;
    s->nranked = 0;
}

/*
 * bm25_scan_build_filtered -- the SQL answer for a scan with WHERE keys of its own.
 *
 * The result of `WHERE k1 AND ... AND kn ORDER BY o` is the rows satisfying every
 * WHERE key, in the order o gives them; an ORDER BY never removes a row. So:
 *
 *   W = the intersection of every WHERE key's membership set -- including M when a
 *       WHERE key is the ORDER BY query itself (so->where_has_orderby: bm25_rescan
 *       gave that key no opaque, its set being M), and
 *   M = the ORDER BY query's membership set, with its ranking.
 *
 * The scan emits the ranking's rows that are in W, in rank order, then W \ M -- the
 * WHERE rows the ORDER BY query does not match -- at distance +Infinity, the value
 * &@@ gives every row off the index. Plain intersection (only W n M) is NOT the
 * answer: it drops exactly those rows. Without an ORDER BY key this scan's own
 * query is the first WHERE key and the result is W alone, in TID order.
 *
 * Every set comes from the exhaustive scorer, the computation @@@ already delegates
 * a phrase or a boolean tree to, so each key's set is what that key alone returns
 * through the index. For a plain OR query its membership equals the flat @@@ union:
 * a document is in it iff some token's posting reaches it from a live source
 * (pending_tids only stops a TID being SCORED twice, and it holds only TIDs that
 * already matched). Using it for the ORDER BY side as well makes M the ranking
 * itself: the exhaustive ranking is uncapped, so "not in M" and "not ranked" are
 * one test, and W \ M cannot place a finitely scored row at +Infinity. That is also
 * why this build never takes WAND (D5 of the #290 decisions): a capped ranking is
 * not M, and the over-pull tail rebuild (ADR 0108) is never armed for it.
 *
 * ONE SNAPSHOT. Every set is computed from one bm25_scan_snapshot, so a concurrent
 * seal or merge cannot move a document between sources between two of them and
 * make it look absent from one set. The whole build is one attempt of the retry
 * wrapper: a seg_gen abort in any of its parts rolls back all of it, and the next
 * attempt captures a fresh snapshot for all of them.
 *
 * Memory: each exhaustive build charges its OWN bm25_native.max_match_memory budget
 * (a budget is per build) and frees its scratch. The builds run one after another,
 * and each WHERE key's ranking is released as soon as its TID set is extracted, so
 * the peak is about two budgets whatever nwhere is: the ORDER BY ranking, which is
 * held across the loop, plus the one WHERE build in flight, plus the TID sets.
 */
static void
bm25_scan_build_filtered(Relation index, BM25ScanOpaque so,
                         const char *query, int querylen)
{
    MemoryContext    scratchctx;
    MemoryContext    oldctx;
    BM25ScanSnapshot snap;
    ItemPointerData *wset = NULL;
    uint32           nw = 0;
    int              i;

    Assert(so->nwhere > 0);     /* checked: bm25_scan_build_ranking_once's only call */

    /* Scratch for the snapshot's catalog copy, the WHERE builds' results and the TID
     * sets; deleted before return. Parented under the retry wrapper's subtransaction
     * context for the reason the exhaustive scorer gives for its own. */
    scratchctx = AllocSetContextCreate(CurrentMemoryContext,
                                       "bm25 filtered build scratch",
                                       ALLOCSET_SMALL_SIZES);
    oldctx = MemoryContextSwitchTo(scratchctx);

    bm25_scan_snapshot(index, &snap);

    /* This scan's own query, ranked into so->ranked (scanctx) as usual. */
    bm25_scan_build_ranking_exhaustive(index, so, query, querylen, NULL, &snap);

    /* Each WHERE key's set. Its opaque's results go to scratch, not scanctx: only
     * the TIDs are kept, and only until the composition below. scanctx is put back
     * by VALUE, not saved and restored: after a retried attempt the saved value
     * would be the rolled-back attempt's scratch context. */
    for (i = 0; i < so->nwhere; i++)
    {
        BM25ScanOpaque   w = so->where[i];
        ItemPointerData *m;
        uint32           nm;

        CHECK_FOR_INTERRUPTS();

        w->scanctx = scratchctx;
        bm25_scan_build_ranking_exhaustive(index, w, w->qterm, w->qtermlen,
                                           NULL, &snap);
        m = bm25_ranked_tid_set(w, &nm);
        bm25_ranked_release(w);
        w->scanctx = so->scanctx;

        if (wset == NULL)
        {
            wset = m;
            nw = nm;
        }
        else
        {
            nw = bm25_tid_set_intersect(wset, nw, m, nm);
            pfree(m);
        }
    }

    if (!so->scoring)
    {
        /* No ORDER BY: this scan's query is the first WHERE key. Its set joins the
         * intersection, and the result replaces the ranking in TID order (the
         * unordered @@@ path copies only the TIDs). */
        uint32           no;
        ItemPointerData *oset = bm25_ranked_tid_set(so, &no);
        uint32           n = bm25_tid_set_intersect(wset, nw, oset, no);

        bm25_ranked_release(so);
        MemoryContextSwitchTo(so->scanctx);
        so->ranked = palloc0(sizeof(BM25Posting) * Max(n, 1));
        so->scores = palloc0(sizeof(double) * Max(n, 1));
        for (i = 0; i < (int) n; i++)
            so->ranked[i].tid = wset[i];
        so->nranked = n;
    }
    else
    {
        uint32           no;
        ItemPointerData *oset = bm25_ranked_tid_set(so, &no);
        ItemPointerData *rem = palloc(sizeof(ItemPointerData) * Max(nw, 1));
        uint32           nkeep = 0,
                         nrem = 0,
                         total,
                         out = 0,
                         r,
                         j,
                         k;
        uint8            ktype = so->ranked_key_type;
        uint16           ksize = so->ranked_key_size;
        BM25Posting     *ranked;
        double          *scores;
        unsigned char   *keys = NULL;
        bool            *present = NULL;

        /* A WHERE key identical to the ORDER BY key was not given a set of its own
         * (bm25_rescan); its set is M, so it joins the intersection here. W is then
         * inside M and the unmatched tail below comes out empty, as it must: every
         * WHERE row satisfies the ORDER BY query. */
        if (so->where_has_orderby)
            nw = bm25_tid_set_intersect(wset, nw, oset, no);

        for (r = 0; r < so->nranked; r++)
        {
            CHECK_FOR_INTERRUPTS();
            if (bm25_tid_set_contains(wset, nw, &so->ranked[r].tid))
                nkeep++;
        }
        /* W \ M, by a merge of the two ascending sets, so it comes out in TID order:
         * the order scored_desc gives rows of equal score, which keeps the whole
         * array in the ranking's own order. */
        for (j = 0, k = 0; j < nw; j++)
        {
            while (k < no && ItemPointerCompare(&oset[k], &wset[j]) < 0)
                k++;
            if (k >= no || ItemPointerCompare(&oset[k], &wset[j]) != 0)
                rem[nrem++] = wset[j];
        }
        total = nkeep + nrem;

        MemoryContextSwitchTo(so->scanctx);
        ranked = palloc0(sizeof(BM25Posting) * Max(total, 1));
        scores = palloc(sizeof(double) * Max(total, 1));
        if (so->ranked_keys != NULL)
        {
            /* The unmatched rows get no key from this build: their slots stay
             * absent, so a key probe for one answers NULL, as a ctid probe does. */
            keys = palloc0((Size) Max(total, 1) * ksize);
            present = palloc0(sizeof(bool) * Max(total, 1));
        }
        for (r = 0; r < so->nranked; r++)
        {
            CHECK_FOR_INTERRUPTS();
            if (!bm25_tid_set_contains(wset, nw, &so->ranked[r].tid))
                continue;
            ranked[out] = so->ranked[r];
            scores[out] = so->scores[r];
            if (keys != NULL)
            {
                memcpy(keys + (Size) out * ksize,
                       so->ranked_keys + (Size) r * ksize, ksize);
                present[out] = so->ranked_key_present[r];
            }
            out++;
        }
        for (j = 0; j < nrem; j++)
        {
            ranked[out].tid = rem[j];
            scores[out] = -get_float8_infinity();   /* bm25_score_is_unmatched */
            out++;
        }
        Assert(out == total);   /* invariant */

        bm25_ranked_release(so);
        so->ranked = ranked;
        so->scores = scores;
        so->ranked_keys = keys;
        so->ranked_key_present = present;
        so->ranked_key_type = (keys != NULL) ? ktype : BM25_KEY_NONE;
        so->ranked_key_size = (keys != NULL) ? ksize : 0;
        so->nranked = total;
    }

    /* The score indexes, if any, describe the arrays just replaced. */
    so->score_by_tid = NULL;
    so->score_by_key = NULL;
    so->rcur = 0;

    MemoryContextSwitchTo(oldctx);
    MemoryContextDelete(scratchctx);
}

/*
 * bm25_scan_build_ranking_once -- M2b seam dispatcher (Task 9).
 *
 * Every ranked (&@@) scan reaches ranking through here (via the retry wrapper
 * below). Decides between the Block-Max WAND driver (bm25_wand_build_ranking,
 * default on) and the exhaustive OR-sum scorer above that WAND replaces:
 *
 *   WAND gate (D7): so->scoring AND !so->qphrase AND
 *   bm25_native.wand_top_k > 0 AND !bm25_qtree_is_multileaf(so->qtree).
 *
 *   A boolean @@@ scan is NOT automatically absent here. A plain bare-term @@@
 *   scan never arrives -- bm25_gettuple routes it to the unordered postings
 *   path before ranking is ever considered -- but a MULTI-LEAF jsonb @@@ filter
 *   and (since #132) a TEXT PHRASE @@@ filter both reach this dispatcher,
 *   because bm25_load_if_needed computes their membership sets by calling
 *   bm25_scan_build_ranking, which re-enters here. What excludes both from WAND
 *   is the so->scoring conjunct: so->scoring is false on that path (D10), so the
 *   gate falls through to the exhaustive scorer -- exactly what they want (the
 *   presence-mask evaluator for the tree, the positional recheck for the phrase).
 *   The phrase case is doubly excluded, since !so->qphrase is a conjunct too.
 *
 *   The design's full gate also conjoins !fallback_and (phrase_fallback='and'
 *   AND-of-terms downgrade), but that conjunct is PROVABLY redundant here:
 *   fallback_and starts false and is set true only inside the exhaustive
 *   scorer's `if (phrase && nq > 0)` degradation block, where `phrase` is
 *   itself so->qphrase's initial value -- so fallback_and can never be true
 *   when so->qphrase is false. Excluding phrase queries via !so->qphrase
 *   alone already excludes every fallback_and case, without having to run
 *   the phrase-degradation check (tokenize + per-segment pos_root probe)
 *   just to compute a conjunct that is provably false at this point.
 *
 * On the WAND path, this function runs the SHARED corpus-stats prologue
 * (bm25_scan_corpus_stats: ONE atomic snapshot, the section I fingerprint gate, then
 * BM25F per-field config/avgdl) -- the same call the exhaustive scorer makes,
 * which is precisely why WAND scores off IDENTICAL stats (D8's bit-exactness
 * foundation) -- then hands off to bm25_wand_build_ranking, which fills
 * so->ranked/scores/nranked/rcur/wand_capped and projects so->ranked_keys
 * itself (M5, mirrored from the exhaustive finalize). The phrase stash /
 * AND-fallback machinery stays out of the shared prologue and remains local to
 * the exhaustive body, which is the only path that needs it.
 *
 * so->wand_capped is unconditionally cleared first: it must be false whenever
 * the exhaustive path is taken (a stale true from a PRIOR scan iteration
 * would wrongly arm bm25_gettuple's Task 10 tail fallback), and the WAND path
 * overwrites it with the real answer before returning.
 *
 * force_exhaustive (M2b fix): the over-pull tail fallback rebuilds the ranking
 * a second time on the SAME scan to recover rows past wand_top_k, and it must
 * run that rebuild through bm25_scan_build_ranking (the retry wrapper) so a
 * seg_gen abort during the rebuild retries instead of surfacing to the user.
 * But the wrapper always re-enters this dispatcher, and so->wand_capped is
 * still true from the FIRST build at that point -- without force_exhaustive the
 * gate below would fire again, re-run WAND, and re-produce the same capped
 * top-k (nranked=k), silently dropping rows k+1..N instead of recovering them.
 * force_exhaustive short-circuits the gate so this second pass always falls
 * through to the exhaustive scorer, which clears wand_capped for real. It also
 * hands that scorer the first build's pinned statistics (so->stats_pin, #268), so
 * the rebuilt ranking extends what was emitted instead of re-scoring it.
 */
static void
bm25_scan_build_ranking_once(Relation index, BM25ScanOpaque so,
                             const char *query, int querylen,
                             bool force_exhaustive)
{
    /* Test lever (t/033): one hit per build attempt, so a parked attempt sits inside
     * the wrapper's subtransaction on a primary and outside any on a standby. */
    bm25_debug_pause_point("rank_build_attempt");

    so->wand_capped = false;
    /* #268: a non-forced build starts a new ranking, so any pin from an earlier one
     * no longer describes what this scan has emitted; the WAND branch sets a fresh
     * one when it caps. A forced build is the tail rebuild (or a debug probe on a
     * zeroed opaque, whose pin is NULL) and consumes the pin as it stands. */
    if (!force_exhaustive)
        so->stats_pin = NULL;

    /* #290: WHERE keys of the scan's own to apply. Always exhaustive (never WAND),
     * for both the scored and the unscored scan; see bm25_scan_build_filtered. */
    if (so->nwhere > 0)
    {
        bm25_scan_build_filtered(index, so, query, querylen);
        return;
    }

    /* WAND is OR-of-terms only: it cannot honor per-leaf field scope, query boost,
     * or must_not exclusion, so a BOOLEAN/BOOST jsonb tree (bm25_qtree_is_multileaf)
     * must fall through to the exhaustive presence-mask scorer. A single MATCH/TERM
     * leaf (copied into so->qterm) is a plain OR query and stays WAND-eligible. */
    if (!force_exhaustive && so->scoring && !so->qphrase && bm25_wand_top_k > 0 &&
        !bm25_qtree_is_multileaf(so->qtree))
    {
        BM25ScanSnapshot   snap;
        BM25AnalyzerConfig qcfg;
        BM25Token         *qtoks;
        int                nq;
        uint64             live_ndocs;
        BM25FieldConfig    fcfg[BM25_MAX_FIELDS];
        double             avgdl_f[BM25_MAX_FIELDS];
        uint64             fld_ndocs[BM25_MAX_FIELDS];
        MemoryContext      scratchctx,
                           oldctx;

        /* Scratch (snap.segs, qtoks, fcfg-derived reads) lives in its own child
         * context, deleted before return -- mirrors the exhaustive body's
         * scratchctx discipline so a retried attempt (the wrapper below) never
         * accumulates leftover per-attempt allocations in the subxact context.
         *
         * H17: this must be created BEFORE the corpus-stat prologue for the comment
         * above to be true of snap.segs. It used to sit after the prologue, so the
         * catalog copy and the corpus-stat reads landed in the subtransaction
         * context and lived to end of transaction -- the comment named snap.segs as
         * covered while the code allocated it three statements too early. snap is
         * last read by bm25_wand_build_ranking below, still inside the context. */
        scratchctx = AllocSetContextCreate(CurrentMemoryContext,
                                           "bm25 wand scan scratch",
                                           ALLOCSET_SMALL_SIZES);
        oldctx = MemoryContextSwitchTo(scratchctx);

        /* Shared prologue (D-SNAP/C3 + section I + BM25F stats) -- see
         * bm25_scan_corpus_stats. No store_pos: WAND never reads positions. */
        bm25_scan_corpus_stats(index, &snap, &qcfg, NULL,
                               &live_ndocs, fcfg, avgdl_f, fld_ndocs);

        /* Same analyzer as every other path: stem the query against the
         * fingerprint-gated config just resolved above. */
        nq = bm25_analyze(&qcfg, query, querylen, &qtoks);

        bm25_wand_build_ranking(index, so, &snap, qtoks, nq, live_ndocs,
                                avgdl_f, fld_ndocs, fcfg, so->qfield,
                                bm25_wand_top_k, NULL);

        MemoryContextSwitchTo(oldctx);
        MemoryContextDelete(scratchctx);
        return;
    }

    bm25_scan_build_ranking_exhaustive(index, so, query, querylen,
                                       force_exhaustive ? so->stats_pin : NULL, NULL);
}

/*
 * bm25_scan_build_ranking -- bounded in-scan retry on the clean reuse-safety abort
 * (D-HORIZON / C4).
 *
 * Horizon bound (accurate statement): on the PRIMARY the executor's active
 * snapshot advertises this backend's xmin, so GlobalVisCheckRemovableFullXid will
 * NOT report a segment's retire_xid as removable while this scan is live -- the
 * pages backing a segment we are reading cannot be reclaimed-and-reused under us,
 * so a primary scan essentially never hits the seg_gen-mismatch abort. The clean
 * abort is the STANDBY / edge mechanism: a Hot Standby whose snapshot predates a
 * primary-side reclaim (notably with hot_standby_feedback=off) can follow a
 * pointer to a page reused for a new segment; option-(d) seg_gen validation
 * (bm25_seg_page_validate) turns that into this retryable abort instead of wrong
 * results. Outside recovery we catch exactly that one abort and retry with a FRESH
 * snapshot in a subtransaction so it is never user-visible; in recovery there is no
 * retry (the hot-standby paragraph below). The retry is bounded (3 attempts) to
 * avoid livelock against a pathologically aggressive concurrent merge; exhausting
 * it re-throws (the caller sees a normal serialization failure -- the documented
 * floor behavior).
 *
 * Context/owner discipline: each attempt (on a primary) runs inside an internal
 * subtransaction (so the abort is contained and any partial buffer pins it took
 * are released).
 * CurrentMemoryContext and CurrentResourceOwner are captured before
 * BeginInternalSubTransaction and RESTORED on EVERY exit path (success, retry,
 * re-throw), because Begin/Release/Rollback swap them to the subxact's. The final
 * ranked[]/scores[] live in so->scanctx, which is owned by the scan (created in
 * bm25_beginscan), not by the subtransaction, so they survive the Release.
 *
 * force_exhaustive is threaded straight through to bm25_scan_build_ranking_once
 * (see its header comment) -- it does not change the retry semantics here, only
 * which of the dispatcher's two branches runs. This lets bm25_gettuple's
 * over-pull tail rebuild reuse this SAME retry wrapper (instead of calling
 * bm25_scan_build_ranking_exhaustive directly), so a seg_gen abort hit while
 * re-reading pages for the tail rebuild retries silently rather than surfacing.
 *
 * Hot standby (#307, decision D22): in recovery the build runs ONCE, directly, with
 * no subtransaction and no retry. The subtransaction is what makes a standby unsafe:
 * core's ProcessRecoveryConflictInterrupt resolves a lock/snapshot/bufferpin/
 * tablespace conflict with an ERROR only when !IsSubTransaction(), and otherwise
 * terminates the session (FATAL), because a subtransaction could catch the ERROR.
 * The build is dense with CHECK_FOR_INTERRUPTS, so a conflict delivered mid-build
 * landed inside this wrapper's subtransaction and killed the client's connection
 * instead of cancelling its statement. Without the subtransaction it is core's
 * ordinary 40001 statement cancel. The cost: the reuse abort this wrapper exists
 * for -- a standby-only event, see the horizon bound above -- now reaches a standby
 * client as the same 40001 rather than being retried away. Standby clients must
 * retry core's conflict 40001 anyway, and @@@ already reported the abort that way
 * (ADR 0110). No catch-based retry is possible in recovery: it would also catch
 * core's conflict 40001, which is exactly what core escalates to FATAL to prevent.
 * Unchanged on the primary, where no recovery conflict can be delivered.
 *
 * Without the subtransaction the dispatcher's scratch contexts are children of the
 * caller's context rather than the subtransaction's (ADR 0036): every branch deletes
 * them on success, statement abort reclaims them on error, and everything the build
 * keeps (ranked, scores, keys, stats_pin) is allocated in so->scanctx either way. A
 * promotion mid-scan is harmless: each build decides afresh.
 */
/* Livelock guard: cap retries of the clean seg_gen reuse abort. Exhausting it
 * re-throws the serialization failure (the documented floor behavior). */
#define BM25_SCAN_RETRY_LIMIT 3
void
bm25_scan_build_ranking(Relation index, BM25ScanOpaque so,
                        const char *query, int querylen,
                        bool force_exhaustive)
{
    int attempts = 0;

    if (RecoveryInProgress())
    {
        bm25_scan_build_ranking_once(index, so, query, querylen, force_exhaustive);
        return;
    }

    for (;;)
    {
        MemoryContext   oldcxt = CurrentMemoryContext;
        ResourceOwner   oldowner = CurrentResourceOwner;
        bool            succeeded = false;
        BM25Posting    *prev_ranked = so->ranked;

        BeginInternalSubTransaction(NULL);
        PG_TRY();
        {
            bm25_scan_build_ranking_once(index, so, query, querylen,
                                         force_exhaustive);
            ReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcxt);
            CurrentResourceOwner = oldowner;
            /* Do NOT `return` here: returning out of a PG_TRY block skips
             * PG_END_TRY, which is what restores PG_exception_stack. A bare
             * `return` would leave PG_exception_stack pointing at this frame's
             * now-dead sigjmp_buf, so the NEXT ereport in the same statement
             * (e.g. a target-list `id/0` evaluated during projection of the
             * scored scan's output) longjmps into this reclaimed frame and
             * re-enters the CATCH arm, calling RollbackAndReleaseCurrentSub-
             * Transaction on an already-released subxact -> backend FATAL
             * ("unexpected state"). Signal success and leave via PG_END_TRY. */
            succeeded = true;
        }
        PG_CATCH();
        {
            ErrorData *edata;

            /* Switch back to a context that outlives the subxact before copying
             * the error (the subxact's contexts are torn down on rollback). */
            MemoryContextSwitchTo(oldcxt);
            edata = CopyErrorData();
            if (edata->sqlerrcode != ERRCODE_T_R_SERIALIZATION_FAILURE ||
                ++attempts >= BM25_SCAN_RETRY_LIMIT)
            {
                /* Not our clean reuse-safety abort, or out of retries: roll back
                 * the subxact, restore context/owner, and propagate. PG_RE_THROW
                 * exits via longjmp, so PG_END_TRY is (correctly) not reached. */
                FreeErrorData(edata);
                RollbackAndReleaseCurrentSubTransaction();
                MemoryContextSwitchTo(oldcxt);
                CurrentResourceOwner = oldowner;
                PG_RE_THROW();
            }
            /* Our retryable abort: discard it, roll back, restore, and re-run the
             * worker with a fresh snapshot on the next loop iteration. */
            FlushErrorState();
            FreeErrorData(edata);
            RollbackAndReleaseCurrentSubTransaction();
            MemoryContextSwitchTo(oldcxt);
            CurrentResourceOwner = oldowner;

            /* Issue #313 XCUT-15: both builders assign ranked/scores/ranked_keys in
             * so->scanctx (which the rollback does not touch) BEFORE their retryable
             * key fills, so a failed attempt that got that far left its arrays on
             * so->. The next attempt would overwrite them unfreed, up to two dead
             * nacc-sized copies per build until endscan. Released only when this
             * attempt replaced the pointer: arrays that predate it (the capped
             * ranking an over-pull tail rebuild replaces) are left exactly as a
             * successful build would leave them.
             *
             * Residual: the filtered builder pfrees the previous arrays before
             * allocating its own, so its new array can land on prev_ranked's
             * address; that attempt's arrays then stay until endscan, as before. */
            if (so->ranked != prev_ranked)
                bm25_ranked_release(so);
        }
        PG_END_TRY();

        /* Reached only via normal fall-through from PG_TRY/PG_CATCH (never via a
         * longjmp), so PG_exception_stack is restored. Return on success; loop to
         * retry the clean seg_gen reuse abort with a fresh snapshot otherwise. */
        if (succeeded)
            return;
    }
}
