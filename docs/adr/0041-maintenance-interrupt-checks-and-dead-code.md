---
id: 0041
title: Make MAINTENANCE paths cancellable, and the dead-check trap that nearly shipped
date: 2026-08-04
status: Accepted
summary: VACUUM, seal, merge, pending-drain, and query-parse/glob now carry real interrupt checks across nine previously-zero-CFI files, after an adversarial review found nine of the first draft's checks were dead code sitting under a held buffer content lock -- fixed by moving the segment-build check to the one genuine lock-free instant chain_ensure's rotation creates, three chain-walk bounds turned from silent early-stops into loud errors, and two new PGC_SUSET wildcard caps.
---

# 0041. Make MAINTENANCE paths cancellable, and the dead-check trap that nearly shipped

## Context

ADR 0024 (H3/#43) made the SCAN paths -- `@@@`, ranked `ORDER BY` -- honor
`CHECK_FOR_INTERRUPTS`. Everything else in the extension did not: VACUUM's
three phases (`bm25_pending_mark_dead`, `bm25_reclaim_orphans`,
`bm25_reclaim_retired`), `bm25_seal`'s drain-then-build, `bm25_merge`'s
accumulate-then-rewrite, and the wildcard pattern matcher all ran to
completion regardless of `statement_timeout`, `pg_cancel_backend`, or a
standby recovery conflict. Sixteen files carried zero `CHECK_FOR_INTERRUPTS`.
On a large corpus (a multi-million-row pending list, a same-layer merge of
several large segments, VACUUM sweeping a wide extent) any of these could run
for many seconds to tens of seconds, uncancellable, exactly the class of bug
ADR 0024 fixed for scans.

**A `CHECK_FOR_INTERRUPTS()` under a buffer content lock is dead code.**
`LWLockAcquire` calls `HOLD_INTERRUPTS()`, and `ProcessInterrupts()` (called
from `CHECK_FOR_INTERRUPTS()`) returns immediately while
`InterruptHoldoffCount != 0` -- see `INTERRUPTS_CAN_BE_PROCESSED` in
`miscadmin.h`. This is not a subtle corner case; it is the ordinary state of
any tight per-item loop that holds a page SHARE- or EXCLUSIVE-locked across
iterations, which is exactly the shape of `bm25_seg_build.c`'s `ChainWriter`:
`chain_write` holds `w->tailbuf` content-locked across every item appended to
that page (by design -- re-locking per item would be its own regression), so
a `CHECK_FOR_INTERRUPTS()` placed inside that per-item loop, or in any of its
`bm25_segment_build_orphans` callers' per-doc/per-term loops, never fires.
An adversarial review of the first draft of this pass found exactly that: 8
checks added to `bm25_seg_build.c`'s DOCMAP/NORMS/POST/POS/DICT write loops,
and 1 in `bm25_merge.c`'s per-term DICT-iteration loop, were dead on arrival.

The fix does not add a lock-free instant to the write loops -- there isn't
one to add, short of re-locking every item, which would reopen the
performance regression the batched-lock design exists to prevent. Instead it
moves the check to the one place a genuine lock-free instant already exists:
`chain_ensure`'s page rotation. The OLD rotation order allocated the new page
(EXCL-locked) *before* releasing the old one, to learn the new page's block
number for the old page's forward link -- so the two tail buffers were held
**simultaneously** at every rotation, and one or the other continuously the
rest of the time. There was never an instant with zero buffers held.

`bm25_seg_dict_iter_next` (`bm25_seg_read.c`) had the same shape one level
down: it holds its current DICT page SHARE-locked across every entry
returned from that page, so a check in a caller's per-entry loop around it
(the dead one in `bm25_merge.c`) was equally inert.

## Decision

**Segment-build write path (`bm25_seg_build.c`).** Restructure
`chain_ensure`'s rotation so the OLD tail page is fully **finished** --
`GenericXLogFinish` + `UnlockReleaseBuffer` -- *before* the new page is
allocated, creating a genuine lock-free instant between them:

```
chain_flush(w)              /* release the OLD tail, fully */
CHECK_FOR_INTERRUPTS()      /* the ONE lock-free instant */
bm25_page_alloc(...)        /* EXCL-lock the NEW tail */
```

The cost: the old page's `nextblk` forward link can no longer ride the SAME
WAL record as its content, because the new block number isn't known until
after the old page is released. The old page is flushed with `nextblk` still
Invalid (`bm25_page_init`'s default), then a second, tiny Generic WAL record
reopens it -- once the new block number is known -- to backfill just that one
field. One extra WAL record per PAGE (a DOCMAP page holds roughly 1300
records before it rotates, so this is not one extra record per document),
in exchange for cancellability that actually fires. Safe under a crash
between the two records: these are orphan pages, unreachable until the
segment's publish record runs far downstream, so a chain that ends up one
link short after a crash is exactly as harmless as any other abandoned
orphan page (same reasoning as every other orphan-page crash window in this
tree). Never unlock a buffer that is still registered in an open
`GenericXLogState` -- the old page's *own* record is finished (`Finish` +
unlock) before the *next* `GenericXLogStart` opens a new window on the
backfill.

The 8 dead per-item checks are removed from `bm25_segment_build_orphans`'s
DOCMAP/NORMS/POST/POS/DICT loops and replaced with comments explaining why a
per-item check there would be dead, pointing back at `chain_ensure`'s header
comment for the actual fix. `bm25_merge.c`'s dead per-term dict-iteration
check is likewise removed; `bm25_seg_dict_iter_next` gained its own check
instead, placed in its own genuine lock-free instant (between releasing the
previous page and locking the next), which covers the merge feed, wildcard
dictionary expansion, and the debug SRFs that walk the same iterator all at
once.

**VACUUM (`bm25_fsm.c`, `bm25.h`).** `bm25_reclaim_orphans`'s `mark_chain`
walk, its orphan-list walk, and its full-extent sweep all gained
`BM25_VACUUM_DELAY_POINT()` (moved to `bm25.h` from `bm25_handler.c` so
`bm25_fsm.c` can share it without duplicating the PG 17/18
`vacuum_delay_point` signature guard) -- the throttling form, not a bare
`CHECK_FOR_INTERRUPTS`, because this path is exclusively VACUUM
(`amvacuumcleanup`'s only caller), and a bare check would silently bypass
the autovacuum cost-delay budget (ADR 0025's precedent for
`bm25_bulkdelete`). `reclaim_one_range`, `bm25_reclaim_retired`'s own chain
walk, and the two `bm25_debug_retired_*` probes that share its shape got
plain `CHECK_FOR_INTERRUPTS` (they are not exclusively VACUUM-path, or, for
the debug probes, are not throttled anywhere else either).

**Chain-walk bounds must not turn a loud failure into a silent one.**
Several of these VACUUM/maintenance walks (`bm25_pending_mark_dead`,
`bm25_pending_drain`, `bm25_pending_truncate`, `bm25_fsm.c`'s
`reclaim_one_range` and `bm25_reclaim_retired`, `bm25_analyzer.c`'s
`bm25_fieldcfg_read`, `bm25_segment.c`'s two DICT-walking debug SRFs) also
gained a `blk < nblocks` bound alongside their new `CHECK_FOR_INTERRUPTS`,
following the corruption-backstop pattern `bm25_fsm.c`'s `mark_chain` chain
already used: a corrupt/out-of-extent `nextblk` cannot walk the loop past
the relation's own extent forever. For three of these -- `bm25_pending_drain`
and `bm25_pending_mark_dead` specifically, and every other bounded walk in
this pass on general principle -- the bound is followed by a **loud**
post-loop check: `if (blk != InvalidBlockNumber) ereport(ERROR,
errcode(ERRCODE_INDEX_CORRUPTED), ...)`. A pending/reclaim chain always
terminates naturally at `InvalidBlockNumber`; landing at the bound with `blk`
still set means the bound cut the walk short, not that the chain ended.
Silent about that in `bm25_pending_drain` specifically would be worse than
the bound not existing at all: `bm25_seal_index`'s publish record resets
`pending_head` on the premise that the drain it is publishing always
consumed the *whole* chain (see `bm25_pending_truncate`'s header, which is
why that function must never touch the pending anchor itself). A silently
truncated drain would publish a partial accumulator, advance the anchor past
pending docs the drain never read, and `bm25_pending_truncate` would then
detach and free the rest -- committed, heap-visible rows becoming unfindable
until REINDEX. Loud beats silently wrong; see ADR 0040 for the identical
principle applied to `pd_lower` underflow. (`bm25_pending_truncate` itself
keeps the bound but not the loud check: by the time it runs, the identical
block range was already walked without error by the drain that produced it,
under the same metapage singleton, so an out-of-extent link there is
unreachable in practice; a leaked recycle opportunity, not a correctness
risk, if it were ever hit.)

The bound is *not* a cycle guard. `blk < nblocks` stops a stray link that
points **past** the relation's own extent; it does nothing about a corrupt
`nextblk` that cycles back to an **already-visited in-extent** page --
`mark_chain` is the one walk in this pass with an independent cycle guard
(a `reachable[]` bitmap); every other bounded walk here would still loop
forever on an in-extent cycle. What makes that survivable is
`CHECK_FOR_INTERRUPTS`, not the bound. Every comment added alongside one of
these bounds says so explicitly, because ADR 0039/0040's own experience is
that an unstated invariant here gets restated as fact by the next reader
(ADR 0040's finding 2 is the same lesson: a comment asserting a guarantee
that was never actually true).

**Wildcard pattern matching (`bm25_query.c`, `bm25_handler.c`, `bm25.h`).**
`bm25_glob_match` is O(pattern-length x term-length) worst case, and neither
dimension was bounded before this pass -- `bm25_wildcard_min_prefix` bounds
how much of a pattern must be literal, and `bm25_wildcard_max_expansions`
bounds how many dictionary matches a pattern can expand to, but nothing
bounded the pattern's own text before evaluation. Two new PGC_SUSET GUCs,
`bm25_native.wildcard_max_pattern_length` (default 256, max 8192) and
`bm25_native.wildcard_max_stars` (default 8, max 64), close that, checked in
`validate_wildcard_pattern` before any scan of the pattern buffer. Neither
GUC reaches `bm25_debug_glob_match`: that SQL-visible unit-test function
calls `bm25_glob_match` directly, bypassing `validate_wildcard_pattern`
entirely (the point of the probe is to exercise the primitive without the
query-tree machinery around it) -- its cancellability comes from the
`CHECK_FOR_INTERRUPTS` `bm25_glob_match`'s own main loop gained in this same
pass, and access to it at all is gated by the `bm25_debug_*` REVOKE loop
(ADR 0020), not by either new GUC. `MarkGUCPrefixReserved("bm25_native")` is
called after every `bm25_native.*` GUC is registered, so a typo'd parameter
name (`wildcard_max_starss`) errors instead of silently being accepted as a
placeholder USERSET custom GUC -- which would otherwise let a non-superuser
silently no-op the guardrail they meant to (fail to) tighten.

**Regression coverage.** The CI floor (`ci.yml`) moved from a single
aggregate count to an aggregate-plus-per-file shape: floor 61 overall (was
42 pre-pass), with the pass's arithmetic spelled out in a comment (42 base +
26 added − 7 removed via the dead-check fix = 61), plus a per-file floor for
each of the nine files this pass brought up from zero. `sql/80_maintenance_
interrupts.sql` is new: it builds a pending list large enough that an
uncancelled `bm25_seal()` takes just over a second (500,000 docs, ~1.2 s on
the development machine), then wraps the call in a PL/pgSQL
`BEGIN ... EXCEPTION WHEN query_canceled` block that measures
`clock_timestamp()` elapsed and returns a boolean
(`elapsed < interval '300 ms'`) -- output-comparable, unlike a raw duration,
and unlike a bare `SET statement_timeout; SELECT ...` (ADR 0024's own
suite's header explains why that proves nothing: pg_regress compares
output, and an uncancelled-but-eventually-erroring call prints the identical
`ERROR: canceling statement due to statement timeout` either way).

A first version of this suite built its pending list via 3,000,000 ordinary
INSERTs into an already-indexed table and covered both `bm25_seal()` (~7.5 s
uncancelled) and `bm25_merge()` (4 x 400,000-doc same-layer segments,
~4.5 s). It worked and was verified the same way described below, but took
~8 minutes end to end -- the INSERT path's per-row tokenize-and-WAL-log-to-
pending-list cost, not the seal or merge themselves, which together were
under 12 seconds of that. Run on every push across two PG versions plus the
hardening job, that is a ~6x pipeline tax for one suite. Cut to the 500,000-
row seal-only version below; see the suite's own header for the full
before/after and the two negative results (one about `CREATE INDEX`, one
about `bm25_merge()`) that shaped the final design.

## Non-obvious findings

1. **Counting call sites is not evidence of cancellability.** The CI floor
   greps for `CHECK_FOR_INTERRUPTS()` occurrences and cannot tell a live
   check from a dead one; a naive floor bump to 68 would have enshrined the
   9 inert checks this review found, permanently, since nothing would ever
   make that number go down again by accident. This is *why* `sql/80` exists
   as a behavioral (latency) test rather than another structural (grep)
   floor: the floor can only ever catch a check being *deleted*, never a
   check that never worked in the first place.

2. **A `CHECK_FOR_INTERRUPTS()` compiles, links, and passes every existing
   test whether or not it ever fires.** Nothing about the 9 dead checks
   looked wrong locally -- they were syntactically in a loop, in a
   maintenance file, following the exact pattern used everywhere else in
   this pass. The only way to know a check is live is to reason about what
   is locked at that program point, or to measure. This pass did both:
   the fix's own header comment states the lock-state reasoning explicitly
   (`chain_write` holds `w->tailbuf` across iterations "by design"), and
   `sql/80` measures the consequence.

3. **Fixes with overlapping coverage make black-box regression tests unable
   to attribute a failure to one specific function -- and that is fine.**
   Verifying `sql/80` catches a regression (per this repo's "a floor/suite
   is only worth adding if you verify it fails on the pre-fix tree" policy)
   turned up a real surprise: reverting *only* `chain_ensure`'s rotation-gap
   fix does **not** make the suite fail. `bm25_seal_index` is drain-then-
   build; `bm25_pending_drain` (this same pass) gained its own
   `CHECK_FOR_INTERRUPTS`, independent of `chain_ensure`. A 10 ms
   `statement_timeout` on the whole `bm25_seal()` call is caught by
   whichever phase's check it reaches first, which chronologically is
   always the read side (drain runs before build). Reverting chain_ensure
   alone: still cancels in ~12-14 ms (unaffected). Reverting chain_ensure
   **and** `bm25_pending_drain`'s check together: cancellation degrades to
   ~660 ms on the 500,000-doc suite, past its 300 ms boundary -- correctly
   caught. `sql/80`'s header documents both results. This is defense in
   depth working as intended, not a test gap: `bm25_pending_drain`'s check
   is independently load-bearing for a DIFFERENT case chain_ensure's fix
   does not cover on its own -- a pending list that is mostly a fast drain
   of many small documents but takes a long time in aggregate purely from
   drain-side work (HTAB accumulation), never reaching the build phase's
   own checks within a short timeout at all. The lesson is narrower than
   "always isolate the fix under test": when two fixes cover the same
   call path by design, a black-box latency test can only ever prove the
   PATH is fixed, not attribute which function fixed it, and that is the
   correct scope for it to have.

4. **A custom index AM's own interrupt handling can be completely invisible
   to a black-box cancellation test, because PostgreSQL's generic
   infrastructure already covers the path the test would otherwise probe.**
   A `CREATE INDEX`-based version of `sql/80` was tried first: cancel
   `CREATE INDEX ... USING bm25_native` on a table pre-populated with NO
   index yet (so setup is a cheap untokenized heap load, and `ambuild`'s
   tokenize-and-build happens entirely inside the timed, cancellable
   statement). Measured cheap (~1.6 s heap load + ~5.3 s `CREATE INDEX` for
   1.5M rows, vastly better than the pending-list design). But
   `heapam_index_build_range_scan` (`src/backend/access/heap/
   heapam_handler.c`, confirmed against the local PG 18.3 source tree,
   line ~1335) calls `CHECK_FOR_INTERRUPTS()` on every tuple of the
   heap-scan phase every index AM's `ambuild` runs through, via
   `table_index_build_scan` -- entirely outside `bm25_native`, added by
   neither this pass nor any earlier one. A 10 ms timeout is caught there,
   always, before `bm25_build_callback` or `chain_ensure` are ever
   reached. Verified: reverting `chain_ensure`'s fix, `CREATE INDEX` still
   cancelled in ~10-13 ms across three separate runs -- indistinguishable
   from the fixed tree. Unlike finding 3 (two `bm25_native` fixes
   overlapping each other), this overlap is with core PostgreSQL
   infrastructure that exists independently of whether `bm25_native` has
   any interrupt handling in its build path AT ALL, so a `CREATE INDEX`-
   based cancellation test would carry zero regression-detection value no
   matter how it were tuned. Cheapness and test validity are different
   axes; a design can win decisively on one and lose completely on the
   other, and only running the actual acceptance check (not just reasoning
   about which functions are on the call path) reveals which.

## Alternatives considered

- **Re-lock the tail page per item in the write loops**, so a
  `CHECK_FOR_INTERRUPTS` could sit inside the per-item loop and actually be
  live. Rejected: this is the exact per-item lock/unlock cost the batched-
  write design exists to avoid (a DOCMAP page holds ~1300 records; re-
  locking 1300 times to write one page is the regression, not the fix).
- **A bare `CHECK_FOR_INTERRUPTS()` in `bm25_reclaim_orphans`'s VACUUM-only
  loops**, matching the plain form used in `bm25_reclaim_retired` and the
  debug probes. Rejected: this path is reachable only from
  `amvacuumcleanup`, and ADR 0025 already established that autovacuum's
  cost-delay budget must be honored on every VACUUM-exclusive loop in this
  extension, not just cancellation.
- **Fold the `blk < nblocks` bound into a shared helper**, the way
  `bm25_page_content_bytes` (ADR 0040) centralized the page-content-length
  derivation. Considered and deferred: the six sites bounded in this pass
  are a mix of EXCLUSIVE-locked mutating walks (pending drain/mark-dead/
  truncate, reclaim) and SHARE-locked read-only ones (the two debug SRFs),
  with different post-loop behavior (loud error vs. quiet leak-tolerant
  return per the `bm25_pending_truncate` exception above) -- a shared
  helper would need to parameterize the post-loop action, which is more
  machinery than six call sites justify today. Worth revisiting if a
  seventh walker needs the same bound.
- **Isolate `chain_ensure`'s fix in `sql/80` via a calibrated (non-tiny)
  `statement_timeout`**, picked to land after the measured drain duration
  and before total completion. Tried and rejected: direct measurement
  showed drain's own duration varies by more than the size of the window
  being targeted, run to run on the same machine (observed 3.3 s and >4.5 s
  across consecutive runs against the same 3M-doc setup, before the suite
  was cut down), almost certainly checkpointer I/O contention on the
  development cluster. A calibrated midpoint timeout would be exactly as
  fragile as a raw wall-clock assertion -- the class of thing this repo's
  CI-floor policy already rejects for the same reason. The tiny (10 ms)
  timeout this pass uses instead is immune to that noise: elapsed is
  always compared only against the boundary, never against the timeout's
  own value.
- **Cancel `CREATE INDEX` instead of `bm25_seal()`**, to get the setup-cost
  win (a plain heap load instead of tokenize-on-insert) without shrinking
  the suite's data size. Tried and rejected: see finding 4. PostgreSQL's
  own generic per-tuple `CHECK_FOR_INTERRUPTS()` in
  `heapam_index_build_range_scan` makes this design blind to `bm25_native`'s
  own interrupt handling entirely, regardless of data size or timeout
  tuning -- cheaper is not a trade worth making against zero test validity.
- **Keep a `bm25_merge()` cancellation case alongside the seal one.** Tried
  at a proportionally-scaled-down size (4 x 50,000-doc same-layer segments,
  merge ~0.67 s uncancelled) and dropped: reverting `chain_ensure`,
  `bm25_pending_drain`, `bm25_merge.c`'s own per-doc check,
  `bm25_seg_dict_iter_next`'s check, AND `bm25_page_alloc`'s check
  (`bm25_meta.c`, shared by every writer including `chain_ensure`) all
  together still cancelled the merge in ~12 ms -- some further
  interruptibility this investigation did not fully identify (possibly
  `bm25_page_alloc`'s own check firing on the very first page of a brand
  new output chain, which is genuinely lock-free the way finding 3
  describes, before any subsequent rotation would be) keeps saving it even
  with every `bm25_native`-specific check on the path removed. Growing the
  merge case enough to reliably exceed whatever margin that gives would
  cost real suite time for a case `bm25_seal()` already exercises anyway
  (`bm25_merge_maybe` calls `bm25_seal_index` first).

## Consequences

- VACUUM, seal, merge, pending-drain/mark-dead, and wildcard pattern
  evaluation now honor `statement_timeout`, `pg_cancel_backend`, and standby
  recovery conflicts, closing the same class of defect ADR 0024 closed for
  scans -- for a large pending list or a large same-layer merge, previously
  the operator's only recourse was to wait it out or kill the backend.
- `bm25_seg_build.c`'s segment-build write path now costs one extra small
  Generic WAL record per rotated page (not per document) to backfill the
  forward link the restructured rotation order can no longer carry on the
  original record. Measured cost: not observable in `installcheck` suite
  timing (all 86 suites pass, no suite moved outside prior noise).
  Cancellation granularity for a seal/merge/build's write phase is one page
  rotation (~1300 documents for DOCMAP).
- A previously-working wildcard pattern with 9 or more `*` characters, or
  longer than 256 bytes, now errors by default
  (`ERRCODE_PROGRAM_LIMIT_EXCEEDED`) where it previously succeeded. This is
  a user-visible compatibility change for any caller of `bm25_wildcard()`
  with an unusually large pattern; the fix is a superuser (or a role granted
  `SET` on the parameter, PG15+) raising
  `bm25_native.wildcard_max_pattern_length` / `_max_stars`, matching the
  existing `wildcard_min_prefix`/`wildcard_max_expansions` compatibility
  posture (ADR 0025).
- `bm25_reclaim_orphans` now honors the autovacuum cost-delay budget in its
  full-extent sweep and both mark-walks, not just cancellation -- a large,
  mostly-live relation's orphan reclaim no longer runs unthrottled between
  cancellation checks.
- Three chain walks (`bm25_pending_drain`, `bm25_pending_mark_dead`, and by
  the same pattern every other bounded walk in this pass) that could
  previously loop forever on a corrupt out-of-extent `nextblk` now fail
  loudly with `ERRCODE_INDEX_CORRUPTED` instead of looping forever (pre-
  this-pass) or, had the bound been added silently, terminating early and
  wrong. None of these bounds stop an in-extent cycle; that protection is
  `CHECK_FOR_INTERRUPTS`, not the bound, and every comment introducing one
  of these bounds says so.
- `sql/80_maintenance_interrupts.sql` is a real, verified regression guard
  for "seal stopped being promptly cancellable" (confirmed: passes on the
  current tree at 500,000 pending docs in ~5 s total suite runtime, fails
  when the seal path's two overlapping fixes are both reverted, at ~660 ms
  past its 300 ms boundary) but cannot, by construction, attribute a future
  regression to one specific function when its fix overlaps another's
  coverage -- see finding 3 -- and cannot say anything at all about
  `bm25_merge()` or `CREATE INDEX`'s own cancellability -- see finding 4
  and the alternatives above. The CI floor's per-file counts remain the
  first signal for "which file lost a check"; `sql/80` is the signal for
  "seal stopped being cancellable at all."
- Follow-up not done here: no equivalent latency suite for `bm25_merge()`
  (tried, dropped -- see alternatives), VACUUM's own three phases
  (mark-dead, orphan-reclaim, retired-reclaim), `CREATE INDEX` (tried,
  rejected as invalid rather than merely dropped -- see finding 4), or
  wildcard-pattern evaluation under the new caps. All of these remain
  structurally covered (CI floor per-file counts,
  `sql/67_wildcard_guc_privileges`'s behavioral GUC checks) but not
  latency-measured the way `bm25_seal()` is here. Worth revisiting if any
  of their own cancellability is ever brought into question specifically
  -- keeping in mind finding 4's lesson that a cheap-looking design can
  still be a vacuous test, and must be verified against a real revert
  before it is trusted.

## Addendum (2026-08-23)

Issue #145 (ADR 0083) corrects one claim in the Decision above and extends the
lesson to three loops this pass did not reach.

The claim: "`bm25_seg_dict_iter_next` gained its own check instead, placed in
its own genuine lock-free instant, which covers the merge feed, wildcard
dictionary expansion, and the debug SRFs that walk the same iterator all at
once." Half of that was never true. `bm25_debug_terms` and
`bm25_debug_postings` (`bm25_segment.c`) and `bm25_debug_segterms`
(`bm25_seg_read.c`) open-code their own DICT page walk and have never gone
through that iterator, so the check inside it never covered them at all. What
they had was a per-PAGE check of their own, and it went dead the moment the
page lock was taken — with, in `bm25_debug_postings`' case, an entire nested
postings replay inside the window.

All three now decode a page COPY with no content lock held and check per ENTRY.
The wildcard expander, which does use the iterator, additionally calls
`bm25_seg_dict_iter_unlock` per term and places its check in the gap, exactly
as the merge feed does — so it too is no longer covered only by the iterator's
own check.

This is the third time the same pattern has appeared (the 68 → 61 drop above,
then #139's POS/POST copies, now this): the interrupt COUNT is unchanged by all
three of these fixes, because the checks already existed and were simply dead.
The CI grep floor remains a delete-detector and never evidence of
cancellability. `sql/80_maintenance_interrupts` gained a `bm25_debug_postings`
latency harness, A/B-verified against the pre-fix tree (f at ~1.16 s, t at
~11 ms, boundary 300 ms), which is what actually pins this one.

## Addendum (2026-10-05)

XCUT-08 (#300), decided 2026-10-05: the five bare `CHECK_FOR_INTERRUPTS()` sites on the
maintenance path that look convertible to `BM25_VACUUM_DELAY_POINT()` stay as they are:
`bm25_fsm.c` (`reclaim_one_range`'s loop and `bm25_reclaim_retired`'s walk), `bm25_merge.c`
(the per-document and per-term loops) and `bm25_seg_build.c` (the chain rotation). The issue
asked that throttling land with or after bounding the holds and never before, because a nap
inside a hold that blocks inserts lengthens that hold. The holds are bounded under one
singleton (ADR 0118) and not removed, so every one of those sites is still inside an
insert-blocking hold, and none was converted. This record's choice of the plain form at the
reclaim loops stands. A different lock design that took merges and reclaims off the insert
path would reopen it.

## Addendum (2026-10-05, PRs #331-#350)

- **Retired-list walks are bounded** (#302.E, PR #342). Every retired-list walk (the orphan
  sweep's, `bm25_reclaim_retired`'s, the debug walkers') gets a visit cap at `nblocks`. The
  cap alone is not enough for `bm25_reclaim_retired`: it restarts its walk per chunk (ADR
  0118), so a list that links back to its head unlinked and freed the head once per chunk
  without end, under a fresh cap each time. It therefore also refuses a walk that comes back
  to the head, and the shared retired-page validator refuses a DELETED page on the list (a
  link into a freed descriptor freed it again). `bm25_debug_retired_pages` gains the
  page-kind check its siblings already ran.
- **The KEYMAP writer is cancellable** (#305 XCUT-05, PR #339). `bm25_keymap_write` held a
  buffer content lock at every instant of the chain, so interrupts stayed held from its first
  page to its last. Each page boundary now finishes the tail's record, unlocks the tail
  (keeping its pin), runs a work unit and `CHECK_FOR_INTERRUPTS`, allocates, and re-locks
  the pinned old tail for the link record: the shape this record gave `chain_ensure`.
  Nothing else writes the unlocked tail, because the chain is an orphan until the segment
  publishes, and a page allocated just before a throw is an orphan the reclaim path already
  handles. Pause point 16, `keymap_rotation`, marks the gap, and `sql/148` injects a cancel
  there with `debug_cancel_at`.
- **Two dead checks removed** (#313 XCUT-09, PR #345): one in `bm25_seg_read.c` and one in
  `bm25_seg_debug.c`, both under a held LWLock where `ProcessInterrupts` cannot act. The
  per-file floors were lowered in the same PR (ADR 0024's addendum).
