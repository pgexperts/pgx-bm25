---
id: 0044
title: bm25_accum_term_postings returns reused scratch with a caller-visible lifetime contract
date: 2026-08-05
status: Accepted
summary: The three parallel posting arrays are a single grow-only per-accumulator buffer valid only until the next call, replacing per-call allocations that were retained for the whole build.
---

# 0044. bm25_accum_term_postings returns reused scratch with a caller-visible lifetime contract

## Context

`bm25_accum_term_postings` splits the accumulator's interleaved `AccumPosting`
array into three parallel `uint32` arrays the segment builder needs. It allocated
three fresh arrays per call into `a->cxt` — the accumulator's own context, which
lives until `bm25_accum_free` at the very end of the build — and no caller ever
freed them.

The function's comment claimed the builder calls it "once per term", and that the
arrays are "freed with it". Both were misleading. The builder calls it once per
term in the POST pass and again per term in the POS pass whenever any field
stores positions, which is the default. And "freed with it" means freed at the
end of the entire build, so every term's scratch is retained simultaneously: 24
bytes per posting of pure scratch, on top of a 32-byte-per-posting `AccumPosting`
array, on an accumulator already unbounded by `maintenance_work_mem`. On a
300-million-posting build that is ~7.2 GB of scratch that is never reused and
never released.

A closely related leak sat three lines away in the same loop: `doclens` in
`bm25_segment_build_orphans` was pallocated per term inside the POST loop and
never freed, landing in the caller's context — `bm25_segment_build_and_commit` or
`..._swap`, neither of which creates a context of its own — for the whole build.

## Decision

Return pointers into a **single grow-only per-accumulator buffer**. `BM25Accum`
gains `post_docids` / `post_tfs` / `post_fields` / `post_cap`, allocated in
`a->cxt` and `repalloc`'d only when a term needs more than the current capacity,
with a `Max(npost, 1)` floor so the arrays stay non-NULL for a zero-posting term
(`encode_block` documents a dependence on that).

The contract this creates is stated as a titled block on the function and
mirrored at both builder call sites and on the declaration in `bm25.h`:

> The returned pointers are valid only until the next `bm25_accum_term_postings`
> call on the same accumulator.

`doclens` is hoisted to the same grow-on-demand treatment, matching the idiom
`posbuf`/`poscap` already uses a few lines below it in the same function.

No explicit `pfree` in `bm25_accum_free`: the buffers live in `a->cxt`, which that
function deletes wholesale, exactly as it already does for `terms`, the postings
arrays, `docs`, and the hashtable.

## Alternatives considered

- **`pfree` the three arrays on the caller side (drop `const`)** — needs the
  `const` removed from the declaration and both definitions, four call sites
  edited, and twelve `pfree`s added, with a fresh foot-gun for every future
  caller. Strictly more churn than the reused buffer for the same result.
- **Caller-owned scratch passed in** — pushes sizing knowledge (the maximum
  `npost` across terms) into four callers, including two debug SRFs, for no gain.
- **A caller-supplied MemoryContext reset per term** — the POST loop runs with the
  chain writer's tail buffer content-locked across iterations; adding per-term
  context churn inside that region is the wrong direction.

## Consequences

- Scratch is bounded by the single largest term's posting count instead of the
  sum over all terms — on the 300M-posting example, gigabytes down to megabytes.
- **The aliasing hazard is now the thing to protect.** Verified at adoption that
  no caller holds a returned pointer across a subsequent call, and that the two
  builder passes are strictly sequential (POST drains all terms, then POS). The
  failure mode if that is ever violated is the nasty kind: it does not fault, it
  silently returns the *next* term's postings, which single-field tests pass
  straight through. This is why the contract is stated in three places rather
  than one, and why the header declaration carries it too — a caller reading only
  `bm25.h` previously saw nothing warning them off.
- Misleading local names (`f` holding tfs, `g` holding field ids, in a file where
  `f` is the field-index loop variable everywhere else) disappear rather than
  being renamed: the data now lives in self-describing struct members.
- A `repalloc` that ereports mid-grow leaves `post_cap` at its old, smaller value,
  which stays an accurate lower bound; a retry re-grows all three. Output pointers
  are assigned only after the copy loop, so no caller observes the window.

## Addendum (2026-08-23)

"An accumulator already unbounded by `maintenance_work_mem`" is no longer true; see
`docs/adr/0084`. The scratch reuse this record decided is not made redundant by that
bound -- it is made more load-bearing. The budget is measured as
`MemoryContextMemAllocated` over the accumulator's own context, which is exactly
where the per-call scratch used to accumulate, so the pre-0044 shape would have been
charged against the budget and would have made every chunk smaller for no benefit.
