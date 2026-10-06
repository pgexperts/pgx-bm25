---
id: 0010
title: Honest planner numbers for @@@ — two blind constants, no estimator
date: 2026-07-16
status: Accepted
summary: Replace contsel/contjoinsel and the default procost on bm25_match with compile-time constant selectivity functions (bm25_matchsel/bm25_matchjoinsel, 0.05) and an honestly-priced match function (COST 5000), rather than building a df-reading restriction estimator.
---

# 0010. Honest planner numbers for @@@ — two blind constants, no estimator

## Context

`@@@` shipped with `RESTRICT = contsel` (a hardwired 0.001) and `bm25_match`
shipped with the default `procost = 1`. Both numbers are lies, and each one
independently distorts a different node of the plan.

**Corrected root cause.** The original investigation framed the failure as
"the planner thinks seqscan is cheap." That framing is backwards. The actual
mechanism is a **Limit-proration clamp**: with `reltuples × sel` below the
query's `LIMIT`, PostgreSQL's Limit node cannot prorate the ordered index
scan's cost down to a `LIMIT/rows` slice, so the *full* scan cost is billed
for the index path instead — contsel's 0.001 **overprices the index path
~25x**, it does not underprice the seqscan. The failure window is
`reltuples ≤ LIMIT / sel`: ~10k documents at `LIMIT 10` with the old 0.001
constant, but **~200k documents at BriefBank's `SEARCH_WINDOW = 200`** — the
consumer's whole operating range, not a toy-corpus curiosity. Separately,
`procost = 1` on `bm25_match` underprices the seqscan fallback itself:
the standalone evaluator re-analyzes the query string and tokenizes the
entire document per row (~56µs/eval measured on SciFact abstracts), which
the cost model was charging one `cpu_operator_cost` (0.0025 units) for.

**Margin, not precision.** At defaults on a 5,183-doc corpus, the ordered
index path won the plan by **0.86%** (976.43 vs 984.87 modeled cost) while
the real execution times differed **173x** (333ms vs 57.8s for a 200-query
LATERAL batch). The planner was right by a coin flip: `seq_page_cost = 0.5`
or `random_page_cost = 8` — both ordinary settings — flip it to the
disastrous plan, and under that fallback `bm25_score(ctid)` returns NULL
rather than erroring, so the failure mode is silently-wrong output, not a
loud complaint. With both numbers honest, the model's gap widens from 0.86%
to roughly 300x, which no plausible GUC setting can invert. The model is
still guessing — but it is guessing with the sign and magnitude of the
truth.

**Two distinct bogus selectivities exist, and fixing one does not fix the
other.** `bm25_costestimate` (`src/bm25_handler.c`) independently hardcodes
`*sel = 0.05`, which drives the Index Scan *node's* heap-fetch cost (the
bulk of its total; a `10.0 + 0.01×tuples` floor is a small fraction). The
operator's `oprrest`, by contrast, drives the *row estimate* and therefore
Limit proration. These are two separate numbers on two separate code paths
that happen, after this change, to share a value (0.05) — that is a
coherence bonus, not a structural unification. A future change to
`BM25_MATCH_SEL` will not automatically update `bm25_costestimate`'s stub,
and vice versa.

## Decision

Ship **two honest numbers**; build **no estimator**.

- A trivial C restriction-selectivity function, `bm25_matchsel(internal,
  oid, internal, integer) RETURNS float8`, returning a compile-time
  constant `BM25_MATCH_SEL = 0.05` and ignoring the RHS entirely
  (`src/bm25_selfuncs.c`). Blindness is the feature: it fires identically
  for `Const`, `Param`, and correlated `Var` RHS (uniform behavior across
  plan-cache modes), and it can never perform plan-time I/O or execute
  `bm25_match` against statistics samples.
- A matching `bm25_matchjoinsel(internal, oid, internal, smallint,
  internal) RETURNS float8` for the `JOIN` slot, same constant, replacing
  `contjoinsel`'s 0.001 so the two slots cannot contradict each other.
  Included for internal consistency, not from a measured join-shape need.
- `ALTER OPERATOR @@@ (text, text) SET (RESTRICT = bm25_matchsel, JOIN =
  bm25_matchjoinsel)` and the same for `(text, jsonb)` — both `@@@`
  variants get the same treatment.
- `ALTER FUNCTION bm25_match(text, text) COST 5000` and `ALTER FUNCTION
  bm25_match_jsonb(text, jsonb) COST 5000`, making the seqscan fallback
  honestly expensive for every query shape, `LIMIT` or not.

Packaged as `pg_bm25_index--0.2--0.3.sql` (`default_version = '0.3'`); no
on-disk format change (still v6 — a 0.2 binary and a 0.3 binary read the
same index). `pg_bm25_index--0.1.sql` is not edited; a fresh install
chains 0.1 → 0.2 → 0.3.

### Values and their derivation

**Selectivity constant — `BM25_MATCH_SEL = 0.05`.** Swept candidates
`{0.01, 0.05, 0.1, 0.25, 0.5}` against: (a) a 5,183-row corpus under both
`random_page_cost = 8` and `seq_page_cost = 0.5` — must plan the ordered
Index Scan; (b) defaults at 5k and 100k rows — must not regress any
existing suite's plan; (c) a bare single-term `@@@` filter with no
`LIMIT`, true match rate 431/5,183 = 8.3% — must not absurdly overstate a
narrow term's selectivity. All five candidates passed (a) and (b) at every
tested point (proration-restoration alone does not discriminate — any
value ≥ 0.01 restores it in this matrix). Criterion (c) discriminated:
0.5 reproduced the "pins to half the table" absurdity almost exactly (50%
estimate vs 8.3% true, 6x over); 0.25 was a lesser but still poor 3x
overestimate; 0.01 and 0.05 underestimate (8.3x and 1.7x under,
respectively — never absurd on the high side); 0.1 was the closest
single-point estimate (1.2x over) but otherwise equivalent to 0.05 on
every planning criterion. **0.05 was frozen**: it passes every outcome
criterion with no absurdity, and — per the spec's stated tiebreaker, not
the deciding criterion — it equals `bm25_costestimate`'s existing
hardcoded `*sel = 0.05`, closing the 50x contradiction between the two
selectivity paths noted above at zero extra cost. (A two-term OR-ish
ranked query, `'negligence liability'`, matched 32.0% at both 5k and 100k
rows — confirming no single blind constant can be exactly right for both
a narrow single term and a broad multi-term OR-ish query; the freeze
criterion is "no absurdity," not "exact fit.")

**`bm25_match`/`bm25_match_jsonb` COST = 5000.** Re-measured on the same
5,183-row corpus (Apple M3 Max, PG18, fully cached — a cached-table lower
bound on the true cost, not a cold-disk measurement), forcing the seqscan
path via `enable_indexscan = off; enable_bitmapscan = off`: 5 runs each of
a filtered count (median **10.467ms**) and a bare count (median
**0.198ms**) over 5,183 rows. Arithmetic: µs/eval = (10467 − 198)µs /
5183 rows ≈ 1.98µs/eval (the marginal per-row cost `bm25_match` adds on
this corpus's short synthetic bodies — far shorter than the ~150–250-word
SciFact abstracts the original investigation calibrated against, so a
lower µs/eval here is expected, not a contradiction); the bare scan's own
modeled-vs-actual ratio (Seq Scan node `cost=0.00..97.83` for 5,183 rows,
actual 198µs) gives 198µs / 97.83 units ≈ 2.02µs/unit; cost units added
per eval = 1.98µs / 2.02(µs/unit) ≈ 0.98 units; procost truth-equivalent =
0.98 / `cpu_operator_cost`(0.0025) ≈ **392** on this corpus/hardware. This
sits below the spec's mandated `[1e3, 1e4]` bound, and below the original
SciFact-based investigation's calibration of ~5.6×10⁴ (itself a
cached-table lower bound on the *error*, not a target — real-document
bodies are far longer and tokenize slower). 5000 sits close to the
geometric mean of the two — `sqrt(392 × 56000) ≈ 4685` — giving roughly an
order of magnitude of margin in both directions: safely above what even a
cheap-to-tokenize corpus demands, safely below the real-document worst
case, and consistent with the spec's own note that procost ≥ 100 already
fixes every mis-planned cell in the original sweep. **Frozen: procost =
5000.**

### The startup-cost coupling (spec §7), verified

`bm25_costestimate` sets `*su = 1.0` — near-zero startup for a scan that
in reality builds its (WAND-capped) ranking before emitting the first
tuple. Before this change that understatement was dormant: contsel's
sub-`LIMIT` row estimate kept Limit proration off, so startup cost never
entered the arithmetic. This change turns proration on, which raises the
question of whether the now-load-bearing `*su = 1.0` could flip the plan
back once a future cost pass raises it toward honesty.

**Measured, not hypothesized: not load-bearing at shipped values.** An
experiment (5,183-row corpus, same shape as `sql/56_planner_estimates.sql`,
three GUC cells: default, `random_page_cost = 8`, `seq_page_cost = 0.5`)
patched `bm25_costestimate` to set `*su = *tot × 0.9` — startup raised to
90% of total, a deliberately extreme probe, far beyond any realistic
future correction. The Index Scan plan was chosen in **all three cells,
both before and after the patch** — no flip to Seq Scan anywhere,
including both adversarial cells. Forcing the Seq Scan + Sort alternative
directly (`enable_indexscan = off`) against the *patched* build showed it
costing **~925x–1030x more** than the patched Index Scan path in every
cell; the gap is dominated by `@@@`'s `procost ≥ 1000` charging the Seq
Scan's per-row filter evaluation, not by page costs. Raising `*su` to 90%
of `*tot` added only about 54 cost units to the index path's total — the
Seq Scan alternative would need to fall by three orders of magnitude, not
two, for this startup-cost probe alone to flip the plan.

**The refined constraint this places on a future M6 cost pass:** the
hazard is not `*su` alone. It is the **combination** of raising `*su`
toward honesty *and* a much-higher effective selectivity that shrinks the
procost-driven gap between the two paths — on this corpus and query
shape, the margin comes almost entirely from the selectivity/procost fix,
not from the startup-cost lie being small. A future cost pass may raise
`*su` toward a genuinely honest model without revisiting `BM25_MATCH_SEL`
or the match functions' `COST` *for this corpus shape and query pattern*;
but the margin should be re-checked if a future corpus or query shape
drives `@@@` selectivity dramatically higher (shrinking the procost gap)
at the same time `*su` is raised. Re-deriving this coupling from scratch
in a future task is the failure mode this record exists to prevent.

## Alternatives considered

- **A statistics- or index-reading restriction estimator** (the original
  ticket's headline) — rejected on four independent grounds. (1) It never
  fires for the shape that motivated this work: a correlated-`Var` RHS
  (LATERAL per-query search) is not a `Const`, so `get_restriction_variable`
  fails and every Const-inspecting estimator in core (verified in
  `tsmatchsel` and `generic_restriction_selectivity`) punts to a flat
  default anyway — measured `rows = 52` = `DEFAULT_MATCHING_SEL × 5183`
  exactly. (2) Plan-time index reads are lawful (the planner holds the
  executor's lock on every index of the rel, and `RelOptInfo.indexlist` is
  populated before clause selectivity runs) but unwise here: a df lookup
  costs a metapage SHARE + full segment-catalog copy + several dict-page
  reads per term per segment, and `bm25_scan_snapshot`'s format-version
  gate can `ereport(ERROR)` at plan time. (3) `df` is per-segment and the
  pending list has no dict at all, so the number would be structurally
  incomplete regardless of implementation effort. (4) A wildcard term
  walks the entire dict chain per segment — O(vocabulary × segments)
  inside the planner. If per-term `df` is ever genuinely wanted, the right
  home is `bm25_costestimate` (per-index-path, already expected to touch
  the index, can reach a `Const` via `path->indexclauses`) — a later
  refinement that still would not help correlated-`Var` queries.
- **`RESTRICT = matchingsel`** — rejected because with a `Const` RHS,
  `generic_restriction_selectivity` executes the operator's own procedure
  (`bm25_match`: re-analyze the query and tokenize the entire document)
  against the column's MCV/histogram samples (~110 documents) at plan
  time, and behaves asymmetrically under prepared statements: it fires on
  custom plans (`Param`s fold to `Const`s) but never on generic plans.
- **Reworking `bm25_costestimate`** — out of scope by user decision. Its
  `*sel = 0.05` and `*su = 1.0` remain known stubs awaiting a future M6
  cost pass; the startup-cost coupling constraint above is binding on that
  future work.

## Consequences

`@@@` now carries real planner selectivity and evaluation cost on both
`(text,text)` and `(text,jsonb)` variants: ranked queries choose the index
path on realistic corpora and query shapes without an `enable_seqscan`
workaround, and the margin (roughly 300x at the tested scale) is wide
enough that no ordinary GUC setting inverts it. No on-disk format change,
no C-visible ABI change beyond the two new selectivity functions and the
catalog `ALTER`s in the upgrade script.

That on-disk interchangeability does not extend to catalog/binary skew
across nodes: standby binaries must be upgraded before the primary runs
`ALTER EXTENSION ... UPDATE`, because `bm25_matchsel`/`bm25_matchjoinsel`
are resolved at plan time for every `@@@` query -- a standby whose binary
lags the primary's `UPDATE` fails all `@@@` planning once the catalog
change arrives via WAL, not merely calls to the two new functions.

What remains open, deliberately: the two selectivity paths
(`bm25_matchsel`'s `oprrest` and `bm25_costestimate`'s `*sel`) are not
structurally unified, only coincidentally equal — a future change to one
will not propagate to the other, and a reader touching either should
check both. `bm25_costestimate`'s `*su = 1.0` stub is still dishonest; the
startup-cost coupling constraint recorded above binds whoever eventually
fixes it. Neither constant is claimed to be *correct* — both are
deliberately blind to the query, chosen only to avoid the specific,
measured failure mode (Limit-proration collapse under an under-`LIMIT`
row estimate) without introducing plan-time I/O, ERROR risk, or
prepared-statement asymmetry. `sql/56_planner_estimates.sql` is the
suite that pins this behavior without any `enable_seqscan` crutch;
`sql/10_order_by.sql` and `sql/53_concurrent_scored_scans.sql` keep their
`enable_seqscan = off` settings for scan-path determinism (asserting plan
shape and ranked output, not the planner's unaided choice) — verifying
that the planner picks the index path unaided at default settings is
suite 56's job, not theirs.

## Addendum (2026-10-05, PRs #331-#350)

The rollout note above ("standby binaries must be upgraded before the primary runs
`ALTER EXTENSION ... UPDATE`") assumes an update script. Pre-release there is none: the 1.0
script is edited in place and an SQL change is picked up by drop-and-recreate. The note
applies once update scripts exist (ADR 0009's addendum of this date, #311 CI-05).
