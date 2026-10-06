---
id: 0018
title: Metapage before segment-catalog page is the index-wide buffer lock order
date: 2026-07-29
status: Accepted
summary: The seal publish and bm25_livedocs_clear now take the metapage EXCLUSIVE before the segment-catalog page, conforming to the direction bm25_scan_snapshot cannot change.
---

# 0018. Metapage before segment-catalog page is the index-wide buffer lock order

## Context

`bm25_scan_snapshot` holds the metapage `BUFFER_LOCK_SHARE` across its entire
segment-catalog walk, taking each SEGCAT page `BUFFER_LOCK_SHARE` inside it. Two
writers ran the opposite way:

- the seal publish (`bm25_segment_build_and_commit`) took the SEGCAT page
  EXCLUSIVE, then the metapage EXCLUSIVE;
- `bm25_livedocs_clear` took LIVE → SEGCAT → metapage, all EXCLUSIVE.

Buffer content locks are LWLocks. There is **no deadlock detector**, they are not
cancel-interruptible, and nothing else serializes the two paths: the seal/merge
`LockPage` singleton is never taken by a scan, and `AccessShareLock` does not
conflict with `RowExclusiveLock` or `ShareUpdateExclusiveLock`. So an ordinary
`SELECT` and an ordinary `INSERT` that crosses the seal threshold — or a VACUUM
in `bm25_bulkdelete` — could wedge both backends until `SIGKILL` and a cluster
restart (review ref C5, issue #36).

`ARCHITECTURE.md` described this as "latent, resolve when the merge path adds
concurrent segcat writes". That was wrong on both counts. It was never latent —
the seal publish and `bm25_livedocs_clear` *are* concurrent segcat writers, live
on the default path. And the merge, the thing the note was waiting for, is the
one path that was never part of the problem: its swap builds a fresh orphan
catalog chain and locks only the metapage.

## Decision

We will fix the **index-wide buffer content-lock order as metapage before
segment-catalog page**, and move the writers to conform.

The reader's direction is the one that cannot change: the metapage lock held
across `bm25_scan_snapshot`'s catalog walk is precisely what stops a publish
moving the chain under the copy, which is the whole atomicity mechanism of the
snapshot. So the seal publish and `bm25_livedocs_clear` take the metapage
EXCLUSIVE first.

The seal reads its anchor with `bm25_meta_read_locked` from under that lock
instead of an earlier unlocked `bm25_meta_read`, and calls `bm25_meta_validate`
explicitly — `_locked` checks only the magic, and the format-version floor gate
the previous call supplied still has to run.

## Alternatives considered

- **Make `bm25_scan_snapshot` drop the metapage lock, walk the catalog, then
  re-lock and re-validate `segcat_root`/`nsegs`, retrying on change** (the
  report's option (b)). This is the only alternative that preserves the writers'
  order, and it is strictly more machinery: an optimistic retry loop on the query
  hot path, plus a new unbounded-retry failure mode, to avoid changing two
  writers. Rejected — two writers moving is a smaller and more auditable change
  than a retry protocol on every scan.
- **Serialize scans against writers with the existing `LockPage` singleton** —
  would make every ranked `SELECT` contend with every seal on a heavyweight lock.
  That is a throughput regression to fix an ordering bug.

## Consequences

- There is now ONE stated index-wide order, recorded as a binding invariant in
  `ARCHITECTURE.md`. Any new writer that touches both pages must take the
  metapage first; any new reader may hold the metapage across a catalog
  acquisition but never the reverse.
- The seal publish holds the metapage EXCLUSIVE slightly longer — it now spans
  the root-page capacity check and, when the root is full, the
  `bm25_page_alloc` for the prepended page. That allocation under the metapage
  lock is already sanctioned (D-ALLOC/M6: the stamp-and-gate allocator only
  touches index buffers via `ReadBuffer` + `ConditionalLockBuffer` and never
  blocks) and is exactly what `bm25_pending_append_multi` already does.
  `bm25_page_alloc` itself never reads the metapage, so there is no self-deadlock.
- `bm25_livedocs_clear` goes from LIVE → segcat → meta to meta → LIVE → segcat.
  LIVE stays ahead of segcat, so no other pairwise order changed.
  `bm25_segcat_locate_entry`, called before the block, takes and releases its
  metapage and segcat locks independently and holds neither across the other.
- **No regression test accompanies this.** The window between "metapage locked"
  and "SEGCAT page requested" is microseconds inside a single SQL statement;
  `isolationtester` interleaves between statements, so it cannot open it, and
  reproducing the hang deterministically would need a fault-injection point in
  the scan path. Adding a debug hook for it was rejected while issue #38 in the
  same series is removing debug surface. The fix is verified structurally (both
  inverting sites corrected, all other sites audited: `bm25_segcat_locate_entry`,
  the merge swap, and `bm25_segcat_build_orphan_chain` take at most one of the
  two) and by the full suite showing no functional regression.

## Addendum (2026-08-16)

The Decision above says the seal "calls `bm25_meta_validate` explicitly —
`_locked` checks only the magic". That is no longer how the code reads. Issue
#136 found the other consequence of the split: `bm25_pending_append_multi` is
the second caller of `bm25_meta_read_locked` and never made the follow-up call,
so `aminsert` reached the pending list without the format gate at all — the
first validated read on that path was `bm25_pending_should_seal`, *after* the
append had been made durable by `GenericXLogFinish`.

`bm25_meta_validate` is therefore folded INTO `bm25_meta_read_locked`, and the
seal's explicit follow-up call is deleted as redundant. The lock order this
record fixes is untouched; only where the gate runs changed, and it now runs for
both callers by construction rather than by each remembering. The blast radius is narrower than it first appears, and worth stating precisely:
DML on a too-new index was ALREADY refused, because `bm25_insert` calls
`bm25_pending_should_seal` unconditionally after the append and that runs the full
gate. What changes is that the refusal is now ATOMIC. Previously the gate fired
only after `GenericXLogFinish` had made the pending record durable, so the
statement aborted while the index record stayed -- a physical Generic WAL write
survives the abort. `sql/88_pending_tail_space` pins that: `pending_ndocs` goes
1 -> 2 across a refused INSERT before this change, and 1 -> 1 after it.

## Addendum (2026-09-29)

This record fixed the order in which the primary ACQUIRES buffer locks. It said
nothing about replay, and the order it fixed was not the order a hot standby
takes. Issue #240 (PR #255) closes that gap.

**The rule.** On replay, `generic_redo` locks every block a Generic WAL record
registered EXCLUSIVE, in registration (block_id) order, and holds them all until
the record is applied. A Generic WAL record's registration order is therefore its
standby lock order, so it must follow the same index-wide order: **any
multi-buffer Generic WAL record that includes the metapage registers the
metapage FIRST.** On the primary, registration takes no locks, which is why the
primary-side fix above never surfaced it.

**What was wrong.** Three records registered the metapage after another page:
`bm25_livedocs_clear` (LIVE, catalog, metapage), the in-place branch of
`bm25_segcat_publish_append` (catalog, metapage), and
`bm25_pending_append_multi` (tail, metapage). For the first two the catalog page
is reachable from the metapage, so a standby scan holding the metapage SHARE
across its catalog walk (`scan_snapshot_sampled`) against the startup process
holding that catalog page EXCLUSIVE and waiting for the metapage is the same
uninterruptible LWLock cycle this record describes, with the startup process as
one party. `bm25_pending_append_multi` could not close a cycle at the time
(`bm25_scan_snapshot` releases the metapage before walking the pending chain); it
was conformed so a future reader that holds the metapage across a pending-page
lock does not inherit the hazard. All three now register the metapage first; the
primary-side acquisition order this record fixed is unchanged. The fresh-page
branch of `bm25_segcat_publish_append` writes a page no reader can reach and was
not the hazard, but it shares the code path and now registers the same way.

**Evidence level.** The deadlock was derived from reading the server's
`generic_redo`, `XLogReadBufferForRedoExtended` and `LWLockAcquire` (issue #240
checked PG 17 and 18.6); it was never reproduced. Reproducing it needs a pause
point between the standby scan's metapage lock and its first catalog lock, which
would put a pause under a buffer lock, and `bm25_debug_pause_point` (the hook added
by [0102](0102-bulkdelete-holds-the-singleton-for-its-whole-pass.md)) documents that
"Callers must hold no buffer lock here". No such test was written.

**Enforcement.** `t/021_generic_wal_meta_first.pl` runs a workload that emits all
three record shapes and asserts with `pg_waldump` that every Generic WAL record
touching the index metapage carries it as block reference #0; it also requires
each shape to have been observed, so a workload that stops emitting one fails
rather than passing vacuously. That is the whole mechanical guard: no `Assert`
and no cassert wrapper around `GenericXLogRegisterBuffer` (which the issue suggested);
the maintainer declined one in favour of the test.
A NEW multi-buffer record including the metapage is covered only if the workload
reaches it, so adding one means extending that suite's workload as well.

PostgreSQL 19 moved buffer content locks off LWLocks. In REL_19_STABLE's
`bufmgr.c` (read 2026-09-29), `BufferLockAcquire` still calls `HOLD_INTERRUPTS()`
before it waits ("cancel/die interrupts are held off until lock release"), so a
wait on one is still uncancellable and the hazard class persists.

## Addendum (2026-10-05)

#300 (ADR 0116): a pending append that needs a fresh page is now one Generic WAL record,
where it was three. It registers the metapage first (block reference 0), then the old tail,
then the new page, which is three buffers inside the four-buffer cap and in the order this
record requires. `GenericXLogStart` is hoisted above the allocation, so nothing between
`bm25_page_alloc` and `GenericXLogFinish` can throw. `t/021_generic_wal_meta_first.pl` now
observes the three-block shape and rejects any INSERT record without the metapage. The new
orphan-bracket and sweep-completion records each register the metapage alone.

## Addendum (2026-10-05, PRs #331-#350)

Residual (#309 REGR #36): the primary-side lock order this record fixes (metapage, then
LIVE, then catalog) has no test that fails when it is reversed. The failure is an LWLock
deadlock inside buffer-content-lock windows, where no pause point may sit, so a witness
needs a new source lever; the bounded-test work of PR #347 left it out of its tests-only
scope. `t/037` records it as not witnessed.
