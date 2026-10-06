---
id: 0028
title: bm25_distance and bm25_distance_jsonb are STABLE PARALLEL RESTRICTED, not IMMUTABLE PARALLEL SAFE
date: 2026-07-29
status: Accepted
summary: Both distance functions read backend-local scored-scan state, so IMMUTABLE PARALLEL SAFE was wrong in both halves; STABLE PARALLEL RESTRICTED is the strictest marking that still keeps the amcanorderbyop index path.
---

# 0028. bm25_distance and bm25_distance_jsonb are STABLE PARALLEL RESTRICTED, not IMMUTABLE PARALLEL SAFE

## Context

`bm25_distance(text, text)` and `bm25_distance_jsonb(text, jsonb)` — the functions
behind the `&@@` ordering operator — were declared `IMMUTABLE STRICT PARALLEL SAFE`.
Their body is:

```c
BM25ScanOpaque so = bm25_scored_scan_head();
if (so != NULL) PG_RETURN_FLOAT8(so->cur_orderby_dist);
PG_RETURN_FLOAT8(get_float8_infinity());
```

a read of the per-backend scored-scan registry. The same arguments give different
answers on different rows, and no answer at all outside a scored scan.

The extension script already contradicted itself: `bm25_score(tid)` and the four
`bm25_score_key()` overloads are `PARALLEL RESTRICTED` over the *identical* state,
with the comment "it reads backend-local scan state, which is per-worker and not
shareable."

Both halves of the marking were load-bearing:

- **`IMMUTABLE`** invited constant-folding by `eval_const_expressions`, and made
  the expression legal wherever an immutable expression is required. Verified
  pre-fix: `CREATE INDEX ON dv ((body &@@ 'alpha'))` **succeeds**, producing a btree
  over a scan-state read whose every entry is `+inf` — there is no scored scan
  during an index build — permanently baked into an index that would then answer
  queries from those values.
- **`PARALLEL SAFE`** let a parallel worker evaluate it with no scan state,
  silently returning `+inf` as the sort key.

## Decision

Declare both `STABLE STRICT PARALLEL RESTRICTED`.

`STABLE` rather than `VOLATILE` is a deliberate compromise, and the reason is
measured rather than assumed. `VOLATILE` is what the behavior literally is — the
docs define it as "the function value can change even within a single table scan",
which is exactly this function. But on PG 18.3, marking it `VOLATILE` makes the
planner **refuse the index ordering path**:

```
VOLATILE:  Limit -> Sort -> Index Scan      (rows returned in the WRONG order)
STABLE:    Limit -> Index Scan (Order By)   (correct)
IMMUTABLE: Limit -> Index Scan (Order By)   (correct, but see above)
```

Under `VOLATILE` the inserted Sort evaluates the function outside any scored scan,
so every key is `+inf` and ranking silently breaks — a worse outcome than the
defect being fixed. `STABLE` is the strictest marking that preserves
`amcanorderbyop`, and it is defensible on its own terms: within one index scan the
value is a projection of that scan's current row.

## Alternatives considered

- **`VOLATILE PARALLEL RESTRICTED`** — the honest marking, rejected on the measured
  evidence above: it disables the index ordering path the operator exists to drive.
- **Leave `IMMUTABLE`, document the hazard** — the index-expression case is not
  hypothetical; it builds a permanently wrong index with no error and no warning.
- **`PARALLEL UNSAFE` instead of `RESTRICTED`** — stronger than needed.
  `RESTRICTED` already confines evaluation to the leader, which is the actual
  requirement; `UNSAFE` would additionally forbid parallelism anywhere in the plan.
- **Keep `PARALLEL SAFE` and make the C body detect a worker** — pushes a planner
  contract into a runtime check, and the correct behavior in a worker is "do not run
  here", which is precisely what `RESTRICTED` expresses.

## Consequences

- `&@@` can no longer be used in an index expression, a materialized view index, or
  anywhere else demanding an immutable expression. This is a **behavior break** on a
  1.0 surface, and it is the point: every such use was silently recording `+inf`.
- Plans that previously parallelized a `&@@` query below the Gather no longer will.
  Those plans were producing `+inf` sort keys in workers.
- A `CHECK` constraint using `&@@` is still accepted — PostgreSQL does not enforce
  volatility there — so that part of the original finding is not closed by this
  change. Noted rather than worked around; a `CHECK` over scan state is
  meaningless either way, and there is no marking that prevents it.
- `sql/70_distance_volatility` pins the catalog markings, that no scan-state
  function is `PARALLEL SAFE`, that both index-expression attempts are refused
  (red/green — pre-fix they succeeded), that the index Order By path survives, and
  that the value still resolves per row rather than folding to a constant.
- **Issue #42's body describes a different finding** (ACL checks on regclass entry
  points) than its title and `FINDINGS.md` line 333, which both describe this
  volatility defect. The ACL work is [0029](0029-mutating-entry-point-ownership.md),
  driven by issue #45, whose own suggested fix is a shared owner-check helper across
  every mutating entry point.
