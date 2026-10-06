---
id: 0088
title: The segment catalog carries a token count, in its padding hole, trusted via a build-time feature bit
date: 2026-08-24
status: Accepted
summary: BM25SegCatEntry and BM25SegmentHeader name their existing 4-byte padding holes as total_tokens so the merge memory-budget estimator can charge per token rather than per source run; the field is trusted only when BM25_FEAT_SEGCAT_TOKENS is set, which only a fresh build ever stamps.
---

# 0088. The segment catalog carries a token count, in its padding hole, trusted via a build-time feature bit

## Context

`bm25_accum_estimate_bytes` predicts how much accumulator residency replaying one
segment will cost, so the merge selection trim can refuse a set that cannot fit
`maintenance_work_mem`. Its dominant term was

```
total_len × live_frac × (sizeof(AccumPosting) + (has_positions ? 4 : 0))
```

`total_len` is the segment's summed doclen. Since analyzer revision 5 (ADR 0087)
doclen counts source word **runs**, while both quantities that expression charges
for scale with **tokens**: `sizeof(AccumPosting)` (32 bytes here) is per posting,
and the `+4` is per stored position, of which there is exactly one per token. For
an ispell/hunspell compound dictionary tokens run about 5× runs, so both halves
under-estimated by that factor. Snowball English emits one lexeme per run, so
English indexes were and are unaffected.

The consequence was bounded rather than dramatic, which is why ADR 0087 recorded it
as follow-up rather than a blocker: `BM25_ACCUM_SLACK_FACTOR` absorbs the first 2×,
`bm25_accum_over_budget` measures actual residency, and the progress check stops the
force loop. What remained was one no-op index rewrite per autovacuum for a
compound-dictionary index sitting at the merge budget floor.

Two constraints shaped every option. The estimator runs during **selection**, over
every candidate, before the merge has committed to reading anything — so it cannot
afford an O(dict) pass to sum `df`. And the segment catalog is a **packed
fixed-stride array with no per-record length prefix**: readers derive the entry
count from `meta.nsegs` and page bytes and stride by the compile-time `sizeof`, so
appending a field desynchronises an old reader at the second record. ADR 0009 names
the segcat entry explicitly in its *breaking* carve-out for exactly this reason.

## Decision

Name the 4-byte padding hole each struct already carries as `uint32 total_tokens`:

- `BM25SegCatEntry` — the hole at offset 4, between `header_blkno` and the 8-aligned
  `ndocs`. `sizeof` stays 40, the on-page stride stays 40, entries-per-page stays 203.
- `BM25SegmentHeader` — the hole after `gen`, same shape. `sizeof` stays 64. This copy
  is the **immutable master**; the catalog entry's copy is what the tombstone path
  decays, exactly as it already decays `total_len`, using an average derived from this
  one.

This is ADR 0009's **additive** shape (iii), "a previously invalid/zero sentinel field
being filled": no other byte moves, no stride changes, and an old binary reads the
same 40 bytes and ignores four of them. `min_read_version` does not rise,
`BM25_OLDEST_READABLE` does not move, no upgrade-registry entry is needed, and
`BM25_FORMAT_VERSION` is **not** bumped — the format generation did not change shape,
and bumping would additionally invalidate `sql/55`'s use of 9 as its future-version
literal for no functional gain.

The accumulator gains a `total_tokens` counter fed at both ingest entry points —
`bm25_accum_add_field_tokens` adds `ntok` (build, seal, drain),
`bm25_accum_add_posting` adds `tf` (merge replay, counted before its repeat-merge
early return). `bm25_accum_add_positions_to_last` deliberately does not add; it
decorates a posting already counted. The value is saturated into the `uint32` at seal.

**Trust is gated by `BM25_FEAT_SEGCAT_TOKENS` (feature_flags bit 3), stamped only by a
fresh build.** `bm25_merge_estimates` passes the field to the estimator only when the
bit is set, and 0 otherwise; the estimator charges `Max(stored_tokens, total_len)`, so an
unflagged index gets an estimate numerically identical to what it got before.

**This landed in two units, deliberately.** The first wrote, decayed and exposed the
field while leaving `bm25_accum_estimate_bytes` completely untouched, so that "no
estimate moved anywhere" was provable — `expected/104_build_memory_budget.out` and
`expected/105_merge_memory_budget.out` byte-identical. The second gave the estimator the
field. Both are byte-identical on those two files, since their fixtures are English and
the clamp picks the same number either way; the second unit's A/B tooth is instead
`sql/108` PART FIVE (`every_estimate_rose` flips t→f) and PART SIX
(`chunks_believed` 2→1) against the first unit's build.

## Alternatives considered

- **Charge from a posting count instead of a token count** — rejected on the error
  direction, which the estimator's own header fixes as policy: over-estimating costs
  a refused merge that would have fit; under-estimating costs a full no-op rewrite per
  vacuum; *err high*. Charging tokens over-estimates the 32-byte postings term (tokens
  ≥ postings) and is **exact** on the positions term. Charging postings would be exact
  on the big term but under-estimate positions — the cheap direction traded for the
  expensive one. Charging tokens also preserves calibration continuity: the measured
  1.6× actual/estimate ratio behind `BM25_ACCUM_SLACK_FACTOR = 2` was taken when
  `total_len == tokens`, so nothing needs re-measuring.
- **Grow `BM25SegCatEntry` to 48 bytes and carry both counts** — rejected as breaking
  per ADR 0009, and expensively so: it would need the first real `bm25_upgrade`
  transform, a `BM25_OLDEST_READABLE` bump, and a REINDEX story, to buy a second
  counter whose term is 8× smaller than the one already handled.
- **Gate on `format_version >= 9` instead of a feature bit** — rejected, and this is
  the finding that decided the design. `bm25_upgrade`'s transform registry is empty,
  so every accepted version gap takes the **identity restamp** path: it raises
  `format_version` in place with *no segment rewrite*. A pre-ADR-0074 index carries
  stack residue in these bytes (that fix landed 2026-08-20; before it the merge path
  copied an entry built on the stack and the residue persisted for the life of the
  segment). A version gate would let a restamp bless that residue as a token count.
  A build-time feature bit cannot, because nothing but a fresh build ever sets it.
- **Derive the bit in `bm25_derive_feature_flags`** — rejected, and forbidden in a
  comment there. Every other feature bit is a property of index *content*, derivable
  by looking. This one is a property of the *writer*, and no amount of reading an
  existing index establishes it.
- **Sum `df` over the segment dictionary at selection time** — no new bytes, but an
  O(dict) read per candidate during selection, which is precisely the cost the
  estimator exists to avoid paying.
- **A scaled fixed-point tokens-per-run ratio** — fits the same 4 bytes and cannot
  overflow, but loses exactness exactly at the small segments near the budget floor
  where the defect bites, and invents a fixed-point convention for no gain that
  `Max(stored, total_len)` does not already provide.
- **A blanket multiplier on the existing charge** — already rejected in the
  estimator's own header: it taxes every English index to insure against a dictionary
  it is not using.

## Consequences

**Existing indexes gain nothing until REINDEX**, because `bm25_upgrade` deliberately
does not confer the bit. That is a smaller cost than it looks: the estimate only
matters for dictionaries that emit several lexemes per word, and analyzer revision 5
already forces a REINDEX on exactly those (ADR 0087's fingerprint gate). The
population that needs the fix is the population already being rebuilt.

**`feature_flags`' contract is narrowed, not broken.** It was documented as
"INFORMATIONAL ONLY — never consulted by the gate." Bit 3 *is* consulted — by the
merge estimator, as a trust signal for an optional field — while remaining invisible
to `bm25_meta_validate`, which still decides readability from `format_version` and
`min_read_version` alone. The comments on the flag defines and on the metapage field
now say so explicitly.

**The #144 padding-residue class is retired for `BM25SegCatEntry`, not merely
guarded.** The struct no longer has an implicit hole for stack residue to hide in;
the writer assigns every byte. `bm25_segcat_entry_from_hdr`'s `memset` stays as
defence against a future member reorder reintroducing one, and
`BM25RetiredEntry` still carries its hole and its assertion.

`sql/96`'s segcat assertion changed from "these four bytes are zero" to "these four
bytes decode to the entry's `total_len`". The intermediate formulation — compare the
bytes against what `bm25_debug_segcat` reports — was written first and is worthless:
both sides read the *same on-page bytes*, so they agree on residue as readily as on
data, and a build with the counter neutralised passed it while `sql/108` failed.
Comparing against `total_len` works only because that suite's fixture is English and
deletes nothing, which makes the run count and the token count the same number by
construction and therefore an independent witness. So the replacement is stronger than
the retired zero-check — it requires the bytes to be right rather than merely quiet —
but only in that specific form, and it additionally pins survivor preservation across
the merge swap's whole-struct copy, which nothing else exercises.

**`BM25SegmentHeader` gains a size assertion it never had**, which matters because
its per-field arrays are serialized at `base + sizeof(BM25SegmentHeader)`.

**Saturation is accepted and bounded.** A segment past `PG_UINT32_MAX` tokens stores
the cap. The estimator takes `Max(stored, total_len)`, so an absent, saturated or
otherwise-low value degrades to the run count and can never charge *less* than the
pre-change estimate. A segment that large is refused by any realistic budget on
either number.

**The write-time invariant `total_tokens >= total_len` is not a lifetime invariant.**
Tombstone decay subtracts integer averages from both, and the floors can round the
token count below the run count on a heavily-tombstoned segment. Nothing may assert
the inequality for a decayed entry; `Max()` absorbs it by construction.

**Two residual hazards, recorded rather than engineered around, both requiring a
binary older than this change to write into an index built by one newer.** A
post-ADR-0074 but pre-this-change binary tombstoning such an index decays `total_len`
and leaves `total_tokens` alone, drifting the entry high — an over-estimate, the cheap
direction. And an index built by a counter-aware binary (bit set) whose catalog is
later written by a *pre*-ADR-0074 binary could plant residue in a trusted field. That is a two-binary development
scenario spanning 2026-08-20, impossible in any released artifact, and its worst case
under `Max()` is an inflated estimate — a refused merge, the documented cheap
direction — never the rewrite loop this work exists to remove.

## Addendum (2026-08-24)

Adversarial review of the second unit turned up a third defect in the same
expression -- the live-document discount was applied twice -- which is now fixed.
This record did not name it: it was scoped out of the work by the specification
behind it, not by anything written here, so a reader looking above for the
"non-goal" will not find one. The choice of which side to fix is the part worth
recording.

`bm25_accum_estimate_bytes` multiplied its postings/positions charge by
`live_ndocs / ndocs` even though its input arrives **already** scaled to live
documents: `bm25_livedocs_clear` — the only tombstone path — decays the catalog
entry's `total_len` and `total_tokens` in place in the same WAL record that
decrements `live_ndocs`. Measured on a 10-document segment with one document
tombstoned, the charge was for 486 tokens where 540 are replayed; the error grew
as `live_ndocs` fell, and it was in the expensive direction.

**The fix had to be on the estimator side, not the decay side.** The entry's
`total_len` is folded into the metapage's corpus-wide `total_len` by
`bm25_segcat_publish_append` and `_swap`, and that is `avgdl`'s numerator — a
segment that stopped decaying would leave dead documents' lengths in a scoring
statistic. Trading a wrong ranking answer for a tighter memory estimate is not a
trade worth making, so the decay stays and the multiplier goes. That makes the
catalog entry's `total_len`/`total_tokens` being **live-scaled by contract** an
invariant the estimator now depends on, which is why `sql/105`'s new section
asserts the premise — the two fixtures agree on every input `bm25_debug_segcat`
exposes and differ only in `ndocs` — alongside the equality itself.

This record's mirroring of the decay for `total_tokens` was not the cause — it kept
the error proportional rather than introducing it — but it is what made the fix a
single-expression change, since both quantities were already decayed in step.

Two approximations remain, neither of which `live_frac` was correcting: the decay
subtracts the segment's *average* doclen per tombstone, so it drifts if deleted
documents are longer or shorter than average, and it clamps at 0.
