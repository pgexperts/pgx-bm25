-- 99_query_semantics: query and analyzer semantics that used to differ between paths (#151).
--
-- Every finding in that issue is a place where two things that should agree did not --
-- the index path vs the off-index filter, a builder vs its documentation, the analyzer
-- at index time vs at query time.
CREATE EXTENSION bm25_native;

CREATE TABLE qs (id int primary key, body text, title text);
INSERT INTO qs VALUES
  (1, 'error connection refused today', 'alpha report'),
  (2, 'unrelated prose here',           'beta report'),
  (3, 'connection established',         'gamma');
CREATE INDEX qs_bm ON qs USING bm25_native (body, title);
SET enable_seqscan = off;

-- ------------------------------------------------------------ SCAN-05
-- The field-scope split ran before the phrase parse and searched the WHOLE right-hand
-- side for a colon, so a colon inside a quoted phrase was read as a field name. A
-- perfectly ordinary phrase search over log text was rejected outright, with an error
-- naming a field the user never wrote:
--     ERROR: bm25: unknown search field ""error"
-- The colon search now stops at the first quote, which makes both forms follow from one
-- rule instead of one working by luck.
SELECT id FROM qs WHERE body @@@ '"error connection refused"' ORDER BY id;
SELECT id FROM qs WHERE body @@@ '"error: connection refused"' ORDER BY id;
-- field:"phrase" still resolves the field -- this is the form that used to work only
-- because its colon happened to come first.
SELECT id FROM qs WHERE body @@@ 'body:"error connection"' ORDER BY id;
-- Plain field:term is untouched, and an unknown field still errors.
SELECT id FROM qs WHERE body @@@ 'title:alpha' ORDER BY id;
\set VERBOSITY terse
SELECT id FROM qs WHERE body @@@ 'nosuch:alpha';
\set VERBOSITY default

-- ------------------------------------------------------------ QRY-05
-- bm25_term was behaviourally identical to bm25_match: both analyzed their value and
-- OR'd every token. So a "term" query for 'error refused' matched a document containing
-- only one of them -- OR semantics from a builder named for a single term, with
-- README documenting it as single-term. It now requires exactly one.
SELECT id FROM qs WHERE body @@@ bm25_term('body','connection') ORDER BY id;
\set VERBOSITY terse
-- Enforced on the single-leaf path (the text-equivalent shortcut) ...
SELECT id FROM qs WHERE body @@@ bm25_term('body','error refused');
-- ... and on the multi-leaf tree path, which is a different site.
SELECT id FROM qs WHERE body @@@ bm25_boolean(
  must => ARRAY[bm25_term('body','error refused'), bm25_match_terms('title','alpha')]);
\set VERBOSITY default
-- match still ORs, which is what it is for and what the name says.
SELECT id FROM qs WHERE body @@@ bm25_match_terms('body','error refused') ORDER BY id;
-- The check is on TOKENS, not on whitespace: a value that analyzes to one term is legal
-- however it is written, which is also why the error is analyzer-dependent.
SELECT id FROM qs WHERE body @@@ bm25_term('body','  connection  ') ORDER BY id;

-- ------------------------------------------------------------ SQL-12
-- bm25_match(text,text) evaluates with the english/default analyzer whatever the index's
-- reloptions say, because it receives two bare text values and cannot discover which
-- index covers its left argument. That is structural, not unfixed -- see the C function.
-- Pinned here so the divergence stays a KNOWN, TESTED property rather than a latent
-- surprise, which is the whole remit of this finding.
--
-- On an english index the two paths agree, which is the common case and the reason
-- erroring would be worse than documenting:
SELECT (SELECT count(*) FROM qs WHERE body @@@ 'connection')                       AS index_path,
       (SELECT count(*) FROM qs WHERE bm25_match(body, 'connection'))              AS filter_path;

-- On a NON-english index they can disagree. german_stem folds 'Verbindungen' to a stem
-- the english analyzer never produces, so the index finds it and the hard-coded filter
-- does not. This is the divergence, asserted rather than described.
CREATE TABLE qsde (id int primary key, body text);
INSERT INTO qsde VALUES (1, 'Verbindungen wurden abgelehnt'), (2, 'nichts hier');
CREATE INDEX qsde_bm ON qsde USING bm25_native (body) WITH (language = 'german');
SELECT count(*) AS german_index_path FROM qsde WHERE body @@@ 'verbindung';
SELECT count(*) AS english_filter_path FROM qsde WHERE bm25_match(body, 'verbindung');

-- ------------------------------------------------------------ TEXT-01
-- NOT a behaviour change, and this section records why. The finding proposed erroring
-- off-index "like the jsonb sibling"; bm25_distance's actual jsonb sibling is
-- bm25_distance_jsonb, which calls the SAME resolver and returns +infinity identically.
-- The function it was compared against is bm25_match_jsonb -- the @@@ FILTER, which
-- errors because a jsonb tree cannot be evaluated off-index at all. Different operators,
-- different obligations: &@@ is projected as a resjunk column on every ranked query and
-- must not error. Both fall-through routes are pinned as decisions in sql/50 and sql/87.
SET enable_seqscan = on;
SELECT DISTINCT (body &@@ 'connection') = 'Infinity'::float8 AS text_inf_off_index FROM qs;
SELECT DISTINCT (body &@@ bm25_term('body','connection')) = 'Infinity'::float8
         AS jsonb_inf_off_index FROM qs;
SET enable_seqscan = off;

-- ------------------------------------------------------------ TEXT-05 / TEXT-06
-- TEXT-06 changes what bm25_analyze STORES, so it rides BM25_ANALYZER_REVISION 3.
-- TEXT-05 does NOT, because it was attempted and REVERTED -- see below.
--
-- TEXT-05, NOW AT PARITY AND GUARDED (issue #184, ADR 0087, analyzer revision 5).
-- Position advances once per source RUN, matching core FTS's ts_parse.c: a
-- compound-splitting dictionary's lexemes SHARE their word's position, and a run's
-- repeated lexemes are deduplicated.  These assertions used to pin the divergence
-- deliberately, so the flip would have a place to land and a before/after to point at.
-- They now pin the parity, and the before/after is in this file's history.
--
-- WHY THIS TOOK THREE CHANGES, kept here because the failure it guards against is
-- invisible to every English corpus.  The doc-side fix was once written on its own and
-- REVERTED (ADR 0077): the query side expanded a phrase into one matcher term per query
-- LEXEME and bm25_phrase.c requires STRICTLY INCREASING document positions, so
-- co-positioning a run's lexemes made N slots unfillable and phrase queries over
-- compound words returned ZERO ROWS.  Measured, not theorised.  Core survives the same
-- layout because phraseto_tsquery compensates on the QUERY side.  The counterpart landed
-- first and inert (ADR 0085: a phrase slot is one source WORD), then run-count doclen
-- (ADR 0086), and only then this.
--
-- The parity is exact against to_tsvector on the same dictionary, term multiset and
-- position grouping alike:
--     to_tsvector('footballklubber')          -> 5 lexemes, all at :1
--     to_tsvector('footballklubber football') -> 'ball':1,2 'foot':1,2 'football':1,2
--                                                'footballklubber':1 'klubber':1
--
-- Two divergences REMAIN, named here so they are not later rediscovered as defects of
-- this work.  (1) Under stopwords=default a dropped stopword consumes no position here,
-- where core lets it consume one.  (2) A slot ORs all of a run's lexemes, where core ANDs
-- the lexemes of one variant and ORs the variants -- recall-only, never a lost match,
-- because nvariant is discarded when tokens are built.  Both are ADR 0087 scope calls.
CREATE TEXT SEARCH DICTIONARY isp_stem (
    TEMPLATE = ispell, DictFile = ispell_sample, AffFile = ispell_sample);

-- Single-lexeme runs: one position each. Matches core's spacing.
SELECT bm25_debug_analyze_positions('the quick brown fox','standard','none','english')
         AS pos_single_lexeme;
-- One run, five distinct lexemes, ONE position -- ispell emits 'klubber' twice and the
-- repeat is deduped, so this is core's five-lexeme tsvector exactly.
SELECT bm25_debug_tokenize('footballklubber','standard','none','isp') AS toks_compound;
SELECT bm25_debug_analyze_positions('footballklubber','standard','none','isp')
         AS pos_compound_parity;
-- Two multi-lexeme runs: two positions.  The second run's three lexemes are distinct,
-- so only the first run loses a token to dedup -- 5 + 3, not 6 + 3.
SELECT bm25_debug_analyze_positions('footballklubber football','standard','none','isp')
         AS pos_two_runs_parity;

-- THE HEADLINE REGRESSION GUARD.  Under the doc-side-only fix all three of these
-- returned ZERO ROWS; that is the failure ADR 0077 reverted, and no suite phrase-tested a
-- compound dictionary at the time, which is why 105 green suites said nothing.  They must
-- keep matching now that the flip has landed for real -- that is what the slot model buys.
--
-- One verdict CHANGES here, deliberately, and toward core.  '"footballklubber"' now also
-- matches document 2, whose text is the separate words 'football klubber': the phrase is
-- one slot, and any lexeme the run produced satisfies it, so document 2's 'football' does.
-- Core matches document 2 for the same query too, via its variant chain.  This is a recall
-- GAIN, and it is the visible face of the OR-group semantics -- the compensating cost is
-- named in ADR 0087.  The other two verdicts are unchanged, which is the point of pinning
-- all three: the flip moved exactly one of them.
CREATE TEXT SEARCH DICTIONARY pg_catalog.isp99_stem (
    TEMPLATE = ispell, DictFile = ispell_sample, AffFile = ispell_sample);
CREATE TABLE phc (id int primary key, body text);
INSERT INTO phc VALUES (1, 'they saw the footballklubber yesterday'),
                       (2, 'football klubber as separate words');
CREATE INDEX phc_bm ON phc USING bm25_native (body)
  WITH (language = 'isp99', stopwords = 'none');
SELECT id FROM phc WHERE body @@@ '"footballklubber"' ORDER BY id;
SELECT id FROM phc WHERE body @@@ '"footballklubber yesterday"' ORDER BY id;
SELECT id FROM phc WHERE body @@@ '"football klubber"' ORDER BY id;

-- TEXT-06: run splitting no longer consults LC_CTYPE, and the fingerprint now covers the
-- database encoding. The encoding component is what makes a dump/restore into a
-- differently-encoded database fail the gate instead of silently keeping terms that were
-- tokenized under different rules.
SELECT analyzer_fingerprint <> 0 AS fingerprint_present FROM bm25_stats('qs_bm');

RESET enable_seqscan;
DROP TABLE phc;
DROP TEXT SEARCH DICTIONARY pg_catalog.isp99_stem;
DROP TEXT SEARCH DICTIONARY isp_stem;
DROP TABLE qsde;
DROP TABLE qs;
DROP EXTENSION bm25_native;
