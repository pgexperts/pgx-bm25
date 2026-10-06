-- 14_scale: a corpus that would have ERRORED in M0/M1.
--  * 2500 docs all containing 'common'  -> >1 posting page for that term
--  * each doc also has a unique term     -> >2500 distinct dict terms (>1 dict page)
CREATE EXTENSION bm25_native;
CREATE TABLE big (id int primary key, body text);
INSERT INTO big
  SELECT g, 'common term' || ' u' || g
  FROM generate_series(1, 2500) g;
CREATE INDEX big_bm25 ON big USING bm25_native (body);

-- builds without the M0/M1 single-page errors:
SELECT nsegs, ndocs FROM bm25_stats('big_bm25');

-- 'common' is in all 2500 docs:
SELECT df FROM bm25_debug_segterms('big_bm25') WHERE term = 'common';

-- distinct dict terms exceed a single dict page (2500 unique + common + term):
SELECT count(*) >= 2500 AS many_terms FROM bm25_debug_segterms('big_bm25');

-- a query for a rare unique term returns exactly its one doc:
SELECT count(*) AS hits
  FROM big WHERE body @@@ 'u1777';

-- ranked top-3 for the common term returns 3 rows (correctness of multi-page postings read):
SELECT count(*) AS top3
  FROM (SELECT id FROM big ORDER BY body &@@ 'common' LIMIT 3) s;

-- FULL multi-block decode: 'common' spans ~20 posting blocks. The boolean (@@@)
-- and scored (&@@) paths must each decode EVERY block, not just the first 128.
-- (Regression guard for the per-block delta-base reset in bm25_seg_scan_postings:
-- a decoder that carried the delta base across blocks returned ~640, not 2500.)
SET enable_seqscan = off;
SELECT count(*) AS common_hits_boolean
  FROM big WHERE body @@@ 'common';
SELECT count(*) AS common_hits_scored
  FROM (SELECT id FROM big WHERE body @@@ 'common' ORDER BY body &@@ 'common' LIMIT 5000) s;
RESET enable_seqscan;

DROP TABLE big;
DROP EXTENSION bm25_native;
