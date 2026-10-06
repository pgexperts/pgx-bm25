-- 29_multifield_decl: M5 field-config machinery (C-FIELDCFG) — single-column
-- config/resolver/key_field/per-field-knob VALIDATION. Since C-BUILD (C2) landed
-- the field-aware ingest loop AND flipped amcanmulticol=true, a multicolumn CREATE
-- INDEX now SUCCEEDS (the per-field posting round-trip is asserted in
-- 30_field_postings; this suite keeps its single-column config-resolution focus).
CREATE EXTENSION bm25_native;
CREATE TABLE docs (id int primary key, title text, summary text, body text);

-- Multicolumn now SUCCEEDS (amcanmulticol=true after C2's field-aware ingest loop).
CREATE INDEX docs_mc ON docs USING bm25_native (title, body);
SELECT field_count FROM bm25_stats('docs_mc');
DROP INDEX docs_mc;

-- Single-column config: field_count 1, field_name == attname (M5 uses the column
-- name; M3 wrote ""), k1/b default to the metapage, boost defaults to 1.0.
CREATE INDEX docs_one ON docs USING bm25_native (body);
SELECT field_count FROM bm25_stats('docs_one');
SELECT field_id, field_name, k1, b, boost FROM bm25_debug_fieldcfg('docs_one');
DROP INDEX docs_one;

-- Per-field knobs resolve against the attname and land on the field's config
-- (single field here, but the same code path C-BUILD uses for N fields).
CREATE INDEX docs_knob ON docs USING bm25_native (body)
  WITH (boost_body = '2.5', k1_body = '1.5', b_body = '0.4');
SELECT field_name, k1, b, boost FROM bm25_debug_fieldcfg('docs_knob');
DROP INDEX docs_knob;

-- Per-field knob values are validated at CREATE INDEX (they bypass the reloption
-- validator, so bm25_resolve_fields is the trust boundary): non-numeric, b out of
-- [0,1], and negative k1 each ERROR rather than silently misparsing to 0/garbage.
\set VERBOSITY terse
CREATE INDEX docs_bad1 ON docs USING bm25_native (body) WITH (boost_body = 'notanumber'); -- ERROR
CREATE INDEX docs_bad2 ON docs USING bm25_native (body) WITH (b_body = '2.0');            -- ERROR
CREATE INDEX docs_bad3 ON docs USING bm25_native (body) WITH (k1_body = '-5');            -- ERROR
\set VERBOSITY default

-- key_field reloption: parses, resolves the key column type (int4 here), builds.
-- The key column is carried as an INCLUDE column (its type needs no bm25_native opclass);
-- the resolver matches key_field against the index attributes (key + included).
CREATE INDEX docs_k ON docs USING bm25_native (body) INCLUDE (id) WITH (key_field = 'id');
DROP INDEX docs_k;

-- A key_field column that is neither indexed nor INCLUDE'd ERRORs at build (its
-- value could not reach the build callback to populate the keymap).
\set VERBOSITY terse
CREATE INDEX docs_knotincl ON docs USING bm25_native (body) WITH (key_field = 'id');       -- ERROR
-- Unknown key_field column ERRORs at build (no such index attribute).
CREATE INDEX docs_kbad ON docs USING bm25_native (body) WITH (key_field = 'nope');        -- ERROR
\set VERBOSITY default

DROP TABLE docs;
DROP EXTENSION bm25_native;
