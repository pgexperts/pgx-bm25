-- Debug-surface privilege gate (review ref C7).
--
-- Every bm25_debug_* function is TEST-ONLY, yet PostgreSQL grants EXECUTE to
-- PUBLIC by default and the install script had no REVOKE anywhere.  The mutating
-- ones take a regclass and write pages under a Generic WAL window, and
-- index_open() validates relkind and NOTHING else -- not the access method, not
-- ownership.  So any unprivileged user could run
--
--     SELECT bm25_debug_stamp_version('pg_class_oid_index'::regclass, 0, 0, 0);
--
-- BM25PageGetMeta resolves to PageGetContents, so BM25MetaPageData.format_version
-- lands exactly on BTMetaPageData.btm_version; setting it to 0 makes _bt_getmeta
-- reject every pg_class OID lookup.  WAL-logged, so it replays on every standby.
--
-- Two independent layers are asserted here: the C-level gate
-- (bm25_index_open_owned: ownership + AM identity, before any page is touched --
-- the helper was called bm25_debug_open_index before ADR 0029 renamed it),
-- and the script-level REVOKE.
--
-- H13 (issue #52) extended the C layer to the READ-ONLY half of the surface. The
-- REVOKE already covered those functions, so PUBLIC could not reach them, but the
-- read-only bm25_debug_* SRFs still called index_open() directly: any role explicitly
-- granted EXECUTE -- a plausible thing to hand an ops or debugging role -- could dump
-- the dictionary, postings, positions, per-doc keys and live/dead counts of any bm25
-- index in the database, including indexes on tables it had no SELECT privilege on.
-- Pointed at a foreign AM they also read the special area as a 24-byte BM25PageOpaque
-- (btree's is 16), i.e. past pd_special. They now open through
-- bm25_index_open_readable, the same helper bm25_stats/bm25_wand_stats use: SELECT on
-- the INDEXED TABLE (not ownership -- reading an index's contents is reasonable for a
-- non-owner who can already read the rows) plus AM identity.

CREATE EXTENSION bm25_native;

CREATE TABLE dbgp (id int primary key, body text);
INSERT INTO dbgp VALUES (1, 'alpha beta'), (2, 'beta gamma');
CREATE INDEX dbgp_bm25 ON dbgp USING bm25_native (body);
SELECT bm25_seal('dbgp_bm25');

-- ------------------------------------------------------------ AM identity
-- dbgp_pkey is a btree.  Even as its OWNER -- ownership alone is not enough --
-- every mutating debug entry point must refuse it rather than write its block 0.
SELECT bm25_debug_stamp_version('dbgp_pkey'::regclass, 6, 6, 0);
SELECT bm25_debug_write_optional_region('dbgp_pkey'::regclass);
SELECT bm25_debug_alloc_unknown_page('dbgp_pkey'::regclass);
SELECT bm25_debug_pending_append('dbgp_pkey'::regclass, 'delta');
SELECT bm25_debug_seal_unpublished('dbgp_pkey'::regclass);
SELECT bm25_debug_stamp_segcat_empty('dbgp_pkey'::regclass, 0);
SELECT bm25_debug_clear_keystamp('dbgp_pkey'::regclass);
SELECT bm25_debug_set_pending_tail_epoch('dbgp_pkey'::regclass, 0);
-- The raw page lever: block 0 of a btree is its metapage, and a poke there is exactly
-- the damage the AM check exists to refuse.
SELECT bm25_debug_poke_page('dbgp_pkey'::regclass, 0, 24, '\x00');
-- #309 REGR sql/63: eight writers added after this list was written were never put
-- in it, so nothing showed the gate was on their path. Each takes arguments its own
-- validation accepts, so the refusal can only come from the gate. The three PUBLIC
-- maintenance functions open through the same gate and are listed with them.
-- test/check_owned_gate_coverage.py (a CI step) derives the gate's callers from
-- src/ and fails if any is missing here, so the list cannot drift again.
SELECT bm25_debug_pending_invalidate_page('dbgp_pkey'::regclass, 1);
SELECT bm25_debug_stamp_seg_field_count('dbgp_pkey'::regclass, 0, 1);
SELECT bm25_debug_stamp_chain_next('dbgp_pkey'::regclass, 'post', 0, 0, 0);
SELECT bm25_debug_stamp_chain_lower('dbgp_pkey'::regclass, 'post', 0, 0, 0);
SELECT bm25_debug_stamp_post_block('dbgp_pkey'::regclass, 0, 0, 0, 'ndocs', 1);
SELECT bm25_debug_stamp_docmap_tid('dbgp_pkey'::regclass, 0, 0, '(0,1)');
SELECT bm25_debug_stamp_seg_root('dbgp_pkey'::regclass, 0, 'live', 0);
SELECT bm25_debug_livedocs_clear('dbgp_pkey'::regclass, 0, 0);
SELECT bm25_seal('dbgp_pkey'::regclass);
SELECT bm25_merge('dbgp_pkey'::regclass);
SELECT bm25_upgrade('dbgp_pkey'::regclass);

-- The btree is untouched and still usable.
SELECT id FROM dbgp WHERE id = 2;

-- The same calls against a real bm25 index owned by the caller still work.
SELECT bm25_debug_stamp_version('dbgp_bm25'::regclass, 6, 6, 0);
SELECT bm25_debug_write_optional_region('dbgp_bm25'::regclass);
SELECT bm25_debug_check_optional_region('dbgp_bm25'::regclass);
-- Poking the opaque's unused pad with the bytes it already holds changes nothing.
SELECT bm25_debug_poke_page('dbgp_bm25'::regclass, 0,
         (SELECT off FROM bm25_debug_layout() WHERE struct = 'page' AND field = 'special')
       + (SELECT off FROM bm25_debug_layout() WHERE struct = 'BM25PageOpaque' AND field = 'unused'),
       '\x0000') AS poke_owned_bm25;

-- ------------------------------------------------------ AM identity, readers
-- The read-only half must refuse a foreign AM too -- not because it can corrupt it,
-- but because it reads bm25 page layouts: the special area comes back as a 24-byte
-- BM25PageOpaque where btree's is 16, so the flags/nextblk/seg_gen it reports come
-- from past pd_special. Owning the btree is not enough, same as above.
SELECT * FROM bm25_debug_segterms('dbgp_pkey'::regclass);
SELECT * FROM bm25_debug_segcat('dbgp_pkey'::regclass);
SELECT bm25_debug_npages('dbgp_pkey'::regclass);
SELECT * FROM bm25_debug_keystamp('dbgp_pkey'::regclass);
SELECT * FROM bm25_debug_terms('dbgp_pkey'::regclass);

-- Against the real bm25 index they still work (the gate is not just refusing).
SELECT count(*) > 0 AS segterms_ok FROM bm25_debug_segterms('dbgp_bm25'::regclass);
SELECT count(*) AS segcat_rows  FROM bm25_debug_segcat('dbgp_bm25'::regclass);

-- ------------------------------------------------------------ REVOKE sweep
-- ENUMERATE AND PIN, rather than re-running the install script's own predicate.
--
-- This block used to be `count(*) WHERE proname LIKE 'bm25\_debug\_%' AND ... AND
-- has_function_privilege('public', ...)`, expecting 0 -- a WHERE clause that was
-- character-for-character identical to the install script's REVOKE loop. A test that
-- reuses the implementation's predicate can only ever confirm the predicate is
-- self-consistent; it cannot notice that the predicate is the WRONG predicate. It did
-- not: bm25_wand_stats is a debug probe that does not carry the bm25_debug_ prefix, so
-- it was invisible to the loop AND to the test asserting the loop worked, and shipped
-- PUBLIC-executable through a suite named "debug privileges" (SQL-03).
--
-- The old sanity companion (`count(*) > 40`) could not rescue it either: it only proved
-- the pattern matched something, and its threshold left so much headroom that the
-- surface could have halved and stayed green.
--
-- So: list the ENTIRE public-executable surface by name and pin it. Guilty until named
-- innocent. Adding a function that is PUBLIC-executable -- deliberately or by
-- forgetting -- changes this output and fails the suite, which is a diff a reviewer
-- reads rather than a count they have to recompute. Note what is NOT here: any
-- bm25_debug_* function, and bm25_wand_stats.
SELECT p.proname || '(' || pg_get_function_arguments(p.oid) || ')' AS public_executable
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
  JOIN pg_language l ON l.oid = p.prolang
 WHERE n.nspname = current_schema()
   AND l.lanname = 'c'
   AND p.proname LIKE 'bm25%'
   AND has_function_privilege('public', p.oid, 'EXECUTE')
 -- COLLATE "C": the default collation decides whether '(' sorts before '_', so under
 -- ICU or glibc en_US.UTF-8 bm25_match_jsonb sorts BEFORE bm25_match(text, text) and
 -- this pinned list fails on a cluster CI never runs (every CI leg here is C-locale).
 -- Byte order is the only order that is the same everywhere.
 ORDER BY 1 COLLATE "C";

-- Independent of the list above: nothing named bm25_debug_* may be public, and
-- neither may bm25_wand_stats. Stated separately so the specific regression that
-- motivated this rewrite has an assertion of its own that does not depend on the
-- pinned list being read carefully.
SELECT count(*) AS public_debug_or_wand_stats
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
  JOIN pg_language l ON l.oid = p.prolang
 WHERE n.nspname = current_schema()
   AND l.lanname = 'c'
   AND (p.proname LIKE 'bm25\_debug\_%' OR p.proname = 'bm25_wand_stats')
   AND has_function_privilege('public', p.oid, 'EXECUTE');

-- The raw page lever and its layout table by name, so the allowlist loop's coverage of
-- them does not rest on the prefix query above (issue #302/#303 test lever).
SELECT has_function_privilege('public', 'bm25_debug_poke_page(regclass, bigint, int, bytea)',
                              'EXECUTE') AS poke_public,
       has_function_privilege('public', 'bm25_debug_layout()', 'EXECUTE') AS layout_public;

-- Sanity: the schema really does hold a large C-function surface, so a query that
-- matched nothing cannot masquerade as a clean result. Deliberately expressed as a
-- floor on the TOTAL, not on the prefix-matched subset -- the whole point above is
-- that the prefix is not the right lens.
SELECT (count(*) > 80) AS surface_present
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
  JOIN pg_language l ON l.oid = p.prolang
 WHERE n.nspname = current_schema()
   AND l.lanname = 'c'
   AND p.proname LIKE 'bm25%';

-- ------------------------------------------------------------ ownership
-- A non-superuser explicitly granted EXECUTE (PUBLIC no longer has it) still
-- cannot aim a mutating function at an index it does not own.
--
-- EVERY owned-gate caller is listed here, not one representative (#309). The AM
-- identity block above cannot tell the two gates apart -- both run the same AM check,
-- so a btree is refused identically through either -- and a writer moved onto
-- bm25_index_open_readable would have passed it. This role has no SELECT on dbgp, so
-- the owned gate answers "must be owner of index" and the readable gate would answer
-- "permission denied for table": a swap changes this output.
-- test/check_owned_gate_coverage.py requires each caller's line here, followed by the
-- ownership error in the expected output. Names without argument lists: each is
-- unique, and the list then cannot disagree with the install script's signatures.
CREATE ROLE bm25_c7_other NOLOGIN;
GRANT EXECUTE ON FUNCTION
    bm25_debug_stamp_version,
    bm25_debug_write_optional_region,
    bm25_debug_alloc_unknown_page,
    bm25_debug_pending_append,
    bm25_debug_seal_unpublished,
    bm25_debug_stamp_segcat_empty,
    bm25_debug_clear_keystamp,
    bm25_debug_set_pending_tail_epoch,
    bm25_debug_pending_invalidate_page,
    bm25_debug_stamp_seg_field_count,
    bm25_debug_stamp_chain_next,
    bm25_debug_stamp_chain_lower,
    bm25_debug_stamp_post_block,
    bm25_debug_stamp_docmap_tid,
    bm25_debug_stamp_seg_root,
    bm25_debug_livedocs_clear,
    bm25_debug_poke_page
  TO bm25_c7_other;
GRANT USAGE ON SCHEMA public TO bm25_c7_other;

SET ROLE bm25_c7_other;
SELECT bm25_debug_stamp_version('dbgp_bm25'::regclass, 6, 6, 0);
SELECT bm25_debug_write_optional_region('dbgp_bm25'::regclass);
SELECT bm25_debug_alloc_unknown_page('dbgp_bm25'::regclass);
SELECT bm25_debug_pending_append('dbgp_bm25'::regclass, 'delta');
SELECT bm25_debug_seal_unpublished('dbgp_bm25'::regclass);
SELECT bm25_debug_stamp_segcat_empty('dbgp_bm25'::regclass, 0);
SELECT bm25_debug_clear_keystamp('dbgp_bm25'::regclass);
SELECT bm25_debug_set_pending_tail_epoch('dbgp_bm25'::regclass, 0);
SELECT bm25_debug_pending_invalidate_page('dbgp_bm25'::regclass, 1);
SELECT bm25_debug_stamp_seg_field_count('dbgp_bm25'::regclass, 0, 1);
SELECT bm25_debug_stamp_chain_next('dbgp_bm25'::regclass, 'post', 0, 0, 0);
SELECT bm25_debug_stamp_chain_lower('dbgp_bm25'::regclass, 'post', 0, 0, 0);
SELECT bm25_debug_stamp_post_block('dbgp_bm25'::regclass, 0, 0, 0, 'ndocs', 1);
SELECT bm25_debug_stamp_docmap_tid('dbgp_bm25'::regclass, 0, 0, '(0,1)');
SELECT bm25_debug_stamp_seg_root('dbgp_bm25'::regclass, 0, 'live', 0);
SELECT bm25_debug_livedocs_clear('dbgp_bm25'::regclass, 0, 0);
SELECT bm25_debug_poke_page('dbgp_bm25'::regclass, 0, 24, '\x00');
SELECT bm25_seal('dbgp_bm25'::regclass);
SELECT bm25_merge('dbgp_bm25'::regclass);
SELECT bm25_upgrade('dbgp_bm25'::regclass);
RESET ROLE;

-- The index survived the attempt: it still reads normally.
SET enable_seqscan = off;
SELECT id FROM dbgp WHERE body @@@ 'beta' ORDER BY id;
RESET enable_seqscan;

-- ------------------------------------------------------ table SELECT, readers
-- H13: a role explicitly granted EXECUTE on a read-only debug SRF still may not read
-- an index whose table it cannot SELECT. This is the gap the REVOKE alone did not
-- close -- granting EXECUTE was enough to dump any bm25 index in the database.
GRANT EXECUTE ON FUNCTION bm25_debug_segterms(regclass) TO bm25_c7_other;
GRANT EXECUTE ON FUNCTION bm25_debug_seg_keymap(regclass, int) TO bm25_c7_other;
GRANT USAGE ON SCHEMA public TO bm25_c7_other;

SET ROLE bm25_c7_other;
-- No SELECT on dbgp: refused on the TABLE, not the index or the function.
SELECT * FROM bm25_debug_segterms('dbgp_bm25'::regclass);
SELECT * FROM bm25_debug_seg_keymap('dbgp_bm25'::regclass, 0);
RESET ROLE;

-- With SELECT on the indexed table the same role is allowed: the gate is the table
-- privilege, deliberately not ownership -- a role that can already read the rows
-- learns nothing new from the dictionary.
GRANT SELECT ON dbgp TO bm25_c7_other;
SET ROLE bm25_c7_other;
SELECT count(*) > 0 AS segterms_allowed_with_select
  FROM bm25_debug_segterms('dbgp_bm25'::regclass);
RESET ROLE;
REVOKE SELECT ON dbgp FROM bm25_c7_other;

REVOKE EXECUTE ON FUNCTION bm25_debug_segterms(regclass) FROM bm25_c7_other;
REVOKE EXECUTE ON FUNCTION bm25_debug_seg_keymap(regclass, int) FROM bm25_c7_other;
REVOKE EXECUTE ON FUNCTION
    bm25_debug_stamp_version,
    bm25_debug_write_optional_region,
    bm25_debug_alloc_unknown_page,
    bm25_debug_pending_append,
    bm25_debug_seal_unpublished,
    bm25_debug_stamp_segcat_empty,
    bm25_debug_clear_keystamp,
    bm25_debug_set_pending_tail_epoch,
    bm25_debug_pending_invalidate_page,
    bm25_debug_stamp_seg_field_count,
    bm25_debug_stamp_chain_next,
    bm25_debug_stamp_chain_lower,
    bm25_debug_stamp_post_block,
    bm25_debug_stamp_docmap_tid,
    bm25_debug_stamp_seg_root,
    bm25_debug_livedocs_clear,
    bm25_debug_poke_page
  FROM bm25_c7_other;
REVOKE USAGE ON SCHEMA public FROM bm25_c7_other;
DROP ROLE bm25_c7_other;

DROP TABLE dbgp;
DROP EXTENSION bm25_native;
