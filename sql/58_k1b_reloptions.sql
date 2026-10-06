-- 58_k1b_reloptions: k1/b/boost are user-settable and LIVE.
-- Stanzas land with their mechanism (plan Tasks 1-3): (1) global WITH(k1,b)
-- + bm25_stats honesty; (3) value validation at ALTER/CREATE time; (2) live
-- ALTER (Task 2); (4) precedence (T1 CREATE-half, T2 live-half); (5) WAND
-- parity under non-default k1/b (Task 3); (6) fingerprint-gate regression.
\set VERBOSITY terse
CREATE EXTENSION IF NOT EXISTS bm25_native;

-- Corpus: distinct doc lengths so b (length norm) discriminates rank order;
-- 'alpha' tf-tiers so k1 (tf saturation) discriminates too.
CREATE TABLE kb (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO kb VALUES
  (1, 'alpha alpha alpha alpha beta'),
  (2, 'alpha beta'),
  (3, 'alpha ' || repeat('pad ', 40) || 'beta'),
  (4, 'alpha alpha ' || repeat('pad ', 20) || 'beta');
-- (1) Global reloptions: accepted, honest in bm25_stats, and effective.
CREATE INDEX kb_default ON kb USING bm25_native (body) INCLUDE (id) WITH (key_field='id');
SELECT bm25_seal('kb_default');
CREATE TABLE kb2 AS SELECT * FROM kb; ALTER TABLE kb2 ADD PRIMARY KEY (id);
CREATE INDEX kb_tuned ON kb2 USING bm25_native (body) INCLUDE (id)
  WITH (key_field='id', k1='0.9', b='0.0');
SELECT bm25_seal('kb_tuned');
SELECT k1, b FROM bm25_stats('kb_default');
SELECT k1, b FROM bm25_stats('kb_tuned');
SET enable_seqscan = off;
-- b=0.0 turns length normalization off: doc 3 (longest, one alpha) must rank
-- equal-tf docs by tf only; assert the two rankings DIFFER and the tuned one
-- matches the hand-computed b=0 order (1,4 tie-broken by ctid, then 2,3 by ctid).
SELECT array_agg(id) AS default_order FROM
  (SELECT id FROM kb  WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 4) s;
SELECT array_agg(id) AS tuned_order FROM
  (SELECT id FROM kb2 WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 4) s;
RESET enable_seqscan;
-- (3) Value validation fires at DDL time, not REINDEX time.
CREATE INDEX kb_bad ON kb USING bm25_native (body) WITH (k1_body='banana');  -- ERROR
CREATE INDEX kb_bad ON kb USING bm25_native (body) WITH (b_body='5.0');      -- ERROR
CREATE INDEX kb_bad ON kb USING bm25_native (body) WITH (k1='-1');           -- ERROR
CREATE INDEX kb_bad ON kb USING bm25_native (body) WITH (b='1.5');           -- ERROR
ALTER INDEX kb_default SET (k1_body = 'banana');                      -- ERROR
ALTER INDEX kb_default SET (k1_body = 'nan');                         -- ERROR
ALTER INDEX kb_default SET (k1_body = 'inf');                         -- ERROR
ALTER INDEX kb_default SET (b_body = '5.0');                          -- ERROR
ALTER INDEX kb_default SET (boost_body = '-2');                       -- ERROR
ALTER INDEX kb_default SET (store_positions_body = 'maybe');          -- ERROR
-- (2) LIVE ALTER: the headline. No REINDEX anywhere in this stanza.
SET enable_seqscan = off;
SELECT array_agg(id) AS before_alter FROM
  (SELECT id FROM kb WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 4) s;
ALTER INDEX kb_default SET (b_body = '0.0');
SELECT array_agg(id) AS after_alter FROM
  (SELECT id FROM kb WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 4) s;
ALTER INDEX kb_default RESET (b_body);
SELECT array_agg(id) AS after_reset FROM
  (SELECT id FROM kb WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 4) s;
-- (4) Precedence: CREATE-time half via the build stamp; live half via ranking.
CREATE TABLE kb3 AS SELECT id, body AS title, body FROM kb2; ALTER TABLE kb3 ADD PRIMARY KEY (id);
CREATE INDEX kb3_idx ON kb3 USING bm25_native (title, body) INCLUDE (id)
  WITH (key_field='id', k1='2.0', k1_title='0.5');
SELECT bm25_seal('kb3_idx');
SELECT field_name, k1, b FROM bm25_debug_fieldcfg('kb3_idx') ORDER BY field_name;
ALTER INDEX kb3_idx SET (k1_body = '0.5');   -- live per-field overriding the global
SET enable_seqscan = off;
-- with k1_title=0.5 and k1_body now 0.5, tf saturation is uniform again:
-- assert the ranked order equals the all-0.5 hand computation.
SELECT array_agg(id) AS live_precedence_order FROM
  (SELECT id FROM kb3 WHERE title @@@ 'alpha' ORDER BY title &@@ 'alpha' LIMIT 4) s;
-- The order alone is vacuous on this corpus: it happens to be {1,2,4,3} both
-- before AND after the ALTER (uniform k1 with b=0.75 keeps the same tf-driven
-- rank shape at k1=2.0 or k1=0.5). Pin the SCORE values too, so a regression
-- that silently reverted live per-field resolution (k1_body stuck at the
-- CREATE-time 2.0 instead of the live-ALTERed 0.5) fails here even though the
-- order check would not catch it.
SELECT id, round(bm25_score_key(id)::numeric, 6) AS score
FROM kb3 WHERE title @@@ 'alpha' ORDER BY title &@@ 'alpha' LIMIT 4;
RESET enable_seqscan;
-- (6) The analyzer fingerprint gate is untouched by the resolver refactor.
-- The gate's ERROR message embeds fingerprint hash integers that fold the
-- database encoding (component 6, not stable across environments -- see
-- sql/26_fingerprint_gate.sql), so pinning the raw message is unportable. Assert only the SQLSTATE, via that suite's DO-block
-- pattern: PERFORM the gated query, catch feature_not_supported, and RAISE
-- NOTICE with a fixed (hash-free) string so the gate firing is still proven.
ALTER INDEX kb_default SET (language = 'german');
DO $$
BEGIN
  PERFORM count(*) FROM kb WHERE body @@@ 'alpha';
  RAISE EXCEPTION 'gate did NOT fire (unexpected)';
EXCEPTION
  WHEN feature_not_supported THEN
    RAISE NOTICE 'gate fired: ERROR (feature_not_supported), as expected';
END $$;
ALTER INDEX kb_default RESET (language);
SELECT count(*) FROM kb WHERE body @@@ 'alpha';   -- fine again

-- (5) WAND == exhaustive under non-default k1/b, including across a live
-- ALTER. Pins the bound-safety argument (raw impact ingredients + shared
-- fcfg[]) instead of trusting it: if any path caches or bakes k1/b, the
-- two orders diverge here.
--
-- body's first 100 docs are "landmark" docs (sql/45's technique): heavy
-- repeats of the query terms so they fill the top-100 heap immediately and
-- raise theta before the uniform peripheral tail (g > 100, the plain
-- brief-shaped modulus formula) is walked. Checked empirically: the plain
-- formula alone (no landmarks) never drove theta high enough to prune
-- anything at this stanza's non-default k1/b (blocks_skipped =
-- deep_check_skips = 0) -- the M2b lesson that a parity test which never
-- prunes is vacuous applies here too, so the landmark rows were added.
CREATE TABLE kbw (id int PRIMARY KEY, title text, body text) WITH (autovacuum_enabled = off);
INSERT INTO kbw SELECT g,
  concat_ws(' ', CASE WHEN g % 40 = 0 THEN 'negligence' END,
                 CASE WHEN g % 25 = 0 THEN 'liability' END, 'brief'),
  CASE WHEN g <= 100 THEN
    concat_ws(' ', repeat('liability ', 8 + (g % 9)), repeat('negligence ', 4 + (g % 6)),
                   repeat('damages ', 8 + (g % 7)), repeat('pad ', g % 13))
  ELSE
    concat_ws(' ', CASE WHEN g % 4 = 0 THEN 'liability' END,
                   CASE WHEN g % 15 = 0 THEN 'negligence' END,
                   CASE WHEN g % 3 = 0 THEN 'damages' END, repeat('pad ', g % 11))
  END
FROM generate_series(1, 20000) g;
CREATE INDEX kbw_idx ON kbw USING bm25_native (body, title) INCLUDE (id)
  WITH (key_field='id', k1='2.0', b='0.3');
SELECT bm25_seal('kbw_idx');
SET enable_seqscan = off;
SET bm25_native.wand_top_k = 100;
CREATE TEMP TABLE wand_run AS
  SELECT id, row_number() OVER () rn FROM
    (SELECT id FROM kbw WHERE body @@@ 'negligence liability damages'
     ORDER BY body &@@ 'negligence liability damages' LIMIT 100) s;
SET bm25_native.wand_top_k = 0;
CREATE TEMP TABLE exh_run AS
  SELECT id, row_number() OVER () rn FROM
    (SELECT id FROM kbw WHERE body @@@ 'negligence liability damages'
     ORDER BY body &@@ 'negligence liability damages' LIMIT 100) s;
SELECT count(*) = 0 AS wand_parity_nondefault
  FROM (SELECT * FROM wand_run EXCEPT SELECT * FROM exh_run
        UNION ALL SELECT * FROM exh_run EXCEPT SELECT * FROM wand_run) d;
-- anti-neuter probe (M2b lesson, sql/43-45's discipline): passing parity
-- above doesn't prove the block-max skip mechanism ran -- a "WAND" that
-- always decodes every block would pass too. deep_check_skips > 0 witnesses
-- the deep check's own shallow-skip actually firing, at THESE non-default
-- k1=2.0/b=0.3 values (sql/43-45 only exercise the shipped default 1.2/0.75).
SELECT (deep_check_skips > 0 OR blocks_skipped > 0) AS pruned_at_nondefault_k1b
  FROM bm25_wand_stats('kbw_idx', 'negligence liability damages', 100);
ALTER INDEX kbw_idx SET (k1_body = '0.5');
-- same anti-neuter probe, post-ALTER: the live k1_body override must still
-- feed the WAND bound (not a build-time-only value), so pruning still fires.
SELECT (deep_check_skips > 0 OR blocks_skipped > 0) AS pruned_at_nondefault_k1b_post_alter
  FROM bm25_wand_stats('kbw_idx', 'negligence liability damages', 100);
SET bm25_native.wand_top_k = 100;
CREATE TEMP TABLE wand_run2 AS
  SELECT id, row_number() OVER () rn FROM
    (SELECT id FROM kbw WHERE body @@@ 'negligence liability damages'
     ORDER BY body &@@ 'negligence liability damages' LIMIT 100) s;
SET bm25_native.wand_top_k = 0;
CREATE TEMP TABLE exh_run2 AS
  SELECT id, row_number() OVER () rn FROM
    (SELECT id FROM kbw WHERE body @@@ 'negligence liability damages'
     ORDER BY body &@@ 'negligence liability damages' LIMIT 100) s;
SELECT count(*) = 0 AS wand_parity_after_live_alter
  FROM (SELECT * FROM wand_run2 EXCEPT SELECT * FROM exh_run2
        UNION ALL SELECT * FROM exh_run2 EXCEPT SELECT * FROM wand_run2) d;
-- prove the ALTER actually changed the ranking (guards stanza vacuity):
SELECT count(*) > 0 AS alter_changed_ranking
  FROM (SELECT * FROM wand_run EXCEPT SELECT * FROM wand_run2) d;
RESET bm25_native.wand_top_k; RESET enable_seqscan;
DROP TABLE kb, kb2, kb3, kbw;
DROP EXTENSION bm25_native;
