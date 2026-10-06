-- 96_wal_page_determinism: no uninitialized bytes on a WAL-logged index page (#144).
--
-- This class is invisible to every ordinary assertion. Uninitialized struct padding
-- changes no query result, so a suite that checks answers cannot distinguish a
-- memset-before-fill from its absence -- which is exactly how these survived. What the
-- bytes DO affect is the Generic WAL image: a page carrying stack residue differs
-- between two otherwise identical builds, ships that residue to a standby, and (for the
-- segment catalog) carries it forward into every later merge.
--
-- So the assertions here are on BYTES, via two probes that return an on-page descriptor
-- verbatim.
--
--   BM25RetiredEntry   sizeof 48, hole at [36,40) -- uint32 gen, then 8-aligned xid
--   BM25SegCatEntry    sizeof 40, NO HOLE since ADR 0088
--
-- THE SEGCAT HALF OF THIS SUITE CHANGED SHAPE, and the reason is that the hazard was
-- retired rather than re-guarded. BM25SegCatEntry's 4 bytes at [4,8) used to be
-- implicit padding and were asserted to be zero; they are now the named total_tokens
-- field (ADR 0088), which the writer always assigns. There is no longer a hole in that
-- struct for stack residue to hide in -- the cure the format header's #144 note wished
-- for, and available without a format break because naming a hole moves no other byte.
--
-- The replacement assertion must be against an INDEPENDENTLY KNOWN value, and getting
-- that wrong is easy: comparing the raw bytes against what bm25_debug_segcat reports
-- proves nothing at all, because both read the SAME on-page bytes and would agree on
-- residue just as happily as on data. (Measured: a build with the token counter
-- neutralised passed that version of this assertion while sql/108 failed.)
--
-- So the comparison below is against total_len. This fixture is English and deletes
-- nothing, and for a one-lexeme-per-run dictionary the token count and the run count are
-- the same number by construction -- so total_len is a value derived from the CORPUS
-- rather than from the bytes under test. That restores the residue-sensitivity the
-- retired zero-check had, and adds what it never had: the bytes must be RIGHT, not
-- merely quiet. It also pins survivor preservation across the merge swap's whole-struct
-- copy, which nothing else exercises -- sql/108's merge consumes all its inputs and
-- leaves no survivor.
--
-- Decoded in both byte orders and accepted if either matches, because the probe returns
-- raw on-page bytes and this assertion must hold on a big-endian host too.
--
-- Only the padding, the sizes, and that one derived field are asserted. The other live
-- fields (block numbers, the retire xid) legitimately vary run to run, and pinning them
-- would make this suite fail for reasons that have nothing to do with what it tests.
--
-- HOW MUCH THIS PROVES, stated honestly. The A/B was run: with the merge path's memset
-- removed and the extension rebuilt, these assertions still passed, because the stack
-- slot backing `newentry` happened to hold zeros on this platform and build. So this
-- suite is a REGRESSION GUARD on a stated invariant, not a demonstration that the fix
-- was necessary. It is worth having for the same reason the fix is worth making --
-- "the padding happened to be zero this time" is not a property C gives you, and a
-- different compiler, optimization level or call path can change it -- but the
-- authoritative detectors for this class are MSan and wal_consistency_checking, not a
-- SQL assertion. Do not read a green run here as proof the padding is initialized.
CREATE EXTENSION bm25_native;

CREATE TABLE det (id int primary key, body text);
INSERT INTO det SELECT g, 'alpha beta doc' || g FROM generate_series(1, 50) g;
CREATE INDEX det_bm ON det USING bm25_native (body);

-- ------------------------------------------------------------ struct sizes
-- These are what the new StaticAssertDecls pin at compile time. Asserting them again
-- from SQL catches the case where the header and the running build disagree -- a mixed
-- build, which PGXS makes possible because it does not track header dependencies.
SELECT octet_length(bm25_debug_segcat_entry_bytes('det_bm', 0)) AS segcat_entry_size;

-- ------------------------------------------------------------ seal path
-- The seal builds its catalog entry IN PLACE on a PageInit'd page, so this path was
-- already clean. Asserted anyway: it is the control that shows the probe reports the
-- field correctly for a known-good writer, so a match from the merge path below means
-- something. English fixture, so total_tokens equals total_len here -- this suite is
-- about bytes, not about the run/token distinction (sql/108 covers that).
SELECT bm25_debug_segcat_entry_bytes('det_bm', 0) IS NOT NULL AS seal_entry_readable;

-- ------------------------------------------------------------ merge path
-- The path that was broken. The merge swap (bm25_segcat_publish_swap, via
-- bm25_segcat_entry_from_hdr) builds the entry on the
-- STACK and memcpy's it onto the page, and a memcpy carries padding that an in-place
-- field fill never writes -- so bm25_page_init's zeroing of that page is undone for
-- exactly these four bytes, below pd_lower, inside a GENERIC_XLOG_FULL_IMAGE record.
INSERT INTO det SELECT g, 'gamma delta doc' || g FROM generate_series(51, 100) g;
SELECT bm25_seal('det_bm');
INSERT INTO det SELECT g, 'epsilon doc' || g FROM generate_series(101, 150) g;
SELECT bm25_seal('det_bm');
SELECT bm25_merge('det_bm');

-- EVERY catalog entry, iterated over the live count rather than a hardcoded 0. The
-- merge copies survivors forward through the same stack struct and re-emits them onto
-- the new chain, so a leak propagates to all of them -- but a fixture whose merge
-- consumes every segment leaves no survivor to check, and an assertion over entry 0
-- alone would then be testing the newly built entry twice and the survivor path never.
SELECT count(*) > 1 AS survivors_present FROM bm25_debug_segcat('det_bm');
-- row_number() over bm25_debug_segcat matches the probe's entry index: both walk the
-- catalog chain in on-page order, which is the same assumption the retired-descriptor
-- assertion below already makes with its generate_series.
WITH cat AS (
  SELECT (row_number() OVER ()) - 1 AS n, total_len FROM bm25_debug_segcat('det_bm')
), raw AS (
  SELECT n, total_len,
         substring(bm25_debug_segcat_entry_bytes('det_bm', n::int) FROM 5 FOR 4) AS b
    FROM cat
)
SELECT bool_and(
         get_byte(b,0)::bigint + get_byte(b,1) * 256::bigint
           + get_byte(b,2) * 65536::bigint + get_byte(b,3) * 16777216::bigint = total_len
      OR get_byte(b,3)::bigint + get_byte(b,2) * 256::bigint
           + get_byte(b,1) * 65536::bigint + get_byte(b,0) * 16777216::bigint = total_len
       ) AS all_segcat_token_bytes_match
  FROM raw;

-- ------------------------------------------------------------ retired descriptors
-- BM25RetiredEntry is filled in place on a PageInit'd page, so like the seal path it was
-- not leaking -- but nothing said so, and the compaction path copies these with a
-- whole-struct assignment, which C does not require to copy padding at all. Both are now
-- explicit (memset before fill, memcpy on copy-back).
-- A single merge does not reliably leave descriptors behind -- with no older snapshot
-- holding the horizon back, reclaim can free them in the same call. Two more segments
-- and a second merge does, and the count is asserted below so that if this stops being
-- true the suite fails rather than quietly testing an empty set.
INSERT INTO det SELECT g, 'zeta doc' || g FROM generate_series(151, 200) g;
SELECT bm25_seal('det_bm');
INSERT INTO det SELECT g, 'eta doc' || g FROM generate_series(201, 250) g;
SELECT bm25_seal('det_bm');
SELECT bm25_merge('det_bm');

SELECT bm25_debug_retired_count('det_bm') > 0 AS have_retired_entries;
SELECT octet_length(bm25_debug_retired_entry_bytes('det_bm', 0)) AS retired_entry_size;
SELECT bool_and(
         encode(substring(bm25_debug_retired_entry_bytes('det_bm', n) FROM 37 FOR 4), 'hex')
           = '00000000') AS all_retired_pads_zero
  FROM generate_series(0, bm25_debug_retired_count('det_bm')::int - 1) n;

-- ------------------------------------------------------------ still correct
-- The memsets sit on the seal and merge paths, so confirm they changed no answer.
SET enable_seqscan = off;
SELECT count(*) AS alpha_hits FROM det WHERE body @@@ 'alpha';
SELECT count(*) AS epsilon_hits FROM det WHERE body @@@ 'epsilon';
SELECT count(*) AS eta_hits FROM det WHERE body @@@ 'eta';
SELECT ndocs FROM bm25_debug_global_stats('det_bm');
RESET enable_seqscan;

-- ------------------------------------------------------------ not asserted here
-- Three findings in #144 have no behavioural assertion, stated rather than implied:
--
--   BUILD-07  a zero-posting term's term_post_root/_off were uninitialized when copied
--             into a DICT entry. The fix pre-sets InvalidBlockNumber/0, mirroring the
--             POS pass in the same function. Reachability is NOT established -- the
--             accumulator's own `Max(t->npost, 1u)` anticipates a zero-posting term but
--             nothing in-tree was shown to produce one -- so there is no fixture that
--             would discriminate.
--   SCAN-08   pending walkers read uninitialized per-document scratch when the first
--             record seen is a stranded continuation (ADR 0069: an ordinary cancelled
--             VACUUM produces one). The value is discarded before use, so the defect is
--             the read itself; it is detectable by MSan, not by output.
--   SCAN-06   fcfg[] was partly uninitialized when the metapage field_count exceeds the
--             field-config page's. Needs two disagreeing on-disk counts, which no debug
--             lever can produce.
--
-- The ASan and cassert/UBSan CI legs cover the reads; nothing here does.
DROP TABLE det;
DROP EXTENSION bm25_native;
