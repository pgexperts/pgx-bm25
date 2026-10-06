-- 98_unbounded_input_loops: the two survivors of the 2026-07 review (#58, #68).
--
-- Both are "work driven by user-supplied input, with a bound that is either absent or
-- applied too late". They are what is left after the validating-accessor layer retired
-- the other 29 findings in those two issues.
CREATE EXTENSION bm25_native;

-- ------------------------------------------------------------ accumulator term key
-- The accumulator's dynahash keyed on the first ACCUM_KEY_MAX-1 = 255 bytes of a term,
-- so the key was a PREFIX and collisions were selectable by whoever supplies the text.
-- N distinct terms sharing a 255-byte prefix all landed on one hash entry, and every
-- lookup past the first fell into an O(nterms) linear memcmp fallback -- making indexing
-- O(N^2), with no CHECK_FOR_INTERRUPTS anywhere in the file, so it was also unkillable.
-- The BM25_MAX_TERM_BYTES cap (2047) does not help: it is eight times the key width, so
-- the whole collision class fits inside what a legal document may contain.
--
-- A long term then keyed on a hash of its FULL bytes, tagged so hashed and verbatim keys
-- could not coincide (ADR 0076). #305 replaced that in turn: the key is now (pointer,
-- length) over the whole term, with no fallback -- see 147_accum_full_term_key. The
-- checks below still guard the current key; the boundary block is historical.
--
-- WHAT THIS SUITE PROVES: nothing about the bug, and that is not a flaw in the suite.
-- The pre-fix defect was never CORRUPTION -- both the hash-hit path and the linear
-- fallback full-memcmp the whole term, so colliding terms were always resolved to the
-- right entry. It was correct-but-quadratic, and unkillable. So every assertion below
-- passes byte-identically against the pre-fix code (verified by A/B build), and no SQL
-- assertion could do otherwise: complexity and cancellability are not observable in
-- output, timing assertions are flaky on shared CI, and this repo keeps latency work in
-- the dedicated interrupt suites. These are REGRESSION GUARDS on the new key's
-- correctness -- which is the thing the fix could plausibly have broken -- not evidence
-- the fix was needed.
CREATE TABLE pfx (id int primary key, body text);

-- 300 distinct terms sharing a 300-byte prefix: past the 255-byte key width, so under
-- the old key every one of them hashed identically.
INSERT INTO pfx SELECT 1, string_agg(repeat('p', 300) || 's' || g, ' ')
  FROM generate_series(1, 300) g;
CREATE INDEX pfx_bm ON pfx USING bm25_native (body);

-- Each is individually findable: they were not merged into one term.
SELECT count(*) AS finds_s7   FROM pfx WHERE body @@@ (repeat('p',300) || 's7');
SELECT count(*) AS finds_s250 FROM pfx WHERE body @@@ (repeat('p',300) || 's250');
-- And a term sharing the same 300-byte prefix that was never indexed does NOT match.
-- This would fail if the NEW key merged the prefix group -- it is a guard on the fix,
-- not a reproduction of the bug, which never merged anything.
SELECT count(*) AS no_such    FROM pfx WHERE body @@@ (repeat('p',300) || 's999');
-- All 300 resolve, none lost.
SELECT count(*) AS all_found FROM generate_series(1,300) g
 WHERE EXISTS (SELECT 1 FROM pfx WHERE body @@@ (repeat('p',300) || 's' || g));

-- Short terms still take the verbatim (exact, never-colliding) key path.
CREATE TABLE shortt (id int primary key, body text);
INSERT INTO shortt VALUES (1, 'alpha beta gamma'), (2, 'alpha delta');
CREATE INDEX shortt_bm ON shortt USING bm25_native (body);
SELECT id FROM shortt WHERE body @@@ 'alpha' ORDER BY id;
SELECT id FROM shortt WHERE body @@@ 'delta' ORDER BY id;
-- The verbatim/hashed boundary, on both sides of where it actually fell. accum_make_key
-- took the verbatim branch iff termlen < ACCUM_KEY_MAX - 1, so 254 was the LAST verbatim
-- length and 255 the FIRST hashed one -- not 255/256, which is what an earlier version
-- of this comment claimed and which would have left the verbatim side of the real
-- boundary untested. The off-by-one was load-bearing: the long-term tag byte was 0xFF = 255,
-- so a 255-byte verbatim key would write buf[0] = 255 and alias the tag exactly.
CREATE TABLE bnd (id int primary key, body text);
INSERT INTO bnd VALUES (1, repeat('a', 253)), (2, repeat('b', 254)),
                       (3, repeat('c', 255)), (4, repeat('d', 256));
CREATE INDEX bnd_bm ON bnd USING bm25_native (body);
SELECT id FROM bnd WHERE body @@@ repeat('a', 253);   -- verbatim
SELECT id FROM bnd WHERE body @@@ repeat('b', 254);   -- verbatim, last one
SELECT id FROM bnd WHERE body @@@ repeat('c', 255);   -- hashed, first one
SELECT id FROM bnd WHERE body @@@ repeat('d', 256);   -- hashed

-- ------------------------------------------------------------ jsonb leaf cap timing
-- BM25_QUERY_MAX_LEAVES was enforced only in flatten_recurse, which runs AFTER parse has
-- materialized the whole tree. Depth was bounded; BREADTH was not, so a wide-but-shallow
-- value allocated every node before anything objected. The cap now also runs as each
-- leaf is parsed.
--
-- Through the scan path, for a query invalid ONLY on leaf count, this is not a new
-- observable output: the parse-time check reuses flatten's message and SQLSTATE, and
-- the same queries are legal and rejected either way, just sooner. That is not the
-- whole story -- a query invalid for more than one reason can now report the leaf cap
-- where it used to report a later-detected error, sometimes with a different SQLSTATE
-- (ADR 0099) -- but the queries below are invalid only on leaf count, so that
-- reordering has no visible effect here; this just asserts the cap still holds at the
-- boundary.
\set VERBOSITY terse
-- 64 leaves is legal.
SELECT count(*) AS at_cap FROM shortt
 WHERE body @@@ jsonb_build_object('boolean', jsonb_build_object('should',
         (SELECT jsonb_agg(jsonb_build_object('match',
                 jsonb_build_object('field','body','terms','alpha')))
            FROM generate_series(1, 64))));
-- 65 is not.
SELECT count(*) FROM shortt
 WHERE body @@@ jsonb_build_object('boolean', jsonb_build_object('should',
         (SELECT jsonb_agg(jsonb_build_object('match',
                 jsonb_build_object('field','body','terms','alpha')))
            FROM generate_series(1, 65))));
-- Nor is a very wide one, which is the shape that used to allocate first and object
-- afterwards.
SELECT count(*) FROM shortt
 WHERE body @@@ jsonb_build_object('boolean', jsonb_build_object('should',
         (SELECT jsonb_agg(jsonb_build_object('match',
                 jsonb_build_object('field','body','terms','alpha')))
            FROM generate_series(1, 5000))));
-- bm25_debug_query_parse renders a tree WITHOUT flattening it, so it had no leaf cap at
-- all before this change and would render an arbitrarily wide one. It now shares the
-- parse-time cap. That IS an observable change to a shipped function -- the one place
-- the "same queries legal, same rejected" claim does not hold -- so it is asserted here
-- rather than left to be discovered.
SELECT length(bm25_debug_query_parse('shortt_bm',
         jsonb_build_object('boolean', jsonb_build_object('should',
           (SELECT jsonb_agg(jsonb_build_object('match',
                   jsonb_build_object('field','body','terms','alpha')))
              FROM generate_series(1, 64)))))) > 0 AS parse_64_ok;
SELECT bm25_debug_query_parse('shortt_bm',
         jsonb_build_object('boolean', jsonb_build_object('should',
           (SELECT jsonb_agg(jsonb_build_object('match',
                   jsonb_build_object('field','body','terms','alpha')))
              FROM generate_series(1, 65)))));
\set VERBOSITY default

DROP TABLE bnd;
DROP TABLE shortt;
DROP TABLE pfx;
DROP EXTENSION bm25_native;
