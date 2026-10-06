---
id: 0049
title: max_num_chars counts characters, not bytes
date: 2026-08-10
status: Accepted
summary: bm25_snippet's window budget is measured in characters of the original field text, resolved by one forward pass over the field rather than a per-comparison rescan, so the same budget buys the same excerpt length in every language.
---

# 0049. max_num_chars counts characters, not bytes

## Context

`bm25_snippet`'s budget parameter is named `max_num_chars`, and `ARCHITECTURE.md`
described the function as selecting "the densest ≤`max_num_chars` window". Every
comparison that implemented it derived from BYTE offsets: `SnippetHit.start`/`.end`
are `src_off` / `src_off + src_len` from the analyzer, which index into
`VARDATA_ANY(field)`, and both the two-pointer window loop and the surrounding-context
slack compared those byte deltas directly against `max_num_chars`.

The name, the documentation, and the implementation disagreed, and the disagreement was
per-language. In UTF-8 a Japanese corpus is 3 bytes per character and a Greek or Russian
one is 2, so the default budget of 300 bought roughly 100 and 150 characters
respectively — a third to a half of what the parameter promised. A caller sizing a UI
column by the parameter got a different answer per language for the same setting.

Review finding #67.9 offered two fixes: rename the parameter to `max_num_bytes` and
document byte semantics (the smaller change, preserving O(1) comparisons), or walk with
`pg_mblen` as the boundary snapping already did. The repo owner's binding decision was
chars-not-bytes.

## Decision

`max_num_chars` counts characters of the ORIGINAL field text. Not bytes, and not output
length — an escaped `&` spends one unit of budget and emits five bytes, which is what
keeps this decision independent of [0048](0048-snippet-escapes-by-default.md).

Three implementation constraints shaped it:

- **One pass, not one per comparison.** `snippet_hit_charpos` resolves every hit edge's
  character offset in a single forward walk over the field, exploiting the fact that hit
  edges are non-decreasing (hits come from non-overlapping source runs in token order,
  and consecutive identical spans from a compound splitter are collapsed before this
  point). `SnippetHit` carries both denominations because the budget arithmetic needs
  characters while every read of the text needs bytes.

- **Forward walks only.** `pg_mblen` reads a LEAD byte and reports that character's
  length, so it cannot step backward, and no general server encoding supports a reliable
  backward step. Converting a character allowance back into a byte bound is therefore
  structured as a forward walk from a known boundary: the left bound restarts from byte
  0, the right bound continues from the core span's own end, whose character offset was
  already paired with it by the same pass. (The continuation-byte nudges in
  `snap_left`/`snap_right` are a different, UTF-8-specific mechanism that only adjusts an
  already-computed edge.)

- **The right-hand target is summed in int64 and clamped to the field's byte length.**
  This is hardening, not a fix: adversarial review established that the int32 sum cannot
  actually overflow, because a hit is a word run of at least one character (so
  `core_span >= 1`, bounding `right_room` at 1073741823) and a varlena payload is under
  `MaxAllocSize` (so `cend <= 1073741819`) — 2147483642 at worst, five short of `INT_MAX`.
  It is kept because that proof is a conjunction of three facts nothing states together —
  the minimum span of a hit, the maximum size of a `text` datum, and the halving of slack
  — and widening any one of them silently puts the sum back in range of a wrap that would
  hand the walk a target LEFT of its anchor, producing no trailing context rather than
  all of it. Summing wide keeps the safety local to the line instead of distributed over
  three other files. The original commit message claimed the wrap was reachable; it is
  not, and the corrected reasoning is recorded here rather than left to mislead.

## Alternatives considered

- **Rename to `max_num_bytes`** (the reviewer's cheaper suggestion) — rejected by the
  owner. It is the honest minimal fix and it keeps the comparisons O(1), but it pushes
  per-language arithmetic onto every caller who wants a predictable excerpt length.
- **`pg_mbstrlen_with_len` per comparison** — rejected. That is an O(flen) rescan inside
  an O(nhits²) window loop.
- **Precompute a full character→byte index for the field** — rejected. Four bytes per
  character of field text, allocated per projected row, to avoid one linear walk that is
  already dwarfed by the two `bm25_analyze` calls the function makes.
- **Budget the OUTPUT length instead of the source** — rejected. It would make an
  excerpt's source content depend on how many metacharacters it happened to contain, so
  the same query over the same row would show less text merely because the passage had
  ampersands in it.

## Consequences

- **Behaviour change on non-ASCII corpora, in the direction of more text**: at the same
  budget an excerpt grows by up to the corpus's bytes-per-character factor (4× for
  4-byte UTF-8). ASCII output is byte-identical, because for ASCII the two units
  coincide.
- **Cost is one extra forward pass over the field text per call**, plus two `int`s per
  hit. Both are negligible against the two `bm25_analyze` calls already in the path.
- **No existing suite moved, and that was the finding underneath the finding.**
  `sql/39_snippet.sql` Part 4 runs a CJK + emoji corpus under six budgets but asserts
  only a UTF-8 round-trip boolean; `sql/82_encoding_aware_tokens.sql` Part 4 asserts a
  `LIKE`. Neither pins a length, so neither could ever have detected the unit being
  wrong. `sql/84_...` Part 4 is the first assertion in the tree that distinguishes a
  character budget from a byte one — it checks both that the window is ≤ 24 characters
  and that it exceeds 24 BYTES, so passing it requires the unit to have actually changed
  rather than merely a number to have grown.

## Addendum (2026-10-05, PRs #331-#350)

- **Unused budget carries to the other side** (#308 TEXT-06, PR #337). The window was split
  evenly around the hit, so a hit near a field edge produced an excerpt about half the
  requested length, with a spurious ellipsis. Each side's room is now measured in
  characters, each side takes `min(room, half)`, and the shortfall carries to the other side
  before the byte conversion. Budgets stay in characters, as this record decided.
- **One bounded character-length primitive** (D18, #308 TEXT-08). `pg_mblen`, which core
  marked deprecated in 17.8 and 18.2, is replaced in the snippet and the analyzer by
  `bm25_mblen_bounded(p, remaining)` over `pg_encoding_mblen`, which reads only the lead
  byte in every server encoding. A length that overruns the owned bytes (a truncated trailing
  sequence, possible only from corrupt input) raises core's "invalid byte sequence" error
  instead of being clamped silently. Rejected: core's bounded `pg_mblen_range`. It is absent
  before those minors and the floor is 17.0, so using it needs a minor-version `#if`, under
  which a binary built against 17.8+ headers fails to load on 17.0-17.7.
- The snippet now highlights any query tree (ADR 0123).
