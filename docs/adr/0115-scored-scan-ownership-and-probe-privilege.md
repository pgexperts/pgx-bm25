---
id: 0115
title: The scored-scan registry is per user, and a scan the caller may not read is invisible to every accessor
date: 2026-10-05
status: Accepted
summary: Registration records GetUserId(); every registry walker skips scans owned by another user id; the row-addressed accessors also skip scans whose heap the caller cannot SELECT (table, partition ancestor, or every indexed column) or where RLS applies, so a refused scan is invisible rather than answering NULL.
---

# 0115. The scored-scan registry is per user, and a scan the caller may not read is invisible to every accessor

## Context

The active scored-scan registry (ADR 0007, ADR 0103, ADR 0105) is per backend, and
`bm25_score`, `bm25_score_key` (all forms), `bm25_snippet` and the `&@@` distance are
executable by PUBLIC. Every walker matched only on query bytes and row identity, so code
running as one role could probe a scan another role had started in the same backend.

Two shapes reach it (issue #301). A `SECURITY DEFINER` `LANGUAGE sql` set-returning
function in the caller's target list keeps the definer's executor and scan open between
rows, because ProjectSet calls it once per row. A definer-opened refcursor does the same.
The rows the definer filters out above the index scan are still in its ranking, and the
query-qualified accessors (ADR 0105) probe each candidate's whole ranking, so the caller
got an exact per-document term and score oracle for rows it cannot SELECT. A third shape
needs no missing privilege: a refcursor over a row-level-security table where the caller
has SELECT, opened by a definer whose plan bypasses the policy.

`bm25_beginscan` does not run at ExecutorStart. PostgreSQL begins an index scan lazily, at
the first tuple fetch. A refcursor therefore registers its scan at the caller's first FETCH,
under the caller's user id, so ownership alone cannot close that shape. Not vulnerable, from
the issue's testing: a plpgsql `RETURN QUERY` definer (the executor has finished before the
caller's target list runs), the same SQL function in FROM (the function scan materializes
first), and the caller's own ranked query on an RLS table or a `security_barrier` view
(`@@@` is not leakproof, so the plan is Seq Scan plus Sort and no bm25 scan registers).

The user decided on 2026-10-05 (SECURITY DEFINER wrappers in scope) the checks below.

## Decision

**Ownership.** `bm25_register_scored_scan` records `GetUserId()` in the scan's
`owner_userid`, at the point `bm25_gettuple` registers it, which is where that id is the one
running the query. A rescan re-registers, so the owner follows the latest (re)load. Every
registry walker skips scans whose owner is not the current user id, as if they were not
registered; only registration and deregistration walk the raw list. That closes the SQL
function shape: each call into the definer runs under the definer's user id.

**Privilege for the row-addressed accessors.** `bm25_score`, `bm25_score_key` and
`bm25_snippet` also require that the caller may read the scan's heap
(`bm25_probe_acl_compute`):
- no row-level security in force for the caller on the heap or on any partition ancestor
  (`check_enable_rls` returning `RLS_ENABLED` refuses, with `noError`, so `row_security =
  off` still refuses a caller the policy applies to); and
- `SELECT` on the heap, or on a partition ancestor, or on every heap column the index reads
  (key and INCLUDE columns and those its expressions and predicate reference; a whole-row
  reference refuses).

Only partition ancestors count. A partition has exactly its parent's columns, so a grant on
a partitioned table reads every column of every partition. A legacy `INHERITS` parent does
not count: a child can add columns the parent's grant does not reach, so accepting it let a
role holding SELECT on the parent probe the child's own indexed column.

**A refused scan is invisible, not NULL.** Every probe walker tests a scan's privilege
before it looks at the scan's ranking or current row, and skips a refused scan as
candidate, as match and as rival in the sole-live-scan counts. The first version tested
after a lookup hit and answered NULL, which made the NULL itself the signal: with the
caller's own scan ranking the same query, NULL versus a score for each of its rows
reported whether the refused ranking held that row. The sole-live-scan fallback had the
same flaw, since counting a refused scan as a rival made whether it could still emit,
which depends on what it ranked, decide NULL for the caller's own rows. The one dependence
that remains is `bm25_snippet`'s error when the caller has no visible scan but owns a
refused one; it depends only on that scan existing, which the caller caused.

**What a refusal returns.** `bm25_score` and `bm25_score_key` return NULL; `bm25_snippet`
keeps its fail-loud error (ADR 0007). An RLS refusal drops results even for a policy that
folds to constant true, the one shape where the caller's own ranked query reaches the
index; the README documents the cost.

**The distance gets the ownership filter only.** `bm25_distance` and
`bm25_distance_jsonb` apply no ACL or RLS check. `&@@` is the ORDER BY key that Merge
Append merges partitions on, and a role ranking through a view it was granted, without
SELECT on the base table, runs the scan under its own user id and needs a real number. A
refusal would degrade every such row to `+Infinity` and silently unorder the result. What
the check would protect is small: the value is the score of the row the owner's scan
emitted last, and the caller cannot choose that row.

**The check is cached per call site** in `fn_extra`, keyed on `(index oid, user id)`. The
index determines the heap and the column half of the check depends on the index. The user
id in the key keeps the cache correct across `SET ROLE` and `SECURITY DEFINER` switches
inside one query. It does not guarantee that a GRANT, REVOKE or `row_security` change is
seen at the next statement: `fn_extra` lives as long as the expression state, which for a
PL/pgSQL simple expression can be the whole transaction, so a same-user REVOKE
mid-transaction can go unseen there.

## Alternatives considered

- **NULL on a refused hit.** The first version; it was a membership oracle (round 1 of
  review), described above.
- **Accept any `pg_inherits` ancestor's grant.** Wrong for legacy inheritance, above.
- **Record the user id at `bm25_beginscan`.** Equivalent to registration-time capture, since
  the scan begins at the first fetch, and still cannot close the refcursor shape.
- **ACL and RLS checks on the distance too.** Unorders Merge Append, above.
- **Bind probes to the scan's own executor.** Would break the supported shape in
  `sql/116_score_query_overloads` section 3d (a separate statement scoring rows against two
  open cursors' scans), per the issue.
- **Mark `bm25_match` LEAKPROOF.** Would open the RLS path to this oracle.

## Consequences

- A role changing between two FETCHes of one cursor gets NULL accessors for the later rows
  (`+Infinity` distance), because the scan stays owned by the role that fetched first. A
  cursor DECLAREd as one role and fetched as another is unaffected.
- A role ranking through a plain view it was granted, without base-table SELECT, gets NULL
  score accessors. PostgreSQL does not treat a non-`security_barrier` view as protection
  against leaky functions, so this is documented, not worked around.
- A `security_barrier` view whose definition embeds a fixed `@@@` query runs under the
  caller's own id, so ownership does not cover it; exposure is limited to that fixed query,
  and the ACL and RLS checks cover the cases where the caller lacks base-table SELECT or the
  hidden rows are policy-filtered.
- **Residual: column grants on a partitioned parent are not honoured.** The per-column check
  runs only against the partition's own heap id, while the table-level check runs against
  each ancestor. A role holding column-only grants on the parent is refused (NULL, or the
  snippet error). It fails safe.
- Pinned by `sql/131_scored_scan_owner`.

## Addendum (2026-10-05, PRs #331-#350)

The whole-index reporting surface (`bm25_stats` and the read-only debug SRFs) now has its
own RLS rule: the readable gate refuses with 42501 when RLS applies to the caller on the
leaf heap, instead of returning a value (ADR 0020's addendum, D24, PR #333). This record's
NULL-on-refusal rule stays the right one for the row-addressed accessors.
