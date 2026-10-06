-- 143_query_args_strict -- the jsonb query tree is validated as written, not as
-- the parser happened to read it (#304 QUERY-02/05/01/09, #305 QUERY-03).
--
-- What each section returned before its fix (every one a silent answer, no error):
--   QUERY-02  an unknown key was ignored: "mustnot" dropped the exclusion and
--             returned every row; "fild" searched all fields; "slop" on a term
--             and "field" on a boolean were accepted and meant nothing.
--   QUERY-05  a fractional slop was ROUNDED: 1.7 ran as 2 ({1,2,3}) and -0.4 as 0.
--   QUERY-01  a folded boost outside [1e-6, 1e6] was accepted if finite and > 0:
--             5e-324 on a low-idf term underflowed boost*idf to 0 and the
--             matching rows vanished; elsewhere it was accepted and meaningless.
--   QUERY-03  any number of leafless {"boolean":{}} nodes parsed (the 64-leaf
--             cap counts leaves only), each one evaluated per candidate.
--   QUERY-09  a NULL builder result inside an array was reported as "must be a
--             query node object".
-- Outcomes go through qa_try, which reports the matching ids or the error's
-- SQLSTATE and message, so one line per case carries the whole assertion.
CREATE EXTENSION bm25_native;

-- slop 0 / 1 / 2 / 3 select {1} / {1,2} / {1,2,3} / {1,2,3,4} for "red car", which
-- is what lets a rounded fractional slop show up as a different row set.
CREATE TABLE qa (id int PRIMARY KEY, body text, title text);
INSERT INTO qa VALUES
  (1, 'red car',         'alpha'),
  (2, 'red x car',       'beta'),
  (3, 'red x y car',     'gamma'),
  (4, 'red x y z car',   'delta'),
  (5, 'red bike',        'epsilon');
CREATE INDEX qa_bm ON qa USING bm25_native (body, title);
SET enable_seqscan = off;

-- Filter path (@@@) outcome for a raw tree.
CREATE FUNCTION qa_try(q jsonb) RETURNS text LANGUAGE plpgsql AS
$$
DECLARE ids int[];
BEGIN
    SELECT array_agg(id ORDER BY id) INTO ids FROM qa WHERE body @@@ q;
    RETURN coalesce(ids::text, '{}');
EXCEPTION WHEN OTHERS THEN
    RETURN format('ERROR %s: %s', SQLSTATE, SQLERRM);
END
$$;

-- Ranked path (&@@ under its @@@), membership only: a single-key ORDER BY, then
-- the ids sorted, so no score tie order reaches the output.
CREATE FUNCTION qa_try_ranked(q jsonb) RETURNS text LANGUAGE plpgsql AS
$$
DECLARE ids int[];
BEGIN
    SELECT array_agg(id ORDER BY id) INTO ids FROM
      (SELECT id FROM qa WHERE body @@@ q ORDER BY body &@@ q LIMIT 10) s;
    RETURN coalesce(ids::text, '{}');
EXCEPTION WHEN OTHERS THEN
    RETURN format('ERROR %s: %s', SQLSTATE, SQLERRM);
END
$$;

-- Off-index path: ORDER BY &@@ with no @@@ has no scored scan, so the projection
-- validates the tree with bm25_query_validate (#245, sql/115).
CREATE FUNCTION qa_try_offindex(q jsonb) RETURNS text LANGUAGE plpgsql AS
$$
DECLARE n bigint;
BEGIN
    SELECT count(*) INTO n FROM (SELECT body &@@ q FROM qa ORDER BY 1 LIMIT 3) s;
    RETURN format('ok: %s rows', n);
EXCEPTION WHEN OTHERS THEN
    RETURN format('ERROR %s: %s', SQLSTATE, SQLERRM);
END
$$;

-- The HINT of the error a filter query raises, or 'no error'.
CREATE FUNCTION qa_hint(q jsonb) RETURNS text LANGUAGE plpgsql AS
$$
DECLARE h text;
BEGIN
    PERFORM count(*) FROM qa WHERE body @@@ q;
    RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS h = PG_EXCEPTION_HINT;
    RETURN coalesce(nullif(h, ''), '(no hint)');
END
$$;

-- Do the ranked and off-index paths give the filter path's outcome? Ranked must
-- return the same ids or the same error; off-index (which returns no ids) must
-- raise the same error, or succeed where the filter succeeds.
CREATE FUNCTION qa_paths_agree(q jsonb) RETURNS bool LANGUAGE sql AS
$$ SELECT qa_try_ranked(q) = qa_try(q)
      AND CASE WHEN qa_try(q) LIKE 'ERROR%' THEN qa_try_offindex(q) = qa_try(q)
               ELSE qa_try_offindex(q) LIKE 'ok:%' END $$;

-- ================================================================ QUERY-02
-- Every builder still parses: the builders must never emit a key the closed sets
-- reject. First the keys each one emits (the closed sets, from the builder side)...
SELECT b, array_agg(k ORDER BY k) AS keys FROM (
  SELECT 'match' AS b, jsonb_object_keys(bm25_match_terms('body', 'red') -> 'match') AS k
  UNION ALL SELECT 'term', jsonb_object_keys(bm25_term('body', 'red') -> 'term')
  UNION ALL SELECT 'phrase', jsonb_object_keys(bm25_phrase('body', 'red car') -> 'phrase')
  UNION ALL SELECT 'wildcard', jsonb_object_keys(bm25_wildcard('body', 're*') -> 'wildcard')
  UNION ALL SELECT 'boolean', jsonb_object_keys(bm25_boolean() -> 'boolean')
  UNION ALL SELECT 'boost', jsonb_object_keys(bm25_boost(2.0, bm25_term('body', 'red')) -> 'boost')
) s GROUP BY b ORDER BY b;
-- ...then every builder, with defaults and with every argument, through all three
-- paths. A rejected key would show as an ERROR line here.
CREATE TABLE qa_builders (label text, q jsonb);
INSERT INTO qa_builders VALUES
  ('match_terms',     bm25_match_terms('body', 'red bike')),
  ('term',            bm25_term('body', 'bike')),
  ('phrase_default',  bm25_phrase('body', 'red car')),
  ('phrase_all_args', bm25_phrase('body', 'car red', 3, false)),
  ('wildcard',        bm25_wildcard('body', 'bik*')),
  ('boolean_default', bm25_boolean()),
  ('boolean_all',     bm25_boolean(must     => ARRAY[bm25_term('body', 'red')],
                                   should   => ARRAY[bm25_term('body', 'car')],
                                   must_not => ARRAY[bm25_term('body', 'bike')])),
  ('boost',           bm25_boost(2.0, bm25_term('body', 'bike'))),
  ('nested',          bm25_boost(3.0, bm25_boolean(should => ARRAY[
                        bm25_boost(2.0, bm25_phrase('body', 'red car', 1)),
                        bm25_wildcard('title', 'alph*')])));
SELECT label, qa_try(q) AS filter, qa_paths_agree(q) AS paths_agree
  FROM qa_builders ORDER BY label;
SELECT label, bm25_debug_query_parse('qa_bm', q) AS parsed
  FROM qa_builders WHERE label IN ('phrase_all_args', 'nested') ORDER BY label;

-- The issue's typo: the correct spelling excludes the "car" rows...
SELECT qa_try('{"boolean": {"must": [{"term": {"value": "red"}}],
                            "must_not": [{"term": {"value": "car"}}]}}') AS must_not_spelled_right;
-- ...and the misspellings used to return all five rows. Each now errors.
-- The HINT offers the key a near-spelling meant; an unrelated key gets none.
SELECT k, qa_try(q) AS outcome, qa_hint(q) AS hint
  FROM (SELECT k, jsonb_build_object('boolean', jsonb_build_object(
                    'must', '[{"term": {"value": "red"}}]'::jsonb,
                    k,      '[{"term": {"value": "car"}}]'::jsonb)) AS q
          FROM unnest(ARRAY['mustnot', 'mustNot', 'must-not', 'MUST_NOT', 'Must Not', 'not']) k) s;
-- A stray key on every node kind, including "field" where it is not inherited.
CREATE TABLE qa_stray (label text, q jsonb);
INSERT INTO qa_stray VALUES
  ('term_fild',        '{"term": {"value": "red", "fild": "title"}}'),
  ('term_slop',        '{"term": {"value": "red", "slop": 5}}'),
  ('match_value',      '{"match": {"field": "body", "terms": "red", "value": "x"}}'),
  ('phrase_Slop',      '{"phrase": {"phrase": "red car", "Slop": 2}}'),
  ('wildcard_ordered', '{"wildcard": {"pattern": "bik*", "ordered": true}}'),
  ('boolean_field',    '{"boolean": {"should": [{"term": {"value": "red"}}], "field": "title"}}'),
  ('boost_field',      '{"boost": {"weight": 2, "query": {"term": {"value": "red"}}, "field": "body"}}'),
  ('nested_stray',     '{"boolean": {"should": [{"boost": {"weight": 2,
                         "query": {"term": {"value": "red", "x": null}}}}]}}');
SELECT label, qa_try(q) AS filter, qa_paths_agree(q) AS paths_agree, qa_hint(q) AS hint
  FROM qa_stray ORDER BY label;
-- The DETAIL lists the node's keys.
SELECT count(*) FROM qa WHERE body @@@ '{"boolean": {"should": [{"term": {"value": "red"}}],
                                                     "must-not": []}}'::jsonb;
SELECT count(*) FROM qa WHERE body @@@ '{"term": {"value": "red", "fild": "body"}}'::jsonb;

-- ================================================================ QUERY-05
-- Integral values are accepted however they are spelled: 2.0 has display scale 1
-- and 1e0 an exponent, and both are whole numbers.
SELECT s, qa_try(jsonb_build_object('phrase', jsonb_build_object(
            'field', 'body', 'phrase', 'red car', 'slop', s::numeric))) AS outcome
  FROM unnest(ARRAY['0', '1', '1e0', '2', '2.0', '3.000']) s;
-- Fractions error. Before: 1.4 -> {1,2}, 1.7 -> {1,2,3}, -0.4 -> {1}.
SELECT s, qa_try(jsonb_build_object('phrase', jsonb_build_object(
            'field', 'body', 'phrase', 'red car', 'slop', s::numeric))) AS outcome
  FROM unnest(ARRAY['1.4', '1.5', '1.7', '-0.4', '0.0001', '2e-1']) s;
-- Integral but beyond int32 keeps its existing error (the int4 cast's own).
SELECT qa_try('{"phrase": {"phrase": "red car", "slop": 1e20}}') AS beyond_int32;
-- The builder takes an int, so it cannot produce a fraction at all.
SELECT qa_try(bm25_phrase('body', 'red car', 1)) AS builder_slop_1;

-- ================================================================ QUERY-01
-- One weight, all three paths: the range is closed and inclusive at both ends.
CREATE TABLE qa_w (w text);
INSERT INTO qa_w VALUES ('5e-324'), ('1e-300'), ('1e-7'), ('0.000001'), ('1'),
                        ('1000000'), ('1000001'), ('1e308');
SELECT w, qa_try(q) AS filter, qa_paths_agree(q) AS paths_agree
  FROM (SELECT w, jsonb_build_object('boost', jsonb_build_object('weight', w::numeric,
               'query', '{"term": {"field": "body", "value": "bike"}}'::jsonb)) AS q
          FROM qa_w) s
 ORDER BY w::numeric;
-- Where the underflow was real: "red" is in every row, so its idf is small and
-- 5e-324 * idf rounded to exactly 0. Before the fix this returned {} -- all five
-- matching rows silently gone.
SELECT qa_try('{"boost": {"weight": 5e-324, "query": {"term": {"field": "body", "value": "red"}}}}')
       AS denormal_on_common_term;
-- The same weights inside bm25_boolean(must => ...), built by the builders.
SELECT w, qa_try(bm25_boolean(must => ARRAY[bm25_boost(w::float8, bm25_term('body', 'bike'))])) AS outcome
  FROM qa_w ORDER BY w::numeric;
-- It is the PRODUCT that is checked: two in-range weights whose product leaves the
-- range error, and two out-of-range weights whose product is 1 do not.
SELECT qa_try(bm25_boost(1000, bm25_boost(1000, bm25_term('body', 'bike')))) AS product_1e6;
SELECT qa_try(bm25_boost(1000, bm25_boost(1001, bm25_term('body', 'bike')))) AS product_over;
SELECT qa_try(bm25_boost(0.001, bm25_boost(0.0009, bm25_term('body', 'bike')))) AS product_under;
SELECT qa_try(bm25_boost(1e7, bm25_boost(1e-7, bm25_term('body', 'bike')))) AS product_back_in_range;
-- A boost over a whole boolean reaches every scoring leaf beneath it.
SELECT qa_try(bm25_boost(1e7, bm25_boolean(should => ARRAY[bm25_term('body', 'bike')]))) AS over_boolean;
-- must_not leaves are exempt (their boost is never read): an out-of-range weight
-- there is accepted and the exclusion still applies.
SELECT qa_try(bm25_boolean(must     => ARRAY[bm25_term('body', 'red')],
                           must_not => ARRAY[bm25_boost(1e7, bm25_term('body', 'car'))])) AS must_not_exempt;
-- ...but the exemption is per leaf: a boost spanning both a must and a must_not
-- leaf still errors for the must one.
SELECT qa_try(bm25_boost(1e7, bm25_boolean(must     => ARRAY[bm25_term('body', 'red')],
                                           must_not => ARRAY[bm25_term('body', 'car')]))) AS spans_both;
-- The DETAIL says what is folded and why must_not is exempt.
SELECT count(*) FROM qa WHERE body @@@ bm25_boost(1e7, bm25_term('body', 'bike'));

-- ================================================================ QUERY-03
-- should = n leafless {"boolean":{}} padders plus one real leaf, under a root
-- boolean: n + 2 nodes in all. 1022 padders is exactly 1024 nodes.
CREATE FUNCTION qa_padded(n int) RETURNS jsonb LANGUAGE sql AS
$$ SELECT jsonb_build_object('boolean', jsonb_build_object('should',
            (SELECT jsonb_agg(x) FROM (SELECT '{"boolean": {}}'::jsonb AS x
                                         FROM generate_series(1, n)
                                       UNION ALL SELECT bm25_term('body', 'bike')) s))) $$;
SELECT n, qa_try(qa_padded(n)) AS filter, qa_paths_agree(qa_padded(n)) AS paths_agree
  FROM unnest(ARRAY[1022, 1023, 100000]) n ORDER BY n;
-- bm25_debug_query_parse never flattens, so it had neither cap on interior nodes
-- before; the node cap is in the parser and binds it too.
SELECT length(bm25_debug_query_parse('qa_bm', qa_padded(1022))) > 0 AS debug_at_cap;
\set VERBOSITY terse
SELECT bm25_debug_query_parse('qa_bm', qa_padded(1023));
\set VERBOSITY default
-- Leaves count as nodes too; 64 real leaves (the leaf cap) are 65 nodes, far
-- inside the node cap, and still parse and match.
SELECT qa_try(jsonb_build_object('boolean', jsonb_build_object('should',
         (SELECT jsonb_agg(bm25_term('body', CASE WHEN g = 64 THEN 'bike' ELSE 'w' || g END))
            FROM generate_series(1, 64) g)))) AS leaf_cap_64;

-- ================================================================ QUERY-09
-- The builders stay STRICT: a NULL argument makes the builder NULL, and
-- col @@@ NULL matches nothing (ADR 0015) -- unchanged, pinned as a decision.
SELECT count(*) AS strict_null_field FROM qa WHERE body @@@ bm25_term(NULL, 'red');
-- Inside an array that NULL is now named for what it is, with its position.
SELECT qa_try(bm25_boolean(should => ARRAY[bm25_term('body', 'bike'), bm25_term(NULL, 'red')])) AS null_elem;
SELECT count(*) FROM qa WHERE body @@@
  bm25_boolean(must => ARRAY[bm25_term(NULL, 'red'), bm25_term('body', 'bike')]);
-- A non-NULL non-object element keeps the generic message.
SELECT qa_try('{"boolean": {"should": [1]}}') AS scalar_elem;
-- The all-fields leaf the builders cannot produce, in raw jsonb: omit "field" (or
-- give it as null). "red" is in every body, "alpha" only in row 1's title.
SELECT qa_try('{"term": {"value": "alpha"}}') AS all_fields_omitted,
       qa_try('{"term": {"field": null, "value": "alpha"}}') AS all_fields_null,
       qa_try(bm25_term('body', 'alpha')) AS body_only;

-- pg_regress shares ONE database across suites; leave no extension behind.
DROP FUNCTION qa_try(jsonb), qa_try_ranked(jsonb), qa_try_offindex(jsonb), qa_hint(jsonb),
  qa_paths_agree(jsonb), qa_padded(int);
DROP TABLE qa, qa_builders, qa_stray, qa_w;
DROP EXTENSION bm25_native;
