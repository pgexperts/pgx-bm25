---
id: 0074
title: Every on-page struct is memset before fill and memcpy'd on every hop to the page, and the two holed descriptors have their sizes pinned
date: 2026-08-20
status: Accepted
summary: A full sweep of all 34 GenericXLogRegisterBuffer sites found exactly one struct genuinely leaking uninitialized stack padding into WAL (BM25SegCatEntry on the merge path, where it also propagates forward through every later merge); the fix is memset-before-fill PLUS memcpy on every copy between there and the page, since C does not require a struct assignment to copy padding, and StaticAssertDecls pin both sizes because neither has a named pad member.
---

# 0074. Every on-page struct is memset before fill and memcpy'd on every hop to the page, and the two holed descriptors have their sizes pinned

## Context

Six of this codebase's on-disk structs already `memset` before a field-by-field fill,
four of them carrying an explicit "WAL determinism" comment. Two did not, and a
review asked whether that mattered.

A full sweep — all 16 on-disk structs, all 34 `GenericXLogRegisterBuffer` call sites,
with the padding layouts computed rather than assumed — found **exactly one genuine
leak**:

**`BM25SegCatEntry` on the merge/swap path.** `bm25_segment_build_and_commit_swap`
declares `newentry` as a bare stack automatic, assigns six fields, and copies it into
a `palloc` (not `palloc0`) `survivors[]` array, which
`bm25_segcat_build_orphan_chain` then `memcpy`s onto a page registered
`GENERIC_XLOG_FULL_IMAGE`. The struct has a 4-byte hole at offset 4 — `BlockNumber
header_blkno` at 0, then a 8-aligned `uint64 ndocs` — that no assignment writes. The
`bm25_page_init` two lines above the `memcpy` does zero the page, and the `memcpy`
then undoes that for exactly those four bytes, below `pd_lower`, inside the WAL image.

It also **persists and propagates**. Every catalog reader copies entries at full
`sizeof`, so the residue is carried into `catarr[]`, re-copied into the next merge's
`survivors[]`, and re-emitted onto that merge's fresh chain. One merge's stack
residue lives in that entry for the life of the segment.

The seal path's sibling writer builds its entry **in place** on a `PageInit`'d page
and is unaffected — which is precisely the distinction: a `memcpy` of a stack struct
carries padding that an in-place field fill never writes.

**`BM25RetiredEntry` was reported as the second leak and is not one.** It has a real
4-byte hole at offset 36, and `bm25_retire_segment` genuinely never `memset`s — but
its only caller hands it a page that `bm25_page_init` zeroed on the *registered copy*
inside the same Generic WAL window, and the fill is in place. A compiled probe
confirmed the compaction path is likewise safe today: whole-struct assignment does
copy padding at `-O0` and `-O2` on this toolchain, so `keep[]`'s uninitialized slots
carry the page's zeros rather than stack residue.

## Decision

Apply the `memset`-before-fill convention to both, and state which one was leaking.

- `BM25SegCatEntry`: `memset(&newentry, 0, sizeof(newentry))` before the field fill —
  **and `memcpy` for every copy between there and the page.** The `memset` alone is not
  enough and it took adversarial review to see why: the zeroed struct reaches the page
  through `survivors[nsurv++] = newentry`, and C does not require a struct assignment to
  copy padding. A compiler that scalarizes that copy (SRA) drops the zeros, and
  `survivors` is `palloc`, not `palloc0`, so the hole arrives at the page uninitialized
  exactly as before. Both assignments into `survivors[]` — the new entry and the copied
  survivors — are now `memcpy`.
- `BM25RetiredEntry`: `memset(&arr[n], 0, sizeof(arr[n]))` before the in-place fill,
  and BOTH halves of the compaction round trip switched to `memcpy` — the copy-back
  `arr[w] = keep[w]` and the gather `keep[nkeep++] = arr[k]` that feeds it. This is
  **hardening, not a leak fix**, and the code says so. The objection to the status quo
  is the shape of the reliance, not a present disclosure: correctness-by-remote-invariant,
  where the invariant lives in another file and nothing connects the two — plus a
  struct assignment whose padding behaviour C leaves unspecified.

Two `StaticAssertDecl`s pin `sizeof(BM25SegCatEntry) == 40` and
`sizeof(BM25RetiredEntry) == 48`. Neither has a named pad member, both are strided by
`MAXALIGN(sizeof(...))` at read and write, and `BM25_RETIRED_PER_PAGE` is *derived*
from the retired size — so a member reorder would silently repartition an existing
page rather than fail to compile.

Three in-memory siblings from the same review are fixed by declaration-site
zero-initialization: the two pending walkers' per-document scratch (reset only inside
the head-record branch, so a stranded continuation — which ADR 0069 records an
ordinary cancelled VACUUM produces — accumulates into uninitialized stack), and
`bm25_scan_load_fieldcfg`'s `fcfg[]`. A zero-posting term's
`term_post_root`/`term_post_off` are pre-set to `InvalidBlockNumber`/`0`, mirroring the
POS pass in the same function.

## Alternatives considered

- **Switch `survivors` to `palloc0`** — the obvious one-word fix, and it does not
  work on its own. The whole-struct assignment `survivors[nsurv++] = newentry`
  overwrites the zeroed destination padding with the stack object's padding. The
  `memset` has to be on the source.
- **`memset` the source and leave the struct assignments alone** — what this record
  originally decided, and it was self-inconsistent: the same commit replaced an
  identical assignment in `bm25_fsm.c` with `memcpy` on the grounds that C leaves
  assignment padding unspecified, while relying on that exact behaviour to carry the
  new `memset` in `bm25_seg_build.c`. Adversarial review caught the contradiction. The
  guarantee is only as strong as its weakest hop, so every hop is now byte-exact.
- **Give both structs named pad members**, as `BM25DictEntry` (`dict_pad`) and
  `BM25KeymapHeader` (`pad0`) do. That is the better design and is why those two have
  no implicit holes at all — but adding a field to a packed multi-record struct is an
  ADR 0009 format break for a problem a `memset` solves. Size assertions instead.
- **Leave `BM25RetiredEntry` alone since it is provably safe today** — rejected
  because the proof depends on a `PageInit` in a different file and on unspecified
  struct-assignment behaviour. Both are true today and neither is written down where
  someone editing `bm25_retire_segment` would see it.
- **Reconcile the two disagreeing field counts behind SCAN-06** rather than zeroing
  `fcfg[]` — that is the real defect there, and it is a different one. Zeroing makes
  the degenerate case deterministic instead of stack-dependent, which is this issue's
  remit; the disagreement itself is left standing and named in the code.

## Consequences

- The one genuine leak is closed, and with it the propagation into every subsequent
  merge's catalog.
- New suite `96_wal_page_determinism` asserts both holes are zero, via two probes that
  return an on-page descriptor's raw bytes. **The A/B did not discriminate**: with the
  merge-path `memset` removed and the extension rebuilt, the assertions still passed,
  because the stack slot happened to hold zeros on this platform and build. The suite
  is therefore a regression guard on a stated invariant, not a demonstration that the
  fix was necessary, and it says so in its own header. The authoritative detectors for
  this class remain MSan and `wal_consistency_checking`.
- `BUILD-07`, `SCAN-06` and `SCAN-08` have no behavioural assertion at all — one has
  no established reachability, two are invisible in output by construction. The suite
  lists all three explicitly rather than leaving a reader to assume they are covered.
- Cost: one 40-byte `memset` per merged segment, one 48-byte `memset` per retired
  segment, one ~4 KB `MemSet` per scan (not per row), and two zeroed stack arrays per
  pending walk. The assignment-to-`memcpy` changes are free — the same bytes move
  either way; only the padding guarantee differs.
- The rule generalises to a shape worth remembering: zeroing a struct is pointless if
  any copy between it and the page is a struct assignment. `memset` at the source,
  `memcpy` on every hop.

## Addendum (2026-08-23)

`bm25_segment_build_and_commit_swap` no longer exists; the memset-before-fill of
`BM25SegCatEntry` described above moved into `bm25_segcat_entry_from_hdr`, which is
now the single constructor for a catalog entry and is shared by the append (seal /
build) and swap (merge) publish paths. The reasoning is unchanged and, if anything,
strengthened: both paths now reach the page by byte-exact `memcpy` from a
caller-supplied struct, so zeroing at construction is the only thing keeping the
4-byte hole deterministic on disk. See `docs/adr/0084`.

## Addendum (2026-10-05, PRs #331-#350)

`wal_consistency_checking`, named above as one of the authoritative detectors this suite
could not replace, now runs in CI (#309 CI-01, PRs #348 and #350). The hardening job sets
it to `'all'` (a cheap superset of `'generic'`). Generic WAL's rmgr has a mask function,
so REDO compares every page it rebuilds with an image the primary logged, and a page dirtied
outside `GenericXLog` or a buffer missing from a record stops recovery with "inconsistent
page found". It runs only where WAL is replayed, so it is wired twice: `TEMP_CONFIG` puts it
in every TAP node (their crash restarts and standbys replay), and the pg_regress cluster
streams to a standby (`ci/wal_check_standby.sh`) that replays the whole run and must end
clean. Verified both ways on PG 18.6 with a metapage byte written after
`GenericXLogFinish`: every suite passes without the setting, and crash, standby and
regression-standby checks fail with it.

The regression standby needed rework after its first CI run (PR #350). The run writes about
21 GB of WAL with the check images, a cassert standby replays it far more slowly than the
primary writes, and on the 2-core runner it fell more than 4 GB behind and lost its slot.
Now:

- the standby is synchronous and the primary runs `synchronous_commit = remote_apply`, so
  lag is bounded by the WAL of the transactions in flight (the largest is `sql/80`'s
  4.8 GB INSERT) plus any WAL written with no waiting commit (VACUUM, CHECKPOINT, xid-less
  statements), which the next commit covers, not by runner speed; the run proceeds at replay speed;
- the script owns `max_slot_wal_keep_size`: 8 GB while the standby is healthy, 0 once the
  primary has been released, so a gone standby's slot is invalidated instead of pinning the
  run's WAL;
- a watchdog releases the primary (empties `synchronous_standby_names`) if the standby
  server is dead, has had no replication connection for 30 s, or has not advanced replay for
  5 minutes with WAL outstanding; a reconnected standby still catching up counts as healthy.
  A planted inconsistency therefore fails `verify` instead of hanging the job;
- the standby runs with `fsync=off` like the primary (the base backup does not copy the
  primary's command-line setting), and `verify` reports the peak slot retention, warns past
  three quarters of the cap, and prints disk headroom.

The same PR made t/029's log-scan exemption for its deliberate SIGKILL accept PG17's
wording ("server process (PID n)") as well as PG18's ("client backend (PID n)"), pinned to
the killed backend's PID.

Residuals: a slower runner or a suite that outgrows the cap fails the leg loudly rather than
silently; MSan is still not run.
