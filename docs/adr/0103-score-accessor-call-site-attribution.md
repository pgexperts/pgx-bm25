---
id: 0103
title: Score accessor call sites attribute their own scan instead of always trusting the head-first pick
date: 2026-09-28
status: Accepted
supersedes: 0007
summary: bm25_score/bm25_score_key give each call site a serial-and-emit-stamp binding in fn_extra so a nested or correlated scored scan's projection returns its OWN scan's score rather than the registry head's, bounded to rows projected directly on that call site's own scan; the "never a wrong number" claim is removed and ADR 0007's recency-first current-row collision clause is superseded for this residual.
---

# 0103. Score accessor call sites attribute their own scan instead of always trusting the head-first pick

## Context

`bm25_resolve_score_tid`/`bm25_resolve_score_key` (`src/bm25_score.c`) walked
the backend-local scored-scan registry from the head and returned the first
scan whose *current row* matched the probed ctid/key. The head is the most
recently (re-)registered scan, and a rescanned correlated inner scan
re-registers itself at the head on every outer row. When the inner scan ranks
the same index and its current row coincides with the outer's — its top hit,
which the correlation filter never rejects — the outer row's
`bm25_score_key(o.id)`/`bm25_score(o.ctid)` returned the *inner* scan's score:
a plausible, silently wrong number, not a crash or a NULL (#242, reproduced on
main 3b5ee6f, PG18.6).

`sql/53_concurrent_scored_scans`, the suite meant to guard exactly this, hit
the bug on its own query shape and passed anyway: its JOIN-based checks
happened to plan as a Nested Loop once the table had statistics (autovacuum
on), which does not exercise the collision, and failed (`f`) on a fresh table
with no stats (autovacuum off, no ANALYZE), where the same check planned as a
Hash Join. The suite's green result therefore depended on which plan the
planner happened to pick, not on the accessors being correct.

ADR 0007 documented this same shape as a deliberate, narrow limitation
("`bm25_score`/`bm25_score_key` resolve a CURRENT-ROW identity collision
recency-first") and, along with the SQL `COMMENT ON FUNCTION bm25_score`,
stated the accessors return "never a wrong number." Both claims are false for
this shape and are removed rather than softened, following the same
supersession pattern ADR 0061 used against ADR 0007's distance-family claim
(0007's statement that the residual affects "the materialized VALUE, not the
row order the executor already has", which 0061 called false on measurement).

## Decision

Each scored scan carries a backend-unique `scan_serial`, and the backend
tracks a single monotonic `bm25_emit_seq`, stamped onto a scan
(`last_emit_seq`) every time it emits a row (`bm25_scored_scan_emitted`).
Each textual `bm25_score(...)`/`bm25_score_key(...)` call site — one `FmgrInfo`
per call site in the plan, evaluated once per row its own scan projects —
caches a `BM25ScoreCallSite {bound_serial, last_call_seq}` in `flinfo->fn_extra`.

The answer is the previous algorithm — the head-first current-row walk, then
(only when that finds no match, and only for `bm25_score`'s tid path) the
HOT-root pass that retries the walk against the root of the row's HOT chain,
then the unchanged ADR 0064 hash fallback — **except** when all of the following
hold:

- the call site has a bound scan (`bound_serial != 0`) and that scan is still
  registered;
- the bound scan's current row matches the probe;
- the head-first walk picked a *different* scan;
- **both** the bound scan and the head-first pick have emitted a row since
  this call site's last call (`last_emit_seq > last_call_seq` for both).

Then the bound scan's score is returned instead of the walk's pick. Requiring
the walk's pick to also be fresh (not just the bound scan) is what keeps a
hash join's finished build side — whose current row is its stale last row,
projected out of the hash table after the probe side has moved on — from
being overridden by a probe-side binding that would otherwise look fresh: on
the row equal to the build side's last row, the walk's head-first pick is the
correct one, not the bound scan.

The binding updates after the answer is chosen: cleared when the previously
bound scan has left the registry, replaced whenever exactly one registered
scan's current row matches the probe, and `last_call_seq` recorded on every
call. A caller invoked with no `FmgrInfo` resolves by the plain walk alone.

**The contract is bounded, and is now stated as such in code and SQL
comments.** Projected directly on its own scan's emitted rows, the accessor
returns that scan's score, even when another live scan's current row happens
to coincide with the probe. Two residual shapes remain, where the accessor
can still return another concurrent scan's score:

- **unattributable collisions** — the call site never saw a probe that
  matched exactly one scan alone (e.g. a join on the key, with the accessor
  in the top-level projection above the join), so it has no binding and falls
  back to the walk's head-first pick;
- **decoupled projections** — the row being projected is not its scan's
  current row: a PL/pgSQL `FOR` loop prefetching rows ahead of the loop body,
  a `Sort`/`Materialize` above the scan, or a cursor. Here a binding learned
  from an earlier coincidental match can be the wrong scan, and the answer can
  differ from the plain walk's in either direction. A constructed
  PL/pgSQL-prefetch shape is worse than the plain walk and is pinned in
  `sql/53` as a documented residual, not silently fixed.

`sql/53` was made plan-stable (`ANALYZE`, materialized single-scan CTAS
baselines) and extended: per-row nested assertions for both accessors (which
fail on main with autovacuum both on and off), the two-subquery JOIN, `UNION
ALL`, and LIMIT-branch shapes, plus the residual shapes pinned with their
values labelled NOT correct. A new CI job runs the SQL suites with autovacuum
off so stats-dependent passes surface instead of hiding behind a Nested Loop
plan.

## Alternatives considered

- **Planner-support binding** (a support function rewriting the call to carry
  its scan node's identity). Infeasible: a support function sees the
  expression tree, not the chosen plan, so it cannot know which index scan
  node will execute the call, and the bm25 AM scan itself never learns its own
  range-table index to thread back out. Rejected as unreachable, not merely
  heavyweight.
- **Return NULL on ambiguity**, mirroring `bm25_snippet`'s fail-loud strategy.
  Tried and rejected on measurement: it turned some *correct* answers into
  NULL (ordinary join shapes that were never actually ambiguous), and combined
  with an "exhausted" flag it turned some correct answers into *wrong* ones
  (a `UNION ALL` with a `LIMIT` branch, where a finished sibling's exhaustion
  state produced a wrong non-NULL result instead of the right one). Fail-loud
  works for `bm25_snippet` because it has no row-identity argument to resolve
  against at all; `bm25_score`/`bm25_score_key` do, and degrading a previously
  correct answer to protect against a narrower set of wrong ones was a worse
  trade.
- **Fresh-bound-only, without requiring the head-first pick to also be
  fresh.** The first cut of this design bound to a scan whenever it alone was
  fresh, regardless of the walk's pick. Measurement found this made a hash
  join *worse* than the unmodified walk (the stale build-side case above), so
  the freshness requirement was extended to both sides.
- **A complete fix — scan identity threaded into the accessor itself** (a new
  function signature, or an executor hook exposing the calling scan node).
  Would remove both residuals above outright. Deferred: it is a
  SQL-signature-breaking or executor-touching change, out of proportion to
  the defect, and the same class of "heavyweight identity threading" ADR 0007
  and ADR 0061 already declined for the sibling accessors.

## Consequences

- The reported #242 shape (correlated/nested scored scans over the same
  index, projecting directly on their own scan's rows) is fixed. The
  differential harness compared 1,096 values against pre-fix main: 12
  improved, 0 worse. The constructed PL/pgSQL-prefetch shape noted above is a
  separate case, worse than the plain walk, pinned as a residual.
- The "never a wrong number" claim is removed from the code comments and the
  SQL `COMMENT ON FUNCTION bm25_score`/`bm25_score_key`, replaced by the
  bounded contract and its two named residual shapes.
- `sql/53` is now plan-stable and discriminates the fix from a lucky plan
  choice; the new autovacuum-off CI job exists so a future regression in this
  area cannot hide behind autovacuum's default Nested Loop plan the way #242
  did.
- Cost is one `fn_extra` struct (two `uint64`s) per accessor call site,
  populated lazily, plus a backend-wide `uint64` emit counter that cannot wrap
  in a backend's lifetime.
- Both residuals (unattributable collisions; decoupled projections) are
  unfixed and are expected to stay that way short of the deferred
  scan-identity alternative above. They are now discoverable in code and in
  `sql/53`'s pinned, labelled assertions rather than only in prose.
- This record supersedes ADR 0007 specifically for its current-row-collision
  clause ("resolve a CURRENT-ROW identity collision recency-first") and its
  "never a wrong number" claim, following the partial-supersession precedent
  ADR 0061 already set against that same record's distance-family claim.
  ADR 0007's frontmatter is already `status: Superseded` / `superseded_by:
  0061`; `0103` is appended to that list rather than touching `status` again.
  ADR 0007's body is not rewritten; its Consequences bullet about
  recency-first resolution is addressed by an `## Addendum (2026-09-28)`
  pointing here, the same shape ADR 0061 itself used against it.

## Addendum (2026-09-29)

`last_emit_seq` now has a second consumer. [0104](0104-distance-among-same-query-siblings-resolves-by-emit-recency.md)
uses it as the tiebreak in `bm25_distance_for_query` among positioned scans ranking
the byte-identical query (issue #252, a Merge Append over per-child scans). It reads
the stamp only; the call-site `fn_extra` binding described above is not involved,
and `bm25_score`/`bm25_score_key` are unchanged.

## Addendum (2026-09-29, query-qualified overloads)

Both residuals named under Decision (unattributable collisions, decoupled
projections) can now be avoided by naming the scan.
[0105](0105-query-qualified-score-accessors-name-the-scan.md) adds
`bm25_score(tid, query [, regclass])` and `bm25_score_key(key, query)`, which
resolve by the ranked query against each candidate scan's whole ranking and
return NULL when two candidates disagree (issue #253). The alternative deferred
above as "scan identity threaded into the accessor itself" was realized as
additive overloads rather than a changed signature. This record is amended, not
superseded: `bm25_score(tid)` and `bm25_score_key(key)` are unchanged and still
follow the bounded contract above, with both residuals, so calls that stay on the
one-argument forms keep them.

One side effect reaches the one-argument forms. The WAND over-pull rebuild now
re-positions the scan on the last emitted row, so in the capped exactly-`wand_top_k`
shape a one-argument `bm25_score` projected above a `Sort` answers where it used to
return NULL (details in 0105). Both shapes are among the decoupled-projection
residuals recorded here.

## Addendum (2026-10-05)

#301 (ADR 0115): the call site's serial-and-emit-stamp binding resolves only among the scans
the caller owns and may read. Each row-addressed call site's `fn_extra` also carries a per-site
privilege cache keyed on `(index oid, user id)`. For a PL/pgSQL simple expression `fn_extra`
can live for the transaction, so a same-user REVOKE mid-transaction may go unseen at such a
call site until it ends. The user id in the key is what keeps `SET ROLE` and `SECURITY DEFINER`
switches correct.

## Addendum (2026-10-05, PRs #331-#350)

Residual (#314 SCORE-07, D20; recorded at `bm25_resolve_score_tid`, PR #349). When
`bm25_score(ctid)` is projected out of scan order (a Sort or Materialize above the scan, a
cursor, a PL/pgSQL FOR loop), the exact compare misses on essentially every row, so the
HOT-root matcher, which reads a heap page, runs for every row and every registered scan before
the per-scan hash that actually answers. Measured: on a 20,000-row table with every row
matching, projecting `bm25_score(ctid)` through a Sort showed 20,538 shared buffer hits against
539 without it (EXPLAIN BUFFERS). The answer is correct either way. Trying the hash first would
reorder matchers inside the call-site binding and `nmatch` logic this record, ADR 0105 and
#301 settled, which a performance item does not justify.
