-- 138_retired_reclaim_validation -- a corrupt retired descriptor or retired list is an
-- ERROR, never a free of block 0 or of a live page, and never an endless walk under the
-- singleton (issue #302 C, E and the segment-header half of C).
--
-- reclaim_one_range used to free whatever a retired descriptor named, bounded only by
-- the relation's extent: a root of 0 stamped the metapage DELETED, and a root into a
-- live chain freed it under its readers. It now checks every page before the stamp --
-- not block 0, the chain's kind, seg_gen equal to the descriptor's gen, not already
-- DELETED -- and raises ERRCODE_INDEX_CORRUPTED. The error fires AFTER the descriptor is
-- compacted (decision D3), so each case below pins both halves: the first VACUUM fails
-- and frees nothing reachable, the compaction survives the abort (index WAL is
-- physical), and the NEXT VACUUM succeeds, its orphan sweep recovering the pages the
-- failed one left behind.
--
-- Corruption is forged with bm25_debug_poke_page, aimed through bm25_debug_layout.
-- Block numbers are taken from the index (page kind and seg_gen), never written as
-- literals; values are encoded in host byte order, which is read off a descriptor whose
-- header block is known to be below 256.
CREATE EXTENSION bm25_native;
\set VERBOSITY terse
SET enable_seqscan = off;

-- See 21_retired_descriptors and docs/adr/0031: a held snapshot pins the horizon.
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

CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || SQLERRM;
END; $$ LANGUAGE plpgsql;

-- ---------------------------------------------------------------- layout and encoding
CREATE FUNCTION pg_temp.off(s text, f text) RETURNS int AS $$
    SELECT off FROM bm25_debug_layout() WHERE struct = s AND field = f;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.size(s text, f text) RETURNS int AS $$
    SELECT size FROM bm25_debug_layout() WHERE struct = s AND field = f;
$$ LANGUAGE sql;

CREATE TEMP TABLE host (le bool);
INSERT INTO host VALUES (NULL);
CREATE FUNCTION pg_temp.u32(b bytea, o int) RETURNS bigint AS $$
    SELECT CASE WHEN (SELECT le FROM host)
                THEN get_byte(b, o) + get_byte(b, o + 1) * 256
                   + get_byte(b, o + 2) * 65536 + get_byte(b, o + 3)::bigint * 16777216
                ELSE get_byte(b, o + 3) + get_byte(b, o + 2) * 256
                   + get_byte(b, o + 1) * 65536 + get_byte(b, o)::bigint * 16777216 END;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.enc(v bigint) RETURNS bytea AS $$
    SELECT CASE WHEN (SELECT le FROM host)
                THEN decode(substr(h, 7, 2) || substr(h, 5, 2) || substr(h, 3, 2) ||
                            substr(h, 1, 2), 'hex')
                ELSE decode(h, 'hex') END
      FROM (SELECT lpad(to_hex(v), 8, '0') AS h) s;
$$ LANGUAGE sql;

-- ---------------------------------------------------------------- index inspection
CREATE FUNCTION pg_temp.pages(idx regclass)
RETURNS TABLE (blk int, flags int, gen bigint) AS $$
    SELECT b, bm25_debug_page_flags(idx, b), bm25_debug_page_seg_gen(idx, b)
      FROM generate_series(0, bm25_debug_npages(idx)::int - 1) b;
$$ LANGUAGE sql;
-- Field f of retired entry n.
CREATE FUNCTION pg_temp.entry(idx regclass, n int, f text) RETURNS bigint AS $$
    SELECT pg_temp.u32(bm25_debug_retired_entry_bytes(idx, n), pg_temp.off('BM25RetiredEntry', f));
$$ LANGUAGE sql;
-- The one live descriptor page (each index here has exactly one).
CREATE FUNCTION pg_temp.desc_blk(idx regclass) RETURNS int AS $$
    SELECT blk FROM pg_temp.pages(idx) WHERE flags & 256 <> 0 AND flags & 512 = 0;
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.retired_gens(idx regclass) RETURNS bigint[] AS $$
    SELECT array_agg(pg_temp.entry(idx, n, 'gen') ORDER BY n)
      FROM generate_series(0, bm25_debug_retired_count(idx)::int - 1) n;
$$ LANGUAGE sql;
-- Pages of the given retired gens not yet stamped DELETED: what a failed reclaim leaked.
CREATE FUNCTION pg_temp.leaked(idx regclass, gens bigint[]) RETURNS bigint AS $$
    SELECT count(*) FROM pg_temp.pages(idx) WHERE gen = ANY (gens) AND flags & 512 = 0;
$$ LANGUAGE sql;
-- A live page of the given kind: not DELETED, gen not retired, gen nonzero.
CREATE FUNCTION pg_temp.live_page(idx regclass, kind int, gens bigint[]) RETURNS int AS $$
    SELECT min(blk) FROM pg_temp.pages(idx)
     WHERE flags = kind AND gen <> 0 AND gen <> ALL (gens);
$$ LANGUAGE sql;

-- ---------------------------------------------------------------- forging
CREATE FUNCTION pg_temp.poke_entry(idx regclass, n int, f text, v bigint) RETURNS void AS $$
    SELECT bm25_debug_poke_page(idx, pg_temp.desc_blk(idx),
               pg_temp.off('page', 'contents') + n * pg_temp.size('BM25RetiredEntry', '*')
               + pg_temp.off('BM25RetiredEntry', f),
               pg_temp.enc(v));
$$ LANGUAGE sql;
CREATE FUNCTION pg_temp.poke_next(idx regclass, blk int, v bigint) RETURNS void AS $$
    SELECT bm25_debug_poke_page(idx, blk,
               pg_temp.off('page', 'special') + pg_temp.off('BM25PageOpaque', 'nextblk'),
               pg_temp.enc(v));
$$ LANGUAGE sql;

-- Four 50-doc segments merged into one (four retired entries on one descriptor page),
-- then a fifth live segment. Each further round merges four more segments, which puts
-- a second descriptor page on the list. Autovacuum off: a background VACUUM would take
-- the error.
CREATE FUNCTION pg_temp.mk(t text, rounds int DEFAULT 1) RETURNS void AS $$
DECLARE w text;
BEGIN
  EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off)', t);
  EXECUTE format('CREATE INDEX %I ON %I USING bm25_native (body)', t || '_bm', t);
  FOR r IN 0..rounds - 1 LOOP
    FOR k IN 0..3 LOOP
      w := (ARRAY['alpha', 'beta', 'gamma', 'delta'])[k + 1];
      EXECUTE format('INSERT INTO %I SELECT %s + g, %L || '' database storage'' FROM generate_series(1, 50) g',
                     t, 1000 * r + 100 * k, w);
      PERFORM bm25_seal((t || '_bm')::regclass);
    END LOOP;
    PERFORM bm25_merge((t || '_bm')::regclass);
    IF r = 0 THEN
      EXECUTE format('INSERT INTO %I SELECT 400 + g, ''epsilon database storage'' FROM generate_series(1, 50) g', t);
      PERFORM bm25_seal((t || '_bm')::regclass);
    END IF;
  END LOOP;
END $$ LANGUAGE plpgsql;

-- Every word reaches every one of its documents through the index.
CREATE FUNCTION pg_temp.hits(t text) RETURNS text AS $$
DECLARE r text := ''; n bigint; w text;
BEGIN
  FOREACH w IN ARRAY ARRAY['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'database'] LOOP
    EXECUTE format('SELECT count(*) FROM %I WHERE body @@@ %L', t, w) INTO n;
    r := r || w || '=' || n || ' ';
  END LOOP;
  RETURN rtrim(r);
END $$ LANGUAGE plpgsql;

SELECT pg_temp.mk(t) FROM unnest(ARRAY['r_zero', 'r_live', 'r_kind', 'r_cycle',
                                       'r_list', 'r_head', 'r_hdr']) t;
SELECT pg_temp.mk('r_del', 2);

-- r_ok, the healthy caller: a keyed, two-field index storing positions, so its retired
-- ranges carry every chain kind reclaim_one_range walks. Four seals, then a merge.
CREATE TABLE r_ok (id int PRIMARY KEY, title text, body text) WITH (autovacuum_enabled = off);
CREATE INDEX r_ok_bm ON r_ok USING bm25_native (title, body) INCLUDE (id)
  WITH (key_field = 'id');
DO $$ BEGIN
  FOR k IN 0..3 LOOP
    INSERT INTO r_ok SELECT 100 * k + g, 'title ' || g, 'body database storage ' || g
      FROM generate_series(1, 50) g;
    PERFORM bm25_seal('r_ok_bm');
  END LOOP;
  PERFORM bm25_merge('r_ok_bm');
END $$;

-- Host byte order, from a header block below 256: its low byte is the nonzero one.
UPDATE host SET le = get_byte(bm25_debug_retired_entry_bytes('r_zero_bm', 0),
                              pg_temp.off('BM25RetiredEntry', 'header_blkno')) <> 0;
SELECT pg_temp.entry('r_zero_bm', 0, 'header_blkno') < 256 AS header_decodes,
       bm25_debug_retired_count('r_zero_bm') AS entries,
       bm25_debug_retired_pages('r_zero_bm') AS descriptor_pages;

CREATE TEMP TABLE gens AS
SELECT t, pg_temp.retired_gens((t || '_bm')::regclass) AS g
  FROM unnest(ARRAY['r_zero', 'r_live', 'r_kind', 'r_cycle', 'r_list', 'r_head', 'r_hdr',
                    'r_del', 'r_ok']) t;
SELECT t, cardinality(g) AS retired_entries FROM gens ORDER BY t;

-- ================================================================ the corruptions
-- r_zero: entry 0's dictionary root is block 0.
SELECT pg_temp.poke_entry('r_zero_bm', 0, 'dict_root', 0);

-- r_live: entry 0's postings root is a live POST page of the merged segment. Its kind is
-- right, so only the gen check stands between it and the FSM.
CREATE TEMP TABLE live_target AS
SELECT pg_temp.live_page('r_live_bm', 16, g) AS blk FROM gens WHERE t = 'r_live';
SELECT pg_temp.poke_entry('r_live_bm', 0, 'posts_root', blk) FROM live_target;
SELECT bm25_debug_page_flags('r_live_bm', blk) AS live_flags_before FROM live_target;

-- r_kind: entry 0's dictionary root is its own segment's NORMS root. The gen is right,
-- so only the kind check stops it.
SELECT pg_temp.poke_entry('r_kind_bm', 0, 'dict_root',
                          pg_temp.entry('r_kind_bm', 0, 'norms_root'));

-- r_cycle: entry 0's one-page dictionary chain links back to itself. The walk stamps it,
-- comes back, and finds it DELETED; before the fix it re-stamped it until cancelled.
SELECT pg_temp.poke_next('r_cycle_bm', pg_temp.entry('r_cycle_bm', 0, 'dict_root')::int,
                         pg_temp.entry('r_cycle_bm', 0, 'dict_root'));

-- Advance the horizon past every merge's retire_xid (see 21_retired_descriptors).
SELECT pg_temp.wait_for_xmin_horizon();
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset

-- A statement that loops is a failure here, not a hang.
SET statement_timeout = '20s';

-- ================================================================ 0. a healthy index
-- The per-page checks accept every valid chain kind: r_ok's retired ranges span all eight
-- (header SEGCAT, DICT, POST, NORMS, LIVE, DOCMAP, KEYMAP, POS), and one VACUUM frees
-- every one of their pages without an error.
SELECT array_agg(DISTINCT p.flags ORDER BY p.flags) AS retired_kinds
  FROM gens, pg_temp.pages('r_ok_bm') p
 WHERE t = 'r_ok' AND p.gen = ANY (g);
SELECT pg_temp.leaked('r_ok_bm', g) > 0 AS retired_pages_present FROM gens WHERE t = 'r_ok';
VACUUM r_ok;
SELECT bm25_debug_retired_count('r_ok_bm') AS entries_after,
       pg_temp.leaked('r_ok_bm', g) AS retired_pages_not_freed,
       (SELECT count(*) FROM r_ok WHERE body @@@ 'database') AS hits
  FROM gens WHERE t = 'r_ok';

-- ================================================================ 1. block 0
VACUUM r_zero;
\echo :LAST_ERROR_SQLSTATE
-- The metapage is untouched (flags META only) and the index still answers.
SELECT bm25_debug_page_flags('r_zero_bm', 0) AS meta_flags;
SELECT pg_temp.hits('r_zero');
-- The compaction survived the abort: no entry is left to fail again. The pages the
-- failed walk never reached are leaked, not freed.
SELECT bm25_debug_retired_count('r_zero_bm') AS entries_after_error,
       pg_temp.leaked('r_zero_bm', g) > 0 AS pages_leaked
  FROM gens WHERE t = 'r_zero';
-- The next VACUUM succeeds and its orphan sweep frees the leak.
VACUUM r_zero;
SELECT pg_temp.leaked('r_zero_bm', g) AS leaked_after_sweep,
       bm25_debug_page_flags('r_zero_bm', 0) AS meta_flags
  FROM gens WHERE t = 'r_zero';
-- New segments reuse the freed pages; nothing live was among them.
INSERT INTO r_zero SELECT 500 + g, 'zeta database storage' FROM generate_series(1, 50) g;
SELECT bm25_seal('r_zero_bm');
SELECT pg_temp.hits('r_zero'), (SELECT count(*) FROM r_zero WHERE body @@@ 'zeta') AS zeta;

-- ================================================================ 2. a live chain
VACUUM r_live;
\echo :LAST_ERROR_SQLSTATE
SELECT bm25_debug_page_flags('r_live_bm', blk) AS live_flags_after FROM live_target;
SELECT bm25_debug_retired_count('r_live_bm') AS entries_after_error;
VACUUM r_live;
SELECT bm25_debug_page_flags('r_live_bm', blk) AS live_flags_after_sweep FROM live_target;
SELECT pg_temp.leaked('r_live_bm', g) AS leaked_after_sweep FROM gens WHERE t = 'r_live';
INSERT INTO r_live SELECT 500 + g, 'zeta database storage' FROM generate_series(1, 50) g;
SELECT bm25_seal('r_live_bm');
SELECT pg_temp.hits('r_live');

-- ================================================================ 3. wrong kind
VACUUM r_kind;
\echo :LAST_ERROR_SQLSTATE
SELECT bm25_debug_retired_count('r_kind_bm') AS entries_after_error;
VACUUM r_kind;
SELECT pg_temp.leaked('r_kind_bm', g) AS leaked_after_sweep FROM gens WHERE t = 'r_kind';
SELECT pg_temp.hits('r_kind');

-- ================================================================ 4. a cycle in a range
VACUUM r_cycle;
\echo :LAST_ERROR_SQLSTATE
SELECT bm25_debug_retired_count('r_cycle_bm') AS entries_after_error;
VACUUM r_cycle;
SELECT pg_temp.leaked('r_cycle_bm', g) AS leaked_after_sweep FROM gens WHERE t = 'r_cycle';
SELECT pg_temp.hits('r_cycle');

-- ================================================================ 5. a cycle in the list
-- The descriptor page links to itself, and its four entries are made unremovable (a
-- retire_xid far in the future), so no walk finds work to end it. Every walker of the
-- retired list stops at the visit cap instead of spinning: the three debug walkers, and
-- VACUUM, whose bm25_reclaim_retired holds the singleton while it walks.
CREATE TEMP TABLE list_desc AS SELECT pg_temp.desc_blk('r_list_bm') AS blk;
CREATE TEMP TABLE list_xid AS
SELECT n, bm25_debug_poke_page('r_list_bm', l.blk,
              pg_temp.off('page', 'contents') + n * pg_temp.size('BM25RetiredEntry', '*')
              + pg_temp.off('BM25RetiredEntry', 'retire_xid'),
              '\x7f7f7f7f7f7f7f7f') AS old
  FROM list_desc l, generate_series(0, 3) n;
SELECT pg_temp.poke_next('r_list_bm', blk, blk) FROM list_desc;
SELECT pg_temp.err_of($q$SELECT bm25_debug_retired_count('r_list_bm')$q$) AS retired_count;
SELECT pg_temp.err_of($q$SELECT bm25_debug_retired_pages('r_list_bm')$q$) AS retired_pages;
SELECT pg_temp.err_of($q$SELECT bm25_debug_retired_entry_bytes('r_list_bm', 1000)$q$)
       AS retired_entry_bytes;
VACUUM r_list;
\echo :LAST_ERROR_SQLSTATE
SELECT bm25_debug_page_flags('r_list_bm', 0) AS meta_flags,
       bm25_debug_page_flags('r_list_bm', blk) AS descriptor_flags
  FROM list_desc;
-- A link onto a page of another kind: bm25_debug_retired_pages now checks the kind its
-- siblings already did, rather than counting the page and following its link.
SELECT pg_temp.poke_next('r_list_bm', l.blk, pg_temp.live_page('r_list_bm', 8, g))
  FROM list_desc l, gens WHERE t = 'r_list';
SELECT pg_temp.err_of($q$SELECT bm25_debug_retired_pages('r_list_bm')$q$) AS retired_pages;
-- Repaired, the list is walkable again and VACUUM finishes, reclaiming all four ranges.
SELECT pg_temp.poke_next('r_list_bm', blk, 4294967295) FROM list_desc;
SELECT length(bm25_debug_poke_page('r_list_bm', l.blk,
                  pg_temp.off('page', 'contents') + x.n * pg_temp.size('BM25RetiredEntry', '*')
                  + pg_temp.off('BM25RetiredEntry', 'retire_xid'), x.old)) AS restored
  FROM list_desc l, list_xid x ORDER BY x.n;
SELECT bm25_debug_retired_pages('r_list_bm') AS descriptor_pages;
VACUUM r_list;
SELECT bm25_debug_retired_count('r_list_bm') AS entries,
       pg_temp.leaked('r_list_bm', g) AS leaked
  FROM gens WHERE t = 'r_list';
SELECT pg_temp.hits('r_list');

-- ================================================================ 6. back to the head
-- The same self-link with removable entries. The first chunk reclaims the four ranges
-- and leaves the emptied head linked; the next walk meets the head again as an emptied
-- page with a predecessor, which it used to unlink and free -- the head the metapage
-- still names -- once per chunk, without end.
CREATE TEMP TABLE head_desc AS SELECT pg_temp.desc_blk('r_head_bm') AS blk;
SELECT pg_temp.poke_next('r_head_bm', blk, blk) FROM head_desc;
VACUUM r_head;
\echo :LAST_ERROR_SQLSTATE
SELECT bm25_debug_page_flags('r_head_bm', blk) AS head_flags FROM head_desc;
SELECT pg_temp.poke_next('r_head_bm', blk, 4294967295) FROM head_desc;
VACUUM r_head;
SELECT bm25_debug_retired_count('r_head_bm') AS entries,
       bm25_debug_retired_pages('r_head_bm') AS descriptor_pages,
       pg_temp.leaked('r_head_bm', g) AS leaked
  FROM gens WHERE t = 'r_head';
SELECT pg_temp.hits('r_head');

-- ================================================================ 7. a freed descriptor
-- r_del has two descriptor pages. A clean VACUUM drains both, unlinks the older one and
-- stamps it DELETED. Linking the head back to it must not free it a second time.
VACUUM r_del;
SELECT bm25_debug_retired_count('r_del_bm') AS entries,
       bm25_debug_retired_pages('r_del_bm') AS descriptor_pages;
CREATE TEMP TABLE del_desc AS
SELECT pg_temp.desc_blk('r_del_bm') AS head,
       (SELECT min(blk) FROM pg_temp.pages('r_del_bm') WHERE flags = 256 + 512) AS freed;
SELECT pg_temp.poke_next('r_del_bm', head, freed) FROM del_desc;
SELECT pg_temp.err_of($q$SELECT bm25_debug_retired_pages('r_del_bm')$q$) AS retired_pages;
VACUUM r_del;
\echo :LAST_ERROR_SQLSTATE
SELECT pg_temp.poke_next('r_del_bm', head, 4294967295) FROM del_desc;
VACUUM r_del;
SELECT pg_temp.hits('r_del');
-- The same check through the probe that drives the shared validator directly.
SELECT pg_temp.err_of($q$SELECT bm25_debug_retired_page_bounds(256 + 512, 0)$q$) AS probe;

-- ================================================================ 8. a live header root of 0
-- The newest live segment's header names block 0 as its key-map root. A keyless index
-- never reads that chain, so before the fix a scan answered, and a merge copied the 0
-- into a retired descriptor for a later VACUUM to free the metapage. Three more
-- segments of its size make the merge take it.
CREATE TEMP TABLE hdr_target AS
SELECT blk FROM pg_temp.pages('r_hdr_bm')
 WHERE flags = 4 AND gen = (SELECT max(gen) FROM pg_temp.pages('r_hdr_bm') WHERE flags = 4);
DO $$ BEGIN
  FOR k IN 1..3 LOOP
    INSERT INTO r_hdr SELECT 500 + 100 * k + g, 'zeta database storage' FROM generate_series(1, 50) g;
    PERFORM bm25_seal('r_hdr_bm');
  END LOOP;
END $$;
SELECT length(bm25_debug_poke_page('r_hdr_bm', blk,
                                   pg_temp.off('page', 'contents') +
                                   pg_temp.off('BM25SegmentHeader', 'keymap_root'),
                                   pg_temp.enc(0))) AS poked
  FROM hdr_target;
SELECT bm25_debug_retired_count('r_hdr_bm') AS entries_before;
-- Every read of the header refuses it now: a scan, and the merge.
SELECT pg_temp.err_of($q$SELECT count(*) FROM r_hdr WHERE body @@@ 'alpha'$q$) AS scan;
SELECT pg_temp.err_of($q$SELECT bm25_merge('r_hdr_bm')$q$) AS merge;
SELECT bm25_debug_retired_count('r_hdr_bm') AS entries_after,
       bm25_debug_page_flags('r_hdr_bm', 0) AS meta_flags;
-- Repaired, the segment reads and merges again.
SELECT length(bm25_debug_poke_page('r_hdr_bm', blk,
                                   pg_temp.off('page', 'contents') +
                                   pg_temp.off('BM25SegmentHeader', 'keymap_root'),
                                   '\xffffffff')) AS repaired
  FROM hdr_target;
SELECT bm25_merge('r_hdr_bm');
SELECT bm25_debug_retired_count('r_hdr_bm') AS entries_after_merge;
SELECT pg_temp.hits('r_hdr'), (SELECT count(*) FROM r_hdr WHERE body @@@ 'zeta') AS zeta;

RESET statement_timeout;
RESET enable_seqscan;
DROP TABLE r_zero, r_live, r_kind, r_cycle, r_list, r_head, r_hdr, r_del, r_ok;
DROP EXTENSION bm25_native CASCADE;
