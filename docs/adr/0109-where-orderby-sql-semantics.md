---
id: 0109
title: An index scan returns the SQL answer, every WHERE @@@ key applies and ORDER BY &@@ only orders
date: 2026-10-05
status: Accepted
supersedes: 0021
summary: bm25_rescan applies every WHERE @@@ key (intersection, an invalid RHS raises, a NULL key gives no rows), orders the result by the &@@ distance with the WHERE rows the ORDER BY query does not match last at +Infinity, and keeps the error for more than one ORDER BY key.
---

# 0109. An index scan returns the SQL answer, every WHERE @@@ key applies and ORDER BY &@@ only orders

## Context

ADR 0021 (#39) refused a scan with more than one `@@@` key or more than one `&@@` key,
because `bm25_rescan` read only `keyData[0]` and `orderByData[0]` while
`bm25_gettuple` sets `xs_recheck = false` and the planner removes an index clause from
the qual list: a key the AM does not apply is applied by nothing. It left one case
alone on purpose, "ORDER BY wins over WHERE": with both keys present the scan took its
whole query from `orderByData[0]` and never parsed or evaluated `keyData[0]`. 0021
recorded the consequence and backed out an agreement check that broke `sql/38_phrase`,
which depends on a bare `WHERE` with a field-scoped `ORDER BY`.

The 2026-10-04 review (#290) reproduced it as a plan-dependent wrong answer on a
default plan. `WHERE body @@@ 'cat' ORDER BY body &@@ 'dog'` returned {2,3} through
the index and {1,3} through a seqscan, so the answer depended on whether the planner
chose the index. The WHERE key's parse errors were skipped as well, and a second WHERE
key was an outright error.

The user decided on 2026-10-05 that the scan
returns what SQL says: the rows satisfying the WHERE clause, in `&@@` order. Redefining
a shipped contract is the design decision 0021 declined to make as a bug fix; this
record is that decision. It replaces 0021 as a whole. The one rule of 0021 that
survives, an error for more than one ORDER BY key, is restated below rather than left
in a record that is otherwise reversed.

## Decision

**Every WHERE key applies.** `bm25_rescan` parses every `@@@` key. An invalid RHS (an
unknown field, a malformed phrase, a `must_not` phrase leaf) raises there, as it would
as the scan's own query. A NULL in any WHERE key means no rows, because `bm25_match`
is strict and the WHERE clause is the AND of its keys. A NULL `&@@` key keeps ADR
0015's behaviour: the matching rows qualify and sort last.

**Several WHERE keys intersect, and `sk_attno` is ignored.** A bare RHS searches every
field of the index, so n keys are an AND of n all-fields matches, the same rule as for
the scan's own query. Several WHERE keys with no ORDER BY return their intersection in
TID order.

**The result is the WHERE set, ordered by the ORDER BY query.** Let W be the
intersection of the WHERE sets and M the ORDER BY query's membership set with its
ranking. The scan emits the ranked rows that are in W, in rank order, then W minus M at
distance `+Infinity` (the value `&@@` returns for any row off the index), with NULL from
the score accessors. The remainder is W minus the ORDER BY query's membership set from
the same snapshot, not W minus the ranking. An ORDER BY never removes a row, so plain
intersection of W and M is not the answer.

**A WHERE key byte-identical to the ORDER BY key is skipped, but its condition is
kept.** Its set is M, so `bm25_rescan` does not parse it again and the common
`WHERE col @@@ q ORDER BY col &@@ q` keeps its plan, cost and WAND path. When another,
different WHERE key sends the scan to the filtered build, `so->where_has_orderby`
makes the build intersect M into W, so `WHERE body @@@ 'dog' AND body @@@ 'cat' ORDER
BY body &@@ 'dog'` is the dog-and-cat rows with an empty unmatched tail. The first
version dropped that condition and returned the cat rows without dog; the first round
of review found it.

**A WHERE key with a different RHS forces the exhaustive scorer.** The filtered build
(`bm25_scan_build_filtered`) is always exhaustive: every set, the ORDER BY query's and
each WHERE key's, comes from the exhaustive scorer under one `bm25_scan_snapshot`,
inside one attempt of the retry wrapper, so a concurrent seal or merge cannot move a
document between two sets. It is never capped and never reaches WAND or the over-pull
tail rebuild (ADR 0108). A capped ranking is not M, so "not in M" and "not ranked"
would stop being the same test.

**Memory.** Each exhaustive build charges its own `bm25_native.max_match_memory`
budget (ADR 0047) and frees its scratch. The builds run one after another and each
WHERE key's ranking is released once its TID set is extracted, so the peak is about
twice the setting however many WHERE keys there are: the ORDER BY ranking held across
the loop plus the one WHERE build in flight, plus the TID sets. An earlier commit
message and the README said (n+1) times; the builds are sequential and the code
comment and README now say twice.

**More than one ORDER BY key is still an error**, `ERRCODE_FEATURE_NOT_SUPPORTED`, as
in 0021. Its hint no longer recommends `bm25_match`, whose off-index semantics differ
(ADR 0078, and #298, recorded in 0004 and 0005).

**The field-scope idiom moves to the WHERE.** The documented way to rank within one
field of a multi-field index was a bare WHERE with a scoped ORDER BY, which depended on
ORDER-BY-wins. It is now `WHERE title @@@ 'body:"a b"' ORDER BY title &@@ 'body:"a b"'`.
With the scope in the ORDER BY alone, the unscoped WHERE rows the scoped query does not
match follow the ranked ones at `+Infinity`. `sql/38_phrase` is rewritten to scope in
the WHERE and pins the old spelling's new output; `sql/60_null_scankey` and
`sql/64_multi_scankey` are updated; the README documents the semantics, the cost and
the change.

## Alternatives considered

- **Keep ORDER-BY-wins and document it (0021's position).** It returns rows disjoint
  from the SQL answer and different from a seqscan's, so the same statement changes
  its result with the plan. The user decided against it.
- **Require the ORDER BY query to be a field-scoped form of the WHERE query** (parse
  both and compare post-field-split terms), the other closing option 0021 listed. It
  keeps ORDER-BY-wins for the shapes it admits and refuses the rest. Not taken; the
  user chose to evaluate the WHERE.
- **Plain intersection of W and M.** It drops exactly the WHERE rows the ORDER BY query
  does not match, which a seqscan returns.
- **Remainder = W minus the ranking.** Correct only for an uncapped ranking. A WAND
  ranking is capped, so rows past `wand_top_k` would be placed at `+Infinity` after
  scoring finitely. Using M, the exhaustive membership set, makes the test the same
  for a capped and an uncapped scan. The user decided on membership.
- **`xs_recheck = true` when there are several keys.** Still rejected for 0021's
  reason: the recheck defers to `bm25_match`, which re-tokenizes with the english
  default analyzer and drops correct rows on any other.
- **WAND with a post-filter for a differing WHERE key.** Not attempted. The user
  decided that version one evaluates every differing key exhaustively.

## Consequences

- A query that returned the ORDER BY query's rows now returns the WHERE query's rows.
  That is a behaviour change for anyone relying on ORDER-BY-wins, and the field-scope
  idiom needs its scope in the WHERE; the README carries both as release notes.
- A scan whose WHERE and ORDER BY queries differ scores every matching document of each
  query, with no WAND, so it costs more on common terms than the single-query form.
- A remainder row has no ranked key: `bm25_score_key` for it answers NULL, as a ctid
  probe does. `bm25_snippet` on a remainder row has no test.
- Each WHERE build resolves the KEYMAP and discards it. Cost only, filed with the
  scan constant-factor work (#314).
- Pinned by `sql/129_where_orderby_sql_semantics`, which compares the index plan
  with a seqscan ground truth, including the shape from the first review round.
- 0021's status is `Superseded`. Its refusal of more than one ORDER BY key is carried
  here unchanged.

## Addendum (2026-10-05, PRs #331-#350)

Residual (#314 SCAN-11, D20; recorded at the delegated membership path in `bm25_scan.c`, PR
#349). Delegated `@@@` membership, which this record's #290 change extended to any scan with
several WHERE keys, pays for the whole ranked build (key discovery, the keymap fill, the score
sort) and keeps only the TIDs. It is a constant factor over walks the ranking does anyway, not a
defect, and it is unmeasured. A membership-only build would have to keep the same retry wrapper
`bm25_scan_build_ranking` uses (ADR 0121).
