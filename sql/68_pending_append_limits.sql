-- H7 (issue #47): the pending append de-duplicated a document's tokens with a
-- linear scan over every entry seen so far, and pre-sized EVERY distinct entry's
-- position array to the whole field's token count -- both O(ntok^2) in a value the
-- inserting user controls, and both incurred BEFORE the size check that was
-- guaranteed to reject such a document anyway. The report measured a 200 KB value
-- (~28k mostly-distinct tokens) costing ~4x10^8 comparisons and ~3.1 GB.
--
-- Fixed three ways: a cheap lower-bound size check before any per-token work, an
-- HTAB keyed on (field_id, term bytes) replacing the linear scan, and geometric
-- growth of each entry's position array.
--
-- Measured, 8000 distinct tokens (steady state, PG 18.3): 90 ms -> 7.4 ms.
--
-- Most of that is not observable in expected output -- an over-large document
-- ended in the same ERROR before and after, just far more expensively. What IS
-- observable is the silent-corruption path the size pre-check closes, which is
-- what this suite pins.
CREATE EXTENSION bm25_native;

CREATE TABLE pal (id int, body text);
CREATE INDEX pal_idx ON pal USING bm25_native (body);

-- ---------------------------------------------------------------- tf wrap
-- e_tf is uint16. A term occurring exactly 65536 times in one field used to wrap
-- the counter to 0 during de-dup, producing a zero-length position blob and so a
-- SMALL `need` -- the document was ACCEPTED. Observed on the pre-fix build:
--
--     INSERT                     -> success
--     ... WHERE body @@@ 'alpha' -> 1   (matches while pending)
--     bm25_seal(...)
--     ... WHERE body @@@ 'alpha' -> 0   (silently gone, permanently)
--
-- A row present in the table and unfindable through its index, with the loss
-- landing at seal time rather than at INSERT. The size pre-check caps total_tok
-- far below 65535, so the counter can no longer reach the wrap and the document
-- is refused loudly, at INSERT, like any other oversized one.
INSERT INTO pal SELECT 1, trim(repeat('alpha ', 65536));
SELECT count(*) AS rows_after_rejected_insert FROM pal;

-- ---------------------------------------------------------------- size guard
-- The report's repro: ~28k mostly-distinct tokens. This used to be REJECTED (the
-- one-document-one-page ceiling); since v7 a document spans pages, so it is now
-- ACCEPTED and must be searchable. What the H7 fix is still doing here is bounding the
-- per-token work: the de-dup is an HTAB rather than a linear first-seen scan, and each
-- entry's position array grows geometrically, so 28k tokens cost ~28k operations and
-- ~O(T) bytes instead of ~4x10^8 comparisons and ~3.1 GB.
INSERT INTO pal SELECT 2, string_agg('w' || g::text, ' ') FROM generate_series(1, 28000) g;
SELECT count(*) AS spanning_doc_searchable FROM pal WHERE body @@@ 'w27999';

-- ---------------------------------------------------------------- still works
-- The guard must not have moved the accept/reject boundary for ordinary
-- documents. A few hundred distinct terms is well inside a pending page.
INSERT INTO pal SELECT 3, string_agg('term' || g::text, ' ') FROM generate_series(1, 200) g;
SELECT array_agg(id) AS found FROM pal WHERE body @@@ 'term7';

-- A high-multiplicity term (one entry, long position list) exercises the
-- geometric growth of that entry's position array across many doublings.
INSERT INTO pal SELECT 4, trim(repeat('beta ', 2000));
SELECT array_agg(id) AS found_beta FROM pal WHERE body @@@ 'beta';

-- ---------------------------------------------------------------- de-dup
-- The HTAB replaced the linear first-seen scan, so pin what de-dup must still
-- produce: correct tf aggregation, correct positions (phrase search reads them),
-- and per-FIELD keying -- the same term in two columns stays two entries.
-- enable_seqscan=off and the field:term RHS idiom, per 32_field_query: the LHS
-- column is only the opclass anchor, the scope lives in the query text. A seqscan
-- would hit the inert bm25_match recheck, which ignores both field scope and
-- phrase syntax -- these assertions would then pass vacuously.
CREATE TABLE pmf (id int, title text, body text);
CREATE INDEX pmf_idx ON pmf USING bm25_native (title, body)
  WITH (store_positions = true);
INSERT INTO pmf VALUES
  (1, 'alpha beta',  'beta gamma beta'),
  (2, 'gamma delta', 'alpha alpha beta');
SET enable_seqscan = off;

-- All fields: both rows carry each term somewhere.
SELECT array_agg(id ORDER BY id) AS beta_rows  FROM pmf WHERE title @@@ 'beta';
SELECT array_agg(id ORDER BY id) AS alpha_rows FROM pmf WHERE title @@@ 'alpha';

-- De-dup is keyed per FIELD, so the same term in two columns stays two entries
-- and a field-scoped query can tell them apart. 'beta' is in row 1's title and in
-- both rows' bodies.
SELECT array_agg(id ORDER BY id) AS beta_in_title FROM pmf WHERE title @@@ 'title:beta';
SELECT array_agg(id ORDER BY id) AS beta_in_body  FROM pmf WHERE title @@@ 'body:beta';

-- Positions survived de-dup: a phrase matches only where adjacency actually holds.
-- Row 1 body is 'beta gamma beta', so both orders match it; row 2 body is
-- 'alpha alpha beta', which has 'alpha beta' but neither gamma pair.
--
-- Each phrase pairs the @@@ key with an ORDER BY &@@ on the SAME literal, per
-- 38_phrase: a bare @@@ falls to bm25_match, which has no positions and so cannot
-- filter a phrase at all (that is exactly what these returned before -- every row).
SELECT array_agg(id ORDER BY id) AS phrase_beta_gamma FROM
  (SELECT id FROM pmf WHERE title @@@ '"beta gamma"' ORDER BY title &@@ '"beta gamma"') s;
SELECT array_agg(id ORDER BY id) AS phrase_gamma_beta FROM
  (SELECT id FROM pmf WHERE title @@@ '"gamma beta"' ORDER BY title &@@ '"gamma beta"') s;
SELECT array_agg(id ORDER BY id) AS phrase_alpha_beta FROM
  (SELECT id FROM pmf WHERE title @@@ '"alpha beta"' ORDER BY title &@@ '"alpha beta"') s;
SELECT array_agg(id ORDER BY id) AS phrase_none FROM
  (SELECT id FROM pmf WHERE title @@@ '"delta alpha"' ORDER BY title &@@ '"delta alpha"') s;

-- Same answers after a seal drains the pending list into a segment: the drain
-- reconstructs tf and positions from exactly these entries.
SELECT bm25_seal('pmf_idx');
SELECT array_agg(id ORDER BY id) AS beta_in_title_sealed FROM pmf WHERE title @@@ 'title:beta';
SELECT array_agg(id ORDER BY id) AS phrase_sealed FROM
  (SELECT id FROM pmf WHERE title @@@ '"beta gamma"' ORDER BY title &@@ '"beta gamma"') s;
SELECT array_agg(id ORDER BY id) AS phrase_none_sealed FROM
  (SELECT id FROM pmf WHERE title @@@ '"delta alpha"' ORDER BY title &@@ '"delta alpha"') s;
RESET enable_seqscan;

DROP TABLE pmf;
DROP TABLE pal;
DROP EXTENSION bm25_native;
