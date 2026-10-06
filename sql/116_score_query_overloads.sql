-- 116_score_query_overloads -- the query-qualified score accessors (#253):
-- bm25_score(tid, query [, regclass]) and bm25_score_key(key, query).
--
-- The one-argument accessors receive only row identity, and the #242 call-site
-- binding (ADR 0103) cannot learn the owning scan in three residual classes: a
-- probe matching several scans' current rows (a key join of two ranked
-- subqueries), a projection decoupled from its scan (PL/pgSQL FOR prefetch, Sort or
-- Materialize above the scan, a cursor), and a ctid repeated across the children of
-- a Merge Append. sql/53 pins the one-argument values on some of those shapes. The
-- overloads name the scan: the query it ranks (the ORDER BY ... &@@ RHS, matched
-- byte for byte as the &@@ distance projection does, ADR 0061) and, optionally, the
-- heap. Each candidate's whole ranking is probed, not its current row, and two
-- candidates disagreeing give NULL.
--
-- Oracle: every score is compared with a SINGLE-scan baseline for that row, taken
-- in its own statement (a baseline computed as a sibling scan in the same statement
-- would measure the collision instead of the answer -- see sql/53). Each shape also
-- reports whether the one-argument accessor misattributes on it, so the test
-- records why the overload is needed there; those columns are the unchanged
-- residuals, not assertions about the overloads. Output is booleans and counts; a
-- plan guard precedes each shape so no assertion can pass on a plan that never
-- builds the shape.
CREATE EXTENSION bm25_native;

CREATE FUNCTION sqo_plan(q text) RETURNS SETOF text LANGUAGE plpgsql AS
$$ BEGIN RETURN QUERY EXECUTE 'EXPLAIN (COSTS OFF) ' || q; END $$;

-- Every doc matches both 'foo' and 'bar' with different term frequencies, so a
-- misattribution is a wrong number, not a NULL. Same fixture as sql/53.
CREATE TABLE sq(id int PRIMARY KEY, body text, note text);
INSERT INTO sq(id, body) VALUES
 (1, 'foo foo foo bar alpha'),
 (2, 'foo bar bar bar beta gamma'),
 (3, 'foo foo bar bar delta epsilon zeta'),
 (4, 'foo bar eta theta'),
 (5, 'foo foo foo foo bar iota'),
 (6, 'foo bar bar kappa lambda mu nu xi');
CREATE INDEX sq_bm25 ON sq USING bm25_native (body) INCLUDE (id)
  WITH (key_field='id', language='english');
SELECT bm25_seal('sq_bm25') IS NOT NULL AS sealed;
ANALYZE sq;
SET enable_seqscan = off;
SET enable_sort = off;

-- Baselines, one single-scan statement each, through the one-argument accessor
-- (correct with one live scan).
CREATE TEMP TABLE sq_foo AS
  SELECT id, ctid AS tid, round(bm25_score_key(id)::numeric,6) AS s
    FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
CREATE TEMP TABLE sq_bar AS
  SELECT id, ctid AS tid, round(bm25_score_key(id)::numeric,6) AS s
    FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar';
SELECT count(*) AS foo_rows, count(DISTINCT s) AS foo_distinct,
       (SELECT count(*) FROM sq_bar) AS bar_rows,
       (SELECT count(*) FROM sq_foo f JOIN sq_bar b USING (id) WHERE f.s <> b.s) AS foo_bar_differ
  FROM sq_foo;

-- ===========================================================================
-- Part 1: single scan -- every overload equals the one-argument accessor, and
-- the NULL cases.
-- ===========================================================================
CREATE TEMP TABLE sq_single AS
  SELECT id,
         round(bm25_score(ctid)::numeric,6)                      AS one_arg,
         round(bm25_score(ctid, 'foo')::numeric,6)               AS tid_text,
         round(bm25_score(ctid, 'foo', tableoid)::numeric,6)     AS tid_text_rel,
         round(bm25_score_key(id, 'foo')::numeric,6)             AS key_text,
         bm25_score(ctid, 'bar')                                 AS other_query,
         bm25_score(ctid, 'Foo')                                 AS other_bytes,
         bm25_score(ctid, 'foo', 'sq_foo'::regclass)             AS other_heap,
         bm25_score(ctid, bm25_term('body', 'foo'))              AS other_family,
         bm25_score_key(id::bigint, 'foo')                       AS other_key_type,
         bm25_score('(0,0)'::tid, 'foo')                         AS invalid_tid,
         bm25_score('(9999,1)'::tid, 'foo')                      AS absent_tid
    FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
SELECT bool_and(tid_text = one_arg AND tid_text_rel = one_arg AND key_text = one_arg)
         AS overloads_equal_one_arg,
       bool_and(one_arg = (SELECT s FROM sq_foo f WHERE f.id = x.id)) AS equal_baseline,
       count(other_query) AS other_query_nonnull,
       count(other_bytes) AS other_bytes_nonnull,
       count(other_heap) AS other_heap_nonnull,
       count(other_family) AS other_family_nonnull,
       count(other_key_type) AS other_key_type_nonnull,
       count(invalid_tid) AS invalid_tid_nonnull,
       count(absent_tid) AS absent_tid_nonnull
  FROM sq_single x;

-- jsonb query trees: the jsonb overloads resolve a jsonb-ranked scan, and the text
-- overload does not cross-match it (the two &@@ families never share an identity).
CREATE TEMP TABLE sq_json AS
  SELECT id,
         round(bm25_score(ctid)::numeric,6)                                          AS one_arg,
         round(bm25_score(ctid, bm25_term('body', 'foo'))::numeric,6)                AS tid_jsonb,
         round(bm25_score(ctid, bm25_term('body', 'foo'), tableoid)::numeric,6)      AS tid_jsonb_rel,
         round(bm25_score_key(id, bm25_term('body', 'foo'))::numeric,6)              AS key_jsonb,
         bm25_score(ctid, 'foo')                                                     AS text_query
    FROM sq WHERE body @@@ bm25_term('body', 'foo') ORDER BY body &@@ bm25_term('body', 'foo');
SELECT count(*) AS json_rows,
       bool_and(tid_jsonb = one_arg AND tid_jsonb_rel = one_arg AND key_jsonb = one_arg)
         AS jsonb_overloads_equal_one_arg,
       count(text_query) AS text_query_nonnull
  FROM sq_json;

-- ===========================================================================
-- Part 2: key-join collision -- two ranked subqueries joined on the key, the
-- accessors in the top-level projection. Every projected row is the current row
-- of both scans, so no call site ever binds (ADR 0103).
-- ===========================================================================
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT bool_or(l ~ 'Nested Loop') AS nestloop,
       count(*) FILTER (WHERE l ~ 'Index Scan using sq_bm25') AS bm25_scans
  FROM sqo_plan($q$ SELECT 1 FROM
      (SELECT id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) a
      JOIN (SELECT id, ctid FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' OFFSET 0) b
        ON a.id = b.id $q$) l;
CREATE TEMP TABLE sq_kj AS
  SELECT 'nestloop' AS plan, a.id,
         round(bm25_score_key(a.id, 'foo')::numeric,6) AS foo_key,
         round(bm25_score(a.ctid, 'foo')::numeric,6)   AS foo_tid,
         round(bm25_score_key(b.id, 'bar')::numeric,6) AS bar_key,
         round(bm25_score(b.ctid, 'bar')::numeric,6)   AS bar_tid,
         round(bm25_score_key(a.id)::numeric,6)        AS foo_old,
         round(bm25_score_key(b.id)::numeric,6)        AS bar_old
    FROM (SELECT id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) a
    JOIN (SELECT id, ctid FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' OFFSET 0) b
      ON a.id = b.id;
SET enable_nestloop = off;
SET enable_hashjoin = on;
SELECT bool_or(l ~ 'Hash Join') AS hashjoin,
       count(*) FILTER (WHERE l ~ 'Index Scan using sq_bm25') AS bm25_scans
  FROM sqo_plan($q$ SELECT 1 FROM
      (SELECT id, ctid FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' OFFSET 0) a
      JOIN (SELECT id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) b
        ON a.id = b.id $q$) l;
INSERT INTO sq_kj
  SELECT 'hashjoin', b.id,
         round(bm25_score_key(b.id, 'foo')::numeric,6),
         round(bm25_score(b.ctid, 'foo')::numeric,6),
         round(bm25_score_key(a.id, 'bar')::numeric,6),
         round(bm25_score(a.ctid, 'bar')::numeric,6),
         round(bm25_score_key(b.id)::numeric,6),
         round(bm25_score_key(a.id)::numeric,6)
    FROM (SELECT id, ctid FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' OFFSET 0) a
    JOIN (SELECT id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) b
      ON a.id = b.id;
RESET enable_nestloop;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT k.plan, count(*) AS rows_,
       bool_and(coalesce(k.foo_key = f.s AND k.foo_tid = f.s
                     AND k.bar_key = b.s AND k.bar_tid = b.s, false)) AS overloads_correct,
       bool_or(k.foo_old IS DISTINCT FROM f.s OR k.bar_old IS DISTINCT FROM b.s)
         AS one_arg_misattributes
  FROM sq_kj k JOIN sq_foo f USING (id) JOIN sq_bar b USING (id)
 GROUP BY k.plan ORDER BY k.plan;

-- The same ranked query on both sides of the join: two candidates hold every row,
-- with the same score, so that score is the answer (not NULL).
SET enable_hashjoin = off;
SET enable_mergejoin = off;
CREATE TEMP TABLE sq_self AS
  SELECT a.id, round(bm25_score(a.ctid, 'foo')::numeric,6) AS a_tid,
         round(bm25_score_key(b.id, 'foo')::numeric,6)     AS b_key
    FROM (SELECT id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) a
    JOIN (SELECT id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) b
      ON a.id = b.id;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT count(*) AS rows_,
       bool_and(coalesce(x.a_tid = f.s AND x.b_key = f.s, false)) AS same_score_candidates_resolve
  FROM sq_self x JOIN sq_foo f USING (id);

-- ===========================================================================
-- Part 3: decoupled projections.
-- ===========================================================================

-- 3a. Sort above an Append of two ranked branches: the accessor is volatile, so
-- make_sort_input_target projects it ABOVE the Sort, after both branches have
-- finished. One call site serves both branches; the query is a per-row column.
SET enable_sort = on;
SELECT bool_or(l ~ '^\s*Sort') AS sort_node, bool_or(l ~ 'Append') AS append_node,
       count(*) FILTER (WHERE l ~ 'Index Scan using sq_bm25') AS bm25_scans
  FROM sqo_plan($q$ SELECT u.id, bm25_score(u.ctid, u.q) FROM
      ((SELECT 'foo' AS q, id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo')
       UNION ALL
       (SELECT 'bar', id, ctid FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar')) u
      ORDER BY u.q, u.id $q$) l;
CREATE TEMP TABLE sq_sort AS
  SELECT u.q, u.id,
         round(bm25_score(u.ctid, u.q)::numeric,6)     AS tid_new,
         round(bm25_score_key(u.id, u.q)::numeric,6)   AS key_new,
         round(bm25_score(u.ctid)::numeric,6)          AS tid_old
    FROM ((SELECT 'foo' AS q, id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo')
          UNION ALL
          (SELECT 'bar', id, ctid FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar')) u
   ORDER BY u.q, u.id;
SELECT count(*) AS rows_,
       bool_and(coalesce(tid_new = want AND key_new = want, false)) AS overloads_correct,
       bool_or(tid_old IS DISTINCT FROM want) AS one_arg_misattributes
  FROM (SELECT x.*, CASE x.q WHEN 'foo' THEN f.s ELSE b.s END AS want
          FROM sq_sort x JOIN sq_foo f USING (id) JOIN sq_bar b USING (id)) z;
RESET enable_sort;

-- 3b. Materialize: a nested loop whose inner side is a Materialize over the 'bar'
-- scan, so every inner row after the first pass is replayed from the tuplestore
-- while the 'foo' scan streams. The planner only materializes when it expects
-- enough outer rows, hence a larger table than sq.
CREATE TABLE sm(id int PRIMARY KEY, body text);
INSERT INTO sm SELECT g, repeat('foo ', g % 4 + 1) || repeat('bar ', g % 3 + 1) || 'w' || g
  FROM generate_series(1, 400) g;
CREATE INDEX sm_i ON sm USING bm25_native (body) INCLUDE (id)
  WITH (key_field='id', language='english');
SELECT bm25_seal('sm_i') IS NOT NULL AS sealed;
ANALYZE sm;
CREATE TEMP TABLE sm_foo AS SELECT id, round(bm25_score_key(id)::numeric,6) AS s
  FROM sm WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
CREATE TEMP TABLE sm_bar AS SELECT id, round(bm25_score_key(id)::numeric,6) AS s
  FROM sm WHERE body @@@ 'bar' ORDER BY body &@@ 'bar';
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT bool_or(l ~ 'Materialize') AS materialize_node,
       count(*) FILTER (WHERE l ~ 'Index Scan using sm_i') AS bm25_scans
  FROM sqo_plan($q$ SELECT 1 FROM
      (SELECT id, ctid FROM sm WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) a
      JOIN (SELECT id, ctid FROM sm WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' OFFSET 0) b
        ON a.id = b.id + 1 $q$) l;
CREATE TEMP TABLE sq_mat AS
  SELECT a.id AS aid, b.id AS bid,
         round(bm25_score(a.ctid, 'foo')::numeric,6)   AS foo_new,
         round(bm25_score_key(b.id, 'bar')::numeric,6) AS bar_new,
         round(bm25_score(b.ctid, 'bar')::numeric,6)   AS bar_tid_new,
         round(bm25_score(a.ctid)::numeric,6)          AS foo_old,
         round(bm25_score(b.ctid)::numeric,6)          AS bar_old
    FROM (SELECT id, ctid FROM sm WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) a
    JOIN (SELECT id, ctid FROM sm WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' OFFSET 0) b
      ON a.id = b.id + 1;
RESET enable_hashjoin;
RESET enable_mergejoin;
SELECT count(*) AS pairs,
       bool_and(coalesce(m.foo_new = f.s AND m.bar_new = b.s AND m.bar_tid_new = b.s, false))
         AS overloads_correct,
       bool_or(m.foo_old IS DISTINCT FROM f.s OR m.bar_old IS DISTINCT FROM b.s)
         AS one_arg_misattributes
  FROM sq_mat m JOIN sm_foo f ON f.id = m.aid JOIN sm_bar b ON b.id = m.bid;

-- 3c. PL/pgSQL FOR prefetch (batches of 10, then 50), the sql/53 residual shape.
-- Eleven rows ranked by 'foo', ids 11 down to 1. Inside the loop body an inner FOR
-- ranks 'bar' over the outer's row and the row before it, and every row the inner
-- loop visits is scored by both accessors while both loops are live. At n = 11 the
-- one-argument accessor returns 'foo''s 0.042681 for a 'bar' row whose score is
-- 0.084465 (sql/53).
CREATE TABLE sq_plx(id int PRIMARY KEY, body text);
INSERT INTO sq_plx SELECT g, repeat('foo ', g) || repeat('bar ', 12 - g) || repeat('pad ', g % 3)
  FROM generate_series(1, 11) g;
CREATE INDEX sq_plx_bm25 ON sq_plx USING bm25_native (body) INCLUDE (id)
  WITH (key_field='id', language='english');
SELECT bm25_seal('sq_plx_bm25') IS NOT NULL AS sealed;
ANALYZE sq_plx;
CREATE TEMP TABLE plx_foo AS SELECT id, round(bm25_score_key(id)::numeric,6) AS s
  FROM sq_plx WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
CREATE TEMP TABLE plx_bar AS SELECT id, round(bm25_score_key(id)::numeric,6) AS s
  FROM sq_plx WHERE body @@@ 'bar' ORDER BY body &@@ 'bar';
CREATE FUNCTION sq_plx_probe()
RETURNS TABLE(n int, which text, rid int, new_key numeric, new_tid numeric, old_key numeric)
LANGUAGE plpgsql AS $$
DECLARE o record; i record; ids int[];
BEGIN
  n := 0;
  FOR o IN SELECT id, ctid FROM sq_plx WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' LOOP
    n := n + 1;
    -- sql/53's constructed ids at n = 10 and 11; the outer row and its successor
    -- elsewhere.
    ids := CASE n WHEN 10 THEN ARRAY[2, 11] WHEN 11 THEN ARRAY[1]
                  ELSE ARRAY[o.id, o.id % 11 + 1] END;
    FOR i IN SELECT id, ctid FROM sq_plx WHERE body @@@ 'bar' AND id = ANY (ids)
             ORDER BY body &@@ 'bar' LIMIT cardinality(ids) LOOP
      which := 'inner'; rid := i.id;
      new_key := round(bm25_score_key(i.id, 'bar')::numeric, 6);
      new_tid := round(bm25_score(i.ctid, 'bar')::numeric, 6);
      old_key := round(bm25_score_key(i.id)::numeric, 6);
      RETURN NEXT;
      which := 'outer'; rid := o.id;
      new_key := round(bm25_score_key(o.id, 'foo')::numeric, 6);
      new_tid := round(bm25_score(o.ctid, 'foo')::numeric, 6);
      old_key := round(bm25_score_key(o.id)::numeric, 6);
      RETURN NEXT;
    END LOOP;
  END LOOP;
END $$;
CREATE TEMP TABLE sq_plx_out AS SELECT * FROM sq_plx_probe();
SELECT count(*) AS rows_, count(DISTINCT n) AS outer_rows,
       bool_and(coalesce(p.new_key = w.s AND p.new_tid = w.s, false)) AS overloads_correct,
       bool_or(p.old_key IS DISTINCT FROM w.s) AS one_arg_misattributes
  FROM sq_plx_out p
  JOIN (SELECT 'inner' AS which, id, s FROM plx_bar
        UNION ALL SELECT 'outer', id, s FROM plx_foo) w
    ON w.which = p.which AND w.id = p.rid;
-- The exact sql/53 probe, with the overload beside the one-argument accessor: the
-- inner loop scores only its first row and exits. At n = 11 the one-argument
-- accessor returns 'foo''s 0.042681 for the 'bar' row whose score is 0.084465.
CREATE FUNCTION sq_plx_pinned() RETURNS TABLE(n int, iid int, got_old numeric, got_new numeric)
LANGUAGE plpgsql AS $$
DECLARE o record; i record; ids int[];
BEGIN
  n := 0;
  FOR o IN SELECT id FROM sq_plx WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' LOOP
    n := n + 1;
    ids := CASE n WHEN 10 THEN ARRAY[2, 11] WHEN 11 THEN ARRAY[1] END;
    CONTINUE WHEN ids IS NULL;
    FOR i IN SELECT id FROM sq_plx WHERE body @@@ 'bar' AND id = ANY (ids)
             ORDER BY body &@@ 'bar' LIMIT cardinality(ids) LOOP
      iid := i.id;
      got_old := round(bm25_score_key(i.id)::numeric, 6);
      got_new := round(bm25_score_key(i.id, 'bar')::numeric, 6);
      RETURN NEXT;
      EXIT;
    END LOOP;
  END LOOP;
END $$;
SELECT p.n, p.iid, p.got_old, p.got_new, b.s AS bar_want
  FROM sq_plx_pinned() p JOIN plx_bar b ON b.id = p.iid ORDER BY p.n;

-- 3d. Cursors. Two cursors ranking different queries are open and advanced; a
-- separate statement then scores rows by ctid and key while both scans are live
-- and positioned. The projection is fully decoupled from both scans.
BEGIN;
DECLARE c_foo CURSOR FOR SELECT id FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
DECLARE c_bar CURSOR FOR SELECT id FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar';
MOVE 2 IN c_foo;
MOVE 3 IN c_bar;
CREATE TEMP TABLE sq_cur AS
  SELECT f.id,
         round(bm25_score(f.tid, 'foo')::numeric,6)     AS foo_new,
         round(bm25_score_key(f.id, 'bar')::numeric,6)  AS bar_new,
         round(bm25_score_key(f.id)::numeric,6)         AS one_arg,
         f.s AS foo_want, b.s AS bar_want
    FROM sq_foo f JOIN sq_bar b USING (id);
CLOSE c_foo;
CLOSE c_bar;
COMMIT;
SELECT count(*) AS rows_,
       bool_and(coalesce(foo_new = foo_want AND bar_new = bar_want, false)) AS overloads_correct,
       bool_or(one_arg IS DISTINCT FROM foo_want) AS one_arg_not_foo,
       bool_or(one_arg IS DISTINCT FROM bar_want) AS one_arg_not_bar
  FROM sq_cur;

-- 3e. The #242 nested shape (correlated inner re-registered on every outer row),
-- which the one-argument accessor already gets right: the overloads must too.
CREATE TEMP TABLE sq_nested AS
  SELECT o.id, round(bm25_score_key(o.id, 'foo')::numeric,6) AS key_new,
         round(bm25_score(o.ctid, 'foo')::numeric,6) AS tid_new
    FROM sq o
   WHERE o.body @@@ 'foo'
     AND (SELECT i.id FROM sq i WHERE i.body @@@ 'bar' AND i.body <> (o.body||'x')
           ORDER BY i.body &@@ 'bar' LIMIT 1) >= 0
   ORDER BY o.body &@@ 'foo';
SELECT count(*) AS rows_,
       bool_and(coalesce(n.key_new = f.s AND n.tid_new = f.s, false)) AS overloads_correct
  FROM sq_nested n JOIN sq_foo f USING (id);

-- ===========================================================================
-- Part 4: ctid collisions across heaps under a Merge Append.
-- ===========================================================================
-- Inheritance: two children with the same row count, so every ctid exists in both,
-- and different bodies, so the same ctid scores differently in each.
CREATE TABLE ih (id int, body text);
CREATE TABLE ih1 () INHERITS (ih);
CREATE TABLE ih2 () INHERITS (ih);
INSERT INTO ih1 SELECT g, repeat('foo ', (g % 5) + 1) || 'x' || g || ' ' || repeat('pad ', g % 3)
  FROM generate_series(1, 12) g;
INSERT INTO ih2 SELECT g + 100, repeat('foo ', (g % 4) + 1) || 'y' || g || ' ' || repeat('pad ', g % 2)
  FROM generate_series(1, 12) g;
CREATE INDEX ih1_i ON ih1 USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id', language = 'english');
CREATE INDEX ih2_i ON ih2 USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id', language = 'english');
SELECT bm25_seal('ih1_i') IS NOT NULL AS sealed_1, bm25_seal('ih2_i') IS NOT NULL AS sealed_2;
ANALYZE ih, ih1, ih2;
-- Baselines: each child ranked alone.
CREATE TEMP TABLE ih_base AS
  SELECT tableoid AS rel, ctid AS tid, id, round(bm25_score(ctid)::numeric,6) AS s
    FROM ih1 WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
INSERT INTO ih_base
  SELECT tableoid, ctid, id, round(bm25_score(ctid)::numeric,6)
    FROM ih2 WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
SELECT count(*) AS base_rows,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM ih_base o WHERE o.tid = b.tid
                                      AND o.rel <> b.rel AND o.s <> b.s)) AS colliding_rows
  FROM ih_base b;
SELECT bool_or(l ~ 'Merge Append') AS merge_append,
       count(*) FILTER (WHERE l ~ 'Index Scan using ih[12]_i') AS child_index_scans
  FROM sqo_plan($q$ SELECT id FROM ih WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' $q$) l;
CREATE TEMP TABLE ih_out AS
  SELECT tableoid AS rel, ctid AS tid, id,
         round(bm25_score(ctid, 'foo', tableoid)::numeric,6) AS rel_new,
         round(bm25_score(ctid, 'foo')::numeric,6)           AS norel_new,
         round(bm25_score_key(id, 'foo')::numeric,6)         AS key_new,
         round(bm25_score(ctid)::numeric,6)                  AS one_arg
    FROM ih WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
-- With tableoid every row is right. Without it a colliding ctid is ambiguous and
-- NULL, never another child's number; the keys here do not collide, so the key
-- form is right throughout.
SELECT count(*) AS rows_,
       bool_and(coalesce(o.rel_new = b.s, false))           AS tableoid_form_correct,
       count(*) FILTER (WHERE o.norel_new = b.s)            AS no_tableoid_correct,
       count(*) FILTER (WHERE o.norel_new IS NULL)          AS no_tableoid_null,
       count(*) FILTER (WHERE o.norel_new <> b.s)           AS no_tableoid_wrong,
       bool_and(coalesce(o.key_new = b.s, false))           AS key_form_correct,
       bool_or(o.one_arg IS DISTINCT FROM b.s)              AS one_arg_misattributes
  FROM ih_out o JOIN ih_base b ON b.rel = o.rel AND b.tid = o.tid;

-- Declarative partitioning, the same collision through leaf partitions.
CREATE TABLE pp (id int, body text) PARTITION BY RANGE (id);
CREATE TABLE pp1 PARTITION OF pp FOR VALUES FROM (1) TO (13);
CREATE TABLE pp2 PARTITION OF pp FOR VALUES FROM (13) TO (25);
INSERT INTO pp SELECT g, repeat('foo ', (g % 5) + 1) || 'x' || g || ' ' || repeat('pad ', g % 3)
  FROM generate_series(1, 24) g;
CREATE INDEX pp_i ON pp USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'english');
SELECT bm25_seal('pp1_body_id_idx') IS NOT NULL AS sealed_1,
       bm25_seal('pp2_body_id_idx') IS NOT NULL AS sealed_2;
ANALYZE pp;
CREATE TEMP TABLE pp_base AS
  SELECT tableoid AS rel, ctid AS tid, round(bm25_score(ctid)::numeric,6) AS s
    FROM pp1 WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
INSERT INTO pp_base
  SELECT tableoid, ctid, round(bm25_score(ctid)::numeric,6)
    FROM pp2 WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
SELECT bool_or(l ~ 'Merge Append') AS merge_append,
       count(*) FILTER (WHERE l ~ 'Index Scan using pp[12]_body_id_idx') AS child_index_scans
  FROM sqo_plan($q$ SELECT id FROM pp WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' $q$) l;
CREATE TEMP TABLE pp_out AS
  SELECT tableoid AS rel, ctid AS tid,
         round(bm25_score(ctid, 'foo', tableoid)::numeric,6) AS rel_new,
         round(bm25_score(ctid, 'foo')::numeric,6)           AS norel_new
    FROM pp WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
SELECT count(*) AS rows_,
       bool_and(coalesce(o.rel_new = b.s, false))  AS tableoid_form_correct,
       count(*) FILTER (WHERE o.norel_new <> b.s)  AS no_tableoid_wrong
  FROM pp_out o JOIN pp_base b ON b.rel = o.rel AND b.tid = o.tid;

-- ===========================================================================
-- Part 5: scan modes -- the answer comes from each candidate's ranking, so it must
-- hold for every mode that builds one.
-- ===========================================================================

-- 5a. WAND-capped ranking pulled past k (the tail rebuild replaces the ranking and
-- drops the hash), projected above a Sort so the projection runs after the tail.
-- A single-term text query always takes WAND when wand_top_k > 0, and 60 matches
-- fill k = 5, so the first build is capped and the sixth pull rebuilds; the 'pad'
-- branch stops at LIMIT 3, inside its capped ranking.
CREATE TABLE sw(id int PRIMARY KEY, body text);
INSERT INTO sw SELECT g, repeat('foo ', (g % 7) + 1) || repeat('pad ', g % 11) || 'w' || g
  FROM generate_series(1, 60) g;
CREATE INDEX sw_i ON sw USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id', language = 'english');
SELECT bm25_seal('sw_i') IS NOT NULL AS sealed;
ANALYZE sw;
CREATE TEMP TABLE sw_base AS
  SELECT id, round(bm25_score_key(id)::numeric,6) AS s
    FROM sw WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
CREATE TEMP TABLE sw_pad AS
  SELECT id, round(bm25_score_key(id)::numeric,6) AS s
    FROM sw WHERE body @@@ 'pad' ORDER BY body &@@ 'pad';
SET bm25_native.wand_top_k = 5;
-- Streaming, on the scan's own rows: the first five are answered from a hash built
-- over the capped ranking, and from the sixth on the rebuilt ranking must have
-- dropped that hash, or every later row would miss and read NULL.
CREATE TEMP TABLE sw_stream AS
  SELECT id, round(bm25_score_key(id, 'foo')::numeric,6) AS key_new,
         round(bm25_score(ctid, 'foo')::numeric,6) AS tid_new
    FROM sw WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
SELECT count(*) AS streamed_rows,
       bool_and(coalesce(o.key_new = b.s AND o.tid_new = b.s, false)) AS streamed_correct
  FROM sw_stream o JOIN sw_base b USING (id);
SET enable_sort = on;
CREATE TEMP TABLE sw_out AS
  SELECT u.q, u.id, round(bm25_score_key(u.id, u.q)::numeric,6) AS key_new,
         round(bm25_score(u.ctid, u.q)::numeric,6) AS tid_new
    FROM ((SELECT 'foo' AS q, id, ctid FROM sw WHERE body @@@ 'foo' ORDER BY body &@@ 'foo')
          UNION ALL
          (SELECT 'pad', id, ctid FROM sw WHERE body @@@ 'pad' ORDER BY body &@@ 'pad' LIMIT 3)) u
   ORDER BY u.q, u.id;
RESET enable_sort;
RESET bm25_native.wand_top_k;
SELECT count(*) FILTER (WHERE q = 'foo') AS foo_rows,
       bool_and(coalesce(o.key_new = b.s AND o.tid_new = b.s, false)) FILTER (WHERE q = 'foo')
         AS past_k_correct,
       count(*) FILTER (WHERE q = 'pad' AND o.key_new = p.s AND o.tid_new = p.s)
         AS pad_rows_correct
  FROM sw_out o LEFT JOIN sw_base b USING (id) LEFT JOIN sw_pad p USING (id);

-- Exactly k matches: the capped build already holds every match, so the pull after
-- row k rebuilds the same k rows and ends the scan without emitting. The rebuild
-- used to leave the scan unpositioned, and every query-qualified accessor projected
-- above the Sort read NULL.
CREATE TABLE sx(id int PRIMARY KEY, body text);
INSERT INTO sx SELECT g, repeat('foo ', g) || 'pad' FROM generate_series(1, 5) g;
INSERT INTO sx SELECT g, 'bar pad' FROM generate_series(6, 20) g;
CREATE INDEX sx_i ON sx USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id', language = 'english');
SELECT bm25_seal('sx_i') IS NOT NULL AS sealed;
ANALYZE sx;
CREATE TEMP TABLE sx_base AS
  SELECT id, round(bm25_score_key(id)::numeric,6) AS s
    FROM sx WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
SET bm25_native.wand_top_k = 5;
SET enable_sort = on;
CREATE TEMP TABLE sx_out AS
  SELECT u.q, u.id, round(bm25_score_key(u.id, u.q)::numeric,6) AS key_new,
         round(bm25_score(u.ctid, u.q)::numeric,6) AS tid_new
    FROM ((SELECT 'foo' AS q, id, ctid FROM sx WHERE body @@@ 'foo' ORDER BY body &@@ 'foo')
          UNION ALL
          (SELECT 'bar', id, ctid FROM sx WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' LIMIT 2)) u
   ORDER BY u.q, u.id;
RESET enable_sort;
RESET bm25_native.wand_top_k;
SELECT count(*) AS foo_rows,
       bool_and(coalesce(o.key_new = b.s AND o.tid_new = b.s, false)) AS exactly_k_correct
  FROM sx_out o JOIN sx_base b USING (id) WHERE o.q = 'foo';

-- 5c. Duplicate keys inside one ranking. key_field need not be unique, and text keys
-- are stored in their first 16 bytes, so two ranked rows can share a key. With
-- different scores the key cannot say which row is meant, because the key form
-- probes the whole ranking: it is NULL, and the ctid form is right. Projected
-- streaming on the scan's own rows, as here, the one-argument key accessor is right
-- too -- its current-row fast path compares the one row just emitted -- but its hash
-- fallback reports the higher score for both rows. For a non-unique key_field, use
-- the ctid form.
CREATE TABLE sdk(id int, tk text, body text);
INSERT INTO sdk VALUES
  (1, 'abcdefghijklmnopqrstuvwxyz1', 'foo foo foo x'),
  (1, 'abcdefghijklmnopqrstuvwxyz2', 'foo y z w v u'),
  (2, 'short', 'foo foo q'),
  (3, 'same1', 'foo r s'),
  (3, 'same2', 'foo t u'),
  (4, 'lowest', 'foo a b c d e f g h i j');
-- One index at a time, so each query below plans on the index keyed as intended.
CREATE INDEX sdk_i ON sdk USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id', language = 'english');
SELECT bm25_seal('sdk_i') IS NOT NULL AS sealed_i;
ANALYZE sdk;
CREATE TEMP TABLE sdk_int AS
  SELECT ctid AS tid, id, round(bm25_score(ctid)::numeric,6) AS s,
         round(bm25_score(ctid, 'foo')::numeric,6)     AS tid_new,
         round(bm25_score_key(id, 'foo')::numeric,6)   AS key_new,
         round(bm25_score_key(id)::numeric,6)          AS key_old
    FROM sdk WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
DROP INDEX sdk_i;
CREATE INDEX sdk_t ON sdk USING bm25_native (body) INCLUDE (tk) WITH (key_field = 'tk', language = 'english');
SELECT bm25_seal('sdk_t') IS NOT NULL AS sealed_t;
CREATE TEMP TABLE sdk_text AS
  SELECT ctid AS tid, tk, round(bm25_score(ctid)::numeric,6) AS s,
         round(bm25_score_key(tk, 'foo')::numeric,6)   AS key_new
    FROM sdk WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
SELECT 'int' AS key, count(*) AS rows_,
       bool_and(tid_new = s) AS tid_form_correct,
       count(*) FILTER (WHERE id = 1 AND key_new IS NULL) AS dup_key_null,
       bool_and(key_new = s) FILTER (WHERE id IN (2, 3)) AS unique_or_equal_key_correct,
       bool_and(key_old = s) AS one_arg_streaming_correct
  FROM sdk_int
UNION ALL
SELECT 'text', count(*), NULL,
       count(*) FILTER (WHERE tk LIKE 'abc%' AND key_new IS NULL),
       bool_and(key_new = s) FILTER (WHERE tk NOT LIKE 'abc%'), NULL
  FROM sdk_text;
-- The one-argument key accessor's hash fallback: projected above a Sort, after the
-- only live scan has finished on its lowest-ranked row (id 4), so key 1 is not its
-- current row. It reports the higher of the two id-1 scores for both, as it always
-- has, where the query-qualified form is NULL.
CREATE INDEX sdk_i ON sdk USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id', language = 'english');
DROP INDEX sdk_t;
SELECT bm25_seal('sdk_i') IS NOT NULL AS sealed_i2;
SET enable_sort = on;
CREATE TEMP TABLE sdk_fb AS
  SELECT x.id, round(bm25_score_key(x.id)::numeric,6) AS key_old,
         round(bm25_score_key(x.id, 'foo')::numeric,6) AS key_new
    FROM (SELECT id FROM sdk WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) x
   ORDER BY x.id;
RESET enable_sort;
SELECT count(*) AS dup_rows,
       bool_and(f.key_old = (SELECT max(s) FROM sdk_int WHERE id = 1)) AS one_arg_fallback_reports_max,
       count(f.key_new) AS key_form_nonnull
  FROM sdk_fb f WHERE f.id = 1;

-- 5b. A pending (unsealed) row and a HOT-updated row. The pending row is ranked
-- from the pending list with its key backfilled; the HOT row is projected at its
-- new ctid while the ranking holds the chain's root.
INSERT INTO sq(id, body) VALUES (7, 'foo foo bar omicron');
UPDATE sq SET note = 'hot' WHERE id = 3;
SELECT ctid <> (SELECT tid FROM sq_foo WHERE id = 3) AS id3_moved FROM sq WHERE id = 3;
-- One pending document: the insert. A non-HOT update would have called aminsert and
-- made it two, so this is what shows the update was HOT.
SELECT pending_ndocs FROM bm25_stats('sq_bm25');
CREATE TEMP TABLE sq_pend AS
  SELECT id, round(bm25_score(ctid)::numeric,6) AS s
    FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo';
CREATE TEMP TABLE sq_pend2 AS
  SELECT a.id,
         round(bm25_score(a.ctid, 'foo')::numeric,6)   AS foo_tid,
         round(bm25_score_key(a.id, 'foo')::numeric,6) AS foo_key,
         round(bm25_score(b.ctid, 'bar')::numeric,6)   AS bar_tid
    FROM (SELECT id, ctid FROM sq WHERE body @@@ 'foo' ORDER BY body &@@ 'foo' OFFSET 0) a
    JOIN (SELECT id, ctid FROM sq WHERE body @@@ 'bar' ORDER BY body &@@ 'bar' OFFSET 0) b
      ON a.id = b.id;
SELECT count(*) AS rows_,
       bool_and(coalesce(p.foo_tid = s.s AND p.foo_key = s.s, false)) AS pending_and_hot_correct,
       bool_and(p.bar_tid IS NOT NULL) AS bar_side_resolved,
       bool_or(p.id = 7) AS has_pending_row,
       bool_or(p.id = 3) AS has_hot_row
  FROM sq_pend2 p JOIN sq_pend s USING (id);

RESET enable_sort;
RESET enable_seqscan;
DROP FUNCTION sqo_plan(text);
DROP FUNCTION sq_plx_probe();
DROP FUNCTION sq_plx_pinned();
DROP TABLE sq_foo, sq_bar, sq_single, sq_json, sq_kj, sq_sort, sq_mat, sm_foo, sm_bar, plx_foo, plx_bar,
           sq_plx_out, sq_cur, sq_nested, ih_base, ih_out, pp_base, pp_out, sw_base, sw_out,
           sq_pend, sq_pend2, sq_self, sw_pad, sw_stream, sdk_int, sdk_text, sdk_fb, sx_base, sx_out;
DROP TABLE sq, sm, sq_plx, ih1, ih2, ih, pp, sw, sx, sdk;
DROP EXTENSION bm25_native;
