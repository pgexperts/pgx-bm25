---
id: 0072
title: impact_bytes == 0 is corruption, and bm25_format.h is the authority when a comment contradicts the code
date: 2026-08-20
status: Accepted
summary: bm25_block_validate now bounds impact_bytes on both sides; the decoder's "tolerated defensively" treatment of a zero-length impact table was not defensive but harmful, because a zero-field table makes bm25_block_ub return 0.0 and the WAND driver prune every posting in the block.
---

# 0072. impact_bytes == 0 is corruption, and bm25_format.h is the authority when a comment contradicts the code

## Context

`BM25BlockHeader.impact_bytes` is the length of a posting block's trailing impact
table. Until now it appeared in `bm25_block_validate` only as a summand in
`block_len`, so a too-*large* value was caught incidentally by the page-overrun
check while **zero passed straight through**.

Zero is the damaging value, and it is damaging on the default path.
`bm25_decode_impact_table` returned an all-zero table for `nbytes == 0`,
documented at its head as "should not occur post-v5, but tolerated defensively".
That tolerance is not defensive. An all-zero table yields `imp->nfields == 0`;
`bm25_block_ub` then loops zero times and returns `ub = 0.0`; the WAND driver
prunes every posting in the block. WAND is on by default, so the visible symptom
is *rows silently missing from an ordinary ranked query*, with no error anywhere.

Two comments in the tree contradicted each other outright.
`src/bm25_format.h` specifies `impact_bytes` is "never 0 — every block has at
least a 1-byte nfields count". `src/bm25_seg_build.c` said the zero case was
tolerated. ADR 0027, which introduced both functions and discusses `nfields` and
table length, never mentions `nbytes == 0`, so this was not a recorded accepted
risk — it was drift.

## Decision

`impact_bytes` is bounded on both sides in `bm25_block_validate`: at least the
1-byte `nfields` count, at most one count plus a full `BM25_MAX_FIELDS` table
(`1 + 32*9 = 289`). Both bounds raise `ERRCODE_INDEX_CORRUPTED`.

**When an inline comment and `bm25_format.h` disagree about the on-disk contract,
`bm25_format.h` wins.** It is the file the format is specified in; a decoder
comment describing tolerance it should not have been extending is the thing that
gets corrected.

The early return in `bm25_decode_impact_table` is kept, with its justification
rewritten to the truth: it now serves only `bm25_debug_impact_decode_bytes`, the
single caller that supplies its own buffer and may legitimately pass an empty
`bytea`. No page-sourced caller can reach it, because both on-page readers route
through `bm25_block_validate` first, and `bm25_encode_impact_table` always emits
at least the count byte.

## Alternatives considered

- **Keep tolerating zero and fix `bm25_block_ub` to treat a zero-field table as
  "no bound available" rather than 0.0** — would stop the silent pruning, but it
  spreads one on-disk contract across two files and leaves a block on the page
  that the format says cannot exist. Corruption should be rejected where it is
  read, not compensated for downstream.
- **Update `bm25_format.h` to permit zero instead** — the cheaper way to resolve
  the contradiction, and wrong: the encoder cannot produce a zero-length table, so
  permitting one documents a state no writer creates and every reader must then
  handle.
- **Bound only the zero case, leaving the upper side to the overrun check** — the
  overrun check does catch large values today, but only *incidentally*, and only
  because the page happens to be small enough. Naming the real bound (`289`) makes
  the failure report the actual violated constraint instead of a byte count.

## Consequences

- A corrupt block with no impact table now raises `ERRCODE_INDEX_CORRUPTED`
  instead of silently dropping every posting in that block from a ranked result.
- One existing assertion in `sql/69_decode_boundary` had to change its inputs. It
  used `impact_bytes := 65535` purely as padding to trigger the page-overrun
  check; that value is now rejected earlier, so the assertion would have stopped
  testing what it names. It uses `field_rle_bytes` alone for the padding now, with
  every other field well-formed — a stronger version of the same test. The
  behaviour change is pinned by new assertions in the same suite.
- `bm25_decode_impact_table`'s zero branch is now reachable only from a debug
  probe. If that probe is ever removed, the branch should go with it.

## Addendum (2026-10-05, PRs #331-#350)

One level down from this record, the impact table's values remain trusted (#303.E, D5, PR
#344). An entry that understates a real tf or overstates a real doclen, or a table missing a
field the block holds postings in, gives a bound below the block's real scores, and WAND prunes
true top-k documents without error. A load-time cross-check in `wand_cursor_load_block`
(every decoded posting's field present and tf <= its `max_tf`) was built and measured in an
interleaved A/B at -O2 on 120-150k-document corpora whose ranked queries skip blocks, with
identical `bm25_wand_stats` counters and results: about 2.7% on a two-field index. A
vectorized single-field form was inside the noise (+0.4%, +1.2%). Per D5 (land only if free in
the pruning regime) neither landed; the narrower single-field variant was offered and not
taken. Even with the check, a never-decoded block escapes it, because the pivot's
`global_ub` and `next_geq`'s skips read header-only peeks, and the doclen half could be
checked only per scored document. Recorded at `bm25_decode_impact_table` and in ADR 0119.
