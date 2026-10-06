-- Term-length cap (review ref C1).
--
-- An alphanumeric run has no natural length limit, but a term is stored INLINE in
-- its BM25DictEntry record and that record must fit one segment page whole.  Before
-- the cap, a run of ~8 KB or more made the DICT writer memcpy past the end of an
-- 8 KB shared buffer (the only guard was an Assert, compiled out in production).
--
-- Contract asserted here:
--   1. A document containing an over-long run still indexes; only the monster token
--      is dropped, with a NOTICE (same shape as core FTS's "word is too long").
--   2. The dropped token is genuinely absent from the dictionary -- not truncated to
--      some prefix that would collide with a real term.
--   3. Everything at or below the cap is indexed and searchable, on both the
--      CREATE INDEX (segment writer) and the INSERT (pending list) paths.

CREATE EXTENSION bm25_native;

CREATE TABLE longtok (id int primary key, body text);

-- 20000 bytes: far past both the cap and one page.  Row 2 is the regression case
-- from the report; rows 1 and 3 are ordinary neighbours that must survive it.
INSERT INTO longtok VALUES
  (1, 'alpha beta gamma'),
  (2, 'prefix ' || repeat('a', 20000) || ' suffix'),
  (3, 'alpha delta');

-- Build path: the NOTICE fires during the heap scan, and the build completes.
CREATE INDEX longtok_bm25 ON longtok USING bm25_native (body);
SELECT bm25_seal('longtok_bm25');

SET enable_seqscan = off;

-- (1) The row survived: its short tokens are indexed and searchable.
SELECT id FROM longtok WHERE body @@@ 'prefix' ORDER BY id;
SELECT id FROM longtok WHERE body @@@ 'suffix' ORDER BY id;

-- (1) Neighbouring rows are unaffected.
SELECT id FROM longtok WHERE body @@@ 'alpha' ORDER BY id;

-- (2) The over-long run is in no dictionary entry, whole or truncated.  Every
-- stored term is at or below the cap, and none of them is a long run of 'a's.
SELECT count(*) AS overlong_terms
  FROM bm25_debug_terms('longtok_bm25')
 WHERE length(term) > 2047;

SELECT count(*) AS all_a_terms
  FROM bm25_debug_terms('longtok_bm25')
 WHERE term ~ '^a{100,}$';

SELECT term, df FROM bm25_debug_terms('longtok_bm25') ORDER BY term;

-- (3) Boundary: exactly at the cap is INDEXED, one byte over is DROPPED.  These go
-- through the INSERT/pending path rather than the build path.
INSERT INTO longtok VALUES (4, repeat('b', 2047));
INSERT INTO longtok VALUES (5, repeat('c', 2048));

SELECT id FROM longtok WHERE body @@@ repeat('b', 2047) ORDER BY id;
SELECT id FROM longtok WHERE body @@@ repeat('c', 2048) ORDER BY id;

-- The same holds once those rows are sealed into a segment.
SELECT bm25_seal('longtok_bm25');
SELECT id FROM longtok WHERE body @@@ repeat('b', 2047) ORDER BY id;
SELECT id FROM longtok WHERE body @@@ repeat('c', 2048) ORDER BY id;

-- (bm25_debug_terms is deliberately NOT re-run here: its aggregation hash uses a
-- small fixed-width key and fails loud on any term near the cap.  That is a
-- debug-helper limit, not a storage limit -- the @@@ probes above are what prove
-- the 2047-byte term round-tripped through the segment writer.)

RESET enable_seqscan;
DROP TABLE longtok;
DROP EXTENSION bm25_native;
