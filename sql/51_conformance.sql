-- 51_conformance — query-grammar conformance suite.
--
-- Proves every documented grammar form (docs/grammar-mapping.md)
-- maps to a native bm25_native call returning the expected id-set / ranking /
-- highlight, on a synthetic DISCRIMINATING corpus. No C change, no format change.
--
-- CONVENTIONS (see Global Constraints of the plan):
--   * Every @@@ / &@@ anchors on `title` (the first indexed column); field scope
--     comes from the jsonb tree's `field` arg, not the anchor.
--   * Membership assertions use array_agg(id ORDER BY id) = ARRAY[...] (rank-collapse
--     safe). The ranked-SEQUENCE assertions use a bare single-key ORDER BY ... &@@ ...
--     with NO secondary key.
--   * bm25_score_key / bm25_snippet / the &@@ distance read the ACTIVE scored scan slot,
--     so every such query carries ORDER BY title &@@ <same query>.
--   * Every expected stem below was confirmed with bm25_debug_tokenize; every expected
--     id-set is a property of the token placement in THIS corpus.
CREATE EXTENSION bm25_native;

-- ===========================================================================
-- Synthetic corpus. Each row's fields are engineered so a specific grammar form
-- is discriminating (see the per-block comments in later assertion groups):
--   boolean       : body_plain tort={1,3,5} battery={3,4} theft={5,6}
--   stemming      : body_plain negligence/negligently/negligent = {7,8,9} (one stem 'neglig')
--   wildcard judg : body_plain judge/judgment/judgement/judo = {10,11,12,13}
--   wildcard wom  : body_plain woman/women/wombat = {14,15,16}
--   per-field boost/fan-out : 'mandamus' in TITLE of 17, in BODY_PLAIN of 18 (only)
--   field scope   : 'estoppel' in TITLE of 19, SUMMARY of 20, BODY_PLAIN of 21 (only)
--   phrase vs bag : 'product liability' adjacent in 22; both words non-adjacent in 23
--   proximity     : 'proximate cause' at content-token gaps 0/1/1(rev)/2/7 in {24,25,26,27,28}
-- Filler tokens (docket/entry/record/notice/case/file/hearing/memo/filing/stub/matter/
-- general/procedural/exhibit) collide with NO query term.
CREATE TABLE conf_brief (id int PRIMARY KEY, title text, summary text, body_plain text);
INSERT INTO conf_brief VALUES
 (1,  'tort claim summary',        'docket entry',                'plaintiff alleges tort and seeks damages'),
 (2,  'contract dispute notice',   'record notice',               'defendant breached the contract terms'),
 (3,  'tort and battery matter',   'case file',                   'court reviews tort and battery claims here'),
 (4,  'battery assault case',      'hearing memo',                'defendant committed battery causing injury'),
 (5,  'tort theft hybrid claim',   'filing stub',                 'tort claim involving theft of trade secrets'),
 (6,  'theft and burglary report', 'matter record',               'defendant committed theft of property'),
 (7,  'negligence standard note',  'docket entry',                'the negligence standard applies today'),
 (8,  'negligent conduct memo',    'record notice',               'defendant acted negligently'),
 (9,  'negligent act filing',      'case file',                   'a negligent act by the defendant harmed the plaintiff'),
 (10, 'ruling by the judge',       'hearing memo',                'the judge issued a ruling'),
 (11, 'summary judgment order',    'filing stub',                 'summary judgment was granted'),
 (12, 'appeal judgement notice',   'matter record',               'the judgement on appeal stands'),
 (13, 'sports law curiosity',      'docket entry',                'a judo match unrelated to law'),
 (14, 'testimony record',          'record notice',               'the woman testified today'),
 (15, 'class action filing',       'case file',                   'several women filed suit'),
 (16, 'exhibit catalog',           'hearing memo',                'a wombat appeared in the exhibit'),
 (17, 'petition for mandamus',     'filing stub',                 'general docket entry only'),
 (18, 'routine scheduling order',  'matter record',               'the court denied the mandamus petition'),
 (19, 'estoppel doctrine memo',    'docket entry',                'no specific claim here'),
 (20, 'general filing notice',     'argument rests on estoppel',  'procedural background only'),
 (21, 'general notice two',        'procedural record',           'the defense raised estoppel late'),
 (22, 'liability filing',          'case file',                   'a product liability claim was filed'),
 (23, 'defective goods notice',    'record notice',               'liability for the defective product'),
 (24, 'causation memo one',        'hearing memo',                'the proximate cause of the harm'),
 (25, 'causation memo two',        'filing stub',                 'proximate legal cause was argued'),
 (26, 'causation memo three',      'matter record',               'the cause seemed proximate'),
 (27, 'causation memo four',       'docket entry',                'proximate factors and later the cause emerged'),
 (28, 'causation memo five',       'record notice',               'proximate issues remained pending before the panel finally reached the ultimate cause');

CREATE INDEX conf_bm25 ON conf_brief USING bm25_native (title, summary, body_plain)
  INCLUDE (id)
  WITH (key_field = 'id', language = 'english',
        boost_title = 5, boost_summary = 3, boost_body_plain = 1);
SELECT bm25_seal('conf_bm25');
SET enable_seqscan = off;

-- ---------------------------------------------------------------------------
-- SMOKE: the match predicate works and the corpus is loaded. A body-scoped
-- 'tort' term returns exactly {1,3,5}. (Full grammar coverage in later blocks.)
-- ---------------------------------------------------------------------------
SELECT array_agg(id ORDER BY id) = ARRAY[1,3,5] AS smoke_tort_body_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_term('body_plain','tort')
        ORDER BY title &@@ bm25_term('body_plain','tort'), id ) s;

-- ===========================================================================
-- (A) Default-fields FAN-OUT + per-field BOOST. A bare stemmed term with no
-- field scope fans out across all three indexed fields; the index's 5/3/1 boosts
-- weight the fan-out. 'mandamus' is in the TITLE of 17 and the BODY_PLAIN of 18
-- (and nowhere else), so:
--   * fan-out must return BOTH {17,18} -- proving it searches title AND body_plain,
--     not just one field (a single-field bug returns {17} or {18} alone).
--   * ranked single-key: doc 17 (title hit, boost 5) must OUTRANK doc 18 (body
--     hit, boost 1) -> sequence [17,18], NOT [18,17].
-- ===========================================================================
SELECT array_agg(id ORDER BY id) = ARRAY[17,18] AS fanout_finds_both_fields
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')])
        ORDER BY title &@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')]), id ) s;

-- ranked SEQUENCE (single-key, no secondary sort): title-boost wins -> [17,18].
SELECT array_agg(id) = ARRAY[17,18] AS boost_title_outranks_body
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')])
        ORDER BY title &@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')]) LIMIT 2 ) s;

-- Boost MAGNITUDE (final-review hardening): the title hit's score must exceed 3x the
-- body hit's -- a ratio only the live boost_title=5 produces (without the 5:1 field
-- boost, title vs body differ by field-length normalization alone, ~1.2x, far below 3x).
-- Pins the 5/3/1 boost magnitude, not just the [17,18] order. (Verified: 16.88 vs 2.94.)
SELECT max(s) > 3.0 * min(s) AS boost_magnitude_title_over_body
FROM ( SELECT bm25_score_key(id) AS s FROM conf_brief
        WHERE title @@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')])
        ORDER BY title &@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'),bm25_term('summary','mandamus'),bm25_term('body_plain','mandamus')]) ) s;

-- ===========================================================================
-- (B) FIELD SCOPE ti()/su()/te(). 'estoppel' sits in exactly one field per doc
-- (title:19, summary:20, body_plain:21). Each scoped term must return ONLY its
-- own doc -- a field-blind implementation would return {19,20,21} for all three.
-- ===========================================================================
SELECT array_agg(id ORDER BY id) = ARRAY[19] AS scope_title_only
FROM ( SELECT id FROM conf_brief WHERE title @@@ bm25_term('title','estoppel')
        ORDER BY title &@@ bm25_term('title','estoppel'), id ) s;
SELECT array_agg(id ORDER BY id) = ARRAY[20] AS scope_summary_only
FROM ( SELECT id FROM conf_brief WHERE title @@@ bm25_term('summary','estoppel')
        ORDER BY title &@@ bm25_term('summary','estoppel'), id ) s;
SELECT array_agg(id ORDER BY id) = ARRAY[21] AS scope_body_only
FROM ( SELECT id FROM conf_brief WHERE title @@@ bm25_term('body_plain','estoppel')
        ORDER BY title &@@ bm25_term('body_plain','estoppel'), id ) s;

-- ===========================================================================
-- (C) BOOLEAN matrix over body_plain. Stem facts (confirmed above):
--   tort={1,3,5}  battery={3,4}  theft={5,6}.  Doc 3 has BOTH tort+battery.
-- Three grammar forms, three DISTINCT hand-derived sets.
-- ===========================================================================
-- A AND NOT B : tort AND NOT battery -> {1,3,5} minus {3,4} = {1,5}.
-- DISCRIMINATING: doc 3 has both, so a must_not that failed to exclude it leaves 3 rows.
SELECT array_agg(id ORDER BY id) = ARRAY[1,5] AS bool_and_not_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_boolean(must=>ARRAY[bm25_term('body_plain','tort')], must_not=>ARRAY[bm25_term('body_plain','battery')])
        ORDER BY title &@@ bm25_boolean(must=>ARRAY[bm25_term('body_plain','tort')], must_not=>ARRAY[bm25_term('body_plain','battery')]), id ) s;

-- A OR B : tort OR battery -> {1,3,5} union {3,4} = {1,3,4,5}.
SELECT array_agg(id ORDER BY id) = ARRAY[1,3,4,5] AS bool_or_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_boolean(should=>ARRAY[bm25_term('body_plain','tort'),bm25_term('body_plain','battery')])
        ORDER BY title &@@ bm25_boolean(should=>ARRAY[bm25_term('body_plain','tort'),bm25_term('body_plain','battery')]), id ) s;

-- (A OR B) AND C : (tort OR battery) AND theft -> {1,3,4,5} intersect {5,6} = {5}.
-- DISCRIMINATING: a should-as-must misreading of the inner clause (require BOTH tort
-- AND battery: only doc 3) intersected with theft{5,6} gives {}, not {5}.
SELECT array_agg(id ORDER BY id) = ARRAY[5] AS bool_nested_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_boolean(must=>ARRAY[bm25_boolean(should=>ARRAY[bm25_term('body_plain','tort'),bm25_term('body_plain','battery')]), bm25_term('body_plain','theft')])
        ORDER BY title &@@ bm25_boolean(must=>ARRAY[bm25_boolean(should=>ARRAY[bm25_term('body_plain','tort'),bm25_term('body_plain','battery')]), bm25_term('body_plain','theft')]), id ) s;

-- ===========================================================================
-- (D) PHRASE vs bag-of-words. 'product liability' is ADJACENT (in order) in doc 22;
-- doc 23 has both words but NON-adjacent and reversed. The exact phrase (slop 0,
-- ordered) must return {22} only; the bag (must=[product,liability]) keeps both.
-- ===========================================================================
SELECT array_agg(id ORDER BY id) = ARRAY[22,23] AS bag_keeps_both
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_boolean(must=>ARRAY[bm25_term('body_plain','product'),bm25_term('body_plain','liability')])
        ORDER BY title &@@ bm25_boolean(must=>ARRAY[bm25_term('body_plain','product'),bm25_term('body_plain','liability')]), id ) s;
SELECT array_agg(id ORDER BY id) = ARRAY[22] AS phrase_excludes_nonadjacent
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_phrase('body_plain','product liability')
        ORDER BY title &@@ bm25_phrase('body_plain','product liability'), id ) s;

-- ===========================================================================
-- (E) PROXIMITY. 'proximate cause' at content-token gaps in {24,25,26,27,28}:
--   24 gap 0 (ordered)   25 gap 1 (ordered)   26 gap 1 (REVERSED)
--   27 gap 2 (ordered)   28 gap 7 (ordered)
-- Four nested-distinct sets isolate ordered-vs-unordered AND slop magnitude:
--   PRE/1 (slop 1, ordered)   -> {24,25}          (26 reversed excluded)
--   W/1   (slop 1, unordered) -> {24,25,26}       (adds the reversed doc)
--   W/5   (slop 5, unordered) -> {24,25,26,27}    (adds gap-2)
--   W/S   (slop 25, unordered)-> {24,25,26,27,28} (adds gap-7)
-- ===========================================================================
SELECT array_agg(id ORDER BY id) = ARRAY[24,25] AS pre1_ordered_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_phrase('body_plain','proximate cause',1,true)
        ORDER BY title &@@ bm25_phrase('body_plain','proximate cause',1,true), id ) s;
SELECT array_agg(id ORDER BY id) = ARRAY[24,25,26] AS w1_unordered_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_phrase('body_plain','proximate cause',1,false)
        ORDER BY title &@@ bm25_phrase('body_plain','proximate cause',1,false), id ) s;
SELECT array_agg(id ORDER BY id) = ARRAY[24,25,26,27] AS w5_unordered_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_phrase('body_plain','proximate cause',5,false)
        ORDER BY title &@@ bm25_phrase('body_plain','proximate cause',5,false), id ) s;
SELECT array_agg(id ORDER BY id) = ARRAY[24,25,26,27,28] AS ws_sentence_ok
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_phrase('body_plain','proximate cause',25,false)
        ORDER BY title &@@ bm25_phrase('body_plain','proximate cause',25,false), id ) s;

-- ===========================================================================
-- (F) WILDCARD truncation. Matched (lowercased) against the STEMMED dict, so:
--   judg* -> judge/judgment/judgement = {10,11,12}, NOT judo(13).
--   wom*n -> woman/women = {14,15}, NOT wombat(16).
-- Both Lexis `!` and Westlaw `*` map to our `*` glob (documented convention).
-- ===========================================================================
SELECT array_agg(id ORDER BY id) = ARRAY[10,11,12] AS wildcard_judg_ok
FROM ( SELECT id FROM conf_brief WHERE title @@@ bm25_wildcard('body_plain','judg*')
        ORDER BY title &@@ bm25_wildcard('body_plain','judg*'), id ) s;
SELECT array_agg(id ORDER BY id) = ARRAY[14,15] AS wildcard_womn_ok
FROM ( SELECT id FROM conf_brief WHERE title @@@ bm25_wildcard('body_plain','wom*n')
        ORDER BY title &@@ bm25_wildcard('body_plain','wom*n'), id ) s;

-- ===========================================================================
-- (G) INJECTION-SAFETY (§1c). A term whose VALUE is literally an operator/wildcard
-- is DATA, never parsed. Every subquery uses the RANKED (ORDER BY ... &@@ ...) form:
-- the bare @@@ filter path is priming-dependent on this index shape (returns 0 on a
-- cold scan until a scored scan has run in the session -- see the T10 gap finding),
-- so the ranked form is the reliable, deterministic way to materialize a match set.
--   * bm25_term('body_plain','judg*') treats 'judg*' as a literal token (the '*' is
--     stripped by the analyzer, not a glob) -> a STRICT SUBSET of the true expansion
--     bm25_wildcard('body_plain','judg*')={10,11,12}. If '*' were parsed as a glob by
--     the TERM builder, the two sets would be equal.
--   * bm25_term('body_plain','AND') searches the token 'and' (a stopword) -> no query
--     tokens; 'AND' is never read as a boolean operator (0 rows).
-- ===========================================================================
SELECT coalesce(term_set, '{}'::int[]) <@ wild_set
   AND coalesce(term_set, '{}'::int[]) <> wild_set
   AND coalesce(term_set, '{}'::int[]) = ARRAY[10] AS injection_star_is_data
FROM ( SELECT (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM conf_brief
                 WHERE title @@@ bm25_term('body_plain','judg*')
                 ORDER BY title &@@ bm25_term('body_plain','judg*')) s)     AS term_set,
              (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM conf_brief
                 WHERE title @@@ bm25_wildcard('body_plain','judg*')
                 ORDER BY title &@@ bm25_wildcard('body_plain','judg*')) s) AS wild_set ) t;
SELECT count(*) = 0 AS injection_and_is_stopword
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_term('body_plain','AND')
        ORDER BY title &@@ bm25_term('body_plain','AND') ) s;

-- ===========================================================================
-- (H) RELEVANCE SCORE. bm25_score_key(id) is the paradedb.score(id) analogue,
-- keyed by key_field='id', read inside the ordered scan.
--   * all matched scores are strictly POSITIVE.
--   * across the ranked result the score is STRICTLY DESCENDING. Asserted via the
--     &@@ distance projection (= -score, real per-row value post rank-collapse-fix):
--     distance is strictly INCREASING down the scored order. The negligence set
--     {7,8,9} has distinct doclens -> distinct scores -> a strict order exists.
-- ===========================================================================
SELECT bool_and(s > 0) AS scores_all_positive
FROM ( SELECT bm25_score_key(id) AS s FROM conf_brief
        WHERE title @@@ bm25_term('body_plain','negligence')
        ORDER BY title &@@ bm25_term('body_plain','negligence') ) t;

SELECT bool_and(dist > prev) AS distance_strictly_increasing
FROM ( SELECT title &@@ bm25_term('body_plain','negligence') AS dist,
              lag(title &@@ bm25_term('body_plain','negligence'))
                OVER (ORDER BY title &@@ bm25_term('body_plain','negligence')) AS prev
         FROM conf_brief WHERE title @@@ bm25_term('body_plain','negligence') ) w
WHERE prev IS NOT NULL;

-- ===========================================================================
-- (I) No-Sort ranked top-N. A single-key ORDER BY ... &@@ ... LIMIT n plans to
-- Limit -> Index Scan with NO Sort/Incremental Sort node (the index supplies the
-- order). COSTS OFF + enable_seqscan=off -> the chosen plan has no disabled node,
-- so this pin is portable on PG 17/18 (mirrors the proven 49_m6_acceptance idiom).
-- ===========================================================================
EXPLAIN (COSTS OFF)
SELECT id FROM conf_brief
 WHERE title @@@ bm25_term('body_plain','negligence')
 ORDER BY title &@@ bm25_term('body_plain','negligence') LIMIT 3;

-- ===========================================================================
-- (J) Query-time BOOST multiplies on top of the index's per-field boost. Base:
-- doc 17 (title:mandamus, index boost 5) outranks doc 18 (body:mandamus, boost 1)
-- -> [17,18] (proved in Task 3). Boosting the BODY clause 50x must FLIP the order
-- -> [18,17], proving bm25_boost applies query-time weight on the body leaf.
-- ===========================================================================
SELECT array_agg(id) = ARRAY[18,17] AS query_boost_flips_order
FROM ( SELECT id FROM conf_brief
        WHERE title @@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'), bm25_boost(50.0, bm25_term('body_plain','mandamus'))])
        ORDER BY title &@@ bm25_boolean(should=>ARRAY[bm25_term('title','mandamus'), bm25_boost(50.0, bm25_term('body_plain','mandamus'))]) LIMIT 2 ) s;

-- ===========================================================================
-- (K) SNIPPET highlight. bm25_snippet(field) highlights the ACTIVE scored scan's
-- query TERMS in the passed field's text. It reads the single-leaf term slot
-- (so->qterm), which a multi-leaf boolean/fan-out scan does NOT populate -> snippet
-- returns NULL under a boolean query (see the T10 gap finding). So this block uses
-- two SINGLE-LEAF scans on the mandamus docs (17 title-only / 18 body):
--   * term('body_plain','mandamus') -> doc 18: body has the hit -> a <mark>-wrapped
--     excerpt, bounded (original text, tags/ellipses stripped) by max_num_chars.
--   * term('title','mandamus')      -> doc 17: body LACKS the hit -> SQL NULL
--     (callers typically drop the row's highlight in this case).
-- Both snippet calls sit INSIDE the scan SELECT (slot active); each aggregated to ONE
-- deterministic row (order-independent, no secondary sort key).
-- ===========================================================================
SELECT bool_or(snip LIKE '%<mark>mandamus</mark>%')                      AS snippet_wraps_body_hit,
       bool_and(length(replace(replace(replace(snip20,
              '<mark>',''),'</mark>',''),'…','')) <= 20)                  AS snippet_within_budget
FROM ( SELECT bm25_snippet(body_plain) AS snip,
              bm25_snippet(body_plain,'<mark>','</mark>',20) AS snip20
         FROM conf_brief
        WHERE title @@@ bm25_term('body_plain','mandamus')
        ORDER BY title &@@ bm25_term('body_plain','mandamus') ) s;
SELECT bool_and(snip IS NULL) AS snippet_null_when_no_body_hit
FROM ( SELECT bm25_snippet(body_plain) AS snip FROM conf_brief
        WHERE title @@@ bm25_term('title','mandamus')
        ORDER BY title &@@ bm25_term('title','mandamus') ) s;

RESET enable_seqscan;
DROP TABLE conf_brief;
DROP EXTENSION bm25_native;
