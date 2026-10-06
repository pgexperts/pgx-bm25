CREATE EXTENSION bm25_native;
CREATE TABLE gdocs (id int primary key, body text);
INSERT INTO gdocs VALUES (1, 'the negligent defendants'), (2, 'a quiet contract');

-- (1) Default analyzer index: a matching-fingerprint query works.
CREATE INDEX gdocs_en ON gdocs USING bm25_native (body) WITH (language = 'english');
SET enable_seqscan = off;
SELECT id FROM gdocs WHERE body @@@ 'negligent' ORDER BY id;   -- expect 1
RESET enable_seqscan;

-- The gate fires iff the supplied fp differs from the index's stored fp. The stored fp
-- folds the DATABASE ENCODING (component 6), so the (query N, index M) integers in the
-- gate message are environment-dependent and deliberately NOT asserted here. We assert
-- the matching path returns t, and the mismatch ERROR/WARNING BEHAVIOR (raised vs
-- returns f) without pinning the volatile integer. (The stemmer component used to be
-- the english_stem dict OID, which added a per-cluster source of variance on top;
-- ADR 0080 replaced it with a name-derived identity, and sql/101_stemmer_identity
-- covers that property.)

-- Matching fp (recomputed exactly as the scan-start gate does) => no fire, true.
SELECT bm25_debug_fingerprint_gate('gdocs_en',
         bm25_debug_analyzer_fingerprint('gdocs_en'));   -- expect t

-- (2) Mismatch under require_analyzer_match = true (the default) => ERROR.
-- Wrapped so only the deterministic SQLSTATE/message-prefix is asserted, never
-- the environment-dependent index fp integer (which the EXCEPTION handler discards).
CREATE INDEX gdocs_none ON gdocs USING bm25_native (body)
  WITH (language = 'english', stopwords = 'none', require_analyzer_match = true);
DO $$
BEGIN
  PERFORM bm25_debug_fingerprint_gate('gdocs_none', 0::bigint);  -- 0 != stored fp
  RAISE EXCEPTION 'gate did NOT fire (unexpected)';
EXCEPTION
  WHEN feature_not_supported THEN
    RAISE NOTICE 'gate fired: ERROR (feature_not_supported), as expected';
END $$;

-- (3) require_analyzer_match = false => WARNING, still proceeds (returns f).
-- client_min_messages = error suppresses the fingerprint-bearing WARNING line; the
-- returned mismatch flag (f) is the deterministic assertion.
CREATE INDEX gdocs_relaxed ON gdocs USING bm25_native (body)
  WITH (language = 'english', require_analyzer_match = false);
SET client_min_messages = error;
SELECT bm25_debug_fingerprint_gate('gdocs_relaxed', 0::bigint); -- WARNING suppressed, returns f
RESET client_min_messages;

DROP TABLE gdocs;
DROP EXTENSION bm25_native;
