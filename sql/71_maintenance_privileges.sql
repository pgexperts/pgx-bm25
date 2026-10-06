-- H5 (issue #45), and the body of issue #42: the PUBLIC maintenance surface took a
-- regclass and index_open()ed it with no privilege check at all.
--
-- index_open() validates relkind and NOTHING else -- not the access method, not the
-- ACL, not ownership. So any role that could connect could seal, merge, re-stamp
-- and (with bm25_debug_enable_synthetic_transform) fully re-emit every segment of
-- an index it has no rights to, repeatable as cheap WAL amplification plus
-- retired-page bloat.
--
-- A REVOKE is not the answer here the way it was for the debug surface (ADR 0020):
-- these functions are meant to be callable, just not against someone else's index.
-- So they route through bm25_index_open_owned -- ownership + AM identity, before
-- any page is touched -- and the read-only reporters through
-- bm25_index_open_readable, which asks for SELECT on the indexed TABLE instead.
--
-- Note the regclass input cannot be filtered upstream: a bare numeric OID cast to
-- regclass resolves with no schema privileges at all, which is asserted below.
CREATE EXTENSION bm25_native;

CREATE TABLE mp (id int PRIMARY KEY, body text);
INSERT INTO mp VALUES (1, 'alpha beta'), (2, 'beta gamma');
CREATE INDEX mp_idx ON mp USING bm25_native (body);

-- ------------------------------------------------------------ owner
-- The owner is unaffected: every maintenance function still works.
SELECT bm25_seal('mp_idx');
SELECT bm25_merge('mp_idx');
SELECT bm25_upgrade('mp_idx');
SELECT ndocs FROM bm25_stats('mp_idx');

-- ------------------------------------------------------------ AM identity
-- Ownership alone is not enough: aimed at a btree the caller owns, each must
-- refuse rather than write bm25 structures onto btree pages.
SELECT bm25_seal('mp_pkey');
SELECT bm25_merge('mp_pkey');
SELECT bm25_upgrade('mp_pkey');
SELECT ndocs FROM bm25_stats('mp_pkey');

-- ------------------------------------------------------------ non-owner
CREATE ROLE bm25_h5_reader NOLOGIN;
CREATE ROLE bm25_h5_stranger NOLOGIN;
GRANT USAGE ON SCHEMA public TO bm25_h5_reader, bm25_h5_stranger;
GRANT SELECT ON mp TO bm25_h5_reader;

-- A role WITH SELECT still may not mutate: these are owner-only.
SET ROLE bm25_h5_reader;
SELECT bm25_seal('mp_idx');
SELECT bm25_merge('mp_idx');
SELECT bm25_upgrade('mp_idx');
-- ...but reporting is allowed, because SELECT on the table is the gate there.
SELECT ndocs FROM bm25_stats('mp_idx');
RESET ROLE;

-- A role WITHOUT SELECT gets nothing, including the read-only reporter: the stats
-- must not become a side channel around table privileges.
SET ROLE bm25_h5_stranger;
SELECT bm25_seal('mp_idx');
SELECT ndocs FROM bm25_stats('mp_idx');
RESET ROLE;

-- ------------------------------------------------------------ numeric OID
-- A schema-qualified regclass literal would hit LookupExplicitNamespace's USAGE
-- check, but a bare numeric OID cast needs no schema privileges at all -- so the
-- ownership check, not name resolution, is what has to hold. Asserted with the OID
-- resolved by the (world-readable) catalog, exactly as an attacker would.
SET ROLE bm25_h5_stranger;
SELECT bm25_seal((SELECT oid FROM pg_class WHERE relname = 'mp_idx')::regclass);
SELECT bm25_upgrade((SELECT oid FROM pg_class WHERE relname = 'mp_idx')::regclass);
RESET ROLE;

-- ------------------------------------------------------------ the toggle
-- bm25_debug_enable_synthetic_transform forces bm25_upgrade down the full-rewrite
-- path. It matches bm25_debug_%, so the install script's REVOKE loop already
-- covers it -- asserted here so that stays true.
SELECT has_function_privilege('public', 'bm25_debug_enable_synthetic_transform(boolean)', 'EXECUTE')
       AS toggle_public_executable;

-- ------------------------------------------------------------ intact
-- The index survived every rejected attempt and still answers correctly.
SET enable_seqscan = off;
SELECT array_agg(id ORDER BY id) AS beta_rows FROM mp WHERE body @@@ 'beta';
RESET enable_seqscan;

REVOKE SELECT ON mp FROM bm25_h5_reader;
REVOKE USAGE ON SCHEMA public FROM bm25_h5_reader, bm25_h5_stranger;
DROP ROLE bm25_h5_reader, bm25_h5_stranger;
DROP TABLE mp;
DROP EXTENSION bm25_native;
