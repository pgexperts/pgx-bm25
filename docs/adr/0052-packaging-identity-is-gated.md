---
id: 0052
title: Packaging manifests are gated by a derived-from-source consistency check
date: 2026-08-10
status: Accepted
summary: META.json and .pgx-build.yml are corrected to the bm25_native 1.0 / PG17+ identity and then held there by test/check_packaging_identity.py, which derives the expected values from the Makefile, the control file and the #error floor and runs as the first CI step.
---

# 0052. Packaging manifests are gated by a derived-from-source consistency check

## Context

`META.json` (PGXN) and `.pgx-build.yml` (PGX packaging) are the only two files in this repo
that describe the extension to the outside world, and **nothing in the build or test path reads
either of them**. `git grep pgx-build` has no in-repo hits; PGXS never opens `META.json`. That
is not incidental — it is the whole reason both had rotted undetected through a rebrand and a
PG-version-floor change, with CI fully green the entire time.

What each said:

| File | Stale value | Truth |
|---|---|---|
| `META.json` | `"name": "pg_bm25_index"`, `"version": "0.1.0"`, `provides` → `pg_bm25_index--0.1.sql` | `bm25_native`, `1.0.0`, `bm25_native--1.0.sql` |
| `.pgx-build.yml` | `version: "0.1"`, `pgversions: "16+"` | `1.0`, `17+` |
| `test/oracle/bm25_oracle.py:2` | docstring "cross-checking pg_bm25_index" | `bm25_native` |

`META.json` had never been touched since the original PGXS scaffolding commit. Its
`provides->file` names a script that does not exist in the tree, so a PGXN release built from
this repo would publish a distribution called `pg_bm25_index` 0.1.0, advertising an extension
that `CREATE EXTENSION` cannot create (the control file is `bm25_native.control`), while the
actual 1.0 release stays unreachable through the index.

`pgversions: "16+"` is the one with behavioral consequences rather than cosmetic ones. The
extension has a **hard** PG17 floor: `src/bm25.h` guards with `#if PG_VERSION_NUM < 170000` and
`#error`, because PG16's planner will not build an incremental-sort path over an
`amcanorderbyop` index and `ORDER BY x &@@ q, <tiebreak>` therefore collapses onto the tiebreak
(ADR 0002). Advertising 16+ does not get a PG16 user a working build — it gets them a compile
that dies on the `#error`, instead of a clean refusal at package resolution.

Correcting three files is a ten-minute edit. The problem is that it is the *second* time these
values would be correct-by-luck, and there is no mechanism that would notice the third drift.

## Decision

Fix the three files, and add `test/check_packaging_identity.py` as the first step of the
`build-and-test` CI job.

The check does not hardcode the expected identity. It **derives** it from the files the build
actually uses — `EXTENSION`/`DATA` from the `Makefile`, `default_version` from
`bm25_native.control`, and the PG floor from the `#if PG_VERSION_NUM < NNNNNN` guard in
`src/bm25.h` — and then asserts the manifests agree. So a future `1.0 → 1.1` bump, or a floor
move to PG18, fails the gate until the manifests follow, without anyone editing the check.

Three details are deliberate:

- **The PG floor is read from the `#error` guard**, not from the CI matrix or the README. The
  guard is what a too-old build actually hits; the other two are descriptions of it.
- **`provides->file` is checked for existence on disk**, not just for string equality. The
  original defect was a manifest naming a script that was not in the tarball, and a string
  comparison against `DATA` alone would not have caught a `DATA` line that was itself wrong.
- **The rebrand-residue scan is scoped to the packaging files**, never tree-wide. The old
  identity is preserved on purpose in ADR bodies, in `ARCHITECTURE.md:69` (a historical
  reference to the former upgrade script), and in ~20 plan archives under `docs/`. A tree-wide
  grep-and-replace would corrupt the project's own record of what was decided when — the same
  discipline that kept the prior identity in the ADR bodies during the rebrand itself.

The script is stdlib-only, with a flat `key: value` reader instead of PyYAML, so it runs before
any build step and on a laptop with nothing installed.

## Alternatives considered

- **Just fix the three files** — what the finding asked for, and insufficient. The values were
  already wrong for the length of an entire rebrand plus a version-floor change while every gate
  stayed green; nothing about fixing them a second time changes the property that made them rot.
- **A TAP test (`t/0NN_packaging_identity.pl`)** — would be auto-discovered by `TAP_TESTS=1` and
  need no workflow edit. Rejected because TAP is CI-only in this repo (no `--enable-tap-tests`
  locally), and this is a static file-consistency check with no server involvement: making it
  require a running cluster to run at all is backwards. As a plain script it is runnable locally,
  which is also how both its negative controls were demonstrated.
- **Inline shell in `ci.yml`, matching the interrupt-check floors** — the house style for static
  gates. Rejected here because JSON and version-tuple comparison in shell is materially worse
  than in Python, and the existing inline steps are single `grep -c` counts. The CI step is a
  one-liner calling the script instead.
- **Delete `.pgx-build.yml` and `META.json` until a release actually needs them** — honest, and
  arguably the smaller surface. Rejected because PGXN publication is a stated 1.0 goal and a
  deleted manifest is not obviously better than a checked one.

## Consequences

- The manifests are now derived-checked, so a version bump that forgets them fails CI at the
  first step rather than at publication. Verified by reverting all three files: 10 disagreements,
  exit 1.
- **A version bump is now a two-file edit minimum** (`.control` plus both manifests), and the
  gate will block until it is complete. That is the intended cost.
- The check encodes the *shape* of `.pgx-build.yml` as flat `key: value`. A nested rewrite of
  that file fails the key lookups loudly rather than parsing wrongly — acceptable, but it means
  the reader must be revisited if the packaging format grows structure.
- `META.json` remains otherwise unvalidated: nothing here checks it against the PGXN meta-spec,
  only against this repo's own identity. A malformed-but-self-consistent manifest still passes.
- CI now depends on `python3` being present before the build. It already was, for the oracle TAP
  suite, but this makes it a hard dependency of the first step.
