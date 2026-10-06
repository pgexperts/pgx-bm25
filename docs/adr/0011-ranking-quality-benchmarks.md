---
id: 0011
title: Gate ranking quality on vendored BEIR SciFact; defer the pg_search head-to-head
date: 2026-07-16
status: Accepted
summary: Vendor BEIR SciFact byte-for-byte and gate CI on two NDCG@10 thresholds (multifield >= 0.64, flat >= 0.66) derived from a real-index honesty run, because ranking quality has no timing component and is a correctness property; the ParadeDB head-to-head is deferred, not dropped, pending a practical local deployment path.
---

# 0011. Gate ranking quality on vendored BEIR SciFact; defer the pg_search head-to-head

## Context

The ROADMAP's "Benchmarks vs `pg_search`" cross-cutting item asked for three
comparisons: index build time, query latency, and ranking quality (NDCG).
Build time and query latency are wall-clock measurements and slot cleanly
into the project's existing report-only benchmark policy — this repo's own
`bench/README.md` states it plainly: "Report-only, non-gating per project
policy. Perf/benchmark steps are non-blocking on CI; only correctness
(build, regression, TAP) gates the pipeline," because "a one-off spike on
shared CI runners (noisy-neighbor VMs can vary 2x+ on identical code)" must
not fail a build. That reasoning is entirely about *timing noise* on shared
hardware. NDCG@10 on a fixed, vendored corpus has no timing component: it is
a pure function of index contents and scoring math, identical on the
noisiest CI VM in the fleet. A silent ranking-quality regression is a
correctness bug indistinguishable in kind from a wrong query result, so it
does not belong in the report-only bucket just because the word
"benchmark" is in the ROADMAP line that spawned it. This ADR records why the
suite (`sql/57_ranking_quality.sql`) gates CI while `bench/index_build.sh`
(the third arm, build time) stays report-only in `bench/`.

**Corpus and licensing.** The suite is measured against BEIR SciFact,
vendored byte-for-byte as `data/scifact_corpus.data` (5,183 docs),
`data/scifact_queries.data` (1,109 lines — see the query-count finding
below), and `data/scifact_qrels.data` (339 judgment rows over 300 distinct
judged query ids, all binary relevance). Licensing is attribution-only and
clean for vendoring: claims/queries/qrels are CC BY 4.0 (Wadden et al.,
AllenAI); the underlying paper abstracts are ODC-By 1.0 (S2ORC, Allen
Institute for AI). The three files are committed unmodified from BEIR's
published zip (md5 `5f7d1de60b170fc8027bb7898e2efca1`, verified by `cmp`
against the extracted upstream files before commit) rather than a
pre-flattened or trimmed derivative, so the byte-identity claim in
`data/README.md` stays checkable forever instead of needing re-justification
each time someone asks "is this really the official corpus." One of the
three files (`scifact_qrels.data`) is genuinely CRLF-terminated in the
upstream release; this repo's `core.autocrlf=input` would silently
normalize it to LF on commit, breaking that same byte-identity check on
every future clone. `.gitattributes` (`data/*.data -text`) disables that
normalization for the vendored directory.

**The query-count finding.** The raw vendored `queries.jsonl` bundles both
BEIR splits in one file — 1,109 lines total (809 train + 300 test) — not
the 300 an early plan draft assumed. Only `qrels/test.tsv` scopes the
canonical 300-query test set (339 judgment rows across those 300 ids); the
suite loads all 1,109 raw queries into a staging table (so a truncated or
mis-renamed vendor file still fails loudly on the raw count) and then
filters to `qid IN (SELECT DISTINCT query_id FROM qrels)` before running the
ranked fan-out — running the other 809 would cost roughly 3.7x the query
time for zero effect on the NDCG mean, since `ndcg10()`'s join to `qrels`
already ignores unjudged queries.

**The honesty run.** Before any threshold was frozen, the real C scan path
(`docs`/`docs_flat` BM25 indexes, ordered Index Scan via `@@@`/`&@@`) was run
end-to-end over the genuine corpus in a throwaway database, specifically to
check whether the analyzer + a hand-written formula transcription of the
scorer (0.6661 multifield / 0.6871 flat) agreed with what the index itself
actually produces:

| Config | Formula transcription | Real measured (full precision) |
|---|---|---|
| multifield | 0.6661 | `0.66605412296643666992` |
| flat | 0.6871 | `0.68712589012291646837` |

Both agree with their transcription to the 4th decimal place (0.6661 vs.
0.666054…, 0.6871 vs. 0.687126…) — reproduced identically on a second,
independent run in a fresh session. This is the strongest evidence this
project has that the BM25 scorer is implemented correctly: two independently
derived computations of the same quantity — one via the real index's C scan
path, one via a formula transcription of the scoring math applied to
analyzer output — landed within four decimal places of each other on a
5,183-document, 300-query real-world corpus, not a synthetic 3-row unit
test.

**Comparison to Anserini.** The BEIR paper's published SciFact baseline of
"~0.665" is Anserini/Lucene's multifield BM25 run, not Elasticsearch (a
provenance correction against an earlier draft's assumption — Anserini
publishes 0.6647 multifield / 0.6789 flat). Our real measured numbers
(0.666054 / 0.687126) land essentially on par with Anserini's on both
configurations, using our own k1/b defaults (1.2/0.75) rather than
Anserini's (k1=0.9/b=0.4) — see the Decision section on why we did not
adopt Anserini's tuning.

## Decision

We will gate CI on `sql/57_ranking_quality.sql` with two independent
boolean thresholds, derived from the honesty-run numbers above with roughly
two points of headroom each:

- **Multifield** (`USING bm25 (title, body)`, title first — the ordered
  scan anchors on the first indexed column): **NDCG@10 >= 0.64** (measured
  0.666054).
- **Flat** (one concatenated column): **NDCG@10 >= 0.66** (measured
  0.687126).

Two points of headroom absorbs avgdl/IDF drift and float-rounding noise
while still failing on any real ranking regression. Neither gate is paired
with a rounded literal canary (e.g. "NDCG rounds to 0.67"): both measured
values sit inside the ±0.005 half-cell around their nearest rounding
boundary (0.666054 is 0.00105 from 0.665; 0.687126 is 0.00213 from 0.685),
so a boundary-straddling literal would flake on a 1-ULP cross-arch score
difference without indicating an actual regression. The suite instead pins
a gap canary (`flat_exceeds_multifield`) — flat outscoring multifield on
SciFact is a genuine, previously-measured property of this corpus (both
Anserini's baselines and our own honesty run agree it isn't an artifact),
so collapsing or inverting that gap is caught even if both boolean gates
still pass.

We keep our k1/b defaults (1.2/0.75) rather than adopting Anserini's tuning
(k1=0.9/b=0.4) — the suite regression-tests *our* defaults, which measure
higher than Anserini's own numbers on this corpus, not a copy of Anserini's
configuration. We do not boost the title field: boosting was measured to
monotonically destroy quality on this dataset (boost 2.0 -> 0.6312, boost
3.0 -> 0.5996, both well below the unboosted 0.666054), so multifield ships
unboosted.

The ParadeDB head-to-head (the third arm of the original ROADMAP ask) is
**deferred, not dropped**, because deploying `pg_search` on the local dev
machine is currently impractical. Groundwork is recorded here so the
deferral stays actionable rather than becoming a stale TODO:

- ParadeDB ships a prebuilt `.deb` per release
  (`postgresql-18-pg-search_0.24.2-1PARADEDB-noble_amd64.deb`) — no Rust
  toolchain or build step needed — and a `paradedb/paradedb:latest-pg18`
  Docker image as an alternative path.
- `shared_preload_libraries = 'pg_search'` is mandatory; their own PG18
  benchmark harness sets it too, so any comparison harness must provision a
  cluster with that preload rather than installing into a running one.
- `pg_search` is AGPL-3.0 licensed. For a local, non-distributing benchmark
  harness that only queries a `pg_search`-equipped cluster and never ships
  or distributes `pg_search` itself, this is not expected to be a licensing
  obstacle. This is our own standard license analysis, not a carve-out
  ParadeDB has published — anyone relying on it for a distribution scenario
  should re-derive it for that scenario.
- Whichever mechanism is eventually chosen (a Docker Compose harness, a
  prebuilt-package VM, or a hosted instance), the comparison arm reads the
  same vendored SciFact corpus and qrels this ADR describes — no change to
  the harness shape above is anticipated to accommodate it.

## Alternatives considered

- **NFCorpus** — has graded (non-binary) qrels, which would exercise the
  gain function's non-binary path that SciFact's all-score-1 judgments
  never touch. Rejected: its license is academic-use-only, which is not
  clean for a vendored, publicly-committed file in this repository.
- **MS MARCO** — the standard large-scale IR benchmark. Rejected on size
  alone: 8.8M passages / ~2.9 GB is not something to vendor into a git repo
  or load in a regression suite's runtime budget.
- **TREC-COVID** — graded relevance (0/1/2) at realistic scale, but 171,332
  documents / ~70 MB download (figures from direct inspection of the BEIR
  distribution during the 2026-07-15 corpus survey for this decision:
  byte-exact content-length on the published zip). Vendoring it is repo-hostile
  and downloading it in CI conflicts with the suite's no-network requirement, so
  it was set aside with the same reasoning as MS MARCO, at smaller scale.
- **Report-only NDCG instead of gating** — the option that best matches the
  existing benchmark policy's letter, if not its intent. Rejected because it
  leaves a silent-regression window: a scorer or analyzer change that
  quietly degrades ranking quality would pass every existing membership and
  sequence assertion (none of which measure rank *quality*, only rank
  *presence* or *order* on synthetic fixtures) and only show up in a human
  glancing at a non-gating report, if anyone does. The determinism argument
  above (no timing component) is what makes gating safe to do without
  reintroducing the flakiness the report-only policy exists to avoid.
- **Running the ParadeDB head-to-head now** — rejected for this round only;
  see the Decision section's deferral and recorded groundwork.

## Consequences

- CI now fails loudly on a real ranking-quality regression on real data,
  closing the gap every prior suite left open (they assert presence/order
  on synthetic corpora, never quality on a real one).
- The suite carries a one-time absolute claim — our BM25 lands on
  Anserini's published SciFact baseline — and an ongoing regression pin;
  these two claims age differently. If a future, deliberate scoring change
  moves the real numbers, the threshold derivations in this ADR (not the
  suite's SQL comments) are the source to re-derive from, and this record
  should be superseded rather than edited in place.
- The "Benchmarks vs `pg_search`" ROADMAP item is now two-thirds landed
  (build time: report-only bench; ranking quality: gated suite) with the
  third arm (head-to-head) tracked here as an explicit, groundwork-recorded
  deferral rather than a vague future intention.
- Anyone re-vendoring or replacing the corpus must re-run the honesty-run
  methodology (real index measurement vs. transcription, boundary check,
  threshold re-derivation) rather than assuming the 0.64/0.66 gates still
  fit — they are derived from this specific corpus and these specific
  measured numbers, not first-principles targets.
