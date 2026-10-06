---
id: 0085
title: A phrase slot is one source word, not one query lexeme — the query-side counterpart lands first, inert
date: 2026-08-24
status: Accepted
summary: The phrase matcher's unit becomes the query SLOT (one source word, satisfied by any lexeme the analyzer emitted for it) instead of the query lexeme, which cost unordered_match its disjointness axiom and bought it a runtime classification plus an exact bipartite-matching path; it is deliberately inert until the analyzer emits per-run positions, because the reverted attempt proved the document side must never move first.
---

# 0085. A phrase slot is one source word, not one query lexeme — the query-side counterpart lands first, inert

## Context

`bm25_analyze` advances its position counter once per emitted **lexeme**. Core FTS
advances once per source **run** (`src/backend/tsearch/ts_parse.c`), so a
compound-splitting or thesaurus dictionary makes our positions drift from
`to_tsvector`'s for the same text and the same dictionary — and the drift grows with
every multi-lexeme run.

ADR 0077 records what happened when that was fixed on the document side alone: the
positions changed exactly as intended, all 105 suites stayed green, and phrase search
silently broke. `'"footballklubber"'`, `'"footballklubber yesterday"'` and
`'"football klubber"'` all went from matching to **zero rows** under `ispell_sample`.
The mechanism is structural, not a bug in the change: the query side expanded a phrase
into one matcher term per query *lexeme*, and `ordered_match` requires strictly
increasing document positions, so N co-positioned document lexemes could never fill N
slots. Core survives the identical document layout because `phraseto_tsquery`
compensates on the **query** side, emitting alternatives
(`'footballklubber' | 'foot' <-> 'ball' <-> 'klubber' | …`). This engine had no
same-position handling in its matcher at all.

So the divergence cannot be closed by changing the position line. It needs the query-side
counterpart first, and that counterpart has to be *provably inert* while it waits, or it
is just an unreviewable change bundled with the one that makes it observable.

## Decision

**The unit of a phrase becomes the SLOT — one source word of the query — and a slot is
satisfied by ANY of the lexemes the analyzer emitted for that word.**

- `bm25_phrase_slot_map` derives the grouping from `bm25_analyze`'s own token positions:
  a new slot begins wherever `.pos` changes. It relies on nothing but the analyzer's
  position counter being nondecreasing, which it asserts.
- `phrase_recheck_tid` consults the map per field: it gathers each slot's member lists,
  disqualifies the field only when a slot has **no** member present (per-slot ANY-of,
  replacing the per-token all-of), borrows the pointer outright when a slot has exactly
  one member, and otherwise merges + deduplicates them (`bm25_phrase_merge_lists`) into
  transient scratch. **The position stash does not move**: it stays keyed by token
  ordinal, `cur_qi` stays the token ordinal, and both position callbacks are untouched.
- `bm25_phrase_match`'s signature is unchanged; its **contract** widens. The lists it
  receives are per-slot, deduplicated, and may **overlap arbitrarily across slots**.
  `nterms` semantically becomes `nslots`, so the span bound `(nterms−1)+slop` is measured
  in source words — core's notion of word distance.
- `ordered_match` needed **no code change**. Its greedy strictly-increasing chain was
  already exactly the per-slot invariant: the exchange argument assumes only that each
  list ascends, and strict increase already forces distinct positions across slots. This
  is the payoff of merging a slot's members *before* the matcher rather than teaching the
  matcher about groups.
- `unordered_match` lost its KEY FACT. Its counting reduction rested on an axiom — a
  position ordinal belongs to exactly one token, so different phrase terms' position sets
  are disjoint — that slot unions retire. The axiom becomes a **runtime-checked
  precondition**: an O(m) scan of the already-sorted tagged stream for two adjacent
  entries sharing a position (exact, since equal positions sort adjacent and can only come
  from different groups). Disjoint ⇒ the old proof applies verbatim and the old sweep runs.
  Overlapping ⇒ an exact **windowed incremental bipartite matching** sweep decides each
  window by Hall's theorem, with repeated-word multiplicities folded into slot instances.

**It is deliberately inert.** Because it derives its slots from the analyzer's positions,
and the analyzer still emits one position per lexeme, every slot is exactly one token: the
map is the identity, merging is always a pointer borrow, and the classification always
selects the counting sweep. Verified three ways — by reading the emission loop (one shared
counter, incremented exactly once per emitted token, so `toks[i].pos == i`); by an
instrumented build that ERRORs when `nslots != nq`, which fired zero times across all 112
suites while its inverted form fired 25 times, proving the probe was live; and by running
the full suite against the parent build and the new build and diffing the `results/`
directories byte for byte.

## Alternatives considered

- **Teach the matcher about groups directly** (pass a group id per list and have
  `ordered_match`/`unordered_match` reason about alternatives) — rejected. It would have
  changed `ordered_match`, which is the one path with a clean correctness proof today, and
  it triples the surface of both sweeps for a semantic the merge expresses in one place.
  Merging one call earlier is why the ordered path is untouched.
- **Reconstruct core's per-variant AND-chains** (`'footballklubber' | 'foot' & 'ball' &
  'klubber'`) instead of OR-per-slot — rejected for now, and the divergence is named rather
  than hidden. `bm25_analyze` discards `TSLexeme.nvariant`, so the chains are not
  recoverable without carrying it through `BM25Token` and giving each slot an AND-of-OR
  structure. The divergence is **recall-only and one-directional**: a document containing
  just `foot` will satisfy a `footballklubber` slot, but no document core matches is ever
  dropped. Tightening later is a query-side-only change with no on-disk impact.
- **Keep the counting sweep and accept its answer under overlap** — rejected outright. It
  is wrong in the permissive direction: `S₀={5}, S₁={5,9}` at slop 0 has a window `[5,5]`
  holding one entry of each group, and the sweep reports a match although the only distinct
  assignment spans 4. A quietly wrong phrase verdict is the exact failure class this
  engine's degradation gates exist to refuse.
- **Rebuild a Kuhn matching from scratch per candidate window** — rejected on cost: N
  augmentations per window over D windows, order 4e10 on reachable input, which is the "no
  quadratic on user-sized input" rule (#139) again. The incremental version is
  O(D·G·(G+N)) — order 1e9 at the simultaneous ceiling of all three bounds, roughly an
  order of magnitude above the two sweeps already in this file, and reachable only with a
  compound dictionary, a 64-word phrase whose words alias each other's lexemes, and
  documents at the per-document token cap.
- **Land the whole thing — query side, document side, revision bump — as one change** —
  rejected. That is precisely the shape ADR 0077 recorded as unreviewable: the suites that
  would have caught the breakage did not exist, and bundling makes "which half is wrong"
  unanswerable. Landing the query side inert means its correctness bar is a byte-identity
  assertion anyone can re-run.

## Consequences

- **The document-side flip is now unblocked but still not done.** It additionally owes
  within-run deduplication (a compound's variants repeat lexemes), `doclen` switched from a
  lexeme count to a run count, and a `BM25_ANALYZER_REVISION` bump — and those belong
  together, because each one alone would cost users a separate REINDEX.
- **Two semantic changes are queued, not shipped, and must reach release notes when they
  do.** `'"footballklubber"'` will start matching documents that contain only `football`
  (the compensating recall win of OR-per-slot); `'"football klubber"'` will stop matching a
  document whose only occurrence is the compound (`klubber` never sits at the slot after
  `football`'s) — core's verdict, but a deliberate loss of a query that works today.
- **The overlap paths have no SQL-reachable test and cannot have one until the flip.** The
  discriminating evidence is `bm25_phrase.c`'s `-DBM25_PHRASE_SELFTEST` main(), now a
  gating CI step, carrying the `{5} / {5,9}` false-positive case (which fails against the
  unmodified matcher), the displacement and contract-and-re-augment shapes, and a
  200,000-case fuzz against brute force over small overlapping sets. The selftest command
  the file documented did not compile; it now carries its own backend stubs and does.
- **`sdr_sweep` is the genuinely hard code in this change**, its failure mode is a wrong
  phrase verdict under overlap, and SQL cannot reach it. The mitigations are structural: it
  is a pure function, its two augmenting-search directions each carry the Berge/exchange
  argument that makes ONE rooted search sufficient, its bound is argued from matching
  theory, it allocates nothing indexed by the position count, and the counting fast path is
  preserved for every non-compound call.
- **`BM25_PHRASE_MAX_TERMS` still caps TOKENS, not slots.** After the flip a
  compound-heavy phrase will burn the 64-term budget faster than its word count suggests,
  and the error message will name a number larger than the words typed. Accepted;
  re-basing the cap on slots would move the stash sizing for no demonstrated need.
- **The analyzer's revision history was corrected while here.** It claimed revision 3
  landed per-run positions — the half ADR 0077 records as reverted. Left standing, the work
  that finally lands them would have concluded the bump was already spent.
- ADR 0077 stands; this record resolves the *query-side* half of the TEXT-05 problem it
  left open, and does not supersede it.

## Addendum (2026-08-24)

The length half of issue #184 landed separately as ADR 0086: `doclen` is now defined as
`max(token position) + 1` — a count of emitting source runs — and the pending list stores
it per field (format v8) rather than reconstructing it as a sum of `tf`. Like the slot
model recorded here it is inert at the current analyzer revision, and for the same reason:
it is written as a function of the very positions the emission loop produces, so while
position advances per lexeme the run count and the token count are the same number.

Consequence for this record's forward-looking notes: what the analyzer flip still owes is
the emission line itself, within-run deduplication, and the `BM25_ANALYZER_REVISION` bump —
`doclen` is no longer on that list. Both enabling halves are in place, and both were built
to change behavior automatically when the flip lands, with no further edit to the phrase
matcher or to any doclen producer.

## Addendum (2026-08-24)

Issue #184's third stage — the flip this record's mechanism was built to wait for — has
landed as ADR 0087 (`BM25_ANALYZER_REVISION` 4 → 5). `bm25_analyze` now stamps one position
per source run instead of per emitted lexeme, deduplicating a run's repeated lexemes as it
goes, so `bm25_phrase_slot_map`'s grouping is no longer the identity for a multi-lexeme
dictionary: the mechanism recorded above is live, not inert. Both queued semantic changes
from the "Consequences" section are pinned in `sql/99_query_semantics`, and exactly ONE
verdict actually moved: `'"footballklubber"'` now also matches a document containing only
the separate words `football klubber`. The predicted companion — `'"football klubber"'`
ceasing to match a document whose only occurrence is the compound — did NOT move, because
that query never matched such a document in the first place: pre-flip the per-lexeme
expansion required `football` before `foot`, which no document satisfies. Measured across
the flip, not inferred. The overlap paths (`sdr_sweep` and the
classification pass) are SQL-reachable for the first time: `sql/107_per_run_positions`
PART TWO constructs a genuinely overlapping slot set with an *unordered* (`~n`) phrase
over `ispell_sample` (`'"klubber football"~0'` against a `footballklubber x football`
document) and pins both the slop-0 no-match and the slop-1 match that distinguishes a
correct bipartite-matching verdict from the counting sweep's false positive.
`-DBM25_PHRASE_SELFTEST` is no longer the only harness for this path, though it remains
the only one that fuzzes it against brute force.
