---
id: 0120
title: Sealed segment chains are walked through one validated reader, BM25SegWalk, with a cheap order invariant per chain family
date: 2026-10-05
status: Accepted
summary: Every sealed-chain walk (DICT, POST, POS, LIVE, DOCMAP, NORMS, KEYMAP) reads its pages through BM25SegWalk, which checks block 0, extent, a revisit test, a visit cap, content bytes, gen and kind; each family adds the cheap invariant that ends a cycle at its first revisit; a gen mismatch on a segment still in the live catalog is XX002, not the reclaim race's 40001.
---

# 0120. Sealed segment chains are walked through one validated reader, BM25SegWalk, with a cheap order invariant per chain family

## Context

Issue #303.A/B was the third issue in the chain-walk-bounds family (#225, #243 and #244,
then #293 and #294, then #303). ADR 0095 bounded the walkers by the extent, ADR 0111 gave
the POST and dense chains count and span rules and handed two residuals on (a POST cycle
through zero-block pages spun, and a single-document self-loop emitted duplicates; WAND
had no cross-block order check), and ADR 0110 built `BM25PendingWalk` for the pending
side. The segment side still had about eighteen walk sites each carrying its own subset of
the per-page checks, and they had drifted:

- a mid-chain link to block 0 read the metapage, failed the gen check and raised a
  retryable 40001 (the ranking build retried it three times);
- an in-extent cycle spun until cancelled in the DICT lookup, the merge feed's iterator,
  the wildcard expander, the debug dumps and the KEYMAP walk (a cycle of pages with no
  keys);
- a self-loop or a link back to a dense chain's root answered from the wrong page;
- a corrupt in-extent link into another segment's page failed the gen check and raised
  40001, which a client that retries 40001 retries forever;
- the debug dumps sampled the extent before reading the catalog, so a concurrently
  sealed segment's dictionary was dropped (SEGREAD-07).

The user decided (D2) on a design pass for a segment twin of `BM25PendingWalk` with
DICT/POST/WAND page-order invariants, with an independent design check before any code.
That check found four required changes (R1-R4, below), all adopted.

## Decision

**The walker** (`BM25SegWalk`, `bm25_seg_walk_init` / `bm25_seg_walk_read` /
`bm25_seg_walk_get` / `bm25_seg_walk_check`, implemented in `bm25_seg_chain.c`). Per page,
in order:

1. block 0 is `ERRCODE_INDEX_CORRUPTED` (the clause now lives in
   `bm25_seg_chain_extent_validate`, so every direct caller gets it);
2. extent: `blk >= nblocks` is XX002 (ADR 0095); a quiet walk (the debug dumps)
   re-samples through `bm25_blk_in_extent` and stops if still past it; a walk opened with
   `nblocks == 0` samples at its first read, which is how the dumps fix SEGREAD-07;
3. revisit: a link to the page just read, to the walk's first page, or to the chain's
   root is a cycle, because every walk moves forward along a simple list and nothing links
   to a root (a resumed cursor sets `root` so a link back to it counts);
4. visit cap: `visited >= nblocks`, the `bm25_pending_cycle_cap_validate` shape, a
   backstop that keeps "every segment walk is bounded" true independently of the family
   rules;
5. content bytes (`bm25_page_content_bytes`) before any opaque read;
6. gen, then kind (ADR 0062's order). A gen mismatch goes to `bm25_seg_gen_mismatch`.

`bm25_chain_page_get` is folded into `bm25_seg_walk_get`. The walker runs no
`CHECK_FOR_INTERRUPTS`; each caller has one at the top of its page loop with no buffer
held, as with `BM25PendingWalk`.

**Family invariants**, checked by the callers on values their decode already has:

- **DICT:** a page holds at least one entry (R4), and its first term sorts strictly after
  the previous page's last (`bm25_term_cmp`, now exported). A DICT cycle therefore stops at
  its first revisit, and an unsorted page boundary, which made a present term read as
  absent, is corruption. The merge feed and the three dumps also check order within a page
  and that a whole-chain walk ends at the header's `nterms`, so a merge never writes a
  short, doubled or unsorted dictionary.
- **POST** (`bm25_seg_scan_postings`): a page entered mid-run must yield at least one block
  (every POST page of a run starts with a whole block), and blocks ascend on
  `(docid, field)` lexicographically. An ADR 0113 straddle continues the same document at a
  higher field, so the rule is exact for both layouts, and it rejects the single-document
  self-loop the old `>=` docid rule admitted.
- **WAND:** the open-time sweep and `next_geq`'s peek hold block headers to ascending
  `last_docid`, equal only for a final straddle-only block of at most `BM25_MAX_FIELDS`
  postings. The decoder holds a block's first posting after the previous block's last:
  exactly `(docid, field)` after `next()`, by docid after a `next_geq` landing, whose
  predecessor is passed explicitly as the last bypassed header (R2: the landing header's
  own `last_docid` has overwritten the candidate by then). `bm25_seg_block_header_read_lead`
  refuses a page linked to itself.
- **KEYMAP:** every page walked past holds `bm25_keymap_full_span` (the root's span is
  measured after the header).
- **LIVE, DOCMAP, NORMS, POS:** the walker alone on top of ADR 0111's span and budget rules.

**The gen arm.** `bm25_seg_gen_mismatch` decides between corruption and the reclaim race:
retirement removes a segment's catalog entry before any of its pages can be reclaimed, WAL
replay keeps that order on a standby, and gens never repeat, so a live segment never owns a
page carrying another gen. If the walk's own segment is still in the live catalog the
mismatch is XX002; if it has left the catalog it is the race and stays 40001. The catalog is
read through `bm25_scan_snapshot`, which holds the metapage SHARE across its walk, never
through the unlocked `bm25_segcat_find_entry` walk ADR 0107 forbids (R1: on a standby that
walk could meet a reused catalog page and turn the legitimate 40001 into XX002). A backend
holding the seal/merge singleton needs no read, since its segments cannot retire. The
merge feed's DICT iterator and `bm25_livedocs_locate` now carry their segment's real gen,
and `bm25_livedocs_clear` checks the gen of the LIVE page it is about to write under its
EXCLUSIVE lock, before the WAL window (R3: locate hands that page on unread, so a link
into another segment's LIVE chain put the tombstone bit on that segment's document).

**The page-image fast path.** `chain_read_at` runs per scored posting and nearly every call
lands on the page its cursor holds as an image. A one-page lookup gives the walker nothing
to check, so that case skips the walker's bookkeeping and re-checks only the content bound,
gen and kind on the image; any other first page, and every later page, goes through the
walker (PR #346 commit ded857d).

Landed in PR #346 (#303, #313 SEGREAD-07).

## Alternatives considered

- **Per-site visit caps only** (the issue's proposal). A cap ends a spin only after up to
  `nblocks` reads, does nothing for duplicates or wrong-page answers, and leaves the
  walkers free to drift apart again, which is what D2 exists to stop.
- **A per-walk or per-lookup visited set** (as `mark_chain` keeps). O(nblocks) memory and a
  hash probe per page on per-posting walks; rejected by the hot-path rule of ADR 0119.
- **Brent's cycle detection.** O(1) state and catches every cycle, but only after up to
  mu + 2 lambda steps, so an offset-bounded walk can answer from the wrong page first. The
  family invariants end every cycle earlier; redundant.
- **Gen mismatch is XX002 whenever `!RecoveryInProgress()`.** Rests on the primary-horizon
  argument (`bm25_scan_build_ranking`'s header), which is not proven for every reader; the
  catalog recheck is exact.
- **Look the gen up with `bm25_segcat_find_entry`** (the design's first draft). Rejected by
  the design check (R1), above.
- **Without the gen arm** a cross-segment link stays a retryable 40001, which D1 does not
  allow.

## Consequences

- Corrupt links that raised a retryable 40001, spun until cancelled, answered from the
  wrong page, or wrote a tombstone into another segment now raise XX002.
- Measured cost (interleaved A/B in the regimes each check affects, PR #346): WAND top-k
  0.97-0.99x and exhaustive ranking 0.985x (both flat to faster, after the fast path above;
  before it, a frequent-term WAND top-k was about 6% slower); a DICT lookup of a late-sorting
  term about +2.8% (about 10 us per query term), a prefix wildcard about +2.5%, VACUUM about
  +1.9%. `bm25_wand_stats` counters were identical.
- The stamp lever keeps an extent-only walk, so `sql/113` can still aim it through a forged
  metapage link.
- **Residuals** (D1's second sentence; ADR 0119 lists them with the rest): 303.A(f)
  narrowed to a link that is not to the first, previous or root page; DICT within-page
  order unchecked by lookup and the wildcard expander (it would double the per-entry
  compares of an O(dict) scan); POST in-block disorder across a cycle of two or more pages
  (needs a per-posting compare); a KEYMAP root past the extent gets `ReadBuffer`'s XX001;
  the gen arm's not-live 40001 branch is untested (no pause point on the segment scan
  path) and its XX002 rests on "gens never repeat, retire before reclaim" by inspection;
  the WAND header-order rule has no test that fails before the fix (it needs header-only
  pruning); SEGREAD-07 has no race test without a new pause point (the fix is structural).
- Closes ADR 0111's two residuals (ADR 0111 addendum). The block-0 clause in the extent
  helper and the dumps' lazy re-sample are recorded on ADR 0095; the gen arm's catalog read
  on ADR 0107.
- Pinned by `sql/139_seg_walk` and the updated `sql/124`.
