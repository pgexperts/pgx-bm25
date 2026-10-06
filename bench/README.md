# Benchmarks (M2+)

Report-only, non-gating per project policy.  Perf/benchmark steps are
non-blocking on CI; only correctness (build, regression, TAP) gates the
pipeline.  Benchmark baselines are for trend-tracking, not pass/fail; gate
on perf only on consistent/dedicated hardware, never the shared CI pool.

## Landed: WAND vs exhaustive (M2b)

`wand_vs_exhaustive.sh` measures ranked top-N latency with block-max WAND
(`bm25_native.wand_top_k=100`, the shipped default) against the exhaustive scan
(`=0`), across `LIMIT` &isin; {10, 100, 1000} and 1–4 query terms, on a
realistic multi-field BM25F corpus (title boost 5.0 / body boost 1.0 — the same
field shape as `sql/45_m2b_acceptance.sql`, scaled up). Each cell is the
minimum of several `EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON)` runs (minimum,
not mean, to damp noise upward-only) against a throwaway database that the
script creates and drops.

```bash
PGHOST=... PGPORT=... ./bench/wand_vs_exhaustive.sh [ndocs=100000] [reps=3]
```

Output is CSV (`mode,wand_top_k,limit,terms,query,ms`) on stdout, preceded by
a `#`-prefixed metadata line (corpus size, reps, server version) — redirect
it into `bench/baselines/` to update the recorded baseline.

**Observed shape** (100k docs, PG 18, local run — see
`bench/baselines/wand_vs_exhaustive.csv`): WAND wins decisively while
`LIMIT <= wand_top_k` (roughly 2–5x faster here). Above that threshold the
gettuple tail-fallback (§ARCHITECTURE.md) re-runs the exhaustive scan on top
of the already-completed WAND build, so total latency at `LIMIT=1000`
(`wand_top_k=100`) runs *slower* than a plain exhaustive scan from the start —
expected, not a regression: it is the price of guaranteeing exact results when
the caller asks for more rows than the capped build produced, and callers who
want `LIMIT`s consistently above 100 should raise `bm25_native.wand_top_k` to match.

## Landed: decoupled score hash (Task 4)

`decoupled_score_hash.sh` times the total wall clock to project
`bm25_score_key(id)` for every row of a decoupled (materialized CTE) ranked
result, at growing result sizes (1k/4k/16k/64k rows out of a 100k-doc corpus).
Before Task 4 this fallback path (single active scan, no current-row match)
resolved each lookup with a linear scan of the ranking, so projecting N rows
cost O(N^2) total; Task 4 replaces it with a lazily-built per-scan hash, an
O(1) probe per lookup (O(N) total).

```bash
PGHOST=... PGPORT=... ./bench/decoupled_score_hash.sh [reps=3]
```

Output is CSV (`n,ms`) on stdout, preceded by a `#`-prefixed metadata line —
redirect it into `bench/baselines/` to update the recorded baseline.

**Observed shape** (100k-doc corpus, PG 18, local run — see
`bench/baselines/decoupled_score_hash.csv`): total projection time stays
essentially flat (~170-180ms) as N grows 64x (1000 -> 64000 rows), consistent
with O(1) per-lookup scaling; a linear-scan fallback would show that total
growing with N (and, across repeated single-row lookups against the same
ranking, quadratically).

## Landed: index build time

`index_build.sh` times `CREATE INDEX ... USING bm25_native (title, body) INCLUDE (id)
WITH (key_field='id')` on a two-field `generate_series` corpus at 10k, 100k,
and 1M documents. Per scale, the table is built once; then `reps` (default 3)
rounds of `DROP INDEX IF EXISTS` + timed `CREATE INDEX` run and the minimum
wall clock is kept (`CREATE INDEX` has no `EXPLAIN` output to parse, unlike
the other two scripts, so timing wraps the whole `psql` invocation in
epoch-millisecond timestamps instead).

```bash
PGHOST=... PGPORT=... ./bench/index_build.sh [reps=3]
```

Output is CSV (`ndocs,ms`) on stdout, preceded by a `#`-prefixed metadata line
(scales, reps, server version) — redirect it into `bench/baselines/` to update
the recorded baseline.

**Observed shape** (dev server, PG 18.3, minimum of 3 reps — see
`bench/baselines/index_build.csv`):

```
ndocs,ms
10000,53
100000,328
1000000,3106
```

Roughly linear, slightly sublinear between 10k and 100k (6.2x time for 10x
docs — plausibly per-invocation connection/startup overhead being a larger
fraction of the smaller total) before settling closer to linear between 100k
and 1M (9.5x time for 10x docs). Not a red flag: a report-only trend baseline,
not a regression gate.

## Landed: WAND buffer profile (QRY-03 / #229)

`wand_global_ub.sh` reports where a WAND build's buffer accesses go, on the
corpus `docs/adr/0096-keep-the-open-time-global-ub-sweep.md` measured with: two
fields (title 3-8 tokens, boost 3.0; body 20-300, boost 1.0), term ranks drawn
log-uniformly over w1..w4999, inserted into an index created empty and sealed
from the pending list (53 segments at the default 100k documents). An optional
third argument, `merged`, runs `bm25_merge` after the seal under a 1GB
`maintenance_work_mem`, which leaves the 100k-document corpus in two segments
(60,751 and 39,249 documents), so the dense per-document chains span many pages.
For each of ten queries at `LIMIT` &isin; {10, 100, 1000}, with
`bm25_native.wand_top_k` set to the `LIMIT` so the rows are different builds, it
records the ranked query's
latency and buffer accesses, one isolated WAND build's buffer accesses (a
`bm25_wand_stats` call under `EXPLAIN (ANALYZE, BUFFERS)`), the open-time
`global_ub` sweep's exact cost (one access per block of every query term's run
in every segment), all four `bm25_wand_stats` counters, and build accesses per
scored (term, document) pair.

```bash
PGHOST=... PGPORT=... ./bench/wand_global_ub.sh [ndocs=100000] [reps=3] [segments|merged]
```

Output is CSV (`limit,terms,query,ms,query_blks,build_blks,sweep_blks,
blocks_examined,blocks_skipped,docs_scored,deep_check_skips,blks_per_pair`) on
stdout, preceded by a `#`-prefixed metadata line (corpus size, reps, layout,
segment count, largest segment's document count, server version). Needs a role
that may call the `bm25_debug_*` functions. The buffer columns are
deterministic -- hit + read counts `ReadBuffer` calls, whatever is cached -- so
they compare across machines and
build types in a way `ms` does not.

The script began as ADR 0096's stored-vs-swept comparison. The stored side
(a format-v9 term-level impact table) was rejected and never landed; this
version keeps the corpus and queries and reports the swept build alone.

**Observed shape before ADR 0100** (100k docs, 53 segments, PG 18.6 local
cassert build, so only the buffer columns are meaningful): the sweep was under 1%
of every frequent-term build (`w1`, `LIMIT` 10: 1,093 of 372,819 accesses). The
eight ordinary queries cost 4.1-7.8 accesses per scored pair, and `w1 w3000` cost
40 at `LIMIT` 10, where the multi-field df pass walked `w1`'s whole run for 3,953
scored pairs. The main cost was LIVEDOCS reads against segments with no
tombstones. ADR 0100 brought the eight to 1.5-2.4 per pair and `w1 w3000` to 3.3,
and the rare term `w4000` from 13.1 to 9.5.

**Observed shape now** (re-measured for #274 on 82d7b97, after #246 items 1, 3
and 4; same corpus, 3 reps, same cassert build). Many-segment layout at `LIMIT`
10: the eight ordinary queries cost 1.49-2.43 accesses per scored pair (`w1`:
129,949 accesses for 53,476 pairs), `w1 w3000` 3.12, and `w4000` 6.79 (1,746
accesses for 257 pairs). The sweep is 0.7-2.1% of the eight builds and 9.3% of
`w1 w3000`'s, where WAND skips 373 blocks. At `LIMIT` 100 and 1000 the eight stay
at 1.50-2.41 per pair. At `LIMIT` 1000 the ranked query's accesses exceed the
isolated build's by 969, consistent with one heap fetch per returned row (the
difference is 69 at `LIMIT` 100 and -21 at `LIMIT` 10).

The merged layout separates per-segment from per-pair costs. The eight ordinary
queries cost the same per pair in both layouts, within 0.12 in every cell (`w1`,
`LIMIT` 10: 2.42 merged, 2.43 many-segment), so that cost is per pair. ADR 0100
attributes it mostly to one NORMS access per field posting and one DOCMAP access
per candidate, the reads #267 targets. `w4000` drops from 6.79 to 2.64 per
pair at `LIMIT` 10 (1,260 accesses for 478 pairs over 2 segments, against 1,746
for 257 over 53), so on the many-segment layout a rare term is dominated by fixed
costs per segment. At `LIMIT` 1000 on the merged layout it costs 3.60 per pair.

**After #267** (chain page images; same corpus, reps and build). A NORMS, DOCMAP
or KEYMAP lookup on the page the previous one landed on no longer costs a buffer
access. Many-segment layout at `LIMIT` 10: the eight ordinary queries cost
0.03-0.23 accesses per scored pair (`w1`: 3,365 accesses for 53,476 pairs), `w1
w3000` 0.95 and `w4000` 4.98; at `LIMIT` 1000 the eight cost 0.03-0.08. The
open-time sweep, under 1% of every frequent-term build when ADR 0096 kept it, is
now 15-36% of the eight builds at `LIMIT` 10 (`w1`: 1,093 of 3,365), and up to 43%
on the merged layout, so it is again a cost worth measuring. On the merged layout
`w4000` costs 0.63 per pair at `LIMIT` 10 and 0.61 at `LIMIT` 1000.

No `ms` baseline is recorded yet: the local build is cassert, and a baseline
needs an optimized build on real hardware.

## Ranking quality (NDCG): not here, by design

Ranking-quality regression testing lives in `sql/57_ranking_quality.sql`
(gated CI suite over vendored BEIR SciFact) and `docs/adr/0011-ranking-quality-benchmarks.md`
(corpus/licensing, threshold derivation, the real measured numbers), not in
`bench/` — NDCG on a fixed corpus has no timing component, so a regression in
it is a correctness bug rather than the kind of noisy wall-clock measurement
this directory's report-only policy exists to protect CI from.

## Baselines

Stored in `bench/baselines/` and updated by hand from a **local run on real
hardware** (as the "Observed shape" notes above cite -- "dev server, PG
18.3", "local run"). Nothing diffs a run's numbers against these files
automatically; there is no comparison step, and no failure a shift in either
direction could trigger. Update a baseline only after a persistent multi-run
shift observed locally, never from a single run and never from CI's own
numbers (see below).

## CI coverage

A report-only `bench` job in `.github/workflows/ci.yml` runs all four
scripts above on every CI run. CI is triggered manually (`gh workflow run CI --ref main`)
once a block of work has landed; `docs/adr/0070-ci-triggers-and-latency-suites.md` records the
earlier push/PR trigger,
against a plain PostgreSQL 18 built from the PGDG apt package -- `continue-on-error: true` at job level, so it can never
fail a build, and nothing depends on it. It exists purely so the CSVs can be
eyeballed for a trend across many runs; a shared, noisy CI runner is not
where a single trustworthy number comes from (a throttled or noisy-neighbor
VM can swing an allocation-heavy run 2x+ on byte-identical code), so its
output is uploaded as a build artifact, never written into `bench/baselines/`
and never compared against those files.

To keep the job cheap, it scales each script down from its own defaults via
each script's own CLI arguments: `wand_vs_exhaustive.sh` and
`wand_global_ub.sh` run at 2,000 docs x 1 rep (vs. the 100k x 3 default) and
`decoupled_score_hash.sh` /
`index_build.sh` at 1 rep (their corpus sizes and scale sweep are fixed in
the scripts, so `index_build.sh`'s 1M-doc cell still runs once per CI run).
None of that is a trustworthy performance number by itself -- it is a
much smaller corpus on a shared runner, run once. A real, citable number
for the "Observed shape" notes above, or for updating a baseline, still
means running these scripts locally at their own defaults, as documented
per script above.
