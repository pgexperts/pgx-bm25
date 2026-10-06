---
id: 0083
title: A buffer content lock covers page bytes only, and a Generic WAL window stays throw-free
date: 2026-08-23
status: Accepted
summary: Every reader that decodes a page now copies it under the SHARE lock and releases before decoding, so nothing that allocates, hashes, or nests a page read runs under a buffer content lock; the iterator unlock/relock pair is confined to the merge feed because its precondition is unsound for a reader on a hot standby; the keymap writer's allocation moves out of its Generic WAL window; and three comments that overclaimed what the allocator and the FSM rule guarantee are corrected.
---

# 0083. A buffer content lock covers page bytes only, and a Generic WAL window stays throw-free

## Context

Issue #145 collected five findings that share one root cause: work whose
duration and failure modes are unbounded — `palloc`, a dynahash `HASH_ENTER`
that can rehash, an `ereport`, an entire nested chain replay, a relation
extension — was being done while a buffer content lock was held, or while a
Generic WAL window was open. Both are windows that must stay short and
predictable, and both were being used as if they were ordinary scopes.

The project had already reached the right answer twice and written the
machinery for it. `pending_phrase_stash` and `pending_and_stash`
(`bm25_scan.c`) take a single bounded `BLCKSZ` copy under the SHARE lock and
release it before decoding. `bm25_seg_dict_iter_unlock`/`_relock`
(`bm25_seg_read.c`) were added so the merge feed could drop its DICT page lock
around each term's postings replay. `bm25_fsm.c` states the WAL rule outright:
no `ReadBuffer`/`LockBuffer`/`palloc`/`ereport` between `GenericXLogStart` and
`GenericXLogFinish`. What was missing was not a design; it was consistency —
the remaining sites had simply not been converted, and two of the five findings
were comments asserting properties the code did not have.

Two of those windows were also uncancellable, which is the part a count-based
CI gate cannot see. `LWLockAcquire` calls `HOLD_INTERRUPTS()`, so every
`CHECK_FOR_INTERRUPTS` inside a held-content-lock window is a silent no-op —
the same trap ADR 0041 recorded for the maintenance pass and #139 recorded for
the POS cursor, reappearing at the wildcard expander's per-term loop and at
both `bm25_segment.c` debug SRFs.

## Decision

We will hold a buffer content lock for reading or writing page bytes and
nothing else, and we will keep every Generic WAL window free of calls that can
throw — applying the patterns already in the tree rather than inventing new
ones.

Concretely:

- **Copy-then-unlock everywhere a reader decodes a page**: the wildcard
  expander's *both* passes (segment and pending) and all three DICT-walking debug
  SRFs (`bm25_debug_terms`, `bm25_debug_postings`, `bm25_debug_segterms`). Each
  takes one `PGAlignedBlock` copy under the SHARE lock, releases, and decodes the
  copy.
- **Unlock/relock stays confined to the merge feed**, whose metapage-singleton
  precondition is genuine and primary-only, and its header now says so in those
  terms.
- **Allocate outside the window**: `bm25_keymap_write`'s continuation path
  finishes the tail page's record before calling `bm25_page_alloc`, then writes
  the link and the new page's init in one small follow-up record.
- **State the properties the code actually has**: `bm25_page_alloc` never
  `table_open`s and never waits on another buffer's content lock (FSM candidates
  via `ConditionalLockBuffer`, eviction via `GetVictimBuffer`'s
  `LWLockConditionalAcquire`), which is what makes nesting it under the metapage
  lock deadlock-free. It also takes **no relation-extension lock at all** —
  `ReadBuffer_common` passes `EB_SKIP_EXTENSION_LOCK` for `P_NEW` — so
  `ReadBufferExtended`'s "caller is responsible for ensuring that only one
  backend tries to extend a relation at the same time" obligation falls on this
  AM, and what discharges it is the metapage lock pair (singleton `ExclusiveLock`
  for every maintenance writer; `ShareLock` + the metapage buffer content lock
  for the appender, with `ShareLock`/`ExclusiveLock` conflict keeping the two
  families apart). And "never nest the page lock with the FSM update" governs the
  FREE side; it does not forbid the allocate side.

The unlock/relock pair's precondition is now stated in its header as **the
metapage singleton and nothing weaker**, with an explicit warning against the
substitute a draft of this change tried. That draft added the wildcard expander
as a second caller on the reasoning that an ordinary MVCC snapshot including the
segment holds back `GlobalVisCheckRemovableFullXid`, which is what gates
`bm25_page_alloc`'s reuse of a retired page. On the primary that is true and
survives attack. On a **hot standby without `hot_standby_feedback`** it is worth
nothing: that horizon is evaluated on the primary and a standby reader's snapshot
restrains it not at all. Generic WAL redo takes an ordinary exclusive content lock
and carries no recovery-conflict information, so the startup process can replay
retire → stamp → reuse over the pinned page inside the unlocked window, and
`_relock` resumes `it->cur`/`it->end` against rewritten bytes with **no
revalidation of any kind** — the iterator carries `expected_gen = 0`, so not even
the page-kind check it runs at page crossings applies mid-page. Memory-safe,
silently wrong. Copying the page dissolves that: the cursor addresses caller
memory, so there is no window in which anything can change under it.

Converting the segment pass to a copy also lets it carry two backstops the
iterator structurally cannot — the segment's **real** `seg_gen` (the iterator
passes 0 everywhere) and the `blk < nblocks` extent bound. The gen check turns
the standby-substitution case into `ERRCODE_T_R_SERIALIZATION_FAILURE`, which is
exactly what `bm25_scan_build_ranking`'s bounded subtransaction retry exists for,
and this function is reached only from `bm25_scan_build_ranking_exhaustive`
underneath it. So the expander went from *no* page validation to the same
validation every other segment read gets.

## Alternatives considered

- **Convert the debug SRFs onto `bm25_seg_dict_iter_*`, as the issue's proposed
  fix and a standing TODO in `bm25_seg_read.c` both suggest.** Rejected on
  inspection: that iterator carries `expected_gen = 0` by design and has no
  `blk < RelationGetNumberOfBlocks` extent bound, while both SRFs validate with
  the segment's real gen and carry the extent bound. Routing them through it
  would have fixed the lock-hold defect by DROPPING two corruption backstops.
  Copy-then-unlock keeps both and yields a stronger result anyway (no lock held
  at all during the entry loop, rather than a lock dropped per term).

- **Pre-allocate the pending append's pages before taking the metapage lock
  (PEND-19's option (b), the "real" fix).** Rejected for three independent
  reasons, any one of which is sufficient. (1) The page count is not knowable
  outside the lock — the part count falls out of `ndistinct`/`e_posbytes`, but
  how many parts need a NEW page depends on `meta.pending_tail_free`, which is
  metapage state. (2) A surplus pre-allocation cannot be returned cheaply:
  `bm25_page_alloc` accepts only pages stamped `BM25_PAGE_DELETED` and
  permanently drops unmarked ones from the FSM, so handing one back costs a
  Generic WAL record and FSM churn on every insert — including the common
  single-part insert that fits the existing tail. (3) Releasing the metapage
  between parts, the other half of that proposal, is a correctness regression:
  a document's parts must be consecutive records in chain order, the held
  metapage lock is the only thing providing that, and the drain reassembles a
  document by accumulating consecutive same-tid parts. We took option (a) —
  narrow the rule to what it actually governs and document the nesting as
  deliberate at the call site — with the reasoning recorded rather than
  asserted.

- **Leave the two comment-only findings alone, since no live bug follows.**
  Rejected. Both comments are load-bearing: `bm25_seg_build.c` cites the
  allocator's claimed properties verbatim to justify its own nesting, and the
  FSM rule's blanket phrasing was already being read as prohibiting something
  the tree does on purpose. An overclaiming comment is a hazard for the next
  change that leans on it, which is precisely how it was being used.

- **Adopt the issue's own refutation of XCUT-09 as written.** Rejected after
  checking it. The issue argued that `ReadBufferExtended(…, P_NEW, …)` *does*
  take the heavyweight relation-extension lock but that the wait is harmless
  because the lock is released inside `ExtendBufferedRel`. The first half is
  false: `ReadBuffer_common` passes `EB_SKIP_EXTENSION_LOCK` for `P_NEW`, so the
  lock is never taken and the code path the argument describes is never reached.
  A first draft of this change shipped that reasoning into three source comments,
  ARCHITECTURE.md and this record before an adversarial review caught it. The
  corrected statement is recorded in `bm25_pending_append_multi`'s header
  alongside the original "never blocks" wording, so a future reader can see both
  wrong versions and why each is wrong.

- **Add a latency assertion for the wildcard expander too.** Rejected as
  untestable at the available granularity: the pre-fix uncancellable window
  there is one DICT page's worth of glob matches, a few hundred microseconds,
  so no boundary separates pre- from post-fix without a contrived pathological
  pattern. The wildcard change is covered by a behaviour-preservation fixture
  instead (multi-page dict, multi-page pending), and that limitation is stated
  in the suite.

- **Keep the wildcard expander on `bm25_seg_dict_iter_unlock`/`_relock` and
  document the standby caveat.** Rejected. It is the option that leaves a known
  silent-wrong-answer path in the tree and writes the unsound precondition into a
  contract other callers would then copy. A page-granularity version of the same
  standby exposure predates this issue and is acknowledged in ADR 0063, which
  argues it degrades to a retryable serialization failure because the next page's
  own gen check catches substitution — but that argument does not transfer to an
  iterator carrying `expected_gen = 0`, and the unlock/relock form widens the
  window from page-crossing to mid-page while removing even the kind check. This
  repo ships standby TAP suites, so it is not hypothetical.

- **Revalidate after `_relock`** (`bm25_seg_page_validate_kind` + recompute
  `end`). Considered as the fallback if copying proved impossible. It is strictly
  worse than copying here: it costs two LWLock operations *per entry* against one
  `memcpy` per ~254 entries, it still cannot check the gen (the iterator does not
  carry one), and it leaves the reader re-deriving a cursor into a page that is
  free to change again on the next entry.

## Consequences

- The invariant "nothing that can allocate, hash, or take a second buffer runs
  under a buffer content lock" is now true across the tree rather than at four
  sites, so a reviewer can apply it as a rule instead of checking each caller.
- Three previously-dead `CHECK_FOR_INTERRUPTS` became live with no change in
  the interrupt COUNT — the third instance of the pattern ADR 0041 and #139
  record. The CI grep floor is again a delete-detector only;
  `sql/80_maintenance_interrupts` pins the behaviour, with an A/B-verified
  boundary (pre-fix `bm25_debug_postings` cancels at ~1.16 s, post-fix at
  ~11 ms, boundary 300 ms).
- `bm25_keymap_write` now emits one extra WAL record per continuation page and
  registers two buffers in that record instead of one. Both are well inside the
  4-buffer cap, and this is the same "correctness over a single combined record"
  trade `bm25_pending_append_multi` already makes. A crash between the two
  records leaves a full-but-unlinked tail plus an orphan successor — the state
  an aborted seal already produces, which the reclaim path already collects.
- The unlock/relock pair is back to ONE caller (the merge feed) with a
  precondition that is stated, checkable, and primary-only. The residual hazard
  is that nothing mechanical enforces it: a future caller that reaches for the
  pair without the singleton would be silently wrong on a standby, exactly as the
  rejected draft was. The header names the failure mode and points at the
  copy-then-unlock pattern as the thing to reach for first.
- PEND-19 stays a documented deliberate nesting rather than a fix. The metapage
  is the busiest lock in the index and every concurrent inserter still
  serializes behind `bm25_page_alloc` there. If that shows up in an insert
  profile, the exit is not pre-allocation but changing the publish protocol so
  a document's parts need not be consecutive — a much larger change, recorded
  here so the next attempt starts from the right constraint.
- Two things remain untestable from SQL and are called out as such rather than
  papered over with a test that passes either way: PEND-10's actual failure
  mode needs an allocation to throw mid-write (an injected OOM), and XCUT-09 is
  a comment correction with no behavioural surface at all.
- Working through XCUT-09 properly surfaced something the issue did not: because
  `P_NEW` skips the extension lock, the metapage lock pair is the *only* thing
  keeping relation extends single-threaded in this AM. That is now written down
  where a future change would trip over it. Two callers hold neither half of the
  pair, and both were checked rather than assumed. `bm25_build` (`ambuild`) is
  safe even under `CREATE INDEX CONCURRENTLY`: `index_concurrently_build` calls
  `index_build` and only *afterwards* calls
  `index_set_state_flags(INDEX_CREATE_SET_READY)`, with `DefineIndex` committing
  after that to publish it — so `aminsert` cannot run against the index while
  `ambuild` does, and phase-3 `validate_index` inserts go through `aminsert` and
  therefore through the appender's locks. The test-only
  `bm25_debug_alloc_unknown_page` is `heaprel == NULL`/extend-only and REVOKEd
  from PUBLIC.

## Addendum (2026-09-21)

The summary's "every reader that decodes a page now copies it" had one exception:
`bm25_pending_drain`, the seal's reader of the pending list, still ran
`drain_doc_flush` (accumulator inserts -- dynahash growth) and
`drain_doc_add_part` (palloc/repalloc of a document's parts) with each pending
page's SHARE lock held. The #67 triage (2026-09-21) found it; it now takes the same
`PGAlignedBlock` copy under the lock and decodes the copy, so the rule holds
without an exception.

It was never a contention bug, which is worth recording because it is why nothing
noticed: the seal holds the metapage singleton in ExclusiveLock across the whole
drain, and both production writers of pending pages -- `bm25_pending_append_multi`
and VACUUM's pending sweep (ADR 0066) -- take it in ShareLock, so no production
path could have waited on that content lock. (The owner-only test lever
`bm25_debug_pending_invalidate_page` takes no singleton; the SHARE lock at copy
time serialises it exactly as the held lock used to.) What holding it did cost was interrupt holdoff
across each page's accumulator work, and a second, remote justification ("safe
because of the singleton") for a rule the rest of the tree keeps unconditionally.
The conversion changes nothing observable: `DrainDoc` already copied every byte it
keeps out of the page. Negative control: decoding the first page's image for every
later page turns the seal-dependent suites red.

## Addendum (2026-10-04)

The fresh-eyes review of this date found several sites that raise or may raise inside an open Generic WAL window, contrary to this record: bm25_upgrade's identity-path validation, the segment builder's palloc/ereport calls, and a test-only stamping lever. Tracked in #312 and #313; the builder case needs either a code change or a superseding statement of the rule.

## Addendum (2026-10-05)

Two applications of this record's rules (#291, #300).

- The statement that the MVCC-horizon argument is void on a hot standby without feedback is
  carried to the pending chain by ADR 0110, which stamps pending pages with an epoch.
- A Generic WAL window stays throw-free: the pending append's `GenericXLogStart` is hoisted above
  `bm25_page_alloc`, so nothing between the allocation and `GenericXLogFinish` can throw
  (ADR 0116). `bm25_crash_epoch()` can raise inside `GetNamedDSMSegment`, so it is called first
  in `bm25_reclaim_orphans`, before any buffer lock or WAL window, and its failure is caught in
  an internal subtransaction.

## Addendum (2026-10-05, PRs #331-#350)

- **"Throw-free" is scoped to windows over published structure** (D27, #312 SEGBUILD-06 and
  #313; decided as this addendum, no builder change). The segment builder's orphan windows
  are not throw-free: the DICT tail window stays open across terms while `palloc` runs, and
  `chain_ensure` raises `PROGRAM_LIMIT_EXCEEDED` for an oversized record. Every page they
  write is an unpublished orphan until the caller's publish record, and the builder runs in a
  transaction that aborts on error with no `PG_CATCH` resuming, so a throw discards private
  state only and resource-owner cleanup releases the buffer and the `GenericXLogState`. The
  rule stands for every window over published structure (the publish and swap records, the
  pending append, the catalog appender). The `bm25_seg_build.c` comment says so (PR #349).
  Rejected: `palloc` and capacity-check before each window, a hot-loop change for no
  runtime gain.
- **The KEYMAP writer's windows are unchanged**; the content lock is now dropped between
  pages so the chain is cancellable (ADR 0041's addendum, PR #339).
- **Validation moved ahead of two windows** (#313 META-10, SURFACE-12, PR #345):
  `bm25_upgrade`'s identity path reads and validates the metapage before `GenericXLogStart`,
  and `bm25_debug_stamp_seg_field_count` validates the page before opening its window.
  `bm25_next_gen_check` and the page lever's checks (ADR 0126) also run before their windows.
  `bm25_livedocs_clear` checks the LIVE page's gen before its window (ADR 0120).
