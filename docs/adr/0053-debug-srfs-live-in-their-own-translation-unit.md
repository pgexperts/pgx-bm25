---
id: 0053
title: The debug SRFs live in their own translation unit behind a narrow bm25_scan.h
date: 2026-08-10
status: Accepted
summary: The 15 bm25_debug_* SRFs and their private helpers move out of bm25_scan.c into bm25_debug.c, with exactly four shared scan-start helpers promoted through a new bm25_scan.h; the debug references also stop relying on MemSet to avoid the WAND path, and the CI interrupt floor gains a per-file entry for the new file because a split moves a check the aggregate floor cannot see.
---

# 0053. The debug SRFs live in their own translation unit behind a narrow bm25_scan.h

## Context

`bm25_scan.c` was 5,210 lines. 1,218 of them — a quarter of the file — were 15
`PG_FUNCTION_INFO_V1` debug SRFs and their private helpers, and they were not appended at
the end: they sat *between* the ranking dispatcher and the AM's
`beginscan`/`rescan`/`gettuple`/`endscan` lifecycle, in three separate blocks
(`bm25_debug_fingerprint_gate` next to the gate it wraps, the main body of 11 probes, and
the jsonb AST renderers). Reading the access method's callback flow meant paging past
test-only tuplestore boilerplate three times.

This is finding #69.5, filed as maintainability. It is worth recording *why* it was worth
acting on rather than tolerating: the review that filed it also observed that the
interleaved SRF region "is exactly where the four duplicated stat prologues and the
fingerprint-gate-less copy hid" — the defects PR-E (#59.5) later consolidated. A region
nobody reads contiguously is a region where duplication survives.

The user decision governing this work was explicit and narrow: **split the debug SRFs out
of `bm25_scan.c` only — NOT the 926-line exhaustive scorer.** So this record is about a
move, not a redesign.

### What made the move non-trivial

A file split is mechanical only if the static surface is inventoried exactly. Enumerating
every identifier in the 21 moving function bodies against `bm25_scan.c`'s file-scope
statics produced a surface of exactly four functions that **both** production and debug
code call:

| helper | production callers | debug callers |
|---|---|---|
| `bm25_scan_corpus_stats` | exhaustive scorer, `_build_ranking_once` | 3 probes |
| `bm25_term_idf` | `bm25_wand_prepare_terms`, exhaustive scorer | 1 probe |
| `field_corpus_stats` | `bm25_scan_corpus_stats` | 1 probe |
| `bm25_fingerprint_gate` | `bm25_scan_corpus_stats`, `bm25_load_if_needed` | 1 probe |

Two candidates the plan had predicted would be in that set were not:
`bm25_phrase_fields_have_positions` and `bm25_phrase_segments_have_positions` have no
debug callers at all. One apparent dependency —
`bm25_debug_query_parse` → `bm25_scan_build_ranking_exhaustive` — was a comment mention,
not a call.

### The hazard the plan did not anticipate

CI carries a **per-file** floor of 23 `CHECK_FOR_INTERRUPTS()` on `src/bm25_scan.c`, and
the file sat exactly at 23. One of them (`bm25_debug_cursor_scan`'s per-docid loop) is
inside the moving region.

The general trap — "a refactor legitimately lowers a count floor" — is already recorded in
ADRs 0035 and 0041, but always for *dedup* PRs, where consolidating N copies into one
helper genuinely removes checks. A split is different in a way that matters: it **moves** a
check rather than removing one, so the aggregate floor does not move at all (59 before, 59
after) and is structurally incapable of witnessing the change. Only a per-file pair can.

## Decision

**Move the debug surface to `src/bm25_debug.c`** — the 15 SRFs, the six private helpers
(`wand_debug_ctx_and_block`, `term_contrib_debug_cb`, the four `bm25_query_render*` AST
printers), the `TermContribDebugState` type, and `bm25_global_stats` (whose only caller is
`bm25_debug_global_stats`). `bm25_scan.c` drops to 3,992 lines and holds zero
`PG_FUNCTION_INFO_V1`. `bm25_global_stats` becomes `static` and its declaration is dropped
from `bm25.h`: a shared header declaring a symbol only one translation unit ever uses is
the same smell #67.13 named in `bm25_wand.h`, and until the split there was no file where
that was visible.

**Promote exactly four helpers through a new `src/bm25_scan.h`**, with the header's stated
rule being deliberately narrow: only declare a `bm25_scan.c` symbol when a second
translation unit genuinely calls it. "Four" counts what this split PROMOTED, and is not the
whole seam: `bm25_debug.c` also calls `bm25_scan_build_ranking`, which was already extern
via `bm25.h` long before this change, so the link-level dependency of `bm25_debug.c` on
`bm25_scan.c` is five symbols. `bm25_scan.h` says so, since a header stating a narrow rule
invites being read as the complete list. `field_corpus_stats` is renamed
`bm25_field_corpus_stats` on promotion — an extern symbol in a shared module shares one
namespace with every other extension in the backend, and the other three were already
prefixed.

The four stay in `bm25_scan.c` rather than moving with the probes because **a debug probe
is only a reference if it runs the code the real scan runs.** `sql/43_wand_parity` diffs
WAND against `bm25_debug_rank`; a probe that recomputed corpus stats or idf its own way
would silently stop witnessing anything while staying green.

**Relocate the two `bm25_wand_*` declarations** from `bm25_wand.h` into `bm25_scan.h`
(#67.13). They are defined in `bm25_scan.c` and were declared in `bm25_wand.h` with a
`(defined in bm25_scan.c)` note — a header pointing at another module's definitions. The
underlying call-graph cycle (wand → scan → wand) is **kept and documented**, not removed;
unwinding it means moving `AccEnt`/`pending_df`/`pending_score_term` into a shared stats
module, which is a larger change than this split.

**Make the uncapped-reference guarantee explicit** (#67.4): `bm25_debug_rank` and
`bm25_debug_rank_key` now pass `force_exhaustive = true`. Both previously passed `false`
and avoided the WAND branch only because `MemSet` left `so->scoring` false and the D7 gate
happens to test it.

**Lower the `bm25_scan.c` interrupt floor 23 → 22 and add a floor of 1 on
`bm25_debug.c`**, with the reasoning above stated in the workflow.

## Alternatives considered

- **Move the four shared helpers into `bm25_debug.c` too, and have `bm25_scan.c` call
  back.** Inverts the dependency: the production scan path would reach into the debug
  translation unit, so a build without the debug surface becomes impossible and the
  probes stop being subordinate to the code they witness.

- **Duplicate the four helpers into `bm25_debug.c` to avoid a new header.** Rejected
  outright: this recreates by hand the exact duplication PR-E spent #59.5 removing, and
  the copies would drift silently — the parity suite would keep passing while comparing
  WAND against a stale reference.

- **Split three ways (`bm25_scan.c` / `bm25_rank.c` / `bm25_debug.c`)**, as the finding
  itself suggested. Out of scope by explicit user decision: the scorer stays put. Noted
  because `bm25_scan.c` is still 3,992 lines and above the project's 2,000-line guidance —
  this ADR does not claim to have resolved that.

- **Take the full #67.13 fix** (a new `bm25_stats.c` holding `pending_df`, `AccEnt`,
  `field_df_cb`, `pending_score_term`). Deferred: it moves production scorer statics,
  which is a different risk class from moving test-only code. Only the declaration
  placement was corrected here.

- **Leave the interrupt floors alone and let the aggregate cover it.** Rejected: the
  aggregate is provably blind to a move, and `bm25_debug.c` at a floor of zero would have
  nothing pinning its one check.

## Consequences

- `bm25_scan.c` 5,210 → 3,992 lines; `bm25_debug.c` is 1,279; the AM's callback lifecycle
  is contiguous.
- Four helpers lose `static`. That is a real, accepted cost — the compiler can no longer
  prove they have no outside callers. The header states the narrow rule so the set does
  not creep.
- Adding a debug SRF now means editing `bm25_debug.c` and adding its `REVOKE` to the
  extension script; the file header says so.
- `bm25_wand.c` gains `#include "bm25_scan.h"`.
- **Verification.** The move is behaviour-neutral and the evidence is that suite 43's
  existing 540 lines of expected output are byte-identical (the regression diff is
  additions only); all 91 SQL suites pass. Both new interrupt floors were negative-
  controlled by deleting a check and confirming each fails.
- **The #67.4 fix was negative-controlled, not assumed.** `sql/43_wand_parity` gained a
  canary section: it first proves `wand_top_k = 5` genuinely caps at 5 on the corpus
  (`bm25_debug_wand_rank` returns 5 of 12), *then* asserts both references return 12.
  The canary is load-bearing — "returned more than `wand_top_k` rows" is also satisfied by
  a `wand_top_k` that caps nothing, the same vacuous-assertion trap `sql/85`'s HIJACKED
  twins exist to close. Injecting the hazard (`so->scoring = true` with
  `force_exhaustive = false`) turns the suite **red**; restoring `force_exhaustive = true`
  while *keeping* `so->scoring = true` turns it green again, which is what establishes the
  one-word change as load-bearing rather than cosmetic.
- **Dead includes, and the line the split draws through them.** `utils/uuid.h` and
  `funcapi.h`/`utils/array.h` became unused in `bm25_scan.c` *because* their only consumers
  moved, so removing them is part of this change. `executor/tuptable.h` was already unused
  before the split, so it is left in place and belongs to the conventions sweep — the
  intent being that every line of this diff traces to a finding this PR owns. (`uuid.h` was
  missed on the first pass and caught in adversarial review; the grep that cleared it had
  matched the include's own trailing comment.)
- `bm25_scan.c` remains over the project's 2,000-line guidance, by user decision.

## Addendum (2026-09-21)

The call-graph cycle this record kept (wand -> scan -> wand) was removed by
`docs/adr/0093`. The statistics both ranking builders share (`bm25_term_idf`,
`pending_score_term` now named `bm25_pending_score_term`, `AccEnt` now named
`BM25AccEnt`, the match-set budget and `bm25_ranked_keys_fill_from_pending`) moved to
a new `bm25_stats.c`. The two `bm25_wand_*` helpers whose declarations this record
moved into `bm25_scan.h` are now statics in `bm25_wand.c`, so `bm25_scan.h` declares
neither of them. `bm25_term_idf`, one of the helpers this record promoted, is now
declared in `bm25_stats.h`; `bm25_debug.c` includes that header and still calls the
same function the scorer calls. As a second split, it re-split the interrupt floors
for the same reason given above.
