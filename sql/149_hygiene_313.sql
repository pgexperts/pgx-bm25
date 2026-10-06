-- 149_hygiene_313 -- the behavioural items of issue #313's smell sweep (and #312 item
-- 3's SQLSTATE). Every block fails against the pre-fix build; the comment, dead-code and
-- errdetail-wording items have no behaviour to pin and are not here.
--
--   1. open helpers: a table, OID 0 or a dangling OID reached IndexGetRelation or
--      index_open and raised XX000; now 42809 / 42704 (META-06/SURFACE-05)
--   2. a POS page with no content raised XX001 (DATA_CORRUPTED), the one corruption
--      site off XX002 (XCUT-12, #312 item 3)
--   3. the over-pull tail rebuild's analyzer-drift check raised XX000; concurrent DDL
--      reaches it, so it is 55000 now (XCUT-12)
--   4. next_gen: drawing PG_UINT32_MAX wrapped the counter to the 0 sentinel; both draw
--      sites (a pending chain start and a seal) refuse it now, and 0 is corrupt
--      (META-08)
--   5. a segment header whose ndocs exceeds PG_UINT32_MAX is refused at the header read
--      (REGR-06 / HDL-12)
--   6. debug probes: bm25_debug_score's negative counts (SCORE-08), bm25_debug_topk's
--      offsets (SURFACE-10), bm25_debug_terms on a term of 64 bytes or more
--      (SEGREAD-06), bm25_debug_seg_keymap's key_text cut mid-character (META-09), an
--      over-long language name in the tokenizer probes (TEXT-10)
--   7. an Invalid field_config_blkno raised 55000 from the field-config readers, a
--      "prerequisite state" code for what is corruption in every readable format
--      (XCUT-12 inventory); now XX002
--
-- Corruption is forged with bm25_debug_poke_page aimed through bm25_debug_layout. The
-- forged values are endian-symmetric (all-ones, all-zeros, \x1f1f) except one pd_lower,
-- whose byte order is learned from the metapage magic.
CREATE EXTENSION bm25_native;

-- SQLSTATE beside the message; block, generation and OID numbers depend on the layout.
CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' ||
           regexp_replace(SQLERRM, '(block|page|generation|OID) [0-9]+', '\1 N', 'g');
END; $$ LANGUAGE plpgsql;

CREATE FUNCTION pg_temp.off(s text, f text) RETURNS int AS $$
    SELECT off FROM bm25_debug_layout() WHERE struct = s AND field = f;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.meta_off(f text) RETURNS int AS $$
    SELECT pg_temp.off('page', 'contents') + pg_temp.off('BM25MetaPageData', f);
$$ LANGUAGE sql;
-- The first block of a kind (and, for segment pages, of a generation > 0).
CREATE FUNCTION pg_temp.first_blk(idx regclass, kind int) RETURNS int AS $$
    SELECT min(b) FROM generate_series(0, bm25_debug_npages(idx)::int - 1) b
     WHERE bm25_debug_page_flags(idx, b) = kind AND bm25_debug_page_seg_gen(idx, b) > 0;
$$ LANGUAGE sql;

SET enable_seqscan = off;

-- ================================================================ 1. open helpers
-- The readable gate (bm25_stats, and a debug SRF) and the owned gate (bm25_seal), on a
-- table, on OID 0 and on an OID that names nothing. As superuser the ownership check
-- is skipped, so the relkind check is all that stands in front of index_open.
CREATE TABLE o_docs (id int PRIMARY KEY, body text);
INSERT INTO o_docs VALUES (1, 'alpha');
CREATE INDEX o_bm ON o_docs USING bm25_native (body);
SELECT pg_temp.err_of($q$SELECT bm25_stats('o_docs')$q$)          AS stats_table;
SELECT pg_temp.err_of($q$SELECT bm25_stats(0::regclass)$q$)       AS stats_oid0;
SELECT pg_temp.err_of($q$SELECT bm25_stats(1::regclass)$q$)       AS stats_oid1;
SELECT pg_temp.err_of($q$SELECT bm25_debug_npages(1::regclass)$q$) AS debug_oid1;
SELECT pg_temp.err_of($q$SELECT bm25_seal('o_docs')$q$)           AS seal_table;
SELECT pg_temp.err_of($q$SELECT bm25_seal(1::regclass)$q$)        AS seal_oid1;
-- An index of another access method still gets the AM-identity message, and a real
-- bm25 index still opens.
SELECT pg_temp.err_of($q$SELECT bm25_stats('o_docs_pkey')$q$)     AS stats_btree;
SELECT pg_temp.err_of($q$SELECT bm25_stats('o_bm')$q$)            AS stats_bm25;
DROP TABLE o_docs;

-- ================================================================ 2. empty POS page
-- Zero the content of the segment's first POS page, then lower its pd_lower onto the
-- header: two pokes, because the lever refuses a pd_lower that would leave live bytes
-- in the hole. A phrase query then opens the position chain on it.
CREATE TABLE p_docs (id int PRIMARY KEY, body text);
INSERT INTO p_docs SELECT g, 'alpha beta gamma ' || g FROM generate_series(1, 40) g;
CREATE INDEX p_bm ON p_docs USING bm25_native (body);
-- Byte order, from the magic 0x424D3235: little-endian stores 0x35 first.
CREATE TEMP TABLE endian AS
SELECT get_byte(old, 0) = 53 AS le, old
  FROM (SELECT bm25_debug_poke_page('p_bm', 0, pg_temp.meta_off('magic'), '\x00000000') AS old) s;
SELECT length(bm25_debug_poke_page('p_bm', 0, pg_temp.meta_off('magic'), old)) AS magic_restored
  FROM endian;
-- The POS page's pd_lower, read by raising it to 0x1f1f (any order) and restoring it.
CREATE TEMP TABLE pos_page AS
SELECT pg_temp.first_blk('p_bm', 4096) AS blk;
CREATE TEMP TABLE pos_lower AS
SELECT bm25_debug_poke_page('p_bm', blk, pg_temp.off('PageHeaderData', 'pd_lower'), '\x1f1f') AS old
  FROM pos_page;
SELECT length(bm25_debug_poke_page('p_bm', p.blk, pg_temp.off('PageHeaderData', 'pd_lower'), l.old))
       AS lower_restored
  FROM pos_page p, pos_lower l;
SELECT pg_temp.err_of($q$SELECT count(*) FROM p_docs WHERE body @@@ '"alpha beta"'$q$) AS phrase_healthy;
SELECT length(bm25_debug_poke_page('p_bm', p.blk, pg_temp.off('page', 'contents'),
              decode(repeat('00', (CASE WHEN e.le THEN get_byte(l.old, 0) + 256 * get_byte(l.old, 1)
                                        ELSE get_byte(l.old, 1) + 256 * get_byte(l.old, 0) END)
                                  - pg_temp.off('page', 'contents')), 'hex'))) > 0 AS content_zeroed
  FROM pos_page p, pos_lower l, endian e;
SELECT length(bm25_debug_poke_page('p_bm', p.blk, pg_temp.off('PageHeaderData', 'pd_lower'),
              CASE WHEN e.le
                   THEN set_byte(set_byte('\x0000'::bytea, 0, pg_temp.off('page', 'contents')), 1, 0)
                   ELSE set_byte(set_byte('\x0000'::bytea, 1, pg_temp.off('page', 'contents')), 0, 0)
              END)) AS lower_on_header
  FROM pos_page p, endian e;
-- Pre-fix: XX001.
SELECT pg_temp.err_of($q$SELECT count(*) FROM p_docs WHERE body @@@ '"alpha beta"'$q$) AS phrase_empty_pos;
DROP TABLE p_docs;

-- ================================================================ 3. tail-rebuild drift
-- wand_top_k = 2 makes the third row come from the over-pull tail rebuild. Between the
-- second and third FETCH the index's dictionary gains a stoplist (ALTER TEXT SEARCH
-- DICTIONARY takes no index lock), so the rebuild analyzes 'the quick' to one term
-- where the build it continues pinned two. require_analyzer_match = false lets the
-- scan run past the fingerprint gate, whose WARNING (it prints fingerprints) is muted.
CREATE TEXT SEARCH DICTIONARY pg_catalog.h313_stem (TEMPLATE = snowball, Language = english);
CREATE TABLE t_docs (id int PRIMARY KEY, body text);
INSERT INTO t_docs SELECT g, 'the quick fox ' || repeat('x ', g % 5) FROM generate_series(1, 12) g;
CREATE INDEX t_bm ON t_docs USING bm25_native (body)
  WITH (language = 'h313', require_analyzer_match = false);
CREATE TEMP TABLE drift (rows_before_error int, sqlstate text, message text);
SET bm25_native.wand_top_k = 2;
SET client_min_messages = error;
DO $$
DECLARE
    c refcursor;
    r record;
    n int := 0;
BEGIN
    OPEN c FOR SELECT id FROM t_docs WHERE body @@@ 'the quick' ORDER BY body &@@ 'the quick';
    FETCH c INTO r; n := n + 1;
    FETCH c INTO r; n := n + 1;
    EXECUTE 'ALTER TEXT SEARCH DICTIONARY pg_catalog.h313_stem (StopWords = english)';
    LOOP
        FETCH c INTO r;
        EXIT WHEN NOT FOUND;
        n := n + 1;
    END LOOP;
    INSERT INTO drift VALUES (n, 'none', 'NO ERROR RAISED');
EXCEPTION WHEN OTHERS THEN
    INSERT INTO drift VALUES (n, SQLSTATE, SQLERRM);
END $$;
RESET client_min_messages;
RESET bm25_native.wand_top_k;
-- Pre-fix: XX000.
SELECT * FROM drift;
DROP TABLE t_docs;
DROP TEXT SEARCH DICTIONARY pg_catalog.h313_stem;

-- ================================================================ 4. next_gen exhaustion
-- CREATE INDEX leaves the pending list empty, so the first INSERT starts a chain and
-- draws an epoch from next_gen; a seal draws a segment gen. Pre-fix both succeeded and
-- stored 0 back into the counter.
CREATE TABLE g_docs (id int PRIMARY KEY, body text);
INSERT INTO g_docs SELECT g, 'alpha ' || g FROM generate_series(1, 5) g;
CREATE INDEX g_bm ON g_docs USING bm25_native (body);
SELECT bm25_debug_pending_head('g_bm') IS NULL AS pending_empty;
CREATE TEMP TABLE next_gen AS
SELECT bm25_debug_poke_page('g_bm', 0, pg_temp.meta_off('next_gen'), '\xffffffff') AS old;
SELECT pg_temp.err_of($q$INSERT INTO g_docs VALUES (100, 'alpha chain start')$q$) AS chain_start_at_max;
SELECT bm25_debug_pending_head('g_bm') IS NULL AS still_empty;
-- Restored, the insert starts the chain; at the maximum again, the seal refuses.
SELECT length(bm25_debug_poke_page('g_bm', 0, pg_temp.meta_off('next_gen'), old)) AS restored
  FROM next_gen;
INSERT INTO g_docs VALUES (101, 'alpha pending');
SELECT bm25_debug_poke_page('g_bm', 0, pg_temp.meta_off('next_gen'), '\xffffffff')
       <> '\xffffffff'::bytea AS counter_advanced;
SELECT pg_temp.err_of($q$SELECT bm25_seal('g_bm')$q$) AS seal_at_max;
-- 0 is not an exhausted counter but a corrupt one: it starts at 1 and never wraps now.
SELECT length(bm25_debug_poke_page('g_bm', 0, pg_temp.meta_off('next_gen'), '\x00000000')) AS zeroed;
SELECT pg_temp.err_of($q$SELECT bm25_seal('g_bm')$q$) AS seal_at_zero;
-- Back to a live value, the seal succeeds and the rows are found.
SELECT length(bm25_debug_poke_page('g_bm', 0, pg_temp.meta_off('next_gen'), old)) AS restored
  FROM next_gen;
SELECT pg_temp.err_of($q$SELECT bm25_seal('g_bm')$q$) AS seal_restored;
SELECT array_agg(id ORDER BY id) AS found FROM g_docs WHERE body @@@ 'alpha';
DROP TABLE g_docs;

-- ================================================================ 5. segment ndocs bound
-- Local docids are uint32, so no segment holds more than PG_UINT32_MAX documents. Pre-fix
-- the header read accepted it and a later check reported something else.
CREATE TABLE n_docs (id int PRIMARY KEY, body text);
INSERT INTO n_docs SELECT g, 'alpha ' || g FROM generate_series(1, 5) g;
CREATE INDEX n_bm ON n_docs USING bm25_native (body);
CREATE TEMP TABLE seg_ndocs AS
SELECT bm25_debug_poke_page('n_bm', pg_temp.first_blk('n_bm', 4),
                            pg_temp.off('page', 'contents') + pg_temp.off('BM25SegmentHeader', 'ndocs'),
                            '\xffffffffffffffff') AS old;
SELECT pg_temp.err_of($q$SELECT count(*) FROM n_docs WHERE body @@@ 'alpha'$q$) AS scan;
SELECT length(bm25_debug_poke_page('n_bm', pg_temp.first_blk('n_bm', 4),
                            pg_temp.off('page', 'contents') + pg_temp.off('BM25SegmentHeader', 'ndocs'),
                            old)) AS restored
  FROM seg_ndocs;
SELECT count(*) AS found FROM n_docs WHERE body @@@ 'alpha';
DROP TABLE n_docs;

-- ================================================================ 6. debug probes
-- bm25_debug_score: negative counts used to be cast to huge unsigned values; (-1, ...)
-- returned 69.57.
SELECT pg_temp.err_of($q$SELECT bm25_debug_score(-1, 1, 1, 1, 10, 1.2, 0.75)$q$) AS n_neg;
SELECT pg_temp.err_of($q$SELECT bm25_debug_score(100, -1, 1, 1, 10, 1.2, 0.75)$q$) AS df_neg;
SELECT pg_temp.err_of($q$SELECT bm25_debug_score(100, 10, -1, 1, 10, 1.2, 0.75)$q$) AS tf_neg;
SELECT pg_temp.err_of($q$SELECT bm25_debug_score(100, 10, 1, -1, 10, 1.2, 0.75)$q$) AS dl_neg;
SELECT round(bm25_debug_score(0, 0, 1, 0, 10, 1.2, 0.75)::numeric, 6) AS zeros_accepted;

-- bm25_debug_topk: offset 0 tripped Assert(ItemPointerIsValid) on a cassert build, and
-- offsets past MaxOffsetNumber truncated into other tids.
SELECT pg_temp.err_of($q$SELECT * FROM bm25_debug_topk(3, '{1.0,2.0}', '{0,1}')$q$)     AS off_zero;
SELECT pg_temp.err_of($q$SELECT * FROM bm25_debug_topk(3, '{1.0,2.0}', '{1,70000}')$q$) AS off_70000;
SELECT rank, tid FROM bm25_debug_topk(3, '{1.0,2.0}', '{1,2048}') ORDER BY rank;

-- bm25_debug_terms: a term of 64 bytes or more raised "term too long for debug
-- aggregation", though bm25_debug_postings read the same index.
CREATE TABLE l_docs (id int PRIMARY KEY, body text);
INSERT INTO l_docs VALUES (1, 'hello ' || repeat('x', 70)), (2, 'hello world');
CREATE INDEX l_bm ON l_docs USING bm25_native (body);
SELECT length(term) AS term_bytes, df FROM bm25_debug_terms('l_bm') ORDER BY 1, 2;
DROP TABLE l_docs;

-- bm25_debug_seg_keymap: a text key longer than key_size is cut by BYTES on disk, which
-- can split a character; key_text now ends on the last whole one (key_bytes keeps the
-- stored bytes). 'a' plus eight two-byte characters is 17 bytes against a 16-byte key.
CREATE TABLE k_docs (id int PRIMARY KEY, k text, body text);
INSERT INTO k_docs VALUES (1, 'a' || repeat(U&'\00e9', 8), 'alpha'), (2, 'plain', 'alpha');
CREATE INDEX k_bm ON k_docs USING bm25_native (body) INCLUDE (k) WITH (key_field = 'k');
SELECT local_docid, octet_length(key_bytes) AS stored_bytes, octet_length(key_text) AS text_bytes,
       convert_from(key_text::bytea, 'UTF8') = 'a' || repeat(U&'\00e9', 7)
         OR convert_from(key_text::bytea, 'UTF8') = 'plain' AS decodes
  FROM bm25_debug_seg_keymap('k_bm', 0) ORDER BY 1;
DROP TABLE k_docs;

-- The tokenizer probes truncated an over-long language name silently before resolving
-- it; they now refuse it with the reloption validator's message.
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_tokenize('hello', 'standard', 'default', %L)$q$,
                             repeat('e', 40))) AS tokenize_long_language;
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_analyze_positions('hello', 'standard', 'default', %L)$q$,
                             repeat('e', 40))) AS positions_long_language;
SELECT bm25_debug_tokenize('the running dogs', 'standard', 'default', 'english') AS still_works;

-- ================================================================ 7. field-config root
-- Every readable format has a field-config page, written by the build before the index
-- is visible, so an Invalid root on a built index is corruption. The two readers that
-- refuse it (the config and the key-stamp read) are reached through their probes; the
-- query paths take the config from elsewhere and do not read it here.
CREATE TABLE f_docs (id int PRIMARY KEY, body text);
INSERT INTO f_docs SELECT g, 'alpha ' || g FROM generate_series(1, 5) g;
CREATE INDEX f_bm ON f_docs USING bm25_native (body);
CREATE TEMP TABLE fieldcfg_root AS
SELECT bm25_debug_poke_page('f_bm', 0, pg_temp.meta_off('field_config_blkno'), '\xffffffff') AS old;
-- Pre-fix: 55000 from both.
SELECT pg_temp.err_of($q$SELECT * FROM bm25_debug_fieldcfg('f_bm')$q$) AS fieldcfg_read;
SELECT pg_temp.err_of($q$SELECT * FROM bm25_debug_keystamp('f_bm')$q$) AS keystamp_read;
SELECT length(bm25_debug_poke_page('f_bm', 0, pg_temp.meta_off('field_config_blkno'), old)) AS restored
  FROM fieldcfg_root;
SELECT pg_temp.err_of($q$SELECT * FROM bm25_debug_fieldcfg('f_bm')$q$) AS fieldcfg_restored;
DROP TABLE f_docs;

RESET enable_seqscan;
DROP EXTENSION bm25_native CASCADE;
