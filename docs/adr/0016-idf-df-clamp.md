---
id: 0016
title: Clamp df to ndocs inside bm25_idf, not the idf result to zero
date: 2026-07-29
status: Accepted
summary: bm25_idf clamps df to ndocs so its result is strictly positive, restoring the WAND block-max bound's monotonicity precondition without colliding with the idf == 0.0 "term absent from this field" sentinel.
---

# 0016. Clamp `df` to `ndocs` inside `bm25_idf`, not the idf result to zero

## Context

`bm25_idf` returns `log(1 + (N - df + 0.5) / (df + 0.5))`, which is non-negative
exactly when `df <= N`. Its header documented that precondition as "guaranteed by
caller". No caller guaranteed it.

The single-field scan path deliberately sums the **raw** dictionary `df` across
segments — `df` is never decremented on delete, which is what keeps the idf
denominator M3-identical under tombstones — while the denominator `live_ndocs`
**is** live (`bm25_seg_read.c` decrements `meta->ndocs` on every tombstone).
Delete more than roughly half of a term's documents with no intervening merge and
`df > N`, so the log argument falls below 1 and the idf goes negative. (The
multi-field path is safe: `field_df_cb` gates on liveness.)

A negative idf is not just an odd score. `bm25_termscore` is
`idf * (tf*(k1+1)) / denom`, so a negative idf **inverts** its monotonicity in
`tf` and in `doclen` — and that monotonicity is the entire basis of the WAND
block-max bound. Evaluated at `(max_tf, min_doclen)`, `bm25_block_ub` then
returns the block **minimum**; the "upper" bound sinks below every real score,
`sum >= theta` is false at the first pivot check, and the driver breaks out of
the segment as "nothing remaining can reach theta" — silently truncating the
ranking. That breaks both the answer and the bit-identical
WAND ≡ exhaustive contract (review ref C3, issue #34).

## Decision

We will clamp **`df` to `ndocs`** at the top of `bm25_idf`:

```c
double df_eff = Min((double) df, (double) ndocs);
return log(1.0 + ((double) ndocs - df_eff + 0.5) / (df_eff + 0.5));
```

This says the honest thing — "more documents contain this term than exist" means
all of them do — and yields `log(1 + 0.5/(N+0.5))`, strictly positive for every
representable `N`.

The fix lives in `bm25_idf` because that is the single source both the exhaustive
scorer and the WAND bound read, so the two stay bit-identical **by construction**
rather than by a second, separately-maintained guard on the bound side.

## Alternatives considered

- **Clamp the idf RESULT to `>= 0`** — the report's suggested fix, and the one
  first implemented here. It is wrong, and testing caught it: `idf_f[field] == 0.0`
  is a live **in-band sentinel** meaning "term absent from this field" (the C4
  field-scope gate pre-zeroes out-of-scope fields; `seg_posting_cb`,
  `bm25_block_ub` and `bm25_wand_cursor_score_doc` all skip on it). A *present*
  term whose idf clamped to exactly `0.0` is read as absent, contributes nothing,
  and drops out of the ranking entirely. Measured: the ranked scan returned
  **0 rows** where `@@@` membership returned 100. That trades a mis-ordered answer
  for no answer — strictly worse than the bug.
- **Live-gate the single-field `df`** like `field_df_cb` does — fixes the root
  inconsistency and gives a *true* BM25 idf. Rejected for now on two counts: it
  requires a full posting decode pass purely to compute `df`, which is exactly the
  work WAND exists to avoid; and it silently changes scores for every existing
  single-field index with deletes, contradicting an explicit documented choice
  (the M3-identity note in `bm25_scan.c`). Worth revisiting if per-field df
  decoding ever becomes free.
- **Fall through to the exhaustive scorer when any `idf_f[f] < 0`** — keeps WAND
  honest but leaves the negative scores, so the *ranking itself* stays inverted
  (a doc with a rarer term scores worse). It fixes the symptom the bound exposed,
  not the wrong scores underneath.
- **Clamp to a small positive epsilon** — arbitrary, and would still need a
  justification for the constant. Clamping `df` produces the correct value
  naturally.

## Consequences

- Terms in the `df > N` regime now score as though every live document contains
  them: a tiny positive, near-uniform contribution. Ranking among them falls back
  to `tf`/`doclen` and then the TID tie-break. That is the same treatment a
  genuinely universal term gets, which is the right answer.
- The correction is invisible once a merge runs: the merge rebuilds `df` from live
  documents only, so `df <= N` holds again and the clamp becomes a no-op.
- `bm25_idf` is now the **only** place an idf may be derived. Re-deriving one
  inline anywhere would reintroduce the negative case on that path alone and
  silently break WAND parity.
- `idf == 0.0` remains a reserved sentinel value. Any future change to `bm25_idf`
  has to keep its result strictly positive for a term that is actually present.
- Covered by `sql/61_negative_idf`: unit-level monotonicity and
  not-the-sentinel assertions, plus an end-to-end WAND-vs-exhaustive top-30
  array equality. The end-to-end gate needs **both** varying `tf` *and* enough
  live docs to span several 128-posting blocks — with a few hundred docs
  everything lands in one block and WAND is trivially correct however wrong the
  bound is, which is why `44_wand_skip` never caught this.

## Addendum (2026-10-05, PRs #331-#350)

- **The monotonicity this record restores is exact-arithmetic monotonicity.** IEEE-754
  evaluation of `bm25_termscore` can invert adjacent tf values by a few `DBL_EPSILON` (at
  k1 = 0 for any tf), which `wand_widen_ub`'s headroom absorbs. The `bm25_wand.c` header
  now says so (#312 SCORE-09, PR #349). The clamp is still required: a negative idf inverts
  the bound outright, not by an epsilon.
- **`sql/61_negative_idf` now proves its regime from the index** (#309, PR #347). It used to
  assert df > N on literals; it now reads df off the index. Its "after a merge" block never
  left the regime (a one-segment catalog makes `bm25_merge` a no-op, so df stayed 3000); it
  now merges for real and asserts df <= live rows. A second corpus with tf varying by block
  makes WAND actually prune while df > N, so the WAND-versus-exhaustive parity check also
  runs with pruning engaged.
