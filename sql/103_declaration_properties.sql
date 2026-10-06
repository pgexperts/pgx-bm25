-- 103_declaration_properties: a function's DECLARED properties are part of its
-- contract, and this file's were mostly defaults by omission (#148 SQL-01, -02,
-- -04, -05, -09, -10, -13).
--
-- Why a suite exists for what looks like catalog tidiness: two of these had
-- measured consequences and none of them is visible to any answer-checking test.
-- A wrong provolatile, proparallel, procost or prorows changes WHICH PLAN runs,
-- not which rows come back, so every existing suite passes identically before and
-- after -- the same shape as sql/86_errcodes_and_amvalidate (a wrong SQLSTATE is
-- not in stdout unless a test puts it there) and sql/70_distance_volatility.
--
-- Also note what this suite cannot assert and does not pretend to: SQL-11 changed
-- bm25_native.control's module_pathname from a bare name to '$libdir/bm25_native'.
-- Nothing in the catalog records it. The witness is that this suite runs at all --
-- CREATE EXTENSION below resolves MODULE_PATHNAME through the new string.
CREATE EXTENSION bm25_native;

-- Two of the assertions below are about WHICH PLAN the planner chose, and a plan
-- assertion that pastes EXPLAIN output is brittle across environments (worker
-- counts, node labels, row estimates). Reduce each to one boolean instead: does the
-- chosen plan contain this node? Both uses are canaried -- an environment where the
-- expected node does not appear at all fails visibly rather than passing vacuously.
CREATE FUNCTION pg_temp.plan_has(q text, pat text) RETURNS bool
LANGUAGE plpgsql AS $$
DECLARE ln text;
BEGIN
  FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF ln LIKE '%' || pat || '%' THEN RETURN true; END IF;
  END LOOP;
  RETURN false;
END $$;

-- ------------------------------------------------------------ SQL-01 catalog
-- bm25_boost stays IMMUTABLE, but only because its rendering was fixed. The other
-- five builders were always honest. provolatile: i=immutable s=stable v=volatile.
-- proparallel: s=safe u=unsafe r=restricted.
SELECT p.proname, p.provolatile, p.proparallel
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname IN ('bm25_match_terms', 'bm25_term', 'bm25_phrase',
                     'bm25_wildcard', 'bm25_boolean', 'bm25_boost')
 ORDER BY 1;

-- ------------------------------------------------------------ SQL-01 rendering
-- The defect: jsonb_build_object routes a float8 through float8out, which honours
-- the PGC_USERSET GUC extra_float_digits, so an IMMUTABLE-declared function had a
-- session-dependent result. Two renderings of the SAME call under two settings must
-- now be one distinct value. Captured into a table rather than compared inline
-- because a single statement can only have one setting in effect.
CREATE TEMP TABLE boost_render (setting int, v jsonb);
SET extra_float_digits = 1;      -- the default
INSERT INTO boost_render VALUES (1, bm25_boost(1.0/3.0, '{"x":1}'::jsonb));
SET extra_float_digits = -3;
INSERT INTO boost_render VALUES (-3, bm25_boost(1.0/3.0, '{"x":1}'::jsonb));
RESET extra_float_digits;
SELECT count(DISTINCT v) AS distinct_renderings FROM boost_render;
SELECT DISTINCT v AS rendered FROM boost_render;

-- ------------------------------------------------------------ SQL-01 functional index
-- The sharpest consequence, and the one that produces WRONG ANSWERS rather than a
-- bad plan. IMMUTABLE is what admits a function to an index expression, and the
-- planner checks only the DECLARED volatility -- it does not inline the body to
-- find out. So the index stored the value rendered under the BUILDING session's
-- extra_float_digits, and a later session with a different setting probed it with a
-- different string.
--
-- Both answers are printed. Pre-fix they diverge (index path 0, heap path 1) --
-- which is also why the heap path is here: an assertion on the index count alone
-- would not distinguish "the fix worked" from "the row was never there".
CREATE TABLE fidx (w float8, q jsonb);
SET extra_float_digits = 1;
INSERT INTO fidx VALUES (1.0/3.0, '{"x":1}');
CREATE INDEX fidx_expr ON fidx ((bm25_boost(w, q)));
SET extra_float_digits = -3;

SET enable_seqscan = off;
-- CANARY: without this the count below is not evidence of anything. If the planner
-- did not reach fidx_expr, both paths are the heap path and both answer 1 whether or
-- not the fix is in.
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM fidx
       WHERE bm25_boost(w, q) = bm25_boost((1.0/3.0)::float8, '{"x":1}'::jsonb) $q$,
  'fidx_expr') AS uses_expression_index;
SELECT count(*) AS via_expression_index FROM fidx
 WHERE bm25_boost(w, q) = bm25_boost((1.0/3.0)::float8, '{"x":1}'::jsonb);
RESET enable_seqscan;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS via_heap FROM fidx
 WHERE bm25_boost(w, q) = bm25_boost((1.0/3.0)::float8, '{"x":1}'::jsonb);
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET extra_float_digits;
DROP TABLE fidx;

-- ------------------------------------------------------------ SQL-01 parser
-- The rendering changed from a float8 literal to a numeric one. jsonb stores every
-- number as a Numeric internally either way, so src/bm25_query.c's parse_boost
-- (jbvNumeric -> numeric_float8) should be indifferent -- assert it rather than
-- assume it.
--
-- What these three DO prove is that the C side accepts and evaluates the new
-- rendering: it parses, it flattens, and the boosted query returns the right rows.
-- What they do NOT prove is which rendering produced them -- both probes print at
-- ~6 significant digits, so their output is identical pre- and post-fix. The
-- rendering itself is pinned by the two sections above; this section is the
-- transparency claim, not a second witness for it.
CREATE TABLE bdocs (id int primary key, body text);
INSERT INTO bdocs VALUES (1, 'tort negligence'), (2, 'contract liability tort'),
                         (3, 'unrelated prose');
CREATE INDEX bdocs_bm ON bdocs USING bm25_native (body);
SELECT bm25_debug_query_parse('bdocs_bm', bm25_boost(2.5, bm25_term('body', 'tort')));
SELECT bm25_debug_query_flatten('bdocs_bm', bm25_boost(1.0/3.0, bm25_term('body', 'tort')));
SELECT array_agg(id ORDER BY id) AS boosted_match
  FROM bdocs WHERE body @@@ bm25_boost(2.5, bm25_term('body', 'tort'));

-- ...and the narrow band of inputs the rendering change BROKE, bracketed at the exact
-- edge. float8_numeric rounds to 15 significant digits, round-to-NEAREST, so a weight
-- near DBL_MAX rounds OUTWARD past it and numeric_float8 overflows reading it back.
-- Measured: the top FOUR doubles overflow (DBL_MAX = 1.7976931348623157e308 down to
-- 1.7976931348623151e308); the fifth round-trips. The two values below are ADJACENT
-- doubles, so they pin the boundary itself rather than merely straddling it -- 1e308
-- would be ~4e15 ULPs away and would prove nothing about where the edge is.
--
-- Asserted through a SQLSTATE handler rather than by pinning the message, which is a
-- 300-character rendering of the out-of-range value.
DO $dblmax$
DECLARE
  v float8;
  r text;
BEGIN
  FOREACH v IN ARRAY ARRAY[1.797693134862315e308::float8,    -- largest that works
                           1.7976931348623151e308::float8,   -- smallest that overflows
                           1.7976931348623157e308::float8]   -- DBL_MAX
  LOOP
    BEGIN
      PERFORM bm25_debug_query_parse('bdocs_bm', bm25_boost(v, bm25_term('body','tort')));
      r := 'round-trips';
    EXCEPTION WHEN numeric_value_out_of_range THEN
      r := 'numeric_value_out_of_range';
    END;
    RAISE NOTICE 'weight % -> %', v, r;
  END LOOP;
END
$dblmax$;

-- ------------------------------------------------------------ SQL-02 plan shape
-- The builders defaulted to PARALLEL UNSAFE. standard_planner derives
-- glob->parallelModeOK from max_parallel_hazard() over the RAW parse tree, BEFORE
-- eval_const_expressions folds the IMMUTABLE builder call to a Const -- so one
-- builder call anywhere in a query de-parallelised the ENTIRE plan even though the
-- call never executed.
--
-- Asserted as a boolean, not by pasting a plan: EXPLAIN output is environment
-- sensitive. The GUCs the reproduction depends on are pinned, and parallel_workers
-- is set on the table so the decision does not depend on how many pages the tuples
-- happened to occupy.
CREATE TABLE ptest (id int, body text);
INSERT INTO ptest SELECT g, 'row ' || g FROM generate_series(1, 20000) g;
ALTER TABLE ptest SET (parallel_workers = 2);
ANALYZE ptest;

SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;

-- CANARY FIRST. If the baseline is not parallel in this environment then every
-- assertion below is vacuously equal to it and would stay true after a revert.
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM ptest WHERE body <> 'x' $q$, 'Gather') AS baseline_parallel;

-- One PARALLEL UNSAFE builder call with Const arguments -- folded away long before
-- execution -- used to take the Gather with it. All six are exercised.
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM ptest
       WHERE body <> 'x' AND bm25_term('body','tort') IS NOT NULL $q$, 'Gather') AS with_term;
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM ptest
       WHERE body <> 'x' AND bm25_match_terms('body','tort') IS NOT NULL $q$, 'Gather') AS with_match_terms;
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM ptest
       WHERE body <> 'x' AND bm25_phrase('body','a tort') IS NOT NULL $q$, 'Gather') AS with_phrase;
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM ptest
       WHERE body <> 'x' AND bm25_wildcard('body','tor*') IS NOT NULL $q$, 'Gather') AS with_wildcard;
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM ptest
       WHERE body <> 'x' AND bm25_boolean() IS NOT NULL $q$, 'Gather') AS with_boolean;
SELECT pg_temp.plan_has(
  $q$ SELECT count(*) FROM ptest
       WHERE body <> 'x' AND bm25_boost(2.0, bm25_term('body','tort')) IS NOT NULL $q$, 'Gather') AS with_boost;

RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
DROP TABLE ptest;

-- ------------------------------------------------------------ SQL-04 / SQL-05
-- bm25_score, bm25_score_key and bm25_snippet read the IDENTICAL backend-local
-- scored-scan registry and must carry the identical volatility. bm25_snippet was
-- STABLE under a comment claiming it was marked "exactly like bm25_score", which is
-- VOLATILE. STABLE is the unsafe direction: evaluate_function folds a STABLE call
-- whose arguments are all Const when context->estimate is set, and bm25_snippet's
-- four SQL defaults ARE Consts, so bm25_snippet('a literal') is a wholly-Const call
-- that gets evaluated during estimation with no scored scan live.
--
-- That folding path (estimate_expression_value) is not reachable deterministically
-- from SQL, so the gate here is the catalog property rather than an observed fold.
-- It is still a true A/B: pre-fix bm25_snippet reports 's'.
SELECT p.oid::regprocedure AS fn, p.provolatile, p.proparallel, p.procost
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname IN ('bm25_score', 'bm25_score_key', 'bm25_snippet')
 ORDER BY 1;

-- Stated as the invariant rather than as three literals, so it keeps holding if the
-- shared marking is ever revisited: no scan-state reader may differ from the others.
SELECT count(DISTINCT p.provolatile) AS distinct_scan_state_volatilities
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname IN ('bm25_score', 'bm25_score_key', 'bm25_snippet');

-- SQL-04: bm25_snippet re-analyzes the query terms AND the whole field value per
-- row, so procost = 1 by omission mis-priced every path carrying the projection.
-- It now matches bm25_match(text,text), whose derivation it borrows.
SELECT (SELECT procost FROM pg_proc WHERE oid = 'bm25_snippet(text,text,text,int,boolean)'::regprocedure)
     = (SELECT procost FROM pg_proc WHERE oid = 'bm25_match(text,text)'::regprocedure)
       AS snippet_cost_matches_match;

-- SQL-05 functional witness. VOLATILE has a real plan-shape consequence:
-- make_sort_input_target postpones UNCONDITIONALLY for a tlist column containing a
-- volatile function, so the snippet is now projected ABOVE the sort rather than
-- below it, and contain_volatile_functions on a subquery targetlist blocks subquery
-- pull-up. The shape below hits both. The point is that the snippet is still
-- CORRECT there -- the scored-scan registry is keyed on the SCAN, and the scan node
-- is not shut down until the plan ends -- because "the marking is safe" is the half
-- a provolatile assertion cannot reach.
SET enable_seqscan = off;
SELECT array_agg(id ORDER BY id) AS ids_with_a_snippet
  FROM (SELECT id, bm25_snippet(body) AS s
          FROM bdocs WHERE body @@@ 'tort' ORDER BY body &@@ 'tort') t
 WHERE s IS NOT NULL;
RESET enable_seqscan;

-- ------------------------------------------------------------ SQL-09
-- Every set-returning function got prorows = 1000 and procost = 1 by omission,
-- across probes whose real cardinality spans six orders of magnitude. Assert no SRF
-- is left on the default, and show the buckets.
SELECT count(*) AS srfs_at_default_rows
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname LIKE 'bm25%'
   AND p.proretset
   AND p.prorows = 1000;

SELECT p.prorows, count(*) AS srfs
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname LIKE 'bm25%'
   AND p.proretset
 GROUP BY 1 ORDER BY 1;

-- Everything in the extension that carries the bm25_match cost, listed rather than
-- asserted against a name list so a new COST 5000 shows up here as a diff. Seven:
-- the two operator anchors whose derivation this is (bm25_match, bm25_match_jsonb),
-- bm25_snippet (SQL-04, which borrows it), and the four probes that drive a whole
-- ranking build per call (SQL-09).
SELECT p.proname, p.procost
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname LIKE 'bm25%'
   AND p.procost = 5000
 ORDER BY 1;

-- ------------------------------------------------------------ SQL-10
-- Every PUBLIC-executable function must carry a COMMENT. The predicate is
-- deliberately NOT a copy of the list in bm25_native--1.0.sql: it is derived from
-- the GRANT state, which is computed independently of the COMMENT block, so adding
-- a new public function without documenting it fails here. (That is the lesson
-- sql/63_debug_privileges recorded the hard way: a test that restates the code's own
-- predicate can only confirm the predicate is self-consistent.)
SELECT p.oid::regprocedure AS undocumented_public_function
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname LIKE 'bm25%'
   AND has_function_privilege('public', p.oid, 'EXECUTE')
   AND obj_description(p.oid, 'pg_proc') IS NULL
 ORDER BY 1;

SELECT count(*) AS documented_public_functions
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname LIKE 'bm25%'
   AND has_function_privilege('public', p.oid, 'EXECUTE')
   AND obj_description(p.oid, 'pg_proc') IS NOT NULL;

-- The access method, its default opclass and all four operators too.
SELECT obj_description(oid, 'pg_am') IS NOT NULL AS am_documented
  FROM pg_am WHERE amname = 'bm25_native';
SELECT o.oid::regoperator AS op, obj_description(o.oid, 'pg_operator') IS NOT NULL AS documented
  FROM pg_operator o
  JOIN pg_namespace n ON n.oid = o.oprnamespace
 WHERE n.nspname = current_schema()
   AND o.oprname IN ('@@@', '&@@')
 ORDER BY 1;
SELECT obj_description(oc.oid, 'pg_opclass') IS NOT NULL AS opclass_documented
  FROM pg_opclass oc JOIN pg_am a ON a.oid = oc.opcmethod
 WHERE a.amname = 'bm25_native';

-- A sample of the text, so the comments are pinned as CONTENT and not merely as
-- "not null" -- the escape-default divergence from ts_headline is the one a user
-- most needs to find from \df+.
SELECT obj_description('bm25_snippet(text,text,text,int,boolean)'::regprocedure, 'pg_proc')
       LIKE '%DIVERGES from ts_headline%' AS snippet_comment_names_the_divergence;

-- ------------------------------------------------------------ SQL-13
-- Four probes met the file's own stated "pure function of its arguments" test while
-- being left VOLATILE by omission, one had its keywords inverted relative to the
-- other seventeen, and the two selectivity estimators diverged from how core marks
-- eqsel/scalarltsel.
SELECT p.proname, p.provolatile
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname IN ('bm25_debug_field_rle_roundtrip', 'bm25_debug_accum',
                     'bm25_debug_accum_multi', 'bm25_debug_topk',
                     'bm25_debug_glob_match',
                     'bm25_matchsel', 'bm25_matchjoinsel')
 ORDER BY 1;

-- The pure probes still answer correctly under the new marking -- IMMUTABLE on a
-- set-returning function buys no folding (evaluate_function refuses proretset), but
-- assert rather than reason.
SELECT array_agg(field_id ORDER BY idx) AS rle_roundtrip
  FROM bm25_debug_field_rle_roundtrip(ARRAY[0,0,1,1,1,0]);
SELECT term, df, tf FROM bm25_debug_accum(ARRAY['tort tort', 'tort liability'])
 ORDER BY term, tf, df;
SELECT rank, tid FROM bm25_debug_topk(2, ARRAY[1.0, 3.0, 2.0], ARRAY[10, 20, 30])
 ORDER BY rank;
SELECT bm25_debug_glob_match('tor*', 'tort') AS glob_hit;

DROP TABLE bdocs;
DROP EXTENSION bm25_native;
