-- 12_meta_v4: a fresh index reports the current directory: this build's
-- format_version, 0 segs,
-- empty pending list, 0 live docs, default k1/b, and field_count == 1 (the M3
-- single-default-field invariant). bm25_build stamps the analyzer fingerprint onto
-- the metapage even for an EMPTY index, so it is populated (non-zero) here too.
CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, body text);
CREATE INDEX docs_bm25 ON docs USING bm25_native (body);  -- empty build
SELECT format_version, ndocs, nsegs, pending_ndocs, k1, b, field_count
  FROM bm25_stats('docs_bm25');
-- The fingerprint folds the database encoding (component 6), so its literal value is
-- environment-dependent; assert only that the empty build populated it. (It no longer
-- folds the english_stem dict OID -- ADR 0080 replaced that with a name-derived
-- identity -- but the encoding operand keeps a pinned integer unportable.)
SELECT analyzer_fingerprint <> 0 AS fingerprint_set FROM bm25_stats('docs_bm25');
DROP TABLE docs;
DROP EXTENSION bm25_native;
