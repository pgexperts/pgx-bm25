-- H12 (issue #51): the tokenizer's scratch allocations -- bm25_analyze's lowercased
-- copy, its BM25Token array and every per-lexeme copy -- were made in
-- CurrentMemoryContext and never reclaimed. During ambuild that context is the CREATE
-- INDEX statement's (heapam_index_build_range_scan resets econtext's per-tuple context
-- but does not switch into it before calling the callback), and during aminsert it is
-- estate->es_query_cxt (ExecInsertIndexTuples does not switch to a per-tuple context
-- around index_insert). bm25_insert also built a fresh untransformRelOptions List per
-- row via bm25_resolve_fields and freed none of it. Both now run in a scratch context
-- the caller resets per tuple / deletes per row, as gininsert does.
--
-- WHAT THIS SUITE CAN AND CANNOT PROVE. The defect was memory FOOTPRINT, not outcome:
-- pre-fix, every row produced the same postings, just with ~13x the scanned text bytes
-- left live until the statement ended. pg_regress compares output, so no expected-output
-- test separates pre- from post-fix, and the CI floor in ci.yml is what fails if either
-- scratch context is deleted.
--
-- What this suite DOES pin is the hazard the fix introduces: the build callback now
-- resets a context while the accumulator holds the terms it was handed. That is only
-- safe because accum_find_or_add_term palloc+memcpy's each term into the accumulator's
-- own context and copies positions by value. If that ever stops being true, the
-- accumulator ends up pointing at freed scratch and the symptom is wrong search results
-- -- not a crash -- so the assertions below are deliberately over many rows and many
-- distinct terms per row, with the same corpus fed through BOTH paths and compared.
CREATE EXTENSION bm25_native;

-- ---------------------------------------------------------------- build path
-- 200 rows x ~40 distinct stems each, so the callback resets its arena 200 times and
-- every reset happens with ~8000 accumulated terms already live in the accumulator.
-- A term freed under the accumulator shows up as a miss or a wrong id here.
CREATE TABLE tsb (id int, body text);
INSERT INTO tsb
SELECT g, (SELECT string_agg('term' || ((g * 40 + w))::text, ' ')
           FROM generate_series(1, 40) w)
FROM generate_series(1, 200) g;
CREATE INDEX tsb_idx ON tsb USING bm25_native (body);
SET enable_seqscan = off;

-- Exact-membership probes spread across the corpus: first row's first term, a
-- mid-corpus term, the last row's last term. Each term is unique to one row, so the
-- answer is a single id and a garbled term byte would change it.
SELECT array_agg(id ORDER BY id) AS build_first  FROM tsb WHERE body @@@ 'term41';
SELECT array_agg(id ORDER BY id) AS build_middle FROM tsb WHERE body @@@ 'term4020';
SELECT array_agg(id ORDER BY id) AS build_last   FROM tsb WHERE body @@@ 'term8040';
-- Every row must be reachable by its own first term: 200 distinct single-row hits.
SELECT count(*) AS build_rows_reachable FROM (
  SELECT g FROM generate_series(1, 200) g
  WHERE EXISTS (SELECT 1 FROM tsb WHERE body @@@ ('term' || (g * 40 + 1)::text)
                                    AND id = g)) s;

-- ---------------------------------------------------------------- insert path
-- The same corpus through aminsert instead, in ONE multi-row statement so all 200
-- index inserts share one es_query_cxt -- the shape that accumulated the leak. Empty
-- index first so the comparison is path-vs-path, not path-vs-mixed.
CREATE TABLE tsi (id int, body text);
CREATE INDEX tsi_idx ON tsi USING bm25_native (body);
INSERT INTO tsi
SELECT g, (SELECT string_agg('term' || ((g * 40 + w))::text, ' ')
           FROM generate_series(1, 40) w)
FROM generate_series(1, 200) g;

SELECT array_agg(id ORDER BY id) AS insert_first  FROM tsi WHERE body @@@ 'term41';
SELECT array_agg(id ORDER BY id) AS insert_middle FROM tsi WHERE body @@@ 'term4020';
SELECT array_agg(id ORDER BY id) AS insert_last   FROM tsi WHERE body @@@ 'term8040';
SELECT count(*) AS insert_rows_reachable FROM (
  SELECT g FROM generate_series(1, 200) g
  WHERE EXISTS (SELECT 1 FROM tsi WHERE body @@@ ('term' || (g * 40 + 1)::text)
                                    AND id = g)) s;

-- The two paths must agree term for term. A per-row scratch bug on either side shows
-- up as a non-empty symmetric difference.
SELECT count(*) AS build_vs_insert_diff FROM (
  SELECT g FROM generate_series(1, 200) g
  WHERE (SELECT count(*) FROM tsb WHERE body @@@ ('term' || (g * 40 + 7)::text))
     <> (SELECT count(*) FROM tsi WHERE body @@@ ('term' || (g * 40 + 7)::text))) s;

-- ---------------------------------------------------------------- seal after scratch
-- The opportunistic seal runs AFTER the insert scratch is deleted, so the accumulator
-- bm25_pending_drain builds must not be parented on the freed context. seal_threshold is
-- in kB with a 64 kB floor, so cross it with volume: 400 rows x 20 stems is ~160 kB of
-- pending records, which fires the seal partway through the INSERT statement itself --
-- i.e. with a scratch context live on the stack below it.
SET bm25_native.seal_threshold = 64;
CREATE TABLE tss (id int, body text);
CREATE INDEX tss_idx ON tss USING bm25_native (body);
INSERT INTO tss
SELECT g, (SELECT string_agg('stem' || ((g * 20 + w))::text, ' ')
           FROM generate_series(1, 20) w)
FROM generate_series(1, 400) g;
-- Sealed segments now exist alongside whatever is still pending; both must answer.
SELECT count(*) > 0 AS seal_fired_mid_insert FROM bm25_debug_segcat('tss_idx');
SELECT array_agg(id ORDER BY id) AS sealed_first FROM tss WHERE body @@@ 'stem21';
SELECT array_agg(id ORDER BY id) AS sealed_last  FROM tss WHERE body @@@ 'stem8020';
SELECT count(*) AS sealed_rows_reachable FROM (
  SELECT g FROM generate_series(1, 400) g
  WHERE EXISTS (SELECT 1 FROM tss WHERE body @@@ ('stem' || (g * 20 + 1)::text)
                                    AND id = g)) s;
SELECT bm25_seal('tss_idx');
SELECT array_agg(id ORDER BY id) AS sealed_first_after FROM tss WHERE body @@@ 'stem21';
SELECT count(*) AS sealed_rows_after FROM (
  SELECT g FROM generate_series(1, 400) g
  WHERE EXISTS (SELECT 1 FROM tss WHERE body @@@ ('stem' || (g * 20 + 1)::text)
                                    AND id = g)) s;
RESET bm25_native.seal_threshold;

-- ---------------------------------------------------------------- multi-field + key
-- bm25_resolve_fields runs inside the insert scratch and its untransformRelOptions List
-- was part of the leak. A multi-column index with a key_field exercises the key extract
-- and the field resolution on the same per-row context, and field scoping proves the
-- per-field token arrays were not crossed by the reset.
-- key_field is carried as an INCLUDE column (amcaninclude), per 33_keymap.
CREATE TABLE tsk (id int, title text, body text);
CREATE INDEX tsk_idx ON tsk USING bm25_native (title, body) INCLUDE (id)
  WITH (key_field = 'id');
INSERT INTO tsk
SELECT g, 'title' || g::text || ' shared', 'body' || g::text || ' shared'
FROM generate_series(1, 50) g;
SELECT array_agg(id ORDER BY id) AS key_title_scope FROM tsk WHERE title @@@ 'title:title7';
SELECT array_agg(id ORDER BY id) AS key_body_scope  FROM tsk WHERE title @@@ 'body:body7';
SELECT count(*) AS key_shared_all FROM tsk WHERE title @@@ 'shared';
SELECT bm25_seal('tsk_idx');
SELECT array_agg(id ORDER BY id) AS key_title_scope_sealed FROM tsk WHERE title @@@ 'title:title7';
-- The key extracted inside the per-row scratch reached the KEYMAP intact: once sealed,
-- bm25_score_key resolves the row by its decoded key rather than returning NULL. (Before
-- the seal the doc is still a pending record with no keymap, so a NULL there is the
-- pending/sealed boundary, not this fix -- 33_keymap owns that case.)
SELECT id, bm25_score_key(id) IS NOT NULL AS has_score
FROM tsk WHERE title @@@ 'title:title7' ORDER BY title &@@ 'title:title7';

RESET enable_seqscan;
DROP TABLE tsk;
DROP TABLE tss;
DROP TABLE tsi;
DROP TABLE tsb;
DROP EXTENSION bm25_native;
