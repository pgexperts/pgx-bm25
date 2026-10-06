-- 137_page_roles -- issues #302 (A, B, D) and #303 (C): a block pointer read off a
-- page must lead to a page of the role the reader or writer expects, or the index
-- raises ERRCODE_INDEX_CORRUPTED (XX002) before anything is written.
--
-- Shapes, each forged with the raw page lever (bm25_debug_poke_page, aimed through
-- bm25_debug_layout) and restored afterwards:
--   1. a metapage block pointer of 0 (#302.A). Any of the five; segcat_root = 0 made a
--      seal wait forever on the metapage lock it already held, uncancellably.
--   2. segcat_root naming a segment HEADER page or a DICT page (#302.A, #276): the
--      catalog appender wrote its entries into that page, and the catalog walkers took
--      a header page (SEGCAT too) for a catalog page.
--   3. a catalog entry with gen 0 (#303.C), the "don't validate" sentinel, which
--      switched off reuse detection for its whole segment.
--   4. pending_tail naming a catalog, DICT or segment header page, or a freed pending
--      page (#302.B): INSERT wrote a pending record or a forward link into it.
--   5. pd_special at BLCKSZ (#302.D), which puts the page's opaque past the block.
--
-- The lever writes through Generic WAL and the suite reads bytes back the same way
-- (pg_temp.peek pokes and restores), so nothing here depends on byte order: block
-- numbers are copied between fields as bytes, and decoded only through pg_temp.u32,
-- which learns the byte order from a field with a known value.
CREATE EXTENSION bm25_native;

-- SQLSTATE beside the message; block and generation numbers depend on the layout.
CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || regexp_replace(SQLERRM, '(block|page|generation) [0-9]+', '\1 N', 'g');
END; $$ LANGUAGE plpgsql;

-- Page offset of a struct member placed at a page anchor ('contents' or 'special').
CREATE FUNCTION pg_temp.at(anchor text, s text, f text) RETURNS int AS $$
    SELECT (SELECT off FROM bm25_debug_layout() WHERE struct = 'page' AND field = anchor)
         + (SELECT off FROM bm25_debug_layout() WHERE struct = s AND field = f);
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.fsize(s text, f text) RETURNS int AS $$
    SELECT size FROM bm25_debug_layout() WHERE struct = s AND field = f;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.meta_off(f text) RETURNS int AS $$
    SELECT pg_temp.at('contents', 'BM25MetaPageData', f);
$$ LANGUAGE sql;

-- Read bytes by writing zeros and writing the returned bytes back. Only for fields in
-- a page's contents: the lever refuses a zeroed page-header field.
CREATE FUNCTION pg_temp.peek(idx regclass, blk bigint, off int, len int) RETURNS bytea AS $$
DECLARE
    old bytea;
BEGIN
    old := bm25_debug_poke_page(idx, blk, off, decode(repeat('00', len), 'hex'));
    PERFORM bm25_debug_poke_page(idx, blk, off, old);
    RETURN old;
END; $$ LANGUAGE plpgsql;

-- Byte order, learned from field_config_blkno, whose value bm25_stats reports.
CREATE TEMP TABLE endian (le bool);
CREATE FUNCTION pg_temp.u32(b bytea) RETURNS bigint AS $$
    SELECT CASE WHEN (SELECT le FROM endian)
           THEN get_byte(b, 0)::bigint + (get_byte(b, 1)::bigint << 8)
              + (get_byte(b, 2)::bigint << 16) + (get_byte(b, 3)::bigint << 24)
           ELSE get_byte(b, 3)::bigint + (get_byte(b, 2)::bigint << 8)
              + (get_byte(b, 1)::bigint << 16) + (get_byte(b, 0)::bigint << 24) END;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.b16(v int) RETURNS bytea AS $$
    SELECT CASE WHEN (SELECT le FROM endian)
           THEN set_byte(set_byte('\x0000'::bytea, 0, v & 255), 1, v >> 8)
           ELSE set_byte(set_byte('\x0000'::bytea, 1, v & 255), 0, v >> 8) END;
$$ LANGUAGE sql;

-- A metapage field, as a block number or as raw bytes.
CREATE FUNCTION pg_temp.meta_bytes(idx regclass, f text) RETURNS bytea AS $$
    SELECT pg_temp.peek(idx, 0, pg_temp.meta_off(f), pg_temp.fsize('BM25MetaPageData', f));
$$ LANGUAGE sql;

-- Run q with one metapage field forged to `val`, then restore the field.
CREATE FUNCTION pg_temp.with_meta(idx regclass, f text, val bytea, q text) RETURNS text AS $$
DECLARE
    old bytea;
    res text;
BEGIN
    old := bm25_debug_poke_page(idx, 0, pg_temp.meta_off(f), val);
    res := pg_temp.err_of(q);
    PERFORM bm25_debug_poke_page(idx, 0, pg_temp.meta_off(f), old);
    RETURN res;
END; $$ LANGUAGE plpgsql;

-- Run q with `val` written at (blk, off), then restore those bytes.
CREATE FUNCTION pg_temp.with_bytes(idx regclass, blk bigint, off int, val bytea, q text)
RETURNS text AS $$
DECLARE
    old bytea;
    res text;
BEGIN
    old := bm25_debug_poke_page(idx, blk, off, val);
    res := pg_temp.err_of(q);
    PERFORM bm25_debug_poke_page(idx, blk, off, old);
    RETURN res;
END; $$ LANGUAGE plpgsql;

SET enable_seqscan = off;

-- ======================================================= 1. a metapage pointer of 0
-- Block 0 is the metapage, and no pointer on it may name it: each is Invalid or a page
-- allocated later. bm25_meta_validate refuses all five, so every metapage reader
-- (bm25_stats here) raises. Before #302.A the gate checked none of them.
CREATE TABLE m_docs (id int PRIMARY KEY, body text);
INSERT INTO m_docs SELECT g, 'alpha beta ' || g FROM generate_series(1, 20) g;
CREATE INDEX m_bm ON m_docs USING bm25_native (body);
INSERT INTO m_docs VALUES (100, 'gamma pending');     -- a pending list, so all five are set
INSERT INTO endian
SELECT get_byte(pg_temp.meta_bytes('m_bm', 'field_config_blkno'), 0) = field_config_blkno
  FROM bm25_stats('m_bm');
SELECT pg_temp.u32(pg_temp.meta_bytes('m_bm', 'field_config_blkno'))
       = (SELECT field_config_blkno FROM bm25_stats('m_bm')) AS byte_order_learned;

SELECT f AS pointer,
       pg_temp.with_meta('m_bm', f, '\x00000000', $q$SELECT * FROM bm25_stats('m_bm')$q$)
       AS stats
  FROM unnest(ARRAY['pending_head', 'pending_tail', 'segcat_root', 'retired_head',
                    'field_config_blkno']) AS f;
-- The scan path's own metapage read meets the same gate.
SELECT pg_temp.with_meta('m_bm', 'segcat_root', '\x00000000',
                         $q$SELECT count(*) FROM m_docs WHERE body @@@ 'alpha'$q$) AS scan;
-- The writers. segcat_root = 0 used to wedge the seal: the catalog appender read block
-- 0 while holding the metapage EXCLUSIVE and waited on its own lock with interrupts
-- held, so neither statement_timeout nor pg_terminate_backend ended it. That pre-fix
-- shape cannot run in a suite (it never returns), so only the fixed outcome is pinned.
SELECT pg_temp.with_meta('m_bm', 'segcat_root', '\x00000000',
                         $q$SELECT bm25_seal('m_bm')$q$) AS seal_root_zero;
SELECT pg_temp.with_meta('m_bm', 'pending_tail', '\x00000000',
                         $q$INSERT INTO m_docs VALUES (101, 'delta')$q$) AS insert_tail_zero;
-- Restored, the index is whole: the seal publishes the pending document.
SELECT bm25_seal('m_bm');
SELECT count(*) AS alpha_rows FROM m_docs WHERE body @@@ 'alpha';
SELECT id FROM m_docs WHERE body @@@ 'gamma';

-- ======================================================= block numbers for 2-5
-- One segment (built by CREATE INDEX) and a pending page (the INSERT after it). The
-- catalog root, the segment's header and its first DICT page come off the pages.
CREATE TABLE r_docs (id int PRIMARY KEY, body text);
INSERT INTO r_docs SELECT g, 'alpha beta ' || g FROM generate_series(1, 20) g;
CREATE INDEX r_bm ON r_docs USING bm25_native (body);
INSERT INTO r_docs VALUES (100, 'gamma pending');
CREATE TEMP TABLE blk AS
SELECT pg_temp.u32(pg_temp.meta_bytes('r_bm', 'segcat_root')) AS catroot,
       pg_temp.u32(pg_temp.meta_bytes('r_bm', 'pending_tail')) AS tail;
ALTER TABLE blk ADD COLUMN header bigint, ADD COLUMN dict bigint;
UPDATE blk SET header = pg_temp.u32(pg_temp.peek('r_bm', catroot,
                 pg_temp.at('contents', 'BM25SegCatEntry', 'header_blkno'), 4));
UPDATE blk SET dict = pg_temp.u32(pg_temp.peek('r_bm', header,
                 pg_temp.at('contents', 'BM25SegmentHeader', 'dict_root'), 4));
-- Sanity: each is the kind it is taken for (SEGCAT 0x4, SEGCAT 0x4, DICT 0x8, PENDING
-- 0x2), the header carries a gen and the catalog page does not.
SELECT bm25_debug_page_flags('r_bm', catroot::int) AS catroot_flags,
       bm25_debug_page_flags('r_bm', header::int)  AS header_flags,
       bm25_debug_page_flags('r_bm', dict::int)    AS dict_flags,
       bm25_debug_page_flags('r_bm', tail::int)    AS tail_flags,
       bm25_debug_page_seg_gen('r_bm', catroot::int) AS catroot_gen,
       bm25_debug_page_seg_gen('r_bm', header::int) > 0 AS header_has_gen
  FROM blk;
CREATE FUNCTION pg_temp.blk_bytes(which text) RETURNS bytea AS $$
    SELECT CASE which
             WHEN 'catroot' THEN pg_temp.peek('r_bm', 0, pg_temp.meta_off('segcat_root'), 4)
             WHEN 'tail'    THEN pg_temp.peek('r_bm', 0, pg_temp.meta_off('pending_tail'), 4)
             WHEN 'header'  THEN pg_temp.peek('r_bm', (SELECT catroot FROM blk),
                                   pg_temp.at('contents', 'BM25SegCatEntry', 'header_blkno'), 4)
             WHEN 'dict'    THEN pg_temp.peek('r_bm', (SELECT header FROM blk),
                                   pg_temp.at('contents', 'BM25SegmentHeader', 'dict_root'), 4)
           END;
$$ LANGUAGE sql;

-- ======================================================= 2. segcat_root of another role
-- The catalog appender (bm25_seal publishing the pending document) appends in place to
-- the root when it has room. Before #302.A it checked only that room, so both seals
-- below succeeded and wrote a catalog entry into the header or DICT page.
SELECT pg_temp.with_meta('r_bm', 'segcat_root', pg_temp.blk_bytes('header'),
                         $q$SELECT bm25_seal('r_bm')$q$) AS seal_root_header;
SELECT pg_temp.with_meta('r_bm', 'segcat_root', pg_temp.blk_bytes('dict'),
                         $q$SELECT bm25_seal('r_bm')$q$) AS seal_root_dict;
-- The catalog walkers took a header page for a catalog page too (#276): its kind is
-- SEGCAT, and only seg_gen tells the two apart.
SELECT w AS walker,
       pg_temp.with_meta('r_bm', 'segcat_root', pg_temp.blk_bytes('header'),
                         format($q$SELECT bm25_debug_segcat_walk('r_bm', %L, -1)$q$, w))
       AS root_is_header
  FROM unnest(ARRAY['scan_snapshot', 'segcat_read', 'first_entry', 'find_absent',
                    'locate_absent']) AS w;
-- Nothing was written: the header and DICT pages still read as before.
SELECT count(*) AS segments, sum(ndocs) AS docs FROM bm25_debug_segcat('r_bm');
SELECT count(*) AS alpha_rows FROM r_docs WHERE body @@@ 'alpha';

-- ======================================================= 3. a catalog entry with gen 0
-- Every reader passes the entry's gen as the expected gen, and 0 means "don't check".
-- Before #303.C the scan ran normally on such an entry, with reuse detection off for
-- the whole segment.
SELECT pg_temp.with_bytes('r_bm', (SELECT catroot FROM blk),
                          pg_temp.at('contents', 'BM25SegCatEntry', 'gen'), '\x00000000',
                          $q$SELECT count(*) FROM r_docs WHERE body @@@ 'alpha'$q$)
       AS scan_gen_zero;
SELECT w AS walker,
       pg_temp.with_bytes('r_bm', (SELECT catroot FROM blk),
                          pg_temp.at('contents', 'BM25SegCatEntry', 'gen'), '\x00000000',
                          format($q$SELECT bm25_debug_segcat_walk('r_bm', %L, -1)$q$, w))
       AS gen_zero
  FROM unnest(ARRAY['segcat_read', 'first_entry']) AS w;
SELECT count(*) AS alpha_rows FROM r_docs WHERE body @@@ 'alpha';

-- ======================================================= 4. pending_tail of another kind
-- The pending appender checked only the byte budget, so each INSERT below succeeded
-- before #302.B and wrote a pending record into the named page. Each uses its own id,
-- so that on a build without the check one success does not turn the next into a
-- duplicate-key error.
SELECT pg_temp.with_meta('r_bm', 'pending_tail', pg_temp.blk_bytes('catroot'),
                         $q$INSERT INTO r_docs VALUES (201, 'epsilon')$q$) AS tail_catalog;
SELECT pg_temp.with_meta('r_bm', 'pending_tail', pg_temp.blk_bytes('dict'),
                         $q$INSERT INTO r_docs VALUES (202, 'epsilon')$q$) AS tail_dict;
-- A freed pending page: bm25_page_mark_deleted leaves BM25_PAGE_PENDING set, so the
-- kind test alone passes it. 512 is BM25_PAGE_DELETED (bm25_format.h).
SELECT pg_temp.with_bytes('r_bm', tail, pg_temp.at('special', 'BM25PageOpaque', 'flags'),
                          pg_temp.b16(bm25_debug_page_flags('r_bm', tail::int) | 512),
                          $q$INSERT INTO r_docs VALUES (203, 'epsilon')$q$) AS tail_deleted
  FROM blk;
-- The link path: no room claimed on the tail, so the INSERT allocates a page and links
-- it from the old tail, whose seg_gen also becomes the new chain page's epoch (#291).
-- Here the "old tail" is the segment header. The check runs before the allocation, so
-- the index does not grow.
CREATE TEMP TABLE np AS SELECT bm25_debug_npages('r_bm') AS before;
CREATE FUNCTION pg_temp.link_path() RETURNS text AS $$
DECLARE
    old_free bytea;
    res text;
BEGIN
    old_free := bm25_debug_poke_page('r_bm', 0, pg_temp.meta_off('pending_tail_free'),
                                     '\x00000000');
    res := pg_temp.with_meta('r_bm', 'pending_tail', pg_temp.blk_bytes('header'),
                             $q$INSERT INTO r_docs VALUES (204, 'epsilon')$q$);
    PERFORM bm25_debug_poke_page('r_bm', 0, pg_temp.meta_off('pending_tail_free'), old_free);
    RETURN res;
END; $$ LANGUAGE plpgsql;
SELECT pg_temp.link_path() AS tail_header_link;
SELECT bm25_debug_npages('r_bm') = before AS no_page_allocated FROM np;
-- The flags of the pages named above are unchanged, and the index is whole.
SELECT bm25_debug_page_flags('r_bm', catroot::int) AS catroot_flags,
       bm25_debug_page_flags('r_bm', dict::int)    AS dict_flags,
       bm25_debug_page_flags('r_bm', tail::int)    AS tail_flags
  FROM blk;
INSERT INTO r_docs VALUES (200, 'epsilon');
SELECT id FROM r_docs WHERE body @@@ 'gamma' OR body @@@ 'epsilon' ORDER BY id;

-- ======================================================= 5. pd_special at BLCKSZ
-- PostgreSQL's page check accepts pd_special == BLCKSZ, which puts the 24-byte opaque
-- wholly past the block. Before #302.D the appender's in-place path never read the
-- opaque and the INSERT succeeded; bm25_page_content_bytes now refuses the page, on the
-- write path and on the scan's pending walk alike.
SELECT pg_temp.with_bytes('r_bm', tail,
                          (SELECT off FROM bm25_debug_layout()
                            WHERE struct = 'PageHeaderData' AND field = 'pd_special'),
                          pg_temp.b16((SELECT size FROM bm25_debug_layout()
                                        WHERE struct = 'page' AND field = '*')),
                          $q$INSERT INTO r_docs VALUES (300, 'zeta')$q$) AS insert_special
  FROM blk;
SELECT pg_temp.with_bytes('r_bm', tail,
                          (SELECT off FROM bm25_debug_layout()
                            WHERE struct = 'PageHeaderData' AND field = 'pd_special'),
                          pg_temp.b16((SELECT size FROM bm25_debug_layout()
                                        WHERE struct = 'page' AND field = '*')),
                          $q$SELECT count(*) FROM r_docs WHERE body @@@ 'gamma'$q$)
       AS scan_special
  FROM blk;
SELECT bm25_seal('r_bm');
SELECT id FROM r_docs WHERE body @@@ 'gamma' OR body @@@ 'epsilon' ORDER BY id;

-- ======================================================= 6. pd_special in the orphan sweep
-- The sweep marks every page reachable from the live roots (mark_chain), then frees
-- every page it did not mark. mark_chain follows nextblk, which sits at pd_special, so
-- with the DICT root's pd_special at BLCKSZ the walk read a neighbour's bytes and
-- stopped early, and the rest of the live DICT chain was stamped DELETED and recorded
-- free. A fresh index's first VACUUM sweeps (swept_epoch 0). VACUUM cannot run inside
-- err_of, so its error is read back through psql's LAST_ERROR_* variables.
CREATE TABLE s_docs (id int PRIMARY KEY, body text);
INSERT INTO s_docs SELECT g, 'w' || g || ' x' || (g * 7) FROM generate_series(1, 4000) g;
CREATE INDEX s_bm ON s_docs USING bm25_native (body);
CREATE TEMP TABLE sblk AS
SELECT pg_temp.u32(pg_temp.peek('s_bm', pg_temp.u32(pg_temp.meta_bytes('s_bm', 'segcat_root')),
                                pg_temp.at('contents', 'BM25SegCatEntry', 'header_blkno'), 4))
       AS header;
ALTER TABLE sblk ADD COLUMN dict bigint, ADD COLUMN dict2 bigint, ADD COLUMN old_special bytea;
UPDATE sblk SET dict = pg_temp.u32(pg_temp.peek('s_bm', header,
                 pg_temp.at('contents', 'BM25SegmentHeader', 'dict_root'), 4));
UPDATE sblk SET dict2 = pg_temp.u32(pg_temp.peek('s_bm', dict,
                 pg_temp.at('special', 'BM25PageOpaque', 'nextblk'), 4));
SELECT bm25_debug_page_flags('s_bm', dict::int) AS root_flags,
       bm25_debug_page_flags('s_bm', dict2::int) AS second_flags
  FROM sblk;
UPDATE sblk SET old_special = bm25_debug_poke_page('s_bm', dict,
                 (SELECT off FROM bm25_debug_layout()
                   WHERE struct = 'PageHeaderData' AND field = 'pd_special'),
                 pg_temp.b16((SELECT size FROM bm25_debug_layout()
                               WHERE struct = 'page' AND field = '*')));
\set VERBOSITY sqlstate
VACUUM s_docs;
\set VERBOSITY default
SELECT :'LAST_ERROR_SQLSTATE' AS sweep_sqlstate,
       regexp_replace(:'LAST_ERROR_MESSAGE', 'page [0-9]+', 'page N') AS sweep_error;
-- The second DICT page is still a live DICT page, not DELETED.
SELECT bm25_debug_page_flags('s_bm', dict2::int) AS second_flags_after FROM sblk;
SELECT bm25_debug_poke_page('s_bm', dict,
                            (SELECT off FROM bm25_debug_layout()
                              WHERE struct = 'PageHeaderData' AND field = 'pd_special'),
                            old_special) IS NOT NULL AS special_restored
  FROM sblk;
SELECT count(*) AS w4000_rows FROM s_docs WHERE body @@@ 'w4000';
DROP TABLE s_docs;

-- ======================================================= 7. pd_special on a freed page
-- The seal frees the drained pending pages (DELETED). With one of them carrying
-- pd_special at BLCKSZ, bm25_page_alloc must not take it (its DELETED test would read
-- the flags past the block); it drops the page from the FSM instead. The sweep's
-- full-extent pass then meets the page as unreachable, and must refuse it rather than
-- read its flags and stamp it.
CREATE TABLE f_docs (id int PRIMARY KEY, body text);
CREATE INDEX f_bm ON f_docs USING bm25_native (body);
INSERT INTO f_docs SELECT g, repeat('v' || g || ' ', 40) FROM generate_series(1, 300) g;
CREATE TEMP TABLE fblk AS SELECT bm25_debug_pending_head('f_bm') AS freed;
SELECT bm25_seal('f_bm');
SELECT bm25_debug_page_flags('f_bm', freed::int) & 512 <> 0 AS freed_is_deleted FROM fblk;
SELECT length(bm25_debug_poke_page('f_bm', freed,
                (SELECT off FROM bm25_debug_layout()
                  WHERE struct = 'PageHeaderData' AND field = 'pd_special'),
                pg_temp.b16((SELECT size FROM bm25_debug_layout()
                              WHERE struct = 'page' AND field = '*')))) AS poked
  FROM fblk;
-- Allocate: more pending pages, then a seal's segment pages.
INSERT INTO f_docs SELECT g, repeat('u' || g || ' ', 40) FROM generate_series(301, 600) g;
SELECT bm25_seal('f_bm');
-- Not reused: its pd_special is still BLCKSZ (the poke hands back the bytes it replaced).
SELECT bm25_debug_poke_page('f_bm', freed,
                            (SELECT off FROM bm25_debug_layout()
                              WHERE struct = 'PageHeaderData' AND field = 'pd_special'),
                            pg_temp.b16((SELECT size FROM bm25_debug_layout()
                                          WHERE struct = 'page' AND field = '*')))
       = pg_temp.b16((SELECT size FROM bm25_debug_layout()
                       WHERE struct = 'page' AND field = '*')) AS not_reused
  FROM fblk;
\set VERBOSITY sqlstate
VACUUM f_docs;
\set VERBOSITY default
SELECT :'LAST_ERROR_SQLSTATE' AS sweep_sqlstate,
       regexp_replace(:'LAST_ERROR_MESSAGE', 'page [0-9]+', 'page N') AS sweep_error;
SELECT count(*) AS v1_rows FROM f_docs WHERE body @@@ 'v1';
DROP TABLE f_docs;

RESET enable_seqscan;
DROP TABLE m_docs;
DROP TABLE r_docs;
DROP EXTENSION bm25_native CASCADE;
