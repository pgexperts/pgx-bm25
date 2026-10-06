---
id: 0125
title: The accumulator's term map keys on (pointer, length) over the whole term, with no collision fallback
date: 2026-10-05
status: Accepted
supersedes: 0076
summary: AccumHashEntry keys on a pointer to the AccumTerm's own copy of the term plus its length, hashed with hash_bytes and compared byte for byte (HASH_FUNCTION | HASH_COMPARE), so two distinct terms never share an entry at any length and the linear collision fallback is deleted; entries shrink from 260 bytes to 24, and a sorted accumulator refuses further term inserts.
---

# 0125. The accumulator's term map keys on (pointer, length) over the whole term, with no collision fallback

## Context

ADR 0076 keyed the accumulator's term map on a fixed 256-byte key: a term of at most 254
bytes verbatim, a longer one as a tagged 32-bit `hash_bytes` of its full bytes, with a
linear `memcmp` scan of every term as the fallback when two long terms shared a hash. Its
premise was that nobody can aim a 32-bit hash collision. It named a pointer-and-length
key as "strictly better ... and the right destination", and deferred it on risk.

The 2026-10-04 review found the premise false (#305 PEND-02). A birthday search over
some tens of thousands of candidate words finds colliding long-term pairs in about a second of SQL. The
second word of a pair took over the shared entry, and every later occurrence of the first
word scanned `terms[]` from the start: O(occurrences x nterms) inside a build, a seal
(which holds the seal singleton, so INSERTs queue behind it) or a merge. CREATE INDEX on a
small crafted corpus went from 289 ms to 1,313 ms. Separately, every entry carried the
256-byte key, which was most of a short term's footprint (PEND-06).

## Decision

Decided by the user as D19; landed in PR #338 (#305).

- The map keys on `(ptr, len)` over the whole term, hashed with `hash_bytes` and compared
  byte for byte, through dynahash's `HASH_FUNCTION | HASH_COMPARE`. Two distinct terms never
  share an entry at any length, a lookup is one probe, and the linear fallback is deleted.
- The pointer stored in an entry is the `AccumTerm`'s own palloc'd copy (alive as long as
  the map), re-pointed right after `HASH_ENTER`, never the caller's token buffer.
  `bm25_accum_sort`'s qsort moves `AccumTerm` structs but not the bytes they point at, so
  the pointers survive the sort.
- The sort leaves every entry's `termidx` stale. The old fallback had silently absorbed
  that; now `a->frozen` makes a sorted accumulator refuse further term inserts instead of
  misfiling them.
- `AccumHashEntry` shrinks from 260 bytes to 24 on a 64-bit build, and dynahash hashes
  `termlen` bytes per lookup instead of 256.

## Alternatives considered

- **Keep ADR 0076's key** and accept the fallback. Its premise does not hold.
- **A keyed (seeded) hash with the fixed-width key.** Makes collisions unaimable again but
  keeps the fallback, the 256-byte entries and the length split.
- **Raise the key width above `BM25_MAX_TERM_BYTES`.** A 2 KB key per entry, rejected by ADR
  0076 for the same reason.

## Consequences

- Lookups are exact at every length; the defect class (an aimable linear fallback) is gone.
- Measured (A/B, PR #338): build 0.74x, seal 0.67x, merge 0.66x of the old time; plain
  INSERT 1.01x. The smaller entries are the gain.
- **Residuals.** `hash_bytes` is unkeyed, so many terms with equal low hash bits lengthen one
  bucket chain; each probe there is a hash compare first and a `memcmp` only on equal
  hashes, the same exposure as every core dynahash keyed on user text.
  `BM25_ACCUM_SLACK_FACTOR` (2x) was not recalibrated: dynahash allocates elements in
  batches sized from the entry (58 x 280 B before, 51 x 40 B now), so on `sql/105`'s
  term-heavy segments the factor now over-estimates by roughly 2x, the safe direction (the
  merge trim refuses some merges that would have fit).
- `sql/147_accum_full_term_key` builds over a hard-coded colliding pair through build, seal
  and merge replay; `sql/105`'s budget-calibrated merge sections moved to 96 kB and two
  pinned counts became relational.
- ADR 0076's other content stands where it does not depend on the key: the 254/255
  boundary is moot, and its "drain holds the page lock across the flush" statement was
  already stale (ADR 0076 addendum).
