---
id: 0045
title: One shared scan-start prologue; the WAND debug probes gate like the scorer
date: 2026-08-05
status: Accepted
summary: The five hand-copied corpus-stats prologues and three df-to-idf blocks collapse into two helpers, which makes the analyzer fingerprint gate uniform across scoring and debug paths.
---

# 0045. One shared scan-start prologue; the WAND debug probes gate like the scorer

## Context

Every ranked path opened with the same sequence: one atomic snapshot, the §I
analyzer fingerprint gate, `pending_global_stats`, the global avgdl, the
field-config load, and the per-field avgdl. It existed as **five** hand-maintained
copies — `bm25_scan_build_ranking_exhaustive`, the dispatcher's WAND branch,
`bm25_debug_wand_rank`, `wand_debug_ctx_and_block`, and `bm25_wand_stats` — and
the per-term df→idf block existed as three.

The dispatcher's own comment defended this, arguing that sharing would need "more
machinery than duplicating roughly eighty lines."

The duplication had already drifted, which is the part that mattered:

- **`wand_debug_ctx_and_block` never ran the fingerprint gate.** It went from
  snapshot straight to `pending_global_stats`. The copy that created it dropped
  the gate and nothing afterwards could notice.
- **That same copy had no `CHECK_FOR_INTERRUPTS` anywhere**, in a tree that had
  just landed a pass making ranked and `@@@` scans cancellable.

The bound-safety probes (`bm25_debug_block_ub` versus `bm25_debug_term_contrib`)
are the only check that `bm25_block_ub` never under-bounds a real contribution,
and both sides of that comparison are computed from this copy's stats — so a
drift here moves both numbers together and the safety gate keeps passing while
comparing numbers the real scorer no longer uses.

## Decision

Collapse to two file-local statics in `bm25_scan.c`:

- `bm25_scan_corpus_stats(...)` — snapshot, gate, global and per-field stats.
- `bm25_term_idf(...)` — the per-term df sweep and idf derivation, returning
  whether the term has any df at all.

Differences between call sites are expressed as **plain scalar parameters and
nullable out-params, never boolean mode flags**: `qfield` and `boost` are scalars
(the sites that had no boost factor pass `1.0`, which is bit-exact for every
value `bm25_idf` can produce), and `store_pos` is NULL where the position bits are
not wanted.

**The fingerprint gate becomes uniform.** `wand_debug_ctx_and_block` now resolves
an analyzer config and gates like every other path.

Both helpers carry `CHECK_FOR_INTERRUPTS` in their segment loops, so the debug
probe gains cancellability it never had.

The dispatcher's comment defending the duplication is deleted — the extraction
disproves it. The phrase-positionless predicate got the same treatment in the
same pass (`bm25_phrase_fields_have_positions` /
`bm25_phrase_segments_have_positions`), split in two so the jsonb surface can test
one field scope per phrase leaf without re-reading every segment header per leaf.

## Alternatives considered

- **Leave it duplicated and just add the missing gate to copy #4** — restores
  parity once and leaves five copies to drift again. The gate omission is the
  symptom; five hand-maintained copies is the cause.
- **One helper with boolean mode flags** (`gate_fingerprint`, `want_store_pos`,
  …) — a fake-generic helper with a pile of flags is harder to read than the
  duplication it replaces. Rejected in favour of scalars and nullable out-params,
  which turned out to cover every real difference.
- **Keep `qcfg` nullable so copy #4 could keep skipping the gate** — would have
  preserved the exact drift being removed, in the signature.

## Consequences

- The five WAND debug SRFs can now ERROR on an analyzer-fingerprint mismatch
  where they previously ran regardless. This is the intended behaviour change:
  on such an index the real scanner refuses to run, so the probe's numbers were
  meaningless anyway. Verified no regression suite alters the analyzer in front
  of these SRFs — only `sql/42_format_v5.sql` and `sql/43_wand_parity.sql` call
  them, and neither touches `language`/`stemmer`.
- WAND's bit-exactness against the exhaustive scorer (D8) now rests on both
  consuming stats from literally the same code, rather than from two copies that
  happen to agree.
- **A known test gap is not closed by this.** `sql/43_wand_parity.sql` does
  cross-check the debug copy's stats against the exhaustive path — but every
  corpus in it is sealed before probing, so `pending_global_stats` contributes
  zero on both sides. A change to how pending folds into `live_ndocs` — the exact
  drift this record exists to prevent — would still slip through. Adding a
  probe with a live pending arm is open follow-up work.
- `bm25_debug_field_stats` deliberately keeps its own partial prologue: it reports
  raw per-field stats with no gate and no pending fold, and routing it through the
  helper would change what it returns.
- **The CI interrupt-check floor drops 62 → 59, and that drop is the point.** Three
  `df→idf` copies carried 2 + 2 + **0** checks; the one helper carries 2. Two
  phrase-positionless segment sweeps carried 1 each; the one helper carries 1. All
  −3 are in `bm25_scan.c`, every one is de-duplication rather than deletion, and
  runtime cancellability strictly *increased* because the debug copy had none at
  all. This is the same shape as the 44 → 42 drop in ADR 0035, and it is exactly
  why that record added a per-file floor: a raw aggregate cannot distinguish a
  de-duplicated check from a deleted one. `bm25_scan.c` therefore gains its own
  floor (23), verified to fail when a single check is removed — the two new helpers
  are now shared chokepoints, so losing a check in either de-cancels several call
  sites at once while the aggregate stays satisfied by any unrelated addition.
- **`ci/check_scan_scratch.py` (the H17 / ADR 0036 ordering floor) had to be
  re-pointed**, because it greps for `bm25_scan_snapshot(` inside the three hot
  scan paths and the snapshot now happens one level down, inside
  `bm25_scan_corpus_stats`. The ordering property itself is unchanged — both
  scorer paths still create their scratch context first. It now accepts either
  call form, *and* separately asserts that `bm25_scan_corpus_stats` still contains
  the snapshot: without that second assertion, removing the snapshot from the
  helper would leave the scorer paths matching a call that allocates nothing, and
  the check would report OK while guarding nothing. Both failure modes were
  verified to exit 1 before the change was accepted.

## Addendum (2026-10-05, PRs #331-#350)

One more prologue step that both ranking builders hand-copied, discovering the index's
`(key_type, key_size)` before projecting ranked-row keys, is now one helper,
`bm25_ranked_key_config` in `bm25_stats.c` (#303.I, PR #343). It answers from the key stamp
when the index has one (ADR 0112's addendum) and keeps the full discovery otherwise.
