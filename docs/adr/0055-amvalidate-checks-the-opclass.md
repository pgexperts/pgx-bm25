---
id: 0055
title: amvalidate really validates the opclass, and its canaries prove it can say no
date: 2026-08-11
status: Accepted
summary: bm25_validate walks the opfamily and checks strategy, purpose, signature, sort family and completeness instead of returning true unconditionally, with six canary opclasses in sql/86 because a "valid opclass validates" assertion is also satisfied by a stub.
---

# 0055. amvalidate really validates the opclass, and its canaries prove it can say no

## Context

`bm25_validate`, the AM's `amvalidate` callback, was `return true;` with a
trailing comment deferring the real check to "Task 11" — a milestone that had
shipped five milestones earlier. It never read its `opclassoid` argument, so it
reported success for any OID at all, including one belonging to a different
access method.

The `amapi` contract permits this. A stub returning true is legal, and only
`opr_sanity` and the `amvalidate()` SQL function consult it, so nothing was
broken. What was missing is the check's actual purpose: this AM's strategy
numbers live entirely in the opclass DDL (`amstrategies` is 0, like GiST/GIN and
pgvector), so the catalog is the ONLY place the contract is written down, and
nothing verified that a declared opclass matches it.

The failure this permits is quiet. A second opclass for `bm25_native` that omits
the `@@@` member, or attaches `&@@` as a search operator rather than an ORDER BY
one, is accepted at DDL time without complaint; it surfaces much later as a
planner "operator is not a member of opfamily" or as a scan that never receives
the qual it expects, at a site with no obvious connection to the DDL that caused
it.

## Decision

`bm25_validate` walks the opfamily's `pg_amop`/`pg_amproc` members and checks:

- strategy numbers are within `[1, 2]`;
- strategy 1 is a `SEARCH` member returning `boolean`;
- strategy 2 is an `ORDER BY` member returning `double precision`, whose
  `amopsortfamily` can sort `float8`;
- there are no support procedures at all, since `amsupport` is 0;
- the named opclass carries both a match and an ordering member **for its own
  `opcintype`**, so a family may still carry cross-type members legitimately.

Problems are reported with `ereport(INFO)` and a `false` return, per the
`amvalidate` convention, so `opr_sanity` collects every problem in one pass
rather than stopping at the first.

The opfamily name is fetched with `SearchSysCache1(OPFAMILYOID)` rather than
`get_opfamily_name()`. The latter is PG 18+ and this extension's floor is 17;
the syscache lookup is what PG 17's own `blvalidate.c` does and compiles on
both, so one code path serves the whole supported range with no `#if`.

## Alternatives considered

- **Keep the stub, fix only the stale comment** — this was on the table and was
  rejected by the user. It is the cheaper half of the finding (#64.19 treats the
  comment as the defect; #69.3 treats the absent validation as the defect) and
  would have left an AM that cannot tell a working opclass from a broken one.
- **`ERROR` instead of `INFO` + `false`** — rejected. It reads as stricter but
  is worse: it stops at the first problem, and it contradicts what every
  in-tree validator does, so `opr_sanity` output would not look like anything
  else's.
- **Validate only the members, not completeness** — rejected. "Every member
  present is well-formed" is trivially satisfied by an opclass with no members
  at all, which is exactly case (a) below and cannot drive a scan.
- **Require completeness across the whole FAMILY rather than the opclass's own
  input type** — rejected as a false-positive risk. A family may legitimately
  carry cross-type members (this one carries `(text,jsonb)` alongside
  `(text,text)`), and rejecting those would make `opr_sanity` fail for a user
  who did nothing wrong. Core validators key completeness to `opcintype` for
  the same reason.

## Consequences

- **`sql/86_errcodes_and_amvalidate` Part 3 is six CANARY opclasses**, not an
  assertion that the shipped opclass validates. That assertion is worth having
  but proves nothing on its own, because `return true` satisfies it too. Each
  canary trips a different primary branch: missing ORDER BY member, missing
  match member, right operator at the wrong purpose, an undefined strategy
  number, a sort family that cannot sort `float8`, and an ORDER BY member of the
  wrong result type. Under the old stub all six report valid and all ten INFO
  lines vanish — verified as a negative control on the full suite run.
- **Two validator branches deliberately have no canary**, and this is recorded
  rather than quietly tolerated: `CREATE OPERATOR CLASS` rejects a non-boolean
  SEARCH operator and any support procedure itself, so those shapes cannot be
  constructed to test against. They are still checked, as a backstop that costs
  nothing and would matter the day `amsupport` grows. A canary that cannot be
  injected is not a canary.
- **What core screens is not what you would guess, and the canaries exist
  because of the gap.** Core rejects a non-boolean operator declared as a search
  member — but accepts a BOOLEAN one declared `FOR ORDER BY`, and accepts an
  ORDER BY member whose sort family cannot sort the result type. Everything core
  declines to check is this validator's job.
- **`CREATE OPERATOR CLASS` still does not invoke this.** Core calls `amvalidate`
  only from the SQL function. A user can still create an opclass this AM cannot
  execute and hear nothing until the planner disagrees; what changed is that
  `opr_sanity`, and anyone who asks, now get a straight answer.
- **A PG-version portability trap was caught in review, not in CI.** The first
  draft used `get_opfamily_name()`, transcribed from PG 18.3's `blvalidate.c`;
  it does not exist in PG 17 and would have reddened that CI leg. Local
  development here is PG 18.3 only, so nothing on this machine could have caught
  it — worth remembering when transcribing from core.
