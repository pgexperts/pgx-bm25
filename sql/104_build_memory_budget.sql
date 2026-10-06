-- 104_build_memory_budget: BUILD-04, the ambuild half. ambuild accumulated the
-- ENTIRE heap's postings into one BM25Accum with no spill, no tuplesort and no
-- memory bound -- the corpus was the only limit. The callback now seals the chunk
-- it holds and starts a fresh accumulator whenever it crosses
-- bm25_maintenance_budget_bytes, so a build publishes ceil(corpus / budget)
-- segments through the same two-phase commit a seal uses.
--
-- WHAT WOULD BE OBSERVABLY BROKEN WITHOUT THE FIX: every `nsegs` assertion below
-- returns 1. That is the A/B tooth and it is the ONLY one -- the membership and
-- ranked-order equalities pass either way by construction, because a correct
-- single-segment build and a correct chunked build must answer identically. They
-- are here to prove the chunking did not change the answers, not to prove it
-- happened. Read `nsegs > 1` as "the chunk path ran" and everything beside it as
-- "and it was still correct".
--
-- SCALE: the budget is shrunk, never the corpus grown -- the doctrine sql/83
-- states for max_match_memory, for the same reason. maintenance_work_mem's own GUC
-- floor is 1 MB, which sits only just above the accumulator's fixed baseline, so
-- driving this through the real knob would need a corpus large enough to make the
-- whole regression run expensive. bm25_native.debug_budget is PGC_SUSET and is
-- documented as a test lever precisely so this file can exist; it is not a second
-- way to spell maintenance_work_mem.
--
-- DELIBERATELY NOT ASSERTED: how many segments a given budget buys. The per-entry
-- charges are sums of sizeof()s, so pinning a segment count would fail on any
-- platform whose struct padding differs from the CI pair's. Every assertion is
-- `> 1`, an equality against a control, or a count of rows.
CREATE EXTENSION bm25_native;

-- A corpus of a few hundred short documents with a large shared vocabulary: the
-- accumulator's residency is dominated by distinct terms and postings, so distinct
-- per-row filler is what makes a 64 kB budget reachable in a few hundred rows.
CREATE TABLE bmb (id int PRIMARY KEY, body text);
INSERT INTO bmb
SELECT i,
       'alpha bravo charlie document ' || i || ' word' || i || ' term' || (i % 37)
                                       || ' token' || (i % 53)
  FROM generate_series(1, 400) i;

SET enable_seqscan = off;

-- ------------------------------------------------------------- control build
-- Default budget: one accumulator, one segment. This is also the control the
-- chunked build's answers are compared against.
CREATE INDEX bmb_bm ON bmb USING bm25_native (body);
SELECT count(*) AS control_nsegs FROM bm25_debug_segcat('bmb_bm');
SELECT count(*) AS control_reltuples_match
  FROM pg_class WHERE relname = 'bmb_bm' AND reltuples = 400;

SELECT array_agg(id ORDER BY id) AS ids
  FROM bmb WHERE body @@@ 'alpha' \gset ctl_member_
WITH r AS (SELECT id FROM bmb WHERE body @@@ 'alpha'
             ORDER BY body &@@ 'alpha' LIMIT 25)
SELECT array_agg(id) AS ids FROM r \gset ctl_rank_

-- ------------------------------------------------------------ chunked REINDEX
-- REINDEX takes the ambuild path, so this is the same code CREATE INDEX runs.
SET bm25_native.debug_budget = '64kB';
REINDEX INDEX bmb_bm;
SELECT count(*) > 1 AS reindex_chunked FROM bm25_debug_segcat('bmb_bm');
-- Summed across chunks. Reading the last chunk's accumulator alone under-reported
-- index_tuples by every earlier chunk's docs, and that is what the index's
-- pg_class.reltuples is set from.
SELECT count(*) AS reindex_reltuples_match
  FROM pg_class WHERE relname = 'bmb_bm' AND reltuples = 400;
-- Every doc still live and countable.
SELECT sum(live_ndocs) AS reindex_live_docs FROM bm25_debug_segcat('bmb_bm');

-- Same answers as the control, membership and ranked order alike.
SELECT array_agg(id ORDER BY id) AS ids
  FROM bmb WHERE body @@@ 'alpha' \gset chunk_member_
WITH r AS (SELECT id FROM bmb WHERE body @@@ 'alpha'
             ORDER BY body &@@ 'alpha' LIMIT 25)
SELECT array_agg(id) AS ids FROM r \gset chunk_rank_
SELECT :'ctl_member_ids'::int[] = :'chunk_member_ids'::int[] AS membership_unchanged;
-- Single-key ORDER BY, no tiebreak column: a secondary sort key collapses the BM25
-- ranking onto that key and the comparison would then hold vacuously.
SELECT :'ctl_rank_ids'::int[] = :'chunk_rank_ids'::int[] AS ranked_order_unchanged;

-- A term confined to one chunk-worth of documents still resolves, i.e. the DICT of
-- the chunk that owns it is reachable through the multi-segment catalog.
SELECT array_agg(id ORDER BY id) AS narrow_term FROM bmb WHERE body @@@ 'word7';

-- ------------------------------------------------- chunked CREATE INDEX, keyed
-- A second index built from scratch under the budget, with a key_field: each chunk
-- must carry its own KEYMAP, or the rows in every chunk but the first silently
-- revert to ctid identity. bm25_score_key resolving a row's score BY KEY is what
-- proves the per-chunk keymeta re-init happened -- it reads ranked_keys, which is
-- populated only from a segment that actually wrote a KEYMAP.
CREATE INDEX bmb_key ON bmb USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
SELECT count(*) > 1 AS create_index_chunked FROM bm25_debug_segcat('bmb_key');
DROP INDEX bmb_bm;
-- Rows from the far end of the corpus, i.e. from a LATE chunk: a per-chunk keymeta
-- that was not re-applied leaves these with no key and a NULL score.
SELECT count(*) AS keyed_scores_present
  FROM (SELECT id FROM bmb WHERE body @@@ 'token40'
          ORDER BY body &@@ 'token40') s
 WHERE bm25_score_key(s.id) IS NOT NULL;

-- Post-build inserts land in the pending list and are visible before any seal, so
-- a chunked build has not disturbed read-your-writes.
RESET bm25_native.debug_budget;
INSERT INTO bmb VALUES (401, 'alpha bravo freshly inserted row');
SELECT array_agg(id ORDER BY id) AS after_insert
  FROM bmb WHERE body @@@ 'freshly';

DROP INDEX bmb_key;

-- ------------------------------------------------------- CONCURRENTLY variants
-- The only suite in the tree that builds CONCURRENTLY, and it is here because
-- chunking is what makes the question worth asking: ambuild now publishes segments
-- DURING the heap scan, so "can anything observe a half-built index" stops being
-- vacuously true. It is still safe, and by a mechanism rather than by luck --
-- ambuild runs in CIC phase 2 with indisready = false, so the planner ignores the
-- index and no backend runs aminsert into it; indisready flips only after
-- index_build returns. Asserted, not assumed: the index is valid and ready
-- afterwards, answers the corpus, and takes inserts.
--
-- The terms below are deliberately not stopwords. An earlier draft probed with
-- "after", which the english analyzer drops, and the empty result looked exactly
-- like a lost row.
SET bm25_native.debug_budget = '64kB';
CREATE INDEX CONCURRENTLY bmb_cic ON bmb USING bm25_native (body);
SELECT count(*) > 1 AS cic_chunked FROM bm25_debug_segcat('bmb_cic');
SELECT indisvalid, indisready FROM pg_index WHERE indexrelid = 'bmb_cic'::regclass;
SELECT sum(live_ndocs) AS cic_live_docs FROM bm25_debug_segcat('bmb_cic');
SELECT count(*) AS cic_rows FROM bmb WHERE body @@@ 'alpha';
INSERT INTO bmb VALUES (402, 'alpha bravo zebracrossing');
SELECT array_agg(id ORDER BY id) AS cic_after_insert
  FROM bmb WHERE body @@@ 'zebracrossing';
-- REINDEX CONCURRENTLY builds a new index OID through the same machinery.
REINDEX INDEX CONCURRENTLY bmb_cic;
SELECT count(*) > 1 AS ric_chunked FROM bm25_debug_segcat('bmb_cic');
SELECT count(*) AS ric_rows FROM bmb WHERE body @@@ 'alpha';
RESET bm25_native.debug_budget;
DROP INDEX bmb_cic;
RESET enable_seqscan;
DROP TABLE bmb;
DROP EXTENSION bm25_native;
