-- #57: a document's pending records used to have to fit ONE page, which capped an INSERT
-- at ~380 distinct 7-byte stems while CREATE INDEX had no such limit -- so a row a rebuild
-- indexed happily could be refused by a later INSERT of the same value, and ordinary
-- long-form prose (a 2000-word article is ~600-900 distinct stems) hit it.
--
-- Format v7 removes that ceiling: a document too large for one page is written as several
-- consecutive records that all carry the same TID, the later ones flagged
-- BM25_PENDING_DOC_CONT, and the drain accumulates them into one accumulator doc. This
-- suite was originally written to PIN the ceiling; it now pins its removal, the two
-- narrower limits that replace it, and the properties the multi-part path could plausibly
-- get wrong.
--
-- The exact numbers here are BLCKSZ-dependent (capacity is 8144 at the default 8 KB) and
-- would need re-blessing on a non-default-BLCKSZ build, as would much of the tree.
CREATE EXTENSION bm25_native;

CREATE TABLE pdc (id int, body text);
CREATE INDEX pdc_idx ON pdc USING bm25_native (body);
SET enable_seqscan = off;

-- ---------------------------------------------------------------- ceiling is gone
-- 380 was the old maximum; 381 was the first refusal. Both now succeed, and so does a
-- document an order of magnitude past it. Each must be searchable by its FIRST and LAST
-- term -- the last term is the one that lands on a later part, so a broken continuation
-- shows up as a miss here rather than as an error.
INSERT INTO pdc SELECT 380, (SELECT string_agg('wr'||lpad(g::text,5,'0'), ' ')
                             FROM generate_series(1, 380) g);
INSERT INTO pdc SELECT 381, (SELECT string_agg('xr'||lpad(g::text,5,'0'), ' ')
                             FROM generate_series(1, 381) g);
INSERT INTO pdc SELECT 4000, (SELECT string_agg('yr'||lpad(g::text,5,'0'), ' ')
                              FROM generate_series(1, 4000) g);
SELECT array_agg(id ORDER BY id) AS at_380         FROM pdc WHERE body @@@ 'wr00380';
SELECT array_agg(id ORDER BY id) AS past_380       FROM pdc WHERE body @@@ 'xr00381';
SELECT array_agg(id ORDER BY id) AS spanning_first FROM pdc WHERE body @@@ 'yr00001';
SELECT array_agg(id ORDER BY id) AS spanning_last  FROM pdc WHERE body @@@ 'yr04000';
-- A term from the middle of the 4000-term document, i.e. some interior part.
SELECT array_agg(id ORDER BY id) AS spanning_middle FROM pdc WHERE body @@@ 'yr02000';
-- EVERY term of the spanning document resolves to it, so no part was dropped or
-- mis-attributed: 4000 distinct single-row lookups.
SELECT count(*) AS spanning_all_terms_found FROM (
  SELECT g FROM generate_series(1, 4000) g
  WHERE EXISTS (SELECT 1 FROM pdc
                WHERE body @@@ ('yr' || lpad(g::text,5,'0')) AND id = 4000)) s;

-- ---------------------------------------------------------------- build == insert
-- The asymmetry that made this a bug: CREATE INDEX accepted what INSERT refused. Feed the
-- SAME 4000-term value through both paths and require identical answers.
CREATE TABLE pdcb (id int, body text);
INSERT INTO pdcb SELECT 1, (SELECT string_agg('zr'||lpad(g::text,5,'0'), ' ')
                            FROM generate_series(1, 4000) g);
CREATE INDEX pdcb_idx ON pdcb USING bm25_native (body);        -- build path
INSERT INTO pdcb SELECT 2, (SELECT string_agg('zr'||lpad(g::text,5,'0'), ' ')
                            FROM generate_series(1, 4000) g);  -- insert path
SELECT array_agg(id ORDER BY id) AS both_paths_first FROM pdcb WHERE body @@@ 'zr00001';
SELECT array_agg(id ORDER BY id) AS both_paths_last  FROM pdcb WHERE body @@@ 'zr04000';

-- ---------------------------------------------------------------- seal round-trip
-- The drain is where multi-part reassembly actually happens, and where feeding the
-- accumulator once per PART instead of once per (doc, field) would corrupt things
-- silently: add_field_tokens ASSIGNS doclen_by_field and bumps ndocs_by_field, so a
-- per-part call would leave the last part's length as the whole doc's and count the
-- document several times in that field's N. Both move the score, so pin it across the
-- seal, and pin the segment's own doc count and average length.
-- Both drivers score the pending arm through the same helper (bm25_wand_score_pending
-- delegates to bm25_pending_score_term), so pin both on a SPANNING pending document:
-- the WAND parity suites never build one, and a per-part doclen here would diverge
-- silently.
SET bm25_native.wand_top_k = 0;
SELECT round(bm25_score(ctid)::numeric, 6) AS score_before_seal_exhaustive
  FROM pdcb WHERE body @@@ 'zr00001' ORDER BY body &@@ 'zr00001' LIMIT 1;
SET bm25_native.wand_top_k = 100;
SELECT round(bm25_score(ctid)::numeric, 6) AS score_before_seal_wand
  FROM pdcb WHERE body @@@ 'zr00001' ORDER BY body &@@ 'zr00001' LIMIT 1;
RESET bm25_native.wand_top_k;
SELECT round(bm25_score(ctid)::numeric, 6) AS score_before_seal
  FROM pdcb WHERE body @@@ 'zr00001' ORDER BY body &@@ 'zr00001' LIMIT 1;
SELECT bm25_seal('pdcb_idx');
SELECT array_agg(id ORDER BY id) AS after_seal_first FROM pdcb WHERE body @@@ 'zr00001';
SELECT array_agg(id ORDER BY id) AS after_seal_last  FROM pdcb WHERE body @@@ 'zr04000';
SELECT round(bm25_score(ctid)::numeric, 6) AS score_after_seal
  FROM pdcb WHERE body @@@ 'zr00001' ORDER BY body &@@ 'zr00001' LIMIT 1;
-- One row was built, one inserted, and they carry the identical document: the sealed
-- segment holds exactly the inserted one, counted ONCE, with its full length.
SELECT ndocs, live_ndocs FROM bm25_debug_segcat('pdcb_idx');
SELECT total_len / NULLIF(ndocs, 0) AS avg_doclen FROM bm25_debug_segcat('pdcb_idx');

-- ---------------------------------------------------------------- positions survive
-- Each part carries its own entries' position blobs, positions absolute per entry, so a
-- phrase whose terms landed on DIFFERENT parts must still match -- and a reversed phrase
-- must still not.
CREATE TABLE pdcp (id int, body text);
CREATE INDEX pdcp_idx ON pdcp USING bm25_native (body) WITH (store_positions = true);
INSERT INTO pdcp SELECT 1,
  (SELECT string_agg('pr'||lpad(g::text,5,'0'), ' ') FROM generate_series(1, 1200) g)
  || ' needle haystack';
SELECT array_agg(id ORDER BY id) AS phrase_late_in_spanning FROM
  (SELECT id FROM pdcp WHERE body @@@ '"needle haystack"'
   ORDER BY body &@@ '"needle haystack"') s;
SELECT array_agg(id ORDER BY id) AS phrase_wrong_order FROM
  (SELECT id FROM pdcp WHERE body @@@ '"haystack needle"'
   ORDER BY body &@@ '"haystack needle"') s;
SELECT bm25_seal('pdcp_idx');
SELECT array_agg(id ORDER BY id) AS phrase_after_seal FROM
  (SELECT id FROM pdcp WHERE body @@@ '"needle haystack"'
   ORDER BY body &@@ '"needle haystack"') s;

-- ---------------------------------------------------------------- multi-field spanning
-- De-dup is per field, so a spanning document's parts can carry entries from several
-- fields; a part boundary must not cross-attribute them.
CREATE TABLE pdcf (id int, a text, b text);
CREATE INDEX pdcf_idx ON pdcf USING bm25_native (a, b);
INSERT INTO pdcf SELECT 1,
  (SELECT string_agg('fa'||lpad(g::text,5,'0'), ' ') FROM generate_series(1, 400) g),
  (SELECT string_agg('fb'||lpad(g::text,5,'0'), ' ') FROM generate_series(1, 400) g);
SELECT array_agg(id ORDER BY id) AS field_a_scoped FROM pdcf WHERE a @@@ 'a:fa00400';
SELECT array_agg(id ORDER BY id) AS field_b_scoped FROM pdcf WHERE a @@@ 'b:fb00400';
-- A field-b term must NOT be findable scoped to field a.
SELECT count(*) AS no_cross_attribution FROM pdcf WHERE a @@@ 'a:fb00400';
SELECT bm25_seal('pdcf_idx');
SELECT array_agg(id ORDER BY id) AS field_a_sealed FROM pdcf WHERE a @@@ 'a:fa00400';
SELECT array_agg(id ORDER BY id) AS field_b_sealed FROM pdcf WHERE a @@@ 'b:fb00400';
SELECT count(*) AS no_cross_attribution_sealed FROM pdcf WHERE a @@@ 'a:fb00400';

-- ---------------------------------------------------------------- residual limits
-- Two narrower limits replace the page ceiling, and both must still fail LOUDLY.
--
-- (1) Total tokens must fit uint16, because BM25PendingTermEntry.tf is 16 bits on disk.
-- This is also what keeps H7/#47's tf-wrap unreachable now that the page bound is gone:
-- a term occurring 65536 times would wrap tf to 0, yielding an empty position blob and an
-- ACCEPTED document carrying tf = 0 -- present in the table, unfindable through the index.
INSERT INTO pdc SELECT 99, trim(repeat('alpha ', 65536));
SELECT count(*) AS wrap_doc_absent FROM pdc WHERE id = 99;

-- Just inside the cap IS accepted, provided no single entry gets too big — so spread the
-- tokens over distinct terms. 65000 distinct 8-byte stems is ~1.5 MB of pending records,
-- roughly 200 parts, which is also the deepest spanning case in the suite.
INSERT INTO pdc SELECT 98, (SELECT string_agg('tk'||lpad(g::text,6,'0'), ' ')
                            FROM generate_series(1, 65000) g);
SELECT array_agg(id ORDER BY id) AS deep_span_first FROM pdc WHERE body @@@ 'tk000001';
SELECT array_agg(id ORDER BY id) AS deep_span_last  FROM pdc WHERE body @@@ 'tk065000';
SELECT array_agg(id ORDER BY id) AS deep_span_mid   FROM pdc WHERE body @@@ 'tk032500';

-- (2) One (field, term) entry with its position blob must fit a page: an entry's term
-- bytes and positions are contiguous, so this one genuinely cannot be split. Reachable
-- with a high-MULTIPLICITY term rather than a long document -- 20000 occurrences of one
-- term encode to a position blob past a page. Note this fires BEFORE limit (1) is
-- relevant: the two limits catch different shapes, repeated vs distinct.
INSERT INTO pdc SELECT 97, trim(repeat('gamma ', 20000));
SELECT count(*) AS oversize_entry_absent FROM pdc WHERE id = 97;

-- The index is still healthy after both refusals.
SELECT array_agg(id ORDER BY id) AS healthy_after_refusals FROM pdc WHERE body @@@ 'wr00380';

RESET enable_seqscan;
DROP TABLE pdcf;
DROP TABLE pdcp;
DROP TABLE pdcb;
DROP TABLE pdc;
DROP EXTENSION bm25_native;
