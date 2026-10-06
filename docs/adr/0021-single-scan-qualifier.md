---
id: 0021
title: An index scan accepts exactly one bm25 qualifier and refuses the rest
date: 2026-07-29
status: Superseded
superseded_by: 0109
summary: bm25_rescan raises ERRCODE_FEATURE_NOT_SUPPORTED when given more than one scan key or order-by key, instead of applying the first and silently discarding the others.
---

# 0021. An index scan accepts exactly one bm25 qualifier and refuses the rest

## Context

`bm25_rescan` copies all `nkeys` `ScanKey`s into `scan->keyData` but reads only
`keyData[0]`, and only `orderByData[0]`. `bm25_gettuple` sets
`xs_recheck = false` — the index match is declared authoritative — and the
planner removes an index clause from `qpqual`. So keys `1..n-1` were evaluated by
**nothing**.

`amcanmulticol = true`, so a multicolumn bm25 index is a normal thing to have,
and the planner turns both quals of
`WHERE title @@@ 'alpha' AND body @@@ 'beta'` into index clauses without any
`enable_*` coercion. Measured on a three-row fixture: the index path returns
`{1, 2}`; the true answer is `{1}`, because row 2's body is `'gamma four'`. A
silent wrong answer on a default plan (review ref C8, issue #39).

## Decision

`bm25_rescan` raises `ERRCODE_FEATURE_NOT_SUPPORTED` when `nkeys > 1` or
`norderbys > 1`, with a hint naming two supported ways to express the query:

- one query as an M6 boolean tree —
  `@@@ bm25_boolean(must => ARRAY[bm25_match_terms('title','alpha'), bm25_match_terms('body','beta')])`;
- or keep the extra condition out of the index scan by writing it in function
  form — `bm25_match(body, 'beta')` is not an operator clause, so the planner
  leaves it as a `Filter` above the index scan.

Both were verified to return the true answer.

## Alternatives considered

- **`xs_recheck = true` when `nkeys > 1`** — the report's "at absolute minimum".
  It is not a fix. The recheck defers to `bm25_match`, which has no index
  `Relation` and re-tokenizes with the english DEFAULT analyzer, so on a
  non-english index it drops correct matches (the reasoning is already written
  out at length in `bm25_gettuple`). It trades wrong extra rows for wrong missing
  ones.
- **Intersect the additional keys' match sets** — the feature-complete answer,
  and out of scope for a bug fix: it needs a full match pipeline per key plus a
  decision about whose scores rank the result, which the M6 boolean tree already
  answers properly.
- **Set `amcanmulticol = false`** — would forbid multicolumn *indexes*, which
  work correctly for building, ingest and merge (proven by
  `sql/30_field_postings`). The defect is in scanning with several qualifiers,
  not in having several columns.

## Consequences

- A query that used to return wrong rows now errors. That is the intent, and the
  hint gives two rewrites, but it is a behaviour change for anyone who had such a
  query and did not notice it was wrong.
- Single-qualifier scans, including ranked ones and every existing suite, are
  untouched — `sql/64_multi_scankey` asserts that explicitly alongside the
  refusals.

### Deliberately NOT changed: ORDER BY wins over WHERE

When both keys are present, the scoring branch takes its query from
`orderByData[0]` and never reads `keyData[0]`. If the two name different queries
the WHERE clause is silently discarded, and the result can be **disjoint** from
the true answer: `WHERE title @@@ 'alpha' ORDER BY title &@@ 'delta'` was
measured returning `{3}` against a true answer of `{1,2}`.

An agreement check was implemented and then **backed out**. It broke
`sql/38_phrase`, which uses a bare `WHERE` with a *field-scoped* `ORDER BY`
(`WHERE title @@@ '"delta echo"' ORDER BY title &@@ 'body:"delta echo"'`)
deliberately — that is the documented way to rank within one field of a
multi-field index, and it depends on ORDER-BY-wins. Neither the query text nor
`sk_attno` distinguishes "a refinement of the WHERE query" from "a different
question", so refusing the second necessarily refuses the first.

Redefining a shipped, tested contract is a design decision, not a bug fix, so it
is documented here and in `bm25_rescan` rather than made unilaterally. If the
disjoint-answer case is to be closed, the options are to require the ORDER BY
query to be a field-scoped form of the WHERE query (parse both, compare
post-field-split terms), or to drop ORDER-BY-wins and apply both keys — which is
the "intersect the keys" feature above.

## Addendum (2026-10-04)

The fresh-eyes review of this date reproduced the 'ORDER BY wins' consequence as a plan-dependent wrong answer on a default plan: `WHERE body @@@ 'cat' ORDER BY body &@@ 'dog'` returns {2,3} via the index and {1,3} via a seqscan. #290 asks for a superseding record that makes the scan return the SQL answer (the WHERE set, ranked, non-matches last at Infinity) and evaluates multiple `@@@` keys instead of refusing them; the field-scope idiom would then need its scope in the WHERE as well.
