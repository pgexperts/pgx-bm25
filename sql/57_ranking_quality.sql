-- 57_ranking_quality -- gated NDCG@10 regression on vendored BEIR SciFact.
-- Unlike the build-time bench (bench/index_build.sh, report-only per the CI
-- noise policy -- wall clock on a shared runner is untrustworthy), NDCG@10
-- on a fixed corpus is a pure function of index contents + scoring math: no
-- timing component, identical on the noisiest CI VM in the fleet. A ranking-
-- quality regression is a correctness bug, so it gates like one.
--
-- Threshold derivation (multifield >= 0.64, flat >= 0.66) and the real
-- measured values this suite pins against are recorded in ADR 0011
-- (docs/adr/0011-ranking-quality-benchmarks.md); do not re-derive them here.
--
-- Corpus provenance: data/README.md. The raw vendored queries file bundles
-- BOTH BEIR splits (1109 lines); the canonical 300-query test set is
-- whatever qrels/test.tsv actually judges, so the queries table below is
-- built FILTERED to judged ids -- running the other 809 would burn ~3.7x
-- the fan-out cost for zero effect on the trec_eval mean (ndcg10()'s JOIN to
-- idcg already ignores unjudged queries; filtering earlier just saves time).
CREATE EXTENSION IF NOT EXISTS bm25_native;

-- \copy does NOT interpolate psql variables (\cd does) -- this is why the
-- staging load goes through \cd rather than a variable-substituted path.
-- The input/*.source + @abs_srcdir@ substitution mechanism this project
-- used to be able to reach for is gone on PG17/18; PG_ABS_SRCDIR is the
-- replacement pg_regress exports for exactly this purpose.
\getenv abs_srcdir PG_ABS_SRCDIR
\cd :abs_srcdir

CREATE TABLE stage_corpus(line text);
CREATE TABLE stage_queries(line text);
CREATE TABLE qrels(query_id int, corpus_id int, score int);
-- 0x01/0x02 delimiter/quote trick: keeps embedded JSON double-quotes and the
-- two LaTeX-bearing abstracts' backslashes intact. FORMAT text corrupts both.
\copy stage_corpus FROM 'data/scifact_corpus.data' WITH (FORMAT csv, DELIMITER E'\x02', QUOTE E'\x01')
\copy stage_queries FROM 'data/scifact_queries.data' WITH (FORMAT csv, DELIMITER E'\x02', QUOTE E'\x01')
\copy qrels FROM 'data/scifact_qrels.data' WITH (FORMAT csv, DELIMITER E'\t', HEADER true)

-- Raw-load counts FIRST, before any parsing/filtering -- a truncated or
-- wrongly-renamed vendored file fails loudly right here instead of quietly
-- skewing the NDCG mean downstream.
SELECT count(*) AS n_stage_corpus FROM stage_corpus;               -- 5183
SELECT count(*) AS n_stage_queries FROM stage_queries;              -- 1109 (both BEIR splits)
SELECT count(*) AS n_qrels FROM qrels;                              -- 339
SELECT count(DISTINCT query_id) AS n_judged_queries FROM qrels;     -- 300

CREATE TABLE docs (id int PRIMARY KEY, title text, body text)
    WITH (autovacuum_enabled = off);
INSERT INTO docs
SELECT ((line::jsonb)->>'_id')::int, (line::jsonb)->>'title', (line::jsonb)->>'text'
FROM stage_corpus;
CREATE TABLE docs_flat (id int PRIMARY KEY, body text)
    WITH (autovacuum_enabled = off);
INSERT INTO docs_flat SELECT id, title || ' ' || body FROM docs;

SELECT count(*) AS n_docs FROM docs;                                -- 5183

-- Filtered to judged ids only -- see header. This is the 300 "test queries"
-- BEIR papers report against; it is a strict subset of stage_queries.
CREATE TABLE queries (qid int PRIMARY KEY, qtext text);
INSERT INTO queries
SELECT ((line::jsonb)->>'_id')::int, (line::jsonb)->>'text'
FROM stage_queries
WHERE ((line::jsonb)->>'_id')::int IN (SELECT DISTINCT query_id FROM qrels);

SELECT count(*) AS n_queries FROM queries;                         -- 300

-- Title first -- the ordered scan anchors on the FIRST indexed column.
CREATE INDEX docs_bm25 ON docs USING bm25_native (title, body) INCLUDE (id)
    WITH (key_field = 'id');
SELECT bm25_seal('docs_bm25');
ANALYZE docs;
CREATE INDEX docs_flat_bm25 ON docs_flat USING bm25_native (body) INCLUDE (id)
    WITH (key_field = 'id');
SELECT bm25_seal('docs_flat_bm25');
ANALYZE docs_flat;

CREATE FUNCTION qj_multi(q text) RETURNS jsonb LANGUAGE sql IMMUTABLE STRICT AS $$
  SELECT bm25_boolean(should => ARRAY[bm25_match_terms('title', q),
                                      bm25_match_terms('body',  q)]) $$;
CREATE FUNCTION qj_flat(q text) RETURNS jsonb LANGUAGE sql IMMUTABLE STRICT AS $$
  SELECT bm25_match_terms('body', q) $$;

-- Sanctioned use of enable_seqscan=off (scan-path determinism): under a
-- seqscan fallback there is no active scored scan to own the row, so &@@
-- degrades to a constant +infinity: every row ties and the Sort's order is
-- arbitrary SILENTLY, corrupting NDCG rather than erroring. The plan-shape
-- guard below turns "the forced path got defeated anyway" into a loud
-- failure instead of silently-wrong NDCG numbers.
SET enable_seqscan = off;
-- jit=off: the fan-out query's cost estimate clears jit_above_cost, and on
-- some local LLVM/Homebrew combinations PG18's JIT provider fails to dlopen
-- (unrelated to this extension). JIT is a semantics-preserving expression-
-- compilation strategy, so plan shape and NDCG values are expected to be
-- identical with it on or off BY DESIGN -- that expectation has not been
-- verified by an on/off A-B run on this machine (its JIT is the one that's
-- broken). Disabling avoids a spurious hard failure on affected dev
-- machines while costing nothing on machines where JIT works.
SET jit = off;

CREATE TEMP TABLE run_multi AS
SELECT q.qid, d.rnk, d.docid
FROM queries q CROSS JOIN LATERAL (
  SELECT id AS docid, row_number() OVER () AS rnk
  FROM docs
  WHERE title @@@ qj_multi(q.qtext)
  ORDER BY title &@@ qj_multi(q.qtext)   -- single-key ORDER BY, house rule
  LIMIT 10) d;

CREATE TEMP TABLE run_flat AS
SELECT q.qid, d.rnk, d.docid
FROM queries q CROSS JOIN LATERAL (
  SELECT id AS docid, row_number() OVER () AS rnk
  FROM docs_flat
  WHERE body @@@ qj_flat(q.qtext)
  ORDER BY body &@@ qj_flat(q.qtext)     -- single-key ORDER BY, house rule
  LIMIT 10) d;

-- Portable plan-shape guard (sql/53 style): prove both LATERAL bodies still
-- compile to an ordered Index Scan rather than silently falling back to a
-- Sort over a sequential scan. Matches the structural "Index Scan using
-- <index> on <table>" line only (not the Index Cond/Order By text, which
-- embeds the query-specific jsonb literal) -- same discipline as suite 53's
-- css_plan wrapper: pin the shape, not the per-query payload.
CREATE FUNCTION rq_plan(q text) RETURNS SETOF text LANGUAGE plpgsql AS
$$ BEGIN RETURN QUERY EXECUTE 'EXPLAIN (COSTS OFF) ' || q; END $$;

SELECT count(*) AS multifield_is_index_scan
FROM rq_plan($q$
  SELECT id AS docid, row_number() OVER ()
  FROM docs
  WHERE title @@@ qj_multi('tumor suppressor gene')
  ORDER BY title &@@ qj_multi('tumor suppressor gene')
  LIMIT 10
$q$) AS line
WHERE line LIKE '%Index Scan using docs_bm25 on docs%';

-- A Sort above the LATERAL would renumber row_number() in sort order after
-- scan-order assignment, making ranks garbage even with the index scan present.
SELECT count(*) AS multifield_plan_sort_nodes
FROM rq_plan($q$
  SELECT id AS docid, row_number() OVER ()
  FROM docs
  WHERE title @@@ qj_multi('tumor suppressor gene')
  ORDER BY title &@@ qj_multi('tumor suppressor gene')
  LIMIT 10
$q$) AS line
WHERE line LIKE '%Sort%';

SELECT count(*) AS flat_is_index_scan
FROM rq_plan($q$
  SELECT id AS docid, row_number() OVER ()
  FROM docs_flat
  WHERE body @@@ qj_flat('tumor suppressor gene')
  ORDER BY body &@@ qj_flat('tumor suppressor gene')
  LIMIT 10
$q$) AS line
WHERE line LIKE '%Index Scan using docs_flat_bm25 on docs_flat%';

-- A Sort above the LATERAL would renumber row_number() in sort order after
-- scan-order assignment, making ranks garbage even with the index scan present.
SELECT count(*) AS flat_plan_sort_nodes
FROM rq_plan($q$
  SELECT id AS docid, row_number() OVER ()
  FROM docs_flat
  WHERE body @@@ qj_flat('tumor suppressor gene')
  ORDER BY body &@@ qj_flat('tumor suppressor gene')
  LIMIT 10
$q$) AS line
WHERE line LIKE '%Sort%';

-- trec_eval ndcg_cut_10 semantics: LINEAR gain (raw qrel score), discount
-- log2(rank+1), unjudged = 0, IDCG over min(10, judged) ideal ranks, mean
-- over EXACTLY the 300 test queries (a query retrieving nothing scores 0,
-- it does not shrink the denominator).
CREATE FUNCTION ndcg10(runtab regclass) RETURNS numeric LANGUAGE plpgsql AS $$
DECLARE v numeric;
BEGIN
  EXECUTE format($f$
    WITH dcg AS (
      SELECT r.qid, sum(qr.score / log(2, r.rnk + 1)) AS dcg
      FROM %s r JOIN qrels qr ON qr.query_id = r.qid AND qr.corpus_id = r.docid
      GROUP BY r.qid),
    idcg AS (
      SELECT query_id AS qid, sum(score / log(2, rn + 1)) AS idcg
      FROM (SELECT query_id, score,
                   row_number() OVER (PARTITION BY query_id ORDER BY score DESC) AS rn
            FROM qrels) s
      WHERE rn <= 10 GROUP BY query_id)
    SELECT avg(coalesce(d.dcg, 0) / i.idcg)
    FROM queries q
    JOIN idcg i ON i.qid = q.qid
    LEFT JOIN dcg d ON d.qid = q.qid
  $f$, runtab) INTO v;
  RETURN v;
END $$;

-- Gates. No rounded canaries: the honesty run measured both real values
-- inside the +-0.005 rounding-boundary band (0.666054, 0.00105 from the
-- 0.665 boundary; 0.687126, 0.00213 from 0.685) -- a boundary-straddling
-- literal would flake on a 1-ULP cross-arch score difference, so the
-- boolean gate stands alone per the spec's output discipline.
SELECT ndcg10('run_multi') >= 0.64 AS multifield_ndcg_ok;
SELECT ndcg10('run_flat') >= 0.66 AS flat_ndcg_ok;

-- Gap canary: flat outscoring multifield on SciFact is a genuine, previously-
-- measured property of this corpus (both Anserini's published baselines and
-- our own honesty run agree), not an artifact -- pin it so a future change
-- that collapses or inverts the gap is caught even if both gates still pass.
SELECT ndcg10('run_flat') > ndcg10('run_multi') AS flat_exceeds_multifield;

DROP FUNCTION ndcg10(regclass);
DROP FUNCTION rq_plan(text);
DROP FUNCTION qj_multi(text);
DROP FUNCTION qj_flat(text);
DROP TABLE run_multi;
DROP TABLE run_flat;
DROP TABLE queries;
DROP TABLE docs_flat;
DROP TABLE docs;
DROP TABLE qrels;
DROP TABLE stage_corpus;
DROP TABLE stage_queries;
RESET enable_seqscan;
RESET jit;
DROP EXTENSION bm25_native;
