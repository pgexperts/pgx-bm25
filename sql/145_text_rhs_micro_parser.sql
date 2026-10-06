-- 145_text_rhs_micro_parser -- the text-RHS `field:` scope and phrase whitespace
-- rules, compared on the bm25 index path and the filter path (#306).
--
-- The index path used to take everything before the first colon (before any
-- quote) as a field name, so ordinary text with a colon -- `meeting 10:30`, a URL,
-- `' body:cat'` -- raised `unknown search field` whenever the bm25 index answered
-- and returned rows whenever it did not. It now uses bm25_query_field_prefix, the
-- rule bm25_match already refused on (#298): leading whitespace, then a run with no
-- whitespace and no quote, ended by a colon. For a name the index does not have, a
-- colon followed by '/' or a digit is literal text; any other unknown name still
-- errors (typo detection). Off the index every scope-shaped RHS stays refused, so
-- the two paths refuse or agree but never silently differ.
--
-- Whitespace: one class (space, tab, newline, CR, form feed) everywhere, so a tab
-- before a phrase no longer silently turns it into an OR query (on either path),
-- and trailing whitespace after `"..."` or `~n` is no longer "text after phrase".
CREATE EXTENSION bm25_native;

-- The statement mp_ids runs for a query, so the plan guard below explains exactly
-- what is executed (the aggregate wrapper can change the plan choice).
CREATE FUNCTION mp_wrap(q text) RETURNS text LANGUAGE sql IMMUTABLE
  RETURN 'SELECT coalesce(array_agg(id ORDER BY id)::text, ''{}'') FROM (' || q || ') s';

-- The id set a query returns, or a short tag for the error it raises.
CREATE FUNCTION mp_ids(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  EXECUTE mp_wrap(q) INTO r;
  RETURN r;
EXCEPTION
  WHEN feature_not_supported THEN
    RETURN CASE WHEN SQLERRM LIKE '%phrase query%' THEN 'refused: phrase'
                WHEN SQLERRM LIKE '%field-scoped%' THEN 'refused: scope'
                ELSE 'feature_not_supported: ' || SQLERRM END;
  WHEN undefined_column OR syntax_error THEN RETURN 'ERROR: ' || SQLERRM;
END $$;

-- One RHS on both paths, with a plan guard (no version-variant EXPLAIN text is
-- pinned): plans_ok is true only when the first run is a bm25 index scan with no
-- @@@ Filter and the second applies @@@ as a Filter without the bm25 index. All
-- three planner GUCs are set explicitly in each phase: set_config(..., true) lasts
-- to the end of the transaction, i.e. across the LATERAL rows of one SELECT.
CREATE FUNCTION mp_paths(rhs text, OUT index_path text, OUT filter_path text,
                         OUT plans_ok bool)
LANGUAGE plpgsql AS $$
DECLARE
  q    text := 'SELECT id FROM d WHERE body @@@ ' || quote_literal(rhs);
  l    text;
  i_ix bool := false;
  i_fl bool := false;
  f_ix bool := false;
  f_fl bool := false;
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_indexscan', 'on', true);
  PERFORM set_config('enable_bitmapscan', 'on', true);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || mp_wrap(q) LOOP
    i_ix := i_ix OR l LIKE '%d\_bm25%';
    i_fl := i_fl OR (l LIKE '%Filter:%' AND l LIKE '%@@@%');
  END LOOP;
  index_path := mp_ids(q);
  PERFORM set_config('enable_seqscan', 'on', true);
  PERFORM set_config('enable_indexscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || mp_wrap(q) LOOP
    f_ix := f_ix OR l LIKE '%d\_bm25%';
    f_fl := f_fl OR (l LIKE '%Filter:%' AND l LIKE '%@@@%');
  END LOOP;
  filter_path := mp_ids(q);
  plans_ok := i_ix AND NOT i_fl AND f_fl AND NOT f_ix;
END $$;

CREATE TABLE d (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO d VALUES
  (1, 'red car'),
  (2, 'a car that is red'),
  (3, 'red red car'),
  (4, 'the red car was fast'),
  (5, 'see http example com'),
  (6, 'the meeting at 10 30'),
  (7, 'the cat sat'),
  (8, 'a body of work');
CREATE INDEX d_bm25 ON d USING bm25_native (body);

-- The rhs column shows control characters escaped and the value bracketed, so
-- leading and trailing whitespace is visible.
CREATE FUNCTION mp_show(t text) RETURNS text LANGUAGE sql IMMUTABLE
  RETURN '[' || replace(replace(replace(t, E'\t', '\t'), E'\n', '\n'), E'\r', '\r') || ']';

-- ------------------------------------------------------------ SCAN-04: the scope rule
-- Before #306 the index path raised `unknown search field` on rows 1-4 and 6-9.
-- `body:10` scopes (known field, digit follower): {6}, not the literal {6,8}.
-- ` body:cat` scopes too (leading whitespace skipped): {7}, not {7,8}.
SELECT n, mp_show(rhs) AS rhs, p.*
FROM (VALUES
  (1,  'meeting 10:30'),             -- whitespace before the colon: not a scope
  (2,  'note to self: "red car"'),   -- not a scope, and not a phrase: an OR query
  (3,  ' body:cat'),                 -- a scope; refused off the index (#298)
  (4,  ' body:"red car"'),           -- a scoped phrase; #132 refusal off the index
  (5,  'body:10'),                   -- known field before a digit: still a scope
  (6,  E'a\tb:cat'),                 -- a tab ends the run: not a scope
  -- An unknown name before '/' or a digit: literal text on the index path,
  -- refused off it (bm25_match cannot tell `10:30` from `body:10`).
  (7,  'http://example.com'),
  (8,  '10:30'),
  (9,  ':30'),
  -- Any other unknown name: still an ERROR on the index path (typo detection).
  (10, 'titel:car'),
  (11, 'Note: buy milk'),
  (12, 'http:'),
  (13, 'nosuch:"red car"'),
  (14, ':running')
) v(n, rhs), LATERAL mp_paths(rhs) p
ORDER BY n;

-- The ranked path runs the same split: a URL ranks as text.
SET enable_seqscan = off;
SELECT mp_ids($$SELECT id FROM d WHERE body @@@ 'http://example.com'
                ORDER BY body &@@ 'http://example.com'$$) AS ranked_url;
SELECT mp_ids($$SELECT id FROM d WHERE body @@@ 'titel:car'
                ORDER BY body &@@ 'titel:car'$$) AS ranked_typo;
RESET enable_seqscan;

-- ------------------------------------------------------------ QUERY-06: whitespace
-- Every variant answers as `"red car"` / `"red car"~2` on the index path, and every
-- phrase is refused off the index (a leading tab used to hide it from #132).
SELECT n, mp_show(rhs) AS rhs, p.*
FROM (VALUES
  (1,  '"red car"'),
  (2,  '  "red car"'),
  (3,  E'\t"red car"'),
  (4,  E'\n"red car"'),
  (5,  E'\r\n "red car"'),
  (6,  '"red car" '),
  (7,  E'"red car"\n'),
  (8,  E'\t"red car"\t\n'),
  (9,  'body:"red car"'),
  (10, E'body:\t"red car"'),
  (11, E' body: \n"red car" '),
  (12, '"red car"~2'),
  (13, '"red car"~2 '),
  (14, E'"red car"~>2\n'),
  -- Unchanged: a quote that does not open the RHS is ordinary text (an OR query),
  -- an unterminated quote and a malformed suffix still fail, trailing space or not.
  (15, 'fast "red car"'),
  (16, '"red car '),
  (17, '"red car" x'),
  (18, '"red car"~ ')
) v(n, rhs), LATERAL mp_paths(rhs) p
ORDER BY n;

DROP TABLE d;
DROP FUNCTION mp_paths(text);
DROP FUNCTION mp_ids(text);
DROP FUNCTION mp_wrap(text);
DROP FUNCTION mp_show(text);
DROP EXTENSION bm25_native;
