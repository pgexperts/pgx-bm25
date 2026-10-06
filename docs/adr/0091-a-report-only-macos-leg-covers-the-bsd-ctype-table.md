---
id: 0091
title: A report-only macOS leg covers the BSD ctype table
date: 2026-09-21
status: Accepted
summary: CI runs the pg_regress half of installcheck on macos-latest as a continue-on-error job, because every other job is glibc and the BSD single-byte ctype divergence the project documents in its own suite had no automated coverage; it is promoted to gating only once observed green on main.
---

# 0091. A report-only macOS leg covers the BSD ctype table

## Context

Every CI job ran on `ubuntu-latest`, which means glibc. `sql/82_encoding_aware_tokens`
records a divergence that glibc cannot exhibit: under a UTF-8 `LC_CTYPE`, a BSD
single-byte ctype table reports `isalnum(0xC3)` true and lowercases the lead byte
of every Latin-1-range character, while glibc treats `0x80-0xFF` as separators.
Same code, two behaviours, and the first was found only by manual off-CI testing
(ADR 0046). REVIEW-2026-08-16 filed it as BLD-01; issue #142 carried it.

The job landed with the ASan gate in e8ca59a (PR #168) on 2026-08-17 without a
decision record. This record closes that gap retroactively, alongside ADR 0090.

## Decision

A `macos` job builds against Homebrew `postgresql@18` and runs the `pg_regress`
half of `installcheck`, with `continue-on-error: true`.

- **Report-only to start.** A first-run failure would be a real pre-existing
  platform difference, not a regression from the PR adding the job, and blocking
  every PR on it is the wrong trade. It is promoted to gating by deleting
  `continue-on-error` once it is observed green on `main`; a red result is itself
  the finding and gets its own issue.
- **SQL half only.** Homebrew ships no `PostgreSQL::Test`, so the TAP tier cannot
  run there -- the same scope limit the `folding-collation` job carries (ADR 0057).
- The job's header in `ci.yml` records its first-run result (2026-08-18): 93 of
  94 suites passed, `sql/82` among them, and the single failure was an interrupt
  latency assertion timing out on the slower runner, not a correctness divergence.
  The interrupt-latency suites are excluded on this leg for that reason.

## Alternatives considered

- **Gate immediately.** Rejected for the reason above: the first run is the
  discovery, and a platform difference should not block unrelated work.
- **A Linux aarch64 runner.** Would help alignment-sensitive code (this codebase
  casts page bytes to structs) but does not address the ctype table, since
  aarch64 Linux is still glibc. Not a substitute.

## Consequences

- The BSD ctype behaviour has automated coverage, but a report-only job does not
  fail a workflow run, so its results have to be read to be worth anything.
- Promotion to gating is blocked on making the interrupt-latency suites
  independent of runner speed.
- `README.md` and `ARCHITECTURE.md` name this job as non-gating wherever they list
  what gates CI.

## Addendum (2026-09-23)

The blocker named under Consequences is cleared: the interrupt-latency suites now bound
latency by work done rather than wall clock (issue #156, ADR 0070's addendum), and this
leg runs the full REGRESS list with nothing subsetted out. It remains continue-on-error;
promotion still waits on observing it green on main.
