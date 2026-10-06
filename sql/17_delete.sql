-- End-to-end delete suite: verifies that tombstoned docs vanish from both @@@ membership
-- and &@@ ranked output, that bm25_stats reflects the deletion, and that the surviving
-- documents' ranked order is preserved.  Corpus uses distinct doclens so every &@@
-- ranked prefix has strictly distinct BM25 scores (no ties → deterministic qsort order
-- on all platforms).
--
-- Corpus design (distinct doclens break score ties):
--   doc1: 'apple banana cherry'          dl=3, apple tf=1, cherry tf=1
--   doc2: 'apple apple banana'           dl=3, apple tf=2  (top apple doc)
--   doc3: 'banana cherry date fig grape' dl=5, cherry tf=1
--   doc4: 'apple cherry cherry date'     dl=4, apple tf=1, cherry tf=2  (top cherry doc)
--   doc5: 'date date date'               dl=3, no apple/cherry
--
-- Apple scores (pre-delete): doc2 (tf=2) > doc1 (tf=1,dl=3) > doc4 (tf=1,dl=4) — distinct.
-- Cherry scores: doc4 (tf=2) > doc1 (tf=1,dl=3) > doc3 (tf=1,dl=5) — distinct.

CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
INSERT INTO docs VALUES
  (1, 'apple banana cherry'),
  (2, 'apple apple banana'),
  (3, 'banana cherry date fig grape'),
  (4, 'apple cherry cherry date'),
  (5, 'date date date');
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
SELECT bm25_seal('docs_bm25');

-- Task-16 coverage: tombstone introspection immediately after seal.
-- All 5 docs are live; none are dead yet.
SELECT
  count(*) FILTER (WHERE live)       AS live,
  count(*) FILTER (WHERE NOT live)   AS dead
FROM bm25_debug_tombstone('docs_bm25');

SET enable_seqscan = off;

-- Baseline membership: docs containing 'apple' (ids 1,2,4 in order).
SELECT id FROM docs WHERE body @@@ 'apple' ORDER BY id;

-- Baseline ranked 'apple': doc2 (tf=2) > doc1 (tf=1,dl=3) > doc4 (tf=1,dl=4).
-- Filter by @@@ first so the BM25 index scan supplies scores; then sort by &@@.
-- Scores are strictly distinct; order is deterministic.
SELECT id FROM docs WHERE body @@@ 'apple' ORDER BY body &@@ 'apple';

RESET enable_seqscan;

-- Delete doc 2 (highest-scoring 'apple' doc) and VACUUM to register the tombstone.
DELETE FROM docs WHERE id = 2;

-- Wait for the xmin horizon to clear before VACUUMing.
--
-- VACUUM only hands a TID to the AM's bulkdelete callback if the dead tuple is
-- REMOVABLE. A tuple deleted while another backend in this database holds an older
-- snapshot is merely "recently dead", so the callback never sees it, no tombstone
-- is written, and the two assertions below read 5 live / 0 dead and ndocs 5 /
-- sealed_avgdl 3.6000 instead of 4 / 1 and 4 / 3.7500. That is correct VACUUM behaviour,
-- not a bm25 bug -- but it made this suite fail roughly one run in six.
--
-- Reproduced deterministically by holding `BEGIN ISOLATION LEVEL REPEATABLE READ;
-- SELECT 1;` in a second session on this database across the DELETE. A snapshot in
-- a DIFFERENT database does NOT do it: ComputeXidHorizons derives a normal table's
-- horizon per-database. During installcheck pg_regress owns `regression`
-- exclusively, so the realistic culprit is an autovacuum worker running ANALYZE
-- (lazy VACUUM sets PROC_IN_VACUUM and is excluded from the horizon; ANALYZE is not).
--
-- Waiting here is sufficient AND permanent, not just a narrower race: once no other
-- backend holds a snapshot, any snapshot taken afterwards necessarily starts after
-- the DELETE committed, cannot see doc 2, and therefore cannot keep it un-removable.
--
-- pg_stat_clear_snapshot() is load-bearing. pg_stat_activity is materialised once
-- per transaction (stats_fetch_consistency defaults to 'cache'), so without it this
-- loop re-reads its own first sample forever and always burns the full timeout.
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

SELECT pg_temp.wait_for_xmin_horizon();

VACUUM docs;

SET enable_seqscan = off;

-- Post-delete membership: doc 2 must have vanished.
SELECT id FROM docs WHERE body @@@ 'apple' ORDER BY id;

-- Post-delete ranked 'apple': only docs 1 and 4 remain; doc2 must not appear.
SELECT id FROM docs WHERE body @@@ 'apple' ORDER BY body &@@ 'apple';

RESET enable_seqscan;

-- Task-16 coverage: tombstone introspection after delete + VACUUM.
-- 4 live, 1 dead (doc 2 tombstoned in the segment).
SELECT
  count(*) FILTER (WHERE live)       AS live,
  count(*) FILTER (WHERE NOT live)   AS dead
FROM bm25_debug_tombstone('docs_bm25');

-- Task-19 coverage: stats reflect the deletion (ndocs drops from 5 to 4;
-- sealed_avgdl shifts).  The column is the SEALED-segment average only; the seal
-- above emptied the pending list, so here it also equals the scorer's avgdl.
SELECT ndocs, round(sealed_avgdl::numeric, 4) AS sealed_avgdl
  FROM bm25_stats('docs_bm25');

SET enable_seqscan = off;

-- Survivor order preserved for 'cherry': doc4 (tf=2,dl=4) > doc1 (tf=1,dl=3) > doc3 (tf=1,dl=5).
-- Scores are strictly distinct; order is deterministic.
SELECT id FROM docs WHERE body @@@ 'cherry' ORDER BY body &@@ 'cherry' LIMIT 3;

RESET enable_seqscan;

DROP TABLE docs;
DROP EXTENSION bm25_native;
