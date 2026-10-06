CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);   -- empty build path
-- Fresh index: 0 docs, current format version, defaults k1/b.
SELECT ndocs, format_version, k1, b FROM bm25_stats('docs_bm25');
DROP TABLE docs;
DROP EXTENSION bm25_native;
