---
id: 0071
title: On-disk block pointers are gated before ReadBuffer, and deliberately not bounded against the relation's extent
date: 2026-08-20
status: Accepted
summary: A shared bm25_seg_blkno_validate rejects InvalidBlockNumber (which is P_NEW, so an unchecked corrupt pointer silently extends the relation from a read-only scan) and the metapage block; it omits the RelationGetNumberOfBlocks bound that looks like its obvious third clause because ReadBuffer already errors past EOF and RelationGetNumberOfBlocks costs an lseek per call, not for any correctness reason.
---

# 0071. On-disk block pointers are gated before ReadBuffer, and deliberately not bounded against the relation's extent

## Context

Segment roots, chain links and DICT pointers are `BlockNumber`s read straight off
index pages. `BM25SegCatEntry.header_blkno` reaches `ReadBuffer` via
`bm25_seg_header_read`, `bm25_seg_header_read_lens` and two debug SRFs; a DICT
entry's `post_root` and a POST page's `nextblk` reach it via
`wand_cursor_load_block`. Neither `bm25_scan_snapshot` nor `bm25_segcat_read`
validates any field of a catalog entry — they bound the entry *count* against the
page's content length and nothing else — so every one of those pointers arrived
unchecked.

The value that makes this urgent is `InvalidBlockNumber`, because **`P_NEW` is
`InvalidBlockNumber`**. A pointer corrupted to `0xFFFFFFFF` therefore does not
fail the read. It *extends the relation*, from a read-only scan, and returns a
freshly zeroed page. Nothing about that is loud.

What follows is worse than merely quiet. The zero page reaches
`bm25_seg_page_validate_kind`, whose first test is the gen check (ADR 0062, and
that ordering is correct for its own reasons), so an all-zero opaque raises
`ERRCODE_T_R_SERIALIZATION_FAILURE` — "segment reclaimed concurrently; retry" —
which `bm25_scan_build_ranking` catches and retries up to three times. One corrupt
block number can thus extend the relation three times in a single query and then
surface a misleading retry error rather than a single `ERRCODE_INDEX_CORRUPTED`.

## Decision

Add `bm25_seg_blkno_validate(blkno, what)` and call it immediately *before* every
`ReadBuffer` that takes a block number sourced from a page. It rejects exactly two
values:

- `InvalidBlockNumber`, because it is `P_NEW` and would extend the relation;
- `BM25_METAPAGE_BLKNO`, which is a real readable page but never a segment page,
  so without naming it the gen check reports a retryable race three times instead
  of reporting corruption once.

The guard lives in the callee (`wand_cursor_load_block`) rather than at its call
sites, so a future fourth caller cannot reintroduce the hole.

**It does not bound `blkno` against `RelationGetNumberOfBlocks`**, for two reasons,
neither of which is correctness:

- *Redundancy.* For any block past EOF `ReadBuffer` already raises a loud
  `ERRCODE_DATA_CORRUPTED` short-read error out of `mdreadv`. Out-of-range is the
  case the buffer manager handles well; `InvalidBlockNumber` is the one it handles
  by growing the file, which is why only that one needs naming.
- *Cost.* `RelationGetNumberOfBlocks` reaches `mdnblocks`, which `lseek`s on every
  call in a normal backend — `smgrnblocks_cached` consults its cache **only under
  `InRecovery`**, by its own documented contract ("lack of a shared invalidation
  mechanism for changes in file size"), verified in both PG 17 and PG 18. The
  validator runs once per posting-block load, so that syscall would land in the
  innermost loop of every ranked scan.

If a future change does want the bound, the shape to copy is `pos_cursor_load`'s,
already in this tree: capture `nblocks` once at cursor open and compare against the
captured value, paying one `lseek` per scan rather than one per block.

## Alternatives considered

- **Also reject `blkno >= RelationGetNumberOfBlocks(index)`** — the shape the
  originating review proposed, and the one a reader will reach for. Declined on
  redundancy and cost, as set out above. It is worth recording that the first
  draft of this record declined it on a *third*, wrong ground: that a per-backend
  `nblocks` cache would go stale under a concurrent merge and reject a
  legitimately new segment page. Adversarial review caught it. The cache is
  recovery-only in both supported versions, so that failure mode does not exist,
  and this tree's own `pos_cursor_load` has bounded chain blocks against
  `RelationGetNumberOfBlocks` all along without incident. The decision survives
  the correction; the reasoning did not, and the wrong version is preserved here
  because it is the more memorable one.
- **Validate the whole `BM25SegCatEntry` at `bm25_segcat_read`** — closer to the
  source and would cover more fields at once, but the catalog reader has no
  relation context for some checks and the entry's other fields have genuinely
  different validity rules (see ADR 0072 and the tombstone-fraction guard). Kept
  the per-quantity validator convention the tree already uses instead.
- **Assert rather than `ereport`** — the codebase has repeatedly been bitten by
  `Assert` being compiled out in exactly the build where a corrupt page matters.
  Not seriously considered.

## Consequences

- One corrupt block pointer now raises `ERRCODE_INDEX_CORRUPTED` once, naming
  which pointer, instead of silently extending the relation up to three times and
  then reporting a retryable serialization failure.
- The relation can no longer grow as a side effect of a `SELECT`.
- A block number past EOF still produces the buffer manager's own error rather
  than ours. That is intentional; anyone reading a short-read error from this
  extension should not go looking for a missing bm25 guard.
- `bm25_debug_stamp_seg_field_count`, the test-only corruption lever, now also
  validates the page kind before poking `field_count`. It exists to corrupt one
  field of a well-formed header, not to write into whatever page a bad catalog
  entry names.
- Cost is two integer comparisons per segment-header read and per posting-block
  load. No `smgrnblocks` call — hence no `lseek` — was added to any hot path.
- All three block readers are gated, including `bm25_seg_block_header_read`, which
  only peeks a header and was missed in the first pass. That one matters more than
  its "peek" framing suggests: `wand_cursor_sweep_global_ub` calls it at cursor
  OPEN, before any gated `wand_cursor_load_block`, so it is the first thing a
  corrupt `post_root` reaches on a ranked query.

## Addendum (2026-09-22)

The Consequences bullet above — "a block number past EOF still produces the buffer
manager's own error rather than ours ... should not go looking for a missing bm25
guard" — is now true only of this record's own three **per-pointer** sites
(`bm25_seg_header_read`, `bm25_seg_header_read_lens`, `bm25_seg_block_header_read`,
plus `wand_cursor_load_block`). Every **per-walk** chain loop in `src/bm25_seg_read.c`
now does raise a bm25-named `ERRCODE_INDEX_CORRUPTED` for an out-of-extent link, under
ADR 0095, which capitalizes on the escape hatch this record left open: capture `nblocks`
once per walk rather than per pointer. A reader who sees a short-read error from this
extension should now check which of the two kinds of site produced it.

ADR 0095 also reports a *measured* cost for one shape of the bound this record
declined. Its first landing gave `chain_read_at` a `RelationGetNumberOfBlocks` on the
arm taken when no cursor is supplied; six production call sites reached that arm per
posting, and the default ranked path slowed measurably. This record's cost objection
was therefore right about the per-pointer case and right about the mechanism; ADR 0095
records what happened when the boundary between "per pointer" and "per walk" was drawn
in the wrong place, and how it was repaired (those callers were given a
`BM25SegReader`, which hoists the extent out of the per-posting path entirely).

## Addendum (2026-10-04)

The fresh-eyes review of this date found the block-0 gate not applied at the chain-hop and keymap_root ReadBuffer sites, so a corrupt link of 0 reads the metapage and surfaces as a retryable 40001 after three attempts instead of INDEX_CORRUPTED. Tracked in #303.

## Addendum (2026-10-05)

The same hazard as `InvalidBlockNumber` as a block pointer reappears at the docid-to-TID
boundary (#294, ADR 0111). A DOCMAP cell holding `InvalidBlockNumber` reached the heap fetch,
whose `ReadBuffer` reads it as `P_NEW`, so a read-only SELECT extended the heap. The single
chokepoint `seg_docid_to_tid_cur` now rejects a block of `InvalidBlockNumber` and an offset
outside `FirstOffsetNumber` to `MaxOffsetNumber`. The upper bound is `MaxOffsetNumber` and not
`MaxHeapTuplesPerPage`, because bm25 does not restrict the table access method.

## Addendum (2026-10-05, PRs #331-#350)

The write half of this record is applied (#302.A/B, PR #341). `bm25_meta_validate`, which
every metapage reader passes, refuses block 0 in all five metapage pointers (`pending_head`,
`pending_tail`, `segcat_root`, `retired_head`, `field_config_blkno`). Block 0 is never a
legitimate value, and it is the one value that turned a corrupt pointer into a hang: the
catalog appender holds the metapage EXCLUSIVE when it reads `segcat_root`, so a root of 0
re-locked the page it held and waited on itself with interrupts held. The gate replaces the
hand-rolled block-0 tests in the pending appender and `bm25_segcat_first_entry`. Where a
pointer leads after that is the dereferencing site's check: the catalog appender validates the
root's role (ADR 0062's addendum) before appending in place, and the pending appender checks
the tail page's kind and that it is not DELETED after each `LockBuffer`, before allocation and
before the WAL window. Sealed-chain links to block 0 are refused by the walker (ADR 0120).
