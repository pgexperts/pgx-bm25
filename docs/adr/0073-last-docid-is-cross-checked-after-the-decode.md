---
id: 0073
title: A posting block's last_docid is cross-checked against its decoded run, in the one reader that materializes the run
date: 2026-08-20
status: Accepted
summary: last_docid was the only BM25BlockHeader field no reader validated, and two distinct WAND behaviours trust it; the check cannot live in bm25_block_validate because the run does not exist yet when that runs, so it is a separate validator called immediately after the decode loop in wand_cursor_load_block — the only reader that materializes a docids array.
---

# 0073. A posting block's last_docid is cross-checked against its decoded run, in the one reader that materializes the run

## Context

`BM25BlockHeader.last_docid` was the one header field no reader checked against
anything. `bm25_block_validate` never read it. Two separate WAND behaviours trust
it, and they fail in opposite directions:

- **Overstatement.** `bm25_wand_cursor_next_geq` selects a landing block because
  the peeked `hdr.last_docid >= target`, then scans
  `while (pos < ndocs && docids[pos] < target)`. If `last_docid` claims a docid
  larger than the run actually contains, `pos` walks off `docids[]`. At a full
  block (`ndocs == BM25_POSTINGS_PER_BLOCK == 128`) `docids[128]` aliases
  `tfs[0]`; at a shorter block it yields a stale docid left by the *previously
  decoded* block, which then drives `score_doc` to read `tfs[]`/`fields[]` from
  that same stale window. A confidently wrong score on an ordinary ranked query,
  not a crash.
- **Understatement.** The block-max deep check computes
  `skip_target = min_last + 1` and relies on it exceeding the pivot — an invariant
  that holds only if `last_docid` really is the block's maximum. A value that
  understates it makes every `next_geq` return at its "already at/past target"
  guard, nothing advances, and the driver loop spins, cancellable only by
  `CHECK_FOR_INTERRUPTS`.

The originating review filed these as two findings (QRY-08 and QRY-07) and
proposed folding both into `bm25_block_validate`. That is impossible as stated:
`bm25_block_validate` runs on the *header*, before the varbyte deltas are
expanded, so the value it would need to compare against does not exist yet.

## Decision

Add `bm25_block_last_docid_validate(hdr, docids)` — a separate validator asserting
`docids[ndocs - 1] == hdr.last_docid` — and call it immediately after the decode
loop in `wand_cursor_load_block`.

Exact equality, not a bound. The encoder writes `last_docid = docids[n-1]` by
construction, so equality is the strongest available statement and costs nothing
more than an inequality.

**Only one of the three block readers calls it, and that is a decision rather than
an omission:**

- `wand_cursor_load_block` materializes the whole run into `cur->docids[]`, is the
  reader whose consumers trust `last_docid`, and is the default ranked path. It
  calls the validator.
- `bm25_seg_block_header_read` never expands the deltas — it is the header-only
  *peek* `next_geq` uses to choose a landing block. There is nothing to compare
  against.
- `bm25_seg_scan_postings` streams each posting to a callback, carrying only a
  running `prev`. It never materializes the run, so there is no array to compare
  against without restructuring its innermost loop — and it does not consume
  `last_docid` in the first place.

The peek therefore remains unvalidated by construction, and this record states the
residual plainly rather than claiming the finding is fully closed: a
peeked-and-**skipped** block whose `last_docid` *understates* its true maximum has
its postings bypassed with no error — silently missing rows under corruption, which
is the failure shape this change elsewhere converts into loud ones. Validating it
would require decoding the very block the peek exists to avoid decoding, so closing
it means giving up header-only skipping. What the check does close is every use of a
`last_docid` that has been **decoded**: the landing scan cannot run off the run, and
the deep check's `skip_target` cannot stall on a value the block does not support.

Separately, `skip_target = min_last + 1` is made saturating. With `last_docid` now
cross-checked, reaching `PG_UINT32_MAX` requires a segment with 2^32 local docids
rather than one corrupt header field — but the arithmetic is made total anyway,
because the cost is a comparison and the failure mode is a hang.

## Alternatives considered

- **Bound `last_docid` inside `bm25_block_validate` without the run** — e.g.
  reject `PG_UINT32_MAX`, or compare against the segment's `ndocs`. Closes the
  narrow wrap case only. It cannot catch the far wider and more damaging case: a
  `last_docid` that is a perfectly plausible number and simply does not match the
  block it heads.
- **Validate in all three readers for symmetry** — neither of the other two has a
  materialized run to compare against, so this is not a choice so much as a
  restructuring proposal; see the next entry for what it would cost. (An earlier
  draft of this record justified the omission by claiming `bm25_seg_scan_postings`
  stops early at `emitted == df` and so would reject well-formed blocks. That is
  wrong: blocks are written per term at seal, so on a well-formed index `emitted`
  reaches `df` exactly at the final block's final posting, and a mid-block stop
  happens only when the dict `df` already disagrees with the blocks. Adversarial
  review caught it. The conclusion is unchanged — there is still no array to check
  — but the stated reason was not the real one.)
- **Have `bm25_seg_scan_postings` decode the full run so it too can check** — a
  real cost on the innermost decode loop of every scored and `@@@` scan, to
  validate a field that path does not consume.
- **Validate the peek by decoding it** — defeats the entire point of a header-only
  peek, which exists so `next_geq` can reject a block without decoding it.

## Consequences

- A block header disagreeing with its own run raises `ERRCODE_INDEX_CORRUPTED`
  naming both values, on the default ranked path, before either failure mode can
  express itself.
- The out-of-bounds `docids[]` read and the non-terminating deep-check loop are
  both closed by the same check.
- `bm25_block_validate` and `bm25_block_last_docid_validate` are now a pair, split
  by *when the fact becomes knowable* rather than by topic. Anything else that can
  only be checked post-decode belongs in the second one.

## Addendum (2026-10-04)

The fresh-eyes review of this date found that `bm25_seg_scan_postings` never checks that a POST chain yields df postings: a chain cut short is read as a short list on the `@@@`, phrase and exhaustive paths (WAND's next() raises on an Invalid link, but its next_geq skip path under-answers), and the next merge writes the truncated list into a well-formed segment, making the loss permanent. The reason recorded here for not cross-checking last_docid at that site (no materialized array to compare against) does not hold for a running-prev check, which can compare prev against hdr.last_docid once a block is consumed. Tracked in #293.

## Addendum (2026-10-05)

Three changes from #293 and #294 (ADR 0111).

- The streaming reader now checks too. `bm25_seg_scan_postings` compares its running `prev`
  with the header's `last_docid` after each block, the running-prev comparison the 2026-10-04
  addendum said was possible, and requires `emitted == df`. WAND's global-bound sweep enforces
  the same count. `wand_cursor_load_block` keeps this record's check of the decoded run.
- The peek's `last_docid` is still not validated, and the cross-block order check on the WAND
  side exists only in the straddle peek (`wand_lead_continues`), which runs only for a
  multi-field index. Single-field WAND reads a backwards block without complaint. That and the
  POST visit cap belong to #303.
- Since #289 (ADR 0113) the writer cuts blocks at document boundaries, so a non-final block
  holds 97 to 128 postings, not necessarily 128. The `ndocs == 128` aliasing case above applies
  only to a block that does hold 128.
