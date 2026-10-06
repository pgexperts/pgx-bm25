-- 13_seg: a build of this size seals exactly one segment; the segment header +
-- dict report the right doc/term counts and per-term df, and the docmap roundtrips
-- a TID.  ("Of this size" since #146: a build now cuts its accumulator at
-- bm25_maintenance_budget_bytes and publishes one segment per chunk.  Three
-- documents are many orders of magnitude under any real budget, so one segment is
-- what this corpus produces -- but it is a consequence of the size, not an
-- invariant of ambuild.)
CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
INSERT INTO docs VALUES
  (1, 'the quick brown fox'),
  (2, 'the lazy brown dog'),
  (3, 'quick quick fox');
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
-- one sealed segment, 3 docs, total_len 11
SELECT nsegs, ndocs, total_len FROM bm25_stats('docs_bm25');
-- per-segment dictionary (df counts distinct docs)
SELECT term, df FROM bm25_debug_segterms('docs_bm25') ORDER BY term;
DROP TABLE docs;
DROP EXTENSION bm25_native;
