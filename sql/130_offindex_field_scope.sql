-- 130_offindex_field_scope -- a field-scoped @@@ applied as a FILTER refuses
-- instead of answering a different question (#298).
--
-- On the bm25 index path `title @@@ 'body:cat'` means "rows whose body field
-- contains cat". Off the index the operator's function, bm25_match, sees only the
-- title value and no index, so it used to search `body` and `cat` as two terms in
-- the title: rows disjoint from the index answer, silently. It now raises
-- feature_not_supported, as it already did for a phrase (#132). The filter path is
-- reached by ordinary shapes, each pinned below with a plan guard: enable_indexscan
-- off, `@@@ ... OR ...` (the AM has no amgetbitmap), and a non-owner on a table with
-- a row-level security policy (bm25_match is not LEAKPROOF).
--
-- What does NOT change, also pinned: the index path (scope honoured, unknown field
-- an error) and a bare RHS off the index, which still evaluates against the LHS
-- column only -- the documented multi-column divergence (ADR 0004).
CREATE EXTENSION bm25_native;

-- The id set a query returns, or the SQLSTATE name and message it raises.
CREATE FUNCTION fs_ids(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  EXECUTE 'SELECT coalesce(array_agg(id ORDER BY id)::text, ''{}'') FROM (' || q || ') s'
    INTO r;
  RETURN r;
EXCEPTION
  WHEN feature_not_supported THEN RETURN 'feature_not_supported: ' || SQLERRM;
  WHEN undefined_column THEN RETURN 'undefined_column: ' || SQLERRM;
END $$;

-- Plan shape, without pinning version-variant EXPLAIN text: does the bm25 index
-- answer the query, and is @@@ applied as a Filter?
CREATE FUNCTION fs_plan(q text, OUT bm25_index bool, OUT filter_at3 bool)
LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  bm25_index := false;
  filter_at3 := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    bm25_index := bm25_index OR l LIKE '%m_bm25%';
    filter_at3 := filter_at3 OR (l LIKE '%Filter:%' AND l LIKE '%@@@%');
  END LOOP;
END $$;

CREATE TABLE m (id int PRIMARY KEY, title text, body text) WITH (autovacuum_enabled = off);
INSERT INTO m VALUES
  (1, 'cat', 'zebra'),
  (2, 'dog', 'cat'),
  (3, 'meeting at 10:30', 'notes');
CREATE INDEX m_bm25 ON m USING bm25_native (title, body);

-- ------------------------------------------------------------ index path: unchanged
SET enable_seqscan = off;
SELECT * FROM fs_plan($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);    -- {2}: body only
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'cat'$$);         -- {1,2}: every field
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'nope:cat'$$);    -- unknown field
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:cat' ORDER BY title &@@ 'body:cat'$$);

-- ------------------------------------------------------------ enable_indexscan = off
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT * FROM fs_plan($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);
-- The full error once (message, detail, hint), then through fs_ids.
SELECT id FROM m WHERE title @@@ 'body:cat';
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);
-- The scope rule (bm25_query_field_prefix): no whitespace or quote before the colon,
-- leading whitespace skipped, a space after the colon allowed. Not identifier-only.
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ '  body:cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body: cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'nope:cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ ':cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ '10:30'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'http://example.com'$$);
-- A space before the colon: a bare term, still evaluated here.
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'meeting 10:30'$$);  -- {3}
-- A scoped phrase keeps its #132 phrase error (that check runs first).
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:"zebra cat"'$$);
-- A colon inside a leading phrase is not a scope: still the phrase error.
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ '"a: b"'$$);
-- Bare RHS: still evaluated, against the LHS column only ({1}, where the index
-- path above gave {1,2}) -- the documented divergence, not refused.
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'cat'$$);
-- The jsonb builders were already refused off the index, scope or not.
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ bm25_term('body', 'cat')$$);
RESET enable_indexscan;

-- ------------------------------------------------------------ @@@ ... OR ...
SET enable_seqscan = off;
SELECT * FROM fs_plan($$SELECT id FROM m WHERE title @@@ 'body:cat' OR id = 4$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:cat' OR id = 4$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'cat' OR id = 4$$);    -- {1}

-- ------------------------------------------------------------ RLS, non-owner
ALTER TABLE m ENABLE ROW LEVEL SECURITY;
CREATE POLICY m_pos ON m USING (id > 0);
CREATE ROLE bm25_h9_reader NOLOGIN;
GRANT USAGE ON SCHEMA public TO bm25_h9_reader;
GRANT SELECT ON m TO bm25_h9_reader;
SET ROLE bm25_h9_reader;
SELECT * FROM fs_plan($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:cat' ORDER BY title &@@ 'body:cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'cat'$$);           -- {1}
RESET ROLE;
-- The owner is exempt from the policy, so it still reaches the index.
SELECT * FROM fs_plan($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);
SELECT fs_ids($$SELECT id FROM m WHERE title @@@ 'body:cat'$$);       -- {2}
RESET enable_seqscan;

-- ------------------------------------------------------------ the function form
-- Used to return t: the field name `body` was searched as a term.
SELECT bm25_match('nobody here has a body', 'body:zzz');
SELECT bm25_match('nobody here has a body', 'body zzz');              -- t: bare

REVOKE SELECT ON m FROM bm25_h9_reader;
REVOKE USAGE ON SCHEMA public FROM bm25_h9_reader;
DROP ROLE bm25_h9_reader;
DROP TABLE m;
DROP FUNCTION fs_ids(text);
DROP FUNCTION fs_plan(text);
DROP EXTENSION bm25_native;
