---
id: 0038
title: A pending document spans pages as same-TID continuation records (format v7)
date: 2026-07-29
supersedes: 0037
status: Accepted
summary: The one-document-one-pending-page ceiling is lifted; a document too large for a page is written as several consecutive records carrying the same TID, the later ones flagged BM25_PENDING_DOC_CONT, and every walker that aggregates per document reassembles them. BM25_FORMAT_VERSION goes to 7 and min_read_version rises to 7 lazily, only on the first spanning write.
---

# 0038. A pending document spans pages as same-TID continuation records (format v7)

## Context

[0037](0037-pending-document-ceiling.md) documented, measured and pinned the ceiling but
deliberately did not lift it: an `INSERT` was capped at ~380 distinct 7-byte stems at the
default `BLCKSZ`, while `CREATE INDEX` had no such limit, so a row a rebuild indexed
happily could be refused by a later `INSERT` of the same value — and ordinary long-form
prose reaches it (a 2000-word article is ~600–900 distinct stems after stopword removal).
It also set out what each way of lifting it would cost. This record makes the choice and
implements it (issue #57).

Two things found while reading the append path changed the plan that 0037 recommended:

**The orphan-chain scheme was solving a problem that does not exist.** 0037 proposed
building an oversized document's pages as unreferenced orphans and linking them with a
final atomic record, to avoid a crash leaving a half-document on the chain. But a
multi-record append runs inside the *inserting transaction*. Generic WAL page writes do
survive abort, so a partial document can indeed persist — and that is already a state this
design tolerates and handles: the transaction did not commit, so the TID is invisible, and
the partial postings are exactly the aborted-insert orphans the pending list already
carries until a seal tombstones them. A partially-written document is indistinguishable
from a fully-written one whose insert rolled back. No orphan-then-link machinery is needed.

**A new header field can be layout-neutral.** `BM25PendingDocHeader` was 36 bytes with a
`MAXALIGN` stride of 40, so a `uint32` lands in slack that was already being written as
padding: `sizeof` goes 36 → 40, `MAXALIGN(40)` is still 40, and every term entry stays at
the byte offset it had. ADR [0009](0009-format-stability.md) lists "a packed multi-record
struct grown" as breaking, and the reason it gives is stride desync — which does not occur
inside the stride. That is worth stating precisely rather than relying on it: the floor
still rises, for a different reason (below).

## Decision

**Same-TID continuation records.** A document whose records do not fit one page is written
as several records that all carry its TID, the second and later flagged
`BM25_PENDING_DOC_CONT`. Parts need only be *consecutive records in chain order*, not to
own their pages: the metapage buffer is held `EXCLUSIVE` for the whole append and every
appender takes it, so no other document can interleave. A document that fits a page
produces exactly one record, byte-identical to v6.

**Every walker that aggregates per DOCUMENT had to learn about it, and that is where the
real risk lived.** Four sites, all of which produced *silently wrong numbers* rather than
errors, and three of which were found only because the new suite asserted a score:

- `bm25_pending_drain` — `bm25_accum_add_field_tokens` *assigns* `doclen_by_field` and
  bumps `ndocs_by_field`, so a per-part call would leave the last part's token count as
  the document's length and count the document several times in that field's N. It must be
  called exactly once per `(doc, field)`, so the drain buffers a document's parts, copying
  term bytes (a part's page buffer is released before the next part is read).
- `pending_global_stats` — counted parts as documents and added the whole-document
  `doclen` once per part. Measured: a 4000-term row spanning ~21 parts made N read 22 for
  a two-row table and the score come out **15x high**.
- `pending_stats_by_field` — `out_ndocs[f]` is a count of documents having the field, so
  per-part emission inflated N_field and deflated that field's idf.
- `pending_score_term` — computed per-field doclen from one record's entries, scoring a
  spanning document as a far shorter one. Measured exactly: `dl` read 380 instead of 4000,
  giving 0.289504 against the same document's sealed score of 0.182322.

Term-membership walkers (the `@@@` collector, the AND-presence mask, the phrase stash, the
wildcard expander) are correct unchanged: they walk *entries*, each entry appears exactly
once wherever it lands, and they key on TID.

**The floor rises lazily.** `BM25_FORMAT_VERSION` goes to 7 and `BM25_OLDEST_READABLE`
stays 5. `min_read_version` is raised to 7 by `Max()`, in the same metapage record that
lands the first continuation — not at index creation and not by `bm25_upgrade`. A v6 reader
would treat each part as its own document and register one docid per part, so the guard is
needed; but an index that never inserts an oversized document contains nothing a v6 reader
mis-parses and keeps whatever floor it had. Monotonic: not lowered when a seal drains those
records, because a reader can hold a snapshot across the seal. No `bm25_upgrade` transform
is required — existing v6 bytes are already valid v7 bytes — so the transform registry
stays empty and its `StaticAssertDecl` is untouched.

**Two narrower limits replace the page ceiling, and both still fail loudly.** The old size
pre-check could not simply be deleted, because H7/#47 leaned on it for an unrelated reason:
it capped `total_tok` far below 65535 as a side effect, which is what made the `e_tf`
uint16 wrap unreachable. Deleting it without a replacement would have reopened that
silent-corruption path (an accepted document carrying `tf = 0`, present in the table and
unfindable through its index). So:

1. a document's total token count must fit `uint16` (~65k tokens — ~170x the old ceiling,
   and far past any natural prose document);
2. one `(field, term)` entry plus a doc header must fit one page, since an entry's term
   bytes and position blob are contiguous. Reachable only with a high-*multiplicity* term,
   tens of thousands of occurrences in one field.

The two catch different shapes — repeated versus distinct — and `sql/77` exercises both.

## Alternatives considered

- **The orphan overflow chain** (0037's recommendation, and what was originally approved).
  Rejected once the atomicity argument above showed it was unnecessary: it would have
  added a page flag, cross-page entry spanning and a chain-aware rewrite of the per-page
  iterator at both call sites, for no gain over consecutive records.
- **Interleaved continuation pages.** Collides with the Generic WAL 4-buffer cap — the
  append record already registers the metapage and the tail page — so it raises the ceiling
  roughly 3x rather than removing it.
- **Spill an oversized document into its own sealed segment.** Needs the seal singleton
  unconditionally, breaking `bm25_insert`'s never-block property, and scales backwards for
  the long-document corpora that motivate the fix: every insert would spill to its own
  segment and segment count would explode into the merge policy.
- **Widen `tf` to `uint32` on disk** to remove limit (1) as well. A real on-disk change for
  a shape no corpus has; the 65k-token cap is not a limit real content meets.
- **Split into same-TID records but make `add_field_tokens` additive.** Would avoid the
  drain's part buffer, but changes accumulator semantics that `ambuild` and the merge also
  depend on — a wider blast radius than buffering one document.

## Consequences

- The build/insert asymmetry is gone. `sql/77`, which previously pinned the ceiling, now
  pins its removal: 380/381/4000-term documents all insert and are searchable by first,
  middle and last term; all 4000 terms of the spanning document resolve to it; the same
  value through `CREATE INDEX` and `INSERT` gives identical answers; a 65000-term document
  (~200 parts) round-trips.
- Scores are identical before and after a seal (0.182322 either side), and identical
  between the exhaustive and WAND drivers on a *spanning pending* document — which the
  existing WAND parity suites never build, so `sql/77` pins that pair explicitly.
- Positions survive a part boundary: a phrase whose terms land on different parts still
  matches, and its reverse still does not.
- **No TAP coverage for a spanning document across crash or replication yet.** The format
  change and the multi-record append deserve it, and the existing crash/replica suites will
  exercise v7 generally but never build a spanning document. TAP cannot run in this
  development environment (no `--enable-tap-tests`), so writing it blind was declined
  rather than done badly — called out here and in the PR as the follow-up.
- `sql/55_format_compat`'s (C)/(D) cases hardcode "this build + 1"; they moved 7 → 8. Worth
  noting because re-blessing their output instead would have silently turned the
  forward-gate case into a no-op — at v7 the old literal 7 was no longer in the future, so
  (D) began *passing* the gate it exists to trip.

## Addendum (2026-08-24)

Format v8 (ADR 0086, issue #184) changes this record's layout again, and in a way that
touches the reasoning above at three points.

- **The header region grew.** A `uint32 doclen_by_field[nfieldlens]` array now sits between
  the doc header and the first term entry, so the offset from a header to its entries is
  `MAXALIGN(sizeof(BM25PendingDocHeader) + 4 * nfieldlens)`, not
  `MAXALIGN(sizeof(BM25PendingDocHeader))`. Every walker goes through
  `bm25_pending_doc_entries_off`. The `flags` field this record added is what announces the
  array (`BM25_PENDING_DOC_FIELDLENS`), and the count reuses the two bytes of `tid` padding,
  so the fixed struct is still exactly 40 bytes and the 40-byte stride assertion still holds
  for a record without the array.
- **Per-field doclen is no longer reconstructed.** This record's consequence list notes that
  every walker aggregating per DOCUMENT had to learn about continuations, `pending_score_term`
  and `pending_stats_by_field` among them, because per-field doclen was summed from `tf`
  across a document's parts. Those two now read the stored array off the document's first
  part instead; the sum-of-`tf` path is retained only for pre-v8 records. The span question
  is therefore moot for the length half and still live for the `match_tf` half.
- **The floor.** v7's `BM25_MIN_READ_PENDING_SPAN` (7) is now dominated by v8's
  `BM25_MIN_READ_PENDING_FIELDLENS` (8), since every record carries the array while only an
  oversized document spans. Both are still raised lazily, by `Max()`, in the same WAL record
  as the write that needs them; the span constant is kept live rather than folded away,
  because it remains the correct answer to "what does a continuation record require".

The "no TAP coverage for a spanning document" follow-up above is still open.
