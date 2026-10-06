-- 28_nonenglish_recall: a non-english index must not silently drop matches.
--
-- REGRESSION GUARD for the recheck-analyzer bug. The @@@ operator's standalone
-- evaluator (bm25_match) has no index handle, so it tokenizes with the DEFAULT
-- english analyzer. The bm25_native index scan therefore must NOT route its recheck
-- through bm25_match: bm25_gettuple sets xs_recheck=false because the index match
-- (made with the index's OWN analyzer) is authoritative. Before that fix, a german
-- index matched 'arbeit' against a german-stemmed 'arbeiten' doc in the scan, then
-- the executor re-ran bm25_match with ENGLISH stemming ('arbeiten' != 'arbeit') and
-- silently dropped the row — a false negative invisible to every english test.
--
-- The discriminator is OID-independent and version-stable: german stems both
-- "arbeiten" and "arbeit" to "arbeit", while english keeps "arbeiten" distinct from
-- "arbeit" (verified: see 25_analyzer for the tokenizer determinism guarantee).
CREATE EXTENSION bm25_native;
CREATE TABLE de (id int primary key, body text);
INSERT INTO de VALUES
  (1, 'die leute arbeiten'),      -- "arbeiten" -> german "arbeit"; english keeps "arbeiten"
  (2, 'viel arbeit heute'),       -- "arbeit"   -> "arbeit" in BOTH analyzers (literal control)
  (3, 'das wetter ist schoen');   -- no "arbeit" stem -> must NOT match
CREATE INDEX de_bm25 ON de USING bm25_native (body) WITH (language = 'german');

-- The ranked &@@ path forces the bm25_native index scan (the analyzer path). Both the
-- inflected doc (1, "arbeiten") and the literal doc (2, "arbeit") share the german
-- stem "arbeit", so both must come back. With the english-recheck bug, row 1 would
-- be dropped and only {2} returned.
SET enable_seqscan = off;
SELECT id FROM de WHERE body @@@ 'arbeit' ORDER BY body &@@ 'arbeit', id;

-- Count form (the boolean @@@ path, also rechecked pre-fix): exactly the two
-- arbeit-stem docs, no false negative (row 1), no false positive (row 3).
SELECT count(*) AS arbeit_matches FROM de WHERE body @@@ 'arbeit';

-- The inflected-only doc (1) is reachable on its own: a query for the inflected
-- surface form "arbeiten" matches it (german stem equality), proving the recall is
-- the analyzer's, not a literal-substring fluke.
SELECT id FROM de WHERE body @@@ 'arbeiten' ORDER BY body &@@ 'arbeiten', id;

RESET enable_seqscan;
DROP TABLE de;
DROP EXTENSION bm25_native;
