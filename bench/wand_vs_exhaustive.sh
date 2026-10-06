#!/usr/bin/env bash
# bench/wand_vs_exhaustive.sh -- M2b report-only benchmark: ranked top-N latency,
# WAND (bm25_native.wand_top_k=100, the shipped default) vs exhaustive (=0), across a
# spread of LIMITs and query term counts, on a sizable multi-field BM25F corpus.
#
# Report-only per project policy (README.md / .github/workflows/ci.yml): this
# script's numbers never gate a build. They exist to spot a PERSISTENT trend
# across commits, not to pass/fail a single noisy run -- a throttled/shared VM
# can swing an allocation-heavy run 2x+ on byte-identical code, so eyeball the
# CSV over several runs/commits rather than reacting to any one number.
#
# EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) is used instead of psql \timing: the
# printed "Execution Time" is the whole-plan wall clock regardless of the TIMING
# option (TIMING only controls whether EVERY node also gets its own per-node
# actual-time instrumentation, which adds measurable overhead of its own and
# would bias exactly the WAND-vs-exhaustive comparison this bench exists to make).
#
# Usage:
#   PGHOST=... PGPORT=... ./bench/wand_vs_exhaustive.sh [ndocs] [reps]
# Requires bm25_native already built+installed into the target PostgreSQL
# (as `make installcheck` does for the regression suite) and a database
# reachable via the standard PG* libpq environment variables.
set -euo pipefail

NDOCS="${1:-100000}"
REPS="${2:-3}"
DB="bm25_bench_$$"

psql -d postgres -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"$DB\";" >/dev/null
trap 'psql -d postgres -c "DROP DATABASE IF EXISTS \"$DB\";" >/dev/null' EXIT

# The corpus: same realistic multi-field title(boost 5.0)/body(boost 1.0) fields as
# sql/45_m2b_acceptance.sql, scaled up to $NDOCS docs and a 5-term vocabulary so
# every LIMIT/term-count combination below has real recall to rank over.
psql -d "$DB" -v ON_ERROR_STOP=1 -v ndocs="$NDOCS" -q <<'SQL' >/dev/null
CREATE EXTENSION bm25_native;
CREATE TABLE briefs (id int PRIMARY KEY, title text, body text)
    WITH (autovacuum_enabled = off);
INSERT INTO briefs SELECT g,
  concat_ws(' ', CASE WHEN g % 40 = 0 THEN 'negligence' END,
                 CASE WHEN g % 25 = 0 THEN 'liability' END,
                 CASE WHEN g % 10 = 0 THEN 'contract' END, 'brief'),
  concat_ws(' ', CASE WHEN g % 4  = 0 THEN 'liability' END,
                 CASE WHEN g % 15 = 0 THEN 'negligence' END,
                 CASE WHEN g % 3  = 0 THEN 'damages' END,
                 CASE WHEN g % 6  = 0 THEN 'equity' END,
                 CASE WHEN g % 10 = 0 THEN 'contract' END,
                 repeat('pad ', g % 11))
FROM generate_series(1, :ndocs) g;
CREATE INDEX briefs_bm25 ON briefs USING bm25_native (title, body) INCLUDE (id)
    WITH (key_field = 'id', boost_title = '5.0', boost_body = '1.0');
SELECT bm25_seal('briefs_bm25');
SQL

# One (mode, limit, query) timing: minimum of $REPS EXPLAIN ANALYZE runs (min,
# not mean -- damps scheduler/cache noise upward-only, same rationale as any
# microbenchmark harness: a run can only be slowed by outside interference,
# never spuriously sped up).
time_query() {
  local k="$1" limit_n="$2" query="$3" best="" ms
  for _ in $(seq 1 "$REPS"); do
    ms=$(psql -d "$DB" -v ON_ERROR_STOP=1 -Atq <<SQL | grep '^Execution Time:' | awk '{print $3}'
SET enable_seqscan = off;
SET bm25_native.wand_top_k = $k;
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, COSTS OFF)
SELECT id FROM briefs WHERE title @@@ '$query'
  ORDER BY title &@@ '$query' LIMIT $limit_n;
SQL
)
    if [[ -z "$best" ]] || awk "BEGIN{exit !($ms < $best)}"; then best="$ms"; fi
  done
  echo "$best"
}

echo "# ndocs=$NDOCS reps=$REPS pg_version=$(psql -d "$DB" -Atc 'SHOW server_version;')"
echo "mode,wand_top_k,limit,terms,query,ms"
for mode_k in "wand:100" "exhaustive:0"; do
  mode="${mode_k%%:*}"
  k="${mode_k##*:}"
  for limit_n in 10 100 1000; do
    for query in "liability" "negligence liability" "negligence liability damages" \
                 "negligence liability damages contract"; do
      terms=$(awk '{print NF}' <<<"$query")
      ms=$(time_query "$k" "$limit_n" "$query")
      echo "$mode,$k,$limit_n,$terms,\"$query\",$ms"
    done
  done
done
