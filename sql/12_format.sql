-- 12_format: a fresh index reports this build's BM25_FORMAT_VERSION.
-- (The number is deliberately not repeated in prose -- a prose copy rots at the
--  next bump. What this suite asserts is that a fresh index builds at all and
--  that bm25_stats reports the version the build was compiled with.)
CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
SELECT format_version FROM bm25_stats('docs_bm25');
DROP TABLE docs;
DROP EXTENSION bm25_native;
