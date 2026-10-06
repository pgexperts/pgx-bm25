-- 32_field_query.sql — @@@ field:term micro-parse + single-scan BM25F (cluster C4).
--
-- Covers: (C4.1) the RHS "field:term" parse in bm25_rescan — split on the FIRST
-- colon, resolve the left side to a dense field_id against the BAKED field-config
-- page (R3: not catalog attnames), ERROR on unknown field, no colon => all fields
-- (BM25_FIELD_ALL); (C4.2) threading so->qfield into BOTH the BM25F scorer and the
-- @@@ boolean path so a scoped query ignores other fields; (C4.3) &@@ ranked order
-- composes unchanged and the multi-column index is driven by ONE Index Scan.
--
-- The index scope lives entirely in the RHS text — the LHS column is just the
-- opclass anchor; driving @@@ on any indexed text column runs the one scan.
CREATE EXTENSION bm25_native;

CREATE TABLE fq (
    id     int    PRIMARY KEY,
    title  text,
    body   text
) WITH (autovacuum_enabled = off);
INSERT INTO fq VALUES
    (1, 'running shoes',     'a long body about hiking boots and trails'),
    (2, 'hiking boots',      'these running shoes are light and fast'),
    (3, 'trail guide',       'nothing relevant here at all');

CREATE INDEX fq_bm25 ON fq USING bm25_native (title, body)
    WITH (language = 'english');

SET enable_seqscan = off;

-- ===== C4.1: field:term parse contract (F-QUERY-EDGES) =====

-- (a) unknown field name ERRORs (undefined_column). The left side "nosuchfield"
--     matches no baked field_name, so the parse raises before any scan.
SELECT id FROM fq WHERE title @@@ 'nosuchfield:running'
ORDER BY title &@@ 'nosuchfield:running';

-- (a2) empty field (":term") is an unknown field "" -> same ERROR (R9).
SELECT id FROM fq WHERE title @@@ ':running'
ORDER BY title &@@ ':running';

-- (b) a bare (no-colon) query is accepted: BM25F across all fields. 'running'
--     is in doc1's title AND doc2's body, so both match.
SELECT id FROM fq WHERE title @@@ 'running'
ORDER BY title &@@ 'running';

-- (b2) a bare term that IS a field name ("title") with no colon is NOT a field
--      scope — it is an all-fields search for the stem of "title" (R9). No row
--      contains the word "title", so this returns zero rows (NOT an error, NOT a
--      title-field scope).
SELECT id FROM fq WHERE title @@@ 'title'
ORDER BY title &@@ 'title';

-- ===== C4.2: field scope restricts BOTH scan paths =====

-- (c) scoped scoring query: title:running returns ONLY docs whose TITLE has the
--     term. doc1 (title 'running shoes') matches; doc2 (title 'hiking boots',
--     'running' only in body) must be ABSENT under title scope.
SELECT id FROM fq WHERE title @@@ 'title:running'
ORDER BY title &@@ 'title:running';

-- (c2) the complementary scope: body:running returns ONLY doc2 (body has it),
--      never doc1 (whose 'running' is in title). Proves the scope is a real
--      per-field filter, not "field 0 vs everything".
SELECT id FROM fq WHERE title @@@ 'body:running'
ORDER BY title &@@ 'body:running';

-- (d) bare query is BM25F across all fields: both doc1 (title) and doc2 (body).
SELECT id FROM fq WHERE title @@@ 'running'
ORDER BY title &@@ 'running';

-- NOTE: a field:term scope is honored ONLY when the bm25_native index answers the query
-- (the scope is parsed inside the index scan, in bm25_rescan). The ranked
-- `col &@@ 'field:term'` form above FORCES the bm25_native index — only it can compute the
-- &@@ order — so those scoped results are reliable on every PG version. A BARE
-- boolean `@@@` filter (e.g. `WHERE title @@@ 'body:running' ORDER BY id`) can
-- instead be answered by another index (here the id PRIMARY KEY, since seqscan is
-- off) with `@@@` applied as a recheck via bm25_match, which has no index Relation
-- and cannot resolve field names — it matches all fields (and the planner's choice
-- is cost-driven, so it varies across PG majors). This is the same class as the
-- documented bare-@@@ seqscan limitation (bm25_match uses the default analyzer):
-- field:term is a property of the index SCAN, not the standalone operator. So we do
-- NOT assert a plan-dependent boolean-filter result here; the &@@ tests (c)/(c2)
-- above are the reliable per-field-scope proof.

-- ===== C4.1 edge: colon-in-term + trailing colon =====

-- (f) colon-in-term: only the FIRST colon splits, so "title:running:shoes" scopes
--     to field "title" with the term text "running:shoes". That term tokenizes to
--     "running" + "shoes" (the colon is a separator), and doc1's title has both.
--     doc2's 'running' is in body, so under title scope only doc1 matches. Uses the
--     ranked &@@ form so the bm25_native index (which parses field:term) reliably answers it.
SELECT id FROM fq WHERE title @@@ 'title:running:shoes'
ORDER BY title &@@ 'title:running:shoes';

-- (g) trailing colon ("title:") -> field "title", EMPTY term. An empty term
--     tokenizes to nothing and matches nothing -> ZERO rows, NOT an error (R9).
SELECT id FROM fq WHERE title @@@ 'title:'
ORDER BY title &@@ 'title:';

-- ===== C4.3: &@@ ranked composition + single Index Scan in EXPLAIN =====

-- (h) &@@ order-by composes: ranked, no Sort node, ONE Index Scan over the
--     multi-column index (spec §10 "single index scan in EXPLAIN"). A future
--     regression that split the multi-column drive into per-column scan keys or
--     lost the order-by amop would surface here as a Seq Scan / Sort diff.
EXPLAIN (COSTS OFF)
SELECT id FROM fq WHERE title @@@ 'running'
ORDER BY title &@@ 'running' LIMIT 5;

-- (i) a field-scoped ORDER BY still produces a single Index Scan (the scope is
--     internal to the scorer; the plan shape is identical to the bare query).
EXPLAIN (COSTS OFF)
SELECT id FROM fq WHERE title @@@ 'title:running'
ORDER BY title &@@ 'title:running' LIMIT 5;

RESET enable_seqscan;
DROP TABLE fq;
DROP EXTENSION bm25_native;
