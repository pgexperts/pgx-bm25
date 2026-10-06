---
id: 0124
title: A jsonb query tree is validated as written -- closed key sets per node kind, integral slop, a bounded folded boost, and a node cap
date: 2026-10-05
status: Accepted
summary: Each jsonb query-node kind accepts a closed set of argument keys and any other key is a 22023 ERROR with a near-spelling hint; a fractional phrase slop is an ERROR rather than rounded; a scoring leaf's folded boost must lie in [1e-6, 1e6] (must_not leaves exempt); a tree may have at most BM25_QUERY_MAX_NODES = 1024 nodes; the builders stay STRICT and a NULL array element gets its own message.
---

# 0124. A jsonb query tree is validated as written -- closed key sets per node kind, integral slop, a bounded folded boost, and a node cap

## Context

The jsonb query parser (`bm25_query.c`, ADR 0004's "jsonb query object, not a parsed DSL"
entry) read each node's keys by name and ignored the rest (#304 QUERY-02). A typo was
dropped silently: `"mustnot"` removed the exclusion and returned the rows it was written
to exclude, and `"fild"` searched every field. Four neighbouring inputs were accepted and
then did something other than what was written:

- a raw-jsonb `phrase.slop` with a fractional part was rounded by `numeric_int4` (1.7 ran
  as 2, -0.4 as 0 and passed the `< 0` check) (QUERY-05);
- a folded boost that was finite and positive could still underflow `boost * idf` to 0
  (a silent drop) or overflow it (QUERY-01);
- the 64-leaf cap (ADR 0099) bounded shape, not width: leafless `{"boolean":{}}` padding
  was unbounded, and `bm25_query_eval` walks every node for every candidate (QUERY-03);
- a NULL argument to a STRICT builder makes the builder return NULL, and inside a
  `must`/`should`/`must_not` array the error named a symptom ("must be a query node
  object") (QUERY-09).

## Decision

Decided by the user as D9-D13. Landed in PR #334 (#304, #305).

- **Closed key sets (D9).** match `{field, terms}`; term `{field, value}`; phrase
  `{field, phrase, slop, ordered}`; wildcard `{field, pattern}`; boolean
  `{must, should, must_not}`; boost `{weight, query}`. Any other key is an ERROR
  (`ERRCODE_INVALID_PARAMETER_VALUE`, 22023) naming the key, with a "did you mean" hint for
  near-spellings such as `mustNot` or `must-not`. `field` is not a key of boolean or boost.
  The builders emit exactly these keys, and `sql/143` parses every builder's output.
- **Integral slop (D10).** A slop with a fractional part is an ERROR; integral spellings
  such as `2.0` and `1e0` stay legal.
- **Folded boost range (D11).** A scoring leaf's folded boost (the product of its enclosing
  weights) must lie in `[BM25_QUERY_MIN_BOOST, BM25_QUERY_MAX_BOOST]` = `[1e-6, 1e6]`,
  tested once at the leaf in `flatten_recurse` (an intermediate product that left the range
  by a finite factor and came back is legal; one that reached 0 or Inf cannot come back).
  Written as a negated range test so NaN is rejected. Leaves under `must_not` are exempt:
  their boost is never read.
- **Node cap (D12).** At most `BM25_QUERY_MAX_NODES` = 1024 nodes, counted in `parse_node`
  before anything is allocated for the node, SQLSTATE 22023 like the leaf cap.
- **NULL elements (D13).** The builders stay STRICT. A NULL array element gets "array
  element N is NULL" with a hint that a builder returns NULL when any argument is NULL, and
  the docs show raw jsonb (`{"term":{"value":...}}`) for an all-fields leaf, which no
  builder can produce.

## Alternatives considered

- **Ignore unknown keys** (the old behaviour). A typo changes the result set silently.
- **Round slop, or truncate it.** Either widens or narrows the proximity window silently.
- **Check `boost * idf` at use.** Depends on df, so the same query would fail or succeed as
  the corpus drifts; the parse-time range is deterministic.
- **Check the boost per weight in `parse_boost`.** Rejects legal nestings whose product is
  in range.
- **A node cap of about 200** (the size of realistic generated trees). 1024 rejects only
  synthetic unary chains and padding; a tree with no leafless subtrees exceeds 1024 only
  through long unary boost/boolean chains over many of its 64 leaves (up to about 2000
  nodes, per the `BM25_QUERY_MAX_NODES` comment in `src/bm25_query.h`).
- **Make `field` nullable in the builders.** Changes a builder's NULL semantics against ADR
  0015's "NULL query matches nothing" rule, for an all-fields leaf raw jsonb already
  expresses.

## Consequences

- User-visible (release note): unknown keys, fractional slop, an out-of-range folded boost
  and trees over 1024 nodes are ERRORs where they used to run.
- `bm25_debug_query_parse` never flattens, so the boost range does not apply there; the node
  cap does (it is counted in `parse_node`). **Residual:** a floating-point product within a
  few ULP of a range edge can land on either side; the claim is about the folded value as
  computed, not the real-number product.
- ADR 0047 (folded boost) and ADR 0099 (node cap) carry addenda. Pinned by
  `sql/143_query_args_strict`; `sql/83` and `sql/115` changed only in the reworded
  folded-boost error.
