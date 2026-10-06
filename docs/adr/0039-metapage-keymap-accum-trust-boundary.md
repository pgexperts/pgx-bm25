---
id: 0039
title: Extend the decode boundary to the metapage, KEYMAP, accumulator and reclaim trust boundaries H6 scoped out
date: 2026-08-04
status: Accepted
summary: field_count on the metapage (a different struct from the segment header), the KEYMAP's key_type/key_size and per-key page span, the accumulator's key width, the merge idmap docid, and the retired-list page bounds are now validated at the point each is first trusted, closing the same class of hole ADR 0027 closed for the posting decode.
---

# 0039. Extend the decode boundary to the metapage, KEYMAP, accumulator and reclaim trust boundaries H6 scoped out

## Context

[0027](0027-posting-decode-boundary.md) (review ref H6) established one decode
boundary for the posting-block decode: `bm25_block_validate`, a bounded varbyte
decoder, a bounded impact-table decoder, and a `field_count` guard on the
**segment header** (`bm25_seg_header_read`'s struct). It deliberately did not
reach several other on-disk quantities that follow the identical pattern —
untrusted bytes off a page used directly as a bound, a size, or an index, guarded
by nothing or by an `Assert` that a production (non-cassert) build compiles out.
This record closes those:

- **The METAPAGE's own `field_count`.** `BM25MetaPageData.field_count` is a
  *different* struct from the segment header ADR 0027 bounded — it is read by
  `bm25_meta_read` and `bm25_scan_snapshot` on every query, not per-segment. One
  `bm25_meta_validate` check on this field is the single gate three otherwise-
  unrelated consumers reach through: `bm25_merge.c`'s `dlbf[MAX_FIELDS]`,
  `bm25_meta.c`'s own `store_pos[MAX_FIELDS]`, and `bm25_scan_snapshot`'s
  `out->field_count`, which in turn bounds `tok_by_field` / `seen` /
  `doclen_by_field` / `df_by_field[MAX_FIELDS]` throughout `bm25_scan.c` — because
  `bm25_meta_read` and `bm25_scan_snapshot` both call `bm25_meta_validate` before
  handing `field_count` to any of them. Bounding it once here is what makes every
  `MAX_FIELDS`-sized array indexed by it safe by construction, the same shape as
  ADR 0027's single gate for the segment header's `field_count`.
- **The DICT entry decode, at two walkers ADR 0027 missed.** `bm25_dictentry_validate`
  already existed for the sorted-chain lookup and the merge/debug iterator. Two
  more walkers — `bm25_debug_terms` and `bm25_debug_postings`, both in
  `bm25_segment.c` — duplicated the identical unchecked `MAXALIGN(sizeof(BM25DictEntry)
  + termlen)` pattern and were not on the list. **Five walkers share this decode
  boundary now, not three.** The two missed ones matter more than their "debug"
  name suggests: [0020](0020-debug-surface-privileges.md)'s H13 extension
  (issue #52) gated the read-only debug SRF surface on table `SELECT` plus AM
  identity, but never on ownership — so any role that can already read the
  indexed table could aim a corrupt-`termlen` page at `bm25_debug_terms` or
  `bm25_debug_postings` and walk off the page, the exact H6 hole, through a path
  H6's own audit did not enumerate.
- **The KEYMAP's key_type/key_size and per-key page span.** `bm25_seg_keymeta`
  (the one place merge, the debug SRF, and the scan-start key discovery in
  `bm25_scan.c`/`bm25_wand.c` all learn a segment's key config) trusted an on-disk
  `key_type`/`key_size` pair with no correspondence check. `bm25_seg_key`'s
  decode loop trusted `key_size` as a `memcpy` length into every caller's fixed
  `kbuf[BM25_KEY_MAX_SIZE]` stack buffer, and computed `pagebytes -= sizeof(hdr)`
  with no guard that `pagebytes` (a `uint32` only lower-bounded by
  `PageIsVerifiedExtended`'s `pd_lower >= SizeOfPageHeaderData`) was large enough
  to subtract from first.
- **The accumulator's key width.** `bm25_accum_set_keymeta`'s `key_size` — sourced
  from either a pending doc header or a source segment's KEYMAP header, both
  on-disk and untrusted — sized a `palloc0` and every subsequent `memcpy` stride
  with no bound. `bm25_accum_set_doc_key`'s per-doc `key_size` agreement check was
  `Assert`-only.
- **The merge idmap docid bound** (`bm25_merge.c`): a source segment's decoded
  posting `old_docid` indexed `idmap[]`, sized exactly `ndocs`, with nothing
  bounding the cumulative docid against that count (`bm25_block_validate` bounds
  a per-block count, never the running total).
- **The retired-list page-kind and entry-count bounds** (`bm25_fsm.c`):
  `bm25_reclaim_retired` walks a chain that, unlike the orphan sweep, has no
  independent root set to fall back on, so a stray `nextblk` landing on an
  unrelated live page was read straight through as a `BM25RetiredEntry` array,
  and a corrupt `pd_lower` overstating the entry count could write past the
  fixed `keep[]`/`drop[]` stack arrays.

`bm25_seg_page_validate` covers none of this: it compares the page opaque's
`seg_gen` against the catalog and nothing else.

## Decision

Extend ADR 0027's shape — validate at the point of read, not at each use — to
every quantity above:

- **`bm25_meta_validate`** gains a `field_count` bound (`[1, MAX_FIELDS]`),
  mirroring the segment-header check ADR 0027 already added one file over.
- **`bm25_dictentry_validate`** (unchanged) is now called from
  `bm25_debug_terms` and `bm25_debug_postings` too, closing the two missed
  walkers.
- **`bm25_seg_keymeta_validate`** (new, `bm25_keymap.c`) rejects any `key_type`
  not in `{INT4, INT8, UUID, TEXT}` or a `key_size` that does not equal exactly
  the fixed width that type implies (`bm25_key_type_width`, the existing
  OID-independent mirror of `bm25_key_type_from_oid`). Called from
  `bm25_seg_keymeta`.
- **`bm25_seg_key_header_validate`** and **`bm25_seg_key_span_validate`** (new,
  `bm25_keymap.c`) split `bm25_seg_key`'s three checks at their natural
  boundary: the root-page header step (`key_size` in `[1, BM25_KEY_MAX_SIZE]`,
  then the `pagebytes < sizeof(hdr)` underflow guard, in that order since the
  second only makes sense once the first has bounded what will be subtracted)
  and the per-page span check (`local_off + key_size <= pagebytes`, catching a
  corrupt trailing partial-key run the page-fit check alone does not).
- **`bm25_accum_set_keymeta`** rejects `key_size` outside `[1, BM25_KEY_MAX_SIZE]`
  whenever `key_type != BM25_KEY_NONE`, before it becomes an allocation size.
  Deliberately does **not** also check type/width correspondence — see the
  first non-obvious finding below.
- **`bm25_accum_set_doc_key`**'s `Assert(key_size == a->key_size)` becomes a real
  `ereport`, but with `ERRCODE_FEATURE_NOT_SUPPORTED` and a `REINDEX` hint, not
  `ERRCODE_INDEX_CORRUPTED` — see the third non-obvious finding below.
- **`bm25_merge_docid_validate`** (new, `bm25_merge.c`) rejects `old_docid >=
  ndocs` before it indexes `idmap[]`.
- **`bm25_retired_page_flags_validate`** and **`bm25_retired_count_validate`**
  (new, `bm25_fsm.c`) reject a retired-list page missing the
  `BM25_PAGE_RETIRED` flag, and an entry count past `BM25_RETIRED_PER_PAGE`,
  respectively — checked in that order (page kind before count) since a
  wrong-kind page's "entry count" is meaningless.
- **`strnlen`** now bounds the `field_name`/`stemmer_name` reads in
  `bm25_debug_fieldcfg` (`bm25_analyzer.c`) to their declared fixed widths
  (`BM25_FIELD_NAME_LEN` / `BM25_STEMMER_NAME_LEN`) instead of `strlen`, which
  trusted writer-convention NUL-termination. This one does not reject anything —
  a corrupt page with no NUL anywhere in the array now reads back the full
  width verbatim instead of scanning past it.

Every new validator is extracted to its own function — even where, unlike ADR
0027's four pieces, the surrounding function was not otherwise shared — so a
`bm25_debug_*` probe can exercise the identical check the production code path
runs, following ADR 0027's own reasoning that "a regression suite cannot
produce a corrupt page."

### Non-obvious findings

1. **Five DICT walkers, not three.** `bm25_debug_terms` and `bm25_debug_postings`
   (`bm25_segment.c`) duplicated H6's exact unchecked pattern and were outside
   ADR 0027's audited list. Both back debug SRFs gated (H13) only on table
   `SELECT`, not ownership, so an unprivileged reader who can already query the
   indexed table could reach a corrupt DICT page through either — the same class
   of hole H6 fixed, through a path H6's own review did not enumerate. The
   lesson generalizes: an audit scoped to "callers of function X" misses
   duplicated inline logic that never called X in the first place.
2. **Validating `key_size` for *consistency* is insufficient when both sides of
   the comparison come from the same untrusted source.** `bm25_accum_set_doc_key`'s
   pre-existing `Assert(key_size == a->key_size)` only proves a *later* doc's
   `key_size` agrees with the *first* one `bm25_accum_set_keymeta` was ever
   called with — a uniformly-corrupt `key_size` (wrong on every doc alike)
   sails through equality checks and still reaches the `palloc0` and every
   `memcpy` stride. Magnitude must be bounded at the point a value is *first*
   trusted (`bm25_accum_set_keymeta`), not only cross-checked for internal
   consistency downstream. Two validators exist here for this reason and are
   deliberately not merged: `bm25_accum_set_keymeta` bounds magnitude once;
   `bm25_accum_set_doc_key` checks a *different* thing (agreement across docs),
   addressed separately below.
3. **A `key_field` reloption change after build is DDL-reachable, not on-disk
   corruption — and used to brick writes with a corruption errcode.**
   `bm25_resolve_fields` re-resolves `key_field` on every `aminsert`, so
   `ALTER INDEX ... SET (key_field = ...)` between two inserts queues pending
   docs of different key widths under the ordinary reloption-ALTER path — no
   corruption involved, no cross-check the catalog performs against pending
   writes already queued under the old `key_field`. Before this fix, the
   `Assert`-only guard meant a non-cassert build's next drain would silently
   `memcpy` a wider key into a narrower slot (a heap overflow) instead of
   erroring at all. Simply turning the `Assert` into an `ereport` was not
   enough either: the natural first instinct — `ERRCODE_INDEX_CORRUPTED`, matching
   every other guard in this record — would have been the wrong signal, sending
   an operator hunting for storage corruption that does not exist. It is now
   `ERRCODE_FEATURE_NOT_SUPPORTED` with an explicit `REINDEX` hint, matching what
   actually fixes it.

## Alternatives considered

- **Leave the metapage `field_count` unbounded, reasoning that ADR 0027 already
  covers `field_count`.** It covers a *different struct* (the segment header)
  read through a different path. The metapage copy feeds `bm25_scan_snapshot`
  directly and was never touched by that fix.
- **Check `key_size` type/width correspondence inside `bm25_accum_set_keymeta`
  too**, folding it together with the magnitude bound. Rejected: the
  accumulator's contract is "however wide the caller says, allocate that much
  and stay in bounds" — it has no independent notion of which `key_type`
  implies which width (that mapping lives in `bm25_keymap.c`, on the other
  side of a module boundary this accumulator does not otherwise depend on).
  Bounding magnitude here and correspondence in `bm25_seg_keymeta_validate`
  keeps each validator answering the one question its own module can answer
  without new coupling.
- **`ERRCODE_INDEX_CORRUPTED` for the key-width-change guard**, matching every
  other validator in this record. Rejected per finding 3 above: it is not
  corruption, and the errcode is operator-facing signal, not decoration.
- **A single mega-validator function called once per segment/accumulator/merge
  pass**, mirroring `bm25_block_validate`'s single-call shape. Rejected: unlike
  the posting block (one struct, decoded once, whose fields are all known at
  the same point), these checks arrive at different times from different
  sources (metapage read, KEYMAP header read, per-key page walk, per-doc
  accumulator call, per-posting merge callback, per-page reclaim walk) — forcing
  them into one function would mean passing irrelevant state through call sites
  that do not have it yet.

## Consequences

- A corrupt metapage, KEYMAP header, KEYMAP page, accumulator input, merge
  source segment, or retired-list page now aborts the statement with
  `ERRCODE_INDEX_CORRUPTED` (or, for the one DDL-reachable case,
  `ERRCODE_FEATURE_NOT_SUPPORTED` with a `REINDEX` hint) naming the
  inconsistency, instead of indexing a stack array out of bounds, wrapping a
  `uint32` subtraction, or silently overflowing a heap allocation.
- Eight new `bm25_debug_*` probes (`meta_field_count_validate`,
  `dictentry_validate`, `keymeta_validate`, `seg_key_bounds`,
  `accum_set_keymeta`, `merge_docid_validate`, `retired_page_bounds`,
  `bounded_name_bytes`) run the same validators — extracted to their own
  functions for this purpose, following ADR 0027's own reasoning — over
  caller-supplied values, because a regression suite cannot manufacture a
  corrupt page. All are pure functions of their arguments, open no relation,
  and are covered automatically by the install script's REVOKE loop
  ([0020](0020-debug-surface-privileges.md)).
- The key-width-change guard is instead covered end-to-end with real DDL (a
  `CREATE INDEX ... WITH (key_field=...)`, an insert, an `ALTER INDEX ... SET
  (key_field=...)`, a second insert, then a drain) rather than a probe, since
  it is reachable without any page corruption.
- `sql/78_trust_boundary_bounds` covers the negative paths and the DDL-reachable
  case; the positive direction — that no guard rejects what the writer emits —
  is covered by the other 83 suites plus a short positive control in this one
  (a keyed, multi-doc index built, sealed, and ranked through every validator
  above).
- A small per-call cost: a handful of comparisons per metapage read, per
  KEYMAP header/key fetch, per accumulator call, per merge posting, and per
  retired-list page. No suite timing moved outside noise.

## Addendum (2026-08-05)

Two more sites of this record's own shape — an on-disk quantity used as an array
index while guarded only by an `Assert` — were found on the merge-replay path and
promoted to real `ERRCODE_INDEX_CORRUPTED` errors: the `field_id` bounds in
`bm25_accum_add_posting` and `bm25_accum_add_positions_to_last`.

The gap is a width mismatch between two correct bounds. `bm25_field_rle_decode`
bounds a decoded field id to `MAX_FIELDS`, which is right for the scorer's
`MAX_FIELDS`-wide arrays — but the merge accumulator's `AccumDoc.doclen_by_field`
is `palloc0`'d only `field_count` wide. An id in `[field_count, MAX_FIELDS)`
therefore passes the decode and then indexes past the end of the per-doc arrays:
the POST pass reads `doclen_by_field[fid]` and the POS pass reads
`store_pos[fid]` out of bounds, baking garbage doclens into the merged segment's
norms and impact tables. Silent corruption propagation, in exactly the production
builds where `Assert` is compiled out — the same argument this record already made
for `bm25_accum_set_doc_key`.

Enumerated per ADR 0040's countermeasure rather than fixed only where the issue
named it. All five `Assert(field_id < a->field_count)` sites in `bm25_accum.c`
were given a verdict: the two above take ids decoded off a source segment and are
now real checks; `bm25_accum_add_field_tokens` (field id from the index's own
column mapping) and `bm25_accum_ndocs_field` / `bm25_accum_doc_len_field` (the
builder's own loop variable) are caller-controlled, never on-disk, and stay
`Assert`s. That verdict is recorded in the code so the next sweep does not
re-litigate it.

Covered by `sql/78_trust_boundary_bounds` via a new `bm25_debug_accum_field_bound`
probe, following this record's own "a regression suite cannot produce a corrupt
segment" pattern: it drives the real functions over caller-supplied values through
a throwaway accumulator. Both directions per entry point, including the
exactly-at-limit case and the `[field_count, MAX_FIELDS)` shape that is the actual
corruption. Verified non-vacuous — neutralising both guards fails the suite, with
the out-of-range id returning instead of erroring.
