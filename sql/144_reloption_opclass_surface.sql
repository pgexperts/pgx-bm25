-- 144_reloption_opclass_surface: three input-validation gaps on the reloption and
-- opclass surface (#304 SCORE-02, SURFACE-06, SURFACE-09).
--
--  (1) SCORE-02: k1_<col> is capped at 1e30 (the index-wide k1's cap) and
--      boost_<col> at 1e6, at CREATE INDEX / ALTER INDEX SET only. A value already
--      stored in pg_class.reloptions is still parsed leniently by the scan and the
--      INSERT path, so an index carrying one never wedges (the #292 pattern).
--  (2) SURFACE-06: a per-field knob whose suffix names no key column (a typo, or an
--      INCLUDE column) draws a WARNING at CREATE INDEX and REINDEX, not an ERROR.
--  (3) SURFACE-09: a superuser-built opclass whose operator takes a right-hand type
--      the scan cannot decode. Before the fix the scan read an int4 argument as a
--      text pointer and the backend crashed; now every such scan key is a clean
--      ERROR, and amvalidate reports the member.
\set VERBOSITY terse
CREATE EXTENSION IF NOT EXISTS bm25_native;

-- No primary key: with enable_seqscan off, PG17 would otherwise pick a full
-- btree scan plus the always-true operator function in section (3) over the
-- bm25 index, and the section would stop exercising the AM.
CREATE TABLE rs (id int, title text, body text)
  WITH (autovacuum_enabled = off);
INSERT INTO rs VALUES
  (1, 'red car', 'red red red car'),
  (2, 'blue car', 'a red bicycle'),
  (3, 'green van', 'red and green'),
  (4, 'yellow bus', 'nothing to see');

-- ---------------------------------------------------------------------------
-- (1) SCORE-02: upper caps at DDL time.
-- ---------------------------------------------------------------------------
CREATE INDEX rs_k1_huge ON rs USING bm25_native (title, body) WITH (k1_body = '1e308');
CREATE INDEX rs_boost_huge ON rs USING bm25_native (title, body) WITH (boost_body = '1e308');
CREATE INDEX rs_k1_over ON rs USING bm25_native (title, body) WITH (k1_body = '1.1e30');
CREATE INDEX rs_boost_over ON rs USING bm25_native (title, body) WITH (boost_body = '1000001');
-- the boundary values themselves are accepted
CREATE INDEX rs_caps ON rs USING bm25_native (title, body)
  WITH (k1_body = '1e30', boost_body = '1e6');
DROP INDEX rs_caps;
-- ALTER INDEX SET is the other DDL path
CREATE INDEX rs_idx ON rs USING bm25_native (title, body) INCLUDE (id)
  WITH (key_field = 'id');
ALTER INDEX rs_idx SET (k1_title = '1e31');
ALTER INDEX rs_idx SET (boost_title = '2e6');
ALTER INDEX rs_idx SET (k1_title = '2.0', boost_title = '3');
SELECT array_agg(o ORDER BY o) FROM unnest((SELECT reloptions FROM pg_class
  WHERE oid = 'rs_idx'::regclass)) o;

-- An out-of-range value already stored (as a pre-fix ALTER could leave it,
-- simulated here by writing the catalog directly) is NOT re-checked by the
-- ranked scan or by INSERT: the cap binds DDL only. Moving the cap into the
-- shared value parser would make both statements below ERROR.
UPDATE pg_class SET reloptions = ARRAY['key_field=id', 'k1_body=1e308',
                                        'boost_title=1e300']
 WHERE oid = 'rs_idx'::regclass;
SET enable_seqscan = off;
SELECT count(*) AS ranked_rows_with_stored_huge_knobs FROM
  (SELECT id FROM rs WHERE body @@@ 'red' ORDER BY body &@@ 'red' LIMIT 10) s;
INSERT INTO rs VALUES (5, 'red truck', 'red');
SELECT array_agg(id ORDER BY id) AS insert_then_match FROM rs WHERE body @@@ 'red';
RESET enable_seqscan;
-- the repair: RESET the stored value, after which SET works again
ALTER INDEX rs_idx SET (k1_title = '2.0');
ALTER INDEX rs_idx RESET (k1_body, boost_title);
ALTER INDEX rs_idx SET (k1_title = '2.0');
DROP INDEX rs_idx;

-- ---------------------------------------------------------------------------
-- (2) SURFACE-06: unmatched per-field knob suffix -> WARNING, index still built.
-- ---------------------------------------------------------------------------
\set VERBOSITY default
-- a typo of a key column
CREATE INDEX rs_typo ON rs USING bm25_native (title, body) WITH (k1_titel = '2.0');
-- an INCLUDE column is not a field: per-field knobs on it have no effect
CREATE INDEX rs_incl ON rs USING bm25_native (title, body) INCLUDE (id)
  WITH (key_field = 'id', boost_id = '2');
-- every knob family, matched and unmatched together: one WARNING per unmatched
CREATE INDEX rs_mix ON rs USING bm25_native (title, body)
  WITH (k1_title = '1.5', b_bdy = '0.5', boost_body = '2',
        store_positions_nosuch = 'off');
\set VERBOSITY terse
SELECT relname FROM pg_class WHERE relname IN ('rs_typo', 'rs_incl', 'rs_mix')
 ORDER BY relname;
-- a matched knob alone draws no WARNING
CREATE INDEX rs_ok ON rs USING bm25_native (title, body) WITH (k1_title = '1.5');
-- an ALTER'd typo is not reported at ALTER (amoptions has no column list) and,
-- above all, does not break INSERT or a scan ...
ALTER INDEX rs_ok SET (boost_nosuch = '3');
INSERT INTO rs VALUES (6, 'red kite', 'kite');
SET enable_seqscan = off;
SELECT array_agg(id ORDER BY id) AS after_altered_typo FROM rs WHERE title @@@ 'red';
RESET enable_seqscan;
-- ... and REINDEX reports it, with the RESET hint
\set VERBOSITY default
REINDEX INDEX rs_ok;
\set VERBOSITY terse
DROP INDEX rs_typo, rs_incl, rs_mix, rs_ok;

-- ---------------------------------------------------------------------------
-- (3) SURFACE-09: an operator whose right-hand type the scan cannot decode.
-- ---------------------------------------------------------------------------
-- plpgsql, not SQL: an inlinable SQL function would be folded away and the
-- clause would never become an index qual.
CREATE FUNCTION s9_int(text, int4) RETURNS bool LANGUAGE plpgsql IMMUTABLE
  AS $$ BEGIN RETURN true; END $$;
CREATE OPERATOR @@@ (LEFTARG = text, RIGHTARG = int4, PROCEDURE = s9_int);
-- a domain over text IS decodable: the gate must not refuse it
CREATE DOMAIN s9_textdom AS text;
CREATE FUNCTION s9_dom(text, s9_textdom) RETURNS bool LANGUAGE plpgsql IMMUTABLE
  AS $$ BEGIN RETURN true; END $$;
CREATE OPERATOR @@@ (LEFTARG = text, RIGHTARG = s9_textdom, PROCEDURE = s9_dom);
CREATE OPERATOR CLASS s9_ops FOR TYPE text USING bm25_native AS
  OPERATOR 1 @@@ (text, text),
  OPERATOR 1 @@@ (text, int4),
  OPERATOR 1 @@@ (text, s9_textdom),
  OPERATOR 2 &@@ (text, text) FOR ORDER BY float_ops;

-- amvalidate reports the int4 member, and only it (the domain member is fine)
SELECT amvalidate(oid) AS s9_ops_valid FROM pg_opclass
 WHERE opcname = 's9_ops'
   AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'bm25_native');

CREATE INDEX rs_s9 ON rs USING bm25_native (body s9_ops);
SET enable_seqscan = off;
-- the plan really is an index scan on the int4 operator, so the statements
-- below reach the AM (pre-fix: the first one crashed the backend)
EXPLAIN (COSTS OFF) SELECT id FROM rs WHERE body @@@ 42;
-- as the scan's own query
SELECT id FROM rs WHERE body @@@ 42;
-- as a second WHERE key, which is compared with the first before it is parsed
SELECT id FROM rs WHERE body @@@ 'red' AND body @@@ 42;
-- as a WHERE key under a ranked scan
SELECT id FROM rs WHERE body @@@ 42 ORDER BY body &@@ 'red' LIMIT 3;
-- a domain-over-text argument still runs through the index (the operator
-- function returns true for every row, so only the index can narrow the set)
EXPLAIN (COSTS OFF) SELECT id FROM rs WHERE body @@@ 'red'::s9_textdom;
SELECT array_agg(id ORDER BY id) AS domain_rhs FROM rs
 WHERE body @@@ 'red'::s9_textdom;
RESET enable_seqscan;

DROP INDEX rs_s9;
DROP OPERATOR CLASS s9_ops USING bm25_native;
DROP OPERATOR FAMILY s9_ops USING bm25_native;
DROP OPERATOR @@@ (text, int4);
DROP OPERATOR @@@ (text, s9_textdom);
DROP FUNCTION s9_int(text, int4);
DROP FUNCTION s9_dom(text, s9_textdom);
DROP DOMAIN s9_textdom;
DROP TABLE rs;
-- pg_regress shares ONE database across suites; leave no extension behind.
DROP EXTENSION bm25_native;
