---
id: 0017
title: The seal publish prepends a fresh segment-catalog page when the root is full
date: 2026-07-29
status: Accepted
summary: bm25_segment_build_and_commit checks the root SEGCAT page's remaining space and, when the new entry does not fit, prepends a fresh page and flips segcat_root inside the same publish record.
---

# 0017. The seal publish prepends a fresh segment-catalog page when the root is full

## Context

Phase 2 of `bm25_segment_build_and_commit` appended the new `BM25SegCatEntry` at
the root SEGCAT page's `pd_lower` with **no room check**, never followed
`nextblk`, and never chained a page.

A SEGCAT page holds 203 entries: 8144 usable content bytes
(`BLCKSZ` 8192 − 24 header − `MAXALIGN(sizeof(BM25PageOpaque))` 24) divided by
`MAXALIGN(sizeof(BM25SegCatEntry))` = 40. Entry 204 was written straight through
`BM25PageOpaque` — `nextblk`, `flags`, `retire_xid`, `seg_gen` — and past the end
of the 8 KB buffer, leaving `pd_lower > pd_upper` for `GenericXLogFinish` to
compute a page delta from and WAL-log (review ref C4, issue #35).

Nothing bounds the segment count between VACUUMs. `bm25_merge_maybe` has exactly
two call sites — `amvacuumcleanup` and the manual `bm25_merge()` SQL — and
`BM25_TARGET_SEGMENT_COUNT` is a merge-*selection* target, not a seal-time gate.
So the state is reachable from ordinary SQL: repeated `bm25_seal()` (which has no
`REVOKE` and is executable by anyone), or the opportunistic `aminsert` seal on an
insert-heavy table whose autovacuum is lagging.

The reader has always handled chains — `bm25_seg_read.c` follows `nextblk` until
it has `nsegs` entries, and the merge swap already builds multi-page chains via
`bm25_segcat_build_orphan_chain`, which computes exactly this 203-entry bound.
The seal was the one writer that ignored it.

## Decision

We will test the root page's remaining space before appending, and when the entry
does not fit, **prepend** a fresh page: the new page becomes the root, its
`nextblk` points at the old root, and `meta->segcat_root` flips — all inside the
single existing publish record.

## Alternatives considered

- **Fail loud** (`ERRCODE_PROGRAM_LIMIT_EXCEEDED`) at 203 segments — the report's
  minimum bar. Rejected as the primary fix: it converts a corruption into an
  outage on a perfectly ordinary workload (insert-heavy table, lagging
  autovacuum), and the reader already supports the chain that makes it
  unnecessary.
- **Walk to the tail and append there, chaining a new tail page when needed** —
  the intuitive "append" shape, and how the entries would stay in seal order. It
  costs a chain walk under lock and a 3-buffer record (tail + new page +
  metapage) instead of 2. Catalog order carries no meaning — readers copy entries
  in chain order until they have `nsegs` of them, and the merge already rebuilds
  the chain wholesale — so the extra work buys nothing.
- **Cap segment count at seal time and force a merge** — makes an unbounded
  INSERT path do unbounded maintenance work synchronously, and merges cannot run
  under the same singleton the seal already holds without restructuring the
  lifecycle.

## Consequences

- Catalog entries are no longer in seal order once the chain grows past one page:
  a prepended page's entries precede older ones. Nothing depends on the order
  today, and this record is the place to look if something ever does.
- The publish record still registers 2 buffers, well inside the 4-buffer Generic
  WAL cap, so the "single-record seal" and "two-phase install" invariants are
  untouched.
- The `bm25_reclaim_orphans` sweep already marks the catalog with
  `mark_chain(index, meta.segcat_root, …)`, which follows `nextblk`, so a
  multi-page seal-built chain is reachable to the sweep exactly as a
  merge-built one is.
- `sql/62_segcat_chain` covers it with 210 single-segment seals — past the
  203-entry bound with margin on both sides — then reads back the catalog and
  probes documents from before, at and after the boundary. Verified as a real
  guard: on the pre-fix code the loop crashes the backend outright.
