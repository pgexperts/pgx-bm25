---
id: 0059
title: A docs-only change is verified by preprocessor token identity, not by comparing object files
date: 2026-08-11
status: Accepted
summary: Comparing before/after object files cannot verify a comment-only change, because ereport and Assert bake __LINE__ into the binary and any added comment line shifts it; preprocessing each translation unit and masking only the __LINE__ argument proves the claim exactly, and both negative controls fire.
---

# 0059. A docs-only change is verified by preprocessor token identity, not by comparing object files

## Context

ADR 0058 corrected comments in 26 files. Its central safety claim is that it
changed **no executable byte**. That claim needs a proof, because nothing else in
the pipeline can supply one: comments are invisible to the compiler and to all 92
regression suites, so a green build and a green suite are equally green whether
the change was comment-only or not.

The house method for exactly this was recorded during PR-K: *verify a mechanical
sweep with the COMPILER, not a scanner*. A hand-written checker had produced false
positives on `bm25_scan.c` and `bm25_snippet.c` because character literals like
`'"'` in the HTML escaper desynchronise naive string tracking. Compiling every
translation unit at `-g0` before and after and diffing the object files answered
it exactly -- 22/22 byte-identical.

Applied here, that method reported **20 of 22 objects differing**. Read naively,
that says a docs-only PR changed twenty translation units.

It did not. PostgreSQL's `ereport` and `Assert` expand to calls carrying
`__FILE__`, `__LINE__` and `__func__`; `Assert` becomes
`ExceptionalCondition("...", "src/bm25_phrase.c", 273)`. Adding or removing a
comment line shifts every line number below it, so the *constants compiled into
the binary* change while the code does not. PR-K's 22/22 held only because that
sweep was an in-place transliteration of non-ASCII characters, which preserved
line counts exactly. The moment a sweep reflows a comment block, object-file
comparison stops being a proof.

This matters more than a false alarm. The failure is directional: a reviewer who
expects byte-identity, sees twenty diffs, and reasons "line numbers, obviously"
has stopped verifying and started assuming -- and that assumption would equally
absorb a real code change hiding among the shifts.

## Decision

A docs-only change is verified by **preprocessing every translation unit before
and after and comparing the token streams**, masking only the `__LINE__` argument.

The preprocessor strips comments by definition, so a comment-only change must
produce an identical stream. Concretely: preprocess each `src/*.c` from both trees
with `clang -E -P` using the same relative path from each tree root (so `__FILE__`
matches), then mask exactly the line-number argument that follows a `"src/bm25_*.c"`
string literal, then require the results to be byte-identical.

**Mask the `__LINE__` argument specifically, never all digit runs.** Blanket digit
normalisation also masks a changed numeric constant, which is a code change this
check exists to catch.

**The check ships with both negative controls, and they are run.** (a) An injected
statement must be detected. (b) A changed numeric constant -- the precise thing
sloppy masking would hide -- must be detected. A verification that has never been
shown to fail is not evidence; this repo has already shipped one regression suite
that was green 92/92 while asserting nothing, and one CI floor that could not
witness the change it existed to catch.

## Alternatives considered

- **Compare object files (the PR-K method).** Correct for a line-count-preserving
  sweep, wrong here, and wrong in the dangerous direction -- see Context. Kept in
  the toolbox for transliteration-shaped changes only.
- **Compile with `-g0` and hope debug info was the difference.** This was the first
  hypothesis and it is wrong: `-g0` strips DWARF, but `__LINE__` reaches the binary
  as an ordinary immediate operand in `.text`, untouched by debug flags.
- **Trust `git diff` filtered to comment-looking lines.** Effectively what each
  author already self-reports, and it is the exact shape PR-K proved unreliable:
  it requires classifying every line as comment or code without a C lexer, and C
  string and character literals defeat that. Useful as a cheap pre-check, not as
  the proof.
- **Strip comments with a purpose-built tool and diff the result.** Same lexing
  problem as above, solved worse than by the compiler that is already installed.
- **Rely on the regression suite.** Structurally incapable: the suite exercises
  behavior, and a comment has none. It is a necessary check for this PR, not a
  sufficient one.

## Consequences

- ADR 0058's core claim is established rather than asserted: all 22 translation
  units are token-identical after masking, with both negative controls firing.
  That is a materially stronger statement than "the tests still pass".
- Any future docs-only sweep has a documented, cheap, dependency-free proof. It
  needs a working `pg_config` and the SDK include paths, which is a lower bar than
  a build.
- The method is limited to `src/*.c`. It says nothing about `bm25_native--1.0.sql`
  (verified separately by requiring every changed line to be a `--` comment and by
  `check_packaging_identity.py`), and nothing about markdown.
- One repo-specific hazard sits outside this check and must be watched by hand on
  any docs change: CI enforces interrupt-check floors with a raw
  `grep -o 'CHECK_FOR_INTERRUPTS()' src/`, matching the CALL FORM WITH PARENTHESES.
  A comment that merely *mentions* the macro with parens moves a safety gate's
  count -- upward, which passes the floor while corrupting its meaning. Prose must
  write it without parentheses. Counts were confirmed unchanged across this change
  (aggregate 59, and 7 for `BM25_VACUUM_DELAY_POINT()`).
- This is a procedure, not a gate. It is not wired into `ci.yml`, because it needs
  a before-tree to compare against and CI has no natural one. It is a thing to run,
  which means it depends on someone remembering to.
