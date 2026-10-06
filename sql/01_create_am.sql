CREATE EXTENSION bm25_native;
-- AM is registered
SELECT amname, amtype FROM pg_am WHERE amname = 'bm25_native';
-- An index can be created (ambuild is real: bm25_build scans the heap and
-- commits a segment; this case only pins that the AM is usable from DDL)
CREATE TABLE docs (id int primary key, body text);
INSERT INTO docs VALUES (1, 'the quick brown fox'), (2, 'a lazy dog');
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
  WHERE c.relname = 'docs_bm25';
-- Multicolumn bm25_native indexes are supported since M5/C2 (amcanmulticol=true): each
-- indexed column becomes a dense BM25F field. Build/INSERT/merge are field-aware
-- (per-field postings verified in 30_field_postings).
CREATE TABLE multi (id int primary key, a text, b text);
CREATE INDEX multi_bm25 ON multi USING bm25_native (a, b);
SELECT field_count FROM bm25_stats('multi_bm25');
DROP TABLE multi;
DROP TABLE docs;
DROP EXTENSION bm25_native;
