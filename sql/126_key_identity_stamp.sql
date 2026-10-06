-- 126_key_identity_stamp -- a key_field change is refused at INSERT even while the
-- index has no segments (#292).
--
-- key_field is structural: it fixes the type and width of every KEYMAP and what each
-- stored key means. It is also an ordinary reloption, so ALTER INDEX ... SET
-- (key_field = ...) is accepted, and bm25_insert re-resolves it on every row. The
-- INSERT-time gate (ADR 0067) used to compare a row only against segment 0's KEYMAP
-- header. With no segment -- an index created on an empty table and then loaded, until
-- its first seal -- it compared against nothing, so every change below went into the
-- pending chain:
--
--   width change (int4 -> int8)   every seal and VACUUM failed; reverting did not help
--   keyed -> RESET -> keyed       sealed silently with key 0 for the keyless rows
--   keyless -> SET key_field      sealed silently with key 0 for the pre-ALTER rows
--   int4 column A -> column B     sealed silently with the other column's values
--   uuid -> text (same width)     sealed text bytes under a uuid KEYMAP tag
--
-- The build now stamps the resolved (key_type, key_size, key_attno) on the
-- field-config page and every INSERT is compared with it, so each of those rows is
-- refused before it reaches the chain: nothing is mis-keyed, the seal still works,
-- and restoring key_field recovers with no REINDEX.
--
-- HOW THIS FAILS PRE-FIX. Every refused INSERT below was accepted, so the outputs
-- differ at the first refusal of each part; part 1's seal then raised the width
-- error, and parts 2-3 sealed key 0 into the keymaps the suite pins. No new debug
-- probe is used here (sql/127 has those), so the suite runs unchanged against the
-- pre-fix library.
--
-- Every part starts on an EMPTY catalog, pinned by a segment count of 0: the pre-fix
-- gate only had something to compare against once a segment existed, and a part that
-- sealed first would pass against the broken build.
CREATE EXTENSION bm25_native;

-- Shows a segment's keys in docid order, so a mis-keyed row is visible as a 0 or as
-- the wrong column's value.
CREATE FUNCTION keys_of(ix regclass) RETURNS TABLE (seg int, keys text) LANGUAGE sql AS $$
  SELECT s, string_agg(coalesce(k.key_int4::text, k.key_int8::text, k.key_uuid::text,
                                k.key_text), ',' ORDER BY k.local_docid)
    FROM generate_series(0, (SELECT count(*) FROM bm25_debug_segcat(ix))::int - 1) s,
         LATERAL bm25_debug_seg_keymap(ix, s) k
   GROUP BY s ORDER BY s
$$;

-- ------------------------------------------------------------ 1. width change
CREATE TABLE kw (id int, id8 bigint, body text);
CREATE INDEX kw_ix ON kw USING bm25_native (body) INCLUDE (id, id8) WITH (key_field = 'id');
INSERT INTO kw SELECT g, g + 1000, 'alpha bravo ' || g FROM generate_series(1, 5) g;
SELECT count(*) AS segments FROM bm25_debug_segcat('kw_ix');

ALTER INDEX kw_ix SET (key_field = 'id8');
-- Refused: the index was built keyed on the int4 column.
INSERT INTO kw SELECT g, g + 1000, 'alpha charlie ' || g FROM generate_series(6, 10) g;

-- Nothing divergent reached the chain, so neither the seal nor VACUUM's seal wedges
-- while the reloption is still wrong.
SELECT bm25_seal('kw_ix') IS NOT NULL AS sealed;
VACUUM kw;

-- Restoring key_field is enough; no REINDEX.
ALTER INDEX kw_ix SET (key_field = 'id');
INSERT INTO kw SELECT g, g + 1000, 'alpha charlie ' || g FROM generate_series(6, 10) g;
SELECT bm25_seal('kw_ix') IS NOT NULL AS sealed;
SELECT * FROM keys_of('kw_ix');

-- REINDEX is the supported way to change key_field: it rebuilds from the heap under
-- the new value and restamps, after which rows keyed on the new column are accepted.
ALTER INDEX kw_ix SET (key_field = 'id8');
REINDEX INDEX kw_ix;
INSERT INTO kw SELECT g, g + 1000, 'alpha delta ' || g FROM generate_series(11, 12) g;
SELECT bm25_seal('kw_ix') IS NOT NULL AS sealed;
SELECT * FROM keys_of('kw_ix');
-- ...and the old column is now the refused one.
ALTER INDEX kw_ix SET (key_field = 'id');
INSERT INTO kw VALUES (13, 1013, 'alpha echo');
ALTER INDEX kw_ix SET (key_field = 'id8');
SET enable_seqscan = off;
SELECT count(*) AS alpha_rows FROM kw WHERE body @@@ 'alpha';
RESET enable_seqscan;

-- ------------------------------------------------- 2. keyed -> RESET -> keyed
CREATE TABLE kr (id int, body text);
CREATE INDEX kr_ix ON kr USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
INSERT INTO kr SELECT g, 'golf hotel ' || g FROM generate_series(1, 5) g;
SELECT count(*) AS segments FROM bm25_debug_segcat('kr_ix');
ALTER INDEX kr_ix RESET (key_field);
-- Refused: a keyless row in a keyed index would be sealed with key 0.
INSERT INTO kr SELECT g, 'golf india ' || g FROM generate_series(6, 10) g;
ALTER INDEX kr_ix SET (key_field = 'id');
INSERT INTO kr SELECT g, 'golf india ' || g FROM generate_series(6, 10) g;
SELECT bm25_seal('kr_ix') IS NOT NULL AS sealed;
-- Ten rows, every key its own id: no 0 anywhere.
SELECT * FROM keys_of('kr_ix');

-- ------------------------------------------------- 3. keyless -> SET key_field
CREATE TABLE kl (id int, body text);
-- INCLUDE (id) with NO key_field: keyless, but 'id' resolves, so the ALTER below gets
-- past bm25_resolve_fields' column check and reaches the gate.
CREATE INDEX kl_ix ON kl USING bm25_native (body) INCLUDE (id);
INSERT INTO kl SELECT g, 'juliet kilo ' || g FROM generate_series(1, 5) g;
SELECT count(*) AS segments FROM bm25_debug_segcat('kl_ix');
ALTER INDEX kl_ix SET (key_field = 'id');
-- Refused: a keyed row in a keyless index.
INSERT INTO kl SELECT g, 'juliet lima ' || g FROM generate_series(6, 10) g;
ALTER INDEX kl_ix RESET (key_field);
INSERT INTO kl SELECT g, 'juliet lima ' || g FROM generate_series(6, 10) g;
SELECT bm25_seal('kl_ix') IS NOT NULL AS sealed;
-- Still keyless: the probe's single summary row reports key type 0, no KEYMAP chain.
SELECT key_type, key_size, local_docid IS NULL AS no_docs
  FROM bm25_debug_seg_keymap('kl_ix', 0);

-- ------------------------------------- 4. same type, different column (int4 -> int4)
CREATE TABLE kc (id int, other int, body text);
CREATE INDEX kc_ix ON kc USING bm25_native (body) INCLUDE (id, other) WITH (key_field = 'id');
INSERT INTO kc SELECT g, g + 100, 'mike november ' || g FROM generate_series(1, 5) g;
SELECT count(*) AS segments FROM bm25_debug_segcat('kc_ix');
ALTER INDEX kc_ix SET (key_field = 'other');
-- Refused on the empty catalog: same type and width, different column.
INSERT INTO kc SELECT g, g + 100, 'mike oscar ' || g FROM generate_series(6, 10) g;
ALTER INDEX kc_ix SET (key_field = 'id');
SELECT bm25_seal('kc_ix') IS NOT NULL AS sealed;
SELECT count(*) AS segments FROM bm25_debug_segcat('kc_ix');
-- And refused WITH segments present, too. A KEYMAP header records type and width but
-- not the column, so this was ADR 0067's accepted residual: the row was admitted and
-- bm25_score_key answered for the other column's values.
ALTER INDEX kc_ix SET (key_field = 'other');
INSERT INTO kc SELECT g, g + 100, 'mike oscar ' || g FROM generate_series(6, 10) g;
ALTER INDEX kc_ix SET (key_field = 'id');
INSERT INTO kc SELECT g, g + 100, 'mike oscar ' || g FROM generate_series(6, 10) g;
SELECT bm25_seal('kc_ix') IS NOT NULL AS sealed;
SELECT * FROM keys_of('kc_ix');

-- -------------------------------------- 5. same width, different type (uuid -> text)
CREATE TABLE ku (u uuid, t text, body text);
CREATE INDEX ku_ix ON ku USING bm25_native (body) INCLUDE (u, t) WITH (key_field = 'u');
INSERT INTO ku SELECT ('00000000-0000-0000-0000-' || lpad(g::text, 12, '0'))::uuid,
                      'text-key-' || g, 'papa quebec ' || g
  FROM generate_series(1, 3) g;
SELECT count(*) AS segments FROM bm25_debug_segcat('ku_ix');
ALTER INDEX ku_ix SET (key_field = 't');
-- Refused: both keys are 16 bytes wide, so only the type tells them apart.
INSERT INTO ku SELECT ('00000000-0000-0000-0000-' || lpad(g::text, 12, '0'))::uuid,
                      'text-key-' || g, 'papa romeo ' || g
  FROM generate_series(4, 6) g;
ALTER INDEX ku_ix SET (key_field = 'u');
SELECT bm25_seal('ku_ix') IS NOT NULL AS sealed;
SELECT * FROM keys_of('ku_ix');
DROP FUNCTION keys_of(regclass);
DROP TABLE kw, kr, kl, kc, ku;
DROP EXTENSION bm25_native;
