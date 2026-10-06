---
id: 0065
title: The pending-list anchor reset is unconditional and ordered before the page recycle
date: 2026-08-20
status: Accepted
summary: A drained pending chain is detached from the metapage whether or not a segment was published, in its own record when the publish record does not run, and always before bm25_pending_truncate frees the pages; the two seal sites share one implementation so the pairing cannot drift again.
---

# 0065. The pending-list anchor reset is unconditional and ordered before the page recycle

## Context

ADR 0009/D-SEAL made the segment-publish Generic WAL record the linearization
point of a seal: it appends the segment-catalog entry, updates global stats, AND
resets the pending anchor (`pending_head`/`pending_tail`/`pending_tail_free`/
`pending_npages`/`pending_ndocs`) in ONE record, so a crash can never strand the
drained documents in both the segment and the pending list.

`bm25_pending_truncate`, the post-commit page recycler, was then explicitly
forbidden from touching the anchor — its contract says the publish record already
did. That contract is sound only if the publish record always runs. It does not:
it is gated on the drain having produced at least one live document, while the
recycle is gated on something else entirely, whether any chain was drained.

Those conditions come apart exactly when VACUUM's pending sweep has invalidated
every document's TID. `bm25_pending_drain` skips every invalidated slot and
returns `ndocs == 0`, so nothing is published and the anchor is never reset —
while `drained_head` is a real block, so the whole chain is handed to the FSM. The
metapage is left naming free pages with `pending_tail_free` nonzero, and the next
INSERT writes pending-record bytes over pages the allocator has since reissued.
Nothing repairs the anchor, so the state LATCHES.

Reachable from an ordinary INSERT / DELETE / VACUUM sequence: a single VACUUM
supplies both halves, since `bm25_bulkdelete`'s sweep runs before
`bm25_vacuumcleanup`'s seal. Measured against the unfixed build, 36 of 100 rows
inserted after the zero-doc drain were silently unfindable. Issue #131.

## Decision

Detaching the chain is unconditional. When a chain was drained but nothing was
published, `bm25_pending_reset_anchor` writes the same five fields in its own
metapage-only Generic WAL record, and it runs BEFORE the recycle, never after.

The full reset (rather than a partial one) rests on the argument the publish
record already relies on: `bm25_pending_append_multi` takes the seal singleton in
`ShareLock` mode, so no append can land between the drain snapshot and this
record.

`bm25_seal_index` and `bm25_insert`'s opportunistic seal, which carried
byte-identical copies of the drain/publish/recycle body, now call one
implementation (`bm25_seal_pending_locked`). `bm25_segment_build_and_commit`'s
empty-build early return also resets the anchor when a chain was drained.

## Alternatives considered

- **Skip the recycle when nothing was published** — the smaller change, and the
  issue offered it. Rejected: it leaks the all-dead chain until some later seal
  happens to publish something, trading a corruption for an unbounded leak, and it
  leaves the dangerous shape ("publish skipped, recycle not skipped") one edit away
  from returning.
- **Fold the reset into the truncate record** — rejected: it would put the anchor
  back under a function whose contract is deliberately "recycles pages, touches no
  anchor", and the truncate is explicitly not a linearization point.
- **Fix only `bm25_seal_index`** — rejected. The defect existed twice because the
  body existed twice; fixing one copy would have left the opportunistic seal
  corrupting indexes, and `bm25_pending_should_seal` is byte-based over
  `pending_npages`, which the sweep never decrements, so that copy genuinely
  reaches the zero-doc drain.

## Consequences

A crash between the reset record and the truncate leaves the chain detached but
unrecycled — ordinary orphans for VACUUM, the same benign window the publish path
already had. The dangerous ordering (recycle before detach) is now unreachable.

The empty-build reset in `bm25_segment_build_and_commit` is unreachable today,
since both callers gate on `ndocs > 0`; it is there because the shape that caused
this bug must not be reintroducible by a change to Phase 1 alone.

`sql/90_pending_zero_doc_drain` asserts both the anchor field and the data path.
`bm25_stats` reports `pending_ndocs`, but the sweep drives it to 0 before the
defective seal runs, so it reads identically either way — hence the new
`bm25_debug_pending_head` probe, which exposes the block number that actually
latches. A fix repairing the field without repairing the data path fails the suite.
