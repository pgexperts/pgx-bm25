---
id: 0100
title: WAND checks a segment's LIVEDOCS bitmap up front, not once per lookup
date: 2026-09-27
status: Accepted
summary: A per-chain buffer profile of WAND's roughly seven accesses per scored pair found per-lookup LIVEDOCS reads against tombstone-free segments to be 36-92% of a frequent-term build, so the ranked WAND path and the shared df pass now verify a segment's bitmap is all set once up front (gated on the lookups it can save) and skip the per-lookup read, never trusting the catalog's live_ndocs for it, which leaves NORMS, DOCMAP and the KEYMAP key projection as the measured remainder.
---

# 0100. WAND checks a segment's LIVEDOCS bitmap up front, not once per lookup

## Context

ADR 0096 kept the open-time `global_ub` sweep after finding it under 1% of a WAND
build, and measured the build as a whole at about seven buffer accesses per scored
(term, document) pair (`w1`: 371,726 over 53,476). It named one suspect it did not
address, `bm25_term_idf`'s multi-field df pass, and left the rest unexplained. Issue
#229 asked for the cost broken down by chain before anything changed.

**Method.** The profile came from a scratch-only instrumented build that was never
committed: a header force-included into every translation unit renamed `ReadBuffer`
to a wrapper counting each call by (phase, page kind), where the page kind is read
from the page's special space and the phase is set at the call sites that matter
(prologue, df pass, segment open, sweep, block decode, `next_geq` header peek,
`bm25_wand_cursor_score_doc`, the driver's candidate resolve, key projection). Two
cross-checks held throughout: the sweep's count equalled the term's block count
from `bm25_debug_block_impacts`, one access per block as ADR 0096 found, and the
part of `EXPLAIN (BUFFERS)` the wrapper did not see grew with the result size
(28 / 118 / about 1,018 at k = 10 / 100 / 1000), consistent with heap fetches.

**Corpora.** The ADR 0096 corpus (100k documents, Zipf-ish vocabulary) in four
layouts: two fields (title and body, as 0096) and body only, each as many segments
(`a2`, 29; `a1`, 26: empty index, INSERT, seal) and as one 99.7k-document segment
plus a 273-document one (`a2m`, `a1m`: built, then merged). Also ADR 0096's corpus B
(every document holds `w1`, the first 2,000 hold it five times), where WAND skips
blocks and later segments quit at the pivot test, single-field (`b1`, 19 segments)
and two-field (`b2`, 24). Also `a1d`/`a2d`: copies of `a1`/`a2` with 515 documents
(0.5%) tombstoned by a VACUUM that could not merge, so that 20 of 26 and 23 of 29
segments carry tombstones. Queries ran from a
rare term (`w4000`) to frequent ones (`w1`, `w1 w10`), plus mixtures (`w3 w30 w300`,
`w1 w3000`). `bm25_native.wand_top_k` was set to the LIMIT, 10, 100 and 1000.

**What the profile showed.** Shares of the ranked query's buffer accesses, before
this change, at k = 10 unless marked (selected cells from 90):

| index | query | total | per pair | LIVE (df pass / score / candidate) | NORMS | DOCMAP | KEYMAP | sweep | decode+peek | DICT+headers | examined / skipped / scored / deep |
|---|---|---|---|---|---|---|---|---|---|---|---|
| a1 | `w1` | 398,527 | 4.02 | 0 / 24.9 / 24.9 | 24.9 | 24.9 | 0.0 | 0.2 | 0.2 | 0.0 | 787 / 0 / 99,176 / 0 |
| a1 | `w1 w10` | 456,846 | 3.02 | 0 / 33.1 / 16.6 | 33.1 | 16.6 | 0.0 | 0.3 | 0.3 | 0.0 | 1,389 / 0 / 151,089 / 3 |
| a2 | `w1` | 375,901 | 6.93 | 36.2 / 14.4 / 14.4 | 19.8 | 14.4 | 0.0 | 0.3 | 0.4 | 0.0 | 1,078 / 0 / 54,225 / 496 |
| a2 | `w3 w30 w300` | 289,546 | 7.23 | 56.4 / 13.8 / 6.4 | 15.1 | 6.4 | 0.0 | 0.5 | 0.8 | 0.6 | 1,306 / 0 / 40,028 / 3,362 |
| a2 | `w1 w3000` | 157,540 | 36.63 | 86.7 / 2.7 / 2.4 | 3.7 | 2.4 | 0.0 | 0.7 | 0.8 | 0.5 | 906 / 425 / 4,301 / 24 |
| a2 | `w4000` | 3,510 | 7.53 | 15.2 / 13.3 / 13.3 | 15.2 | 14.8 | 1.0 | 0.8 | 0.8 | 24.0 | 29 / 0 / 466 / 0 |
| b1 | `w1` | 9,557 | 4.67 | 0 / 21.4 / 21.4 | 21.4 | 21.4 | 0.2 | 8.3 | 0.8 | 4.8 | 53 / 0 / 2,048 / 19 |
| b2 | `w1 f10` | 125,579 | 27.83 | 83.8 / 3.6 / 2.7 | 5.2 | 2.8 | 0.0 | 0.7 | 0.5 | 0.5 | 385 / 3 / 4,512 / 129 |
| a1m | `w4000`, k=1000 | 15,686 | 30.64 | 0 / 3.3 / 3.3 | 3.6 | 3.7 | 82.3 | 0.0 | 0.0 | 0.4 | 5 / 0 / 512 / 0 |
| a2m | `w300`, k=1000 | 58,097 | 9.28 | 10.8 / 10.8 / 10.8 | 11.0 | 10.9 | 43.8 | 0.1 | 0.1 | 0.0 | 50 / 0 / 6,261 / 0 |

Read per pair, the costs are simple, because every NORMS, LIVEDOCS and DOCMAP lookup
is at least one `ReadBuffer`, and for a frequent term almost exactly one (`a2` `w1`:
74,281 NORMS reads for 74,204 lookups). The H16 chain cursor saves the walk from the
root, not the buffer access. The excess, up to 18% on a rare term, is each cursor's
first walk from its chain's root.

- one LIVEDOCS read per scored posting (`score_doc`), one NORMS read per field
  posting, one LIVEDOCS and one DOCMAP read per candidate -- four per pair for a
  single-term, single-field query;
- on a multi-field index, one LIVEDOCS read per posting of each query term's whole
  run in each segment, in the df pass, before any pruning. That is the "seven": `w1`
  on `a2` spends 135,970 accesses on it for 54,225 scored pairs. When a frequent
  term is paired with a rare one, WAND scores few pairs but the df pass still walks
  the frequent term's whole run, which is how `w1 w3000` reaches 36.6 per pair.

LIVEDOCS was therefore the largest single chain in 83 of the 90 cells: 36-50% of a
frequent-term build on one field, 56-70% on two, and 90-92% for the mixed two-field
queries at k = 10, 84-87% of it in the df pass alone. Every one of those reads was
against a segment whose catalog entry recorded no tombstone. The other seven cells
were rare- and mid-frequency terms at large k, six of them on the merged index, where
KEYMAP was larger (below). The sweep was under 1% except on single-field indexes where
WAND skips or abandons much of a frequent term's run, all at k = 10: 6.0-7.2% for
`w1 w3000`, 3.3-3.4% for `w3 w30 w300`, and 2.7-8.3% on corpus B, the case ADR 0096
built to favour it. Block decodes and header peeks
peaked at 11% (`a1m` `w1 w3000`). Dictionary walks mattered only for rare terms,
up to 26% (`a1` `w4000`), since `bm25_seg_dict_lookup` runs twice per (term,
segment). KEYMAP key projection was the other large cost, for a different reason: on
703513a, where this profile was taken, `bm25_seg_key` walked the KEYMAP chain from its
root for every returned row, so a row cost about one access per 2,000 docids of its
position in the segment. On the merged index at k = 1000 that was 80-82% of a
rare-term query.

Issue #225 (ADR 0095's 2026-09-27 addendum) landed before this change and gave the
ranked-row finalizers a per-segment KEYMAP reader (`BM25SegKeyCache`). Re-measured on
its tip, 3dbc2f6, with this change on top, KEYMAP accesses per returned row roughly
halved (merged index, rare term, k = 1000: 12,916 -> 6,272, 25 -> 12 per row), but key
projection is still 78% of that query. Rows arrive in score order, so within a segment
every backward docid jump re-walks the chain from its root.

## Decision

**A query reader checks the segment's LIVEDOCS bitmap once, at init, and when the
first `ndocs` bits are all set it answers liveness without a per-lookup read.**
`bm25_seg_reader_init_checked(r, index, h, expected_lookups)` walks the segment's
LIVEDOCS chain (one page per ~65k documents, with the same page-kind, gen and extent
checks as `chain_read_at`, stopping at the first clear bit) and sets
`BM25SegReader.assume_live` if every document's bit is set. Bytes past the chain's end
count as set, which is what the per-lookup path already answers for them. Since bits
only go from set to clear after seal, a bit seen set was set at every earlier instant,
so a true result means every bit was set when the walk began (the first page's read),
not at its end: an early page can be tombstoned while later pages are read. The start
of the walk is after the scan's MVCC snapshot, which is what the safety argument below
needs. `bm25_seg_reader_doc_is_live` keeps its
docid range check and then skips the bitmap.

**The check is gated on what it can save.** It runs only when `expected_lookups`, the
caller's upper bound on the reader's liveness lookups, is at least twice the bitmap's
page count, so it never costs more than half of what it might save. Without the gate a
rare term in a 10M-document segment would walk ~150 bitmap pages to save a handful of
lookups -- invisible on the corpora measured here, whose largest segment needs two.

Two places opt in. `bm25_term_idf`'s multi-field df pass checks per (term, segment),
gated on the term's df in that segment; it is shared by the WAND builder, the
exhaustive scorer's idf and the WAND debug probes, so idf stays one computation and
the exhaustive path's df pass gets the saving too. The WAND driver checks once per
segment, after its dictionary lookups (a segment holding no query term pays nothing),
gated on the summed df of the present terms, which bounds every lookup its candidate
reader and the cursors can make, and hands the result to each cursor
(`bm25_wand_cursor_open` takes it as `all_live`; the debug probes pass false).

**The catalog's `live_ndocs` is not consulted at all.** It cannot be evidence that a
segment is tombstone-free, because it can overstate the live set.
`bm25_segcat_publish_swap` (merge) copies the surviving catalog entries in its Step A
and publishes the copy later. `bm25_bulkdelete` deliberately does not hold the
metapage singleton across its tombstone loop, and `bm25_merge()`'s RowExclusiveLock is
compatible with VACUUM's lock, so a tombstone on a surviving segment in that window
keeps its cleared bit while the published entry still says `live_ndocs == ndocs`. The
first draft of this change trusted the count and was wrong for exactly that
interleaving: it would have scored a vacuumed document whose heap line pointer may
since have been reused. The count is not used even as a shortcut in the sound
direction (skip the check when it records a tombstone). The gate already bounds a
wasted check, and with the bits as its only input the check can be tested on its own:
sql/111 tombstones the last document of a 13-document segment, whose only clear bit is
in the masked tail byte, and a build that skips the tail byte ranks the dead TID.

What changes is when liveness is sampled: once per reader (per segment, for WAND), not
at each lookup. A document VACUUM tombstones after the check is counted in df and
scored as live, which is what already happens to every dead tuple VACUUM has not yet
reached, and the executor's MVCC visibility check drops it the same way. Its line
pointer can be reused only after the tombstone, which is after the check and so after
the scan's MVCC snapshot, so a new tuple there is invisible to the scan. The ranked
paths already build their whole ranking once and return its TIDs later without
re-reading any bit, so no returned TID was ever checked for liveness at return time.

The instrumentation is not landed. What stays in the tree is the restored
`bench/wand_global_ub.sh`, which reports the deterministic totals: the ranked query's
and the isolated build's buffer accesses, the sweep's exact block count, all four
`bm25_wand_stats` counters, and accesses per scored pair.

**Measured effect** (interleaved A/B, 9 rounds per build alternating order, all 126
(index, query, k) cells; the cassert -Og build inflates absolute times, so buffers
are the primary metric). Ranked `(id, distance)` md5 was identical before and after
in every cell, and identical between WAND and the exhaustive scorer (`wand_top_k =
0`). All four `bm25_wand_stats` counters were identical in every cell. The
instrumented profile showed every non-LIVEDOCS chain unchanged, access for access,
and no per-lookup LIVEDOCS read left on a tombstone-free segment. The checks cost at
most 58 accesses per query (2.8% of `a2` `w4000`, one page per segment for the driver
and one per segment for the df pass, against the 1,467 per-lookup reads they
replaced).

| index | query | k | buffers before -> after | median ms before -> after |
|---|---|---|---|---|
| a1 | `w1` | 10 | 398,509 -> 200,183 (-49.8%) | 32.2 -> 18.7 (-42.0%) |
| a2 | `w1` | 10 | 375,883 -> 131,521 (-65.0%) | 31.4 -> 13.7 (-56.2%) |
| a2 | `w1 w10` | 10 | 643,474 -> 229,293 (-64.4%) | 52.6 -> 22.8 (-56.6%) |
| a2 | `w1 w3000` | 10 | 157,522 -> 12,941 (-91.8%) | 13.9 -> 3.1 (-77.7%) |
| a2 | `w4000` | 10 | 3,492 -> 2,083 (-40.3%) | 1.3 -> 1.3 (-3.9%) |
| b2 | `w1 f10` | 10 | 125,561 -> 12,407 (-90.1%) | 10.8 -> 2.5 (-77.2%) |
| a1m | `w4000` | 1000 | 15,668 -> 14,645 (-6.5%) | 2.0 -> 1.8 (-6.1%) |
| a2d | `w1` | 10 | 378,794 -> 270,716 (-28.5%) | 31.9 -> 25.2 (-20.8%) |
| a1d | `w1 w3000` | 100 | 64,338 -> 63,492 (-1.3%) | 6.0 -> 5.9 (-2.0%) |

Buffers fell in all 126 cells. Median time fell in 121. The five exceptions were
rare-term cells of about 2 ms or less, four of them on the tombstoned `a2d`/`a1d`
indexes: +0.2% to +2.2%. Earlier runs of the same cells moved several percent either
way between runs, so that is at the noise floor, not a measured regression. On the
ADR 0096 corpus as `bench/wand_global_ub.sh` builds it (53 segments), the `w1` build
went from 372,819 accesses (0096's "swept" figure, reproduced exactly) to 130,002,
from 6.97 to 2.43 per pair.

The figures above were measured against 703513a. After rebasing onto #225's tip
(3dbc2f6) the check was repeated against that base on 18 cells (`a1`, `a2`, `b2` x
`w1`, `w1 w10`, `w1 w3000` x k = 10, 1000; 6 interleaved rounds). Ranked md5 was
identical before and after in every round, WAND equalled exhaustive, all four counters
were identical, and buffers fell 40-92% (`a2` `w1` k = 10: 375,880 -> 131,518).

## Alternatives considered

- **Trust the catalog count** (`live_ndocs == ndocs` in the scan snapshot's entry),
  which costs nothing at all. This was the first draft. Adversarial review found the
  merge-swap interleaving above, which makes the count overstate liveness, and trusting
  it would return an unrelated row on a reused TID. Checking the bits costs at most
  one pass over the bitmap per segment, bounded by the gate, and needs no invariant
  from any writer.
- **Fix the lost update instead**, by having `bm25_bulkdelete` hold the metapage
  singleton in share mode across its tombstone loop, or by having the swap re-check
  the entries it copied before its flip. Either is worth doing for its own sake (see
  Consequences), but it changes VACUUM/merge concurrency and is a separate decision.
  This change does not depend on it.
- **Make only the df pass conditional** (the issue's named suspect). This is a
  subset of the decision: it removes the df pass's LIVEDOCS reads (36% of `a2` `w1`)
  and does nothing on a single-field index, where the df pass does not run and
  LIVEDOCS is still half the build. The same check covers the WAND driver and cursors
  with no further risk.
- **Keep the current chain page pinned in `BM25ChainCursor`**, so consecutive
  lookups on one page cost a lock instead of a `ReadBuffer`. This would also cut
  NORMS and DOCMAP, which are now most of what remains, but a pin that outlives a
  call has to survive callbacks, errors and resource-owner cleanup at every
  reader's call site, and it still takes the content lock per lookup. It is a
  larger change with a different risk profile. Not done here.
- **Store per-field df in the dictionary record.** That would remove the df pass
  entirely, but it grows a packed record, a breaking format change (ADR 0009), on
  the same terms ADR 0096 rejected for the stored impact table. The df pass's
  remaining cost after this change is one access per POST page (about 81 for `w1`),
  so the saving would be small.
- **Land per-chain counters as extra `bm25_wand_stats` fields.** Rejected: counting
  by chain means threading a counter through the segment reader, `chain_read_at`,
  the dictionary walk and the POST walkers, all of them hot paths, for one
  measurement. The `EXPLAIN (BUFFERS)` totals and the bench script track the number
  that matters over time, and the breakdown is recorded here.
- **Apply the same check to the exhaustive scorer's scoring callback, the `@@@`
  collector and the phrase pass too.** The same argument covers them, since each
  builds its result once. They were left out to keep this change to the WAND path the
  measurement covered; the exhaustive path shares only the df pass.

## Consequences

- A tombstone-free segment's liveness now costs one bitmap check per segment on the
  ranked WAND path (plus one per (term, segment) in the multi-field df pass), when the
  gate lets it run. After the change a build costs about one NORMS access per field
  posting and one DOCMAP access per candidate: 1.5-2.5 per pair on frequent-term and
  multi-term queries. Rare-term queries remain dominated by fixed costs per segment
  and per returned row (on 3dbc2f6, up to 16 per pair: `a2m` `w4000` at k = 1000,
  78% KEYMAP).
- A segment with any tombstone pays the old price, plus the check's walk up to its
  first clear bit, until a merge rewrites it, and a segment is a merge candidate only
  at 15% tombstoned (`BM25_MERGE_TOMBSTONE_FRAC`).
  With 0.5% of documents tombstoned across 20 of 26 segments, the saving on `w1` fell
  from about 50% to about 24%. A delete-heavy workload between merges will see much
  less of this.
- The cross-page part of the walk is load-bearing and is tested: sql/111 builds a
  140,000-document single segment (a three-page bitmap, well under a second on a
  cassert build) and tombstones a document on the last page. A walk that stops after
  the first page calls that segment all-live and ranks the dead TID; independent review
  found that mutant surviving the suite before this case was added.
- Liveness on this path is sampled at reader init. Anything that needs "tombstoned
  after that" to exclude a document from a ranked build, rather than leaving that to
  heap visibility, has to open its readers with `bm25_seg_reader_init`, not
  `_init_checked`. VACUUM's bulkdelete and the merge replay already do, and must keep
  doing so: they decide what to tombstone or drop from the bits themselves.
- `bm25_wand_cursor_open` takes an `all_live` flag, which must come from
  `bm25_seg_reader_init_checked` on the same segment in the same call. The debug
  probes pass false, so they read the bitmap per posting as before.
- The review that found the merge-swap interleaving also found things this change
  neither causes nor fixes. They predate it and need their own issues. First, the same
  interleaving loses the tombstone's decrement to `live_ndocs`, `total_len` and
  `total_tokens` in the published catalog, which is statistics drift. Second, a
  tombstone landing on a merge INPUT after the replay read its bit is lost outright:
  the merged segment republishes the document live, on every query path. Third,
  `bm25_livedocs_clear` registers its Generic WAL buffers LIVE, catalog, metapage,
  and redo locks them in that order, which may invert ADR 0018's metapage-first rule
  on a hot standby. That last one has not been verified against the server source.
- Measured and not done here, in order of size: KEYMAP key projection still re-walks
  from the chain root on each backward docid jump, because rows arrive in score order
  (78% of a rare-term k=1000 query on a large segment even with #225's per-segment
  reader; resolving keys in (segment, docid) order would make it one forward pass);
  NORMS and DOCMAP cost one `ReadBuffer` per lookup even when the cursor is already on
  the page;
  `bm25_seg_dict_lookup` runs twice per (term, segment) on the ranked path, up to 26%
  of a rare-term query; the exhaustive scorer's scoring callback, the `@@@` collector
  and the phrase pass still read LIVEDOCS per lookup on tombstone-free segments.

## Addendum (2026-09-28)

The Consequences section named, as found in review and left unfixed, "a
tombstone landing on a merge INPUT after the replay read its bit is lost
outright: the merged segment republishes the document live." That race
(issue #239) and its counter-drift companion (issue #241, the merge swap
losing a concurrent tombstone's `live_ndocs`/`total_len`/`total_tokens`
decrement) are now both fixed: [0102](0102-bulkdelete-holds-the-singleton-for-its-whole-pass.md)
makes `bm25_bulkdelete` hold the metapage singleton in ShareLock mode across
its whole tombstone pass, so no merge, seal, `bm25_upgrade` rewrite, or
reclaim can swap the catalog underneath a snapshot VACUUM is still acting on.
This ADR's decision not to trust `live_ndocs` as liveness evidence is
unaffected — the WAND path still verifies the LIVEDOCS bitmap itself rather
than the catalog count, since the count is now merely no-longer-wrong instead
of load-bearing.

## Addendum (2026-09-29)

The Consequences section flagged that `bm25_livedocs_clear` "registers its Generic
WAL buffers LIVE, catalog, metapage" and that the resulting redo lock order
inverting ADR 0018 on a hot standby "has not been verified against the server
source." It has been verified and fixed: `generic_redo` locks registered blocks
EXCLUSIVE in registration order and holds them until the record is applied, so the
inversion was real. Issue #240 / PR #255 registers the metapage first in
`bm25_livedocs_clear` and in two other records with the same shape; see the
addendum on [0018](0018-metapage-before-segcat-lock-order.md) for the rule and its
enforcement (`t/021_generic_wal_meta_first.pl`).

## Addendum (2026-09-29, #246 items 4, 1 and 3)

Three of the five items in issue #246, which collected this record's "measured and
not done" list and added a fifth, landed as its follow-up, each as its own change.
Items 2 and 5 were deferred by decision and are tracked in #267. Every measurement below is an interleaved A/B
against the change's parent (the three are stacked, in the order 4, 1, 3), six
rounds, with the result md5 identical in every cell (the ranked `(id, distance)` pairs, or
the id set for the `@@@` filter cells) and, where a cell reports them, all four `bm25_wand_stats` counters identical; buffers are the
primary metric, as above. The corpus is 100,000 synthetic documents over about
5,000 terms (`w1` and up) drawn with a skewed frequency distribution. `a1` is one field with many segments, `a2` two fields with many
segments, `a1m` and `a2m` the same tables after one `bm25_merge` pass (15 and 16 segments at
setup), and `a2d` an `a2` layout with about 0.5% of documents (516 rows) deleted and
tombstoned by a VACUUM.

**Item 4: LIVEDOCS is checked once per segment on the remaining paths (PR #264).**
The alternative this record declined ("Apply the same check to the exhaustive
scorer's scoring callback, the `@@@` collector and the phrase pass too") was taken.
The exhaustive scorer's reader and the `@@@` collector's reader open with
`bm25_seg_reader_init_checked`, gated on the term's `seg_df` as the df pass is. The
phrase stash and the boolean/AND presence pass reuse the scorer reader's answer for
the same (term, segment) through `bm25_seg_reader_init_known`, so all three gate a
posting on one liveness sample, as their "gated identically to the scorer" contract
asks. The safety argument above carries over: the check runs after the scan's MVCC
snapshot, and each path builds its result once and returns the TIDs later without
re-reading a bit. VACUUM's bulkdelete and the merge replay keep the per-lookup
reader. Across 36 cells the buffer counts fell by 6.3% to 48.3%; for example `a1`
`w1` at k = 10 on the exhaustive path went from 397,160 to 298,034 (-25.0%), the
phrase `"w1 w2"` from 1,027,252 to 634,528 (-38.2%), and the `@@@` filter on `w1`
from 207,772 to 108,646 (-47.7%). The tombstoned `a2d` saved less (-7.5% to -29.3%),
as this record predicts for a segment that pays the per-lookup price.

One correction to the Consequences section: "The debug probes pass false, so they
read the bitmap per posting as before" now holds only for the WAND probes, which
open cursors with `all_live` false. `bm25_debug_rank` and `bm25_debug_rank_key` run
the exhaustive builder and sample liveness once per (term, segment) like the query
path. They still have no heap fetch, so, like any dead row VACUUM has not yet
tombstoned, they can list a row a query would drop at its visibility check.

**Item 1: ranked rows' keys are resolved in one forward KEYMAP pass per segment
(PR #265).** Both ranked-row finalizers used to resolve each row's `key_field` in
rank order through the per-segment reader from #225, and every backward docid jump
re-walked the segment's KEYMAP chain from its root. They now collect each row's
(segment, docid, rank slot) and hand them to `bm25_seg_key_cache_fill`. Rows of a
segment whose KEYMAP spans several pages are sorted by (segment, docid) and resolved
in one forward pass; rows of a one-page KEYMAP, where a backward jump costs nothing,
are resolved at once in rank order, because sorting about 100,000 of them measurably
slowed an exhaustive ranking on an index of small segments and saved no access. A
key is a function of (segment, docid) and the slots do not overlap, so the ranked
keys and presence flags are the same bytes as before, and both builders go through
one helper. The per-document charge in `BM25_MATCH_BYTES_PER_DOC` includes the key
request. Across 67 cells, on the merged indexes: `a1m` `w4000` at
k = 1000 on the WAND path 8,001 to 2,293 buffers (-71.3%), `a1m` `w300` k = 1000
20,742 to 14,445 (-30.4%), `a1m` `w1` k = 10 on the exhaustive path 504,184 to
297,753 (-40.9%), and `a2m` `w1 w10` k = 1000 exhaustive 1,669,673 to 536,715
(-67.9%). The many-segment indexes (`a1`, `a2`) change by 0.0% in the cells shown.

The Context's access counts ("12,916 -> 6,272") were measured on `a1m`; the
Consequences' "78% KEYMAP" for `w4000` at k = 1000 is `a2m`'s. The #246 measurement
notes give the key projection's share as 78.4% on `a1m` and 78.2% on `a2m`, and,
after item 1, 562 KEYMAP accesses on `a1m` and 585 on `a2m`, about 24% of the query.
Those figures come from comparing a keyed index against a keyless one, and that
output was not kept, so they are notes, not retained measurements. The query
totals above agree with them: for `a1m` `w4000` at k = 1000, 6,272 of 8,001 buffers is 78.4%, and the drop of 5,708 buffers leaves
564 accesses against the 562 recorded; for `a2m`, 78.2% of 8,514 less the 6,073-buffer
drop leaves 585.

**Item 3: the df pass hands its dictionary lookups to the scorers (PR #266).**
`bm25_term_idf` records each (term, segment) lookup (found, postings root and offset,
df, and the entry's POS root and offset) in `BM25TermSegLoc`, and the WAND driver
and the exhaustive scorer read those instead of walking the dictionary a second
time. Both run on the snapshot the df pass used, and a sealed segment's dictionary
does not change, so the entries are what a second walk would return. A segment
holding no query term is also skipped before its header is read, since the df pass
already read and gen-checked that header in the same build. This removes the second
of the two `bm25_seg_dict_lookup` walks per (term, segment) that this record
measured, together, at up to 26% of a rare-term query. Across 71 cells: `a1` `w4000` at k = 10 on the WAND path 2,110 to 1,460
buffers (-30.8%), the three-rare-term query `w4000 w4001 w4002` 5,101 to 3,151
(-38.2%) on WAND and 9,036 to 7,086 (-21.6%) on the exhaustive path, `a1` `w300`
k = 10 13,147 to 12,697 (-3.4%), and the frequent term `w1` essentially unchanged
(200,291 to 200,241). The commit message's reason for the shape: the lookup is a
linear walk of the sorted dictionary chain, so a rare term late in the sort order
paid for most of the chain twice per segment.

Tests: `sql/117_exhaustive_livedocs_checked` (item 4), `sql/118_ranked_keys_forward_pass`
(item 1), `sql/119_dict_lookup_handoff` (item 3).

What remains is #267 (NORMS/DOCMAP `ReadBuffer` per lookup with the cursor already
on the page, item 2, from this record's list; a key resolved for every ranked row
rather than only the rows returned, item 5, which #246 added), and #274 (the exhaustive path still
reads a segment header twice per (term, segment) where the term is present, and
`bench/wand_global_ub.sh` has no merged-segment or k = 1000 cells).

## Addendum (2026-10-04, #267, #269 and #274)

The 2026-09-29 addendum left three things open: #267 (the NORMS and DOCMAP
`ReadBuffer` per lookup with the cursor already on the page, item 2, and a key resolved
for every ranked row, item 5) and #274 (the exhaustive path's repeated header read, and
a bench with no merged-segment or k = 1000 cells). They are settled as follows.

**Item 2: a per-cursor page image, not a pin (#267).** The alternative this record
declined, keeping the chain page pinned in `BM25ChainCursor`, was not built. The user
decided on a page-image copy; the pin had been deferred because of resource-owner
cleanup at about twenty call sites. A cursor that opts in through
`bm25_seg_reader_cache_pages` keeps a `BLCKSZ` copy of the page its last lookup landed
on, taken under the share lock after the usual extent, kind and gen checks. A later
lookup on that block is served from the copy with no buffer access, and a lookup on
another page reads, validates and replaces the copy. The buffer is still released before
every lookup returns, so no reader needs resource-owner handling, and the copy is
ordinary memory freed with its context on ERROR.

The copy equals the page because the chains are immutable at their gen: NORMS, DOCMAP
and KEYMAP are written once before the segment is published, and gens never repeat.
LIVEDOCS is never imaged, because `bm25_livedocs_clear` flips its bits in place, and it
is excluded twice: `bm25_seg_reader_cache_pages` never gives a LIVEDOCS cursor a
context, and the page-access helpers refuse `BM25_PAGE_LIVE`. Opting in is explicit
because the copy lives in a context the reader does not own, so each owner names one that
outlives the reader and bounds the copies. WAND's cursors and driver reader live in its
per-segment context, one copy per query term plus one. The exhaustive scorer's three
readers and the `@@@` collector release theirs after each (term, segment). The
ranked-row key cache gives images to at most 64 segment readers (512 KB). None of it is
charged to `max_match_memory`, which bounds per-document materialization, not readers.

**Measured effect.** Interleaved A/B against the parent, 6 rounds, 192 cells: the
bench's ten queries at k = 10, 100 and 1000 on WAND and the exhaustive path, plus
phrase, boolean and `@@@` cells, on a 53-segment, a merged two-field and a merged
single-field 100k-document corpus. Ranked output md5 and all four `bm25_wand_stats`
counters were identical in every cell. Buffers fell in every cell, by 27.3% to 99.9%
(`a2` `w1` WAND k = 10: 129,910 -> 3,326; `a2` `w1` exhaustive: 372,143 -> 860; `a2m`
`w1` WAND k = 1000: 239,660 -> 3,430; the least, `a2` `w4000` WAND k = 10: 1,707 ->
1,241). Median time fell in every cell, by 2.1% to 75.5%, on a cassert build at a load
average of 4.5 to 6.7 (`a2` `w1` WAND k = 10: 14.4 -> 5.1 ms). After the change
`bench/wand_global_ub.sh` reports 0.03-0.23 accesses per scored pair for the eight
ordinary queries at `LIMIT` 10 on the many-segment layout (`w1`: 3,365 accesses for
53,476 pairs), against 1.49-2.43 before it (`w1`: 129,949), so the Consequences'
figure of 1.5-2.5 per pair no longer describes the build. `sql/121_chain_page_image`
compares every ranked path on a one-segment, multi-page shape with an identical table
whose rows are all pending, which are scored without any segment chain, and bounds the
buffer accesses per scored pair; the bound fails on the parent, and a copy served
without the block-number check fails the comparison in every mode. Its 72-segment keyed
case covers the key cache across segments, past its 64-image cap, and WAND's per-segment
context; it adds coverage, not discrimination.

**Item 5 is a residual.** `bm25_seg_key_cache_fill` (`bm25_keymap.c`) still resolves a
key for every ranked row, while under `ORDER BY ... LIMIT k` the executor returns only
the first k; an exhaustive ranking of about 100,000 rows resolves about 100,000 keys.
With the page images a key on the page the previous lookup landed on costs no buffer
access, so what is left is mostly per-row CPU. Resolving lazily, on emit or per block of
rows, is entangled with three things that each want every key up front:
`bm25_build_score_index` builds the score-by-key hash over all of `ranked_keys`; the
query-qualified `bm25_score_key(key, query)` accessors (#263) read that hash; and the
over-pull tail rebuild ([0108](0108-a-wand-rebuild-may-only-extend-the-emitted-prefix.md))
replaces the ranking mid-scan, so keys resolved after it must come from the rebuilt
ranking. A lazy scheme has to fill the remaining keys before the hash is built and
re-key on a rebuild.

**#274.** The bench gained a `merged` layout, which runs `bm25_merge` after the seal
under a 1GB `maintenance_work_mem`, and `LIMIT 1000` cells; the CSV columns and the CI's
two-argument invocation are unchanged. `bench/README.md` was re-measured at 100k
documents in both layouts. P1, the exhaustive scorer's repeated header read and
multi-field LIVEDOCS check, stays a residual, recorded at the re-read in
`bm25_scan_rank.c`. The exhaustive scorer re-reads the segment header the df pass just
read for each (term, segment) holding the term, and on a multi-field index walks the
LIVEDOCS bitmap the df pass's own check already walked, because `BM25TermSegLoc` carries
neither answer forward. The repeat is at most one header page plus one bitmap page per
about 65k documents. Bounds computed on the bench corpus (53 segments, `wand_top_k = 0`,
`LIMIT` 10, PG 18.6 cassert): at most 424 of 852,401 accesses for `w1 w3 w10 w30`, and
106 of 2,852 for the rare `w4000`. A wider hand-off struct was judged not worth its
coupling yet. If it is built, it must keep this record's rule that liveness is sampled
once, after the scan's MVCC snapshot.

**#269: the safety argument is now tested.** The Decision's safety argument, that a
document tombstoned after the sample can have its heap line pointer reused only after
the scan's snapshot, so heap visibility drops the new tuple, had been a comment and a
paragraph. `t/023_slow_cursor_tid_reuse.pl` runs the interleaving. Five REPEATABLE READ
cursors (exhaustive, phrase, boolean, unranked filter and WAND) each fetch one row while
the deleted document is still marked live; VACUUM then tombstones it, an INSERT takes
its line pointer, and the cursors fetch the rest. The new row matches the queries, so
only heap visibility can keep it out. The test asserts the heap placement and the reuse
by ctid, the document's LIVEDOCS bit set at sampling and clear after VACUUM, that the
reused row never appears, and that each cursor returns exactly the live rows; a regime
witness shows the index still handed out the dead TID. A mutation that bypasses the scan
snapshot fails it on all five paths. The fresh-scan half is left to `sql/117` and, for
WAND, `sql/111`. T1 to T3 of #269 are recorded as known, deliberately untested residuals
in comments in `sql/114` (T1) and `sql/113` (T2, T3), each either covered by suites that
are already mutation-tested or needing a new pause point for little gain.

**Not done here, and why.**

- The merge replay (`bm25_merge.c`) still opens its reader with plain
  `bm25_seg_reader_init`, so it does not use page images, though it reads liveness for every
  document of every chosen input segment, and DOCMAP and NORMS for every live one. The merge holds
  the singleton in ExclusiveLock across the replay, and nothing has been measured, so
  whether and how to opt it in is left open.
- Aborted and uncommitted pending entries, and the scanning transaction's own, still
  count in idf and avgdl until a seal tombstones them. That is the statistics model the
  ranked paths already had, noted at the tail rebuild's invariant comment in
  `bm25_scan.c`; this work did not change it, and #268 only pins it for the tail.

## Addendum (2026-10-04)

The fresh-eyes review of this date found that treating 'past chain end' as live (and, for DOCMAP/NORMS, as a default value) lets a corrupt short LIVEDOCS chain resurrect tombstoned documents as wrong rows on reused TIDs (permanently, through a merge), lets a short DOCMAP chain hand the executor an invalid TID that makes a read-only SELECT extend the heap (or aborts a cassert backend), and a short NORMS chain score with doclen 0. The builder writes exact lengths, so a short chain is never a legitimate state. Tracked in #294.

## Addendum (2026-10-05)

#293 and #294 (ADR 0111) reverse one sentence of the Decision: "Bytes past the chain's end
count as set". `seg_livedocs_all_set` now raises `ERRCODE_INDEX_CORRUPTED` at the chain end
and on a page that is not the writer's full span, as `chain_read_at` does, and the per-lookup
path that "already answered" live for those bytes was the fallback being removed.
`bm25_livedocs_locate` applies the same rules, and `bm25_livedocs_clear` bounds its byte by
the page content. The safety argument is unaffected: bits still only go from set to clear
after seal.
