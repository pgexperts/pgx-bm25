---
id: 0102
title: bm25_bulkdelete holds the seal/merge singleton for its whole tombstone pass
date: 2026-09-28
status: Accepted
summary: bulkdelete now takes LockPage(metapage, ShareLock) from before its catalog snapshot through the pending sweep, so no seal, merge, upgrade or reclaim can swap the catalog underneath VACUUM's tombstone loop; a new PGC_SUSET debug GUC, bm25_native.debug_pause, parks a backend at a named step so t/020 can drive all four interleavings deterministically.
---

# 0102. bm25_bulkdelete holds the seal/merge singleton for its whole tombstone pass

## Context

`bm25_bulkdelete` took the metapage seal/merge singleton only around
`bm25_segcat_read`, then tombstoned segments and swept the pending list without
it. Every seal and merge takes that same singleton in ExclusiveLock mode, so
either could run inside VACUUM's loop and swap the catalog while VACUUM still
held a stale snapshot of it (issues #239, #241; both code-verified against
main 3b5ee6f, #239's harmful interleaving reasoned but not reproduced, #241 not
reproduced). Effects:

- a seal drained a dead pending document into a segment the snapshot never saw,
  before the pending sweep reached it, so nothing tombstoned it;
- a merge that replayed an input segment's LIVEDOCS bits before VACUUM cleared
  one republished that document live in the merged output; once the heap freed
  and reused the corresponding line pointer, `@@@`/ranked queries returned an
  unrelated row with `xs_recheck = false` (#239);
- a merge that swapped the catalog mid-loop instead left VACUUM tombstoning a
  segment already retired from the catalog: `bm25: segment header N not found
  in catalog` (#239);
- `bm25_segcat_publish_swap`'s pre-flip catalog copy (its "Step A") overwrote a
  surviving segment's `live_ndocs`/`total_len`, and the metapage `ndocs`/
  `total_len` decrements VACUUM made between that copy and the flip were lost,
  drifting corpus statistics high by one document per lost decrement (#241).

ADR 0100 had already recorded the first of these as a known, unfixed
interleaving ("a tombstone landing on a merge INPUT after the replay read its
bit is lost outright") and explicitly declined to trust `live_ndocs` as
liveness evidence for exactly this reason. ADR 0066 had already made VACUUM's
*pending* sweep take the singleton in ShareLock mode, on the same reasoning,
but left the *segment tombstone loop* unprotected — this record extends that
trade to the whole pass.

## Decision

`bm25_bulkdelete` holds `LockPage(BM25_METAPAGE_BLKNO, ShareLock)` from before
`bm25_segcat_read` through `bm25_pending_mark_dead`, covering the catalog
snapshot, every segment's tombstone loop, and the pending sweep in one
critical section. ShareLock, not ExclusiveLock: it does not conflict with
pending appenders (ADR 0022, same mode) or with `bm25_pending_mark_dead`'s own
ShareLock (a same-backend re-acquisition, granted locally without queueing),
but it does conflict with every writer that can change which documents live
where — the seal, the merge, `bm25_upgrade`'s rewrite, and both reclaims — all
of which take the singleton ExclusiveLock. None of those can run inside the
pass. The lock is released at the normal exit; an ERROR releases it at
transaction abort, the same discipline `bm25_pending_append_multi` and
`bm25_pending_mark_dead` already use.

Also decided: a new debug lever, `bm25_native.debug_pause` (PGC_SUSET, string
GUC, empty string = off), inert by default. It names one of five points
(`bulkdelete_start`, `bulkdelete_segment`, `bulkdelete_pending`,
`merge_preswap`, `swap_after_snapshot`); when the backend reaches the named
point it acquires and immediately releases a ShareLock on the advisory lock tag
that `pg_advisory_lock(BM25_DEBUG_PAUSE_LOCKKEY, N)` (N being the point's
position) takes exclusively. A TAP test that holds that advisory lock with
`pg_advisory_lock` therefore parks the backend at the point until the test
releases it, deterministically. Setting the GUC alone parks nothing; at the
`bulkdelete_*` points the parked backend is holding the seal/merge singleton
while it waits. `t/020_vacuum_merge_race.pl` uses it to drive all
four interleavings on demand: pending sweep vs. seal, merge between segments,
merge replaying before the loop and swapping after it, and the #241 stats
race. It works on stock server builds and needs no injection points.
`bm25_debug_pause_check` (a GUC check hook) rejects any value not in the
pause-point table.

## Alternatives considered

- **Per-segment ShareLock with a catalog re-read** (a variant of the issue's
  Option A, which proposes the ShareLock either per segment or for the whole
  loop, re-validating that the segment is still in the catalog). Bounds a
  blocking seal/merge's stall to one segment instead of the whole pass, but is
  more complex. A per-segment variant that simply skips a segment no longer in
  the catalog is unsound: it never tombstones the merged output that now holds
  the document, so the deleted document stays live there.
- **Swap-time revalidation** (the issue's Option B): re-read each input's
  LIVEDOCS bits under the metapage lock immediately before the flip, and clear
  the corresponding bits in the swap's output. Rejected: it needs an
  old-docid-to-new-docid map kept until the swap, does not cover the seal
  variant of the race (a seal has no "replay" step to re-validate against),
  and does not fix #241 (the survivors' counter drift) without a second,
  separate re-read pass over every surviving entry.
- **Injection points for the deterministic test.** Rejected because standard
  server builds lack them (they need `--enable-injection-points`, as ADR 0032
  already noted), and the CI legs that install PostgreSQL from the
  apt.postgresql.org packages are standard builds. The advisory-lock pause hook
  needs nothing beyond the ordinary lock manager, so it works on those builds.

## Consequences

- An explicit `bm25_seal()`, `bm25_merge()` or `bm25_upgrade()` call now waits
  for VACUUM's whole tombstone pass to finish, not just its catalog snapshot.
  While one waits, the lock manager queues new ShareLock requests (inserts)
  behind it, so writers can stall behind a blocked maintenance call. This
  extends ADR 0066's accepted trade — previously scoped to the pending sweep —
  to the whole pass.
- Under autovacuum, a waiter blocked long enough triggers the standard
  autovacuum auto-cancel after `deadlock_timeout`, except an anti-wraparound
  VACUUM, which is not auto-cancelled. A manual `VACUUM` is never
  auto-cancelled either.
- The opportunistic writers — `aminsert`'s opportunistic seal and
  `merge_maybe(false)`'s opportunistic merge — already use
  `ConditionalLockPage` and skip rather than wait, so they are not newly
  starved by a long-running VACUUM; they simply defer to a later opportunity.
- Stale comments that asserted VACUUM serializes against a concurrent merge,
  or that only bulkdelete's catalog snapshot needs the singleton, are
  rewritten to state the invariant this lock now provides: in
  `bm25_handler.c` by 18c821b, and three more in `bm25_seg_read.c` by 9e895aa
  (refs #239).
- `docs/adr/0100`'s recorded race (lost tombstone on a merge input) and its
  companion in #241 (lost counter decrement on a survivor) are both closed by
  this lock, since neither a merge nor a seal can observe or publish a catalog
  state that VACUUM's snapshot has already made stale.
- New test surface: `t/020_vacuum_merge_race.pl` (four scenarios, 49
  assertions) and `bm25_native.debug_pause`, a superuser-only GUC that can
  stall every writer of an index if set outside a test — hence PGC_SUSET, and
  `MarkGUCPrefixReserved("bm25_native")` runs after it is registered so a
  typo'd parameter name cannot silently no-op it.

## Addendum (2026-09-29, what the stall covers and what cleanup does)

Written after issues #254 and #277. Nothing above changes; this states more exactly
what the Consequences section's stall is, and adds the other direction.

**The stall arises only in VACUUM's index-vacuuming pass.** `bm25_bulkdelete` runs
only when PostgreSQL calls `ambulkdelete`, and by PG 18.6's `vacuumlazy.c` that
happens only when the heap scan found dead items (`lazy_vacuum` is called when the
dead-item count is above zero). Even then the pass can be skipped: `INDEX_CLEANUP off`
skips it, and so does the wraparound failsafe. Under the default `INDEX_CLEANUP auto`
it can also be bypassed, but only before any index pass has run in that VACUUM, and
only when fewer than 2% of the table's pages hold dead items and the dead-item
TidStore is under 32 MB. A VACUUM whose dead items outgrow their memory runs the pass
more than once, and each pass takes the lock for its whole length. So "an explicit
`bm25_seal()`, `bm25_merge()` or `bm25_upgrade()` waits for VACUUM's whole tombstone
pass" is true of a VACUUM that reaches that pass, not of every VACUUM. While one
waits, the lock manager queues new ShareLock requests behind it, so inserts that add a
document stall too; the waiter shows in `pg_locks` as a page lock on block 0 of the
index, and cancelling it releases the inserts. The README's maintenance section and
upgrade procedure say so (PR #259 for #254, corrected by the change for #277), and the
`bm25_seal`, `bm25_merge` and `bm25_upgrade` function comments carry one sentence.

**`amvacuumcleanup` waits, except for one call.** `bm25_vacuumcleanup` runs
`bm25_seal_index`, `bm25_reclaim_orphans`, `bm25_merge_maybe(index, false)` and
`bm25_reclaim_retired`. The seal and both reclaims take the singleton with a blocking
`LockPage(ExclusiveLock)`; only `merge_maybe(false)` uses `ConditionalLockPage` and
skips. The Consequences bullet that the opportunistic writers "skip rather than wait"
is therefore about `aminsert`'s seal and that merge, not about cleanup as a whole. A
cleanup that arrives while an explicit seal, merge or upgrade holds the singleton
waits for that hold to end: the reverse of `bm25_bulkdelete`'s stall, where the
explicit call is the waiter. Inserts are already blocked by the holder (ADR 0022);
once it releases, the cleanup's own hold is the same seal and reclaims every VACUUM
makes. `bm25_merge` and a rewriting `bm25_upgrade` release the lock between their seal
and their second phase.

**Autovacuum's lock-conflict cancel cuts one way.** A cleanup that is waiting is the
waiter, so nothing cancels it. Once an autovacuum cleanup holds the ExclusiveLock for
its seal or a reclaim, an insert that queues on it is blocked by an autovacuum holder,
and after `deadlock_timeout` the insert's deadlock check cancels it, unless it is an
anti-wraparound autovacuum (PG 18.6: `deadlock.c` 619-621 records the autovacuum
blocker, `proc.c` 1509-1536 sends the cancel and skips `PROC_VACUUM_FOR_WRAPAROUND`).
This is the same trade ADR 0066 records for autovacuum's drain (its "Symmetrically" paragraph).

## Addendum (2026-10-04)

See #300: the fresh-eyes review of this date measured the autovacuum-cancellation cost of long singleton holds in cleanup passes.

## Addendum (2026-10-05)

#300 (ADRs 0116, 0117, 0118) changes the cleanup this record's 2026-09-29 addendum describes.

- Cleanup runs the seal, `bm25_reclaim_retired`, `merge_maybe(false)`, the orphan sweep, then a
  whole-map FSM vacuum. It used to run the seal, the sweep, the merge and the reclaim.
- The sweep no longer holds the singleton in ExclusiveLock. It holds ShareLock, shared with
  inserts and with `bm25_bulkdelete`, and runs only on durable evidence that an orphan can
  exist. It is no longer the long holder of the autovacuum-cancel trade: the Exclusive holds
  that remain are the seal, one reclaim chunk, and a merge pass.
- `bm25_reclaim_retired` takes the singleton one descriptor page at a time, and a forced merge
  releases between passes. A `bm25_bulkdelete` that takes its whole-pass ShareLock in the gap is
  waited for on re-acquire, so it still never sees a swap mid-pass.
- The share-mode sweep has the shape of the stall recorded here: a blocking `bm25_seal()`,
  `bm25_merge()` or `bm25_upgrade()` arriving during it queues for Exclusive, later appenders
  queue behind that waiter, and the waiter cancels an autovacuum after `deadlock_timeout`.
- `bm25_pending_truncate` vacuums only the FSM range it freed, which leaves the upper FSM
  levels stale for leaves written elsewhere (`bm25_page_alloc`'s requeue) until a whole-map
  vacuum. Cleanup now does that on every run, whether or not the sweep ran (ADR 0116).
- `bm25_native.debug_pause` now names one of fourteen points and accepts a comma-separated
  list, so one backend can park twice.

## Addendum (2026-10-05, PRs #331-#350)

- Two pause points were appended to the table (append-only, so earlier numbers hold):
  15 `rank_build_attempt` (a ranking build attempt entered, nothing read; ADR 0121, PR #331)
  and 16 `keymap_rotation` (a KEYMAP page finished, its successor not allocated; PR #339).
  The list now has sixteen points.
- **`bm25_native.debug_cancel_at`** (PGC_SUSET, same check hook and points as
  `debug_pause`) makes the backend raise a query cancel against itself at a named point, so a
  single-session suite can show where a cancel takes effect (`sql/148` at
  `keymap_rotation`). It is a test lever, inert unless set.
