---
id: 0095
title: Every segment chain walk carries an extent bound, and a chain cursor records which chain it belongs to
date: 2026-09-22
status: Accepted
summary: The four remaining unbounded walkers in bm25_seg_read.c (bm25_seg_dict_lookup, bm25_seg_dict_iter_next, chain_read_at, bm25_seg_scan_postings) gain a `blk >= nblocks` bound that ERRORS rather than terminating, with nblocks captured once per walk or per cursor; BM25ChainCursor gains `root` so a cursor cannot resume inside a chain it does not belong to; and every per-posting caller that still read a chain without a cursor is given a BM25SegReader, because measurement showed the per-call capture those callers would otherwise pay lands squarely on the default ranked path — the cost ADR 0071 declined.
---

# 0095. Every segment chain walk carries an extent bound, and a chain cursor records which chain it belongs to

## Context

Two findings from the issue #154 read of `src/bm25_seg_read.c`, landed together
because both change `chain_read_at`'s loop head.

**SEGREAD-11.** A comment in `bm25_segment.c` justified that file's own
`blk < nblocks` backstop by saying the DICT chain walk there "had no bound of its
own, unlike the reader chains in bm25_seg_read.c." That was backwards. #139
(SEGREAD-07) bounded the SEGCAT walkers, and `pos_cursor_load`, the LIVEDOCS
locator and the wildcard DICT walk were bounded separately -- but the three
walkers most analogous to the comment's own subject were not:

- `bm25_seg_dict_lookup` -- the query path's per-term DICT walk;
- `bm25_seg_dict_iter_next` -- the merge feed's DICT iterator;
- `chain_read_at` -- the LIVEDOCS / DOCMAP / NORMS reader, called per scored posting;
- `bm25_seg_scan_postings` -- the POST walk every non-WAND path uses.

The fourth was named in the finding and then simply left out of the first landing,
which bounded the other three and called the set complete -- `bm25_livedocs_locate`,
which a reader may expect to find in that list, was already bounded by #139 and is not
what was counted. It is also the one walk of the four with a second defect rather than
only an attribution one.
Its loop is bounded by `emitted < df`, a *posting counter*, which constrains nothing
about where the chain points; and `post_root` arrives raw off a DICT page with nothing
upstream having examined it, because `bm25_dictentry_validate` bounds the entry's
header and MAXALIGN'd term span and looks at no pointer field. ADR 0071's per-pointer
gate covers the *other* POST walk (`wand_cursor_load_block`), so WAND was protected and
this was not. With `post_root == InvalidBlockNumber` the loop guard ends the walk before
its first iteration and **a term the dictionary says occurs in `df` documents matches
none of them, silently** -- on the exhaustive ranked path, on `@@@`, on the phrase pass,
and in the merge's replay, where the loss would be written into a new segment
permanently. Reproduced against a build with the guard removed: the probe reports
`emitted 0` for a term whose recorded `df` is 400.

All four already carried `CHECK_FOR_INTERRUPTS`, so this was never a hang. What
it was is an attribution failure. Measured on the pre-fix build, an out-of-extent
chain pointer surfaced as one of three errors, none of them naming an index
structure and none of them `ERRCODE_INDEX_CORRUPTED`:

| value | SQLSTATE | message |
|---|---|---|
| far past EOF, `bm25_seg_dict_lookup` | `58P01` undefined_file | `could not open file "base/.../N.1" (target block N): previous segment is only 12 blocks` |
| far past EOF, iterator / `chain_read_at` | `XX000` internal_error | same text |
| one past the last page | `XX001` data_corrupted | `could not read blocks 12..12 ...: read only 0 of 8192 bytes` |

`XX000 internal_error` for a corrupt on-disk pointer is precisely the misclassification
`test/check_source_style.py`'s errcode rule exists to prevent, arriving from underneath
the extension rather than from inside it.

**SEGREAD-14.** `BM25ChainCursor` held `blk` and `seen` and nothing else, and
`chain_read_at`'s resume test was `cur->blk != InvalidBlockNumber && cur->blk !=
BM25_METAPAGE_BLKNO && cur->seen <= off`. Both stored fields are quantities *of one
chain*: `seen` is a byte origin and `blk` a page of the chain the cursor last walked.
A cursor carried to a second chain therefore resumed at the first chain's page using
the first chain's origin. Two comments asserted this could not matter -- "a cursor is
a pure OPTIMIZATION and cannot affect the answer", "correctness does not depend on the
reader being fresh, matching the segment, or being reset". Both were true, and both
were statements about the call sites (each re-inits per segment, one cursor per chain)
rather than about the code, with nothing testing either.

## Decision

**Bound all four walks, and error rather than terminate.** A shared
`bm25_seg_chain_extent_validate(blk, nblocks, what)` raises
`ERRCODE_INDEX_CORRUPTED` before `ReadBuffer`. Two forms of the bound now coexist in
the tree, and which one a walker takes follows from whether a truncated walk is a
defensible answer for it:

- *quiet terminate* (`while (... && blk < nblocks)`) -- `bm25_segcat_find_entry`,
  `bm25_segcat_locate_entry`, and the three debug DICT dumps. A catalog probe
  answering "gen not live" and a dump reporting what it could reach are both
  legitimate answers.
- *error* -- the four walkers here plus `bm25_dict_expand_wildcard`, whose header
  already stated the rule. `bm25_seg_dict_lookup` stopping early reports a term
  ABSENT; `bm25_seg_dict_iter_next` stopping early hands the merge a short dictionary
  and writes a permanently incomplete segment; `chain_read_at` stopping early returns
  false, which its callers read as "treat as live", an invalid TID and doclen 0;
  `bm25_seg_scan_postings` stopping early drops the tail of a term's postings.
  Every one of those is a silently wrong answer.

**Check `post_root` where it is followed, not where it is decoded.**
`bm25_seg_scan_postings` calls `bm25_seg_blkno_validate(post_root, ...)` at entry when
`df > 0` -- ADR 0071's own validator, on the POST walk that lacked it. It is deliberately
NOT added to `bm25_dictentry_validate`, which stays a pure span check: only a follower
knows whether `InvalidBlockNumber` is corruption or the builder's legitimate "this term
wrote no blocks" sentinel (BUILD-07), which is exactly what a `df == 0` entry carries.

**Capture `nblocks` once per walk, never per pointer.** Per-term walks hoist at
function entry (`bm25_seg_dict_lookup`, `bm25_seg_scan_postings`), the iterator captures
in `_begin`, and `chain_read_at` -- which runs per scored posting -- takes it from the
cursor, captured once in `bm25_seg_reader_init` for all three of a reader's cursors.
`BM25ChainCursor` gains `nblocks` for that, with 0 meaning "not captured" so a
zero-filled reader falls back to a per-call capture instead of rejecting every block.

**Give every per-posting chain reader a cursor, so no hot path takes the fallback.**
The paragraph above only removes the syscall from callers that *have* a reader, and the
first landing of this record left six production call sites without one -- the WAND
driver's per-candidate resolve, `@@@`'s TID collector, the BM25F df pass, the phrase and
AND-fallback callbacks, and VACUUM's per-document tombstone sweep. Measurement (below)
showed that is the default ranked path, not a corner. Each of those contexts already has
per-(term, segment) or per-segment scope, so each gets a `BM25SegReader` by value on the
same pattern `TermScoreCtx` has used since H16: opened where the context is pointed at a
segment, never carried across one. Where a context outlives a segment, the reader is
re-opened in the same statement pair that re-points `seg`, and left zeroed while `seg`
is NULL. SEGREAD-14's `root` test is what keeps a missed re-open a *performance* bug
rather than a wrong answer; nothing here relies on that, but it is why the invariant is
enforceable at all.

**Add `BlockNumber root` to `BM25ChainCursor`** and `cur->root == root` to the resume
test, stamped wherever `blk`/`seen` are stamped. A cursor pointed at another chain now
falls back to a fresh root walk.

Both get call-site probes rather than pure-function ones:
`bm25_debug_seg_chain_extent` (reads a real segment header into a private stack copy
and substitutes a caller-chosen chain root, so a corrupt pointer takes the real read
path without writing a page) and `bm25_debug_chain_cursor_crosstalk` (positions one
cursor on NORMS, reads DOCMAP through it, returns the TID for comparison against the
row's heap ctid).

## Alternatives considered

- **Quiet `while (blk != InvalidBlockNumber && blk < nblocks)`, matching the SEGCAT
  siblings letter for letter.** The obvious reading of "match the in-file precedent",
  and wrong for these three: it converts a corrupt link into a smaller result set, a
  short dictionary, or a default value, with nothing raised. The file's own wildcard
  walk already documents the distinction. It is also untestable -- a bound that raises
  nothing cannot be discriminated from its own absence.
- **A visit-count cap, `pos_cursor_load`'s other guard.** Considered and declined: it
  catches cycles, not out-of-extent pointers, and the reported defect is the extent.
  (`pos_cursor_load` keeps both; nothing here removes that.)
- **Hoisting `RelationGetNumberOfBlocks` to the top of `chain_read_at`,** the shape
  the two SEGCAT siblings use. This function runs per scored posting, so that is one
  `lseek` per posting -- the exact cost ADR 0071 refused for `bm25_seg_blkno_validate`.
  ADR 0071 names the alternative itself: capture at cursor open, which is what the
  cursor arm does.

  **The first landing of this record got the boundary wrong, and the correction is the
  point of the paragraph.** It claimed that taking the capture per call on the
  *cursor-less* arm "is a rounding error beside the root-to-target re-walk they already
  do", and that 0071 was therefore "extended here, not contradicted". Both claims died
  on measurement. Six production call sites reached that arm, two of them per posting on
  the *default* ranked path, and the syscall cost 5-18% of wall clock on every shape
  tried (an independent review on a different corpus measured 29-55%). A per-call
  `RelationGetNumberOfBlocks` in `chain_read_at` is not a variant of 0071's declined
  clause; it *is* 0071's declined clause, arriving through a different function. The
  resolution is not to soften either record but to remove the case: those callers now
  hold a reader, so the cursor-less arm no longer runs on any per-posting path and the
  capture happens once per (segment, scan) as 0071 prescribes. 0071's cost objection
  stands, unweakened and now with numbers behind it.
- **Extending `bm25_seg_blkno_validate` with the extent clause instead.** Same
  objection, and it would reverse a decision 0071 makes on measured grounds. The two
  guards are complementary and their rejected values are disjoint: 0071 catches
  `InvalidBlockNumber` (P_NEW, which would EXTEND the relation) and the metapage
  before any read; this catches a link past the extent.
- **Testing the bound as an extracted pure function**, the convention of
  sql/78 and sql/95. It proves the predicate `blk >= nblocks`, which no one doubts,
  and leaves the wiring unasserted -- as sql/95's own header says of every other
  block-pointer guard. For a two-comparison check the wiring IS the change, so a
  predicate-only test would have been green against the pre-fix build.
- **Leaving SEGREAD-14 as a documentation fix**, since no caller crosses chains. It
  would leave the safety property resting on call-site discipline that nothing checks,
  in a struct whose whole purpose is to be passed around.

## Consequences

- One corrupt chain link raises `ERRCODE_INDEX_CORRUPTED` naming the chain, uniformly,
  from every walker in `bm25_seg_read.c` -- instead of `58P01`, `XX000` or `XX001`
  depending on which reader followed it and how far past the extent it pointed.
- A `df > 0` term whose `post_root` cannot be entered now raises instead of matching
  nothing. This is the only *answer-changing* consequence in the record; the rest are
  attribution.
- Cost: one `lseek` per `bm25_seg_reader_init` (per segment per scan, covering all
  three of that reader's chains), per dict iterator, per `bm25_seg_dict_lookup` call,
  and per `bm25_seg_scan_postings` call -- all per (term, segment), none per posting.
  Exactly two call sites reach `chain_read_at` with `cur == NULL` at all, both
  single-call debug probes, and one of those is `bm25_debug_seg_chain_extent`'s `chain`
  walker, which exists to exercise that arm. The per-posting and per-document debug SRFs
  (`bm25_debug_term_contrib`, `bm25_debug_wand_rank`, `bm25_debug_tombstone`) were
  converted along with the production sites: a debug SRF is still a per-posting path, and
  leaving them out would have made the invariant "no cursor-less caller on a per-posting
  path" true only of some files.
- **Measured, 300k docs, two fields, index fully in shared buffers (`shared hit` only,
  zero `read`), PG 18.6, macOS/arm64, `-Og` build; minimum of 3 runs, two interleaved
  repetitions.** "no-lseek" is this tree with the cursor-less capture removed, i.e. the
  behaviour before this record; "+bound" is the first landing; "+readers" is the record
  as it stands.

  | shape | no-lseek | +bound | +readers |
  |---|---|---|---|
  | WAND top-10 (default ranked path) | 487 / 492 ms | 576 / 579 ms | **49.9 / 50.7 ms** |
  | `@@@` membership, 84k hits | 479 / 473 ms | 515 / 517 ms | **22.9 / 23.7 ms** |
  | BM25F 2-field exhaustive, LIMIT 10 | 665 / 646 ms | 691 / 680 ms | **619 / 631 ms** |
  | WAND top-1000 | 1131 / 1123 ms | 1268 / 1265 ms | **670 / 696 ms** |
  | VACUUM, 5% deleted, 300k-doc segment | 1774 / 1763 ms | 1890 / 1926 ms | **168 / 167 ms** |

  The bound alone cost 5-18%. With the readers the same paths are 1.05x to 20x faster
  than the pre-record baseline, because the reader removes the root-to-target re-walk as
  well as the syscall: the top-10 WAND drops from 6.65M buffer hits to 591k, and `@@@`
  from 6.26M to 177k. The exhaustive row moves least by design -- its scorer already had
  a reader (H16), so only the df pass was converted there, which is also the shape that
  shows the syscall was never the whole story. All three builds return bit-identical
  scores, TIDs and counts on this corpus.
- Five comments that asserted the old state are corrected in place: the
  `bm25_segment.c` claim that started this, the DICT-iterator type comment, the
  wildcard walk's backstop list, `bm25_debug_segterms`' "which walkers have what"
  paragraph, and the two cursor notes.
- Three new `bm25_debug_*` functions, revoked from PUBLIC by the install script's
  allowlist loop like the rest of the surface. `bm25_debug_seg_chain_extent` is the
  first lever in the tree able to put an out-of-extent pointer on a real read path,
  which closes for those four call sites the wiring gap sql/95's header could only
  state. `bm25_debug_seg_postings_count` is the same lever on the POST chain, and it
  returns a COUNT rather than a bool on purpose: the defect there is silence, so a probe
  asserting only "did not raise" would have passed the broken build. Its negative
  control asserts `count == df` read back through `bm25_debug_segterms`, not `count > 0`,
  so a walk that emitted some postings and lost the rest fails it too.
- ADR 0071 carries an addendum pointing here: its "a block number past EOF produces the
  buffer manager's own error rather than ours" consequence now describes only its own
  per-pointer sites.
- No probe writes a page, so unlike `bm25_debug_stamp_seg_field_count` they leave the
  index usable; sql/95 asserts that afterwards.
- **Scope, stated so the next reader does not mistake it for completeness.** Three chain
  walks reachable from `src/` still have no extent bound and are deliberately out of
  this record: `bm25_scan_snapshot` and `bm25_segcat_read` (both bounded by an entry
  COUNT against `meta.nsegs`, not by the extent) and `bm25_seg_key` in
  `src/bm25_keymap.c`. They are tracked separately. `bm25_dict_expand_wildcard`'s
  PENDING walk is unbounded on purpose and stays that way -- a live pending chain can
  legitimately grow past an extent read at entry, which its own header explains.

## Addendum (2026-09-27)

Issue #225 closes the "Scope" bullet above. The three chain walks it named as out of
this record are now bounded, and the last per-posting caller without a reader has one.
This record's rule -- every segment chain walk carries an extent bound, and a per-row
caller holds a reader -- now has no named exception.

**SEGCAT.** `bm25_scan_snapshot` and `bm25_segcat_read` raise `ERRCODE_INDEX_CORRUPTED`
("segment catalog chain leaves the index") before ReadBuffer. That is the *error* form,
chosen for this record's own reason: a snapshot or catalog copy that stops early drops
live segments.

They differ from the four walkers above in one respect that had to be decided.
`nblocks` is sampled BEFORE the chain's root is read. `bm25_scan_snapshot` reads
`segcat_root` under the metapage SHARE and walks the whole chain under it, and the
capture is kept off that lock.

`bm25_seg_chain_extent_validate`'s lower-bound argument ("nothing truncates, so a
captured value can never reject a block that exists") assumes the capture FOLLOWS the
chain's publication. Every other reader satisfies that; these walkers do not. A seal or
merge allocates its new catalog page(s) with `bm25_page_alloc`, which extends the
relation when the FSM has nothing, and only then flips `segcat_root` under the metapage
EXCLUSIVE. A walker that sampled before the extension and read the metapage after the
flip holds a healthy root past its own sample.

So the SEGCAT test (`segcat_blk_in_extent`) re-samples once on a would-be violation and
errors only if the block is still past the fresh extent. The ordering is re-sample
after root read, root read after flip, flip after extension, so the fresh extent covers
every page the walker can reach. The re-sample runs only on that path. In
`bm25_scan_snapshot` it runs under the metapage SHARE: one lseek, cheaper than the
ReadBuffer I/O the walk already does under that lock.

`bm25_segcat_find_entry` and `bm25_segcat_locate_entry` keep their *quiet* form, but they
had the same pre-root sample and now take the same re-sample. `locate_entry` runs on
the VACUUM tombstone path without the seal singleton, so there the race could have
reported a healthy entry as "not found in catalog". That is reasoned from the locking,
not reproduced. Its effect is driven instead by `bm25_debug_segcat_walk`, which runs
each walker with a caller-chosen stale sample.

**KEYMAP.** `bm25_seg_key` declined the extent bound because of the per-call
`RelationGetNumberOfBlocks` cost, which is the objection this record answered for the
other dense chains. The answer is the same here: `BM25SegReader` gains a KEYMAP
cursor, and `bm25_seg_reader_init` captures the extent once for all four cursors.

- The cursor carries a cached `key_size`, because a resume needs it before the root
  header is read. It is trusted only while the cursor's root matches (SEGREAD-14's
  test). `bm25_debug_keymap_cursor_crosstalk` is that test's probe, the KEYMAP
  counterpart of `bm25_debug_chain_cursor_crosstalk`.
- The walk is its own loop, `seg_key_cur`, not `chain_read_at`: the root page's header
  puts KEYMAP's byte origin somewhere different from every other dense chain's.
- `bm25_seg_key` stays as a one-shot wrapper over a fresh cursor, so no KEYMAP walk is
  unbounded.

Callers:

- The merge uses its existing per-source reader. Docids ascend, so it makes one forward
  pass.
- The two ranked-row finalizers use `BM25SegKeyCache`: one header and one reader per
  distinct (header block, gen), in the builder's scratch context. This also removes
  their per-row `bm25_seg_header_read`.

Rows arrive in score order, so within a segment a backward jump still re-walks from the
root. For a fully random order the expected gain is about a third fewer KEYMAP page
visits. That figure is an analysis, not a measurement, and is far smaller than the gains
in the table below.

**`bm25_debug_postings`** reads doclens through a per-segment reader via the new
`bm25_seg_reader_doclen`. That function shares its summation body with
`bm25_seg_doclen`, which remains as the root-walk reference sql/110 compares the reader
against.

**Measured.** Setup: a 300k-doc single-segment int8-keyed index, fully in shared
buffers; PG 18.6, macOS/arm64, `-Og` cassert build. The table shows medians of six
interleaved rounds per build (the minimums agree).

| shape | before | after | buffers before -> after |
|---|---|---|---|
| WAND top-100 | 33.4 ms | 33.6 ms | 402,405 -> 402,106 |
| LIMIT 10000, default path (WAND, then exhaustive tail over 100k matches) | 1264 ms | 96.5 ms | 15.59M -> 0.81M |
| LIMIT 10000, wand_top_k = 10000 | 147 ms | 43.4 ms | 1.72M -> 0.42M |
| LIMIT 10000, 3k matches | 41.4 ms | 4.7 ms | 484k -> 33k |
| LIMIT 10000, near-unique scores (100k docs) | 472 ms | 141 ms | 5.67M -> 1.36M |
| merge of 4 x 50k keyed segments | 678 ms | 356 ms | 5.16M -> 1.18M |
| `bm25_debug_postings`, 100k docs / 548k postings | 1350 ms | 137 ms | 13.7M -> 0.55M |

- The ranked shapes gain most where score ties sort rows by TID within a tie, which
  suits a forward cursor. The near-unique row is the less favourable order.
- The per-row header read is gone in every shape.
- Ranked ids, keys and scores were identical before and after on every shape, compared
  by md5 of the ordered result.
- sql/80's cancellation fixture was sized around the old `bm25_debug_postings` cost, so
  it was resized from 100k to 400k docs under its own stated rule. It still fails
  against a mutation that restores the pre-SEGREAD-10 behaviour.

Still out of scope, flagged rather than fixed because each is a different fix:

- `bm25_pending_mark_dead` samples `nblocks` before its `LockPage(ShareLock)`, then walks
  a live pending chain that concurrent appenders (same lock mode) may extend. The same
  stale-low shape could raise a spurious "pending-list chain exceeds the relation's
  extent" during a VACUUM that runs alongside inserts.
- The SEGCAT walkers validate no page kind and cap no visits, so an in-extent stray link
  is limited only by the entry count. A cycle through entry-bearing pages copies
  duplicate entries until the count is met, raising nothing. A cycle through
  zero-content pages would spin inside `bm25_scan_snapshot` under the metapage lock,
  where its interrupt check cannot fire.
- The query-side pending-list walkers remain unbounded by design, for the reason the
  Scope bullet gives for the wildcard PENDING walk.

## Addendum (2026-09-29, #243 and #244)

Two of the three items the 2026-09-27 addendum listed as "still out of scope"
landed, and the helper that addendum names changed name.

**The shared helper.** The addendum's SEGCAT extent test, `segcat_blk_in_extent`,
and the pending walkers' copy of it, `pending_blk_in_extent`, had identical bodies.
They are now one exported function, `bm25_blk_in_extent` (`bm25_seg_read.c`,
declared in `bm25.h`): it tests `blk < *nblocks` and, when that fails, re-reads the
relation's extent once and tests again. Each file keeps its own note on why its
sample can go stale.

**#243: VACUUM's pending sweep re-samples before calling a link corrupt (PR #260).**
This is the first out-of-scope item ("`bm25_pending_mark_dead` samples `nblocks`
before its `LockPage(ShareLock)`"). The sweep holds the seal singleton in ShareLock
mode, which appenders also take, so an INSERT can extend the relation and link a new
tail page after the sweep's sample; a sweep that reached that link raised "pending-list
chain exceeds the relation's extent" on a healthy index. `bm25_pending_mark_dead`
now tests each link with `bm25_blk_in_extent`, and its cycle cap reads the
refreshed extent. The re-sample is sound for the pending chain for a different reason
than for SEGCAT: the appender extends before it links, and the sweep reads the link
under the page's buffer lock, so a sample taken after reading the link covers every
block the link can name. `bm25_debug_pending_nth_page` had the same stale sample and
got the same treatment. `bm25_pending_drain` keeps a plain `blk < nblocks` bound: it
runs under the singleton in ExclusiveLock mode, which the appenders' ShareLock
excludes, so no page can be linked while it walks. Test-only additions: the probe
`bm25_debug_pending_sweep(index, nblocks)` runs the real sweep
(`pending_mark_dead_sampled`) with a caller-chosen sample and a callback that never
reports a TID dead, so a stale sample can be driven deterministically; a `'pending'`
chain for `bm25_debug_stamp_chain_next` shows that a genuinely out-of-extent pending
link still raises, through the probe and through VACUUM. Suite:
`sql/113_pending_sweep_extent`.

**#244: the SEGCAT walkers reject corrupt in-extent links (PR #261).** This is the
second out-of-scope item. The four walkers (`bm25_scan_snapshot`,
`bm25_segcat_read`, `bm25_segcat_find_entry`, `bm25_segcat_locate_entry`) bounded a
link by the extent only. The threat model is on-disk corruption only; nothing in
normal operation produces these links. Three outcomes were possible: a cycle through
entry-bearing pages copied the same entries again until the count was met, so a scan
answered from duplicated segments with no error; a link to a page of another kind had
that page's bytes copied as catalog entries; and a cycle through pages with no entries
never advanced the count, which in `bm25_scan_snapshot` spins under the metapage
SHARE content lock, where `LWLockAcquire` holds interrupts, so the backend could not
be cancelled and writers needing the metapage EXCLUSIVE queued behind it. The lookups
have no entry count at all, so any cycle kept them walking. Every walker now raises
`ERRCODE_INDEX_CORRUPTED` for these four shapes, each caught by a check the others
miss:

- the page kind, on every page a walker reads;
- a page with no entries that links onward (the only legitimately empty page is the
  lone root of an empty catalog or, after a chained publish prepends in front of it,
  the chain's last page);
- a visit cap of `nsegs + 1` pages per walk, checked before `ReadBuffer`;
- for the two copying walkers, no generation listed twice, the only check that sees a
  packed root linked to itself, which fills the count within both other bounds. In
  `bm25_scan_snapshot` it runs after the metapage is released.

`bm25_scan_snapshot` also refuses a link to block 0 before reading it. It holds the
metapage's content lock across the whole walk, so reading the metapage again would
lock a buffer this backend already holds locked. PostgreSQL 18's LWLock-based content
locks let a second SHARE through; PG19 reworked buffer locking and asserts on it, and
nothing promises a production build more than a self-deadlock. The refusal uses the
kind check's own message, so the error does not depend on the major version. The other
three walkers hold no lock across iterations and reach the kind check safely. Test-only
levers: `bm25_debug_segcat_walk` gains `find_absent` and `locate_absent` (they look up a
target that is never present, so no full read is needed first, which a corrupt chain
would defeat), and `bm25_debug_stamp_segcat_empty` writes a catalog page with no entries
that links onward. Suite: `sql/114_segcat_link_corruption`.

Residuals, recorded in the code and closed as residuals in issue #276 rather than
fixed:

- a link to a segment header page passes the kind check, since header pages carry the
  SEGCAT kind too; only the visit cap ends a cycle through them. A walker could
  reject it by requiring `seg_gen == 0` (catalog pages carry 0, the builder stamps a
  header page with its gen, at least 1), with no format change; none does;
- a corrupt `meta.nsegs` is trusted as the cap, the copying walkers' entry count and
  their allocation size.

**Known, open: `bm25_segcat_read` walks the catalog without the metapage lock
(#270).** It reads the metapage under a lock, releases it, and walks the catalog pages
holding only per-page locks. The INSERT path calls it once per row through
`bm25_validate_key_config_for_insert`, before the append takes the seal singleton, so
nothing on that path excludes a merge flip followed by `bm25_reclaim_orphans` and reuse
of a freed catalog page between the meta read and the walk. Before #261 that race
produced garbage entries caught only by later checks; since the kind check it would
surface as a user-visible `ERRCODE_INDEX_CORRUPTED` on a healthy index. The mechanism
is from code reading; it has not been reproduced and its reachability on the INSERT
path is not established.

## Addendum (2026-10-04, #270)

The "Known, open" paragraph about `bm25_segcat_read` and the INSERT path is closed.
The race was reproduced (`t/022_insert_keycheck_race.pl`, with a pause point inside the
old walk) and fixed by issue #270; [0107](0107-catalog-walkers-require-the-metapage-singleton.md)
records the decision. The INSERT key check now copies catalog entry 0 through
`bm25_segcat_first_entry`, which reads the root under the metapage SHARE, and production
callers of the unlocked catalog walkers hold the seal/merge singleton, asserted under
cassert. `bm25_segcat_first_entry` is a fifth reader of the catalog, of the root page
only. It samples the extent before the metapage like the four walkers and shares the
re-sample, the page-kind check and the empty-page-that-links-onward check, and
`sql/110` and `sql/114` drive it through `bm25_debug_segcat_walk`. It does not carry the
visit cap or the duplicate-generation check, which only make sense for a walk.

## Addendum (2026-10-05)

#293 and #294 (ADR 0111) add length bounds to the extent bounds recorded here. A POST run must
emit exactly `df` postings, chain_read_at errors at the chain end and on a page that is not the
writer's full span for its kind, and the docid-to-TID chokepoint validates the TID. The
scan-side pending walkers now read through `bm25_pending_walk_read` (ADR 0110), which carries
the extent bound with re-sampling, the visited-count cycle cap, the epoch and the kind in one
place. Residual, owned by #303 section A: `bm25_seg_scan_postings` has no visit cap, so a cycle
through POST pages holding no blocks never advances `emitted` and spins until cancelled, and
the count bound covers non-empty cycles only.

## Addendum (2026-10-05, PRs #331-#350)

- **Block 0 is refused at both ends.** The metapage gate refuses block 0 in all five metapage
  pointers (ADR 0071's addendum, PR #341), and `bm25_seg_chain_extent_validate` now refuses
  block 0 too, so every sealed-chain walker reports a mid-chain link to the metapage as
  XX002, where it used to read the metapage, fail the gen check and raise a retryable 40001
  (#303.B, PR #346). The SEGCAT and pending walkers reach that helper only for a block already
  out of extent, so the held-metapage case is unaffected.
- **Segment-header roots of 0 are refused** by `bm25_segheader_validate` (PR #342), so a
  corrupt root cannot be followed or harvested into a retired descriptor.
- **The #276 header-link residual is closed** by the catalog/header role check (ADR 0062's
  addendum, PR #341).
- **The residual handed to #303 above is closed.** `bm25_seg_scan_postings` reads through
  `BM25SegWalk` with a visit cap and a per-page progress rule (ADR 0120).
- **The debug dumps take their extent sample lazily** (SEGREAD-07). They used to sample
  `nblocks` before `bm25_segcat_read`, so a concurrently sealed segment's dictionary was
  dropped; a quiet walk now samples at its first read, after the catalog read, and re-samples
  at a link past the extent. This record's precondition (the sample follows the chain's
  publication) now holds at every walker.
