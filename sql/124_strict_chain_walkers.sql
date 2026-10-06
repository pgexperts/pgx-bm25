-- 124_strict_chain_walkers -- issues #293 and #294: the "validated walker" contract
-- for a segment's POST chain and its dense per-docid chains (LIVE, DOCMAP, NORMS).
--
-- A walker that reaches a chain's end (nextblk == InvalidBlockNumber) or a page's end
-- before the length the DICT entry or segment header implies must ERROR with
-- ERRCODE_INDEX_CORRUPTED, never answer short or fall back to a default. Before this
-- fix, a short POST run was read as a shorter posting list on every non-WAND path,
-- and a short LIVE/DOCMAP/NORMS chain read as "live", an invalid TID (which the heap
-- fetch extends the heap with) and doclen 0 -- and the merge replay then wrote the
-- damage into a new, healthy-looking segment.
--
-- Every corruption below is forged on a real page by a test-only lever and each index
-- is left corrupt, so every section builds its own table. The planner is pinned to the
-- index (no seqscan/bitmap scan) because a heap scan would answer the membership
-- queries correctly and the assertions would pass vacuously.
CREATE EXTENSION bm25_native;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- SQLSTATE beside the message, because which error is raised is the point (the WAND
-- path already raised a DIFFERENT one for a short POST chain). Block numbers are
-- stripped: they depend on how many pages this build happened to write.
CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, 'block [0-9]+', 'block N', 'g');
END; $$ LANGUAGE plpgsql;

-- ===================================================== 1. POST: a chain that ends early
-- 8000 documents share 'common', a run of 63 blocks over three POST pages (the last
-- block partly filled). Truncating the chain after its first page leaves the DICT
-- entry claiming 8000 postings over a chain that holds 3584.
CREATE TABLE pc (id int PRIMARY KEY, body text);
INSERT INTO pc SELECT g, 'common w' || g FROM generate_series(1, 8000) g;
CREATE INDEX pc_bm ON pc USING bm25_native (body);

-- Controls on the healthy run, through each reader the corruption is aimed at.
SELECT count(*) AS members FROM pc WHERE body @@@ 'common';
SELECT count(*) AS postings FROM bm25_debug_seg_postings('pc_bm', 'common');
SELECT count(*) AS phrase FROM pc WHERE body @@@ '"common w5"';
SET bm25_native.wand_top_k = 0;
SELECT count(*) AS exhaustive FROM
  (SELECT id FROM pc WHERE body @@@ 'common' ORDER BY body &@@ 'common' LIMIT 5) s;
RESET bm25_native.wand_top_k;
SELECT count(*) AS wand FROM
  (SELECT id FROM pc WHERE body @@@ 'common' ORDER BY body &@@ 'common' LIMIT 5) s;

SELECT bm25_debug_stamp_chain_next('pc_bm', 'post', 0, 0, 4294967295) > 0 AS stamped;

-- Pre-fix: 3584 members, 3584 postings, and a 7168-row debug dump, all without error.
SELECT pg_temp.err_of($q$SELECT count(*) FROM pc WHERE body @@@ 'common'$q$) AS members;
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_seg_postings('pc_bm', 'common')$q$)
  AS postings;
SELECT pg_temp.err_of($q$SELECT count(*) FROM pc WHERE body @@@ '"common w5"'$q$) AS phrase;
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_postings('pc_bm')$q$) AS dump;
SET bm25_native.wand_top_k = 0;
SELECT pg_temp.err_of($q$SELECT id FROM pc WHERE body @@@ 'common'
                          ORDER BY body &@@ 'common' LIMIT 5$q$) AS exhaustive;
RESET bm25_native.wand_top_k;
-- The WAND path raised before too, but a different error (the block-pointer gate at
-- the step onto the Invalid link); the cursor's open-time sweep now raises the same
-- one as every other reader, before anything is scored.
SELECT pg_temp.err_of($q$SELECT id FROM pc WHERE body @@@ 'common'
                          ORDER BY body &@@ 'common' LIMIT 5$q$) AS wand;
-- Control: a term whose run starts past the cut still reads. Its DICT entry points
-- straight at its own page, so only the runs that cross the cut are short.
SELECT pg_temp.err_of($q$SELECT count(*) FROM pc WHERE body @@@ 'w7999'$q$) AS later_term;

-- ===================================================== 2. POST: straddling documents
-- Control for the cross-block order check. The builder cuts a block every 128
-- POSTINGS, and a multi-field document has one posting per field, so a document can
-- straddle two blocks: the next block then starts at the docid the previous one
-- ended on. Document 1 has the term in one field only, so every later document's two
-- postings sit at odd offsets and every block boundary splits a document. A strict
-- (`>`) order check would reject this healthy index.
CREATE TABLE ps (id int PRIMARY KEY, a text, b text);
INSERT INTO ps SELECT g, 'straddle', CASE WHEN g = 1 THEN 'other' ELSE 'straddle' END
  FROM generate_series(1, 1000) g;
CREATE INDEX ps_bm ON ps USING bm25_native (a, b);
SELECT count(*) AS members FROM ps WHERE a @@@ 'straddle';
SET bm25_native.wand_top_k = 0;
SELECT count(*) AS exhaustive FROM
  (SELECT id FROM ps WHERE a @@@ 'straddle' ORDER BY a &@@ 'straddle' LIMIT 5) s;
RESET bm25_native.wand_top_k;
SELECT count(*) AS wand FROM
  (SELECT id FROM ps WHERE a @@@ 'straddle' ORDER BY a &@@ 'straddle' LIMIT 5) s;

-- ===================================================== 3. POST: block headers vs the run
-- 3a. last_docid disagrees with the decoded run. Only the WAND decode checked it; the
-- streaming reader every other path uses now checks it from its running prev.
CREATE TABLE pl (id int PRIMARY KEY, body text);
INSERT INTO pl SELECT g, 'common w' || g FROM generate_series(1, 8000) g;
CREATE INDEX pl_bm ON pl USING bm25_native (body);
SELECT bm25_debug_stamp_post_block('pl_bm', 0, 0, 0, 'last_docid', 99999) > 0 AS stamped;
-- Pre-fix: 8000, no error.
SELECT pg_temp.err_of($q$SELECT count(*) FROM pl WHERE body @@@ 'common'$q$) AS members;

-- 3b. A middle block claiming fewer postings than it holds. The reader advances past
-- the whole block by its byte lengths, so the run silently loses the 28 postings the
-- header no longer counts. Pre-fix: 7972, no error.
CREATE TABLE pu (id int PRIMARY KEY, body text);
INSERT INTO pu SELECT g, 'common w' || g FROM generate_series(1, 8000) g;
CREATE INDEX pu_bm ON pu USING bm25_native (body);
SELECT bm25_debug_stamp_post_block('pu_bm', 0, 0, 1, 'ndocs', 100) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT count(*) FROM pu WHERE body @@@ 'common'$q$) AS members;

-- 3c. A block claiming more postings than the run has left. 'aaa' (first in the
-- shared chain) occurs in five documents, 200 docids apart and 130 times each, so its
-- one block carries two-byte docid deltas and two-byte tfs: ten bytes of each, room
-- for a header claiming up to ten postings to pass the block's own byte-length checks.
-- Pre-fix the df bound simply stopped the decode at five: no error.
CREATE TABLE po (id int PRIMARY KEY, body text);
INSERT INTO po SELECT g, CASE WHEN g % 200 = 0 THEN repeat('aaa ', 130) ELSE 'zzz' END
  FROM generate_series(1, 1000) g;
CREATE INDEX po_bm ON po USING bm25_native (body);
SELECT count(*) AS members FROM po WHERE body @@@ 'aaa';
SELECT bm25_debug_stamp_post_block('po_bm', 0, 0, 0, 'ndocs', 6) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT count(*) FROM po WHERE body @@@ 'aaa'$q$) AS members;
SELECT pg_temp.err_of($q$SELECT id FROM po WHERE body @@@ 'aaa'
                          ORDER BY body &@@ 'aaa' LIMIT 5$q$) AS wand;

-- 3d. A link back into the run's own first page. Every block decodes and every
-- header agrees with its run. The walker's revisit test sees it first (issue #303:
-- the link is to the walk's first page); before that, only the cross-block order did:
-- the third page the walk visits starts at docid 0, below the 7167 the previous block
-- ended on.
-- The lever's no-op stamp of block 0's own ndocs (it is a full block) is just a way to
-- learn the run's first page's block number. Pre-fix: the first 832 documents twice
-- and the last 832 never, no error.
CREATE TABLE pk (id int PRIMARY KEY, body text);
INSERT INTO pk SELECT g, 'common w' || g FROM generate_series(1, 8000) g;
CREATE INDEX pk_bm ON pk USING bm25_native (body);
SELECT bm25_debug_stamp_chain_next('pk_bm', 'post', 0, 1,
         bm25_debug_stamp_post_block('pk_bm', 0, 0, 0, 'ndocs', 128)) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_seg_postings('pk_bm', 'common')$q$)
  AS postings;

-- ===================================================== 4. POST: merge must refuse
-- A merge replays every term through the same reader. Pre-fix it wrote the truncated
-- run into a new segment that no reader could fault, and the loss became permanent:
-- VACUUM's opportunistic merge and bm25_merge both succeeded, and afterwards @@@
-- returned the short count with no error at all.
CREATE TABLE pm (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO pm SELECT g, 'common w' || g FROM generate_series(1, 8000) g;
CREATE INDEX pm_bm ON pm USING bm25_native (body);
INSERT INTO pm SELECT g, 'common x' || g FROM generate_series(8001, 8100) g;
SELECT bm25_seal('pm_bm');
SELECT bm25_debug_stamp_chain_next('pm_bm', 'post', 0, 0, 4294967295) > 0 AS stamped;
-- 75% tombstoned: well over the merge trigger, so both of these select the segment.
DELETE FROM pm WHERE id <= 6000;
-- VACUUM hands bulkdelete only REMOVABLE tuples. Another backend in this database
-- holding an older snapshot (in installcheck, an autovacuum worker's ANALYZE) leaves
-- the rows deleted above "recently dead": bulkdelete never sees them and nothing is
-- tombstoned. Each VACUUM that depends on reclaiming them waits for no other backend
-- here to hold an xmin first; sql/17_delete documents the mechanism and why the wait
-- is sufficient, not just a narrower race.
CREATE FUNCTION pg_temp.wait_for_xmin_horizon() RETURNS void AS $$
DECLARE
  deadline timestamptz := clock_timestamp() + interval '30 seconds';
BEGIN
  LOOP
    PERFORM pg_stat_clear_snapshot();   -- else pg_stat_activity is cached per xact
    EXIT WHEN NOT EXISTS (
      SELECT 1 FROM pg_stat_activity
       WHERE datname = current_database()
         AND pid <> pg_backend_pid()
         AND backend_xmin IS NOT NULL);
    IF clock_timestamp() > deadline THEN
      RAISE EXCEPTION
        'xmin horizon still held by another backend after 30s; VACUUM cannot reclaim';
    END IF;
    PERFORM pg_sleep(0.01);
  END LOOP;
END
$$ LANGUAGE plpgsql;
SELECT pg_temp.wait_for_xmin_horizon();
\set VERBOSITY terse
VACUUM pm;
\set VERBOSITY default
SELECT pg_temp.err_of($q$SELECT bm25_merge('pm_bm')$q$) AS merge;
SELECT count(*) AS segments, sum(ndocs) AS docs FROM bm25_debug_segcat('pm_bm');
SELECT pg_temp.err_of($q$SELECT count(*) FROM pm WHERE body @@@ 'common'$q$) AS members;

-- ===================================================== 5. LIVE: a final page cut short
-- 3000 documents fit one LIVE page (375 bytes). The 100 'zebra' documents are deleted
-- and vacuumed (their bits cleared, their heap TIDs freed), 100 unrelated rows reuse
-- the space, and the page is then cut to 300 bytes: docids 2400 and up are past the
-- chain's end. Pre-fix those read as LIVE -- the vacuumed zebra docids came back to
-- life and @@@ 'zebra' returned whatever rows now sit at their TIDs, with no recheck.
CREATE TABLE lf (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO lf SELECT g, CASE WHEN g > 2900 THEN 'zebra' ELSE 'filler' END
  FROM generate_series(1, 3000) g;
CREATE INDEX lf_bm ON lf USING bm25_native (body);
DELETE FROM lf WHERE id > 2900;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM lf;
SELECT bm25_debug_seg_doc_live('lf_bm', 0, 2950) AS zebra_live;
SELECT count(*) AS zebra FROM lf WHERE body @@@ 'zebra';
INSERT INTO lf SELECT g, 'unrelated' FROM generate_series(100001, 100100) g;
SELECT bm25_debug_stamp_chain_lower('lf_bm', 'live', 0, 0, 300) > 0 AS stamped;
-- Healthy below the cut: a docid inside the page still reads.
SELECT bm25_debug_seg_doc_live('lf_bm', 0, 100) AS below_cut;
SELECT pg_temp.err_of($q$SELECT bm25_debug_seg_doc_live('lf_bm', 0, 2950)$q$) AS zebra_live;
SELECT pg_temp.err_of($q$SELECT count(*) FROM lf WHERE body @@@ 'zebra'$q$) AS zebra;
-- 'filler' has df 2900, so the WAND driver checks the bitmap up front
-- (seg_livedocs_all_set) rather than per lookup; that walk used to count the missing
-- bytes as set.
SELECT pg_temp.err_of($q$SELECT id FROM lf WHERE body @@@ 'filler'
                          ORDER BY body &@@ 'filler' LIMIT 5$q$) AS wand_all_set;
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_tombstone('lf_bm')$q$) AS tombstone;
-- VACUUM's own write path, reached without bulkdelete's liveness read (which now
-- raises first): the byte for docid 2500 is past the page's content. Pre-fix it was
-- read from the zeroed hole as "already tombstoned" and the tombstone was skipped.
SELECT pg_temp.err_of($q$SELECT bm25_debug_livedocs_clear('lf_bm', 0, 2500)$q$) AS clear;
-- And VACUUM itself, which reads every document's liveness before tombstoning.
DELETE FROM lf WHERE id = 2501;
SELECT pg_temp.wait_for_xmin_horizon();
\set VERBOSITY terse
VACUUM lf;
\set VERBOSITY default

-- ===================================================== 6. LIVE: merge must refuse
-- Same cut, with enough tombstones (16%) for a merge to select the segment. Pre-fix
-- the replay kept every docid past the cut as live, so the merged segment -- healthy
-- by every check -- carried the resurrected zebra documents for good.
CREATE TABLE lm (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO lm SELECT g, CASE WHEN g > 2900 THEN 'zebra' ELSE 'filler' END
  FROM generate_series(1, 3000) g;
CREATE INDEX lm_bm ON lm USING bm25_native (body);
DELETE FROM lm WHERE id > 2900 OR id <= 400;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM lm;
INSERT INTO lm SELECT g, 'unrelated' FROM generate_series(100001, 100100) g;
SELECT bm25_debug_stamp_chain_lower('lm_bm', 'live', 0, 0, 300) > 0 AS stamped;
SELECT bm25_seal('lm_bm');
SELECT pg_temp.err_of($q$SELECT bm25_merge('lm_bm')$q$) AS merge;
SELECT count(*) AS segments FROM bm25_debug_segcat('lm_bm');

-- ===================================================== 7. LIVE: two pages
-- 67000 documents take two LIVE pages (the first holds 65152 documents' bits).
-- 7a. The issue's shape: tombstones on the second page, their TIDs reused, and the
-- first page's link cut. Pre-fix the second page's documents all read as live.
CREATE TABLE l2 (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO l2 SELECT g, CASE WHEN g > 66000 THEN 'rareterm' ELSE 'alpha' END
  FROM generate_series(1, 67000) g;
CREATE INDEX l2_bm ON l2 USING bm25_native (body);
DELETE FROM l2 WHERE id > 66000;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM l2;
SELECT bm25_debug_seg_doc_live('l2_bm', 0, 66500) AS rare_live;
INSERT INTO l2 SELECT g, 'unrelated text' FROM generate_series(100001, 101000) g;
SELECT bm25_debug_stamp_chain_next('l2_bm', 'live', 0, 0, 4294967295) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT bm25_debug_seg_doc_live('l2_bm', 0, 66500)$q$) AS rare_live;
SELECT pg_temp.err_of($q$SELECT count(*) FROM l2 WHERE body @@@ 'rareterm'$q$) AS rare;
SELECT pg_temp.err_of($q$SELECT id FROM l2 WHERE body @@@ 'rareterm'
                          ORDER BY body &@@ 'rareterm' LIMIT 3$q$) AS rare_ranked;

-- 7b. The first page one byte short of the full span, its link intact. Every reader
-- addresses the second page's bits by cumulative bytes and VACUUM by a fixed
-- bits-per-page divide, so a short non-final page makes the two disagree about every
-- later document. Pre-fix nothing noticed.
CREATE TABLE l3 (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO l3 SELECT g, CASE WHEN g > 66000 THEN 'rareterm' ELSE 'alpha' END
  FROM generate_series(1, 67000) g;
CREATE INDEX l3_bm ON l3 USING bm25_native (body);
SELECT bm25_debug_stamp_chain_lower('l3_bm', 'live', 0, 0, 8143) > 0 AS stamped;
-- Below the short page's end a lookup never walks past it, so it still reads.
SELECT bm25_debug_seg_doc_live('l3_bm', 0, 100) AS first_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_seg_doc_live('l3_bm', 0, 66500)$q$) AS lookup;
SELECT pg_temp.err_of($q$SELECT id FROM l3 WHERE body @@@ 'alpha'
                          ORDER BY body &@@ 'alpha' LIMIT 3$q$) AS wand_all_set;
SELECT pg_temp.err_of($q$SELECT bm25_debug_livedocs_clear('l3_bm', 0, 66500)$q$) AS locate;

-- ===================================================== 8. DOCMAP
-- 3000 documents take three DOCMAP pages (1357 cells each). Read through
-- bm25_debug_chain_cursor_crosstalk, which returns the TID without fetching the heap,
-- so the pre-fix answer is visible instead of being handed to the executor.
CREATE TABLE dm (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO dm SELECT g, CASE WHEN g > 2900 THEN 'zebra' ELSE 'filler' END
  FROM generate_series(1, 3000) g;
CREATE INDEX dm_bm ON dm USING bm25_native (body);
CREATE TABLE dm2 AS SELECT * FROM dm;
CREATE INDEX dm2_bm ON dm2 USING bm25_native (body);
CREATE TABLE dm3 AS SELECT * FROM dm;
CREATE INDEX dm3_bm ON dm3 USING bm25_native (body);
CREATE TABLE dm4 AS SELECT * FROM dm;
CREATE INDEX dm4_bm ON dm4 USING bm25_native (body);
-- Control: the cell is the row's real ctid.
SELECT bm25_debug_chain_cursor_crosstalk('dm_bm', 0, 2950) = ctid AS real_tid
  FROM dm WHERE id = 2951;

-- 8a. A short non-final page: the first page one cell short. Pre-fix the docid read
-- the NEXT document's TID.
SELECT bm25_debug_stamp_chain_lower('dm_bm', 'docmap', 0, 0,
         (1357 - 1) * 6) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT bm25_debug_chain_cursor_crosstalk('dm_bm', 0, 2950)$q$)
  AS short_page;

-- 8b. Corrupt CELLS in an intact chain, which no chain check can see. Each must be
-- refused at the one docid -> TID chokepoint. (InvalidBlockNumber, 1) passes
-- ItemPointerIsValid, which tests only the offset.
SELECT bm25_debug_stamp_docmap_tid('dm2_bm', 0, 10, '(0,0)') > 0 AS stamped;
SELECT bm25_debug_stamp_docmap_tid('dm2_bm', 0, 20, '(1,4000)') > 0 AS stamped;
SELECT bm25_debug_stamp_docmap_tid('dm2_bm', 0, 2950, '(4294967295,1)') > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT bm25_debug_chain_cursor_crosstalk('dm2_bm', 0, 10)$q$)
  AS zero_cell;
SELECT pg_temp.err_of($q$SELECT bm25_debug_chain_cursor_crosstalk('dm2_bm', 0, 20)$q$)
  AS offset_past_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_chain_cursor_crosstalk('dm2_bm', 0, 2950)$q$)
  AS invalid_block;
-- (These cells are also read through the executor, in section 11.)
-- Healthy cells of the same chain still read.
SELECT bm25_debug_chain_cursor_crosstalk('dm2_bm', 0, 100) = ctid AS other_cell
  FROM dm2 WHERE id = 101;

-- 8c. A segment with documents but no DOCMAP chain at all, refused when the header is
-- read. Pre-fix every lookup fell through to the invalid TID.
SELECT bm25_debug_stamp_seg_root('dm3_bm', 0, 'docmap', 4294967295) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT bm25_debug_chain_cursor_crosstalk('dm3_bm', 0, 5)$q$)
  AS no_docmap;

-- 8d. The chain's link cut after its second page. Pre-fix: (4294967295,0).
SELECT bm25_debug_stamp_chain_next('dm4_bm', 'docmap', 0, 1, 4294967295) > 0 AS stamped;
SELECT bm25_debug_chain_cursor_crosstalk('dm4_bm', 0, 100) = ctid AS before_cut
  FROM dm4 WHERE id = 101;
SELECT pg_temp.err_of($q$SELECT bm25_debug_chain_cursor_crosstalk('dm4_bm', 0, 2950)$q$)
  AS past_cut;

-- ===================================================== 9. NORMS, and the LIVE root
-- 3000 documents take two NORMS pages (2036 cells each).
CREATE TABLE nm (id int PRIMARY KEY, body text);
INSERT INTO nm SELECT g, CASE WHEN g > 2900 THEN 'zebra one two' ELSE 'filler' END
  FROM generate_series(1, 3000) g;
CREATE INDEX nm_bm ON nm USING bm25_native (body);
CREATE TABLE nm2 AS SELECT * FROM nm;
CREATE INDEX nm2_bm ON nm2 USING bm25_native (body);
CREATE TABLE nm3 AS SELECT * FROM nm;
CREATE INDEX nm3_bm ON nm3 USING bm25_native (body);
-- 9a. Link cut after the first page: the zebra documents' lengths are past it.
-- Pre-fix they scored with doclen 0, silently.
SELECT bm25_debug_stamp_chain_next('nm_bm', 'norms', 0, 0, 4294967295) > 0 AS stamped;
SET bm25_native.wand_top_k = 0;
SELECT pg_temp.err_of($q$SELECT id FROM nm WHERE body @@@ 'zebra'
                          ORDER BY body &@@ 'zebra' LIMIT 5$q$) AS exhaustive;
RESET bm25_native.wand_top_k;
SELECT pg_temp.err_of($q$SELECT id FROM nm WHERE body @@@ 'zebra'
                          ORDER BY body &@@ 'zebra' LIMIT 5$q$) AS wand;
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_postings('nm_bm')$q$) AS dump;
-- @@@ reads no lengths, so membership is unaffected.
SELECT count(*) AS zebra FROM nm WHERE body @@@ 'zebra';

-- 9b. No NORMS chain, and no LIVE chain, on a segment with documents. Pre-fix: every
-- length 0, every document live.
SELECT bm25_debug_stamp_seg_root('nm2_bm', 0, 'norms', 4294967295) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT id FROM nm2 WHERE body @@@ 'zebra'
                          ORDER BY body &@@ 'zebra' LIMIT 5$q$) AS no_norms;
SELECT bm25_debug_stamp_seg_root('nm3_bm', 0, 'live', 4294967295) > 0 AS stamped;
SELECT pg_temp.err_of($q$SELECT count(*) FROM nm3 WHERE body @@@ 'zebra'$q$) AS no_live;

-- ===================================================== 10. lever arguments
\set VERBOSITY terse
SELECT bm25_debug_stamp_chain_next('nm_bm', 'post', -1, 0, 1);
SELECT bm25_debug_stamp_chain_next('nm_bm', 'nosuch', 0, 0, 1);
SELECT bm25_debug_stamp_chain_lower('nm_bm', 'norms', 0, 0, -1);
SELECT bm25_debug_stamp_chain_lower('nm_bm', 'norms', 0, 0, 100000);
SELECT bm25_debug_stamp_chain_lower('nm_bm', 'norms', 0, -1, 0);
SELECT bm25_debug_stamp_post_block('nm_bm', 0, 0, -1, 'ndocs', 1);
SELECT bm25_debug_stamp_post_block('nm_bm', 0, 0, 0, 'ndocs', 0);
SELECT bm25_debug_stamp_post_block('nm_bm', 0, 0, 0, 'last_docid', -1);
SELECT bm25_debug_stamp_post_block('nm_bm', 0, 0, 0, 'tf_bytes', 1);
SELECT bm25_debug_stamp_post_block('nm_bm', 0, 0, 100000, 'ndocs', 1);
SELECT bm25_debug_stamp_docmap_tid('nm_bm', 0, -1, '(1,1)');
SELECT bm25_debug_stamp_docmap_tid('nm_bm', 0, 3000, '(1,1)');
SELECT bm25_debug_stamp_seg_root('nm_bm', 0, 'post', 1);
SELECT bm25_debug_stamp_seg_root('nm_bm', -1, 'live', 1);
SELECT bm25_debug_stamp_seg_root('nm_bm', 0, 'live', -1);
SELECT bm25_debug_stamp_seg_root('nm_bm', 5, 'live', 1);
SELECT bm25_debug_livedocs_clear('nm_bm', -1, 0);
SELECT bm25_debug_livedocs_clear('nm_bm', 0, -1);
SELECT bm25_debug_livedocs_clear('nm_bm', 0, 3000);
SELECT bm25_debug_livedocs_clear('nm_bm', 5, 0);
\set VERBOSITY default

-- ===================================================== 11. DOCMAP, through the executor
-- Last on purpose: before the fix each of these handed an impossible TID to the
-- executor, which on an assert-enabled build aborts the backend. Kept at the end so
-- a pre-fix run loses only this section.
--
-- 11a. Section 8b's (InvalidBlockNumber, 1) cell. Pre-fix the heap fetch read
-- InvalidBlockNumber as P_NEW and EXTENDED the heap -- from a read-only query, without
-- the extension lock, once per such TID -- and an assert-enabled build then aborted in
-- heapam.c on the block number. The heap must not change size.
SELECT pg_relation_size('dm2') AS heap_before \gset
SELECT pg_temp.err_of($q$SELECT count(*) FROM dm2 WHERE body @@@ 'zebra'$q$) AS zebra;
SELECT pg_temp.err_of($q$SELECT id FROM dm2 WHERE body @@@ 'zebra'
                          ORDER BY body &@@ 'zebra' LIMIT 1000$q$) AS zebra_ranked;
SELECT pg_relation_size('dm2') = :heap_before AS heap_unchanged;

-- 11b. A cut DOCMAP chain. Pre-fix it handed (4294967295,0) to the executor:
-- indexam.c's ItemPointerIsValid Assert on @@@, TidStoreIsMember's on VACUUM.
CREATE TABLE dx (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO dx SELECT g, CASE WHEN g > 2900 THEN 'zebra' ELSE 'filler' END
  FROM generate_series(1, 3000) g;
CREATE INDEX dx_bm ON dx USING bm25_native (body);
SELECT bm25_debug_stamp_chain_next('dx_bm', 'docmap', 0, 1, 4294967295) > 0 AS stamped;
SELECT pg_relation_size('dx') AS heap_before \gset
SELECT pg_temp.err_of($q$SELECT count(*) FROM dx WHERE body @@@ 'zebra'$q$) AS zebra;
SELECT pg_temp.err_of($q$SELECT id FROM dx WHERE body @@@ 'zebra'
                          ORDER BY body &@@ 'zebra' LIMIT 5$q$) AS zebra_ranked;
DELETE FROM dx WHERE id = 2950;
SELECT pg_temp.wait_for_xmin_horizon();
\set VERBOSITY terse
VACUUM dx;
\set VERBOSITY default
SELECT pg_relation_size('dx') = :heap_before AS heap_unchanged;

RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE pc, ps, pl, pu, po, pk, pm, lf, lm, l2, l3, dm, dm2, dm3, dm4, nm, nm2, nm3, dx;
DROP EXTENSION bm25_native;
