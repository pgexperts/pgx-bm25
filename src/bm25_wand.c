/* bm25_wand.c -- block-max WAND engine: cursor, top-k heap, BMW driver.
 *
 * M2b Task 4: the safe per-block score upper bound (bm25_block_ub) and the
 * scoring context it runs on (BM25WandCtx, bm25_wand_ctx_build). This is the
 * ONE piece every later WAND component (per-term cursor, top-k heap, the BMW
 * skip driver itself) depends on for correctness -- if the bound can ever
 * fall below a real posting's contribution, the driver built on top of it
 * silently drops a doc that belonged in the top-N, with no query-time signal
 * that anything went wrong. That is why it lands first, alone, with its own
 * proof (below) and its own regression suite (sql/43_wand_parity.sql) that
 * checks it against a real per-posting contribution rather than against
 * itself.
 *
 * M2b Task 5: the bounded top-k min-heap (BM25TopK) below it. A block-max
 * driver only ever needs to know the WORST score currently worth keeping
 * (bm25_topk_threshold, to decide whether a block's bm25_block_ub even makes
 * it worth decoding) and how to fold in one more candidate
 * (bm25_topk_offer) -- both O(log k) against an array that grows on demand but
 * never past k (see BM25TopK). The heap itself does not need to match
 * anything bit-exactly (its internal array order is never observed); only
 * its FINAL drained order (bm25_topk_drain_sorted) does, since that is what
 * Task 8's WAND driver hands back as the scan's result set, and it must be
 * indistinguishable from what the exhaustive scan (bm25_scan_rank.c:scored_desc)
 * would have produced over the same corpus.
 *
 * M2b Task 6: BM25WandCursor, a per-term per-segment pull cursor over a
 * term's block-encoded postings (open/docid/next/block_last/block_max/
 * global_ub/score_doc). score_doc's bit-exact match to the exhaustive
 * scorer's per-doc, per-term contribution (bm25_scan_rank.c:seg_posting_cb) is
 * THE thing every later WAND driver's correctness rests on -- a driver that
 * skips postings via block bounds is only as trustworthy as this cursor's
 * scoring is faithful to the scorer it replaces. Task 6 itself only walks
 * the cursor LINEARLY (next()).
 *
 * M2b Task 7: bm25_wand_cursor_next_geq, the block-SKIP jump built on top of
 * that same cursor. A skipped block costs one header-only read (Task 3's
 * bm25_seg_block_header_read: BM25BlockHeader + the impact table, never the
 * docid/tf/field-RLE streams) instead of a full posting decode -- this is the
 * entire performance case for block-max WAND. Correctness rests on the same
 * df-bound every other per-term block walk in this AM uses (a term's blocks
 * share the segment's one POST chain with no delimiter, so nothing else
 * stops a skip at the term's own last block) and on next_geq never landing
 * anywhere the exhaustive linear walk (next()) would not also land -- proven
 * by sql/43_wand_parity.sql's skip-vs-linear equivalence check, not merely
 * asserted.
 *
 * M2b Task 8/9 wired all of the above into the scan, and WAND is now the
 * DEFAULT ranking path: bm25_wand_build_ranking (bottom of this file) is
 * called from bm25_scan_build_ranking_once (bm25_scan_rank.c), whose gate takes the
 * WAND driver whenever !force_exhaustive && so->scoring && !so->qphrase &&
 * bm25_wand_top_k > 0 && !bm25_qtree_is_multileaf(so->qtree) -- and
 * bm25_native.wand_top_k defaults to 100 (bm25_handler.c), so nothing has to be
 * turned on for this file to run. Everything the gate excludes (phrase queries,
 * multi-leaf BOOLEAN/BOOST jsonb trees, wand_top_k = 0, and the over-pull tail
 * fallback's forced rebuild) falls through to the exhaustive scorer instead.
 * The debug SRFs -- bm25_debug_block_ub / bm25_debug_term_contrib /
 * bm25_debug_topk / bm25_debug_cursor_scan / bm25_debug_cursor_skip /
 * bm25_debug_wand_rank / bm25_wand_stats -- reach these same primitives
 * independently and live in bm25_debug.c.
 */

#include "postgres.h"

#include "bm25_wand.h"
#include "bm25_stats.h"     /* bm25_term_idf, bm25_pending_score_term, BM25AccEnt,
                             * bm25_ranked_keys_fill_from_pending */
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include <float.h>      /* DBL_MAX: bm25_topk_threshold's "accept anything" sentinel;
                         * DBL_EPSILON: wand_widen_ub's prune slack */

/*
 * wand_field_ub -- one field's term of bm25_block_ub: boost_f * bm25_termscore at
 * the block's (max_tf, min_doclen) for that field, or 0.0 for a field the query
 * cannot score (unknown id, out of the query's field scope, or idf == 0 -- the
 * same gate seg_posting_cb applies). Shared by bm25_block_ub and by
 * wand_cursor_sweep_global_ub's per-field maxima (issue #289), so the two can
 * never disagree on what a field's bound is.
 */
static inline double
wand_field_ub(const BM25FieldImpact *fi, const BM25WandCtx *ctx)
{
    uint8  f = fi->field_id;
    double contrib;

    if (f >= ctx->field_count || !ctx->field_in_query[f] || ctx->idf_f[f] == 0.0)
        return 0.0;
    /* Round the per-field boosted score to a double FIRST (returned), and let the
     * caller fold it in -- the SAME two-rounding sequence
     * bm25_wand_cursor_score_doc uses. Still a
     * SAFETY requirement even though the prune comparisons are now widened,
     * because it is a PREMISE of that widening's derivation, not merely a
     * tightness trick: wand_widen_ub assumes every per-field bound TERM is
     * >= the scorer's corresponding term, and bounds only the error of the
     * ADDITIONS on top of that. Since this term uses (max_tf, min_doclen) it
     * is >= the real contribution, and bit-identical at the coincidence
     * boundary (same two-rounding product). Folding in one fused expression
     * (ub += boost*termscore) could contract to an FMA whose single rounding
     * puts the bound TERM itself below the scorer's -- attacking the premise
     * rather than merely widening the gap the slack budgets for. (The slack
     * would in fact still absorb it: a per-term deficit is bounded by one eps
     * relative, well inside the derivation's headroom. Keeping the barrier is
     * about not making the premise depend on that margin.)
     *
     * The real barrier is Makefile PG_CFLAGS -ffp-contract=off, NOT the named
     * intermediate: a named single-use temporary stops contraction under C11
     * semantics (clang's default), but GCC's -ffp-contract=fast -- the GNU-mode
     * default -- forward-substitutes it and fuses across the statement anyway.
     * Keep BOTH. The flag is also what protects the separate bit-identity
     * contract between score_doc and seg_posting_cb, which no slack covers. */
    contrib = ctx->boost_f[f]
              * bm25_termscore(ctx->idf_f[f], fi->max_tf, fi->min_doclen,
                               ctx->avgdl_f[f], ctx->k1_f[f], ctx->b_f[f]);
    return contrib;
}

/*
 * bm25_block_ub -- safe upper bound on this term's contribution, from the
 * postings this block holds, to any doc in the block *imp describes: summed
 * boost*termscore over every field present in the block AND in the query's
 * scope. A doc whose postings continue into a neighbouring block gets the rest
 * of its contribution from THAT block's bound -- see WHAT IT DOES NOT COVER.
 *
 * Why this dominates every real per-doc contribution (the property the whole
 * WAND milestone depends on, per the task's correctness note): the exact
 * scorer's per-(doc,field) contribution is boost_f * bm25_termscore(idf_f, tf,
 * doclen_f, avgdl_f, k1_f, b_f) (bm25_scan_rank.c:seg_posting_cb). We evaluate the
 * bound with the SAME bm25_termscore, at this field's (max_tf, min_doclen) over
 * the WHOLE block.
 *
 * EVERY monotonicity claim below is conditional on idf >= 0, and inverts if it
 * is not: bm25_termscore is idf * (tf*(k1+1)) / denom, so a negative idf turns
 * "evaluate at (max_tf, min_doclen)" into the block MINIMUM and this "upper"
 * bound sinks below real scores. That precondition is not assumed here -- it is
 * ENFORCED at the single source both this bound and the exhaustive scorer read:
 * bm25_idf (src/bm25_score.c) clamps df to ndocs, so its result is strictly
 * positive; see its header for how df > N arises, and for why the clamp is on df
 * rather than on the result (idf == 0.0 is the "field not in query" sentinel the
 * loop below tests). Do not re-derive an idf without going through bm25_idf.
 *
 * Given idf >= 0: bm25_termscore is monotone NON-DECREASING in tf (more
 * occurrences, more score) and monotone NON-INCREASING in doclen (length
 * normalization penalizes longer docs) for any fixed avgdl/k1/b/idf -- both
 * standard BM25 properties in exact arithmetic. The IEEE-754 evaluation is NOT
 * exactly monotone: tf appears in the numerator and the denominator, so correct
 * rounding of each operation does not make the quotient monotone in tf. Adjacent
 * tf values can invert by a few DBL_EPSILON (about 2 at worst) when
 * k1 * (1 - b + b * norm) is small against one ulp of tf -- at k1 = 0, a legal
 * reloption, for any tf; at ordinary k1 only for tf far beyond what a document
 * holds. That is not a live hole: the per-term deficit is relative to the term,
 * and wand_widen_ub's headroom (at least 0.995n + 3 DBL_EPSILON, below) absorbs
 * it. Read the "dominates" statements below as "dominates to within that
 * headroom", and the exact-arithmetic proof as the shape of the argument, not as
 * a bit-level guarantee. Since max_tf >= every real tf and min_doclen <= every
 * real doclen in the block, the per-field term is >= every real posting's
 * contribution for that field (to within that headroom) -- and
 * BIT-EXACTLY EQUAL when (max_tf, min_doclen) coincides with a real doc's
 * (tf, doclen), because it is the identical function evaluated by the identical
 * arithmetic (this is why we call bm25_termscore here rather than an inlined
 * copy: a differently-grouped copy could round 1 ULP below the scorer at the
 * coincidence boundary and become an UNSAFE bound). Summing those per-field
 * maxima over the block's fields in turn dominates the per-field SUM of every
 * posting THE BLOCK HOLDS for one doc (each term of the real sum is <= the
 * corresponding term of the bound; idf,boost >= 0 so extra fields only add
 * non-negative slack). Fields absent from the block (the loop only iterates
 * fields IN the block) or outside the query's scope (field_in_query gates it,
 * mirroring the C4 idf==0 skip in the real scorer) contribute 0 to both, so the
 * inequality survives.
 *
 * WHAT IT DOES NOT COVER: a document whose postings for the term are split across
 * two blocks (issue #289). A doc's postings for one term are adjacent, one per
 * field, and a block used to be cut every BM25_POSTINGS_PER_BLOCK postings with no
 * regard for documents, so on a multi-field index the last doc of a block can
 * continue into the next one -- (d, title) ending block A, (d, body) opening
 * block B. A's bound then covers only d's title half and B's only its body half;
 * neither dominates d. The writer now cuts at document boundaries
 * (bm25_segment_build_orphans), but segments written before that still hold such
 * straddles, and nothing on disk says which segment is which, so the READER
 * covers them at both places a bound is consumed: wand_cursor_sweep_global_ub
 * sums per-field maxima instead of taking a max of these per-block sums, and the
 * deep check adds, for a cursor whose block's last doc continues into the next
 * block, that doc's postings there, scored exactly (wand_cursor_straddle_ub). A doc spans at most TWO blocks:
 * it holds at most BM25_MAX_FIELDS postings per term, and that is
 * <= BM25_POSTINGS_PER_BLOCK (StaticAssert'd there).
 *
 * CONTRACT -- what this bound owes and what it does NOT. It owes DOMINATION:
 * bound >= the exhaustive scorer's contribution, from this block's postings, to
 * every doc the block holds -- which is the doc's whole contribution unless the
 * doc straddles one of the block's two boundaries (above).
 * It does NOT owe bit-exact agreement with the scorer's arithmetic, and this
 * comment used to claim it did. That claim was not keepable. The bound is
 * consumed at two prune sites and BOTH regroup it relative to the scorer's
 * fold, irreducibly:
 *   - the block-max deep check folds one whole `ub` subtotal per TERM, while
 *     the scorer folds every (term, field) posting flat into one running
 *     double -- SUM_terms(SUM_fields) vs a flat fold, which differ under
 *     IEEE-754 whenever a term after the first matches in >1 field;
 *   - the pivot test sums PRECOMPUTED per-term global_ub maxima in DOCID
 *     order, because walking cursors in docid order IS the WAND pivot rule.
 *     A max-over-blocks cannot be folded into a caller's accumulator, and the
 *     order cannot be changed without destroying the algorithm.
 * So the driver does not ask this function for bit-exactness. It WIDENS every
 * comparison against theta by the worst-case regrouping error instead -- see
 * wand_widen_ub, which carries the arithmetic and its derivation. The
 * bit-exactness that IS required is elsewhere and untouched by this: on the
 * SCORE path (bm25_wand_cursor_score_doc) and in the delivered result order
 * (bm25_wand.h's BIT-EXACT CONTRACT on BM25TopK).
 *
 * Summation ORDER is therefore a SLACK MINIMIZER here, not a correctness
 * dependency: score_doc/seg_posting_cb fold a doc's fields in ASCENDING
 * field_id (posting-decode order), but the on-disk impact table is written in
 * FIRST-OCCURRENCE order (encode_block), which for 3+ fields with
 * heterogeneous per-block presence can be a non-adjacent permutation of
 * field_id (e.g. [1,2,0]). IEEE-754 add is not associative -- (c1+c2)+c0 can
 * land 1 ULP below (c0+c1)+c2 -- so folding in ascending field_id is what
 * makes this function's sum bit-identical to the scorer's at a FULL-COINCIDENCE
 * doc (max_tf AND min_doclen realized in every field => zero slack), and that
 * is what holds wand_widen_ub's slack down to its analytic minimum. Keep the
 * sort: removing it is no longer UNSAFE, but it would inject regrouping error
 * the widening then has to cover, for nothing in return. This is a read-time
 * sort local to the WAND engine; the writer and the on-disk format are
 * untouched.
 *
 * idf_f/avgdl_f/k1_f/b_f/boost_f MUST be the same scan-time values the real
 * scorer would use for this query/term (see BM25WandCtx's header comment):
 * they are index-wide statistics that drift as the corpus grows, so a value
 * baked in at seal time (rather than looked up live here) would go stale and
 * could make this bound wrong in either direction.
 */
double
bm25_block_ub(const BM25BlockImpact *imp, const BM25WandCtx *ctx)
{
    double ub = 0.0;
    uint8  order[BM25_MAX_FIELDS];    /* indices into imp->fields, ascending field_id */
    int    i;

    /* Order the block's present fields by field_id so the accumulation below
     * folds them in the SAME sequence score_doc/seg_posting_cb do. This is a
     * SLACK MINIMIZER, not a safety gate -- see the summation-ORDER paragraph
     * in the header comment: the prune sites widen their theta comparison by
     * the worst-case regrouping error (wand_widen_ub), so a mismatch here
     * would cost tightness, not correctness. Matching the scorer's order keeps
     * that widening at its analytic minimum, so do not drop the sort.
     * field_ids in an impact table are distinct (encode_block keeps one slot
     * per field), so this is a strict sort. nfields <= BM25_MAX_FIELDS, so the
     * insertion sort is trivially cheap. */
    for (i = 0; i < imp->nfields; i++)
    {
        int j = i;

        while (j > 0 &&
               imp->fields[order[j - 1]].field_id > imp->fields[i].field_id)
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = (uint8) i;
    }

    for (i = 0; i < imp->nfields; i++)
    {
        /* Named intermediate, then fold: see wand_field_ub on why the per-field
         * term is rounded on its own before it is added. A gated field returns
         * 0.0, and ub + 0.0 == ub, so this is the old `continue` bit for bit. */
        double contrib = wand_field_ub(&imp->fields[order[i]], ctx);

        ub += contrib;
    }
    return ub;
}

/*
 * bm25_wand_ctx_build -- see bm25_wand.h for why this is split from the stat
 * computation itself: a pure struct-packing step shared by every caller that
 * has already worked out idf_f/avgdl_f its own way.
 */
void
bm25_wand_ctx_build(BM25WandCtx *ctx, uint32 field_count,
                    const double *idf_f, const double *avgdl_f,
                    const BM25FieldConfig *fcfg, int32 qfield)
{
    uint32 f;

    ctx->field_count = field_count;
    for (f = 0; f < field_count; f++)
    {
        ctx->idf_f[f]         = idf_f[f];
        ctx->avgdl_f[f]       = avgdl_f[f];
        ctx->k1_f[f]          = fcfg[f].k1;
        ctx->b_f[f]           = fcfg[f].b;
        ctx->boost_f[f]       = fcfg[f].boost;
        ctx->field_in_query[f] = (qfield == BM25_FIELD_ALL) || (qfield == (int32) f);
    }
}

/* -------------------------------------------------------------------------
 * M2b Task 5: bounded top-k min-heap
 * -------------------------------------------------------------------------
 * Array-backed binary min-heap of at most k elements (a WAND driver always knows
 * k up front from the query's LIMIT/top_k, unlike the exhaustive scan's
 * unbounded accumulator). The array is allocated small and grown by repalloc
 * toward k as candidates arrive (QRY-10), never past it. arr[0] is the root = the
 * WORST currently-kept element, in the sense defined by worse() below -- NOT
 * necessarily the lowest score once ties are in play, because the tie-break
 * direction (larger tid ranks worse) has to mirror scored_desc exactly or a
 * WAND scan could keep a different doc than the exhaustive scan at a
 * score-tied k-th boundary.
 */
struct BM25TopK
{
    BM25Scored     *arr;    /* arr[0..n-1] populated, heap-ordered; cap slots allocated */
    int             k;      /* logical capacity: the k the caller asked for */
    int             cap;    /* slots actually allocated, 0 < cap <= k (QRY-10) */
    int             n;      /* current count, 0 <= n <= cap */
    /* No MemoryContext member: repalloc resolves the chunk's own context, which
     * is the `cxt` create was handed, so growth needs nothing recorded here. */
};

/*
 * worse -- total order the heap is built on: a is worse than b (should sit
 * closer to the root, i.e. be evicted first) iff a's score is lower, or
 * scores tie and a's tid is the LARGER one. That second clause is the whole
 * bit-exact contract: scored_desc (bm25_scan_rank.c) sorts ties by ASCENDING
 * tid, so among score-tied candidates the one with the larger tid is the
 * one that sorts LAST and must be the one dropped when the heap is full.
 * Getting this backwards would make the heap agree with the exhaustive scan
 * on every score but silently disagree on tie-breaks at the k-th boundary.
 */
static inline bool
worse(const BM25Scored *a, const BM25Scored *b)
{
    if (a->score != b->score)
        return a->score < b->score;
    return ItemPointerCompare((ItemPointer) &a->tid, (ItemPointer) &b->tid) > 0;
}

static inline void
topk_swap(BM25Scored *arr, int i, int j)
{
    BM25Scored tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
}

/* Bubble a newly-appended element at index i up toward the root while it is
 * worse than its parent (standard min-heap sift-up, "<" instantiated as
 * worse()). Only ever called with i == n-1 right after an append, so the
 * rest of the heap is already valid going in. */
static void
topk_sift_up(BM25TopK *h, int i)
{
    while (i > 0)
    {
        int parent = (i - 1) / 2;

        if (!worse(&h->arr[i], &h->arr[parent]))
            break;
        topk_swap(h->arr, i, parent);
        i = parent;
    }
}

/* Restore the heap property downward from i (standard min-heap sift-down):
 * repeatedly swap with whichever child is worse than the current node,
 * until neither child is. Only ever called on the root right after
 * replacing it, so both children (if any) are already valid subheaps. */
static void
topk_sift_down(BM25TopK *h, int i)
{
    for (;;)
    {
        int left     = 2 * i + 1;
        int right    = 2 * i + 2;
        int smallest = i;

        if (left < h->n && worse(&h->arr[left], &h->arr[smallest]))
            smallest = left;
        if (right < h->n && worse(&h->arr[right], &h->arr[smallest]))
            smallest = right;
        if (smallest == i)
            break;
        topk_swap(h->arr, i, smallest);
        i = smallest;
    }
}

/* Slots the heap allocates before any candidate is offered. Small enough that a
 * ranked scan over a tiny corpus costs nothing, large enough that the default
 * wand_top_k of 100 reaches its capacity in three doublings. */
#define BM25_TOPK_INITIAL_SLOTS 16

/*
 * bm25_topk_create -- a bounded top-k min-heap with capacity k.
 *
 * QRY-10. Two things used to be wrong here, and they are independent:
 *
 * 1. The only guard on k was `Assert(k > 0)`, which is compiled out in exactly
 *    the builds this ships in. The three debug SRFs reject a non-positive k
 *    themselves, so the Assert was never the last line of defence for that case
 *    -- but a k of 0 would have made bm25_topk_full() true for an empty array,
 *    and bm25_topk_threshold would then read arr[0] out of bounds. Real checks
 *    now, so the contract holds in a production build.
 *
 * 2. The array was sized eagerly to k, which is a function of the GUC and NOT of
 *    the corpus or the query, so `SET bm25_native.wand_top_k = 33554431` made
 *    every ranked scan on a ten-row table allocate ~1 GB before touching a single
 *    posting -- and one notch higher it became palloc's anonymous
 *    "invalid memory alloc request size" (XX000) rather than a user-facing limit.
 *    The array now grows by doubling toward k as candidates are actually offered,
 *    so the heap costs what it holds. arr contents and heap order are byte-for-byte
 *    what the eager version produced -- growth never reorders, and the BIT-EXACT
 *    drain contract (bm25_wand.h) is untouched.
 *
 * The upper bound stays as well, because growth alone does not bound it: a corpus
 * with more than BM25_WAND_TOP_K_MAX matching documents would still grow the array
 * past what one palloc can hold. Rejecting at create time names the knob.
 */
BM25TopK *
bm25_topk_create(int k, MemoryContext cxt)
{
    BM25TopK *h;

    if (k <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: top-k heap capacity must be positive (got %d)", k)));
    if (k > BM25_WAND_TOP_K_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: top-k heap capacity %d exceeds the maximum of %d",
                        k, BM25_WAND_TOP_K_MAX),
                 errhint("Lower bm25_native.wand_top_k.")));

    h = MemoryContextAlloc(cxt, sizeof(BM25TopK));
    h->k   = k;
    h->cap = Min(k, BM25_TOPK_INITIAL_SLOTS);
    h->n   = 0;
    h->arr = MemoryContextAlloc(cxt, sizeof(BM25Scored) * h->cap);
    return h;
}

bool
bm25_topk_full(BM25TopK *h)
{
    /* Against k, NOT cap: cap is an allocation detail, and a heap that has merely
     * filled its current slots is not full in the sense every caller means (the
     * threshold is only meaningful once k candidates are held). */
    return h->n >= h->k;
}

int
bm25_topk_count(BM25TopK *h)
{
    return h->n;
}

double
bm25_topk_threshold(BM25TopK *h)
{
    return bm25_topk_full(h) ? h->arr[0].score : -DBL_MAX;
}

/*
 * bm25_topk_offer -- fold in one (score, tid, segment-source) candidate.
 *
 * Below capacity: always kept, sift-up insert at the end. At capacity: kept
 * only if strictly better than the current root (worse(root, cand) -- see
 * worse()'s tie-break note for why this must be the SAME comparator as the
 * eviction check, not a separate score-only one), in which case it replaces
 * the root and sift-down restores the heap. Otherwise rejected outright --
 * this is the mechanism that lets a block-max driver skip decoding blocks
 * whose bm25_block_ub doesn't clear bm25_topk_threshold, since any candidate
 * from such a block would be rejected here anyway.
 */
bool
bm25_topk_offer(BM25TopK *h, double score, ItemPointer tid,
                BlockNumber src_hdr, uint32 src_gen, uint32 src_docid)
{
    BM25Scored cand;

    cand.score     = score;
    cand.tid       = *tid;
    cand.src_hdr   = src_hdr;
    cand.src_gen   = src_gen;
    cand.src_docid = src_docid;

    if (h->n < h->k)
    {
        int i;

        /* QRY-10: grow toward k on demand, never past it. Doubling keeps the
         * amortized cost per offer O(1), and clamping at k means the array is
         * exactly the eager one by the time the heap is full -- so the steady
         * state of a query that really does produce k candidates is unchanged. */
        if (h->n == h->cap)
        {
            int newcap = (h->cap > h->k / 2) ? h->k : h->cap * 2;

            h->arr = repalloc(h->arr, sizeof(BM25Scored) * newcap);
            h->cap = newcap;
        }
        i = h->n++;

        h->arr[i] = cand;
        topk_sift_up(h, i);
        return true;
    }

    if (worse(&h->arr[0], &cand))
    {
        h->arr[0] = cand;
        topk_sift_down(h, 0);
        return true;
    }
    return false;
}

/*
 * bm25_scored_desc_cmp -- qsort comparator for bm25_topk_drain_sorted,
 * byte-identical in logic to bm25_scan_rank.c's scored_desc (that one is file-
 * static and operates on the unrelated BM25ExhScored type, so it cannot be
 * called directly here; this is a deliberate duplicate, not a drifted
 * copy -- see bm25_wand.h's BIT-EXACT CONTRACT note on BM25TopK for why the
 * two must never diverge).
 */
static int
bm25_scored_desc_cmp(const void *a, const void *b)
{
    double sa = ((const BM25Scored *) a)->score;
    double sb = ((const BM25Scored *) b)->score;

    if (sa < sb)
        return 1;
    if (sa > sb)
        return -1;
    return ItemPointerCompare((ItemPointer) &((const BM25Scored *) a)->tid,
                              (ItemPointer) &((const BM25Scored *) b)->tid);
}

/*
 * bm25_topk_drain_sorted -- copy the heap's array and sort it into final
 * scan order (score desc, tid asc). A plain copy-then-qsort rather than the
 * classic repeated-pop-the-root drain: n is bounded by k (a query's top_k,
 * always small relative to a corpus), so O(n log n) here is not a
 * bottleneck, and it leaves the heap itself untouched (callers that peek at
 * it again -- e.g. via bm25_topk_threshold -- keep working after a drain).
 */
int
bm25_topk_drain_sorted(BM25TopK *h, BM25Scored *out)
{
    memcpy(out, h->arr, sizeof(BM25Scored) * h->n);
    qsort(out, h->n, sizeof(BM25Scored), bm25_scored_desc_cmp);
    return h->n;
}

/* -------------------------------------------------------------------------
 * M2b Task 6: per-term, per-segment pull cursor
 * -------------------------------------------------------------------------
 * BM25WandCursor holds the CURRENT decoded block's postings in fixed
 * BM25_POSTINGS_PER_BLOCK-sized arrays (the builder's own per-block cap, so
 * these never overflow) plus the position within them. A block is always
 * replaced wholesale on load, never grown, so no palloc/repalloc traffic on
 * the per-posting hot path -- only one page read per block crossing.
 *
 * df, not chain end, bounds a term's run throughout (matching every other
 * D-POST walk in this AM -- bm25_seg_scan_postings, bm25_seg_block_header_read's
 * callers): a segment lays every term's blocks into ONE shared chain with no
 * inter-term delimiter, so decoding past df would silently start consuming
 * the NEXT term's postings.
 */
struct BM25WandCursor
{
    Relation           index;
    BM25SegmentHeader  seg;         /* copy: doclen/liveness/tid lookups need only this */
    /* H16: forward cursors over seg's LIVEDOCS/DOCMAP/NORMS chains. WAND skips docids
     * FORWARD only, so every lookup resumes from the page the previous one landed on
     * rather than re-walking from the chain root. Initialized in cursor_open -- the
     * cursor is MemoryContextAllocZero'd and InvalidBlockNumber is not 0. */
    BM25SegReader      rdr;
    const BM25WandCtx *ctx;         /* NOT owned -- see bm25_wand_cursor_open's comment */
    BM25WandStats     *stats;       /* NOT owned; NULL disables counting (Task 11) */

    uint32             df;          /* this term's total posting count in this segment */
    uint32             consumed;    /* postings advanced past so far, 0..df */

    /* Decoded current block. */
    BM25BlockHeader    hdr;
    BM25BlockImpact    imp;
    uint32             docids[BM25_POSTINGS_PER_BLOCK];
    uint32             tfs[BM25_POSTINGS_PER_BLOCK];
    uint32             fields[BM25_POSTINGS_PER_BLOCK];
    uint32             pos;         /* index into the arrays above, 0..hdr.ndocs-1 */

    BlockNumber        next_blk;    /* where the block AFTER the current one starts */
    uint16             next_off;

    double             global_ub;   /* precomputed once at open, see bm25_wand.h */
    /* QRY-11 (issue #153): bm25_block_ub(&imp, ctx) for the block currently
     * decoded into hdr/imp/docids/tfs/fields -- a pure function of those two, both
     * constant for the block's whole residency, recomputed per pivot iteration per
     * aligned cursor before this cache. Same shape as global_ub above, one scope
     * narrower: global_ub is fixed for the cursor's life, this is fixed for the
     * current block's. It is written in the SAME statement sequence that writes
     * `imp` and nowhere else (wand_cursor_load_block's tail), so the cache and its
     * only input share one write site by construction and cannot drift; a fourth
     * way to load a block would have to go through that function to set `imp` at
     * all. Zero before the first load, matching bm25_block_ub over the
     * MemoryContextAllocZero'd (nfields == 0) impact table, so a df == 0 cursor --
     * which never loads a block -- reads the same 0.0 it read before. */
    double             block_ub;    /* bm25_block_ub of the resident block */

    /* Issue #289: what the deep check needs to bound the resident block's LAST doc
     * when that doc continues into the next block (a straddle, possible in a
     * segment written before the writer cut blocks at document boundaries).
     * straddle_state is set by wand_cursor_load_block, the one place a block becomes
     * resident: NONE when no continuation can matter, LEAD when the next block's
     * lead (its first document's postings) was read off the page the load already
     * held and the doc does continue, UNKNOWN when the next block is on another page
     * and wand_cursor_straddle_ub must peek it, only if the deep check ever asks.
     * straddle_ub caches the continuation's bound once computed (straddle_ub_valid).
     * srdr is a second reader over this segment for the continuation's per-field
     * doclens: it looks up block_last values, which ascend, so it stays forward-only,
     * whereas sharing rdr would jump it ahead of the docs score_doc then reads and
     * send each of its lookups back to the chain root. */
    uint8              straddle_state;
    bool               straddle_ub_valid;
    double             straddle_ub;
    BM25BlockLead      next_lead;
    BM25SegReader      srdr;

    uint32             blocks_skipped; /* Task 7: cumulative HEADER-ONLY block
                                        * bypasses across every next_geq call on
                                        * this cursor -- blocks whose postings
                                        * were never varbyte-decoded. EXCLUDES
                                        * the already-resident block a skip run
                                        * starts on (it was decoded before the
                                        * run began), on exactly the same rule as
                                        * BM25WandStats.blocks_skipped, so the sum
                                        * of this counter over every cursor of one
                                        * WAND build EQUALS that struct's total.
                                        * Zeroed by MemoryContextAllocZero in
                                        * open(). */
};

/* Issue #289: BM25WandCursor.straddle_state. NONE is 0 so a zeroed cursor (df == 0,
 * never loads a block) reads as "no continuation". */
#define BM25_STRADDLE_NONE      0   /* the resident block's last doc gets nothing from the next */
#define BM25_STRADDLE_LEAD      1   /* it continues: next_lead holds its continuation */
#define BM25_STRADDLE_UNKNOWN   2   /* next block on another page, not read yet */

/* Could a continuation of a doc whose posting in this block is in field last_field
 * contribute anything? Its postings would be in fields AFTER last_field (a doc's
 * postings for a term are in ascending field_id), so only if the query scores one of
 * those. Always false on a single-field index and for a query scoped to one field
 * that is not after it: the common cases pay nothing for issue #289. */
static inline bool
wand_continuation_can_score(const BM25WandCtx *ctx, uint32 last_field)
{
    uint32 f;

    for (f = last_field + 1; f < ctx->field_count; f++)
        if (ctx->field_in_query[f] && ctx->idf_f[f] != 0.0)
            return true;
    return false;
}

/* Does the next block's lead continue the resident block's last doc? Within one
 * term's run docids ascend across blocks, so the lead's docid is either that doc
 * (a straddle) or later; earlier is a corrupt run, refused rather than guessed at. */
static uint8
wand_lead_continues(const BM25WandCursor *cur, const BM25BlockLead *lead)
{
    if (lead->docid == cur->hdr.last_docid)
        return BM25_STRADDLE_LEAD;
    if (lead->docid > cur->hdr.last_docid)
        return BM25_STRADDLE_NONE;
    ereport(ERROR,
            (errcode(ERRCODE_INDEX_CORRUPTED),
             errmsg("bm25: posting block starts at docid %u, before the previous block's last docid %u",
                    lead->docid, cur->hdr.last_docid)));
    return BM25_STRADDLE_NONE;      /* keep the compiler quiet */
}

/* ---- POST block order on the WAND path (issue #303) ----
 *
 * The exhaustive reader holds a term's run to ascending (docid, field) across blocks
 * (bm25_seg_scan_postings). The WAND cursor reads the same run through three other
 * doors -- the open-time header sweep, next_geq's header peek, and the block decode --
 * and checked none of them, so a link back into the run re-scored documents and a link
 * forward skipped them, with no error (an ADR 0111 residual). The rules here are the
 * same order, applied to what each door has:
 *
 *   - a header (sweep, peek): last_docid > the previous block's last_docid. Equality
 *     is legitimate in one shape only: a block holding nothing but the straddling
 *     document of an old count-sliced segment (issue #289). Such a block has at most
 *     BM25_MAX_FIELDS postings, under BM25_POSTINGS_PER_BLOCK, so the writer cannot
 *     have cut it short anywhere but at the run's end: it is the run's last block.
 *     The current writer cuts at document boundaries and never writes one.
 *   - a decoded block: its first (docid, field) after the previous block's last.
 *     Exact when the previous block was decoded (next(), or next_geq's first peek
 *     past the resident block); when next_geq bypassed headers, only the last
 *     bypassed header's last_docid is known, so the first docid must be >= it (a
 *     straddle continues the same docid).
 *
 * The sweep visits every block header of the run at cursor open, over pages that do
 * not change for the segment's gen, so a header out of order is caught before any
 * posting is scored; the peek repeats the check on the headers it uses. The decode
 * check covers what a header cannot show: a block whose header is in order but whose
 * first postings step back. None of this moves a BM25WandStats counter: the checks
 * read only values the walk already has. */
static void
wand_header_order_validate(uint32 prev_last, const BM25BlockHeader *hdr,
                           uint32 before, uint32 df, BlockNumber blk)
{
    if (hdr->last_docid > prev_last)
        return;
    if (hdr->last_docid == prev_last && before + hdr->ndocs == df &&
        hdr->ndocs <= BM25_MAX_FIELDS)
        return;
    ereport(ERROR,
            (errcode(ERRCODE_INDEX_CORRUPTED),
             errmsg("bm25: posting block header ends at docid %u, not after the previous "
                    "block's last docid %u", hdr->last_docid, prev_last),
             errdetail("The block is on page %u of the postings chain.", blk),
             errhint("REINDEX the index.")));
}

/* What wand_cursor_load_block knows of the block before the one it decodes. */
typedef enum
{
    WAND_PRED_NONE,             /* the run's first block (cursor open) */
    WAND_PRED_EXACT,            /* the previous block was decoded: (docid, field) */
    WAND_PRED_DOCID             /* only a bypassed header's last_docid */
} WandPredKind;

typedef struct WandPred
{
    WandPredKind kind;
    uint32       docid;
    uint32       field;
} WandPred;

/*
 * wand_cursor_load_block -- decode ONE block's postings at (blk, off) into the
 * cursor's arrays, and determine the (blk, off) of the block that follows it
 * in the chain. The page-walk / boundary rule (does another whole block fit
 * on THIS page, else nextblk starts fresh at content offset 0) is IDENTICAL
 * to bm25_seg_block_header_read's -- both read the same on-disk layout -- but
 * that reader stops at the header + impact table, while a pull cursor also
 * needs the docid/tf/field-RLE streams varbyte-decoded (bm25_seg_scan_postings'
 * inner-loop algorithm, reproduced here rather than driven through that
 * function's push-callback interface, because the cursor needs one block at
 * a time on demand, not the whole df run pushed through a callback).
 *
 * count_examined -- Task 11 instrumentation: increment cur->stats->blocks_examined
 * for THIS call, unless the caller already counted this exact block's header touch
 * itself (bm25_wand_cursor_next_geq peeks a block's header via
 * bm25_seg_block_header_read BEFORE deciding to decode it here as the skip's
 * landing block -- that peek is the touch; decoding it a moment later must not
 * count a second time). Every other caller (cursor open, a plain next() block
 * crossing) never peeked first, so those pass true.
 *
 * pred -- what the caller knows of the run's previous block, for the first-posting
 * order check (issue #303, see "POST block order on the WAND path" above). Passed by
 * value: next()'s exact predecessor is the block this call overwrites.
 */
static void
wand_cursor_load_block(BM25WandCursor *cur, BlockNumber blk, uint16 off, bool count_examined,
                       WandPred pred)
{
    Buffer       buf;
    Page         pg;
    char        *cp,
                *pend;
    BlockNumber  nextpage;
    const uint8 *dp,
                *tp,
                *dend,
                *tend;
    uint32       prev = 0;         /* reset per block: each block delta-encodes from 0 */
    uint32       i;
    Size         block_len;

    /* Gate before ReadBuffer (QRY-01). Two of the three callers feed an untrusted
     * value: bm25_wand_cursor_open passes post_root straight off a DICT entry, and
     * bm25_wand_cursor_next passes the page-opaque nextblk -- the same quantity
     * bm25_seg_scan_postings already guards. (next_geq's cand_blk is dominated by
     * its own peek, but the guard belongs in the callee so a fourth caller cannot
     * reintroduce the hole.) InvalidBlockNumber here is P_NEW: a corrupt chain link
     * would extend the relation from inside an ordinary ranked scan, WAND being the
     * default path. */
    bm25_seg_blkno_validate(blk, "postings block pointer");
    buf = ReadBuffer(cur->index, blk);

    if (count_examined && cur->stats != NULL)
        cur->stats->blocks_examined++;

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    /* Gen through the arm (issue #303): a corrupt link into another segment's POST page
     * is XX002, the reclaim race stays 40001. Released first; then kind. */
    if (BM25PageGetOpaque(pg)->seg_gen != cur->seg.gen)
    {
        uint32  page_gen = BM25PageGetOpaque(pg)->seg_gen;

        UnlockReleaseBuffer(buf);
        bm25_seg_gen_mismatch(cur->index, blk, page_gen, cur->seg.gen,
                              "segment POST chain");
    }
    bm25_seg_page_validate_kind(pg, 0, BM25_PAGE_POST);
    nextpage = BM25PageGetOpaque(pg)->nextblk;
    cp   = (char *) PageGetContents(pg) + off;
    /* Validated content length, not raw pd_lower -- see bm25_seg_scan_postings.
     * WAND is the default ranked path, so a silently-empty page here is a wrong
     * answer on ordinary queries, not just on a debug probe. */
    pend = (char *) PageGetContents(pg) + bm25_page_content_bytes(pg);

    /* Same decode boundary as bm25_seg_scan_postings / bm25_seg_block_header_read.
     * This path is not a lesser one: WAND is on by default, so this is where an
     * ordinary ranked query decodes its blocks, and cur->docids/tfs/fields are all
     * BM25_POSTINGS_PER_BLOCK-wide members of the cursor. Both Asserts here were
     * compiled out in exactly the build where a corrupt page matters. */
    if (cp + sizeof(BM25BlockHeader) > pend)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block offset %u past the end of block %u",
                        off, blk)));
    memcpy(&cur->hdr, cp, sizeof(BM25BlockHeader));
    block_len = bm25_block_validate(&cur->hdr, cp, pend);

    dp = (const uint8 *) (cp + sizeof(BM25BlockHeader));
    dend = dp + cur->hdr.docid_bytes;
    tp = dend;
    tend = tp + cur->hdr.tf_bytes;

    /* Field-id RLE decoded ONCE up front, in lockstep with docid/tf below --
     * exactly bm25_seg_scan_postings' order. field_rle_bytes == 0 means every
     * posting is field 0 (single-field segment, no RLE stream on the page). */
    if (cur->hdr.field_rle_bytes > 0)
        bm25_field_rle_decode(tp + cur->hdr.tf_bytes, cur->hdr.field_rle_bytes,
                              cur->fields, cur->hdr.ndocs);
    else
        for (i = 0; i < cur->hdr.ndocs; i++)
            cur->fields[i] = 0;

    for (i = 0; i < cur->hdr.ndocs; i++)
    {
        uint32 delta;

        dp += bm25_varbyte_decode(dp, dend, &delta);
        tp += bm25_varbyte_decode(tp, tend, &cur->tfs[i]);
        prev += delta;
        cur->docids[i] = prev;
    }

    /* The run now exists, so the header's last_docid can finally be checked against
     * it. next_geq's landing scan and the block-max deep check both trust this value;
     * see bm25_block_last_docid_validate for what each does when it is wrong. */
    bm25_block_last_docid_validate(&cur->hdr, cur->docids);

    /* The block's first posting after the previous block's last (issue #303). ndocs
     * is at least 1 here: bm25_block_validate rejects an empty block. */
    if ((pred.kind == WAND_PRED_EXACT &&
         (cur->docids[0] < pred.docid ||
          (cur->docids[0] == pred.docid && cur->fields[0] <= pred.field))) ||
        (pred.kind == WAND_PRED_DOCID && cur->docids[0] < pred.docid))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block starts at docid %u field %u, not after the "
                        "previous block's last posting (docid %u)",
                        cur->docids[0], cur->fields[0], pred.docid),
                 errdetail("The block is on page %u of the postings chain.", blk),
                 errhint("REINDEX the index.")));

    bm25_decode_impact_table((const uint8 *) (cp + sizeof(BM25BlockHeader) +
                                              cur->hdr.docid_bytes + cur->hdr.tf_bytes +
                                              cur->hdr.field_rle_bytes),
                             cur->hdr.impact_bytes, &cur->imp);

    cp += block_len;        /* computed and bounds-checked by bm25_block_validate */

    if (cp + sizeof(BM25BlockHeader) <= pend)
    {
        /* Another whole block fits on THIS page: stay put. */
        cur->next_blk = blk;
        cur->next_off = (uint16) (cp - (char *) PageGetContents(pg));
    }
    else
    {
        /* Page exhausted: the chain continues on nextpage, which always
         * starts fresh at content offset 0 (same convention every other
         * POST-chain walker in this AM uses for continuation pages).
         * nextpage == InvalidBlockNumber here means true chain end. */
        cur->next_blk = nextpage;
        cur->next_off = 0;
    }

    /* Issue #289: classify the resident block's tail for the deep check (see
     * straddle_state). consumed counts the postings ahead of this block here -- every
     * caller loads a block with consumed at its first posting -- so the term's run
     * continues past it iff consumed + ndocs < df. When the next block is on this
     * same page, read its lead now, under the lock already held: that costs no buffer
     * access, where a later peek would cost one per block. */
    cur->straddle_state = BM25_STRADDLE_NONE;
    cur->straddle_ub_valid = false;
    if (cur->consumed + cur->hdr.ndocs < cur->df &&
        wand_continuation_can_score(cur->ctx, cur->fields[cur->hdr.ndocs - 1]))
    {
        if (cur->next_blk == blk && cp + sizeof(BM25BlockHeader) <= pend)
        {
            BM25BlockHeader nh;

            memcpy(&nh, cp, sizeof(BM25BlockHeader));
            (void) bm25_block_validate(&nh, cp, pend);
            bm25_block_lead_decode(&nh, cp, cur->hdr.last_docid, &cur->next_lead);
            cur->straddle_state = wand_lead_continues(cur, &cur->next_lead);
        }
        else
            cur->straddle_state = BM25_STRADDLE_UNKNOWN;
    }

    UnlockReleaseBuffer(buf);
    cur->pos = 0;

    /* QRY-11: the block's upper bound, computed ONCE here rather than on every
     * bm25_wand_cursor_block_max call. Deliberately after the unlock -- it touches
     * only cur->imp (already copied out) and cur->ctx, so the insertion sort and the
     * per-field bm25_termscore calls no longer run under a buffer SHARE lock.
     * bm25_block_ub is a pure function of those two, so this value is bit-identical
     * to what the accessor used to return for every call until the next load. */
    cur->block_ub = bm25_block_ub(&cur->imp, cur->ctx);
}

/*
 * wand_cursor_straddle_ub -- issue #289: a bound on what the resident block's LAST
 * doc gets from postings in the NEXT block, or 0.0 if it has none there.
 *
 * The deep check calls this only for a cursor whose block_last lies inside the
 * skip window, where block_max alone under-bounds a straddler: its own block's
 * bound covers only the postings that block holds. The rest of the straddler's
 * postings for the term are all in the next block -- a doc holds at most
 * BM25_MAX_FIELDS <= BM25_POSTINGS_PER_BLOCK postings per term (StaticAssert'd at
 * the writer), so it spans at most two -- and they are exactly that block's lead
 * (bm25_block_lead_decode). So the bound here is their EXACT contribution: each
 * posting's boost_f * bm25_termscore at its own tf and the doc's own doclen for the
 * field, the expression score_doc evaluates for the same posting. Domination then
 * holds with equality per term, and the widening covers the summation.
 *
 * Exact rather than the next block's bound because that is what keeps the deep
 * check's pruning on old segments: the next block's (max_tf, min_doclen) over ~100
 * other docs is far above one doc's posting. In this change's A/B (100k-document
 * corpora, two and three fields, count-sliced layout), adding the next block's
 * bm25_block_ub scored up to 24% more documents than the pre-fix build in the worst
 * cell; the exact continuation, at most 4.3%. Liveness is not consulted (a dead
 * straddler still adds its bound), which can only cost pruning, never correctness.
 *
 * Costs: the lead was usually read at load time off the page already held
 * (straddle_state LEAD/NONE); a next block on another page is peeked here, once,
 * only if the deep check asks (UNKNOWN). The doclens are srdr lookups, one per
 * continuation posting, on a forward-only reader. Neither is counted in
 * BM25WandStats, like the open-time sweep: they are not the runtime skip
 * behaviour those counters witness, and a peeked block decoded later must still
 * count once.
 */
static double
wand_cursor_straddle_ub(BM25WandCursor *cur)
{
    if (cur->straddle_state == BM25_STRADDLE_UNKNOWN)
    {
        BM25BlockHeader hdr;
        BM25BlockImpact imp;
        BlockNumber     next_blk;
        uint16          next_off;

        /* The run continues, so an InvalidBlockNumber here is corruption, and the
         * reader's own block-number validation refuses it. */
        bm25_seg_block_header_read_lead(cur->index, cur->next_blk, cur->next_off,
                                        cur->seg.gen, &hdr, &imp,
                                        &next_blk, &next_off, cur->hdr.last_docid,
                                        &cur->next_lead);
        cur->straddle_state = wand_lead_continues(cur, &cur->next_lead);
    }
    if (cur->straddle_state == BM25_STRADDLE_NONE)
        return 0.0;

    if (!cur->straddle_ub_valid)
    {
        const BM25BlockLead *lead = &cur->next_lead;
        const BM25WandCtx   *ctx = cur->ctx;
        uint32               j;

        cur->straddle_ub = 0.0;
        for (j = 0; j < lead->n; j++)
        {
            uint32 f = lead->field_id[j];

            if (f < ctx->field_count && ctx->field_in_query[f] && ctx->idf_f[f] != 0.0)
            {
                uint32 doclen_f = bm25_seg_reader_doclen_field(&cur->srdr, lead->docid, f);
                /* Named intermediate, as in score_doc and wand_field_ub. */
                double contrib = ctx->boost_f[f] *
                                 bm25_termscore(ctx->idf_f[f], lead->tf[j], doclen_f,
                                                ctx->avgdl_f[f], ctx->k1_f[f], ctx->b_f[f]);

                cur->straddle_ub += contrib;
            }
        }
        cur->straddle_ub_valid = true;
    }
    return cur->straddle_ub;
}

/*
 * wand_cursor_sweep_global_ub -- the term's global upper bound in this
 * segment: for each field, the MAX over every block of that field's bound
 * (wand_field_ub), SUMMED over fields in ascending field_id, read via the
 * header-only reader (bm25_seg_block_header_read -- no docid/tf/RLE decode).
 *
 * Why a sum of per-field maxima and not the max of per-block sums (issue #289):
 * the pivot test needs a bound on the term's WHOLE contribution to any remaining
 * doc, and a doc whose postings straddle two blocks draws its fields from both.
 * The max of bm25_block_ub over blocks covered neither half in full, so a
 * straddler whose two halves together beat theta could fail the pivot and be
 * abandoned with the rest of the segment. Each of a doc's postings lies in SOME
 * block holding its field, where that field's bound dominates it, so the sum of
 * per-field maxima dominates every doc however its postings are split. It is
 * never below the old value and equals it when one block realizes every field's
 * maximum; ADR 0096 measured that loosening this bound moves only deep-check
 * skips, not documents scored. Same walk, same reads, so no added I/O.
 *
 * Header-only saves the
 * decode, not the walk: this is one ReadBuffer/lock/release per BLOCK, linear in
 * the term's block count, paid per (query term, segment) before anything is
 * pruned. It was measured rather than assumed cheap (QRY-03, issue #153): under 1%
 * of a WAND build's buffer accesses on an ordinary 100k-doc corpus, where the build
 * costs about seven accesses per scored (term, document) pair, and 7.8% on a corpus
 * built so later segments quit at the pivot test right after this sweep. Storing
 * the bound's inputs in the DICT record was built and rejected on those numbers;
 * batching this loop by page was declined on them without being built -- see ADR
 * 0096 before optimizing it. df-bounded like every other per-term block walk here,
 * for the same D-POST reason.
 *
 * Held to df exactly, like bm25_seg_scan_postings (issue #293): a block claiming more
 * postings than the run has left, or a chain ending before df, ERRORs. The bound alone
 * would be harmless over a short chain (every block that exists was swept), but this
 * sweep runs at every cursor open over the whole run, so being strict here is what
 * makes a short run fail the same way on the WAND path as on the exhaustive one,
 * before any posting is scored -- one comparison per block.
 */
static double
wand_cursor_sweep_global_ub(Relation index, BM25SegmentHeader *seg,
                            BlockNumber post_root, uint16 post_off, uint32 df,
                            const BM25WandCtx *ctx)
{
    BlockNumber blk = post_root;
    uint16      off = post_off;
    uint32      seen = 0;
    uint32      prev_last = 0;      /* the previous block's last_docid (issue #303) */
    double      ub = 0.0;
    double      field_max[BM25_MAX_FIELDS] = {0};
    uint32      f;

    while (blk != InvalidBlockNumber && seen < df)
    {
        BM25BlockHeader hdr;
        BM25BlockImpact imp;
        BlockNumber     next_blk;
        uint16          next_off;
        int             fi;

        CHECK_FOR_INTERRUPTS();

        bm25_seg_block_header_read(index, blk, off, seg->gen, &hdr, &imp,
                                   &next_blk, &next_off);
        if (hdr.ndocs > df - seen)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: posting block claims %u postings but the term's run "
                            "has only %u left",
                            hdr.ndocs, df - seen),
                     errdetail("The block is on page %u of the postings chain.", blk),
                     errhint("REINDEX the index.")));
        if (seen > 0)
            wand_header_order_validate(prev_last, &hdr, seen, df, blk);
        prev_last = hdr.last_docid;
        for (fi = 0; fi < imp.nfields; fi++)
        {
            uint8  id = imp.fields[fi].field_id;
            double this_ub;

            /* wand_field_ub is 0.0 for such a field anyway; the test is here to
             * bound the field_max index (field_count <= BM25_MAX_FIELDS, the
             * size of every BM25WandCtx array). */
            if (id >= ctx->field_count)
                continue;
            this_ub = wand_field_ub(&imp.fields[fi], ctx);
            if (this_ub > field_max[id])
                field_max[id] = this_ub;
        }
        seen += hdr.ndocs;
        blk = next_blk;
        off = next_off;
    }
    if (seen != df)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: postings chain ends after %u of the term's %u postings",
                        seen, df),
                 errdetail("The chain starts at block %u, offset %u.", post_root, post_off),
                 errhint("REINDEX the index.")));

    /* Ascending field_id, the scorer's per-doc fold order: a slack minimizer, as in
     * bm25_block_ub. Absent fields add an exact 0.0. */
    for (f = 0; f < ctx->field_count; f++)
        ub += field_max[f];
    return ub;
}

BM25WandCursor *
bm25_wand_cursor_open(Relation index, BM25SegmentHeader *seg,
                      BlockNumber post_root, uint16 post_off, uint32 df,
                      const BM25WandCtx *ctx, MemoryContext cxt,
                      BM25WandStats *stats, bool all_live)
{
    BM25WandCursor *cur = (BM25WandCursor *)
        MemoryContextAllocZero(cxt, sizeof(BM25WandCursor));

    cur->index    = index;
    cur->seg      = *seg;
    cur->ctx      = ctx;
    cur->stats    = stats;
    cur->df       = df;
    cur->consumed = 0;
    /* Reads through the cursor's OWN seg copy, whose lifetime is the cursor's -- not
     * the caller's `seg`, which cursor_open does not promise to outlive. */
    bm25_seg_reader_init_known(&cur->rdr, index, &cur->seg, all_live);
    /* Issue #267: the cursor's NORMS lookups (one per scored field posting, nearly all
     * on the page the previous one landed on) read a page image instead of the
     * buffer. In cxt, the cursor's own context, so the image lives and dies with the
     * cursor; the driver's cursors are in its per-segment context (bm25_wand_segment). */
    bm25_seg_reader_cache_pages(&cur->rdr, cxt);
    /* Issue #289: the straddle continuation's doclen reader (see srdr). Its page
     * image is allocated on first use, so a segment with no straddle (anything the
     * current writer produced) never pays the 8 KB; on an old one it turns one
     * buffer access per straddle into about one per NORMS page. */
    bm25_seg_reader_init_known(&cur->srdr, index, &cur->seg, all_live);
    bm25_seg_reader_cache_pages(&cur->srdr, cxt);

    /* Precompute BEFORE any posting decode: a header-only pass over every block
     * (linear in the term's block count -- see wand_cursor_sweep_global_ub for
     * what it costs), yielding a fixed ceiling for this cursor's whole lifetime
     * (see bm25_wand.h). Deliberately NOT counted into stats -- see
     * BM25WandStats's comment on why this always-done sweep is out of scope for
     * the runtime block-skip witness. */
    cur->global_ub = wand_cursor_sweep_global_ub(index, seg, post_root, post_off,
                                                 df, ctx);

    if (df > 0)
    {
        WandPred    none = {WAND_PRED_NONE, 0, 0};

        wand_cursor_load_block(cur, post_root, post_off, true, none);
    }

    return cur;
}

uint32
bm25_wand_cursor_docid(BM25WandCursor *cur)
{
    if (cur->consumed >= cur->df)
        return BM25_DOCID_MAX;
    return cur->docids[cur->pos];
}

void
bm25_wand_cursor_next(BM25WandCursor *cur)
{
    if (cur->consumed >= cur->df)
        return;                     /* already exhausted: no-op */

    cur->consumed++;
    cur->pos++;
    if (cur->consumed < cur->df && cur->pos >= cur->hdr.ndocs)
    {
        WandPred    pred = {WAND_PRED_EXACT, cur->docids[cur->hdr.ndocs - 1],
                            cur->fields[cur->hdr.ndocs - 1]};

        wand_cursor_load_block(cur, cur->next_blk, cur->next_off, true, pred);
    }
}

/*
 * bm25_wand_cursor_next_geq -- Task 7's block-SKIP jump: advance to the first
 * posting with docid >= target.
 *
 * Two no-op guards up front, in order: exhaustion (matches next()'s own
 * guard) and "already there" -- a cursor only ever moves FORWARD, so if the
 * current posting already satisfies target there is nothing to skip, and
 * re-deriving that from scratch would risk landing one posting later than
 * where the cursor already sits.
 *
 * The skip loop tracks the block currently under consideration in LOCAL
 * variables (cand_ndocs/cand_last/cand_pos/cand_blk/cand_off) -- starting
 * from the cursor's own already-decoded block -- and peeks ONE block ahead
 * per iteration via the Task 3 header-only reader, bm25_seg_block_header_read
 * (BM25BlockHeader plus the impact table, never the docid/tf/field-RLE
 * varbyte streams), rather than writing each peeked header into cur->hdr as
 * it goes. That indirection matters: cur->hdr and cur->imp/docids/tfs/fields
 * are a matched pair everywhere else in this file (always the SAME block,
 * both refreshed together by wand_cursor_load_block), and block_last()/
 * block_max() read them on that assumption. Mutating cur->hdr with a merely
 * PEEKED header before knowing whether the term's run ends first (target
 * beyond the last posting) would leave that pair mismatched on the
 * exhaustion path -- cur->hdr describing a block cur->imp/docids never
 * decoded. Keeping peek state local until a real decode is committed avoids
 * that trap entirely: on early return (exhausted), cur->hdr/imp/pos are
 * untouched, exactly the same self-consistent state a plain next()-driven
 * exhaustion would leave.
 *
 * Each loop iteration:
 *   1. accounts for the candidate block as bypassed: consumed advances past
 *      whatever of it was not yet consumed (cand_ndocs - cand_pos; cand_pos
 *      is 0 for every candidate except possibly the very first, since a
 *      peeked-but-not-decoded block is never partially consumed). This keeps
 *      the "consumed == total postings before the current block + pos"
 *      invariant next()/docid() rely on intact even though these postings
 *      are never varbyte-decoded.
 *   2. checks df (not chain end) to decide whether the term's run has ended
 *      before reaching target -- exactly the D-POST bound every other
 *      per-term block walk in this file/AM enforces, since a term's blocks
 *      share the segment's one POST chain with no inter-term delimiter and
 *      nothing else stops a skip from wandering into the NEXT term's blocks.
 *   3. peeks the next block's header, replacing the candidate so the loop's
 *      own condition re-tests against IT next iteration.
 * The loop exits the instant a peeked block's last_docid >= target: that
 * block is the landing block, and its own (blk, off) is what gets a REAL
 * decode below.
 *
 * If the cursor's already-decoded block satisfies target immediately (no
 * skip needed at all), the loop body never runs (skipped_any stays false)
 * and the already-decoded arrays are reused as-is -- no redundant re-decode
 * of a block already in hand.
 *
 * Once positioned on the landing block (freshly decoded if the loop ran,
 * otherwise the cursor's own current block), pos/consumed advance linearly
 * within it to the first docid >= target. This cannot run off the end of
 * the block's arrays: last_docid is that block's MAXIMUM docid (the
 * ascending delta-decoded stream's final element), and last_docid >= target
 * is exactly the condition that got us here, so some index in [pos, ndocs)
 * satisfies docids[i] >= target.
 */
void
bm25_wand_cursor_next_geq(BM25WandCursor *cur, uint32 target)
{
    uint32      cand_ndocs = cur->hdr.ndocs;
    uint32      cand_last  = cur->hdr.last_docid;
    uint32      cand_pos   = cur->pos;
    bool        cand_is_resident = true;   /* Task 11: the FIRST cand is the
                                             * block already decoded and in
                                             * hand -- see BM25WandStats's
                                             * comment on why bypassing its
                                             * remainder must not count as a
                                             * header-only skip */
    BlockNumber cand_blk   = InvalidBlockNumber;   /* only used once skipped_any */
    uint16      cand_off   = 0;
    BlockNumber peek_blk;
    uint16      peek_off;
    bool        skipped_any = false;
    /* The landing block's predecessor (issue #303): the resident block, known
     * exactly, until a peeked header is bypassed; then that header's last_docid. */
    WandPred    pred = {WAND_PRED_EXACT, cur->hdr.last_docid,
                        cur->fields[cur->hdr.ndocs > 0 ? cur->hdr.ndocs - 1 : 0]};

    if (cur->consumed >= cur->df)
        return;                             /* already exhausted: no-op */
    if (bm25_wand_cursor_docid(cur) >= target)
        return;                             /* already at/past target: no-op,
                                              * never move backward */

    peek_blk = cur->next_blk;
    peek_off = cur->next_off;

    while (cand_last < target)
    {
        BM25BlockHeader hdr;
        BM25BlockImpact imp;
        BlockNumber     next_blk;
        uint16          next_off;

        CHECK_FOR_INTERRUPTS();

        cur->consumed += cand_ndocs - cand_pos;
        /* Both counters gate on !cand_is_resident, and must stay in lockstep:
         * the resident block was fully varbyte-decoded before this skip run
         * started, so walking off its tail is not a header-only bypass. Counting
         * it made the per-cursor counter report a "skip" for a target one block
         * ahead, where next_geq peeks one header and then decodes that block --
         * work indistinguishable from a plain linear next() crossing, i.e.
         * exactly the disguised-linear-walk case the counter exists to rule
         * out (sql/43_wand_parity.sql). */
        if (!cand_is_resident)
        {
            cur->blocks_skipped++;
            if (cur->stats != NULL)
                cur->stats->blocks_skipped++;
        }

        if (cur->consumed >= cur->df)
        {
            /* Term's run ends before reaching target: exhausted, but the
             * skips already taken above still counted. cur->hdr/imp/pos are
             * untouched -- still the last block genuinely decoded. */
            cur->consumed = cur->df;
            return;
        }
        /* Postings remain but the chain does not (issue #293). This used to share
         * the branch above and read as exhaustion, dropping the rest of the run,
         * while a plain next() crossing the same link ERRORs in
         * wand_cursor_load_block; a skip and a step over the same corrupt link now
         * fail alike. */
        if (peek_blk == InvalidBlockNumber)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: postings chain ends after %u of the term's %u postings",
                            cur->consumed, cur->df),
                     errhint("REINDEX the index.")));

        bm25_seg_block_header_read(cur->index, peek_blk, peek_off, cur->seg.gen,
                                   &hdr, &imp, &next_blk, &next_off);
        if (cur->stats != NULL)
            cur->stats->blocks_examined++;     /* Task 11: this header just got touched */
        /* Issue #303: in order after the candidate it replaces, whose last_docid is
         * still in cand_last; consumed now counts the postings before this block. */
        wand_header_order_validate(cand_last, &hdr, cur->consumed, cur->df, peek_blk);
        if (!cand_is_resident)
        {
            pred.kind  = WAND_PRED_DOCID;
            pred.docid = cand_last;
        }
        cand_blk   = peek_blk;
        cand_off   = peek_off;
        cand_ndocs = hdr.ndocs;
        cand_last  = hdr.last_docid;
        cand_pos   = 0;
        cand_is_resident = false;
        skipped_any = true;

        if (cand_last >= target)
            break;                  /* landing block: cand_blk/cand_off name it */

        peek_blk = next_blk;
        peek_off = next_off;
    }

    /* count_examined = false: the landing block's header was already counted
     * at the peek above (Task 11) -- decoding it now is not a second touch. */
    if (skipped_any)
        wand_cursor_load_block(cur, cand_blk, cand_off, false, pred);   /* now DO decode */

    while (cur->pos < cur->hdr.ndocs && cur->docids[cur->pos] < target)
    {
        cur->pos++;
        cur->consumed++;
    }
}

uint32
bm25_wand_cursor_blocks_skipped(BM25WandCursor *cur)
{
    return cur->blocks_skipped;
}

uint32
bm25_wand_cursor_block_last(BM25WandCursor *cur)
{
    return cur->hdr.last_docid;
}

double
bm25_wand_cursor_block_max(BM25WandCursor *cur)
{
    return cur->block_ub;       /* QRY-11: cached at block load; see the field */
}

double
bm25_wand_cursor_global_ub(BM25WandCursor *cur)
{
    return cur->global_ub;
}

/*
 * bm25_wand_cursor_score_doc -- sum every field-posting at the current docid,
 * bit-exactly matching seg_posting_cb's (bm25_scan_rank.c) per-doc, per-term
 * contribution. This identity is the whole point of this task (D8): the WAND
 * driver built on top of this cursor (bm25_wand_build_ranking, below) is only
 * as trustworthy as this match is exact, so every gate and the termscore call
 * itself mirror the real scorer rather than approximate it:
 *   - idf_f[field_id] == 0.0 or field_in_query false -> field contributes 0,
 *     exactly seg_posting_cb's "term absent from this field" skip (the
 *     latter check is redundant whenever idf_f was itself pre-zeroed for an
 *     out-of-scope field, as the real scorer's C4 gate does, but bm25_block_ub
 *     already checks both -- see its comment -- so score_doc matches it).
 *   - a tombstoned doc contributes 0 from every field (seg_posting_cb returns
 *     before accumulating anything for a dead doc); checked ONCE per doc,
 *     not per field, since liveness does not vary by field, but this ONLY
 *     zeroes the contribution -- the doc's postings still occupy the block
 *     and must be consumed via next() to reach the next docid.
 *   - doclen comes from the segment's NORMS (bm25_seg_doclen_field), the
 *     doc's REAL per-field length, never the block's impact-table min_doclen
 *     (that quantity bounds a whole block for bm25_block_ub; it is not any
 *     single doc's actual length and would silently produce the wrong score
 *     here).
 *   - bm25_termscore itself, never an inlined reimplementation, so the
 *     floating-point rounding is identical to the scorer's, not merely close
 *     (the same reason bm25_block_ub calls it rather than duplicating it).
 *   - fields of one doc are summed in field-stream (decode) order -- the
 *     SAME order seg_posting_cb's caller accumulates them in (D-ACCUM's
 *     ascending-(docid,field_id) posting order) -- so the addition sequence,
 *     and therefore its rounding, matches too.
 *   - each field-posting is folded straight into *acc (`*acc += contrib`), NOT
 *     into a private per-term subtotal that is then added to the caller's
 *     running score. seg_posting_cb does acc[D] += contrib per posting, so for
 *     a doc matching THIS term in >1 field the exhaustive path computes
 *     ((r + a) + b); a subtotal-then-add here would compute r + (a + b), a
 *     1-ULP divergence under IEEE-754 that fails the parity gate (Task 8, D8).
 *     The BMW driver accumulates a candidate's WHOLE score into one running
 *     double across all its query terms by calling this once per term with the
 *     same acc, reproducing the exhaustive accumulator's exact addition order.
 *   - *contributed latches true whenever a posting clears the SAME gate that
 *     lets seg_posting_cb call bm25_scores_add (live, in-scope, idf != 0) --
 *     see the header comment for why the driver must test this, not *acc's
 *     value, to decide whether a candidate belongs in the results at all.
 */
void
bm25_wand_cursor_score_doc(BM25WandCursor *cur, double *acc, bool *contributed)
{
    uint32 docid = bm25_wand_cursor_docid(cur);
    bool   live;

    Assert(docid != BM25_DOCID_MAX);   /* checked: caller tests cursor_docid() first */

    live = bm25_seg_reader_doc_is_live(&cur->rdr, docid);

    while (bm25_wand_cursor_docid(cur) == docid)
    {
        uint32 field_id = cur->fields[cur->pos];
        uint32 tf       = cur->tfs[cur->pos];

        if (live &&
            field_id < cur->ctx->field_count &&
            cur->ctx->field_in_query[field_id] &&
            cur->ctx->idf_f[field_id] != 0.0)
        {
            uint32 doclen_f = bm25_seg_reader_doclen_field(&cur->rdr, docid,
                                                          field_id);
            /* Round the boosted term score to a double FIRST, then fold that
             * rounded value into *acc -- TWO separate roundings, exactly as
             * seg_posting_cb does (contrib = boost*termscore; acc[D] += contrib).
             * Folding in one expression (*acc += boost*termscore) would let the
             * compiler contract it to a single-rounding FMA, diverging from the
             * exhaustive scorer by 1 ULP whenever *acc is already non-zero (i.e.
             * for the 2nd+ query term of a candidate) -- the multi-term BM25F
             * parity failure this materialized as. The named intermediate is
             * a rounding barrier FP-contraction cannot cross. */
            double contrib = cur->ctx->boost_f[field_id] *
                             bm25_termscore(cur->ctx->idf_f[field_id], tf, doclen_f,
                                            cur->ctx->avgdl_f[field_id],
                                            cur->ctx->k1_f[field_id],
                                            cur->ctx->b_f[field_id]);

            *acc += contrib;
            *contributed = true;
        }
        bm25_wand_cursor_next(cur);
    }
}

/* -------------------------------------------------------------------------
 * M2b Task 8: the Block-Max WAND driver
 * -------------------------------------------------------------------------
 * bm25_wand_build_ranking replaces the exhaustive per-source scorer with
 * Ding-Suel Block-Max WAND for bag-of-words scored scans (D4/D7), producing a
 * result BIT-IDENTICAL to the exhaustive path's top-k prefix (D1/D8). The
 * engine here uses only the public cursor + heap + block-bound primitives
 * above. The per-term idf and pending scoring it needs come from bm25_stats.c,
 * the layer the exhaustive scorer uses too, through the two static adapters
 * bm25_wand_prepare_terms and bm25_wand_score_pending defined just before
 * bm25_wand_build_ranking. This file calls nothing in the scanner, whose
 * bm25_scan_rank.c calls it (#67.13, ADR 0093). Those adapters were once
 * defined in bm25_scan.c, which made the two modules call each other.
 */

/*
 * wand_widen_ub -- widen a WAND upper bound UPWARD by the worst-case error
 * between its own floating-point summation and the exhaustive scorer's, so the
 * widened value may be compared against theta directly. EVERY prune in this
 * driver must go through this; there are exactly two (the pivot/termination
 * test and the block-max deep check, both in bm25_wand_segment).
 *
 * WHY this exists rather than a bit-exact bound: see bm25_block_ub's CONTRACT
 * paragraph. Both prune sites regroup and/or reorder the bound's summands
 * relative to the scorer's flat fold, and neither regrouping can be undone
 * without breaking WAND itself. Domination is the property the algorithm
 * actually needs; bit-exactness stays where it is achievable and required (the
 * score path, and the drained result order). A widened prune can only ever
 * score MORE candidates than a tight one, never fewer, so it cannot change the
 * delivered ranking -- only how much work is skipped on the way to it.
 *
 * THE ARITHMETIC (check it; do not take it on faith). Let u = 2^-53 be the
 * unit roundoff, so DBL_EPSILON == 2u == 2^-52.
 *
 * (1) Every summand on both sides is non-negative, and each is correctly
 *     rounded. Non-negativity is ENFORCED, not assumed: bm25_idf clamps df to
 *     ndocs so idf > 0, per-field boost reloptions reject negatives and
 *     NaN/inf (bm25_build.c), and a query-node boost must be > 0
 *     (bm25_query.c). It is load-bearing HERE too, not just for the bound's
 *     monotonicity proof -- ub >= 0 is what makes multiplying by (1 + eps)
 *     move the value UP. A negative contribution would invert the widening
 *     into a narrowing and make both prunes unsafe. For n non-negative values
 *     combined by any TREE of correctly-
 *     rounded additions -- which covers both a flat fold and this driver's
 *     sum-of-subtotals -- the classic bound (Higham, "Accuracy and Stability
 *     of Numerical Algorithms" 2nd ed., sec. 4.2) is
 *         |fl(S) - S| <= gamma_n * S,   gamma_n = n*u / (1 - n*u).
 *     Here n <= nsummands <= (query terms) * BM25_MAX_FIELDS, astronomically below
 *     1/u, so gamma_n <= 1.001 * n*u; read gamma_n as n*u below.
 *
 * (2) In EXACT arithmetic the bound dominates term by term (bm25_block_ub's
 *     monotonicity proof), so S_ub >= S_score. The floating-point term is not
 *     exactly monotone in tf (bm25_block_ub: a few DBL_EPSILON per term, at
 *     worst about 2); that deficit is charged to step (4)'s headroom.
 *
 * (3) Chaining (1) and (2):
 *         fl(score) <= S_score * (1 + gamma_n)
 *                   <= S_ub    * (1 + gamma_n)
 *                   <= fl(ub) * (1 + gamma_n)/(1 - gamma_n)
 *                   <= fl(ub) * (1 + 2.01*n*u)
 *                    = fl(ub) * (1 + 1.005*n*DBL_EPSILON).
 *
 * (4) Forming the widened value itself rounds twice (the 1 + x, then the
 *     multiply), each costing at most u relative, i.e. at most one further
 *     DBL_EPSILON. Taking the factor as (1 + (2n + 4) * DBL_EPSILON) leaves
 *     (2n + 4) - 1.005n - 1 = 0.995n + 3 DBL_EPSILON of headroom for every
 *     n >= 0. The +4 is what makes the n == 0 and n == 1 cases hold.
 *
 * The result is a relative widening on the order of 1e-14: a block is spared
 * only when its bound sits within ~1e-14 of theta, so the pruning power lost
 * is not measurable. That is the whole trade -- a rounding-scale loss of
 * skipping in exchange for a domination property that holds unconditionally.
 *
 * nsummands must be an UPPER bound on the count of elementary (term, field)
 * contributions folded at the call site -- not the number of cursors, since
 * each cursor's bound is itself a sum over that block's fields.
 */
static inline double
wand_widen_ub(double ub, int nsummands)
{
    return ub * (1.0 + (double) (2 * nsummands + 4) * DBL_EPSILON);
}

/* Insertion sort the present-cursor working array ascending by CURRENT docid
 * (BM25_DOCID_MAX == PG_UINT32_MAX, so exhausted cursors sort to the end). This
 * is the per-pivot RE-sort: the array was sorted on the previous iteration and
 * only the cursors that moved are out of place, the input an insertion sort is
 * near-linear on. The FIRST sort of a segment starts from query order instead and
 * goes through wand_sort_cursors_initial. This array is the pivot view; the fixed
 * by_qi[] mapping (query-term order) is what scoring iterates, so re-sorting here
 * never disturbs trap #1's summation order. */
static void
wand_sort_cursors_by_docid(BM25WandCursor **cur, int nt)
{
    int i,
        j;

    for (i = 1; i < nt; i++)
    {
        BM25WandCursor *key = cur[i];
        uint32          kd  = bm25_wand_cursor_docid(key);

        for (j = i - 1; j >= 0 && bm25_wand_cursor_docid(cur[j]) > kd; j--)
            cur[j + 1] = cur[j];
        cur[j + 1] = key;
    }
}

/* One cursor in wand_sort_cursors_initial: its docid, read once, and its position
 * in the incoming (query-term) order as the tiebreak. */
typedef struct WandSortItem
{
    BM25WandCursor *cur;
    uint32          docid;
    int             pos;
} WandSortItem;

static int
wand_sort_item_cmp(const void *a, const void *b)
{
    const WandSortItem *x = (const WandSortItem *) a;
    const WandSortItem *y = (const WandSortItem *) b;

    if (x->docid != y->docid)
        return (x->docid < y->docid) ? -1 : 1;
    return (x->pos > y->pos) - (x->pos < y->pos);
}

/* The first sort of a segment's cursors (#305 XCUT-07). They arrive in query-term
 * order, which can be any permutation of docid order, and the insertion sort above
 * is quadratic on a reversed one: about nt^2/2 shifts with no interrupt check, so a
 * bag-of-words query of tens of thousands of terms (nt is uncapped there) held off
 * a cancel for seconds. qsort makes it O(nt log nt). The position tiebreak makes
 * the result the stable order the insertion sort produced, so pivot selection, and
 * every bm25_wand_stats counter, is unchanged. */
static void
wand_sort_cursors_initial(BM25WandCursor **cur, int nt)
{
    WandSortItem   *items;
    int             i;

    if (nt < 2)
        return;
    items = (WandSortItem *) palloc(sizeof(WandSortItem) * nt);
    for (i = 0; i < nt; i++)
    {
        items[i].cur = cur[i];
        items[i].docid = bm25_wand_cursor_docid(cur[i]);
        items[i].pos = i;
    }
    qsort(items, nt, sizeof(WandSortItem), wand_sort_item_cmp);
    for (i = 0; i < nt; i++)
        cur[i] = items[i].cur;
    pfree(items);
}

/*
 * bm25_wand_segment_scan -- one sealed segment's BMW pass under the shared global
 * heap (D5). This is the scan itself; bm25_wand_segment further below is the thin
 * memory-context wrapper QRY-04 added around it, and carries its own header.
 *
 * Opens one cursor per query term PRESENT in the segment (skipping
 * terms with no in-scope df, whose contribution is identically zero), then
 * repeatedly:
 *
 *   1. sort cursors by current docid; stop when all are exhausted.
 *   2. WAND pivot on the GLOBAL per-term upper bounds UB_t: walk cursors in
 *      docid order summing UB_t until the running sum >= theta. If no prefix
 *      reaches theta, NO remaining doc in this segment can beat theta (each remaining
 *      doc's score <= sum UB_t over the terms it holds <= sum over all live
 *      cursors < theta) -- the segment is DONE (safe termination, D4). This is why
 *      the pivot uses UB_t (bounds ALL of a term's remaining docs, including one
 *      whose postings straddle two blocks -- see wand_cursor_sweep_global_ub),
 *      not the per-block bound (which a later block could exceed).
 *   3. if the lowest cursor lags the pivot docid, next_geq the laggers up to it
 *      and re-pivot (alignment).
 *   4. once every cursor <= pivot sits exactly ON the pivot docid, run the
 *      block-max DEEP check: sum block_max over EVERY cursor now at the pivot
 *      (== the exact set that contributes to this doc, so the sum is a safe
 *      upper bound; summing only the [0..piv] prefix could under-bound a doc a
 *      later equal-docid cursor also matches and WRONGLY skip it), plus, for a
 *      cursor whose block's last doc continues into the next block and lies in
 *      the skip window, that doc's postings there (issue #289: block_max covers
 *      only the postings its own block holds). If that sum < theta, no doc
 *      within the window can beat theta -- shallow-skip every involved cursor
 *      to Min(min block_last + 1, next_docid), where next_docid is the smallest
 *      current docid among cursors STRICTLY beyond the pivot. min block_last + 1
 *      > pivot alone only guarantees PROGRESS (termination); the next_docid CAP
 *      is what makes the skip SAFE -- without it a pivot cursor could jump PAST
 *      a docid another (below-pivot-bound) cursor also matches, whose true score
 *      could beat theta once both cursors' block_max is counted together,
 *      silently dropping it (standard Ding-Suel BMW shallow-skip cap).
 *   5. otherwise score the candidate: term-major (query-term order, trap #1),
 *      posting-at-a-time (trap #2) via score_doc, which ALSO consumes every
 *      cursor at the pivot (so the loop always advances). Dead or pending-owned
 *      (deduped, pending-wins) docs are consumed the same way but never offered.
 *
 * Every offered candidate is a live, non-deduped doc with its exact score;
 * pruned/terminated docs provably have score strictly below the (monotonically
 * rising) theta, so they cannot belong to the final top-k. The heap resolves
 * score-ties by TID exactly as scored_desc, so the drained set matches the
 * exhaustive top-k bit-for-bit -- including at the k-th boundary. NOTE the
 * candidate is ALWAYS offered (no `> theta` pre-gate): the heap's own tie-aware
 * accept test keeps a score == theta candidate with a smaller TID (which belongs in
 * the top-k), whereas a strict `> theta` pre-gate would drop it and diverge.
 */
static void
bm25_wand_segment_scan(Relation index, const BM25SegCatEntry *segcat,
                       BM25Token *qtoks, int nq, const BM25WandCtx *ctx,
                       const bool *active, const BM25TermSegLoc *locs, uint32 loc_stride,
                       BM25TopK *heap, HTAB *pending_tids, BM25WandStats *stats)
{
    BM25SegmentHeader seg;
    /* The DRIVER's own forward cursors over seg's LIVEDOCS/DOCMAP chains, for the
     * per-candidate tid+liveness resolve at the pivot below. Distinct from the per-term
     * BM25WandCursor.rdr: those resolve doclen while SCORING one term's postings, this
     * one resolves the candidate document ONCE per pivot, and each cursor holding its
     * own reader is what lets all of them stay positioned in their chains at the same
     * time. Same shape and reasons as BM25WandCursor.rdr (and TermScoreCtx.rdr in
     * bm25_scan_rank.c): the one-shot accessors it replaces re-walked each chain from its
     * root per candidate AND took a SEGREAD-11 RelationGetNumberOfBlocks apiece, which
     * lseeks on every call outside recovery -- on the DEFAULT ranked path.
     *
     * Opened on this function's own `seg`, which it fills just below and which outlives
     * every use; one segment per call, so this reader is never carried to another.
     *
     * Liveness (issue #229, ADR 0100): on a tombstone-free segment the candidate's
     * and each scored posting's liveness test was a LIVEDOCS buffer access apiece,
     * about two of the seven accesses a scored (term, document) pair cost. The
     * bitmap is now checked ONCE per segment, here, after the dictionary lookups --
     * so a segment holding no query term pays nothing -- and gated on the summed df,
     * which bounds every lookup this reader and the cursors can make. The cursors
     * take the result rather than re-walking the same chain. */
    BM25SegReader     rdr;
    BM25WandCursor  **by_qi;   /* [nq] query-term-order: cursor or NULL. Scoring view. */
    BM25WandCursor  **cur;     /* [nt] present cursors, re-sorted by docid. Pivot view. */
    BlockNumber      *post_root;   /* [nq] the dictionary lookups, kept for the opens */
    uint16           *post_off;
    uint32           *df;
    bool             *present;
    uint64            sum_df = 0;
    bool              all_live;
    int               nt = 0;
    int               qi,
                      i;

    by_qi     = (BM25WandCursor **) palloc(sizeof(BM25WandCursor *) * Max(nq, 1));
    cur       = (BM25WandCursor **) palloc(sizeof(BM25WandCursor *) * Max(nq, 1));
    post_root = (BlockNumber *) palloc(sizeof(BlockNumber) * Max(nq, 1));
    post_off  = (uint16 *) palloc(sizeof(uint16) * Max(nq, 1));
    df        = (uint32 *) palloc(sizeof(uint32) * Max(nq, 1));
    present   = (bool *) palloc0(sizeof(bool) * Max(nq, 1));

    /* The dictionary lookups come from bm25_term_idf's df pass on this same snapshot
     * (issue #246): locs[qi * loc_stride] is term qi's entry for this segment. Walking
     * the dictionary again here gave the same answer -- a sealed segment's dictionary
     * never changes -- at up to a quarter of a rare-term query over many segments. */
    for (qi = 0; qi < nq; qi++)
    {
        const BM25TermSegLoc *loc = &locs[(Size) qi * loc_stride];

        by_qi[qi] = NULL;
        if (!active[qi])
            continue;               /* term absent / fully out of query scope */
        if (loc->found)
        {
            post_root[qi] = loc->post_root;
            post_off[qi] = loc->post_off;
            df[qi] = loc->df;
            present[qi] = true;
            sum_df += df[qi];
            nt++;
        }
    }
    if (nt == 0)
        return;                     /* no query term present in this segment */

    /* Read only for a segment that holds a query term, which is also the only kind
     * that opens cursors on it. The df pass already read and gen-checked it. */
    bm25_seg_header_read(index, segcat->header_blkno, segcat->gen, &seg);

    all_live = bm25_seg_reader_init_checked(&rdr, index, &seg, sum_df);
    /* Issue #267: one DOCMAP image for the per-candidate TID lookups. CurrentMemoryContext
     * is bm25_wand_segment's per-segment context, deleted when this scan returns, which
     * is also when rdr goes out of scope. */
    bm25_seg_reader_cache_pages(&rdr, CurrentMemoryContext);

    nt = 0;
    for (qi = 0; qi < nq; qi++)
    {
        if (present[qi])
        {
            BM25WandCursor *c;

            /* Per cursor open (#305 XCUT-07): nt is uncapped for a bag-of-words
             * query, and each open sets up two readers and loads a block. */
            CHECK_FOR_INTERRUPTS();
            c = bm25_wand_cursor_open(index, &seg, post_root[qi],
                                      post_off[qi], df[qi], &ctx[qi],
                                      CurrentMemoryContext, stats,
                                      all_live);

            by_qi[qi] = c;
            cur[nt++] = c;
        }
    }
    wand_sort_cursors_initial(cur, nt);

    for (;;)
    {
        double          theta,
                        sum,
                        bsum,
                        running;
        int             piv;
        int             naligned;   /* cursors folded into bsum, for the slack */
        int             nstraddle;  /* #289: continuation bounds folded into bsum */
        uint32          pivot,
                        min_last,
                        next_docid,
                        skip_target;
        ItemPointerData tid;
        bool            skip;
        bool            scored;
        /* WAND driver: bounded by k only once the heap fills, so a
         * low-selectivity query walks the whole posting space here. */
        CHECK_FOR_INTERRUPTS();

        wand_sort_cursors_by_docid(cur, nt);
        if (bm25_wand_cursor_docid(cur[0]) == BM25_DOCID_MAX)
            break;                          /* every cursor exhausted */

        theta = bm25_topk_threshold(heap);

        /* WAND pivot on global UB_t (safe termination).
         *
         * PRUNE SITE 1 of 2. `sum` is folded in cur[]'s DOCID order (the pivot
         * rule itself) over PRECOMPUTED per-term global_ub maxima, so it is
         * regrouped and reordered relative to the flat, query-term-order fold
         * the scorer would produce for the same doc -- and unlike the deep
         * check below, neither can be repaired: a max-over-blocks does not
         * compose into a caller's accumulator, and reordering destroys the
         * pivot. So `sum` is widened UPWARD before the test. Direction check:
         * a larger `sum` reaches theta at an EARLIER i, giving a smaller pivot
         * docid and making `piv < 0` (abandon the rest of the segment) fire
         * less often -- strictly more candidates considered, never fewer.
         *
         * Coincidence is not exotic here, which is why this site matters as
         * much as the deep check: any term with df <= BM25_POSTINGS_PER_BLOCK
         * has a single block, so its global_ub IS that block's bound, and a
         * term occurring in one doc realizes it exactly. */
        sum = 0.0;
        piv = -1;
        for (i = 0; i < nt; i++)
        {
            if (bm25_wand_cursor_docid(cur[i]) == BM25_DOCID_MAX)
                break;                      /* exhausted cursors sort last: none beyond */
            sum += bm25_wand_cursor_global_ub(cur[i]);
            /* (i + 1) terms folded so far, each a sum over at most BM25_MAX_FIELDS
             * fields: a deliberately loose but obviously-correct upper bound on
             * the elementary summand count wand_widen_ub needs. */
            if (wand_widen_ub(sum, (i + 1) * BM25_MAX_FIELDS) >= theta)
            {
                piv = i;
                break;
            }
        }
        if (piv < 0)
            break;                          /* no remaining doc can reach theta: segment done */

        pivot = bm25_wand_cursor_docid(cur[piv]);

        if (bm25_wand_cursor_docid(cur[0]) < pivot)
        {
            /* align laggers up to the pivot docid, then re-pivot */
            for (i = 0; i < piv; i++)
                if (bm25_wand_cursor_docid(cur[i]) < pivot)
                    bm25_wand_cursor_next_geq(cur[i], pivot);
            continue;
        }

        /* fully aligned: block-max deep check over EVERY cursor now at pivot.
         * cur[] is docid-sorted and cur[0] == pivot here (the alignment branch
         * above guarantees no cursor lags pivot), so the cursors AT pivot are a
         * contiguous prefix; the first cursor whose docid != pivot is thus the
         * smallest docid STRICTLY beyond pivot -- the shallow-skip cap. */
        bsum       = 0.0;
        min_last   = PG_UINT32_MAX;
        next_docid = BM25_DOCID_MAX;   /* stays MAX => no cap when nothing is beyond pivot */
        for (i = 0; i < nt; i++)
        {
            if (bm25_wand_cursor_docid(cur[i]) != pivot)
            {
                next_docid = bm25_wand_cursor_docid(cur[i]);
                break;                  /* end of the pivot run: no more == pivot */
            }
            {
                uint32 bl = bm25_wand_cursor_block_last(cur[i]);

                bsum += bm25_wand_cursor_block_max(cur[i]);
                if (bl < min_last)
                    min_last = bl;
            }
        }
        /* The loop above either breaks at the first cursor past pivot or runs
         * out at nt, so i is exactly the number of cursors folded into bsum --
         * the same set the scoring loop below walks, just in a different order. */
        naligned = i;

        /* The shallow-skip target, Min(min_last+1, next_docid), computed BEFORE the
         * prune test because the test's bound depends on it (#289, below).
         * min_last+1 > pivot (block_last >= pivot for a block the cursor is
         * positioned in) guarantees PROGRESS; capping at next_docid is the SAFETY
         * half -- a bare min_last+1 could jump a cursor PAST a docid another cursor
         * sits on, whose combined block_max could beat theta, dropping it unscored
         * (Ding-Suel BMW cap).
         *
         * Saturating, not wrapping (QRY-07). min_last is a block header's
         * last_docid; at PG_UINT32_MAX the bare `+ 1` wraps to 0, skip_target
         * collapses to 0, every cursor's next_geq returns at its "already at/past
         * target" guard, nothing advances, and the driver loop below spins --
         * cancellable only by CHECK_FOR_INTERRUPTS. The value is now cross-checked
         * against its decoded run (bm25_block_last_docid_validate), so reaching
         * UINT32_MAX needs a segment with 2^32 local docids rather than one
         * corrupt header field; this keeps the arithmetic total regardless, since
         * the cost is a comparison and the failure mode is a hang. */
        skip_target = (min_last == PG_UINT32_MAX) ? PG_UINT32_MAX : min_last + 1;
        if (next_docid < skip_target)
            skip_target = next_docid;

        /* Issue #289: a prune must dominate every doc in [pivot, skip_target), and
         * the resident blocks' bounds do so for every doc whose postings for the
         * term all lie in the resident block. The exception is a block's LAST doc
         * when it continues into the next block (a straddle, possible in any
         * segment written before the writer cut blocks at document boundaries).
         * Such a doc is in the window exactly when its cursor's block_last <
         * skip_target -- skip_target <= min_last + 1, so block_last == min_last --
         * and wand_cursor_straddle_ub supplies its continuation scored EXACTLY --
         * that doc's own tf and doclen for the fields in the next block's lead,
         * not the next block's bm25_block_ub, which over-bounds (0.0 when there is
         * no straddle). This
         * covers the pivot doc itself too, when it is the straddler. No doc of the
         * NEXT block other than the straddler can be in the window, since its
         * docids exceed block_last >= skip_target - 1.
         *
         * Consulted only when the resident bounds alone would prune: adding
         * non-negative bounds can only keep bsum at or above theta, so when it is
         * already there the answer is "score", and no straddle read is paid. */
        nstraddle = 0;
        if (wand_widen_ub(bsum, naligned * BM25_MAX_FIELDS) < theta)
            for (i = 0; i < naligned; i++)
                if (bm25_wand_cursor_block_last(cur[i]) < skip_target)
                {
                    double sub = wand_cursor_straddle_ub(cur[i]);

                    if (sub > 0.0)
                    {
                        bsum += sub;
                        nstraddle++;
                    }
                }

        /* PRUNE SITE 2 of 2. bsum is folded over cur[] (docid order, re-sorted
         * every iteration) as SUM_terms(SUM_fields), while the score below is a
         * flat fold over every (term, field) posting in fixed by_qi order. Both
         * the ORDER and the GROUPING therefore differ from the scorer's, so the
         * comparison is widened rather than assumed bit-exact -- see
         * wand_widen_ub and bm25_block_ub's CONTRACT paragraph. A straddle bound
         * is one more sum of up to BM25_MAX_FIELDS per-posting terms (a doc's
         * continuation), so each one counts as a further cursor's worth of
         * summands.
         *
         * Direction check: widening bsum UPWARD makes `< theta` fire less
         * often, so strictly fewer shallow-skips and strictly more candidates
         * scored -- never the reverse. The widening is about the summation of
         * the bounds; their per-doc validity over the window is the straddle
         * paragraph above.
         *
         * Why the tie case makes this reachable at all: bm25_topk_offer keeps a
         * candidate whose score EQUALS theta when its TID is the smaller one
         * (worse()'s tie-break, the BIT-EXACT CONTRACT in bm25_wand.h), so a
         * bsum even one ULP under a true score that happens to equal theta
         * would drop a doc the exhaustive path keeps. */
        if (wand_widen_ub(bsum, (naligned + nstraddle) * BM25_MAX_FIELDS) < theta)
        {
            /* Can't beat theta within these blocks: shallow-skip the involved
             * cursors to skip_target (above). */
            if (stats != NULL)
                stats->deep_check_skips++;     /* Task 11 (C-TEST fix): THIS
                                                 * branch, not alignment's own
                                                 * next_geq call above, is the
                                                 * block-max deep check firing */
            for (i = 0; i < nt; i++)
                if (bm25_wand_cursor_docid(cur[i]) == pivot)
                    bm25_wand_cursor_next_geq(cur[i], skip_target);
            continue;
        }

        /* candidate at pivot: resolve dedup (pending wins) + tombstone once */
        tid  = bm25_seg_reader_docid_to_tid(&rdr, pivot);
        skip = !bm25_seg_reader_doc_is_live(&rdr, pivot);
        if (!skip && pending_tids != NULL)
        {
            bool found;

            (void) hash_search(pending_tids, &tid, HASH_FIND, &found);
            skip = found;
        }

        /* Score TERM-MAJOR (query-term order, trap #1), POSTING-AT-A-TIME
         * (trap #2): score_doc folds each field-posting straight into `running`
         * and consumes every cursor at the pivot, so the loop makes progress
         * whether or not we offer (a dead/deduped doc's postings are consumed
         * too -- score_doc zeroes a dead doc internally, and we drop it below). */
        running = 0.0;
        scored  = false;
        for (qi = 0; qi < nq; qi++)
            if (by_qi[qi] != NULL && bm25_wand_cursor_docid(by_qi[qi]) == pivot)
            {
                bm25_wand_cursor_score_doc(by_qi[qi], &running, &scored);
                if (stats != NULL)
                    stats->docs_scored++;      /* Task 11: one (term, candidate) decode */
            }

        /* `scored` (not "running != 0.0"): a candidate whose only postings at
         * this docid all failed score_doc's live/in-scope/idf!=0 gate (most
         * commonly: every occurrence of every query term lands on a field
         * OUTSIDE a field-scoped query's so->qfield, C4) must NOT be offered,
         * exactly as the exhaustive accumulator never inserts such a TID (no
         * bm25_scores_add call ever fires for it) -- see bm25_wand_cursor_
         * score_doc's header comment for why "running > 0" is the WRONG test
         * (a genuinely scored zero-boost-field contribution is legitimately
         * 0.0 and must still be offered). Without this gate a field-scoped
         * query offers every doc ANY query term touches in ANY field, with a
         * spurious score of 0.0, once the heap is not yet full.
         *
         * M2b Task 9: segment source for M5 key projection -- header_blkno comes
         * from the catalog entry (BM25SegmentHeader has no self-block field,
         * matching every other src_hdr use in this AM), gen from the just-read,
         * validated header, docid from the pivot this candidate scored at. */
        if (!skip && scored)
            (void) bm25_topk_offer(heap, running, &tid,
                                   segcat->header_blkno, seg.gen, pivot);
    }
}

/*
 * bm25_wand_segment -- QRY-04: the per-segment memory boundary.
 *
 * The scan above allocates a BM25WandCursor per present query term (about 2.5-3 KB
 * each: a 128-slot docid/tf/field block, a BM25BlockImpact, a BM25SegmentHeader copy
 * and a BM25SegReader) plus the two BM25WandCursor* arrays. All of it used to land
 * in the driver's single scratchctx, which is deleted only after the LAST segment --
 * so a 20-token query over a 60-segment index held ~1,200 live cursors (~3 MB) by
 * the end, even though only one segment's worth (~60 KB) is ever in use, and none of
 * it is charged against bm25_native.max_match_memory. (Since issue #267 a segment's
 * worth also includes one 8 KB page image per cursor and one for the driver's reader,
 * about 170 KB more for that query; bounded the same way. A cursor on a segment
 * written before issue #289 can allocate a second image, for its straddle reader
 * srdr, when it first meets a straddle.) The `nt == 0` early return
 * leaked its two arrays the same way.
 *
 * A private child context deleted on return bounds the whole subsystem at ONE
 * segment's cursors, and the wrapper shape means the early return needs no cleanup
 * of its own -- the mistake a per-exit-path pfree invites. Same discipline
 * bm25_scan_build_ranking_once already applies to its own scratch.
 *
 * NOTHING OF THE CURSORS ESCAPES, which is what makes this safe: candidates reach
 * the caller only through bm25_topk_offer, which copies scalars into the heap's own
 * array; that array lives in the driver's scratchctx and its growth repalloc
 * resolves the chunk's own context, so it keeps growing there rather than here.
 * `heap`, `pending_tids` and `stats` are all caller-owned and merely written
 * through. The segment header the scan reads is a stack copy.
 */
static void
bm25_wand_segment(Relation index, const BM25SegCatEntry *segcat,
                  BM25Token *qtoks, int nq, const BM25WandCtx *ctx,
                  const bool *active, const BM25TermSegLoc *locs, uint32 loc_stride,
                  BM25TopK *heap, HTAB *pending_tids, BM25WandStats *stats)
{
    MemoryContext segcxt = AllocSetContextCreate(CurrentMemoryContext,
                                                 "bm25 wand segment cursors",
                                                 ALLOCSET_SMALL_SIZES);
    MemoryContext oldcxt = MemoryContextSwitchTo(segcxt);

    bm25_wand_segment_scan(index, segcat, qtoks, nq, ctx, active, locs, loc_stride,
                           heap, pending_tids, stats);

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(segcxt);
}

/* -------------------------------------------------------------------------
 * M2b Task 8: WAND stat helpers (idf + pending) for bm25_wand_build_ranking
 * -------------------------------------------------------------------------
 * The driver needs per-query-term global idf (to build each term's BM25WandCtx
 * and block bound) and the pending arm scored into its heap. Both rest on
 * bm25_stats.c (bm25_term_idf; bm25_pending_score_term and its BM25AccEnt
 * accumulator), and these two thin adapters fit that layer to the driver's
 * shapes. Both reproduce the exhaustive scorer's exact df->idf and pending
 * accumulation, which is what makes the WAND result bit-identical (D8): the SAME
 * idf feeds the block bound and the score, and the SAME bm25_pending_score_term
 * fills the heap as fills the exhaustive accumulator.
 *
 * They live here, not in bm25_stats.c, because they call bm25_wand_ctx_build and
 * bm25_topk_offer: in the stats layer they would make it depend on this file.
 * They moved here from bm25_scan.c with #67.13 and became static, since
 * bm25_wand_build_ranking is their only caller.
 */
static void
bm25_wand_prepare_terms(Relation index, const BM25ScanSnapshot *snap,
                        BM25Token *qtoks, int nq, uint64 live_ndocs,
                        const double *avgdl_f, const uint64 *fld_ndocs,
                        const BM25FieldConfig *fcfg, int32 qfield,
                        BM25WandCtx *out_ctx, double *out_idf, bool *out_active,
                        BM25TermSegLoc *out_locs)
{
    int    qi;

    /* Per-term stats come from the SHARED bm25_term_idf, the same routine the
     * exhaustive scorer runs -- that identity is what makes the WAND bound and
     * score bit-exact against it (D8). out_active[qi] is its `any_df` (false =>
     * the driver opens no cursor and pending scoring no-ops on this term). */
    for (qi = 0; qi < nq; qi++)
    {
        double *idf_f = out_idf + (Size) qi * BM25_MAX_FIELDS;

        /* boost is 1.0 unconditionally: a query-time boost exists only on the M6
         * boolean path, and a boolean tree never reaches WAND at all (the seam
         * excludes it via bm25_qtree_is_multileaf), so no WAND term can carry one. */
        out_active[qi] = bm25_term_idf(index, snap, qtoks[qi].ptr, qtoks[qi].len,
                                       live_ndocs, fld_ndocs, qfield, 1.0, idf_f,
                                       out_locs + (Size) qi * snap->nsegs);
        bm25_wand_ctx_build(&out_ctx[qi], snap->field_count, idf_f, avgdl_f,
                            fcfg, qfield);
    }
}

static void
bm25_wand_score_pending(Relation index, BlockNumber pending_head,
                        uint32 epoch_bound,
                        BM25Token *qtoks, int nq, uint32 field_count,
                        const double *idf, const double *avgdl_f,
                        const BM25FieldConfig *fcfg,
                        BM25TopK *heap, HTAB *pending_tids)
{
    HTAB           *acc;
    HASHCTL         ctl;
    HASH_SEQ_STATUS seq;
    BM25AccEnt     *e;
    int             qi;

    /* Same TID-keyed accumulator the exhaustive path fills for pending, so a
     * pending doc's summed score is bit-identical here. Scoped to
     * CurrentMemoryContext (the driver's scratch context), freed below. */
    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(ItemPointerData);
    ctl.entrysize = sizeof(BM25AccEnt);
    ctl.hcxt      = CurrentMemoryContext;
    acc = hash_create("bm25 wand pending acc", 256, &ctl,
                      HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    for (qi = 0; qi < nq; qi++)
        bm25_pending_score_term(index, pending_head, epoch_bound,
                                qtoks[qi].ptr, qtoks[qi].len,
                                field_count, idf + (Size) qi * BM25_MAX_FIELDS, avgdl_f, fcfg,
                                acc, pending_tids,
                                /* Unaccounted on purpose: this accumulator holds only the
                                 * pending list, whose size bm25_native.seal_threshold
                                 * already caps. Charging it here would bound the same
                                 * memory twice. */
                                NULL);

    /* Offer each pending doc's FULL summed score to the shared heap (order of
     * offering is irrelevant -- the heap + final drain impose scored_desc). This
     * primes theta before the segments (D6); pending_tids now holds every scored
     * pending TID so the segment passes dedupe against them (pending wins).
     * e->src_hdr is always InvalidBlockNumber here (bm25_pending_score_term never
     * records a segment source), so a pending-only ranked row correctly gets
     * no key later (no KEYMAP entry yet -- matches the exhaustive path). */
    hash_seq_init(&seq, acc);
    while ((e = (BM25AccEnt *) hash_seq_search(&seq)) != NULL)
    {
        CHECK_FOR_INTERRUPTS();

        (void) bm25_topk_offer(heap, e->score, &e->key,
                               e->src_hdr, e->src_gen, e->src_docid);
    }

    hash_destroy(acc);
}

/* #268: copy the inputs this build scored under into CurrentMemoryContext (the
 * caller is in so->scanctx, so the pin lives as long as the ranking it describes).
 * Only the values a document's score reads are kept -- see BM25PinnedStats for why
 * live N and per-field N are not. idf is copied whole in the driver's own
 * [nq][BM25_MAX_FIELDS] layout, including the zero rows of inactive terms: a term
 * this build never scored must not start scoring in the tail either. */
static BM25PinnedStats *
bm25_wand_pin_stats(int nq, const double *idf, const double *avgdl_f,
                    const BM25FieldConfig *fcfg, uint32 field_count)
{
    BM25PinnedStats *pin;
    uint32           f;

    pin = (BM25PinnedStats *) palloc0(offsetof(BM25PinnedStats, idf) +
                                      sizeof(double) * BM25_MAX_FIELDS * (Size) Max(nq, 0));
    pin->field_count = field_count;
    pin->nq = nq;
    for (f = 0; f < field_count; f++)
    {
        pin->avgdl_f[f] = avgdl_f[f];
        pin->k1[f]      = fcfg[f].k1;
        pin->b[f]       = fcfg[f].b;
        pin->boost[f]   = fcfg[f].boost;
    }
    if (nq > 0)
        memcpy(pin->idf, idf, sizeof(double) * BM25_MAX_FIELDS * (Size) nq);
    return pin;
}

void
bm25_wand_build_ranking(Relation index, BM25ScanOpaque so,
                        const BM25ScanSnapshot *snap,
                        BM25Token *qtoks, int nq, uint64 live_ndocs,
                        const double *avgdl_f, const uint64 *fld_ndocs,
                        const BM25FieldConfig *fcfg, int32 qfield, int wand_top_k,
                        BM25WandStats *stats)
{
    MemoryContext scratchctx,
                  oldctx;
    BM25TopK     *heap;
    BM25WandCtx  *ctx;          /* [nq] per query term */
    double       *idf;          /* [nq * BM25_MAX_FIELDS] per-term per-field idf (for pending) */
    bool         *active;       /* [nq] */
    BM25TermSegLoc *locs;       /* [nq * nsegs] df-pass dictionary lookups */
    HTAB         *pending_tids;
    HASHCTL       ctl;
    BM25Scored   *arr;
    int           n,
                  i;
    uint32        si;
    uint8         km_type;      /* M5 key_field: index-wide key config (Task 9) */
    uint16        km_size;
    BM25SegKeyCache keycache;   /* issue #225: per-segment KEYMAP readers for the drain */
    BM25KeyReq   *kreqs = NULL; /* issue #246: rows' key sources, keyed index only */
    uint32        nkreq = 0;

    /* All transient WAND state lives in one scratch context, dropped on exit;
     * only the final ranked[]/scores[] (allocated in so->scanctx below) survive.
     * Parented under CurrentMemoryContext so a subtransaction rollback (the seam
     * retry wrapper, Task 9) reclaims it for free, mirroring _once. */
    scratchctx = AllocSetContextCreate(CurrentMemoryContext,
                                       "bm25 wand build",
                                       ALLOCSET_SMALL_SIZES);
    oldctx = MemoryContextSwitchTo(scratchctx);

    heap = bm25_topk_create(wand_top_k, scratchctx);

    /* Per-term idf + ctx (global df->idf, identical to the exhaustive scorer).
     *
     * Documented residual (#275): these are plain palloc, so an absurdly large query
     * fails with "invalid memory alloc request size" rather than running slowly. Two
     * corners, both far past any realistic query: ctx first, at sizeof(BM25WandCtx)
     * (1320 bytes on a 64-bit build) per query token, i.e. about 813K tokens; then
     * locs below, at sizeof(BM25TermSegLoc) (24 bytes) per (token, segment) pair, i.e.
     * about 44.7M pairs. Both are MaxAllocSize divided by the element size. */
    ctx    = (BM25WandCtx *) palloc(sizeof(BM25WandCtx) * Max(nq, 1));
    idf    = (double *) palloc(sizeof(double) * BM25_MAX_FIELDS * Max(nq, 1));
    active = (bool *) palloc(sizeof(bool) * Max(nq, 1));
    /* [nq][nsegs]: each term's df-pass dictionary lookups, handed to the segment
     * passes below so no dictionary is walked twice (issue #246). */
    locs   = (BM25TermSegLoc *) palloc(sizeof(BM25TermSegLoc) *
                                     Max((Size) nq * snap->nsegs, 1));
    bm25_wand_prepare_terms(index, snap, qtoks, nq, live_ndocs,
                            avgdl_f, fld_ndocs, fcfg, qfield, ctx, idf, active,
                            locs);

    /* TIDs pending scored: segment BMW dedupes against these (pending wins). Pending is
     * scored for every term before any segment, unlike the exhaustive builder's
     * term-major order; see bm25_scan_rank.c (#314 SCORE-03) for why the difference
     * cannot be observed. */
    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(ItemPointerData);
    ctl.entrysize = sizeof(ItemPointerData);    /* set: key only */
    ctl.hcxt      = scratchctx;
    pending_tids  = hash_create("bm25 wand pending tids", 256, &ctl,
                                HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

    /* (D6) pending arm FIRST: exhaustive, non-prunable, primes theta. */
    bm25_wand_score_pending(index, snap->pending_head, snap->next_gen,
                            qtoks, nq, snap->field_count,
                            idf, avgdl_f, fcfg, heap, pending_tids);

    /* (D5) per-segment BMW under the shared heap; theta rises across segments. */
    for (si = 0; si < snap->nsegs; si++)
    {
        CHECK_FOR_INTERRUPTS();

        bm25_wand_segment(index, &snap->segs[si], qtoks, nq, ctx, active,
                          locs + si, snap->nsegs, heap, pending_tids, stats);
    }

    /* Drain into final scan order (score desc, TID asc -- Task 5's comparator,
     * == scored_desc). arr holds up to k rows, each carrying (beyond tid/score)
     * the segment source of whichever posting last touched it -- exactly the
     * (src_hdr, src_gen, src_docid) triple the key lookup needs (InvalidBlockNumber
     * for a pending-only candidate). */
    /* QRY-10: sized by what the heap actually holds, not by wand_top_k. This was
     * the SECOND eager k-sized allocation per ranked scan (the heap's own array
     * was the first), and it fires even when the corpus has three matching rows. */
    arr = (BM25Scored *) palloc(sizeof(BM25Scored) *
                                Max(bm25_topk_count(heap), 1));
    n   = bm25_topk_drain_sorted(heap, arr);

    /* M2b Task 9: M5 key_field projection, the same discovery (one shared
     * function, bm25_ranked_key_config) + per-row resolve as
     * bm25_scan_build_ranking_exhaustive's finalize -- a keyed index must return the
     * SAME (key, score) rows regardless of which scorer produced the ranking
     * (BM25_KEY_NONE => keyless, ctid-only). Done BEFORE switching to so->scanctx so
     * the transient header read stays in this call's scratchctx. */
    bm25_ranked_key_config(index, snap, &km_type, &km_size);

    /* Ranked-row key readers (issue #225), built in scratchctx before the switch
     * below, and only for a keyed index, as in the exhaustive builder. */
    if (km_type != BM25_KEY_NONE)
    {
        bm25_seg_key_cache_init(&keycache, index, scratchctx);
        /* Rows' key sources, resolved by bm25_seg_key_cache_fill after the loop below
         * (issue #246); scratch, like the cache. */
        kreqs = (BM25KeyReq *) palloc(sizeof(BM25KeyReq) * Max(n, 1));
    }

    /* Persistent results in so->scanctx (outlive this call, across gettuple). */
    MemoryContextSwitchTo(so->scanctx);
    /* Drop any lazily-built score index: it describes the ranking being replaced.
     * Same reasoning as the exhaustive builder -- see the comment there. */
    so->score_by_tid = NULL;
    so->score_by_key = NULL;
    so->ranked = (BM25Posting *) palloc(sizeof(BM25Posting) * Max(n, 1));
    so->scores = (double *) palloc(sizeof(double) * Max(n, 1));
    if (km_type != BM25_KEY_NONE)
    {
        so->ranked_keys     = palloc0((Size) Max(n, 1) * km_size);
        /* Parallel presence flags -- see the exhaustive builder and the
         * ranked_key_present comment in bm25.h. A keyed index can rank rows whose
         * key this scan cannot resolve, and an unresolved slot's zero bytes are
         * indistinguishable from a real id = 0, so absence is recorded out of band. */
        so->ranked_key_present = (bool *) palloc0(sizeof(bool) * Max(n, 1));
        so->ranked_key_type = km_type;
        so->ranked_key_size = km_size;
    }
    else
    {
        so->ranked_keys     = NULL;
        so->ranked_key_present = NULL;
        so->ranked_key_type = BM25_KEY_NONE;
        so->ranked_key_size = 0;
    }
    for (i = 0; i < n; i++)
    {
        so->ranked[i].tid    = arr[i].tid;
        so->ranked[i].tf     = 0;       /* unused after ranking */
        so->ranked[i].doclen = 0;
        so->scores[i]        = arr[i].score;

        if (so->ranked_keys != NULL && arr[i].src_hdr != InvalidBlockNumber)
        {
            kreqs[nkreq].header_blkno = arr[i].src_hdr;
            kreqs[nkreq].gen          = arr[i].src_gen;
            kreqs[nkreq].local_docid  = arr[i].src_docid;
            kreqs[nkreq].slot         = (uint32) i;
            nkreq++;
        }
    }
    /* The same helper the exhaustive builder calls, so the two fill ranked_keys and
     * ranked_key_present identically by construction. They must: either can produce
     * the ranking a bm25_score_key() call reads, and the WAND over-pull tail rebuild
     * swaps one for the other mid-scan. */
    if (so->ranked_keys != NULL)
        bm25_seg_key_cache_fill(&keycache, kreqs, nkreq, so->ranked_keys,
                                so->ranked_key_present, km_size);
    so->nranked     = (uint32) n;
    so->rcur        = 0;
    so->wand_capped = (n == wand_top_k);
    /* Only a capped ranking can reach the over-pull tail rebuild, the pin's one
     * consumer; an uncapped one already holds every match. Allocated here, while
     * CurrentMemoryContext is so->scanctx. */
    so->stats_pin = so->wand_capped
        ? bm25_wand_pin_stats(nq, idf, avgdl_f, fcfg, snap->field_count)
        : NULL;

    /* Same pending backfill the exhaustive builder runs, and for the same reason: a
     * pending-only row has no segment to resolve its key from. BOTH builders must do
     * it -- the over-pull tail rebuild replaces a WAND ranking with an exhaustive
     * one mid-scan, so a fix in only one would be silently discarded the moment an
     * executor pulls past wand_top_k. Placed after nranked is assigned: the helper
     * walks ranked_key_present[0..nranked). */
    bm25_ranked_keys_fill_from_pending(index, so, snap->pending_head, snap->next_gen);

    MemoryContextSwitchTo(oldctx);
    MemoryContextDelete(scratchctx);
}
