CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text) WITH (autovacuum_enabled=off);
INSERT INTO docs VALUES (1, 'quick brown fox'), (2, 'lazy brown dog');
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);   -- ambuild seals 1 segment
-- Insert more, then force a second sealed segment.
INSERT INTO docs VALUES (3, 'quick quick fox'), (4, 'sleepy cat');
SELECT bm25_seal('docs_bm25');
-- Two sealed segments now exist; the catalog snapshot must show both.
SELECT count(*) AS nsegs, sum(ndocs) AS total_docs
FROM bm25_debug_segcat('docs_bm25');
-- Decode 'quick' postings from each segment via the reader. doc1 (seg0) and
-- doc3 (seg1, tf=2) both contain 'quick'; the per-segment df must be 1 each.
SELECT seg, local_docid, tf
FROM bm25_debug_seg_postings('docs_bm25', 'quick')
ORDER BY seg, local_docid;
-- df-bound regression guard: 'brown' is the FIRST term alphabetically in seg0
-- (docs 1 and 2 both contain it, df=2). All terms' posting blocks share ONE
-- segment-wide chain with no inter-term delimiter, so an UNBOUNDED decode would
-- over-read past 'brown' into dog/fox/lazy/quick. The df bound must stop the
-- decode at exactly 2 postings: seg0 local 0/1, each tf 1, and nothing else.
SELECT seg, local_docid, tf
FROM bm25_debug_seg_postings('docs_bm25', 'brown')
ORDER BY seg, local_docid;
-- Global stats cache: 4 live docs across both segments; total tokens =
-- 3 (quick brown fox) + 3 (lazy brown dog) + 3 (quick quick fox) + 2 (sleepy cat) = 11.
SELECT ndocs, total_len FROM bm25_debug_global_stats('docs_bm25');
-- Every doc is live immediately after seal (no deletes yet).
SELECT bm25_debug_seg_doc_live('docs_bm25', 0, 0) AS seg0_d0_live,
       bm25_debug_seg_doc_live('docs_bm25', 1, 0) AS seg1_d0_live;
SET enable_seqscan = off;
-- Cross-segment ordered scan: 'quick' appears in doc1 (seg0, tf=1) and doc3
-- (seg1, tf=2). The multi-source scorer must union ACROSS the segment boundary
-- and rank doc3 above doc1 (higher tf -> higher BM25). A segment-0-only scorer
-- would miss doc3 entirely.
SELECT d.id
FROM docs d WHERE d.body @@@ 'quick'
ORDER BY d.body &@@ 'quick';
RESET enable_seqscan;
-- Single-segment twin: same corpus, one ambuild (no intermediate seal).
-- Boundary-invariance: splitting a corpus across two sealed segments must not
-- change BM25 scores vs. the same corpus built as one segment.  If global stats
-- (N, avgdl, df) are computed corpus-wide rather than per-segment, the IDF and
-- length-normalisation terms are identical and scores must match bit-for-bit.
CREATE TABLE docs_one (id int primary key, body text) WITH (autovacuum_enabled=off);
INSERT INTO docs_one VALUES
  (1, 'quick brown fox'), (2, 'lazy brown dog'),
  (3, 'quick quick fox'), (4, 'sleepy cat');
CREATE INDEX docs_one_bm25 ON docs_one USING bm25_native (body);  -- one segment
-- Materialize each side's (id, score) from its own independent ordered scan so
-- both bm25_score() calls read their respective active scans, not a shared one.
SET enable_seqscan = off;
CREATE TEMP TABLE ms_scores AS
  SELECT id, round(bm25_score(ctid)::numeric, 6) AS s
  FROM docs WHERE body @@@ 'brown' ORDER BY body &@@ 'brown';
CREATE TEMP TABLE os_scores AS
  SELECT id, round(bm25_score(ctid)::numeric, 6) AS s
  FROM docs_one WHERE body @@@ 'brown' ORDER BY body &@@ 'brown';
SELECT ms.id, ms.s AS multiseg_score, os.s AS oneseg_score, (ms.s = os.s) AS equal
  FROM ms_scores ms JOIN os_scores os ON ms.id = os.id ORDER BY ms.id;
RESET enable_seqscan;
DROP TABLE docs_one;
DROP TABLE docs;
DROP EXTENSION bm25_native;
