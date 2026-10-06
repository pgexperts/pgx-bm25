---
id: 0040
title: Bound the page content length itself, below every decode boundary that measures against it
date: 2026-08-04
status: Accepted
summary: bm25_page_content_bytes is now the single read-side derivation of a page's content byte count everywhere in the tree, closing an arithmetic underflow that let a corrupt pd_lower defeat every decode-boundary check built on top of it (ADR 0027, ADR 0039), plus three narrower siblings — a pending-list entry span, chain_read_at's memcpy span, and the POS varbyte cursor's width bound.
---

# 0040. Bound the page content length itself, below every decode boundary that measures against it

## Context

[0027](0027-posting-decode-boundary.md) (H6) and [0039](0039-metapage-keymap-accum-trust-boundary.md)
each added decode boundaries for what a page's content is *interpreted as* —
`bm25_block_validate` for a posting block, `bm25_dictentry_validate` for a DICT
entry, `bm25_seg_key_span_validate` for a KEYMAP key, and so on. Every one of
those validators is handed an `end`/`pend` pointer (or a `pagebytes` count) to
check against. Across the whole tree, every one of those bounds was derived the
same unchecked way, inline at each call site:

```c
end = (char *) pg + ((PageHeader) pg)->pd_lower;          /* or */
pagebytes = ((PageHeader) pg)->pd_lower - SizeOfPageHeaderData;
```

`PageIsVerifiedExtended` (`storage/bufpage.c`, `PIV_*` flags) checks
`pd_lower <= pd_upper <= pd_special <= BLCKSZ` — it bounds `pd_lower` from
**above only**. It does not, and by design cannot, bound `pd_lower` from below:
`PageIsEmpty` (`storage/bufpage.h`) is defined as `pd_lower <= SizeOfPageHeaderData`,
and an uninitialized/zeroed page (`PageIsNew`) is `pd_lower == 0`. Both are
legitimate, expected on-disk states, not corruption. So a torn page, a bit
flip, or a hostile page image in a restored data directory can carry a
`pd_lower` anywhere in `[0, SizeOfPageHeaderData)` while still passing
`PageIsVerifiedExtended` outright, or passing `bm25_seg_page_validate`'s
`seg_gen` check (which examines the page opaque only, nothing in the header).
`pd_lower - SizeOfPageHeaderData` then underflows to just under `SIZE_MAX` (or
`UINT32_MAX`, at the sites still using a `uint32` accumulator), and every
downstream length/offset comparison built against that value reads "plenty of
room" instead of corruption — a bound is only as trustworthy as the value it is
measured against, and this value was, until now, the one thing in the chain
nobody checked.

This is not hypothetical for this codebase specifically: ADR 0039's own
`bm25_seg_key_header_validate` comment asserted that `pagebytes` was "only
lower-bounded by `PageIsVerifiedExtended`'s `pd_lower >= SizeOfPageHeaderData`"
— stating, as settled fact, a guarantee `PageIsVerifiedExtended` does not
actually provide. Under that belief, `bm25_seg_key_header_validate`'s
`pagebytes < sizeof(hdr)` check was assumed to catch every case where the
KEYMAP root page lacked room for its header. It does not: if `pd_lower` is 0,
the *unchecked* subtraction that produced `pagebytes` wraps to just under
`UINT32_MAX` first, and `UINT32_MAX < sizeof(hdr)` is false — the guard added
for exactly this purpose does not fire, because the value it is comparing was
already garbage before the comparison ran. ADR 0039 fixed the entries
(`bm25_seg_key_span_validate`, the DICT/KEYMAP/retired-list bounds, and the
rest) without fixing the one quantity every one of them was measured against.
This record is that fix, one layer down.

## Decision

Add `bm25_page_content_bytes(Page pg)` (`bm25_seg_read.c`, declared
`bm25.h`) as the single decode boundary for a page's content byte count:

```c
if (pd_lower < SizeOfPageHeaderData || pd_lower > pd_upper || pd_upper > BLCKSZ)
    ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), ...));
return (Size) (pd_lower - SizeOfPageHeaderData);
```

`pd_lower == SizeOfPageHeaderData` returns 0 and is explicitly **not** an
error — that is `PageIsEmpty`'s own definition, a legitimate freshly-initialized
or emptied page, not corruption.

Route every read-side derivation of a page's content length through it. As of
this sweep that is: `chain_read_at` and the DICT-chain walkers
(`bm25_seg_dict_lookup`, `bm25_seg_dict_iter_next`, `bm25_debug_segterms`,
`bm25_debug_terms`, `bm25_debug_postings`); `bm25_seg_key` (KEYMAP);
`bm25_pending_iter_begin`; `pos_cursor_open`/`pos_cursor_next_page` (POS
cursor); the SEGCAT walkers (`bm25_scan_snapshot`, `bm25_segcat_read`,
`bm25_segcat_find_entry`, `bm25_segcat_locate_entry`); the field-config walker
(`bm25_fieldcfg_read`); and all four retired-list walkers
(`bm25_reclaim_orphans`'s Phase 4, `bm25_reclaim_retired`,
`bm25_debug_retired_page_bounds`, `bm25_debug_retired_count`). The three
POST-block readers (`bm25_seg_scan_postings`, `bm25_seg_block_header_read`,
`wand_cursor_load_block`) route through it too — they already had an explicit
`cur + sizeof(hdr) > pend` check that a corrupt `pd_lower` trips unconditionally
(memory-safe), but the guard's *outcome* there was "no blocks on this page": a
term's postings vanishing silently rather than an error, the same silent-wrong-
answer shape as the DICT walkers, and on the WAND path (default ranked scan)
that is an ordinary query, not a debug probe.

Two write-side sites are deliberately **not** routed through it:
`chain_write`/`chain_write_stream`/`bm25_keymap_write`/`bm25_fieldcfg_write`
read `pd_lower` back only on a page the same call just `bm25_page_init`'d —
not an on-disk trust boundary, since nothing untrusted has touched the page
yet — and `bm25_segment_build_and_commit`'s segcat-capacity check
(`pd_lower + entrysize > pd_upper`) is addition-based, so it cannot underflow
the way this function guards against, even though it does read back an
existing page. See `bm25_page_content_bytes`'s header comment for the full
caller list and the exact reasoning for both exceptions.

Three narrower fixes shipped in the same pass, all downstream of the same
underflow shape:

- **`bm25_pending_term_entry_span(cur, end)`** (new, static, `bm25_pending.c`)
  — the pending-list analogue of `bm25_dictentry_validate`: bounds one on-page
  pending term entry's fixed header, then its MAXALIGN'd
  `(header + termlen + pos_bytes)` span, before either raw on-page `uint16` is
  trusted as a memcpy length or a cursor stride. Shared by
  `bm25_pending_iter_next`'s stride loop and `drain_doc_add_part`'s re-decode —
  one struct, one stride rule, one place it is checked, following ADR 0027's
  own extraction pattern.
- **`chain_read_at`'s memcpy span check.** The existing `off < seen + pagebytes`
  guard only bounded the read's *start* offset; nothing bounded `len` past that
  point, so a page truncated mid-record by a corrupt `pd_lower` could have the
  memcpy copy page slack — uninitialized bytes past the last whole record —
  into `*dst` as a NORMS doclen, a DOCMAP tid, or a LIVEDOCS bitmap byte. Added
  `off - seen + len > pagebytes` alongside it, and widened
  `BM25ChainCursor.seen` (and `chain_read_at`'s local `seen`/`pagebytes`) from
  `uint32` to `Size` — a chain whose total content exceeds 4 GB would otherwise
  wrap the accumulator itself, which the per-iteration cast alone cannot fix.
- **`pos_cursor_varbyte`'s byte-count bound.** Unlike `bm25_varbyte_decode`,
  this cursor was already memory-safe (`pos_cursor_byte` errors once the POS
  chain is exhausted), but nothing bounded `shift`: a stream whose
  continuation bit never clears pushed `shift` past 31 on the fifth-and-later
  byte, undefined behaviour for a `uint32` shift. Bounded to
  `BM25_VARBYTE_MAX_BYTES`, reusing the same constant `bm25_varbyte_decode`
  bounds its own run against.

`bm25_field_rle_decode` also gained a symmetric tail check
(`p != end || idx != n`, catching overshoot as well as the pre-existing
undershoot check) and `encode_block`/`bm25_segment_build_orphans` had their
dead `field_ids ? ... : 0` NULL guards removed in favor of an `Assert` and a
contract comment — `bm25_accum_term_postings` never returns NULL, so the guard
never fired and `NULL + off` (had it ever been reached) is a wild pointer, not
a caught case. Tree-wide, every `ERRCODE_DATA_CORRUPTED` became
`ERRCODE_INDEX_CORRUPTED`, unifying on-disk-corruption reports under one
SQLSTATE class.

### Non-obvious findings

1. **A bound is only as trustworthy as the value it is measured against.**
   ADR 0039's DICT/KEYMAP checks, and this sweep's own pending-entry spans,
   were all validated against an `end` (or `pagebytes`) derived from raw
   `pd_lower`. Fixing the *entries* without fixing the *bound* left the whole
   layer resting on an untrusted number — the KEYMAP underflow example above
   is not a hypothetical, it is what ADR 0039's own code did.
2. **`PageIsVerifiedExtended` does not lower-bound `pd_lower`.** It enforces
   `pd_lower <= pd_upper <= pd_special <= BLCKSZ` and nothing else
   (`storage/bufpage.c`'s `PageIsVerifiedExtended`, `storage/bufpage.h`'s
   `PageIsEmpty`/`PageIsNew`). PostgreSQL deliberately admits
   `pd_lower <= SizeOfPageHeaderData`, down to and including 0, as a normal
   page state — VACUUM and page initialization both produce it. A checked-in
   comment in this codebase (ADR 0039, `bm25_seg_key_header_validate`) stated
   the opposite as settled fact; this record is partly a correction of that
   belief, not only of the code built on it.
3. **Silent is worse than loud.** The pending iterator, the DICT walkers, and
   the POST-block readers all failed *safe for memory* on a corrupt
   `pd_lower` but returned "empty page" — documents vanishing from results,
   terms silently dropped mid-merge, postings missing from a WAND scan.
   Memory safety and answer correctness are different properties, and the
   first does not imply the second; every one of these was memory-safe and
   still wrong.
4. **Three consecutive fixes each landed one reader short.** ADR 0039 missed
   `bm25_seg_keymeta` and two `bm25_segment.c` DICT walkers (closed by that
   record after the fact); this pass initially missed the bound its own new
   checks were built on, plus a third retired-list walker
   (`bm25_reclaim_orphans`'s Phase 4) whose failure mode is premature page
   free during VACUUM, not a decode error. The countermeasure: grep for the
   defect's *shape* — `pd_lower - SizeOfPageHeaderData` / `((PageHeader) pg)->pd_lower`
   as a raw length — tree-wide and enumerate every site with a verdict,
   rather than fixing only the sites an issue report happened to name.
   `bm25_page_content_bytes`'s header comment now carries that exhaustive
   list so the next pass has it to check against.
5. SQLSTATE unified on `ERRCODE_INDEX_CORRUPTED` (was a mix with
   `ERRCODE_DATA_CORRUPTED`) so an operator can match one class across every
   on-disk corruption report in the extension.

## Alternatives considered

- **Fix each of the ~20 call sites' subtraction inline**, checking
  `pd_lower >= SizeOfPageHeaderData` at each one. Rejected for the same reason
  ADR 0027 extracted `bm25_block_validate` instead of patching each decode
  site: a duplicated check drifts, and the next new reader (there have been
  three waves of "one reader short" already — see finding 4) would as likely
  omit it as include it. One function, one place it can be gotten right.
- **Fold the bound into `bm25_seg_page_validate`**, since every chain reader
  already calls it. Rejected: `bm25_seg_page_validate` runs once per page
  fetch and checks `seg_gen` against the catalog, a segment-identity check;
  several callers of `bm25_page_content_bytes` (the SEGCAT/retired-list/
  pending walkers) do not have a segment generation to check against at all.
  Keeping the two separate keeps each answering one question.
- **Only bound the sites a concrete corruption report named** (the pending
  iterator and `chain_read_at`, the two with the clearest silent-wrong-answer
  stories). Rejected per finding 4: this is exactly the pattern that left
  ADR 0039 and this pass each one reader short the first time. The exhaustive
  grep-for-the-shape sweep costs little more and is the whole point.
- **Leave `BM25ChainCursor.seen` as `uint32`, relying on the per-iteration
  `(Size)` cast in the comparison to catch a 4 GB+ chain.** Rejected: the cast
  only helps the comparison; a chain whose cumulative content already exceeds
  4 GB would have wrapped the *stored* `seen` on a previous iteration, before
  any cast runs. The accumulator itself had to widen.

## Consequences

- A corrupt `pd_lower` anywhere below `SizeOfPageHeaderData` — the exact state
  `PageIsEmpty`/`PageIsNew` treat as legitimate for a *properly* zeroed page,
  but illegitimate for a page any reader here is about to trust as populated —
  now aborts the statement with `ERRCODE_INDEX_CORRUPTED` naming the bad
  `pd_lower`/`pd_upper` pair, instead of silently returning "empty page" (lost
  documents, dropped merge terms, missing postings on a ranked scan) or, at
  the KEYMAP site, defeating a check that believed it already had this covered.
- Four new `bm25_debug_*` probes (`page_content_bytes`, `chain_span_validate`,
  `pending_term_entry_span`, `field_rle_decode_bytes`) run the underlying
  validators/functions directly over caller-supplied values, extracted for
  this purpose where the check was previously inline, following ADR 0027's
  and ADR 0039's own reasoning that a regression suite cannot manufacture a
  corrupt page. All are pure functions of their arguments, open no relation,
  and are covered automatically by the install script's REVOKE loop
  ([0020](0020-debug-surface-privileges.md)) — verified directly against
  `pg_proc`/`has_function_privilege`, not assumed.
- `pos_cursor_varbyte`'s width bound is **not** independently probed: a
  `PosCursor` cannot be pointed at a plain byte buffer, only at a real POS
  chain, so there is no caller-supplied-bytes entry point the way
  `bm25_varbyte_decode` has one. Its loop body is textually identical to
  `bm25_varbyte_decode`'s and shares the same constant, so the shift-past-31
  arithmetic is already exercised (both directions) by
  `sql/69_decode_boundary.sql`'s varbyte probes; only the wiring into the
  page-crossing cursor is untested, and reaching that would require an actual
  corrupt on-disk page — the case this whole probe family declines to
  fabricate.
- `sql/79_page_content_bounds.sql` covers the negative paths for all four
  probed validators plus the wiring rationale for the fifth; the positive
  direction — that no guard rejects what the writer emits — is covered by the
  other 84 suites plus a short positive control in this one (a build, insert,
  seal, delete, re-insert, re-seal, and ranked query, exercising
  `bm25_pending_term_entry_span` on every seal and `chain_read_at` on every
  ranked lookup).
- A small per-call cost: two comparisons per page-content-length derivation,
  now centralized instead of duplicated ad hoc. No suite timing moved outside
  noise (build: clean, no warnings; full 85-suite `installcheck`: all pass).

## Addendum (2026-10-05, PRs #331-#350)

`bm25_page_content_bytes` now also checks `pd_special` (#302.D, PR #341). Core's page
verification accepts `pd_special == BLCKSZ`, which puts the 24-byte `BM25PageOpaque`
wholly past the block, and `BM25PageGetOpaque` is a bare page plus `pd_special`, so every
bm25 page must have `pd_special == BM25_PAGE_SPECIAL_OFF`. One compare covers every caller of
this function. The opaque readers that do not call it (`bm25_page_mark_deleted`, the orphan
sweep's reachability walk `mark_chain` and its full-extent pass) check through
`bm25_page_special_validate`, and `mark_chain` and the retired-list walkers now call this
function before their first opaque read (PR #342). `bm25_page_alloc` drops such a page
instead of raising, as it drops other unusable free-list entries. `BM25PageGetOpaque`
itself does not throw: it runs on registered copies inside WAL windows (ADR 0083).
