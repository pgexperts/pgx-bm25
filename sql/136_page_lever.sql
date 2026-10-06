-- 136_page_lever -- the raw corruption lever (bm25_debug_poke_page) and the layout
-- table it is aimed with (bm25_debug_layout), added for the corrupt-pointer tests of
-- issues #302/#303/#309.
--
-- The stamp levers each forge one named field. The remaining corrupt-pointer tests need
-- about twenty more (metapage pointers, retired roots, catalog gen, fieldcfg field_id,
-- pending field_id, DICT/POS nextblk, impact max_tf, pd_special), so they get one raw
-- writer plus a table of offsets instead. This suite characterizes both: the layout
-- rows, the writer's argument and result checks, an exact round trip, and one poke that
-- reaches a validator the catalog walkers already run.
--
-- Endian-independent: every multi-byte value poked here reads the same either way round
-- (\x7f7f7f7f, \x0101, \xffff), as in 96_wal_page_determinism. Privileges are in
-- 63_debug_privileges.
CREATE EXTENSION bm25_native;

-- SQLSTATE beside the message. The index's block count and the metapage's pd_lower are
-- stripped: they depend on the page layout, not on the check under test.
CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' ||
           regexp_replace(regexp_replace(SQLERRM, 'block \d+ is past the end of the index \(\d+ blocks\)',
                                         'block N is past the end of the index (N blocks)'),
                          'hole \[\d+, \d+\)', 'hole [pd_lower, pd_upper)', 'g');
END; $$ LANGUAGE plpgsql;

-- Page offset of a struct member placed at a page anchor: anchor + member.
CREATE FUNCTION pg_temp.at(anchor text, s text, f text) RETURNS int AS $$
    SELECT (SELECT off FROM bm25_debug_layout() WHERE struct = 'page' AND field = anchor)
         + (SELECT off FROM bm25_debug_layout() WHERE struct = s AND field = f);
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.blk_size() RETURNS int AS $$
    SELECT size FROM bm25_debug_layout() WHERE struct = 'page' AND field = '*';
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.hdr(f text) RETURNS int AS $$
    SELECT off FROM bm25_debug_layout() WHERE struct = 'PageHeaderData' AND field = f;
$$ LANGUAGE sql;

-- ======================================================= 1. layout table
-- The whole table, in declaration order. It doubles as a SQL pin of the on-disk struct
-- sizes and offsets: a format change shows up here as a diff.
SELECT l.struct, l.field, l.off, l.size
  FROM bm25_debug_layout() WITH ORDINALITY AS l(struct, field, off, size, n)
 ORDER BY l.n;

-- Every struct a corruption test addresses is present.
SELECT want AS missing_struct
  FROM unnest(ARRAY['PageHeaderData', 'page', 'BM25PageOpaque', 'BM25MetaPageData',
                    'BM25SegCatEntry', 'BM25RetiredEntry', 'BM25SegmentHeader',
                    'BM25KeymapHeader', 'BM25DictEntry', 'BM25BlockHeader',
                    'impact_table', 'impact_entry', 'BM25FieldConfigHeader',
                    'BM25FieldConfig', 'BM25KeyStamp', 'BM25PendingDocHeader',
                    'BM25PendingTermEntry']) AS want
 WHERE NOT EXISTS (SELECT 1 FROM bm25_debug_layout() WHERE struct = want AND field = '*');

-- Internal consistency: every member lies inside its struct, and no two members of a
-- struct overlap (the 'page' anchors are deliberately disjoint too).
SELECT m.struct, m.field AS member_outside_struct
  FROM bm25_debug_layout() m JOIN bm25_debug_layout() w
    ON w.struct = m.struct AND w.field = '*'
 WHERE m.field <> '*' AND (m.off < 0 OR m.size <= 0 OR m.off + m.size > w.size);
SELECT a.struct, a.field, b.field AS overlaps
  FROM bm25_debug_layout() a JOIN bm25_debug_layout() b
    ON a.struct = b.struct AND a.field < b.field
 WHERE a.field <> '*' AND b.field <> '*'
   AND a.off < b.off + b.size AND b.off < a.off + a.size;

-- ======================================================= 2. argument checks
CREATE TABLE pl_docs (id int PRIMARY KEY, body text);
INSERT INTO pl_docs VALUES (1, 'alpha beta'), (2, 'beta gamma'), (3, 'gamma delta');
-- Sixty distinct terms, so the DICT page's pd_lower is well past 257 (section 3).
INSERT INTO pl_docs SELECT g, 'w' || g FROM generate_series(10, 69) g;
CREATE INDEX pl_bm ON pl_docs USING bm25_native (body);
SELECT bm25_seal('pl_bm');

-- Not a block number; past the end of the index; nothing to write.
SELECT pg_temp.err_of($q$SELECT bm25_debug_poke_page('pl_bm', -1, 100, '\x00')$q$) AS neg_block;
SELECT pg_temp.err_of($q$SELECT bm25_debug_poke_page('pl_bm', 4294967295, 100, '\x00')$q$)
       AS invalid_block;
SELECT pg_temp.err_of($q$SELECT bm25_debug_poke_page('pl_bm', bm25_debug_npages('pl_bm'), 100, '\x00')$q$)
       AS past_end;
SELECT pg_temp.err_of($q$SELECT bm25_debug_poke_page('pl_bm', 0, 100, '')$q$) AS empty;
-- The range: below pd_flags (pd_lsn and pd_checksum are rewritten by WAL and by the
-- buffer write, so a poke there would be lost), negative, and past the block's end.
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, '\x00')$q$,
                             pg_temp.hdr('pd_checksum'))) AS into_checksum;
SELECT pg_temp.err_of($q$SELECT bm25_debug_poke_page('pl_bm', 0, -5, '\x00')$q$) AS neg_off;
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, '\x0000')$q$,
                             pg_temp.blk_size() - 1)) AS past_block;

-- ======================================================= 3. result checks
-- A poke whose result would fail PostgreSQL's page header check is refused: such a page
-- reads fine while cached and is an "invalid page" once evicted, so a test built on it
-- would pass or fail with buffer pressure. pd_lower above pd_upper would also PANIC in
-- GenericXLogFinish, which zeroes the hole inside its critical section.
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, '\xffff')$q$,
                             pg_temp.hdr('pd_lower'))) AS lower_above_upper;
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, '\x0000')$q$,
                             pg_temp.hdr('pd_upper'))) AS upper_zero;
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, '\x0101')$q$,
                             pg_temp.hdr('pd_special'))) AS special_unaligned;
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, '\xffff')$q$,
                             pg_temp.hdr('pd_flags'))) AS unknown_flag_bits;
-- pd_lower inside the page header passes core's check, but the hole would then start
-- in the header: Generic WAL would zero pd_upper and pd_special in the buffer, leaving
-- pd_lower > pd_upper there, the PANIC precondition the check above excludes. The case
-- that matters ends the poked bytes exactly at the new pd_lower (14), where the overlap
-- check below does not see it. No byte-symmetric value lies in [14, 24), so 14 is
-- written in host order, read off the metapage's pd_pagesize_version (BLCKSZ 8192 |
-- layout version 4 = 0x2004): poke its little-endian image, keep what it replaced, and
-- poke that back, so the page is unchanged either way.
CREATE TEMP TABLE pl_host AS
SELECT bm25_debug_poke_page('pl_bm', 0, pg_temp.hdr('pd_pagesize_version'), '\x0420') AS was;
SELECT length(bm25_debug_poke_page('pl_bm', 0, pg_temp.hdr('pd_pagesize_version'), was))
       AS pagesize_version_restored
  FROM pl_host;
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, %L)$q$,
                             pg_temp.hdr('pd_lower'),
                             CASE WHEN was = '\x0420' THEN '\x0e00' ELSE '\x000e' END))
       AS lower_in_header
  FROM pl_host;

-- Lowering pd_lower over live bytes would turn them into hole, which Generic WAL zeroes:
-- they could not be restored by poking the old pd_lower back. 257 (\x0101) lands inside
-- the DICT page's entries.
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', %s, %s, '\x0101')$q$,
                             (SELECT min(b) FROM generate_series(0, bm25_debug_npages('pl_bm') - 1) b
                               WHERE bm25_debug_page_flags('pl_bm', b::int) & 8 <> 0),
                             pg_temp.hdr('pd_lower'))) AS lower_over_live_bytes;
-- A byte in the hole between pd_lower and pd_upper would be zeroed by Generic WAL. The
-- last byte before the special area is in the metapage's hole.
SELECT pg_temp.err_of(format($q$SELECT bm25_debug_poke_page('pl_bm', 0, %s, '\x01')$q$,
                             pg_temp.at('special', 'BM25PageOpaque', 'flags') - 1)) AS into_hole;
-- None of the refusals wrote anything: the index still answers.
SELECT id FROM pl_docs WHERE body @@@ 'beta' ORDER BY id;

-- ======================================================= 4. round trip
-- Poke the metapage's opaque flags, read them back through an independent probe, then
-- poke the returned bytes back. The second poke must hand back exactly the bytes the
-- first wrote, and the page must read as before.
CREATE TEMP TABLE pl_rt AS
SELECT bm25_debug_page_flags('pl_bm', 0) AS flags_before,
       bm25_debug_poke_page('pl_bm', 0, pg_temp.at('special', 'BM25PageOpaque', 'flags'),
                            '\x0101') AS old_bytes;
SELECT bm25_debug_page_flags('pl_bm', 0) AS flags_poked;
SELECT bm25_debug_poke_page('pl_bm', 0, pg_temp.at('special', 'BM25PageOpaque', 'flags'),
                            old_bytes) AS second_poke_returns
  FROM pl_rt;
SELECT bm25_debug_page_flags('pl_bm', 0) = flags_before AS flags_restored,
       length(old_bytes) = (SELECT size FROM bm25_debug_layout()
                             WHERE struct = 'BM25PageOpaque' AND field = 'flags')
       AS old_bytes_sized
  FROM pl_rt;
-- A poke may end exactly at the block's last byte (the opaque's tail padding here).
-- Nested: the inner poke writes, the outer one writes the inner one's old bytes back.
SELECT bm25_debug_poke_page('pl_bm', 0, pg_temp.blk_size() - 4,
                            bm25_debug_poke_page('pl_bm', 0, pg_temp.blk_size() - 4,
                                                 '\x7f7f7f7f')) AS tail_restored_returns;
SELECT id FROM pl_docs WHERE body @@@ 'beta' ORDER BY id;

-- ======================================================= 5. a poke reaches a validator
-- The metapage's segcat_root moved past the end of the index. The catalog walkers
-- already bound a catalog link by the relation's extent (issue #225); this pins that a
-- poked metapage pointer reaches that check, through the snapshot walker the ranked
-- scan uses and through bm25_segcat_read.
SELECT length(bm25_debug_poke_page('pl_bm', 0, pg_temp.at('contents', 'BM25MetaPageData', 'segcat_root'),
                                   '\x7f7f7f7f')) AS poked_root;
SELECT pg_temp.err_of($q$SELECT bm25_debug_segcat_walk('pl_bm', 'scan_snapshot', -1)$q$)
       AS snapshot_walker;
SELECT pg_temp.err_of($q$SELECT count(*) FROM bm25_debug_segcat('pl_bm')$q$) AS segcat_read;

DROP TABLE pl_docs;
DROP EXTENSION bm25_native;
