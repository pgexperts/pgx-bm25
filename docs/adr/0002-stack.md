---
id: 0002
title: STACK
date: 2026-07-11
status: Accepted
summary: Pure C on stock PostgreSQL 17/18 via PGXS (min PG17, PG16 dropped), ts_lexize/Snowball tokenization, and installcheck SQL + TAP suites on PG17/18 with -ffp-contract=off pinned.
---

# 0002. STACK

- Pure **C** against stock PostgreSQL **17 / 18** server headers, built via **PGXS**
  (`MODULE_big`). No external libraries beyond the server. **Minimum PostgreSQL 17** — PG16 was
  dropped post-M6 (its planner does not build an incremental-sort path over an `amcanorderbyop`
  index, so the `&@@` ranked secondary-key tiebreak collapses on 16; enforced by a
  `#if PG_VERSION_NUM < 170000 #error` floor in `src/bm25.h` — see the rank-collapse fix in TRADEOFFS).
- Tokenization is core `ts_lexize` over Snowball dictionaries (M3 analyzer), resolved by
  dictionary NAME (e.g. `english_stem`) for replica determinism.
- Tests: PGXS `installcheck` SQL suites (`sql/NN_*.sql` in `REGRESS`) + `TAP_TESTS=1` TAP
  suites (`t/*.pl`, `PostgreSQL::Test::Cluster`, CI-only). CI runs build + regression + TAP
  on PG 17/18 via `pg_virtualenv`; benchmarks are report-only/non-gating. As of the BriefBank §5
  conformance suite: **57 SQL** + 15 TAP suites (M6 added `46_m6_boolean` / `47_m6_wildcard` /
  `48_m6_builders` / `49_m6_acceptance`; the rank-collapse fix adds `50_orderby_dist`; the
  BriefBank §5 conformance suite adds `51_briefbank_conformance` + the replica TAP
  `t/008_briefbank_conformance.pl`).
  **The Makefile pins `-ffp-contract=off`** so no compiler fuses a
  multiply-add — load-bearing for the WAND==exhaustive bit-exact contract (see PATTERNS).
  (CI is non-cassert PGDG, so it does not catch heap overreads/asserts —
  a cassert/valgrind/ASan job is a known coverage gap; the M4 snippet UTF-8 overread was only
  caught under ASan in review.) **Local build gotcha:** PGXS does NOT track header deps — a
  change to a struct/enum/#define in a shared header (`src/bm25.h`, `src/bm25_format.h`)
  requires `rm -f pg_bm25_index.dylib && make clean && make`; a plain `rm dylib && make` leaves
  stale `.o` files with disagreeing struct offsets (a mixed build). CI is immune (clean checkout).
- M6 adds one source file, `src/bm25_query.c` (the jsonb `BM25Query` AST: parse, flatten,
  boolean-formula eval over a uint64 presence bitmask, and glob), and the jsonb query
  builders + `(text,jsonb)` operator overloads in `pg_bm25_index--0.1.sql`. **No on-disk
  format change (still v5) — M6 is a query-language layer.**
