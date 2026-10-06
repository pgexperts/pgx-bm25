---
id: 0111
title: A chain walk is held to the length its header implies, and a fallback that only serves corruption is an error
date: 2026-10-05
status: Accepted
summary: The POST, LIVEDOCS, DOCMAP and NORMS walkers raise INDEX_CORRUPTED (REINDEX hint) when a chain ends early, a page is not the writer's full span, or a count or last_docid disagrees with the header, instead of answering live, an invalid TID or doclen 0; the docid-to-TID chokepoint rejects an impossible TID.
---

# 0111. A chain walk is held to the length its header implies, and a fallback that only serves corruption is an error

## Context

The builder writes every segment chain to an exact length, but several readers had a
fallback for a chain that was shorter than its header said. Each fallback could only
ever serve corruption (issues #293, #294):

- `bm25_seg_scan_postings` ended a term's run at `nextblk == InvalidBlockNumber` or at
  the last page's last block without checking that it had emitted the DICT `df`, and
  never compared its running `prev` with a block's `last_docid`. WAND's `next_geq` read
  an Invalid peek link as exhaustion. The result was short `@@@`, exhaustive and phrase
  answers.
- `chain_read_at` returned false when a LIVEDOCS, DOCMAP or NORMS walk reached the chain
  end before an in-range offset, and its three callers turned that into "live", an
  invalid TID and `doclen` 0. A tombstoned row could come back at a reused TID.
- `seg_livedocs_all_set` counted bytes past the end as set (ADR 0100).
- An `InvalidBlockNumber` TID reached the heap fetch, which passes it to `ReadBuffer`
  as `P_NEW`, so a read-only SELECT could extend the heap.
- A merge replay carried all of it into a new segment that looked healthy, so the loss
  became permanent.

ADR 0095 had added extent bounds that error instead of ending a walk, and ADR 0073 the
`last_docid` cross-check in the one reader that materializes a block's run. Neither asked
that a walk end where the header said it would.

The user decided on 2026-10-05 that this is a contract applied to every chain family
and not a per-site patch: an expected length or count, the page kind, the generation or
epoch, the extent, and the full span of each non-final page. It was applied to POST,
LIVEDOCS, DOCMAP and NORMS in the change for #293 and #294, and to the pending chain in
the change for #291 (ADR 0110). It is an error, with no repair: a chain that is short is
corrupt, and a repair would have to guess which bytes were meant. The strict walk covers
`wand_cursor_sweep_global_ub` as well.

## Decision

**Failures raise `ERRCODE_INDEX_CORRUPTED` with the hint "REINDEX the index."**

**POST** (`bm25_seg_scan_postings`):
- `emitted` must equal `df` at the end of the run;
- a block may not claim more postings than the run has left;
- `prev` must equal the header's `last_docid` after each block;
- a block may not start below the previous block's `last_docid`. The comparison is
  `>=`, not `>`, because one multi-field document can straddle two blocks, which a segment
  written before ADR 0113 can still contain.

`wand_cursor_sweep_global_ub` enforces the same count, and `next_geq` raises on an
Invalid link while postings remain.

**LIVEDOCS, DOCMAP and NORMS:**
- `chain_read_at` raises at the chain end and on any page it walks past that is not the
  writer's full span for the kind. `bm25_chain_full_span` is the one definition: the page
  capacity for LIVEDOCS, rounded down to a whole `ItemPointerData` for DOCMAP and a whole
  `uint32` for NORMS.
- `seg_livedocs_all_set` and `bm25_livedocs_locate` apply the same rules, and
  `bm25_livedocs_clear` bounds its byte by the page content. This reverses ADR 0100's
  "bytes past the chain's end count as set".
- `bm25_segheader_validate` rejects a missing root on a segment with documents.

**The docid to TID chokepoint.** `seg_docid_to_tid_cur` is the single place a DOCMAP cell
becomes a TID, for every scan path, the merge replay, bulkdelete's callback and the debug
SRFs. It rejects a block of `InvalidBlockNumber` and an offset outside
`FirstOffsetNumber` to `MaxOffsetNumber`. A chain check cannot catch a corrupt cell in an
intact chain, such as a zeroed one, and the builder only writes heap TIDs.

The upper bound is `MaxOffsetNumber`, not `MaxHeapTuplesPerPage`. The user's decision of
2026-10-05 named `MaxHeapTuplesPerPage`, and the commit message for the change repeats
it; the code and its comment use `MaxOffsetNumber`, and the user was told of the change.
The reason is that bm25 does not restrict the table access method, and a non-heap access
method may use offsets a heap page never would. An offset inside the bound but past the
page's last item is the table AM's to refuse, and reads as "no such tuple".
`ItemPointerIsValid` alone is not enough, because it tests only that the offset is
nonzero, so `(InvalidBlockNumber, 1)` passes it.

**Test levers.** `bm25_debug_stamp_chain_next` now takes `post`, `live`, `docmap` and
`norms`; new `bm25_debug_stamp_chain_lower`, `_stamp_post_block`, `_stamp_docmap_tid`,
`_stamp_seg_root` and `bm25_debug_livedocs_clear`. Suite `sql/124_strict_chain_walkers`.

## Alternatives considered

- **Repair, or keep a lenient fallback with a WARNING.** The user decided on error only.
  A fallback that serves corruption is what produced the silent short answers and the
  merge laundering.
- **Bound the TID offset by `MaxHeapTuplesPerPage`.** Tighter for heap, wrong for any
  other table AM; see above.
- **A visit cap for the POST walk.** Not in this change; see Consequences.

## Consequences

- A corrupt index now fails loudly where it used to return a plausible wrong answer.
  Every one of these errors is on a path that a healthy index never takes.
- **Residual, owned by #303 section A:** `bm25_seg_scan_postings` has no visit cap. The
  df-bounded `emitted` count and the cross-block order check end a cycle through pages
  that hold blocks, but a cycle through POST pages holding zero blocks never advances
  `emitted` and spins (cancellably). A one-page self-loop holding a single one-document
  block, where first equals last, does not trip the strict `<` order check and emits `df`
  duplicate docids before the count stops it. The count bound covers non-empty cycles
  only.
- **Residual, #303:** WAND's `wand_cursor_load_block` has no cross-block docid order
  check. The only one on the WAND side is `wand_lead_continues`, which runs inside the
  straddle peek and only when `wand_continuation_can_score` is true, which is multi-field
  indexes. Single-field WAND reads a backwards block without complaint. It is
  corruption-only (a checksum-valid bad page). The effect of a backwards block on the
  pivot logic was not run.
- Applied to the pending chain by ADR 0110.

## Addendum (2026-10-05, PRs #331-#350)

Both residuals above are closed by `BM25SegWalk` and its family invariants (ADR 0120, PR
#346): `bm25_seg_scan_postings` gains a visit cap and refuses a page entered mid-run that
yields no block, and orders blocks on `(docid, field)`, which rejects the single-document
self-loop; WAND's sweep, `next_geq` peek and decoder hold blocks to ascending order. This
record's contract is now implemented by one reader on the segment side, as ADR 0110 does on
the pending side, and is extended with block 0, a revisit test, a visit cap and KEYMAP's full
span. The residuals that remain are listed in ADR 0119 and ADR 0120.
