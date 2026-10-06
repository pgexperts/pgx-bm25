-- H2 (issue #42): bm25_distance / bm25_distance_jsonb were declared
-- IMMUTABLE ... PARALLEL SAFE while reading mutable backend-local state.
--
-- The body is `so = bm25_scored_scan_head(); return so ? so->cur_orderby_dist : +inf`
-- -- a read of the per-backend scored-scan registry, so the same arguments give
-- different answers on different rows. The file contradicted itself: bm25_score(tid)
-- and bm25_score_key() are PARALLEL RESTRICTED over the IDENTICAL state.
--
-- Consequences of the old markings: constant-folding by eval_const_expressions;
-- legality in index expressions, CHECK constraints and partition pruning, none of
-- which have a scored scan; and evaluation in a parallel worker with no scan state,
-- silently yielding +inf as the sort key.
--
-- NOTE on the issue text: #42's title and FINDINGS.md both describe this volatility
-- defect, but the issue BODY was filled with a different finding (ACL checks on
-- regclass entry points). That one is handled with #45, whose own suggested fix is
-- the shared owner-check helper across every mutating entry point.
CREATE EXTENSION bm25_native;

-- Distinct 'alpha' multiplicity per row, so every score -- and so every distance
-- -- is distinct and the ranked sequence below has no ties to break.
CREATE TABLE dv (id int PRIMARY KEY, body text, meta jsonb);
INSERT INTO dv SELECT g, 'beta ' || repeat('alpha ', g), '{}'::jsonb
FROM generate_series(1, 20) g;
CREATE INDEX dv_idx ON dv USING bm25_native (body);

-- ------------------------------------------------------------ catalog
-- Both distance functions must now match their siblings: not IMMUTABLE, not
-- PARALLEL SAFE. provolatile: i=immutable s=stable v=volatile.
-- proparallel:  s=safe u=unsafe r=restricted.
SELECT p.oid::regprocedure AS fn, p.provolatile, p.proparallel
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname IN ('bm25_distance', 'bm25_distance_jsonb', 'bm25_score', 'bm25_score_key')
 ORDER BY 1;

-- Nothing that reads scan state may be PARALLEL SAFE. bm25_snippet is in the list
-- even though this suite is about the distance functions: ADR 0028 cites this query
-- as the pin for "no scan-state reader is PARALLEL SAFE", and bm25_snippet reads the
-- identical backend-local registry, so leaving it out made that citation false. Its
-- VOLATILE/PARALLEL RESTRICTED marking itself is pinned in sql/103 (ADR 0082).
SELECT count(*) AS parallel_safe_scan_state_fns
  FROM pg_proc p
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE n.nspname = current_schema()
   AND p.proname IN ('bm25_distance', 'bm25_distance_jsonb', 'bm25_score',
                     'bm25_score_key', 'bm25_snippet')
   AND p.proparallel = 's';

-- Error renderer (the house form from sql/110). The two CREATE INDEX statements below
-- are expected to fail, and a raw error is not portable: PostgreSQL 19 adds a parser
-- error position (LINE n / caret) that 18 does not print, and VERBOSITY terse would
-- still append " at character N". SQLSTATE plus message is the same on every major.
CREATE FUNCTION pg_temp.err_of(q text) RETURNS text AS $$
BEGIN
    EXECUTE q;
    RETURN 'NO ERROR RAISED';
EXCEPTION WHEN OTHERS THEN
    RETURN SQLSTATE || ' ' || SQLERRM;
END; $$ LANGUAGE plpgsql;

-- ------------------------------------------------------------ index expression
-- The sharpest consequence of IMMUTABLE. Pre-fix this SUCCEEDED, producing a btree
-- over `body &@@ 'alpha'` whose every entry is +inf -- there is no scored scan
-- during an index build -- permanently baked into an index that would then answer
-- queries from those values. PostgreSQL now refuses it, which is the whole point of
-- the marking.
SELECT pg_temp.err_of($q$CREATE INDEX dv_bad_expr ON dv ((body &@@ 'alpha'))$q$);
SELECT pg_temp.err_of($q$CREATE INDEX dv_bad_expr_jsonb ON dv ((body &@@ '{"match":{"field":"body","query":"alpha"}}'::jsonb))$q$);

-- ------------------------------------------------------------ still ranks
-- STABLE, not VOLATILE: VOLATILE is what the behavior literally is, but measured on
-- PG 18.3 it makes the planner refuse the index ordering path and insert a Sort,
-- which evaluates the function outside any scored scan and returns rows in the
-- WRONG order. STABLE is the strictest marking that keeps Order By on the index.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM dv WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 3;

-- The ranking itself is unchanged: more 'alpha' occurrences rank first, and the
-- distance is negative score so it ascends.
SELECT id, round((body &@@ 'alpha')::numeric, 4) AS dist
FROM dv WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 5;

-- The materialized resjunk projection resolves PER ROW rather than folding to a
-- constant: two different ranks report two different distances. Under the old
-- IMMUTABLE marking this expression was a constant-folding candidate.
SELECT round((body &@@ 'alpha')::numeric, 4) AS dist_at_rank_1
FROM dv WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 1;
SELECT round((body &@@ 'alpha')::numeric, 4) AS dist_at_rank_15
FROM dv WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 1 OFFSET 14;
RESET enable_seqscan;

DROP TABLE dv;
DROP EXTENSION bm25_native;
