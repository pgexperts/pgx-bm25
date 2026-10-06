---
id: 0007
title: Replace the single active-scored-scan slot with an identity-resolving registry
date: 2026-07-12
status: Superseded
superseded_by: 0061,0103
summary: Replace the single backend-global active-scored-scan pointer with an intrusive registry so bm25_score/bm25_score_key resolve by row identity under concurrent scored scans, bm25_snippet fails loud instead of guessing, and bm25_distance/bm25_distance_jsonb keep head-resolving (they double as the per-row &@@ sort-key materializer on every ranked query).
---

# 0007. Replace the single active-scored-scan slot with an identity-resolving registry

## Context

`bm25_score(ctid)`, `bm25_score_key(id)`, `bm25_snippet(...)`, and
`bm25_distance`/`bm25_distance_jsonb` (the `&@@` operator functions) all needed to
answer "what is the BM25 state of the row currently being projected?" without any
argument that names a scan — PostgreSQL gives them only the projected value (a
ctid, a key, a field's text), never a handle to the index scan node that produced
it. Since M2a this was answered by reading one backend-global variable,
`bm25_active_scored_scan`, set to `so` whenever a scored (`&@@`) scan's
`bm25_gettuple` entered scoring mode, cleared in `bm25_endscan`.

That works as long as at most one bm25 scored scan is alive in the backend at a
time. It breaks silently — not with a crash or a NULL, but with a **wrong finite
number** — as soon as two are concurrently alive in the SAME query: a correlated
subquery where both the outer and inner sides run a ranked `&@@` scan, or a
self-join with both arms ranked. Whichever scan's `bm25_gettuple` happened to run
most recently owned the single slot; an accessor evaluated for the OTHER scan's
projected row silently read the wrong scan's ranking array and returned that
scan's score (or, worse, coincidentally found a TID/key match in it and returned a
plausible but wrong value instead of NULL).

An empirical repro nailed down the shape of the problem and ruled out the obvious
fix. Query: an outer `SELECT o.id, bm25_score_key(o.id) FROM t o WHERE o.body @@@
'foo' AND (correlated subquery on i ranking 'bar') ... ORDER BY o.body &@@ 'foo'`.
Each execution of the correlated inner subquery runs its own `bm25_gettuple` calls
AFTER the outer scan has already emitted its current row and is evaluating the
outer's target list (which includes `bm25_score_key(o.id)`) — so at the moment
`bm25_score_key` runs, the INNER scan is legitimately the most-recently-registered
one, even though it is the OUTER's score that must be returned. "Whichever scan
registered most recently wins" is exactly backwards here. Any fix keying
resolution purely on recency (a stack, a single pointer, "last write wins") gets
this case wrong; the only sound signal is which scan's OWN just-emitted row
matches the value being projected.

This is ROADMAP §3 (score-accessor robustness) — the last of the pre-existing
scored-scan fragility items, after the M6 conformance suite (#2) and ahead of the
cassert/ASan CI gate (#1) and the format-upgrade story (#4).

## Decision

Replace the single `bm25_active_scored_scan` pointer with a backend-local
intrusive REGISTRY of active scored scans: a singly-linked list threaded through
`BM25ScanOpaqueData.next_active`, head = most-recently-registered, managed by
`bm25_register_scored_scan`/`bm25_deregister_scored_scan`/`bm25_scored_scan_head`/
`bm25_scored_scan_count` (all in `src/bm25_score.c`). A scan registers itself on
its first scoring `bm25_gettuple` call (idempotently — re-registering moves it to
head rather than duplicating it, so a rescanned inner scan can't appear twice) and
deregisters in `bm25_endscan`.

Give the four accessor families four DIFFERENT resolution strategies against that
registry, because they differ in what identity they can observe:

- **`bm25_score(ctid)` / `bm25_score_key(id)` — resolve by row identity, fully
  concurrent-correct.** Every scan stamps `cur_ranked_idx` (a `BM25_NO_CUR`
  sentinel when unpositioned) on the row `bm25_gettuple` just emitted. The
  resolver walks the registry from the head looking for a scan whose CURRENT row
  matches the argument — an O(1) hot path, correct regardless of which scan
  registered most recently, because it is keyed on the argument's identity, not
  on recency. Recency only breaks the tie in the narrow case where two scans'
  current rows coincidentally share identity (see Consequences). When the
  current-row check fails and exactly one scored scan is registered, a
  single-active-scan fallback covers a DECOUPLED projection (the id read after
  its row was emitted, e.g. under `ORDER BY ... LIMIT`, or in arbitrary order) —
  originally a linear rescan of the ranking array, upgraded to an O(1) lazily-built
  per-scan hash (`score_by_tid`/`score_by_key`, built on first fallback probe,
  freed with `scanctx`) so a full score projection over a large ranking is O(n)
  instead of O(n²). Otherwise: NULL. Never a wrong number.
- **`bm25_snippet` — fail loud.** A snippet has no row-identity argument to
  resolve against at all (it takes the field VALUE, not a ctid/key). Guessing is
  therefore unsound in a way row-identity resolution is not, so
  `bm25_sole_scored_scan()` raises `ERROR (ERRCODE_FEATURE_NOT_SUPPORTED)` when
  another registered scan could still be genuinely concurrent — rather than
  silently attaching the wrong scan's query terms/analyzer to the excerpt.
  "Could still be genuinely concurrent" is deliberately narrower than "is
  registered": a finished sibling scan (e.g. one arm of a `UNION ALL` of ranked
  snippet queries) lingers in the registry until its own `bm25_endscan`, which for
  an `Append`/`SubqueryScan` child fires only at `ExecutorEnd` — long after that
  sibling returned its last row. The ambiguity check therefore walks the registry
  behind the head and errors only when a scan there could still emit a FUTURE row
  (`rcur < nranked`, or WAND-capped with the exhaustive tail not yet rebuilt),
  mirroring `bm25_gettuple`'s own "return false" condition.
- **`bm25_distance` / `bm25_distance_jsonb` (the `&@@` operator) — head-resolve,
  deliberately not fail-loud.** These two are not just a user-facing accessor:
  PostgreSQL invokes them to materialize the `&@@` sort key as a per-row resjunk
  column on EVERY ranked query whenever a secondary `ORDER BY` key or a direct
  `SELECT` of the distance forces materialization (see the prior rank-collapse
  ADR/fix). Making them fail loud on multi-scan ambiguity would break any
  statement with two coexisting ranked scans — including the ordinary,
  overwhelmingly common case of one ranked scan referenced twice (a view, a CTE
  inlined twice). So they keep resolving to `bm25_scored_scan_head()`'s stashed
  `cur_orderby_dist`: correct whenever only one scan is active, and — even when
  two are — row ORDER stays index-correct regardless (the index supplies
  `xs_orderbyvals` directly; the distance functions only affect the
  materialized VALUE, not the row order the executor already has).
- **Teardown via a memory-context callback, not only the explicit deregister
  call.** `bm25_endscan` is skipped entirely on the aborted-portal path
  (`PORTAL_FAILED`): a runtime error in a scored scan's target list can leave a
  portal that a LATER statement's portal teardown `MemoryContextDelete`s —
  including the scan's `scanctx` — without PostgreSQL ever calling `amendscan`.
  A `MemoryContextCallback` armed on `scanctx` in `bm25_beginscan`, and RE-ARMED
  after every `bm25_rescan`'s `MemoryContextReset` (PostgreSQL's per-context
  reset-callback list is one-shot, so an unrenewed callback is silently consumed
  by the mandatory post-beginscan `bm25_rescan` before the scan ever registers),
  deregisters the scan on this path too. `bm25_deregister_scored_scan` is
  idempotent, so the callback and the explicit `bm25_endscan` call cannot
  double-fault each other.

## Alternatives considered

- **Keep the single global slot.** This is the status quo being replaced — it is
  the bug, not an alternative fix, but it is worth stating precisely why "just
  leave it" was rejected: it does not merely degrade gracefully, it returns a
  silently WRONG finite number under concurrency, which is worse than a crash or
  a NULL for a ranking/relevance feature (a legal-search product cannot afford a
  score that looks plausible and is wrong).
- **A save/restore stack (push the previous active scan on entry, pop on exit).**
  This was the design's first instinct: nested scored scans nest lexically, so a
  stack should track "innermost live scan" correctly. The empirical repro above
  disproves it directly — the CORRELATED inner subquery is genuinely the
  innermost/most-recently-active scan by construction, yet it is the OUTER's
  score that the target list needs. A stack (or any purely recency-based
  structure) resolves to the wrong end of the nesting in exactly the case that
  matters. Rejected once the repro made this concrete, in favor of resolving by
  the ARGUMENT's identity against every candidate scan, not by structural
  position.
- **Heavyweight identity threading for every accessor (a resjunk score column,
  or a custom scan node, so even `bm25_snippet`/`bm25_distance` are
  identity-bound like `bm25_score`).** This is the only approach that would make
  ALL FOUR accessor families fully concurrent-correct with no residual
  limitation. Rejected as out of scope for this pass: it requires threading a
  new per-row identity value through the executor's target-list evaluation (or
  building a custom scan node) for functions whose PostgreSQL call signature
  (`bm25_snippet(field, ...)`, the `&@@` operator) has no slot for it today —
  a large, query-surface-touching change, versus a registry + identity
  resolution that fits entirely inside the existing scan opaque and function
  signatures with NO format or SQL-signature change. Deferred, not abandoned;
  flagged as the natural follow-up if the residual limitations below ever prove
  to matter in practice.

## Consequences

**What is now fully fixed.** `bm25_score(ctid)` and `bm25_score_key(id)` are
concurrent-correct for any number of concurrently-active bm25 scored scans in one
query: each resolves strictly by the identity of the row being projected, never by
which scan happened to run most recently. This closes the ROADMAP §3 silent-wrong
bug for the two accessors that carry an identity argument.

**What is now safe instead of wrong (fail loud).** `bm25_snippet` under genuine
concurrency now raises a clear `ERROR` naming the ambiguity, with a hint pointing
at `bm25_score`/`bm25_score_key` as the concurrency-safe alternative, instead of
silently attaching one scan's query terms to another scan's excerpt. This is a
strictly better failure mode (loud, actionable) even though it is not a fix.

**What remains a documented, deliberate limitation.**
- **`bm25_distance`/`bm25_distance_jsonb` still head-resolve under two-or-more
  concurrently-active scored scans** — a non-driving scan's `&@@` projection can
  read the driving (head) scan's distance instead of its own, a wrong finite
  value in that exotic case. This is unchanged from every version of this code
  back to the original single-slot design; it is not a new regression introduced
  here, and it cannot be fail-loud without breaking the ordinary single-scan
  case these functions serve on every ranked query.
- **`bm25_snippet` can still return a wrong excerpt, silently, in one narrow
  shape:** a scan projecting its ONLY/FINAL matched row (`rcur == nranked`) while
  sitting BEHIND a still-live registry head is, by the ambiguity check's own
  definition, no longer able to produce a future row — so it is judged safe and
  the snippet resolves against the head instead of erroring. This is a strict
  improvement over pre-R3 behavior (wrong for every concurrent case, not just
  this one), is not reached by the test suite, and is not fixable without the
  heavyweight identity-threading alternative rejected above.
- **`bm25_snippet` can spuriously ERROR** for a WAND-capped, `LIMIT`-stopped
  sibling scan whose exhaustive tail has not yet been rebuilt, even though that
  sibling will never actually emit another row in practice for that query shape.
  Accepted: the safe direction (a false-positive ERROR) is preferred over a
  false-negative silent wrong answer.
- **`bm25_score`/`bm25_score_key` resolve a CURRENT-ROW identity collision
  recency-first:** if two concurrently-active scans' current rows happen to
  share the same ctid/key at the same instant, the most-recently-registered scan
  wins. This is narrow — far smaller than the pre-R3 bug, which was wrong for
  ANY overlapping domain between two scans, not just a coincident current-row
  match — and is called out explicitly rather than left implicit.

**What this does not touch.** No on-disk format change (still v5) and no SQL
function signature change — the registry, identity resolution, fail-loud check,
and teardown callback are entirely internal to `src/bm25_score.c`/`bm25_scan.c`/
`bm25.h`. `sql/53_concurrent_scored_scans.sql` is the new suite (59 total);
`bench/decoupled_score_hash.sh` records the O(1)-hash-vs-O(n)-linear-scan
improvement for the decoupled fallback path as a report-only benchmark, not a
CI gate.

**Follow-up, if ever needed.** Should the residual `bm25_snippet`/`bm25_distance`
limitations prove to matter in a real workload, the documented escape hatch is
the heavyweight identity-threading alternative above (a resjunk identity column
or custom scan) — a larger, separately-scoped change, not a natural extension of
this one.

## Addendum (2026-07-12)

The cross-key-type `bm25_score_key` decode hazard noted as a deferred follow-up
during this record's final review is now fixed.

`bm25_score_key` decoded its SQL argument with the **active scan's** `key_field`
type (the registry head), not the argument's own type. Two concurrent scored
scans on indexes with *different* key types — e.g. an int-keyed outer projecting
`bm25_score_key(o.id)` while a uuid-keyed inner scan is the registry head —
decoded the int Datum as a uuid pointer (`DatumGetUUIDP`) and dereferenced it,
crashing the backend. Pre-existing: identical to the pre-registry single-slot,
which also decoded by the active scan's type; the registry change neither
introduced nor worsened it.

Fix: decode by the argument's **own** SQL type — `get_fn_expr_argtype()` →
`bm25_key_type_from_oid()` (a shared helper now also used by the build-time
key-type resolver in `bm25_build.c`) — and resolve only against active scans
whose `ranked_key_type` equals the argument's. Matching by type subsumes the
earlier width guard (uuid and text share a 16-byte width, so width alone could
mis-match). A cross-type call is now a clean NULL; a concurrent *same*-type scan
still resolves correctly, so different-key-type scored scans compose. This
threads the argument's TYPE — a targeted slice of the "identity-threading" escape
hatch above — not full per-row identity. Regression: `sql/54_score_key_keytype`
(the crash path → NULL, plus two concurrent int/uuid scored scans each resolving
to their own-type score). No on-disk format change.

## Addendum (2026-09-28)

The Consequences section's "resolve a CURRENT-ROW identity collision
recency-first" clause, and the "never a wrong number" claim it and the SQL
`COMMENT ON FUNCTION bm25_score` made for `bm25_score`/`bm25_score_key`, are
false for one shape: a rescanned correlated inner scan that re-heads the
registry and whose current row coincides with the outer's (#242). That shape
is reachable, not merely theoretical — it is what `sql/53`'s own JOIN checks
hit, hidden by a stats-dependent plan choice. [0103](0103-score-accessor-call-site-attribution.md)
supersedes this clause: each accessor call site now attributes a row to its
own scan by a serial-and-emit-stamp binding, correct whenever the accessor is
projected directly on its own scan's emitted rows. Recency-first resolution,
and the wider set of unbounded wrong answers "never a wrong number" implied,
survive only as 0103's two named residuals (unattributable collisions;
decoupled projections). This is the same partial-supersession shape 0061
already used against this record's distance-family claim.

## Addendum (2026-10-05)

The registry this record introduced is now per user and privilege-gated (#301, ADR 0115).
Registration records the owner's user id and every walker skips scans owned by another id.
`bm25_snippet` keeps the fail-loud error described here, but only over scans the caller owns
and may read.
