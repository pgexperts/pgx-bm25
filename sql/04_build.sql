CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
INSERT INTO docs VALUES
  (1, 'the quick brown fox'),
  (2, 'the lazy brown dog'),
  (3, 'quick quick fox');
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
-- 3 docs indexed; total_len = 4+4+3 = 11 tokens.
SELECT ndocs, total_len FROM bm25_stats('docs_bm25');
-- distinct terms present (debug introspection of the dictionary):
SELECT term, df FROM bm25_debug_terms('docs_bm25') ORDER BY term;
-- v4: the metapage carries the analyzer fingerprint + a single default field.
SELECT analyzer_fingerprint <> 0 AS fp_set, field_count
  FROM bm25_stats('docs_bm25');
-- bm25_debug_analyzer_fingerprint(index) must equal the stored metapage value.
SELECT bm25_debug_analyzer_fingerprint('docs_bm25')
     = (SELECT analyzer_fingerprint FROM bm25_stats('docs_bm25')) AS fp_matches;
DROP TABLE docs;
DROP EXTENSION bm25_native;
