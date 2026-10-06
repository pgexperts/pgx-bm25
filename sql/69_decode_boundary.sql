-- H6 (issue #46): bm25_seg_read.c decoded the posting stream straight off the page
-- and used on-disk counts, lengths and ids as loop bounds, memcpy sizes and array
-- indices without validating any of them. Each site was guarded, if at all, by an
-- Assert -- compiled out in exactly the build where a corrupt page matters.
--
-- bm25_seg_page_validate does NOT cover any of this: it compares the page opaque's
-- seg_gen and nothing else, so a torn page, a bit flip, or a hostile page image in
-- a restored data directory passes it carrying an arbitrary block header.
--
-- The fix is one decode boundary -- bm25_block_validate + a bounded varbyte decoder
-- + a bounded impact-table decoder + a segment-header field_count guard -- so no
-- raw on-disk quantity reaches a bound, a size, or an index unchecked.
--
-- These probes run the SAME validators over caller-supplied values, because a
-- regression suite cannot produce a corrupt page. The positive direction -- that
-- the guards do not reject anything the writer actually emits -- is covered by the
-- other 74 suites, every one of which decodes real blocks through this path.
CREATE EXTENSION bm25_native;

-- ------------------------------------------------------------ varbyte decoder
-- Byte-oriented wire format, so these bytes mean the same on any host.
-- Well-formed: 0x00 -> 0, 0x7f -> 127, 0x80 0x01 -> 128.
SELECT bm25_debug_varbyte_decode_bytes('\x00'::bytea)     AS v_zero,
       bm25_debug_varbyte_decode_bytes('\x7f'::bytea)     AS v_127,
       bm25_debug_varbyte_decode_bytes('\x8001'::bytea)   AS v_128;

-- The whole uint32 range still round-trips through the bounded decoder.
SELECT bm25_debug_varbyte_decode_bytes('\xffffffff0f'::bytea) AS v_uint32_max;

-- Truncated stream: every byte has the continuation bit, so the decoder runs out
-- of input. This is the case that used to walk off the end of the page.
SELECT bm25_debug_varbyte_decode_bytes('\x80'::bytea);
SELECT bm25_debug_varbyte_decode_bytes('\x808080'::bytea);

-- Over-wide sequence: six continuation bytes. Besides being a wrong answer, the
-- old decoder's `shift` reached 35 here, which is undefined behaviour for a uint32.
SELECT bm25_debug_varbyte_decode_bytes('\x808080808080'::bytea);

-- ------------------------------------------------------------ block header
-- A well-formed single-field block: 3 docs, one byte per docid and per tf, no RLE,
-- a 1-field impact table (1 + 9). 16 + 3 + 3 + 0 + 10 = 32 bytes.
SELECT bm25_debug_block_validate(3, 3, 3, 0, 10, 8000) AS block_len;

-- ndocs beyond the builder's cap. This is the stack smash: blk_fields[] is a fixed
-- 128-entry array and the RLE fill wrote ndocs entries into it, so 65535 wrote
-- 256 KB over a 512-byte frame.
SELECT bm25_debug_block_validate(65535, 65535, 65535, 0, 10, 8000);
SELECT bm25_debug_block_validate(129, 129, 129, 0, 10, 8000);
-- ndocs 0 is not something encode_block can emit either (it asserts n > 0).
SELECT bm25_debug_block_validate(0, 0, 0, 0, 10, 8000);

-- Run lengths inconsistent with ndocs. Every posting varbyte-encodes to at least
-- one byte and at most five, so these bracket what the writer can produce; a run
-- shorter than ndocs means the decode would read into the next run.
SELECT bm25_debug_block_validate(10, 9, 10, 0, 10, 8000);      -- docid run too short
SELECT bm25_debug_block_validate(10, 51, 10, 0, 10, 8000);     -- docid run too long
SELECT bm25_debug_block_validate(10, 10, 9, 0, 10, 8000);      -- tf run too short

-- Block overruns the page. The four uint16 run lengths sum to at most 262140, so
-- this is how the decode cursors ended up ~250 KB past an 8 KB page. Uses the RLE
-- length, which ndocs does not bound, so the page check is what fires rather than
-- the run-length check above.
--
-- This case used to pass impact_bytes := 65535 as well. It cannot any more: the
-- impact-table bound below now rejects that value BEFORE the page arithmetic runs,
-- so the overrun check would never be reached and this assertion would silently
-- stop testing what it names. field_rle_bytes alone still overruns the page
-- (16 + 3 + 3 + 65535 + 10 = 65567), so the page check is reached with every other
-- field well-formed -- which is the stronger version of the same test.
SELECT bm25_debug_block_validate(3, 3, 3, 65535, 10, 8000);

-- impact_bytes has its own bound now (SEGREAD-08). Zero is the damaging value and
-- was the one that used to pass: it decodes to a zero-field table, bm25_block_ub
-- returns 0.0, and the WAND driver prunes every posting in the block -- silently
-- missing rows on an ordinary ranked query rather than an error. The upper bound is
-- one nfields byte plus a full BM25_MAX_FIELDS table (1 + 32*9 = 289).
SELECT bm25_debug_block_validate(3, 3, 3, 0, 0, 8000);         -- impact table absent
SELECT bm25_debug_block_validate(3, 3, 3, 0, 290, 8000);       -- one field too many
SELECT bm25_debug_block_validate(3, 3, 3, 0, 289, 8000) AS impact_max_ok;
SELECT bm25_debug_block_validate(3, 3, 3, 0, 1, 8000) AS impact_min_ok;
-- Exactly at the boundary is fine; one byte over is not.
SELECT bm25_debug_block_validate(3, 3, 3, 0, 10, 32) AS exactly_fits;
SELECT bm25_debug_block_validate(3, 3, 3, 0, 10, 31);

-- ------------------------------------------------------------ impact table
-- Only the leading nfields byte and the total length are validated, and both are
-- endian-independent. Well-formed: 1 field -> 1 + 9 bytes.
SELECT bm25_debug_impact_decode_bytes('\x01'::bytea || decode(repeat('00', 9), 'hex')) AS nfields_1;
-- Zero-length table is tolerated (pre-v5 pages), yielding a zero-field table.
SELECT bm25_debug_impact_decode_bytes('\x'::bytea) AS nfields_empty;

-- nfields past BM25_MAX_FIELDS. The decoder writes into a caller's STACK
-- BM25BlockImpact, so 255 fields wrote ~2.7 KB past a 388-byte struct on the WAND
-- open path -- reached for the first block of every term on a ranked query.
SELECT bm25_debug_impact_decode_bytes('\xff'::bytea || decode(repeat('00', 2295), 'hex'));

-- nfields plausible but not matching the table's actual length: the stronger of
-- the two checks, since it catches a small nfields the block header does not
-- account for.
SELECT bm25_debug_impact_decode_bytes('\x02'::bytea || decode(repeat('00', 9), 'hex'));
SELECT bm25_debug_impact_decode_bytes('\x01'::bytea || decode(repeat('00', 20), 'hex'));

-- ------------------------------------------------------------ still decodes
-- Positive control: a real index still builds, seals, scans and ranks through all
-- of the above. If any guard were too strict this would fail rather than the
-- negative cases passing.
CREATE TABLE dbz (id int PRIMARY KEY, title text, body text);
INSERT INTO dbz SELECT g, 'title ' || g, 'alpha beta gamma ' || g FROM generate_series(1, 300) g;
-- One unambiguously top-ranked row, so the assertion below does not depend on how
-- equal-scoring rows tie-break.
INSERT INTO dbz VALUES (999, 'title top', 'alpha alpha alpha alpha alpha');
CREATE INDEX dbz_idx ON dbz USING bm25_native (title, body) WITH (store_positions = true);
SELECT bm25_seal('dbz_idx');
SET enable_seqscan = off;

-- The @@@ / &@@ anchor is `title`, the index's FIRST column. Anchoring on `body`
-- produces no Index Cond, so the planner takes a Seq Scan even with
-- enable_seqscan=off (it is only discouraged, not forbidden) and the inert
-- bm25_match recheck answers instead of the index -- which silently returned 301
-- phrase matches out of 300 and ranked the wrong row first while drafting this.
-- The scope still lives in the RHS text, per 32_field_query.
EXPLAIN (COSTS OFF)
SELECT id FROM dbz WHERE title @@@ 'alpha' ORDER BY title &@@ 'alpha' LIMIT 1;

SELECT count(*) AS alpha_matches FROM dbz WHERE title @@@ 'alpha';
-- Row 999 carries 'alpha' five times against everyone else's one, so it is
-- unambiguously first and this does not depend on tie-breaking.
SELECT id AS top_ranked FROM dbz WHERE title @@@ 'alpha' ORDER BY title &@@ 'alpha' LIMIT 1;
-- 300, not 301: row 999 has no 'beta', so the phrase must exclude it.
SELECT count(*) AS phrase_matches FROM
  (SELECT id FROM dbz WHERE title @@@ '"alpha beta"' ORDER BY title &@@ '"alpha beta"') s;
RESET enable_seqscan;

DROP TABLE dbz;
DROP EXTENSION bm25_native;
