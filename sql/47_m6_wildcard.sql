-- 47_m6_wildcard — M6 Task 7: WILDCARD leaf expansion + strict GUC guardrails.
--
-- A WILDCARD leaf's glob pattern is expanded over the byte-sorted segment dicts
-- (prefix range + embedded glob) plus the pending list, deduped into ONE distinct
-- set, and each expanded term scores as an OR alternative under the leaf's single
-- presence bit. D8: the pattern is matched RAW-lowercased against the STEMMED dict
-- bytes (NOT re-stemmed), so judg* -> judg/judgment/judgement (from judge/judgment/
-- judgement) but NOT judo; wom*n -> woman/women but NOT wombat. The expected id
-- sets were verified against bm25_debug_tokenize (judge->judg, judo->judo,
-- woman->woman, women->women, wombat->wombat) — NOT hand-computed.
--
-- MEMBERSHIP idiom (array_agg(id ORDER BY id) over an inner ORDER BY ... &@@ ..., id):
-- SET-correct, so it never asserts a score-desc sequence off a query with a secondary
-- sort key (the old secondary-key rank collapse is fixed; single-key ORDER BY is the
-- house style for ranked sequences).
-- enable_seqscan=off forces the bm25_native index scan: a seqscan would apply the
-- jsonb @@@ as a filter, which bm25_match_jsonb refuses with an ERROR, and neither
-- the parse-time min-prefix ERROR nor the expander's cap ERROR would fire.

CREATE EXTENSION bm25_native;
CREATE TABLE wc (id int, body text);
INSERT INTO wc VALUES (1,'judge'),(2,'judgment'),(3,'judgement'),(4,'woman'),(5,'women'),(6,'judo'),(7,'wombat');
CREATE INDEX wc_bm25 ON wc USING bm25_native (body);
SELECT bm25_seal('wc_bm25');
SET enable_seqscan=off;

-- judg* -> judge/judgment/judgement (NOT judo). Ranked form so the scorer expands.
SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM wc WHERE body @@@ bm25_wildcard('body','judg*') ORDER BY body &@@ bm25_wildcard('body','judg*'), id) s;  -- expect {1,2,3}
-- wom*n (embedded glob) -> woman/women (NOT wombat).
SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM wc WHERE body @@@ bm25_wildcard('body','wom*n') ORDER BY body &@@ bm25_wildcard('body','wom*n'), id) s;  -- expect {4,5}

-- D12 (Task 8): the bare @@@ wildcard filter (NON-scoring, no ORDER BY) EQUALS the
-- ranked &@@ set. Before Task 8 a bare wildcard @@@ ERRORed ("not yet supported by the
-- filter-only (@@@) scan path"); now the filter path drives the SAME exhaustive
-- membership computation the scorer runs, so it honors the wildcard expansion by
-- construction. array_agg(id ORDER BY id) makes both sides SET-comparable.
SELECT (SELECT array_agg(id ORDER BY id) FROM wc WHERE body @@@ bm25_wildcard('body','judg*'))
     = (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM wc WHERE body @@@ bm25_wildcard('body','judg*') ORDER BY body &@@ bm25_wildcard('body','judg*'), id) s)
  AS wild_filter_equals_rank;   -- expect t
-- Pin the bare filter SET (not just equality-of-two-empties): judg* -> {1,2,3}
-- (judge/judgment/judgement, NOT judo) through the non-scoring filter path.
SELECT array_agg(id ORDER BY id) AS wild_filter_set FROM wc WHERE body @@@ bm25_wildcard('body','judg*');  -- expect {1,2,3}
-- Embedded-glob wildcard on the filter path too: wom*n -> {4,5} (NOT wombat).
SELECT array_agg(id ORDER BY id) AS wild_filter_set_womn FROM wc WHERE body @@@ bm25_wildcard('body','wom*n');  -- expect {4,5}

-- min-prefix ERROR: raised at PARSE in bm25_rescan (any forced-index form triggers it).
SELECT id FROM wc WHERE body @@@ bm25_wildcard('body','a*') ORDER BY body &@@ bm25_wildcard('body','a*');  -- expect ERROR: prefix too short (min 3)

-- expansion-cap ERROR: raised in the expander once the distinct set exceeds the cap.
SET bm25_native.wildcard_max_expansions = 1;
SELECT id FROM wc WHERE body @@@ bm25_wildcard('body','judg*') ORDER BY body &@@ bm25_wildcard('body','judg*');  -- expect ERROR: expands to more than 1 terms
RESET bm25_native.wildcard_max_expansions;

-- Double-count discriminator: a stem living in TWO sealed segments must be scored
-- ONCE under a wildcard leaf (the cross-segment dedup invariant). wc2 seals 'judge'
-- (stem 'judg') into segment A, then seals a second 'judge' into segment B, so 'judg'
-- is in BOTH segment dicts. The wildcard judg* score of each doc must equal its plain
-- bm25_match_terms('body','judge') score — a missing dedup would put 'judg' in the OR
-- work list twice and DOUBLE each doc's score. Separate temp tables so each
-- bm25_score() reads its own active scan (per 15_multiseg).
CREATE TABLE wc2 (id int, body text);
INSERT INTO wc2 VALUES (1,'judge');
CREATE INDEX wc2_bm25 ON wc2 USING bm25_native (body);
SELECT bm25_seal('wc2_bm25');            -- seal segment A (dict: judg)
INSERT INTO wc2 VALUES (2,'judge');
SELECT bm25_seal('wc2_bm25');            -- seal segment B (dict: judg again) -> 'judg' in two segments
CREATE TEMP TABLE wc2_wild AS
  SELECT id, round(bm25_score(ctid)::numeric,6) AS s
  FROM wc2 WHERE body @@@ bm25_wildcard('body','judg*') ORDER BY body &@@ bm25_wildcard('body','judg*');
CREATE TEMP TABLE wc2_match AS
  SELECT id, round(bm25_score(ctid)::numeric,6) AS s
  FROM wc2 WHERE body @@@ bm25_match_terms('body','judge') ORDER BY body &@@ bm25_match_terms('body','judge');
SELECT bool_and(w.s = m.s) AS wildcard_scores_once, count(*) AS n
FROM wc2_wild w JOIN wc2_match m USING (id);  -- expect (t, 2)

-- ===========================================================================
-- Task 10 review follow-up: DELETE-tombstone gating for wildcard expansion.
-- A wildcard leaf's expanded OR-set is computed from the segment dict, but
-- each expanded stem's per-doc match still has to honor the SAME live-docs
-- gating a plain term leaf gets -- a stem whose ONLY holder is hard-deleted
-- must not resurrect just because the dict entry (byte-range/glob match)
-- survives the delete. Own small fixture; DELETE + VACUUM per 17_delete.sql's
-- pattern (VACUUM is what actually retires the posting via the AM's own
-- bulkdelete/tombstone callback -- a bare DELETE alone leaves the posting
-- present until vacuum runs).
-- ===========================================================================
CREATE TABLE wcd (id int, body text);
INSERT INTO wcd VALUES (1,'judge'),(2,'judgment'),(3,'judgement'),(4,'judo');
CREATE INDEX wcd_bm25 ON wcd USING bm25_native (body);
SELECT bm25_seal('wcd_bm25');
SET enable_seqscan = off;
-- Pre-delete sanity: judg* -> {1,2,3} (NOT judo), same shape as the wc fixture above.
SELECT array_agg(id ORDER BY id) = ARRAY[1,2,3] AS wcd_predelete_ok
FROM (SELECT id FROM wcd WHERE body @@@ bm25_wildcard('body','judg*') ORDER BY body &@@ bm25_wildcard('body','judg*'), id) s;
RESET enable_seqscan;

DELETE FROM wcd WHERE id = 1;   -- doc 1 ('judge', stem 'judg') is now the ONLY holder of that stem -- tombstone it
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
VACUUM wcd;

SET enable_seqscan = off;
-- Post-delete: judg* must EXCLUDE the tombstoned doc 1, leaving only {2,3}
-- (judgment/judgement -- sibling live terms sharing the 'judg*' prefix, NOT
-- the now-dead 'judg' stem). DISCRIMINATING: a live-docs-gating regression in
-- the wildcard expander would wrongly resurrect doc 1, since its segment-dict
-- entry for 'judg' still exists after the delete -- only the per-doc
-- tombstone bit says it's gone.
SELECT array_agg(id ORDER BY id) = ARRAY[2,3] AS wildcard_excludes_tombstoned
FROM (SELECT id FROM wcd WHERE body @@@ bm25_wildcard('body','judg*') ORDER BY body &@@ bm25_wildcard('body','judg*'), id) s;
RESET enable_seqscan;
DROP TABLE wcd;

-- ===========================================================================
-- XCUT-03 (issue #145): the expander now copies each page instead of locking it
-- ===========================================================================
-- bm25_dict_expand_wildcard used to run its prefix compare, its
-- bm25_glob_match and bm25_wild_add (a MemoryContextAlloc plus a
-- hash_search(HASH_ENTER) that can rehash) with the segment DICT page still
-- SHARE-locked, and its pending pass ran the same work with the pending page
-- SHARE-locked. Both passes now COPY the page under its SHARE lock, release,
-- and decode the private copy -- and the segment pass, which previously went
-- through bm25_seg_dict_iter_*, open-codes its own page walk so it can carry
-- the segment's real seg_gen and a `blk < nblocks` bound that the iterator does
-- not have. (The intermediate design used the iterator's unlock/relock pair per
-- term; that is unsound for a READER on a hot standby without feedback, because
-- the horizon the argument rests on is evaluated on the primary. See
-- bm25_seg_dict_iter_unlock's header.)
--
-- WHAT THIS FIXTURE IS FOR, STATED HONESTLY. The defect was lock-hold time and
-- cancellability, and unlike the debug-SRF case (sql/80, SEGREAD-10) it is NOT
-- latency-testable: the pre-fix uncancellable window here is ONE DICT page's
-- worth of glob matches -- a few hundred microseconds -- so no boundary
-- separates pre- from post-fix without a contrived pathological pattern. What
-- IS worth pinning, and what this section pins, is that the restructured walk
-- returns the same answers: the fixtures above are all single-page (7 docs), so
-- before this section nothing exercised a DICT walk that CROSSES PAGES at all,
-- and the whole change is about how a page boundary is handled.
--
-- IT DISCRIMINATES -- verified by mutation, not assumed. Truncating the walk to
-- a single DICT page (`dblk = InvalidBlockNumber` in place of the nextblk step)
-- moves alf 676 -> 254, mid 876 -> 201, zed 676 -> 0 and mid*cq 26 -> 0. The 254
-- is the per-page entry count derived below, arrived at independently.
--
-- FIXTURE. 676 docs x 3 terms gives 2,028 sorted dict entries in one segment.
-- A dict entry is MAXALIGN(sizeof(BM25DictEntry) + termlen) = MAXALIGN(20 + 6)
-- = 32 bytes, and a page holds ~8,144 content bytes, so that is ~254 entries
-- per page: 8 DICT pages, with each prefix group's accepted range starting and
-- ending mid-page. 200 post-build rows land unsealed in the pending list and
-- span more than one pending page (asserted below), so the pending pass crosses
-- a page boundary too. Terms end in 'q' deliberately: english_stem has no rule
-- for that ending, so the stored dict bytes equal the source bytes and the
-- expected counts are exact rather than stemmer-dependent (verified with
-- bm25_debug_tokenize, per this file's house rule -- 'alfaae' would stem to
-- 'alfaa', 'alfaaq' does not stem at all).
--
-- Group sizes are 676 and 200 so no single expansion exceeds the default
-- bm25_native.wildcard_max_expansions of 1000. That cap counts distinct TERMS, not
-- matching docs: 'mid*' expands to 676 terms (the pending rows all repeat the
-- already-sealed 'midaaq', which dedups) while matching 876 docs. 676 is the number
-- that has to stay under 1000.
CREATE TABLE wcspan (id int, body text);
INSERT INTO wcspan
SELECT g, 'alf' || chr(97 + (g / 26)) || chr(97 + (g % 26)) || 'q '
       || 'mid' || chr(97 + (g / 26)) || chr(97 + (g % 26)) || 'q '
       || 'zed' || chr(97 + (g / 26)) || chr(97 + (g % 26)) || 'q'
FROM generate_series(0, 675) g;
CREATE INDEX wcspan_idx ON wcspan USING bm25_native (body);
-- Post-build rows: unsealed, so the expander's PENDING pass sees them. Each
-- also repeats the sealed term 'midaaq', so 'mid*' exercises the cross-source
-- dedup with the pending page copy in play.
INSERT INTO wcspan
SELECT 1000 + k, 'pen' || chr(97 + (k / 20)) || chr(97 + (k % 20)) || 'q midaaq'
FROM generate_series(0, 199) k;

-- Fixture witnesses: the dict really is multi-page (2,028 entries at 32 bytes
-- each cannot fit one 8 KB page), and the pending list really is multi-page.
SELECT count(*) AS sealed_dict_entries FROM bm25_debug_segterms('wcspan_idx');
SELECT bm25_debug_pending_nth_page('wcspan_idx', 1) IS NOT NULL AS pending_spans_pages;

SET enable_seqscan = off;
-- Accepted range spans several DICT pages: every group returns its whole 676.
SELECT count(*) AS alf_docs FROM wcspan WHERE body @@@ bm25_wildcard('body','alf*');
-- 876 = 676 sealed + 200 pending (each pending row also carries 'midaaq', which
-- is already in the sealed dict -- deduped to one expansion term, and the doc
-- set is unaffected either way).
SELECT count(*) AS mid_docs FROM wcspan WHERE body @@@ bm25_wildcard('body','mid*');
SELECT count(*) AS zed_docs FROM wcspan WHERE body @@@ bm25_wildcard('body','zed*');
-- Pending-only prefix: 200, and reachable ONLY through the copied pending pages.
SELECT count(*) AS pen_docs FROM wcspan WHERE body @@@ bm25_wildcard('body','pen*');
-- 'aaa' sorts before EVERY dict entry, so the very first entry examined trips
-- the past-the-prefix-range early stop -- the entry loop breaks on its first
-- iteration and the page walk stops with it, without reading a second page.
SELECT count(*) AS before_all FROM wcspan WHERE body @@@ bm25_wildcard('body','aaa*');
-- 'zzz' sorts after every entry: a full multi-page walk that accepts nothing
-- and never early-stops.
SELECT count(*) AS after_all FROM wcspan WHERE body @@@ bm25_wildcard('body','zzz*');
-- Embedded glob (bm25_glob_match runs per term, now against the PAGE COPY):
-- mid*cq matches 'mid<x>cq' for each of the 26 first letters, spread across
-- page boundaries.
SELECT count(*) AS mid_glob_docs FROM wcspan WHERE body @@@ bm25_wildcard('body','mid*cq');
-- Same set through the SCORING path, not just the filter path, so the expander
-- is exercised from bm25_rescan's ranked entry too.
SELECT (SELECT array_agg(id ORDER BY id) FROM wcspan WHERE body @@@ bm25_wildcard('body','midaa*'))
     = (SELECT array_agg(id ORDER BY id) FROM (SELECT id FROM wcspan WHERE body @@@ bm25_wildcard('body','midaa*') ORDER BY body &@@ bm25_wildcard('body','midaa*'), id) s)
  AS span_filter_equals_rank;
RESET enable_seqscan;
DROP TABLE wcspan;

RESET enable_seqscan;
DROP TABLE wc2; DROP TABLE wc; DROP EXTENSION bm25_native;
