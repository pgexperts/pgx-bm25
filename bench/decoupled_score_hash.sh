#!/usr/bin/env bash
# bench/decoupled_score_hash.sh -- Task 4 report-only benchmark: total wall time
# to project bm25_score_key(id) for EVERY row of a decoupled (materialized CTE)
# ranked result, across growing corpus/result sizes.
#
# Before Task 4, this projection path (single active scan, no current-row
# match -- see bm25_resolve_score_key's fallback in src/bm25_score.c) was a
# linear scan of the ranking per lookup, so projecting N rows cost O(N^2)
# total. Task 4 replaces that fallback with an O(1) hash probe (lazily built
# once per scan-load), making the same total projection O(N). This script
# does not diff against the pre-Task-4 binary; it just shows the CURRENT
# scaling is linear, not quadratic, as the corpus grows -- report-only per
# project policy (README.md / .github/workflows/ci.yml), never gating.
#
# Usage:
#   PGHOST=... PGPORT=... ./bench/decoupled_score_hash.sh [reps=3]
# Requires bm25_native already built+installed (as `make installcheck` does)
# and a database reachable via the standard PG* libpq environment variables.
set -euo pipefail

REPS="${1:-3}"
DB="bm25_bench_$$"

psql -d postgres -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"$DB\";" >/dev/null
trap 'psql -d postgres -c "DROP DATABASE IF EXISTS \"$DB\";" >/dev/null' EXIT

psql -d "$DB" -v ON_ERROR_STOP=1 -q <<'SQL' >/dev/null
CREATE EXTENSION bm25_native;
CREATE TABLE css(id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO css SELECT g, 'foo bar ' || repeat('pad ', g % 11)
FROM generate_series(1, 100000) g;
CREATE INDEX css_bm25 ON css USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id');
SELECT bm25_seal('css_bm25');
SQL

# One (n) timing: minimum of $REPS EXPLAIN ANALYZE runs of the decoupled
# projection over the top N ranked rows (min, not mean -- damps noise
# upward-only; same rationale as bench/wand_vs_exhaustive.sh).
time_query() {
  local n="$1" best="" ms
  for _ in $(seq 1 "$REPS"); do
    ms=$(psql -d "$DB" -v ON_ERROR_STOP=1 -Atq <<SQL | grep '^Execution Time:' | awk '{print $3}'
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, COSTS OFF)
WITH ranked AS MATERIALIZED (
  SELECT id FROM css WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' LIMIT $n
)
SELECT count(*) FROM (SELECT id, bm25_score_key(id) AS s FROM ranked) k;
SQL
)
    if [[ -z "$best" ]] || awk "BEGIN{exit !($ms < $best)}"; then best="$ms"; fi
  done
  echo "$best"
}

echo "# reps=$REPS pg_version=$(psql -d "$DB" -Atc 'SHOW server_version;')"
echo "n,ms"
for n in 1000 4000 16000 64000; do
  echo "$n,$(time_query "$n")"
done
