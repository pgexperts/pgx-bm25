-- 117_exhaustive_livedocs_checked -- the exhaustive scorer, the phrase stash, the
-- boolean/AND presence pass and the @@@ collector check a tombstone-free segment's
-- LIVEDOCS bitmap once per (term, segment) instead of once per posting, and still
-- read it per posting on a segment with a tombstone (issue #246; ADR 0100 did the
-- same for the WAND path).
--
-- This is a performance change, so nothing here returned a wrong row before it. What
-- the file pins down:
--   * the saving, as buffer accesses per posting, which fails on the pre-change build
--     and on a build where any one of the four readers still reads per posting;
--   * that a segment WITH a tombstone is still read per posting on every one of those
--     paths, observed through a reused heap line pointer: a reader that trusted a
--     tombstoned segment would hand the dead document's TID to the executor, which
--     would fetch the unrelated row now stored there.
--
-- WHY RATIOS. hit + read is the number of ReadBuffer calls, deterministic whatever is
-- cached, but a literal would move with every unrelated page-layout change. A scored
-- posting costs LIVEDOCS + DOCMAP + NORMS per lookup before the change and DOCMAP +
-- NORMS after it; a position or presence posting costs LIVEDOCS + DOCMAP before and
-- DOCMAP after; an @@@ posting likewise. The bounds sit between those.
CREATE EXTENSION bm25_native;

-- Three sealed segments (400, 400, 401 documents), one field, positions on (the
-- default). 'common' is in every document; x3 is in every seventh.
CREATE TABLE ex (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
CREATE INDEX ex_bm ON ex USING bm25_native (body);
INSERT INTO ex SELECT g, 'common x' || (g % 7) || ' y' || (g % 3) FROM generate_series(1, 400) g;
SELECT bm25_seal('ex_bm');
INSERT INTO ex SELECT g, 'common x' || (g % 7) || ' y' || (g % 3) FROM generate_series(401, 800) g;
SELECT bm25_seal('ex_bm');
INSERT INTO ex SELECT g, 'common x' || (g % 7) || ' y' || (g % 3) FROM generate_series(801, 1201) g;
SELECT bm25_seal('ex_bm');

SELECT count(*) AS segs, sum(ndocs) AS ndocs, bool_and(has_positions) AS positions,
       count(*) FILTER (WHERE live_ndocs < ndocs) AS tombstoned_segs
  FROM bm25_debug_segcat('ex_bm');

-- Buffer accesses of the top plan node. FORMAT JSON keeps the parse independent of
-- the text layout, which differs between server versions.
CREATE FUNCTION pg_temp.bufs(q text) RETURNS bigint AS $$
DECLARE
  plan json;
BEGIN
  EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q
     INTO plan;
  RETURN (plan -> 0 -> 'Plan' ->> 'Shared Hit Blocks')::bigint
       + (plan -> 0 -> 'Plan' ->> 'Shared Read Blocks')::bigint;
END
$$ LANGUAGE plpgsql;

-- wand_top_k = 0 sends the plain ranked query to the exhaustive scorer; a phrase and a
-- boolean tree go there whatever it is set to. LIMIT 5 keeps the heap fetches a
-- handful: the ranking is built whole either way.
SET enable_seqscan = off;
SET bm25_native.wand_top_k = 0;
CREATE TEMP TABLE q (name text PRIMARY KEY, sql text, postings int);
INSERT INTO q VALUES
  ('exhaustive',
   $$SELECT id FROM ex WHERE body @@@ 'common' ORDER BY body &@@ 'common' LIMIT 5$$, NULL),
  ('phrase',
   $$SELECT id FROM ex WHERE body @@@ '"common x3"' ORDER BY body &@@ '"common x3"' LIMIT 5$$, NULL),
  ('boolean_and',
   $$SELECT id FROM ex
      WHERE body @@@ bm25_boolean(must => ARRAY[bm25_term('body', 'common'), bm25_term('body', 'x3')])
      ORDER BY body &@@ bm25_boolean(must => ARRAY[bm25_term('body', 'common'),
                                                   bm25_term('body', 'x3')]) LIMIT 5$$, NULL),
  ('boolean_not',
   $$SELECT id FROM ex
      WHERE body @@@ bm25_boolean(must => ARRAY[bm25_term('body', 'common')],
                                  must_not => ARRAY[bm25_term('body', 'x3')])
      ORDER BY body &@@ bm25_boolean(must => ARRAY[bm25_term('body', 'common')],
                                     must_not => ARRAY[bm25_term('body', 'x3')]) LIMIT 5$$, NULL),
  ('filter',
   $$SELECT count(*) FROM ex WHERE body @@@ 'common'$$, NULL);
-- Postings each query decodes: every query term's whole run.
UPDATE q SET postings = (SELECT count(*) FROM ex)
               + CASE WHEN name IN ('exhaustive', 'filter') THEN 0
                      ELSE (SELECT count(*) FROM ex WHERE body LIKE '% x3 %') END;

-- Warm the relcache and catalog so the measured calls count index and heap pages only.
SELECT count(*) AS warmed FROM q, LATERAL pg_temp.bufs(q.sql) b WHERE b > 0;

-- Per posting, after the change: exhaustive ~2.0 (3.0 before); phrase and boolean_and
-- ~3.0 (5.0 before, 4.0 if the phrase stash or presence reader alone still read per
-- posting); boolean_not ~2.8 (4.6 before, 3.7 if the presence reader alone did); the
-- @@@ filter ~1.0 (2.0 before).
CREATE TEMP TABLE live_bufs AS SELECT name, postings, pg_temp.bufs(sql) AS bufs FROM q;
SELECT name, postings,
       bufs < postings * CASE name WHEN 'exhaustive' THEN 2.5
                                   WHEN 'filter' THEN 1.5
                                   WHEN 'boolean_not' THEN 3.3
                                   ELSE 3.5 END AS checked_once
  FROM live_bufs ORDER BY name;

-- The exhaustive reference probe runs the same builder: every document ranked.
SELECT count(*) AS ranked FROM bm25_debug_rank('ex_bm', 'common');

-- ------------------------------------------------------------ one tombstone per segment
-- VACUUM hands the AM only REMOVABLE tuples; see 17_delete for why this wait is needed
-- and sufficient.
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

-- id 10, 410 and 810 each hold x3, one per segment; one tombstone in 400 is far under
-- the 15% that makes a segment a merge candidate, and three segments are under the
-- merge fan-out, so VACUUM leaves the layout alone.
SELECT array_agg(ctid ORDER BY id) AS dead FROM ex WHERE id IN (10, 410, 810) \gset
DELETE FROM ex WHERE id IN (10, 410, 810);
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM ex;

SELECT count(*) AS segs, sum(ndocs) AS ndocs,
       count(*) FILTER (WHERE live_ndocs < ndocs) AS tombstoned_segs
  FROM bm25_debug_segcat('ex_bm');

-- Every segment now reads its bitmap per posting again, on every path: at least one
-- more access per posting than the tombstone-free measurement (the three fewer live
-- documents cost a handful of NORMS/DOCMAP reads, far under the slack).
SELECT l.name, pg_temp.bufs(q.sql) - l.bufs >= l.postings * 0.9 AS reads_bitmap_per_posting
  FROM live_bufs l JOIN q USING (name) ORDER BY l.name;

-- And the dead documents are not ranked (the probe returns TIDs without the heap).
SELECT count(*) AS ranked, bool_and(tid <> ALL (:'dead'::tid[])) AS dead_docs_absent
  FROM bm25_debug_rank('ex_bm', 'common');

-- ------------------------------------------------------------ a reused line pointer
-- One heap page, one segment of 20 documents. Tombstone id 10 (it holds x3), then
-- insert a row that holds none of the query terms: it takes the freed line pointer, so
-- the dead posting's TID now names a live, visible row. A reader that answered
-- liveness for that segment without reading the cleared bit would return that row from
-- every query below. Its own postings are in the pending list, and it matches none of
-- them.
CREATE TABLE ru (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
CREATE INDEX ru_bm ON ru USING bm25_native (body);
INSERT INTO ru SELECT g, 'common x' || (g % 7) || ' y' || (g % 3) FROM generate_series(1, 20) g;
SELECT bm25_seal('ru_bm');
SELECT ctid AS dead_ru FROM ru WHERE id = 10 \gset
DELETE FROM ru WHERE id = 10;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM ru;
INSERT INTO ru VALUES (1000, 'unrelated words only');

-- The witness is real: the new row sits on the dead document's TID, and the segment
-- (the only one) carries the tombstone.
SELECT ctid = :'dead_ru'::tid AS line_pointer_reused FROM ru WHERE id = 1000;
SELECT count(*) AS segs, sum(ndocs - live_ndocs) AS tombstones FROM bm25_debug_segcat('ru_bm');

SELECT array_agg(id ORDER BY id) AS exhaustive
  FROM (SELECT id FROM ru WHERE body @@@ 'x3' ORDER BY body &@@ 'x3' LIMIT 50) s;
SELECT array_agg(id ORDER BY id) AS phrase
  FROM (SELECT id FROM ru WHERE body @@@ '"common x3"' ORDER BY body &@@ '"common x3"' LIMIT 50) s;
SELECT array_agg(id ORDER BY id) AS boolean_and
  FROM (SELECT id FROM ru
         WHERE body @@@ bm25_boolean(must => ARRAY[bm25_term('body', 'common'), bm25_term('body', 'x3')])
         ORDER BY body &@@ bm25_boolean(must => ARRAY[bm25_term('body', 'common'),
                                                      bm25_term('body', 'x3')]) LIMIT 50) s;
SELECT array_agg(id ORDER BY id) AS filter FROM ru WHERE body @@@ 'x3';
-- The same sets, read from the heap.
SELECT array_agg(id ORDER BY id) AS expected_x3 FROM ru WHERE body LIKE '% x3 %';

RESET bm25_native.wand_top_k;
RESET enable_seqscan;
DROP TABLE ex, ru;
DROP EXTENSION bm25_native;
