# ROADMAP

`bm25_native` is feature-complete as of its 1.0 release. The landed engine and
query surface — configurable analyzer, multi-field BM25F with per-field boosts
and a `key_field`, stored positions (phrase / proximity / snippets), the
boolean + wildcard jsonb query API, live `k1`/`b`/`boost` reloptions, tiered
segment merge with page reclamation, the forward/backward on-disk format
compatibility contract with online `bm25_upgrade`, and concurrency-correct score
accessors — are documented in [ARCHITECTURE.md](ARCHITECTURE.md),
[THEORY.md](THEORY.md), and the decision records under [docs/adr/](docs/adr/).

This file tracks only work that has **not** landed.

---

## Parallel scan (optional)

Implement `amcanparallel` so a ranked or match scan can be divided across
parallel workers. The engine is single-worker today (`amcanparallel = false`),
which is correct and sufficient for current use — this is a throughput
optimization, not a correctness gap. A candidate once a scan-bound workload
shows the ranked top-N path as the bottleneck; until then it stays deferred to
avoid the shared-memory scan-state complexity it would add.

## Competitive performance benchmark (deferred)

A head-to-head performance comparison against an established BM25 extension
(ParadeDB `pg_search`) — index build time, query latency, and index size on a
shared corpus. Deferred rather than dropped: it needs a practical side-by-side
deployment of the comparison target, which does not yet exist on the
development machine.

The correctness-and-quality half of the benchmark work has already landed and
gates CI: `sql/57_ranking_quality.sql` measures NDCG@10 against the vendored
BEIR SciFact corpus, and build-time measurement (`bench/index_build.sh`) runs
report-only. The groundwork for the head-to-head — packaging, the
`shared_preload_libraries` requirement, and licensing — is recorded in
[docs/adr/0011-ranking-quality-benchmarks.md](docs/adr/0011-ranking-quality-benchmarks.md)
so the deferral stays actionable. Any head-to-head numbers would be report-only,
never a CI gate, per the project's policy of keeping wall-clock measurements off
shared runners.

---

*Landed milestones and their acceptance criteria live in the git history, the
ADRs, and `ARCHITECTURE.md`; this file lists only remaining, unlanded work.*
