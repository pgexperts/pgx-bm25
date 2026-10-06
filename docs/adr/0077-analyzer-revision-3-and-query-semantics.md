---
id: 0077
title: Analyzer revision 3 — run splitting drops LC_CTYPE and the fingerprint covers the database encoding; per-lexeme positions stay, deliberately
date: 2026-08-20
status: Accepted
summary: Single-byte run splitting no longer consults LC_CTYPE and the fingerprint gains the database encoding as a sixth component, together forcing BM25_ANALYZER_REVISION 3. The sibling fix — co-positioning a compound run's lexemes to match core FTS — was written, measured to break phrase search into silent zero-row results, and reverted; it needs a query-side counterpart this engine does not have.
---

# 0077. Analyzer revision 3 — run splitting drops LC_CTYPE and the fingerprint covers the database encoding; per-lexeme positions stay, deliberately

## Context

**TEXT-06 — run splitting consulted `LC_CTYPE`.** `bm25_is_word_byte` fell through to a
bare `isalnum()`, which is `LC_CTYPE`-dependent. In a single-byte server encoding that
fallback decides run boundaries for *every* byte including `0x80-0xFF`, so the same
LATIN1 text tokenized differently under a different `LC_CTYPE` — changing `doclen`, and
therefore every score — with nothing recording it.

`LC_CTYPE` is not the database default collation and is not named in the determinism
contract this codebase publishes (`src/bm25.h`: "a pure function of
(cfg->stem_dict_oid, database default collation, token)"). The module's own header
argues the locale's `isalnum()` is "correct and necessary" for a single-byte encoding,
which as linguistics it is. **The defect is the unreconciled pair**, not the
`isalnum()` call in isolation.

Separately, the five existing fingerprint components describe the analyzer's
configuration and code and nothing described its *environment* — yet run splitting
branches on `pg_database_encoding_max_length()`. An index dumped and restored into a
differently-encoded database kept a fingerprint asserting its terms were still valid.

## Decision

Bump `BM25_ANALYZER_REVISION` from 2 to 3.

- **`bm25_is_word_byte` is ASCII-only below the multibyte branch.** Between changing the
  determinism contract and changing the code, the code loses: a scoring input that
  varies with an environment setting nothing records is not a property this format can
  carry. The cost is that a high byte in a single-byte encoding is now a separator
  rather than a letter. UTF-8 databases are byte-identical — the multibyte branch above
  always dominated there.
- **The database encoding is appended as a sixth fingerprint component.** Encoding is
  the right operand and `LC_CTYPE` is not, which is the point of the other half:
  fingerprinting a dependency is the alternative to removing it, and removing it is
  better where the dependency was never wanted. What remains — the encoding branch — *is*
  wanted, and is now recorded. Appended, never reordered.

## The TEXT-05 attempt, and why it was reverted

This section is the more useful half of the record, because the change looked obviously
correct and is not.

`bm25_analyze` advances position once per **lexeme**. Core FTS advances once per source
**run**, plus once more per `TSL_ADDPOS` lexeme (`src/backend/tsearch/ts_parse.c`), so a
compound split's lexemes share the run's position. Ours therefore drift from
`to_tsvector`'s for the same text and the same dictionary, and the drift grows with
every multi-lexeme run.

The doc-side fix — co-position a run's lexemes — was implemented, verified against core's
source, A/B-proven to change positions exactly as intended (`footballklubber` went from
`{0,1,2,3,4,5}` to `{0,0,0,0,0,0}`), and passed all 105 suites.

**It also silently broke phrase search.** Adversarial review caught it; the mechanism is
that the document side is only half the pipeline. The query side expands a phrase into
one matcher term per query *lexeme*, and `bm25_phrase.c`'s `ordered_match` requires
strictly increasing document positions. N co-positioned document lexemes can never fill
N strictly increasing slots. Measured under `ispell_sample`:

| query | before | with the doc-side fix |
|---|---|---|
| `'"footballklubber"'` | matches | **0 rows** |
| `'"footballklubber yesterday"'` | matches | **0 rows** |
| `'"football klubber"'` | matches | **0 rows** |

Core survives the identical document layout because `phraseto_tsquery` compensates on
the **query** side, emitting alternatives (`'footballklubber' | 'foot' & 'ball' &
'klubber' | …`). This engine has no same-position handling in its matcher at all.

So closing TEXT-05 means building that query-side counterpart — grouping co-positioned
query lexemes into per-position alternative groups — not changing the position line. Per-lexeme
positions are at least *self-consistent* between the two sides, which is exactly what
keeps phrase search working today. Reverted, and the code says "do not fix this in
isolation; it has been tried."

**Also unfixed, and named here so it is not rediscovered as new:** the `doclen` half of
TEXT-05. A 6-lexeme compound contributes 6 to the BM25 length denominator, so documents
with ambiguous words are length-penalised. That belongs with the query-side work.

## Alternatives considered

- **Ship the position fix anyway and note the phrase caveat.** No. It converts working
  queries into silent empty results, which is the failure class #132 treated as
  critical. A half-migration is worse than either end state.
- **Widen the fingerprint to cover `LC_CTYPE` and leave `isalnum()` alone.** Makes the
  gate honest without changing tokenization. Rejected: it converts a silent wrong answer
  into a REINDEX demand every time a locale changes, for a dependency the contract never
  wanted. Detecting a problem is worth less than not having it.
- **Fix run splitting and leave the fingerprint at five components.** Closes this
  instance and leaves the fingerprint blind to the whole environment class, which is how
  this one arrived.
- **Bump the revision twice, once per finding.** Moot now that only one finding changes
  output, but the principle stands: a needless bump costs every user a REINDEX.

## Consequences

- **Existing indexes fail the fingerprint gate and must be reindexed.** That is what a
  revision bump is for; `bm25_fingerprint_gate` raises `ERRCODE_FEATURE_NOT_SUPPORTED`
  rather than degrading. A single global counter sweeps up indexes whose output did not
  change — UTF-8 english users among them. That is the existing design, not something
  this record introduces.
- A new probe, `bm25_debug_analyze_positions`, returns each token's position parallel to
  `bm25_debug_tokenize`'s text. It was added to test the reverted change and is kept,
  because the divergence it exposes is real and now pinned: `sql/99_query_semantics`
  asserts the current per-lexeme positions **as divergent on purpose**, so the eventual
  query-side fix has a before/after to point at.
- `sql/99_query_semantics` also gains the phrase regression guard the revert exists to
  protect — three phrase queries over a compound dictionary. **No suite phrase-tested a
  compound dictionary before**, which is why 105 green suites said nothing while the
  change was broken. That gap, not the position arithmetic, is what let this get as far
  as review.

## Addendum (2026-08-24)

The query-side counterpart this record said TEXT-05 needs now exists: `docs/adr/0085`
makes the phrase matcher's unit a **slot** (one source word, satisfied by any lexeme
the analyzer emitted for it) rather than a query lexeme. It landed on its own and is
**inert**, because it derives its slots from the analyzer's positions and the analyzer
still emits one per lexeme — so every slot is one token, the grouping is the identity,
and all 112 suites' output is byte-identical to the parent build's.

Two things this record said, which that change makes obsolete rather than wrong:

- "This engine has no same-position handling in its matcher at all" — it does now.
- "closing TEXT-05 means building that query-side counterpart … not changing the
  position line" — the counterpart is built; the position line is what remains, and it
  must arrive together with within-run deduplication, a run-count `doclen`, and a
  `BM25_ANALYZER_REVISION` bump.

Separately, `src/bm25_analyzer.c`'s revision history had been asserting that revision 3
already advanced position per run — the very half this record documents as reverted.
Corrected there; nothing about revision 3's actual content changes.

## Addendum (2026-08-24)

The position line this record deferred has now landed, as `docs/adr/0087`
(`BM25_ANALYZER_REVISION` 4 → 5, issue #184): `bm25_analyze` advances position once per
source run, with within-run deduplication and a run-count `doclen` landing in the same
change, exactly as the previous addendum said it must. That resolves the **TEXT-05**
half this record recorded as attempted, reverted, and left open. This record's
**TEXT-06** half — ASCII-only run splitting below the multibyte branch, and the database
encoding as the fingerprint's sixth component — is untouched by ADR 0087 and still
stands as documented above; this record is not superseded.

## Addendum (2026-10-04)

The fresh-eyes review of this date found that in single-byte server encodings every byte 0x80-0xFF is a separator: Cyrillic text in a WIN1251/KOI8 database indexes nothing, and LATIN1 text indexes as colliding fragments. Under a real single-byte LC_CTYPE this is a regression against core to_tsvector. Tracked in #295.

## Addendum (2026-10-05)

The decision to drop `LC_CTYPE` stands, and the sixth fingerprint component still covers the
encoding. The cost it accepted, that a high byte in a single-byte encoding is a separator, is
replaced at revision 6 (#295, ADR 0114): the classification now comes from a fixed table
generated from PostgreSQL 18.6's conversion maps and Unicode 16.0, not from the locale, so the
predicate is still independent of `LC_CTYPE`. WIN1251 and KOI8-R Cyrillic and LATIN1 accents
now tokenize as words. Residual: in WIN1258 and WIN874 a combining mark the generator does not
classify as alphabetic or a digit is still a separator (bytes `0xCC`, `0xD2`, `0xDE`, `0xEC`
and `0xF2` in WIN1258; `0xE7` to `0xEC` and `0xEE` in WIN874). That is not a regression, since
every high byte was a separator at revision 3.
