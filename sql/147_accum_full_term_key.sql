-- 147_accum_full_term_key: the accumulator's term map keys on the FULL term (#305
-- PEND-02 / PEND-06, superseding ADR 0076).
--
-- The map used to key a term of 255+ bytes on a 32-bit hash_bytes of its bytes, with a
-- linear scan of every term as the fallback when two long terms shared a hash. ADR 0076
-- argued nobody could aim that collision. They can: a birthday search over ~300k
-- candidate words finds colliding pairs in about a second of SQL. When the second word
-- of a pair arrived it took over the shared entry, and every later occurrence of the
-- FIRST word scanned terms[] from the start: O(occurrences x position of that word)
-- inside a build, a seal or a merge. Now the key is (pointer, length) over the whole
-- term, hashed and compared over every byte, and the fallback is gone.
--
-- The pair below was found offline with
--   SELECT array_agg(i) FROM (SELECT i, repeat('b',270) || translate(md5(i::text),
--     '0123456789','ghijklmnop') || 'qz' AS t FROM generate_series(1,300000) i) w
--   GROUP BY hashtext(t) HAVING count(*) > 1;
-- and is hardcoded so the suite does not repeat the search. hashtext over a
-- deterministic collation IS hash_bytes over the bytes, the function the old key used.
-- 304 letters-only bytes, 'qz'-final so the stemmer leaves the words alone.
CREATE EXTENSION bm25_native;

CREATE TEMP VIEW pair AS
  SELECT repeat('b',270) || translate(md5('3991'),  '0123456789','ghijklmnop') || 'qz' AS a,
         repeat('b',270) || translate(md5('216950'),'0123456789','ghijklmnop') || 'qz' AS b;

-- ------------------------------------------------------------ preconditions
-- If any of these flips -- a different hash, a different tokenizer, a different
-- endianness -- the rest of the suite no longer exercises a colliding pair and would
-- pass vacuously. They are what keeps it honest.
SELECT hashtext(a COLLATE "C") = hashtext(b COLLATE "C") AS pair_collides,
       a <> b                                            AS pair_distinct,
       length(a) = 304 AND length(b) = 304               AS both_long,
       bm25_debug_tokenize(a) = ARRAY[a]
         AND bm25_debug_tokenize(b) = ARRAY[b]           AS tokenizer_keeps_both
  FROM pair;

-- ------------------------------------------------------------ exactness, all three feeders
-- Exact under the old key too (both of its paths memcmp'd the whole term), so these are
-- guards on the NEW key -- the thing deleting the fallback could break -- not the
-- regression test, which is the timing block below.
--
-- The accumulator directly: a in docs 0 and 1, b in docs 0 and 2, distinct tf. Each
-- word keeps its own postings.
SELECT CASE term WHEN p.a THEN 'a' WHEN p.b THEN 'b' END AS which, df, local_docid, tf
  FROM pair p,
       bm25_debug_accum(ARRAY[p.a || ' ' || p.b,
                              p.a || ' ' || p.a,
                              p.b || ' ' || p.b || ' ' || p.b]) d
 WHERE term IN (p.a, p.b)
 ORDER BY which, local_docid;

-- The build path (ambuild), then the drain (seal) and merge-replay paths, each fed the
-- pair in both orders. Expected counts: 'a' alone, 'b' alone, both together.
CREATE TABLE ak (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO ak SELECT 1, a || ' ' || b FROM pair;
INSERT INTO ak SELECT 2, a FROM pair;
INSERT INTO ak SELECT 3, b || ' ' || b FROM pair;
CREATE INDEX ak_bm ON ak USING bm25_native (body);
SELECT (SELECT array_agg(id ORDER BY id) FROM ak WHERE body @@@ (SELECT a FROM pair)) AS a_ids,
       (SELECT array_agg(id ORDER BY id) FROM ak WHERE body @@@ (SELECT b FROM pair)) AS b_ids;

INSERT INTO ak SELECT 4, b || ' ' || a FROM pair;
INSERT INTO ak SELECT 5, b FROM pair;
SELECT bm25_seal('ak_bm');
SELECT (SELECT array_agg(id ORDER BY id) FROM ak WHERE body @@@ (SELECT a FROM pair)) AS a_ids,
       (SELECT array_agg(id ORDER BY id) FROM ak WHERE body @@@ (SELECT b FROM pair)) AS b_ids;

-- Two more one-document segments: the merge selector wants four segments in one size
-- layer before it merges anything.
INSERT INTO ak SELECT 6, a FROM pair;
SELECT bm25_seal('ak_bm');
INSERT INTO ak SELECT 7, a || ' ' || b || ' ' || a FROM pair;
SELECT bm25_seal('ak_bm');
SELECT nsegs AS nsegs_before_merge FROM bm25_stats('ak_bm');
SELECT bm25_merge('ak_bm');
SELECT nsegs AS nsegs_after_merge FROM bm25_stats('ak_bm');
SELECT (SELECT array_agg(id ORDER BY id) FROM ak WHERE body @@@ (SELECT a FROM pair)) AS a_ids,
       (SELECT array_agg(id ORDER BY id) FROM ak WHERE body @@@ (SELECT b FROM pair)) AS b_ids;
-- Phrase evaluation reads positions, which merge replays through a second lookup of the
-- same term (bm25_accum_add_positions_to_last): a misfiled lookup would desync it.
SELECT array_agg(id ORDER BY id) AS b_then_a_phrase
  FROM ak WHERE body @@@ (SELECT bm25_phrase('body', b || ' ' || a) FROM pair);
DROP TABLE ak;

-- ------------------------------------------------------------ the regression test
-- Amplified so the old cost is unmistakable: 50,000 distinct filler words of the SAME
-- length as 'a' and sharing its first 298 bytes, indexed before it, so every fallback
-- iteration paid a ~300-byte memcmp ('zq'-final where 'a' is 'qz'-final, so no filler
-- can equal it); then 'a' and 'b' together (b takes over the shared
-- entry); then 40,000 more occurrences of 'a' in one document. Under the old key each
-- of those scanned all 50,000 fillers: 2e9 long compares, 62 s on the machine that
-- wrote this (cassert PG 18, Apple silicon), against ~0.5 s for the same build now --
-- and against the same ~0.5 s for the OLD build when 'b' is swapped for a word that
-- does not collide, so the cost is the collision alone.
--
-- statement_timeout is the assertion, and its direction is chosen so slow hardware
-- cannot fail it falsely: the fixed build finishes ~20x inside the limit (room for a
-- sanitizer leg on a shared runner), the old one ran ~6x past it. pg_regress does not
-- time statements, so a bound is the only way to make "quadratic" visible in output.
-- If a future machine is fast enough that the old build would finish inside 10 s,
-- raise the filler count rather than lowering the limit.
CREATE TABLE amp (id int PRIMARY KEY, body text) WITH (autovacuum_enabled = off);
INSERT INTO amp
  SELECT d, string_agg(left(p.a, 298)
                       || chr(97 + g / 17576 % 26) || chr(97 + g / 676 % 26)
                       || chr(97 + g / 26 % 26)    || chr(97 + g % 26) || 'zq', ' ')
    FROM pair p, generate_series(0, 49999) g, LATERAL (SELECT g / 20000 + 1 AS d) x
   GROUP BY d;
INSERT INTO amp SELECT 100, a || ' ' || b FROM pair;
INSERT INTO amp SELECT 101, repeat(a || ' ', 40000) FROM pair;
-- The fillers are what the old fallback scanned, so check they really are 50,000
-- distinct 304-byte terms, none of them 'a' (a filler equal to 'a' would put 'a' early
-- in terms[] and shorten every scan to that position).
SELECT count(DISTINCT t) AS fillers, min(length(t)) AS minlen, max(length(t)) AS maxlen,
       bool_or(t = p.a) AS a_among_fillers
  FROM amp, pair p, unnest(bm25_debug_tokenize(body)) t WHERE id < 100;

SET statement_timeout = '10s';
CREATE INDEX amp_bm ON amp USING bm25_native (body);
RESET statement_timeout;
SELECT array_agg(id ORDER BY id) AS a_ids FROM amp WHERE body @@@ (SELECT a FROM pair);
SELECT array_agg(id ORDER BY id) AS b_ids FROM amp WHERE body @@@ (SELECT b FROM pair);
DROP TABLE amp;

DROP VIEW pair;
DROP EXTENSION bm25_native;
