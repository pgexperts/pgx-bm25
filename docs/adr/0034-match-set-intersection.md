---
id: 0034
title: The @@@ standalone evaluator intersects token sets by sort plus binary search
date: 2026-07-29
status: Accepted
summary: bm25_match compared every query token against every document token in a nested loop with no interrupt point, so unbounded user text could burn days of uncancellable CPU; it now sorts the smaller token set and probes it with the larger, and both that loop and bm25_analyze's per-word-run loop gained CHECK_FOR_INTERRUPTS.
---

# 0034. The @@@ standalone evaluator intersects token sets by sort plus binary search

## Context

`bm25_match` is the `@@@` operator's standalone evaluator — the bare-filter / seqscan
path, reached whenever the planner applies `col @@@ 'q'` without a bm25 index answering
the query. It tokenizes both operands and returns true if the token sets intersect.

It did that with a doubly nested scan and no interrupt point (review ref H15, issue #54):

```c
for (qi = 0; qi < nq; qi++)
    for (di = 0; di < nd; di++)
        if (dt[di].len == qt[qi].len && memcmp(...) == 0)
            PG_RETURN_BOOL(true);
```

Both operands come straight from SQL. The only cap is the tokenizer's own
`palloc(sizeof(BM25Token) * (textlen/2 + 1))`, which admits roughly 87 MB per side and
~13M tokens. The only early exit is a match, so the all-miss case is simultaneously the
worst case and the common case for a nonsense query: ~1.8e14 length comparisons at that
ceiling.

The distinguishing defect is not the CPU burn — a `generate_series` bomb burns CPU too,
and is cancellable. It is that there was **no interrupt point anywhere in the path**:
not in the loop, not in `memcmp`, and — this is the part the report did not name — not
in `bm25_analyze` either. `SIGTERM` and `SIGINT` are only acted on at a
`CHECK_FOR_INTERRUPTS`, so the backend ignored `pg_cancel_backend`,
`pg_terminate_backend` and `statement_timeout` for the entire run, while holding its
transaction snapshot open and pinning the vacuum horizon cluster-wide. ADR
[0024](0024-scan-interrupt-checks.md) made the *scan* paths cancellable and did not
reach either of these.

`COST 5000` on the SQL declaration discourages the planner from choosing this path but
does nothing about an explicitly written call.

## Decision

**Sort the smaller token set, probe it with the larger.** O(s log s + l log s) replaces
O(nd x nq). Sorting the *smaller* side is what makes the realistic shape cheap — a short
query against a long document sorts the query and spends a handful of comparisons per
document token — and it bounds the sort by `min(nd, nq)` in the pathological case.

**Sort plus binary search rather than a hash.** The report offered either. Token keys are
variable-length `(ptr, len)` pairs, and PostgreSQL's `HTAB` needs a fixed-size key, so a
hash means the truncate-to-N-bytes plus linear-collision-fallback that `bm25_accum`
carries for exactly this reason. `qsort`/`bsearch` over an exact comparator has no such
caveat, and no rehash-on-collision path to get wrong.

The comparator orders by length first, then bytes. That keeps `memcmp` well-defined (it
only ever compares equal-length runs, so no shortest-common-prefix rule is needed) and
makes equality under the order exactly the `(len, bytes)` equality the old test used —
which is why the answers are unchanged.

**`CHECK_FOR_INTERRUPTS` in the probe loop, and in `bm25_analyze`.** The probe loop check
is per document token. The tokenizer check is per word run, and it is the more important
of the two: `bm25_analyze` is the funnel every ingest and query path goes through, so its
loop was an uninterruptible stretch proportional to input size for `ambuild`,
`bm25_insert` and the scan paths as well, not only for this operator. Fixing only the
site the report named would have left a multi-second uncancellable window on the very
expression the report used as its repro.

## Alternatives considered

- **Cap the operand length and raise an error above it.** Bounds the work, but it changes
  the operator's semantics and would reject documents the index itself accepts. The
  tokenizer's `MaxAllocSize` ceiling is an implementation limit, not a contract worth
  promoting to a user-visible one.
- **Sort both sides and linear-merge.** O(nd log nd + nq log nq), and elegant, but it
  sorts the large side too — strictly worse than sorting only the smaller for the shape
  that actually occurs, and no simpler.
- **Add the interrupt check and leave the nested loop.** Makes the attack cancellable,
  which is the security-relevant half, and would have satisfied the report's minimum. Not
  taken: it leaves a trivially-written expression that occupies a backend for days until
  someone notices and cancels it, and the algorithmic fix is a dozen lines.
- **A `CHECK_FOR_INTERRUPTS` inside the comparator, to make the `qsort` itself
  interruptible.** Rejected: it runs O(s log s) times to shorten a window that is now
  seconds rather than days, and it would slow every sort. The residual uninterruptible
  stretch is the sort alone, ~3e8 comparisons at the tokenizer ceiling.

## Consequences

- Measured all-miss, development machine (Apple silicon, PG 18.3):

  | tokens/side | before | after |
  |---|---|---|
  | 20k | 390 ms | 16.9 ms |
  | 80k | 6025 ms | 39.5 ms |
  | 800k | ~10 min (extrapolated) | 375 ms |

- `statement_timeout = '1ms'` is now honored on an 800k-token-per-side call, which
  `sql/74_match_intersect` asserts. The margin is ~375x and widens on a slower runner.
- **The answers are unchanged, and that was verified rather than assumed.** The suite's
  semantics half was run against both the pre-fix and post-fix builds and diffed: all 24
  boolean results plus the seqscan-filter queries are byte-identical. The suite covers
  both branches of the smaller/larger choice, empty and punctuation-only operands,
  case folding, stemming, stopwords, duplicate tokens on either side, and
  prefix/suffix non-matches (the comparator's length-first order).
- The interrupt-check floor in `ci.yml` rises 42 → 44, with an additional per-file check
  that `src/bm25_tokenize.c` keeps at least one: the aggregate floor alone would let a
  refactor delete the tokenizer's check and add one elsewhere.
- Every `bm25_analyze` caller is now cancellable in proportion to its input, including
  `CREATE INDEX` and `INSERT` on a very large value. That is a side benefit of fixing the
  funnel rather than the reported site.

## Addendum (2026-10-05, PRs #331-#350)

The same remedy now covers `bm25_snippet`'s hit test (#305 XCUT-06/REGR-04, PR #337), which
compared every field token against every query term in a linear probe with no interrupt
check. The hit set is sorted once per scan and probed with `bsearch` over `bm25_token_cmp`,
now shared from `bm25_handler.c`, and the hit loop checks for interrupts per field token.
Wildcard patterns are globbed per token, at most `BM25_QUERY_MAX_LEAVES` of them (ADR 0123).
`sql/146` bounds the old shape by wall clock (ADR 0123 records that departure).
