---
id: 0061
title: Resolve the &@@ distance projection by ORDER BY query identity, not by registry head
date: 2026-08-16
status: Accepted
supersedes: 0007
summary: bm25_distance/bm25_distance_jsonb resolve the projected row's distance by matching the projected expression's own RHS against each scoring scan's stashed ORDER BY RHS, replacing 0007's head-resolution, which returned another scan's distance under nesting and — contrary to 0007's stated rationale — corrupted row ORDER, not just the materialized value.
---

# 0061. Resolve the `&@@` distance projection by ORDER BY query identity, not by registry head

## Context

ADR 0007 replaced a single backend-global active-scored-scan slot with an
intrusive registry, and gave each accessor a resolution strategy:
`bm25_score`/`bm25_score_key` resolve by ROW identity (they are handed a ctid or a
key); `bm25_snippet` fails loud when ambiguous (it is handed neither); and
`bm25_distance`/`bm25_distance_jsonb` read the registry HEAD, because they are the
per-row `&@@` sort-key materializer on every ranked query and therefore must not
error.

Head-resolution is wrong, and 0007's justification for tolerating it is wrong.

The head is the most recently **registered** scan, and registration happens at
scan **load** — not at emit. Under a correlated subquery the inner scan loads, and
therefore registers, *during the outer row's projection*. So when the outer's
resjunk `&@@` is evaluated, the head is the inner scan, and the outer row is
projected with the inner scan's distance.

0007's Consequences section says the residual affects "the materialized VALUE, not
the row order the executor already has." **That is false**, and measurement is what
established it. Sorting on the projected value — the ordinary
`ORDER BY <distance>, <tiebreak>` shape this project uses throughout — makes every
row a tie once the value is clobbered to a constant, and the BM25 ranking collapses
to the tiebreak. On the regression fixture the ranked order degrades from
`{5,3,1,4,2,6}` to `{1,2,3,4,5,6}`, i.e. pure id order. A wrong sort key is not a
cosmetic residual.

Two further defects share the cause: an inner scan matching nothing registers
having emitted no row, so its `+inf` initializer was projected onto every outer
row; and `bm25_distance_jsonb` — a second, independent copy of the same body in
`bm25_handler.c` — had drifted out of scope of earlier fixes in this area.

## Decision

`bm25_distance` and `bm25_distance_jsonb` resolve by **ORDER BY query identity**,
through one shared resolver, `bm25_distance_for_query`.

Each scoring scan stashes a pristine copy of its ORDER BY key's RHS
(`orderby_rhs`, `orderby_rhs_len`, `orderby_rhs_jsonb`) at the scan-key capture
site. The resolver walks the registry and returns `cur_orderby_dist` from the first
scan that is scoring, is POSITIONED (`cur_ranked_idx != BM25_NO_CUR`), belongs to
the same operator family, and whose stashed RHS is byte-equal to the RHS of the
expression being projected. Failing that it returns `+infinity`.

Three properties make this the right identity to use:

- The query is an identity these functions **already receive** — it is argument 1
  of `bm25_distance(text, text)` and `bm25_distance_jsonb(text, jsonb)` — and was
  simply ignored.
- The stash must be PRISTINE. The field-scope and phrase micro-parsers rewrite
  `so->qterm` in place, so `qterm` is not what the caller wrote and cannot serve.
- The positioned check makes the `+inf` case structural rather than accidental.

`bm25_score`, `bm25_score_key`, `bm25_snippet`, the registry itself, and the
scanctx teardown callback are all carried forward from 0007 unchanged. This record
supersedes 0007 only in the distance family's resolution strategy and in the
falsified claim about row order.

## Alternatives considered

- **Re-head the registry on emit ("most recently emitted wins").** Does not fix
  the reported shape: in a correlated subquery the inner scan *is* the most recent
  emitter. 0007 itself rejected recency for this exact reason, before the bug was
  observed — "the CORRELATED inner subquery is genuinely the innermost /
  most-recently-active scan by construction, yet it is the OUTER's score that the
  target list needs." Adopting it would have re-introduced a rejected design and
  still left the bug.
- **Skip registry entries that cannot emit (liveness filter only).** Fixes the
  exhausted-scan and no-match cases but not the reported one, where the inner scan
  is live and positioned. Its liveness idea survives as the positioned check.
- **Resolve by ROW identity, as `bm25_score` does.** Unavailable: these functions
  are handed (document value, query) and no ctid, and fmgr gives a scalar function
  no handle on the slot being projected. Threading one in means new operator
  signatures — the "heavyweight identity threading" 0007 rejected, and still
  oversized.
- **Return the value through PostgreSQL's own `xs_orderbyvals`.** Investigated
  specifically, because if core already had the mechanism the registry would be
  the root cause rather than the workaround. It does not: `xs_orderbyvals` is
  consumed only inside `IndexNextWithReorder` — three sites there (the
  reorder-queue push, the pairing-heap comparison, and the `lastfetched_vals`
  aliasing), and nowhere else in the backend. No planner or executor path projects
  it into a target list; `ExecProject` always re-evaluates the tlist's own `OpExpr`. Core never
  needed such plumbing because GiST/SP-GiST KNN distances are pure functions of
  (row value, query) and so are recomputable at projection time. A BM25 distance
  is not — it depends on corpus statistics, the analyzer, and WAND state that live
  on the scan. So the registry is the only channel available to this AM, and a
  projection-from-`xs_orderbyvals` mechanism would be a core patch.
- **A planner support function rewriting the tlist occurrence.** Self-defeating:
  `eval_const_expressions` processes the ORDER BY expression too, and a rewritten
  expression no longer matches the opfamily's ordering operator, disabling the
  `amcanorderbyop` path outright.

## Consequences

- The reported shape, both its wrong value and its wrong row order, is fixed, as
  are the exhausted-scan and empty-inner-scan cases and the jsonb copy.
- **Deliberate behaviour change**: projecting a query that no live scan is ranking
  now yields `+infinity`. It previously yielded the ranked scan's distance for a
  *different* query — a silently wrong finite number. `+inf` is the same answer the
  off-index (seqscan) path already gives, so the degrade contract is uniform.
  Pinned by `sql/87_distance_scan_identity`.
- **Residual, unfixable within the operator signature**: two live scans ranking the
  BYTE-IDENTICAL query resolve recency-first, exactly as before this change. When
  the queries are identical nothing in `(document value, query)` separates the
  scans. The regression suite is written to avoid manufacturing this case by
  accident — its baselines are materialised in their own statements, because
  computing a baseline as a sibling subquery would put two same-text scans in one
  statement and measure the residual instead of the fix.
- A second residual, also carried over: a projection fully decoupled from emission
  (a scan exhausted before an upper node projects) gets that scan's last-row
  distance. Same signature limit.
- Cost is a length-gated `memcmp` against N registry entries per projected row,
  N = concurrently-live scored scans, typically 1–2. Negligible against fmgr
  overhead.
- The escape hatch for both residuals is unchanged from 0007: a custom scan node
  or a resjunk identity column. Still out of proportion.

## Addendum (2026-09-29)

The Consequences section names, as unfixable within the operator signature, that
"two live scans ranking the BYTE-IDENTICAL query resolve recency-first, exactly as
before this change." Issue #252 showed that residual includes every ranked query on
a partitioned or inheritance parent (a Merge Append over one same-query index scan
per child), where "recency" meant the most recently REGISTERED scan and produced
out-of-order rows and a wrong `LIMIT k` top-k. [0104](0104-distance-among-same-query-siblings-resolves-by-emit-recency.md)
changes that one residual: among RHS-matching positioned candidates the resolver now
picks the most recent EMITTER (highest `last_emit_seq`), which is correct when the
projection runs right after its own scan's emit. The decision above is otherwise
unchanged. The re-head-the-registry-on-emit alternative was rejected because in the
correlated shape the inner scan is the most recent emitter yet the outer's
distance is wanted; the RHS match still runs first and separates scans ranking
different queries, and 0104 applies recency only among candidates that already match it. The
decoupled-projection residual stands.

## Addendum (2026-09-29, the +inf fall-through and issue #245)

This record's resolver ends in `+infinity` when no live positioned scan ranks the
projected query. That degradation is issue #151's TEXT-01 finding, decided and kept:
TEXT-01 proposed erroring for parity with its jsonb sibling, but the comparison does not hold
(`bm25_distance_jsonb` is the same resolver; the function it was compared with is
`bm25_match_jsonb`, the `@@@` filter, which errors because a jsonb tree cannot be
evaluated without the index), and both fall-through cases are pinned as decisions by
`sql/50_orderby_dist` (no scan registered) and `sql/87_distance_scan_identity` (scans
exist but none owns the query). No ADR recorded that decision before this addendum;
the record was the code comment in `bm25_distance_for_query` and ARCHITECTURE.md.

Issue #245 narrowed "must not error" without reversing it (PR #262). `ORDER BY col
&@@ <jsonb>` with no `@@@` predicate is not an index path, so no scan parses the
tree, and a structurally invalid one, such as a 65-leaf tree or `{"nonsense": 1}`,
was accepted silently although the index path rejects it at rescan: the same query
text succeeded or raised depending on whether a `@@@` was present. Decided:

- **Validate on the fall-through.** The resolver now reports whether a scan owned the
  row. When none did, `bm25_distance_jsonb` runs `bm25_query_validate`, the same parse
  and flatten the index path runs, in a structural-only mode, and raises the index
  path's SQLSTATE and message. A valid tree still projects `+inf`. The check covers both
  fall-through cases (no scan at all, and scans that rank a different query), because
  they are the same return. The last valid tree is cached by value in `fn_extra`, so a
  constant right-hand side is parsed once per query rather than once per row. The text
  `&@@` form is unchanged.
- **Field names are not resolved.** The validator's field configuration is NULL, which
  type-checks a `"field"` value but does not resolve it, since a field name can only be
  judged against an index. An unknown field is therefore still accepted off the index.
- **Rejected: a NOTICE or WARNING** on the fall-through, because it would fire on the
  cases `sql/50` and `sql/87` pin as deliberate; and **a planner-side check.**

Documentation that said an `&@@` `ORDER BY` forces the index was corrected in the same
change: without a `@@@` predicate the query plans as Seq Scan + Sort and every row's
distance is `+inf`.

Residuals, none fixed:

- The validator skips the checks `bm25_rescan_parse_jsonb` applies after flatten, so a
  tree with a `must_not` phrase leaf, which the index path rejects, is accepted off the
  index and projects `+inf` (#273, closed as a documented residual).
- The cache keys on the jsonb bytes only, but validity also depends on the three
  `PGC_SUSET` wildcard guardrail GUCs (`bm25_wildcard_max_pattern_length`,
  `bm25_wildcard_min_prefix`, `bm25_wildcard_max_stars`). Tightening one between two
  evaluations at the same call site keeps returning the cached "valid" answer for a
  tree that would now be rejected (#272, open).
- The text `&@@` without a `@@@` predicate still returns `+inf` for every row in an
  arbitrary order with no signal, as does a valid jsonb tree. Whether to error, warn or
  check in the planner is a design decision, not a code tweak (#271, open).

## Addendum (2026-10-04, #271 and #272)

The 2026-09-29 addendum above lists two residuals as open. Both are settled.

**#272 is fixed (PR #282).** The off-index jsonb validation cache keyed on the tree's
bytes only, but a wildcard leaf's validity also depends on three `PGC_SUSET` guardrail
GUCs (`bm25_native.wildcard_min_prefix`, `bm25_native.wildcard_max_pattern_length` and
`bm25_native.wildcard_max_stars`). The cache now stores the three values next to the
bytes and reuses a cached valid verdict only when the bytes and all three are unchanged,
so a superuser who tightens one between rows of one statement gets the rejection a
fresh evaluation gives. `sql/115` has three cases, one per GUC, each evaluating one
constant tree over two rows of a single statement and tightening the GUC between the
rows with `set_config`; they fail on the previous build, and a mutation that drops any
one GUC from the key fails its case.

**#271 is decided as documented, and closed.** The `+inf` fall-through stays, as
decided under issue #151 (TEXT-01) in the addendum above: a text `&@@` with no `@@@`
predicate, and a valid jsonb tree, still project `+inf` for every row in arbitrary
order with no signal. A NOTICE and a planner-side check stay rejected; the addendum
above already rejected them. The behavior is documented in README.md (the `&@@`
operator entry), ARCHITECTURE.md and THEORY.md, and the 2026-09-29 addendum's residual
list should be read with this one: its #272 and #271 items are closed, and #273 remains
a documented residual.
