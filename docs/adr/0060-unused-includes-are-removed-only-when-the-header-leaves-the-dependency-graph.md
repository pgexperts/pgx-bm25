---
id: 0060
title: An include is removed only when the header leaves the dependency graph
date: 2026-08-11
status: Accepted
summary: "Compiles without it" is not evidence an include is unused - 68 of 165 includes pass that test, including one inside an #ifdef the build never compiles; the gate is that `gcc -M` no longer lists the header at all, which cut the safe set to 13 and matched clangd's independent verdict exactly.
---

# 0060. An include is removed only when the header leaves the dependency graph

## Context

Three consecutive passes had each removed one unused include from one file:
ADR 0053's split flagged `executor/tuptable.h` in `bm25_scan.c`, a later pass
removed it, and clangd then flagged `utils/builtins.h` in the same file. Fixing
them one at a time was clearly not converging, so this pass audited all 165
`#include` lines across the 22 `.c` files and 5 headers at once.

The obvious test is mechanical: delete the include, recompile the translation
unit at `COPT=-Werror`, and if it still builds the include was unnecessary.
**That test passes for 68 of the 165** - and acting on it would have been wrong
in three distinct ways:

- **It is blind to conditional compilation.** `bm25_phrase.c` includes
  `<assert.h>` and `<stdio.h>` inside `#ifdef BM25_PHRASE_SELFTEST`. The normal
  build never compiles that block, so the test reports "removable" for headers
  whose removal silently breaks the standalone self-check the file's own header
  comment documents (`cc -DBM25_PHRASE_SELFTEST -x c src/bm25_phrase.c`).
- **It cannot distinguish "unused" from "arrives anyway".** Most of the 68
  compile without the include only because some other header pulls the same one
  in transitively. Removing those is not a cleanup; it is a bet that the
  transitive path is stable. This repository builds against **two** PostgreSQL
  majors in CI, whose header graphs differ, so that bet is exactly the wrong one
  to take silently.
- **It points the wrong way on headers.** Seven includes in `src/*.h` pass the
  test, but they are what make those headers self-contained: `bm25.h` uses
  `Relation` 64 times and `BlockNumber` 28 times, `bm25_format.h` uses
  `BlockNumber` 24 times, `bm25_wand.h` uses `HTAB`. They compile without only
  because every consumer happens to include them earlier. PostgreSQL ships
  `src/tools/pginclude/headerscheck` to enforce self-containment; removing these
  would regress against a property core treats as a rule.

## Decision

An include is removed only when **the header disappears from the translation
unit's dependency graph** - `gcc -M` after the deletion no longer lists it - AND
the unit still compiles at `-Werror`. Headers still pulled in transitively are
left alone, whatever the compile test says.

Two riders:

- **`src/*.h` is out of scope entirely.** Includes there exist for
  self-containment, and that is the property to preserve, not eliminate.
- **An include inside a conditional block is never judged by a build that does
  not compile that block.** Check the `#if` nesting depth before trusting any
  result.

Applied, this cut 68 candidates to **13**, across 10 files.

## Alternatives considered

- **Trust the compile test (the 68).** Rejected for the three reasons above. The
  conditional-block case alone is disqualifying: it produces a confident
  "removable" for a header that is load-bearing somewhere the test cannot see.
- **Trust clangd's `unused-includes` diagnostic alone.** It is the better
  signal - a true "no symbol from this header is used DIRECTLY" statement - and
  it agreed with all 13. But it could not be obtained for the whole tree here:
  `clangd --check` does not emit IncludeCleaner diagnostics, and driving clangd
  over LSP to collect them did not produce diagnostics in this environment. It
  is corroboration, not the gate.
- **Run include-what-you-use.** The right tool, not installed, and introducing a
  toolchain dependency to justify a 13-line deletion is the wrong trade. Worth
  revisiting if the tree ever needs the *additive* half of IWYU (see
  Consequences).
- **Keep fixing them one file at a time.** What the previous three passes did.
  It is how `bm25_scan.c` got audited three times and still carried an unused
  include.

## Consequences

- 13 includes removed from 10 files. The extension's 328 exported symbols are
  byte-identical to the previous commit, so nothing observable changed.
- **Four of the deleted lines carried trailing comments asserting why the header
  was needed, and all four claims were false**: `makeDefElem` appears nowhere in
  `bm25_handler.c`; `index_open/index_close via relation_open` was wrong when
  written, since `index_close` is declared in `access/genam.h`; the sole
  `get_index_am_oid` hit in `bm25_merge.c` is inside a comment describing that
  call's own removal; and `bm25_debug_pending_append` handles its text argument
  through `PG_GETARG_TEXT_PP`/`VARDATA_ANY`, not through anything in
  `utils/builtins.h`. This is ADR 0058's defect class living in a place that
  sweep did not look - and the `get_index_am_oid` case is the recorded grep trap
  in a second form: not the include's own comment this time, but a *different*
  comment describing deleted code.
- The gate is a procedure, not a CI check. Nothing stops an unused include being
  added tomorrow. A gate is possible (the dependency-graph test is scriptable)
  but was not built here, because it would need a per-file baseline and the
  failure it prevents is cosmetic.
- **The tree still has the opposite problem, unaddressed.** Several units call
  functions they reach only through deep transitive chains - `bm25_keymap.c`
  gets `index_close` via `funcapi.h`'s executor chain, while `bm25_merge.c`
  includes `access/genam.h` directly for the same symbol. A real IWYU pass would
  ADD includes there. This decision covers only the subtractive half, and the
  additive half is the more valuable one for surviving a PostgreSQL major bump.
- Local verification could only cover PG 18.3 and Apple clang. The PG 17 axis
  was closed in review by a symbol-closure analysis against both PG 17.10 and
  18.3, and then by CI's two build legs on PGDG gcc at `-O2`.
