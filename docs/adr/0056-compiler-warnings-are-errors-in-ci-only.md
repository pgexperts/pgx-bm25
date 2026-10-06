---
id: 0056
title: Compiler warnings are errors in CI, and nowhere else
date: 2026-08-11
status: Accepted
summary: The CI matrix build passes COPT=-Werror so a warning fails the pipeline on both supported majors, while the Makefile stays permissive so a third-party build is never blocked by a warning this project has never seen.
---

# 0056. Compiler warnings are errors in CI, and nowhere else

## Context

Review finding #67.1 (L4, *"CI has no compiler-warning gate and no
AddressSanitizer job"*) observed that the build step ran a plain `make` and
failed only on a hard error.

The gap was not that warnings were absent — it was that nobody read them. PGXS
compiles every translation unit with PostgreSQL's own curated set, baked into
`pg_config --cflags` at server build time: `-Wall -Wmissing-prototypes
-Wpointer-arith -Wdeclaration-after-statement -Werror=vla -Wendif-labels
-Wmissing-format-attribute -Wcast-function-type -Wformat-security`, and more.
(The exact list is compiler-dependent — that one is what the local Apple clang 16
server reports; a gcc-built server's differs in both directions, which is part of
why the gate runs on the CI toolchain rather than only locally.) Those
diagnostics were being emitted on every CI build already. `make` exited 0, the
pipeline went green, and
the text scrolled past in a log that nobody opens on a passing run. A gate that
produces the right evidence and then discards it is indistinguishable from no
gate.

The classes this leaves uncovered are ordinary ones for this codebase, not
exotic. A `-Wmaybe-uninitialized` on a scan-state field is a live risk in a
scanner whose opaque struct is `MemSet` to zero and then populated by mode; a
`-Wformat` mismatch inside an `ereport` is a live risk in a codebase where the
format string and its arguments are routinely several lines apart. Both compile,
both run, and neither shows up in `pg_regress` output.

The same finding noted a second, unrelated omission in the same file: the
workflow declared no top-level `permissions:` block, so every job inherited the
repository default `GITHUB_TOKEN` scope. Nothing in this workflow needs more than
a checkout. It is recorded here rather than in its own ADR because it has no
competing alternative — least privilege is simply the correct default — and
because it shipped as the other half of the same finding.

## Decision

**The CI matrix build passes `COPT=-Werror`. The Makefile does not.**

`COPT` is the PGXS hook for exactly this: `Makefile.global` appends it to both
`CFLAGS` and `LDFLAGS`, so a single command-line variable promotes the existing
warning set without the extension's own build files taking a position on
strictness.

It runs on **both** matrix legs. A warning can originate in a server header this
project does not control, and PG 17 and PG 18 ship different ones, so a
single-leg gate would be blind to exactly the version-specific case.

The workflow also declares `permissions: contents: read` at the workflow level —
not per job, so that adding a job cannot silently opt back into the repository
default by omitting a block.

## Alternatives considered

- **Put `-Werror` in the Makefile's `PG_CFLAGS`.** Rejected on the grounds the
  finding itself raised: PGXS pulls in server headers this project does not
  control, and a user building the extension against a compiler this project has
  never seen would have their build fail on a diagnostic nobody could have
  anticipated. Their build succeeding while ours is strict is the correct
  asymmetry. There is a second, mechanical reason: `PG_CFLAGS` is the variable
  the `hardening` job already overrides on the command line to inject its
  sanitizer flags, and a command-line assignment replaces the Makefile's value
  outright — so a `-Werror` placed there would silently vanish from the one job
  that builds with the most unusual flags.
- **Grep the build log for `warning:` in a following step.** Rejected. It needs
  the build output captured and re-read, it misattributes lines when `make -j`
  interleaves output, and it has to re-implement the "which warnings count"
  judgement that `-Werror` gets from the compiler for free.
- **`-Werror` on one matrix leg only,** to halve the blast radius of a compiler
  bump. Rejected: the header-originated warning is the version-specific case, so
  this drops precisely the coverage that motivated running a matrix at all.
- **Add an AddressSanitizer job,** as the finding proposed. Excluded by standing
  user decision ("CI: `-Werror` + a least-privilege `permissions:` block. NO
  ASan"). The finding's own verifier note supports that call: `--enable-cassert`
  already defines `CLOBBER_FREED_MEMORY` and `MEMORY_CONTEXT_CHECKING`, so the
  existing hardening job catches a substantial share of the use-after-free and
  overrun class on palloc'd memory, and the finding's claim that the class has no
  coverage at all is wrong.
- **Add a pgindent / lint job,** also proposed by the finding. Out of scope by
  the standing #66 decision, which scoped conventions to a cheap subset and
  explicitly excluded the 79-column reflow (3,967 lines), comment-opener style
  (803) and PG file headers (0 of 25) that pgindent would enforce wholesale. A
  lint job would re-open all three by machinery.

## Consequences

- **A compiler bump on the runner image can turn CI red on code that did not
  change.** This is the deliberate cost, and it is the reason the flag is on the
  CI command line rather than in the tree: the fix is local to one file. Fix the
  warning, or add a targeted `-Wno-...` to that line with a note naming the
  compiler that forced it. Do not delete the gate.
- **Local and third-party builds are unaffected.** Contributors keep a build that
  warns rather than one that stops, which matters most during the exploratory
  edit-compile loop where a half-written function is expected to be unused.
- **The pipeline now fails on a class no test can express.** Verified, not
  assumed: a probe introducing an unused variable and an unused static function
  was compiled both ways on the current tree. Plain `make` → exit 0 with two
  warnings; `make COPT=-Werror` → exit 2, `error: unused variable ...
  [-Werror,-Wunused-variable]`.
- **Local evidence could not have settled this; CI did.** The tree builds clean
  under `-Werror` locally on Apple clang 16 at `-Og`, but CI is PGDG **gcc at
  `-O2`** — a different compiler, a different optimization level, and a
  materially different warning set. The PGDG server's `pg_config --cflags`
  carries `-Wimplicit-fallthrough=3` and `-Wshadow=compatible-local`, which the
  local clang server's does not, and `-O2` enables the optimization-dependent
  diagnostics (`-Wmaybe-uninitialized` above all) that have no clang equivalent
  at `-Og`. No machine available to this project runs the enforcing toolchain, so
  the branch's own first CI run was the first real test of the gate. **It passed
  clean on both legs** — 22 translation units plus the link, PG 17 and PG 18 —
  which is what makes the flag safe to leave enabled rather than a coin flip
  inherited by the next contributor. The general point survives the good news:
  "builds clean locally" is not evidence about this gate, and a future compiler
  bump gets no such reassurance in advance.
- **`permissions: contents: read` does not affect the hardening job's cache.**
  `actions/cache` authenticates with the runner's `ACTIONS_RUNTIME_TOKEN`, not
  `GITHUB_TOKEN`. A future job that genuinely needs to write — publishing a
  release, commenting on a PR — must widen the scope in that job's own block.

## Addendum (2026-09-21)

The "add an AddressSanitizer job" alternative above was reversed on 2026-08-17:
e8ca59a (PR #168, issue #142) added a GATING `asan` job, after the next review
round found three memory-safety defects of exactly that class, one of them a stack
array that cassert's allocator instrumentation cannot observe at all. That change
shipped without a decision record, so this file went on stating the rejection for
a month. ADR 0090 records the decision and why the cassert argument quoted above
does not cover the class; the report-only `macos` leg added in the same commit is
ADR 0091. The `-Werror` decision itself is unaffected.

## Addendum (2026-09-21)

The "pgindent / lint job" alternative above is still rejected, but CI now has a
style gate of a different kind. ADR 0092 documents the tree's own house style
in `ARCHITECTURE.md` and adds `test/check_source_style.py` to `build-and-test`,
which enforces only that style's mechanically decidable rules: no tabs, CRs or
trailing whitespace, no line over 120 columns, `postgres.h` first in every `.c`
and in no header, and `errcode()` in every `ereport` that can raise an error. It
does not run pgindent and does not reopen the 79-column reflow, comment-opener
normalization or PG file headers that the #66 decision excluded.
