CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
-- append three docs to the pending list directly, then seal.
SELECT bm25_debug_pending_append('docs_bm25', 'the quick brown fox');
SELECT bm25_debug_pending_append('docs_bm25', 'the lazy brown dog');
SELECT bm25_debug_pending_append('docs_bm25', 'quick quick fox');
SELECT nsegs, pending_ndocs FROM bm25_stats('docs_bm25');
SELECT bm25_seal('docs_bm25');
SELECT nsegs, ndocs, pending_ndocs FROM bm25_stats('docs_bm25');
DROP TABLE docs;
DROP EXTENSION bm25_native;
