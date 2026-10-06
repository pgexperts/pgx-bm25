---
id: 0116
title: VACUUM's orphan sweep runs only on durable evidence that an orphan, or a lost free-space entry, can exist
date: 2026-10-05
status: Accepted
summary: Three metapage tail fields (orphan_ops_begun, orphan_ops_done, swept_epoch) bracket every maintenance op that can leave orphans and record the crash epoch of the last completed sweep, and the pending append became one WAL record so appenders are no longer an orphan source.
---

# 0116. VACUUM's orphan sweep runs only on durable evidence that an orphan, or a lost free-space entry, can exist

## Context

Every non-ANALYZE VACUUM ran `bm25_reclaim_orphans`. It marks every live chain and visits
every block, throttled by the cost-based delay, so its cost is O(index) on every run. It
held the seal/merge singleton in ExclusiveLock (ADR 0019), and every document-adding
INSERT needs that singleton in ShareLock (ADR 0022). Each insert waited for the whole
pass. Under autovacuum the waiting insert's deadlock check cancelled the worker after
`deadlock_timeout`, so on an insert-busy table autovacuum was cancelled on every attempt,
and its heap work (freeze horizons, `reltuples`, statistics) never landed. ADR 0066
had accepted the cancel trade as "a slightly higher autovacuum-cancellation likelihood"
and ADR 0102 extended it to bulkdelete's whole pass; for the sweep the 2026-10-04 review
found it deterministic once one hold exceeds `deadlock_timeout`, and the sweep's length grows with the index on every VACUUM
(issue #300). A reproduction with per-table cost-delay settings and a 600-document index
isolated the sweep, and not the drain, as the long holder.

Gating the sweep needs evidence that survives a crash and covers every way an orphan, or a
free page the free-space map has lost, can come to exist. The issue listed the hazards a
gate must close: durable before the first allocation of any build; not "nblocks grew",
which is unsound; every source covered; no flag that stays set forever on an insert-busy
table; an index created before the change reads as sweep-needed.

## Decision

**The evidence.** Three fields are appended to the metapage past its old 104-byte end: a
4-byte `reserved_tail_pad` (the old trailing padding at offset 100, never interpreted),
`orphan_ops_begun` at 104, `orphan_ops_done` at 108 and `swept_epoch` (uint64) at 112,
`sizeof` 120, with offsets pinned by `StaticAssertDecl`. This is additive (ADR 0009): an
older binary copies exactly 104 bytes and `bm25_meta_set_pd_lower` is `Max()`, so the tail
and `pd_lower` survive an old writer. On every existing metapage the bytes are zero (the
page was zero-initialized, nothing wrote there, and full-page-image replay restores the hole
as zeros), and zero means "never swept, so sweep". No read gate, `format_version` move or
feature bit is involved.

**Brackets.** `bm25_orphan_op_begin` and `bm25_orphan_op_end` each write one Generic WAL
metapage record that bumps `begun` or `done`, in place under the metapage EXCLUSIVE lock,
like `bm25_next_gen`. Begin is written before the op's first allocation, which is what makes
it crash-proof: the begin record precedes every page-init record the op writes in LSN order,
and a page reaches disk only after its WAL is flushed, so any orphan whose bytes survive a
crash has its begin replayed. The ops that bracket:
- the seal, from the drain through the truncate (`bm25_seal_pending_locked`), only when the
  chain is not empty, so a VACUUM on an idle index writes no WAL;
- `bm25_reclaim_retired`, lazily, before its first descriptor compaction;
- each merge pass (`bm25_merge_execute`), opened after both `return false` exits so a pass
  that merges nothing leaves no evidence, and `bm25_merge_rewrite_all`;
- `bm25_debug_seal_unpublished`, opened and deliberately never closed, because it fakes a
  died build.

**A swap never closes its bracket.** A merge or upgrade swap rebuilds the catalog as a fresh
chain and orphans the old one on its success path (`bm25_segcat_publish_swap`: no
`retire_xid`, freed only by the sweep, ADR 0107). So a completed merge leaves its bracket
open on purpose, and cleanup runs the sweep after its own merge, which retires it. The design
pass had closed the bracket after a successful swap; review found that leaks at least one
catalog page per merging VACUUM with no bound, because the sweep is then gated off.

**Counters, not a bit.** An op that dies leaves `begun` ahead of `done`, and the next
successful op's begin and end move both by one, so the gap survives. A set-then-clear bit
would be cleared by that next op. Every bracketing op holds the singleton EXCLUSIVE across
its bracket and the sweep holds it in Share (ADR 0117), so no bracket is open while the sweep
decides or completes: whatever gap it sees belongs to ops that died.

**The crash epoch.** `swept_epoch` records `bm25_crash_epoch()` at the last completed sweep.
The epoch is a nonzero random 64-bit value in a named DSM segment, `bm25_native.crash_epoch`,
initialized by `GetNamedDSMSegment` (the PG 19 signature takes an extra argument and is
shimmed). The segment is created empty with the registry, which `DSMRegistryShmemInit`
re-creates on every shared-memory initialization, so the value changes on a start, a restart
and a crash-restart under `restart_after_crash`. It needs no `shared_preload_libraries` and
exists in every supported major (17 and later). The name and size are frozen: another binary
asking for the same name with a different size raises an ERROR for the life of the server.
- **Why not the postmaster start time.** `PgStartTime` is assigned once, in `PostmasterMain`
  (PG 18.6, `postmaster.c` 1372). The crash-reinit path re-creates shared memory without
  reassigning it, so under the default `restart_after_crash = on` a backend crash between
  two sweeps would go undetected. The marker has to change on every shared-memory
  reinitialization.
- **Failure means "unknown".** `GetNamedDSMSegment` can ERROR (out of DSM segments, a full
  `/dev/shm`, a PG 17 minor whose registry keeps a half-initialized entry after one failed
  create). The call runs in an internal subtransaction, the only safe way to recover from an
  ERROR raised while the registry's LWLocks were held; a query cancel is re-thrown. On
  failure the epoch is 0, which means "unknown": the gate always sweeps and the completion
  record stores 0, so a DSM problem costs VACUUM time and does not fail every cleanup of
  every bm25 index. A standalone backend also returns 0. The call is made before any lock or
  buffer, and the value is cached per backend.

**The gate.** The sweep is needed when `epoch == 0`, or `swept_epoch != epoch`, or
`orphan_ops_begun != orphan_ops_done`. It is checked first without the singleton, so a VACUUM
of an idle index never queues behind a waiting `bm25_seal()`: a stale "needed" costs only the
locked re-check, and a stale "not needed" means an op opened its bracket after the read, whose
own evidence sends a later VACUUM. It is checked again under the singleton.

**Completion.** After the sweep, under the metapage EXCLUSIVE lock, `orphan_ops_done =
orphan_ops_begun` and `swept_epoch = epoch`. Clearing is sound because the decision and the
completion both run while the sweep holds the singleton in Share, which excludes every
bracketing op, so `begun - done` at decision time counts dead ops only and the sweep frees
what they left.

**The pending append is one record.** A part that needs a fresh page used to write three
Generic WAL records: initialize the new page, link the old tail to it, then the data and
metapage record. A crash or ERROR after the first left an initialized `PENDING` page nothing
linked. One after the second left the page hanging off the old tail past `meta.pending_tail`,
where the next append's link overwrote the pointer and orphaned it through a successful
append. An appender could only have been bracketed by writing evidence on every allocation.
Instead the new page's initialization, the old tail's forward link and the metapage update
ride the part's single append record, registered metapage first, then the old tail, then the
new page: three buffers, inside the four-buffer cap and in the registration order ADR 0018
requires. `GenericXLogStart` is hoisted above the allocation, so nothing between
`bm25_page_alloc` and `GenericXLogFinish` can throw. A crash or ERROR leaves at most a zero
page or a popped `DELETED` page, which is free-space drift and not an initialized page that
nothing reaches. ADR 0110's epoch bump rides the same record, so no two chains share an epoch.
The allocator's comment no longer claims it never waits on a content lock: the `P_NEW` page's
own lock can be held briefly by a share-mode sweep.

**The whole free-space map is vacuumed on every cleanup.** `bm25_vacuumcleanup` calls
`IndexFreeSpaceMapVacuum` on every run, outside every heavyweight lock. Until then the sweep,
which ran every time, was the only unconditional FSM vacuum, since
`bm25_pending_truncate` vacuums only the range it freed (`FreeSpaceMapVacuumRange`, PEND-07).
`RecordFreeIndexPage` writes only a leaf, `fsm_search` corrects a stale parent only downward,
and `bm25_page_alloc`'s requeue of a rejected candidate vacuums nothing, so without it those
leaves can become invisible to `GetFreeIndexPage` and the relation grows. It costs about
`nblocks / 4000` FSM pages.

**Tests.** `sql/134_orphan_sweep_gate` (the gate, behaviourally: a throttled idle VACUUM
finishes well inside a timeout a sweep cannot; brackets; a merge's open bracket) with probes
`bm25_debug_sweep_evidence` and `bm25_debug_orphan_sweeps`; `t/029_orphan_sweep_evidence.pl`
(evidence across a crash, a crash-restart with the postmaster surviving, an ERROR between
`bm25_reclaim_retired`'s compaction and its frees); `t/028_append_one_record_crash.pl`; and
`t/021_generic_wal_meta_first.pl`, which now observes the three-block record. Suites that
assert a sweep effect now pin that the sweep ran (`sql/19`, `33`, `55`, `89`, `t/027`).

## Alternatives considered

- **The postmaster start time as the crash marker.** Unsound across a crash-restart (above).
- **`nblocks` growth as evidence.** Unsound, per the issue.
- **A sticky per-allocation flag.** Perpetual on an insert-busy table.
- **An evidence bracket around the three-record append.** It would need epoch-qualified
  "dead appender" evidence that can be cleared only after that chain drains, plus handling
  for the dangling-link case. Making the append one record is smaller and removes the source.
- **Bracketing `bm25_page_alloc`** to close the residual below: WAL on every allocation, for a
  free-space leak.
- **Closing the swap bracket on success.** Leaks the old catalog chain (above).
- **Repairing dangling pending links or legacy chains.** They leak and are documented.

## Consequences

- The sweep runs after a crash or restart, after a maintenance op died, after any merge or
  segment-rewriting upgrade, and once on an index this binary has not swept (a fresh
  `CREATE INDEX` also reads `swept_epoch == 0`, so its first VACUUM sweeps once). Otherwise a
  VACUUM does not sweep, and an idle index's VACUUM stays WAL-free.
- **Every merge forces the next sweep**, because its bracket stays open.
- **Residual: an ERROR inside `bm25_page_alloc`** after `GetFreeIndexPage` popped a candidate
  drops one `DELETED` page out of the FSM, and an ERROR after a `P_NEW` extension strands a
  zero page, until the next sweep (a restart or a dead op). A space leak, never corruption.
- **Residual: legacy epoch-0 pending orphans leak** (ADR 0117).
- **Mixed binaries.** A binary from before this change does not bracket its ops. Its orphans
  are caught by its own every-VACUUM sweep or by the next restart's epoch check; the gap is a
  no-restart window in which a dead op's orphans stay leaked until the next restart. The
  README says to restart the server after installing a new release. Mixed binaries may leak,
  never corrupt.
- A new orphan source without a bracket leaks silently, and no test sees space that is never
  reclaimed until something measures growth. ARCHITECTURE.md carries this as a landmine.
- ADR 0019's whole-duration ExclusiveLock is superseded by ADR 0117. ADR 0066's and
  ADR 0102's autovacuum-cancel trade no longer applies to the sweep (their addenda).
