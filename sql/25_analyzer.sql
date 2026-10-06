-- M3 analyzer regression: deterministic stemmed/stopword-filtered tokenization
-- and a stable, config-sensitive analyzer fingerprint. Pins EXACT token output
-- so a Snowball-dict change (a PG minor revising the stemmer) is caught here.
-- The fingerprint is verified via determinism + differentiation assertions only
-- (no pinned magic number) so the suite stays green wherever the database encoding
-- differs -- the encoding is fingerprint component 6. The stemmer component is no
-- longer the catalog OID of english_stem (ADR 0080); sql/101_stemmer_identity pins
-- that it is derived from the dictionary's qualified name and template instead.
CREATE EXTENSION bm25_native;

-- --- Tokenization: english/default (stopword drop + stemming) ---------------
-- "The"/"and" dropped as stopwords; "negligent"->"neglig"; "defendants"->"defend".
SELECT bm25_debug_tokenize('The negligent defendants and the plaintiff');
-- Inflections collapse to one stem (the @@@-match property: negligence ~ negligent).
SELECT bm25_debug_tokenize('negligence negligent negligently');
-- Case/punctuation normalized; bare numbers survive stemming.
SELECT bm25_debug_tokenize('Running, RAN, runs! 2024');

-- --- stopwords = none keeps stoplist words ----------------------------------
-- "the" appears in the output here even though it is normally a stopword.
SELECT bm25_debug_tokenize('The negligent defendants',
                           'standard', 'none', 'english');

-- --- Fingerprint: determinism (stable across calls) -------------------------
CREATE TABLE fp_docs (id int, body text);
CREATE INDEX fp_default ON fp_docs USING bm25_native (body);
CREATE INDEX fp_nostop  ON fp_docs USING bm25_native (body) WITH (stopwords = 'none');
SELECT bm25_debug_analyzer_fingerprint('fp_default')
     = bm25_debug_analyzer_fingerprint('fp_default') AS stable;

-- --- Fingerprint: sensitive to stopwords + language -------------------------
SELECT bm25_debug_analyzer_fingerprint('fp_default')
     <> bm25_debug_analyzer_fingerprint('fp_nostop') AS stopwords_differ;
CREATE INDEX fp_french ON fp_docs USING bm25_native (body) WITH (language = 'french');
SELECT bm25_debug_analyzer_fingerprint('fp_default')
     <> bm25_debug_analyzer_fingerprint('fp_french') AS language_differ;

-- --- Fingerprint: build-time == query-time (the gate's invariant) -----------
-- The build path stamps the analyzer fingerprint onto the metapage (even for this
-- empty index), so the stored value equals the fingerprint re-resolved from the
-- same reloptions at query time. This is exactly the equality the scan-start gate
-- checks (contract §I).
SELECT (bm25_stats('fp_default')).analyzer_fingerprint
     = bm25_debug_analyzer_fingerprint('fp_default') AS build_eq_query;

DROP TABLE fp_docs;
DROP EXTENSION bm25_native;
