-- 128_pending_chain_epoch: every pending page carries its chain's epoch (#291).
--
-- A scan walks the pending chain it captured long after the capture. On a hot
-- standby without feedback the primary can recycle that chain under the walk, and a
-- page re-initialized as a NEW pending page used to pass every check, so the scan
-- followed the new chain and silently lost the old chain's rows. Each pending page
-- now carries an epoch in seg_gen, drawn from the metapage's next_gen when its chain
-- starts, and every scan-side walker rejects a page whose epoch is at or above the
-- next_gen its snapshot captured (40001). The standby race itself is
-- t/025_standby_pending_recycle.pl; this suite pins the primary half:
--   PART ONE   the stamping: one epoch per chain, drawn from the gen counter that
--              segments share, so chains and segments interleave without repeats;
--   PART TWO   healthy scans over a multi-page epoch-stamped chain still see every
--              pending row, through each scan-side walker;
--   PART THREE the epoch rule itself, through the pure probe.
CREATE EXTENSION bm25_native;
SET bm25_native.seal_threshold = 4000000;
SET enable_seqscan = off;

-- ---- PART ONE: stamping ----
CREATE TABLE ep (id int, body text);
-- key_field so the ranked paths also walk the chain for the key config
-- (bm25_pending_keymeta) and the key backfill.
CREATE INDEX ep_idx ON ep USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id');

-- Every block of the current pending chain with its seg_gen, in chain order.
CREATE FUNCTION ep_chain() RETURNS TABLE (n int, epoch bigint)
LANGUAGE sql AS $$
  SELECT n, bm25_debug_page_seg_gen('ep_idx', b::int)
    FROM (SELECT n, bm25_debug_pending_nth_page('ep_idx', n) AS b
            FROM generate_series(0, 50) n) s
   WHERE b IS NOT NULL
   ORDER BY n
$$;
-- The distinct seg_gen of every live DICT page: the sealed segments' gens.
CREATE FUNCTION ep_dict_gens() RETURNS bigint[]
LANGUAGE sql AS $$
  SELECT array_agg(DISTINCT bm25_debug_page_seg_gen('ep_idx', b))
    FROM generate_series(1, (pg_relation_size('ep_idx')
                             / current_setting('block_size')::int)::int - 1) b
   WHERE bm25_debug_page_flags('ep_idx', b) & 8 <> 0
     AND bm25_debug_page_flags('ep_idx', b) & 512 = 0
$$;

-- An empty build seals nothing, so the first chain draws next_gen = 1, and every
-- page of the chain carries it, not just the head.
INSERT INTO ep SELECT g, 'alpha w' || g FROM generate_series(1, 300) g;
SELECT count(*) AS chain_pages, min(epoch) AS min_epoch, max(epoch) AS max_epoch
  FROM ep_chain();                                        -- expect >= 3 pages, all 1

-- The seal draws the next gen (2) for its segment, after the chain's epoch.
SELECT bm25_seal('ep_idx');
SELECT ep_dict_gens() AS dict_gens;                       -- expect {2}
SELECT count(*) AS chain_pages_after_seal FROM ep_chain(); -- expect 0

-- The next chain draws 3: chains and segments share the counter, so no epoch ever
-- equals a segment gen or another chain's epoch.
INSERT INTO ep SELECT g, 'beta w' || g FROM generate_series(301, 600) g;
SELECT count(*) AS chain_pages, min(epoch) AS min_epoch, max(epoch) AS max_epoch
  FROM ep_chain();                                        -- expect >= 3 pages, all 3
-- Appending to an existing chain copies its epoch; it does not draw a new one.
INSERT INTO ep SELECT g, 'beta w' || g FROM generate_series(601, 700) g;
SELECT min(epoch) AS min_epoch, max(epoch) AS max_epoch
  FROM ep_chain();                                        -- expect 3, 3

-- ---- PART TWO: healthy scans see every pending row ----
-- The beta rows (400 of them, ids 301..700) are all on the epoch-3 chain. Each
-- statement below walks it through a different scan-side walker, with the snapshot
-- bound at 4: a wrong rule (an exact match, or <= instead of <) would raise 40001
-- here on a healthy index.
SELECT count(*) AS membership FROM ep WHERE body @@@ 'beta';        -- expect 400
-- The ranked paths, both builders: corpus stats, per-term pending scoring, key
-- config and key backfill. Every row must come back and every key must resolve.
SELECT count(*) AS ranked_wand, count(bm25_score_key(id)) AS keyed_wand
  FROM (SELECT id FROM ep WHERE body @@@ 'beta'
         ORDER BY body &@@ 'beta' LIMIT 1000) s;                    -- expect 400, 400
SET bm25_native.wand_top_k = 0;
SELECT count(*) AS ranked_exhaustive, count(bm25_score_key(id)) AS keyed_exhaustive
  FROM (SELECT id FROM ep WHERE body @@@ 'beta'
         ORDER BY body &@@ 'beta') s;                               -- expect 400, 400
RESET bm25_native.wand_top_k;
-- The phrase stash and the wildcard expander's pending pass.
SELECT count(*) AS phrase
  FROM (SELECT id FROM ep WHERE body @@@ '"beta w650"'
         ORDER BY body &@@ '"beta w650"') s;                        -- expect 1
SELECT count(*) AS wildcard
  FROM (SELECT id FROM ep WHERE body @@@ bm25_wildcard('body', 'bet*')
         ORDER BY body &@@ bm25_wildcard('body', 'bet*')) s;        -- expect 400
-- The sealed alpha rows are untouched by any of it.
SELECT count(*) AS sealed FROM ep WHERE body @@@ 'alpha';           -- expect 300

-- ---- PART THREE: the epoch rule ----
-- Passes: 0 (a chain an older binary started carries no epoch, and is passed
-- rather than rejected with a 40001 no retry would clear), and anything below the
-- bound (a page written before the snapshot, including pages appended to the
-- captured chain after it, which copy the chain's older epoch).
SELECT bm25_debug_pending_epoch_validate(0, 5) AS legacy_page,
       bm25_debug_pending_epoch_validate(4, 5) AS older_page,
       bm25_debug_pending_epoch_validate(4294967294, 4294967295) AS top_of_range;
-- Fails: at or above the bound -- re-initialized after the snapshot.
SELECT bm25_debug_pending_epoch_validate(5, 5);
SELECT bm25_debug_pending_epoch_validate(6, 5);
-- The probe's own argument checks.
SELECT bm25_debug_pending_epoch_validate(-1, 5);
SELECT bm25_debug_pending_epoch_validate(1, 4294967296);

RESET enable_seqscan;
RESET bm25_native.seal_threshold;
DROP TABLE ep;
DROP FUNCTION ep_chain();
DROP FUNCTION ep_dict_gens();
DROP EXTENSION bm25_native;
