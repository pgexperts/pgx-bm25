-- 118_ranked_keys_forward_pass -- the ranked-row finalizers resolve key_field values
-- in docid order within each multi-page KEYMAP and scatter them back to rank order
-- (issue #246).
--
-- Rows are finalized in score order, so within one segment the docids jump backward
-- all the time, and the KEYMAP cursor resumes only forward: every backward jump used to
-- re-walk that segment's KEYMAP chain from its root. Both finalizers now collect each
-- row's (segment, docid, rank slot); rows of a one-page KEYMAP are resolved at once, in
-- rank order, and the rest are sorted by (segment, docid), resolved in one forward
-- pass per segment, and each key is written into its own slot. That is a performance change
-- and returns the same bytes, so nothing here failed before it. What the file pins is
-- the scatter: every ranked row must carry ITS OWN key, on both builders, with sealed
-- rows from two multi-page KEYMAPs and a one-page one (the two paths) interleaved with
-- pending rows that have no segment source at all. A slot mix-up shows up as a key that is not the row's id
-- (bm25_debug_rank_key against the heap) or as bm25_score_key() returning another
-- row's score.
CREATE EXTENSION bm25_native;

-- Sealed segments of 3,000, 500 and 3,000 documents, then 40 pending rows. An int4
-- KEYMAP holds about 2,000 keys per page, so the 3,000-document segments' KEYMAPs span
-- two pages (sorted, and the forward pass crosses a page boundary) and the 500-document
-- one fits on one page (resolved in rank order). The body mixes tf (1-3 x 'common') with
-- doclen (0-96 pads) so scores are nearly unique and not monotone in id.
CREATE TABLE kf (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
CREATE INDEX kf_bm ON kf USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
INSERT INTO kf SELECT g, repeat('common ', 1 + g % 3) || repeat('pad ', (g * 7919) % 97) || 'x' || (g % 5)
  FROM generate_series(1, 3000) g;
SELECT bm25_seal('kf_bm');
INSERT INTO kf SELECT g, repeat('common ', 1 + g % 3) || repeat('pad ', (g * 7919) % 97) || 'x' || (g % 5)
  FROM generate_series(3001, 3500) g;
SELECT bm25_seal('kf_bm');
INSERT INTO kf SELECT g, repeat('common ', 1 + g % 3) || repeat('pad ', (g * 7919) % 97) || 'x' || (g % 5)
  FROM generate_series(3501, 6500) g;
SELECT bm25_seal('kf_bm');
INSERT INTO kf SELECT g, repeat('common ', 1 + g % 3) || repeat('pad ', (g * 7919) % 97) || 'x' || (g % 5)
  FROM generate_series(6501, 6540) g;

SELECT count(*) AS segs, sum(ndocs) AS sealed_docs FROM bm25_debug_segcat('kf_bm');
SELECT count(*) AS keymap_entries, count(DISTINCT key_int4) AS distinct_keys
  FROM generate_series(0, 2) s, bm25_debug_seg_keymap('kf_bm', s);

-- The exhaustive builder, row by row: the key in each rank slot is the id of the heap
-- row at that slot's TID. Both probes run the same builder, so their orders agree.
CREATE TEMP TABLE exh AS
SELECT r.n, r.tid, r.score, k.key_int4 AS key
  FROM bm25_debug_rank('kf_bm', 'common') WITH ORDINALITY r(tid, score, n)
  JOIN bm25_debug_rank_key('kf_bm', 'common') WITH ORDINALITY k(key_int4, key_int8, key_uuid,
                                                                  key_text, kscore, n)
       USING (n);
SELECT count(*) AS ranked,
       count(*) FILTER (WHERE e.key IS DISTINCT FROM kf.id) AS wrong_key,
       count(*) FILTER (WHERE kf.id > 6500) AS pending_rows
  FROM exh e JOIN kf ON kf.ctid = e.tid;

-- The regime: within one segment, rank order walks docids backward many times (ids
-- are inserted in docid order within a segment). Ties sort by TID, so each of a
-- segment's distinct scores (at most 291) starts at most one backward jump.
SELECT count(*) > 600 AS many_backward_jumps
  FROM (SELECT key, lag(key) OVER (PARTITION BY CASE WHEN key <= 3000 THEN 0
                                                     WHEN key <= 3500 THEN 1 ELSE 2 END
                                   ORDER BY n) AS prev
          FROM exh WHERE key <= 6500) s
 WHERE key < prev;

-- Through the executor: bm25_score_key(id) resolves the row's score from ranked_keys,
-- so it must equal the score the builder gave that TID, on the WAND builder (k = 1000,
-- which also crosses all three segments' KEYMAPs) and on the exhaustive one.
SET enable_seqscan = off;
SET bm25_native.wand_top_k = 1000;
SELECT count(*) AS rows, count(*) FILTER (WHERE q.s IS DISTINCT FROM w.score) AS wrong_score,
       count(*) FILTER (WHERE q.id > 6500) AS pending_rows
  FROM (SELECT id, ctid, bm25_score_key(id) AS s FROM kf WHERE body @@@ 'common'
         ORDER BY body &@@ 'common' LIMIT 1000) q
  JOIN bm25_debug_wand_rank('kf_bm', 'common', 1000) w ON w.tid = q.ctid;
SET bm25_native.wand_top_k = 0;
SELECT count(*) AS rows, count(*) FILTER (WHERE q.s IS DISTINCT FROM e.score) AS wrong_score
  FROM (SELECT id, ctid, bm25_score_key(id) AS s FROM kf WHERE body @@@ 'common'
         ORDER BY body &@@ 'common' LIMIT 3000) q
  JOIN exh e ON e.tid = q.ctid;

RESET bm25_native.wand_top_k;
RESET enable_seqscan;
DROP TABLE kf;
DROP EXTENSION bm25_native;
