-- 14_pending: with a low seal threshold, inserts accumulate in the pending list
-- then a manual seal drains them into a segment. Before seal: pending_ndocs>0,
-- nsegs=0 (empty initial build). After seal: pending_ndocs=0, one segment.
CREATE EXTENSION bm25_native;
SET bm25_native.seal_threshold = '1048576';   -- 1 MB: high enough to not auto-seal here
CREATE TABLE docs (id int primary key, body text);
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);   -- empty build, 0 segs
INSERT INTO docs VALUES
  (1, 'the quick brown fox'),
  (2, 'the lazy brown dog'),
  (3, 'quick quick fox');
-- inserts landed in the pending list, not yet sealed
SELECT nsegs, pending_ndocs FROM bm25_stats('docs_bm25');
SELECT bm25_seal('docs_bm25');
-- now drained into one segment
SELECT nsegs, ndocs, pending_ndocs FROM bm25_stats('docs_bm25');
SELECT term, df FROM bm25_debug_segterms('docs_bm25') ORDER BY term;
DROP TABLE docs;
DROP EXTENSION bm25_native;
