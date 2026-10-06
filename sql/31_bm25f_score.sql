-- 31_bm25f_score.sql — BM25F scoring (cluster C3).
--
-- Covers: (C3.1) per-field corpus stats — Σdoclen per field AND N_field (live
-- docs that HAVE the field, i.e. >=1 token in it) serialized in the segment
-- header; (C3.2) the BM25F scorer sum
--   Σ_field boost_f · idf_f · termscore(tf, doclen_f, avgdl_f, k1_f, b_f)
-- in the single scorer pass; (C3.3) single-segment == multi-segment equality.
--
-- Single-field back-compat: a one-column index must collapse to the exact M3
-- formula (N_field[0]=ndocs, df_field[0]=dict.df, boost=1.0) — suites 00..30
-- assert that by staying byte-identical.
CREATE EXTENSION bm25_native;

-- ===== C3.1: per-field corpus stats (Σdoclen + N_field) =====
-- N_field counts docs that HAVE >=1 token in the field. To discriminate a REAL
-- ndocs_by_field[] from the naive "N_field == live_ndocs" shortcut, doc 4 has an
-- EMPTY body: it contributes to title's N but NOT to body's N.
CREATE TABLE bm25f_docs (id int PRIMARY KEY, title text, body text)
  WITH (autovacuum_enabled = off);
INSERT INTO bm25f_docs VALUES
  (1, 'alpha beta',   'alpha alpha gamma delta epsilon zeta'),
  (2, 'alpha',        'beta gamma'),
  (3, 'gamma delta',  'alpha'),
  (4, 'lone title',   '');          -- empty body: NOT counted in body's N_field
-- Default (english) analyzer: the Greek-letter tokens are stem-stable and
-- non-stopwords, so token counts equal word counts. That matters because a field's
-- LENGTH is its word-run count, not its token count (bm25_accum_add_field_tokens
-- computes max position + 1): with this analyzer the two coincide, so the arithmetic
-- below can be read off the words. Avoids a 'simple' dict this
-- PG build lacks; the BM25F math is analyzer-agnostic.
CREATE INDEX bm25f_idx ON bm25f_docs USING bm25_native (title, body)
  WITH (boost_title = '5.0', boost_body = '1.0');

-- title lengths: [2,1,2,2] = 7 over 4 docs  -> avgdl_title = 7/4 = 1.75
-- body  lengths: [6,2,1,0] = 9 over 3 docs  -> avgdl_body  = 9/3 = 3.00
--   (doc 4's empty body is NOT in body's N: N_body = 3, N_title = 4).
SELECT field_id, ndocs_field, total_len_field,
       round((total_len_field::numeric / ndocs_field), 4) AS avgdl_field
FROM bm25_debug_field_stats('bm25f_idx')
ORDER BY field_id;

-- Per-segment witness: the header carries total_len_by_field[] already (C2);
-- ndocs_by_field[] is the C3.1 addition (present only when field_count > 1).
SELECT field_id, total_len FROM bm25_debug_seg_lenfields('bm25f_idx', 0)
ORDER BY field_id;

-- ===== C3.2: BM25F scorer — per-field boost sum =====
-- 'alpha' appears in:
--   doc 1: title (tf 1, doclen_title 2) AND body (tf 2, doclen_body 6)
--   doc 2: title only (tf 1, doclen_title 1)      -> short, high-boost field
--   doc 3: body only  (tf 1, doclen_body 1)        -> short, NO boost
-- With boost_title = 5.0, boost_body = 1.0, the 5x title weight makes doc 2's
-- lone short 'alpha' title the top hit (5 x a strong length-norm term), ahead of
-- doc 1 (boosted short title + un-boosted long body) and far ahead of the
-- un-boosted body-only doc 3. Expected id order: 2, 1, 3 (doc 4 has no 'alpha').
-- The operator is text @@@ text (C4 adds the key-column form); driving it on any
-- indexed text column runs the multi-column index scan, which scores BM25F across
-- ALL fields internally (the LHS column does not scope the search in C3).
SET enable_seqscan = off;
SELECT id, round(bm25_score(ctid)::numeric, 6) AS score
FROM bm25f_docs
WHERE title @@@ 'alpha'
ORDER BY title &@@ 'alpha';

-- The id order 2,1,3 above IS a boost lock: with a neutered boost (both 1.0) doc 1
-- rises to the top (order 1,2,3), so that ORDER BY result flips. (Note: a naive
-- "doc2 score > doc3 score" check would NOT test boost — title and body here have
-- different N_field, 4 vs 3, so idf alone separates them. The symmetric test below
-- isolates boost.)
RESET enable_seqscan;

-- Discrimination: boost in ISOLATION. A symmetric 2-doc corpus gives title and body
-- IDENTICAL corpus stats for 'zeta' (N_field=2, df=1, avgdl=1.5, doclen=1 on both
-- sides), so the ONLY factor separating a title-'zeta' doc from a body-'zeta' doc is
-- the 5:1 boost. The boosted-title doc must score EXACTLY 5x the un-boosted-body doc;
-- a neutered boost (1:1) collapses the ratio to 1.0.
CREATE TABLE boost_sym (id int PRIMARY KEY, title text, body text)
  WITH (autovacuum_enabled = off);
INSERT INTO boost_sym VALUES
  (1, 'zeta',         'kappa lambda'),   -- 'zeta' in title only (tf1, dl1)
  (2, 'kappa lambda', 'zeta');           -- mirror: 'zeta' in body only (tf1, dl1)
CREATE INDEX boost_sym_idx ON boost_sym USING bm25_native (title, body)
  WITH (boost_title = '5.0', boost_body = '1.0');
SET enable_seqscan = off;
WITH scored AS (
  SELECT id, bm25_score(ctid) AS score
  FROM boost_sym WHERE title @@@ 'zeta' ORDER BY title &@@ 'zeta'
)
SELECT round(((SELECT score FROM scored WHERE id = 1) /
              (SELECT score FROM scored WHERE id = 2))::numeric, 6)
       AS title_over_body_ratio;   -- 5.000000 iff boost is the isolated factor
RESET enable_seqscan;
DROP TABLE boost_sym;

-- ===== C3.3: single-segment == multi-segment BM25F equality =====
-- The per-field stats (total_len_by_field[], N_field, df_field) all SUM over
-- segments, so a corpus split across sealed segments must score bit-for-bit like
-- the same corpus in one segment. Twin table forced into >1 segment via bm25_seal
-- between batches (the 15_multiseg mechanism). A per-field stat read from a single
-- segment (instead of the corpus sum) would break this.
CREATE TABLE bm25f_docs_ms (id int PRIMARY KEY, title text, body text)
  WITH (autovacuum_enabled = off);
INSERT INTO bm25f_docs_ms SELECT * FROM bm25f_docs WHERE id = 1;
CREATE INDEX bm25f_ms_idx ON bm25f_docs_ms USING bm25_native (title, body)
  WITH (boost_title = '5.0', boost_body = '1.0');   -- ambuild seals segment 0
INSERT INTO bm25f_docs_ms SELECT * FROM bm25f_docs WHERE id IN (2, 3, 4);
SELECT bm25_seal('bm25f_ms_idx');                   -- segment 1: docs 2,3,4
-- Two sealed segments now hold the same corpus as the single-segment bm25f_idx.
SELECT count(*) AS nsegs FROM bm25_debug_segcat('bm25f_ms_idx');

SET enable_seqscan = off;
SELECT s.id, round(s.score::numeric, 6) = round(m.score::numeric, 6) AS scores_match
FROM (SELECT id, bm25_score(ctid) AS score FROM bm25f_docs
        WHERE title @@@ 'alpha' ORDER BY title &@@ 'alpha') s
JOIN (SELECT id, bm25_score(ctid) AS score FROM bm25f_docs_ms
        WHERE title @@@ 'alpha' ORDER BY title &@@ 'alpha') m USING (id)
ORDER BY s.id;
RESET enable_seqscan;

-- ===== C3.4: BM25F over UNSEALED pending rows (read-your-writes) =====
-- The pending list has its OWN per-field BM25F path (bm25_pending_score_term /
-- pending_stats_by_field / pending_df_by_field). Build a non-empty index, then
-- INSERT rows that stay in the pending list (no seal) and score a query: the
-- boosted-title doc must still outrank the un-boosted-body doc, proving the pending
-- path partitions fields + applies per-field boost like the sealed path.
CREATE TABLE bm25f_pend (id int PRIMARY KEY, title text, body text)
  WITH (autovacuum_enabled = off);
INSERT INTO bm25f_pend VALUES (1, 'seed one', 'seed two');   -- non-empty build
CREATE INDEX bm25f_pend_idx ON bm25f_pend USING bm25_native (title, body)
  WITH (boost_title = '5.0', boost_body = '1.0');
-- These rows land in the pending list (unsealed) — exercises the read-your-writes
-- BM25F scoring path, not the sealed-segment path.
INSERT INTO bm25f_pend VALUES
  (2, 'omega',  'kappa lambda'),   -- 'omega' in boosted title (tf1, dl1)
  (3, 'kappa lambda', 'omega');    -- mirror: 'omega' in un-boosted body (tf1, dl1)
SET enable_seqscan = off;
-- Both pending docs match (recall); the boosted-title doc 2 ranks ahead of the
-- un-boosted-body doc 3 — the symmetric-corpus boost isolation, from pending.
SELECT id FROM bm25f_pend WHERE title @@@ 'omega' ORDER BY title &@@ 'omega';
RESET enable_seqscan;
DROP TABLE bm25f_pend;

DROP TABLE bm25f_docs_ms;
DROP TABLE bm25f_docs;
DROP EXTENSION bm25_native;
