---
id: 0035
title: Dense per-docid chain lookups resume from a forward cursor instead of the root
date: 2026-07-29
status: Accepted
summary: The LIVEDOCS, DOCMAP and NORMS readers each re-walked their page chain from the root on every call, and all three run per scored posting, so scoring was quadratic in segment size; a BM25SegReader now holds one forward cursor per chain, which every hot caller can use because they all walk docids ascending.
---

# 0035. Dense per-docid chain lookups resume from a forward cursor instead of the root

## Context

A segment stores three dense per-docid arrays, each spread over a `nextblk` page chain:
LIVEDOCS (a bitmap), DOCMAP (`ItemPointerData[]`) and NORMS (`uint32[]`, row-major by
docid, `field_count` cells per doc). All three readers had the same shape — walk from the
chain root accumulating content bytes until the target byte offset falls inside the
current page — and therefore the same cost: O(chain length) per lookup, with no
memoization and no index over the chain (review ref H16, issue #55).

All three are called **per scored posting**. `seg_posting_cb` calls
`bm25_seg_doc_is_live`, then `bm25_seg_docid_to_tid`, then `bm25_seg_doclen_field`, for
every posting of every term. `bm25_wand_cursor_score_doc` calls two of them the same way.
The merge replay is worse: `for d in 0..ndocs` calling live + tid + `field_count`
doclens, i.e. `ndocs × (2 + field_count)` full walks per source segment.

A single-field segment with 1M docs has a 4 MB NORMS chain (~490 pages, ~2036 cells per
page), so a term with `df = 500k` performed roughly 500,000 × 245 average page visits —
~1.2e8 `ReadBuffer` + `LWLockAcquire`/`Release` pairs for that one term, and the cost
grows quadratically with segment size. `bm25_seg_doclen` multiplied it by `field_count`
for cells the writer had deliberately laid out contiguously.

No wrong answers, but superlinear on the primary query path, bounded by no guardrail, and
— unlike the other known ceilings in that file, which carry explicit `ponytail:` notes —
with nothing acknowledging it.

## Decision

**Exploit the access pattern rather than building an index over the chain.** Every hot
caller walks docids *forward*: postings arrive in ascending `(docid, field_id)` order
(D-ACCUM), the merge loops `d = 0..ndocs`, and WAND only ever skips forward. One docid's
`field_count` NORMS cells are contiguous. So a caller-held cursor — the page a lookup
landed on, plus the content bytes before it — makes the next lookup resume there instead
of at the root, and ascending access becomes **O(1) amortized**: the cursor advances one
page per ~2000 docids rather than re-walking half the chain each time.

`BM25SegReader` bundles the three cursors plus the `Relation` and header pointer. Hot
callers init one per (loop, segment) and use the reader accessors:
`seg_posting_cb` holds it by value in `TermScoreCtx` (already per (term, segment)),
`BM25WandCursor` holds one initialized in `cursor_open`, and the merge replay declares one
next to its header.

**A cursor cannot change an answer, and that is structural rather than argued.** It
supplies only a *starting point*; the walk restarts from the root whenever the target is
behind the cursor; and every page actually read is still `seg_gen`-validated (option (d)).
Pages the walk now skips are pages it no longer touches at all, so skipping their
validation removes reads rather than removing checks. A reader therefore needs no
invalidation, no reset between segments to stay *correct*, and no cleanup — the cursors
are plain `(BlockNumber, uint32)` pairs, normally stack-declared. Passing `NULL` is always
correct, which is what the unchanged one-shot public functions do.

**The four public functions keep their signatures.** They became thin wrappers passing a
`NULL` cursor. That leaves the ~26 cold call sites (debug SRFs, `bm25_handler`, one-off
scan probes) untouched and confines the change to the three hot loops. `bm25_seg_doclen`
gets a *local* cursor, which is enough to read a docid's whole contiguous row in one page
visit instead of `field_count` root walks.

**`blk == BM25_METAPAGE_BLKNO` also counts as unpositioned.** `InvalidBlockNumber` is
`0xFFFFFFFF`, not 0, so a reader embedded in a `MemoryContextAllocZero`'d struct — which
`BM25WandCursor` is — would otherwise claim to be positioned at block 0 and read the
metapage. `cursor_open` does call the initializer, but block 0 is never a chain page, so
rejecting it makes an all-zeroes reader safe by construction rather than by discipline.

## Alternatives considered

- **Resolve each chain to a `BlockNumber[]` + cumulative-offset array once per (scan,
  segment) and binary-search it** — the report's suggestion. Strictly better for *random*
  access, and it would also cover a backward-jumping caller. Rejected: it needs an
  allocation with a lifetime to manage, per chain per segment, to beat O(1) amortized on
  the only access pattern that actually occurs. The cursor needs no allocation at all and
  degrades to exactly the old behavior when access is not ascending.
- **A backend-static memo keyed on `(relid, root, gen)`.** Needs no signature or struct
  changes anywhere, and is safe in principle because sealed segments are immutable and
  `gen` is globally unique. Rejected on precedent: [0007](0007-score-accessor-concurrency.md)
  replaced exactly this shape — a hidden process-global slot — with an explicit registry
  after it produced silent wrong answers. Hidden mutable state is not the house style, and
  an explicit reader is barely larger.
- **Cache the resolved chain on `BM25SegmentHeader`.** The natural home, since every
  caller already holds one per segment. Impossible: `BM25SegmentHeader` is the *on-disk*
  struct in `bm25_format.h`, and its `sizeof` is part of the layout.
- **Derive the page from arithmetic (`cells_per_page` is deterministic).** True today —
  `chain_write` never splits a value, so a page holds `floor(capacity / elemsize)`
  elements — but it hard-codes a writer invariant into the reader, and a future
  variable-fill writer would silently return wrong cells. The cursor reads `pd_lower` per
  page, exactly as before, so it stays correct under any fill pattern.

## Consequences

- Measured, development machine (Apple silicon, PG 18.3), 100k-doc single-field segment,
  `df = 100k`, exhaustive scorer, warm cache: ranked query **~500 ms → ~49 ms** (~10x).
  A 4 × 25k-doc merge: **253 ms → 154 ms**; the chains are only ~13 pages at that size,
  and the merge gain grows with segment size.
- The improvement compounds with segment size, since the old cost was quadratic in it.
  The 1M-doc case in the finding is where it matters most and is the case this suite
  cannot afford to build.
- `sql/75_seg_chain_cursor` pins the failure mode rather than the timing, over chains
  that **span pages** — a single-page chain never advances a cursor and would pass
  regardless. Reading the wrong cell of a dense array is silent, so the suite makes it
  loud: docs whose only difference is length, so the ranked top-N *is* a NORMS assertion;
  the same top-N at `wand_top_k` of 0, 5 and 100 (the exhaustive scorer and the WAND
  cursor read through separate reader instances) with bit-equal scores; a
  uniquely-marked spread of ids to catch a wrong DOCMAP cell; a delete-and-vacuum round
  for LIVEDOCS; a four-segment merge; and a three-field index for the contiguous-row
  path.
- The tie-break trap in that suite is called out in a comment: after the deletes, only
  two docs have a distinct length and the rest tie, so the assertion asks for `LIMIT 2`.
  Asking for 5 would have pinned an arbitrary ordering among ~5993 equal-scoring docs —
  the class of expected-output fragility that has bitten this suite family before.
- The WAND parity suites (`43_wand_parity`, `44_wand_skip`, `45_m2b_acceptance`) assert
  bit-identical scores between the WAND and exhaustive drivers, and both now read through
  cursors. They pass unchanged, which is the strongest available evidence that the reader
  returns the same cells.
