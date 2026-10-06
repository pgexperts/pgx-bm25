-- 81_vacuum_reltuples: bm25_vacuumcleanup (bm25_handler.c) filled only
-- stats->num_pages, leaving num_index_tuples and estimated_count at the
-- palloc0'd zero/false default. Core's own contract for that result (vacuumlazy.c,
-- update_relstats_all_indexes) treats estimated_count == false as "trust
-- num_index_tuples": it writes that zero straight into pg_class.reltuples for the
-- INDEX relation on every VACUUM that reaches cleanup -- which, per this
-- function's own header comment, is standalone even when there are no dead
-- tuples to remove (bulkdelete is skipped by core in that case; cleanup still
-- runs). reltuples is set CORRECTLY at CREATE INDEX time (bm25_build returns a
-- real IndexBuildResult), so the defect is invisible until the first VACUUM,
-- then permanent: nothing else in this AM ever corrects the index's reltuples
-- again. The fix is stats->estimated_count = true, telling core the zero is not
-- a real count so it leaves reltuples exactly as CREATE INDEX set it.
--
-- WHY THIS MATTERS: pg_class.reltuples/relpages for an index relation feeds
-- monitoring and bloat-estimate queries directly, and (for a PARTIAL bm25 index
-- specifically, via plancat.c's estimate_rel_size path) the planner's own row
-- estimate -- collapsed to 0, a partial index's selectivity estimate flattens
-- along with it.
--
-- THIS SUITE'S ASSERTION FAILS BEFORE THE FIX, PASSES AFTER: reltuples for the
-- index relation is checked against the known row count immediately after
-- CREATE INDEX (baseline -- always correct, independent of this fix) and again
-- after a VACUUM with nothing to delete. Pre-fix the second check is 0
-- (stomped); post-fix it is unchanged from the baseline.
--
-- NO DEAD TUPLES, DELIBERATELY: this isolates amvacuumcleanup's own
-- num_pages/estimated_count contribution from bm25_bulkdelete's
-- tuples_removed accounting (already correct, and a separate code path) --
-- and it is not a weaker test for skipping bulkdelete. The opposite: it proves
-- the defect fires even on the "nothing to clean up" VACUUM every autovacuum
-- worker runs constantly, which is the common case that made the original bug
-- silently universal rather than delete-triggered.
CREATE EXTENSION bm25_native;
CREATE TABLE vr_docs (id int, body text) WITH (autovacuum_enabled = off);
INSERT INTO vr_docs SELECT g, 'alpha beta gamma ' || g FROM generate_series(1, 500) g;
CREATE INDEX vr_docs_idx ON vr_docs USING bm25_native (body);

-- Baseline: CREATE INDEX sets reltuples correctly via bm25_build's own
-- IndexBuildResult (heap_tuples/index_tuples), a path this fix does not touch.
SELECT reltuples::int = 500 AS reltuples_correct_after_create
FROM pg_class WHERE oid = 'vr_docs_idx'::regclass;

-- No dead tuples to remove, so core's bulk-delete phase does not even run for
-- this index; only the cleanup phase (amvacuumcleanup) does.
VACUUM vr_docs;

-- The regression: pre-fix, reltuples is stomped to 0 here and stays 0 on every
-- subsequent VACUUM; post-fix, estimated_count = true leaves it untouched.
SELECT reltuples::int = 500 AS reltuples_survives_vacuum
FROM pg_class WHERE oid = 'vr_docs_idx'::regclass;

DROP TABLE vr_docs;

DROP EXTENSION bm25_native;
