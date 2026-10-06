---
id: 0042
title: WAL-log the unlogged init fork, and stop holding buffer locks across allocation
date: 2026-08-05
status: Accepted
summary: An unlogged bm25 index could not survive a crash because every INIT_FORKNUM write went through GenericXLog, which emits no WAL for a non-permanent relation, and the smgrimmedsync that was believed to cover it only flushed the zeros page extension had written; the init fork now uses core's log_newpage_buffer discipline, VACUUM stops corrupting the segment-only ndocs counter, the segment-catalog walk is loud and locked, four pending-page walkers copy the page instead of allocating under its lock, and the merge feed no longer nests a postings replay inside a held DICT lock.
---

# 0042. WAL-log the unlogged init fork, and stop holding buffer locks across allocation

## Context

Seven defects in locking, crash safety and page accounting, filed as parts of
issues #59, #65 and #67. They are one record because five of them share a root
shape — work done while holding something that should have been released first
— and because the two that do not (init-fork durability, FSM accounting) were
both cases where a fix's *mechanism* silently failed to do what its comment
claimed.

The headline is the init fork. `bm25_buildempty` wrote a bare metapage and no
analyzer fingerprint, so after a crash reset an unlogged index failed
`bm25_fingerprint_gate` on every query instead of reading as empty. The obvious
fix — stamp the fingerprint in the init fork, matching what a real build
produces — was written, verified byte-for-byte against `MAIN_FORKNUM` with
`pageinspect`, and was still wrong, because the mechanism underneath it does
nothing for an unlogged relation:

- `GenericXLogStart` sets `isLogged` from `RelationNeedsWAL`, which requires
  `RelationIsPermanent` (`rel.h:639`). For an unlogged index that is false, so
  `GenericXLogFinish` applies the page image and marks the buffer dirty and
  **emits no WAL record at all**.
- The `smgrimmedsync` believed to cover that flushes the *file* at the md
  layer. The content was modified in shared buffers and never `smgrwrite`n, so
  it flushes the zeros `smgrzeroextend` wrote at `P_NEW` time. The real
  content reaches disk only at the next checkpoint.

So `CREATE UNLOGGED TABLE` + `CREATE INDEX USING bm25` + crash before the next
checkpoint left a zero-filled init fork; `ResetUnloggedRelations` copied those
zeros over the main fork, and every query then failed the magic gate in
`bm25_meta_validate`. On a standby the same hole surfaces as "could not read
block 0" after promotion, because the standby receives core's `log_smgrcreate`
(an empty file) and never the page contents.

This was **pre-existing** in `bm25_meta_init`'s `INIT_FORKNUM` path, not
introduced by the fix. It went unnoticed because a checkpoint between
`CREATE INDEX` and the crash makes it work, and because — see Consequences —
no TAP suite exercises an unlogged index at all.

## Decision

**Write init-fork pages with core's buildempty discipline, not GenericXLog.**
Each page: `START_CRIT_SECTION()`, fill, `MarkBufferDirty`,
`log_newpage_buffer(buf, true)`, `END_CRIT_SECTION()` — what `ginbuildempty`
and `brinbuildempty` do. `log_newpage_buffer` emits an unconditional
`XLOG_FPI` record independent of `RelationNeedsWAL`, which is exactly the
property an unlogged init fork needs. `smgrimmedsync` is dropped: FPI replay
is what provides durability, and a synchronous flush at creation time only
helps if no crash intervenes first — which was the bug.

`bm25_meta_init` is split into `bm25_meta_extend` / `bm25_meta_fill` /
`bm25_meta_finish`, with `bm25_meta_finish` branching on forknum so the
`MAIN_FORKNUM` path keeps GenericXLog unchanged. `bm25_buildempty` reserves
block 0, writes the field-config page to block 1, then finishes the metapage
once with the known `field_config_blkno` — one write per page rather than
init-then-restamp.

The other six:

- **`bm25_pending_mark_dead` no longer decrements `meta->ndocs`.** That counter
  is segment-only and incremented solely at seal; tombstoning a *pending*
  document was deflating the sealed-corpus base that avgdl and idf are computed
  from, silently skewing every score. Only `pending_ndocs` is touched now.
- **`bm25_segcat_read` is loud and its unprotected caller is locked.** A short
  chain set `*nsegs = n` and returned silently; it now errors. `bm25_bulkdelete`
  was the one production caller taking no metapage singleton; it does now.
- **Four pending-page walkers copy the page instead of allocating under its
  lock.** `pending_phrase_stash`, `pending_and_stash`, `pending_score_term` and
  the inline block in `bm25_load_if_needed` held a SHARE lock across `palloc`,
  `repalloc` and dynahash inserts — unbounded work (an allocation can fault, a
  hash can grow) on a page that pending appends and seals contend for. They now
  take one bounded `BLCKSZ` copy under the lock, release, and run unchanged
  logic against the copy.
- **The merge feed drops its DICT lock around the postings replay.**
  `bm25_seg_scan_postings` holds POST and POS locks across its callback, and
  that ran nested inside a held DICT-page lock. `bm25_seg_dict_iter_unlock` /
  `_relock` open a gap; the term bytes are copied out first, because the
  iterator yields `term` pointing into the locked page.
- **`bm25_upgrade` derives `feature_flags` before taking the metapage lock,**
  not inside an open GenericXLog window where it would acquire a second buffer.
- **`bm25_page_alloc` returns rejected FSM candidates instead of consuming
  them**, via a deferred requeue (see Alternatives).

## Alternatives considered

- **Exempt empty indexes in `bm25_fingerprint_gate`** instead of stamping the
  init fork — rejected. The gate runs on every query on every index; widening
  it to tolerate an unstamped index is a far larger blast radius than making
  one build path produce correct on-disk state.
- **Keep `smgrimmedsync` alongside `log_newpage_buffer`** — rejected as
  cargo-culting. FPI replay is the durability mechanism; the sync was only ever
  there because the missing WAL record was not noticed.
- **`RecordFreeIndexPage` immediately on rejection** — this was the *first*
  version of the FSM fix, and it traded a leak for a livelock. The requeued
  block is handed straight back by the next `GetFreeIndexPage`; pre-fix, every
  rejection strictly shrank the FSM, and that is what guaranteed termination.
  Concretely: a seal's `ChainWriter` holds a page EXCLUSIVE across an entire
  tail fill (ADR 0041), so a concurrent pending-append pops a double-listed
  block, fails `ConditionalLockBuffer`, requeues, re-pops — busy-spinning for
  the seal's whole page hold, where before it fell through to extension. With
  `heaprel == NULL` (`bm25_debug_alloc_unknown_page`) and any valid-`retire_xid`
  DELETED page present, it never terminates at all. Rejected blocks are now
  collected and requeued once, after the loop exits.
- **Fully restructuring the nested DICT/POST/POS locking** — deferred. It spans
  three files and changes the callback contract. The minimal version (drop the
  DICT lock around the replay) removes the nesting without touching semantics.

## Consequences

**An unlogged bm25 index now survives a crash.** Verified with
`pg_walinspect`: a bm25-only `CREATE INDEX` on an unlogged table now emits two
`XLOG/FPI` records, one per init-fork page. Pre-fix it emitted **zero**.

**There is no test coverage for this, and that is the largest known gap.**
`grep -il unlogged t/*.pl` returns nothing across all 17 TAP suites;
`002_crash.pl` and `003_replica.pl` both use logged `CREATE TABLE`, so
`bm25_buildempty` is never invoked. The crash suites are not masking the bug
with a checkpoint — they simply never reach the code. TAP cannot run locally
here (no `--enable-tap-tests`), so a suite written now would be unverified
until CI; it is recorded as follow-up rather than written blind. Until it
exists, the durability property rests on the `pg_walinspect` observation above
and on code review, not on a regression gate.

**The `bm25_livedocs_clear` TOCTOU is documented, not fixed.** A lazy or auto
VACUUM backend is `PROC_IN_VACUUM` and holds no snapshot, so it does not hold
back the horizon; a concurrent `bm25_merge_sql` (RowExclusiveLock, compatible
with VACUUM's ShareUpdateExclusiveLock) can retire a segment mid-tombstone-loop.
The gen-validated header read makes that a loud error, but `bm25_livedocs_clear`
re-reads with `expected_gen = 0` and no validation. Pre-existing; the narrow
singleton window and the gen-validated read are what safety currently rests on,
and the comment in `bm25_bulkdelete` now says so instead of claiming — falsely —
that horizon gating covers it.

**Three debug SRFs can now surface `ERRCODE_INDEX_CORRUPTED` on a healthy
index.** `bm25_debug_seg_keymap`, `bm25_debug_seg_doc_live` and
`bm25_debug_segterms` call `bm25_segcat_read` without the singleton, so a
concurrent merge swap can trip the new loud check. This replaces a silent
undercount, so it is still an improvement; the race is noted in their headers.

**One more live interrupt check.** Dropping the DICT lock created a genuine
lock-free instant per term, so `bm25_accum_from_segments` now carries a check
there — making large merges cancellable per-term rather than per-DICT-page.
The CI aggregate floor moves 61 → 62 and `bm25_merge.c`'s per-file floor 2 → 3.
This is the inverse of ADR 0041's lesson: there, removing a lock-free instant
made checks dead; here, creating one made a new check live.

## Addendum (2026-08-23)

Issue #145 (ADR 0083) completed the "nothing that can allocate or hash runs
under a buffer content lock" sweep this record started, and in doing so had to
GENERALIZE the safety argument for the `bm25_seg_dict_iter_unlock`/`_relock`
pair introduced above.

The pair was justified here by the merge feed's circumstance: it holds the
metapage singleton for the whole accumulation, which blocks the one thing that
could free and reuse the segment's pages. The wildcard expander
(`bm25_dict_expand_wildcard`) is now a second caller and holds no singleton, so
the contract is restated as the property it actually needs: **the caller must
hold something that prevents this segment's pages from being freed and reused
for the duration of the iteration.** The singleton is one sufficient form; an
ordinary MVCC snapshot that already includes the segment is another, because
reclaim can only return a retired segment's pages through `bm25_page_alloc`'s
horizon gate (`GlobalVisCheckRemovableFullXid`) and such a snapshot holds that
horizon back. Immutability of a live segment's pages is unconditional either
way — sealed pages are never rewritten in place, and retire resets no links —
so the pin plus either form of the precondition is what makes the content
unchanged across the gap. A caller that can satisfy neither must not use the
pair; nothing mechanical enforces that.

Two further corrections in the same pass, both about prose rather than code.

First, `bm25_page_alloc` was described as one that "never blocks". What it
actually guarantees is that it never `table_open`s and never waits on another
buffer's CONTENT lock (FSM candidates go through `ConditionalLockBuffer`;
eviction goes through `GetVictimBuffer`'s `LWLockConditionalAcquire`) — that, and
not the absence of all waiting, is what makes nesting it under the metapage
LWLock deadlock-free. It takes no relation-extension lock either:
`ReadBuffer_common` passes `EB_SKIP_EXTENSION_LOCK` for a `P_NEW` blockNum. So
`ReadBufferExtended`'s "caller is responsible for ensuring that only one backend
tries to extend a relation at the same time" obligation is ours, and the metapage
lock pair is what discharges it — the singleton `ExclusiveLock` for every
maintenance writer, `ShareLock` plus the metapage buffer content lock for the
appender. (An intermediate draft of ADR 0083 asserted the opposite, that the
extension lock *is* taken and merely released early; that is false and was caught
in review. Both wrong versions are preserved in the source comment.)

Second, the free path's rule "never nest the page lock with the FSM update" was
being read as a blanket ban; it governs the FREE side only, and the allocate side
deliberately runs under the metapage lock at three call sites. ADR 0083 records
why the alternative there was rejected.

## Addendum (2026-10-04)

Two statements here are stale after issue #270 ([0107](0107-catalog-walkers-require-the-metapage-singleton.md)).
The summary calls the segment-catalog walk "loud and locked", and the Decision's
bullet says `bm25_bulkdelete` was the one production caller of `bm25_segcat_read` that
took no metapage singleton. It was not the only one: `bm25_validate_key_config_for_insert`
(ADR 0067) called the same walk once per INSERT row, holding no singleton. The walk
releases the metapage after reading `segcat_root`, and a catalog chain orphaned by a
merge gets no `retire_xid`, so the next VACUUM's orphan sweep frees it for immediate
reuse. An INSERT stalled across a merge flip, that sweep and an allocation read a page
of another kind and failed with XX002.

0107 records the fix and the rule it enforces. Production callers of the unlocked
catalog walkers must hold the singleton, ShareLock or stronger, asserted under cassert
by `bm25_segcat_read_locked` and `bm25_livedocs_clear`; the INSERT check reads entry 0
under the metapage SHARE through `bm25_segcat_first_entry` and no longer walks the
catalog. The debug SRFs keep the unlocked `bm25_segcat_read`, so the Consequences
paragraph about three debug SRFs surfacing `ERRCODE_INDEX_CORRUPTED` on a healthy index
still holds.

## Addendum (2026-10-05)

`bm25_buildempty` broke this record's own rule about buffer locks: `bm25_meta_extend` returns
the init-fork metapage EXCLUSIVE-locked, and the function then opened the heap, resolved the
analyzer configuration and fields and computed the analyzer fingerprint under it. Since #296
the fingerprint includes a dictionary probe of a few hundred `lexize` calls, so the work was
also uncancellable for its duration (lwlock-held sections hold off interrupts). The relation
was created in the current transaction, so no other backend could reach the block and there
was no lock-order deadlock; the cost was the uncancellable window and a violated house rule.
The reservation now follows all the catalog work and the probe, which touch neither fork, and
only the field-config page has to come after it. The order of pages in the fork is unchanged.
