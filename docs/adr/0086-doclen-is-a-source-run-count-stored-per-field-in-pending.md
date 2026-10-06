---
id: 0086
title: doclen is a source-run count, stored per field in pending format v8
date: 2026-08-24
status: Accepted
summary: Define a field's BM25 length as max(token position) + 1 rather than its token count, and store the per-field value in each pending document record (format v8) instead of reconstructing it as a sum of tf, so pending and sealed scoring cannot diverge once the analyzer emits several tokens per source word.
---

# 0086. doclen is a source-run count, stored per field in pending format v8

## Context

Issue #184 has two halves. ADR 0085 closed the query-side half: a phrase slot is one
source word, not one lexeme. This record closes the length half, which the issue named
and 0077 left explicitly open: *"a 6-lexeme compound contributes 6 to the BM25 length
denominator, so documents containing ambiguous words are systematically length-penalised."*

BM25's `b` term divides a document's length by the corpus average. If a compound-splitting
or thesaurus dictionary turns one source word into six lexemes and all six are charged to
the denominator, the penalty is an artifact of the dictionary rather than of the text: two
documents saying the same thing rank differently because one used a word the dictionary
happens to decompose. Core FTS does not have this problem, because `ts_parse.c` advances
its position counter once per source word run.

The obvious fix is to count runs. The constraint that shapes this record is that the
run-count fix cannot land in the same change as the analyzer flip that makes runs and
lexemes differ, and it must not move a single score when it lands on its own.

Two facts make that possible and one makes it hard.

- `bm25_analyze` stamps every emitted token with a per-(document, field) position, and
  nothing else touches that counter, so positions are dense `0..ntok-1` today. Therefore
  `max(position) + 1 == ntok` on every path — including the drain's reconstructed token
  arrays, whose positions decode back to the originals. A definition written in terms of
  positions is value-identical to the current one at the current analyzer revision.
- Sealed segments have always stored doclen explicitly, in the per-field NORMS chain. The
  seal, the merge and the scorer all read a stored number.
- The pending list did not. A pending record carried per-(field, term) `tf` and no length,
  so three readers — `pending_stats_by_field`, `pending_score_term`, and by extension the
  WAND pending arm — RECONSTRUCTED per-field doclen as the sum of `tf` over a field's term
  entries. That is exact only while doclen is a token count.

The last point is the whole problem. Under a run-count doclen with a multi-lexeme
dictionary, an unsealed document would be scored on its lexeme count while the same
document, after a `bm25_seal()`, would be scored on its run count. A seal would silently
re-rank the corpus — for compound documents only, which is to say invisibly to every
English-Snowball regression suite in the tree, since Snowball emits exactly one lexeme per
accepted run.

## Decision

We will define a field's doclen as **`max(token position) + 1`**, and 0 for a field with
no tokens, computed in the single place both ingest paths reach
(`bm25_accum_add_field_tokens`); and we will **store** the per-field doclen in each pending
document record, as a `uint32 doclen_by_field[]` array between the doc header and the term
entries, which raises `BM25_FORMAT_VERSION` from 7 to 8.

Three details of the storage decision are load-bearing:

- **The record is self-describing.** Presence of the array is announced by
  `BM25_PENDING_DOC_FIELDLENS` in the record's own `flags`, with the element count in a new
  `nfieldlens` field that occupies the two bytes of padding the compiler was already
  inserting after `tid` (so `sizeof(BM25PendingDocHeader)` stays 40 and every other member
  keeps its offset — pinned by an exact-size `StaticAssertDecl` beside the existing
  `MAXALIGN` one). Readers dispatch per record, never on the index's stamped
  `format_version`. `bm25_pending_doc_entries_off` is the one stride rule; the eleven
  walkers that open-coded `MAXALIGN(sizeof(BM25PendingDocHeader))` now call it.
- **The floor rises lazily.** `min_read_version` goes to
  `BM25_MIN_READ_PENDING_FIELDLENS` (8) in the same Generic WAL record as the first v8
  pending record, mirroring `BM25_MIN_READ_PENDING_SPAN` exactly. A plain `CREATE INDEX`
  writes no pending records, so an index that is built and never written to keeps its old
  floor and an older binary can still read it; the first `aminsert` is what closes that
  door, and it closes it before any reader can observe the record it protects. Note that
  `CREATE INDEX CONCURRENTLY` DOES reach `aminsert` in its validate phase, so a CIC over a
  table taking concurrent DML can raise the floor with no user `INSERT` against the
  finished index — the same caveat v7's span floor already carried.
- **`BM25_OLDEST_READABLE` does not move and `bm25_upgrade` gets no registry entry.** The
  change is additive in the ADR 0009 sense: it adds an optional region to a transient
  record and nothing sealed changes shape. Pre-v8 records stay readable through the
  sum-of-`tf` path, which is retained.

This carries **no** `BM25_ANALYZER_REVISION` bump. That constant's contract is "bump on any
change to `bm25_analyze`'s output", and this changes none. The *value* of doclen moves only
when the analyzer starts co-positioning a run's lexemes, and that change carries its own
bump; a second bump here would cost users a REINDEX for a change that moves nothing.

## Alternatives considered

- **Leave pending scoring on the sum-of-`tf` reconstruction.** Free, and correct until the
  analyzer flip — at which point it becomes a wrong-score class that no existing suite can
  see. This is precisely the defect the record exists to prevent, and deferring it would
  mean deferring it into a change (the flip) that is already large enough to hide it.
- **Derive the run count at scan time by decoding the position blobs.** Correct, and needs
  no format change. Rejected on cost: the pending walk is a per-term re-walk of the whole
  chain, and this would turn it into a per-term full position decode on the read path — the
  expensive walk the Phase-1 notes already flag — to avoid four bytes per field per pending
  record.
- **Key the record layout on the index's stamped `format_version`** (a v7 index keeps
  receiving v7 records, a v8 index writes v8 records; the readers branch on the metapage).
  This was the design in the implementation plan, and it is one race short of correct:
  `bm25_upgrade` drains the pending list and *then* re-stamps the metapage under
  `RowExclusiveLock`, which does not exclude inserts, so a concurrent `aminsert` can land a
  v7-layout record in the window and have it read back as v8. A self-describing record
  costs one flag bit and one otherwise-wasted `uint16`, closes that window, and additionally
  makes a v7 record written by an older binary into a v8 index read correctly rather than
  desyncing. It also gives every reader a single stride rule instead of a metapage lookup
  the in-page iterator does not have the arguments to perform.
- **Store the array only on the document's first part** (continuations would omit it).
  Saves a few bytes on multi-part documents. Rejected because it makes a record's layout
  depend on a *neighbouring* record, and ADR 0069 records that an ordinary cancelled VACUUM
  can leave a stranded continuation with no head — which every walker would then stride
  wrong. Repeating the array matches what the record already does with `doclen` and the
  row's key, for the same reason.
- **Make it a hard break** (raise `BM25_OLDEST_READABLE`, force REINDEX). Unnecessary: the
  pending list is transient and drained at every seal, and the lazy floor already gives an
  older binary a clean refusal rather than a misread.

## Consequences

- Scores do not move. The full 112-suite regression run is byte-identical except for the
  suites that print a format version (`02_meta`, `12_format`, `12_meta_v4`, `22_format_v4`,
  `42_format_v5`, `55_format_compat`, `71_maintenance_privileges`), one error DETAIL in
  `77_pending_doc_ceiling` that reports the doc header's byte cost (40 -> 48 at
  `field_count == 1`), and the two suites that gained new assertions.
- The analyzer flip now changes doclen on every path without touching a doclen line, and
  the two ingest paths cannot disagree by construction: the drain re-derives the value from
  the same positions the append stored, and `drain_doc_flush` asserts (under
  `USE_ASSERT_CHECKING`) that the two agree per (doc, field).
- A pending record's header region is 8 bytes larger per PART at `field_count == 1`
  (`MAXALIGN(40 + 4)` = 48 against 40), and 128 bytes larger at `BM25_MAX_FIELDS` = 32
  (`MAXALIGN(40 + 128)` = 168). That is charged against the per-page record budget, so a
  document spans pages very slightly sooner; the one-entry-must-fit-a-page ceiling
  tightens by the same amount, which is the `40 -> 48` in `77_pending_doc_ceiling`'s
  error DETAIL.
- **Any index that receives an INSERT becomes unreadable by a pre-v8 binary.** This is the
  intended behavior of the floor — the alternative is a silent misparse — but it is a real
  downgrade cost, and it is paid on the first insert rather than on some rarer event the way
  v7's spanning floor was.
- `sql/55_format_compat` case (J) pins the whole floor story: built-only stays at 5, the
  first pending write raises it to 8, and a seal does not lower it. That case is this
  change's A/B: on the parent build it prints the old version and a floor that stays at 5.
- **The guard for the NEXT change had to be written over the ispell dictionary, not over
  english, and finding that out cost an experiment.** `sql/16_pending_ryw` gained a
  seal-neutrality section — capture every score and every per-field length statistic over a
  multi-field corpus, seal, capture again, require that nothing moved — and it is worth
  having for the structural property, but it CANNOT catch the failure this record exists to
  prevent: english emits one token per source word, so a pending side left on sum-of-`tf`
  agrees with a sealed side on run counts and the section stays green. Measured, not
  reasoned: a build carrying per-run positions with the v8 array disabled passes
  `16_pending_ryw`. The same assertion over `sql/65_multilexeme_tokens`' compound corpus
  fails on that build — `distinct_scores_before_seal` 1 -> 2,
  `compound_scores_that_moved` 0 -> 1, `compound_field_stats_that_moved` 0 -> 1, with the
  unsealed copy of `'footballklubber'` scoring 0.311209 against its byte-identical sealed
  twin's 0.802591 and the corpus Σdoclen collapsing 11 -> 6 across the seal. That is the
  guard.
- **A latent defect in the two walkers this record touches was fixed alongside it, and
  had to be.** `pending_stats_by_field` and `pending_score_term` reset their per-document
  scratch only on a record WITHOUT `BM25_PENDING_DOC_CONT`, and never compared a
  continuation's TID against the document they were accumulating — so an orphaned
  continuation (part 0 invalidated by a cancelled VACUUM, ADR 0069) had its term entries
  folded into whichever document preceded it on the chain. `bm25_pending_drain` has
  carried that comparison since #57; the scan side never grew one, and its comments
  reasoned only about the orphan being the FIRST record a walk sees. Measured: a ranked
  scan for a term occurring only in the stranded document returned the preceding document
  with a score of 0.287682. This record forced the issue because `len_stored` persists
  across records — it makes the length half correct for an all-v8 chain by accident and
  leaves it wrong for a mixed one — so the honest fix was the TID check the drain already
  has. Pinned before the seal in `sql/92_pending_stranded_continuation` PART TWO.
- The `(C)`/`(D)` "this build + 1" fixtures in `sql/55_format_compat` and
  `t/013_format_compat_replica`, and the target versions in `t/014_upgrade_crash`, moved
  with `BM25_FORMAT_VERSION`, as their own comments require. The two TAP suites are
  verifiable only in CI on this host.

## Addendum (2026-08-24)

Two corrections from adversarial review, neither changing the decision.

**"v8 reads v5" holds for segments, not for every pending record.** v7 inserted
`flags` at the offset a v5/v6 record used for `key_type`/`key_pad0`/`key_size`, so a
pre-v7 KEYED pending record presents key metadata as flag bits: `key_type == 1`
already read as `BM25_PENDING_DOC_CONT` under v7, and `key_type == 2` now reads as
`BM25_PENDING_DOC_FIELDLENS`, which makes `bm25_pending_iter_next` raise
`ERRCODE_INDEX_CORRUPTED` on the array it then cannot find. Exposure is nil -- no
pre-v7 binary was released, and a pending list is drained by the next seal rather
than carried across an upgrade -- and failing loudly beats v7's silent mis-grouping.
Recorded because `BM25_OLDEST_READABLE`'s one-line summary reads as a stronger
promise than the pending path can keep.

**The decoded-position bound needed a wider accumulator to mean what it says.**
`bm25_varbyte_decode` yields a full 32-bit delta, so a `uint32` running sum could
wrap past 2^32 and land back under `PG_UINT16_MAX`, waving through a non-monotonic
position. The accumulator is now `uint64` and the value is narrowed only after it has
been bounded. No memory-safety consequence either way -- the blob's byte extent was
already bounded by `bm25_pending_term_entry_span` -- but a bound that a hostile blob
can step around is not a bound.

## Addendum (2026-08-24)

Issue #184's third stage has landed as ADR 0087 (`BM25_ANALYZER_REVISION` 4 → 5):
`bm25_analyze` now advances position once per source run instead of once per emitted
lexeme. This record's definition of doclen was written to make that the only thing
that had to change — `max(token position) + 1` was chosen precisely so the value
would move automatically once the analyzer stopped emitting one token per run for a
multi-lexeme dictionary — and it did: no doclen producer, on either ingest path, needed
a further edit. The Consequences section's seal-neutrality guard is what proved it:
`sql/16_pending_ryw`'s multi-field section and `sql/65_multilexeme_tokens`' compound
corpus both stay green across a seal at the new revision, where before the flip only
the former did. `sql/65`'s commentary now reports 5 distinct lexemes for
`footballklubber` (within-run dedup, ADR 0087) against a doclen of 1 (one run), rather
than the pre-flip 6 and 6.
