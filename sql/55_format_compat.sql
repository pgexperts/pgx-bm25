-- 55_format_compat — the version-negotiation matrix (roadmap #4 Task 4).
-- Exercises the two-directional gate (bm25_meta_read, Task 1) end to end using
-- the debug levers (Task 3): legacy-v5 accept, too-old-v4 refuse, additive
-- future-version read-through a REAL optional region, breaking future-version
-- refuse, and the exact floor==this-build boundary. A fresh index built by this
-- binary carries this build's BM25_FORMAT_VERSION and min_read_version ==
-- BM25_OLDEST_READABLE (5); bm25_debug_stamp_version pokes the metapage to
-- simulate other generations against this same binary. Deliberately phrased
-- without naming the current number: prose that spells it out rots at the next
-- bump, and the (C)/(D) literals below are the only place a number has to move.
CREATE EXTENSION IF NOT EXISTS bm25_native;
SET enable_seqscan = off;

CREATE TABLE fc (id int primary key, body text);
INSERT INTO fc SELECT g, 'tortoise hare ' || (g % 5) FROM generate_series(1,20) g;
CREATE INDEX fc_bm25 ON fc USING bm25_native (body);

-- Baseline: the current version reads normally. (A) and (C) below must return this SAME ranked
-- sequence. Single-key ORDER BY (no secondary tiebreak column) is the house
-- discriminating style for ranked-sequence assertions -- a tiebreak key would
-- mask a rank collapse instead of exposing one.
SELECT id FROM fc WHERE body @@@ 'tortoise' ORDER BY body &@@ 'tortoise' LIMIT 3;

-- (A) Legacy v5 reads FREE: format_version=5, min_read_version=0 (as a real
-- pre-v6 metapage's zeroed tail would present -- the field didn't exist before
-- v6). Backward check: 5 is NOT < BM25_OLDEST_READABLE(5) -- accepted. Forward
-- check: 0 is NOT > this build -- accepted. Must return the IDENTICAL 3 rows as the
-- baseline: an exact-equality gate (format_version != this build -> ERROR) would wrongly
-- refuse this instead (see the anti-neuter check in the task report).
SELECT bm25_debug_stamp_version('fc_bm25', 5, 0, 0);
SELECT id FROM fc WHERE body @@@ 'tortoise' ORDER BY body &@@ 'tortoise' LIMIT 3;

-- (B) Too-old v4: format_version 4 < BM25_OLDEST_READABLE(5) -- the backward
-- check refuses cleanly (distinct message + REINDEX hint), never mis-parses the
-- page as a differently-shaped struct. min_read_version=0 keeps the forward
-- check from firing first, so the ERROR below is unambiguously the
-- backward-refuse one, not the forward-refuse one from (D).
SELECT bm25_debug_stamp_version('fc_bm25', 4, 0, 0);
\set VERBOSITY terse
SELECT id FROM fc WHERE body @@@ 'tortoise' ORDER BY body &@@ 'tortoise';
\set VERBOSITY default

-- Restore to v6 -- older than this build, still inside the readable range -- for the
-- remaining cases.
SELECT bm25_debug_stamp_version('fc_bm25', 6, 5, 4);

-- (C) Additive future-version read-through: per the additive rule a writer that only ADDS
-- an optional region does not raise the floor -- min_read_version stays at 5,
-- so this build still accepts the index. Order is NOT load-bearing: every
-- metapage writer RAISES pd_lower (bm25_meta_set_pd_lower) rather than assigning
-- it, so no later write can retract the region back into the zeroed page hole.
-- The region is written FIRST here precisely so the stamp_version below is a
-- metapage write ACROSS it; case (G) drives real production writers over it.
--
-- NOTE: the version literals in (C)/(D) are "this build + 1", not fixed numbers.
-- They MUST be bumped with BM25_FORMAT_VERSION or the forward-gate case silently
-- stops testing anything -- when the build reached v7, the old literal 7 was no
-- longer in the future and (D) began PASSING the gate it exists to trip. Re-blessing
-- the output instead of moving the literal would have hidden that.
SELECT bm25_debug_write_optional_region('fc_bm25');
SELECT bm25_debug_stamp_version('fc_bm25', 9, 5, (1::bigint<<31));
SELECT bm25_debug_check_optional_region('fc_bm25') AS region_survived_stamp;
-- SAME 3 rows as the baseline, read with a real synthetic region physically
-- present past the metapage struct -- bm25_meta_read ignores it by
-- construction (a fixed struct-sized copy), not because the suite forgot to
-- write it.
SELECT id FROM fc WHERE body @@@ 'tortoise' ORDER BY body &@@ 'tortoise' LIMIT 3;

-- Boundary: a floor exactly at this build's version still reads (8 is NOT > 8).
SELECT bm25_debug_stamp_version('fc_bm25', 9, 8, 0);
SELECT count(*) FROM fc WHERE body @@@ 'tortoise';

-- (D) Breaking future version: floor raised one past this build (9 > 8) -- the forward
-- check refuses cleanly, with a message distinct from (B)'s backward-refuse.
SELECT bm25_debug_stamp_version('fc_bm25', 9, 9, 0);
\set VERBOSITY terse
SELECT count(*) FROM fc WHERE body @@@ 'tortoise';
\set VERBOSITY default

-- Restore to the current version so cleanup doesn't trip the gate.
SELECT bm25_debug_stamp_version('fc_bm25', 8, 5, 4);

-- (E) bm25_upgrade metadata-only path: a legacy v5-poked index upgrades to the current version
--     with no segment rewrite, queries unchanged, and is idempotent.
SELECT bm25_debug_stamp_version('fc_bm25', 5, 0, 0);
SELECT bm25_upgrade('fc_bm25');
SELECT format_version, min_read_version FROM bm25_stats('fc_bm25');
-- The legacy poke zeroed feature_flags, so the upgrade drives the
-- bm25_derive_feature_flags branch (distinct from the build-path coverage in
-- Task 2). Assert the WAND-impacts bit (1<<2 = 4, always set at v6) survived
-- the UPGRADE path -- a broken derive here would leave feature_flags at 0.
SELECT (feature_flags & 4) <> 0 AS has_impacts FROM bm25_stats('fc_bm25');
SELECT id FROM fc WHERE body @@@ 'tortoise' ORDER BY body &@@ 'tortoise' LIMIT 3;
SELECT bm25_upgrade('fc_bm25');

-- (F) bm25_upgrade SEGMENT-REWRITE path via a test-only synthetic transform.
--     Unlike (E)'s metadata-only re-stamp, the synthetic transform re-emits EVERY
--     segment through the merge accumulate + atomic-swap machinery, folding the
--     version re-stamp into the swap's own catalog-flip WAL record (new-format
--     segments + new metapage version = ONE record, uncleavable by a crash). The
--     re-emit is semantically identity (every doc's tid/key/doclen/postings are
--     replayed), so the ranked query below returns the SAME 3 rows as the baseline.
--     "upgraded N segments" with N>0 proves a real rewrite ran: N==0 would mean it
--     silently fell through to the metadata-only path. format_version=current/min=5 proves
--     the folded re-stamp landed atomically with the catalog flip.
SELECT bm25_debug_enable_synthetic_transform(true);
SELECT bm25_debug_stamp_version('fc_bm25', 5, 0, 0);
SELECT bm25_upgrade('fc_bm25');
SELECT format_version, min_read_version FROM bm25_stats('fc_bm25');
SELECT id FROM fc WHERE body @@@ 'tortoise' ORDER BY body &@@ 'tortoise' LIMIT 3;
SELECT bm25_debug_enable_synthetic_transform(false);

-- (G) The pd_lower-RAISE invariant under REAL production metapage writers. The
--     additive contract says an OLDER binary keeps WRITING a newer index safely --
--     which is only true if an OLDER binary preserves a tail it does not know. Every
--     metapage writer therefore raises pd_lower to Max(current, end-of-its-own-struct)
--     instead of assigning it; assigning would drop the tail into the page hole,
--     which GenericXLogFinish ZEROES on apply (live buffer and WAL redo alike),
--     destroying a future version's fields.
--
--     This deliberately does NOT re-write the region: it INHERITS the one (C) wrote,
--     so it asserts that single region survived every metapage write since -- the
--     (D)/(E) version pokes, the (E) identity-path upgrade re-stamp, and critically
--     the (F) rewrite path's atomic catalog-SWAP writer (bm25_seg_build.c), which
--     nothing else checks. Then drive two more real writers over it here: an aminsert
--     (bumps pending stats on the metapage) and a seal (rewrites the pending
--     linkage). A single assigning writer anywhere in that chain returns false --
--     this is the discriminating check for the invariant, not a smoke test.
INSERT INTO fc VALUES (21, 'tortoise hare 1');
SELECT bm25_seal('fc_bm25');
SELECT bm25_debug_check_optional_region('fc_bm25') AS region_survived_every_writer;

-- (H) The orphan sweep must NEVER free a page kind it cannot reach. "A new page
--     type old binaries never follow a link to" is ADDITIVE, so this binary
--     accepts (and vacuums) an index containing one -- but bm25_reclaim_orphans
--     marks from a HARDCODED compile-time root set, so such a page is unreachable
--     to it purely out of ignorance. Freeing it would stamp it DELETED with an
--     invalid retire_xid, which bm25_page_alloc reuses IMMEDIATELY (no horizon
--     wait) -- handing a newer binary's live chain to the next seal. The sweep
--     therefore leaks any page with a flag bit outside BM25_PAGE_ALL_KNOWN.
--     Allocate a stand-in (flag 1<<13, linked from nothing), VACUUM, and assert the
--     flags are UNTOUCHED: without the guard the page comes back
--     (1<<13)|BM25_PAGE_DELETED(1<<9) = 8704 instead of 8192.
--     The sweep is gated (issue #300) and a gated-off VACUUM would pass this
--     vacuously, so the counter pins that this VACUUM really swept.
SELECT bm25_debug_alloc_unknown_page('fc_bm25') AS unknown_blk \gset
SELECT bm25_debug_orphan_sweeps() AS sweeps_before \gset
VACUUM fc;
SELECT bm25_debug_orphan_sweeps() - :sweeps_before AS unknown_kind_vacuum_swept;
SELECT bm25_debug_page_flags('fc_bm25', :unknown_blk) AS flags_after_vacuum,
       bm25_debug_page_flags('fc_bm25', :unknown_blk) = (1<<13) AS unknown_page_not_freed;

-- (I) min_read_version is MONOTONIC across bm25_upgrade: it may be RAISED, never
--     lowered. The floor is stamped lazily by writers that know nothing about
--     upgrades -- bm25_pending_append_multi lifts it to BM25_MIN_READ_PENDING_SPAN
--     (7) in the same record as the first page-spanning document, and leaves
--     format_version alone. Both upgrade paths used to ASSIGN BM25_OLDEST_READABLE
--     (5), rewriting such a floor back DOWN and advertising the index as readable
--     by a binary that has no reader for the continuation records it still
--     contains. src/bm25_format.h and the raise site both document the field as
--     never-lowered; this is the gate on that.
--
--     Shape: a v6 BODY (so bm25_upgrade does real work instead of reporting
--     already-current) carrying a floor of 7 -- so a downward assignment shows up
--     as 7 -> 5, and only as that. BOTH paths are driven, because they stamp at
--     two different sites: the identity path writes the metapage directly under
--     its own exclusive lock, while the rewrite path folds the value into the
--     catalog-swap WAL record.
--
--     WHAT THIS TEST DOES NOT COVER: the rewrite path must take its Max against
--     the REGISTERED page copy inside the swap window, not against its pre-lock
--     snapshot, or a raise landing DURING the rewrite is still lost. This suite is
--     serial and stamps the floor before calling bm25_upgrade, so a snapshot-based
--     Max would pass it too. That placement is held by code structure and the
--     comment at the stamp site, not by any assertion here; discriminating it would
--     need an injection point the tree does not have.
SELECT bm25_debug_stamp_version('fc_bm25', 6, 7, 4);
SELECT bm25_upgrade('fc_bm25');                     -- identity / metadata-only path
SELECT format_version, min_read_version FROM bm25_stats('fc_bm25');

SELECT bm25_debug_enable_synthetic_transform(true);
SELECT bm25_debug_stamp_version('fc_bm25', 6, 7, 4);
SELECT bm25_upgrade('fc_bm25');                     -- segment-rewrite + swap path
SELECT format_version, min_read_version FROM bm25_stats('fc_bm25');
SELECT bm25_debug_enable_synthetic_transform(false);

--     ...and the ordinary case is unperturbed: a floor already AT the minimum
--     stays there rather than being dragged up by the Max.
SELECT bm25_debug_stamp_version('fc_bm25', 6, 5, 4);
SELECT bm25_upgrade('fc_bm25');
SELECT format_version, min_read_version FROM bm25_stats('fc_bm25');

DROP TABLE fc;

-- (J) The v8 pending record (an explicit per-field doclen array between the doc
--     header and the term entries) raises the floor LAZILY, exactly as v7's
--     continuation records do -- min_read_version tracks capabilities USED, not
--     what the writing binary is capable of.
--
--     CREATE INDEX writes no pending records at all, so a freshly built index sits
--     at BM25_OLDEST_READABLE and a pre-v8 binary can still read every byte of it.
--     The first INSERT writes a record such a binary would stride wrong (it would
--     read the doclen array as a term entry and desync mid-page), and raises the
--     floor in the SAME Generic WAL record as that write -- so the floor is durable
--     before any reader can observe the record it protects.
--
--     This is the A/B for the v8 record itself: on a pre-v8 build the first line
--     prints the old version and the floor after an INSERT stays at 5.
CREATE TABLE fl (id int primary key, body text) WITH (autovacuum_enabled=off);
INSERT INTO fl VALUES (1, 'tortoise hare');
CREATE INDEX fl_bm25 ON fl USING bm25_native (body);
SELECT format_version, min_read_version AS floor_after_build FROM bm25_stats('fl_bm25');
INSERT INTO fl VALUES (2, 'hare tortoise');
SELECT min_read_version AS floor_after_pending_write FROM bm25_stats('fl_bm25');
--     Monotonic across the drain: a seal removes the records, but the floor stays
--     up, because a reader can hold a snapshot across the seal.
SELECT bm25_seal('fl_bm25');
SELECT min_read_version AS floor_after_seal FROM bm25_stats('fl_bm25');
DROP TABLE fl;
RESET enable_seqscan;
DROP EXTENSION bm25_native;
