---
id: 0037
title: The one-document-one-pending-page ceiling is documented and pinned, not yet lifted
date: 2026-07-29
status: Superseded
superseded_by: 0038
summary: An INSERT of a document whose postings do not fit one pending page is refused, while CREATE INDEX accepts the same value; this record measures the real ceiling (~380 distinct 7-byte stems at the default BLCKSZ), makes the error actionable, adds the missing regression coverage, and sets out what each way of lifting the limit actually costs — the lift itself is deferred, not decided here.
---

# 0037. The one-document-one-pending-page ceiling is documented and pinned, not yet lifted

## Context

`bm25_pending_append_multi` stores all of one document's postings consecutively on a
single pending page, because the drain reassembles a document without random access
(design §3.3, and the format comment at `bm25_format.h:435-441` states the physical
consecutiveness requirement). A document that does not fit is refused with
`ERRCODE_PROGRAM_LIMIT_EXCEEDED` (review ref H18, issue #57).

Three things were wrong with the state of that ceiling — none of them the existence of the
ceiling itself:

**It was undocumented.** `docs/adr/0005-tradeoffs.md` acknowledges "the pre-existing 'one
doc must fit one pending page' limit" in a bullet, and nothing in `README.md` or
`ARCHITECTURE.md` mentioned it. A user meeting the error had no way to learn what the
limit was measured in.

**The error was bare** — `errmsg` only, no `errdetail`, no `errhint`. The natural reading
of "document too large for a pending page" is that the row is malformed, not that it
exceeds a per-page byte budget that a rebuild does not share.

**There was no test.** `grep` over `sql/` and `expected/` found nothing pinning the
boundary, so the accept/reject threshold could move in either direction unnoticed.

**The measured ceiling.** The budget is bytes, not terms: `PENDING_PAGE_CAPACITY` is
`BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BM25PageOpaque))` = **8144** at the
default `BLCKSZ` (confirmed by the new `errdetail`), spent on
`MAXALIGN(sizeof(BM25PendingDocHeader))` = 40 plus, per distinct `(field, term)`,
`MAXALIGN(sizeof(BM25PendingTermEntry) + term bytes + position-blob bytes)`.

Binary-searched rather than derived, because the per-entry cost is not constant: a
position delta past 127 needs a second varbyte byte, which pushes a 7-byte stem from the
16-byte bracket into the 24-byte one partway through the document. The measured maximum
for 7-byte stems in a single-field index is **380** distinct terms — 128 entries at 16
bytes plus 252 at 24 bytes is 8096, just inside 8104. The finding's estimate of ~506
assumed a one-byte position blob for every entry and is therefore optimistic by a third.
4-byte stems stay in the 16-byte bracket throughout and 500 of them fit.

**The asymmetry is real.** `bm25_build_callback` feeds `bm25_analyze` output straight into
`BM25Accum` with no page budget, so `CREATE INDEX` over 4000-distinct-term documents
succeeds and a subsequent `INSERT` of one of those same values fails. `REINDEX` likewise.
And this is reachable with ordinary content: a ~2000-word article yields roughly 600–900
distinct stems after stopword removal.

## Decision

**Fix the three things that are wrong, and do not lift the ceiling in this change.**

- Both `ereport`s gain an `errdetail` with the document's actual token/entry counts, its
  computed byte need and the limit, plus a shared `errhint` stating that the budget is
  per-document page space rather than a term count, that `CREATE INDEX`/`REINDEX` do not
  share it, and what the caller can do instead.
- `README.md` gains a "Known limit" section with the measured numbers, both consequences
  (the build/insert asymmetry, and that long-form prose reaches it) and a pointer to
  issue #57.
- `sql/77_pending_doc_ceiling` pins the boundary by binary search (reported as a `NOTICE`,
  so a shift shows up as a readable diff), the just-inside/just-outside cases, that a
  refusal leaves no partial document and the index still answers, that shorter terms buy
  more of them, that fields divide the budget, and — the assertion that matters most —
  that `CREATE INDEX` accepts at 4000 distinct terms what `INSERT` refuses.

Lifting the ceiling is deliberately **not** decided here. It is a design change that
touches the on-disk format or the inserter's blocking contract, both of which are governed
by decisions of their own ([0009](0009-format-stability.md) on format stability, and
`bm25_insert`'s never-block-an-inserter property), and it is not the kind of thing to
settle as a side effect of adding an `errhint`. The alternatives below are recorded so
whoever takes it up starts from the real constraints rather than re-deriving them.

## Alternatives considered

These are the options for lifting the ceiling, with what each actually costs. None is
adopted yet.

- **Continuation pages, interleaved (the report's first suggestion).** A continuation flag
  in `BM25PendingDocHeader` that `BM25PendingIter` follows. Costs an on-disk format change
  (so a `min_read_version` bump, per 0009), and it collides with the Generic WAL 4-buffer
  cap: the append record already registers the metapage and the tail page, leaving room
  for only one or two more pages, so this raises the ceiling roughly 3x rather than
  removing it. Splitting across multiple WAL records instead would make a crash able to
  leave a half-document on the chain.
- **A per-document overflow chain, built as orphans and linked last.** The variant that
  actually removes the ceiling: give an oversized document its own private page chain
  rather than interleaving it, write those pages first while they are still unreferenced
  orphans (crash-safe by construction — the orphan sweep reclaims them, exactly as a
  crashed seal's pages are reclaimed), then link the chain in with the single final record
  that updates the tail and the metapage. That keeps the append atomic inside the existing
  4-buffer cap and mirrors how the seal builds a segment on an orphan chain and publishes
  it atomically. Still an on-disk format change, and it needs the drain, the iterator and
  the truncate recycler taught about the chain, plus crash-recovery TAP coverage. This is
  the option to cost out first.
- **Spill an oversized document straight into a single-document sealed segment (the
  report's second suggestion).** Attractive because it needs no format change and reuses
  the proven seal path. Two real problems. It needs the seal singleton in `ExclusiveLock`
  unconditionally, whereas `bm25_insert` takes it *conditionally* precisely so an inserter
  never blocks — so an oversized insert could wait on an unrelated seal. Worse, it scales
  backwards for the workload that motivates the fix: if most documents are article-length,
  *every* insert spills to its own segment, the pending list's batching is bypassed
  entirely, and segment count explodes into the merge policy. It is a reasonable fallback
  for genuinely rare oversized documents and a poor primary mechanism.
- **Raise `BLCKSZ`.** Works, scales the limit linearly, and is a whole-cluster decision a
  user may not be free to make. Worth stating in the hint (it is), not a fix.
- **Reject oversized documents at `CREATE INDEX` too, for symmetry.** Makes the two paths
  agree by making the build worse. Rejected outright: it would break indexes that build
  correctly today, to tidy an inconsistency in the direction nobody wants.

## Consequences

- The error now tells the caller what the limit is, what their document needed, and that a
  rebuild does not share the limit. The failure is unchanged: still `ERROR`, still
  `ERRCODE_PROGRAM_LIMIT_EXCEEDED`, still loud rather than silent.
- **The ceiling still exists.** A table of article-length documents remains
  un-`INSERT`-able after a successful build, which is the substance of issue #57 and is
  not fixed by this record. The issue should stay open, or be reopened against the option
  chosen above.
- The boundary is now pinned, so any future change to it — including an accidental one
  from a change to `BM25PendingTermEntry`, the varbyte encoding or the de-dup — shows up
  as an expected-output diff naming the old and new maxima.
- The suite's exact numbers are `BLCKSZ`-dependent and would need re-blessing on a
  non-default-`BLCKSZ` build, as would much of the rest of the tree. Stated in the suite
  header rather than left as a surprise.
