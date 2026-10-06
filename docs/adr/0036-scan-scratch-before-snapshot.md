---
id: 0036
title: A scan path opens its scratch context before taking the snapshot
date: 2026-07-29
status: Accepted
summary: bm25_scan_snapshot's catalog copy and bm25_analyze's tokenizer output were allocated in the query or subtransaction context and never freed, so a rescanned scan grew memory without bound; all three rescan-driven paths now create their scratch context before the snapshot rather than after the corpus-stat prologue.
---

# 0036. A scan path opens its scratch context before taking the snapshot

## Context

`bm25_scan_snapshot` palloc's a copy of the entire live segment catalog, and its header
contract in `bm25.h` is explicit about who owns it: *"Allocated in the caller's
CurrentMemoryContext at bm25_scan_snapshot time; the caller must keep that context alive
for the scan (or pfree segs before resetting it)."* No caller did either
(review ref H17, issue #56). `bm25_analyze`'s lowercased copy, token array and per-lexeme
copies leaked the same way.

Which context that landed in, and how long it therefore lived, differed by path:

- **`bm25_load_if_needed`** (the `@@@` filter path) runs with `CurrentMemoryContext ==
  estate->es_query_cxt`, reclaimed only at end of **query**. It pfree'd `col.arr` and
  nothing else.
- **The two ranking builders** each created a `scratchctx`, but *after* the snapshot and
  the whole corpus-stat prologue (`pending_global_stats`, `bm25_scan_load_fieldcfg`,
  `field_corpus_stats`). So all of that landed in the enclosing subtransaction context and
  was reclaimed only at end of **transaction**. The WAND builder's comment even named
  `snap.segs` as living in its scratch context — while the code allocated it three
  statements before that context existed. A comment that contradicted its code.

Either way the growth is unbounded *within one statement*, which is what matters: a
parameterized nested loop with a bm25 scan on the inner side calls `bm25_rescan` once per
outer row, so the leak repeats per outer row.

Measured on a 150k-rescan shape (development machine, PG 18.3), backend RSS during the
statement: **16.8 MB → 42.6 MB, monotonic, still climbing when the statement ended.** The
report's own measurement, against a larger index, reached **1.85 GB in 120 s with no
plateau** and had to be terminated rather than completed.

## Decision

**Create the scratch context before the snapshot, in all three rescan-driven paths.** For
the two builders that is a move, not an addition: the context, its parenting rationale and
its delete already existed and were correct — only its position was wrong. For
`bm25_load_if_needed` it is a new context, deleted at each of the three exits (empty token
list, empty result, normal), replacing the two `pfree(col.arr)` calls.

The builders' existing parenting rationale is preserved verbatim and still holds: the
context is parented on the *current* (subtransaction) context rather than `scanctx`, so a
retryable `seg_gen` abort reclaims it for free, and the results are copied into `scanctx`
— outside the subtransaction — before the explicit delete on the success path.

**No `PG_TRY`**, consistent with [0033](0033-tokenizer-scratch-contexts.md): the scratch
context is a child of a context that dies on error anyway, so an error discards it without
help.

**The seven remaining `bm25_scan_snapshot` call sites are deliberately left alone.** They
are all one-shot debug SRFs reached from SQL with a `regclass` argument, so their snapshot
dies with the statement's own context and cannot repeat. Converting them would be churn
with no bound to improve.

## Alternatives considered

- **`pfree(snap.segs)` and the token array at each return, per the report's second
  suggestion.** Rejected: `bm25_load_if_needed` alone has three exits, `bm25_analyze`
  returns an array of pointers into separately-palloc'd lexeme copies (so "free the
  tokens" is a loop, not a `pfree`), and the next allocation added to either helper
  silently reopens the leak. An arena cannot go stale.
- **Change `bm25_scan_snapshot` to allocate in a context it owns.** Tempting, since the
  contract it documents is the thing every caller got wrong. Rejected: the snapshot's
  correct lifetime genuinely varies by caller — the builders need it until their
  per-segment header sweep, the filter path until its dedupe, the debug SRFs until they
  return — so the function cannot pick one. Making the *callers* right, and adding a CI
  check that they stay right, keeps the contract where the knowledge is.
- **Allocate the snapshot in `scanctx`.** It would stop the per-rescan growth, since
  `bm25_rescan` resets `scanctx`. Rejected: it inverts the builders' deliberate parenting
  decision (a `seg_gen` retry would then orphan the copy under the longer-lived context
  until `endscan`), which ADR-worthy reasoning already settled the other way.

## Consequences

- Measured on the same 150k-rescan shape: **16.8 → 42.6 MB becomes 14.9 → 18.4 MB**, and
  the statement completed faster (~20 s vs ~30 s) for doing less allocation. The residual
  ~3.4 MB is small, does not scale like the leak did, and is not attributed here — it is
  as likely to be catalog/plan cache warm-up over 150k rescans as anything in this path.
- **The fix introduces a use-after-free hazard, and that is what the new test targets.**
  The snapshot now lives in a context that is explicitly deleted, so any code reading
  `snap.segs` after that delete is undefined behavior. It was verified by reading that
  `snap` is last touched before the delete on all three paths (the builders' per-segment
  header sweep, the filter path's dedupe), but the durable guard is mechanical: the
  `hardening` CI job builds PostgreSQL with `--enable-cassert`, which implies
  `CLOBBER_FREED_MEMORY`, so a read of freed scratch shows up as clobbered bytes rather
  than as a lucky pass. `sql/76_rescan_scratch` is deliberately rescan-heavy *and*
  multi-segment so `snap.segs` is a real array, and it asserts that all 200 iterations
  produce one identical ranked set (`count(DISTINCT ids) = 1`) on both the exhaustive and
  the WAND builder, plus exact row counts on the filter path and coverage of the two
  empty-result exits.
- The ordering is enforced by `ci/check_scan_scratch.py`, run as a gating CI step. A grep
  cannot express "A appears before B within this function", which is exactly the property
  at issue, so the check parses the file and compares positions. **It was verified to fail
  on the pre-fix tree for all three call sites** — a floor that does not fail on the bug
  it describes is decoration.
- The WAND builder's comment now matches its code.

## Addendum (2026-10-05, PRs #331-#350)

In recovery the ranking build no longer runs inside an internal subtransaction (ADR 0121),
so the scratch contexts this record creates before the snapshot are children of the
caller's context there, not of a subtransaction's. That is safe: every branch deletes them on
success, statement abort reclaims them on error, and everything the build keeps is allocated
in `so->scanctx` either way. On the primary nothing changed. The retry wrapper also releases
a failed attempt's arrays now (#313 XCUT-15, PR #345); residual: if a new array lands at the
old address the release is skipped and the arrays live until endscan.
