-- H1 (issue #41): the analyzer emits one token per LEXEME, but sized its token
-- array from the WORD-RUN count (textlen/2+1) and appended with no capacity
-- check. Any dictionary returning enough lexemes per run writes past the
-- allocation. The array now grows geometrically instead of trusting the estimate.
--
-- REACHABILITY (measured on PG 18.3 while fixing this; narrower than the report):
--   * `language` is a reloption resolved as "<language>_stem" through
--     get_ts_dict_oid, i.e. by search_path -- but PG 17+ runs CREATE INDEX with
--     search_path restricted to "pg_catalog, pg_temp", and non-relation catalog
--     lookups skip the temp namespace. So a build-time dictionary must live in
--     pg_catalog: superuser only. A dictionary in a user schema is NOT resolvable
--     at CREATE INDEX time even with that schema in the session search_path.
--     (#148 HDL-07 gave `language` a validator, which is why this no longer says
--     "unvalidated". It changes nothing here: it checks only that "<lang>_stem"
--     RESOLVES -- under that same restricted path -- never what the dictionary
--     DOES, so an ispell compound splitter still passes and still emits several
--     lexemes per run, which is the whole subject of this suite.)
--   * The debug tokenizer resolves under the caller's own search_path, but the
--     whole bm25_debug_* surface is REVOKE'd from PUBLIC (ADR 0020).
-- So this is a superuser/owner-reachable heap overflow, not an unprivileged one.
-- It is still a memory-safety defect on the build and insert paths, and the
-- guard is one branch on a path that already pallocs per token.
--
-- The shipped sample dictionaries top out near 0.33 lexemes per input byte
-- (footballklubber: 5 lexemes / 15 bytes) and the old estimate only overflows
-- above 0.5 -- 'ab cd ef' with a 2-lexeme-per-run dictionary (8 bytes, old cap 5,
-- 6 tokens) is the smallest overflowing shape. No shipped dictionary reaches that
-- ratio, so these cases pin the per-lexeme emit path rather than the overflow.
--
-- That ratio was 0.4 (6 lexemes / 15 bytes) until analyzer revision 5 added within-run
-- dedup (issue #184): ispell emits 'klubber' twice for this compound and the second
-- copy is now dropped.  The headroom argument is unchanged in kind -- dedup can only
-- LOWER the ratio, so a bound that held at 0.4 still holds at 0.33 -- but the number is
-- re-derived here rather than left to read as though nothing moved.
CREATE EXTENSION bm25_native;

-- ispell_sample is installed by core PostgreSQL (its own tsdicts suite uses it),
-- so this needs no extra fixture. ispell compound splitting is what makes one run
-- yield several lexemes.
CREATE TEXT SEARCH DICTIONARY isp_stem (
    TEMPLATE  = ispell,
    DictFile  = ispell_sample,
    AffFile   = ispell_sample
);

-- --- Debug tokenizer: one token per lexeme, not one per run -----------------
-- One 15-byte run -> 5 distinct lexemes.  Still one TOKEN per lexeme, which is what
-- this suite is about; what revision 5 changed is that they now share one POSITION
-- (pinned in sql/99) and that a run's repeated lexemes collapse to one.
SELECT bm25_debug_tokenize('footballklubber', 'standard', 'none', 'isp');

-- Two runs, both multi-lexeme: 5 + 3 = 8 tokens from 2 word runs.  The second run
-- ('football') is unaffected by dedup -- its three lexemes are distinct -- which is
-- why only the first run's count moved.
SELECT bm25_debug_tokenize('footballklubber football', 'standard', 'none', 'isp');

-- The token count tracks LEXEMES, not runs -- the invariant the old capacity
-- arithmetic got wrong.  A field's DOCLEN tracks RUNS instead, deliberately (ADR 0086),
-- and since analyzer revision 5 (ADR 0087) these are genuinely different numbers: this
-- text is 8 tokens and doclen 2.  They coincided until the flip only because positions
-- advanced per lexeme, and this suite's dictionary is exactly the one that separates
-- them -- so the two columns below are no longer the same quantity twice.
SELECT array_length(bm25_debug_tokenize('footballklubber football',
                                        'standard', 'none', 'isp'), 1) AS ntokens,
       8 AS expected_lexemes,
       2 AS word_runs;

-- Repeated appends across many multi-lexeme runs (5 lexemes x 64 runs).  Dedup is
-- scoped to ONE run, so 64 repetitions of the same word still emit 64 runs' worth of
-- tokens -- 320, not 5.  That is the assertion that would catch a dedup mistakenly
-- widened to document scope.
SELECT array_length(
         bm25_debug_tokenize(repeat('footballklubber ', 64),
                             'standard', 'none', 'isp'), 1) AS ntokens_64_runs;

-- --- Real build/insert path -------------------------------------------------
-- bm25_build and bm25_insert call the same analyzer. Installed in pg_catalog
-- because that is the only namespace CREATE INDEX can resolve on PG 17+ (see the
-- reachability note above); dropped again at the end of the suite.
CREATE TEXT SEARCH DICTIONARY pg_catalog.isp_stem (
    TEMPLATE  = ispell,
    DictFile  = ispell_sample,
    AffFile   = ispell_sample
);

CREATE TABLE ml_docs (id int PRIMARY KEY, body text);
INSERT INTO ml_docs VALUES (1, 'footballklubber'), (2, 'klubber alone'), (3, 'unrelated text');
CREATE INDEX ml_idx ON ml_docs USING bm25_native (body)
  WITH (language = 'isp', stopwords = 'none');

-- Doc 1 is reachable by each of the compound's parts: only true if every lexeme
-- of the run was emitted and stored.
SELECT array_agg(id ORDER BY id) AS by_foot     FROM ml_docs WHERE body @@@ 'foot';
SELECT array_agg(id ORDER BY id) AS by_ball     FROM ml_docs WHERE body @@@ 'ball';
SELECT array_agg(id ORDER BY id) AS by_football FROM ml_docs WHERE body @@@ 'football';
SELECT array_agg(id ORDER BY id) AS by_klubber  FROM ml_docs WHERE body @@@ 'klubber';

-- Same through the pending path (INSERT after build), not just the heap scan.
INSERT INTO ml_docs VALUES (4, 'footballklubber');
SELECT array_agg(id ORDER BY id) AS by_ball_after_insert FROM ml_docs WHERE body @@@ 'ball';

-- --- A seal must not move a score, ON THE ONE CORPUS THAT CAN TELL -----------
-- sql/16_pending_ryw makes the same assertion, but over an english corpus, where
-- a source word run and a token are always the same thing -- so it can only catch
-- a gross mistake, never the specific one this dictionary creates. HERE one run
-- yields several lexemes, which is exactly the input that separates a field's
-- LENGTH (its run count, ADR 0086) from its token count.
--
-- Doc 4 is unsealed and doc 1 is sealed, and they carry the byte-identical value,
-- so their scores must already agree; sealing doc 4 must then change nothing at
-- all. What would break it is a build where the analyzer co-positions a run's
-- lexemes (making runs and tokens differ) while the pending side still derives
-- doclen by summing tf: doc 4 would be scored as a 5-or-6-token document while
-- its own sealed self is a 1-run document. That is the failure this assertion
-- exists for, and no english suite in the tree can see it.
SET enable_seqscan = off;
CREATE TEMP TABLE ml_stats_before AS
  SELECT field_id, ndocs_field, total_len_field FROM bm25_debug_field_stats('ml_idx');
CREATE TEMP TABLE ml_before AS
  SELECT id, round(bm25_score(ctid)::numeric, 6) AS s
  FROM ml_docs WHERE body @@@ 'ball' ORDER BY body &@@ 'ball';
SELECT bm25_seal('ml_idx');
CREATE TEMP TABLE ml_stats_after AS
  SELECT field_id, ndocs_field, total_len_field FROM bm25_debug_field_stats('ml_idx');
CREATE TEMP TABLE ml_after AS
  SELECT id, round(bm25_score(ctid)::numeric, 6) AS s
  FROM ml_docs WHERE body @@@ 'ball' ORDER BY body &@@ 'ball';
-- The most legible form of the same requirement, and the one a user would notice
-- first: docs 1 and 4 carry the byte-identical value, one indexed by the BUILD and
-- one still sitting in the pending list. They must score the same BEFORE the seal,
-- not merely after it. Measured on a build with per-run positions but sum-of-tf
-- pending doclen, doc 4 scored 0.311209 against doc 1's 0.802591.
SELECT count(DISTINCT s) AS distinct_scores_before_seal FROM ml_before WHERE id IN (1, 4);
SELECT array_agg(id ORDER BY id) AS compound_matched_before FROM ml_before;
SELECT array_agg(id ORDER BY id) AS compound_matched_after  FROM ml_after;
SELECT count(*) AS compound_field_stats_that_moved
FROM ml_stats_before b FULL JOIN ml_stats_after a USING (field_id)
WHERE (b.ndocs_field, b.total_len_field) IS DISTINCT FROM (a.ndocs_field, a.total_len_field);
SELECT count(*) AS compound_scores_that_moved
FROM ml_before b FULL JOIN ml_after a USING (id)
WHERE b.s IS DISTINCT FROM a.s;
-- The built doc and the inserted doc are the same text, so they must also score
-- the SAME as each other -- the build/insert symmetry the length definition owes.
SELECT count(DISTINCT s) AS distinct_scores_for_identical_docs
FROM ml_after WHERE id IN (1, 4);
RESET enable_seqscan;

DROP TABLE ml_docs;
DROP TEXT SEARCH DICTIONARY pg_catalog.isp_stem;
DROP TEXT SEARCH DICTIONARY isp_stem;
DROP EXTENSION bm25_native;
