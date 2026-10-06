---
id: 0087
title: Analyzer revision 5 — one position per source run, with within-run lexeme dedup
date: 2026-08-24
status: Accepted
summary: bm25_analyze advances position once per source word rather than once per emitted lexeme and deduplicates a run's repeated lexemes, reaching term-and-position parity with core FTS for multi-lexeme dictionaries; the query-side slot model and run-count doclen landed first as the enabling counterparts.
---

# 0087. Analyzer revision 5 — one position per source run, with within-run lexeme dedup

## Context

`bm25_analyze` advanced its position counter once per emitted **lexeme**. Core
PostgreSQL FTS advances once per source **run**, plus once more per `TSL_ADDPOS`
lexeme (`src/backend/tsearch/ts_parse.c`). For a dictionary that turns one word
into several lexemes — in practice an ispell/hunspell compound splitter, the only
such class this analyzer can currently be configured with — our positions
therefore drifted from `to_tsvector`'s for the
same text and the same dictionary, and the drift grew with every multi-lexeme run
in the document.

The divergence had three distinct costs, and it is worth separating them because
they are fixed by different halves of this change:

- **Adjacency.** "Next to" meant something different here than in core, so phrase
  and proximity queries over a compound corpus gave different verdicts.
- **doclen.** A six-lexeme compound contributed six to the BM25 length denominator,
  so documents containing ambiguous words were systematically length-penalised.
- **tf.** ispell emits `klubber` twice for `footballklubber`, so a term the text
  contains once was counted twice.

Doing nothing was not an option in the sense that mattered: the divergence was
already pinned in `sql/99` as deliberate, which is a holding position, not a
resting one. It also blocked any honest claim of core-FTS parity in the query
grammar, and it left `tf` and `doclen` wrong for a class of dictionary the engine
otherwise supports.

**The reason this took three changes rather than one is the load-bearing part of
this record.** The obvious fix — co-position a run's lexemes on the document side
— was written once, verified against core's source, passed all 105 suites of the
day, and **silently broke phrase search into zero-row results**. ADR 0077 records
that revert. The document side is only half the pipeline: the query side expanded
a phrase into one matcher term per query *lexeme*, and `bm25_phrase.c`'s
`ordered_match` requires strictly increasing document positions, so N co-positioned
document lexemes could never fill N strictly increasing slots. Core survives the
identical document layout only because `phraseto_tsquery` compensates on the
**query** side, emitting alternatives (`'footballklubber' | 'foot' & 'ball' &
'klubber' | …`). This engine had no same-position handling in its matcher at all.

No suite phrase-tested a compound dictionary at the time, which is why 105 green
suites said nothing. That gap — not the position arithmetic — is what let the
broken change reach review.

## Decision

Advance position **once per source run**: every lexeme a run emits is stamped with
the run's position, and the counter is bumped once after the run, only if the run
emitted at least one token. Deduplicate a run's repeated lexemes by exact term
bytes, scoped to the run rather than the document. Bump `BM25_ANALYZER_REVISION`
from 4 to 5.

Land it only after its two enabling counterparts, both of which shipped
deliberately inert because they derive their behaviour from the very positions
this change emits:

- **ADR 0085** (#193): a phrase slot is one source *word*. `bm25_phrase_slot_map`
  groups the query's tokens into slots, the recheck merges a slot's member
  position lists before calling the matcher, and `unordered_match` gained an exact
  windowed bipartite-matching path behind a runtime disjointness check — because
  co-positioning breaks the disjointness its counting sweep rested on.
- **ADR 0086** (#194): `doclen` is `max(position) + 1`, the count of emitting
  source runs, stored per field in the pending record (format v8) rather than
  re-derived as a token count.

Dedup belongs in this same change rather than after it. Once a run's lexemes share
a position, keeping a repeat gives a term the text contains once a `tf` of 2 and
stores it twice at the same position — wrong in ranking and divergent from core.

It is worth being precise about what would *not* have caught that, because the
tempting belief is wrong: `bm25_seg_build.c`'s `Assert(npos == tfs[j])` is **not**
a tripwire for the un-deduped case. `bm25_accum_add_field_tokens` increments `tf`
and appends a position in the same iteration, so `npos == tf` holds by
construction whatever the values, and both position codecs use unbiased deltas, so
the resulting zero delta encodes and decodes silently. An un-deduped build would
be quietly wrong with every assertion satisfied — the same shape of failure as
0077's. `sql/107` PART ONE's `tf` assertion is the only thing that catches it.

Dedup also cannot land separately afterwards without spending a second
`BM25_ANALYZER_REVISION`, i.e. a second REINDEX for every user.

## Alternatives considered

- **Carry `nvariant` through `BM25Token` and reproduce core's variant chains
  exactly** — core ANDs the lexemes of one variant and ORs the variants; we OR
  everything in a slot, because `nvariant` is discarded when tokens are built.
  Rejected for this change: it triples the matcher surface to a per-slot AND-of-OR
  for a semantic the issue did not ask for, and it can land compatibly later. The
  divergence it leaves is recall-only — extra matches, never lost ones.
- **Fix the document side alone** — this is precisely what ADR 0077 reverted. It
  is recorded here because it is the change a reader will reach for first.
- **Split dedup into a follow-up change** — rejected: it would spend a second
  analyzer revision, and in the meantime the flip would ship a wrong `tf` and a
  term stored twice at one position, with nothing in the build path objecting (see
  the Decision section on why the builder's `npos == tf` assertion does not catch
  it).
- **Also make dropped stopwords consume a position, matching core** — under
  `stopwords = default` core lets a stoplist word consume a position and we do
  not. Rejected as scope: it is another revision bump and another semantics
  debate, and it is orthogonal to the multi-lexeme problem. Named here so it is
  not later rediscovered as a defect of this work.
- **Re-derive the merge memory-budget estimator in the same change** —
  `bm25_accum_estimate_bytes` charges its postings and positions terms from
  `total_len`, which is now a run count while the quantities charged for scale
  with tokens. Rejected because a correct fix needs a per-segment token count, and
  the catalog entry has no such field — adding one is an on-disk format change
  this work deliberately scoped out. The consequence is bounded and is recorded
  under Consequences below.
- **Defer the transition with `require_analyzer_match = false`** — rejected as
  unsafe, and the distinction is documented rather than engineered around. See
  Consequences.

## Consequences

**Parity is now exact** against `to_tsvector` for the same dictionary, in both the
term multiset and the position grouping. `footballklubber` yields five distinct
lexemes at one position, matching core's `'ball':1 'foot':1 'football':1
'footballklubber':1 'klubber':1`.

That parity rests on a reachability fact worth stating, because it is what makes
co-positioning *every* lexeme of a run correct rather than approximately correct.
Core also advances position for a lexeme carrying `TSL_ADDPOS`, which this change
does not reproduce — and does not need to, because no dictionary that sets that
flag can currently be configured here. `bm25_analyze` calls lexize with a NULL
`DictSubState`, so the multi-word substituters that emit `TSL_ADDPOS` fail loud
(`forbidden call of thesaurus or nested call`) at ingest rather than emitting
anything. If lookahead-capable dictionaries are ever supported, the emission loop
must give an `ADDPOS` lexeme its own position or this parity breaks silently.

**Snowball English is byte-identical across this bump.** English emits one lexeme
per run, so per-run and per-lexeme positions coincide and no English index changes
its tokenization at all. This is also the reason the change is hard to test: every
English corpus in the tree passes against a completely broken build, which was
proved during stage 2 by building the flip without its other half and watching
`16_pending_ryw` stay green. `sql/107` and `sql/99` use `ispell_sample` for this
reason, and any future work here must do the same.

**One query verdict changes, deliberately, and toward core.** A phrase
`'"footballklubber"'` now also matches a document containing the separate words
`football klubber`, because the phrase is one slot and any lexeme the run produced
satisfies it. Core matches that document for the same query too. This is a recall
gain and the visible face of the OR-group semantics; it is a semantic change that
belongs in release notes rather than being discovered by a user.

**`require_analyzer_match = false` is NOT a safe deferral for this bump, and the
contrast with revision 4 is the trap.** ADR 0080 recommends that setting for the
#62 fingerprint change specifically *because* that change was byte-identical in
tokenization — an existing index's segments were still correct and REINDEX was the
conservative cure rather than a repair. Revision 5 genuinely changes tokenization,
so deferring means querying an index whose stored terms and positions disagree
with the analyzer answering the query: wrong results, not stale ones. An operator
who carries the #62 assumption across will get silently wrong answers. REINDEX is
the only correct response.

**The merge memory-budget estimator now under-estimates for multi-lexeme
dictionaries.** `total_len` is a run count; the postings and positions charges
stand for token-scaled quantities. The under-estimate is roughly the dictionary's
lexemes-per-run ratio (about 5x for `ispell_sample`), unaffected for English. It
is bounded by the existing layering — `BM25_ACCUM_SLACK_FACTOR` absorbs the first
2x, `bm25_accum_over_budget` measures actual residency, and the progress check
stops the force loop — leaving one no-op index rewrite per autovacuum for a
compound-dictionary index sitting at the budget floor. Follow-up work, requiring a
per-segment token count in the catalog.

**`unordered_match`'s complexity bound becomes reachable.** Before this change no
query could construct overlapping slot sets, so ADR 0085's exact matching path had
only its C selftest. `sql/107` PART TWO is the first SQL that reaches it. Every
event on that path is interrupt-checked.

**`BM25_PHRASE_MAX_TERMS` stays a cap on tokens, not slots.** A compound-heavy
phrase burns it faster than its word count suggests, so the user-visible error can
name a number larger than the words typed. Accepted; re-basing the cap on slots
would touch stash sizing for no demonstrated need.

**This resolves the TEXT-05 half of ADR 0077.** That record's TEXT-06 half — run
splitting no longer consulting `LC_CTYPE` — still stands, so 0077 is not
superseded; it carries an addendum pointing here.

## Addendum (2026-08-24)

The merge memory-budget consequence recorded above — `bm25_accum_estimate_bytes`
charging from `total_len`, which this record made a run count while the quantities
charged for scale with tokens — is addressed by ADR 0088. The segment catalog entry
and the segment header now carry a `total_tokens` field in the 4-byte padding hole
each already had, so `sizeof` and the catalog's fixed stride are unchanged; the field
is trusted only when `BM25_FEAT_SEGCAT_TOKENS` is set, which only a fresh build
stamps. Existing indexes therefore gain the corrected estimate at REINDEX — the same
event this record's analyzer-revision bump already forces on the compound
dictionaries the estimate matters for.

## Addendum (2026-10-05)

Revision 6 follows (#295 and #296, ADR 0114). As for revision 5, `require_analyzer_match =
false` is not a safe deferral for a single-byte database, where tokenization changes; for
other databases the REINDEX only restamps unless the index crossed a `pg_upgrade` whose
stemmer changed. A thesaurus used as `<lang>_stem` is refused at ingest (recorded above) and
now also at `CREATE INDEX`, even on an empty table, because the fingerprint's probe lexizes it.
