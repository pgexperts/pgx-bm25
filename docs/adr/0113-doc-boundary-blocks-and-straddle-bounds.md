---
id: 0113
title: Posting blocks end at document boundaries, and the WAND reader bounds a straddling document on every segment
date: 2026-10-05
status: Accepted
summary: The segment writer cuts a posting block at the last document boundary that fits in 128 postings (97 to 128 per block, no format change), while the reader sums per-field maxima for the global bound and scores a straddler's continuation exactly in the deep check, because old segments keep their straddles.
---

# 0113. Posting blocks end at document boundaries, and the WAND reader bounds a straddling document on every segment

## Context

On a multi-field index a document holds one posting per field for a term, adjacent in
the term's run, and the writer cut a block every 128 postings with no regard for
documents. The last document of a block could continue into the next one: `(d, title)`
ends block A and `(d, body)` opens block B. `bm25_block_ub` bounds only the postings its
own block holds, so neither block's bound dominated `d`. Both WAND prune sites then
under-bounded it. The pivot test's per-term global bound was the max over blocks of the
per-block sums, and the deep check summed only the resident blocks' bounds before
shallow-skipping past the straddler when it was the last document of the block ending
the skip window. The straddler was dropped from WAND top-k results, and through the
over-pull tail's resume-by-key from unlimited ranked scans too (issue #289).

ADR 0043 states what the bound owes: domination of the exhaustive scorer's contribution,
not bit-exactness. The domination claim held per block and was false for a document that
spans two. Existing suites could not catch it: every multi-field WAND corpus was small and
field-homogeneous, so the straddles were harmless.

## Decision

**Writer.** `bm25_segment_build_orphans` ends each block at a document boundary. The cut
backs off to the start of the document the count boundary would split, so a block holds
between `BM25_POSTINGS_PER_BLOCK - (BM25_MAX_FIELDS - 1)` and `BM25_POSTINGS_PER_BLOCK`
postings, that is 97 to 128 (the last block of a run, fewer). It never holds more than
128, which every reader already accepts (`bm25_block_validate`), and no reader assumes a
block is full, so there is no format change. A `StaticAssert` pins
`BM25_MAX_FIELDS <= BM25_POSTINGS_PER_BLOCK`, which keeps every document inside one block
and an old straddler inside two.

**Reader, global bound.** `wand_cursor_sweep_global_ub` takes, per field, the max over
blocks of that field's bound, and sums those maxima over fields. That dominates a
document however its postings are split. The sweep already reads every impact table, so
it adds no I/O.

**Reader, deep check.** The deep check computes the skip target first. For an aligned
cursor whose block's last document lies in the skip window and continues into the next
block, it adds that document's continuation scored exactly: its postings there (the next
block's "lead") with their own `doclen`s. The lead is read at block load when the next
block is on the same page and peeked lazily otherwise, and is consulted only when the
resident bounds alone would prune. A next block starting before the resident block's last
docid is refused as corrupt. A dead straddler still adds its bound, which can only cost
pruning.

**The reader fix is for every segment.** Old segments keep their straddles until a merge
rewrites them, and nothing on disk says which segment is which.

**Test lever.** `bm25_native.debug_count_slicing` (`PGC_SUSET`, off) restores the old
count-based cut so the suites can still build an old layout; `bm25_debug_block_spans`
exposes per-block first and last docids. It is `PGC_SUSET` for `debug_budget`'s reason:
it changes the physical layout of shared on-disk state.

**Cassert check.** `bm25_tail_check_emitted` now also asserts membership: every row the
rebuilt ranking places at or before the resume key, and that the scan's snapshot can see,
must have been emitted. It had compared scores of rows present in both builds, so it was
blind to a missing row. See ADR 0108.

**Tests.** `sql/125_wand_straddle` (the issue's single- and two-segment reproductions on
the old layout, with and without LIMIT, against the exhaustive scorer; the new writer's
layout; the lever's privilege) and a field-heterogeneous three-field case in
`sql/43_wand_parity` whose straddles also cross page boundaries.

## Alternatives considered

- **Add the next block's bound in the deep check.** The first reader candidate. In the
  A/B on 100,000-document corpora with two and three fields on the count-sliced layout,
  it scored up to 24% more documents than the pre-fix build in the worst cell. The exact
  continuation scored at most 4.3% more.
- **Add the next block's bound only for a true straddle** (the next block's first docid
  equals the resident block's last), the refinement the issue recommended. It measured
  +10% in the worst randomized cell against +30% without it. The user accepted up to
  about +10% docs scored in the worst cell, subject to an interleaved A/B that enters
  the deep-check regime; the implemented exact-continuation form measured below that.
- **Let a block grow past 128 postings to finish the document.** A format break:
  `bm25_block_validate` refuses `ndocs > 128`, so an older binary would raise
  `INDEX_CORRUPTED` on new segments. It would need a format-version bump with a
  `min_read_version` floor (ADR 0009) and resizing of the fixed `[128]` arrays in the
  reader, among other sites.
- **A segment-header feature bit marking a segment straddle-free**, so the reader could
  skip the straddle handling there. Deferred by the user's decision; the reader cannot
  otherwise tell a straddle-free segment from an old one.

## Consequences

- WAND returns the same members and order as the exhaustive scorer on old and new
  layouts. Pruning on old segments costs up to about 4.3% more documents scored in the
  worst measured cell.
- A block-count-sensitive expected output on a multi-field index can shift, since blocks
  now hold 97 to 128 postings instead of exactly 128.
- Segments heal as merges rewrite them; the reader code stays.
- Two corruption errors added with the reader are unreachable from the tests (#309).
