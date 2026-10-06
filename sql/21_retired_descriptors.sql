-- 21_retired_descriptors — the retired-list DESCRIPTOR pages stay bounded across many
-- merges. The merge swap prepends one fresh BM25_PAGE_RETIRED descriptor page per merge;
-- bm25_reclaim_retired must unlink + free a descriptor page once it drains to zero
-- entries (else the retired chain grows one ~8 KB page per merge forever — the data
-- pages it described ARE reclaimed, but the descriptor page itself would leak).
--
-- Drive 8 merge cycles (each builds four same-layer 50-doc segments and merges them →
-- one descriptor page per cycle), then burn XIDs so the cluster horizon passes every
-- captured retire_xid and VACUUM so reclaim drains the entries AND unlinks the emptied
-- non-head descriptors. With the fix the chain collapses to O(1) pages; without it
-- bm25_debug_retired_pages would read ~10. (See 20_merge_reclaim for the xid-burn
-- rationale: retire_xid is a future xid only a real write-txn horizon advance clears.)
CREATE EXTENSION bm25_native;

-- Waits until no other backend in this database holds a snapshot -- needed before
-- the XID burn below, since a held snapshot pins the horizon regardless of how many
-- xids get burned. See docs/adr/0031-vacuum-tests-wait-for-xmin-horizon.md for the
-- full rationale (and 20_merge_reclaim for why wait precedes burn).
CREATE FUNCTION pg_temp.wait_for_xmin_horizon() RETURNS void AS $$
DECLARE
  deadline timestamptz := clock_timestamp() + interval '30 seconds';
BEGIN
  LOOP
    PERFORM pg_stat_clear_snapshot();
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

CREATE TABLE docs (id int primary key, body text);
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
SET enable_seqscan = off;
DO $$
DECLARE k int; b int;
BEGIN
  FOR k IN 1..8 LOOP
    b := 100000 * k;
    INSERT INTO docs SELECT b+g,     'alpha database storage' FROM generate_series(1,50) g; PERFORM bm25_seal('docs_bm25');
    INSERT INTO docs SELECT b+100+g, 'beta database storage'  FROM generate_series(1,50) g; PERFORM bm25_seal('docs_bm25');
    INSERT INTO docs SELECT b+200+g, 'gamma database storage' FROM generate_series(1,50) g; PERFORM bm25_seal('docs_bm25');
    INSERT INTO docs SELECT b+300+g, 'delta database storage' FROM generate_series(1,50) g; PERFORM bm25_seal('docs_bm25');
    PERFORM bm25_merge('docs_bm25');
  END LOOP;
END $$;
-- Wait before burning (see 20_merge_reclaim): a held snapshot pins the horizon no
-- matter how many xids get burned afterward, so clear it first.
SELECT pg_temp.wait_for_xmin_horizon();
-- Advance the cluster horizon past every merge's retire_xid (separate autocommit txns),
-- then VACUUM twice so reclaim runs with the horizon cleared.
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset
VACUUM docs;
VACUUM docs;
-- All retired ENTRIES are reclaimed (horizon cleared) ...
SELECT bm25_debug_retired_count('docs_bm25') AS retired_entries;
-- ... and the descriptor PAGES are bounded (emptied non-head descriptors were unlinked
-- and freed). Without the unlink this would be ~10 (one per merge); the <= 2 bound fails
-- loudly in that case while staying robust to merge-policy timing.
SELECT bm25_debug_retired_pages('docs_bm25') <= 2 AS descriptors_bounded;
-- Correctness preserved across all the merge/reclaim churn: every doc still matches.
SELECT count(*) AS matching FROM docs WHERE body @@@ 'database';
RESET enable_seqscan;
DROP TABLE docs;
DROP EXTENSION bm25_native;
