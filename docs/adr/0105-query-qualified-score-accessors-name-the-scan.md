---
id: 0105
title: Query-qualified score accessors name the scan instead of inferring it
date: 2026-09-29
status: Accepted
summary: bm25_score(tid, query [, regclass]) and bm25_score_key(key, query) resolve by the ranked query (and optionally the heap) against each candidate scan's whole-ranking score hash, returning NULL when two candidates disagree, so the residual shapes ADR 0103 could not attribute (key-join collisions, decoupled projections, ctids repeated across Merge Append children) stop returning another scan's score; the one-argument accessors are unchanged and still follow ADR 0103.
---

# 0105. Query-qualified score accessors name the scan instead of inferring it

## Context

`bm25_score(tid)` and `bm25_score_key(key)` receive only row identity. ADR 0103
bounded what that can answer: a call-site binding attributes a row to its own scan
when the accessor is projected directly on that scan's emitted rows, and returns
another scan's score in two named residual shapes. Issue #253 tracked those
residuals. Its reading, and the code comment that now heads the resolver, is that
no rule over (row identity, registry state) separates them; ADR 0103 records the
tiebreaks tried and the counterexample each had. The three shapes, in each of
which the one-argument accessor can return another concurrent scan's score with no
error:

- an unattributable collision: a key join of two ranked subqueries with the
  accessor above the join;
- a projection decoupled from its scan: a PL/pgSQL `FOR` loop (which fetches ahead
  of the loop body), a `Sort` or `Materialize` above the scan, a cursor;
- a ctid that repeats across the children of a Merge Append over an inheritance or
  partitioned parent (the same `(block, offset)` can exist in several children).

What the caller does know is which query it ranked by and which table the row came
from. The row-only signature throws both away.

## Decision

Add overloads that take them: `bm25_score(tid, query)`,
`bm25_score(tid, query, regclass)` and `bm25_score_key(key, query)`, each for a
`text` or a `jsonb` query (`bm25_native--1.0.sql`; `bm25_resolve_by_query` in
`src/bm25_score.c`). `tableoid` supplies the regclass. `bm25_score_key` has no heap
argument: a key held by two ranked rows with different scores gives NULL whether
the rows are in two heaps or one, so it needs none to stay correct.

- **Candidates.** The registered scans ranking the byte-identical query, by the
  rule ADR 0061 introduced for the `&@@` distance projection (now one shared
  predicate, `bm25_scan_ranks_query`), narrowed to scans reading the given heap
  when one is passed.
- **Probe the whole ranking, not the current row.** Each candidate is asked whether
  its per-scan score hash holds the row (`score_by_tid` / `score_by_key`, built
  lazily by `bm25_build_score_index`). The answer therefore does not depend on how
  far the scan has advanced relative to the projection, so a decoupled projection
  resolves for as long as its scan is registered and not rescanned. The current-row
  fast path is deliberately not consulted; it would be a second answer to keep
  consistent with the hash.
- **NULL on disagreement.** One candidate holding the row: its score. Several
  holding it with the same score: that score (a self-join of one ranked query, two
  cursors over it). Several holding it with different scores: NULL. A key that two
  rows of one candidate's ranking share (a non-unique `key_field`, or `text` keys
  equal in their first 16 bytes, `BM25_KEY_MAX_SIZE`) with different scores is
  flagged in the key hash and also gives NULL. The one-argument fallback ignores the
  flag and keeps its first-writer answer, the higher score, as before.
- **The WAND over-pull rebuild re-positions on the last emitted row.** When
  `bm25_gettuple` rebuilds the exhaustive ranking after the scan has been pulled
  past `wand_top_k`, it cleared the scan's current-row index and left it cleared if
  the rebuilt ranking had nothing after the last emitted row (a capped build holding
  exactly `wand_top_k` matches). The scan then read as never positioned, and the
  resolvers that require a positioned scan (the `&@@` distance projection and the
  query-qualified accessors) skipped it, so a query-qualified accessor projected above
  a `Sort` returned NULL. The rebuild now sets `cur_ranked_idx` to
  the found row. That is a side effect on existing behaviour, in the capped
  exactly-k shape only, and it makes the scan behave as an uncapped one already did:
  a one-argument `bm25_score` above a `Sort` now answers instead of returning NULL,
  and an `&@@` projection evaluated in a later statement while a fully fetched
  cursor is open now reads the last emitted row's distance instead of `+inf` (for
  the cursor's other rows that is a finite value that is not their own distance,
  which is the decoupled-projection residual of ADRs 0061 and 0104, now reached
  where it used to give `+inf`). Both shapes were already among the documented
  decoupled-projection residuals.
- **The one-argument accessors are otherwise unchanged.** Their hash-fallback
  lookups were factored into helpers the new resolver shares; ADR 0103 still
  governs them, which is why this record amends it (addendum on 0103) rather than
  superseding it.
- **Documentation carries the other half.** The README's "Score accessors" section
  says when to use each form and lists their limits. Its advice, where possible, is to
  compute the score in the scanning query and carry the column outward, or use
  `-(col &@@ q)`, which is the same score and needs no ctid or key; the overloads
  are for scores that must be computed away from the scan.

## Alternatives considered

- **The ADR 0104 emit-recency tiebreak for the overloads.** Recency is right only
  when the projection runs right after its own scan's emit, and the decoupled shapes
  these overloads exist for are the ones where the most recent emitter is not the
  owner. Rejected. The distance projection has to return a number, because it is
  the resjunk column of every ranked query; these accessors are called explicitly
  and can return NULL.
- **NULL on ambiguity for the one-argument accessors.** ADR 0103 tried it and
  rejected it on measurement: it turned correct answers into NULL. Here there are no
  earlier answers to degrade, and the ambiguous set is smaller: rows that two
  candidate scans of the same query both rank, less the cross-heap part the
  `regclass` argument removes.
- **Change the one-argument accessors.** Not done; they keep ADR 0103's bounded
  contract.
- **A planner-support binding or an executor hook exposing the evaluating scan
  node.** ADR 0103 found the planner-support route unreachable (a support function
  sees the expression tree, not the chosen plan, and the AM scan never learns its
  range-table index) and deferred the hook as SQL-signature-breaking or
  executor-touching. Not pursued here; passing the query and heap as ordinary
  arguments needed neither.
- **Consult the current-row fast path first.** Rejected for the reason under
  "Probe the whole ranking".

## Consequences

- The bounded claim, from the resolver's header comment: for a row emitted by a
  registered, positioned scored scan whose `ORDER BY` query is byte-identical to the
  argument (and whose heap is the `regclass` argument, if given), the result is that
  scan's score for the row in its current ranking, or NULL when another such scan,
  or another row under the same key, holds the same row identity with a different
  score.
- Query matching is byte for byte. A differently spelled query, even an equivalent
  one, matches no scan and gives NULL. A jsonb query compares in its stored form, so
  key order and whitespace do not matter but a number's written scale does; an
  uncast literal is taken as `text` and matches no jsonb-ranked scan (README).
- NULL also results when a ctid collides across heaps and no `regclass` is given,
  when the same query is ranked over two different bm25 indexes, and when the same
  query is ranked under two snapshots whose corpus statistics differ (resolver
  comment). A row that a WAND-capped scan never emitted, because it lies past
  `wand_top_k`, is also NULL.
- Two limits follow from reading the ranking rather than the emitted value (both in
  the resolver comment and the README):
  - after a rescan the answer comes from the new rescan's ranking. For a query that
    does not depend on the outer row that is normally the same score; a row the new
    ranking does not hold is NULL, or another candidate's score if one holds it;
  - the WAND over-pull rebuild re-ranks under a fresh segment snapshot. If the
    index changed since the first build, rows already emitted are scored under the
    new corpus statistics, so the answer can differ from the value the scan
    emitted, and the one-argument accessors read the same rebuilt ranking. The
    rebuild's ordering problem is issue #268 (addendum on ADR 0005).
- Evidence, as stated in the PR #263 description: a differential over 121,848 rows
  (12 shapes x 42 query pairs, with and without stats) found 0 wrong
  answers and 5,348 NULLs, all of them cross-child collisions without `tableoid` or
  rows past the WAND cap that the scan never emitted. The one-argument accessor on
  the same shapes gave 25,532 wrong answers. The harness is not in
  the tree. `sql/116_score_query_overloads` is the in-tree test: it covers every
  residual class against single-scan baselines and records that the one-argument
  accessor misattributes on each.
- Declared VOLATILE by omission and `PARALLEL RESTRICTED`, like the one-argument
  forms: the same backend-local registry is read, and `sql/103` pins that no
  scan-state reader differs from the others.
- The one-argument accessors keep both residuals of ADR 0103. They are now
  avoidable by using the overloads rather than merely documented.

## Addendum (2026-10-05)

#301 (ADR 0115): the candidates the query-qualified forms probe are the scans the caller owns
and may read. The query-qualified forms probed each candidate's whole ranking, which is what
made the cross-role oracle exact. A refused scan is invisible to them, as another role's scan
is, so a refused scan can neither supply an answer nor make two candidates "disagree" into a
NULL that depends on what it ranked.
