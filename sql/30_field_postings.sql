-- 30_field_postings.sql — multi-field build/insert postings (cluster C2).
--
-- Covers: the accumulator's (term, field_id) posting key + per-field doclen/Σlen;
-- the per-block field-id RLE codec round-trip; the segment header's real
-- total_len_by_field[]; and the end-to-end multi-field build + post-build INSERT
-- round-trip through the pending list, asserted via bm25_debug_field_postings.
CREATE EXTENSION bm25_native;

-- ===== Accumulator unit: (term, field_id) is the posting key =====
-- doc0: field0='quick fox'  field1='quick quick'
--   -> quick posts in BOTH (f0,tf=1) and (f1,tf=2); fox posts in (f0,tf=1).
-- doclen is per (doc,field): doc0 f0=2 f1=2. df is the dict df = TOTAL posting
-- count across all fields (R6: per-field df is derived at query time from the RLE,
-- not stored per field) — quick has 2 postings so df=2 on every quick row.
SELECT term, field_id, df, local_docid, tf, doclen
FROM bm25_debug_accum_multi(ARRAY['quick fox'], ARRAY['quick quick'])
ORDER BY term, field_id;

-- Two docs so postings span multiple (doc,field) pairs and per-field doclen
-- differs per doc.
-- doc0: f0='a b'   f1='a'
-- doc1: f0='a'     f1='b b b'
--   a: (f0: doc0 tf1, doc1 tf1) + (f1: doc0 tf1) -> 3 postings, df=3
--   b: (f0: doc0 tf1) + (f1: doc1 tf3)          -> 2 postings, df=2
--   doclen: doc0 f0=2 f1=1 ; doc1 f0=1 f1=3
SELECT term, field_id, df, local_docid, tf, doclen
FROM bm25_debug_accum_multi(ARRAY['a b','a'], ARRAY['a','b b b'])
ORDER BY term, field_id, local_docid;

-- ===== Field-id RLE codec unit =====
-- [0,0,0,1,1,2] -> runs (0,3)(1,2)(2,1) -> decodes back identically.
SELECT idx, field_id
FROM bm25_debug_field_rle_roundtrip(ARRAY[0,0,0,1,1,2])
ORDER BY idx;
-- Single run and a longer alternating pattern (worst-case: every posting its own run).
SELECT idx, field_id
FROM bm25_debug_field_rle_roundtrip(ARRAY[3,3,3,3,3])
ORDER BY idx;
SELECT idx, field_id
FROM bm25_debug_field_rle_roundtrip(ARRAY[0,1,0,1,0,1])
ORDER BY idx;

-- ===== Multi-field build + insert round-trip =====
CREATE TABLE t (id int primary key, title text, body text) WITH (autovacuum_enabled=off);
INSERT INTO t VALUES (1, 'quick fox', 'quick quick brown');
CREATE INDEX t_bm25 ON t USING bm25_native (title, body);
-- Build path: 'quick' posts in BOTH title (field 0, tf=1) and body (field 1, tf=2).
SELECT field_id, tf FROM bm25_debug_field_postings('t_bm25', 'quick')
ORDER BY field_id;
-- 'fox' only in title (field 0); 'brown' only in body (field 1).
SELECT field_id, tf FROM bm25_debug_field_postings('t_bm25', 'fox') ORDER BY field_id;
SELECT field_id, tf FROM bm25_debug_field_postings('t_bm25', 'brown') ORDER BY field_id;
-- Per-field Σdoclen survives seal: title='quick fox' -> 2 ; body='quick quick brown' -> 3.
SELECT field_id, total_len FROM bm25_debug_seg_lenfields('t_bm25', 0) ORDER BY field_id;

-- Insert path: a new row, sealed, must post 'lazi' (stem of 'lazy') under body only.
INSERT INTO t VALUES (2, 'sharp', 'lazy dog');
SELECT bm25_seal('t_bm25');
SELECT field_id, tf FROM bm25_debug_field_postings('t_bm25', 'lazi') ORDER BY field_id;
-- 'sharp' from the inserted row's title -> field 0 only.
SELECT field_id, tf FROM bm25_debug_field_postings('t_bm25', 'sharp') ORDER BY field_id;
DROP TABLE t;

-- ===== >128-posting term across two fields: RLE spans blocks =====
CREATE TABLE big (id int primary key, a text, b text) WITH (autovacuum_enabled=off);
-- 200 rows: 'common' in field a for ids<=100, field b for ids>100.
INSERT INTO big SELECT g, CASE WHEN g<=100 THEN 'common' ELSE 'other' END,
                          CASE WHEN g>100  THEN 'common' ELSE 'other' END
  FROM generate_series(1,200) g;
CREATE INDEX big_bm25 ON big USING bm25_native (a, b);
SELECT bm25_seal('big_bm25');
-- 'common' (df=200): 100 postings field 0, 100 field 1; the RLE must survive the
-- 128-posting block boundary (block 0 = 128 postings, block 1 = 72).
SELECT field_id, count(*) FROM bm25_debug_field_postings('big_bm25', 'common')
GROUP BY field_id ORDER BY field_id;
DROP TABLE big;

-- ===== Merge preserves every field's postings (amcanmulticol flip proof) =====
-- Build a 3-column index, then seal FOUR same-size segments (the BM25_MERGE_LAYER
-- _FANOUT=4 trigger, mirroring 19_merge) so bm25_merge actually consolidates them.
-- Assert each field's postings SURVIVE the merge (not collapsed to field 0). This
-- is the load-bearing test for flipping amcanmulticol=true: the merge re-
-- accumulates through the same field-aware decode + accumulator, so the merged
-- segment must carry field 0/1/2 exactly.
CREATE TABLE m (id int primary key, title text, summary text, body text)
  WITH (autovacuum_enabled=off);
-- 'zephyr' lands in a DIFFERENT field in each row so a merge that collapsed fields
-- would show it only under field 0. Build = segment 0 (rows 1..2).
INSERT INTO m VALUES (1, 'zephyr', 'alpha', 'beta');
INSERT INTO m VALUES (2, 'alpha', 'zephyr', 'beta');
CREATE INDEX m_bm25 ON m USING bm25_native (title, summary, body);
-- Three more sealed segments (each one insert then seal) -> 4 same-size segments.
INSERT INTO m VALUES (3, 'alpha', 'beta', 'zephyr');
SELECT bm25_seal('m_bm25');                                   -- segment 1
INSERT INTO m VALUES (4, 'zephyr', 'zephyr', 'zephyr');
SELECT bm25_seal('m_bm25');                                   -- segment 2
INSERT INTO m VALUES (5, 'beta', 'zephyr', 'alpha');
SELECT bm25_seal('m_bm25');                                   -- segment 3
-- 'zephyr' spread across all three fields, over four segments.
--   f0: rows 1,4        f1: rows 2,4,5      f2: rows 3,4
SELECT field_id, count(*) FROM bm25_debug_field_postings('m_bm25', 'zephyr')
GROUP BY field_id ORDER BY field_id;
SELECT count(*) AS nsegs_before FROM bm25_debug_segcat('m_bm25');
-- Force the merge; then re-assert per-field postings survive it.
SELECT bm25_merge('m_bm25');
SELECT count(*) AS nsegs_after FROM bm25_debug_segcat('m_bm25');
SELECT field_id, count(*) FROM bm25_debug_field_postings('m_bm25', 'zephyr')
GROUP BY field_id ORDER BY field_id;
-- Per-field Σdoclen also survives the merge. Every row is 3 single-token fields, so
-- across all 5 live docs each field's Σlen == 5.
SELECT field_id, total_len FROM bm25_debug_seg_lenfields('m_bm25', 0) ORDER BY field_id;
DROP TABLE m;

DROP EXTENSION bm25_native;
