CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
INSERT INTO docs VALUES (1, 'the quick brown fox');
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);
-- insert AFTER build: must be visible to subsequent scans
INSERT INTO docs VALUES (2, 'brown bear'), (3, 'quick rabbit');
SET enable_seqscan = off;
SELECT id FROM docs WHERE body @@@ 'brown' ORDER BY id;   -- 1,2
SELECT id FROM docs WHERE body @@@ 'quick' ORDER BY id;   -- 1,3
-- bm25_stats.ndocs is SEALED-ONLY (aminsert bumps only pending_ndocs); seal so the
-- post-build inserts (2,3) are drained into the segment and counted. The @@@ scorer
-- above reads pending directly, but ndocs/debug SRFs do not until Task 14, so the
-- Phase-1 suites seal before probing them (plan Task 7 Step 3).
SELECT bm25_seal('docs_bm25');
SELECT ndocs FROM bm25_stats('docs_bm25');                -- 3
-- read-your-writes within a txn
BEGIN;
INSERT INTO docs VALUES (4, 'brown table');
SELECT id FROM docs WHERE body @@@ 'brown' ORDER BY id;   -- 1,2,4
ROLLBACK;
SELECT id FROM docs WHERE body @@@ 'brown' ORDER BY id;   -- 1,2 again
RESET enable_seqscan;
-- NULL document: aminsert skips isnull[0]; NULL must not appear in any search
-- and ndocs must not increase.  Capture ndocs before and after the NULL insert
-- to verify the count is unchanged (independent of earlier rolled-back txns).
-- The rolled-back doc 4's pending posting persists (GenericXLog page mutations are
-- not MVCC-rolled-back, as in M1 where bm25_segment_add_doc bumped ndocs before the
-- abort); seal drains it, so ndocs counts it (4) — matching M1, where the recheck
-- still drops the dead TID from search results below.
SELECT bm25_seal('docs_bm25');
SELECT ndocs AS ndocs_before_null FROM bm25_stats('docs_bm25');
INSERT INTO docs VALUES (5, NULL);
SELECT ndocs AS ndocs_after_null  FROM bm25_stats('docs_bm25');   -- same as above
SET enable_seqscan = off;
SELECT id FROM docs WHERE body @@@ 'brown' ORDER BY id;   -- 1,2 only (no NULL)
RESET enable_seqscan;
-- Duplicate term within one document: df for 'apple' must increase by exactly 1
-- (per-document dedup), not 2.
INSERT INTO docs VALUES (6, 'apple apple banana');
-- bm25_debug_terms is segments-only (Phase 1); seal so the post-build-inserted doc 6
-- is drained and the stored term appears with df=1 (per-document dedup: tf=2 counts
-- as df 1). The stored term is the STEMMED form: 'apple' -> 'appl' via bm25_analyze.
SELECT bm25_seal('docs_bm25');
SELECT df FROM bm25_debug_terms('docs_bm25') WHERE term = 'appl';  -- 1
DROP TABLE docs;

-- v4: a row INSERTed after build must stem identically to a build-time row.
-- 'negligence' (inserted) must match the stemmed query 'negligent'.
CREATE TABLE idocs (id int primary key, body text);
INSERT INTO idocs VALUES (1, 'contract dispute');   -- build-time corpus
CREATE INDEX idocs_bm25 ON idocs USING bm25_native (body);
INSERT INTO idocs VALUES (2, 'gross negligence claim');  -- post-build insert path
SELECT bm25_seal('idocs_bm25');                     -- drain pending -> segment
SET enable_seqscan = off;
SELECT id FROM idocs WHERE body @@@ 'negligent' ORDER BY id;  -- expect: 2
RESET enable_seqscan;
DROP TABLE idocs;

DROP EXTENSION bm25_native;
