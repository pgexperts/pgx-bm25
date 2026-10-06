-- H3 (issue #43): the extension contained no CHECK_FOR_INTERRUPTS at all, so a
-- ranked or @@@ scan ignored Ctrl-C, statement_timeout, pg_cancel_backend and
-- standby recovery-conflict signals for the whole in-AM build.
--
-- WHAT THIS SUITE CAN AND CANNOT PROVE. The defect was LATENCY, not outcome: a
-- pre-fix scan still ended with "canceling statement due to statement timeout",
-- just after the entire build had finished (measured in the report: a 5 ms
-- timeout took 4794 ms on 400k docs). pg_regress compares output, not wall
-- clock, so no expected-output test can separate pre- from post-fix.
--
-- So this suite pins the OUTCOME -- that a scan is interruptible at all, i.e.
-- that the checks sit on paths that actually execute and nothing swallows the
-- interrupt -- and the LATENCY is pinned by the CI grep floor in ci.yml, which
-- fails if the checks are deleted. The measured latency after the fix, same
-- shape as the report's: 4799 ms scan, 5 ms timeout honored in 8 ms (was 4794).
--
-- The PHRASE section at the bottom of this file goes further and pins the latency
-- IN BAND, by bounding the WORK a cancelled scan got through rather than the
-- milliseconds it took (issue #156), with the cancel injected at a fixed step rather
-- than delivered by a timer (ADR 0070's 2026-10-06 addendum). Both halves are what
-- make it runner-independent; see that section's own header for why each was needed.
--
-- Sizing, re-measured on the development machine (Apple silicon, PG 18.6) at the
-- 500k rows below, against the 1 ms timeout used here:
--     @@@ count                     106 ms   -> ~106x margin
--     ranked, WAND (k = 100)         46 ms   -> ~46x margin
--     ranked, exhaustive (k = 0)     46 ms   -> ~46x margin
-- The ranked margins are the tight ones. CI runners are slower than this
-- machine, which widens every margin, so the failure direction would be a
-- machine ~46x faster than current Apple silicon. If that day comes, raise the
-- row count rather than lowering the timeout (1 ms is already the floor).
--
-- (The previous figures here -- 321 / 9 / 9 ms at 100k rows, PG 18.3 -- do not
-- scale to these: the @@@ union got faster in absolute terms while the corpus grew
-- 5x. They are left recorded only so a reader who finds them in the history knows
-- they were superseded by measurement, not by arithmetic.)
--
-- THE FIXTURE CHANGED SHAPE IN #156, in three ways, all driven by the phrase
-- assertion at the bottom of this file. It bounds the WORK a cancelled scan got
-- through against the same query's full work, and the numbers only separate if the
-- fixture is built for it:
--
--   (1) 500,000 rows, not 100,000. While the phrase cancel was timed, the
--       cancelled figure was fixed by the TIMEOUT, not by the corpus, so only a
--       bigger corpus bought headroom. The injected cancel no longer needs it; the
--       rows stay because the 1 ms outcome checks below need every query to outlast
--       its timeout.
--   (2) No md5 term per row. That term was never queried anywhere in this file --
--       it was pure vocabulary -- and it dominated the build accumulator, because a
--       unique term per document is the worst case for it. Dropping it cuts
--       CREATE INDEX from 1539 ms to 676 ms at this row count and, more to the
--       point, lets the whole corpus fit in ONE segment.
--   (3) ONE segment, forced by bm25_native.debug_budget below. This is the load
--       bearing one. bm25_seg_scan_postings is called once per (term, SEGMENT), and
--       the defect the phrase assertion guards against -- a lock held across that
--       call's decode -- is uncancellable for exactly one such call. So the
--       pre-fix work figure is full_work/(terms x segments), which does NOT grow
--       with the corpus: at 400k rows in 7 chunked segments it was 33 units against
--       a full count of 427, and a ratio bound that the fixed tree passed, the
--       defeated tree ALSO passed. One segment makes it full_work/2, which is what
--       gives the assertion a real margin on both sides. Measured, not assumed --
--       the 7-segment version was built and its A/B observed to fail to
--       discriminate.
--
-- The other blocks in this file are indifferent to all three; they only get wider
-- margins and a shorter fixture.
CREATE EXTENSION bm25_native;

-- autovacuum_enabled = false for the reason sql/80_maintenance_interrupts' header
-- records: autovacuum can race the suite and drain/seal/merge the pending list
-- underneath it. At the old 100k rows it never got the chance; at this size it
-- does, and it showed up immediately as a nondeterministic answer to the ranked
-- LIMIT 1 below (three runs of an unchanged tree returned 21729, 21729, 20177).
-- Every document here carries the same two terms at the same positions with the
-- same length, so their BM25 scores tie EXACTLY and the top-1 among them is
-- whatever order the segments happen to present -- which a background merge
-- changes. Pinned two ways: autovacuum off here, and the surviving ranked probe
-- below reports only that it got a row, not which one.
CREATE TABLE int_docs (id int, body text) WITH (autovacuum_enabled = false);
INSERT INTO int_docs
SELECT g, 'alpha common ' || (g % 500)
FROM generate_series(1, 500000) g;
-- 256 MB, and the value is not arbitrary: it is the smallest round budget that
-- keeps this corpus in ONE segment (at the 64 MB default it chunks into two, and
-- with the old md5 vocabulary it chunked into seven). The actual accumulator peak
-- is well under the ceiling -- the same corpus crosses 64 MB just once -- so this
-- is a ceiling being raised, not memory being reserved.
SET bm25_native.debug_budget = 262144;
CREATE INDEX int_idx ON int_docs USING bm25_native (body);
RESET bm25_native.debug_budget;

-- Fixture witness, in the shape sql/80_maintenance_interrupts uses for its own
-- single-DICT-page fixture: assert the segment count the phrase assertion's margin
-- depends on rather than trusting that the budget above did what it says.
SELECT count(*) AS int_segments FROM bm25_debug_merge_plan('int_idx'::regclass);

-- Baseline: the scan works and touches every row.
SELECT count(*) FROM int_docs WHERE body @@@ 'alpha';

-- The @@@ union path honors statement_timeout.
SET statement_timeout = '1ms';
SELECT count(*) FROM int_docs WHERE body @@@ 'alpha';
RESET statement_timeout;

-- The ranked path honors it too, on both the WAND and the exhaustive scorer.
SET statement_timeout = '1ms';
SELECT id FROM int_docs ORDER BY body &@@ 'alpha' LIMIT 1;
SET bm25_native.wand_top_k = 0;
SELECT id FROM int_docs ORDER BY body &@@ 'alpha' LIMIT 1;
RESET bm25_native.wand_top_k;
RESET statement_timeout;

-- ============================================================================
-- PHRASE PATH (issue #139) -- and why it needs a DIFFERENT kind of test
-- ============================================================================
-- Everything above pins OUTCOME, and this file's own header says why that is
-- all it can do: the query cancels either way, so pg_regress sees no
-- difference. For #139 that limitation is fatal rather than merely annoying,
-- because #139 IS the latency. A phrase query opens the lockstep POS cursor
-- (bm25_seg_scan_postings, want_pos), and that cursor used to hold ONE POS page
-- SHARE-locked continuously for the whole POST decode. A buffer content lock is
-- an LWLock, LWLockAcquire does HOLD_INTERRUPTS(), so the per-posting-page
-- CHECK_FOR_INTERRUPTS -- the check whose own comment calls it "what makes a
-- long scan cancellable at all" -- was a silent no-op on EVERY phrase /
-- proximity query. The 1 ms timeout still landed; it landed after the term's
-- entire POST+POS replay. Adding a `SET statement_timeout; SELECT phrase` line
-- above would have printed the identical "canceling statement due to statement
-- timeout" before and after the fix, i.e. proved nothing.
--
-- So this section borrows sql/80_maintenance_interrupts' in-band harness: a
-- plpgsql wrapper runs the query, and a `WHEN query_canceled` handler reports how
-- much WORK the cancelled statement got through. The wrapper RAISEs if the query
-- ever completes without being cancelled, so a corpus too small to reach the
-- assertion fails LOUDLY rather than passing vacuously.
--
-- The query pairs @@@ with ORDER BY ... &@@ on the SAME literal, which is the
-- 38_phrase house pattern: that is what forces the bm25_native ordered index
-- scan, and hence the want_pos path with the POS cursor actually open. A bare
-- @@@ can fall to bm25_match, which reads no positions at all. Phrase queries
-- are also excluded from the WAND driver by construction (bm25_scan_rank.c's gate),
-- so this always exercises the exhaustive scorer's phrase stash.
--
-- WORK, NOT WALL CLOCK (issue #156) -- AND NOT A TIMER EITHER (ADR 0070's 2026-10-06
-- addendum). This assertion was `elapsed < interval '300 ms'` until #156, and that
-- boundary was a property of the RUNNER: macos-latest blew it intermittently on an
-- unchanged tree. It was also no boundary at all on a fast machine: the UNCANCELLED
-- query took 142 ms there, inside the 300 ms limit. #156 replaced it with the WORK
-- the cancelled query got through, against the same query's full work. Those counts
-- do not move when a stalled backend burns wall clock.
--
-- That fixed the measurement and left the CANCEL on a 10 ms statement_timeout, which
-- put the runner straight back in. The count a timed cancel reports is (units per ms)
-- x (the time the cancel actually arrives), and #156's "a slow runner only does
-- fewer units in 10 ms" assumed the timer arrives at 10 ms. On macos-latest it does
-- not. Measured there, 25 runs: the 10 ms timeout was serviced at 13-51 ms, with
-- this loop's checks live and the count still climbing throughout, so it reported
-- 22-88 units against the 52.6 ceiling, 9 runs of 25 over. Main failed this
-- assertion in 5 of 6 runs. The code getting faster pushes the same way: the
-- development machine's count drifted from 11-13 (2026-09-22) to 30-34 (2026-10-06)
-- with no change to this file.
--
-- So the cancel is INJECTED at a fixed step instead of timed.
-- bm25_native.debug_cancel_at = 'scan_post_page' makes the backend set its own
-- query-cancel flags (the assignments StatementCancelHandler makes on SIGINT) at a
-- POST page, immediately ahead of that page's CHECK_FOR_INTERRUPTS, and
-- bm25_native.debug_cancel_after = 50 holds it back to the first such page reached
-- after 50 work units -- MID-loop, not on the first iteration. A first-iteration
-- cancel shows only that the first check is live: a lock taken DURING that iteration
-- and held across the rest would pass it (adversarial review, measured: count 1 on
-- exactly that defect). The arrival point is fixed by the code, so the count is too.
-- What the assertion measures is still the property under test: how much further
-- the scan runs after the cancel arrives, before a check that CAN act on it does.
-- That is the latency #139 was about, in units of work instead of milliseconds, and
-- nothing about the runner enters it. sql/148 established the lever for the KEYMAP
-- writer; the 1 ms outcome checks above still cover statement_timeout itself.
--
-- bm25_debug_work_units() counts steps on the interrupt-checked loops -- here POST
-- pages decoded and POS pages crossed -- and is a C global that SURVIVES the abort of
-- the statement being measured, which is the whole reason it can be read from a
-- `WHEN query_canceled` handler at all (see BM25_WORK_UNIT() in src/bm25.h).
--
-- WHY A RATIO AND NOT A CONSTANT. The denominator is MEASURED by the suite, from
-- the same corpus, seconds later -- so the bound scales with the fixture instead of
-- being a magic number that silently rots when the corpus or the page packing
-- changes. The assertion compares what follows the arrival on both sides:
-- (cancelled - arrival) * 10 < (full - arrival), i.e. once the cancel was pending
-- the scan did under a tenth of the work it had left. It cannot pass on a counter
-- that counts nothing: the counter would never reach the arrival, the cancel would
-- never fire, and the wrapper RAISEs "completed WITHOUT cancellation".
--
-- SIZING (development machine, Apple silicon, PG 18.6, 2026-10-06):
--   500,000 docs in ONE segment, phrase '"alpha common"', both terms df = 500,000.
--     uncancelled work:       526 units  (POST pages + POS pages, both terms)
--     arrival:                the first POST page after 50 units, inside the first
--                             term's 263-unit (term, segment) call
--     cancelled work:         52 units, every run
--     bound asserted:         (cancelled - 50) * 10 < 526 - 50, i.e. cancelled < 97.6
--   The row count no longer buys headroom, because the numerator no longer grows
--   with the machine. What still matters is the ONE segment: it is what makes the
--   pre-fix count half the total work (one whole (term, segment) call) rather than a
--   1/(terms x segments) sliver that the ratio could not tell from a working check.
--   (The 500,000 rows stay for the 1 ms outcome checks at the top of the file, which
--   need every query to outlast its timeout.)
--
-- A/B VERIFIED (2026-10-06), not guessed, against three models of the defect, each
-- the only diff: interrupts held (a) across bm25_seg_scan_postings' whole page loop,
-- (b) from the loop's first check to its end, the shape a lock taken during the first
-- iteration would have, and (c) only while the POS cursor is open, which is the
-- pre-#139 defect exactly. All three service the cancel only after the whole first
-- (term, segment) replay, at 263 units, three runs each, and this assertion returns
-- f. (The timed form's own A/B, 2026-09-22, also saw 263.)
--
-- The work counter is deliberately NOT fused with the interrupt check for exactly
-- this reason: the defect leaves the work happening and only stops the check from
-- firing, so a counter bumped by the check would have gone quiet along with it.
-- Counting the loop instead is what makes the number climb.
CREATE TABLE int_work (what text, units bigint);

CREATE FUNCTION int_phrase_cancel_test(arrival int) RETURNS bigint AS $$
DECLARE
    work bigint;
BEGIN
    /* Nothing between this reset and the measured query touches a bm25 index, so
     * the counter reads as a delta even though it is one process-wide global. */
    PERFORM bm25_debug_work_reset();
    BEGIN
        /* is_local: the lever belongs to this block's subtransaction, so the abort
         * that services the cancel also reverts it, and the uncancelled run below
         * cannot inherit it. */
        PERFORM set_config('bm25_native.debug_cancel_at', 'scan_post_page', true);
        PERFORM set_config('bm25_native.debug_cancel_after', arrival::text, true);
        PERFORM id FROM int_docs
         WHERE body @@@ '"alpha common"'
         ORDER BY body &@@ '"alpha common"'
         LIMIT 1;
        RAISE EXCEPTION 'phrase scan completed WITHOUT cancellation -- the injected cancel never arrived';
    EXCEPTION WHEN query_canceled THEN
        /* Readable HERE, after the subtransaction this block ran in has already
         * aborted, only because the counter is a C global. Anything this block had
         * written to a table would be gone. */
        work := bm25_debug_work_units();
    END;
    RETURN work;
END;
$$ LANGUAGE plpgsql;

-- enable_seqscan = off so the @@@ predicate becomes an Index Cond rather than a
-- bm25_match filter, which reads no positions and would never reach the POST/POS
-- lockstep this measures.
--
-- The cancel arrives at the first POST page reached after 50 units: inside the
-- first term's 263-unit (term, segment) call, which is the window a held lock
-- would span. Not the first page, because a cancel there only shows that the FIRST
-- iteration's check is live; a lock taken during that iteration and held for the
-- rest would pass it. The fixture is single-field, so no positionless postings call
-- (bm25_term_idf's multi-field path) can reach the point first.
SET enable_seqscan = off;
INSERT INTO int_work VALUES ('arrival', 50);
INSERT INTO int_work
SELECT 'cancelled', int_phrase_cancel_test(units::int) FROM int_work WHERE what = 'arrival';
RESET enable_seqscan;

-- The backend is still healthy after the cancellations: no leaked buffer pin,
-- no wedged scan state, same answer as the baseline. The phrase query runs to
-- COMPLETION here so the POS cursor's palloc'd page copy and the POST page copy
-- are exercised on the normal path too, not only through a cancelled one.
-- Counted through a subquery rather than reported as a top-1 row: every doc has
-- the same two terms at the same two positions and the same length, so their
-- BM25 scores tie exactly and any single-row answer would be arbitrary. The
-- count is 500000 either way, and is a real assertion that the phrase recheck
-- KEPT every doc (each row is literally "alpha common ...", adjacent).
--
-- It is ALSO the denominator for the assertion above -- one run serving both jobs,
-- so the ratio costs no extra fixture time. Hence the reset immediately before it:
-- the two @@@ health checks on the line above do their own indexed work and must
-- not be counted.
SELECT count(*) FROM int_docs WHERE body @@@ 'alpha';
-- Reports THAT a row came back, not WHICH -- see the fixture comment at the top:
-- every score ties here, so the identity of the top-1 is arbitrary and pinning it
-- pins segment layout rather than scan health. (This printed a bare id before #156,
-- which was stable only because the corpus was small enough that nothing ever
-- re-laid-out the segments under it.)
SELECT count(*) = 1 AS ranked_scan_healthy
  FROM (SELECT id FROM int_docs ORDER BY body &@@ 'alpha' LIMIT 1) s;
SET enable_seqscan = off;
SELECT bm25_debug_work_reset();
SELECT count(*) AS phrase_matches FROM (
    SELECT id FROM int_docs
     WHERE body @@@ '"alpha common"'
     ORDER BY body &@@ '"alpha common"'
) s;
INSERT INTO int_work SELECT 'full', bm25_debug_work_units();
RESET enable_seqscan;

-- Non-vacuity canary, stated separately from the ratio so a failure says WHICH of
-- the two things went wrong. 526 units measured; a floor of 100 is 5.3x below that,
-- twice the 50-unit arrival, and above the 97.6-unit ceiling the assertion allows the
-- cancelled run -- so the two cannot both be satisfied by a counter that has quietly
-- stopped counting this path.
SELECT (SELECT units FROM int_work WHERE what = 'full') > 100 AS full_work_nontrivial;

-- THE ASSERTION. Once the cancel arrived, the scan did under a tenth of the work it
-- had left: (cancelled - arrival) * 10 < (full - arrival).
SELECT (c - a) * 10 < (f - a) AS phrase_cancelled_early
  FROM (SELECT max(units) FILTER (WHERE what = 'arrival')   AS a,
               max(units) FILTER (WHERE what = 'cancelled') AS c,
               max(units) FILTER (WHERE what = 'full')      AS f
          FROM int_work) w;

DROP FUNCTION int_phrase_cancel_test(int);
DROP TABLE int_work;
DROP TABLE int_docs;
DROP EXTENSION bm25_native;
