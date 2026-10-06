---
id: 0101
title: The segment reader and the scanner are split by role into seven files
date: 2026-09-27
status: Accepted
summary: bm25_seg_read.c splits into catalog/validation, DICT, chain and debug files and bm25_scan.c into AM-callback, ranking and match-filter files, as a pure move; a probe that calls a static stays beside it, only symbols a second file calls cross a header, and every file holding interrupt checks gets its own CI floor.
---

# 0101. The segment reader and the scanner are split by role into seven files

## Context

Issue #228. At the tip this change started from, `src/bm25_seg_read.c` was 4,964
lines and `src/bm25_scan.c` 3,967, against the project's 2,000-line guideline. Both
had been cut before: ADR 0053 moved the scanner's debug SRFs to `bm25_debug.c`, and
ADR 0093 moved the statistics both ranking builders share to `bm25_stats.c`. Each
time the exhaustive scorer stayed in `bm25_scan.c` by decision, and each record noted
the file was still over the guideline. `bm25_seg_read.c` had never been split and had
kept growing (#225 added the SEGCAT extent helpers and the KEYMAP cursor, #229 the
LIVEDOCS all-set check).

The user approved a specific split and scoped it as a pure move with no behaviour
change. Two constraints from the precedents carried over. ADR 0053: promote a static
only when a second translation unit genuinely calls it. ADR 0053 and 0093: a split
moves `CHECK_FOR_INTERRUPTS` calls between files without changing their number, so
the aggregate CI floor cannot see it and only per-file floors can.

## Decision

**The segment reader becomes four files:**

- `bm25_seg_read.c` keeps the SEGCAT snapshot and catalog reads with their extent
  helpers, the page-kind/offset/block-number/content-bytes/extent validators, the
  segment header readers, `bm25_segcat_find_entry`/`_locate_entry`, and the LIVEDOCS
  bitmap code.
- `bm25_seg_dict.c`: `bm25_seg_dict_lookup`, the `BM25DictIter` iterator,
  `bm25_dict_expand_wildcard` with its dedup table, and `bm25_dictentry_validate`.
- `bm25_seg_chain.c`: `bm25_seg_scan_postings` and the POS cursor,
  `bm25_seg_block_header_read`, `chain_read_at` and `bm25_chain_span_validate`, the
  one-shot per-docid lookups, and `BM25SegReader`.
- `bm25_seg_debug.c`: the debug SRFs that use only the reader's API. They are not
  added to `bm25_debug.c`, which holds the scanner's probes.

**The scanner becomes three files:**

- `bm25_scan.c` keeps the AM lifecycle (beginscan, rescan and its parsing helpers,
  gettuple, endscan) and `bm25_load_if_needed`.
- `bm25_scan_rank.c`: the corpus prologue, the exhaustive scorer, the D7 dispatcher
  and the retry wrapper `bm25_scan_build_ranking`.
- `bm25_scan_match.c`: the phrase position stash and recheck, and the AND-of-terms
  fallback.

**Placement rules applied where the approved plan left a choice:**

- *A probe that calls a file-static stays in the file that owns the static;* so does
  a probe that is the harness for one validator. The static then keeps internal
  linkage and the probe runs the code the reader runs. This kept nine probes out of
  `bm25_seg_debug.c`: the page-kind, page-offset, block-number and content-bytes
  probes, `bm25_debug_segcat_walk` (it calls the four `*_sampled` SEGCAT cores) and
  `bm25_debug_seg_lenfields` (`bm25_segheader_span_validate`) in `bm25_seg_read.c`;
  `bm25_debug_dictentry_validate` in `bm25_seg_dict.c`; and
  `bm25_debug_chain_span_validate` and `bm25_debug_chain_cursor_crosstalk` (#225,
  `seg_docid_to_tid_cur`/`seg_doclen_field_cur`) in `bm25_seg_chain.c`. Moving them
  would have promoted eight statics.
- *#229's `seg_livedocs_all_set`, `bm25_seg_reader_init_checked` and
  `bm25_seg_reader_init_known` stay in `bm25_seg_read.c`,* not in the chain file with
  the rest of `bm25_seg_reader_*`. `seg_livedocs_all_set` is both LIVEDOCS-specific
  (it reads only the bitmap) and reader-init-specific (its only caller is
  `init_checked`), so cohesion alone does not decide it. What decides it is the
  geometry helper `bm25_livedocs_bits_per_page`, a `static inline` that
  `init_checked` and `bm25_livedocs_locate` both use. Keeping the three with the
  LIVEDOCS code costs nothing. Moving them would have meant moving that helper into a
  shared header or making it extern. `init_known` is `init_checked`'s companion and
  moved with it. Both call the extern `bm25_seg_reader_init` in `bm25_seg_chain.c`.
- *#225's KEYMAP cursor setup* is four lines inside `bm25_seg_reader_init` and went
  with it to `bm25_seg_chain.c`. The KEYMAP reader itself was already in
  `bm25_keymap.c`.
- *`TidCollector`, `tid_collector_add`, `seg_tid_cb` and `posting_tid_cmp` stay
  static in `bm25_scan.c`.* The plan listed them for promotion, but their only user
  is `bm25_load_if_needed`, which stayed in `bm25_scan.c`. ADR 0053's rule forbids
  the promotion.

**What crosses a file boundary.** Nothing in the segment reader: the split promoted
no symbol, and the object-level global symbol set of the four files equals the old
file's. In the scanner, `bm25_scan.h` gains:

- the five types `PhrasePosList`, `PhraseStashEnt`, `PhraseStashCtx`, `PhraseAndEnt`
  and `PhraseAndCtx`, moved verbatim with their design comments, because the
  exhaustive scorer builds these contexts and reads their entries;
- prototypes for six `bm25_scan_match.c` functions whose `static` is dropped:
  `seg_phrase_pos_cb`, `pending_phrase_stash`, `phrase_and_mark`, `seg_and_cb`,
  `pending_and_stash` and `phrase_recheck_tid`. Their only caller outside that file
  is `bm25_scan_build_ranking_exhaustive`. `phrase_stash_add` stays static;
- `bm25_qtree_is_multileaf`, moved into the header as the `static inline` it
  already was. `bm25_load_if_needed` in `bm25_scan.c`, and the exhaustive scorer
  and the D7 dispatch (`bm25_scan_build_ranking_once`) in `bm25_scan_rank.c`, read
  it.

The header now includes `bm25_query.h` and `bm25_stats.h` for those types.

**Names are kept on promotion.** A promoted static is renamed only if its name would
collide with another definition in the tree or be ambiguous in the module. None of
the six is. ADR 0053 and 0093 renamed on promotion because an extern shares the
backend's namespace, but PGXS builds this module with `-fvisibility=hidden`. The
dylib's exported symbol set is byte-identical before and after (212 symbols), so the
six new externs are not visible outside the module.

**CI.** The interrupt floors are re-split to the actual counts. `bm25_seg_read.c`
goes 19 -> 6, with new floors on `bm25_seg_dict.c` (6), `bm25_seg_chain.c` (3) and
`bm25_seg_debug.c` (4). `bm25_scan.c` goes 16 -> 4, with new floors on
`bm25_scan_rank.c` (10) and `bm25_scan_match.c` (2). The aggregate stays at 92. The
"Stats-layer call direction" step now treats the scanner as three objects. Its canary
now requires four edges that between them read all five objects: stats -> wand ->
scan_rank, scan_match -> scan_rank, and scan_rank -> scan. Its old canary edge,
`bm25_scan.o` using `bm25_wand_build_ranking`, no longer exists, so the old step
fails on the split tree. `ci/check_scan_scratch.py` now reads both files that hold
its three target functions.

## Alternatives considered

- **Put the segment-reader probes in `bm25_debug.c`.** That file would grow to about 2,900
  lines and mix scan-state probes with page-level ones that share no helpers. The
  guideline problem would just move.
- **Move every `bm25_debug_*` SRF to `bm25_seg_debug.c`, as the plan first read.**
  This needs eight statics promoted across two headers' worth of seam, among them the
  four `*_sampled` SEGCAT cores, two cursor-taking chain readers and two span validators. Their whole
  purpose is to be reachable only through the bounded public wrappers.
- **Split the scanner two ways (lifecycle / ranking, with the match filters inside
  ranking).** `bm25_scan_rank.c` would be about 2,290 lines, still over the
  guideline. The match filters are also a separable unit, a stash and a presence set
  with their own pending walks, that the scorer only drives.
- **Put the match types and prototypes in a new `bm25_scan_match.h`.** Tidier for a
  reader of the match file. It was not done because the approved plan named
  `bm25_scan.h` as the one scan-module seam, and a second private header would split
  ADR 0053's single statement of the promotion rule.
- **Make `bm25_qtree_is_multileaf` an extern function in `bm25_scan_rank.c`.** It
  would drop `inline` from a one-line predicate and add a linkage change for no
  benefit. The header copy is the same body.
- **Move `bm25_livedocs_bits_per_page` into `bm25_format.h` so the #229 functions can
  live in the chain file.** It changes a widely included format header to serve a
  file-placement preference. Reversible later if the LIVEDOCS code ever moves as a
  unit.

## Consequences

- Line counts: `bm25_seg_read.c` 1,594, `bm25_seg_dict.c` 1,119, `bm25_seg_chain.c`
  1,127, `bm25_seg_debug.c` 1,233; `bm25_scan.c` 1,624, `bm25_scan_rank.c` 1,876,
  `bm25_scan_match.c` 414, `bm25_scan.h` 240 (was 78). Every `.c` file is under the
  guideline. `bm25_scan_rank.c` is closest. Its next seam, if it grows, is the corpus
  prologue (`bm25_scan_corpus_stats` and its pending walkers, about 380 lines), which
  ADR 0093 already named as a candidate to join `bm25_stats.c`.
- **Inlining changed where a callee left its caller's file.** Per-function
  disassembly of base and tip (cassert and non-cassert `-O2`) shows the per-page
  validators (`bm25_seg_chain_extent_validate`, `bm25_seg_page_off_validate`, and
  in non-cassert builds `bm25_page_content_bytes`) now called out of line from
  `chain_read_at`, `bm25_seg_scan_postings` and the DICT walks, and
  `phrase_and_mark` no longer inlined into the exhaustive scorer's boolean
  phrase-group loop. An interleaved A/B (WAND top-k, exhaustive terms and phrase,
  AND fallback, boolean phrase group; identical buffer counts) found no difference
  outside noise. If a validator ever shows in a profile, the remedy is a `static
  inline` in a header, which ADR 0053's declare-only-what-a-second-file-calls rule
  permits.
- **How the pure move was checked.** A script segmented the old and new files into
  top-level items (function, type, `#define`, declaration, `PG_FUNCTION_INFO_V1`),
  each with its leading comment. It required the multiset of item names to match.
  Each item had to be byte-identical, differ only by a dropped `static`, or have an
  identical comment-stripped token sequence, in which case the comment diff was
  printed for review. It also required the whole comment-stripped token multiset to
  match modulo one `static` per promotion, over at least 30 functions and 100 KB.
  - Reader split: 120 items. 110 are identical and 10 are comment-only (location
    references such as "above" or "in this file" that now point into another file).
    16,028 tokens on each side, with no linkage change.
  - Scanner split: 50 items. 38 are identical, 6 are linkage-only (the six promotions)
    and 6 are comment-only (the scorer banner moved to `bm25_scan_rank.c`, plus
    location references). The only token difference is 6 x `static`.
  - Mutation controls: an operand swap (`alen - blen` to `blen - alen`, same
    multiset), a changed constant in each split, and a reordered `&&` in the moved
    inline all fail the check.
- **Symbols.** The object-level global symbol set of the four reader objects equals
  the old `bm25_seg_read.o`'s (80). The three scanner objects' set is the old
  `bm25_scan.o`'s plus exactly the six promoted names. The dylib's exported set is
  unchanged (212), and all 105 C functions the install script creates resolve to a
  `pg_finfo_` export.
- **Floors.** Every new and lowered per-file floor was negative-controlled. A copy of
  `src/` had one check deleted from the floored file and one added to `bm25_score.c`,
  leaving the aggregate at 92. The step failed at that file's own floor in all seven
  cases. The stats-layer step was controlled four ways: a reference from
  `bm25_stats.o` into `bm25_scan_match.o`, one from `bm25_wand.o` into
  `bm25_scan_rank.o`, and a missing `bm25_scan_match.o` or `bm25_scan.o`. All four
  fail it.
- Both commits build warning-free under `-Werror`. Each passes all 117 SQL suites
  (C-locale UTF8 cluster) and all 22 TAP files.
- The six promoted functions keep file-local-style names (`seg_and_cb` and so on) in
  a header. That is readable inside the scan module and invisible outside it. It
  would need revisiting only if the build ever stopped using `-fvisibility=hidden`.
- Some comments name `bm25_scan.c` or `bm25_seg_read.c` for code that had already
  moved before this change: `sql/43_wand_parity`'s pointer for
  `bm25_debug_cursor_skip` (in `bm25_debug.c` since ADR 0053), `bm25_handler.c`'s
  "~160 bytes/doc" (a `bm25_stats.c` constant since ADR 0093), `bm25.h`'s
  `bm25_match_doc_limit` (no such function), and `bm25_score.c`'s "M3-identity note".
  They were left alone as out of scope. Historical sentences ("used to be in
  `bm25_scan.c`") and dated CI floor deltas also keep the names they were written
  with.

## Addendum (2026-10-05, PRs #331-#350)

No file was split or decomposed in the #302-#314 grind (D8), although several files it touched
are past the 2,000-line guideline: `bm25_pending.c` (3,271 lines), `bm25_scan_rank.c`,
`bm25_wand.c`, `bm25_seg_build.c` and `bm25_seg_read.c` (2,281-2,405 each). The
`BM25SegWalk` implementation went into `bm25_seg_chain.c`, which already owned the chain
page reader it replaced. `bm25_pending.c` is the candidate for a separate, ask-first
decomposition.
