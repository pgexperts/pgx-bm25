---
id: 0107
title: Production walks of the segment catalog hold the metapage singleton, and the INSERT key check reads entry 0 under the metapage lock
date: 2026-10-04
status: Accepted
summary: A catalog chain orphaned by a merge has no xid horizon, so any production walk of the segment catalog outside bm25_scan_snapshot must hold the metapage singleton in ShareLock or stronger, asserted under cassert; the per-row INSERT key check, which holds no singleton, now copies catalog entry 0 under the metapage SHARE through bm25_segcat_first_entry instead of walking the whole catalog.
---

# 0107. Production walks of the segment catalog hold the metapage singleton, and the INSERT key check reads entry 0 under the metapage lock

## Context

Issue #270. `bm25_segcat_read` locks the metapage only to read `segcat_root`. It
releases the metapage and then walks the catalog pages holding per-page locks alone.
`bm25_scan_snapshot` is different: it holds the metapage SHARE across its whole walk,
and a catalog swap needs the metapage EXCLUSIVE to flip the root, so no swap can move
the chain under it (ADR 0018).

A walk that releases the metapage is safe only if nothing can free and reuse a catalog
page while it runs. A catalog chain orphaned by a merge gets no `retire_xid`, so the
next VACUUM's orphan sweep (`bm25_reclaim_orphans`) frees it for immediate reuse,
with no xid horizon to wait for. Only the seal/merge singleton (the heavyweight
`LockPage` on the metapage) keeps a swap and that reclaim out of such a walk.

ADR 0042 had made `bm25_bulkdelete` take the singleton and described the catalog walk
as loud and locked, naming bulkdelete as the one production caller without it. That
list was incomplete. The INSERT path was a second, unnoticed caller without it: `bm25_validate_key_config_for_insert` (ADR 0067) runs once per
INSERT row, before the append takes the singleton, and called `bm25_segcat_read`. An INSERT that
stalled between the root read and the walk while a merge, a VACUUM and an allocation
all ran would read a page of another kind. Since the page-kind check of #261 that
fails a healthy INSERT with XX002 "segment page is not of the expected kind". ADR 0095
recorded this as known and open, from code reading, never reproduced. `t/022`
reproduced it by parking an INSERT after its catalog read: with the pause moved into
the old walk, between its metapage read and its walk, the INSERT fails with XX002 on
the reused root.

The user's decision for #270 was a helper plus a cassert contract, rather than a lock
hoist or a retry-once.

## Decision

**The contract.** A production caller of an unlocked catalog walker must hold the
metapage singleton, ShareLock or stronger. The walkers are `bm25_segcat_read` (through
`bm25_segcat_read_locked`) and `bm25_segcat_locate_entry` (through
`bm25_livedocs_clear`). Callers today: `bm25_bulkdelete` (ShareLock, ADR 0102),
`bm25_reclaim_orphans`, `bm25_merge_execute` and `bm25_merge_rewrite_all` (the merge
paths, ExclusiveLock), and `bm25_segcat_publish_swap`, which only the two merge paths
call. `bm25_scan_snapshot` is outside the contract because it holds the metapage SHARE
for the whole walk.

**Enforcement.** `bm25_segcat_read_locked` asserts, under cassert, that this backend
holds the singleton, using `LockHeldByMe` on the metapage's page-lock tag in
ShareLock mode. "Or stronger" is by lock-mode number, so the ExclusiveLock the seal,
merge and reclaim take passes as well as bulkdelete's ShareLock. `bm25_livedocs_clear`
asserts the same, which covers `bm25_segcat_locate_entry`, since that walk is safe only
because its one production caller runs under bulkdelete's hold. The debug SRFs keep the
unasserted `bm25_segcat_read`: they read an index nobody is guaranteed to be quiescing
and accept that a concurrent merge followed by VACUUM can fail them loudly. The
asserting form took the new name, rather than the debug SRFs taking one, because that
touched five call sites instead of twenty-four (the counts when the change landed; the
unasserted debug call sites number twenty-five at 5cb24e3). The check is cassert-only.

**The INSERT key check.** `bm25_segcat_first_entry` copies catalog entry 0 under the
metapage SHARE, reading the root page while the metapage is held, with the same extent,
page-kind and empty-page checks as the walkers. That is all the check needs, because
every segment shares one key configuration (ADR 0067). The per-row full catalog walk is
gone. The reads that follow, the segment header and the KEYMAP key metadata, stay safe
without the lock: the inserter's snapshot predates any swap that retires the segment,
so its xmin holds the retired pages' horizon, and both reads are gen-checked, so a
reuse the horizon did not prevent raises 40001, which a client can retry, not XX002.
A new pause point, `insert_keycheck`, parks an INSERT after the root read for `t/022`.

## Alternatives considered

- **A lock hoist** (the user's term), such as holding the metapage across the whole
  walk as `bm25_scan_snapshot` does, which issue #270 listed as a fix direction.
  Decided against by the user; the record does not hold the user's own reasons. What the project's own
  sources say bears on it: the INSERT check uses only entry 0, so a whole-catalog
  walk under a lock would be paid per row for one entry, and a metapage content lock
  is an LWLock, which holds interrupts while held and makes every writer that needs
  the metapage EXCLUSIVE wait (ARCHITECTURE.md's lock-order and CHECK_FOR_INTERRUPTS landmines). Rationale beyond
  that was not recovered from project sources.
- **Retry the read once** on a kind, entry-count or gen mismatch. Also decided against
  by the user. Issue #270 listed it as a fix direction and noted that a reused page that
  is still of the SEGCAT kind passes the kind check, which a retry keyed on an error
  would not see. Rationale beyond that was not recovered from project sources.
- **Keep the asserting contract on the name `bm25_segcat_read` and give the debug SRFs
  a new name for the unasserted copy.** Rejected for the call-site count above.

## Consequences

- The INSERT key check now reads the metapage and the catalog's root page, then the
  same segment-header and key-metadata reads as before, instead of walking every
  catalog page. ADR 0067's cost note describes the old read; the new cost was not
  measured.
- The INSERT path no longer notices catalog corruption beyond the root page, which
  the old full walk caught incidentally. The helper reads the root only. This is
  intended.
- `t/022` cannot detect a return to the unlocked walk on the fixed build. The window
  the fix protects, from root read to entry copy, runs under a buffer content lock,
  where no pause can sit, so the test covers the read after that window. The A/B for
  #270 moved the pause into the old walk to show the failure.
- The contract is checked only in cassert builds. A production build relies on every
  caller holding the singleton, which the callers list above states as of this
  record.
- Two guards of `bm25_segcat_first_entry` have no regression coverage, an Invalid root
  with `nsegs > 0` and a root at the metapage: no test lever stamps `segcat_root`. The
  empty-root guard is covered by `sql/114`, and the extent guard's re-sample by
  `sql/110` (a healthy catalog read back with a stale extent sample), both driven
  through `bm25_debug_segcat_walk`. A root truly past the extent would also need a lever
  that stamps `segcat_root`, so that branch is not driven either.
- Debug SRFs can still surface XX002 on a healthy index when a merge and a VACUUM
  land inside their walk. ADR 0042 recorded that for three of them, and it stands.
- The comments in `bm25_seg_build.c` and `bm25_fsm.c` that described the catalog walk
  as locked or gen-protected were corrected, and the contract is documented in
  `bm25.h`. ADR 0042, ADR 0067 and ADR 0095 carry addenda that point here.

## Addendum (2026-10-05)

The rule is unchanged by #300 and the orphan sweep satisfies it in a weaker mode:
`bm25_reclaim_orphans` now holds the singleton in ShareLock (ADR 0117), which is "ShareLock or
stronger". The catalog chain a merge or upgrade swap orphans is freed only by the sweep, and
a swap never closes its orphan bracket, so the next cleanup's sweep is guaranteed to run
(ADR 0116). A walker that holds the singleton may now run beside the sweep, but it took the
singleton after the swap that orphaned any chain the sweep frees, so it reads the new root. The
unlocked debug SRFs stay the exception.

## Addendum (2026-10-05, PRs #331-#350)

The gen arm of ADR 0120 (`bm25_seg_gen_mismatch`) reads the catalog on its error path. It
does so through `bm25_scan_snapshot`, with the metapage SHARE held across the walk, and never
through the unlocked `bm25_segcat_find_entry`, which this record forbids outside the
singleton: on a standby that walk could meet a reused catalog page and turn the legitimate
40001 into XX002 (design check R1). A backend holding the singleton skips the read, since its
segments cannot retire. `bm25_segcat_find_entry` stays test and debug only.
