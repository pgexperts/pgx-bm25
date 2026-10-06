---
id: 0043
title: The WAND block-max bound owes domination, not bit-exactness; prunes are widened
date: 2026-08-05
status: Accepted
summary: Bit-exactness is a property of the score path only; the bound path owes domination, and both WAND prune comparisons are widened by a relative slack proportional to the summand count.
---

# 0043. The WAND block-max bound owes domination, not bit-exactness; prunes are widened

## Context

`bm25_wand.c` opened with an invariant it stated for itself: because IEEE-754
addition is not associative, `(c1+c2)+c0` can land 1 ULP below `(c0+c1)+c2`, so
every accumulation of a block-max bound had to mirror the scorer's fold order
exactly. `bm25_block_ub` went to real lengths to honour it, sorting a block's
fields by `field_id` so the bound's summation matched the scorer's bit for bit
at a zero-slack document.

The file did not actually hold that invariant, in two independent ways:

- **Ordering.** The deep check accumulated `bsum` over `cur[]`, which
  `wand_sort_cursors_by_docid` re-sorts by docid on every iteration. Stability
  preserves the *previous* iteration's order, not query-term order, so equal-docid
  cursors at the pivot can be permuted relative to `by_qi[]` — the order the real
  score is built in. Needs ≥3 cursors at the pivot to bite, since IEEE addition is
  commutative for two summands.
- **Grouping.** `bm25_block_ub` folds per-field contributions into its own local
  `ub = 0.0` and returns a per-term subtotal, while `bm25_wand_cursor_score_doc`
  adds each `(term, field)` posting into the caller's shared running double — a
  flat fold. So the bound computes `Σ_terms(Σ_fields)` against a flat
  `Σ_(term,field)`. This is independent of ordering and bites at two terms
  whenever any term after the first matches more than one field.

Either makes the bound land below the true score, and the top-k heap evicts on an
exact score tie broken by TID, so a 1-ULP-low bound can shallow-skip a document
the exhaustive path keeps — a divergence the BIT-EXACT CONTRACT in `bm25_wand.h`
forbids.

The decisive constraint is that the obvious repair does not reach both prune
sites. Walking `by_qi[]` and adding an accumulate-into-caller variant of
`bm25_block_ub` fixes the deep check. It cannot fix the pivot/termination sum:
that sums `global_ub`, which is a MAX over blocks and therefore inherently a sum
of precomputed subtotals, and it must walk docid order because the
prefix-scan-until-θ *is* the WAND pivot rule. That site prunes with equal force —
`piv < 0` abandons the rest of the segment — so fixing only the named site would
have shipped an invariant the file still could not hold, which is the condition
ADR 0040 was written to stop repeating.

## Decision

Split the two contracts explicitly.

**Bit-exactness belongs to the score path.** `bm25_wand_cursor_score_doc`'s flat
fold into the caller's double, and the `-ffp-contract=off` build flag that
protects it, are unchanged. The BIT-EXACT CONTRACT in `bm25_wand.h` is untouched:
it governs `bm25_topk_drain_sorted`'s output against `scored_desc`, not the
bound's internal arithmetic.

**The bound path owes only domination.** Every prune comparison is widened by a
relative slack sized to the number of summands folded at that site:

```c
static inline double
wand_widen_ub(double ub, int nsummands)
{
    return ub * (1.0 + (double) (2 * nsummands + 4) * DBL_EPSILON);
}
```

Applied at both prune sites, always in the conservative direction: the pivot sum
is widened *upward* before its `>= theta` test, and the deep check's `bsum` is
widened *upward* so `bsum < theta` fires less often. Both changes cause strictly
more candidates to be scored and never fewer, so the delivered ranking cannot
change.

`bm25_block_ub`'s field sort is kept, re-documented as a slack *minimiser* rather
than a correctness dependency.

## Alternatives considered

- **Walk `by_qi[]` and add an accumulate-into-caller `bm25_block_ub` variant
  (the originally-planned fix)** — verified achievable and genuinely bit-exact at
  the deep check: the field sort is pure reordering with no arithmetic, and at a
  coincidence document `bm25_termscore` receives bit-identical integer arguments.
  Rejected because it cannot reach the pivot/termination sum at all, for the
  structural reasons above. Shipping it alone would fix one of two sites while
  leaving the file asserting an invariant it violates at the other.
- **A fixed one-ULP nudge, e.g. `nextafter(theta, -INFINITY)`** — under-specified.
  Two orderings of *n* non-negative summands can differ by up to ~(n−1) ULPs of
  the total, so a constant nudge is a heuristic, not a bound, once a query has
  more than about three `(term, field)` contributions.
- **Do nothing and document the hazard** — the reachability is narrow (≥3 terms
  or multi-field grouping, a full-coincidence block, and an exact score tie at the
  k-th boundary with an unfavourable TID), but the file states this exact
  invariant for itself, and `sql/43_wand_parity.sql` asserts bit-exact
  exhaustive/WAND agreement as a gating test.

## Consequences

- The invariant the file states is now one it can hold at *every* prune site,
  and it survives future refactors of the impact table or the pivot rule —
  neither of which could have preserved the old fold-order requirement.
- Prune loss is ~1e-14 relative: a block is spared only when its bound lands
  within a few ULPs of θ. Not measurable.
- **The widening's soundness now depends on every summand being non-negative.**
  This is enforced today — `bm25_idf` clamps `df_eff = Min(df, ndocs)` so idf is
  strictly positive, query boosts are rejected at parse unless `> 0`, and the
  `k1`/`b`/`boost` reloptions are range- and finiteness-checked at DDL — but a
  future relaxation of any of those would silently invert the widening into a
  *narrowing*, which is unsafe. Called out in `wand_widen_ub`'s own comment.
- `nsummands` is deliberately over-estimated as `cursors × MAX_FIELDS` rather
  than counted exactly, to avoid reaching into the impact table from the prune
  site. Roughly a 32× over-estimate of the slack, which costs nothing at this
  magnitude.
- **No test pins this.** The change only alters behaviour when a bound lands
  within ~1e-14 relative of θ, and no fixture constructs that tie. The existing
  bit-exact parity suite (`sql/43_wand_parity.sql`) guards the *score* path's
  contract, which is what actually protects users; this record is the durable
  artifact for the bound's contract. Constructing a fixture that forces the tie
  is open follow-up work.
- `-ffp-contract=off` in the Makefile is now load-bearing for two distinct
  reasons — the score path's bit-identity (uncovered by any slack) and the
  bound-term premise the widening rests on. Both are documented at the sites.

## Addendum (2026-10-04)

The fresh-eyes review of this date found the domination contract violated for multi-field indexes: the writer cuts blocks every 128 postings regardless of document boundaries, so a document's per-field postings can straddle two blocks and neither block's bound covers its full contribution. WAND then prunes the document, and the resume-by-key tail drops it even from unlimited ranked scans (399 of 400 rows in the repro). Tracked in #289 (Critical).

## Addendum (2026-10-05)

Resolved by ADR 0113 (#289). The domination this record states held per block and was false
for a document whose postings straddle two blocks. The reader now bounds a straddler at both
prune sites on every segment: the global bound sums per-field maxima, and the deep check adds
the continuation's exact contribution. The writer also stops creating straddles. The widening
this record describes still applies to every comparison.
