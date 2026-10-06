/* bm25_stats.c -- the statistics layer both ranking builders stand on: per-term
 * df -> idf, the pending list's contribution to a term's score, the TID-keyed
 * accumulator that contribution lands in, the match-set memory budget that
 * accumulator charges, and the pending-list backfill of ranked-row keys.
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT (#67.13, ADR 0093). The exhaustive scorer
 * (bm25_scan_rank.c) and the Block-Max WAND driver (bm25_wand.c) must compute a term's
 * idf and score the pending list with the SAME code: that identity is what makes
 * WAND bit-exact against the exhaustive path (D8). While the code was file-static
 * in bm25_scan.c, the WAND driver could only reach it by calling back into the
 * scanner that had just called it (bm25_scan.c's dispatcher -> bm25_wand_build_ranking
 * -> two helpers defined in bm25_scan.c), so the two files called each other.
 * Moving the shared machinery down a layer leaves the three files with no call
 * running back up:
 *
 *     bm25_scan_rank.c ----> bm25_wand.c ----> bm25_stats.c
 *          |                                       ^
 *          +---------------------------------------+
 *
 * (The scanner was one file, bm25_scan.c, when this layer was cut. Since #228 its
 * ranking builders are in bm25_scan_rank.c, which is the file that calls the WAND
 * driver; the scanner's other two files, bm25_scan.c and bm25_scan_match.c, call
 * this layer directly for the match-set budget and nothing in bm25_wand.c.)
 *
 * That is a claim about direct calls between these three files, not about the
 * whole extension. Elsewhere, pairs of files still call each other directly, and
 * a path can leave bm25_stats.c through a lower module and come back up (for
 * example, bm25_seg_dict.c calls bm25_query.c, which calls bm25_field_by_name in
 * bm25_scan.c).
 *
 * Not to be confused with the bm25_stats() SQL function, which reports an
 * index's statistics and is defined in bm25_meta.c.
 *
 * THE LAYERING RULE. Nothing here calls anything defined in the scanner
 * (bm25_scan.c, bm25_scan_rank.c, bm25_scan_match.c) or bm25_wand.c. What this
 * file uses is the storage beneath it (bm25_seg_read.c,
 * bm25_seg_dict.c, bm25_seg_chain.c, bm25_pending.c), the scoring formulas
 * (bm25_score.c), and one GUC variable (bm25_max_match_memory_kb, defined with
 * the other GUCs in bm25_handler.c). A
 * helper that needs a scanner or WAND symbol belongs in that caller's file. That
 * is why the two WAND-shaped adapters over this layer (bm25_wand_prepare_terms,
 * bm25_wand_score_pending) live in bm25_wand.c: they call bm25_wand_ctx_build and
 * bm25_topk_offer, which that file defines. CI enforces the rule in its
 * "Stats-layer call direction" step. That step fails if any undefined symbol in
 * bm25_stats.o is defined in one of the scanner's objects or bm25_wand.o, or if
 * bm25_wand.o uses any symbol one of the scanner's objects defines.
 *
 * WHAT DID NOT MOVE. The scan-start corpus prologue (bm25_scan_corpus_stats and
 * its helpers pending_global_stats, pending_stats_by_field, bm25_field_corpus_stats)
 * and every scorer callback stayed in the scanner (bm25_scan_rank.c and
 * bm25_scan_match.c since #228). Only code that BOTH builders call moved, together
 * with whatever that code needs. Some comments below still name those scanner
 * functions (pending_stats_by_field, pending_phrase_stash, seg_posting_cb) as the
 * other half of a shared contract. Search for them by name.
 *
 * The function bodies moved verbatim. The only code edits were linkage (static
 * dropped where a second file now calls a function) and two renames that header
 * scope required: AccEnt -> BM25AccEnt and pending_score_term ->
 * bm25_pending_score_term. Comments changed only where they named an old location,
 * an old name, or a neighbour by position ("above", "in this file").
 */
#include "postgres.h"

#include "bm25.h"
#include "bm25_stats.h"
#include "miscadmin.h"          /* CHECK_FOR_INTERRUPTS, work_mem */
#include "utils/hsearch.h"

/* -------------------------------------------------------------------------
 * Match-set memory bound (#62.5)
 * -------------------------------------------------------------------------
 * The exhaustive scorer holds the WHOLE match set: one BM25AccEnt per matching
 * document in a dynahash, then a flat BM25ExhScored array of the survivors, then
 * the scanctx-lived ranked/scores/ranked_keys arrays. None of it spills and none
 * of it was bounded, so a common term over a large corpus consumed several GB and
 * then died on MaxAllocSize with "invalid memory allocation request size N" -- an
 * error naming neither the query nor any knob that would change the outcome.
 *
 * bm25_native.max_match_memory is the bound, defaulting to 256 MB, with 0 meaning
 * "follow work_mem" for an installation that would rather have one number.
 *
 * work_mem alone was the obvious choice and was tried first; it is wrong here, and
 * measurably so. work_mem sizes ONE spillable operation inside a plan, and its 4 MB
 * default is calibrated for an operation that degrades to disk when it runs out.
 * This sizes the entire output of an index scan that cannot spill at all, so the
 * same 4 MB stops at ~35k documents -- suite 66_scan_interrupts, matching 100,000
 * documents on a common term, went red the moment the strict work_mem bound landed.
 * A full-text index whose default configuration cannot answer a common term over a
 * 100k-row table is not usable, so the budget gets its own knob and its own default
 * (ADR 0047).
 *
 * What no setting can restore is spilling: we hold the whole match set or we do not
 * run, so crossing the line is an ERROR rather than a slower plan. That is still a
 * behaviour change -- a query broad enough to exceed 256 MB of match set used to
 * succeed by consuming several GB -- but the error now names the knob that governs
 * it and the query shape that avoids it entirely.
 *
 * The accounting is in BYTES, not documents, and it is SHARED across every
 * structure a scan materializes. Both properties were learned the hard way from an
 * earlier per-structure document-count version of this bound:
 *
 *   - Counting documents is wrong wherever the structure is not per-document. The
 *     @@@ union collector takes one entry per (term, document) and dedupes only
 *     afterwards, so a document-count limit there is really a posting-count limit:
 *     a 10-term query errored at a tenth of the documents a 1-term query allowed,
 *     and the error text named a document count the query had not reached.
 *   - Counting per structure lets two structures that each hold one entry per
 *     matching document each spend the whole budget. A boolean query fills both the
 *     accumulator and the leaf-presence hash, so two independent caps at N
 *     documents permit ~2N documents' worth of memory.
 *
 * One running byte total, charged by whoever allocates, has neither problem -- and
 * it extends to the structures that are not per-document at all, which is how the
 * phrase stash's position lists (unbounded per document: up to tf positions, and tf
 * reaches 65535 on anything INGESTED since #158, which gave the builder the same
 * per-document token ceiling the pending list already had) come to be covered by the
 * same number.
 *
 * "Ingested since" is the honest qualifier: #158 added no format bump and no read-side
 * tf validation, so a segment sealed by an older binary can still carry tf > 65535 and
 * the scan will read it. That is fine -- phrase_stash_add (bm25_scan_match.c) levies its
 * charge per position ACTUALLY APPENDED, from the real decoded frame length, so it
 * tracks reality whatever tf is. The 65535 figure explains where the budget's scale
 * comes from; it is not a bound the scan relies on.
 *
 * Every charge is a deliberate OVER-estimate; the alternative, charging exactly,
 * would let the true peak exceed the budget, which is the single outcome this
 * exists to prevent. Charges are never released: within one ranking build the total
 * is the peak, and a rescan starts a fresh budget.
 */

/* Peak concurrent bytes for one SCORED document -- the accumulator entry, the
 * pending-dedupe entry shadowing it, the flat drain-array slot (live while the
 * dynahash still is), the scan-lifetime ranked/scores slots, the key slot and its
 * one-byte ranked_key_present flag (allocated with it: per scored row by the
 * exhaustive builder in bm25_scan_rank.c, the one this charge serves; bm25_wand.c
 * allocates one too, for its top-k rows), and the row's key request (BM25KeyReq,
 * live while the keys are resolved, issue #246).
 * BM25_KEY_MAX_SIZE, the presence flag and the key request are charged
 * unconditionally because the index's key width -- and whether it has a key at all --
 * is discovered only AFTER the accumulator is full. */
#define BM25_MATCH_BYTES_PER_DOC                                        \
    (MAXALIGN(sizeof(BM25AccEnt)) + BM25_HASH_ENTRY_OVERHEAD +          \
     MAXALIGN(sizeof(ItemPointerData)) + BM25_HASH_ENTRY_OVERHEAD +     \
     sizeof(BM25ExhScored) +                                            \
     sizeof(BM25Posting) + sizeof(double) + BM25_KEY_MAX_SIZE +         \
     sizeof(bool) + sizeof(BM25KeyReq))

/* KB this scan may spend: the dedicated knob, or work_mem when it is 0. Both are
 * PGC_USERSET, so the value read at scan start is the one that governs. */
static int
bm25_match_budget_kb(void)
{
    return (bm25_max_match_memory_kb > 0) ? bm25_max_match_memory_kb : work_mem;
}

/* int64 throughout -- the budget is an int in KB and MAX_KILOBYTES is INT_MAX on a
 * 64-bit build, so `kb * 1024` overflows a 32-bit long. */
void
bm25_match_budget_init(BM25MatchBudget *b)
{
    b->budget  = (int64) bm25_match_budget_kb() * INT64CONST(1024);
    b->charged = 0;
}

/* One wording for every structure that can hit the bound, so the user sees the same
 * error and the same escapes whichever scan shape produced it. The message names
 * whichever GUC actually supplied the budget -- telling someone to raise
 * max_match_memory when their installation has set it to 0 and is running off
 * work_mem would send them to a knob that changes nothing. It reports the BUDGET and
 * not a document count, because the structures being charged are not all
 * per-document and a count would be false for at least one of them. */
void
bm25_match_charge(BM25MatchBudget *b, int64 bytes)
{
    bool    from_work_mem;

    if (b == NULL || b->budget <= 0)
        return;

    b->charged += bytes;
    if (b->charged <= b->budget)
        return;

    from_work_mem = (bm25_max_match_memory_kb <= 0);
    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bm25: query needs more memory to materialize its match set than %s allows",
                    from_work_mem ? "work_mem" : "bm25_native.max_match_memory"),
             errdetail("This scan holds every matching document in memory and cannot spill; the budget is %dkB.",
                       bm25_match_budget_kb()),
             /* Verified, not assumed: a ranked scan escapes the accumulator only
              * while the consumer stays inside wand_top_k. Reading PAST it -- an
              * ORDER BY with no LIMIT, or a LIMIT above k -- triggers bm25_gettuple's
              * over-pull tail rebuild, which re-runs the EXHAUSTIVE scorer and lands
              * right back here. Hence "LIMIT n", not merely "ranked". */
             errhint("Raise %s, or make the query more selective. A ranked scan "
                     "(ORDER BY col &@@ query LIMIT n) with n no larger than "
                     "bm25_native.wand_top_k returns the top n without materializing "
                     "the whole match set.",
                     from_work_mem ? "work_mem" : "bm25_native.max_match_memory")));
}

/* M1 dynahash upsert: add `contrib` to the running score for `tid` (OR semantics:
 * sum each term's contribution). Identical to the M1 inline upsert, plus M5 records
 * the segment source of a scored posting (Invalid src_hdr for pending) so a keyed
 * scan can resolve docid->key after ranking.
 *
 * budget is the scan's shared materialization total (#62.5); NULL disables the
 * accounting (the WAND pending accumulator, whose size the pending list bounds). */
void
bm25_scores_add(HTAB *acc, ItemPointer tid, double contrib,
                BlockNumber src_hdr, uint32 src_gen, uint32 src_docid,
                BM25MatchBudget *budget)
{
    bool    found;
    BM25AccEnt *e = (BM25AccEnt *) hash_search(acc, tid, HASH_ENTER, &found);

    if (!found)
    {
        e->score = 0.0;
        e->src_hdr = InvalidBlockNumber;
        /* Insertion is the only way this set grows, so this is the only place the
         * charge belongs. Charged AFTER the HASH_ENTER rather than before it: that
         * keeps the common path to a single hash lookup, and the one entry of
         * overshoot costs nothing because the error tears down the whole scratch
         * context on its way out. */
        bm25_match_charge(budget, BM25_MATCH_BYTES_PER_DOC);
    }
    /* Record a segment source when this contribution has one; a later segment
     * posting for the same TID overwrites (same key), a pending contribution
     * (Invalid src_hdr) never clears an already-recorded segment source. */
    if (src_hdr != InvalidBlockNumber)
    {
        e->src_hdr = src_hdr;
        e->src_gen = src_gen;
        e->src_docid = src_docid;
    }
    e->score += contrib;
}

/* Per-field df-counting callback (C3): tallies df_by_field[field_id] once per live
 * posting. BM25 df is documents-with-the-term-in-field; the accumulator emits ONE
 * (term, field_id) posting per doc-field (tf aggregates occurrences), so one live
 * posting == one doc-in-field and a plain increment is correct. Gated on the
 * live-docs bit exactly like seg_posting_cb so a tombstoned doc does not inflate
 * df (idf then matches the scored set). sum_field df_by_field == dict.df (the RLE
 * only partitions the df-bounded decode; it never adds or drops a posting). */
typedef struct
{
    BM25SegmentHeader  *seg;
    /* Forward cursors over *seg's LIVEDOCS chain, the same shape and for the same two
     * reasons as bm25_scan_rank.c's TermScoreCtx.rdr: this callback runs PER POSTING, and
     * the one-shot bm25_seg_doc_is_live it replaces both re-walked the chain from its
     * root every time AND (SEGREAD-11) took a fresh RelationGetNumberOfBlocks for its
     * extent bound, which lseeks on every call outside recovery. The reader hoists the
     * extent to init and resumes each lookup from the page the previous posting landed
     * on -- postings arrive in ascending docid order (D-ACCUM), so that resume hits.
     * Lives here BY VALUE and the ctx is declared inside the per-(term, segment) block
     * of bm25_term_idf's loop, so the reader cannot outlive the segment it was opened
     * on; it also supplies the Relation, which is why this struct no longer carries
     * one of its own. */
    BM25SegReader       rdr;
    /* BM25_MAX_FIELDS wide at the one caller (bm25_term_idf's stack array), not
     * field_count wide -- the increment in the callback is in bounds for any id
     * bm25_field_rle_decode admits. Only the first field_count entries are ever
     * consumed. */
    uint64             *df_by_field;    /* [BM25_MAX_FIELDS], caller-accumulated */
} FieldDfCtx;

static void
field_df_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    FieldDfCtx *c = (FieldDfCtx *) state;

    (void) tf;
    /* Same on-disk provenance as seg_posting_cb's field_id (see there), and the
     * same correction to make: the old comment's "OOB df bump" overstated the
     * danger. df_by_field is BM25_MAX_FIELDS wide and bm25_field_rle_decode
     * already rejects an id >= BM25_MAX_FIELDS, so the increment below cannot run
     * off the end -- there is no OOB write here.
     *
     * What a corrupt id in the field_count..BM25_MAX_FIELDS gap does do is charge
     * a document frequency to a field this segment does not have. That df feeds
     * idf for every field of the query, so the whole ranking shifts -- silently,
     * because the Assert this replaces is not present in a production build. This
     * pass runs before the scoring pass, so on a corrupt segment this is the guard
     * that fires first. */
    if (field_id >= c->seg->field_count)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field id %u in a df-counted posting exceeds the "
                        "segment's field count %u",
                        field_id, c->seg->field_count)));
    if (!bm25_seg_reader_doc_is_live(&c->rdr, local_docid))
        return;
    c->df_by_field[field_id]++;
}

/* Count the pending df for one term: the number of live (TID-valid) pending docs
 * that contain it. Summed with the segment df for IDF.
 *
 * v7 (#57) MULTI-PART DOCUMENTS. This counts DOCUMENTS, not records, so it carries the
 * same per-document state bm25_pending_drain does: the tid of the record that opened
 * the document, plus whether the term has already been credited to it.
 *
 * WHY A CONT RECORD CANNOT SIMPLY BE SKIPPED. A document's distinct (field, term)
 * entries are PARTITIONED across its parts -- bm25_pending_append_multi packs entries
 * [jstart, j) into each record and sets hdr.ndocterms = j - jstart -- so each entry
 * lives in exactly ONE part, very often not part 0. Skipping every continuation would
 * therefore stop counting any term whose entry landed past the first part: df would be
 * DEFLATED for ordinary large documents (sql/77's 4000-term row spans ~21 parts), which
 * inflates idf corpus-wide. Built and measured: sql/92's legitimate spanning document
 * lost its df, and sql/77's spanning phrase stopped matching at all, because a df of
 * zero makes bm25_term_idf drop the term outright. That is a worse error than the one
 * being fixed here and it is not confined to a rare state, so the drain's tid-matching
 * guard is the fix, not the cheaper skip.
 *
 * WHAT THE GUARD IS FOR (ADR 0069). A cancelled VACUUM leaves a stranded continuation:
 * part 0 invalidated, part 1 surviving with a valid tid and BM25_PENDING_DOC_CONT set.
 * Counting it credits a term to a document that is being removed. Every pending walker
 * whose output is a TID -- the @@@ collector, the AND mask, the phrase stash -- is
 * corrected downstream for free, because the fragment's own TID belongs to a heap tuple
 * VACUUM was removing and the heap recheck drops it. df has no TID to be filtered on:
 * it feeds idf, a corpus statistic. The error is also one-directional, because
 * pending_global_stats skips CONT records outright, so N does not move with df.
 * (bm25_dict_expand_wildcard was the one other walker with no TID in its output that
 * still processed a stranded fragment -- pending_global_stats and
 * pending_stats_by_field emit no TID either, but both are CONT-correct, so a fragment
 * never reaches what they publish. It emits a term SET, and a fragment's terms
 * inflated that set until the next seal. It was left out of scope here because after
 * this fix such a term carries df 0 and is dropped from scoring, leaving only the
 * expansion cap it consumes -- issue #197 then closed that last residue with this same
 * guard, so the class is now uniform. Keep it that way: a new pending walker either
 * emits a TID that the heap recheck filters, or carries this guard.)
 *
 * The per-document `counted` dedup is unreachable TODAY -- the sole caller
 * (bm25_term_idf) uses this walker only when field_count == 1, where dedup is by
 * (field, term) and so a term has exactly one entry in the whole document. It is kept
 * because the "a document counts once toward df" contract belongs to this walker rather
 * than to its caller's field count: with more than one field a term in two columns is
 * two entries, which can straddle a part boundary. */
static uint64
pending_df(Relation index, BlockNumber pending_head, uint32 epoch_bound,
           const char *term, int termlen)
{
    BlockNumber blk = pending_head;
    BM25PendingWalk walk;
    uint64      df = 0;
    /* The tid of the document being walked, so a continuation belonging to SOMEONE
     * ELSE (or to nobody) is dropped rather than counted as its own document. */
    ItemPointerData acc_tid;
    bool        acc_active = false;
    bool        counted = false;    /* term already credited to the current document */

    ItemPointerSetInvalid(&acc_tid);

    bm25_pending_walk_init(&walk, index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&walk, blk); /* SHARE-locked */

        pg = BufferGetPage(buf);
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;
            char   *p = (char *) dh + bm25_pending_doc_entries_off(dh);
            uint32  k;

            if (!ItemPointerIsValid(&dh->tid))
                continue;

            if ((dh->flags & BM25_PENDING_DOC_CONT) == 0)
            {
                acc_active = true;
                acc_tid = dh->tid;
                counted = false;
            }
            else if (!acc_active || !ItemPointerEquals(&acc_tid, &dh->tid))
                continue;       /* stranded continuation (ADR 0069) -- see the header */

            if (counted)
                continue;       /* already counted this DOCUMENT, on an earlier part */

            for (k = 0; k < dh->ndocterms; k++)
            {
                BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                if (te->termlen == termlen &&
                    memcmp(p + sizeof(BM25PendingTermEntry), term, termlen) == 0)
                {
                    df++;
                    counted = true;
                    break;      /* per-doc dedup: a doc counts once toward df */
                }
                p += MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
            }
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
        blk = next;
    }
    return df;
}

/* Per-field pending df (BM25F, C3): for one term, add to df_by_field[f] the number
 * of live pending docs that contain the term in field f. A doc counts at most once
 * per field (per-doc-per-field dedup), mirroring pending_df's per-doc dedup and
 * seg_posting_cb's one-posting-per-(doc,field) model, so sum_field pending df matches
 * the segment df partition. df_by_field is caller-accumulated (>= field_count).
 *
 * Carries pending_df's per-document state for the same two reasons, and see that
 * function's header for both in full: a stranded BM25_PENDING_DOC_CONT (ADR 0069) must
 * not be counted as a document of its own, and skipping continuations wholesale is NOT
 * the equivalent simplification -- a document's entries are partitioned across its
 * parts, so that would stop counting any entry living past part 0. seen[] moves with the
 * DOCUMENT rather than with the record for the same reason pending_df's `counted` does;
 * a (field, term) pair is unique within a document, so today the two are the same
 * number, and the per-document form is the one this walker's contract claims. */
static void
pending_df_by_field(Relation index, BlockNumber pending_head, uint32 epoch_bound,
                    const char *term, int termlen,
                    uint32 field_count, uint64 *df_by_field)
{
    BlockNumber blk = pending_head;
    BM25PendingWalk walk;
    /* Zeroed at declaration as well as reset per document: the reset now lives in the
     * head-record branch, so initializing it here is what keeps a walk that opens on a
     * stranded continuation from depending on the guard below to avoid reading
     * uninitialized stack (SCAN-08's lesson, applied before it can bite). */
    bool        seen[BM25_MAX_FIELDS] = {false};  /* per-DOCUMENT: field already counted */
    /* The tid of the document being walked; see pending_df. */
    ItemPointerData acc_tid;
    bool        acc_active = false;

    ItemPointerSetInvalid(&acc_tid);

    bm25_pending_walk_init(&walk, index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&walk, blk); /* SHARE-locked */

        pg = BufferGetPage(buf);
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;
            char   *p = (char *) dh + bm25_pending_doc_entries_off(dh);
            uint32  k;
            uint32  f;

            if (!ItemPointerIsValid(&dh->tid))
                continue;

            if ((dh->flags & BM25_PENDING_DOC_CONT) == 0)
            {
                acc_active = true;
                acc_tid = dh->tid;
                for (f = 0; f < field_count; f++)
                    seen[f] = false;
            }
            else if (!acc_active || !ItemPointerEquals(&acc_tid, &dh->tid))
                continue;       /* stranded continuation (ADR 0069) -- see pending_df */

            for (k = 0; k < dh->ndocterms; k++)
            {
                BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                if (te->field_id < field_count && !seen[te->field_id] &&
                    te->termlen == termlen &&
                    memcmp(p + sizeof(BM25PendingTermEntry), term, termlen) == 0)
                {
                    df_by_field[te->field_id]++;
                    seen[te->field_id] = true;
                }
                p += MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
            }
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
        blk = next;
    }
}

/*
 * bm25_term_idf -- the shared per-term df->idf computation. Sums ONE term's df
 * across every live segment plus the pending list, then turns that into the
 * per-field idf array the exhaustive scorer, the WAND cursor bounds and the WAND
 * debug probes all consume. Returns false when the term contributes nothing (no
 * in-scope field has a non-zero df); every caller treats that as "skip this
 * term" (it is the exhaustive scorer's old `any_df`).
 *
 * Sharing this is what makes WAND bit-identical to the exhaustive path (D8): the
 * SAME idf feeds the block bound and the score.
 *
 * bm25_seg_dict_lookup gives a term's TOTAL df within a segment (across ALL
 * fields); the per-field split comes from the field-id RLE decode. Two cases keep
 * the single-field path M3-identical:
 *
 *  - field_count == 1: everything is field 0. Sum the raw dict df across segments
 *    + pending (NOT live-gated -- that matches the M3 idf denominator exactly,
 *    even under tombstones where dict df is not decremented) and
 *    idf_f[0] = bm25_idf(live_ndocs, df). No RLE decode needed. Because this df
 *    is raw while live_ndocs is live, df CAN exceed the denominator (delete most
 *    of a term's docs with no intervening merge). bm25_idf clamps df to ndocs for
 *    exactly this case -- required, not cosmetic: an idf below zero inverts the
 *    WAND bound's monotonicity. See bm25_idf and bm25_block_ub.
 *
 *  - field_count > 1: a df-count decode pass (field_df_cb) partitions each
 *    segment's df by field_id; pending is partitioned the same way. Then
 *    idf_f[f] = bm25_idf(N_field[f], df_field[f]). Live-gated (df reflects the
 *    scored set); sum_field df_field == dict.df on a fresh index (R6). A segment
 *    whose bitmap is all set is gated by one check of it instead of one read
 *    per posting (bm25_seg_reader_init_checked, ADR 0100), which yields the same df.
 *
 * qfield is the C4 field scope (BM25_FIELD_ALL == unscoped, every field scored).
 * Outside the scope idf is forced to 0 so both seg_posting_cb and
 * bm25_pending_score_term skip the posting -- they already gate on idf_f == 0. That
 * restricts scoring to qfield WITHOUT shortening the RLE decode: the full term
 * run bounded by dict.df is still walked, only non-scoped fields' contributions
 * are dropped (R6).
 *
 * boost is the M6 query-time multiplier (1.0 on every non-boolean path, a
 * no-op there). bm25_termscore is linear in idf, so scaling idf by the leaf's
 * boost scales the term's contribution by exactly boost; presence marking never
 * reads idf and is unaffected.
 *
 * idf_f is zeroed across all BM25_MAX_FIELDS by the CALLEE, so callers may hand over
 * an uninitialized array and downstream consumers that index past field_count
 * (none today) can never read stack garbage.
 */
bool                            /* declared in bm25_stats.h (bm25_scan_rank.c, bm25_wand.c, bm25_debug.c) */
bm25_term_idf(Relation index, const BM25ScanSnapshot *snap,
              const char *term, int termlen,
              uint64 live_ndocs, const uint64 *fld_ndocs,
              int32 qfield, double boost,
              double *idf_f /* [BM25_MAX_FIELDS], zeroed here */,
              BM25TermSegLoc *out_locs /* [snap->nsegs] or NULL */)
{
    uint64  df_by_field[BM25_MAX_FIELDS] = {0};
    bool    any_df = false;
    uint32  si,
            f;

    for (f = 0; f < BM25_MAX_FIELDS; f++)
        idf_f[f] = 0.0;

    /* Both branches below record each segment's lookup in out_locs (issue #246): the
     * scorers used to repeat it, walking every segment's dictionary twice per term,
     * which ADR 0100 measured at up to 26% of a rare-term query over many segments.
     * The POS entry is copied too -- it is in the same dictionary record -- so the
     * exhaustive phrase path can take its roots from here as well. */
    if (snap->field_count == 1)
    {
        /* uint64 like ndocs (issue #313 SCORE-06): per-segment loc.df is uint32, but
         * the sum across segments and the pending list is not bounded by one
         * segment, and a wrapped sum would hand this term a huge idf. */
        uint64 global_df = 0;

        for (si = 0; si < snap->nsegs; si++)
        {
            BM25SegmentHeader   h;
            BM25TermSegLoc      loc = {0};

            CHECK_FOR_INTERRUPTS();

            bm25_seg_header_read(index, snap->segs[si].header_blkno,
                                 snap->segs[si].gen, &h);
            loc.found = bm25_seg_dict_lookup(index, &h, term, termlen,
                                             &loc.post_root, &loc.post_off, &loc.df,
                                             &loc.pos_post_root, &loc.pos_post_off);
            if (out_locs != NULL)
                out_locs[si] = loc;
            if (loc.found)
                global_df += loc.df;
        }
        global_df += pending_df(index, snap->pending_head, snap->next_gen, term, termlen);
        df_by_field[0] = global_df;
    }
    else
    {
        for (si = 0; si < snap->nsegs; si++)
        {
            BM25SegmentHeader   h;
            BM25TermSegLoc      loc = {0};

            CHECK_FOR_INTERRUPTS();

            bm25_seg_header_read(index, snap->segs[si].header_blkno,
                                 snap->segs[si].gen, &h);
            loc.found = bm25_seg_dict_lookup(index, &h, term, termlen,
                                             &loc.post_root, &loc.post_off, &loc.df,
                                             &loc.pos_post_root, &loc.pos_post_off);
            if (out_locs != NULL)
                out_locs[si] = loc;
            if (loc.found)
            {
                FieldDfCtx dc;

                dc.seg         = &h;
                /* Opened on THIS iteration's header, which is re-read per segment
                 * above; dc is declared in this block, so reader and segment are
                 * created and destroyed together and no reader crosses a segment.
                 *
                 * init_checked (issue #229, ADR 0100): field_df_cb gates each posting
                 * of the term's whole run on liveness, and that gate was one
                 * LIVEDOCS buffer access per posting -- the largest single cost of a
                 * WAND build on a multi-field index (136k of 376k accesses for a
                 * frequent term). A segment whose bitmap is all set answers it from
                 * one check at init; df is the same count. */
                (void) bm25_seg_reader_init_checked(&dc.rdr, index, &h, loc.df);
                dc.df_by_field = df_by_field;
                bm25_seg_scan_postings(index, loc.post_root, loc.post_off, loc.df, h.gen,
                                       field_df_cb, &dc,
                                       InvalidBlockNumber, 0, NULL, NULL, NULL, 0);
            }
        }
        pending_df_by_field(index, snap->pending_head, snap->next_gen, term, termlen,
                            snap->field_count, df_by_field);
    }

    for (f = 0; f < snap->field_count; f++)
    {
        if (qfield != BM25_FIELD_ALL && (int32) f != qfield)
        {
            idf_f[f] = 0.0;         /* C4: field out of scope */
            continue;
        }
        if (df_by_field[f] == 0)
        {
            idf_f[f] = 0.0;         /* term absent from field f */
            continue;
        }
        any_df = true;
        idf_f[f] = boost *
            ((snap->field_count == 1)
             ? bm25_idf(live_ndocs, df_by_field[f])
             : bm25_idf(fld_ndocs[f], df_by_field[f]));
    }

    return any_df;
}

/* Score one buffered pending document (v7 #57): for every field the query term matched,
 * add its boosted contribution against the DOCUMENT's per-field doclen. A no-op when
 * nothing is buffered or the term matched nothing. */
static void
pending_score_flush(HTAB *scores, HTAB *pending_tids,
                    const ItemPointerData *tid, bool active,
                    const uint32 *doclen_by_field, const uint32 *match_tf,
                    const double *idf_f, const double *avgdl_f,
                    const BM25FieldConfig *fcfg, uint32 field_count,
                    BM25MatchBudget *budget)
{
    bool    scored = false;
    uint32  f;

    if (!active)
        return;

    for (f = 0; f < field_count; f++)
    {
        double contrib;

        if (match_tf[f] == 0)
            continue;
        contrib = fcfg[f].boost *
            bm25_termscore(idf_f[f], match_tf[f], doclen_by_field[f],
                           avgdl_f[f], fcfg[f].k1, fcfg[f].b);
        /* Pending-only: no keymap entry yet (Invalid src_hdr -> NULL key until the next
         * seal writes this doc into a KEYMAP). */
        bm25_scores_add(scores, (ItemPointer) tid, contrib,
                        InvalidBlockNumber, 0, 0, budget);
        scored = true;
    }

    /* Register the TID once so segment postings dedupe (pending wins), whether the term
     * hit one field or several. */
    if (scored && pending_tids != NULL)
        (void) hash_search(pending_tids, (void *) tid, HASH_ENTER, NULL);
}

/* Accumulate the pending-list contribution for one term into the dynahash. For
 * each live pending doc containing the term, score with the pending doc's own
 * doclen (BM25PendingDocHeader.doclen) and the term's stored tf. Mirrors
 * seg_posting_cb's math; the only difference is the posting source.
 *
 * Task 12: pending_tids is an optional set of every TID this function scores.
 * When non-NULL, each scored TID is registered into it so the multi-source
 * segment scan (seg_posting_cb, and likewise seg_and_cb / seg_phrase_pos_cb /
 * bm25_wand_segment) can dedupe against pending -- a TID pending already scored
 * must not be scored a second time from a segment posting for that same TID
 * (dedupe rule: pending wins).
 * Pass NULL when no segment dedupe is needed (the M1 single-source path). */
void
bm25_pending_score_term(Relation index, BlockNumber pending_head,
                        uint32 epoch_bound,
                        const char *term, int termlen,
                        uint32 field_count, const double *idf_f, const double *avgdl_f,
                        const BM25FieldConfig *fcfg, HTAB *scores,
                        HTAB *pending_tids, BM25MatchBudget *budget)
{
    BlockNumber     blk = pending_head;
    BM25PendingWalk walk;
    /* Both zeroed at declaration for the same reason as pending_stats_by_field's
     * tok_by_field (SCAN-08): the per-document reset is gated on seeing a head record,
     * and a stranded continuation (ADR 0069) can be the first record this walk sees. */
    uint32          doclen_by_field[BM25_MAX_FIELDS] = {0};   /* per-DOCUMENT, spans its parts */
    uint32          match_tf[BM25_MAX_FIELDS] = {0};          /* tf of the term's hit, per field */
    bool            acc_active = false;
    /* See pending_stats_by_field: a v8 record stores its per-field doclen, a pre-v8 one
     * is reconstructed as sum-of-tf. */
    bool            len_stored = false;
    ItemPointerData acc_tid;
    PGAlignedBlock  copy;

    ItemPointerSetInvalid(&acc_tid);

    bm25_pending_walk_init(&walk, index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&walk, blk); /* SHARE-locked */

        /* Fix (2026-08, crash/replica-safety pass): copy-then-unlock, same shape
         * as pending_phrase_stash (bm25_scan_match.c). pending_score_flush (called at each
         * document boundary below) calls bm25_scores_add and, when pending_tids
         * is non-NULL, hash_search(HASH_ENTER) directly -- both dynahash
         * operations that can rehash, unbounded by this page's size, and both
         * used to run while the page was still locked. */
        memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg = (Page) copy.data;
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;
            char   *p;
            uint32  k;
            uint32  f;

            if (!ItemPointerIsValid(&dh->tid))
                continue;

            /* v7 (#57): a document too large for one page is several consecutive
             * same-tid records, so BOTH halves of the old two-pass body have to span
             * them. Per-field doclen belongs to the DOCUMENT -- deriving it per RECORD
             * scored a spanning doc as a far shorter one, which inflates the
             * length-normalization factor. That was measured: the 4000-term row in
             * sql/77 scored 0.289504 against its sealed self's 0.182322, purely
             * because dl read 380 instead of 4000. Since v8 the doclen is read once
             * off the document's first part rather than summed across its records,
             * which makes the span question moot for that half; the match_tf half
             * still has to accumulate across parts.
             *
             * So accumulate, and score when the document ends. Buffering the matches is
             * bounded by design: de-dup is per (field, term), so one term has at most one
             * entry per field in a document, hence at most BM25_MAX_FIELDS matches. */
            if ((dh->flags & BM25_PENDING_DOC_CONT) == 0)
            {
                const uint32 *fl;

                pending_score_flush(scores, pending_tids, &acc_tid, acc_active,
                                    doclen_by_field, match_tf, idf_f, avgdl_f,
                                    fcfg, field_count, budget);
                for (f = 0; f < field_count; f++)
                {
                    doclen_by_field[f] = 0;
                    match_tf[f] = 0;
                }
                acc_active = true;
                acc_tid = dh->tid;

                /* v8: the per-field doclen the append stored, which is what the next
                 * seal will write into this document's NORMS row. THIS is the number
                 * that keeps a pre-seal score equal to a post-seal score; see the
                 * fallback's comment below for why the old sum-of-tf cannot be it. */
                fl = bm25_pending_doc_fieldlens(dh);
                len_stored = (fl != NULL && dh->nfieldlens == field_count);
                if (len_stored)
                    for (f = 0; f < field_count; f++)
                        doclen_by_field[f] = fl[f];
            }
            else if (!acc_active || !ItemPointerEquals(&acc_tid, &dh->tid))
                /* Orphaned continuation -- see pending_stats_by_field's identical arm
                 * and bm25_pending_drain's. Folding it into the preceding document
                 * would have credited that document with another document's term
                 * frequency, i.e. a hit for a term it does not contain. */
                continue;

            p = (char *) dh + bm25_pending_doc_entries_off(dh);
            for (k = 0; k < dh->ndocterms; k++)
            {
                BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                uint32 fid = te->field_id;

                if (fid < field_count)
                {
                    /* Pre-v8 fallback: sum tf over a field's entries. Exact for a
                     * record written before doclen became the source-RUN count,
                     * because the analyzer that wrote it emitted one token per run, so
                     * its token count and its run count are the same number. It is NOT
                     * exact in general -- that is why v8 stores the value -- so this
                     * arm must stay gated. */
                    if (!len_stored)
                        doclen_by_field[fid] += te->tf;
                    if (idf_f[fid] != 0.0 && te->termlen == termlen &&
                        memcmp(p + sizeof(BM25PendingTermEntry), term, termlen) == 0)
                        match_tf[fid] = te->tf;
                }
                p += MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
            }
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;   /* read off the unlocked copy */
        blk = next;
    }
    /* Last document on the chain. */
    pending_score_flush(scores, pending_tids, &acc_tid, acc_active,
                        doclen_by_field, match_tf, idf_f, avgdl_f,
                        fcfg, field_count, budget);
}

/* Hash entry for bm25_ranked_keys_fill_from_pending: the TID of a ranked row whose
 * key is still missing, mapped to its slot in so->ranked_keys. `tid` must be first
 * -- dynahash compares the leading ctl.keysize bytes of the entry. */
typedef struct BM25PendingKeyWant
{
    ItemPointerData tid;        /* hash key */
    uint32          idx;        /* index into so->ranked / ranked_keys */
} BM25PendingKeyWant;

/*
 * Backfill ranked-key slots that no SEGMENT could resolve, reading the key from
 * the PENDING record instead. Call AFTER either ranking builder has filled
 * so->ranked/ranked_keys/ranked_key_present to their final nranked.
 *
 * WHY THIS EXISTS. The KEYMAP lookup resolves a ranked row's user key from the winning
 * posting's segment (src_hdr/gen/docid). A row that only the pending scorer scored
 * has no segment source at all -- pending_score_flush passes InvalidBlockNumber --
 * so its slot stayed absent and bm25_score_key(id) returned NULL for it while
 * bm25_score(ctid) and bm25_snippet(), which key off the TID, worked fine. Since
 * an application's rows are pending exactly between its INSERT and the next seal,
 * the rows missing a score were reliably the newest ones (#100, #204).
 *
 * The key is already on the pending record -- bm25_pending_append writes it into
 * BM25PendingDocHeader.key[] precisely so the next seal can put it in a KEYMAP --
 * so this is a read, not a recomputation, and it cannot disagree with what the
 * seal will later write.
 *
 * WHY A SEPARATE PASS rather than threading the key through the scorer. The key
 * would otherwise have to travel through bm25_scores_add's accumulator entry,
 * BM25ExhScored, and the WAND top-k heap element -- widening three structs and
 * the heap's dedupe on the hottest path in the scan -- to serve a projection that
 * most queries never ask for. This walks the pending chain once per ranking, only
 * when a slot is actually missing, and touches no scoring code. The chain is
 * bounded by seal_threshold, and the walk it does is the same shape the scorer
 * just did.
 *
 * Costs nothing on the common path: a fully-sealed index leaves no slot absent,
 * so this returns after the counting loop without reading a single page.
 */
void
bm25_ranked_keys_fill_from_pending(Relation index, BM25ScanOpaque so,
                                   BlockNumber pending_head, uint32 epoch_bound)
{
    BlockNumber     blk;
    BM25PendingWalk walk;
    HASHCTL         ctl;
    HTAB           *want;
    uint32          i;
    uint32          nwant = 0;
    uint32          nfilled = 0;
    PGAlignedBlock  copy;

    /* Keyless index (nothing projects a key) or an empty ranking. */
    if (so->ranked_keys == NULL || so->nranked == 0)
        return;

    for (i = 0; i < so->nranked; i++)
        if (!so->ranked_key_present[i])
            nwant++;
    if (nwant == 0)
        return;                 /* every ranked row resolved from a segment */

    if (pending_head == InvalidBlockNumber)
        return;                 /* no pending list; the absent slots have no other source */

    /* TID -> ranked index, over the UNRESOLVED rows only. Sized by what is missing
     * rather than by the pending list, so a large pending list under a small
     * ranking (an ORDER BY ... LIMIT) does not pay for entries it will not use. */
    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(ItemPointerData);
    ctl.entrysize = sizeof(BM25PendingKeyWant);
    ctl.hcxt      = CurrentMemoryContext;
    want = hash_create("bm25 pending key backfill", nwant, &ctl,
                       HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    for (i = 0; i < so->nranked; i++)
    {
        BM25PendingKeyWant *w;
        bool                found;

        if (so->ranked_key_present[i])
            continue;
        w = hash_search(want, &so->ranked[i].tid, HASH_ENTER, &found);
        /* First writer wins, matching every other dedupe in the scan. TIDs are
         * unique across a ranking by construction, so this cannot currently
         * diverge; keeping it explicit avoids a latent asymmetry. */
        if (!found)
            w->idx = i;
    }

    /* Same copy-then-unlock walk as bm25_pending_score_term: take the page's bytes under
     * a share lock, release, then iterate the copy. The hash_search below can
     * rehash, and doing that under a buffer lock is what the crash/replica-safety
     * pass removed from the scorer. */
    blk = pending_head;
    bm25_pending_walk_init(&walk, index, epoch_bound);
    while (blk != InvalidBlockNumber && nfilled < nwant)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&walk, blk); /* SHARE-locked */

        memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg   = (Page) copy.data;
        next = BM25PageGetOpaque(pg)->nextblk;   /* read off the unlocked copy */

        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;
            BM25PendingKeyWant   *w;

            if (!ItemPointerIsValid(&dh->tid))
                continue;
            /* A record's key metadata must match what THIS scan is projecting.
             * key_type/key_size come off a page, so they are untrusted input: a
             * mismatch means a record written under a different key_field (an
             * ALTER between append and scan) or a corrupt page, and the right
             * answer for both is to leave the slot absent -- NULL -- rather than
             * copy foreign bytes into a key the caller will compare against. */
            if (dh->key_type != so->ranked_key_type ||
                dh->key_size != so->ranked_key_size)
                continue;

            w = hash_search(want, &dh->tid, HASH_FIND, NULL);
            if (w == NULL)
                continue;                       /* not a row this ranking is missing */
            if (so->ranked_key_present[w->idx])
                continue;                       /* an earlier part already supplied it */

            memcpy(so->ranked_keys + (Size) w->idx * so->ranked_key_size,
                   dh->key, so->ranked_key_size);
            so->ranked_key_present[w->idx] = true;
            nfilled++;
        }
        bm25_pending_iter_end(&it);

        blk = next;
    }

    hash_destroy(want);
}

/*
 * bm25_ranked_key_config -- the index-wide key_field config, (key_type, key_size),
 * that both ranking builders project ranked-row keys with. BM25_KEY_NONE / 0 means a
 * keyless index: ctid-only, nothing allocated. One definition so the exhaustive and
 * WAND builders cannot drift apart on it (they must return the same (key, score) rows).
 *
 * THE STAMP FIRST (issue #303.I). An index built since #292 carries its key identity
 * on the field-config page (BM25KeyStamp, ADR 0112), written once at build and
 * checked against every INSERT, so it answers in one read of a page the scan has
 * already read. Before it existed the config could only be discovered: from the
 * first live segment carrying a KEYMAP, and when there is none -- an index created
 * on an empty table and then INSERTed into keeps its whole corpus pending (#204) --
 * from the pending records themselves. A keyless index has neither, so every ranked
 * scan walked and decoded the whole pending chain a second time just to learn that.
 *
 * THE UNSTAMPED FALLBACK keeps that full discovery, deliberately including the walk
 * to the end of a keyless pending run. Stopping at the first keyless record would be
 * unsound there: an unstamped index with no segment checks nothing on INSERT (ADR
 * 0112's residual), so a key_field set after keyless rows were queued leaves keyed
 * records behind keyless ones, and the drain's mixed-chain refusal fires only at the
 * next seal.
 */
void
bm25_ranked_key_config(Relation index, const BM25ScanSnapshot *snap,
                       uint8 *km_type, uint16 *km_size)
{
    BM25KeyStamp    ks;
    uint32          s;

    *km_type = BM25_KEY_NONE;
    *km_size = 0;

    if (snap->field_config_blkno != InvalidBlockNumber &&
        bm25_fieldcfg_read_keystamp(index, snap->field_config_blkno, &ks))
    {
        *km_type = ks.key_type;
        *km_size = ks.key_size;
        return;
    }

    for (s = 0; s < snap->nsegs; s++)
    {
        BM25SegmentHeader hh;

        CHECK_FOR_INTERRUPTS();

        bm25_seg_header_read(index, snap->segs[s].header_blkno, snap->segs[s].gen, &hh);
        if (bm25_seg_keymeta(index, &hh, km_type, km_size))
            return;
    }
    (void) bm25_pending_keymeta(index, snap->pending_head, snap->next_gen,
                                km_type, km_size);
}
