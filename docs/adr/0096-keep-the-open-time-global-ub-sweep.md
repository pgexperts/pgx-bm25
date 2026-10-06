---
id: 0096
title: Keep the open-time global_ub sweep; stored term-level impact tables rejected on measurement
date: 2026-09-23
status: Accepted
summary: WAND keeps computing each cursor's global upper bound by sweeping the term's block headers at open, because storing the bound's inputs in the dictionary record (format v9) was built, measured, and found to save under 1% of buffer traffic on an ordinary corpus (7.8% on one built to favour it) while costing rare-term queries 11-15%.
---

# 0096. Keep the open-time global_ub sweep; stored term-level impact tables rejected on measurement

## Context

Issue #153's QRY-03 finding: `wand_cursor_sweep_global_ub` reads every block header
of every query term's posting run, per segment, at cursor open and before anything is
pruned -- one `ReadBuffer`/lock/release per block, not per page. The comments called
this "a cheap header-only pass" and "a fixed one-time cost". The issue argued it caps
WAND's achievable speedup, and proposed either (a) storing the bound's inputs at seal
time next to the dictionary entry, or (b) batching the sweep by page.

Option (a) was chosen and built in two stages. The bound cannot be a stored scalar:
`bm25_block_ub` needs query-time idf, boosts, avgdl, k1 and b. What can be stored is a
term-level impact table -- the per-block `BM25BlockImpact` folded over the term's whole
posting run (max `max_tf`, min `min_doclen`, union of fields), which dominates every
block's table, so its bound is looser and never lower. Stage one wrote that table after
the term bytes of each DICT record. That grows a packed record, which ADR 0009
classifies as breaking, so it was format v9: `BM25_FORMAT_VERSION` 8 to 9, a
build-stamped feature bit and read floor, and every index built by the new binary
refused by a v8 one. Stage two made WAND read the table instead of sweeping, falling
back to the sweep for a record without one. The stage-one design record itself required
stage two to show "that the pruning lost is worth the round trips saved, not merely a
correctness argument". This record is that measurement and what followed from it.

## Decision

We keep the sweep. The stored table, the v9 format bump beneath it, and the consumer
that read it are not landed. QRY-03 is resolved by correcting the comments that
understated the sweep's cost, so they state it and point here.

The measurement used the bench script `bench/wand_global_ub.sh`, preserved with both
stages on the unlanded local branch `archive/153-qry03-stored-termimp`. Each corpus
went into pairs of tables with identical rows, each indexed empty, filled by INSERT in
the same order and sealed, so segment layouts match; one index of each pair had the
feature bit cleared first, so its records carry no tables and every cursor sweeps.
Same binary, and the same fallback path the consumer takes for any pre-v9 segment.

**Corpus A, the bench script's own** (two fields: title 3-8 tokens, body 20-300; term
ranks drawn log-uniformly over w1..w4999, so a frequent term's max tf and min doclen
fall in different blocks; 100,000 documents, 53 segments; the script's 8 queries x LIMIT
{10,100}, plus `w1 w3000` and `w4000` run by hand):

- **No extra block read and no extra document scored**, but this corpus barely prunes:
  `blocks_skipped` was 0 in every cell and `blocks_examined` was usually the term's
  whole block count, so the regime QRY-03 worries about did not occur. `docs_scored` and
  `blocks_examined` were identical on both sides in all 16 cells, and every query
  returned the same ids in the same order. The looser bound did change pivot choice:
  `deep_check_skips` was higher on the stored side in 12 of the 16 cells, by up to about
  14% (`w10 w100`, LIMIT 100: 1,625 vs 1,424), and the block-max deep check rejected
  every extra pivot before it cost a decode. The stored bound was 0.17% above the swept
  one on average over `w1`'s 53 segments (3.4% at most; 21% for some mid-frequency
  terms).
- **The sweep was under 1% of the WAND build.** Its cost is exactly one buffer access
  per block (`w1`: 1,093 blocks, and the stored side's saving is exactly 1,093). Isolated
  WAND builds (`bm25_wand_stats` under EXPLAIN BUFFERS): `w1` 371,726 vs 372,819,
  `w1 w3000` 156,029 vs 156,753 (net of the rare term's larger dictionary walk),
  `w1 w10` 678,935 vs 680,699. The build as a whole costs about seven accesses per scored
  (term, document) pair (`w1`: 371,726 over 53,476).
- **Rare terms got worse: +11-15% buffers.** On this two-field
  corpus, with 2-5 byte terms, the table grows DICT records from 24-32 bytes to 32-48
  (a table is `1 + 9 * nfields` bytes before MAXALIGN) and the DICT chains from 1,007
  to 1,440 pages (+43%; corpus B's single-field dictionary, with 1-4 byte terms, grew
  +50%). `bm25_seg_dict_lookup` is a linear walk of every entry sorting before the key,
  run twice per (term, segment) on the ranked path -- by `bm25_term_idf` and by WAND --
  and again by the unordered `@@@` path's `bm25_load_if_needed`. `w4000`: 5,174 vs 4,655
  accesses for the whole ranked query, 3,897 vs 3,378 for the WAND build alone. The
  latency effect is smaller and noisier: interleaved pgbench put stored at 2.06-2.61 ms
  and swept at 1.88-2.48 ms over several runs -- overlapping ranges, stored about 5-10%
  slower at the midpoints.
- **Frequent terms: no latency difference.** Interleaved pgbench: `w1 w10` 62-64 ms
  on both sides, `w1 w3000` 22.5-25.5 ms on both. (The script's own EXPLAIN ANALYZE
  timings put the stored side 0.6-8.4% slower, but it always times the stored table
  first, so those numbers are confounded by run order.)

**Corpus B, built to favour the stored table** (one field; 100,000 documents all
containing `w1`; the first 2,000 inserted hold it five times in a six-token document,
the rest once in a ~100-token document). With the strong documents in the first
segments, the top-k fills early and later segments give up at the pivot test (`piv <
0`) straight after cursor open -- after the sweep has already read every block of the
term. That is the case the sweep is worst at: WAND examined 73 of 815 blocks, the sweep
read all 815. `w1`, LIMIT 10, 39 segments: 8,680 vs 9,416 buffer accesses, a **7.8%**
saving. Merged down to 13 segments the same query saved 1.2% (61,097 vs 61,860); with
the strong documents inserted last, 0.2%.

So the saving is not bounded the way it first appeared. When `bm25_wand_cursor_next_geq`
skips a block it reads that block's header anyway, so under skipping the sweep only
doubles header traffic. But early termination leaves the tail of a term's run unread,
and the sweep's share then grows with the fraction left unread. The largest share
observed was 7.8%, on a corpus built to produce it.

## Alternatives considered

- **Land stages one and two anyway** (stage one through two adversarial reviews and
  the full suites; stage two through the WAND regression suites, not yet reviewed).
  Rejected: a format bump that locks v8 binaries out of every new index, 43-50% more
  DICT pages on the indexes measured and an 11-15% rare-term buffer penalty that comes
  with them, and an
  ADR 0009 exception ("feature_flags never gates" no longer true of writers), to buy
  under 1% on an ordinary corpus and 7.8% on one built to favour it.
- **Land them and make DICT lookup page-skipping** (a per-page first-key header, the
  upgrade path `bm25_seg_dict_lookup`'s own comment names). That would win back the
  rare-term cost, but it is a second format change stacked on a first one that pays
  little by itself. If dictionary lookup ever needs page skipping, it can be built on
  its own merits.
- **Page-batched sweep** (the issue's option (b)): keep the buffer pinned across the
  blocks on one page, cutting the sweep's lock traffic roughly tenfold for a frequent
  term (`w1`'s 1,093 blocks sit on 105 pages) with no format change. Not built: it
  optimizes the same under-1% share, and in corpus B it would still pin every page of
  the unread tail.
- **Tightening global_ub to the max over remaining blocks** (the issue's second point):
  not pursued. Looser bounds (0.17% on average, up to 21%) moved pivot choice on corpus
  A but never what was decoded or scored, which is weak evidence that the tighter
  direction would not help much either -- weak because corpus A barely prunes at all.

## Consequences

- The sweep stays on every WAND cursor open, and its comments now state its cost
  (linear in the term's block count, one buffer round trip per block) instead of
  calling it cheap.
- `BM25DictEntry.dict_pad` stays padding and the format stays v8. The stage-one design
  work -- the residue argument for `dict_pad`, the writer gate, the build-time floor
  stamp -- lives on the archive branch, not in this tree.
- The measurements are from two corpus shapes on one machine, and corpus A never
  exercised block skipping. A workload where WAND terminates early on long posting runs
  across many segments -- corpus B's shape, or one more extreme -- gives the stored table
  its best case. Reopening this needs a measurement from such a workload that beats the
  rare-term penalty, not a re-argument.
- Where WAND's buffer traffic actually goes -- about seven accesses per scored (term,
  document) pair, not yet broken down by chain -- is the lever the issue was looking for.
  One piece is visible already: on a multi-field index `bm25_term_idf` decodes each query
  term's whole posting run per segment for its per-field df before WAND starts, a second
  unconditional pre-pruning walk heavier per block than the header sweep. Neither is
  addressed here.

## Addendum (2026-10-04, the sweep's share after #267)

The premise of this record was that the sweep is under 1% of a WAND build. After #267
([0100](0100-wand-checks-livedocs-up-front.md)'s 2026-10-04 addendum) served same-page
NORMS, DOCMAP and KEYMAP lookups from a page image, it no longer is. `bench/README.md`
("After #267") reports the open-time sweep as 15-36% of the eight ordinary queries'
builds at `LIMIT` 10 on the many-segment layout (`w1`: 1,093 of 3,365 accesses) and up
to 43% on the merged layout. Those are shares of buffer accesses, counted as `ReadBuffer` calls whether they hit or
read; the sweep's share of time was not measured.

What the changed premise invalidates is one of the two reasons given above for not
building the page-batched sweep, that it optimizes the same under-1% share (the other,
that in corpus B it would still pin every page of the unread tail, is untouched). That
alternative needs no format change. What it does not invalidate: the other reasons for not landing the
stored table, which are about the stored table and not about the sweep's share, namely
the rare-term buffer penalty, the growth of the DICT chains, and the v9 format break.
If the sweep is reopened, the page-batched sweep is the first lever to measure. Nothing
is decided here; the sweep stays as this record left it.

## Addendum (2026-10-05)

#289 (ADR 0113) changes what the open-time sweep computes, not what it reads.
`wand_cursor_sweep_global_ub` now takes, per field, the maximum over blocks of that field's
bound and sums the maxima, because the maximum over blocks of per-block sums did not dominate a
document whose postings straddle two blocks. It reads the same block headers and impact tables,
so it adds no I/O. #293 (ADR 0111) makes it enforce the `df` count as well. Nothing is stored
that has to stay in sync.
