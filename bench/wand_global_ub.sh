#!/usr/bin/env bash
# bench/wand_global_ub.sh -- report-only benchmark of where a WAND build's buffer
# accesses go: the open-time global_ub sweep's share (QRY-03, issue #153, ADR 0096)
# and the per-scored-pair cost around it (issue #229, ADR 0100).
#
# History. ADR 0096 measured, with this script, WAND reading each cursor's global
# upper bound from a stored term-level impact table ("stored", a format-v9 feature)
# against computing it by the open-time block-header sweep ("swept"). The stored side
# was rejected and never landed, so this version keeps the corpus and the queries and
# drops the pairing: there is only the sweep. What it reports instead is the quantity
# both ADRs turned on -- how many buffer accesses a WAND build costs, how many of them
# the sweep is, and how many per scored (term, document) pair.
#
# Columns, one row per (LIMIT, query):
#   ms            minimum of $REPS EXPLAIN ANALYZE runs of the ranked query (min, not
#                 mean: outside interference only ever slows a run down).
#   query_blks    shared hit + read of the ranked query's top plan node: the WAND
#                 build plus key projection plus the executor's heap fetches.
#   build_blks    shared hit + read of one bm25_wand_stats call, i.e. the WAND build
#                 alone (bm25_wand_build_ranking, prologue included, no heap).
#   sweep_blks    the sweep's exact cost: one buffer access per block of every query
#                 term's posting run in every segment (ADR 0096 measured it exactly),
#                 counted from bm25_debug_block_impacts rather than inferred.
#   blocks_examined / blocks_skipped / docs_scored / deep_check_skips
#                 bm25_wand_stats' counters. Report all four with any comparison: a
#                 query that never prunes (blocks_skipped = 0) says nothing about the
#                 skip path, and a change that alters docs_scored is not the same
#                 change as one that alters only the cost per pair.
#   blks_per_pair build_blks / docs_scored.
#
# bm25_native.wand_top_k is set to the row's LIMIT, so the LIMIT column changes the
# build (the default of 100 would make LIMIT 10 and LIMIT 100 the same build).
#
# The corpus is Zipf-shaped on purpose: log-uniform term ranks (w1..w4999) and
# per-document lengths spread over 20..300 tokens, two fields. Documents go into an
# index created EMPTY and are sealed from the pending list, so the index has many
# segments -- which is what makes per-segment costs (the sweep, the dictionary walk,
# the df pass) show. (At the CI's 2,000 documents it has only one or two.)
#
# Layout. The default, `segments`, is the many-segment index above. `merged` runs
# bm25_merge after the seal, which drains the index toward its merge target: at the
# default 100k documents that leaves two segments (60,751 and 39,249 documents),
# whose NORMS, DOCMAP and KEYMAP chains span several pages each. That is similar to
# ADR 0100's merged k = 1000 cells (there the index was built, then merged), where
# per-row costs inside a segment (key projection, per-lookup chain reads) show and
# per-segment costs do not. Issues #267 and #274 need both layouts; the metadata
# line names the layout and the largest segment's document count.
#
# Report-only per project policy (bench/README.md): nothing here gates a build.
#
# Usage:
#   PGHOST=... PGPORT=... ./bench/wand_global_ub.sh [ndocs] [reps] [segments|merged]
# Requires bm25_native installed into (or reachable by) the target server and a
# role allowed to call the bm25_debug_* functions (a superuser, as in CI).
set -euo pipefail

NDOCS="${1:-100000}"
REPS="${2:-3}"
LAYOUT="${3:-segments}"
DB="bm25_bench_$$"

case "$LAYOUT" in
  segments|merged) ;;
  *) echo "wand_global_ub.sh: layout must be 'segments' or 'merged', not '$LAYOUT'" >&2
     exit 2 ;;
esac

psql -X -d postgres -v ON_ERROR_STOP=1 -c "CREATE DATABASE \"$DB\";" >/dev/null
trap 'psql -X -d postgres -c "DROP DATABASE IF EXISTS \"$DB\";" >/dev/null' EXIT

psql -X -d "$DB" -v ON_ERROR_STOP=1 -v ndocs="$NDOCS" -q <<'SQL' >/dev/null
CREATE EXTENSION bm25_native;
SELECT setseed(0.153);
-- Vocabulary w1..w4999, rank drawn log-uniformly (P(rank k) ~ 1/k). Title 3..8
-- tokens, body 20..300.
CREATE TABLE src AS
SELECT g AS id,
       (SELECT string_agg('w' || floor(exp(random() * ln(5000)))::int, ' ')
          FROM generate_series(1, 3 + (random() * 5)::int + 0 * g)) AS title,
       (SELECT string_agg('w' || floor(exp(random() * ln(5000)))::int, ' ')
          FROM generate_series(1, 20 + (random() * 280)::int + 0 * g)) AS body
  FROM generate_series(1, :ndocs) g;

CREATE TABLE docs (id int PRIMARY KEY, title text, body text) WITH (autovacuum_enabled = off);
CREATE INDEX docs_bm ON docs USING bm25_native (title, body) INCLUDE (id)
    WITH (key_field = 'id', boost_title = '3.0', boost_body = '1.0');
INSERT INTO docs SELECT * FROM src;
SELECT bm25_seal('docs_bm');
ANALYZE docs;
SQL

# The merge cuts its output at the maintenance budget (BUILD-04), so under the
# default 64MB a 100k-document corpus still merges into a dozen or so segments of up
# to ~7.7k documents. The raised budget is what lets one segment take most of
# the corpus.
if [[ "$LAYOUT" == merged ]]; then
  psql -X -d "$DB" -v ON_ERROR_STOP=1 -q >/dev/null <<'SQL'
SET maintenance_work_mem = '1GB';
SELECT bm25_merge('docs_bm');
SQL
fi

# hit + read from the first "Buffers: shared" line of an EXPLAIN (the top node).
# Only the shared part of that line: a spill adds "local"/"temp" parts with their
# own hit=/read= tokens. One awk that reads all its input, rather than grep -m1 in
# a pipe, so an early exit cannot SIGPIPE psql under pipefail.
blocks_of() {
  awk '/Buffers: shared/ && !done {
         s = $0; sub(/.*Buffers: shared /, "", s); sub(/,.*/, "", s);
         n = split(s, t, " "); h = 0; r = 0;
         for (i = 1; i <= n; i++) { if (t[i] ~ /^hit=/) h = substr(t[i], 5);
                                    if (t[i] ~ /^read=/) r = substr(t[i], 6) }
         print h + r; done = 1 }'
}

# One (limit, query) measurement of the ranked query: "ms,query_blks". The block
# count is from the last run; it is deterministic (hit + read counts ReadBuffer calls
# whatever is cached).
measure() {
  local limit_n="$1" query="$2" best="" ms out blks=""
  for _ in $(seq 1 "$REPS"); do
    out=$(psql -X -d "$DB" -v ON_ERROR_STOP=1 -Atq <<SQL
SET enable_seqscan = off;
SET bm25_native.wand_top_k = $limit_n;
EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, COSTS OFF)
SELECT id FROM docs WHERE title @@@ '$query'
  ORDER BY title &@@ '$query' LIMIT $limit_n;
SQL
)
    ms=$(grep '^Execution Time:' <<<"$out" | awk '{print $3}')
    blks=$(blocks_of <<<"$out")
    if [[ -z "$best" ]] || awk "BEGIN{exit !($ms < $best)}"; then best="$ms"; fi
  done
  echo "$best,$blks"
}

build_blks() {
  psql -X -d "$DB" -v ON_ERROR_STOP=1 -Atq \
    -c "EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, COSTS OFF) SELECT * FROM bm25_wand_stats('docs_bm', '$1', $2);" \
    | blocks_of
}

# Blocks of each query term's run, summed over segments: what the sweep reads. The
# probe's block_no is the PAGE a block sits on (about ten blocks share one), and it
# emits one row per impact-table field of each block, so a block is nfields rows:
# sum(1/nfields) counts blocks. count(DISTINCT (seg, block_no)) would count pages and
# undercount the sweep roughly tenfold.
sweep_blks() {
  local total=0 n t
  for t in $1; do
    n=$(psql -X -d "$DB" -v ON_ERROR_STOP=1 -Atq -c \
      "SELECT coalesce(round(sum(1.0 / nfields)), 0)::bigint
         FROM bm25_debug_block_impacts('docs_bm', '$t');")
    total=$((total + n))
  done
  echo "$total"
}

stats() {
  psql -X -d "$DB" -v ON_ERROR_STOP=1 -Atq -F, \
    -c "SELECT blocks_examined, blocks_skipped, docs_scored, deep_check_skips
          FROM bm25_wand_stats('docs_bm', '$1', $2);"
}

nsegs=$(psql -X -d "$DB" -Atc "SELECT nsegs FROM bm25_stats('docs_bm');")
maxseg=$(psql -X -d "$DB" -Atc "SELECT coalesce(max(ndocs), 0) FROM bm25_debug_segcat('docs_bm');")
echo "# ndocs=$NDOCS reps=$REPS layout=$LAYOUT nsegs=$nsegs max_seg_ndocs=$maxseg pg_version=$(psql -X -d "$DB" -Atc 'SHOW server_version;')"
echo "limit,terms,query,ms,query_blks,build_blks,sweep_blks,blocks_examined,blocks_skipped,docs_scored,deep_check_skips,blks_per_pair"
# LIMIT 1000 is where per-returned-row costs (the KEYMAP key projection, heap fetches)
# show, and where the merged layout's large segment matters most (ADR 0100's k = 1000
# cells).
for limit_n in 10 100 1000; do
  for query in "w1" "w3" "w1 w10" "w2 w30" "w3 w30 w300" "w10 w100" \
               "w1 w3 w10 w30" "w5 w50 w500 w2000" "w1 w3000" "w4000"; do
    terms=$(awk '{print NF}' <<<"$query")
    mb=$(measure "$limit_n" "$query")
    bb=$(build_blks "$query" "$limit_n")
    sb=$(sweep_blks "$query")
    st=$(stats "$query" "$limit_n")
    ds=$(cut -d, -f3 <<<"$st")
    ppp=$(awk -v b="$bb" -v d="$ds" 'BEGIN{ if (d > 0) printf "%.2f", b / d; else print "" }')
    echo "$limit_n,$terms,\"$query\",$mb,$bb,$sb,$st,$ppp"
  done
done
