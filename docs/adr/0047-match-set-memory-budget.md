---
id: 0047
title: Bound the materialized match set with a dedicated memory budget, not work_mem
date: 2026-08-10
status: Accepted
summary: A scan's match set is capped by bm25_native.max_match_memory (256 MB default, 0 = follow work_mem) and exceeding it is an ERROR, because the scan holds every matching document and cannot spill.
---

# 0047. Bound the materialized match set with a dedicated memory budget, not work_mem

## Context

Two scan paths hold the entire match set in backend memory, and neither had any
bound at all (review ref M5, finding #62.5):

- The **exhaustive scorer** (`bm25_scan_build_ranking`) accumulates one `AccEnt`
  per matching document in a dynahash, drains the survivors into a flat
  `BM25ExhScored` array, sorts it, and then copies it again into the scan-lifetime
  `so->ranked` / `so->scores` / `so->ranked_keys` arrays.
- The **non-scoring `@@@` union** (`tid_collector_add`) grows a `BM25Posting`
  array by doubling.

Neither can spill to disk: BM25 ranking is a global sort by score, so the whole
set has to exist before the first row can be returned. The consequence was that a
common term over a large corpus consumed several GB and then died on
`MaxAllocSize` with

```
ERROR:  invalid memory allocation request size 1073741824
```

— an error naming neither the query, nor the index, nor any setting that would
change the outcome. The verifier's note on the finding is the accurate summary:
the ceiling itself is a clean `ereport`, and the genuine harm is the unbounded
memory consumed on the way to it.

The plan's binding decision for this item read simply "`work_mem` bound", and
that is what was implemented first.

## Decision

The budget is **`bm25_native.max_match_memory`** — `PGC_USERSET`, `GUC_UNIT_KB`,
default **256 MB**, where **0 means "follow `work_mem`"**. There is no separate
"unbounded" value; the closest reachable thing is `MAX_KILOBYTES`. Making `0` mean
unbounded would have made the safe-looking reading of a zero the dangerous one.

"Closest reachable", not "the same": `MAX_KILOBYTES` is `INT_MAX` on a 64-bit build
(2 TB — more match set than any relation can produce) but `INT_MAX/1024` on a 32-bit
one (2 GB, ~17M documents). Truly unbounded is gone, deliberately: unbounded is the
defect.

The accounting is in **bytes**, in **one running total shared by every structure
the scan materializes**. Both properties were forced by review; the first
implementation counted *documents*, *per structure*, and was wrong on both counts:

- Counting documents is wrong wherever the structure is not per-document. The
  `@@@` union collector takes one entry per *(term, document)* and dedupes only
  afterwards, so a document limit there is really a posting limit: a ten-term query
  errored at a tenth of the matches a one-term query allowed, and the error text
  named a document count the query had never reached.
- Counting per structure lets two structures that each hold one entry per matching
  document each spend the whole budget. A boolean query fills both the accumulator
  and the leaf-presence hash, so two independent N-document caps permitted ~2N
  documents' worth of memory.

Each site charges what it actually allocates: `BM25_MATCH_BYTES_PER_DOC` for a
scored document (accumulator entry, the pending-dedupe entry shadowing it, the
drain-array slot, the ranked/scores slots, and `BM25_KEY_MAX_SIZE` for the key slot
— charged unconditionally, because the index's key width is discovered only *after*
the accumulator has been filled); `BM25_MATCH_BYTES_PER_PRESENCE` per leaf-presence
entry; `BM25_MATCH_BYTES_PER_TID` per collector posting, at 3× `sizeof(BM25Posting)`
to cover the doubling step and the copy-out. Every charge is a deliberate
over-estimate — charging exactly would let the true peak exceed the budget, which is
the one outcome this exists to prevent. That property is load-bearing and was nearly
lost: `BM25_HASH_ENTRY_OVERHEAD` initially counted only dynahash's `HASHELEMENT`
header and not the bucket-directory slot, which left the presence charge ~25% under
the truth — enough for a `must_not`-dominated boolean query to reach ~1.25× the
budget. Review caught it; the constant covers both. Charges are never released: within one
ranking build the running total *is* the peak, and each rebuild (including the
over-pull tail rebuild and every rescan) starts a fresh budget alongside the fresh
scratch context it allocates into.

Bytes also extend the bound to a structure a document count could never have
covered: the phrase stash's **position lists**, which hold up to `tf` positions per
stashed document and `tf` reaches 65,535. Their document count does track the
accumulator, but their size does not.

Crossing the bound is an **ERROR**, not a degradation, because there is nothing to
degrade to. The message names whichever GUC actually supplied the budget, and
points at the query shape that avoids materialization entirely: a ranked scan with
`LIMIT n` where `n` is no larger than `bm25_native.wand_top_k`.

Charges are taken at the **insertion** points, not the allocation sites —
`bm25_scores_add`, `phrase_and_mark`, `tid_collector_add`, `phrase_stash_add` —
because the dynahash growth is both the larger consumer and the earlier one, and
bounding only the final arrays would leave it unbounded.

`phrase_and_mark` is charged **on its own account** rather than assumed to track
the accumulator. The other per-document hashes on that path do track `acc`:
`pending_tids` is written only for a document that scored, and a `must_not`
*phrase* is rejected before execution (at scan prep, after parse and flatten — early
enough that no stash exists yet), so every stashed document is a scored one. The
leaf-presence hash is the exception, because it also marks leaves that carry no
score. A boolean query whose `must_not` names a very common term grows that hash per
document containing the term while `acc` stays small.

## Alternatives considered

- **`work_mem` alone (the plan's literal instruction).** Implemented first, and
  it is wrong here — measurably. `work_mem` sizes *one* spillable operation inside
  a plan, and its 4 MB default is calibrated for something that degrades to disk
  when it runs out. This sizes the *entire output* of a scan that cannot spill at
  all, so 4 MB stops at ~35,000 documents. Suite `66_scan_interrupts`, which
  matches 100,000 documents on a common term, went red the moment the strict bound
  landed. A full-text index whose default configuration cannot answer a common
  term over a 100k-row table is not usable. Retained as the `0` setting for
  installations that would rather keep one number.
- **`work_mem * hash_mem_multiplier`** (`get_hash_memory_limit()`, what the
  executor applies to hash joins and hash aggregates). Idiomatic for a dynahash
  and adds no new GUC, but the default multiplier is 2.0, so the ceiling merely
  moves to ~70,000 documents. It softens the break without removing it.
- **`MemoryContextAllocHuge` / `repalloc_huge`** for the arrays, as the finding
  suggests. This removes the 1 GB cliff and makes the situation *worse*: the
  cryptic allocator error is what currently stops a runaway query, and lifting it
  trades a bad error message for an OOM kill.
- **Spilling to a tuplesort.** The right long-term answer and the only one that
  makes a broad query slow instead of fatal. Out of scope for a bug-grind PR: it
  restructures the scorer's output path, the WAND seam, and the key-projection
  loop. Recorded here as the follow-up this ADR does *not* deliver.
- **`PGC_SUSET` for the new GUC**, matching the wildcard guardrails. Rejected:
  those decide whether a query may *run at all* and so cannot be in the caller's
  gift, whereas raising this one only lets a user's own session consume what it
  was already consuming before the bound existed — exactly `work_mem`'s bargain.

## Consequences

- **A behaviour change on upgrade.** A query broad enough to materialize more
  than 256 MB of match set used to succeed by consuming several GB; it now errors.
  The threshold is roughly 1.7 million matching documents at the default. The
  error is actionable where the old one was not, but it is still a new error on a
  query that used to return rows.
- **A new knob to document and keep in sync.** `bm25_native.max_match_memory`
  joins the `bm25_native.*` family. Suite `67_wildcard_guc_privileges`' GUC census
  is what asserts its `user` context, and is what will fail if a future GUC lands
  at the wrong one.
- **The per-entry charges are a maintenance obligation.** They are sums of
  `sizeof()`s over structs that other work changes; widening `AccEnt`,
  `BM25ExhScored` or `PhraseAndEnt` without revisiting them silently under-charges.
  None of it is asserted in the regression suite — how many documents a budget buys
  is platform-dependent, so suite 83 shrinks the *budget*, stays an order of
  magnitude clear of the boundary in both directions, and inspects the error's HINT
  through `GET STACKED DIAGNOSTICS` rather than diffing a count out of the text.
- **A negated leaf's folded boost is deliberately NOT checked.** A `must_not` leaf
  marks presence and contributes no score, so its boost is never read: both
  `pending_score_term` and `seg_posting_cb` are guarded by `if (!w->negated)`, and
  presence marking runs through `seg_and_cb`, which ignores idf entirely
  (`bm25_term_idf`'s return value keys off `df`, not off the boosted idf, so a zero
  boost does not even make the leaf drop out). The first version of the guard did
  fire there, rejecting a query whose behaviour was correct and unchanged, with a
  message — "would silently match nothing" — that is false for `must_not`.
- **The WAND escape is narrower than "use a ranked scan".** It holds only while
  the consumer stays inside `wand_top_k`; reading past it triggers
  `bm25_gettuple`'s over-pull tail rebuild, which re-runs the exhaustive scorer and
  lands back at the same bound. The HINT says `LIMIT n` for that reason, and suite
  83 asserts both halves — that `LIMIT 10` succeeds on a budget where `LIMIT 500`
  does not.
- **Position lists are charged, at 2× the positions appended.** The phrase stash's
  arrays double, so they briefly hold the old copy alongside the new; charging per
  position appended rather than per byte allocated smooths the lumpiness, and the 2×
  covers it. An earlier draft of this record listed the stash as a disclosed,
  unaddressed exposure. Review demonstrated the exposure was live — 200 documents of
  repeated text stashed ~37× a 64 kB budget without erroring — so it is charged
  rather than disclosed.

### Also landed in this change (PR-G), recorded for the reasoning

Three narrower bounds, none of which needed a decision of their own:

- **`#65.7` jsonb `phrase.slop`** was checked only for negativity while the text
  surface capped it at `BM25_MAX_PHRASE_SLOP`. The same query was therefore
  rejected as `"red car"~2147483647` and accepted through `bm25_phrase(slop =>
  ...)`, where `(nterms - 1) + slop` wrapped and the span test degenerated to "any
  co-occurrence". One bound now serves both surfaces.
- **`#65.8` folded boost.** `parse_boost`'s "weight must be positive" guard is
  per-weight; the *product* was not covered. `1e-300 * 1e-300` folds to exactly
  `0.0`, and `seg_posting_cb` returns early on `idf_f == 0.0` *before* the
  accumulator — so the document was absent from the result rather than scored
  zero, which is precisely the state that guard exists to reject. Confirmed
  against the pre-fix binary: the query returns 0 rows and no error. Tested at the
  leaf, where the fold is absorbing (`0*x == 0`, `Inf*x == Inf` for finite
  `x > 0`), so one check covers arbitrarily deep nesting.
- **`#65.13` token array.** Both tokenizers allocated `textlen/2+1` `BM25Token`
  entries up front — twelve bytes of scratch per input byte — so the tokenizer
  died on `MaxAllocSize` at about 89.5 MB of input, against `text`'s own ~1 GB.
  The up-front guess is clamped to 1024 entries and the array grows
  geometrically, with a ceiling that names the document rather than the allocator.

  The finding's own framing ("a large but legal text column fails to index") is
  worth correcting, since it overstates the indexing impact: a document is
  independently capped at **65,535 tokens** by the pending list's 16-bit term
  frequency, and that limit already reports itself clearly. Its example —
  `repeat('word ', 20000000)` — was therefore never indexable, and the fix changes
  only *which* error it gets, from `invalid memory allocation request size
  1200000024` to the pending list's accurate "document has 20000000 tokens; the
  limit is 65535". The real ceiling-raise is on the paths with **no** token limit:
  query-side analysis (`bm25_match` over unbounded user text, H15), `bm25_snippet`,
  and the debug SRFs. There it was the operative bound, and a document with few
  tokens but many separator bytes could cross it while nothing else objected.

  No observable pre-fix diff either way: the tokens produced were always correct,
  and the threshold is too expensive to reach in a regression suite. Suite 83's
  assertions here are about token identity across several doublings — the newly
  exercised code — not about the ceiling.

## Addendum (2026-08-24)

Raised in the M5 design review (#62): this record tells an operator that the
closest reachable thing to unbounded is `MAX_KILOBYTES`, and describes that as
"2 TB — more match set than any relation can produce", without saying what
actually happens on the way there.

A budget above roughly **5 GB is not reachable in practice**, because a second,
older ceiling bites first. The scorer's flat drain array is a plain `palloc`:

    arr = palloc(sizeof(BM25ExhScored) * Max(nacc, 1));   /* bm25_scan.c */

`sizeof(BM25ExhScored)` is 32 bytes and `MaxAllocSize` is 1,073,741,823, so the
largest request that still fits is 33,554,431 documents (1,073,741,792 bytes) and
the first that does **not** is **33,554,432**. Reaching that many matched documents
costs `BM25_MATCH_BYTES_PER_DOC` (160 bytes) each, i.e. a budget of about
**5.00 GB**.

(Note the off-by-one: 33,554,431 — `floor(MaxAllocSize/32)` — is the count this
codebase uses elsewhere as a *ceiling that is still legal*, e.g. `BM25_WAND_TOP_K_MAX`.
It is the last size that works, not the first that fails.) `so->ranked`, `so->scores` and `so->ranked_keys` are plain pallocs
on the same path.

So the setting range splits in three, and only the first two were described:

| `max_match_memory` | Behaviour |
|---|---|
| up to ~5 GB | The budget stops the query, with the actionable message this ADR added. |
| above ~5 GB | The budget never fires. `palloc` does, with `invalid memory allocation request size …` — naming neither bm25 nor the query. |
| `MAX_KILOBYTES` | The above, always. |

The consequence worth naming: an operator who raises the knob far enough to
"turn the limit off" does not get the pre-fix unbounded behaviour, they get the
**pre-fix cryptic error** — precisely the failure this ADR was written to
replace, restored by a setting the ADR appeared to endorse.

**No code change.** Converting those pallocs to `MemoryContextAllocHuge` was
considered and rejected above, and that reasoning is unchanged: the allocator
error is what currently stops a runaway query, and lifting it trades a bad
error message for an OOM kill. The defect is that the range was undocumented,
not that the ceiling exists. The decision stands as recorded; this addendum
supplies the missing number, and the GUC's own long description now carries it
so an operator meets it at `SET` time rather than in a postmortem.

Not covered by a regression test, and deliberately: reaching the cliff needs a
33.5-million-document match set. Behaviour *below* it is pinned by
`sql/83_query_limits_memory_bounds`.

## Addendum (2026-10-05, PRs #331-#350)

- **The `@@@` union collector has a hard ceiling** (#305 SCAN-08, PR #339). With
  `max_match_memory` above about 1.5 GB the budget no longer kept `tid_collector_add`'s
  doubling clear of `MaxAllocSize`, so a large enough match set died with the anonymous
  XX000 allocation error this record exists to replace. The doubling now refuses past half of
  `BM25_MATCH_MAX_TIDS` (`MaxAllocSize / sizeof(BM25Posting)`, the most one allocation may
  hold), so it stops at 2^25 entries with `ERRCODE_PROGRAM_LIMIT_EXCEEDED` (54000). The
  comment that claimed the budget kept the doubling clear, and the GUC description, are
  corrected. Huge allocations stay rejected, as above. Residual: no CI test reaches the 54000
  branch (it needs a 512 MB array), and the collector does not dedupe repeated query tokens
  before the sort (bounded by this budget and the ceiling).
- **The folded-boost guard is now a closed range.** The `#65.8` bullet above tests "finite
  and > 0" at the leaf. That still let a single denormal weight underflow `boost * idf` to 0
  and a weight near `DBL_MAX` overflow it, so a scoring leaf's folded boost must now lie in
  [1e-6, 1e6], at the same place and with `must_not` leaves still exempt (ADR 0124, D11).
