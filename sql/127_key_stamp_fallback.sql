-- 127_key_stamp_fallback -- the key-identity stamp's build paths, its unstamped
-- fallback, and the drain's refusal of a mixed pending chain (#292).
--
-- sql/126 covers the behaviour users see. This suite covers the mechanism:
--   1. every build path stamps what it resolved: CREATE INDEX, CREATE INDEX
--      CONCURRENTLY, REINDEX and REINDEX CONCURRENTLY (the last two after an ALTER,
--      so the stamp visibly follows the rebuild); ambuildempty's init-fork stamp is
--      t/016's job, since only a crash reset makes the init fork the main fork.
--   2. an index with NO stamp -- what a binary predating #292 built, emulated by
--      bm25_debug_clear_keystamp -- keeps the pre-#292 check: type and width against
--      segment 0 when there is one, nothing while there is none.
--   3. the drain's backstop for that unchecked window: a pending chain mixing
--      key_field configurations is refused at seal time with a REINDEX hint, for a
--      type change at equal width and for keyless rows in a keyed chain, both of
--      which used to seal silently (key 0, or text bytes under a uuid tag).
--      REINDEX then cures the index and stamps it.
CREATE EXTENSION bm25_native;

-- ------------------------------------------------------- 1. every build path stamps
CREATE TABLE ks (id int, id8 bigint, u uuid, t text, title text, body text);
INSERT INTO ks SELECT g, g, ('00000000-0000-0000-0000-' || lpad(g::text, 12, '0'))::uuid,
                      'k' || g, 'title ' || g, 'body ' || g
  FROM generate_series(1, 20) g;

CREATE INDEX ks_plain ON ks USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
CREATE INDEX ks_keyless ON ks USING bm25_native (body) INCLUDE (id);
-- A two-field index keyed on its third column: key_attno is the INDEX position (2),
-- not the heap attnum (3).
CREATE INDEX ks_multi ON ks USING bm25_native (title, body) INCLUDE (u) WITH (key_field = 'u');
CREATE INDEX CONCURRENTLY ks_cic ON ks USING bm25_native (body) INCLUDE (id8)
  WITH (key_field = 'id8');
CREATE INDEX ks_rx ON ks USING bm25_native (body) INCLUDE (id, t) WITH (key_field = 'id');
CREATE INDEX ks_rxc ON ks USING bm25_native (body) INCLUDE (id, t) WITH (key_field = 'id');

SELECT 'ks_plain' AS ix, * FROM bm25_debug_keystamp('ks_plain')
UNION ALL SELECT 'ks_keyless', * FROM bm25_debug_keystamp('ks_keyless')
UNION ALL SELECT 'ks_multi', * FROM bm25_debug_keystamp('ks_multi')
UNION ALL SELECT 'ks_cic', * FROM bm25_debug_keystamp('ks_cic');

ALTER INDEX ks_rx SET (key_field = 't');
ALTER INDEX ks_rxc SET (key_field = 't');
REINDEX INDEX ks_rx;
REINDEX INDEX CONCURRENTLY ks_rxc;
SELECT 'ks_rx' AS ix, * FROM bm25_debug_keystamp('ks_rx')
UNION ALL SELECT 'ks_rxc', * FROM bm25_debug_keystamp('ks_rxc');

-- The rebuilt indexes take text-keyed rows and refuse int-keyed ones.
INSERT INTO ks VALUES (21, 21, NULL, 'k21', 'title 21', 'body 21');
ALTER INDEX ks_rxc SET (key_field = 'id');
INSERT INTO ks VALUES (22, 22, NULL, 'k22', 'title 22', 'body 22');
ALTER INDEX ks_rxc SET (key_field = 't');

-- ------------------------------------------------ 2. the unstamped fallback
-- (a) With segments: type/width against segment 0, as before #292.
CREATE TABLE kg (id int, id8 bigint, other int, body text);
CREATE INDEX kg_ix ON kg USING bm25_native (body) INCLUDE (id, id8, other)
  WITH (key_field = 'id');
INSERT INTO kg SELECT g, g, g + 100, 'sierra tango ' || g FROM generate_series(1, 5) g;
SELECT bm25_seal('kg_ix') IS NOT NULL AS sealed;
SELECT bm25_debug_clear_keystamp('kg_ix') AS cleared;
SELECT bm25_debug_clear_keystamp('kg_ix') AS cleared_again;   -- nothing left to clear
SELECT * FROM bm25_debug_keystamp('kg_ix');
ALTER INDEX kg_ix SET (key_field = 'id8');
-- Refused by the segment-0 comparison (note its message: no stamp to name a column).
INSERT INTO kg VALUES (6, 6, 106, 'sierra uniform');
-- The fallback cannot see a same-type column change; documented, and why REINDEX
-- (which stamps) is the upgrade advice.
ALTER INDEX kg_ix SET (key_field = 'other');
INSERT INTO kg VALUES (7, 7, 107, 'sierra victor');
ALTER INDEX kg_ix SET (key_field = 'id');

-- (b) No segment: the fallback checks nothing, so a width change gets in -- and the
-- drain refuses the resulting chain instead of the accumulator's width error.
CREATE TABLE kf (id int, id8 bigint, body text);
CREATE INDEX kf_ix ON kf USING bm25_native (body) INCLUDE (id, id8) WITH (key_field = 'id');
SELECT bm25_debug_clear_keystamp('kf_ix') AS cleared;
INSERT INTO kf SELECT g, g, 'whiskey xray ' || g FROM generate_series(1, 3) g;
ALTER INDEX kf_ix SET (key_field = 'id8');
INSERT INTO kf SELECT g, g, 'whiskey yankee ' || g FROM generate_series(4, 6) g;
SELECT bm25_seal('kf_ix');
-- REINDEX is the cure, and it stamps the index with the key_field now in force.
REINDEX INDEX kf_ix;
SELECT * FROM bm25_debug_keystamp('kf_ix');
SELECT bm25_seal('kf_ix') IS NOT NULL AS sealed;
SET enable_seqscan = off;
SELECT count(*) AS whiskey_rows FROM kf WHERE body @@@ 'whiskey';
RESET enable_seqscan;

-- ------------------------------------------------ 3. the drain's mixed-chain refusal
-- (a) Same width, different type: uuid then text, 16 bytes each. The accumulator's
-- width guard cannot see this; pre-#292 the seal published text bytes under a uuid
-- KEYMAP tag.
CREATE TABLE kt (u uuid, t text, body text);
CREATE INDEX kt_ix ON kt USING bm25_native (body) INCLUDE (u, t) WITH (key_field = 'u');
SELECT bm25_debug_clear_keystamp('kt_ix') AS cleared;
INSERT INTO kt SELECT ('00000000-0000-0000-0000-' || lpad(g::text, 12, '0'))::uuid,
                      't' || g, 'zulu alpha ' || g FROM generate_series(1, 3) g;
ALTER INDEX kt_ix SET (key_field = 't');
INSERT INTO kt SELECT ('00000000-0000-0000-0000-' || lpad(g::text, 12, '0'))::uuid,
                      't' || g, 'zulu bravo ' || g FROM generate_series(4, 6) g;
SELECT bm25_seal('kt_ix');
-- VACUUM seals too, so it hits the same refusal; restoring key_field does not help,
-- because the mixed rows are already queued.
ALTER INDEX kt_ix SET (key_field = 'u');
VACUUM kt;
SELECT count(*) AS segments FROM bm25_debug_segcat('kt_ix');

-- (b) Keyless rows in a keyed chain (keyed -> RESET). Pre-#292 they sealed as key 0.
CREATE TABLE kn (id int, body text);
CREATE INDEX kn_ix ON kn USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
SELECT bm25_debug_clear_keystamp('kn_ix') AS cleared;
INSERT INTO kn SELECT g, 'charlie delta ' || g FROM generate_series(1, 3) g;
ALTER INDEX kn_ix RESET (key_field);
INSERT INTO kn SELECT g, 'charlie echo ' || g FROM generate_series(4, 6) g;
ALTER INDEX kn_ix SET (key_field = 'id');
SELECT bm25_seal('kn_ix');
REINDEX INDEX kn_ix;
SELECT bm25_seal('kn_ix') IS NOT NULL AS sealed;
SELECT string_agg(key_int4::text, ',' ORDER BY local_docid) AS keys
  FROM bm25_debug_seg_keymap('kn_ix', 0);

-- (c) A homogeneous chain is untouched by the refusal: keyless rows only, then keyed
-- rows only, each sealed on its own index.
CREATE TABLE kh (id int, body text);
CREATE INDEX kh_keyed ON kh USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
CREATE INDEX kh_keyless ON kh USING bm25_native (body);
INSERT INTO kh SELECT g, 'foxtrot ' || g FROM generate_series(1, 4) g;
SELECT bm25_seal('kh_keyed') IS NOT NULL AS keyed_sealed,
       bm25_seal('kh_keyless') IS NOT NULL AS keyless_sealed;
DROP TABLE ks, kg, kf, kt, kn, kh;
DROP EXTENSION bm25_native;
