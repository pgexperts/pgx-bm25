---
id: 0093
title: The statistics both ranking builders share live in their own translation unit
date: 2026-09-21
status: Accepted
summary: Per-term idf, the pending scoring arm, the score accumulator with its match-set budget, and the pending key backfill move from bm25_scan.c into a new bm25_stats.c that calls nothing in bm25_scan.c or bm25_wand.c, and the two WAND adapters over it become statics in bm25_wand.c, so the WAND driver no longer calls back into the scanner that dispatched it.
---

# 0093. The statistics both ranking builders share live in their own translation unit

## Context

Issue #67 item 13 found a call-graph cycle between the scanner and the WAND
engine. `bm25_scan_build_ranking_once` in `bm25_scan.c` calls
`bm25_wand_build_ranking` in `bm25_wand.c`. The driver then called three functions
defined back in `bm25_scan.c`: `bm25_wand_prepare_terms`, `bm25_wand_score_pending`
and, since #205, `bm25_ranked_keys_fill_from_pending`. The two `bm25_wand_*` helpers
existed only because what they wrapped was file-static in `bm25_scan.c`: `AccEnt`,
`pending_df`, `pending_df_by_field`, `field_df_cb` and `pending_score_term`. They let
the driver reach that code without the scanner exporting it.

The sharing itself is not negotiable. WAND is bit-identical to the exhaustive scorer
(D8) only because both paths run the SAME idf computation and the SAME pending-arm
scorer. The problem was where that shared code lived. A maintainer reading
`bm25_wand.c` could not find the idf/pending half of the driver in it. That half sat
in a file of nearly 4,900 lines, next to the exhaustive scorer's own statics, where a
change to `AccEnt` or `pending_score_term` altered WAND with no compiler signal.

ADR 0053 took the first step. It moved the two declarations out of `bm25_wand.h`
(which had carried a "defined in bm25_scan.c" note) into `bm25_scan.h`, and it
recorded the cycle as kept, because unwinding it meant moving production scorer
statics, a different risk class from moving test-only SRFs. `bm25_scan.c` also
remained well above the project's 2,000-line guidance.

The user's decision governing this change: do the statistics extraction ONLY. Do not
split or move the exhaustive scorer (`bm25_scan_build_ranking_exhaustive`). Change no
behaviour.

## Decision

**Create `src/bm25_stats.c` + `src/bm25_stats.h`, holding exactly the code both
ranking builders call plus what that code needs.** The set was found by computing the
dependency closure of the seven symbols the issue named, not by picking them by eye:

- `bm25_term_idf`, with the static helpers only it calls: `pending_df`,
  `pending_df_by_field`, `field_df_cb` and `FieldDfCtx`.
- `pending_score_term`, renamed **`bm25_pending_score_term`**, with its static helper
  `pending_score_flush`.
- What `pending_score_flush` needs: the accumulator entry `AccEnt`, renamed
  **`BM25AccEnt`**, and its upsert `bm25_scores_add`. Then what `bm25_scores_add`
  charges, which is the #62.5 match-set budget (`BM25MatchBudget`,
  `bm25_match_budget_init`, `bm25_match_charge`, the static `bm25_match_budget_kb`,
  `BM25_HASH_ENTRY_OVERHEAD`, `BM25_MATCH_BYTES_PER_DOC`), and `BM25ExhScored`, one
  slot of which the per-document charge counts.
- `bm25_ranked_keys_fill_from_pending`, whose declaration moves out of `bm25.h`. This
  one was outside the closure of the seven symbols. It moved because it was the third
  wand -> scan edge, and both builders call it.

**The two `bm25_wand_*` adapters move to `bm25_wand.c` as statics, not to
`bm25_stats.c`.** They call `bm25_wand_ctx_build` and `bm25_topk_offer`, which
`bm25_wand.c` defines, so putting them in the stats layer would give that layer a
dependency on the WAND file. Their only caller is `bm25_wand_build_ranking`, so they
lose external linkage.

**The layering rule:** `bm25_scan.c` -> `bm25_wand.c` -> `bm25_stats.c`, and
`bm25_scan.c` -> `bm25_stats.c`. `bm25_stats.c` calls nothing defined in `bm25_scan.c`
or `bm25_wand.c`, and `bm25_wand.c` calls nothing defined in `bm25_scan.c`. After
this change the only thing `bm25_scan.o` resolves from `bm25_wand.o` is
`bm25_wand_build_ranking`.

**Renames**: `AccEnt` -> `BM25AccEnt`, `pending_score_term` ->
`bm25_pending_score_term`. They are applied to every live reference: source, the
regression suites and their expected output, ARCHITECTURE.md and THEORY.md. Dated
records keep the names they were written with: Accepted ADR bodies, the plans and
specs under `docs/superpowers/`, and `REVIEW-2026-08-16.md`. A symbol with header
scope shares one namespace with every other extension loaded into the backend (the
reason ADR 0053 gave for `bm25_field_corpus_stats`). The other exported names already
had the `bm25_` prefix.

**What did not move.** The scan-start corpus prologue stays in `bm25_scan.c`:
`bm25_scan_corpus_stats` with `pending_global_stats`, `pending_stats_by_field`,
`bm25_field_corpus_stats` and `bm25_scan_load_fieldcfg`. So does every scorer
callback. Nothing in the moved set calls them, and WAND receives their results as
arguments. The exhaustive scorer stays by decision.

## Alternatives considered

- **Leave the cycle as documented (ADR 0053's position).** Costs nothing now, but it
  keeps half the WAND driver's logic in the scanner's file, where a change to a
  scanner static silently changes WAND. The review finding stays open indefinitely.

- **Put the two `bm25_wand_*` helpers in `bm25_stats.c`, where the issue's suggested
  fix listed them.** This does not work. They call `bm25_wand_ctx_build` and
  `bm25_topk_offer`, so the stats layer would call into `bm25_wand.c` and a new
  stats <-> wand cycle would replace the old one. The closure computation showed this
  before any code moved.

- **Also move the exhaustive scorer out (a three-way split).** That would bring
  `bm25_scan.c` under the 2,000-line guidance. It was excluded by user decision, as it
  was for ADR 0053.

- **Export `AccEnt`/`pending_df`/`pending_score_term` wholesale from `bm25_scan.h`.**
  This is what the two helpers were originally written to avoid. It would make the
  cycle worse: the WAND driver would call the scanner's internals directly instead of
  through two named entry points.

- **Leave `bm25_ranked_keys_fill_from_pending` in `bm25_scan.c`.** That keeps the move
  strictly to "statistics", but one wand -> scan edge would survive and the layering
  rule above would be false. The function reads the pending list for both builders,
  the same kind of work as the rest of the layer, and it depends on nothing in the
  scanner.

- **Also move the corpus prologue (`bm25_scan_corpus_stats` and its walkers), so that
  every corpus statistic is in `bm25_stats.c`.** Tidier, but nothing in the closure
  calls it, it would have moved about 380 more lines, and the extraction was scoped to
  the shared machinery. Left as a candidate follow-up.

## Consequences

- About a thousand lines leave `bm25_scan.c` (roughly 3,900 remain), `bm25_stats.c`
  and `bm25_stats.h` hold about 930 and 160, and `bm25_wand.c` grows by about 100.
  `bm25_scan.c` is still above the 2,000-line guidance, by decision.
- **No behaviour change, and how that was checked.** Each moved function, type and
  macro was extracted from `main`'s `bm25_scan.c` and from its new file and diffed.
  Code lines differ only in linkage (`static` dropped, or added for the two
  adapters), the two renames, and the continuation lines the longer name realigned.
  Comments were reworded only where they named an old location, an old name, or the
  audience a declaration used to have: positional references ("above", "in this
  file", "the charge below") that now point into another file, `bm25_term_idf`'s
  "declared in" note, and the comments on the types that moved into `bm25_stats.h`.
  Cross-TU calls replace what may previously have been inlined calls
  (`bm25_scores_add` from `seg_posting_cb` is the hot one). That cannot
  change a score's bits, because `-ffp-contract=off` is set project-wide in
  `PG_CFLAGS`. The cost is one call per scored posting, next to a dynahash lookup, and
  one per phrase position charged (`bm25_match_charge` from `phrase_stash_add`).
  Measured, report-only, on a local cassert PG 18.6 with both builds at `-O2`:
  `bench/wand_vs_exhaustive.sh` at 100k documents, 5 reps, main and branch alternated
  twice, gives a branch/main geomean of 0.994 for WAND and 0.996 for exhaustive, with
  every one of the 24 cells within 0.973..1.020. Two phrase queries on the same corpus
  (exact and `~3`, 1,666 and 6,000 matches; 7 reps, 3 alternated rounds) give 0.995
  and 1.000. That is noise, not a regression. All
  115 regression suites pass. The regression-suite diff is comment-only: four suites
  name the renamed or moved symbols in comments that psql echoes.
- **Symbol direction was checked on the object files, not just stated.**
  `nm -u src/bm25_stats.o` resolves nothing from `bm25_scan.o` or `bm25_wand.o`. Its
  only project dependencies are `bm25_seg_read.o`, `bm25_pending.o`, `bm25_score.o`,
  and one GUC variable in `bm25_handler.o`. `bm25_wand.o` resolves nothing from
  `bm25_scan.o`.
- **What "acyclic" means here.** It describes direct calls among these three files,
  not the whole extension. Counting function calls between object files, the largest
  strongly connected component holds 16 of the 22 objects on `main` and 17 of the 23
  here. `bm25_stats.o` joins it because the scanner calls it and it reaches back up
  through lower modules. The six outside it are `bm25_debug.o`, `bm25_segment.o`,
  `bm25_selfuncs.o`, `bm25_snippet.o` and `bm25_upgrade.o`, which no other object
  calls, and `bm25_phrase.o`, a leaf that only the scanner calls. Several pairs of
  modules call each other directly: `bm25_meta.c` and `bm25_build.c`; `bm25_scan.c`
  and `bm25_query.c`, through `bm25_field_by_name`; `bm25_handler.c` and
  `bm25_tokenize.c`; `bm25_seg_read.c` and `bm25_seg_build.c`. So a path can still
  leave `bm25_stats.c` and come back up (`bm25_seg_read.c` -> `bm25_query.c` ->
  `bm25_scan.c`). The module map in ARCHITECTURE.md had said the wand/scan seam was
  the ONE cycle. That was wrong, and it is corrected in the same change.
- **CI interrupt floors, re-split.** A split moves checks without removing any, so
  the aggregate count cannot see it (89 before and after the move). This is the same lesson as
  ADR 0053, and only per-file floors can see the move. Seven checks left
  `bm25_scan.c`: six to `bm25_stats.c`, and one (`bm25_wand_score_pending`'s heap-offer
  loop) to `bm25_wand.c`.
  - `bm25_scan.c`: 22 -> 16. That is six lower, not seven, because the file had 23
    checks against a floor of 22. #205 added `bm25_ranked_keys_fill_from_pending`'s
    check without raising the floor, and that check is one of the seven that moved.
  - `bm25_stats.c`: new floor of 6.
  - `bm25_wand.c`: 5 -> 6.
  - The aggregate: 87 -> 86 -> 88, none of it caused by the move. A comment in
    `bm25_handler.c` wrote the delay macro in its call form, so the aggregate grep had
    been counting it as a check since the floor was set to 87 at 80e54bc (87 matches
    then, 86 of them real). The comment now names the macro without parentheses, so
    the floor first became 86, the real checks it was meant to cover. The tree held 88:
    #205 had added two checks (one in `bm25_ranked_keys_fill_from_pending`, now pinned
    by `bm25_stats.c`'s floor, and one in `bm25_pending_keymeta`) without raising any
    floor. The floor is now 88.
  - **Arrears closed in the same change**, because slack in a floor reopens the hole a
    missing floor does (ADR 0024's addendum) and the step's own rule is that floors
    are set to the actual count: `bm25_pending.c` 3 -> 4 in both places it is floored
    (#205's `bm25_pending_keymeta` check; the comment claiming all of its checks were
    in the throttling form was corrected, since that one is bare), `bm25_fsm.c`'s
    maintenance floor 4 -> 8, `bm25_segment.c`'s maintenance entry 2 -> 4 (to match its
    stricter single-file floor), and `bm25_tokenize.c` 1 -> 2 (`bm25_analyze`'s word-run loop and
    `bm25_tokenize`'s debug-tokenizer loop from #149). Each raise was negative-controlled the same way as below.

  Each per-file floor was negative-controlled by running the workflow step's own `run:`
  block under `bash -e` against a copy of `src/` with one check removed from the
  floored file and one added to `bm25_score.c`. In every case the aggregate was
  unchanged and the file's own floor failed. `main`'s floors fail on the moved tree at
  `bm25_scan.c` (16 < 22).
- `ci/check_scan_scratch.py` needed no change. It inspects only functions that stayed
  in `bm25_scan.c`.
- **Keeping it acyclic.** A new helper that both builders need goes in `bm25_stats.c`
  only if it calls nothing in `bm25_scan.c` or `bm25_wand.c`. A helper that needs the
  WAND heap or the scanner's state belongs in that caller's file. CI enforces this in
  the build job's "Stats-layer call direction" step. After the build, it fails if
  anything `nm -u src/bm25_stats.o` lists is defined (`nm -g --defined-only`) in
  `bm25_scan.o` or `bm25_wand.o`, or if anything `nm -u src/bm25_wand.o` lists is
  defined in `bm25_scan.o`. Both sets must be empty. A canary first requires two
  edges that must exist -- `bm25_term_idf` (defined in `bm25_stats.o`, used by
  `bm25_wand.o`) and `bm25_wand_build_ranking` (defined in `bm25_wand.o`, used by
  `bm25_scan.o`) -- so a missing object file or unreadable nm output fails the step
  rather than passing it. The second edge was added after review found that removing
  `bm25_scan.o` alone passed: nm's error is swallowed inside a process substitution and
  both intersections come out empty. It was
  negative-controlled in both directions: a reference from `bm25_stats.c` to
  `bm25_field_by_name` (a `bm25_scan.c` global) fails the first check, the same
  reference from `bm25_wand.c` fails the second, hiding `bm25_stats.o` trips the canary,
  and reverting passes. Those controls ran against the macOS nm. The CI runner's GNU
  nm prints the same `address type name` and `U name` shapes, and the canary is there
  in case it does not.
- `bm25_scan.h` is now included only by `bm25_scan.c` and `bm25_debug.c`, and no
  longer includes `bm25_wand.h`.
  `bm25_debug.c` had been getting `BM25_WAND_TOP_K_MAX` through that transitive
  include before its own `#include "bm25_wand.h"`, and now includes it directly, in its
  top include group.
- The file name invites a wrong guess: the `bm25_stats()` SQL function is defined in
  `bm25_meta.c`, not here. Both file headers say so.

## Addendum (2026-10-05, PRs #331-#350)

The two ranking builders' key-config discovery is now one function in this layer,
`bm25_ranked_key_config` (`bm25_stats.c`), which answers from the key stamp (ADR 0112's
addendum) when there is one (#303.I, PR #343). Moving its loop out of `bm25_wand.c` lowered
that file's interrupt-check floor; the floors now count calls with comments stripped (ADR
0024's addendum).
