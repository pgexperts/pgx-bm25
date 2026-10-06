/*-------------------------------------------------------------------------
 * bm25_selfuncs.c
 *    Restriction/join selectivity for the @@@ match operator: deliberately
 *    blind compile-time constants.
 *
 * Blindness is the design, not a shortcut.  A statistics- or index-reading
 * estimator was evaluated and rejected on four grounds (spec 2026-07-16,
 * ADR 0010):
 *   1. It never fires for the shape that motivated this work: a correlated
 *      Var RHS (LATERAL per-query search) is not a Const, so any
 *      Const-inspecting estimator punts to a flat default anyway.
 *   2. It puts index I/O on the plan path -- a metapage SHARE + segment
 *      catalog copy + dict page walks per term per segment -- and
 *      bm25_scan_snapshot's format-version gate can ereport(ERROR) at plan
 *      time.
 *   3. df is per-segment and the pending list has no dict at all, so the
 *      number would be structurally incomplete regardless of effort.
 *   4. Wildcard terms expand by walking every dict entry per segment:
 *      O(vocabulary x segments) inside the planner.
 * matchingsel was also rejected: with a Const RHS,
 * generic_restriction_selectivity executes the operator's own procedure
 * (bm25_match: re-analyze + whole-document tokenize) against ~110
 * MCV/histogram samples at plan time, and behaves asymmetrically between
 * custom and generic plans.
 *
 * A constant cannot be right for every query -- a single rare term really
 * is selective, a multi-term OR-ish query really matches ~half the corpus
 * (measured 0.49 mean on natural-language queries).  It only has to be
 * less wrong than contsel's 0.001, whose specific harm was putting
 * reltuples*sel below the query LIMIT on any corpus under ~LIMIT/0.001
 * rows, which disables Limit proration and bills the ordered index path
 * its full scan cost.  The value below restores proration across the
 * failure window; the companion procost change (COST 5000 on bm25_match, whose
 * derivation is in bm25_native--1.0.sql) independently prices the seqscan
 * fallback honestly. It was made in what was then the 0.2->0.3 upgrade script;
 * that lineage was folded into the single 1.0 CREATE EXTENSION script by the
 * rebrand (docs/adr/0013), so no such file exists any more.
 *-------------------------------------------------------------------------*/
#include "postgres.h"
#include "fmgr.h"

/*
 * derivation: swept {0.01, 0.05, 0.1, 0.25, 0.5} on a 5,183-row and a
 * 100,000-row corpus built to the sql/56_planner_estimates.sql vocabulary
 * shape (Apple M3 Max, PG18, fully cached).  All five candidates planned
 * the ordered Index Scan for the ranked LIMIT 10 query at every tested
 * point -- 5k under random_page_cost=8, 5k under seq_page_cost=0.5, and
 * defaults at both 5k and 100k -- so proration-restoration alone does not
 * discriminate; the single-term bare filter (body @@@ 'negligence', true
 * match rate 431/5183 = 8.3%) does: estimated match fraction tracks the
 * constant directly (reltuples * BM25_MATCH_SEL) --
 *   0.01 -> 1.0% est (8x under), 0.05 -> 5.0% est (1.7x under),
 *   0.1 -> 10.0% est (1.2x over), 0.25 -> 25.0% est (3x over),
 *   0.5 -> 50.0% est (6x over) -- reproducing, almost exactly, the
 * half-the-table absurdity spec 4 warns against for a term matching ~6-8%.
 * 0.05 and 0.1 both sit within a reasonable factor of the true rate and
 * are the only candidates on neither extreme.  0.05 also equals
 * bm25_costestimate's existing (src/bm25_handler.c) *sel = 0.05, closing
 * the 50x contradiction between the two selectivity paths at zero extra
 * cost (spec 4's tiebreaker, not the criterion).  Frozen at 0.05.
 */
#define BM25_MATCH_SEL 0.05

PG_FUNCTION_INFO_V1(bm25_matchsel);
PG_FUNCTION_INFO_V1(bm25_matchjoinsel);

/* Restriction: (PlannerInfo*, Oid, List*, int) -> float8.  Args unused. */
Datum
bm25_matchsel(PG_FUNCTION_ARGS)
{
    PG_RETURN_FLOAT8(BM25_MATCH_SEL);
}

/* Join: (PlannerInfo*, Oid, List*, JoinType, SpecialJoinInfo*) -> float8.
 * Same constant as restriction so the two slots cannot contradict each
 * other the way contsel/contjoinsel vs bm25_costestimate's 0.05 did. */
Datum
bm25_matchjoinsel(PG_FUNCTION_ARGS)
{
    PG_RETURN_FLOAT8(BM25_MATCH_SEL);
}
