---
id: 0099
title: The 64-leaf cap is enforced at parse time, not only at flatten
date: 2026-09-27
status: Accepted
summary: Since PR #183 (issue #68) bm25_query_count_leaf rejects a jsonb query tree at its 65th leaf as parse_leaf builds it, rather than only in flatten_recurse after the whole tree is allocated, which is observable as a new cap on bm25_debug_query_parse and as a different, sometimes differently-coded, error for trees invalid in more than one way, while flatten_recurse keeps its now-unreachable check as a guard for future callers.
---

# 0099. The 64-leaf cap is enforced at parse time, not only at flatten

## Context

Issue #68 (the grouped L5 low-severity trust-boundary review; its leaf-cap item was
fixed together with #58 in PR #183, commit `6513e8c`, merged to `main` as `0b7c89c` on
2026-08-20) found that `BM25_QUERY_MAX_LEAVES` (64, a `#define` in
`src/bm25_query.h`) was enforced only in `flatten_recurse` (`src/bm25_query.c`), which
runs after `bm25_query_parse` has already walked the whole jsonb query object and
allocated a `BM25Query` node for every leaf and boolean/boost node in it. Parse-time
recursion depth was already bounded by `BM25_QUERY_MAX_DEPTH` (32); breadth was not. A
wide-but-shallow query object such as `{"boolean": {"should": [{match: ...} x 100000]}}`
allocated all 100000 leaf nodes before flatten's check ever ran. That allocation was
bounded by the jsonb value's own size and was cancellable (an interrupt check exists on
this path), so it was not unbounded in the strict sense -- but "the cap exists" and "the
cap runs before the work it bounds" are different claims, and only the first held.

ADR 0076 (`docs/adr/0076-accumulator-keys-on-the-full-term.md`) records the sibling half
of the same PR -- the accumulator's term-key change -- and is where this decision was
first written down, but only in passing: two bullets of its Consequences list note that
`bm25_debug_query_parse` gained the cap and that the parse-time check reuses flatten's
SQLSTATE, under a title and Context that are both about the accumulator's hash key.
Nobody investigating the query parser's leaf cap would find it there. This record is
retroactive (issue #227) and is the dedicated one: it exists so the query-parser
decision is indexed under a title that names it, with an addendum on ADR 0076 pointing
here.

ADR 0003 (`docs/adr/0003-architecture.md`, in its M6 query-tree bullet) records the cap's
value (`BM25_QUERY_MAX_LEAVES`=64), but not this timing change or either of its
observable effects. (ADR 0068, `docs/adr/0068-indexed-column-type-gate.md`, is unrelated
to this record despite sharing its number with GitHub issue #68 by coincidence -- it
documents where indexed-column type checks run relative to `bm25_build`/`bm25_insert`,
not the query leaf cap.)

## Decision

Count leaves as they are parsed, not only when the tree is later flattened.
`bm25_query_count_leaf` (`src/bm25_query.c`) increments a leaf counter and raises the
same error flatten already used (`ERRCODE_INVALID_PARAMETER_VALUE`, "bm25: query has
more than %d leaf clauses (must + should + must_not)") the moment it would exceed
`BM25_QUERY_MAX_LEAVES`. It is called first thing in `parse_leaf`, the single point
where every `BM25Q_MATCH`/`BM25Q_TERM`/`BM25Q_PHRASE`/`BM25Q_WILDCARD` leaf node is
constructed -- before that leaf's own field, text, slop, or wildcard validation; the
`nleaves` counter is threaded as an `int *` down through `parse_node`, `parse_boolean`,
`parse_boost`, and `parse_node_array`, so every leaf reaches the one counting site
regardless of where it sits in the boolean/boost nesting. A tree with more than 64
leaves is now rejected at the 65th leaf during parse, instead of only after the whole
tree has been built.

`flatten_recurse` keeps its own bound check (in the leaf-kind case of its switch):
`if (*n >= max) ereport(ERROR, ...)`. It runs before `node->leaf_bit = *n` and
`leaves[*n] = node`, but not immediately before either -- the boost-fold-to-zero/overflow
check (`!isfinite(boost_acc) || boost_acc <= 0.0`) sits between the leaf-count check and
the assignment. `max` is a parameter, not the literal constant, but every call site
passes `BM25_QUERY_MAX_LEAVES` (the three call sites are listed below). The check bounds
two things, not one: `node->leaf_bit`, later shifted into a `uint64` presence mask in
`bm25_query_eval` (valid only for 0..63), and the write `leaves[*n] = node` itself, into
each caller's fixed-size `BM25Query *leaves[BM25_QUERY_MAX_LEAVES]` stack array -- the
second of those is a memory-safety guard on the array write, not only a range guard on
`leaf_bit`.

That check is currently unreachable, and untested. `parse_leaf` is the only constructor
of leaf `BM25Query` nodes (`parse_boolean` and `parse_boost` build the interior
BOOLEAN/BOOST nodes, and nothing outside `bm25_query_parse`'s descent builds any node),
and `bm25_query_count_leaf` runs unconditionally inside it. `bm25_query_flatten` has
three call sites, and every one flattens a tree that `bm25_query_parse` produced against
the same constant:

- `bm25_debug_query_flatten` (`src/bm25_debug.c`) calls `bm25_query_parse` and flattens
  its result immediately, in the same function.
- `bm25_rescan_parse_jsonb` (`src/bm25_scan.c`) does the same, storing the parsed tree
  in `so->qtree`.
- `bm25_scan_build_ranking_exhaustive` (`src/bm25_scan.c`) re-flattens `so->qtree`, which
  is only ever assigned from that `bm25_query_parse` call in `bm25_rescan_parse_jsonb`
  (every other assignment resets it to NULL).

(`bm25_debug_query_parse` also calls `bm25_query_parse`, but never flattens, so it is
not a flatten caller.) Any tree that reaches `flatten_recurse` has therefore already
passed the identical count against the identical constant during its own parse --
flatten's `*n >= max` can never be the check that actually fires.
`sql/48_m6_builders.sql`'s 65-leaf case, exercised through `bm25_debug_query_flatten`,
used to be the assertion that exercised flatten's check directly; after this change it
errors during the `bm25_query_parse` call, before `bm25_query_flatten` is even reached,
so it now exercises the parse-time check instead, and nothing in the suite exercises
flatten's own check.

It is kept anyway. `bm25_query_flatten`'s signature (`BM25Query *root, BM25Query
**leaves, int max`) does not itself guarantee `root` came from `bm25_query_parse`, and
nothing prevents a future caller from building or editing a tree by hand, or flattening
it against a smaller `max` than it was parsed against. The check is what stands between
such a caller and both a `leaf_bit` shifted past 63 and a write past the end of its
`leaves[]` array. Removing a bound on a raw array write and a bitmask index on the
strength of an invariant ("every tree reaching flatten was just parsed by
`bm25_query_parse` with the same constant") that the type system cannot check is judged
the wrong trade, even though it currently means the check is dead code with no test
covering it.

The parse-time check reuses flatten's message and its `ERRCODE_INVALID_PARAMETER_VALUE`
(22023) SQLSTATE rather than a distinct one. The reason is continuity: flatten's
pre-existing leaf-cap check already used 22023, and matching it exactly, rather than
reusing only the message text, means a client branching on SQLSTATE sees no change for a
tree that is invalid on leaf count alone, even though the error now fires earlier. The
choice also agrees with the rest of `src/bm25_query.c`, though that is not why it was
made: the depth cap (`BM25_QUERY_MAX_DEPTH`) and the `phrase.slop` upper bound
(`BM25_MAX_PHRASE_SLOP`) raise 22023 too, and the file's only
`ERRCODE_PROGRAM_LIMIT_EXCEEDED` (54000) sites are the two checks in
`validate_wildcard_pattern` against the `PGC_SUSET` GUCs
`bm25_native.wildcard_max_pattern_length` and `bm25_native.wildcard_max_stars`. The leaf
cap, by contrast, is not configurable at all: it is a compile-time constant tied to the
width of the `uint64` presence mask, which also sizes every caller's `leaves[]` array.

## Alternatives considered

- **Leave the cap only in `flatten_recurse`.** Rejected: this is the status quo issue
  #68 found insufficient. A query built to be wide rather than deep pays for allocating
  the entire tree before the existing check has a chance to reject it.
- **A distinct SQLSTATE (`ERRCODE_PROGRAM_LIMIT_EXCEEDED`) for the parse-time check**,
  on the reasoning that it is a resource limit rather than a query-shape complaint.
  Rejected: matching flatten's existing `22023` exactly is what makes the timing change
  invisible to a client branching on SQLSTATE for a leaf-count-only failure; a different
  code for what is otherwise the same user-visible limit reintroduces the very
  difference enforcing the cap earlier is trying to avoid.
- **Remove `flatten_recurse`'s check now that parse enforces the cap first.** Rejected,
  even though the check is currently unreachable through every flatten call site:
  `bm25_query_flatten`'s own signature does not guarantee its input tree came from
  `bm25_query_parse`, and the check is what stands between a future caller and both a
  `leaf_bit` shifted past 63 and a write past the end of a fixed `leaves[]` array.
  Removing it trades a compiler-invisible invariant for one `ereport` call.

## Consequences

For a caller that both parses and flattens a query tree -- the index scan path via the
`@@@`/`&@@` operators (`bm25_rescan_parse_jsonb`), and `bm25_debug_query_flatten` -- the
set of accepted and rejected trees is unchanged: a tree over 64 leaves was already
rejected, and still is, with the same message and SQLSTATE when leaf count is its only
defect. `sql/48_m6_builders.sql` (its "Leaf-cap" block; mirrored in
`expected/48_m6_builders.out`) pins 64-vs-65 leaves through `bm25_debug_query_flatten`;
as covered under Decision, the 65-leaf rejection now happens inside `bm25_query_parse`,
not in `flatten_recurse`, though the suite cannot observe that distinction.

`bm25_debug_query_parse` (`src/bm25_debug.c`) is the exception to "unchanged", and the
first observable effect. It renders a parsed tree as text and never calls
`bm25_query_flatten`, so before this change it had no leaf cap at all and would render an
arbitrarily wide tree (measured: a build of `6513e8c^` renders a 65-leaf tree). After
this change it shares the parse-time counter and rejects a 65-leaf tree the same way the
scan path does -- a query this function used to accept and return now errors.
`sql/98_unbounded_input_loops.sql` (the `bm25_debug_query_parse` pair at the end of its
"jsonb leaf cap timing" block; mirrored in `expected/98_unbounded_input_loops.out`)
asserts both halves directly: 64 leaves still succeed (`parse_64_ok = t`), and 65 leaves
now error where they previously would have rendered successfully.

The second observable effect: a tree that is invalid for more than one reason may now
report the 22023 leaf-cap error where it previously reported a different one -- under a
different SQLSTATE, or under 22023 with a different message. The parse-time count fires
as soon as the 65th leaf is constructed, in depth-first parse order, before parsing or
flattening reaches whatever else makes the tree invalid. Measured through
`bm25_debug_query_flatten` against builds of `6513e8c^` (whose `src/bm25_query.c` has no
`nleaves` counter or `bm25_query_count_leaf` anywhere -- the parse side of the cap did
not exist) and of the current tree:

| Tree | Pre-fix (`6513e8c^`) | Now |
| --- | --- | --- |
| 65 `match` leaves, then a 66th naming an unknown field | 42703, unknown search field (`parse_field_id`) | 22023, leaf cap |
| 65 leaves, then a 66th `wildcard` over a `wildcard_max_*` GUC (1000 stars trips the pattern length; `abc` + 20 stars trips the star count) | 54000 (`validate_wildcard_pattern`) | 22023, leaf cap |
| 65 leaves, then a `boost` with weight `1e400` | 22003, out of range for double precision (`numeric_float8` in `parse_boost`) | 22023, leaf cap |
| a `boolean` with 65 `must_not` leaves and no `must`/`should` | 22023, "must have at least one positive (must/should) clause" (`parse_boolean` checks this only after parsing all three arrays) | 22023, leaf cap |
| `boost(1e-300, boost(1e-300, should: 65 leaves))` | 22023, boost weights fold to 0 (`flatten_recurse`, at the first leaf it visits; each weight alone passes `parse_boost`) | 22023, leaf cap |
| 65 leaves, then a branch nested 40 booleans deep | 22023, query tree nested too deep | 22023, leaf cap |
| a branch nested 40 booleans deep, then 65 leaves | 22023, query tree nested too deep | 22023, query tree nested too deep (unchanged) |

The unknown-field case was also measured through the `@@@` index scan
(`enable_seqscan = off`), with the same result: 42703 before, 22023 now. The last two rows
show the order dependence: the count trips only if the 65th leaf is reached before the
other defect in depth-first parse order; a defect that parse meets first still wins.

In every case the tree was invalid before this change and remains invalid after it; for
a caller that parses and then flattens, nothing becomes legal or illegal that was not
before (`bm25_debug_query_parse`, above, is the one caller whose accepted set shrank).
What changes is which of several simultaneous defects a multiply-invalid tree is told
about, and in some cases the SQLSTATE a client branching on it sees. No suite pins this
reordering; it is recorded here because it is real.

## Addendum (2026-10-05, PRs #331-#350)

A second parse-time cap now sits beside the leaf cap: at most `BM25_QUERY_MAX_NODES` = 1024
nodes of any kind, counted in `parse_node` before the node is allocated, SQLSTATE 22023 for
the reason this record gives for the leaf cap (D12, #305 QUERY-03, PR #334; ADR 0124). The
leaf cap bounded shape, not width: leafless `{"boolean":{}}` padding was unbounded and
`bm25_query_eval` walked it for every candidate. Like the leaf cap it binds
`bm25_debug_query_parse`. The prose above that names "the boost-fold-to-zero/overflow check
(`!isfinite(boost_acc) || boost_acc <= 0.0`)" and the table row whose pre-fix error is "boost
weights fold to 0" are outdated: the fold check is now the closed range [1e-6, 1e6] (ADR 0124),
and its message reads "folds to ... outside the supported range". The ordering argument is
unchanged.
