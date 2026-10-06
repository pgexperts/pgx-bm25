-- 27_m3_acceptance: end-to-end M3 analyzer acceptance (spec §10, ROADMAP §3/§5).
-- One consolidated suite proving the ts_lexize Snowball analyzer (lowercase ->
-- stopword -> stemmer) is live at tokenize, build, and *real ranked-scan* query
-- time, and that the scan-start analyzer fingerprint gate (contract §I) fires on a
-- post-build analyzer edit. Unlike 26_fingerprint_gate (which exercises the gate
-- through the debug SRF), this suite drives the gate through the genuine BM25 index
-- scan: every gated query uses ORDER BY ... &@@, which the planner can ONLY satisfy
-- via the bm25_native index (a plain @@@ filter could be answered by a recheck on another
-- index, bypassing the analyzer path). All assertions are version-stable: the
-- emitted token text and the boolean equalities do not depend on the english_stem
-- catalog OID (which differs across PG 16/17/18), and the gate's OID-bearing
-- ERROR/WARNING message is never pinned -- it is caught or suppressed, and only the
-- BEHAVIOR (raised vs proceeded) is asserted.
CREATE EXTENSION bm25_native;

-- (1) Stemming + stopword removal on the default english analyzer. "The"/"and"/"of"
-- are Snowball english stopwords (dropped); "negligent"->"neglig", "defendants"->
-- "defend", "damages"->"damag". The token TEXT is OID-independent, so this pins the
-- exact analyzer output and would catch a PG minor that revises the Snowball dict.
SELECT bm25_debug_tokenize('The negligent defendants and the damages');

-- The four-arg overload pins analyzer/stopwords/language explicitly. stopwords =
-- 'none' keeps "the"/"and"; the stemmer still folds the content words.
SELECT bm25_debug_tokenize('The negligent defendants and the damages',
                           'standard', 'none', 'english');

CREATE TABLE cases (id int primary key, body text);
INSERT INTO cases VALUES
  (1, 'A finding of negligence against the defendant'),
  (2, 'The contract governs liability and indemnity'),
  (3, 'Negligent conduct caused the plaintiff''s damages');
-- Default reloptions: analyzer=standard, language=english, stopwords=default,
-- require_analyzer_match=true. The build bakes the english fingerprint onto the
-- metapage.
CREATE INDEX cases_bm25 ON cases USING bm25_native (body);

-- (2) The gate's core invariant for a freshly-built index: the fingerprint baked at
-- build (read back via bm25_stats) equals the fingerprint re-resolved from the
-- index's reloptions at query time. (Asserted as a boolean so no OID-bearing
-- integer is pinned.)
SELECT (bm25_stats('cases_bm25')).analyzer_fingerprint
       = bm25_debug_analyzer_fingerprint('cases_bm25') AS build_eq_query;

-- (3) A doc indexed with "negligence" matches a query for "negligent": both stem to
-- "neglig", so the dictionary lookup hits. The ORDER BY ... &@@ forces the bm25_native
-- index scan (the analyzer + gate path). Rows 1 (negligence) and 3 (Negligent) match;
-- row 2 (contract) does not.
SET enable_seqscan = off;
SELECT id FROM cases WHERE body @@@ 'negligent' ORDER BY body &@@ 'negligent', id;

-- (4) Round-trip: a query term tokenizes to the SAME analyzer token as the indexed
-- term. "Negligence", "negligent", "NEGLIGENTLY" all collapse to one stem regardless
-- of case/inflection -- the property that makes (3) match.
SELECT bm25_debug_tokenize('Negligence') = bm25_debug_tokenize('negligent')
       AS stem_roundtrip_equal;
SELECT bm25_debug_tokenize('NEGLIGENTLY') = bm25_debug_tokenize('negligence')
       AS case_and_stem_roundtrip_equal;

-- (5a) Fingerprint gate ERRORs by default on a REAL ranked scan after a post-build
-- analyzer edit. ALTER INDEX ... SET (language='german') changes the fingerprint
-- the scan re-resolves from reloptions, but NOT the english fingerprint baked on the
-- metapage -> mismatch. The ERROR message embeds fingerprint integers that fold the
-- database encoding (component 6) and so vary by environment, which is why we catch
-- feature_not_supported and assert only that the gate FIRED, never the integers.
ALTER INDEX cases_bm25 SET (language = 'german');
DO $$
BEGIN
  PERFORM id FROM cases WHERE body @@@ 'negligent' ORDER BY body &@@ 'negligent';
  RAISE EXCEPTION 'gate did NOT fire (unexpected)';
EXCEPTION
  WHEN feature_not_supported THEN
    RAISE NOTICE 'gate fired: ERROR (feature_not_supported), as expected';
END $$;

-- (5b) Under require_analyzer_match = false the gate downgrades to a WARNING and the
-- scan PROCEEDS instead of aborting. client_min_messages = error suppresses the
-- OID-volatile WARNING line. The scan now returns 0 rows: the german-stemmed query
-- token no longer matches the english-stemmed index tokens. That empty, best-effort
-- result is exactly WHY the gate defaults to ERROR -- a silent analyzer change
-- yields wrong answers, so the relaxed mode is opt-in.
ALTER INDEX cases_bm25 SET (require_analyzer_match = false);
SET client_min_messages = error;
SELECT id FROM cases WHERE body @@@ 'negligent' ORDER BY body &@@ 'negligent', id;
RESET client_min_messages;

-- Restore the matching analyzer; the gate passes again and the original ranking
-- returns with no warning.
ALTER INDEX cases_bm25 RESET (language);
ALTER INDEX cases_bm25 RESET (require_analyzer_match);
SELECT id FROM cases WHERE body @@@ 'negligent' ORDER BY body &@@ 'negligent', id;

RESET enable_seqscan;
DROP TABLE cases;
DROP EXTENSION bm25_native;
