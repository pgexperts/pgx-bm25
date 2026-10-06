-- Segment-catalog page capacity on the seal publish path (review ref C4).
--
-- Phase 2 of bm25_segment_build_and_commit appended the new BM25SegCatEntry at
-- the root SEGCAT page's pd_lower with no room check, never followed nextblk and
-- never chained a page.  A root page holds 203 entries (8144 usable content bytes
-- / MAXALIGN(40)); entry 204 was written straight through BM25PageOpaque and past
-- the end of the 8 KB buffer, leaving pd_lower > pd_upper for GenericXLogFinish
-- to WAL-log.
--
-- Nothing bounds the segment count between VACUUMs: bm25_merge_maybe runs only
-- from amvacuumcleanup and the manual bm25_merge() SQL, and
-- BM25_TARGET_SEGMENT_COUNT is a merge-selection target, not a seal-time gate.
-- So this is reachable by repeated bm25_seal(), and equally by the opportunistic
-- aminsert seal on an insert-heavy table whose autovacuum is lagging.
--
-- The reader has always followed nextblk (the merge swap builds multi-page
-- chains via bm25_segcat_build_orphan_chain); the seal was the one writer that
-- ignored the bound.  It now prepends a fresh page and flips segcat_root inside
-- the same publish record.

CREATE EXTENSION bm25_native;

CREATE TABLE segcat_t (id int primary key, body text);
CREATE INDEX segcat_bm25 ON segcat_t USING bm25_native (body);

-- 210 seals, each publishing exactly one segment: past the 203-entry root-page
-- bound with margin on both sides of it.  No VACUUM and no bm25_merge(), so
-- nothing collapses the catalog along the way.  One segment per seal is a
-- consequence of the one-row drains, not an invariant -- since #146 a drain cuts
-- its accumulator at bm25_maintenance_budget_bytes and publishes one segment per
-- chunk, which is also why the publish record appends an ARRAY of entries rather
-- than a single one.  The root-page capacity this file pins is now tested against
-- that whole array before any buffer is taken.
DO $$
BEGIN
  FOR i IN 1..210 LOOP
    INSERT INTO segcat_t VALUES (i, 'alpha doc' || i);
    PERFORM bm25_seal('segcat_bm25');
  END LOOP;
END $$;

-- Every seal published, and the catalog reads all of them back -- which means the
-- reader crossed at least one nextblk boundary.  (bm25_segcat_read ERRORs with
-- "segment catalog has N entries, expected M" if the walk comes up short.)
SELECT count(*) AS catalog_entries FROM bm25_debug_segcat('segcat_bm25');

SET enable_seqscan = off;

-- Content is intact across the page boundary: the shared token finds every doc,
-- and individual docs from before, at, and after entry 204 are all searchable.
SELECT count(*) AS all_docs FROM segcat_t WHERE body @@@ 'alpha';

SELECT id FROM segcat_t WHERE body @@@ 'doc1' ORDER BY id;
SELECT id FROM segcat_t WHERE body @@@ 'doc203' ORDER BY id;
SELECT id FROM segcat_t WHERE body @@@ 'doc204' ORDER BY id;
SELECT id FROM segcat_t WHERE body @@@ 'doc210' ORDER BY id;

-- Ranked scans read the same chained catalog.
SELECT id FROM segcat_t WHERE body @@@ 'doc205'
 ORDER BY body &@@ 'doc205' LIMIT 1;

-- Deletes must reach a segment whose catalog entry lives past the boundary.
DELETE FROM segcat_t WHERE id = 207;
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
VACUUM segcat_t;
SELECT id FROM segcat_t WHERE body @@@ 'doc207' ORDER BY id;

-- The VACUUM above ran merges (amvacuumcleanup -> bm25_merge_maybe), collapsing
-- the catalog back down.  Everything still reads.
SELECT count(*) AS after_vacuum FROM segcat_t WHERE body @@@ 'alpha';
SELECT (count(*) > 0) AS catalog_nonempty FROM bm25_debug_segcat('segcat_bm25');

RESET enable_seqscan;
DROP TABLE segcat_t;
DROP EXTENSION bm25_native;
