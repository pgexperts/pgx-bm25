-- Issue #152: scored-scan state resolution.
--
-- SCAN-04 -- the score resolvers' fallback treated a REGISTERED scan as a rival.
--   bm25_resolve_score_tid and bm25_resolve_score_key enabled their hash fallback
--   only when bm25_scored_scan_count() == 1.  But a finished scan lingers in the
--   registry until its bm25_endscan runs, which for a UNION ALL sibling (or any
--   Append/SubqueryScan child) only fires at the query's ExecutorEnd -- long after
--   that sibling returned its last row.  So from the second branch onward the raw
--   count is >= 2 and the fallback was disabled exactly when it was needed,
--   returning SQL NULL where a score belongs.  A silent NULL is a wrong answer, and
--   it appears only once a second scan exists, i.e. invisible to single-scan tests.
--
--   bm25_sole_scored_scan already had the right predicate for this -- it walks the
--   registry behind the head and counts only a scan that could still emit a FUTURE
--   row -- and its own comment says a raw count over-reports rivalry.  The two
--   resolvers used exactly the count that comment rejects.  The predicate is now
--   shared (bm25_scan_can_emit_more / bm25_sole_live_scored_scan).
--
-- SCAN-03 / TEXT-02 -- cur_ranked_idx outlived the array it indexes.
--   The WAND over-pull tail rebuild replaces ranked/scores/ranked_keys with freshly
--   palloc'd arrays sized to a NEW survivor count and sets nranked to it, while
--   cur_ranked_idx still points at the old capped nranked - 1.  If a concurrent
--   DELETE+VACUUM shrank the match set, the new nranked can be smaller, and the scan
--   stays registered until bm25_endscan -- so a later bm25_score(ctid) in the same
--   query reads past the end of all three arrays (the resolvers walk EVERY
--   registered scan, not just the row's owner).  Heap garbage compared against a
--   TID, or a segfault.
--
--   NOT ASSERTED HERE, deliberately.  Reaching it needs a concurrent DELETE+VACUUM
--   landing between the WAND build and the tail rebuild, which pg_regress cannot
--   schedule; a test that merely exercises the rebuild without shrinking the match
--   set passes identically before and after the fix, which is worse than no test.
--   Both halves of the fix are defence in depth against a certain-from-the-code
--   defect: the rebuild site clears the index, and both resolvers bound-check it.
--   What IS asserted below is that the tail rebuild still works -- sql/72 covers it
--   in depth; this file only checks the clearing did not break the normal path.
CREATE EXTENSION bm25_native;

-- ------------------------------------------------------------------- SCAN-04
-- Six rows, all matching every probe term, so each branch's scan is EXHAUSTED
-- (rcur reaches nranked) rather than stopped early.  That distinction is the whole
-- point: see the LIMIT note at the end of this file.
CREATE TABLE ssr (id int, body text);
INSERT INTO ssr SELECT g, 'alpha beta gamma' FROM generate_series(1, 6) g;
CREATE INDEX ssr_idx ON ssr USING bm25_native (body);
SELECT bm25_seal('ssr_idx');
SET enable_seqscan = off;

-- The probe is a CONSTANT tid, not the current row, so the current-row fast path
-- misses on every row but one and the query genuinely reaches the fallback.  With
-- bm25_score(ctid) the fast path would resolve everything and the test would pass
-- vacuously -- measured: it does, both before and after the fix.
--
-- Branch a runs with an empty registry and resolved even pre-fix.  Branches b and c
-- run with a finished-but-registered sibling behind the head, and pre-fix returned
-- NULL for every row whose fast path missed.
--   pre-fix:  a = 6/0, b = 1/5, c = 1/5
--   post-fix: a = 6/0, b = 6/0, c = 6/0
SELECT branch, resolved, unresolved FROM (
  SELECT 'a' AS branch,
         count(*) FILTER (WHERE r)     AS resolved,
         count(*) FILTER (WHERE NOT r) AS unresolved
    FROM (SELECT bm25_score('(0,1)'::tid) IS NOT NULL AS r
            FROM ssr WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha') q1
  UNION ALL
  SELECT 'b', count(*) FILTER (WHERE r), count(*) FILTER (WHERE NOT r)
    FROM (SELECT bm25_score('(0,1)'::tid) IS NOT NULL AS r
            FROM ssr WHERE body @@@ 'beta' ORDER BY body &@@ 'beta') q2
  UNION ALL
  SELECT 'c', count(*) FILTER (WHERE r), count(*) FILTER (WHERE NOT r)
    FROM (SELECT bm25_score('(0,1)'::tid) IS NOT NULL AS r
            FROM ssr WHERE body @@@ 'gamma' ORDER BY body &@@ 'gamma') q3
) z ORDER BY branch;

-- The keyed accessor takes the same path and must agree.
CREATE TABLE ssrk (id int, body text);
INSERT INTO ssrk SELECT g, 'delta epsilon zeta' FROM generate_series(1, 6) g;
CREATE INDEX ssrk_idx ON ssrk USING bm25_native (body) INCLUDE (id)
  WITH (key_field = 'id');
SELECT bm25_seal('ssrk_idx');

SELECT branch, resolved, unresolved FROM (
  SELECT 'a' AS branch,
         count(*) FILTER (WHERE r)     AS resolved,
         count(*) FILTER (WHERE NOT r) AS unresolved
    FROM (SELECT bm25_score_key(1) IS NOT NULL AS r
            FROM ssrk WHERE body @@@ 'delta' ORDER BY body &@@ 'delta') q1
  UNION ALL
  SELECT 'b', count(*) FILTER (WHERE r), count(*) FILTER (WHERE NOT r)
    FROM (SELECT bm25_score_key(1) IS NOT NULL AS r
            FROM ssrk WHERE body @@@ 'epsilon' ORDER BY body &@@ 'epsilon') q2
  UNION ALL
  SELECT 'c', count(*) FILTER (WHERE r), count(*) FILTER (WHERE NOT r)
    FROM (SELECT bm25_score_key(1) IS NOT NULL AS r
            FROM ssrk WHERE body @@@ 'zeta' ORDER BY body &@@ 'zeta') q3
) z ORDER BY branch;

-- bm25_snippet's ambiguity check shares the predicate now, so it must NOT have
-- become stricter: three exhausted siblings are not rivals and must not error.
SELECT branch, n FROM (
  SELECT 'a' AS branch, count(*) AS n
    FROM (SELECT bm25_snippet(body, 'alpha') AS s
            FROM ssr WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha') q1
  UNION ALL
  SELECT 'b', count(*)
    FROM (SELECT bm25_snippet(body, 'beta') AS s
            FROM ssr WHERE body @@@ 'beta' ORDER BY body &@@ 'beta') q2
) z ORDER BY branch;

-- ------------------------------------------------- SCAN-03: the normal path holds
-- Clearing cur_ranked_idx before the tail rebuild must not disturb the rebuild
-- itself.  wand_top_k = 2 forces the over-pull path; the ranked sequence and the
-- scores must be identical to the exhaustive build.  (sql/72 is the suite that
-- covers the rebuild properly; this is a guard against the clearing regressing it.)
SET bm25_native.wand_top_k = 0;
SELECT id FROM ssr WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha';
SET bm25_native.wand_top_k = 2;
SELECT id FROM ssr WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha';
SELECT count(*) AS scores_resolve_after_tail_rebuild
  FROM (SELECT bm25_score(ctid) AS sc
          FROM ssr WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha') q
 WHERE sc IS NOT NULL;
RESET bm25_native.wand_top_k;

-- ------------------------------------------------------------------- the LIMIT note
-- A sibling stopped by LIMIT rather than exhaustion is STILL a live rival, and must
-- be: rcur < nranked holds, so the scan genuinely could emit more if the executor
-- asked again -- the AM cannot know the Append moved on.  So the fallback stays
-- disabled here and these branches still return NULL past the fast path.  That is
-- the predicate being conservative in the safe direction, not a residue of the bug;
-- recording it so the next reader does not "fix" it by weakening the test.
SELECT branch, resolved, unresolved FROM (
  SELECT 'a' AS branch,
         count(*) FILTER (WHERE r)     AS resolved,
         count(*) FILTER (WHERE NOT r) AS unresolved
    FROM (SELECT bm25_score('(0,1)'::tid) IS NOT NULL AS r
            FROM ssr WHERE body @@@ 'alpha' ORDER BY body &@@ 'alpha' LIMIT 3) q1
  UNION ALL
  SELECT 'b', count(*) FILTER (WHERE r), count(*) FILTER (WHERE NOT r)
    FROM (SELECT bm25_score('(0,1)'::tid) IS NOT NULL AS r
            FROM ssr WHERE body @@@ 'beta' ORDER BY body &@@ 'beta' LIMIT 3) q2
) z ORDER BY branch;

RESET enable_seqscan;
DROP TABLE ssr;
DROP TABLE ssrk;
DROP EXTENSION bm25_native;
