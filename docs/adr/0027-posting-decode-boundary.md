---
id: 0027
title: One decode boundary validates every on-disk quantity before it becomes a bound, a size or an index
date: 2026-07-29
status: Accepted
summary: Block headers, varbyte runs, impact tables and segment-header field_count are validated where they are read, replacing Asserts that were compiled out in exactly the build where a corrupt page matters.
---

# 0027. One decode boundary validates every on-disk quantity before it becomes a bound, a size or an index

## Context

The posting decoder read counts, lengths and ids straight off a shared-buffer page
and used them as loop bounds, `memcpy` sizes and array indices with no validation
(review ref H6, issue #46). Each site was guarded, if at all, by an `Assert` —
absent in a production build, which is the build where a corrupt page matters.

The justification in the code was that "sealed pages are trusted (option-(d)
gen-validated)". That does not hold. `bm25_seg_page_validate` compares the page
opaque's `seg_gen` against the catalog and nothing else; it says nothing about any
block-header field. A torn page, a bit flip, or a hostile page image in a restored
or copied data directory passes it carrying an arbitrary header.

The sites:

| Site | On-disk value | Consequence |
|---|---|---|
| `bm25_seg_scan_postings` | `hdr.ndocs` | bounds a 128-entry, 512-byte **stack** array; 65535 wrote 256 KB over the frame |
| `bm25_seg_scan_postings` | `docid_bytes` / `tf_bytes` | cursors never bounded by the page end; a 0x80 tail walked the varbyte decoder indefinitely, and `shift` passed 31 (UB) |
| `bm25_field_rle_decode` | `field_id` | reaches the posting callback, which indexes MAX_FIELDS-wide per-field arrays |
| `bm25_seg_block_header_read` | `impact_bytes`, `nfields` | three uint16s sum to ≤196605, so the impact pointer could land ~190 KB past an 8 KB page; `nfields` = 255 wrote ~2.7 KB past a 388-byte stack struct |
| `bm25_seg_header_read` | `field_count` | loop bound over caller arrays documented ">= MAX_FIELDS", against MAX_FIELDS stack arrays in `field_corpus_stats` — on the scored-scan hot path |

The same unguarded decode exists in `bm25_wand.c`'s `wand_cursor_load_block`,
which the report does not mention. That is not a lesser path: WAND is on by
default, so it is where an ordinary ranked query decodes its blocks.

The identical `field_count` quantity is *already* validated one file over, in
`bm25_fieldcfg_read` — so this was an inconsistency, not a considered trust
decision, and the file already fails loud with `ERRCODE_DATA_CORRUPTED` on
comparable on-disk inconsistencies elsewhere.

## Decision

One decode boundary, so no raw on-disk quantity reaches a bound, a size, or an
index unchecked. Four pieces, each at the point of read rather than at each use:

- **`bm25_block_validate`** (new, in `bm25_seg_build.c` beside the encoder it
  mirrors) checks `0 < ndocs <= BM25_POSTINGS_PER_BLOCK`, that the docid and tf run
  lengths are consistent with `ndocs` (each posting varbyte-encodes to between 1
  and `BM25_VARBYTE_MAX_BYTES` bytes, so the runs are bracketed), and that the
  whole block fits before `pd_lower`. Returns the block length so callers do not
  recompute it. Called by all three block readers, including the WAND cursor.
- **`bm25_varbyte_decode` takes an `end`.** Overrunning the run, or a sequence
  wider than a uint32, is `ERRCODE_DATA_CORRUPTED`. This also removes the
  shift-past-31 undefined behaviour.
- **`bm25_decode_impact_table`** rejects `nfields > MAX_FIELDS` and cross-checks
  the table length against `1 + 9 * nfields`. The length check is the stronger of
  the two: it catches a plausible-looking `nfields` the surrounding block header
  does not account for.
- **`bm25_segheader_validate`** rejects `field_count` outside `[1, MAX_FIELDS]`,
  matching `bm25_fieldcfg_read`.

Every bound is satisfied by construction in `encode_block`, which asserts
`0 < n <= BM25_POSTINGS_PER_BLOCK` and varbyte-encodes exactly `n` docid deltas and
`n` tfs. So the guards cannot reject anything the writer emits.

## Alternatives considered

- **Validate at each use rather than at the read.** Five sites today and a new one
  every time someone adds a reader; `wand_cursor_load_block` is the proof — it
  already duplicated the pattern and the report missed it.
- **Keep `Assert` and rely on the cassert CI job** ([0008](0008-cassert-ubsan-ci.md)).
  That build never sees a corrupt page, so the Asserts would still never fire, and
  production still smashes the stack.
- **Cap `bm25_varbyte_decode` at `BM25_VARBYTE_MAX_BYTES` internally** instead of
  taking an `end`, avoiding a signature change across seven call sites. It fixes
  the unbounded walk and the UB but still permits a read of up to five bytes past a
  run, which on a page whose `pd_lower` is at `BLCKSZ` leaves the buffer. Not good
  enough for a hardening change whose whole claim is that nothing unchecked reaches
  a bound.
- **A checksum or per-block CRC.** A far larger format change for corruption that
  `data_checksums` already detects at the page level.
- **Restructuring `bm25_seg_scan_postings` to copy postings out before invoking the
  callback** — this is the issue's `:913` checklist item, and it is a *different*
  problem (buffer-lock hold duration and nested acquisition, not on-disk trust). It
  does not share this fix, the report's own verifier downgraded it to low with "no
  correctness consequence is reachable today", and a lock-restructuring belongs in
  its own change with its own reasoning. Left open deliberately.

## Consequences

- A corrupt page now aborts the statement with `ERRCODE_DATA_CORRUPTED` naming the
  inconsistency, instead of smashing a stack frame or reading off the page, on both
  the exhaustive and the WAND ranked paths.
- `bm25_varbyte_decode`'s signature changed; all seven call sites pass the run end.
  The two pending-list position-blob decoders bound by `te->pos_bytes`.
- Three new `bm25_debug_*` probes (`varbyte_decode_bytes`, `block_validate`,
  `impact_decode_bytes`) run the same validators over caller-supplied values,
  because a regression suite cannot manufacture a corrupt page. They are pure
  functions of their arguments, open no relation, and are covered automatically by
  the install script's REVOKE loop ([0020](0020-debug-surface-privileges.md)).
  `bm25_debug_block_validate` takes header FIELDS rather than a struct bytea so the
  suite does not depend on host endianness or padding.
- `sql/69_decode_boundary` covers the negative paths; the positive direction — that
  no guard rejects what the writer emits — is covered by the other 74 suites, every
  one of which decodes real blocks through this path.
- A small per-block cost: a handful of integer comparisons per block, and one
  pointer comparison per varbyte. No suite timing moved outside noise.
- The `:913` lock-hold item from issue #46 is **not** addressed here; see
  Alternatives.

## Addendum (2026-10-05, PRs #331-#350)

Four more in-range values are bounded at their decode boundaries under ADR 0119's scope
(#303 D, F, G, H; PR #343):

- **Field-config identity.** `bm25_fieldcfg_read` requires entry i to carry field id i. An
  id past `field_count` over-read the phrase stash, `0xFFFFFFFF` (`BM25_FIELD_ALL` once
  cast) widened a scoped query to every field, and two swapped ids answered one column's
  query from the other.
- **Strictly ascending positions.** The sealed POS decoder, the pending phrase stash and the
  pending drain refuse a position list with a zero delta after the first or a wrap. No
  readable analyzer revision writes one (ADR 0087 dedups within a run).
- **Segment field count.** `bm25_field_corpus_stats` refuses a segment header whose
  `field_count` differs from the index's, instead of summing into uninitialized slots or
  silently under-counting.
- **Pending field id at the drain.** `drain_doc_add_part` raised nothing and dropped the
  term entry (an `Assert(false)` on cassert), sealing the document without those postings.
  It now raises `ERRCODE_INDEX_CORRUPTED`, as the merge path already did.

The two write-path refusals (positions at the drain, the pending field id) block every seal
until REINDEX rather than lose postings silently (D6). The query-side pending readers still
skip an out-of-range field id without raising (residual, ADR 0119). An Invalid field-config
root is now XX002, not 55000 (PR #345). The `#312.3` hardening (`bm25_seg_page_off_validate`
before `bm25_seg_block_header_read_lead` forms its pointer, PR #349) applies this record's
"validate where read" rule one step earlier; WAND's `wand_cursor_load_block` still forms
`contents + off` before its range check (no access happens past the page; hygiene).
