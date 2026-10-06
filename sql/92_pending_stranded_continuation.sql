-- A stranded pending continuation record is an ORDINARY state, not corruption.
--
-- Since v7 (#57) a document too large for one pending page is written as several
-- same-tid parts, the later ones flagged BM25_PENDING_DOC_CONT.  Part 0 reuses the
-- existing tail page when it fits; what the packing guarantees is narrower -- two
-- CONSECUTIVE parts never share a page.  bm25_pending_drain reassembles a document
-- by accumulating consecutive same-tid parts, and it has a branch for "a
-- continuation with nothing to continue" that drops the fragment rather than
-- failing the seal.
--
-- That branch used to carry Assert(false), on the belief that only a corrupt chain
-- could reach it.  It is reachable from a cancelled VACUUM:
--
--   * bm25_pending_mark_dead commits ONE GenericXLog record per page, with its
--     delay/interrupt point at the top of the NEXT iteration.
--   * Two consecutive parts of one document always live on separate pages: each
--     part's runneed is budgeted against the whole PENDING_PAGE_CAPACITY, so the
--     entry that ended part k is too big for the room part k left behind.
--   * PostgreSQL has no undo -- an aborted transaction does not roll back physical
--     page changes -- so the invalidations already committed when the cancel lands
--     are permanent.
--
-- So a cancel between two pages leaves part 0 invalidated and part 1 untouched, and
-- the next drain skips part 0 on its invalid TID and arrives at part 1 holding no
-- matching active document.  Autovacuum cancellation is routine (any conflicting
-- lock request causes it), so this is steady state, not a rarity.  On the cassert +
-- UBSan hardening build the assertion turned it into a PANIC:
--
--   TRAP: failed Assert("false"), File: "src/bm25_pending.c" ... bm25_pending_drain
--   client backend was terminated by signal 6: Abort trap: 6
--
-- WHY THIS SUITE DOES NOT CANCEL A VACUUM.  Timing does not discriminate reliably:
-- against 600 multi-part documents, statement_timeout = 30ms stranded nothing and
-- the seal completed, while 15ms stranded a fragment and PANICked.  A suite built on
-- that boundary passes vacuously whenever the cancel lands outside the window --
-- and passing vacuously is indistinguishable from passing correctly.
-- bm25_debug_pending_invalidate_page reproduces the identical on-disk state by
-- invalidating one chosen page, deterministically and with no clock involved.
--
-- DROPPING THE FRAGMENT IS CORRECT, not a lossy compromise: part 0 is invalidated
-- only when the bulkdelete callback judged that TID dead, so the document must not
-- be indexed at all.
CREATE EXTENSION bm25_native;

CREATE TABLE strand (id int, body text);
CREATE INDEX strand_idx ON strand USING bm25_native (body);

-- Never seal during the load: the chain must still be pending when we sweep it.
SET bm25_native.seal_threshold = '1GB';

-- The branch has TWO disjuncts -- no active document, or an active document with a
-- different TID -- and they need different chain shapes, because chain order is
-- append order.  PART ONE below covers the first: the big document is appended
-- FIRST, so the drain reaches its orphaned continuation having buffered nothing.
-- PART TWO covers the second.
--
-- One document of ~3000 distinct 10-byte stems -- far past one pending page, so it
-- becomes a multi-part document owning several whole pages.  Synthetic non-word
-- stems so english_stem is a no-op and the token count does not vary with the
-- cluster's collation.
INSERT INTO strand
SELECT 1, (SELECT string_agg('qq' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t);
INSERT INTO strand VALUES (2, 'small following document alpha beta');

-- The big document spans pages, so the chain is more than one page long.
SELECT bm25_debug_npages('strand_idx') > 2 AS chain_spans_pages;
SELECT pending_ndocs FROM bm25_stats('strand_idx');

-- Invalidate ONLY the chain's head page -- exactly what a sweep cancelled after its
-- first page leaves behind.  The big document's part 0 lives there; its later parts
-- do not, and are untouched.
SELECT bm25_debug_pending_invalidate_page('strand_idx',
                                          bm25_debug_pending_head('strand_idx')::int) > 0
         AS head_page_invalidated;

-- The seal must complete.  Pre-fix, an assertion-enabled build PANICked here and
-- took the backend down with signal 6; a production build already behaved correctly,
-- which is why this went unnoticed.
SELECT bm25_seal('strand_idx');

SELECT 'seal completed' AS outcome;
SELECT bm25_debug_pending_head('strand_idx') IS NULL AS chain_fully_drained;

-- The stranded document is absent, which is the correct outcome: its part 0 was
-- invalidated, and that only happens for a TID the sweep judged dead.
SET enable_seqscan = off;
SELECT count(*) AS stranded_document_not_indexed
  FROM strand WHERE body @@@ 'qq00000001';
-- The document that was never swept is intact, so the drain dropped exactly the
-- fragment and nothing else.
SELECT count(*) AS untouched_document_still_indexed
  FROM strand WHERE body @@@ 'alpha';
RESET enable_seqscan;

-- The index still accepts and seals new work after the drop.
INSERT INTO strand VALUES (3, 'post recovery document gamma delta');
SELECT bm25_seal('strand_idx');
SET enable_seqscan = off;
SELECT count(*) AS post_recovery_rows FROM strand WHERE body @@@ 'gamma';
RESET enable_seqscan;

-- ------------------------------------------------------------------ PART TWO
-- The other disjunct: an orphaned continuation reached while a DIFFERENT document
-- is active.  Part One cannot produce this -- it strands the FIRST document in the
-- chain, so the drain arrives with nothing buffered and takes `!doc.active`.  Here
-- the small document is appended first and the big one second, so the drain has the
-- small document active when it meets the big one's orphaned continuation and takes
-- `!ItemPointerEquals` instead.  Both disjuncts drop the fragment identically today;
-- covering both keeps that true if they are ever separated.
CREATE TABLE strand2 (id int, body text);
CREATE INDEX strand2_idx ON strand2 USING bm25_native (body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand2 VALUES (1, 'small leading document epsilon zeta');
INSERT INTO strand2
SELECT 2, (SELECT string_agg('rr' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t);

-- Chain position 1 is the page holding the big document's part 0: position 0 is the
-- head, which carries the small document.  The big document cannot share the head
-- page -- not because a part always starts a fresh one (part 0 reuses the tail page
-- whenever it fits), but because 3000 distinct terms are far more than one page
-- holds, so part 0 is greedily packed to nearly a full page and its runneed exceeds
-- meta.pending_tail_free after the small document.  Addressed by chain position
-- rather than head + 1 so the test does not silently depend on sequential page
-- allocation.
SELECT bm25_debug_pending_invalidate_page(
         'strand2_idx',
         bm25_debug_pending_nth_page('strand2_idx', 1)::int) > 0
       AS second_page_invalidated;

-- BEFORE the seal: the SCAN walkers meet the same orphan the drain does, and they
-- need the same TID check for the same reason.  bm25_pending_drain compares a
-- continuation's TID against the document it is accumulating; the ACCUMULATING
-- scan-side walkers (bm25_pending_score_term in src/bm25_stats.c,
-- pending_stats_by_field in src/bm25_scan_rank.c) only reset their per-document state on a
-- record WITHOUT the CONT flag, so without that comparison they fold an orphan's term
-- entries into whichever document precedes it on the chain -- crediting that document
-- with another document's term frequency.
-- The df walkers have a different shape and a different symptom; PART THREE covers
-- them, and needs a different kind of assertion to see the difference at all.
--
-- 'rr00002999' occurs ONLY in the stranded document's later parts, so a ranked scan
-- for it must not return the small document that precedes them.  Measured on a build
-- without the check: id 1 came back with a score of 0.287682, for a term its body
-- does not contain.  Membership style over the ranked scan, whose own ORDER BY is
-- single-key -- a secondary key would collapse the rank this is driving.
SET enable_seqscan = off;
SELECT array_agg(id ORDER BY id) AS orphan_term_ranked_before_seal FROM
  (SELECT id FROM strand2 WHERE body @@@ 'rr00002999'
    ORDER BY body &@@ 'rr00002999') s;
RESET enable_seqscan;

SELECT bm25_seal('strand2_idx');
SELECT bm25_debug_pending_head('strand2_idx') IS NULL AS chain2_fully_drained;

SET enable_seqscan = off;
-- The stranded big document is gone; the small document that was active when the
-- drain hit the orphan is untouched, so the drop took exactly the fragment.
SELECT count(*) AS stranded_big_document_not_indexed
  FROM strand2 WHERE body @@@ 'rr00000001';
SELECT count(*) AS leading_document_still_indexed
  FROM strand2 WHERE body @@@ 'epsilon';
RESET enable_seqscan;

-- ---------------------------------------------------------------- PART THREE
-- The df walkers: pending_df and pending_df_by_field (src/bm25_stats.c).
--
-- Everything above turns on MEMBERSHIP, and membership cannot see this defect.  The
-- fragment's own TID belongs to a heap tuple VACUUM was removing, so the heap
-- recheck drops whatever the fragment produces -- which is why the other
-- non-accumulating pending walkers need no correction downstream.  df does not go
-- through the heap.  It is a CORPUS statistic, it feeds idf, and idf moves the score
-- of every document carrying the term -- including documents the fragment never
-- touched.  So the discriminating assertion here is an UNRELATED document's score.
--
-- Two directions have to be pinned, and they need different chain states.

-- (3a) A perfectly ordinary multi-part document, nothing stranded.  This pins the
-- direction the cheap fix would break.  A document's distinct entries are
-- PARTITIONED across its parts (bm25_pending_append_multi sets
-- hdr.ndocterms = j - jstart over entries [jstart, j)), so a term can live ONLY in a
-- continuation: 'zz00009999' sits at the very end of a 3000-term body and lands in
-- the last part.  Counting it is correct -- the document really does contain it.
-- Skipping every continuation instead of tid-matching loses it: measured on such a
-- build, df fell from 2 to 1 and the score below rose from 1.474787 to 2.335308.
-- That is the same error in the opposite direction and, unlike a stranded fragment,
-- it is not a rare state.
CREATE TABLE strand3 (id int, body text);
CREATE INDEX strand3_idx ON strand3 USING bm25_native (body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand3
SELECT 1, (SELECT string_agg('qq' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t) || ' zz00009999';
INSERT INTO strand3 VALUES (2, 'unrelated small document zz00009999'),
                           (3, 'filler alpha one'),
                           (4, 'filler beta two'),
                           (5, 'filler gamma three');

-- bm25_debug_rank drives the exhaustive scorer directly, so it reports the score
-- itself rather than an ordering.  Its ORDER BY is single-key for the house reason:
-- a secondary key has collapsed BM25 rank in this repo before.
SELECT s.id, round(r.score::numeric, 6) AS score_legit_spanning
  FROM bm25_debug_rank('strand3_idx', 'zz00009999') r JOIN strand3 s ON s.ctid = r.tid
 ORDER BY r.score DESC;

-- (3b) The same corpus with the head page invalidated, so document 1 is a stranded
-- fragment: its part 0 is gone and its later parts survive with a valid TID and
-- BM25_PENDING_DOC_CONT set.  Document 1 must now contribute NOTHING -- not a
-- posting (PART TWO), and not a df either.
--
-- Measured on a build whose df walkers had no CONT handling, the small document
-- scored 0.633355 here instead of 1.100116: df read 2 against N = 4 rather than 1,
-- because the fragment was counted as a document containing the term.  N itself was
-- never inflated to match -- pending_global_stats already skipped continuations --
-- and that asymmetry is exactly the defect.
CREATE TABLE strand4 (id int, body text);
CREATE INDEX strand4_idx ON strand4 USING bm25_native (body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand4
SELECT 1, (SELECT string_agg('qq' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t) || ' zz00009999';
INSERT INTO strand4 VALUES (2, 'unrelated small document zz00009999'),
                           (3, 'filler alpha one'),
                           (4, 'filler beta two'),
                           (5, 'filler gamma three');

-- The big document is appended FIRST into an empty chain and its part 0 fills a page
-- on its own, so the head page holds that part and nothing else.
SELECT bm25_debug_pending_invalidate_page('strand4_idx',
         bm25_debug_pending_head('strand4_idx')::int) > 0 AS head_page_invalidated_3;

SELECT s.id, round(r.score::numeric, 6) AS score_with_stranded_fragment
  FROM bm25_debug_rank('strand4_idx', 'zz00009999') r JOIN strand4 s ON s.ctid = r.tid
 ORDER BY r.score DESC;

-- Control: a term the fragment does NOT carry is unmoved, so the shift above is a df
-- effect on that one term and not a corpus-wide change of N or avgdl.
SELECT s.id, round(r.score::numeric, 6) AS control_term_unmoved
  FROM bm25_debug_rank('strand4_idx', 'alpha') r JOIN strand4 s ON s.ctid = r.tid
 ORDER BY r.score DESC;

-- (3c) The per-field walker, pending_df_by_field, which bm25_term_idf uses instead of
-- pending_df whenever field_count > 1.  Same state, same guard, separate code path --
-- the single-field cases above never execute a line of it.
CREATE TABLE strand5 (id int, title text, body text);
CREATE INDEX strand5_idx ON strand5 USING bm25_native (title, body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand5
SELECT 1, 'big', (SELECT string_agg('qq' || lpad(t::text, 8, '0'), ' ')
                    FROM generate_series(1, 3000) t) || ' zz00009999';
INSERT INTO strand5 VALUES (2, 'small', 'unrelated small document zz00009999'),
                           (3, 'small', 'filler alpha one'),
                           (4, 'small', 'filler beta two'),
                           (5, 'small', 'filler gamma three');

SELECT bm25_debug_pending_invalidate_page('strand5_idx',
         bm25_debug_pending_head('strand5_idx')::int) > 0 AS head_page_invalidated_4;

SELECT s.id, round(r.score::numeric, 6) AS score_by_field
  FROM bm25_debug_rank('strand5_idx', 'zz00009999') r JOIN strand5 s ON s.ctid = r.tid
 ORDER BY r.score DESC;

-- (3d) The guard's OTHER disjunct.  (3b) and (3c) strand the FIRST document on the
-- chain, so the walk meets the orphan with nothing open and `!acc_active` alone would
-- answer correctly -- they do not exercise the TID COMPARISON at all.  A cancelled
-- sweep rarely stops at the chain head, so the shape below is the commoner one: a live
-- document is open when the walk reaches the fragment, and only comparing TIDs
-- distinguishes the fragment's entries from that document's own.
--
-- Document 1 deliberately does NOT carry the shared term.  If it did, the per-document
-- dedup would credit the term to document 1 and then skip the fragment for the wrong
-- reason, hiding a missing comparison.  Measured on a build whose guard tested
-- `!acc_active` alone: document 3 scored 0.654875 instead of 1.137496, and this was the
-- only assertion in the suite that moved.
CREATE TABLE strand6 (id int, body text);
CREATE INDEX strand6_idx ON strand6 USING bm25_native (body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand6 VALUES (1, 'leading document epsilon zeta');
INSERT INTO strand6
SELECT 2, (SELECT string_agg('rr' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t) || ' zz00009999';
INSERT INTO strand6 VALUES (3, 'unrelated small document zz00009999'),
                           (4, 'filler alpha one'),
                           (5, 'filler beta two');

-- Chain position 1, exactly as PART TWO: position 0 carries the leading document, and
-- the big document's part 0 fills the next page on its own.
SELECT bm25_debug_pending_invalidate_page(
         'strand6_idx',
         bm25_debug_pending_nth_page('strand6_idx', 1)::int) > 0
       AS second_page_invalidated_df;

SELECT s.id, round(r.score::numeric, 6) AS score_orphan_after_live_doc
  FROM bm25_debug_rank('strand6_idx', 'zz00009999') r JOIN strand6 s ON s.ctid = r.tid
 ORDER BY r.score DESC;

-- ----------------------------------------------------------------- PART FOUR
-- The wildcard expander: bm25_dict_expand_wildcard (src/bm25_seg_dict.c), issue #197.
--
-- The last walker of this class to be corrected, and the only one whose output is
-- neither a TID nor filtered downstream.  PART TWO's walkers accumulate, so a
-- fragment corrupts a NEIGHBOUR's postings; PART THREE's emit a corpus statistic, so
-- a fragment moves an UNRELATED document's score.  This one emits a TERM SET.  There
-- is no TID for the heap recheck to drop and no score for a df to move -- after the
-- PART THREE fix the fragment's terms carry df 0 and are dropped from ranking
-- outright.  What is left is the one thing a set can still spend: the terms count
-- toward bm25_native.wildcard_max_expansions, so a wildcard ERRORs at the cap where
-- it otherwise would not, until the next seal drops the fragment.
--
-- So the discriminating assertion here is neither membership nor score but the
-- expansion COUNT.  No probe reports it, and none is added: the cap itself measures
-- it exactly.  bm25_wild_add raises once a->n >= the cap, BEFORE appending, so a set
-- of exactly N terms succeeds at cap N and ERRORs at cap N-1.  Bracketing with that
-- pair pins the count to a single integer, which is why each case below runs the
-- query twice.
--
-- The text query grammar has no wildcard form -- 'qqq*' parses as a literal term and
-- never reaches the expander -- so these go through bm25_wildcard()'s jsonb leaf.
-- enable_seqscan is off throughout for the usual reason: a bare @@@ answered off the
-- index never calls the expander at all, and would pass this suite vacuously.

-- (4a) The direction the cheap fix breaks, pinned FIRST because it is the commoner
-- state.  An ordinary multi-part document, nothing stranded: its 3000 distinct terms
-- are PARTITIONED across its parts, so most of them live only in continuations.
-- Skipping every CONT record instead of tid-matching silently shrinks the expansion
-- set -- and a wildcard that expands to fewer terms silently drops matching rows,
-- exactly the outcome the sealed pass's past-the-extent ERROR exists to prevent.
-- Measured on such a build the count fell from 3000 to 337 -- part 0's share alone --
-- so the cap-2999 ERROR below did not fire, and this case is the one that separates
-- the two candidate fixes.  It is also the ONLY assertion in PART FOUR that moves on
-- that build: (4b) and (4c) answer correctly there, for the wrong reason.
CREATE TABLE strand7 (id int, body text);
CREATE INDEX strand7_idx ON strand7 USING bm25_native (body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand7
SELECT 1, (SELECT string_agg('qqq' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t);

SET enable_seqscan = off;
-- Upper half of the bracket: 3000 terms fit a cap of 3000.
SET bm25_native.wildcard_max_expansions = 3000;
SELECT array_agg(id ORDER BY id) AS legit_spanning_expansion
  FROM strand7 WHERE body @@@ bm25_wildcard('body','qqq*');
-- Lower half: they do not fit 2999, so the count is exactly 3000 and every
-- continuation's terms are present.
SET bm25_native.wildcard_max_expansions = 2999;
SELECT array_agg(id ORDER BY id) AS legit_spanning_expansion_capped
  FROM strand7 WHERE body @@@ bm25_wildcard('body','qqq*');
RESET bm25_native.wildcard_max_expansions;
RESET enable_seqscan;

-- (4b) The defect: a stranded fragment at the chain head.  Document 1's part 0 is
-- invalidated and its later parts survive, so it must contribute NO terms; document
-- 2 is untouched and contributes exactly its two.
--
-- This is the case that reaches an ordinary user with no GUC set: measured on the
-- unfixed build the fragment's terms took the expansion past the DEFAULT cap of
-- 1000, so 'qqq*' ERRORed outright rather than returning document 2.
CREATE TABLE strand8 (id int, body text);
CREATE INDEX strand8_idx ON strand8 USING bm25_native (body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand8
SELECT 1, (SELECT string_agg('qqq' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t);
INSERT INTO strand8 VALUES (2, 'qqq99990001 qqq99990002 alpha');

-- The big document is appended FIRST into an empty chain and its part 0 fills a page
-- on its own, so the head page holds that part and nothing else.
SELECT bm25_debug_pending_invalidate_page('strand8_idx',
         bm25_debug_pending_head('strand8_idx')::int) > 0 AS head_page_invalidated_wc;

SET enable_seqscan = off;
-- Default cap: pre-fix this ERRORed; the fragment alone exceeds 1000 terms.
SELECT array_agg(id ORDER BY id) AS stranded_expansion_default_cap
  FROM strand8 WHERE body @@@ bm25_wildcard('body','qqq*');
-- Bracket the surviving count at exactly 2 -- document 2's terms, and nothing of the
-- fragment's 3000.  A build that instead skipped every continuation would also
-- answer 2 here, which is why (4a) is the case that separates the two fixes.
SET bm25_native.wildcard_max_expansions = 2;
SELECT array_agg(id ORDER BY id) AS stranded_expansion_cap2
  FROM strand8 WHERE body @@@ bm25_wildcard('body','qqq*');
SET bm25_native.wildcard_max_expansions = 1;
SELECT array_agg(id ORDER BY id) AS stranded_expansion_cap1
  FROM strand8 WHERE body @@@ bm25_wildcard('body','qqq*');
RESET bm25_native.wildcard_max_expansions;
RESET enable_seqscan;

-- (4c) The guard's OTHER disjunct, mirroring (3d).  (4b) strands the FIRST document,
-- so the walk meets the orphan with nothing open and `!acc_active` alone would answer
-- correctly.  Here a live document is open when the walk reaches the fragment, and
-- only comparing TIDs tells the fragment's entries from that document's own.
-- Measured on a build whose guard tested `!acc_active` alone, the fragment's terms
-- were folded into the open document's and 'qqq*' ERRORed at the cap below.
CREATE TABLE strand9 (id int, body text);
CREATE INDEX strand9_idx ON strand9 USING bm25_native (body);
SET bm25_native.seal_threshold = '1GB';

INSERT INTO strand9 VALUES (1, 'qqq77770001 leading document epsilon');
INSERT INTO strand9
SELECT 2, (SELECT string_agg('qqq' || lpad(t::text, 8, '0'), ' ')
             FROM generate_series(1, 3000) t);

-- Chain position 1, exactly as PART TWO: position 0 carries the leading document, and
-- the big document's part 0 fills the next page on its own.
SELECT bm25_debug_pending_invalidate_page(
         'strand9_idx',
         bm25_debug_pending_nth_page('strand9_idx', 1)::int) > 0
       AS second_page_invalidated_wc;

SET enable_seqscan = off;
SET bm25_native.wildcard_max_expansions = 50;
SELECT array_agg(id ORDER BY id) AS orphan_after_live_expansion
  FROM strand9 WHERE body @@@ bm25_wildcard('body','qqq*');
-- Exactly one term survives -- the leading document's own.  A cap of 1 admits one
-- term and no more, and the row still comes back, so the count is 1 rather than 0.
SET bm25_native.wildcard_max_expansions = 1;
SELECT array_agg(id ORDER BY id) AS orphan_after_live_expansion_cap1
  FROM strand9 WHERE body @@@ bm25_wildcard('body','qqq*');
RESET bm25_native.wildcard_max_expansions;
RESET enable_seqscan;

DROP TABLE strand;
DROP TABLE strand2;
DROP TABLE strand3;
DROP TABLE strand4;
DROP TABLE strand5;
DROP TABLE strand6;
DROP TABLE strand7;
DROP TABLE strand8;
DROP TABLE strand9;
DROP EXTENSION bm25_native;
