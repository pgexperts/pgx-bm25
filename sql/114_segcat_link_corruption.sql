-- 114_segcat_link_corruption -- issue #244: the SEGCAT walkers' page kind, page cap,
-- zero-entry and duplicate-entry checks.
--
-- Issue #225 (110_chain_walk_consistency) bounded where a catalog link may POINT. A link
-- that stays inside the index could still be wrong in three ways, all on-disk
-- corruption: (a) a cycle through pages that carry entries, which the two copying
-- walkers filled their entry count from, handing back duplicated segments with no
-- error; (b) a link to a page of another kind, whose bytes were copied as catalog
-- entries; (c) a cycle through pages with no entries, which never advanced the count --
-- and in bm25_scan_snapshot that spin holds the metapage content lock with interrupts
-- held, so nothing could cancel it. The two lookups (find_entry, locate_entry) have no
-- entry count at all, so any cycle kept them walking until cancelled.
--
-- Each walker now raises ERRCODE_INDEX_CORRUPTED (XX002) for each shape. The walkers are
-- driven through bm25_debug_segcat_walk; find_absent and locate_absent look up a target
-- that is never in the catalog, so they must walk the whole chain and need no catalog
-- read of their own first. The corrupt links are real pages, written by the two
-- TEST-ONLY levers bm25_debug_stamp_chain_next and bm25_debug_stamp_segcat_empty.
CREATE EXTENSION bm25_native;

-- SQLSTATE is pinned beside the message: the change is WHICH error, and for several
-- shapes the old code already raised XX002 with a message naming no link. Block and
-- generation numbers are stripped because they depend on BLCKSZ and the page layout.
-- A statement timeout is not caught here (PL/pgSQL's OTHERS excludes query_canceled),
-- so a walker that spins shows up as the top-level timeout error.
CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, '(block|generation) [0-9]+', '\1 N', 'g');
END; $$ LANGUAGE plpgsql;

-- VACUUM hands bulkdelete only REMOVABLE tuples. Another backend in this database
-- holding an older snapshot (in installcheck, an autovacuum worker's ANALYZE) leaves
-- rows deleted before a VACUUM "recently dead": bulkdelete never sees them and nothing is
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

-- ======================================================= 1. legitimate empty pages
-- The zero-entry check and the nsegs + 1 page cap rest on a packing invariant: the
-- only catalog page with no entries is the lone root of an empty catalog or, once a
-- chained publish prepends in front of it, the chain's last page. Both are reached
-- here without a lever, and every walker must accept both.
--
-- A merge whose inputs were all deleted publishes an empty catalog: one page, no
-- entries, nsegs = 0. That is the cap's tightest case: find/locate read one page with
-- nsegs = 0, which a cap of nsegs pages would reject.
CREATE TABLE ce_docs (id int PRIMARY KEY, body text);
CREATE INDEX ce_bm ON ce_docs USING bm25_native (body);
INSERT INTO ce_docs VALUES (1, 'alpha one');
SELECT bm25_seal('ce_bm');
INSERT INTO ce_docs VALUES (2, 'alpha two');
SELECT bm25_seal('ce_bm');
DELETE FROM ce_docs;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM ce_docs;
SELECT count(*) AS empty_catalog_segments FROM bm25_debug_segcat('ce_bm');
-- The empty root page exists (the lever walks to page 0 and returns its block; the
-- link it writes is the Invalid one the page already carries).
SELECT bm25_debug_stamp_chain_next('ce_bm', 'segcat', -1, 0, 4294967295) > 0
       AS empty_root_exists;
SELECT bm25_debug_segcat_walk('ce_bm', 'scan_snapshot', -1) AS snapshot_entries,
       bm25_debug_segcat_walk('ce_bm', 'segcat_read',   -1) AS read_entries,
       bm25_debug_segcat_walk('ce_bm', 'first_entry',   -1) AS first_found,
       bm25_debug_segcat_walk('ce_bm', 'find_absent',   -1) AS find_absent_found;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ce_bm', 'locate_absent', -1)$q$)
       AS locate_absent_empty_catalog;

-- A batch too large for one catalog page is laid on its own chain and prepended in
-- front of the old root, so the empty page becomes the chain's LAST page while nsegs
-- is 250. (The debug budget makes each document its own chunk, as in
-- 106_drain_memory_budget.)
INSERT INTO ce_docs
SELECT i, (SELECT string_agg('k' || i || 'x' || g, ' ') FROM generate_series(1, 400) g)
  FROM generate_series(10, 259) i;
SET bm25_native.debug_budget = '1kB';
SELECT bm25_seal('ce_bm');
RESET bm25_native.debug_budget;
-- Three pages: two carrying the 250 entries, then the empty one (idempotent stamp of
-- the tail's own Invalid link; page 3 does not exist).
SELECT bm25_debug_stamp_chain_next('ce_bm', 'segcat', -1, 2, 4294967295) > 0
       AS empty_tail_exists;
SELECT pg_temp.err_of($q$SELECT bm25_debug_stamp_chain_next('ce_bm', 'segcat', -1, 3, 1)$q$)
       AS no_fourth_page;
-- find_absent crosses the empty tail; the others stop at nsegs entries or at their match.
SELECT bm25_debug_segcat_walk('ce_bm', 'scan_snapshot', -1) AS snapshot_entries,
       bm25_debug_segcat_walk('ce_bm', 'segcat_read',   -1) AS read_entries,
       bm25_debug_segcat_walk('ce_bm', 'find_entry',    -1) AS find_found,
       bm25_debug_segcat_walk('ce_bm', 'locate_entry',  -1) AS locate_found,
       bm25_debug_segcat_walk('ce_bm', 'first_entry',   -1) AS first_found,
       bm25_debug_segcat_walk('ce_bm', 'find_absent',   -1) AS find_absent_found;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ce_bm', 'locate_absent', -1)$q$)
       AS locate_absent_empty_tail;
SET enable_seqscan = off;
SELECT count(*) AS scan_rows FROM ce_docs WHERE body @@@ 'k10x1';
RESET enable_seqscan;
DROP TABLE ce_docs;

-- ================================================ 2. a cycle through entry-bearing pages
-- A two-page catalog: a catalog page holds 203 entries and a seal PREPENDS a fresh root
-- when the current one is full, so 210 one-row seals leave 7 entries on the root and
-- 203 on the page it links to (as in 110_chain_walk_consistency).
CREATE TABLE ca_docs (id int PRIMARY KEY, body text);
CREATE INDEX ca_bm ON ca_docs USING bm25_native (body);
DO $$
BEGIN
  FOR i IN 1..210 LOOP
    INSERT INTO ca_docs VALUES (i, 'alpha doc' || i);
    PERFORM bm25_seal('ca_bm');
  END LOOP;
END $$;

-- Negative control, healthy two-page catalog.
SELECT bm25_debug_segcat_walk('ca_bm', 'scan_snapshot', -1) AS snapshot_entries,
       bm25_debug_segcat_walk('ca_bm', 'segcat_read',   -1) AS read_entries,
       bm25_debug_segcat_walk('ca_bm', 'find_entry',    -1) AS find_found,
       bm25_debug_segcat_walk('ca_bm', 'locate_entry',  -1) AS locate_found,
       bm25_debug_segcat_walk('ca_bm', 'find_absent',   -1) AS find_absent_found;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'locate_absent', -1)$q$)
       AS locate_absent_healthy;

-- Point the root at itself. The lever returns the block it stamps, so the first call
-- (a temporary truncation) yields the root's block for the second.
SELECT bm25_debug_stamp_chain_next('ca_bm', 'segcat', -1, 0, 4294967295) AS root_blk \gset
SELECT bm25_debug_stamp_chain_next('ca_bm', 'segcat', -1, 0, :root_blk) = :root_blk
       AS root_links_to_itself;

-- Backstop only: every walker below must raise long before this.
SET statement_timeout = '5s';
-- The copying walkers fill all 210 slots from the root's 7 entries in 30 visits, under
-- both the entry count and the page cap. Before the fix they returned 210 entries and
-- the scan answered from the 7 root segments alone: 7 rows instead of 210, no error.
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'scan_snapshot', -1)$q$)
       AS snapshot_cycle;
SET enable_seqscan = off;
SELECT pg_temp.err_of($q$SELECT count(*) FROM ca_docs WHERE body @@@ 'alpha'$q$)
       AS scan_cycle;
RESET enable_seqscan;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'segcat_read', -1)$q$)
       AS read_cycle;
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_segcat('ca_bm')$q$)
       AS segcat_srf_cycle;
-- The lookups have no entry count: before the fix they walked the cycle until the
-- statement timeout cancelled them. The page cap ends them now.
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'find_absent', -1)$q$)
       AS find_cycle;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'locate_absent', -1)$q$)
       AS locate_cycle;

-- ======================================================= 3. a link to another page kind
-- The same root, re-linked (page 0 is reached from the metapage, not through the
-- corrupt link). Block 0 is the metapage: its struct was copied as two catalog entries
-- and the walk then ended on the metapage's own Invalid link, so the old code reported
-- "has 9 entries, expected 210" -- corruption, but naming nothing.
SELECT bm25_debug_stamp_chain_next('ca_bm', 'segcat', -1, 0, 0) = :root_blk
       AS root_links_to_metapage;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'scan_snapshot', -1)$q$)
       AS snapshot_metapage;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'segcat_read', -1)$q$)
       AS read_metapage;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'find_absent', -1)$q$)
       AS find_metapage;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'locate_absent', -1)$q$)
       AS locate_metapage;

-- A segment DICT page (flags exactly BM25_PAGE_DICT = 8), so the check is not a
-- block-0 special case.
SELECT min(b) AS dict_blk
  FROM generate_series(1, (bm25_debug_npages('ca_bm') - 1)::int) b
 WHERE bm25_debug_page_flags('ca_bm', b) = 8 \gset
SELECT bm25_debug_stamp_chain_next('ca_bm', 'segcat', -1, 0, :dict_blk) = :root_blk
       AS root_links_to_dict_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'scan_snapshot', -1)$q$)
       AS snapshot_dict_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'segcat_read', -1)$q$)
       AS read_dict_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'find_absent', -1)$q$)
       AS find_dict_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('ca_bm', 'locate_absent', -1)$q$)
       AS locate_dict_page;
RESET statement_timeout;
DROP TABLE ca_docs;

-- ============================================================ 4. a page with no entries
-- A second two-page catalog, and the other lever: the root keeps its link to the full
-- page but reads as empty, while nsegs still says 210.
CREATE TABLE cz_docs (id int PRIMARY KEY, body text);
CREATE INDEX cz_bm ON cz_docs USING bm25_native (body);
DO $$
BEGIN
  FOR i IN 1..210 LOOP
    INSERT INTO cz_docs VALUES (i, 'alpha doc' || i);
    PERFORM bm25_seal('cz_bm');
  END LOOP;
END $$;

\set VERBOSITY terse
SELECT bm25_debug_stamp_segcat_empty('cz_bm', -1);
SELECT bm25_debug_stamp_segcat_empty('cz_bm', 2);
\set VERBOSITY default

SELECT bm25_debug_stamp_segcat_empty('cz_bm', 0) AS empty_blk \gset

SET statement_timeout = '5s';
-- Empty root, then the intact link. The check fires at the empty page, before its link
-- is followed, so what lies beyond cannot matter -- which is what lets this acyclic
-- case stand in for bm25_scan_snapshot's cyclic one below. Before the fix the copying
-- walkers reported "has 203 entries, expected 210", find_absent answered 0 and
-- locate_absent "not found in catalog".
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'scan_snapshot', -1)$q$)
       AS snapshot_empty_page;
SET enable_seqscan = off;
SELECT pg_temp.err_of($q$SELECT count(*) FROM cz_docs WHERE body @@@ 'alpha'$q$)
       AS scan_empty_page;
RESET enable_seqscan;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'segcat_read', -1)$q$)
       AS read_empty_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'find_absent', -1)$q$)
       AS find_empty_page;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'locate_absent', -1)$q$)
       AS locate_empty_page;
-- first_entry reads only the root, and meets the same check there.
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'first_entry', -1)$q$)
       AS first_empty_page;

-- Now the empty root links to itself: the spin that never advanced the count. Before
-- the fix segcat_read, find_absent and locate_absent walked it until this file's
-- statement timeout. bm25_scan_snapshot is deliberately NOT run on this shape: before
-- the fix it spun holding the metapage content lock, where interrupts are held, so
-- neither a statement timeout nor a cancel could stop it and a regression would hang
-- the suite. The acyclic case above pins the same check in that walker.
-- Known residual, left untested on purpose (#269 T1): this shape through
-- bm25_scan_snapshot or a real @@@ scan. Pre-fix it hangs under the LWLock rather
-- than failing, so the zero-entry check and the page cap are pinned through other
-- shapes and walkers instead: the acyclic empty root above, section 2's entry-bearing
-- cycle, and the three walkers below.
SELECT bm25_debug_stamp_chain_next('cz_bm', 'segcat', -1, 0, :empty_blk) = :empty_blk
       AS empty_root_links_to_itself;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'segcat_read', -1)$q$)
       AS read_empty_cycle;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'find_absent', -1)$q$)
       AS find_empty_cycle;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('cz_bm', 'locate_absent', -1)$q$)
       AS locate_empty_cycle;
RESET statement_timeout;
DROP TABLE cz_docs;

-- ================================================== 5. an empty root that ends the chain
-- Issue #270's bm25_segcat_first_entry reads only the root. An empty root with no
-- successor passes the zero-entry check (it is the shape of a legitimate empty
-- catalog), so with nsegs = 2 it must raise the entry-count error the copying walkers
-- raise, rather than return an entry the page does not hold.
CREATE TABLE c1_docs (id int PRIMARY KEY, body text);
CREATE INDEX c1_bm ON c1_docs USING bm25_native (body);
INSERT INTO c1_docs VALUES (1, 'alpha one');
SELECT bm25_seal('c1_bm');
INSERT INTO c1_docs VALUES (2, 'alpha two');
SELECT bm25_seal('c1_bm');
SELECT bm25_debug_segcat_walk('c1_bm', 'first_entry', -1) AS first_found_healthy;
SELECT bm25_debug_stamp_segcat_empty('c1_bm', 0) > 0 AS root_emptied;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('c1_bm', 'first_entry', -1)$q$)
       AS first_empty_root;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('c1_bm', 'segcat_read', -1)$q$)
       AS read_empty_root;
-- And the INSERT path that calls it. Since #292 only an index WITHOUT a key-identity
-- stamp (one a pre-#292 binary built) reaches the catalog from INSERT; a stamped one
-- checks the stamp instead. Clear the stamp so the INSERT takes that fallback.
SELECT bm25_debug_clear_keystamp('c1_bm') AS stamp_cleared;
SELECT pg_temp.err_of($q$INSERT INTO c1_docs VALUES (3, 'alpha three')$q$)
       AS insert_empty_root;
DROP TABLE c1_docs;

DROP EXTENSION bm25_native;
