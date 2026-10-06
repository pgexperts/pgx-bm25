-- 119_dict_lookup_handoff -- the ranked scorers take each (term, segment) dictionary
-- lookup from bm25_term_idf's df pass instead of walking the dictionary again (issue
-- #246).
--
-- The df pass looks every query term up in every segment's dictionary, and the WAND
-- driver and the exhaustive scorer then looked it up a second time, each after a second
-- header read -- ADR 0100 measured up to a quarter of a rare-term query over many
-- segments. The df pass now records (found, postings root and offset, df, POS root and
-- offset) per segment, and the scorers read that. A segment without any query term is
-- also skipped before its header is read. That is a performance change and ranks the
-- same rows, so nothing here failed before it. What the file pins:
--   * the saving, as the marginal buffer cost of a query term per segment, which fails
--     on the pre-change build;
--   * that the handed-over entries are the right ones: terms present in one segment and
--     absent from the rest, a phrase whose POS roots come from the same entries, both
--     df-pass branches (one field and two), WAND against the exhaustive scorer, and
--     both against the heap.
CREATE EXTENSION bm25_native;

-- 24 sealed segments of 10 documents each, on a one-field and a two-field index. Every
-- document holds 'common', 'alpha beta' and its segment's own term s<N>; even documents
-- also end in 'beta alpha'; 'rare' is in four documents of segment 7 only.
CREATE TABLE dh2 (id int PRIMARY KEY, title text, body text) WITH (autovacuum_enabled = off);
CREATE INDEX dh2_bm ON dh2 USING bm25_native (title, body);
CREATE TABLE dh1 (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
CREATE INDEX dh1_bm ON dh1 USING bm25_native (body);
DO $$
BEGIN
  FOR s IN 0..23 LOOP
    INSERT INTO dh2 SELECT s * 10 + g, 'head' || (g % 3),
           'common alpha beta s' || s || CASE WHEN s = 7 AND g <= 4 THEN ' rare' ELSE '' END
           || CASE WHEN g % 2 = 0 THEN ' beta alpha' ELSE '' END
      FROM generate_series(1, 10) g;
    INSERT INTO dh1 SELECT id, body FROM dh2 WHERE id BETWEEN s * 10 + 1 AND s * 10 + 10;
    PERFORM bm25_seal('dh2_bm');
    PERFORM bm25_seal('dh1_bm');
  END LOOP;
END
$$;
SELECT c::text AS idx, count(*) AS segs, sum(ndocs) AS ndocs
  FROM unnest(ARRAY['dh1_bm', 'dh2_bm']::regclass[]) c, bm25_debug_segcat(c)
 GROUP BY c ORDER BY 1;

-- Buffer accesses of the top plan node; the two probes run the real builders and touch
-- no heap page. FORMAT JSON keeps the parse independent of the text layout.
CREATE FUNCTION pg_temp.bufs(q text) RETURNS bigint AS $$
DECLARE
  plan json;
BEGIN
  EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q
     INTO plan;
  RETURN (plan -> 0 -> 'Plan' ->> 'Shared Hit Blocks')::bigint
       + (plan -> 0 -> 'Plan' ->> 'Shared Read Blocks')::bigint;
END
$$ LANGUAGE plpgsql;
SELECT pg_temp.bufs($$SELECT * FROM bm25_wand_stats('dh1_bm', 'rare', 10)$$) > 0 AS warmed,
       pg_temp.bufs($$SELECT * FROM bm25_wand_stats('dh2_bm', 'rare', 10)$$) > 0 AS warmed;

-- The marginal cost of two more query terms (s7, s9), each in one segment and absent
-- from the other 23, divided per (term, segment). It cancels the query's fixed cost.
-- Per (term, segment), the df pass reads the header and walks the dictionary (2); the
-- exhaustive scorer used to repeat both (4 in all), and the WAND driver repeated the
-- walk and read the header once per segment for all terms together. Measured: the
-- exhaustive scorer 2.96 after the change against 4.92 before, the WAND build 2.52
-- against 3.50 -- the rest is the two terms' own postings.
SELECT i::text AS idx,
       (pg_temp.bufs(format('SELECT * FROM bm25_debug_rank(%L, %L)', i, 'rare s7 s9'))
        - pg_temp.bufs(format('SELECT * FROM bm25_debug_rank(%L, %L)', i, 'rare')))
         < 3.5 * 2 * 24 AS exhaustive_dict_walked_once,
       (pg_temp.bufs(format('SELECT * FROM bm25_wand_stats(%L, %L, 10)', i, 'rare s7 s9'))
        - pg_temp.bufs(format('SELECT * FROM bm25_wand_stats(%L, %L, 10)', i, 'rare')))
         < 3.0 * 2 * 24 AS wand_dict_walked_once
  FROM unnest(ARRAY['dh1_bm', 'dh2_bm']::regclass[]) i ORDER BY 1;

-- WAND agrees with the exhaustive scorer row for row (TID and score), and the
-- exhaustive scorer ranks exactly the heap rows that hold a query term.
SELECT i::text AS idx, q,
       (SELECT count(*) FROM (SELECT tid, score FROM bm25_debug_wand_rank(i, q, 1000)
                              EXCEPT
                              SELECT tid, score FROM bm25_debug_rank(i, q)) d) AS wand_minus_exhaustive,
       (SELECT count(*) FROM (SELECT tid, score FROM bm25_debug_rank(i, q)
                              EXCEPT
                              SELECT tid, score FROM bm25_debug_wand_rank(i, q, 1000)) d) AS exhaustive_minus_wand,
       (SELECT count(*) FROM bm25_debug_rank(i, q)) AS ranked
  FROM unnest(ARRAY['dh1_bm', 'dh2_bm']::regclass[]) i,
       unnest(ARRAY['rare', 'rare s7 s9', 's23 s0', 'common', 'nowhere rare']) q
 ORDER BY 1, 2;

-- A phrase takes its POS roots from the same handed-over entries: the even documents
-- in every segment, and nothing else.
SET enable_seqscan = off;
SELECT (SELECT array_agg(id ORDER BY id)
          FROM (SELECT id FROM dh1 WHERE body @@@ '"beta alpha"'
                 ORDER BY body &@@ '"beta alpha"' LIMIT 1000) s)
       = (SELECT array_agg(id ORDER BY id) FROM dh1 WHERE body LIKE '%beta alpha%') AS phrase_one_field,
       (SELECT array_agg(id ORDER BY id)
          FROM (SELECT id FROM dh2 WHERE title @@@ '"beta alpha"'
                 ORDER BY title &@@ '"beta alpha"' LIMIT 1000) s)
       = (SELECT array_agg(id ORDER BY id) FROM dh2 WHERE body LIKE '%beta alpha%') AS phrase_two_fields,
       (SELECT count(*) FROM dh1 WHERE body LIKE '%beta alpha%') AS phrase_rows;
-- A boolean tree with a phrase leaf and a term only segment 7 holds. (A two-field
-- index is queried through its first column; the leaves scope themselves to body.)
SELECT array_agg(id ORDER BY id) AS boolean_phrase_and_rare
  FROM (SELECT id FROM dh2
         WHERE title @@@ bm25_boolean(must => ARRAY[bm25_phrase('body', 'beta alpha'),
                                                   bm25_term('body', 'rare')])
         ORDER BY title &@@ bm25_boolean(must => ARRAY[bm25_phrase('body', 'beta alpha'),
                                                      bm25_term('body', 'rare')]) LIMIT 100) s;

RESET enable_seqscan;
DROP TABLE dh1, dh2;
DROP EXTENSION bm25_native;
