---
id: 0108
title: A WAND over-pull rebuild may only extend the emitted prefix
date: 2026-10-04
status: Accepted
summary: When a WAND-capped scan is pulled past wand_top_k, the tail rebuild scores under the first build's pinned statistics, resumes at the first entry strictly after the last emitted (score, TID) key, and a cassert build checks the invariant that a rebuild may only extend the emitted prefix and emitted rows' scores are immutable.
---

# 0108. A WAND over-pull rebuild may only extend the emitted prefix

## Context

A WAND-capped scan fills only the top `wand_top_k` rows. If the executor pulls past
them, `bm25_gettuple` rebuilds the full exhaustive ranking and resumes where the first
build stopped. Issue #268 asked what that resume owes the rows already handed out.

It was reproduced deterministically. The rebuild recomputed its
scoring inputs from a fresh snapshot, and the corpus statistics that snapshot yields
(live N, avgdl, df, so idf) count every valid pending TID with no visibility check.
Any INSERT between the two builds moved them, whether committed, uncommitted, aborted
or the cursor's own transaction's, and so did a VACUUM or a merge. The per-field
`k1_<col>`, `b_<col>` and `boost_<col>` reloptions were re-read as well. The tail was
then scored on a different scale from the rows already emitted: distances stopped
being monotonic, and resuming after the last emitted TID re-emitted rows or skipped
them. When that TID had vanished, the ordinal fallback resumed one slot off. The
default `wand_top_k` reaches this too, because `LIMIT 5` under a selective filter qual
pulls past the capped 100. ADR 0005's and ADR 0030's 2026-10-04 addenda hold the
reproduction detail and the corrections to earlier claims; this record does not repeat
them.

The seam had been touched before, one symptom at a time. ADR 0030 (#49, review ref H9)
fixed a stale score index and resumed by TID. #242, #252 and #253 (ADRs 0103, 0104,
0105) concern which scan's ranking a projected row belongs to, and #253 also has the
rebuild re-position the scan on the last emitted row. #267's item 5 (lazy key
resolution) is entangled with the rebuild, because keys resolved after it must come
from the rebuilt ranking. The user asked for the property that those fixes share to be
recorded as an invariant, and for #268 to be fixed without a separate design pass:
a rebuild may only extend the emitted prefix, and emitted rows' scores are immutable.
The grouping is the user's. Rationale beyond what each ADR says was not recovered from
project sources.

## Decision

**The invariant.** A rebuild may only extend the emitted prefix; the scores of rows
already emitted are immutable.

**Stats pin.** The capped WAND build pins what a document's score is computed from in
`so->stats_pin` (`BM25PinnedStats`): per-field avgdl, the per-field k1, b and boost,
and each query token's per-field idf. Live N and per-field N are not kept, because they
reach the score only through idf. Only the forced tail rebuild consumes the pin. That
rebuild still reads postings and membership from a fresh snapshot and still runs
`bm25_term_idf` for its dictionary lookups, but scores with the pinned values, so a
document both builds saw keeps a bit-identical score. The 40001 retry and ordinary
builds stay fresh. A capped scan is always a plain OR text query, so the rebuild's work
list matches the pin's token ordinals, and the rebuild raises XX000 if the query now
analyzes to a different token count.

**Resume by key.** The tail resumes at the first entry strictly after the last emitted
(score, TID) in `scored_desc` order (`bm25_tail_resume_pos`, a binary search). That
position is exact even when the last emitted row has vanished. It replaces ADR 0030's
resume-by-TID and the ordinal fallback.

**Cassert check.** After the tail rebuild, `bm25_tail_check_emitted` hashes the emitted
rows, which `wand_top_k` bounds, and makes one pass over the rebuilt array. A row that
appears in both with a different score is a violation only if the scan's snapshot can
see the row. A TID names a document only while its heap line pointer lives, so an
emitted row the executor discarded as dead can be vacuumed between the builds and its
line pointer reused by a new row with its own score. A row the snapshot can see cannot
have been vacuumed. The heap probe runs only on a mismatch.

## Alternatives considered

- **Reuse the first snapshot for the rebuild.** Rejected by the user decision. It does
  not fix pending-append drift. ADR 0005's addendum adds that it would need the segment
  pages behind that snapshot held against reuse for the life of the cursor, while pinning
  the statistics needs nothing held and keeps the seg_gen retry working.
- **A strict continuation**, keeping the fresh statistics but dropping rows whose new
  score exceeds the last emitted one and skipping emitted TIDs. Rejected by the user
  decision because it silently drops rows.
- **A separate design pass.** Not run, by the user decision.

## Consequences

- Membership is still fresh. The rebuilt ranking can hold rows the scan's snapshot
  cannot see, placed by their pinned-scale scores, and the executor's visibility check
  drops them as it drops any dead index entry.
- The pin freezes the statistics model and does not change it. Aborted, uncommitted and
  own-transaction pending entries still count in idf and avgdl for every scan (an
  aborted one until a seal tombstones it); that is the pre-existing model, noted at the invariant's
  comment in `bm25_scan.c`.
- The per-field k1, b and boost reloptions can change under an open scan; the pin
  covers them for the tail. ALTER TEXT SEARCH DICTIONARY takes no index lock, so a
  dictionary change between fetches can alter the token count and trip the XX000 check
  above instead of being re-analysed. The reloption lock-level facts are in ADR 0012's
  2026-10-04 addendum.
- The cassert check covers the emitted rows' scores only. It does not check that no
  visible, unemitted row lands before the resume key; the function header states its
  scope.
- Lazy key resolution (#267 item 5, a residual) has to fill the remaining keys before
  the score-by-key hash is built and re-key on a rebuild.
- Any later change that replaces a scan's ranking mid-scan must keep the invariant.
  `sql/120_wand_tail_stats_pin` and `t/024_wand_tail_vanished_last.pl` pin it: exact
  float equality against a `wand_top_k = 0` reference across same-transaction,
  aborted-insert and mid-scan-seal changes, and a dead last entry vacuumed by another
  session before the rebuild.

## Addendum (2026-10-05)

Two additions from 2026-10-05. `bm25_tail_check_emitted` now also asserts membership: every
row the rebuilt ranking places at or before the resume key, and that the scan's snapshot can
see, must have been emitted (#289, ADR 0113); it had compared scores only for rows present in
both builds. And a scan with WHERE keys of its own (#290, ADR 0109) is always built by the
exhaustive scorer, so it is never capped and never reaches this rebuild.
