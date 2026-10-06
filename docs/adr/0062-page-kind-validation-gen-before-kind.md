---
id: 0062
title: Readers validate the page kind, and the gen check runs before the kind check
date: 2026-08-18
status: Accepted
summary: Every segment and pending reader now tests the BM25_PAGE_* kind flag that bm25_page_init has always stamped and nothing ever read; the gen check runs FIRST so a benign reclaim race stays a retryable serialization failure instead of becoming a non-retryable corruption error.
---

# 0062. Readers validate the page kind, and the gen check runs before the kind check

## Context

`bm25_page_init(pg, kind)` stamps a `BM25_PAGE_*` flag on every page, and
`BM25_PAGE_ALL_KNOWN` exists so that bit is meaningful. No reader on the segment
or pending paths ever tested it. A stray or corrupt `nextblk`, or a recycled
page, was decoded as whatever the walker expected to find.

`seg_gen` did not close this. Every page of one segment — DICT, POST, NORMS,
LIVE, DOCMAP, KEYMAP, POS and the header — carries the *same* gen, so a corrupt
`dict_root` pointing at that segment's NORMS chain passed gen validation and was
decoded as `BM25DictEntry` records. Gen answers "which segment", never "which
chain inside it".

Three populations were unguarded: the pending iterator (shared by eleven
walkers), `bm25_pending_truncate` (the only walker in the extension that
*destroys* pages, and which had nothing but a `blk < nblocks` extent bound), and
every `bm25_seg_page_validate` call site.

## Decision

`bm25_seg_page_validate_kind(page, expected_gen, want_kind)`, with the existing
two-argument form retained as a `want_kind == 0` wrapper, and the expected kind
passed at every call site. `bm25_pending_iter_begin` gains a `BM25_PAGE_PENDING`
check; `bm25_pending_truncate` gains kind, unknown-kind and cycle guards.

Three sub-decisions carry the weight:

**The gen check runs BEFORE the kind check.** A page reclaimed and re-initialised
as another kind fails *both*. `bm25_scan_build_ranking` catches only
`ERRCODE_T_R_SERIALIZATION_FAILURE` in its three-attempt retry, and
`t/005_replica_reuse` asserts a contract of "identical ranking OR a clean
gen-validation abort". Testing kind first would report that benign
reclaim-under-a-standby-scan race as non-retryable `ERRCODE_INDEX_CORRUPTED`,
break the retry, and fail that contract. With gen first, the kind check is
reachable only when the gen MATCHES — i.e. the wrong chain inside the right
segment, which is genuine corruption — or on the `expected_gen == 0` paths, each
of which is serialized by the metapage singleton or by VACUUM.

**The test is `(flags & want_kind) != 0`, never equality.**
`bm25_page_mark_deleted` ORs `BM25_PAGE_DELETED` onto the live kind rather than
replacing it, so a tombstoned-but-still-readable page must still pass. Equality
would have rejected exactly the pages the retired-list and truncate paths
legitimately re-walk.

**The truncate cycle guard is a visited COUNT, not a `reachable[]` bitmap.** Two
independent reasons. The kind check does not double as a cycle guard, precisely
because `mark_deleted` ORs and leaves `BM25_PAGE_PENDING` set — a page revisited
on the second lap still passes it. And this runs on `aminsert`, once per seal, so
a `palloc0(nblocks)` would be a ~13 MB allocation on a 100 GB index to guard a
~512-page walk. All visited blocks are `< nblocks`, so `visited >= nblocks`
proves a revisit by pigeonhole, and a healthy chain has at most `nblocks - 1`
pages.

## Alternatives considered

- **A dedicated `BM25_PAGE_SEGHDR` bit.** The segment header shares
  `BM25_PAGE_SEGCAT` with catalog pages. Rejected: a new flag needs a
  format-version and upgrade story, and the existing gen check already separates
  them — the header is stamped `seg_gen = gen` while catalog pages keep 0, and
  gens are monotonic from 1. One site (`bm25_livedocs_clear`'s own re-read)
  genuinely cannot distinguish the pair; it is VACUUM-serialized and documented
  in place, and every *other* kind is still excluded there.
- **A `LIVE|DOCMAP|NORMS` bitmask for `chain_read_at`**, which is genuinely
  multi-kind. Rejected in favour of threading the caller's single expected kind:
  a mask accepts two kinds the caller knows it does not want.
- **Kind check before gen** — see above; it breaks a documented retry contract.

## Consequences

- A corrupt or stray `nextblk` landing on a live page of another kind now raises
  `ERRCODE_INDEX_CORRUPTED` instead of being decoded as the expected record type.
  For `bm25_pending_truncate` that is the difference between erroring and
  *freeing* a live DICT/POST page — committed rows becoming unfindable until
  REINDEX.
- An unknown-kind page (a newer binary's chain, additive per ADR 0009) is LEAKED
  rather than freed or reported as corruption, matching the orphan sweep.
- This must not land before the pending-recycle horizon (ADR 0019's addendum):
  without it these checks convert that race from silent garbage into a loud
  corruption error on a condition that is not corruption. It did not.
- On-disk compatibility is a non-issue: `bm25_page_init(np, w->kind)` was
  introduced in the same commit as `chain_open`, so no format version ever wrote
  unstamped chain pages.
- Residual, noted rather than fixed: `bm25_page_content_bytes` bounds `pd_lower`
  and `pd_upper` but not `pd_special`, so a corrupt page with plausible
  `pd_lower` and garbage `pd_special` still reaches `PageGetSpecialPointer`. This
  is not new exposure — every pre-existing validate site shares the shape — but
  it is the class ADR 0027 and issue #141 exist for, and bounding `pd_special`
  once inside `bm25_page_content_bytes` would close it everywhere.

## Addendum (2026-10-04)

The fresh-eyes review of this date reproduced, on a primary/standby pair with hot_standby_feedback off, silent missing rows from standby `@@@` scans (about 5,200 of 20,000) when the primary sealed, advanced its horizon and reused pending pages mid-query. Pending pages carry seg_gen 0, so the gen-first check never fires for them and the recycle horizon is primary-only. Tracked in #291.

## Addendum (2026-10-05, PRs #331-#350)

- **A role clause after gen and kind** (#302.A, #303.C, #276; PR #341). Catalog pages and
  segment header pages share the SEGCAT bit, so the kind check alone let a catalog walk or the
  catalog appender take a header page for a catalog page, and a header read with no expected
  gen take a catalog page for a header. `bm25_segcat_page_validate` adds the role, with
  `seg_gen` as the cheap invariant: catalog pages carry 0 and headers their segment's gen,
  which is never 0. Order: content bounds, kind, role. The header readers keep this record's
  order (`bm25_seg_page_validate` for the gen first, so a reused page stays 40001, then kind
  and role). The `BM25_SEGCAT_ROLE_HEADER` clause is a structural backstop no SQL path
  reaches today.
- **Gens are never 0 and never repeat, now enforced.** The catalog copies refuse an entry with
  gen 0 (the skip sentinel, which switched off reuse detection for the whole segment).
  `bm25_next_gen_check` runs at both draw sites (a segment build and a pending chain start),
  before either opens its WAL window, and refuses the draw that would wrap `next_gen` to 0
  (`ERRCODE_PROGRAM_LIMIT_EXCEEDED`, REINDEX resets it) and a counter of 0 (corruption)
  (#313 META-08, PR #345). This record's gen-first argument and the gen arm of ADR 0120 both
  rest on that.
- A gen mismatch from a sealed-chain walk now goes to `bm25_seg_gen_mismatch`, which raises
  XX002 when the walk's own segment is still in the live catalog (ADR 0120). The retryable
  40001 stays for the race this record protects.
