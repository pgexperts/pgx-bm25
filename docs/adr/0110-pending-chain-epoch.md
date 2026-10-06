---
id: 0110
title: Pending pages carry a chain epoch, and a scan rejects a page re-initialized after its snapshot
date: 2026-10-05
status: Accepted
summary: A pending chain's epoch is drawn from meta.next_gen when the chain starts and stamped in seg_gen on every page; scans capture next_gen with pending_head and raise 40001 for a page whose epoch is nonzero and at or above it, through one validated walker that also bounds extent and cycles.
---

# 0110. Pending pages carry a chain epoch, and a scan rejects a page re-initialized after its snapshot

## Context

A scan captures `pending_head` once, in `bm25_scan_snapshot`, and walks the chain
afterwards, unlocked between pages, once for statistics and again per term. A seal
drains the chain and `bm25_pending_truncate` (or the orphan sweep) stamps the drained
pages `DELETED` with a real `retire_xid` (ADR 0019, 2026-08-16 addendum). The allocator
reuses a page once that horizon is removable, and the check runs on the primary.

On a hot standby with `hot_standby_feedback` off, nothing holds that horizon back for a
standby query, and Generic WAL redo carries no `snapshotConflictHorizon`, so no
recovery conflict is raised either. The primary can therefore seal, recycle and
re-initialize a pending page while a standby scan is between two pages of the old
chain. Pending pages carried `seg_gen = 0`, the "skip validation" value, so the only
per-page check was the page kind. A page re-initialized as a new pending page passed it,
the scan followed the new chain, and it silently returned too few rows (101 of 400 in
the test). A page reused as a segment page failed the kind check as XX002
(`ERRCODE_INDEX_CORRUPTED`) on a healthy index. Segment pages have no such hole: every
one carries its generation and `bm25_seg_page_validate_kind` checks it first and raises
`ERRCODE_T_R_SERIALIZATION_FAILURE`, which the ranked path's retry absorbs (issue #291).

The trigger needs only default settings and write traffic, because a seal also fires by
itself at `seal_threshold` on ordinary INSERTs. ADR 0063 had argued that page-granularity
standby exposure "remains a retryable serialization failure", but only for pages that
carry a generation; ADR 0083 had recorded that the horizon argument is void on a
standby without feedback and did not apply it to the pending chain.

The user decided on 2026-10-05 on the epoch scheme below, and that the pending
chain adopts the validated-walker contract of ADR 0111 inside the same change.

## Decision

**The epoch.** A chain's epoch is drawn from `meta.next_gen` when the chain starts, and
`next_gen` is bumped in the same append record that publishes the new `pending_head`.
Every pending page carries the epoch in `seg_gen`; a later page copies it from the locked
tail. The counter is shared with segment generations, so an epoch never collides with a
segment's. There is no new metapage field and no format change.

**The check.** `BM25ScanSnapshot` captures `next_gen` with `pending_head`. A scan-side
walker raises `ERRCODE_T_R_SERIALIZATION_FAILURE` for a page whose `seg_gen` is nonzero
and at or above the captured `next_gen` (`bm25_pending_epoch_validate`). The comparison
is `>=` and not equality because a scan also walks pages appended to its chain after the
capture, and those carry the chain's own, older epoch. The epoch check runs before the
kind check, for the reason `bm25_seg_page_validate_kind` gives: a page reused as another
kind fails both, and only the 40001 is the true answer.

**Epoch 0 passes.** A chain an older binary started carries `seg_gen = 0`. Treating 0 as
valid means mixed binaries lose the protection for that chain instead of raising a 40001
that no retry clears.

**One validated walker.** Every scan-side pending walker reads through
`bm25_pending_walk_read`, which applies in order: the extent bound, re-sampling
`nblocks` on a would-be violation because appenders keep extending while a scan walks;
the visited-count cycle cap; the epoch; the page kind. The scan walkers had only the
kind gate; a link out of the relation reached `ReadBuffer`'s own short-read error, and
an in-extent cycle spun until cancelled. This is the pending-chain half of the contract
in ADR 0111.

**The error is a bare 40001 on `@@@`.** The ranked path's existing bounded retry absorbs
it. `@@@` reports it, since the flat-OR path calls its walkers outside any retry wrapper.
A retry that does not run inside `BeginInternalSubTransaction` is #307, which owns the
subtransaction FATAL and the `RecoveryInProgress` guards. The README recommends
`hot_standby_feedback = on` on the standby as a mitigation, not as the fix. THEORY.md no
longer says the standby edge is never wrong results.

**Test lever.** `bm25_native.debug_pause` gains `scan_pending_page`, in
`bm25_load_if_needed`'s per-term pending walk between two pages of the captured chain,
with no buffer held. `t/025_standby_pending_recycle.pl` parks a standby `@@@` scan there
while the primary reuses the next page, once as a segment page (XX002 before) and once as
a new pending page (101 of 400 rows before); both now raise 40001.
`sql/128_pending_chain_epoch` pins the stamping, healthy scans through every walker, and
the epoch rule through a pure probe.

## Alternatives considered

- **Store the epoch in a new metapage field**, the issue's first proposal. The decided
  design reuses `next_gen` and adds no field; the issue's own constraint was that a new
  field must follow ADR 0009's additivity rules and that an old binary would carry a stale
  nonzero epoch through unchanged. Comparing against the captured `next_gen` needs no stored
  per-chain value at all: any page re-initialized after the capture carries a gen at least
  that large. And because pages an older binary writes carry epoch 0, which passes, an older
  binary appending to a new chain cannot make healthy scans fail, which an exact-match
  stored epoch would.
- **Document `hot_standby_feedback = on` as required.** Feedback is asynchronous and
  drops on a walreceiver reconnect, and core pairs it with conflict records for this
  reason. Kept only as a README mitigation.
- **A recovery-conflict record, as nbtree's `XLOG_BTREE_REUSE_PAGE`.** bm25 writes
  through Generic WAL, which has no record type that carries a conflict horizon.
- **Equality instead of `>=`.** Would reject pages legitimately appended to the scan's
  own chain after the capture.
- **A retry without a subtransaction (a private resource owner and `PG_TRY`).** Unsound:
  continuing after a caught ERROR without (sub)transaction abort leaves LWLocks,
  interrupt holdoff and error state unrecovered (issue #291). Left to #307.

## Consequences

- A standby scan that overlaps a seal-plus-reuse raises 40001, where it returned too few
  rows. On the primary the horizon still prevents reuse under a live snapshot, so
  nothing changes there.
- **Mixed binaries.** An epoch-0 chain, and a binary that writes epoch 0 into an
  epoch-bearing chain, are unprotected on a standby until the chain drains.
- **Residual: a pending page recycled as a SEGCAT or RETIRED page still gives XX002 on a
  standby.** Catalog and retired-descriptor pages carry `seg_gen = 0` (`bm25_page_init`),
  which `bm25_pending_epoch_validate` passes, so the kind gate in
  `bm25_pending_page_flags_validate` fires instead. It is loud, never silent, and needs a
  merge or retire to take the recycled page inside the window. It is not #307.
  Converting it to 40001 would mean raising 40001 on a standby for a page of a known
  non-pending kind, which also masks real corruption there as retryable; it was left as a recorded
  residual.
- Writer-side, the epoch rides the single append record and is the orphan sweep's
  liveness test for a pending page (ADR 0116, ADR 0117). The pending append became one
  record in that change, which removed the case where the next chain reused an un-bumped
  epoch.
- Not fixed here: the inline term-entry stride is repeated in three walkers and the
  debug pending walker is unvalidated (#313).

## Addendum (2026-10-05, PRs #331-#350)

#307 is decided (ADR 0121). In recovery the ranking build runs without the subtransaction
and without a retry, so the ranked path now reports a reuse abort the way this record's flat
`@@@` path already did: a bare 40001. The flat `@@@` retry this record deferred (XCUT-04) is
closed as unneeded. The rejected "private resource owner plus `PG_TRY`" option stays rejected.
The new `next_gen` exhaustion guard (ADR 0062's addendum) covers this record's epoch draw at
the pending chain start as well as the segment draw.
