---
id: 0030
title: A ranking rebuild drops the score index and resumes by row identity
date: 2026-07-29
status: Accepted
summary: The WAND over-pull tail rebuild left score_by_tid/score_by_key describing the capped ranking and resumed at a saved ordinal into a freshly-snapshotted array; both builders now invalidate the hashes where they reassign so->ranked, and the tail resumes after the last emitted TID.
---

# 0030. A ranking rebuild drops the score index and resumes by row identity

## Context

When the executor pulls past `wand_top_k` rows, `bm25_gettuple` lazily rebuilds the
full exhaustive ranking and resumes. Two things about that rebuild were wrong
(review ref H9, issue #49).

**The score index went stale.** The rebuild replaced `so->ranked` / `so->scores` /
`so->nranked` but left `so->score_by_tid` and `so->score_by_key` pointing at hashes
built from the WAND-capped top-k. `bm25_build_score_index`'s own header states the
precondition this breaks: it "assumes so->ranked/scores/ranked_keys are already at
their FINAL nranked (fully loaded)". A hash built from a k-row array therefore
answers NULL for every row k+1..N once the ranking has been extended — a silent
wrong answer, NULL substituted for a real score, whose presence depends on
`bm25_native.wand_top_k`, a GUC documented as performance-only.

Those two fields were assigned `NULL` in exactly one place, `bm25_rescan`, and
neither ranking builder touched them.

**The resume was by ordinal.** The rebuild goes through
`bm25_scan_build_ranking`, which calls `bm25_scan_snapshot` again and therefore sees
the index as of *now*, not as of the first build. Restoring the saved `rcur` into a
differently-ordered array replays a row already returned to the executor, or skips
one that was never returned.

## Decision

**Invalidate at the assignment, not at the caller.** Both ranking builders set
`so->score_by_tid = NULL; so->score_by_key = NULL;` immediately before they
reassign `so->ranked`. Clearing them in `bm25_gettuple` after the tail rebuild — the
report's first suggestion — fixes today's only rebuild path and leaves the next one
to rediscover the bug. The old hashes live in `scanctx` and die with the scan, so
leaking them for one statement is harmless.

**Resume by row identity.** Before the rebuild, remember the TID of the last row
handed to the executor; after it, continue from just past wherever that TID lands in
the new ranking. Ranked rows are deduplicated by TID, so the match is unique, and
when nothing changed it is found at `resume - 1` and the behavior is identical to
the ordinal. O(nranked), once per scan. If the row is gone from the new ranking
(deleted and vacuumed mid-scan) there is no position to resume after, so it falls
back to the ordinal — which is what the code did unconditionally before.

## Alternatives considered

- **Carry the first build's `BM25ScanSnapshot` and reuse it for the tail rebuild.**
  The report's other suggestion, and more principled: it restores the invariant that
  makes resume-by-ordinal correct in the first place, and preserves the documented
  "already-streamed rows are the exhaustive top-k prefix" property exactly. Rejected
  as the larger change — the snapshot carries a copied segment catalog with its own
  lifetime, and it would have to be threaded through the retry wrapper and the seam
  dispatcher, both of which deliberately re-snapshot on the seg_gen abort retry.
  Resume-by-TID is self-contained in `bm25_gettuple` and degrades gracefully.
- **Track every emitted TID and skip them after the rebuild.** Fully correct
  regardless of what changed, but it needs storage proportional to `wand_top_k`,
  which the user can set arbitrarily high, to fix a race that only reorders rows.
- **Never rebuild; make WAND always produce the full ranking.** Discards the entire
  point of block-max WAND.

## Consequences

- A score accessor probing a row the ranking has since grown to include now
  resolves. Measured on the report's 6-row shape at `wand_top_k = 2`: before, every
  probe that was not the current row returned NULL (the report saw a score on
  exactly one row, and only because the current-row fast path matched it); after,
  every row emitted from the rebuilt ranking resolves.
- **A probe issued before the over-pull still returns NULL, and that is not this
  bug.** At that moment the ranking genuinely holds only k rows and the probed row
  is not among them, so there is no score to report. Closing that gap would require
  a score-accessor miss to trigger the exhaustive rebuild *mid-projection*, which
  changes the accessor's no-mutation contract ([0007](0007-score-accessor-concurrency.md))
  and mutates `so->ranked` while `bm25_gettuple` holds a cursor into it. That
  deserves its own decision; `sql/72_wand_tail_score_index` pins the current
  boundary so any future change to it is visible.
- The tail rebuild costs one extra O(nranked) pass to re-anchor the cursor, once per
  scan at most.
- `sql/72_wand_tail_score_index` also pins that the ranked sequence, the row count,
  the distinct-row count and the per-row scores are identical at `wand_top_k` of 0,
  1 and 2 — the WAND == exhaustive contract, which is what would break if
  resume-by-TID ever replayed or skipped a row.

## Addendum (2026-10-04)

The "Resume by row identity" half of this decision is replaced (issue #268); the
score-index invalidation half stands unchanged.

Resuming after the last emitted TID's position in the new ranking assumed that
ranking ordered the emitted rows the way the capped one did. It did not: the rebuild
recomputed the corpus statistics from its fresh snapshot, and those count every valid
pending TID (committed or not, aborted, or the scanning transaction's own), tombstone
and merge, so a write between the builds re-scored every row. Rows the new scores
moved past the last emitted one were emitted again, rows they moved ahead of it were
skipped, and the distances stopped being monotonic. The ordinal fallback, taken when
the TID was missing, resumed one slot off.

Now the capped WAND build pins the inputs a score is computed from (per-field avgdl
and k1/b/boost, and each query token's idf) in `so->stats_pin`, and the tail rebuild
scores under them, so every document both builds saw keeps a bit-identical score. The
tail then resumes at the first entry strictly after the last emitted (score, TID) in
`scored_desc` order (`bm25_tail_resume_pos`, a binary search). That position is exact
whether or not the last emitted row is still ranked, so there is no fallback. The
scan is re-positioned on the entry just before the resume point, which is the last
emitted row whenever it is still ranked, as before. The invariant is now stated and
checked under cassert after the tail rebuild: a rebuild may only extend the emitted
prefix, and emitted rows' scores are immutable. `sql/120_wand_tail_stats_pin` covers
it, and `t/024_wand_tail_vanished_last.pl` the case the fallback got wrong: a dead
entry the executor discarded, vacuumed by another session before the rebuild, after
which the ordinal skipped the next row. ADR 0005's 2026-10-04 addendum has the rest.
