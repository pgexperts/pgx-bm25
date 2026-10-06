-- 151_block_header_offset -- issue #312 item 3: bm25_seg_block_header_read_lead bounds
-- the in-page offset of a postings block BEFORE it forms a pointer from it.
--
-- The offset is a uint16 taken from a DICT entry's post_off (or from the previous
-- block's next_off), and bm25_dictentry_validate never looks at it. The header reader
-- used to form `PageGetContents(pg) + off` and only then compare the result against
-- the page's content end, so a corrupt offset built a pointer up to 64 KB into an 8 KB
-- object before anything rejected it. Both versions raise XX002; the SQLSTATE is not
-- what discriminates them, so every case asserts the MESSAGE, which names the check
-- that fired (the new one: "postings block offset N is past the page's N content
-- bytes"; the old comparison: "posting block offset N past the end of block N").
--
-- The reader is reached through the two header-only paths: bm25_debug_block_impacts
-- (bm25_seg_block_header_read) and the WAND cursor's open-time sweep (the _lead
-- variant). The exhaustive reader has its own copy of the check; it is the control.
CREATE EXTENSION bm25_native;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, '\m\d+\M', 'N', 'g');
END; $$ LANGUAGE plpgsql;

CREATE FUNCTION pg_temp.lay(s text, f text) RETURNS int AS $$
    SELECT off FROM bm25_debug_layout() WHERE struct = s AND field = f;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.contents(s text, f text) RETURNS int AS $$
    SELECT pg_temp.lay('page', 'contents') + pg_temp.lay(s, f);
$$ LANGUAGE sql;

-- Host byte order, read off the metapage's opaque flags: poke ff over them, poke the old
-- bytes back, and compare the first byte with the SQL-visible flags.
CREATE TABLE bo (id int PRIMARY KEY, body text);
INSERT INTO bo VALUES (1, 'alpha');
CREATE INDEX bo_bm ON bo USING bm25_native (body);
CREATE FUNCTION pg_temp.special(f text) RETURNS int AS $$
    SELECT pg_temp.lay('page', 'special') + pg_temp.lay('BM25PageOpaque', f);
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.peek(idx regclass, blk bigint, off int, len int) RETURNS bytea AS $$
DECLARE old bytea;
BEGIN
    old := bm25_debug_poke_page(idx, blk, off, decode(repeat('ff', len), 'hex'));
    PERFORM bm25_debug_poke_page(idx, blk, off, old);
    RETURN old;
END; $$ LANGUAGE plpgsql;
CREATE TEMP TABLE host AS
SELECT get_byte(pg_temp.peek('bo_bm', 0, pg_temp.special('flags'), 2), 0)
         = (bm25_debug_page_flags('bo_bm', 0) & 255) AS le;
DROP TABLE bo;
CREATE FUNCTION pg_temp.b16(v int) RETURNS bytea AS $$
    SELECT CASE WHEN (SELECT le FROM host)
        THEN set_byte(set_byte('\x0000'::bytea, 0, v & 255), 1, (v >> 8) & 255)
        ELSE set_byte(set_byte('\x0000'::bytea, 1, v & 255), 0, (v >> 8) & 255) END;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.u32(b bytea) RETURNS bigint AS $$
    SELECT CASE WHEN (SELECT le FROM host)
        THEN get_byte(b, 0)::bigint + (get_byte(b, 1)::bigint << 8) +
             (get_byte(b, 2)::bigint << 16) + (get_byte(b, 3)::bigint << 24)
        ELSE get_byte(b, 3)::bigint + (get_byte(b, 2)::bigint << 8) +
             (get_byte(b, 1)::bigint << 16) + (get_byte(b, 0)::bigint << 24) END;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.seg_hdr(idx regclass, n int) RETURNS bigint AS $$
    SELECT pg_temp.u32(substring(bm25_debug_segcat_entry_bytes(idx, n)
                                 FROM pg_temp.lay('BM25SegCatEntry', 'header_blkno') + 1
                                 FOR 4));
$$ LANGUAGE sql;

-- One segment, a handful of documents. 'alpha' sorts first, so its DICT entry is the
-- first record on the first DICT page.
CREATE TABLE bh (id int PRIMARY KEY, body text);
INSERT INTO bh SELECT g, 'alpha beta g' || g FROM generate_series(1, 300) g;
CREATE INDEX bh_bm ON bh USING bm25_native (body);
CREATE FUNCTION pg_temp.first_dict_page() RETURNS bigint AS $$
    SELECT pg_temp.u32(pg_temp.peek('bh_bm', pg_temp.seg_hdr('bh_bm', 0),
                                    pg_temp.contents('BM25SegmentHeader', 'dict_root'), 4));
$$ LANGUAGE sql;

-- Controls on the healthy index: the header reader and the ranked scan both work.
SELECT count(*) > 0 AS block_impacts_ok FROM bm25_debug_block_impacts('bh_bm', 'alpha');
SELECT count(*) AS ranked FROM
  (SELECT id FROM bh WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 5) s;

-- Forge post_off of the first DICT entry far past any content (the old value is
-- returned and discarded; the index is dropped at the end).
SELECT bm25_debug_poke_page('bh_bm', pg_temp.first_dict_page(),
                            pg_temp.contents('BM25DictEntry', 'post_off'),
                            pg_temp.b16(60000)) IS NOT NULL AS forged;

-- Header-only reader, plain entry (bm25_seg_block_header_read).
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_block_impacts('bh_bm', 'alpha')$q$)
       AS block_impacts;
-- WAND cursor open (bm25_seg_block_header_read_lead via the global-bound sweep).
SELECT pg_temp.err_of($q$SELECT id FROM bh WHERE body @@@ 'alpha'
                          ORDER BY body &@@ 'alpha' LIMIT 5$q$) AS wand;
-- Control: the exhaustive reader's own check, a different message.
SET bm25_native.wand_top_k = 0;
SELECT pg_temp.err_of($q$SELECT id FROM bh WHERE body @@@ 'alpha'
                          ORDER BY body &@@ 'alpha' LIMIT 5$q$) AS exhaustive;
RESET bm25_native.wand_top_k;

-- pg_regress shares one database across suites; leave nothing behind.
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE bh;
DROP EXTENSION bm25_native CASCADE;
