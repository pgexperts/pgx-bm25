#!/usr/bin/env bash
# bench/index_build.sh -- Task 3 report-only benchmark: wall-clock time to
# CREATE INDEX ... USING bm25_native on the wand-bench two-field corpus, at growing
# document counts (10k/100k/1M), to track index-build scaling across commits.
#
# Report-only per project policy (README.md / .github/workflows/ci.yml): this
# script's numbers never gate a build. They exist to spot a PERSISTENT trend
# across commits, not to pass/fail a single noisy run -- a throttled/shared VM
# can swing an allocation-heavy run 2x+ on byte-identical code, so eyeball the
# CSV over several runs/commits rather than reacting to any one number.
#
# CREATE INDEX has no EXPLAIN to parse (unlike wand_vs_exhaustive.sh, which
# reads EXPLAIN ANALYZE's "Execution Time:"), so timing here wraps the whole
# psql invocation in epoch-millisecond timestamps instead. macOS's date(1) has
# no sub-second precision (no %N, unlike GNU date), so a plain `date +%s`
# can't give millisecond resolution portably; python3's stdlib time.time()
# covers it in one line without adding a dependency.
#
# Usage:
#   PGHOST=... PGPORT=... ./bench/index_build.sh [reps=3]
# Requires bm25_native already built+installed into the target PostgreSQL
# (as `make installcheck` does for the regression suite) and a database
# reachable via the standard PG* libpq environment variables. The 1M-doc cell
# takes minutes -- that is expected, not a hang.
set -euo pipefail

REPS="${1:-3}"
DB="bm25_bench_$$"

psql -d postgres -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"$DB\";" >/dev/null
trap 'psql -d postgres -c "DROP DATABASE IF EXISTS \"$DB\";" >/dev/null' EXIT

psql -d "$DB" -v ON_ERROR_STOP=1 -c "CREATE EXTENSION bm25_native;" >/dev/null

now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }

# One CREATE INDEX timing: minimum of $REPS from-scratch builds (min, not
# mean -- damps scheduler/cache noise upward-only, same rationale as
# wand_vs_exhaustive.sh). DROP INDEX IF EXISTS is untimed and runs before
# every rep (a no-op on the first) so each rep times a real build, never an
# incremental one. Timestamps are already whole milliseconds, so a plain
# bash integer compare suffices here (no need for the awk float-compare
# trick the EXPLAIN-based benches use for fractional "Execution Time" values).
build_index() {
  local best="" start end ms
  for _ in $(seq 1 "$REPS"); do
    psql -d "$DB" -v ON_ERROR_STOP=1 -c "DROP INDEX IF EXISTS docs_bm25;" >/dev/null
    start=$(now_ms)
    psql -d "$DB" -v ON_ERROR_STOP=1 -c \
      "CREATE INDEX docs_bm25 ON docs USING bm25_native (title, body) INCLUDE (id) WITH (key_field='id');" >/dev/null
    end=$(now_ms)
    ms=$((end - start))
    if [[ -z "$best" ]] || (( ms < best )); then best="$ms"; fi
  done
  echo "$best"
}

echo "# scales=10000,100000,1000000 reps=$REPS pg_version=$(psql -d "$DB" -Atc 'SHOW server_version;')"
echo "ndocs,ms"
for ndocs in 10000 100000 1000000; do
  # Same title/body vocabulary as bench/wand_vs_exhaustive.sh, scaled to
  # $ndocs -- built once per scale, then reused (index dropped/rebuilt only)
  # across the $REPS timing runs above.
  psql -d "$DB" -v ON_ERROR_STOP=1 -v ndocs="$ndocs" -q <<'SQL' >/dev/null
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id int PRIMARY KEY, title text, body text)
    WITH (autovacuum_enabled = off);
INSERT INTO docs SELECT g,
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
SQL
  echo "$ndocs,$(build_index)"
done
