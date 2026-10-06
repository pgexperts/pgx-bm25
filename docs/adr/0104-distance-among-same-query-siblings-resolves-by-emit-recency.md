---
id: 0104
title: Distance projection among same-query sibling scans resolves by emit recency
date: 2026-09-29
status: Accepted
summary: bm25_distance_for_query, after matching ORDER BY RHS, picks the positioned candidate with the highest last_emit_seq instead of the first from the registry head, so each child of a Merge Append over a partitioned or inheritance parent is projected with its own row's distance; correct only when the projection runs right after its own scan's emit.
---

# 0104. Distance projection among same-query sibling scans resolves by emit recency

## Context

[0061](0061-distance-resolves-by-query-identity.md) made `bm25_distance` /
`bm25_distance_jsonb` resolve the projected `&@@` by matching the expression's RHS
against each scoring scan's stashed ORDER BY RHS, and named one residual as
unfixable within the operator signature: two live scans ranking the byte-identical
query "resolve recency-first, exactly as before this change." Issue #252 showed the
residual is not exotic. A ranked `ORDER BY body &@@ q` on a partitioned or
inheritance parent plans as a Merge Append over one bm25 index scan per child, and
every child ranks the same query. The resolver took the first RHS match from the
registry head, which is the most recently REGISTERED child, so every child but
the last to load projected that child's current distance. Merge Append compares
children by that projected value, saw a constant for the others, and drained them in
one block: rows out of score order, and `LIMIT k` returned the wrong top-k, with no
error (reproduced on main 2431b2e, PG 18.6, per the issue). `ORDER BY` with no
score accessor in the select list is enough to trigger it.

The rejected alternative in 0061, the re-head-the-registry-on-emit option, is what this
record adopts in a narrower form, so the difference needs stating: 0061 rejected
recency because in the correlated shape of #138 the inner scan is the most recent
emitter yet the outer's distance is wanted. Here recency is applied only AFTER the
RHS match, among candidates that already rank the same query.

## Decision

In `bm25_distance_for_query` (`src/bm25_score.c`), among the registry entries that
are scoring, positioned, in the same operator family and RHS-byte-equal, return the
`cur_orderby_dist` of the one with the highest `last_emit_seq`, not the first found
walking from the head. `last_emit_seq` is the per-scan emit stamp added by #242
([0103](0103-score-accessor-call-site-attribution.md)), drawn from a backend-wide
counter that is pre-incremented, so two positioned scans never tie and a sole
candidate resolves exactly as before. No registry, operator-signature or on-disk
change.

Why it is right where it is right: the `&@@` resjunk is projected on the Index
Scan's own target list immediately after that scan's `gettuple`, with no other scan
emitting in between, so the most recent emitter is the scan that owns the row. For
the same query nested inside itself the two rules agree, since a correlated inner
re-registers on each rescan and then emits.

**The contract is bounded.** It is correct when `&@@` is projected on the scan's own
target list right after its emit, which covers the children of an Append or Merge
Append. Residuals, all unfixed:

- **Decoupled projections.** A projection not evaluated right after its own scan's
  emit gets the newest emitter, which need not be the owner: a join target list
  evaluating two same-query scans' `&@@` after both have emitted, a same-query
  subquery run from the target list ahead of the resjunk, a `Sort`/`Materialize`
  between the scan and the projection, a cursor.
- **Same-RHS correlated shapes**, where the outer and inner rank the byte-identical
  query, the residual 0061 already named. Recency is right for the inner's own
  `&@@` and still wrong for the outer's.
- **An inheritance child with no bm25 index.** Under the Merge Append it is a Sort
  over a Seq Scan, not an index scan, so no scan owns its rows. Observed on the
  fixed code (2026-09-29, PG 18.6, an unindexed child on each side of two indexed
  ones): rows of an unindexed child whose Sort drained before any sibling index scan
  had emitted got `+infinity`; rows of one whose Sort drained after got the newest
  emitter's current distance, a single constant, so they sorted as if tied at a
  sibling's score. By the code the old head-first rule likewise resolved such rows
  to a sibling scan (not A/B'd), so this predates the change; `sql/112` does not
  cover it.

When no scan matches, the function still degrades to `+infinity`, as in 0061.

`sql/112_merge_append_distance` covers declarative partitioning and `INHERITS`
against single-child baselines (each child ranked alone, so no sibling to be
ambiguous with): the merged output must project each row's baseline distance, be
non-decreasing in distance, and its `LIMIT 5` must equal the baseline's top 5. It
also seals every index and `ANALYZE`s every table so neither the pending list nor
autovacuum decides the plan. `sql/111` now drops its tables and the extension at
its end, since it was the only suite that left them behind and 112 runs after it.

## Alternatives considered

The choice among the four below was the maintainer's, made after they were laid
out. The reasons given for the first, third and fourth are the ones stated at that
time; the second has none on record.

- **Call-site binding through `fn_extra`, as [0103](0103-score-accessor-call-site-attribution.md)
  does for `bm25_score`.** Not chosen: emit recency is the minimal change within
  the existing bounded contract, while a binding is a larger change that would
  inherit 0103's attribution edge cases. One further constraint visible in the
  text: 0103's binding is learned when exactly one registered scan's current row
  matches the probe, and the distance operator is handed `(document value, query)`
  with no ctid or key, so there is no per-row probe to learn from (0061). Whether a
  distance binding could be built another way was not worked out.
- **Carry the heap relation OID or a per-scan serial through the resolver and match
  on it** (the first direction in issue #252). Not chosen; no record of why it
  was not pursued.
  > Rationale not recovered from project sources.
- **Fail loudly on two live positioned scans with an identical RHS**, as
  `bm25_snippet` does. Not chosen: it would make ranked queries on partitioned
  parents unsupported. The distance functions are the resjunk projection
  of every ranked query, and 0061 records that they "therefore must not error"; the
  in-code comment on `bm25_distance_for_query` repeats it. With no matching scan it
  degrades to `+infinity` instead.
- **Document ranked `ORDER BY` on a partitioned parent as unsupported and steer
  users to per-partition queries.** Not chosen; documenting alone leaves the
  wrong top-k, silently, in an ordinary schema shape.

## Consequences

- The reported shape is fixed for declarative partitioning and inheritance parents
  whose children all carry a bm25 index: Merge Append input is in score order and
  `LIMIT k` returns the true top-k under each child's own scores.
- 0061's "byte-identical RHS resolves recency-first" residual is now partly
  changed: it is recency by EMIT rather than by registration, which is right for
  scan-node projections. The rest of that residual stands and is listed above. 0061
  stays Accepted; its addendum points here.
- `last_emit_seq` now has two consumers, the `bm25_score` call-site freshness check
  (0103) and this tiebreak. Anything that changes when the stamp is written (today
  every scored emit, `bm25_scored_scan_emitted`) changes both.
- Cross-partition scores remain only approximately comparable, because IDF is
  partition-local (THEORY.md, "Cross-partition global IDF is unsolved by design").
  This record makes the order correct under each child's own scores, not comparable
  across children.
- Cost is one comparison per RHS-matching candidate in the existing registry walk.
