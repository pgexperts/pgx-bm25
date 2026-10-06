---
id: 0031
title: Suites that assert on VACUUM-driven tombstoning wait for the xmin horizon first
date: 2026-07-29
status: Accepted
summary: A tuple deleted while another backend in the same database holds an older snapshot is only "recently dead", so VACUUM never hands it to bulkdelete and no tombstone is written; 17_delete now waits for the horizon to clear before VACUUMing.
---

# 0031. Suites that assert on VACUUM-driven tombstoning wait for the xmin horizon first

## Context

`sql/17_delete` failed once in six consecutive `installcheck` runs on PG 18.3:

```
93c93                              100c100
<     4 |    1                     <      4 | 3.7500
---                                ---
>     5 |    0                     >      5 | 3.6000
```

The DELETEd document was not tombstoned, so `bm25_debug_tombstone` reported 5
live / 0 dead and `bm25_stats` computed `avgdl` over 5 documents.

This is correct PostgreSQL behaviour, not a bm25 defect. VACUUM only hands a TID
to an AM's `bulkdelete` callback if the dead tuple is **removable**. A tuple
deleted while another backend holds an older snapshot is merely "recently dead",
the callback never sees it, and no tombstone is written.

Note which assertions were affected: the `@@@` / `&@@` membership queries earlier
in the suite still passed, because MVCC hides the deleted row from the executor
whether or not the index tombstoned it. Only the two assertions that read the AM's
*internal* state were sensitive.

### Established by measurement

- **Reproduced deterministically** by holding `BEGIN ISOLATION LEVEL REPEATABLE
  READ; SELECT 1;` in a second session on the same database across the DELETE:
  produces exactly 5 / 0 and 3.6000.
- **A snapshot in a different database does not do it** (4 / 1, correct).
  `ComputeXidHorizons` derives a normal table's horizon per-database, so only
  same-database backends matter. This narrows the culprit considerably.
- During `installcheck` pg_regress owns `regression` exclusively and runs suites
  serially, so the realistic culprit is an **autovacuum worker running ANALYZE**.
  A lazy VACUUM worker sets `PROC_IN_VACUUM` and is excluded from the horizon;
  ANALYZE is not excluded.
- **The blast radius is wider than one suite.** Running the whole suite with a
  snapshot held for its entire duration fails six: `17_delete`, `19_merge`,
  `20_merge_reclaim`, `21_retired_descriptors`, `34_merge_multifield`,
  `37_merge_positions`. Suites that DELETE + VACUUM but only assert query results
  (`18_vacuum_reclaim`, `44`, `46`, `47`, `61`, `62`) are unaffected, because those
  assertions are MVCC-correct either way.

## Decision

Before the VACUUM whose tombstone it asserts, `17_delete` waits until no other
backend in this database holds a snapshot.

This is **sufficient and permanent**, not merely a narrower race. Once no other
backend holds a snapshot, any snapshot taken afterwards necessarily begins after
the DELETE committed, cannot see the row, and therefore cannot keep it
un-removable. There is no window between the check and the VACUUM.

The wait is bounded (30 s) and raises a named exception on timeout, so an
unresolvable horizon fails as "xmin horizon still held by another backend" rather
than as a confusing count diff.

`pg_stat_clear_snapshot()` inside the loop is load-bearing and was the one real
trap here: `pg_stat_activity` is materialised **once per transaction**
(`stats_fetch_consistency` defaults to `cache`), so without it the loop re-reads
its own first sample forever and burns the full timeout on every run — strictly
worse than the flake it replaces. The first version of this fix did exactly that.

### Verified

With a rolling holder (1 s snapshot, 0.2 s gap) around the DELETE, over 32 trials:

| | failures |
|---|---|
| without the wait | 6 / 32 (`5\|0`) |
| with the wait | **0 / 32** |

On a quiet cluster the wait costs ~1.5 ms and exits on its first iteration.

## Alternatives considered

- **Make the fixture a TEMP table.** Genuinely immune — temp relations use
  `temp_oldest_nonremovable`, the session's own xmin, because no other backend can
  see them; verified to give 4 / 1 even under a held snapshot. Rejected because it
  would silently move this suite off the WAL-logged, shared-buffer path it exists
  to exercise, and onto the local-buffer path instead.
- **Weaken the assertions** (drop the exact counts, or accept `dead >= 0`). That
  removes the Task-16 / Task-19 coverage the suite was written for.
- **Retry the VACUUM until the tombstone appears.** VACUUM cannot run inside a
  `DO` block, and a retry loop would mask a genuine tombstoning regression as a
  slow pass.
- **Disable autovacuum on the fixture table.** Does not help: the interfering
  worker is analysing some *other* table in the same database.
- **A shared helper** (a `bm25_debug_*` function, or a `\i`-included snippet) so
  the other five affected suites can adopt it in one line. Deferred rather than
  rejected — a generic PostgreSQL test utility does not belong in the shipped
  extension script, and a cross-suite include breaks the property that every suite
  here is self-contained (`CREATE EXTENSION` … `DROP EXTENSION`). Worth revisiting
  when the other five are fixed.

## Consequences

- `17_delete` is deterministic under concurrent same-database snapshots.
- **Five suites remain exposed** — `19_merge`, `20_merge_reclaim`,
  `21_retired_descriptors`, `34_merge_multifield`, `37_merge_positions`. They are
  not fixed here: `20_merge_reclaim` already performs deliberate horizon
  manipulation of its own (it burns transactions to advance past a `retire_xid`),
  so dropping this wait into it needs thought rather than a copy-paste. They flake
  at the same low rate `17_delete` did.
- The suite gains a bounded wait that is a no-op on a quiet cluster, and ~40 lines
  of comment explaining a failure mode that is otherwise very expensive to
  rediscover — the per-database horizon, the `PROC_IN_VACUUM` exclusion, and the
  per-transaction `pg_stat_activity` snapshot are each individually surprising.

## Addendum (2026-08-04)

Extended the wait to the five suites this record's Consequences section flagged as
still exposed — `19_merge`, `20_merge_reclaim`, `21_retired_descriptors`,
`34_merge_multifield`, `37_merge_positions` — and factored the wait itself into a
per-suite `pg_temp` helper, which is the "shared helper for all six" the commit
that established this record asked for. `17_delete` was converted from its inline
`DO` block to the same helper for consistency; its long explanatory comment (the
canonical write-up of the mechanism) was kept verbatim.

### Which assertion flaked in each suite

Each suite asserts on internal AM state that only updates when VACUUM (or a merge's
internal reclaim, which calls the same `bm25_reclaim_retired` VACUUM feeds) actually
ran against a clear horizon:

| Suite | Assertion | Expected | Observed under a held snapshot |
|---|---|---|---|
| `19_merge` | `alpha_postings_after` | 49 | 50 (doc 1's tombstoned posting never dropped) |
| `20_merge_reclaim` | `size_stable` | `t` | `f` (retired pages never reclaimed, so the follow-up seal extends instead of reusing) |
| `21_retired_descriptors` | `retired_entries`, `descriptors_bounded` | 0, `t` | 40, `f` (nothing in the retired chain drains) |
| `34_merge_multifield` | `retired_after_reclaim` | 0 | 4 (same drain failure, keyed-index variant) |
| `37_merge_positions` | `size_stable` (Part 3) | `t` | `f` (POS pages of merged-away segments never reused) |

### Why a `pg_temp` helper, defined per suite

Each suite is self-contained (`CREATE EXTENSION` at the top, `DROP EXTENSION` at the
end) and pg_regress gives each suite its own session, so there is no cross-suite
`\i`-able location that would not either leak into the shipped extension script or
break that self-containment. A `pg_temp.wait_for_xmin_horizon()` function is
session-local, needs no cleanup (it disappears with the session), and lets each
suite carry a one-line pointer comment back to this record instead of repeating the
~20-line rationale. The function body is unchanged from `17_delete`'s original,
including the `pg_stat_clear_snapshot()` call inside the loop, which is exactly as
load-bearing here as it was there.

### `20_merge_reclaim`: wait before the burn, not after

`20_merge_reclaim` deliberately manipulates the horizon: it burns five xids
(`txid_current()` in autocommit) to drive the cluster past the merge's retire_xid,
then VACUUMs so `bm25_reclaim_retired` actually drains. The burn is necessary but
not sufficient — it only advances `nextXid`; it does nothing about a snapshot
another backend has already taken, and the horizon `bm25_reclaim_retired` checks
against is the **minimum** xmin across every backend in the database. A stale held
snapshot pins that minimum regardless of how many xids get burned afterward. So the
wait goes **before** the burn (wait → burn → VACUUM): once no other backend holds a
snapshot, the burn's xids are the newest thing any subsequent snapshot could
observe, and they are what actually pushes the horizon past retire_xid. The same
ordering was applied to `21_retired_descriptors` and `34_merge_multifield`, which
have the identical burn-then-VACUUM reclaim shape.

The suite's *first* VACUUM (right after the DELETE, before the merge) was
deliberately left unprotected: whether it tombstones the deleted docs or not, the
subsequent `bm25_merge()` still selects segments to merge on segment-count/size
triggers independent of tombstone fraction (confirmed by reading `bm25_merge_select`
in `src/bm25_merge.c`), and the suite's own consistency check
(`no_tombstones_after_merge`) only asserts that merge's live-count bookkeeping is
internally coherent, not that any particular doc was actually tombstoned by that
VACUUM. Only the second VACUUM's result is asserted.

### `37_merge_positions`: every post-anchor cycle needs its own wait

Part 3 of `37_merge_positions` runs ten merge cycles with **no VACUUM at all** —
`bm25_merge()` calls `bm25_reclaim_retired` internally (`src/bm25_merge.c`), so each
cycle's merge is itself a reclaim point for the *previous* cycle's retirement. The
first attempt at this fix placed a single wait before the final cycle's burn, on the
reasoning that `bm25_reclaim_retired` rescans the whole retired chain on every call,
so one clear-horizon pass at the end should sweep up anything earlier cycles missed.
Measurement showed this was wrong: 13/16 trials still failed `size_stable` under the
rolling holder. The reasoning missed that **reclaiming late is not the same as
reclaiming in time** — a cycle's freed pages are only available to the *next*
cycle's `bm25_seal()` allocator, and `pg_relation_size` does not shrink mid-file
just because a later reclaim eventually drains the backlog. A cycle whose reclaim
missed the horizon has already forced its next seal to extend the relation, and
that growth is permanent by the time `size_stable` is checked at the end. The fix
is a wait before the burn of **every** post-anchor cycle (five waits, not one).
Pre-anchor cycles were left unprotected: the suite's own comment establishes that
growth up to the anchor is identical whether earlier reclaims succeeded or not,
since the anchor is captured *as* the baseline, not compared against a fixed
constant.

### Verified

Rolling holder (1 s snapshot, 0.2 s gap) around all six suites, 16 trials each way:

| | trials | trials with a failure | total suite-failures |
|---|---|---|---|
| without the wait (`git stash`) | 16 | 16 | 22 |
| with the wait | 16 | **0** | **0** |

The first (incorrect) single-wait version of the `37_merge_positions` fix was also
measured and rejected on this evidence: 13/16 trials still failed `size_stable`
before the per-cycle wait was added.

The full 83-suite `installcheck` (no holder) passed 3/3 clean runs after the fix.

## Addendum (2026-08-04, second)

A **seventh** suite, `75_seg_chain_cursor`, has the same defect and was missed by
the first addendum. It surfaced as a red `main` immediately after the L2
decode-bounds work merged: `bm25_debug_seg_doc_live('sc_idx', 0, 0)` returned `t`
where the suite expects `f`, because the `DELETE ... ; VACUUM sc;` at the top of
its LIVEDOCS section had not tombstoned. Reproduced deterministically with one
held `REPEATABLE READ` snapshot, and fixed with the same `pg_temp` helper placed
before that VACUUM.

### Why it was missed, and what changed

The first addendum fixed the five suites **named in PR #88's commit message**
rather than searching for the defect's shape. That list came from one specific
experiment and was never claimed to be exhaustive. `75_seg_chain_cursor` was
added later (H16, ADR 0035) and asserts on liveness through the debug probes, so
it inherited the same dependency without inheriting the fix.

This is the same failure mode ADR 0040 records for the trust-boundary sweep —
three consecutive fixes landing one reader short because each addressed the sites
an issue enumerated instead of the sites the defect actually occupies. It
recurred here one PR later, in a different subsystem, which is evidence the
countermeasure has to be mechanical rather than remembered.

The enumeration is now mechanical: a suite needs the wait if it contains a
`VACUUM` that precedes any assertion reading AM-internal post-VACUUM state
(`bm25_debug_tombstone`, `bm25_debug_seg_doc_live`, `bm25_debug_retired_count`,
`bm25_debug_merge_plan`, `bm25_stats`). Applying that predicate across `sql/*.sql`
yields exactly seven suites, all seven now protected. Suites that VACUUM without
such an assertion (`18_vacuum_reclaim`, `33_keymap`, `44_wand_skip`,
`46_m6_boolean`, `47_m6_wildcard`, `55_format_compat`, `61_negative_idf`,
`62_segcat_chain`) do not need it: MVCC hides a deleted row from the executor
whether or not the tombstone was written, so only assertions reading the AM's
internal state are sensitive.

### Verified

Rolling holder (1 s snapshot, 0.2 s gap), `75_seg_chain_cursor`, 12 trials each way:

| | trials | trials with a failure |
|---|---|---|
| without the wait | 12 | **9** |
| with the wait | 12 | **0** |

Full 85-suite `installcheck` (no holder): 3/3 clean runs.

## Addendum (2026-08-16)

**The mechanical predicate above is now too narrow, and its exclusion of
`18_vacuum_reclaim` is wrong.** Issue #135.

The predicate reads: a suite needs the wait if it contains a `VACUUM` that
precedes an assertion reading AM-internal post-VACUUM state
(`bm25_debug_tombstone`, `bm25_debug_seg_doc_live`, `bm25_debug_retired_count`,
`bm25_debug_merge_plan`, `bm25_stats`). Both places this record excludes
`18_vacuum_reclaim` give the same reason — it "only asserts query results", which
are MVCC-correct whether or not the tombstone was written.

That reasoning is sound *about tombstoning*, and it silently assumed tombstoning
was the only horizon-sensitive thing a VACUUM does. It is not. The other one is
**page reuse**, and `18_vacuum_reclaim` is the suite that measures it — its
`pages_c3 / pages_c1 < 3.0` bound is an assertion that freed pages came back
through `bm25_page_alloc`. When the record was written, every page
`18_vacuum_reclaim` counted on reusing was freed with `InvalidFullTransactionId`,
which the allocator accepts unconditionally, so nothing in that suite touched a
horizon and the exclusion was correct in fact if not in reasoning. Issue #135
changed the fact: a drained pending page is now stamped with a real
`ReadNextFullTransactionId()` at both free sites (`bm25_pending_truncate` and
`bm25_reclaim_orphans`'s `BM25_PAGE_PENDING` case — see ADR 0019's addendum), so
those pages are reusable only once the cluster horizon clears. One unrelated
backend holding a snapshot — autovacuum, a parallel test, a stray `psql` — now
pins every cycle into extending instead of reusing and flips `bounded` to `f`,
reporting a page leak in an AM that does not have one.

**Corrected predicate.** A suite needs the wait (and the xid burn) if it contains a
`VACUUM`, or any operation that frees pages with a horizon stamp, that precedes
either:

1. an assertion reading AM-internal post-VACUUM state (the original list), **or**
2. an assertion that depends on freed pages being REUSED — `bm25_debug_npages`
   ratios, `pg_relation_size` bounds, any "footprint does not grow" claim.

Clause 2 adds exactly one suite today, `18_vacuum_reclaim`, which now defines the
same `pg_temp.wait_for_xmin_horizon()` helper and, in each of its three cycles,
waits and burns three xids **between the `bm25_seal()` and the `VACUUM`** — not at
the top of the next cycle. The placement is the interesting part and is worth
stating once, because it generalizes: `amvacuumcleanup` runs
`bm25_seal_index` → `bm25_reclaim_orphans` → `bm25_merge_maybe` →
`bm25_reclaim_retired` **in one transaction**, so a merge that VACUUM selects
allocates against pages the same VACUUM (or the explicit seal immediately before
it) has just stamped. There are therefore two consumers of a freed page per cycle
— that intra-VACUUM merge, and the next cycle's `INSERT` — and a burn placed after
the VACUUM covers only the second. Wait-then-burn, as always, since a held snapshot
pins the horizon regardless of how many xids follow it.

`33_keymap`, `44_wand_skip`, `46_m6_boolean`, `47_m6_wildcard`, `55_format_compat`,
`61_negative_idf` and `62_segcat_chain` remain correctly excluded: they VACUUM but
assert neither internal state nor footprint. `20_merge_reclaim`, `34_merge_multifield`
and `37_merge_positions` already satisfy clause 2 — each burns xids inside its cycle,
which absorbs the pending deferral without further change.

**The TAP suites need the same treatment and could not get it from a `pg_temp`
helper.** Four of them compare page counts or relation sizes across a run of
statements that assign **no xid at all** — `VACUUM`, `bm25_seal()`, and read-only
`SELECT`s all leave `nextXid` exactly where it was, so nothing in such a run can
clear a `ReadNextFullTransactionId()` stamp. `t/006_v4_crash.pl` and
`t/008_m5_crash.pl` are the sharp cases: both assert `$pages2 <= $pages1` with
**zero slack** across two consecutive VACUUMs. Each now burns two xids between
them. `t/004_crash_orphan.pl` burns before its second cycle's `INSERT`, and
`t/004_crash_seal.pl` between its VACUUMs — that last one also carried the comment
"Two VACUUMs cross the XID horizon", which was never true and is now corrected
in place, as is a similar note in `t/005_replica_reuse.pl`.

**One correction to a cost claim that is not in this record but travels with it.**
Issue #135 predicted the horizon stamp would cost "one extra VACUUM cycle before
the pages return". It does not. `GlobalVisTestIsRemovableFullXid` re-runs
`GlobalVisUpdate` and retests rather than trusting a cached bound, so on a quiet
cluster the stamp clears after roughly one completed transaction — which is why
the burn above is three `txid_current()` calls and not a VACUUM. The wait exists to
handle the *other* backend's snapshot, not to wait out a slow horizon. The real
cost of the stamp is in the allocator and is bounded separately
(`BM25_ALLOC_MAX_REJECTS`, ADR 0019's addendum).
