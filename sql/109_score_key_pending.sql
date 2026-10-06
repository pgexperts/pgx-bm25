-- 109_score_key_pending -- the score ACCESSORS must return a row's score whatever
-- shape the row is in: sealed segment or unsealed PENDING list for bm25_score_key
-- (#100, #204), and HOT-updated for bm25_score(ctid) (#204). Parts 1-4 cover the
-- key accessor, Part 5 the ctid accessor; they are one suite because the two failures
-- COMPOSE -- a row that was both pending and HOT-updated had no working accessor at
-- all, appearing in the result set, in the right order, with NULL from both.
--
-- What was wrong. A ranked row's user key was resolved only from the winning posting's
-- SEGMENT (src_hdr/gen/docid -> bm25_seg_key). A row scored by the pending scorer has no
-- segment source, so its key slot was left absent and bm25_score_key returned NULL --
-- while bm25_score(ctid) and bm25_snippet(), which key off the TID, worked on the very
-- same scan. Since a row is pending exactly between its INSERT and the next seal, the
-- rows with no score were reliably the NEWEST ones: an application that inserts and then
-- immediately searches got NULL for precisely the content it had just added.
--
-- Two further shapes of the same gap, both covered below:
--   * an index CREATEd on an empty table and then INSERTed into has no sealed segment
--     carrying a KEYMAP at all, so the whole scan took the keyless path and EVERY row
--     projected a NULL key, not just the pending ones;
--   * a pending row was still ENTERED into the by-key lookup under its absent (all-zero)
--     key slot, so a probe for key 0 -- a key no row in the index held -- resolved to
--     that pending row's score. int4/int8 keys are stored raw, so 0 is not an exotic
--     probe; it is what `WHERE id = 0` encodes to.
--
-- WHY THE ASSERTIONS COMPARE PRE-SEAL TO POST-SEAL rather than pinning score literals.
-- The contract is not "a pending row scores 0.127..."; it is "sealing is invisible to
-- the score". Pinning a float would pass just as well against a build that returned some
-- other number, and would have to be re-recorded whenever the ranking constants move.
-- Comparing the same rows across a bm25_seal() states the actual invariant and stays
-- portable across servers.
--
-- EVERY ranked query below is run under BOTH scorers. The key projection is implemented
-- twice -- once in the exhaustive builder, once in the WAND builder -- and the two are
-- near-identical code, so a fix landing in only one is the live hazard here. It is not
-- hypothetical: WAND's over-pull tail rebuild REPLACES a WAND ranking with an exhaustive
-- one mid-scan, so a one-sided fix changes its answer the moment an executor pulls past
-- wand_top_k. bm25_native.wand_top_k = 0 selects the exhaustive path.
CREATE EXTENSION IF NOT EXISTS bm25_native;

SET enable_seqscan = off;
SET enable_sort = off;

-- ---------------------------------------------------------------------------
-- Part 1. A pending row scores, and sealing does not change what it scores.
-- ---------------------------------------------------------------------------
CREATE TABLE sp_mix (id int PRIMARY KEY, body text, note text)
  WITH (autovacuum_enabled = off);
-- doc 1 is present at build time -> sealed segment.
INSERT INTO sp_mix VALUES (1, 'the defendant was negligent', 'x');
CREATE INDEX sp_mix_bm25 ON sp_mix USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'english');
-- docs 2 and 3 arrive after the build -> unsealed pending list.
INSERT INTO sp_mix VALUES (2, 'another negligent brief', 'x');
INSERT INTO sp_mix VALUES (3, 'a third negligent brief', 'x');

-- Every matching row must have a score, under both scorers, while still pending.
SELECT count(*) AS n_rows,
       count(*) FILTER (WHERE s IS NOT NULL) AS n_scored_wand
FROM (SELECT bm25_score_key(id) AS s FROM sp_mix
       WHERE body @@@ bm25_term('body', 'negligent')
       ORDER BY body &@@ bm25_term('body', 'negligent')) q;

SET bm25_native.wand_top_k = 0;
SELECT count(*) AS n_rows,
       count(*) FILTER (WHERE s IS NOT NULL) AS n_scored_exhaustive
FROM (SELECT bm25_score_key(id) AS s FROM sp_mix
       WHERE body @@@ bm25_term('body', 'negligent')
       ORDER BY body &@@ bm25_term('body', 'negligent')) q;
RESET bm25_native.wand_top_k;

-- The invariant: sealing is invisible to bm25_score_key.
CREATE TEMP TABLE sp_pre AS
SELECT id, bm25_score_key(id) AS s FROM sp_mix
 WHERE body @@@ bm25_term('body', 'negligent')
 ORDER BY body &@@ bm25_term('body', 'negligent');

SELECT bm25_seal('sp_mix_bm25');

CREATE TEMP TABLE sp_post AS
SELECT id, bm25_score_key(id) AS s FROM sp_mix
 WHERE body @@@ bm25_term('body', 'negligent')
 ORDER BY body &@@ bm25_term('body', 'negligent');

SELECT count(*) AS compared,
       bool_and(pre.s IS NOT NULL) AS all_pending_scored,
       bool_and(pre.s = post.s)    AS seal_is_invisible
  FROM sp_pre pre JOIN sp_post post USING (id);

-- ---------------------------------------------------------------------------
-- Part 2. An index with NO sealed keyed segment: the whole corpus is pending.
-- This is the ordinary shape of CREATE INDEX on a new table that is then loaded.
-- ---------------------------------------------------------------------------
CREATE TABLE sp_all (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
CREATE INDEX sp_all_bm25 ON sp_all USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'english');
INSERT INTO sp_all VALUES (1, 'alpha alpha bravo'), (2, 'alpha charlie');

SELECT bool_and(s IS NOT NULL) AS all_pending_index_scores_wand
FROM (SELECT bm25_score_key(id) AS s FROM sp_all
       WHERE body @@@ bm25_term('body', 'alpha')
       ORDER BY body &@@ bm25_term('body', 'alpha')) q;

SET bm25_native.wand_top_k = 0;
SELECT bool_and(s IS NOT NULL) AS all_pending_index_scores_exhaustive
FROM (SELECT bm25_score_key(id) AS s FROM sp_all
       WHERE body @@@ bm25_term('body', 'alpha')
       ORDER BY body &@@ bm25_term('body', 'alpha')) q;
RESET bm25_native.wand_top_k;

-- ---------------------------------------------------------------------------
-- Part 3. A pending row must not answer to a key it does not have.
-- No row here holds id = 0 and no key is NULL, so a probe for 0 has no owner.
-- ---------------------------------------------------------------------------
CREATE TABLE sp_zero (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO sp_zero VALUES (1, 'alpha alpha bravo');
CREATE INDEX sp_zero_bm25 ON sp_zero USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'english');
INSERT INTO sp_zero VALUES (2, 'alpha charlie');   -- pending

SELECT bool_and(probe IS NULL) AS absent_key_probe_is_null
FROM (SELECT bm25_score_key(0) AS probe FROM sp_zero
       WHERE body @@@ bm25_term('body', 'alpha')
       ORDER BY body &@@ bm25_term('body', 'alpha')) q;

-- ...and a row whose key genuinely IS 0 still scores. This is the other side of the
-- same coin: the fix must distinguish "no key" from "the key zero", not suppress both.
CREATE TABLE sp_realzero (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO sp_realzero VALUES (7, 'alpha bravo');
CREATE INDEX sp_realzero_bm25 ON sp_realzero USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'english');
INSERT INTO sp_realzero VALUES (0, 'alpha alpha charlie');   -- pending, key = 0

SELECT id, bm25_score_key(id) IS NOT NULL AS scored
  FROM sp_realzero
 WHERE body @@@ bm25_term('body', 'alpha')
 ORDER BY body &@@ bm25_term('body', 'alpha');

-- ---------------------------------------------------------------------------
-- Part 4. A KEYLESS index stays keyless. The pending list is now consulted for the
-- key config when no sealed segment carries one, so this pins that the consultation
-- reports "keyless" rather than inventing a key: bm25_score_key must still be NULL
-- and bm25_score(ctid) must still work.
-- ---------------------------------------------------------------------------
CREATE TABLE sp_keyless (id int, body text) WITH (autovacuum_enabled = off);
CREATE INDEX sp_keyless_bm25 ON sp_keyless USING bm25_native (body)
  WITH (language = 'english');
INSERT INTO sp_keyless VALUES (1, 'alpha bravo'), (2, 'alpha charlie');

SELECT bool_and(k IS NULL)     AS keyless_score_key_is_null,
       bool_and(c IS NOT NULL) AS keyless_ctid_still_scores
FROM (SELECT bm25_score_key(id) AS k, bm25_score(ctid) AS c FROM sp_keyless
       WHERE body @@@ bm25_term('body', 'alpha')
       ORDER BY body &@@ bm25_term('body', 'alpha')) q;

-- ---------------------------------------------------------------------------
-- Part 5. bm25_score(ctid) must survive a HOT UPDATE (#204, second half).
--
-- A HOT update writes the new tuple as a heap-only tuple on the same page and does
-- NOT call aminsert -- that is the point of HOT. So the index entry, and therefore
-- so->ranked[].tid, keeps pointing at the chain's ROOT, while the executor projects
-- the ctid of the tuple it actually fetched: the descendant. The two differ, and
-- every by-TID lookup missed. Sealing does not repair it, because sealing does not
-- change which TID the index holds -- which is what distinguishes this from Parts 1-4.
--
-- The UPDATE must target a NON-indexed column. An update to an indexed column is by
-- definition not a HOT update: it inserts a fresh index entry pointing at the new
-- tuple, so the TIDs agree and the bug is unreachable. sql/16_pending_ryw updates
-- `body`, which is indexed, which is exactly why it never caught this.
-- ---------------------------------------------------------------------------
CREATE TABLE sp_hot (id int PRIMARY KEY, body text, note text)
  WITH (autovacuum_enabled = off);
INSERT INTO sp_hot VALUES (1, 'the defendant was negligent', 'x'),
                          (2, 'another negligent brief', 'x');
CREATE INDEX sp_hot_bm25 ON sp_hot USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id', language = 'english');
SELECT bm25_seal('sp_hot_bm25');
UPDATE sp_hot SET note = 'hot' WHERE id = 1;     -- non-indexed column => HOT

SELECT count(*) AS n_rows,
       count(*) FILTER (WHERE c IS NOT NULL) AS n_scored_by_ctid,
       bool_and(c = k) AS ctid_agrees_with_key
FROM (SELECT bm25_score(ctid) AS c, bm25_score_key(id) AS k FROM sp_hot
       WHERE body @@@ bm25_term('body', 'negligent')
       ORDER BY body &@@ bm25_term('body', 'negligent')) q;

-- The decoupled projection: under ORDER BY ... LIMIT the target list and amgettuple
-- are decoupled, so the per-scan TID hash answers rather than the current-row
-- compare. Both paths need the HOT mapping; this pins the second one.
SELECT count(*) FILTER (WHERE c IS NOT NULL) AS n_scored_decoupled
FROM (SELECT bm25_score(ctid) AS c FROM sp_hot
       WHERE body @@@ bm25_term('body', 'negligent')
       ORDER BY body &@@ bm25_term('body', 'negligent') LIMIT 2) q;

-- A pending AND HOT-updated row: the shape that had no working accessor at all.
INSERT INTO sp_hot VALUES (3, 'a third negligent brief', 'x');
UPDATE sp_hot SET note = 'hot' WHERE id = 3;
SELECT bool_and(c IS NOT NULL) AS pending_and_hot_scores_by_ctid,
       bool_and(k IS NOT NULL) AS pending_and_hot_scores_by_key
FROM (SELECT bm25_score(ctid) AS c, bm25_score_key(id) AS k FROM sp_hot
       WHERE body @@@ bm25_term('body', 'negligent')
       ORDER BY body &@@ bm25_term('body', 'negligent')) q;

-- An arbitrary user-supplied tid stays a quiet NULL. The HOT mapping reads a heap
-- page, and ReadBuffer on an out-of-range block would EXTEND the relation -- turning
-- a read-only accessor into a writer -- so the bound is load-bearing, not cosmetic.
SELECT coalesce(bm25_score('(9999,1)'::tid)::text, 'NULL') AS out_of_range_block,
       coalesce(bm25_score('(0,99)'::tid)::text,   'NULL') AS out_of_range_offset
  FROM sp_hot WHERE body @@@ bm25_term('body', 'negligent')
 ORDER BY body &@@ bm25_term('body', 'negligent') LIMIT 1;

-- `(0,0)` is a legal value of type tid but NOT a valid ItemPointer -- ItemPointerIsValid
-- is false for offset 0. Every tid accessor asserts validity, so on an --enable-cassert
-- build (which the hardening CI leg is, and which gates) probing with it ABORTED THE
-- BACKEND instead of returning NULL. Reachable from plain SQL by any user. The rows
-- returned are irrelevant; that this statement completes at all is the assertion.
-- `(1,0)` is the same class with a different block: the validity guard must fire
-- BEFORE the block-range check, so an invalid offset is rejected whether or not its
-- block exists. Both are quiet NULLs.
SELECT coalesce(bm25_score('(0,0)'::tid)::text, 'NULL') AS invalid_itempointer,
       coalesce(bm25_score('(1,0)'::tid)::text, 'NULL') AS invalid_other_block
  FROM sp_hot WHERE body @@@ bm25_term('body', 'negligent')
 ORDER BY body &@@ bm25_term('body', 'negligent') LIMIT 1;

-- ...and with the scan NOT positioned on a row, which reaches a different accessor
-- (the exact-compare pass short-circuits, so the hash probe answers). This shape
-- aborted even before the HOT mapping existed.
BEGIN;
DECLARE sp_zc CURSOR FOR
  SELECT bm25_score(ctid) FROM sp_hot
   WHERE body @@@ bm25_term('body', 'zzznomatchzzz')
   ORDER BY body &@@ bm25_term('body', 'zzznomatchzzz');
FETCH 1 FROM sp_zc;
SELECT coalesce(bm25_score('(0,0)'::tid)::text, 'NULL') AS invalid_unpositioned,
       coalesce(bm25_score('(1,0)'::tid)::text, 'NULL') AS invalid_unpositioned_blk1;
COMMIT;

RESET enable_sort;
RESET enable_seqscan;
DROP TABLE sp_hot;
DROP TABLE sp_mix;
DROP TABLE sp_all;
DROP TABLE sp_zero;
DROP TABLE sp_realzero;
DROP TABLE sp_keyless;
DROP EXTENSION bm25_native;
