/* bm25_score.c -- BM25 scoring math (Lucene/Tantivy "+1" IDF variant),
 * the &@@ order-by distance anchor, and the bm25_score(ctid) scan accessor.
 *
 * bm25_idf and bm25_termscore are pure (no PG state). bm25_idf is called by the
 * stats layer (bm25_stats.c) and the debug SQL function here; bm25_termscore by
 * every scorer (the exhaustive and WAND builders, the pending scorer and the debug
 * probes).
 * <math.h> is included for log(); link via -lm (PGXS adds it).
 *
 * bm25_distance is the SQL anchor for the &@@ ORDER BY operator.  It IS called
 * on an index-ordered scan: &@@ is projected per-row on the Index Scan node's
 * own target list (a resjunk column) right after each bm25_gettuple, and this
 * function returns the distance bm25_gettuple stashed on the scan opaque for
 * the tuple just returned. Which scan's stash is the whole question, and #138
 * is the record of getting it wrong: it used to read the registry HEAD, which
 * is the most recently REGISTERED scan and not the one that owns the row. It
 * now resolves by ORDER BY query identity -- see bm25_distance_for_query below,
 * which both this and bm25_distance_jsonb (bm25_handler.c) call. It returns
 * +infinity when no live scan ranked the projected query, which covers both the
 * fallback plans (seqscan / Sort, no per-row score exists) and a projection
 * naming a query nothing is ranking -- degrading gracefully instead of
 * returning some other scan's number.
 *
 * The active-scored-scan registry is defined here (one definition rule) and
 * declared extern in bm25.h.  bm25_scan.c registers a scan when gettuple
 * enters scoring mode; bm25_endscan deregisters it.  The score accessors
 * (bm25_score/bm25_score_key) resolve a projected row to its OWNING scan by
 * identity (R3): first the current-row fast path -- each scan stamps
 * cur_ranked_idx on emit, and the resolver finds the scan whose just-emitted row
 * matches -- so several concurrently-live scored scans (a correlated join /
 * nested subqueries) no longer clobber one another through a single shared slot.
 * Row identity alone cannot separate two scans whose CURRENT rows coincide; each
 * accessor call site also remembers the scan it has been projecting, which
 * overrides the head-first pick when both scans have just emitted (#242,
 * bm25_resolve_current_row below; its comment states the bounded contract). When no scan behind
 * the head can still emit a row (SCAN-04: a REGISTERED rival is not a live one --
 * a finished UNION ALL sibling lingers until ExecutorEnd), the fallback is an
 * O(1) probe into a per-scan TID/key dynahash
 * built lazily from that scan's ranking (bm25_build_score_index below), which
 * keeps decoupled projections correct (under ORDER BY ... LIMIT the target-list
 * projection and amgettuple are decoupled, so a single "last emitted" slot
 * alone would be stale). Each hash describes exactly ONE ranking, so it is
 * NULLed on rescan (bm25_rescan) and again wherever a ranking is republished --
 * bm25_scan_build_ranking_exhaustive and bm25_wand_build_ranking each drop it
 * at the point they assign so->ranked/scores -- and the next probe rebuilds it.
 *
 * The query-qualified overloads bm25_score(tid, query [, regclass]) and
 * bm25_score_key(key, query) (#253) take the scan's identity from the caller
 * instead: they pick the scans ranking that query and probe each one's hash. See
 * bm25_resolve_by_query for the contract and for why ambiguity there is NULL.
 *
 * #301: the registry is per backend, not per role. Every walker sees only the
 * scans registered under the caller's current user id, and the row-addressed
 * accessors (score, key, snippet) additionally require that the caller may read
 * the scan's heap. See bm25_owned_from and bm25_probe_acl_compute.
 */

#include "postgres.h"

#include "bm25.h"
#include <math.h>
#include "utils/float.h"
#include "utils/hsearch.h"
#include "access/heapam.h"    /* heap_get_root_tuples -- HOT-chain root resolution */
#include "storage/bufmgr.h"
#include "utils/rel.h"
/* #301: the row-addressed accessors' privilege check (bm25_probe_acl_compute). */
#include "access/sysattr.h"
#include "catalog/partition.h"    /* get_partition_ancestors */
#include "utils/lsyscache.h"     /* get_rel_relispartition */
#include "optimizer/optimizer.h"
#include "utils/acl.h"
#include "utils/rls.h"

/* -------------------------------------------------------------------------
 * Active scored-scan registry. Backend-local intrusive singly-linked list of
 * the scored scans currently able to emit rows. Head = most-recently
 * registered. N = query nesting depth (typically 1-2), so linear walks are
 * cheap. Replaces the old single bm25_active_scored_scan pointer.
 * -------------------------------------------------------------------------*/
static BM25ScanOpaque bm25_scored_scans = NULL;

void
bm25_register_scored_scan(BM25ScanOpaque so, Relation index)
{
    /* Idempotent: if already registered, move to head (a rescanned inner scan
     * re-loads and re-registers; it must not appear twice). */
    bm25_deregister_scored_scan(so);
    /* #301: the owner is whoever is running the query now. A rescan re-registers,
     * so the owner follows the user id of the latest (re)load. */
    so->owner_userid = GetUserId();
    so->indexrel = index;
    so->next_active = bm25_scored_scans;
    bm25_scored_scans = so;
}

/*
 * #301: OWNERSHIP. The registry is backend-local and every accessor is executable by
 * PUBLIC, so without a filter, code running as one role can probe the ranking of a scan
 * another role started in the same backend. The reachable shape is a SECURITY DEFINER
 * function whose ranked scan is still open while the caller's own expressions run: a
 * LANGUAGE sql set-returning function in the caller's target list (ProjectSet calls it
 * once per row, so the definer's executor and scan stay live between rows), or a
 * definer-opened refcursor. The rows the definer filters out above the index scan are
 * still in its ranking, so the query-qualified accessors were an exact per-document
 * term oracle for them.
 *
 * Every walker below therefore sees only the scans whose owner_userid is the caller's
 * current user id, as if the others were not registered. That alone closes the SQL SRF
 * shape: each call into the definer runs under the definer's user id, so its scan
 * registers under it, while the caller's accessors run under the caller's. It does not
 * close the refcursor shape -- the scan there begins at the caller's first FETCH and
 * registers under the CALLER's id -- which is why the row-addressed accessors also
 * require read access to the scan's heap (bm25_probe_allowed below).
 *
 * The distance projection gets this filter and nothing more; see
 * bm25_distance_for_query. A role change between two FETCHes of one cursor makes the
 * later rows' accessors NULL (+infinity for the distance), because the scan stays owned
 * by the role that fetched first.
 */
static inline BM25ScanOpaque
bm25_owned_from(BM25ScanOpaque s)
{
    Oid         me = GetUserId();

    while (s != NULL && s->owner_userid != me)
        s = s->next_active;
    return s;
}

/* The next scan after s that the caller owns. Every registry walk filters through
 * bm25_owned_from, directly (this, from bm25_scored_scan_head(): distance and
 * by_serial) or via bm25_visible_from / bm25_next_visible (the probe accessors and
 * the snippet, which also apply the privilege check); only (de)registration walks
 * the raw list. */
#define bm25_next_owned(s)  bm25_owned_from((s)->next_active)

/* #301: the row-addressed accessors' privilege check and its per-call-site cache,
 * defined further down with their rationale; declared here for the walkers. */
typedef struct BM25ProbeFnCache BM25ProbeFnCache;
static BM25ProbeFnCache *bm25_probe_fncache(FunctionCallInfo fcinfo);
static bool bm25_probe_allowed(BM25ProbeFnCache *fc, BM25ScanOpaque s);

/* The first scan from s that a row-addressed accessor may see: owned by the caller
 * and passing the privilege check. A refused scan is skipped exactly like another
 * role's, so nothing in it can influence an answer. */
static BM25ScanOpaque
bm25_visible_from(BM25ProbeFnCache *fc, BM25ScanOpaque s)
{
    for (s = bm25_owned_from(s); s != NULL; s = bm25_next_owned(s))
        if (bm25_probe_allowed(fc, s))
            return s;
    return NULL;
}

#define bm25_next_visible(fc, s)  bm25_visible_from((fc), (s)->next_active)

void
bm25_deregister_scored_scan(BM25ScanOpaque so)
{
    BM25ScanOpaque *pp = &bm25_scored_scans;
    while (*pp != NULL)
    {
        if (*pp == so)
        {
            *pp = so->next_active;
            so->next_active = NULL;
            return;
        }
        pp = &(*pp)->next_active;
    }
}

/* The most recently registered scan the caller owns (#301), or NULL. */
BM25ScanOpaque
bm25_scored_scan_head(void)
{
    return bm25_owned_from(bm25_scored_scans);
}

/* Could this scan still produce a FUTURE row? rcur < nranked means the ranking is
 * not yet fully iterated; wand_capped && !wand_tail_done means bm25_gettuple's
 * over-pull fallback has yet to rebuild the exhaustive tail, so rcur reaching the
 * CAPPED nranked does not mean done. A scan failing both has either already
 * returned false from gettuple or is guaranteed to on its next call, so it can
 * never again own a projected row.
 *
 * Extracted from bm25_sole_scored_scan (SCAN-04) so the two score resolvers can
 * apply the same test instead of a raw registry count -- see
 * bm25_sole_live_scored_scan. */
static inline bool
bm25_scan_can_emit_more(BM25ScanOpaque s)
{
    return s->rcur < s->nranked || (s->wand_capped && !s->wand_tail_done);
}

/* The head, if no scan BEHIND it can still emit; NULL otherwise (including an
 * empty registry). The non-erroring core of bm25_sole_scored_scan. #301: over the
 * scans the caller may see -- a refused scan is neither the head nor a rival, so
 * whether it could still emit (which depends on what it ranked) cannot turn the
 * caller's own answer into NULL. */
static BM25ScanOpaque
bm25_sole_live_scored_scan(BM25ProbeFnCache *fc)
{
    BM25ScanOpaque head = bm25_visible_from(fc, bm25_scored_scans);
    BM25ScanOpaque s;

    if (head == NULL)
        return NULL;

    for (s = bm25_next_visible(fc, head); s != NULL; s = bm25_next_visible(fc, s))
        if (bm25_scan_can_emit_more(s))
            return NULL;
    return head;
}

/*
 * bm25_sole_scored_scan -- the one LIVE scored scan, or NULL if none registered.
 * ERRORs if a scan behind the head could still be interleaved with it. Used by
 * bm25_snippet: it carries no row identity, so under concurrent scored scans it
 * cannot know which scan owns the row being projected -- fail loud rather than
 * return a plausible wrong excerpt. (The &@@ distance is deliberately NOT
 * routed through this: bm25_distance doubles as the ORDER BY resjunk on every
 * ranked query, so it must not error. It resolves by ORDER BY query identity
 * instead -- bm25_distance_for_query -- which is an identity bm25_snippet does
 * not have, since nothing in its arguments names the query.)
 *
 * Ambiguity is NOT simply "more than one scan is registered": a finished scan
 * lingers in the registry until its bm25_endscan runs, which for a UNION ALL
 * sibling (or any other Append/SubqueryScan child) only fires at the query's
 * ExecutorEnd -- long after that sibling returned its last row. Counting a
 * merely-not-yet-torn-down scan as a rival would make bm25_snippet spuriously
 * ambiguous on every UNION ALL of ranked snippet queries (39_snippet Part 4 is
 * exactly this shape: six independent single-row-corpus scans, each still
 * registered when the next one runs). So instead of a raw count, walk the
 * registry BEHIND the head and flag only a scan that could still produce a
 * FUTURE row: rcur < nranked (ranking not yet fully iterated), or WAND-capped
 * with the exhaustive tail not yet rebuilt (bm25_gettuple's own over-pull
 * fallback -- rcur reaching the capped nranked there does not mean done). A
 * scan failing both tests has either already returned false from gettuple, or
 * is guaranteed to on its very next call, so it can never again own a
 * projected row and is safe to ignore. This mirrors bm25_gettuple's own
 * "return false" condition (src/bm25_scan.c) without adding new scan state.
 *
 * #301: head and rivals are drawn from the scans the caller may see (owned, and
 * passing the privilege check), like bm25_sole_live_scored_scan. When there is no
 * such scan, *refused is set to a scan the caller owns but may not read, if there
 * is one, so bm25_snippet can raise instead of returning the "no scan" NULL. That
 * depends only on the refused scan existing, never on its ranking.
 */
BM25ScanOpaque
bm25_sole_scored_scan(const char *fn, FunctionCallInfo fcinfo, BM25ScanOpaque *refused)
{
    BM25ProbeFnCache *fc = bm25_probe_fncache(fcinfo);
    BM25ScanOpaque head = bm25_visible_from(fc, bm25_scored_scans);
    BM25ScanOpaque s;

    *refused = NULL;
    if (head == NULL)
    {
        *refused = bm25_scored_scan_head();     /* any owned scan here is refused */
        return NULL;
    }

    for (s = bm25_next_visible(fc, head); s != NULL; s = bm25_next_visible(fc, s))
        if (bm25_scan_can_emit_more(s))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bm25: %s is ambiguous with multiple active bm25 scored scans in one query", fn),
                     errhint("Project %s in a query with a single bm25 ORDER BY ... &@@ ... scan; "
                             "for concurrent scored scans use bm25_score(ctid)/bm25_score_key(id), "
                             "which resolve by row identity.", fn)));
    return head;
}

/*
 * Is `s` a positioned scored scan ranking exactly this query? The query-identity
 * rule of ADR 0061, shared by bm25_distance_for_query and the query-qualified score
 * accessors (#253, bm25_resolve_by_query below) so the two cannot drift apart.
 */
static inline bool
bm25_scan_ranks_query(BM25ScanOpaque s, bool is_jsonb, const char *rhs, int rhslen)
{
    if (!s->scoring || s->orderby_rhs == NULL)
        return false;
    /* Never let the (text,text) and (text,jsonb) families cross-match on a
     * coincidental byte sequence. */
    if (s->orderby_rhs_jsonb != is_jsonb)
        return false;
    /* Not positioned => it has emitted no row and cannot own this one. */
    if (s->cur_ranked_idx == BM25_NO_CUR)
        return false;
    if (s->orderby_rhs_len != rhslen)
        return false;
    if (rhslen > 0 && memcmp(s->orderby_rhs, rhs, rhslen) != 0)
        return false;
    return true;
}

/*
 * bm25_distance_for_query -- resolve a &@@ projection to its OWNING scan (#138).
 *
 * WHY NOT THE REGISTRY HEAD, which is what this used to read. The head is the
 * most recently REGISTERED scan, and registration happens at load. Under a
 * correlated subquery the inner scan loads (and so registers) DURING the outer
 * row's projection, so by the time the outer's resjunk `&@@` is evaluated the
 * head is the inner scan and the outer row is projected with the inner's
 * distance. Measured: every outer row got one constant value, and with a
 * secondary sort key on that value the BM25 ranking collapsed to the tiebreak.
 *
 * WHY NOT RECENCY-ON-EMIT, the other obvious repair. Re-heading whichever scan
 * emitted last does not help: in that same shape the inner IS the most recent
 * emitter. ADR 0007 rejected recency for exactly this reason before the bug was
 * observed -- "the CORRELATED inner subquery is genuinely the innermost /
 * most-recently-active scan by construction, yet it is the OUTER's score that
 * the target list needs."
 *
 * WHY NOT ROW IDENTITY, which is how bm25_score resolves. That works because
 * bm25_score is handed a ctid. This function's arguments are (document value,
 * query); fmgr gives a scalar function no handle on the slot being projected,
 * and threading one in would mean new operator signatures.
 *
 * So: resolve by the one identity this function DOES receive -- the query. Each
 * scoring scan stashes its ORDER BY RHS verbatim; the projected expression
 * carries the same RHS; matching them picks the owning scan. A candidate must
 * also be POSITIONED (cur_ranked_idx != BM25_NO_CUR), which is what stops an
 * inner scan that matched nothing from projecting its +inf initializer onto
 * every outer row.
 *
 * SAME-QUERY SIBLINGS (#252): several positioned scans ranking the byte-identical
 * query, which is every ranked query on a partitioned or inheritance parent -- a
 * Merge Append over one index scan per child, all with the same RHS. The query
 * cannot separate them, so among those candidates pick the MOST RECENT EMITTER
 * (highest last_emit_seq, stamped on every scored emit). The &@@ resjunk is
 * projected on the Index Scan's own target list immediately after that scan's
 * gettuple, with no other scan emitting in between, so the most recent emitter
 * is the scan that owns the row. The old rule -- first match from the registry
 * head, i.e. most recently REGISTERED -- resolved every child after the last to
 * load to that last child's current row, so Merge Append saw a constant key for
 * the others and drained them out of score order.
 *
 * That is not the recency ADR 0007 rejected. It is applied only after the query
 * match, so it never chooses between scans ranking different queries (the #138
 * correlated shape resolves before it is reached). For the same query nested
 * inside itself the two rules agree: a correlated inner re-registers on each
 * rescan and then emits, so it is both the newest registration and the newest
 * emitter -- right for the inner's own &@@, and the residual below for the
 * outer's.
 *
 * Residual, deliberate: a projection NOT evaluated right after its own scan's
 * emit -- a join's target list evaluating two same-query scans' &@@ after both
 * emitted, or a same-query subquery run from the target list ahead of the
 * resjunk -- gets the newest emitter, which need not be the owner. Nothing in the
 * operator's signature can separate them. Unlike bm25_snippet this must not
 * ERROR -- it is the resjunk projection of every ranked query -- and with no
 * matching scan it degrades to +infinity, the same answer the off-index
 * (seqscan) path gives.
 *
 * OWNERSHIP ONLY, NO PRIVILEGE CHECK (#301). Only scans the caller owns are
 * candidates, which hides a SECURITY DEFINER function's scan from the caller's own
 * &@@ projections. The SELECT and row-level-security checks the row-addressed
 * accessors make are deliberately not made here. This is the ORDER BY key: Merge
 * Append merges partitions on it, and a role ranking through a view it was granted,
 * without SELECT on the base table, runs the scan under its own user id and still
 * needs a real number. A refusal would degrade every such row to +infinity and
 * silently unorder the result. What the check would protect is small: the value is
 * the score of whichever row the caller's own scan emitted last, and the caller
 * cannot choose that row.
 */
float8
bm25_distance_for_query(bool is_jsonb, const char *rhs, int rhslen, bool *owned)
{
    BM25ScanOpaque s;
    BM25ScanOpaque best = NULL;

    if (owned != NULL)
        *owned = false;

    for (s = bm25_scored_scan_head(); s != NULL; s = bm25_next_owned(s))
    {
        if (!bm25_scan_ranks_query(s, is_jsonb, rhs, rhslen))
            continue;
        /* Strict >: equal stamps are impossible for two positioned scans (the
         * sequence is backend-wide and pre-incremented), so a sole candidate is
         * returned exactly as before. */
        if (best == NULL || s->last_emit_seq > best->last_emit_seq)
            best = s;
    }
    if (best != NULL)
    {
        if (owned != NULL)
            *owned = true;
        return best->cur_orderby_dist;
    }
    /* No live scan ranked this query: no per-row score exists. Degrade to maximum
     * distance / minimum relevance rather than erroring.
     *
     * TEXT-01 (#151) proposed erroring here "like the jsonb sibling does". Two reasons
     * that does not hold, recorded because the comparison is an easy one to re-make:
     *
     * 1. THE SIBLING IS THIS FUNCTION. bm25_distance's jsonb counterpart is
     *    bm25_distance_jsonb, and it calls straight into this resolver -- the two are
     *    the same body, +inf included, deliberately shared so neither can drift. The
     *    finding compared &@@ (an ORDER BY anchor, which must not error: it is projected
     *    as a resjunk column on every ranked query) against bm25_match_jsonb, which is
     *    the @@@ FILTER and errors because a jsonb query tree genuinely cannot be
     *    evaluated without the index. Different operators, different obligations.
     *
     * 2. BOTH FALL-THROUGH CASES ARE PINNED, as decisions rather than accidents.
     *    "No scan registered at all" is sql/50_orderby_dist's seqscan degradation.
     *    "Scans exist but none owns this query" is sql/87_distance_scan_identity, whose
     *    comment marks it a DELIBERATE BEHAVIOUR CHANGE -- +inf REPLACED returning some
     *    other query's finite distance, i.e. it is the fix for an earlier bug, not one.
     *    A "narrow" fail-loud variant that errors on only one of the two has no target
     *    left once both are read.
     *
     * What is genuinely unsatisfying, and is SQL-12's territory rather than this
     * function's: a user who sets enable_seqscan = off can still get a seqscan plan and
     * therefore a silently unranked result. The remedy for that is the planner and the
     * documentation, not an error thrown from an ORDER BY projection.
     *
     * #245 narrows "must not error" without contradicting it: a VALID query still
     * gets +inf here, but the jsonb caller sees *owned == false and validates the
     * tree, because on this path no scan ever parsed it and an invalid tree would
     * otherwise be accepted off the index while the index path rejects it. That is
     * an error about the QUERY, not the ranking-degrade error TEXT-01 proposed. */
    return get_float8_infinity();
}

/* -------------------------------------------------------------------------
 * BM25 math
 * -------------------------------------------------------------------------*/

/*
 * bm25_idf -- Lucene "+1" inverse-document-frequency, over a df clamped to ndocs.
 *
 * The formula is non-negative on its own whenever df <= N:
 *   (N - df + 0.5) / (df + 0.5) >= 0  when df <= N
 *   => ln(1 + x) >= 0 for x >= 0.
 *
 * That precondition used to be documented as "guaranteed by caller" and was NOT.
 * The single-field scan path deliberately sums the RAW dictionary df across
 * segments (df is not decremented on delete -- see the M3-identity note in
 * bm25_scan.c), while the denominator live_ndocs IS live (bm25_seg_read.c
 * decrements meta->ndocs on every tombstone). Delete enough of a term's
 * documents without an intervening merge and df > N, making the log argument
 * < 1 and the idf negative.
 *
 * A negative idf is not merely an odd score: it INVERTS bm25_termscore's
 * monotonicity in tf and doclen, which is the entire basis of the WAND
 * block-max bound (bm25_wand.c). Evaluating the bound at (max_tf, min_doclen)
 * then yields the block MINIMUM, so the "upper" bound sits below real scores
 * and WAND prunes documents the exhaustive scorer keeps -- a silent wrong answer
 * that also breaks the bit-identical WAND == exhaustive contract.
 *
 * The repair is applied to df, NOT to the result. Clamping the RESULT to 0 looks
 * equivalent and is not: `idf_f[field] == 0.0` is a live in-band SENTINEL meaning
 * "term absent from this field" (the C4 field-scope gate pre-zeroes out-of-scope
 * fields, and seg_posting_cb / bm25_block_ub / bm25_wand_cursor_score_doc all
 * skip on it). A present term whose idf clamped to exactly 0.0 would be read as
 * absent, contribute nothing, and drop out of the ranking entirely -- turning a
 * mis-ordered result into an EMPTY one.
 *
 * Clamping df instead says the honest thing: "more documents contain this term
 * than exist" means all of them do. It yields log(1 + 0.5/(N+0.5)) -- strictly
 * positive for every representable N (it underflows to 0.0 only above ~2^1023,
 * far past uint64), so the sentinel keeps its meaning, monotonicity holds, and
 * the exhaustive scorer and the WAND bound stay bit-identical by construction
 * because both read this one function.
 */
double
bm25_idf(uint64 ndocs, uint64 df)
{
    double df_eff = Min((double) df, (double) ndocs);

    return log(1.0 + ((double) ndocs - df_eff + 0.5) /
                     (df_eff + 0.5));
}

/*
 * bm25_termscore -- BM25 score for a single term occurrence.
 *
 * The length-normalization denominator uses avgdl; if avgdl is zero (empty
 * corpus edge case) the length factor degenerates to 1 (no normalization).
 */
double
bm25_termscore(double idf, uint32 tf, uint32 doclen,
               double avgdl, double k1, double b)
{
    double norm  = (avgdl > 0.0) ? ((double) doclen / avgdl) : 1.0;
    double denom = (double) tf + k1 * (1.0 - b + b * norm);
    return idf * ((double) tf * (k1 + 1.0)) / denom;
}

/*
 * bm25_debug_score -- SQL-callable math probe for regression tests.
 *
 * Args: N bigint, df int, tf int, doclen int, avgdl float8, k1 float8, b float8
 * Returns the raw BM25 score as float8.  Round to 6 decimals in SQL to get
 * a stable expected output that is independent of platform float formatting.
 */
PG_FUNCTION_INFO_V1(bm25_debug_score);
Datum
bm25_debug_score(PG_FUNCTION_ARGS)
{
    int64  N_arg  = PG_GETARG_INT64(0);
    int32  df_arg = PG_GETARG_INT32(1);
    int32  tf_arg = PG_GETARG_INT32(2);
    int32  dl_arg = PG_GETARG_INT32(3);
    double avgdl = PG_GETARG_FLOAT8(4);
    double k1    = PG_GETARG_FLOAT8(5);
    double b     = PG_GETARG_FLOAT8(6);
    uint64 N;
    uint32 df,
           tf,
           dl;
    double idf;

    /* Issue #313 SCORE-08: the counts are unsigned in the scorer, and a negative SQL
     * argument cast straight across became a huge value -- (-1, 1, 1, 1, ...) returned
     * 69.57 -- so the probe "verified" the formula on inputs production never
     * produces. Refused for the same reason as the k1/b/avgdl checks below. */
    if (N_arg < 0 || df_arg < 0 || tf_arg < 0 || dl_arg < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_score: N, df, tf and doclen must be >= 0 "
                        "(got " INT64_FORMAT ", %d, %d, %d)",
                        N_arg, df_arg, tf_arg, dl_arg)));
    N  = (uint64) N_arg;
    df = (uint32) df_arg;
    tf = (uint32) tf_arg;
    dl = (uint32) dl_arg;

    /* Apply the real scorer's preconditions (TEXT-08).
     *
     * bm25_termscore has no guards of its own -- correctly, since on the query path k1
     * and b arrive from reloptions that bm25_build.c already validated to k1 >= 0 and
     * b in [0, 1]. This probe took them straight from SQL arguments, so it was the one
     * caller able to violate the contract, and it did so silently: with tf = 0 and
     * k1 = 0 the denominator `tf + k1 * (1 - b + b*norm)` is exactly 0 and the function
     * returns NaN. A probe whose whole purpose is to let a suite check the scoring math
     * must not be reachable with inputs the scorer is not defined for, or a test can
     * "verify" a formula against values production can never produce.
     *
     * The bounds are the reloption validator's, restated rather than shared because
     * that one parses reloption strings and reports per-field names. avgdl >= 0 matches
     * bm25_termscore's own `avgdl > 0.0` fallback; a negative avgdl would invert the
     * length normalization. Non-finite inputs are rejected outright -- SQL float8
     * accepts 'NaN' and 'Infinity' as literals, and either turns every downstream
     * comparison into a silent false. */
    if (!isfinite(avgdl) || !isfinite(k1) || !isfinite(b))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_score: avgdl, k1 and b must be finite")));
    if (k1 < 0.0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_score: k1 must be >= 0 (got %g)", k1)));
    if (b < 0.0 || b > 1.0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_score: b must be in [0, 1] (got %g)", b)));
    if (avgdl < 0.0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_score: avgdl must be >= 0 (got %g)", avgdl)));

    /* The remaining hole is arithmetic rather than a range: the denominator can be
     * exactly zero for inputs that pass every check above, and 0/0 is NaN.
     *
     * COMPUTE IT AND TEST IT, rather than enumerating the input combinations that
     * produce it. An earlier version of this guard rejected `tf == 0 && k1 == 0` and
     * asserted in its own comment that this was the only case. It was not:
     * tf = 0, k1 > 0, b = 1, doclen = 0 also gives zero, because
     * (1 - b + b*norm) collapses to 0 when b == 1 and norm == 0 -- and b == 1 is
     * deliberately inclusive, so nothing else was going to catch it. Adversarial review
     * found it with bm25_debug_score(100, 10, 0, 0, 40.0, 1.2, 1.0).
     *
     * The full condition is tf == 0 AND (k1 == 0 OR (b == 1 AND doclen == 0 AND
     * avgdl > 0)), which is exactly the sort of compound predicate that acquires a
     * fourth clause nobody notices. Mirroring bm25_termscore's own arithmetic cannot
     * drift from it: if the scorer's formula changes, this changes with it. */
    {
        double norm  = (avgdl > 0.0) ? ((double) dl / avgdl) : 1.0;
        double denom = (double) tf + k1 * (1.0 - b + b * norm);

        if (denom == 0.0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25_debug_score: these arguments give a zero BM25 denominator"),
                     errdetail("The denominator tf + k1*(1 - b + b*doclen/avgdl) is exactly 0, so the score "
                               "is 0/0. A term with tf = 0 has no posting, so the real "
                               "scorer is never called this way.")));
    }

    idf = bm25_idf(N, df);
    PG_RETURN_FLOAT8(bm25_termscore(idf, tf, dl, avgdl, k1, b));
}

/* -------------------------------------------------------------------------
 * Order-by operator anchor
 *
 * Not a stub: bm25_distance below IS invoked on an index-ordered scan (see its
 * header, and the ranking-collapse bug that believing otherwise caused).
 * -------------------------------------------------------------------------*/

/*
 * bm25_distance -- SQL anchor for the &@@ operator (ORDER BY strategy 2).
 *
 * This function IS invoked on an index-ordered scan, and NOT gated by
 * xs_recheckorderby: PG's IndexNextWithReorder only consults that flag to
 * decide whether to re-verify the ORDER BY value it already has, but &@@
 * also appears as a plain resjunk column on the Index Scan node's own target
 * list whenever the plan needs the value materialized (e.g. a secondary sort
 * key, or the column is selected directly) -- that projection evaluates this
 * function per row, independent of xs_recheckorderby. (An earlier version of
 * this comment claimed the function was never invoked on the index path;
 * that was wrong, and returning a constant +inf there made any secondary
 * ORDER BY key collapse the BM25 ranking to that key.)
 *
 * WHICH scan's stash to return is the hard part, and this comment used to give
 * the answer that was wrong: "bm25_scored_scan_head() is non-NULL and
 * so->cur_orderby_dist holds the (-score) of the tuple bm25_gettuple most
 * recently returned". The head is the most recently REGISTERED scan, not the
 * one that owns the row -- see bm25_distance_for_query above, which this now
 * calls, and ADR 0061. Argument 1 is the query the caller wrote after `&@@`,
 * and that is the identity the resolver matches on.
 *
 * +infinity is returned when no live scan ranked this query: both the fallback
 * plans (seqscan, or an explicit Sort with the index unordered), where no
 * per-row corpus stats exist, and a projection naming a query nothing is
 * ranking. Degrading gracefully rather than crashing, and rather than handing
 * back some other scan's number.
 */
PG_FUNCTION_INFO_V1(bm25_distance);
Datum
bm25_distance(PG_FUNCTION_ARGS)
{
    text *q = PG_GETARG_TEXT_PP(1);     /* the query the caller wrote after &@@ */

    /* Text RHS: a text query has no tree to be malformed, so the fall-through
     * needs no validation and the owned flag is not asked for. */
    PG_RETURN_FLOAT8(bm25_distance_for_query(false, VARDATA_ANY(q),
                                             VARSIZE_ANY_EXHDR(q), NULL));
}

/* -------------------------------------------------------------------------
 * Score accessor
 * -------------------------------------------------------------------------*/

/*
 * BM25TidScoreEntry -- dynahash entry for score_by_tid (real struct, matching
 * the AccumHashEntry convention in bm25_accum.c, rather than raw offset math).
 * dynahash MAXALIGNs the entry's start address, but a raw `char *e` buffer
 * with the double stashed at the un-padded `sizeof(ItemPointerData)` == 6
 * offset would land it on a 6-mod-8 boundary -- misaligned, and UB on
 * strict-alignment targets (SIGBUS; also trips -fsanitize=alignment). Declaring
 * a real struct lets the compiler pad `score` to its natural 8-byte boundary.
 */
typedef struct BM25TidScoreEntry
{
    ItemPointerData tid;    /* hash key: ctl.keysize stays sizeof(ItemPointerData) */
    double          score;
} BM25TidScoreEntry;

/*
 * bm25_build_score_index -- build so->score_by_tid (+ score_by_key when keyed)
 * from the already-filled ranked/scores/ranked_keys arrays, in so->scanctx so
 * they share the ranking's lifetime (freed on rescan/endscan). Idempotent per
 * build: called lazily from the fallback probes, once per scan-load.
 *
 * TEXT-10: the idempotence guard lives HERE, per hash, and not at the call sites.
 * It used to be the callers' job -- bm25_resolve_score_tid entered on
 * `score_by_tid == NULL`, bm25_resolve_score_key on `score_by_key == NULL` -- while
 * the builder unconditionally hash_create'd BOTH. That is idempotent only if every
 * caller happens to guard on the same member, so the "Idempotent per build" claim
 * above was a property of the CALL GRAPH rather than of this function.
 *
 * NO ORPHANING WAS REACHABLE, and saying otherwise would be the more satisfying
 * story rather than the true one. The pair is only ever (NULL, NULL),
 * (built, built), or (built, NULL); from (built, built) the old key guard is false,
 * and (built, NULL) arises only when ranked_keys was NULL at build time -- in which
 * case bm25_resolve_score_key returns false before it ever reaches its guard. The
 * three reset sites null both members together, so ranked_keys cannot go NULL to
 * non-NULL underneath a surviving score_by_tid.
 *
 * This is hardening, then, not a leak fix: it makes the idempotence a property of
 * the function, so a future third caller guarding on a different member cannot
 * make the builder re-create a live O(nranked) hash and strand the old one until
 * endscan. Both call sites call unconditionally now.
 *
 * The key hash additionally requires ranked_keys != NULL, so an unkeyed scan that
 * reaches here leaves score_by_key NULL and a later keyed probe on the same scan
 * builds it then -- which is exactly the ordering the old call-site guards handled
 * correctly and the reason the second condition is a conjunction, not a nested if.
 *
 * Snapshots the ranking at first-probe time: it assumes so->ranked/scores/
 * ranked_keys are already at their FINAL nranked (fully loaded) -- normal
 * projection ordering guarantees bm25_gettuple has built the complete ranking
 * before any accessor can be called against it, so there is no partial-build
 * race to guard against here.
 *
 * PUBLISH AFTER FILL (issue #313 XCUT-14). Each hash is built in a local and assigned
 * to so-> only once its loop completes, because the `== NULL` guards above read a
 * published hash as "built". HASH_ENTER can raise out of memory mid-loop; if a caller
 * catches that (a PL/pgSQL EXCEPTION block) while the scan survives, a hash published
 * before the fill would stay half built and later probes would answer NULL for the
 * rows it never reached. Built late, the next probe simply rebuilds; the abandoned
 * partial hash lives in scanctx and goes at rescan or endscan.
 */
static void
bm25_build_score_index(BM25ScanOpaque so)
{
    MemoryContext oldctx = MemoryContextSwitchTo(so->scanctx);
    HASHCTL ctl;
    HTAB   *h;
    uint32  i;

    if (so->score_by_tid == NULL)
    {
        memset(&ctl, 0, sizeof(ctl));
        ctl.keysize   = sizeof(ItemPointerData);
        ctl.entrysize = sizeof(BM25TidScoreEntry);
        ctl.hcxt      = so->scanctx;
        h = hash_create("bm25 score-by-tid",
                        Max(so->nranked, 1), &ctl,
                        HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
        for (i = 0; i < so->nranked; i++)
        {
            bool found;
            BM25TidScoreEntry *e;

            /* #290: a WHERE row the ORDER BY query did not match has no score, so
             * it is not entered: a probe for it misses and answers NULL. (The key
             * hash below needs no such test: those rows are never key-present.) */
            if (bm25_score_is_unmatched(so->scores[i]))
                continue;
            e = (BM25TidScoreEntry *)
                hash_search(h, &so->ranked[i].tid, HASH_ENTER, &found);
            /* First writer wins, for symmetry with the key-hash loop below (and
             * because the ranking is score-descending, so the first occurrence is
             * the highest-scored one). TIDs are deduplicated by construction today
             * so this can't currently diverge, but a last-writer-wins asymmetry
             * between the two loops is a latent trap. */
            if (!found)
                e->score = so->scores[i];
        }
        so->score_by_tid = h;
    }
    if (so->ranked_keys != NULL && so->score_by_key == NULL)
    {
        /* Variable-width key: a fixed struct won't work, so pad the offset by
         * hand. voff is MAXALIGNed (a multiple of the same alignment dynahash
         * gives the entry's start), so e+voff is aligned for the double. The
         * entry is [key | pad][double score][bool conflict]; see the loop. */
        Size voff = MAXALIGN(so->ranked_key_size);

        memset(&ctl, 0, sizeof(ctl));
        ctl.keysize   = so->ranked_key_size;
        ctl.entrysize = voff + sizeof(double) + sizeof(bool);
        ctl.hcxt      = so->scanctx;
        h = hash_create("bm25 score-by-key",
                        Max(so->nranked, 1), &ctl,
                        HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
        for (i = 0; i < so->nranked; i++)
        {
            bool found;
            char *e;

            /* A row whose key this scan could not resolve must not be ENTERED under
             * one. Its slot is all-zero, so entering it would claim the key that
             * int4/int8 0 encodes to and answer a bm25_score_key(0) probe with an
             * unrelated row's score. ranked_key_present is non-NULL whenever
             * ranked_keys is (allocated and nulled together), so no NULL check
             * is needed here. */
            if (!so->ranked_key_present[i])
                continue;

            e = hash_search(h,
                            so->ranked_keys + (Size) i * so->ranked_key_size,
                            HASH_ENTER, &found);
            /* First writer wins on a duplicate key: the ranking is sorted
             * score-descending, so the first occurrence is the highest-scored
             * one, which is the score the one-argument fallback reports for a
             * duplicate key.
             *
             * A duplicate that scores differently also sets the conflict flag
             * (#253). Two ranked rows sharing a key -- a non-unique key_field, or
             * text keys that agree in their first BM25_KEY_MAX_SIZE bytes -- are
             * different rows, and the key cannot say which one is being projected.
             * The query-qualified accessor answers NULL there rather than report
             * one row's score for the other; the one-argument path ignores the flag
             * and keeps its first-writer answer. dynahash does not zero a new
             * entry, so the flag is written on first entry too. */
            if (!found)
            {
                *(double *) (e + voff) = so->scores[i];
                *(bool *) (e + voff + sizeof(double)) = false;
            }
            else if (*(double *) (e + voff) != so->scores[i])
                *(bool *) (e + voff + sizeof(double)) = true;
        }
        so->score_by_key = h;
    }
    MemoryContextSwitchTo(oldctx);
}

/*
 * Resolve the BM25 score for a projected row, keyed by heap TID. Returns true
 * (and sets *score) if an active scored scan owns the row, else false -> NULL.
 *
 * Step 1 (O(1) hot path): the scan whose just-emitted row == t, chosen by
 * bm25_resolve_current_row. This used to walk from the head and take the first
 * match, and the head is the most recently (re-)registered scan: when two
 * concurrent scans' CURRENT rows shared a TID (a correlated subquery ranking the
 * same index, whose top row is also an outer row), a rescanned inner won and the
 * outer's projection got the inner's score -- a wrong number, which sql/53 only
 * failed to notice under a stats-dependent plan (#242). That walk is still the
 * answer, except where call-site binding can prove a different owner; see there.
 * Step 2 (fallback): decoupled projection / arbitrary-id lookup, when no scan
 * BEHIND the head can still emit a row (SCAN-04 -- a merely REGISTERED scan is not
 * a rival; a finished UNION ALL sibling lingers until ExecutorEnd). Task 4: an O(1)
 * probe into a per-scan hash, built lazily on first fallback (so a scan that only
 * ever does current-row lookups never pays to build it).
 *
 * RESIDUAL, and it WIDENED with SCAN-04 -- say so plainly. The fallback probes the
 * HEAD's hash without any check that the head owns the probed row, because nothing
 * in bm25_score's arguments names a query (that identity exists only for &@@, via
 * bm25_distance_for_query). So with two finished siblings registered and the
 * projection decoupled by a Sort above an Append, a row belonging to branch A can
 * resolve through branch B's hash and receive B's score for it -- measured, not
 * hypothetical. This is the SAME recency-first-head policy the current-row fast
 * path used to apply before #242, and the identical mis-attribution was reachable
 * pre-SCAN-04 through that fast path; what changed is its reach, from one row per
 * scan to the head's whole ranking. #242's call-site binding touched the fast path
 * only; this fallback still probes the head. The trade was taken deliberately: in
 * exchange the ordinary streaming shape -- the one that returned a silent NULL
 * where a number belongs -- becomes correct. If that trade is ever revisited, the
 * principled fix is to probe
 * EVERY registered scan and resolve only on a UNIQUE hit, treating a multi-scan hit
 * as ambiguous (NULL) rather than letting recency decide. The query-qualified
 * overloads (#253, bm25_resolve_by_query) do that, over the scans ranking the
 * caller's query; this one-argument path is unchanged.
 */
/*
 * Map a projected heap TID to the ROOT line pointer of its HOT chain, in `s`'s heap.
 * Returns false (leaving *root untouched) when there is nothing to map.
 *
 * WHY THIS IS NEEDED. A HOT update writes the new tuple as a heap-only tuple on the
 * same page and does NOT call aminsert, which is the whole point of HOT: the existing
 * index entry keeps pointing at the chain's ROOT and the heap follows the chain on
 * fetch. So the scan ranks -- and stores in so->ranked[].tid -- the root TID, while
 * the executor projects the ctid of the tuple it actually fetched, the descendant.
 * The two differ, every by-TID lookup missed, and bm25_score(ctid) returned NULL for
 * any row updated in a non-indexed column since its last index entry (#204). Sealing
 * does not repair it, because sealing does not change which TID the index holds.
 *
 * A HOT chain never spans pages, so this is one page read, and only on a miss.
 *
 * BOUNDS FIRST, ALWAYS. bm25_score() takes an arbitrary user-supplied tid --
 * bm25_score('(9999,1)'::tid) is a legal call and sql/94 pins that it is a quiet
 * NULL. ReadBuffer on an out-of-range block would EXTEND the relation, turning a
 * read-only accessor into a writer, so the block is checked against the relation's
 * real length before any buffer is touched and an out-of-range probe simply fails to
 * map. Confined to `s`'s own heap: a tid is only ever mapped against the scan it is
 * then compared to, so a second scan on a DIFFERENT heap cannot have its page used
 * to resolve this one's row.
 */
static bool
bm25_hot_root_tid(BM25ScanOpaque s, ItemPointer t, ItemPointerData *root)
{
    BlockNumber     blk = ItemPointerGetBlockNumber(t);
    OffsetNumber    off = ItemPointerGetOffsetNumber(t);
    OffsetNumber    root_offsets[MaxHeapTuplesPerPage];
    Buffer          buf;
    Page            page;
    OffsetNumber    maxoff;

    if (s->heaprel == NULL)
        return false;                   /* no row emitted yet: nothing to resolve */
    if (blk >= RelationGetNumberOfBlocks(s->heaprel))
        return false;                   /* see BOUNDS FIRST above */

    buf = ReadBuffer(s->heaprel, blk);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);

    if (PageIsNew(page) || PageIsEmpty(page))
    {
        UnlockReleaseBuffer(buf);
        return false;
    }
    maxoff = PageGetMaxOffsetNumber(page);
    if (off < FirstOffsetNumber || off > maxoff)
    {
        UnlockReleaseBuffer(buf);
        return false;
    }

    /* The same idiom heapam_index_validate_scan uses to reconcile an index entry
     * with a HOT chain. root_offsets[off - 1] is InvalidOffsetNumber for a line
     * pointer that is not part of any chain. */
    heap_get_root_tuples(page, root_offsets);
    UnlockReleaseBuffer(buf);

    if (root_offsets[off - 1] == InvalidOffsetNumber)
        return false;

    ItemPointerSet(root, blk, root_offsets[off - 1]);
    return true;
}

/* Exact TID resolution against one scan: the current-row fast path only. Split out
 * so the HOT retry below can re-run it against the mapped root without duplicating
 * the bound that protects cur_ranked_idx.
 *
 * SCAN-03/TEXT-02: cur_ranked_idx must be bounded against the CURRENT nranked, not
 * merely non-sentinel. The WAND over-pull tail rebuild replaces ranked/scores/
 * ranked_keys with freshly palloc'd arrays sized to a new survivor count; if a
 * concurrent DELETE+VACUUM shrank the match set, that count can be SMALLER than the
 * stale index left over from the capped ranking. The scan stays registered until
 * bm25_endscan, so any later bm25_score(ctid) in the same query walks it and reads
 * past the end of both arrays -- heap garbage compared against a TID (a spurious
 * match returning a garbage score) or a segfault. bm25_scan.c now also clears the
 * index before the rebuild; this bound is the belt to that braces, and the one that
 * protects any future caller which rebuilds a ranking without going through that
 * exact call site. */
static inline bool
bm25_scan_tid_is_current(BM25ScanOpaque s, ItemPointer t, double *score)
{
    if (s->cur_ranked_idx != BM25_NO_CUR &&
        s->cur_ranked_idx < s->nranked &&
        ItemPointerEquals(t, &s->ranked[s->cur_ranked_idx].tid) &&
        /* #290: a WHERE row the ORDER BY query did not match has no score. Not a
         * match here, so resolution goes on to the hash, which omits it too. */
        !bm25_score_is_unmatched(s->scores[s->cur_ranked_idx]))
    {
        *score = s->scores[s->cur_ranked_idx];
        return true;
    }
    return false;
}

/*
 * #242: CALL-SITE BINDING for the score accessors' current-row fast path.
 *
 * The plain current-row walk returns the first registry scan, from the head,
 * whose current row matches the probe. The head is the most recently
 * (re-)registered scan, and a rescanned correlated inner re-heads itself on every
 * outer row. When that inner ranks the same index and its current row coincides
 * with the outer's (its top hit, which the correlation never rejects), the outer's
 * projection of that row got the inner's score: a plausible wrong number. Row
 * identity cannot separate the two scans; the CALL SITE can. Each textual
 * bm25_score(...) / bm25_score_key(...) has its own FmgrInfo, and a call site in a
 * streaming projection is evaluated once per row its scan emits.
 *
 * So each call site keeps, in flinfo->fn_extra, the scan whose current row ALONE
 * matched one of its probes -- by scan_serial, since a freed opaque's address can
 * be reused by a later scan -- and the backend's scored-emit sequence as of its
 * previous call. A scan is FRESH for the call site when it has emitted a row since
 * then (last_emit_seq > last_call_seq).
 *
 * The answer is the plain walk's -- head-first exact pass, then the HOT-root pass,
 * then the unchanged hash fallback (ADR 0064) -- with ONE exception: the call site
 * is bound, the bound scan's current row matches, the walk picked a DIFFERENT scan,
 * and BOTH have emitted since this call site's last call. Then the bound scan's
 * score is returned. That is the nested shape: the outer emitted the row being
 * projected, and the inner, rescanned for it, emitted too.
 *
 * THE CONTRACT, bounded. When the accessor is projected directly on its own scan's
 * emitted rows -- the #242 nested / correlated case -- it returns that scan's score
 * even when another live scan's current row coincides with the probe. Outside that,
 * two kinds of residual remain, and in both the accessor can return ANOTHER
 * concurrent scan's score:
 *   - collisions the call site cannot attribute: it never saw a probe that matched
 *     one scan alone (a join on the key with the accessors in the top-level
 *     projection; a collision on the first row), so it has no binding and gets the
 *     walk's head-first pick;
 *   - decoupled projections, where the row being projected is not its scan's current
 *     row: a PL/pgSQL FOR loop (which fetches 10 rows, then 50, ahead of the body),
 *     a Sort or Materialize above the scan, a cursor. Current rows then coincide
 *     with probes by accident, a binding learned from such a coincidence can be the
 *     wrong scan, and the bound scan can win over a walk pick that was right -- so
 *     here the answer can differ from the plain walk's, in either direction.
 *     sql/53 pins one such row (the PL/pgSQL shape) as a residual.
 *
 * Why the walk's pick must be fresh as well. A stale pick has not moved since the
 * call site last ran, so its match is a leftover row -- and a leftover row can be
 * the right one. In a hash join the build side's rows are projected out of the hash
 * table after that scan has finished, so its current row is its last one while the
 * probe side streams. A call site projecting the build side's column binds to the
 * probe side (the only scan whose current row keeps matching), and on the row equal
 * to the build side's last row the walk's head-first pick is correct and the bound
 * scan is not. Freshness of the bound scan alone would let it win there.
 *
 * The binding is updated after the answer is chosen: forgotten once the bound scan
 * leaves the registry (ended, or between a rescan and its reload), replaced whenever
 * exactly one registered scan's current row matches the probe, and the emit
 * sequence recorded on every call. A caller with no FmgrInfo resolves by the plain
 * walk.
 *
 */
typedef struct BM25ScoreCallSite
{
    uint64      bound_serial;   /* scan_serial of the bound scan; 0 = unbound */
    uint64      last_call_seq;  /* bm25_emit_seq as of this call site's previous call */
} BM25ScoreCallSite;

/* Backend-wide count of rows emitted by scored scans; see bm25_scored_scan_emitted.
 * 64-bit and backend-local, so it cannot wrap in a backend's lifetime. */
static uint64 bm25_emit_seq = 0;

void
bm25_scored_scan_emitted(BM25ScanOpaque so)
{
    so->last_emit_seq = ++bm25_emit_seq;
}

/* Does scan s's current row match the probe? If so, set *score. probe is an
 * ItemPointer for the tid path and a BM25KeyProbe for the key path. */
typedef bool (*BM25CurRowMatch) (BM25ScanOpaque s, void *probe, double *score);

/*
 * #301: PRIVILEGE CHECK for the row-addressed accessors -- bm25_score and
 * bm25_score_key in all their forms, and bm25_snippet.
 *
 * Ownership (bm25_owned_from) is not enough on its own. A refcursor a SECURITY
 * DEFINER function opens begins its index scan at the caller's first FETCH, so the
 * scan registers under the caller's user id, while the query it runs was planned and
 * permission-checked as the definer. These accessors take an arbitrary tid or key, and
 * the query-qualified forms look it up in the scan's WHOLE ranking, so the caller
 * could read the score of any row the definer's query ranked, including rows it
 * filtered out above the scan.
 *
 * So a scan is visible to these accessors only when the caller may read its heap:
 *   - no row-level security in force for the caller on the heap or any partition
 *     ancestor (check_enable_rls == RLS_ENABLED refuses). A caller granted SELECT
 *     under a policy can see only the policy's rows, and a definer's ranking holds
 *     the others. This also refuses the caller's own ranked query under a policy
 *     that folds to constant true (the one shape where the caller's own query
 *     reaches the index under RLS: bm25_match is not leakproof, so any other policy
 *     keeps @@@ off the index); README documents that cost;
 *   - and SELECT on the heap, on a PARTITION ancestor, or on every heap column the
 *     index reads: key and INCLUDE columns, and those its expressions and predicate
 *     reference. The key_field is an index column, so the key-to-score mapping is
 *     covered. A partition has exactly its parent's columns, so a role granted
 *     SELECT on a partitioned table can read every column of every partition
 *     through it (and each partition is scanned by its own index scan). A legacy
 *     inheritance parent's grant is NOT accepted: a child can add columns, and the
 *     parent's grant reads only the parent's, so accepting it let a role holding
 *     SELECT on the parent probe the child's own indexed column.
 *
 * A REFUSED SCAN IS INVISIBLE, exactly as another role's scan is: every probe
 * walker tests it before it looks at the scan's ranking or current row, and skips
 * it -- as a candidate, as a match, and as a rival in the sole-live-scan counts.
 * Testing it only after a lookup hit, and answering NULL then, made the NULL itself
 * the signal: with the caller's own scan ranking the same query, NULL-versus-score
 * for each of its rows reported whether the refused ranking held that row. So no
 * accessor's result may depend on anything inside a refused scan. The one exception
 * is bm25_snippet's error when the caller has no visible scan at all but does own a
 * refused one (bm25_sole_scored_scan): that depends only on the refused scan
 * existing, which the caller caused, not on what it ranked.
 *
 * Not applied to the &@@ distance; see bm25_distance_for_query.
 *
 * The result is cached per call site, in fn_extra, keyed on (index, user id). The
 * index determines the heap, and the column half of the check depends on the
 * index, so the index is the narrower key. The user id in the key is what keeps
 * the cache correct across SET ROLE and SECURITY DEFINER switches within one
 * query: each id gets its own entry. It is NOT a guarantee that a GRANT, a REVOKE
 * or a row_security change is seen at the next statement. fn_extra lives as long
 * as the expression state, which is one execution for an ordinary query but can be
 * the whole transaction for a PL/pgSQL simple expression, so a privilege change for
 * the SAME user in mid-transaction may go unseen at such a call site until the
 * transaction ends.
 */
typedef struct BM25ProbeAcl
{
    Oid         indexrelid;
    Oid         userid;
    bool        allowed;
} BM25ProbeAcl;

/* fn_extra of every row-addressed accessor. cs is used by the one-argument score
 * accessors only (#242); the others leave it zeroed. */
struct BM25ProbeFnCache
{
    BM25ScoreCallSite cs;
    MemoryContext mcxt;         /* fn_mcxt: acl[] is grown in it */
    int         nacl;
    int         maxacl;
    BM25ProbeAcl *acl;
};

/* The call site's cache, created on first use. NULL for a caller with no FmgrInfo
 * (DirectFunctionCall): it has no call site to remember, and its privilege checks
 * run uncached. */
static BM25ProbeFnCache *
bm25_probe_fncache(FunctionCallInfo fcinfo)
{
    FmgrInfo   *fl = fcinfo->flinfo;
    BM25ProbeFnCache *fc;

    if (fl == NULL)
        return NULL;
    if (fl->fn_extra == NULL)
    {
        fc = MemoryContextAllocZero(fl->fn_mcxt, sizeof(BM25ProbeFnCache));
        fc->mcxt = fl->fn_mcxt;
        fl->fn_extra = fc;
    }
    return (BM25ProbeFnCache *) fl->fn_extra;
}

static BM25ScoreCallSite *
bm25_score_callsite(BM25ProbeFnCache *fc)
{
    return fc != NULL ? &fc->cs : NULL;
}

/* relid and, when it is a partition, every partitioned table above it. Only
 * partition ancestors: see the privilege-check comment above for why a legacy
 * inheritance parent must not count. */
static List *
bm25_heap_and_partition_ancestors(Oid relid)
{
    List       *rels = list_make1_oid(relid);

    if (get_rel_relispartition(relid))
        rels = list_concat(rels, get_partition_ancestors(relid));
    return rels;
}

/* Column-level half of the check: SELECT on every heap column the index reads. A
 * whole-row reference in an expression or predicate needs every column, which is
 * what table-level SELECT (already refused by the caller) would mean, so it refuses.
 * System columns cannot appear in an index definition and are skipped. */
static bool
bm25_index_columns_selectable(Relation index, Oid heapid, Oid userid)
{
    Form_pg_index ix = index->rd_index;
    Bitmapset  *cols = NULL;
    int         i;

    for (i = 0; i < ix->indnatts; i++)
        if (ix->indkey.values[i] != 0)
            cols = bms_add_member(cols,
                                  ix->indkey.values[i] - FirstLowInvalidHeapAttributeNumber);
    pull_varattnos((Node *) RelationGetIndexExpressions(index), 1, &cols);
    pull_varattnos((Node *) RelationGetIndexPredicate(index), 1, &cols);

    i = -1;
    while ((i = bms_next_member(cols, i)) >= 0)
    {
        AttrNumber  att = i + FirstLowInvalidHeapAttributeNumber;

        if (att == InvalidAttrNumber)
            return false;
        if (att < 0)
            continue;
        if (pg_attribute_aclcheck(heapid, att, userid, ACL_SELECT) != ACLCHECK_OK)
            return false;
    }
    return true;
}

static bool
bm25_probe_acl_compute(Relation index, Oid userid)
{
    Oid         heapid = index->rd_index->indrelid;
    List       *rels = bm25_heap_and_partition_ancestors(heapid);
    ListCell   *lc;

    /* RLS first: it refuses even a caller holding SELECT. noError, because with
     * row_security = off check_enable_rls would otherwise raise for a caller the
     * policy applies to; RLS_ENABLED is returned instead, and refuses. */
    foreach(lc, rels)
        if (check_enable_rls(lfirst_oid(lc), userid, true) == RLS_ENABLED)
            return false;
    foreach(lc, rels)
    {
        bool        missing = false;    /* an ancestor dropped concurrently: no grant */

        if (pg_class_aclcheck_ext(lfirst_oid(lc), userid, ACL_SELECT,
                                  &missing) == ACLCHECK_OK)
            return true;
    }
    return bm25_index_columns_selectable(index, heapid, userid);
}

/* The privilege check for scan s, through the call site's cache. fc may be NULL.
 * Only owned scans reach here, so the user id is the scan's owner as well as the
 * caller. */
static bool
bm25_probe_allowed(BM25ProbeFnCache *fc, BM25ScanOpaque s)
{
    Oid         indexrelid = RelationGetRelid(s->indexrel);
    Oid         me = GetUserId();
    bool        allowed;
    int         i;

    if (fc != NULL)
        for (i = 0; i < fc->nacl; i++)
            if (fc->acl[i].indexrelid == indexrelid && fc->acl[i].userid == me)
                return fc->acl[i].allowed;

    allowed = bm25_probe_acl_compute(s->indexrel, me);

    if (fc != NULL)
    {
        if (fc->nacl == fc->maxacl)
        {
            fc->maxacl = Max(4, fc->maxacl * 2);
            fc->acl = fc->acl == NULL
                ? MemoryContextAlloc(fc->mcxt, fc->maxacl * sizeof(BM25ProbeAcl))
                : repalloc(fc->acl, fc->maxacl * sizeof(BM25ProbeAcl));
        }
        fc->acl[fc->nacl].indexrelid = indexrelid;
        fc->acl[fc->nacl].userid = me;
        fc->acl[fc->nacl].allowed = allowed;
        fc->nacl++;
    }
    return allowed;
}

static BM25ScanOpaque
bm25_scored_scan_by_serial(uint64 serial)
{
    BM25ScanOpaque s;

    for (s = bm25_scored_scan_head(); s != NULL; s = bm25_next_owned(s))
        if (s->scan_serial == serial)
            return s;
    return NULL;
}

/* One registry pass with one matcher. *pick and *pick_score are the FIRST match from
 * the head -- the plain walk's answer -- and *bound_hit and *bound_score record whether
 * `bound` matched. Unlike the plain walk this does not stop at the first match: it
 * must count them, because a unique match is what binds a call site. Returns the
 * number of matching scans. */
static int
bm25_walk_current(BM25ProbeFnCache *fc, BM25CurRowMatch match, void *probe,
                  BM25ScanOpaque bound, BM25ScanOpaque *pick, double *pick_score,
                  bool *bound_hit, double *bound_score)
{
    BM25ScanOpaque s;
    int         nmatch = 0;

    /* #301: only visible scans; the privilege check precedes the match so a
     * refused scan's current row is never compared with the probe. */
    for (s = bm25_visible_from(fc, bm25_scored_scans); s != NULL;
         s = bm25_next_visible(fc, s))
    {
        double      sc;

        if (!match(s, probe, &sc))
            continue;
        if (nmatch == 0)
        {
            *pick = s;
            *pick_score = sc;
        }
        if (s == bound)
        {
            *bound_hit = true;
            *bound_score = sc;
        }
        nmatch++;
    }
    return nmatch;
}

/* The current-row half of both resolvers, per the #242 comment above. Returns true
 * with *score set when some scan's current row matches; false sends the caller to
 * its hash fallback. hot, when non-NULL, is a second matcher run only when `exact`
 * matched nothing -- the plain walk's order, and the tid path's HOT-root mapping
 * costs a heap page read, so it stays off the path every ordinary row takes.
 * #301: the walk sees only the scans the caller may read, so a bound scan that has
 * since become refused simply never matches. */
static bool
bm25_resolve_current_row(BM25ProbeFnCache *fc, BM25CurRowMatch exact,
                         BM25CurRowMatch hot, void *probe, double *score)
{
    BM25ScoreCallSite *cs = bm25_score_callsite(fc);
    BM25ScanOpaque bound = NULL;
    BM25ScanOpaque pick = NULL;
    double      pick_score = 0.0;
    double      bound_score = 0.0;
    bool        bound_hit = false;
    int         nmatch;

    if (cs != NULL && cs->bound_serial != 0)
    {
        bound = bm25_scored_scan_by_serial(cs->bound_serial);
        if (bound == NULL)
            cs->bound_serial = 0;
    }

    nmatch = bm25_walk_current(fc, exact, probe, bound, &pick, &pick_score,
                               &bound_hit, &bound_score);
    if (nmatch == 0 && hot != NULL)
        nmatch = bm25_walk_current(fc, hot, probe, bound, &pick, &pick_score,
                                   &bound_hit, &bound_score);

    if (nmatch > 0)
    {
        /* bound_hit implies bound != NULL, which implies cs != NULL. */
        if (bound_hit && bound != pick &&
            bound->last_emit_seq > cs->last_call_seq &&
            pick->last_emit_seq > cs->last_call_seq)
            *score = bound_score;
        else
            *score = pick_score;
    }

    if (cs != NULL)
    {
        if (nmatch == 1)
            cs->bound_serial = pick->scan_serial;
        cs->last_call_seq = bm25_emit_seq;
    }
    return nmatch > 0;
}

static bool
bm25_match_tid_exact(BM25ScanOpaque s, void *probe, double *score)
{
    return bm25_scan_tid_is_current(s, (ItemPointer) probe, score);
}

/* The index holds a HOT chain's ROOT, the executor projects the descendant it
 * fetched, and for a row updated in a non-indexed column those differ (#204). */
static bool
bm25_match_tid_hot_root(BM25ScanOpaque s, void *probe, double *score)
{
    ItemPointer t = (ItemPointer) probe;
    ItemPointerData root;

    if (!bm25_hot_root_tid(s, t, &root) || ItemPointerEquals(&root, t))
        return false;                   /* unmappable, or not a HOT descendant */
    return bm25_scan_tid_is_current(s, &root, score);
}

/* The answer of a whole-ranking lookup. AMBIGUOUS means the ranking holds more than
 * one row under the probe with different scores (only a key can do that: tids are
 * deduplicated); *score is then the first writer's, which is what the one-argument
 * fallback reports, while the query-qualified accessor returns NULL. */
typedef enum BM25RankHit
{
    BM25_RANK_MISS,
    BM25_RANK_HIT,
    BM25_RANK_AMBIGUOUS
} BM25RankHit;

typedef BM25RankHit (*BM25RankLookup) (BM25ScanOpaque s, const void *probe, double *score);

/* Look t up in s's whole ranking rather than its current row: the per-scan hash,
 * then the same HOT-root retry the fast path makes, because the hash is keyed by
 * the ROOT tids the ranking holds. Shared by the head-hash fallback below and the
 * query-qualified accessor (bm25_resolve_by_query). */
static BM25RankHit
bm25_rank_lookup_tid(BM25ScanOpaque s, const void *probe, double *score)
{
    ItemPointer t = (ItemPointer) probe;
    BM25TidScoreEntry *e;
    ItemPointerData root;

    /* TEXT-10: unconditional -- the builder guards each hash itself now. */
    bm25_build_score_index(s);
    e = (BM25TidScoreEntry *) hash_search(s->score_by_tid, t, HASH_FIND, NULL);
    if (e == NULL && bm25_hot_root_tid(s, t, &root) && !ItemPointerEquals(&root, t))
        e = (BM25TidScoreEntry *) hash_search(s->score_by_tid, &root, HASH_FIND, NULL);
    if (e == NULL)
        return BM25_RANK_MISS;
    *score = e->score;
    return BM25_RANK_HIT;
}

static bool
bm25_resolve_score_tid(BM25ProbeFnCache *fc, ItemPointer t, double *score)
{
    BM25ScanOpaque s;

    /* bm25_score() is SQL-callable with an arbitrary tid, and `(0,0)` is a legal
     * value of the type -- but NOT a legal ItemPointer: ItemPointerIsValid is false
     * for offset 0. Every accessor below that inspects a tid asserts validity
     * (ItemPointerGetBlockNumber and ItemPointerEquals both do), so on an
     * --enable-cassert build a probe with offset 0 aborted the backend rather than
     * returning NULL. That is reachable from plain SQL by any user, and the project's
     * hardening CI leg is a gating cassert build.
     *
     * Rejected here, at the single entry point, rather than inside each consumer:
     * an invalid tid cannot identify a row, so "no scan owns it" is the right answer
     * for all of them, and one guard cannot be forgotten by a future third caller.
     * Production builds compile the asserts out and already returned NULL, so this
     * changes no non-assert behaviour. */
    if (!ItemPointerIsValid(t))
        return false;

    /* The HOT-root matcher is the SECOND matcher, not folded into the first: the
     * exact compare is the hot path taken on every projected row of every scan, and
     * mapping there would put a heap page read behind each of its ordinary misses.
     * bm25_resolve_current_row pays for it only once the cheap answer has failed.
     *
     * Residual (#314 SCORE-07, left deliberately; measured). The "ordinary misses"
     * above are rare only for rows projected in scan order. When the score is
     * decoupled from the scan (a Sort or Materialize above it, a cursor, a PL/pgSQL
     * FOR loop) the exact compare misses on essentially every row, so the HOT-root
     * matcher runs for every row and every registered scan before the per-scan hash
     * below, which is what actually answers: one heap buffer access per row per scan.
     * On a 20,000-row table with every row matching, projecting bm25_score(ctid)
     * through a Sort showed 20,538 shared buffer hits against 539 for the same query
     * without it (EXPLAIN BUFFERS). It costs a cache hit per row when the page is
     * resident and real I/O when cold; the answer is correct either way. Trying the
     * hash first would reorder matchers inside the call-site binding and nmatch logic
     * that #242, ADR 0103/0105 and #301 settled, which is not worth a performance
     * item. */
    if (bm25_resolve_current_row(fc, bm25_match_tid_exact, bm25_match_tid_hot_root,
                                 t, score))
        return true;

    /* SCAN-04: a LIVE rival disables the fallback, not merely a registered one.
     * A finished scan lingers in the registry until its bm25_endscan, which for a
     * UNION ALL sibling only fires at ExecutorEnd -- so from the second branch
     * onward a raw count is >= 2 and the fallback was disabled exactly when it was
     * needed, returning SQL NULL where a score belongs. That is a wrong answer that
     * appears only once a second scan exists, i.e. invisible to single-scan tests.
     * bm25_sole_live_scored_scan applies the same "could still emit" test
     * bm25_snippet's own ambiguity check uses. */
    /* The lookup makes the same HOT retry as the fast path above: under ORDER BY ...
     * LIMIT the projection is decoupled from amgettuple, so this hash -- not the
     * current-row compare -- is what answers. */
    s = bm25_sole_live_scored_scan(fc);
    return s != NULL && bm25_rank_lookup_tid(s, t, score) != BM25_RANK_MISS;
}

/* Key analogue. keytype/ksz describe the ARGUMENT's own key encoding (decoded by
 * bm25_score_key from the SQL arg type, NOT the active scan's key type). Only a
 * scan whose ranked_key_type equals keytype is a candidate: matching by TYPE
 * subsumes the width check (uuid and text are both 16 B, so width alone would let
 * a uuid arg memcmp against a text-keyed scan), and because the arg was decoded by
 * its own type there is never a cross-type stride/deref hazard. */
typedef struct BM25KeyProbe
{
    uint8       keytype;
    uint16      ksz;
    const unsigned char *kbuf;
} BM25KeyProbe;

static bool
bm25_match_key_current(BM25ScanOpaque s, void *probe, double *score)
{
    const BM25KeyProbe *k = (const BM25KeyProbe *) probe;

    /* SCAN-03/TEXT-02: same bound as bm25_scan_tid_is_current, and it matters
     * more here -- cur_ranked_idx is MULTIPLIED by the key stride below, so a
     * stale index reads that much further past the end of ranked_keys. The
     * width check further down was already reasoning about "the one place a
     * violated invariant would turn into an actual OOB read" without bounding
     * the index it multiplies. */
    if (s->cur_ranked_idx != BM25_NO_CUR &&
        s->cur_ranked_idx < s->nranked && s->ranked_keys != NULL &&
        /* The current row's key must actually be RESOLVED before its slot is
         * compared: an unresolved slot is all-zero, which matches a genuine
         * id = 0 probe byte-for-byte and would hand back this row's score under
         * a key it does not have. Checked before the memcmp, and bounded by the
         * same cur_ranked_idx test. */
        s->ranked_key_present[s->cur_ranked_idx] &&
        s->ranked_key_type == k->keytype &&   /* match by type (implies ksz == stride) */
        /* Defense in depth: the type match above is SUPPOSED to imply
         * ksz == s->ranked_key_size (bm25_seg_keymeta now bounds an on-disk
         * key_size to the exact width its key_type implies), but this memcmp
         * is the one place a violated invariant would turn into an actual
         * OOB read of ranked_keys with an attacker/corruption-controlled
         * length, so check it explicitly rather than trust the implication. */
        s->ranked_key_size == k->ksz &&
        memcmp(k->kbuf, s->ranked_keys + (Size) s->cur_ranked_idx * s->ranked_key_size,
               k->ksz) == 0)
    {
        *score = s->scores[s->cur_ranked_idx];
        return true;
    }
    return false;
}

/* Key analogue of bm25_rank_lookup_tid: the probed key in s's whole ranking. Only a
 * scan whose key type equals the probe's is a candidate, for the reason given above
 * BM25KeyProbe. */
static BM25RankHit
bm25_rank_lookup_key(BM25ScanOpaque s, const void *probe, double *score)
{
    const BM25KeyProbe *k = (const BM25KeyProbe *) probe;
    char       *e;
    Size        voff;

    if (s->ranked_keys == NULL || s->ranked_key_type != k->keytype ||
        s->ranked_key_size != k->ksz)
        return BM25_RANK_MISS;
    /* TEXT-10: unconditional -- the builder guards each hash itself now. The
     * ranked_keys check above is what used to make this guard safe; see
     * bm25_build_score_index for why that made idempotence a property of this
     * call site rather than of the builder. */
    bm25_build_score_index(s);
    e = hash_search(s->score_by_key, k->kbuf, HASH_FIND, NULL);
    if (e == NULL)
        return BM25_RANK_MISS;
    /* Must agree with the build-side layout in bm25_build_score_index:
     * MAXALIGN(key_size), not the raw key_size, then the conflict flag. */
    voff = MAXALIGN(s->ranked_key_size);
    *score = *(double *) (e + voff);
    return *(bool *) (e + voff + sizeof(double)) ? BM25_RANK_AMBIGUOUS : BM25_RANK_HIT;
}

static bool
bm25_resolve_score_key(BM25ProbeFnCache *fc, uint8 keytype, uint16 ksz,
                       const unsigned char *kbuf, double *score)
{
    BM25ScanOpaque s;
    BM25KeyProbe probe;

    probe.keytype = keytype;
    probe.ksz = ksz;
    probe.kbuf = kbuf;
    /* No HOT analogue: a key is the row's own column value, the same on every
     * version of it. */
    if (bm25_resolve_current_row(fc, bm25_match_key_current, NULL, &probe, score))
        return true;

    /* SCAN-04: see bm25_resolve_score_tid. */
    s = bm25_sole_live_scored_scan(fc);
    return s != NULL && bm25_rank_lookup_key(s, &probe, score) != BM25_RANK_MISS;
}

/*
 * #253: THE QUERY-QUALIFIED ACCESSORS -- bm25_score(tid, query [, regclass]) and
 * bm25_score_key(key, query).
 *
 * Why they exist. The one-argument accessors receive only row identity, and the
 * #242 call-site binding (bm25_resolve_current_row) cannot learn the owning scan
 * when a probe matches several scans' current rows (a key join of two ranked
 * subqueries), when the projected row is not its scan's current row (a PL/pgSQL
 * FOR loop's prefetch, a Sort or Materialize above the scan, a cursor), or when a
 * ctid names a row in more than one heap (the children under a Merge Append). No
 * rule over (row identity, registry state) separates those; ADR 0103 records the
 * tiebreaks tried and the counterexample each one had. The caller does know which
 * query it ranked by, and which table the row came from, so these overloads take
 * both as arguments.
 *
 * Resolution. The candidates are the scans ranking the byte-identical query, by
 * the rule the &@@ distance projection uses (bm25_scan_ranks_query, ADR 0061),
 * narrowed to scans reading heap `relid` when one is given. Each candidate is asked
 * whether its WHOLE ranking holds the row -- the per-scan hash, not the current
 * row -- so the answer does not depend on how far the scan has advanced relative to
 * the projection, and a decoupled projection resolves for as long as its scan is
 * registered and not rescanned. The current-row fast path is deliberately not
 * consulted: it would be a second answer to keep consistent with the hash.
 *
 * One candidate holding the key twice. Two ranked rows can share a key (a
 * non-unique key_field, or text keys equal in their first BM25_KEY_MAX_SIZE bytes).
 * With different scores the key cannot say which row is meant, so the result is
 * NULL (bm25_build_score_index flags the entry); the one-argument fallback keeps
 * reporting the higher score there, as before.
 *
 * Several candidates holding the row. With the same score, that score is returned:
 * it is the right answer whichever candidate owns the row (a self-join of one
 * ranked query; two cursors over it). With different scores the result is NULL.
 * That happens with a colliding ctid across heaps and no relid, the same query
 * ranked over two different bm25 indexes, or the same query ranked under two
 * snapshots whose corpus statistics differ.
 *
 * Why NULL and not the emit-recency tiebreak ADR 0104 gives the distance
 * projection. Recency is right only when the projection runs immediately after its
 * own scan's emit, and the decoupled shapes these overloads exist for are exactly
 * the ones where it does not. The distance projection must return a number,
 * because it is the ORDER BY resjunk of every ranked query. These accessors are
 * called explicitly and can return NULL. ADR 0103 rejected NULL-on-ambiguity for
 * the one-argument accessors because it turned correct answers into NULLs, and here
 * there are no earlier answers to degrade. The ambiguous set also shrinks to rows
 * that two scans of the same query both rank, and the relid argument removes the
 * cross-heap part of it.
 *
 * The bounded claim: for a row emitted by a registered, positioned scored scan
 * whose ORDER BY query is byte-identical to the argument (and whose heap is relid,
 * if given), the result is that scan's score for the row in its current ranking,
 * or NULL when another such scan -- or another row under the same key -- holds the
 * same row identity with a different score. Two limits on "current ranking":
 *   - after a rescan it is the new rescan's ranking. For a query that does not
 *     depend on the outer row that is normally the same score; a row the new
 *     ranking does not hold resolves to NULL, or to another candidate's score if
 *     one holds it;
 *   - the WAND over-pull (bm25_gettuple's tail rebuild) re-ranks under a fresh
 *     segment snapshot. If the index changed since the scan's first build, the
 *     rebuilt ranking scores the rows already emitted under the new corpus
 *     statistics, so the answer can differ from the value the scan emitted. The
 *     one-argument accessors read the same rebuilt ranking.
 */
static bool
bm25_resolve_by_query(BM25ProbeFnCache *fc, bool is_jsonb, const char *rhs, int rhslen,
                      bool have_relid, Oid relid,
                      BM25RankLookup lookup, const void *probe, double *score)
{
    BM25ScanOpaque s;
    bool        found = false;

    /* #301: only visible scans; the privilege check runs before the scan's
     * ranking is consulted, so a refused scan cannot shape the answer. */
    for (s = bm25_visible_from(fc, bm25_scored_scans); s != NULL;
         s = bm25_next_visible(fc, s))
    {
        double      sc;
        BM25RankHit hit;

        if (!bm25_scan_ranks_query(s, is_jsonb, rhs, rhslen))
            continue;
        /* A positioned scan has emitted, and bm25_gettuple sets heaprel on every
         * emit, so heaprel is non-NULL here; checked anyway, since a NULL would be
         * dereferenced. */
        if (have_relid &&
            (s->heaprel == NULL || RelationGetRelid(s->heaprel) != relid))
            continue;
        hit = lookup(s, probe, &sc);
        if (hit == BM25_RANK_MISS)
            continue;
        if (hit == BM25_RANK_AMBIGUOUS)
            return false;           /* two rows share the key in this ranking */
        if (found && sc != *score)
            return false;           /* two owners disagree: ambiguous, see above */
        found = true;
        *score = sc;
    }
    return found;
}

/* The query argument's payload bytes, which is what bm25_stash_orderby_rhs recorded
 * for text and for jsonb alike (VARDATA_ANY of the detoasted value). A packed
 * detoast is enough: the short-header form carries the same payload. */
static void
bm25_query_arg(FunctionCallInfo fcinfo, int argno, const char **rhs, int *rhslen)
{
    struct varlena *v = PG_DETOAST_DATUM_PACKED(PG_GETARG_DATUM(argno));

    *rhs = VARDATA_ANY(v);
    *rhslen = VARSIZE_ANY_EXHDR(v);
}

/* bm25_score(tid, query [, regclass]). The three-argument form restricts the
 * candidates to scans reading that heap; an InvalidOid there matches no scan. */
static Datum
bm25_score_query_common(FunctionCallInfo fcinfo, bool is_jsonb)
{
    ItemPointer t = (ItemPointer) PG_GETARG_POINTER(0);
    bool        have_relid = PG_NARGS() > 2;
    Oid         relid = have_relid ? PG_GETARG_OID(2) : InvalidOid;
    const char *rhs;
    int         rhslen;
    double      score;

    /* Same guard as bm25_resolve_score_tid: offset 0 is a legal tid value but not a
     * valid ItemPointer, and the hash and HOT lookups assert validity. */
    if (!ItemPointerIsValid(t))
        PG_RETURN_NULL();
    bm25_query_arg(fcinfo, 1, &rhs, &rhslen);
    if (!bm25_resolve_by_query(bm25_probe_fncache(fcinfo), is_jsonb, rhs, rhslen,
                               have_relid, relid, bm25_rank_lookup_tid, t, &score))
        PG_RETURN_NULL();
    PG_RETURN_FLOAT8(score);
}

/* bm25_score_key(key, query): the key is decoded by the argument's own type, as in
 * bm25_score_key below. There is no relid form, because the key form does not need
 * one to stay correct: a key held by two ranked rows with different scores gives
 * NULL, whether the rows are in two heaps or in one. */
static Datum
bm25_score_key_query_common(FunctionCallInfo fcinfo, bool is_jsonb)
{
    Oid         argoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
    unsigned char kbuf[BM25_KEY_MAX_SIZE];
    BM25KeyProbe probe;
    const char *rhs;
    int         rhslen;
    double      score;

    if (!bm25_key_type_from_oid(argoid, &probe.keytype, &probe.ksz))
        PG_RETURN_NULL();       /* unsupported arg type or no fn_expr: nothing to match */
    bm25_key_extract(probe.keytype, probe.ksz, PG_GETARG_DATUM(0), kbuf);
    probe.kbuf = kbuf;
    bm25_query_arg(fcinfo, 1, &rhs, &rhslen);
    if (!bm25_resolve_by_query(bm25_probe_fncache(fcinfo), is_jsonb, rhs, rhslen,
                               false, InvalidOid, bm25_rank_lookup_key, &probe, &score))
        PG_RETURN_NULL();
    PG_RETURN_FLOAT8(score);
}

/* One C symbol per query type, rather than dispatching on get_fn_expr_argtype, so
 * the text/jsonb distinction does not depend on fn_expr being available. */
PG_FUNCTION_INFO_V1(bm25_score_query);
Datum
bm25_score_query(PG_FUNCTION_ARGS)
{
    return bm25_score_query_common(fcinfo, false);
}

PG_FUNCTION_INFO_V1(bm25_score_query_jsonb);
Datum
bm25_score_query_jsonb(PG_FUNCTION_ARGS)
{
    return bm25_score_query_common(fcinfo, true);
}

PG_FUNCTION_INFO_V1(bm25_score_key_query);
Datum
bm25_score_key_query(PG_FUNCTION_ARGS)
{
    return bm25_score_key_query_common(fcinfo, false);
}

PG_FUNCTION_INFO_V1(bm25_score_key_query_jsonb);
Datum
bm25_score_key_query_jsonb(PG_FUNCTION_ARGS)
{
    return bm25_score_key_query_common(fcinfo, true);
}

/*
 * bm25_score -- positive BM25 score for a heap tuple identified by ctid.
 *
 * Resolves the TID to the owning scan via bm25_resolve_score_tid: the current-row
 * fast path first (head-first across concurrently-active scored scans, with the
 * #242 call-site override), then a hash-probe fallback for decoupled projections,
 * enabled whenever no scan behind the head can still emit. Returns NULL if no
 * scored scan owns the TID (no active scan, a still-live rival behind the head
 * with no current-row match, or the row was deleted between index scan and
 * projection). Under several concurrent scored scans whose current rows coincide
 * it can return another scan's score -- see the #242 contract and residuals.
 */
PG_FUNCTION_INFO_V1(bm25_score);
Datum
bm25_score(PG_FUNCTION_ARGS)
{
    ItemPointer t = (ItemPointer) PG_GETARG_POINTER(0);
    double      score;
    if (!bm25_resolve_score_tid(bm25_probe_fncache(fcinfo), t, &score))
        PG_RETURN_NULL();       /* no scored scan owns this row */
    PG_RETURN_FLOAT8(score);
}

/*
 * bm25_score_key -- BM25 score of the ranked row whose key_field value matches the
 * argument. Complements bm25_score(tid): use when the projection has the user id
 * (key_field), not the ctid, in scope. Four SQL overloads (int/bigint/uuid/text)
 * bind to this one C body; it decodes the argument by the ARGUMENT's OWN type
 * (get_fn_expr_argtype -> bm25_key_type_from_oid), never the active scan's key
 * type -- decoding an int Datum as a concurrent uuid-keyed scan's type would run
 * DatumGetUUIDP over the integer's value and dereference it as a pointer (a crash).
 * Resolution then considers only scans whose key type equals the argument's, so a
 * call whose type differs from every active scan's key type is a clean NULL, while
 * a concurrent same-type scan resolves exactly as bm25_score does (current-row fast
 * path with the #242 call-site override, then a hash fallback gated on no live rival
 * behind the head), with the same residuals. Returns NULL when no matching-type
 * scored scan is active, the arg type is unsupported / its fn_expr is unavailable,
 * or the key is absent (a pending-only row not yet in a KEYMAP, a dead tuple, or >=2
 * active scans with no current-row match).
 *
 * The argument's type must match the index key_field type -- the documented usage
 * projects the key column itself, whose type does. A mismatched type yields NULL
 * (never a cross-type match): cast a bare literal to the key type, e.g.
 * bm25_score_key(42::bigint) for a bigint key_field.
 */
PG_FUNCTION_INFO_V1(bm25_score_key);
Datum
bm25_score_key(PG_FUNCTION_ARGS)
{
    Oid            argoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
    uint8          keytype;
    uint16         keysize;
    unsigned char  kbuf[BM25_KEY_MAX_SIZE];
    double         score;

    if (!bm25_key_type_from_oid(argoid, &keytype, &keysize))
        PG_RETURN_NULL();       /* unsupported arg type or no fn_expr: nothing to match */
    bm25_key_extract(keytype, keysize, PG_GETARG_DATUM(0), kbuf);
    if (!bm25_resolve_score_key(bm25_probe_fncache(fcinfo), keytype, keysize, kbuf,
                                &score))
        PG_RETURN_NULL();
    PG_RETURN_FLOAT8(score);
}
