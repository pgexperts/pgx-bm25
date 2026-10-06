---
id: 0008
title: Gate CI on a source-built cassert + UBSan installcheck
date: 2026-07-12
status: Accepted
summary: Add a gating CI job that builds PostgreSQL from source with --enable-cassert --enable-debug and the extension with UBSan, then runs the full installcheck, catching the assert / use-after-free / misaligned-access / version-contract bug classes the PGDG package build cannot see.
---

# 0008. Gate CI on a source-built cassert + UBSan installcheck

## Context

The `build-and-test` job installs PostgreSQL from the PGDG apt packages —
release binaries built **without** `--enable-cassert` and with UBSan absent.
That build structurally cannot observe an entire class of defects this
extension is exposed to as an in-process access method manipulating raw
buffer pages:

- **`Assert()` violations** — every `Assert()` in PostgreSQL and in the
  extension compiles to a no-op in a release build. Torn page invariants,
  bogus `PageGetSpecialPointer` offsets, and off-by-one item bounds pass
  silently.
- **Undefined behavior** — misaligned typed stores/loads, signed overflow,
  out-of-bounds array indexing. These "work" on x86/arm release builds until
  a compiler, platform, or optimization change turns them into corruption.
- **Version-contract drift** — the package build tracks the newest PGDG
  minor, so it silently tolerates backend-API behavior that does not exist
  across the extension's full supported version range (floor PG17).

This is not hypothetical. Bringing the job up surfaced two real,
pre-existing defects that had passed every prior green CI run:

1. A misaligned `BM25FieldConfig` page copy in `bm25_analyzer.c` — the
   132-byte `BM25FieldConfigHeader` is not a `MAXALIGN(8)` multiple, so the
   trailing `float8`-bearing config array was written and read at a 4-off-8
   address. Undefined behavior on *every* `CREATE INDEX`, invisible to the
   release build, trapped immediately by UBSan (fixed in `d64acc8`).
2. Two `bm25_debug_*` SRFs called `deconstruct_array_builtin(arr, INT4OID,
   …)`. INT4OID was only added to that wrapper's supported-type switch in
   PostgreSQL 18.2 (back-patched to 17.x). On a stock 18.0/18.1 backend the
   call errors `"type 23 not supported"` — a portability defect against the
   PG17 floor that only a version-pinned source build exposes (fixed in
   `03c1340`; both sites now use the general `deconstruct_array()` with an
   explicit int4 descriptor).

Both were caught by machine, not review. The question was whether to make
that catch a standing gate.

## Decision

Add a **gating** `hardening` job to `.github/workflows/ci.yml`, alongside the
unchanged `build-and-test` matrix:

- Source-build PostgreSQL 18 with `--enable-cassert --enable-debug
  --enable-tap-tests --without-icu`, cached across runs via `actions/cache`
  keyed on the PG version + configure profile (built once per cache lifetime,
  not per run).
- Build **only the extension** with UBSan — `-fsanitize=undefined
  -fno-sanitize-recover=undefined` on the compile lines and, via the
  `PG_LDFLAGS` PGXS knob, on the `.so` link line. PostgreSQL itself is not
  UBSan-instrumented.
- `initdb` / `pg_ctl` a throwaway cluster and run the full `make
  installcheck` (SQL regression suites + TAP) with
  `UBSAN_OPTIONS=halt_on_error=1`, dumping `regression.diffs`, the server
  log, and `tmp_check/log/*.log` on failure.

A cassert or UBSan failure is a correctness failure, so this job **gates**
the pipeline (it is not a benchmark). The documented fallback, if a run
surfaces extensive pre-existing latent findings that cannot all be fixed at
once, is to set `continue-on-error: true` (report-only) and file follow-ups
— this bring-up did not need it.

PG 18 only: the assert / UB / alignment classes are version-independent, so
a single source build carries the coverage; the existing 17/18 PGDG matrix
still exercises both supported majors for functional correctness.

## Alternatives considered

- **Whole-PostgreSQL UBSan** (instrument PG too) — floods the log with
  UB inside PostgreSQL's own code (intentional aliasing, packed-struct
  access) that we cannot fix and would have to suppress wholesale, drowning
  the extension's own findings. Extension-only instrumentation keeps every
  trap actionable.
- **AddressSanitizer** — strictly stronger on heap/UAF/overflow, but a
  bundled extension needs a *fully* ASan-built PostgreSQL (ASan cannot be
  extension-only across the load boundary) plus a maintained LSan
  suppression file for PG's deliberate leaks. Higher setup and maintenance
  cost; deferred, not rejected.
- **Valgrind/memcheck installcheck** — excellent coverage, but 20–60 minutes
  per run makes it a nightly/manual tool, not a per-push gate. Deferred.
- **cassert-only, no UBSan** — catches the assert violations but misses the
  misalignment class, which is exactly one of the two bugs bring-up found.
  UBSan is the cheaper half to add and caught the higher-value bug.
- **Do nothing (rely on review)** — the two defects above passed review and
  every green run for milestones; review demonstrably does not catch this
  class reliably.

## Consequences

- CI builds PostgreSQL from source. Mitigated by caching — a cache hit skips
  the ~5–8 minute build entirely; only the extension rebuild + installcheck
  runs (~2–3 minutes).
- The gate may surface **more** pre-existing latent bugs over time. That is
  the point; each is a real defect. The report-only fallback exists for a
  run that surfaces more than can be fixed in one pass.
- New code is now held to a higher bar: alignment-correct typed page access,
  no reliance on backend behavior newer than the PG17 floor, and assert
  invariants that actually hold. This is a permanent constraint on
  contributions, enforced by machine rather than reviewer diligence.
- The catch is proven, not asserted: the gate surfaced both bugs organically
  during bring-up, and was additionally verified to fail on a deliberate
  reintroduction of the `bm25_analyzer.c` misalignment on the CI runner
  before the introducing PR merged.

## Addendum (2026-10-05, PRs #331-#350)

The "PG 18 only ... version-independent" premise above did not hold. An Assert is a
property of the server's code, and #244's PG19 buffer re-lock assert was caught only by a
hand-built cassert PG19, while PG17, the supported floor, had no assert run at all (#309
CI-06). Since PR #348 the hardening job (and the asan job, ADR 0090) is a matrix over the
current minor of each supported major, PG 17.11 and 18.6, both gating; each `pg_ver` is
bumped when a minor ships, and the cache key carries it. The hardening job also runs with
`wal_consistency_checking = 'all'` (ADR 0074's addendum).

Residual (D28): no PG19-beta cassert leg. A beta can assert on its own bugs, and
build-and-test already carries PG19 as a non-gating leg.
