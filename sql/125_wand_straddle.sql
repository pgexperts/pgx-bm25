-- 125_wand_straddle -- WAND must not prune a document whose postings for a term
-- straddle two posting blocks (issue #289).
--
-- On a multi-field index a document holds one posting per field for a term, and
-- they are adjacent. The segment writer used to cut a block every 128 POSTINGS, so
-- a document's (title) posting could end one block and its (body) posting open the
-- next. Each block's upper bound covers only the postings that block holds, so
-- neither bound covered the straddler's whole score, and WAND pruned it:
--   - the deep check skipped past it when it was the last document of the block
--     that ends the skip window (corpus `st`, one segment);
--   - the pivot test, whose per-term global bound was the MAX over blocks of those
--     per-block sums, abandoned the rest of a segment (corpus `pv`, two segments).
-- Through the over-pull tail's resume-by-key the missing row was lost from
-- UNLIMITED ranked scans as well, not only from a LIMIT top-k.
--
-- The writer now cuts blocks at document boundaries, so it no longer produces a
-- straddle -- but segments written before that still contain them and nothing on
-- disk distinguishes the two layouts, so the reader has to handle them. Both
-- corpora are therefore built under bm25_native.debug_count_slicing, the test
-- lever that restores the old cut-by-count writer, and the suite first proves the
-- fixture really straddles. Then the same corpora are rebuilt with the default
-- writer to show it no longer straddles and answers identically.
--
-- Every ranked query runs with seqscan and bitmapscan off and checks the plan is
-- the bm25 index scan, so the answers below come from the WAND path; the
-- reference is the exhaustive scorer (wand_top_k = 0), which never prunes.
CREATE EXTENSION bm25_native;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- True when `q` plans as an index scan on `idx` (the node name, not a whole plan
-- line, so no version-variant EXPLAIN text is pinned).
CREATE FUNCTION uses_index(q text, idx text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
  l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l LIKE '%Index Scan using ' || idx || ' %' THEN
      RETURN true;
    END IF;
  END LOOP;
  RETURN false;
END $$;

-- Straddles in an index's run of blocks for `term`: a block whose first docid is
-- the previous block's last docid, in the same segment.
CREATE FUNCTION straddles(idx regclass, term text) RETURNS bigint LANGUAGE sql AS $$
  SELECT count(*) FROM (
    SELECT seg, first_docid,
           lag(last_docid) OVER (PARTITION BY seg ORDER BY block_ord) AS prev_last
      FROM bm25_debug_block_spans(idx, term)) s
   WHERE first_docid = prev_last
$$;

-- The ranked query under test, and the exhaustive reference for it.
CREATE FUNCTION ranked_ids(tbl text, lim int) RETURNS int[] LANGUAGE plpgsql AS $$
DECLARE
  r int[];
BEGIN
  EXECUTE format('SELECT array_agg(id) FROM (SELECT id FROM %I WHERE title @@@ %L '
                 'ORDER BY title &@@ %L %s) s', tbl, 'zz', 'zz',
                 CASE WHEN lim IS NULL THEN '' ELSE 'LIMIT ' || lim END) INTO r;
  RETURN r;
END $$;
CREATE FUNCTION exhaustive_ids(tbl text, lim int) RETURNS int[] LANGUAGE plpgsql AS $$
BEGIN
  SET LOCAL bm25_native.wand_top_k = 0;
  RETURN ranked_ids(tbl, lim);
END $$;

-- ------------------------------------------------------------ corpus st
-- Postings for zz in docid order: docs 1..100 in both fields (200 postings),
-- 101..283 title only, then doc 284 in both fields with tf 3, so (284, title) is
-- posting 383, the LAST of block 2 (whose other postings are all title-only), and
-- (284, body) is posting 384, the FIRST of block 3. 284 outscores every other doc.
CREATE TABLE st (id int, title text, body text);
INSERT INTO st SELECT g, 'zz zz', 'zz zz' FROM generate_series(1, 100) g;
INSERT INTO st SELECT g, 'zz', 'qq' FROM generate_series(101, 283) g;
INSERT INTO st VALUES (284, 'zz zz zz', 'zz zz zz');
INSERT INTO st SELECT g, 'qq', 'zz' FROM generate_series(285, 400) g;
SET bm25_native.debug_count_slicing = on;
CREATE INDEX st_old ON st USING bm25_native (title, body);
RESET bm25_native.debug_count_slicing;

-- Fixture check: the old layout splits doc 284 (local docid 283) across blocks 2/3.
SELECT block_ord, ndocs, first_docid, last_docid
  FROM bm25_debug_block_spans('st_old', 'zz') ORDER BY seg, block_ord;
SELECT straddles('st_old', 'zz') AS st_old_straddles;

SELECT uses_index($$SELECT id FROM st WHERE title @@@ 'zz' ORDER BY title &@@ 'zz'$$,
                  'st_old') AS st_index_path;

-- Unlimited: every one of the 400 matching rows, none missing (pre-fix: 399, 284 lost).
SELECT cardinality(ranked_ids('st', NULL)) AS st_unlimited_rows;
SELECT id AS st_missing FROM st
EXCEPT SELECT unnest(ranked_ids('st', NULL)) ORDER BY 1;
SELECT ranked_ids('st', NULL) = exhaustive_ids('st', NULL) AS st_unlimited_matches_exhaustive;
-- LIMIT: 284 first, as the exhaustive scorer ranks it (pre-fix: {1,2,3}).
SELECT ranked_ids('st', 3) AS st_top3;
SELECT ranked_ids('st', 3) = exhaustive_ids('st', 3) AS st_top3_matches_exhaustive;
-- The WAND probe itself, against the exhaustive probe, tids and score bits.
SELECT k, (SELECT array_agg(tid ORDER BY ord) FROM bm25_debug_wand_rank('st_old', 'zz', k)
             WITH ORDINALITY AS w(tid, score, ord))
        = (SELECT array_agg(tid) FROM (SELECT tid FROM bm25_debug_rank('st_old', 'zz')
             LIMIT k) e) AS wand_tids_ok,
          (SELECT array_agg(score ORDER BY ord) FROM bm25_debug_wand_rank('st_old', 'zz', k)
             WITH ORDINALITY AS w(tid, score, ord))
        = (SELECT array_agg(score) FROM (SELECT score FROM bm25_debug_rank('st_old', 'zz')
             LIMIT k) e) AS wand_scores_ok
  FROM unnest(ARRAY[1, 3, 10, 100]) k;
-- The deep check engaged on this query (it is the prune site this corpus reaches),
-- and still pruned: the fix keeps WAND pruning, it does not turn it off.
SELECT deep_check_skips > 0 AS st_deep_check_engaged, docs_scored < 400 AS st_still_prunes
  FROM bm25_wand_stats('st_old', 'zz', 100);

-- ------------------------------------------------------------ corpus pv
-- Two segments. The first holds docs 1..100 in both fields. The second holds
-- 101..227 title only (postings 0..126), then 228 in both fields with tf 3, so
-- (228, title) is posting 127 (last of block 0) and (228, body) posting 128 (first
-- of block 1), then 229..300 body only. 228 needs both halves to outrank docs
-- 1..100; no single block of the second segment bounds it, and with the old
-- max-of-block-sums global bound the pivot test abandoned that segment.
CREATE TABLE pv (id int, title text, body text);
INSERT INTO pv SELECT g, 'zz zz', 'zz zz' FROM generate_series(1, 100) g;
CREATE INDEX pv_old ON pv USING bm25_native (title, body);
INSERT INTO pv SELECT g, 'zz', 'qq' FROM generate_series(101, 227) g;
INSERT INTO pv VALUES (228, 'zz zz zz', 'zz zz zz');
INSERT INTO pv SELECT g, 'qq', 'zz' FROM generate_series(229, 300) g;
SET bm25_native.debug_count_slicing = on;
SELECT bm25_seal('pv_old');
RESET bm25_native.debug_count_slicing;

SELECT seg, block_ord, ndocs, first_docid, last_docid
  FROM bm25_debug_block_spans('pv_old', 'zz') ORDER BY seg, block_ord;
SELECT straddles('pv_old', 'zz') AS pv_old_straddles;
SELECT uses_index($$SELECT id FROM pv WHERE title @@@ 'zz' ORDER BY title &@@ 'zz'$$,
                  'pv_old') AS pv_index_path;

SELECT cardinality(ranked_ids('pv', NULL)) AS pv_unlimited_rows;   -- pre-fix: 299
SELECT id AS pv_missing FROM pv
EXCEPT SELECT unnest(ranked_ids('pv', NULL)) ORDER BY 1;
SELECT ranked_ids('pv', NULL) = exhaustive_ids('pv', NULL) AS pv_unlimited_matches_exhaustive;
SELECT ranked_ids('pv', 3) AS pv_top3;                            -- pre-fix: {1,2,3}
SELECT ranked_ids('pv', 3) = exhaustive_ids('pv', 3) AS pv_top3_matches_exhaustive;

-- ------------------------------------------------------------ the new writer
-- The same two corpora under the default writer: blocks end at document
-- boundaries, so no straddle, every block within the 128-posting limit, and the
-- same answers.
DROP INDEX st_old;
CREATE INDEX st_new ON st USING bm25_native (title, body);
SELECT straddles('st_new', 'zz') AS st_new_straddles,
       max(ndocs) <= 128 AS st_new_blocks_within_limit,
       sum(ndocs) AS st_new_postings
  FROM bm25_debug_block_spans('st_new', 'zz');
SELECT uses_index($$SELECT id FROM st WHERE title @@@ 'zz' ORDER BY title &@@ 'zz'$$,
                  'st_new') AS st_new_index_path;
SELECT ranked_ids('st', NULL) = exhaustive_ids('st', NULL) AS st_new_unlimited_ok,
       ranked_ids('st', 3) AS st_new_top3;

-- A rebuild of the old two-segment index goes through the same writer and heals it.
SELECT straddles('pv_old', 'zz') AS pv_before_reindex;
REINDEX INDEX pv_old;
SELECT straddles('pv_old', 'zz') AS pv_after_reindex,
       max(ndocs) <= 128 AS pv_rebuilt_blocks_within_limit,
       sum(ndocs) AS pv_rebuilt_postings
  FROM bm25_debug_block_spans('pv_old', 'zz');
SELECT ranked_ids('pv', NULL) = exhaustive_ids('pv', NULL) AS pv_rebuilt_unlimited_ok,
       ranked_ids('pv', 3) AS pv_rebuilt_top3;

-- ------------------------------------------------------------ the lever's privilege
-- PGC_SUSET: it changes the physical layout of shared on-disk state.
CREATE ROLE bm25_289_user NOLOGIN;
SET ROLE bm25_289_user;
SET bm25_native.debug_count_slicing = on;
RESET ROLE;
DROP ROLE bm25_289_user;
DROP FUNCTION uses_index(text, text), straddles(regclass, text), ranked_ids(text, int), exhaustive_ids(text, int);
DROP TABLE pv, st;
DROP EXTENSION bm25_native;
