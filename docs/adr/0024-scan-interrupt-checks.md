---
id: 0024
title: Every data-scaling loop on the scan path carries a CHECK_FOR_INTERRUPTS, with a CI floor
date: 2026-07-29
status: Accepted
summary: The extension had no interrupt check anywhere, so scans ignored cancellation for the whole in-AM build; 41 checks now sit at the top of every loop whose trip count scales with data, and ci.yml fails if that count drops.
---

# 0024. Every data-scaling loop on the scan path carries a CHECK_FOR_INTERRUPTS, with a CI floor

## Context

`grep -rn CHECK_FOR_INTERRUPTS src/` returned nothing across all 21 `.c` files. A
PostgreSQL backend only acts on cancellation, `statement_timeout`,
`pg_terminate_backend` and standby recovery-conflict signals at an interrupt
point, so a bm25 scan honored none of them until it returned to the executor —
and the AM builds its entire ranking before yielding the first row.

Measured on PG 18.3, 400k documents, before the fix (review ref H3, issue #43):

| | |
|---|---|
| `SELECT count(*) … WHERE body @@@ 'alpha'` | 4817 ms |
| same, with `statement_timeout = '5ms'` | cancelled after **4794 ms** |

A ~1000x overshoot, scaling linearly with corpus size. On a hot standby the same
backend cannot be signalled to resolve a recovery conflict, so WAL replay stalls
past `max_standby_streaming_delay` instead of terminating the query.

## Decision

Add `CHECK_FOR_INTERRUPTS()` at the top of every loop on the scan path whose trip
count scales with data: page-chain walks, per-segment loops, per-query-term loops,
`hash_seq_search` drains, and the WAND driver. 41 checks across
`bm25_scan.c` (26), `bm25_seg_read.c` (10) and `bm25_wand.c` (5).

Loops bounded by a compile-time constant are deliberately left alone —
`MAX_FIELDS` (32) and `BM25_POSTINGS_PER_BLOCK` (128) loops always nest inside one
of the guarded loops, so a check there buys nothing and adds noise.

Placement is after each block's declaration prologue, because the tree builds with
`-Wdeclaration-after-statement`; the compiler is the gate on that, and the build
is warning-clean.

Several checks sit inside a buffer content-lock region. That is safe and is how
core unwinds: an `ereport` from an interrupt releases pins and LWLocks through the
resource owner and `LWLockReleaseAll` at abort, exactly as an OOM from `palloc`
already could at these same sites. No check was added inside a critical section,
where `ProcessInterrupts` defers anyway.

After the fix, same shape as the measurement above: a 4799 ms scan honors a 5 ms
timeout in **8 ms**; the WAND and exhaustive ranked paths cancel in 7 ms and 6 ms.

## Alternatives considered

- **Check only in the outermost loops** — a single query term across many segments,
  or one segment with a long posting chain, still runs unbounded between checks.
  The per-page loop in `bm25_seg_scan_postings` is where scan time actually goes.
- **Check only in `bm25_seg_scan_postings`** — covers posting decode but not the
  df-summing prologue, the pending-chain walks, or the accumulator drain, each of
  which is a full pass over the data on its own.
- **A periodic check driven by a counter** (every N iterations) — the standard
  micro-optimization, rejected because `CHECK_FOR_INTERRUPTS` is already a test of
  a volatile flag, and a counter adds state and a tuning constant to save nothing
  measurable. No suite timing moved outside noise.
- **`vacuum_delay_point` everywhere** — wrong tool on the query path; it also
  throttles, which a foreground scan must not do. It belongs on the VACUUM path
  and is handled separately.

## Consequences

- Ranked and `@@@` scans are cancellable, `statement_timeout` is honored to within
  milliseconds, and a standby can resolve a recovery conflict against a bm25 query.
- **The regression suite cannot prove this.** The defect was latency, not outcome:
  a pre-fix scan still ended with "canceling statement due to statement timeout",
  just far too late, and `pg_regress` compares output rather than wall clock. So
  the property is pinned in two halves — `sql/66_scan_interrupts` asserts scans are
  interruptible *at all* (the checks are on paths that execute, nothing swallows
  the interrupt, and the backend is healthy afterward), and a grep floor in
  `ci.yml` fails the build if the check count drops below 41. Raise the floor when
  adding checks.
- `sql/66_scan_interrupts` costs ~1.2 s of suite time and is the only timing-
  sensitive suite in the tree. Its header records the measured margins (~320x on
  `@@@`, ~9x on the two ranked paths) so a future maintainer can see how much room
  there is; the failure direction requires a machine ~9x faster than current Apple
  silicon, and CI runners are slower, which widens every margin.
- Maintenance paths — `bm25_bulkdelete`, seal, merge and the FSM reclaim walks —
  are **not** covered by this record. VACUUM is addressed separately (it needs
  `vacuum_delay_point`, not a bare check, so autovacuum throttling engages).

## Addendum (2026-08-11)

"Raise the floor when adding checks", above, had fallen into arrears, and the CI
step's per-file floors were audited against the tree for the first time since
they started accumulating.

Three gaps, all of the same shape — a file whose checks were covered by the
aggregate floor alone. The aggregate cannot distinguish "a check was deleted
here" from "a check was added somewhere else", so any PR that does both in one
commit passes it:

- **`src/bm25_wand.c` had no per-file floor at all**, and it is the least
  defensible of the three: WAND is the *default* ranked-scan path
  (`bm25_wand_top_k > 0` out of the box), so its five checks are the ones an
  ordinary user's `ORDER BY` executes, while every file already pinned is a
  maintenance or debug path. Floor 5: `wand_cursor_sweep_global_ub`,
  `bm25_wand_cursor_next_geq`, `bm25_wand_segment` (the driver loop, bounded by
  *k* only once the heap fills, so a low-selectivity query walks the whole
  posting space inside it), and two in `bm25_wand_build_ranking`.
- **`src/bm25_handler.c` had none either.** The step's own comment names the
  `@@@` standalone evaluator's probe loop as one of ADR 0034's two additions, but
  only the `bm25_tokenize.c` half was ever floored. Floor 2: `bm25_match`'s
  per-token bsearch loop and `bm25_bulkdelete`'s per-segment loop — the latter
  distinct from the separately floored `BM25_VACUUM_DELAY_POINT`, which honours
  the cost budget rather than making cancellation land.
- **`src/bm25_seg_read.c` had drifted to nine checks against a floor of eight.**
  Raised to 9. Slack in a floor reopens the same hole as a missing floor.

All three were negative-controlled in the way that matters for this specific
mechanism: remove one check from the file under test *and add one elsewhere*, so
the aggregate still passes at 59 and only the per-file floor can object. Each
failed with its own error; none reached the aggregate.

No source changed. This is bookkeeping on the gate, not on the property.

## Addendum (2026-10-05, PRs #331-#350)

- **The floors count code, not text.** `ci/count_calls.py` (reusing
  `test/check_source_style.py`'s lexer) blanks comments and string literals before
  counting, so a call named in a comment no longer counts; `ci/check_scan_scratch.py` uses
  the same helper (#309 CI-14, PR #348). The floors were set to the post-batch counts
  (aggregate 99, both forms), so every deletion trips a floor.
- **Lesson:** a PR that deletes a check lowers its own floor, in the same PR, and says why.
  #313 removed two dead checks under a held LWLock (XCUT-09) and #303's key-config move took
  one out of `bm25_wand.c`; each lowered the per-file floor it touched.
- **Interrupt density has a cost on a standby.** The ranking build's many checks are what made
  a recovery conflict likely to land inside its retry subtransaction, where core escalates it
  to FATAL (ADR 0121).
- New checks: the WAND segment scan checks before each cursor open, and the first cursor
  sort is a stable qsort rather than an insertion sort that was quadratic and uncancellable on
  a reversed bag-of-words query (#305 XCUT-07); `bm25_debug_block_impacts` and
  `bm25_debug_tombstone` loops; the KEYMAP writer per page (ADR 0041); the snippet hit loop
  (ADR 0034).
