-- 42_format_v5.sql (renamed intent: the format-negotiation baseline, v6 onward)
-- A freshly built index carries this build's BM25_FORMAT_VERSION.
CREATE EXTENSION bm25_native;
CREATE TABLE fmt (id int primary key, body text);
INSERT INTO fmt VALUES (1,'alpha bravo'),(2,'bravo charlie');
CREATE INDEX fmt_bm25 ON fmt USING bm25_native (body);
SELECT format_version FROM bm25_stats('fmt_bm25');   -- expect this build's version

-- Negotiation baseline (v6 onward): min_read_version floor and the informational
-- feature_flags bitmap (roadmap #4 Task 2). Discriminating on both: swapping
-- the two columns or zeroing either would fail this (min_read_version is
-- never 0 once stamped, and WAND_IMPACTS -- bit 1<<2 == 4 -- is unconditional
-- since v6, so a zeroed feature_flags reads has_impacts = f).
SELECT min_read_version, (feature_flags & 4) <> 0 AS has_impacts
FROM bm25_stats('fmt_bm25');   -- expect 5, t  (WAND_IMPACTS always set since v6)
DROP TABLE fmt;

-- Forward gate (an index whose min_read_version exceeds this build is refused)
-- needs the extended debug poke added in Task 3; that sub-case lands in Task 4.

-- the v4->v5->v6 migration gate. A fresh index carries this build's version; stamping the metapage
-- back to an old format_version makes every read ERROR — the backward-refuse
-- message once that version falls below BM25_OLDEST_READABLE. bm25_debug_stamp_version
-- is a test-only helper that WAL-logs a forced format_version onto block 0 so we
-- exercise the reject path without shipping an old on-disk fixture.
CREATE TABLE fv5 (id int, body text);
INSERT INTO fv5 SELECT g, 'alpha bravo' FROM generate_series(1,20) g;
CREATE INDEX fv5_bm25 ON fv5 USING bm25_native (body);

-- Fresh index carries this build's BM25_FORMAT_VERSION
SELECT (bm25_stats('fv5_bm25')).format_version AS version;

-- Forge a v4 metapage (below BM25_OLDEST_READABLE); scans must demand REINDEX.
-- min_read_version=5 here is inert for this check (the backward gate keys off
-- format_version alone) but keeps the forward gate (min_read_version > this build) from
-- firing first, so the ERROR is unambiguously the backward-refuse one.
SELECT bm25_debug_stamp_version('fv5_bm25', 4, 5, 0);
\set VERBOSITY terse
SELECT (bm25_stats('fv5_bm25')).format_version AS version;  -- expect ERROR: ... format version 4; this build reads >= 5
\set VERBOSITY default

-- Boundary: a legacy v5 index (format_version == BM25_OLDEST_READABLE) is ACCEPTED,
-- REINDEX-free. A freshly built index still has min_read_version == 5, so stamping
-- format_version back to exactly 5 leaves the floor at 5: the two-directional gate
-- accepts it (5 is NOT < 5). This locks the exact accept boundary — it would fail if
-- the backward check were `<=` instead of `<`.
SELECT bm25_debug_stamp_version('fv5_bm25', 5, 5, 0);
SELECT (bm25_stats('fv5_bm25')).format_version AS version;  -- expect 5, accepted

-- Restore so DROP/cleanup works without tripping the gate
SELECT bm25_debug_stamp_version('fv5_bm25', 6, 5, 4);
SELECT (bm25_stats('fv5_bm25')).format_version AS version;

DROP TABLE fv5;

-- Block-impact round-trip (M2b Task 3): known per-doc tf/doclen must produce
-- known per-block (max_tf, min_doclen), proving Task 2's seal-time stamping
-- correct now that something finally reads it back.
--
-- Corpus chosen so every token survives the default english analyzer (no
-- stopwords, no stemming collisions) and doc lengths differ, so neither
-- max_tf nor min_doclen could pass by accident (verified against
-- bm25_debug_tokenize before writing these numbers down):
--   doc 1: 'alpha alpha alpha bravo charlie'
--     -> {alpha,alpha,alpha,bravo,charli}: doclen=5, tf(alpha)=3
--   doc 2: 'alpha delta echo foxtrot golf hotel india juliet kilo lima'
--     -> 10 distinct survivors: doclen=10, tf(alpha)=1
-- Both docs fit in ONE 128-posting block, so term 'alpha' -> one block, one
-- field (field 0, the sole default field): max_tf = max(3,1) = 3,
-- min_doclen = min(5,10) = 5.
CREATE TABLE imp (id int, body text);
INSERT INTO imp VALUES
  (1, 'alpha alpha alpha bravo charlie'),
  (2, 'alpha delta echo foxtrot golf hotel india juliet kilo lima');
CREATE INDEX imp_bm25 ON imp USING bm25_native (body);
SELECT bm25_seal('imp_bm25');
SELECT block_no, nfields, field_id, max_tf, min_doclen
FROM bm25_debug_block_impacts('imp_bm25', 'alpha') ORDER BY block_no, field_id;
DROP TABLE imp;

DROP EXTENSION bm25_native;
