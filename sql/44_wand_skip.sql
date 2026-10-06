-- 44_wand_skip: M2b Task 11 (C-TEST) -- gates bit-exact parity alone
-- (sql/43_wand_parity.sql) does NOT provide:
--
--   1. pruning-fires (anti-neuter): a "WAND" that always decodes every block
--      and never actually calls next_geq's header-only skip still produces
--      the exact right ranked set -- a full decode of every block is also
--      correct, just slow. bm25_wand_stats' blocks_skipped counter is a
--      witness that SOME skip path ran (cf. the M2a hollow-reclaim finding:
--      passing every output-only test is not proof a mechanism executed).
--      BUT blocks_skipped alone conflates two distinct mechanisms: plain WAND
--      pivot ALIGNMENT (advancing a lagging cursor up to the pivot docid --
--      required by ANY disjunctive WAND, block-max or not) and the block-max
--      DEEP CHECK's own shallow-skip (summing every cursor ALIGNED at the
--      pivot's block_max and skipping when the sum can't reach theta -- M2b's
--      headline pruning mechanism). Gate 1 below uses mutually EXCLUSIVE query
--      terms, so at most one cursor is ever aligned at a given pivot and the
--      deep check degenerates to a single-cursor comparison it could never
--      fail to also make correctly without the block-max machinery -- i.e.
--      Gate 1's blocks_skipped > 0 is witnessing ALIGNMENT, not the deep
--      check. Gate 1b uses CO-OCCURRING terms instead, forcing genuine
--      multi-cursor alignment, and asserts the deep-check-specific
--      deep_check_skips counter directly -- the gate a regression that
--      disables the deep check (but leaves alignment intact) would fail.
--   2. stats-drift safety: bm25_block_ub's bound is only safe if it is
--      recomputed from LIVE scan-time idf/avgdl (bm25_wand.c's header
--      comment) -- a value baked in at seal time would go stale as the
--      corpus changes. Deleting a large fraction shifts both statistics
--      away from seal-time; re-asserting WAND == exhaustive AFTER that
--      drift (not before) is what would catch a seal-time-baked bound.
--   3. merge parity: bm25_merge re-encodes every surviving segment's impact
--      tables from scratch (encode_block) -- the recomputed impacts must
--      stay a safe, exact bound too, not just the ones seal originally wrote.
--
-- Gates 1/2/3 run over the SAME corpus and the SAME two-term query, so the
-- "pruning genuinely engages" evidence (gate 1) is tied directly to the
-- query being parity-checked for drift/merge safety in gates 2/3 -- a corpus
-- too small to prune would only ever re-test the exhaustive path. Gate 1b
-- needs its own, differently-shaped corpus (co-occurrence, not mutual
-- exclusion) so it runs separately.
CREATE EXTENSION bm25_native;

-- Corpus: 'rareterm' in 1/997 docs (~20 of 20000), everything else 'common'
-- (mutually exclusive, so 'common rareterm' matches virtually every doc).
-- 'rareterm' is a far stronger BM25 signal (high idf, rare) than 'common'
-- (idf near zero, present in ~99.9% of docs); repeat('pad ', g%5) gives 5
-- doclen buckets so scores cluster with genuine ties, exercising the same
-- tid tie-break parity 43_wand_parity already pins down.
--
-- Why this construction forces a NON-pruning WAND to examine strictly more
-- blocks: theta starts low and fills from whichever docs are seen first
-- (early low-value 'common' docs), but 'rareterm's much higher per-doc score
-- soon evicts them and drives theta up to a value 'common'-only blocks
-- cannot approach (their block_max is bounded by the same low idf
-- everywhere). Once theta clears every 'common' block's max, the WAND pivot
-- skips them header-only for the remainder of the scan (via ALIGNMENT --
-- 'common' and 'rareterm' are mutually exclusive, so the two cursors are
-- never both aligned at the same pivot docid, and the block-max deep check
-- never sums more than one cursor's block_max here; see Gate 1b below for
-- the corpus that actually exercises the deep check's multi-cursor sum) --
-- a driver that instead decoded every 'common' block regardless would do
-- strictly more work for the identical answer.
CREATE TABLE sk (id int, body text);
INSERT INTO sk SELECT g,
  concat_ws(' ', CASE WHEN g % 997 = 0 THEN 'rareterm' ELSE 'common' END,
                 repeat('pad ', g % 5))
FROM generate_series(1, 20000) g;
CREATE INDEX sk_bm25 ON sk USING bm25_native (body);
SELECT bm25_seal('sk_bm25');

-- Gate 1: pivot-alignment pruning fires (anti-neuter). Proves the WAND pivot
-- skip path runs at all; it does NOT prove the block-max deep check's
-- multi-cursor sum ever engages (see the comment above and Gate 1b below).
SELECT (blocks_skipped > 0) AS pruned
FROM bm25_wand_stats('sk_bm25', 'common rareterm', 10);

-- Gate 1b: block-max DEEP CHECK pruning fires (anti-neuter for M2b's headline
-- feature specifically). 'foo' and 'bar' co-occur in EVERY matching doc here
-- (not mutually exclusive like Gate 1's terms), so both cursors' postings
-- share the identical docid sequence and are ALWAYS aligned at the same
-- pivot -- the pivot's `cur[0] < pivot` alignment branch never has anything
-- to do for this query, so any shallow-skip observed can only come from the
-- deep check's own bsum-sum-and-compare (bm25_wand.c's `bsum < theta`
-- branch), not alignment.
--
-- A handful of "heavy" docs (id 1..20) repeat both terms 20x each in a short
-- doc; being the lowest ids, they land in the FIRST block of both terms'
-- postings lists with a correspondingly high block_max, fill the top-10 heap
-- almost immediately, and drive theta up to a value only they can reach. The
-- remaining ~20000 "light" docs repeat each term once in a long (30-pad-token)
-- doc -- low tf, low block_max, well below that theta -- so every LATER
-- block's bsum (the light block_max from BOTH aligned cursors, summed) falls
-- below theta and the deep check shallow-skips them together.
CREATE TABLE skc (id int, body text);
INSERT INTO skc SELECT g,
  CASE WHEN g <= 20 THEN repeat('foo ', 20) || repeat('bar ', 20)
       ELSE 'foo bar ' || repeat('pad ', 30) END
FROM generate_series(1, 20000) g;
CREATE INDEX skc_bm25 ON skc USING bm25_native (body);
SELECT bm25_seal('skc_bm25');

SELECT (deep_check_skips > 0) AS deep_check_fired
FROM bm25_wand_stats('skc_bm25', 'foo bar', 10);

DROP TABLE skc;

-- Gate 2: stats-drift safety. Delete a third of the corpus (shifts avgdl AND
-- every remaining term's idf away from seal-time), VACUUM, then compare
-- WAND's full ordered top-30 id list against the exhaustive scan's -- an
-- array_agg equality, not a count, so a single dropped or reordered doc at
-- the rareterm/common score-tier boundary fails the gate.
DELETE FROM sk WHERE id % 3 = 0;
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
VACUUM sk;

SET enable_seqscan = off;
SET bm25_native.wand_top_k = 100;
WITH w AS (SELECT id FROM sk WHERE body @@@ 'common rareterm'
             ORDER BY body &@@ 'common rareterm' LIMIT 30)
SELECT array_agg(id) AS ids FROM w \gset wand_

SET bm25_native.wand_top_k = 0;
WITH e AS (SELECT id FROM sk WHERE body @@@ 'common rareterm'
             ORDER BY body &@@ 'common rareterm' LIMIT 30)
SELECT array_agg(id) AS ids FROM e \gset exh_

SELECT :'wand_ids'::int[] = :'exh_ids'::int[] AS drift_parity_holds;

-- Pruning must still genuinely engage post-drift, not merely "still correct
-- because nothing was ever skipped in the first place".
SELECT (blocks_skipped > 0) AS pruned_after_drift
FROM bm25_wand_stats('sk_bm25', 'common rareterm', 10);

-- Gate 3: merge parity. Force a merge (impacts recomputed from scratch for
-- every surviving segment), then re-run the identical array_agg equality.
SELECT bm25_merge('sk_bm25');

SET bm25_native.wand_top_k = 100;
WITH w AS (SELECT id FROM sk WHERE body @@@ 'common rareterm'
             ORDER BY body &@@ 'common rareterm' LIMIT 30)
SELECT array_agg(id) AS ids FROM w \gset wandmerge_

SET bm25_native.wand_top_k = 0;
WITH e AS (SELECT id FROM sk WHERE body @@@ 'common rareterm'
             ORDER BY body &@@ 'common rareterm' LIMIT 30)
SELECT array_agg(id) AS ids FROM e \gset exhmerge_

SELECT :'wandmerge_ids'::int[] = :'exhmerge_ids'::int[] AS merge_parity_holds;

SELECT (blocks_skipped > 0) AS pruned_after_merge
FROM bm25_wand_stats('sk_bm25', 'common rareterm', 10);

RESET bm25_native.wand_top_k;
RESET enable_seqscan;
DROP TABLE sk;

DROP EXTENSION bm25_native;
