---
id: 0026
title: The pending append checks its size bound first, de-duplicates through a hash, and grows position arrays geometrically
date: 2026-07-29
status: Accepted
summary: bm25_pending_append_multi was O(ntok^2) in CPU and memory on a user-supplied value, all before the size check that would reject the document anyway; a cheap lower-bound pre-check, an HTAB de-dup and geometric position arrays make every step linear.
---

# 0026. The pending append checks its size bound first, de-duplicates through a hash, and grows position arrays geometrically

## Context

`bm25_pending_append_multi` did three things in the wrong order and at the wrong
complexity (review ref H7, issue #47):

1. De-duplicated each token with a **linear scan over every entry seen so far** —
   O(ntok · ndistinct).
2. Pre-sized **every** distinct entry's position array to the whole field's token
   count: `e_pos[j] = palloc(sizeof(uint32) * Max(ntok, 1))` — so D distinct terms
   among T tokens cost D·T·4 bytes.
3. Only **then** computed the exact byte `need` and rejected the document as too
   large for a pending page.

The comment justifying (1) — "a linear per-field unique pass is fine (docs are
short relative to the corpus)" — states a precondition on a value the inserting
user supplies, and nothing enforced it. The analyzer imposes no token-count cap.

The report's repro: an `INSERT` of a 200 KB text of mostly distinct words yields
~28,000 tokens, so ~4×10⁸ comparisons and ~3.1 GB of allocation — for a document
that was always going to be rejected. Available to any role with `INSERT`.

Measured here at a safe size (8000 distinct tokens, steady state, PG 18.3):
**90 ms before, 7.4 ms after.** The 28,000-token case is now rejected in ~12 ms
before any per-token work.

### A second, unreported defect

`e_tf` is `uint16`. A term occurring exactly 65536 times in one field wrapped the
counter to 0 during de-dup, which produced a zero-length position blob, so a small
`need`, so the document was **accepted**. Reproduced on the pre-fix build:

```
INSERT                        -> success
... WHERE body @@@ 'alpha'    -> 1     (matches while pending)
SELECT bm25_seal(...)
... WHERE body @@@ 'alpha'    -> 0     (silently gone, permanently)
```

A row present in the table and unfindable through its index, with the loss landing
at seal time rather than at INSERT. This was not in the report.

## Decision

Three changes, each closing a distinct pathology, none redundant with the others.

**A size pre-check before any per-token work.** The exact `need` requires the
de-dup pass, but a valid lower bound does not: every token contributes at least
one varbyte byte to exactly one entry's position blob, the total number of
positions across all entries is exactly `total_tok`, and `Σ MAXALIGN(x_j) ≥ Σ x_j`.
So `need ≥ MAXALIGN(header) + total_tok`, and a document exceeding that can never
fit however its tokens de-duplicate. This bounds everything downstream by the page
budget the document has to fit in anyway (~8 KB, a few thousand tokens) — and
because that is far below 65535, it also makes the `e_tf` wrap unreachable.

**An HTAB keyed on `(field_id, term bytes)`** replaces the linear scan. Keyed by
content with no length cap, holding a pointer rather than an inline copy, since
the bytes live in the caller's token arrays which outlive the table. Same shape as
`bm25_seg_read.c`'s wildcard dedup set, for the same reason: a fixed inline key
buffer would impose a cap, and truncating it would false-merge two distinct long
terms — here that would merge their tf and positions.

**Geometric growth of each entry's position array**, starting at 8 slots, so the
total across all entries is O(total_tok) rather than O(ndistinct · ntok).

## Alternatives considered

- **Only the pre-check.** Bounds `total_tok` to ~8100, but an all-distinct
  document still costs ~33M comparisons (~100 ms) and ~256 MB before rejection.
  A cheap repeatable CPU sink.
- **Only the hash and geometric arrays, no pre-check.** Makes everything linear,
  but linear in an unbounded token count, and leaves the `e_tf` wrap reachable.
- **An incremental running `need` with early bail-out** instead of the hash —
  bounds `ndistinct` to ~510 and so the linear scan to ~4M comparisons. Rejected
  as redundant once the hash makes de-dup O(1) per token regardless of
  `ndistinct`; it would only fail marginally earlier.
- **Keeping the linear scan below a token-count threshold** to avoid HTAB setup on
  short documents. Two code paths and a tuning constant to save ~1–2 µs on an
  INSERT that already costs tens of µs in WAL and buffer locks.
- **Widening `e_tf` to `uint32`** to fix the wrap directly. The on-disk
  `BM25PendingTermEntry.tf` is `uint16`, so this would be a format change to
  support documents that cannot fit a pending page regardless.
- **`CHECK_FOR_INTERRUPTS` in the token loop** (the report suggests it). With the
  pre-check the loop is bounded to a few thousand iterations of O(1) work —
  microseconds. A check there is noise; [0024](0024-scan-interrupt-checks.md)
  covers the loops that actually run long.

## Consequences

- An oversized `INSERT` is refused in milliseconds instead of after gigabytes, and
  the refusal happens before the work rather than after it.
- The 65536-occurrence silent index loss is gone: such a document is now refused
  loudly, at INSERT, like any other oversized one.
- **The accept/reject boundary moves slightly.** The pre-check is a lower bound, so
  every document it rejects would also have been rejected by the exact check —
  except the wrapped-`e_tf` case, which used to be wrongly accepted. No document
  that previously stored correctly is now refused.
- This does not address the underlying limitation that a pending page caps an
  INSERT at roughly 500 distinct (term, field) entries — that is issue #57 (H18)
  and needs a multi-page document representation, not a faster de-dup.
- `sql/68_pending_append_limits` pins the `e_tf` case (genuinely red/green), the
  size guard, that ordinary documents still insert, and — because the de-dup
  algorithm changed under them — tf aggregation, per-field keying and position
  fidelity, both in the pending list and after a seal.
- The performance win itself is not regression-tested: an over-large document ends
  in the same ERROR before and after, so only wall clock separates them, and that
  is not assertable in `pg_regress`. The measured numbers are recorded above.
