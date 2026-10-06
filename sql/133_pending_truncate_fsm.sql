-- 133_pending_truncate_fsm: the pages an INSERT-path seal recycles must be findable
-- in the free space map afterwards (issue #300, PEND-07).
--
-- bm25_pending_truncate frees each drained pending page with RecordFreeIndexPage,
-- which writes only that block's FSM LEAF. bm25_page_alloc finds free pages with
-- GetFreeIndexPage, which searches DOWN from the FSM root, so a freed page is
-- invisible to it until an FSM vacuum has propagated the leaf into the upper
-- levels. The truncate used to do that with IndexFreeSpaceMapVacuum -- every FSM
-- page of the relation, under the seal singleton every document-adding insert
-- waits on. It now vacuums only the range of blocks it freed
-- (FreeSpaceMapVacuumRange). This suite pins that the narrower call still does the
-- job on the path that matters: aminsert's opportunistic seal, with no VACUUM
-- anywhere in between to propagate the leaves instead.
--
-- WHAT FAILS WITHOUT IT. Delete the truncate's FSM vacuum entirely and the second
-- batch below cannot find the recycled pages: every new pending page extends the
-- relation (reused_not_extended = f, recycled_pages_consumed = f). So does a
-- range that misses the freed blocks' FSM leaf page entirely, or an empty range.
-- A range off by a block or two INSIDE that leaf page still passes (the vacuum
-- refreshes the whole leaf page's parent slot), so at this suite's size it cannot
-- catch an off-by-one end; it is a guard on the narrowed call, not a bound check.
--
-- WHY THE WAIT AND THE BURN. Recycled pending pages carry a real retire_xid (issue
-- #135, suite 89), and bm25_page_alloc requeues -- rather than reuses -- a page
-- whose horizon has not cleared, then extends. Without driving the horizon past
-- the stamp, the second batch would extend for that reason and the assertion would
-- say nothing about the FSM. Same recipe as 18_vacuum_reclaim and 20_merge_reclaim.
CREATE EXTENSION bm25_native;

CREATE FUNCTION pg_temp.wait_for_xmin_horizon() RETURNS void AS $$
DECLARE
  deadline timestamptz := clock_timestamp() + interval '30 seconds';
BEGIN
  LOOP
    PERFORM pg_stat_clear_snapshot();
    EXIT WHEN NOT EXISTS (
      SELECT 1 FROM pg_stat_activity
       WHERE datname = current_database()
         AND pid <> pg_backend_pid()
         AND backend_xmin IS NOT NULL);
    IF clock_timestamp() > deadline THEN
      RAISE EXCEPTION
        'xmin horizon still held by another backend after 30s; pages cannot be reused';
    END IF;
    PERFORM pg_sleep(0.01);
  END LOOP;
END
$$ LANGUAGE plpgsql;

-- autovacuum off: a VACUUM here would vacuum the whole FSM in its cleanup and
-- hide exactly what this suite looks at.
CREATE TABLE ptf (id int, body text) WITH (autovacuum_enabled = false);
CREATE INDEX ptf_idx ON ptf USING bm25_native (body);
CREATE TEMP TABLE ptf_rows (n int);

-- The smallest threshold (64 kB), so a multi-page chain seals quickly.
SET bm25_native.seal_threshold = 64;

-- One row per INSERT until aminsert's own opportunistic seal publishes the first
-- segment, then stop -- so the seal's truncate is the LAST thing that touched the
-- FSM: no later append in the same batch can pop (and requeue) a recycled page and
-- disturb the state under test.
DO $$
DECLARE
  i int := 0;
BEGIN
  LOOP
    i := i + 1;
    INSERT INTO ptf VALUES (i, 'recycle token alpha beta gamma ' || i || ' ' || (i % 97));
    EXIT WHEN (SELECT count(*) FROM bm25_debug_segcat('ptf_idx')) > 0;
    IF i >= 100000 THEN
      RAISE EXCEPTION 'no opportunistic seal fired after % rows', i;
    END IF;
  END LOOP;
  INSERT INTO ptf_rows VALUES (i);
END
$$;

-- The insert path sealed everything, and its truncate recycled a multi-page chain.
-- (512 | 2) = BM25_PAGE_DELETED | BM25_PAGE_PENDING: a recycled pending page.
SELECT pending_ndocs AS pending_after_insert_seal FROM bm25_stats('ptf_idx');
SELECT count(*) AS recycled_before
  FROM generate_series(1, bm25_debug_npages('ptf_idx')::int - 1) b
 WHERE (bm25_debug_page_flags('ptf_idx', b) & (512 | 2)) = (512 | 2) \gset
SELECT :recycled_before >= 4 AS multi_page_chain_recycled;
SELECT bm25_debug_npages('ptf_idx') AS npages_before \gset

SELECT pg_temp.wait_for_xmin_horizon();
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset
SELECT txid_current() AS burn \gset

-- A quarter of the rows that filled the threshold: a pending chain of about a
-- quarter of the recycled pages, well below the threshold, so no second seal.
INSERT INTO ptf
SELECT 100000 + g, 'recycle token alpha beta gamma ' || g || ' ' || (g % 97)
  FROM generate_series(1, (SELECT n / 4 FROM ptf_rows)) g;
SELECT pending_ndocs > 0 AS second_batch_pending FROM bm25_stats('ptf_idx');

-- THE ASSERTIONS. Every new pending page came out of the FSM: the relation did not
-- grow, and recycled pages were consumed.
SELECT bm25_debug_npages('ptf_idx') = :npages_before AS reused_not_extended;
SELECT count(*) < :recycled_before AS recycled_pages_consumed
  FROM generate_series(1, bm25_debug_npages('ptf_idx')::int - 1) b
 WHERE (bm25_debug_page_flags('ptf_idx', b) & (512 | 2)) = (512 | 2);

-- And the reused pages hold the second batch correctly.
SET enable_seqscan = off;
SELECT count(*) = (SELECT count(*) FROM ptf) AS all_rows_found
  FROM ptf WHERE body @@@ 'recycle';
RESET enable_seqscan;

RESET bm25_native.seal_threshold;
DROP TABLE ptf;
DROP EXTENSION bm25_native;
