---
id: 0094
title: The segment catalog entry's two strides are pinned equal, not unified
date: 2026-09-21
status: Accepted
summary: BM25SegCatEntry is addressed both at the MAXALIGN on-page stride and as a plain C array, so a StaticAssertDecl now pins sizeof == MAXALIGN(sizeof) -- the relationship both depend on -- instead of rewriting either family of sites to a single stride.
---

# 0094. The segment catalog entry's two strides are pinned equal, not unified

## Context

A `BM25_PAGE_SEGCAT` page holds packed `BM25SegCatEntry` records, and the tree
addresses them two ways:

- **MAXALIGN stride** -- `bm25_segcat_entries_per_page` divides by
  `MAXALIGN(sizeof)`; the seal's in-window append and
  `bm25_segcat_build_orphan_chain` advance `pd_lower` by it; `bm25_scan_snapshot`
  and `bm25_segcat_read` step their cursor by it.
- **Array stride (`sizeof`)** -- the orphan-chain writer lays each page's batch
  with ONE bulk `memcpy`; `bm25_segcat_find_entry` and `bm25_segcat_locate_entry`
  read `ents[i]`; `bm25_livedocs_clear` indexes `[catidx]`.

Issue #67 (finding at `bm25_seg_build.c`, 2026-07-28) observed that the two agree
only by coincidence of size, and that the orphan-chain writer's own comment
claimed each entry was MAXALIGN-strided when its copy was not. The existing
`StaticAssertDecl(sizeof(BM25SegCatEntry) == 40)` (#144) pins the NUMBER, which a
deliberate format change updates along with the struct, so it cannot guard the
relationship.

Re-deriving the hazard corrected the finding's own example. It said adding one
`uint32` makes the struct 44 bytes on a 48-byte stride. It does not: the `uint64`
members give the struct the alignment `MAXIMUM_ALIGNOF` measures (8 on 64-bit
targets; 4 on i386, where `int64` and `double` are both 4-aligned inside a struct)
and C pads `sizeof` to it, so adding a member yields 48 on a 64-bit target (44 on
i386, where `MAXALIGN(44)` is 44), and the relationship survives either way. What
breaks it is NARROWING -- the three `uint64` counters becoming `uint32`, a
plausible catalog-space saving -- which on a 64-bit target leaves a 4-byte-aligned
28-byte struct on a 32-byte page stride, so every array-indexed site reads entry
*k* from 4*k* bytes short of it.

## Decision

Keep both families of sites as they are, and pin the relationship they share:

```c
StaticAssertDecl(sizeof(BM25SegCatEntry) == MAXALIGN(sizeof(BM25SegCatEntry)), ...);
```

next to the existing size pin in `bm25_format.h`, with a comment naming every site
on each side. The orphan-chain writer's and the seal writer's comments now state
the real invariant rather than a stride neither of them has.

Negative control: narrowing the three counters to `uint32` AND updating the size
pin to 28 compiles past the size pin and fails on this assertion alone.

## Alternatives considered

- **Unify on the MAXALIGN stride** -- convert the bulk copy to a per-entry loop
  and the three array-indexed readers to byte-pointer arithmetic. Correct under any
  future layout, but it rewrites four working sites for a difference that cannot
  arise while the struct holds an 8-byte member, and the result is still only as
  good as the next site someone adds as `ents[i]`. The assertion catches that new
  site too; a one-time rewrite does not.
- **Unify on the array stride** -- divide and advance by plain `sizeof`
  everywhere. Same site churn, and it silently changes the on-page stride of any
  future non-MAXALIGN-clean layout, which is a format question that deserves a
  compile error rather than a quiet answer.
- **Put both pins in one `StaticAssertDecl`**, as issue #155 (SEGREAD-12)
  sketched. Two asserts give two messages, and the one that fires names the
  property that broke -- the size, or the stride relationship.
- **Rely on the size pin.** It pins the number, not the relationship, and a
  format change moves the number deliberately.

## Consequences

- A struct change that breaks the relationship fails to compile, and the message
  names the two fixes: keep the struct MAXALIGN-clean, or convert the
  array-indexed sites to the byte stride.
- `BM25RetiredEntry` needs no such pin. Every one of its readers and writers
  strides it by plain `sizeof` (the `bm25_format.h` comment claiming otherwise was
  corrected in the same change), so its layout is consistent under any size.

## Addendum (2026-10-05, PRs #331-#350)

The remaining unpinned on-disk structs were pinned by `StaticAssertDecl` in PR #345 (#313
SEGREAD-08): `BM25PageOpaque`, `BM25KeymapHeader`, `BM25DictEntry`, `BM25BlockHeader` and
`BM25PendingTermEntry`. Those are size pins (and offsets where a hole matters), not
stride-relationship pins like this record's, because each is addressed one way only. ADR
0009's addendum of this date lists them.
