---
id: 0098
title: Benchmark, coverage and static-analysis CI legs are report-only
date: 2026-09-23
status: Accepted
summary: CI gains a benchmark job, an lcov coverage job and a cppcheck job, each continue-on-error at job level with nothing depending on it and no threshold, because their signals are either too noisy on shared runners (benchmarks) or not yet measured (coverage floor, cppcheck noise) to gate on.
---

# 0098. Benchmark, coverage and static-analysis CI legs are report-only

## Context

Issue #156 raised three gaps. BLD-03: `bench/README.md` said baselines were "compared on
each run to surface persistent regressions", but nothing in CI touched `bench/`, and
`bench/baselines/` was updated by hand from local runs. BLD-06: nothing measured how much
of `src/*.c` the suites reach, so a coverage regression was invisible. BLD-07: there was
no static analysis at all. (BLD-08, valgrind, is not taken up here; #156 itself ranks it
below the ASan job that ADR 0090 landed.)

The project's standing CI policy is that only correctness gates: build, regression, TAP,
and the hardening, folding-collation and ASan legs. Performance numbers from shared
runners do not, because a throttled or noisy-neighbour VM can swing an allocation-heavy
benchmark 2x or more on identical code.

## Decision

Three new jobs in `.github/workflows/ci.yml`, all `continue-on-error: true` at job level,
with no job depending on them and no pass/fail threshold:

- **`bench`** runs the three `bench/*.sh` scripts with their sizes turned down through the
  scripts' own positional arguments (the scripts are unchanged, so a local run still uses
  their real defaults) and uploads the CSVs as an artifact. It never writes
  `bench/baselines/`; a baseline still comes from a local run on real hardware.
- **`coverage`** builds with `--coverage` appended to `PG_CFLAGS` (appended so
  `-ffp-contract=off`, which keeps WAND and the exhaustive scorer bit-identical, stays),
  runs one instrumented installcheck and summarises with lcov. It starts its cluster
  with initdb/pg_ctl rather than pg_virtualenv, because pg_virtualenv runs the server as
  the postgres user and leaves the `.gcda` files unusable by the runner user.
- **`static-analysis`** runs cppcheck over `src/*.c` with the server headers on the
  include path and prints a per-severity count to the job log.

## Alternatives considered

- **Gate on benchmark regressions.** Rejected under the standing policy: shared-runner
  noise would either fail good commits or need a threshold loose enough to miss real
  regressions.
- **Gate on a coverage floor.** Deferred, not rejected: there is no measured baseline to
  set a floor from. Promoting the job is a later decision made from its own history.
- **clang-tidy instead of cppcheck.** Rejected for now: clang-tidy needs a
  `compile_commands.json`, which PGXS does not emit, while cppcheck takes `-I` directly.
- **Gate on cppcheck findings.** Not possible yet: the tool was not installed on any
  development machine, so its noise level on this tree is unknown until the first CI runs
  report it.

## Consequences

- The benchmark trend, the coverage figure and the cppcheck count exist, but only as
  artifacts and job logs; a regression in any of them fails nothing, so it is seen only if
  someone looks.
- Each leg is promoted to gating by deleting its `continue-on-error` once its numbers are
  understood -- the same path ADR 0091 set out for the macOS leg.
- The bench job's CSVs are not baselines and must not be copied into `bench/baselines/`.

## Addendum (2026-10-05, PRs #331-#350)

- **Report-only canaries** (#309 CI-03, CI-15, PR #348). The bench job warns on an empty CSV
  (`ci/check_bench_csvs.sh`), and the coverage job warns when the `.gcda` count or the
  coverage counters fall and writes the line-coverage figure to the run summary
  (`ci/coverage_summary.sh`). Neither gates, consistent with this record. The coverage
  cluster now runs under `C.UTF-8`, so folding-only paths are measured.
- **The TAP-ran check does gate** (CI-07): build-and-test fails unless prove's `Files=N`
  equals the number of `t/*.pl` (`ci/check_tap_ran.sh`). It runs only on build-and-test.
- **CI-04's bounded target is met without a coverage number.** `sql/150_error_sites`
  executes the user-reachable, non-corruption ERROR sites that no suite reached (measured with
  a gcov build over every SQL suite): the query-tree depth cap, keyless node, non-numeric
  slop, non-boolean `ordered`, non-array clause list, boost weight and query type checks,
  both phrase-length caps, the unsupported `key_field` type, and merge rule (c) with its
  runner-up branch (PR #347). Corruption sites are covered where the lever suites reach them
  (ADR 0126). Still no coverage floor.
- **Residuals (D28):** no clang implicit-conversion leg (CI-02), cppcheck stays `|| true`
  (CI-13), no score oracle comparison in TAP (CI-11), no pg_dump round-trip TAP, and the
  crashed-extend reuse, requeue and idempotence branches have no deterministic lever
  (CI-04(c5)). The PG19 experimental build-and-test leg may go red if the PGDG 19 package
  lacks TAP; it does not gate.
