---
id: 0126
title: Corruption tests forge pages through one raw, owner-gated poke lever plus a struct layout table
date: 2026-10-05
status: Accepted
summary: bm25_debug_poke_page(index, blkno, off, bytes) writes arbitrary bytes into any existing block under the page's EXCLUSIVE lock and a flushed Generic WAL full-page image, refusing only results PostgreSQL cannot carry or that would lose bytes, so every accepted poke can be undone; bm25_debug_layout() reports offsetof/sizeof for the on-disk structs so suites never hard-code offsets.
---

# 0126. Corruption tests forge pages through one raw, owner-gated poke lever plus a struct layout table

## Context

The corrupt-pointer findings of #302 and #303, and #309's complaint that the corruption
tests drove validator predicates rather than call sites (REGR #134/#137/#141, CI-04),
needed about twenty values forged that no lever could write: the five metapage pointers,
retired-descriptor roots and gen, `pd_special`, a catalog entry's gen, the field-config
`field_id`, a pending record's `field_id` and positions, DICT and POS `nextblk`, a block's
impact `max_tf`, and segment-header roots. The existing `bm25_debug_stamp_*` levers each
forge one named field after a validated walk to their target.

Writing relation files from a TAP test was the other route. PG18's initdb enables data
checksums by default, so a byte patch also needs `--no-data-checksums` or a checksum
recompute, and it runs only in the TAP tier, invisible to the gcov-measured SQL suites.

## Decision

Decided by the user as D4; landed in PR #332 (#302, #303, #309).

**`bm25_debug_poke_page(index regclass, blkno bigint, off int, bytes bytea) RETURNS bytea`**
writes `bytes` at byte offset `off` of any existing block, the metapage included, and
returns the bytes it replaced. It validates nothing about bm25 kind, structure or content,
because forging bad structure is its job. Its contract is about the page PostgreSQL holds:

- the range must lie in the block and start at or after `pd_flags` (`pd_lsn` and
  `pd_checksum` are rewritten anyway);
- the result's header must pass the header half of core's page verification, so an outcome
  never depends on whether the buffer is evicted, and `pd_lower > pd_upper` (which would
  PANIC inside `GenericXLogFinish`) cannot arise;
- `pd_lower` must not point inside the page header (the hole would start there and
  `GenericXLogFinish` would zero `pd_upper` and `pd_special`);
- no poked byte may land in the result's hole, and the hole must already be all zero:
  Generic WAL zeroes it, so poked bytes there would vanish and a lowered `pd_lower` would
  destroy live bytes.

The invariant those refusals buy: the buffer equals the validated image, and every
accepted poke can be undone by poking the returned bytes back (all but `pd_lsn` and
`pd_checksum` are restored). `pd_special` is left to core's own check deliberately, so
values that leave less than a `BM25PageOpaque` before `BLCKSZ` are accepted for 302.D's
tests, and no other suite may poke it.

It writes under the target's EXCLUSIVE content lock as a Generic WAL FULL_IMAGE record, so
the forged page reaches standbys, survives crash recovery and is checksum-clean, and it
calls `XLogFlush` before returning: the caller's transaction usually has no xid, so its
commit would not flush, and a poke followed by an immediate stop recovered the unpoked
page. Every check runs before the WAL window. It opens through `bm25_index_open_owned`
(ownership, the recovery refusal of ADR 0029's addendum, AM identity) and is REVOKEd from
PUBLIC by the install script's allowlist loop.

**`bm25_debug_layout() RETURNS TABLE(struct, field, off, size)`** is built from `offsetof`
and `sizeof` for every member of the on-disk structs, plus page anchors and the packed
impact encoding (about 140 rows). It doubles as the SQL probe for the struct pins of #313
SEGREAD-08.

Both live in `bm25_page_lever.c`, apart from `bm25_seg_debug.c`, because neither uses the
segment reader that file is organized around.

## Alternatives considered

- **One semantic stamp lever per field** (about ten more). Each needs a validated walk to
  its target and its own tests, for no gain over a raw writer aimed by the layout table.
- **TAP byte-patching of relation files.** Needs checksums disabled or recomputed, runs only
  in the TAP tier, and is invisible to coverage.
- **pageinspect.** Read-only: `get_raw_page` cannot write.

## Consequences

- The corruption suites (`sql/136`-`sql/140`, `sql/151`) forge any field by name and assert
  the call site's own message, A/B'd against the build without the check.
- **Limits.** It cannot leave an all-zero, never-initialized page (the record stamps an LSN
  and `pd_upper == 0` is refused); a test that needs a link to one produces it another way
  (`t/035` uses a crash mid-extend). It holds no metapage lock or singleton, so it is for a
  quiescent index only. It is as dangerous as every other owner-gated lever that can
  already corrupt an index, and gated the same way.
- Lesson recorded during the grind: tests written against an earlier contract of the lever
  broke when the hole refusal was added; write tests against the lever's final contract or
  re-run them after it changes.
