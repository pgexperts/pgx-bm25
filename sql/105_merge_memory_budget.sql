-- 105_merge_memory_budget: BUILD-04, the merge half -- the one the issue is really
-- about, since bm25_merge_maybe runs from amvacuumcleanup and an OOM there is an
-- OOM in an AUTOVACUUM WORKER, a process with no user-visible failure path. The
-- merge re-accumulated every live doc of up to BM25_MERGE_MAX_INPUTS segments into
-- one BM25Accum before writing a single page; BM25_MERGE_MAX_INPUTS bounds the
-- number of segments, not the documents in them.
--
-- Three things are asserted here, and only the first is about memory:
--
--   (a) CHUNKING. A merge under a small budget consolidates the chosen set but
--       stops short of one segment. WITHOUT the fix a forced merge always collapses
--       to nsegs = 1, so `merged_nsegs > 1` returns f. That is the A/B tooth.
--
--   (b) ATOMICITY, indirectly. The N outputs and the removal of the N inputs are
--       ONE record. What a broken version would look like from SQL is not a missing
--       segment but a DOUBLED result: the exhaustive scorer sums per-TID
--       contributions across segments with no dedup, so a state in which a partial
--       output and its still-live inputs coexist returns each affected id twice on
--       the @@@ path. The membership arrays below are therefore compared for
--       EQUALITY against the pre-merge capture, not merely for containment -- an
--       array with a repeated id fails that comparison.
--
--   (c) TERMINATION. With outputs capped at the budget, merging a rung of
--       budget-sized segments can yield as many segments as it consumed; the
--       selector picks the same rung again and `bm25_merge()` -- which loops until a
--       pass finds nothing to merge -- never returns. The pre-fix symptom of
--       chunking-without-the-progress-guard is a SUITE TIMEOUT, not a diff, which is
--       why the repeated bm25_merge() calls below are an assertion and not scenery.
--
-- SCALE: shrink the budget, never grow the corpus (sql/83's doctrine).
--
-- CHOOSING THE BUDGET. Eight segments of fifty documents; the estimator's raw sum
-- puts each at ~55 kB (~86 kB before #305 shrank the accumulator's term-map entry),
-- so 256 kB is four or five segments' worth. Any budget strictly between
-- one segment and eight leaves the chunk count strictly between 1 and 8, which is
-- the ~3x margin in each direction this file needs -- deliberately not a value tuned
-- to land on a particular chunk count, since the real cut uses MEASURED residency
-- (which runs above the estimate, because the estimate charges data and not
-- allocator slack) and both numbers move with struct padding.
--
-- DELIBERATELY NOT ASSERTED: the number of output segments, or any est_bytes value.
--
-- PARTLY COVERED, stated rather than left to be discovered: the accepted-overshoot
-- WARNING, raised when a SINGLE input segment alone exceeds the budget. The
-- selection trim refuses an infeasible set outright, so the only merge that reaches
-- the overshoot is the lone tombstoned segment the trim deliberately exempts. The
-- shed-order section at the end of this file constructs exactly that state and does
-- reach it deterministically -- but it SUPPRESSES the message rather than pinning it,
-- because the DETAIL names a generation number and the section is about reclamation,
-- not wording. So the overshoot PATH is exercised; its TEXT is not asserted anywhere.
-- bm25_merge_rewrite_all (bm25_upgrade) is the other route and does not trim at all.
CREATE EXTENSION bm25_native;

CREATE TABLE bmm (id int PRIMARY KEY, body text);
SET enable_seqscan = off;

-- Eight separate segments, built with the budget OFF so each is a single unchunked
-- seal: insert a slice, seal it, repeat. bm25_seal drains the pending list into one
-- segment, so the segment count here is exactly the number of rounds. Eight is also
-- what makes the size-layer ladder fire (BM25_MERGE_LAYER_FANOUT is 4).
CREATE INDEX bmm_bm ON bmm USING bm25_native (body);
DO $$
DECLARE i int;
BEGIN
  FOR i IN 0..7 LOOP
    INSERT INTO bmm SELECT g, 'alpha bravo doc ' || g || ' w' || g || ' t' || (g % 31)
      FROM generate_series(i*50+1, i*50+50) g;
    PERFORM bm25_seal('bmm_bm');
  END LOOP;
END $$;
SELECT count(*) AS premerge_nsegs FROM bm25_debug_segcat('bmm_bm');

SELECT array_agg(id ORDER BY id) AS ids
  FROM bmm WHERE body @@@ 'alpha' \gset pre_member_
WITH r AS (SELECT id FROM bmm WHERE body @@@ 'alpha'
             ORDER BY body &@@ 'alpha' LIMIT 25)
SELECT array_agg(id) AS ids FROM r \gset pre_rank_

-- --------------------------------------------------------- (a) chunked merge
SET bm25_native.debug_budget = '256kB';
SELECT bm25_merge('bmm_bm');
-- Strictly between 1 and the pre-merge count: consolidation happened, but the
-- budget stopped it collapsing to a single segment.
SELECT count(*) > 1 AND count(*) < 8 AS merge_chunked
  FROM bm25_debug_segcat('bmm_bm');
-- Every document survives the merge exactly once.
SELECT sum(live_ndocs) AS merged_live_docs FROM bm25_debug_segcat('bmm_bm');

-- ------------------------------------------------------- (b) answers unchanged
SELECT array_agg(id ORDER BY id) AS ids
  FROM bmm WHERE body @@@ 'alpha' \gset post_member_
WITH r AS (SELECT id FROM bmm WHERE body @@@ 'alpha'
             ORDER BY body &@@ 'alpha' LIMIT 25)
SELECT array_agg(id) AS ids FROM r \gset post_rank_
-- Equality, not containment: a doc published in a partial output while its input
-- segment is still live appears TWICE in this array.
SELECT :'pre_member_ids'::int[] = :'post_member_ids'::int[] AS membership_unchanged;
SELECT array_length(:'post_member_ids'::int[], 1) AS matched_rows;
-- Single-key ORDER BY, no tiebreak column. Scores depend on corpus-wide statistics,
-- which a double-counted doc would also disturb, so this is a second, independent
-- witness that nothing was published twice.
SELECT :'pre_rank_ids'::int[] = :'post_rank_ids'::int[] AS ranked_order_unchanged;
-- A term confined to one input segment still resolves through the merged catalog.
SELECT array_agg(id ORDER BY id) AS narrow_term FROM bmm WHERE body @@@ 'w377';

-- ---------------------------------------------------------- (c) termination
-- Repeated forced merges over a steady state that is ALREADY at the budget floor.
-- With chunking but no progress guard, the first of these does not return.
SELECT bm25_merge('bmm_bm');
SELECT count(*) > 1 AS still_chunked FROM bm25_debug_segcat('bmm_bm');
SELECT bm25_merge('bmm_bm');
SELECT sum(live_ndocs) AS steady_state_live_docs FROM bm25_debug_segcat('bmm_bm');

-- --------------------------------------------------- the selection trim, direct
-- Every segment carries a positive estimate, and the trim only ever removes.
-- Values are never printed -- est_bytes is sizeof-derived -- and neither is the
-- segment count, which follows measured residency (#305 moved it from 3 to 2).
SELECT count(*) > 0 AND bool_and(est_bytes > 0) AS every_entry_has_estimate
  FROM bm25_debug_merge_budget_plan('bmm_bm');
SELECT count(*) FILTER (WHERE kept) <= count(*) FILTER (WHERE chosen) AS trim_never_adds
  FROM bm25_debug_merge_budget_plan('bmm_bm');
-- A budget below a single segment makes every set infeasible, so the trim refuses
-- the whole ladder pick rather than choosing work that cannot reduce the count --
-- which is what stops bm25_merge() spinning on it.
SET bm25_native.debug_budget = '8kB';
SELECT count(*) FILTER (WHERE kept) AS kept_under_tiny_budget
  FROM bm25_debug_merge_budget_plan('bmm_bm');
SELECT bm25_merge('bmm_bm');
SELECT sum(live_ndocs) AS tiny_budget_live_docs FROM bm25_debug_segcat('bmm_bm');

-- ------------------------------------------------- chunking follows the budget
-- Refill the ladder with five fifty-document segments and merge with the budget OFF:
-- the refilled rung merges into ONE output, so the extra segments above are a
-- consequence of the budget and not a permanent change of behaviour. Asserted as "at
-- most one more segment than before the refill" rather than as a count: what the
-- ladder then does with that output depends on the sizes the budgeted sections left
-- behind, which follow measured residency. (This used to read "collapses to one
-- segment"; that was the ladder cascading on those sizes, and #305's smaller
-- accumulator entries changed them.)
SELECT count(*) AS n FROM bm25_debug_segcat('bmm_bm') \gset pre_refill_
RESET bm25_native.debug_budget;
DO $$
DECLARE i int;
BEGIN
  FOR i IN 8..12 LOOP
    INSERT INTO bmm SELECT g, 'alpha bravo doc ' || g || ' w' || g || ' t' || (g % 31)
      FROM generate_series(i*50+1, i*50+50) g;
    PERFORM bm25_seal('bmm_bm');
  END LOOP;
END $$;
SELECT bm25_merge('bmm_bm');
SELECT count(*) <= :pre_refill_n + 1 AS refill_rung_unchunked
  FROM bm25_debug_segcat('bmm_bm');
SELECT count(*) AS unbudgeted_rows FROM bmm WHERE body @@@ 'alpha';

-- ----------------------------------------- no rewrite when nothing can be gained
-- The estimator predicts the quantity the CUT measures, and this is what goes wrong
-- when it does not. bm25_accum_over_budget reads MemoryContextMemAllocated, i.e.
-- malloc'd blocks including allocator slack; the estimate is a sum of sizeof()s, so
-- the raw sum runs about 1.6x low. Left on that scale, a ladder of budget-floored
-- segments looks HALF as expensive as it is: the trim predicts half the chunks,
-- declares an infeasible set feasible, and the merge runs -- reading, rewriting and
-- retiring every segment before bm25_merge_execute computes progress and discovers
-- it gained nothing. On every autovacuum, forever, roughly doubling the index's
-- physical size until the retire horizon clears. That is the exact regime the trim
-- exists to prevent, so the estimate carries a slack factor.
--
-- WITHOUT it the generations below CHANGE on every call, because every call rewrote
-- every segment. Sixteen segments, not eight, because the ladder must fire
-- (BM25_MERGE_LAYER_FANOUT is 4) on a set the budget cannot consolidate.
--
-- 96 kB, where this was written at 128 kB. The budget has to sit between the raw sum
-- (which predicts the set feasible) and the slack-scaled estimate (which does not), or
-- this section stops testing the slack factor. #305 shrank the accumulator's per-term
-- map entry from 260 bytes to 24, which moved both: at 128 kB the trim now keeps the
-- set and the merge really does consolidate it (16 segments to 6), which is correct.
CREATE TABLE bmn (id int PRIMARY KEY, body text);
CREATE INDEX bmn_bm ON bmn USING bm25_native (body);
DO $$
DECLARE i int;
BEGIN
  FOR i IN 0..15 LOOP
    INSERT INTO bmn SELECT g, 'alpha bravo doc ' || g || ' w' || g || ' t' || (g % 31)
      FROM generate_series(i*50+1, i*50+50) g;
    PERFORM bm25_seal('bmn_bm');
  END LOOP;
END $$;
SET bm25_native.debug_budget = '96kB';
-- The ladder DOES select them -- the point is not that selection is inert -- and the
-- trim is what refuses. Counted, never listed: which segments are chosen is stable,
-- but printing sixteen rows pins nothing this section is about.
SELECT count(*) FILTER (WHERE chosen) AS ladder_chose,
       count(*) FILTER (WHERE kept)   AS trim_kept
  FROM bm25_debug_merge_budget_plan('bmn_bm');
SELECT string_agg(gen::text, ',' ORDER BY gen) AS g
  FROM bm25_debug_merge_plan('bmn_bm') \gset before_
SELECT bm25_merge('bmn_bm');
SELECT bm25_merge('bmn_bm');
SELECT string_agg(gen::text, ',' ORDER BY gen) AS g
  FROM bm25_debug_merge_plan('bmn_bm') \gset after_
-- Identical generations after two forced merges: no segment was rewritten, so no
-- work was done. A rewrite mints fresh gens for every output, so this comparison
-- cannot be satisfied by a merge that happened to produce the same segment COUNT.
SELECT :'before_g' = :'after_g' AS no_pointless_rewrite;
SELECT count(*) AS unmerged_rows FROM bmn WHERE body @@@ 'alpha';
RESET bm25_native.debug_budget;

-- The trim sheds NON-TOMBSTONE picks first. Add one oversized, heavily-tombstoned
-- segment to the budget-floored rung above and the ladder bundles the two: the set
-- is infeasible, so the trim sheds until it is not. Shedding by estimate alone took
-- the tombstoned giant FIRST -- it is the largest -- then whittled the rung to a lone
-- non-tombstone survivor and returned 0, so the giant's dead space was never
-- reclaimed while that rung persisted. That is the case the exemption exists for and
-- the one it could not reach.
--
-- Asserted as "the survivor is tombstoned", not "kept > 0": a trim that kept an
-- arbitrary rung member would also be non-zero, and that is the wrong answer.
INSERT INTO bmn SELECT g, 'alpha bravo doc ' || g || ' w' || g || ' t' || (g % 31)
  FROM generate_series(10001, 13000) g;
SELECT bm25_seal('bmn_bm');
DELETE FROM bmn WHERE id > 10000 AND id % 10 <> 0;
-- 96 kB for the reason given above: it is what keeps the rung budget-floored.
SET bm25_native.debug_budget = '96kB';
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
-- The accepted-overshoot WARNING fires here, and its DETAIL names a generation
-- number: suppressed rather than pinned, because the message is not what this
-- section is about and the number is an implementation detail.
SET client_min_messages = error;
VACUUM bmn;
RESET client_min_messages;
-- 2700 dead documents pre-fix (the giant is never rewritten), 0 after. Stated as
-- reclaimed dead space rather than "the trim kept something": a trim that kept an
-- arbitrary rung member would also be non-zero, and that is the wrong answer.
SELECT max(ndocs - live_ndocs) AS max_unreclaimed_dead
  FROM bm25_debug_segcat('bmn_bm');
SELECT count(*) AS survivors FROM bmn WHERE body @@@ 'alpha';
RESET bm25_native.debug_budget;
DROP TABLE bmn;

-- Tombstones under a budget: delete half the corpus, let VACUUM tombstone and
-- reclaim, then merge with a budget set. Dead documents are gone and the survivors
-- read back exactly, which is the end-to-end statement the per-piece assertions
-- above do not make on their own.
DELETE FROM bmm WHERE id % 2 = 0;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM bmm;
SET bm25_native.debug_budget = '64kB';
SELECT bm25_merge('bmm_bm');
SELECT array_agg(id ORDER BY id) AS after_delete
  FROM bmm WHERE body @@@ 'alpha' AND id <= 12;
SELECT sum(live_ndocs) AS after_delete_live_docs FROM bm25_debug_segcat('bmm_bm');
RESET bm25_native.debug_budget;

-- ------------------------------------- the live discount is applied exactly once
-- A tombstoned segment must be charged for the documents that will actually be
-- replayed -- no more, and no LESS.
--
-- Getting this wrong is invisible to every assertion above, and was: the tombstone
-- path (bm25_livedocs_clear, the only one) decays the catalog entry's total_len in
-- place in the same WAL record that decrements live_ndocs, so the value the estimator
-- reads has already had the dead documents taken out of it -- and the estimator then
-- multiplied by live_ndocs/ndocs on top. Measured on a 10-document segment with one
-- document tombstoned: the charge was for 486 tokens where 540 are replayed, ~10% low,
-- growing as live_ndocs falls. Under-charging is the expensive direction -- it is what
-- lets the trim call an infeasible set feasible and rewrite the whole index for nothing
-- on every autovacuum, which is the regime this file's other sections exist to prevent.
--
-- THE ASSERTION IS AN EQUALITY BETWEEN TWO INDEXES, not arithmetic on est_bytes, which
-- is a sum of sizeof()s and platform-dependent. Both hold nine live documents with
-- identical bodies, so total_len, live_ndocs and nterms all match and only ndocs
-- differs: nine documents never deleted, against ten with one tombstoned. Same live
-- content must cost the same to replay. Pre-fix the tombstoned one came out strictly
-- lower, which is the whole defect in one comparison; the equality is exact and needs
-- no value printed and no margin argued.
--
-- English fixture on purpose: the double discount has nothing to do with the run/token
-- split (sql/108's subject) and shows up identically here, so this needs no dictionary.
CREATE TABLE bml_never (id int PRIMARY KEY, body text);
CREATE INDEX bml_never_bm ON bml_never USING bm25_native (body);
INSERT INTO bml_never SELECT g, 'alpha bravo charlie delta echo' FROM generate_series(1, 9) g;
SELECT bm25_seal('bml_never_bm');

CREATE TABLE bml_tomb (id int PRIMARY KEY, body text);
CREATE INDEX bml_tomb_bm ON bml_tomb USING bm25_native (body);
INSERT INTO bml_tomb SELECT g, 'alpha bravo charlie delta echo' FROM generate_series(1, 10) g;
SELECT bm25_seal('bml_tomb_bm');
DELETE FROM bml_tomb WHERE id = 10;
SELECT pg_temp.wait_for_xmin_horizon();
VACUUM bml_tomb;

-- The premise, asserted rather than assumed: the two entries agree on every input the
-- estimator reads EXCEPT ndocs -- and that means EVERY one bm25_debug_segcat exposes,
-- not a representative sample, because the row exists to localise a failure.  If a
-- future change stops decaying total_len or total_tokens on the tombstone path, this
-- row moves and the equality below stops meaning what it says; without total_tokens
-- here, such a change would leave every premise green while the equality went red and
-- send the reader looking in the wrong place.
SELECT (SELECT total_len     FROM bm25_debug_segcat('bml_never_bm'))
     = (SELECT total_len     FROM bm25_debug_segcat('bml_tomb_bm'))  AS total_len_matches,
       (SELECT total_tokens  FROM bm25_debug_segcat('bml_never_bm'))
     = (SELECT total_tokens  FROM bm25_debug_segcat('bml_tomb_bm'))  AS total_tokens_matches,
       (SELECT live_ndocs    FROM bm25_debug_segcat('bml_never_bm'))
     = (SELECT live_ndocs    FROM bm25_debug_segcat('bml_tomb_bm'))  AS live_ndocs_matches,
       (SELECT nterms        FROM bm25_debug_segcat('bml_never_bm'))
     = (SELECT nterms        FROM bm25_debug_segcat('bml_tomb_bm'))  AS nterms_matches,
       (SELECT has_positions FROM bm25_debug_segcat('bml_never_bm'))
     = (SELECT has_positions FROM bm25_debug_segcat('bml_tomb_bm'))  AS has_positions_matches,
       (SELECT ndocs         FROM bm25_debug_segcat('bml_never_bm'))
    <> (SELECT ndocs         FROM bm25_debug_segcat('bml_tomb_bm'))  AS ndocs_differs;

SELECT (SELECT sum(est_bytes) FROM bm25_debug_merge_budget_plan('bml_never_bm'))
     = (SELECT sum(est_bytes) FROM bm25_debug_merge_budget_plan('bml_tomb_bm'))
       AS tombstone_discounted_once;
DROP TABLE bml_never;
DROP TABLE bml_tomb;

RESET enable_seqscan;
DROP TABLE bmm;
DROP EXTENSION bm25_native;
