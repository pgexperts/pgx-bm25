---
id: 0064
title: The score accessors' hash fallback gates on a live rival, not on a registry count
date: 2026-08-18
status: Accepted
summary: bm25_score/bm25_score_key enable their hash-probe fallback whenever no scan behind the registry head can still emit a row, replacing a raw bm25_scored_scan_count() == 1 test that read a finished-but-still-registered UNION ALL sibling as a rival and returned SQL NULL where a score belonged.
---

# 0064. The score accessors' hash fallback gates on a live rival, not on a registry count

## Context

ADR 0007 built the scored-scan registry and gave `bm25_score(ctid)` /
`bm25_score_key(id)` a two-step resolution: a current-row fast path, then a
hash-probe fallback for decoupled projections (under `ORDER BY ... LIMIT` the
target-list projection and `amgettuple` are decoupled, so a single "last emitted"
slot alone would be stale). That fallback was gated on
`bm25_scored_scan_count() == 1`.

A scan is deregistered in `bm25_endscan`, which for a `UNION ALL` sibling — or any
other `Append` / `SubqueryScan` child — only fires at the query's `ExecutorEnd`,
long after that sibling returned its last row. So from the second branch onward the
raw count is `>= 2` and the fallback was disabled *exactly* when it was needed. The
accessor returned SQL NULL where a number belongs: a wrong answer, and one that
appears only once a second scan exists, i.e. invisible to every single-scan test.

`bm25_sole_scored_scan` — the ambiguity check `bm25_snippet` uses — had already
identified and solved this. Its comment states outright that a raw count
over-reports rivalry, and it instead walks the registry behind the head counting
only a scan that could still produce a FUTURE row. The two score resolvers used
precisely the count that comment rejects. Issue #152 (SCAN-04).

## Decision

Extract that predicate — `rcur < nranked || (wand_capped && !wand_tail_done)` — as
`bm25_scan_can_emit_more`, and add a non-erroring `bm25_sole_live_scored_scan`
returning the head when no scan behind it can still emit. Both score resolvers gate
their fallback on that instead of a count. `bm25_scored_scan_count` had no other
caller and is deleted rather than left as a loaded gun.

The same change bounds `cur_ranked_idx` against `nranked` in both resolvers, and
clears it before the WAND over-pull tail rebuild (SCAN-03/TEXT-02): the rebuild
replaces `ranked`/`scores`/`ranked_keys` with arrays sized to a new survivor count,
which a concurrent DELETE+VACUUM can make *smaller* than the stale index left from
the capped ranking.

## Alternatives considered

- **Deregister a scan as soon as it can emit no more, rather than at `bm25_endscan`** —
  would make the raw count correct and need no predicate. Rejected: the scan opaque
  must stay reachable for projections that follow the last emitted row, which is the
  entire reason the fallback exists. Deregistering early trades this bug for a worse
  one.
- **Probe every registered scan and resolve on a unique hit** — strictly more correct
  than probing the head (see Consequences). Rejected for now as a larger change than
  the defect warrants, and it would build a hash for every registered scan rather
  than one. Recorded as the principled fix if the residual below is revisited.
- **Leave it and document the NULL** — rejected. A silent NULL where a score belongs
  is a wrong answer, not a documented limitation, and the shape that triggers it
  (`UNION ALL` of ranked queries) is ordinary.

## Consequences

The ordinary streaming shape — a decoupled projection with finished siblings
registered — now resolves correctly. Measured against the pre-fix build, a
three-branch `UNION ALL` went from 5 unresolved rows per branch to 0, for both
`bm25_score(ctid)` and `bm25_score_key(id)`.

**The head-attribution residual WIDENED, and that trade was taken deliberately.**
The fallback probes the head's hash without checking that the head owns the probed
row, because nothing in `bm25_score`'s arguments names a query — that identity
exists only for `&@@`, via ADR 0061's `bm25_distance_for_query`. So with two
finished siblings registered and the projection decoupled by a `Sort` above an
`Append`, a row belonging to branch A can resolve through branch B's hash and
receive B's score. This is the same recency-first-head policy ADR 0007 already
documented ("the head wins — a documented, exotic limitation"), and the identical
mis-attribution was reachable before this change through the current-row fast path;
what changed is its reach, from one row per scan to the head's whole ranking.
Adversarial review measured both directions in one experiment: four cells newly
correct, two newly wrong. The trade is net-positive but it is a trade, and the
resolver comment now says so rather than leaving it to be rediscovered.

A scan stopped by `LIMIT` rather than exhaustion is still a live rival, correctly:
`rcur < nranked` holds, so it genuinely could emit more if the executor asked again,
and the AM cannot know the `Append` moved on. The fallback therefore stays disabled
in that shape. `sql/94_scored_scan_state_resolution` pins this alongside the fix so
it is not later "corrected" by weakening the test.

SCAN-03 is not asserted by a regression test. Reaching it needs a concurrent
DELETE+VACUUM between the WAND build and the tail rebuild; review established that a
two-session TAP test *can* schedule it, but that without a sanitizer the
out-of-bounds read almost always yields a non-matching TID and therefore NULL on
both binaries — a value assertion that passes pre-fix, which is worse than no test.
It remains worth a sanitizer-lane TAP test.

## Addendum (2026-10-05)

#301 (ADR 0115): the live-rival count runs over the scans the caller owns and may read. A
refused scan is neither the head nor a rival, because whether it could still emit depends on
what it ranked, and that would let it decide NULL for the caller's own rows.
