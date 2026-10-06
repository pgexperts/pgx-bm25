CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text) WITH (autovacuum_enabled=off);
INSERT INTO docs VALUES
  (1, 'quick brown fox'),
  (2, 'quick quick quick'),
  (3, 'lazy brown dog'),
  (4, 'nothing relevant here');
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
-- Guard: the order-by operator must be registered as an 'o' amop, else ORDER BY
-- cannot use index order at all and the no-Sort assertion below would fail with
-- a confusing diff instead of a clear error.
SELECT amopstrategy FROM pg_amop a JOIN pg_opfamily f ON a.amopfamily=f.oid
WHERE f.opfname='text_bm25_ops' AND a.amoppurpose='o';
-- enable_seqscan=off below is kept for scan-path determinism: this suite
-- asserts ranked OUTPUT and plan SHAPE (no Sort node), not the planner's
-- unaided choice. Since 0.3, @@@ carries honest selectivity/cost and wins
-- the index path by default without this crutch; verifying that is suite
-- 56's job (sql/56_planner_estimates.sql), not this suite's.
SET enable_seqscan = off;
-- ranked top-2 for 'quick': doc2 then doc1
SELECT id FROM docs WHERE body @@@ 'quick'
ORDER BY body &@@ 'quick' LIMIT 2;
-- score accessor returns positive, descending
SELECT id, round(bm25_score(ctid)::numeric,6) AS score
FROM docs WHERE body @@@ 'brown'
ORDER BY body &@@ 'brown' LIMIT 5;
-- plan uses index order, no Sort node
EXPLAIN (COSTS OFF)
SELECT id FROM docs WHERE body @@@ 'quick' ORDER BY body &@@ 'quick' LIMIT 2;
RESET enable_seqscan;
DROP TABLE docs;
DROP EXTENSION bm25_native;
