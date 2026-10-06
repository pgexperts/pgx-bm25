---
id: 0057
title: CI gates on a folding-collation cluster
date: 2026-08-11
status: Accepted
summary: A third gating job runs the SQL suites against a C.UTF-8 cluster, because every other gate is C-locale and under LC_CTYPE=C the analyzer never folds a non-ASCII character, which is also what decides whether the stemmer engages.
---

# 0057. CI gates on a folding-collation cluster

## Context

ADR 0046 moved the analyzer to character-wise splitting with a fold applied at
emit time, through `str_tolower` under the database default collation. That made
the analyzer's output a function of the cluster's `LC_CTYPE` — and every gate in
this repository runs a cluster where that function is the identity on everything
above ASCII:

| gate | how its cluster is created | ctype |
|---|---|---|
| `build-and-test` | `pg_virtualenv` | C |
| `hardening` | `initdb --no-locale` | C |
| local `_localtest` | inherited | C |

So no gate, anywhere, ever folded a non-ASCII character.

That is not a cosmetic difference, and the reason is the part that is easy to
miss. The fold happens **before** the stemmer sees the word, so a collation that
folds also decides whether the stemmer **engages at all**. `german_stem` given an
already-lowercased accented word stems *and transliterates* it; given the
uppercase form its rules miss, and the whole word is stored. The bytes written to
the index therefore differ **structurally**, not just in case, between a C
cluster and a folding one. This is why the obvious defence — compare
`lower(term)` against `lower(word)` so the fold cancels on both sides — does not
work, and why `sql/82_encoding_aware_tokens` asserts `octet_length` over words
chosen to be length-stable in both directions instead.

PR-F's adversarial review found `sql/82` failing on a UTF-8 cluster precisely
because nothing in CI ran one. It was fixed, and hand-verified under C,
`en_US.UTF-8` and ICU. But from that day the invariance was held by the memory of
whoever last touched the analyzer, and nothing else. Every subsequent PR could
have broken it green.

## Decision

**A third gating job, `folding-collation`, runs `installcheck` against a cluster
created with `initdb --locale=C.UTF-8 -E UTF8`.** The SQL suites run on that
cluster; the TAP suites build their own and do not — see Consequences.

`C.UTF-8` and not `en_US.UTF-8`: its `LC_CTYPE` is full Unicode, so the fold is
exercised, while its `LC_COLLATE` stays codepoint order, so every `ORDER BY`
across the suites keeps the ordering the expected output is already pinned
against. That isolates the single variable under test.

PostgreSQL 18 only. The property is analyzer-level and version-independent, and
`build-and-test` already covers both supported majors under C.

**The job asserts its own premise before running a single suite.** Two queries
run first: `lower(chr(201)) = chr(233)` must come back true, and the resulting
`datctype` is echoed for the log. If the fold does not happen, the job fails with
an explicit error instead of proceeding.

## Alternatives considered

- **A third entry in the existing `build-and-test` matrix.** Rejected on
  mechanics: that job's cluster comes from `pg_virtualenv`, so the locale would
  have to be steered indirectly through `pg_createcluster`'s environment
  inheritance. That is an implicit dependency on a wrapper's internals which,
  if it ever changed, would leave a job that quietly re-runs the C-locale suite
  under a name claiming otherwise. A separate job doing its own `initdb` states
  the locale outright, and follows the shape `hardening` already uses.
- **`en_US.UTF-8`.** It folds, but it also replaces codepoint collation with a
  linguistic one, so a failure would be ambiguous between the property under test
  and an unrelated ordering change. Kept as a *verification* step rather than the
  gate: a full `installcheck` against an `en_US.UTF-8` cluster was run locally
  while writing this and is 92/92, which retires the collation-ordering risk
  separately from the ctype risk this job pins.
- **An ICU cluster.** Equivalent coverage of the fold, at the cost of a provider
  whose behaviour is versioned independently of both PostgreSQL and glibc — a
  third moving part under a gate whose whole purpose is to remove reliance on
  luck. Worth revisiting if the extension ever grows ICU-specific behaviour.
- **Assert the configuration (`datctype = 'C.UTF-8'`) instead of the property.**
  Rejected. The configuration is a proxy; `lower()` folding is what the analyzer
  actually consumes. A property assertion also survives PostgreSQL changing how
  it derives folding from ctype.
- **Do nothing and rely on `sql/82`.** That suite is the assertion; this job is
  the environment the assertion needs to be meaningful in. `sql/82` passes on a C
  cluster whether or not the folding path is correct.

## Consequences

- **CI cost grows by one job**: a PGDG install, an extension build, and a full
  SQL + TAP `installcheck`. It runs in parallel with the other two, so wall-clock
  is roughly unchanged; it is billed minutes that buy an invariant currently held
  by discipline.
- **The folding coverage is the pg_regress half ONLY, and this is the sharpest
  limitation of the record.** `PostgreSQL::Test::Cluster->init` runs its own
  `initdb --no-sync --pgdata ... --auth trust` with no locale argument, so every
  `t/*.pl` cluster takes the runner's environment locale rather than this job's.
  The TAP suites therefore run a third time here as *duplicated* coverage, not
  folding coverage — and `t/018_snippet_encoding.pl`, the one TAP suite whose
  entire subject is encoding, remains un-exercised under a folding ctype. Anyone
  reading "installcheck against a C.UTF-8 cluster" and inferring otherwise has
  been misled; the SQL suites are the ones that ran there.
  The lever for closing it is `LANG`: `PostgreSQL::Test::Utils` deletes `LC_ALL`
  and `LANGUAGE` from the environment but leaves `LANG` alone, so a job-level
  `LANG: C.UTF-8` would reach `initdb` inside every TAP cluster. Deliberately not
  done here: it changes the environment of a dozen suites that have only ever run
  under C, and doing that without re-verifying each one is the kind of blind
  widening this series has been bitten by before. Follow-up work.
- **The TAP re-run was not suppressed** with a `TAP_TESTS=` make override, which
  would have saved the duplicated minutes but reads as "TAP is disabled here" to
  the next person to open the file.
- **A collation-sensitive assertion can now be caught before merge** instead of
  by a reviewer who happens to remember. Correspondingly, a future suite that
  pins C-locale-only output will now go red here — that is the gate working, and
  the fix is the suite, not the job.
- **The canary is load-bearing and was negative-controlled.** "`installcheck`
  passed on the folding cluster" is also satisfied by a cluster that is not one:
  `pg_regress` builds `contrib_regression` from `template0`, so a wrong
  `template0` would make this job an expensive duplicate of `build-and-test`
  reporting green forever. Run against the C-locale dev cluster the canary
  returns `f` and fails; against the folding cluster it returns `t` and passes.
  Same shape as `sql/85`'s hijack canaries and `sql/43`'s cap canary: pin the
  mechanism, not only the outcome.
- **If a runner image ever lacks `C.UTF-8`, `initdb` fails loudly** rather than
  falling back — and if it somehow succeeded with a non-folding ctype, the canary
  catches it. Both failure modes are visible; neither is silent.
- **Confirmed on the first run**, which mattered because glibc's `C.UTF-8` folding
  behaviour was reasoned about rather than observed while this was written: the
  job reported `template0 ctype: C.UTF-8; lower(U+00C9) folds to U+00E9: t` and
  then ran 92 of 92 suites against that cluster.
