-- 46_m6_boolean: M6 Task 3 (C-AST) — bm25_rescan's jsonb branch, single-leaf
-- MATCH/TERM equivalence to the (text,text) path. A single match/term-leaf
-- jsonb query must be indistinguishable from the equivalent text query on
-- every path (filter TIDs, ranked order, scores) -- this is what makes the
-- jsonb builders a drop-in query surface rather than a second code path.
--
-- Boolean/wildcard/phrase-tree cases land in later tasks (4/6/7/8); this file
-- keeps `bl` + `enable_seqscan=off` live at the bottom so Task 4 can insert
-- its boolean cases BEFORE the trailing RESET/DROP block.
CREATE EXTENSION bm25_native;
CREATE TABLE bl (id int, body text);
INSERT INTO bl SELECT g, (ARRAY['tort battery','theft burglary','tort theft','battery only'])[1+(g%4)]
FROM generate_series(1,400) g;
CREATE INDEX bl_bm25 ON bl USING bm25_native (body);
SELECT bm25_seal('bl_bm25');
SET enable_seqscan=off;
-- single match-leaf jsonb query must equal the text query bit-for-bit (same ids, same order)
SELECT (SELECT array_agg(id) FROM (SELECT id FROM bl WHERE body @@@ bm25_match_terms('body','tort') ORDER BY body &@@ bm25_match_terms('body','tort'), id LIMIT 50) a)
     = (SELECT array_agg(id) FROM (SELECT id FROM bl WHERE body @@@ 'tort'                    ORDER BY body &@@ 'tort', id LIMIT 50) b)
  AS jsonb_match_equals_text;   -- expect t

-- ===========================================================================
-- M6 Task 4 (C-EVAL): boolean must/should/must_not over the presence bitmask.
-- The `bl` fixture + enable_seqscan=off above are still live. Every case uses the
-- RANKED &@@ form (a bare @@@ <jsonb> boolean is the non-scoring filter path, wired
-- in Task 8). Expected id sets are hand-derived from (ARRAY[...])[1+(g%4)]:
--   g%4=0 'tort battery' | g%4=1 'theft burglary' | g%4=2 'tort theft' | g%4=3 'battery only'
-- so g%4=2 (the tort-without-battery rows) = generate_series(2,398,4) = {2,6,..,398}.
-- ===========================================================================

-- AND-NOT: tort AND NOT battery -> only 'tort theft' (g%4=2). DISCRIMINATING: an
-- evaluator that ignored must_not would ALSO return 'tort battery' (g%4=0), 200 rows.
SELECT array_agg(id ORDER BY id) = (SELECT array_agg(g ORDER BY g) FROM generate_series(2,398,4) g)
       AS and_not_ok
FROM ( SELECT id FROM bl
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')]), id ) s;   -- expect t

-- should-OR: theft OR burglary -> 'theft burglary' (g%4=1) + 'tort theft' (g%4=2) = 200.
SELECT count(*) AS should_or_count
FROM ( SELECT id FROM bl
        WHERE body @@@ bm25_boolean(should=>ARRAY[bm25_term('body','theft'),bm25_term('body','burglary')])
        ORDER BY body &@@ bm25_boolean(should=>ARRAY[bm25_term('body','theft'),bm25_term('body','burglary')]), id ) s;   -- expect 200

-- nested: (tort OR battery) AND theft -> only 'tort theft' (g%4=2). DISCRIMINATING: a
-- should-as-must reading (require tort AND battery in the inner clause) returns {}.
SELECT array_agg(id ORDER BY id) = (SELECT array_agg(g ORDER BY g) FROM generate_series(2,398,4) g)
       AS nested_ok
FROM ( SELECT id FROM bl
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_boolean(should=>ARRAY[bm25_term('body','tort'),bm25_term('body','battery')]), bm25_term('body','theft')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_boolean(should=>ARRAY[bm25_term('body','tort'),bm25_term('body','battery')]), bm25_term('body','theft')]), id ) s;   -- expect t

-- §10 ranking order: distinct-doclen side corpus so should-clause scores are strictly
-- ordered (no ties). id is INVERSE to doclen, so score-desc {4,3,2,1} differs from the
-- TID tie-break {1,2,3,4} -- proves the boolean path preserves SCORE order, not just the
-- set (shorter doc -> higher BM25 at tf 1). A neutered/tied scorer would give {1,2,3,4}.
CREATE TABLE r10 (id int, body text);
INSERT INTO r10 VALUES
 (1, 'alpha beta gamma delta'),
 (2, 'alpha beta gamma'),
 (3, 'alpha beta'),
 (4, 'alpha');
CREATE INDEX r10_bm25 ON r10 USING bm25_native (body);
SELECT bm25_seal('r10_bm25');
SELECT array_agg(id) = ARRAY[4,3,2,1] AS ranking_ok
FROM ( SELECT id FROM r10
        WHERE body @@@ bm25_boolean(should=>ARRAY[bm25_term('body','alpha')])
        ORDER BY body &@@ bm25_boolean(should=>ARRAY[bm25_term('body','alpha')]) ) s;   -- expect t
DROP TABLE r10;

-- ===========================================================================
-- M6 Task 6 (C-EVAL): a PHRASE leaf inside the boolean tree. Its terms score as OR
-- into acc (BM25F), but its leaf_bit is set ONLY for docs whose terms are ADJACENT
-- (the leaf's own field/slop/ordered), via the M4 positional recheck.
-- ===========================================================================
-- must=[ PHRASE 'tort battery', TERM 'tort' ] -> ONLY the adjacent-phrase rows
-- (g%4=0 'tort battery') = generate_series(4,400,4), 100 rows. DISCRIMINATING: a
-- phrase leaf that ignored adjacency (scored tort|battery as a bare OR, bit set on
-- any tort|battery hit) would ALSO admit the g%4=2 'tort theft' rows.
SELECT array_agg(id ORDER BY id) = (SELECT array_agg(g ORDER BY g) FROM generate_series(4,400,4) g)
       AS phrase_leaf_ok
FROM ( SELECT id FROM bl
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery'), bm25_term('body','tort')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery'), bm25_term('body','tort')]), id ) s;   -- expect t
SELECT count(*) AS phrase_leaf_count
FROM ( SELECT id FROM bl
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery'), bm25_term('body','tort')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery'), bm25_term('body','tort')]) ) s;   -- expect 100

-- A bare (BM25_FIELD_ALL) phrase leaf also works: bm25_phrase over the single 'body'
-- field resolves to that field, so the adjacency still restricts to g%4=0 (100 rows).
SELECT count(*) AS single_phrase_leaf_count
FROM ( SELECT id FROM bl
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery')]) ) s;   -- expect 100

-- A must_not (negated) PHRASE leaf is deliberately NOT wired (its non-idempotent
-- position stash cannot honor the pending-wins dedup a presence bit gets for free);
-- it must ERROR cleanly, never silently fail to exclude.
SELECT id FROM bl
 WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_phrase('body','tort battery')])
 ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_phrase('body','tort battery')]);   -- ERROR (must_not phrase)

-- Same reject, one level deeper: the phrase leaf is reached through an intermediate
-- 'should' node under must_not (depth>1), not a direct must_not child. negated=true
-- threads DOWN through every nested node under a must_not edge (bm25_query.c), and the
-- reject scans the FLATTENED leaf set, so this must ERROR identically -- proves the
-- guard isn't fooled by one extra layer of nesting.
SELECT id FROM bl
 WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')],
                              must_not=>ARRAY[bm25_boolean(should=>ARRAY[bm25_phrase('body','tort battery')])])
 ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')],
                              must_not=>ARRAY[bm25_boolean(should=>ARRAY[bm25_phrase('body','tort battery')])]);   -- ERROR (nested must_not phrase, depth>1)

-- Granular scaffolding: BOOLEAN/BOOST/MATCH/TERM (Task 4), positive PHRASE (Task 6),
-- and WILDCARD (Task 7) trees flow to the scorer; the bare @@@ (filter-only) boolean
-- tree is now wired too (Task 8) — see the D12 filter==rank block near the end.
-- Task 7: a WILDCARD leaf inside a boolean tree expands over the dict — 'tor*' -> {tort}
-- (the only 'tor'-prefixed stem in this corpus: tort/batteri/theft/burglari/onli) — so
-- should=>[tor*] returns exactly the docs matching the plain term 'tort'. Membership
-- idiom (array_agg over inner ORDER BY ..., id) => set-correct.
SELECT (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM bl WHERE body @@@ bm25_boolean(should=>ARRAY[bm25_wildcard('body','tor*')]) ORDER BY body &@@ bm25_boolean(should=>ARRAY[bm25_wildcard('body','tor*')]), id) a)
     = (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM bl WHERE body @@@ bm25_term('body','tort') ORDER BY body &@@ bm25_term('body','tort'), id) b)
  AS wildcard_in_boolean_eq_term;   -- expect t
-- Task 8: the bare @@@ should-OR filter now evaluates the tree (no ERROR). tort OR theft
-- = 'tort battery'(g%4=0) + 'theft burglary'(g%4=1) + 'tort theft'(g%4=2) = 300, excluding
-- 'battery only'(g%4=3). A neutered should-OR (match-all) would give 400.
SELECT count(*) = 300 AS bare_should_or_ok FROM bl WHERE body @@@ bm25_boolean(should=>ARRAY[bm25_term('body','tort'),bm25_term('body','theft')]);  -- expect t

-- ===========================================================================
-- M6 Task 6b (C-EVAL): regression locks for phrase-leaf behaviors verified live in
-- Task 6 review but never pinned to an expected file. Task 7/8 touch this same
-- scorer next, so these lock the boundary before more code lands on top of it.
-- ===========================================================================

-- --- (1) Two-phrase-leaf collision: separate stashes, no cross-leaf bleed ---------
-- Own small fixture (bl's corpus has no 'gamma'/'delta'). Tokens are plain literals
-- (alpha/beta/gamma/delta), already proven to survive the analyzer un-stemmed and
-- un-stopped in the r10 fixture above.
CREATE TABLE pl (id int, body text);
INSERT INTO pl VALUES
  (1, 'alpha beta'),           -- has phrase A ('alpha beta') only
  (2, 'gamma delta'),          -- has phrase B ('gamma delta') only
  (3, 'alpha beta gamma delta'),  -- has BOTH A and B, adjacent -- and 'beta gamma' too
  (4, 'alpha delta'),          -- neither phrase's terms adjacent (scattered noise)
  (5, 'beta gamma'),           -- has 'beta gamma' but no 'alpha' -- shared-term trap
  (6, 'gamma beta');           -- 'beta gamma' reversed -- ordered=true must reject it
CREATE INDEX pl_bm25 ON pl USING bm25_native (body);
SELECT bm25_seal('pl_bm25');

-- must=[A,B] (disjoint terms): ONLY doc 3 has both phrases adjacent. DISCRIMINATING:
-- if the two phrase leaves aliased onto one shared stash/leaf_bit, doc 1 or 2 (which
-- has only ONE of the two phrases) could spuriously read as satisfying both.
SELECT array_agg(id ORDER BY id) = ARRAY[3] AS collision_must_ok
FROM ( SELECT id FROM pl
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','alpha beta'), bm25_phrase('body','gamma delta')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','alpha beta'), bm25_phrase('body','gamma delta')]), id ) s;   -- expect t

-- should=[A,B]: any doc with EITHER phrase -> docs 1, 2, 3 (not 4/5/6, none of which
-- have either phrase adjacent).
SELECT array_agg(id ORDER BY id) = ARRAY[1,2,3] AS collision_should_ok
FROM ( SELECT id FROM pl
        WHERE body @@@ bm25_boolean(should=>ARRAY[bm25_phrase('body','alpha beta'), bm25_phrase('body','gamma delta')])
        ORDER BY body &@@ bm25_boolean(should=>ARRAY[bm25_phrase('body','alpha beta'), bm25_phrase('body','gamma delta')]), id ) s;   -- expect t

-- SHARED-TERM stress: must=[phrase('alpha beta'), phrase('beta gamma')] -- both leaves
-- contain 'beta', at a DIFFERENT within-phrase position each (qi=1 in the first, qi=0
-- in the second). Only doc 3 has both phrases adjacent. DISCRIMINATING: doc 5 ('beta
-- gamma', no 'alpha') is the aliasing trap -- if the two leaves shared one stash keyed
-- only by within-phrase index (not scoped per leaf group), 'beta'@qi0 (belonging to the
-- SECOND phrase) could bleed into the FIRST phrase's qi0 slot (which wants 'alpha'),
-- making the first phrase spuriously "adjacent" and pulling doc 5 into the result.
SELECT array_agg(id ORDER BY id) = ARRAY[3] AS shared_term_must_ok
FROM ( SELECT id FROM pl
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','alpha beta'), bm25_phrase('body','beta gamma')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','alpha beta'), bm25_phrase('body','beta gamma')]), id ) s;   -- expect t
DROP TABLE pl;

-- --- (2) Pending read-your-writes: post-seal UPDATE flips the phrase match ---------
-- Seal with doc 1 holding the phrase and doc 2 not; then swap their bodies (doc 1 loses
-- it, doc 2 gains it) via a post-seal UPDATE, which lands only in the pending list. The
-- phrase-leaf boolean must track the PENDING (current) content, not the stale sealed
-- posting -- this exercises position dedup across sealed+pending, the riskiest path in
-- the Task 6 phrase-leaf stash.
CREATE TABLE pu (id int, body text);
INSERT INTO pu VALUES
  (1, 'india juliet kilo'),   -- sealed: 'india juliet' adjacent -> matches
  (2, 'juliet kilo india');   -- sealed: 'india juliet' NOT adjacent -> doesn't match
CREATE INDEX pu_bm25 ON pu USING bm25_native (body);
SELECT bm25_seal('pu_bm25');

-- Pre-update sanity: only doc 1 matches straight off the sealed segment.
SELECT array_agg(id ORDER BY id) = ARRAY[1] AS pending_ryw_presealed_ok
FROM ( SELECT id FROM pu
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','india juliet')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','india juliet')]), id ) s;   -- expect t

-- Swap the bodies post-seal (pending-only, never resealed): doc 1 now lacks the
-- phrase, doc 2 now has it.
UPDATE pu SET body = 'juliet kilo india' WHERE id = 1;   -- breaks the phrase
UPDATE pu SET body = 'india juliet kilo' WHERE id = 2;   -- creates the phrase

-- DISCRIMINATING both ways: if pending-wins broke, either doc 1 would still show
-- (stale sealed posting wins) or doc 2 would still be absent (update never observed).
SELECT array_agg(id ORDER BY id) = ARRAY[2] AS pending_ryw_postupdate_ok
FROM ( SELECT id FROM pu
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','india juliet')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','india juliet')]), id ) s;   -- expect t
DROP TABLE pu;

-- --- (4) Positionless-index jsonb phrase leaf: clean ERROR, no silent adjacency miss -
-- store_positions=false (index-wide, D7 default 'error' fallback) mirrors the
-- (text,text) phrase path's degradation guard, now on the jsonb boolean path.
CREATE TABLE plnopos (id int, body text) WITH (autovacuum_enabled = off);
INSERT INTO plnopos VALUES (1, 'india juliet kilo');
CREATE INDEX plnopos_bm25 ON plnopos USING bm25_native (body) WITH (store_positions = false);
SELECT id FROM plnopos
 WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','india juliet')])
 ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','india juliet')]);   -- ERROR (no positions)
DROP TABLE plnopos;

-- ===========================================================================
-- M6 Task 8 (C-FILTER, D12): a bare @@@ <jsonb tree> filter (NON-scoring, no ORDER BY)
-- returns EXACTLY the ranked &@@ set. The @@@ filter path reuses the SAME exhaustive
-- membership computation the scorer runs (per-leaf presence mask + bm25_query_eval),
-- so @@@ == &@@ by construction: a must_not clause EXCLUDES its docs on the filter path
-- exactly as on the scored path (a flat-OR filter fallback would wrongly INCLUDE them).
-- The `bl` fixture + enable_seqscan=off from the top of the file are still live.
-- array_agg(id ORDER BY id) makes both sides SET-comparable despite the rank-collapse bug.
-- ===========================================================================

-- must_not is the discriminator: tort AND NOT battery. Bare-@@@ filter set == ranked set;
-- a flat-OR @@@ fallback would ALSO include 'tort battery' (g%4=0) and DIVERGE -> f.
SELECT (SELECT array_agg(id ORDER BY id) FROM bl WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')]))
     = (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM bl WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')])
          ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')]), id) s)
  AS filter_equals_rank;   -- expect t

-- Pin the must_not filter SET (so the equality above can't pass on two coincidentally
-- equal-but-wrong sets): the bare @@@ filter is exactly the tort-without-battery rows
-- (g%4=2) = {2,6,..,398}. A neutered/flat-OR must_not filter would give 200 rows.
SELECT array_agg(id ORDER BY id) = (SELECT array_agg(g ORDER BY g) FROM generate_series(2,398,4) g)
       AS filter_must_not_set_ok
FROM bl WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')]);   -- expect t

-- Phrase leaf on the filter path (D12, all leaf types covered): must=[phrase 'tort
-- battery'] -> adjacency-only 'tort battery' (g%4=0) = {4,8,..,400}. Bare @@@ set ==
-- ranked set, driving the Task 6 phrase gather+recheck through the filter-path reuse.
SELECT (SELECT array_agg(id ORDER BY id) FROM bl WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery')]))
     = (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM bl WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery')])
          ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_phrase('body','tort battery')]), id) s)
  AS filter_phrase_equals_rank;   -- expect t

-- ===========================================================================
-- Task 10 review follow-up: DELETE-tombstone gating for a jsonb must_not
-- clause. A must_not term whose ONLY holder is hard-deleted must simply
-- vanish from the result -- neither resurrected (a gating regression where
-- the tombstoned doc's presence bit for the must_not term wrongly still
-- reads "absent", flipping it to PASS the must_not filter) nor left
-- excluded-as-still-present in some other wrong way. Own small fixture;
-- DELETE + VACUUM per 17_delete.sql's pattern (VACUUM is what actually
-- retires the posting via the AM's own bulkdelete/tombstone callback -- a
-- bare DELETE alone leaves the posting present until vacuum runs).
-- ===========================================================================
CREATE TABLE mnd (id int, body text);
INSERT INTO mnd VALUES (1,'tort claim'),(2,'tort battery claim'),(3,'tort filing');
CREATE INDEX mnd_bm25 ON mnd USING bm25_native (body);
SELECT bm25_seal('mnd_bm25');
SET enable_seqscan = off;
-- Pre-delete: tort AND NOT battery -> {1,3} (doc 2 excluded, holds 'battery').
SELECT array_agg(id ORDER BY id) = ARRAY[1,3] AS must_not_predelete_ok
FROM ( SELECT id FROM mnd
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')]), id ) s;
RESET enable_seqscan;

DELETE FROM mnd WHERE id = 2;   -- the ONLY holder of 'battery' -- tombstone it, don't just rely on must_not
-- VACUUM hands bulkdelete only REMOVABLE tuples. Another backend in this database
-- holding an older snapshot (in installcheck, an autovacuum worker's ANALYZE) leaves
-- the rows deleted above "recently dead": bulkdelete never sees them and nothing is
-- tombstoned. Each VACUUM that depends on reclaiming them waits for no other backend
-- here to hold an xmin first; sql/17_delete documents the mechanism and why the wait
-- is sufficient, not just a narrower race.
CREATE FUNCTION pg_temp.wait_for_xmin_horizon() RETURNS void AS $$
DECLARE
  deadline timestamptz := clock_timestamp() + interval '30 seconds';
BEGIN
  LOOP
    PERFORM pg_stat_clear_snapshot();   -- else pg_stat_activity is cached per xact
    EXIT WHEN NOT EXISTS (
      SELECT 1 FROM pg_stat_activity
       WHERE datname = current_database()
         AND pid <> pg_backend_pid()
         AND backend_xmin IS NOT NULL);
    IF clock_timestamp() > deadline THEN
      RAISE EXCEPTION
        'xmin horizon still held by another backend after 30s; VACUUM cannot reclaim';
    END IF;
    PERFORM pg_sleep(0.01);
  END LOOP;
END
$$ LANGUAGE plpgsql;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM mnd;

SET enable_seqscan = off;
-- Post-delete: the tombstoned doc 2 must be gone entirely -- the surviving
-- set is EXACTLY the live matching rows {1,3}, unchanged from pre-delete.
-- DISCRIMINATING: if tombstone gating regressed, the dead doc's 'battery'
-- posting reading as absent would flip doc 2's must_not check to PASS,
-- resurrecting it into {1,2,3}.
SELECT array_agg(id ORDER BY id) = ARRAY[1,3] AS must_not_tombstone_ok
FROM ( SELECT id FROM mnd
        WHERE body @@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')])
        ORDER BY body &@@ bm25_boolean(must=>ARRAY[bm25_term('body','tort')], must_not=>ARRAY[bm25_term('body','battery')]), id ) s;
RESET enable_seqscan;
DROP TABLE mnd;

RESET enable_seqscan;
DROP TABLE bl;
DROP EXTENSION bm25_native;
