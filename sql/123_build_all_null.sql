-- 123_build_all_null: verify that ambuild and aminsert agree on all-NULL rows.
--
-- The build path (bm25_build_callback) registered all heap rows as documents,
-- including those whose indexed text fields are all NULL. The insert path
-- (bm25_insert) skipped such rows. This caused REINDEX/CREATE INDEX to produce
-- different ndocs/avgdl than incremental inserts, leading to different rankings.
--
-- The fix is to skip all-NULL rows before registering the docid in
-- bm25_build_callback, matching the insert path behavior.

CREATE EXTENSION bm25_native;

-- Test 1: Create index first, insert data, then seal.
-- Expected: ndocs=2 (only non-NULL rows), total_len=6
CREATE TABLE t1(id int primary key, body text);
CREATE INDEX t1_bm25 ON t1 USING bm25_native (body);
INSERT INTO t1 VALUES
  (1, 'apple'),
  (2, 'apple apple apple apple zebra'),
  (3, NULL), (4, NULL), (5, NULL), (6, NULL), (7, NULL),
  (8, NULL), (9, NULL), (10, NULL);
SELECT bm25_seal('t1_bm25');
SELECT ndocs, total_len FROM bm25_stats('t1_bm25');

-- Test 2: Insert data first, then create index.
-- Expected: same ndocs=2, total_len=6
CREATE TABLE t2(id int primary key, body text);
INSERT INTO t2 VALUES
  (1, 'apple'),
  (2, 'apple apple apple apple zebra'),
  (3, NULL), (4, NULL), (5, NULL), (6, NULL), (7, NULL),
  (8, NULL), (9, NULL), (10, NULL);
CREATE INDEX t2_bm25 ON t2 USING bm25_native (body);
SELECT ndocs, total_len FROM bm25_stats('t2_bm25');

-- Test 3: Verify REINDEX also gives the same results
REINDEX INDEX t1_bm25;
SELECT ndocs, total_len FROM bm25_stats('t1_bm25');

-- Test 4: Verify ranking is consistent across all three methods
-- Both should return the same order: id=2 first (higher BM25 score)
SET enable_seqscan = off;
SELECT array_agg(id ORDER BY body &@@ 'apple') FROM t1 WHERE body @@@ 'apple';
SELECT array_agg(id ORDER BY body &@@ 'apple') FROM t2 WHERE body @@@ 'apple';
RESET enable_seqscan;

-- Test 5: Guard against skipping on a single NULL field.
-- A row with one NULL column should still count if other columns have data.
CREATE TABLE t3(id int primary key, body1 text, body2 text);
INSERT INTO t3 VALUES
  (1, 'apple', NULL),
  (2, NULL, 'apple'),
  (3, NULL, NULL);
CREATE INDEX t3_bm25 ON t3 USING bm25_native (body1, body2);
SELECT ndocs, total_len FROM bm25_stats('t3_bm25');
-- Should find both rows with data, not skip either
SET enable_seqscan = off;
SELECT count(*) FROM t3 WHERE body1 @@@ 'apple' OR body2 @@@ 'apple';
RESET enable_seqscan;

DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;
DROP EXTENSION bm25_native;
