---
id: 0025
title: Wildcard guardrails are PGC_SUSET, and bm25_bulkdelete calls vacuum_delay_point
date: 2026-07-29
status: Accepted
summary: The two wildcard limits move from PGC_USERSET to PGC_SUSET so the users they constrain cannot switch them off, and the VACUUM per-document sweep gains the throttling interrupt point it never had.
---

# 0025. Wildcard guardrails are PGC_SUSET, and bm25_bulkdelete calls vacuum_delay_point

## Context

Two findings filed as one issue (#44); its title and summary describe the first,
its evidence section describes the second. Both are real, both are one-line
policy fixes about work the server could not govern, so they are recorded
together.

**Wildcard guardrails were user-settable.** `bm25_native.wildcard_min_prefix`
(default 3, minimum 0) and `bm25_native.wildcard_max_expansions` (default 1000,
maximum 1000000) were both `PGC_USERSET`. Any user could run

```sql
SET bm25_native.wildcard_min_prefix     = 0;
SET bm25_native.wildcard_max_expansions = 1000000;
SELECT ... WHERE body @@@ bm25_wildcard('body', '*');
```

and expand one pattern to a million dictionary terms. A limit whose entire
purpose is to constrain the caller cannot be in the caller's gift.

**The VACUUM sweep had no throttling point.** `bm25_bulkdelete` walks every docid
of every segment, each following a DOCMAP chain and reading a buffer — the file's
own header comment concedes the cost is `O(ndocs · docmap-chain)` per segment. It
contained neither `CHECK_FOR_INTERRUPTS` nor `vacuum_delay_point`, so autovacuum's
cost-delay budget was silently bypassed for the whole sweep and the vacuum
saturated I/O the foreground workload needed. Core AMs guard exactly this
(`btvacuumpage`, `ginVacuumPostingTreeLeaves`).

[0024](0024-scan-interrupt-checks.md) covers the scan path and explicitly leaves
VACUUM out, because VACUUM needs the throttling form rather than a bare check.

## Decision

**Both wildcard GUCs become `PGC_SUSET`.** `seal_threshold` and `wand_top_k` stay
`PGC_USERSET`: moving those only trades performance, while these two decide
whether a query is allowed to run at all.

**`bm25_bulkdelete` calls `vacuum_delay_point()` per document** and
`CHECK_FOR_INTERRUPTS()` per segment. `vacuum_delay_point` calls
`CHECK_FOR_INTERRUPTS` internally, so the per-document site is both the
cancellation point and the throttling point, matching the core AMs.

The signature differs across the supported range — `vacuum_delay_point(void)` on
the PG 17 floor, `vacuum_delay_point(bool is_analyze)` from PG 18 — so the call
goes through a `BM25_VACUUM_DELAY_POINT()` macro guarded on `PG_VERSION_NUM`.
CI builds both versions, which is what gates the guard.

## Alternatives considered

- **A `check_hook` allowing only the tightening direction** — lets a user raise
  `min_prefix` or lower `max_expansions` while blocking the reverse. Genuinely
  better ergonomics, and rejected as too much machinery for the problem: two hook
  functions and a hand-rolled asymmetric-permission rule that PostgreSQL has no
  precedent for, to serve a user who wants to be stricter than their
  administrator. `GRANT SET ON PARAMETER` already covers the real delegation case.
- **`PGC_SIGHUP` / `PGC_POSTMASTER`** — stronger than needed, and it would stop an
  administrator from tuning a single session.
- **Leave them `PGC_USERSET` and rely on the interrupt checks from
  [0024](0024-scan-interrupt-checks.md)** — cancellability makes the runaway
  *killable*, not *prevented*; the expansion still allocates and still burns the
  I/O budget until someone notices.
- **A bare `CHECK_FOR_INTERRUPTS` in the VACUUM loop** — restores cancellation but
  not throttling, which is half the defect and the half a DBA cannot work around.

## Consequences

- A non-superuser cannot weaken either wildcard limit. They also cannot *tighten*
  it, which is the accepted cost; `GRANT SET ON PARAMETER
  bm25_native.wildcard_min_prefix TO <role>` (PG 15+) is the documented escape
  hatch, and `67_wildcard_guc_privileges` asserts that it works and that it is
  per-parameter.
- Any existing application that lowered these limits from a non-superuser role now
  gets `permission denied to set parameter`. This is a deliberate behavior break
  on a 1.0 surface; the guardrail was not enforceable before.
- VACUUM of a bm25 index is cancellable and honors `vacuum_cost_delay`, so
  autovacuum throttling engages for this AM as it does for btree and GIN.
- `67_wildcard_guc_privileges` is a genuine red/green test: pre-fix the
  non-superuser `SET` succeeded and the short-prefix query returned rows instead
  of erroring.
- **The throttling itself is not regression-tested.** Neither delay timing nor
  cost accounting is observable in `pg_regress` output, so `ci.yml` asserts the
  call site still exists instead, alongside the interrupt-check floor. Correctness
  of the sweep is already covered by `18_vacuum_reclaim`.

## Addendum (2026-10-04)

The fresh-eyes review of this date measured the USERSET `seal_threshold` floor: an INSERT-only role setting 64 kB produced ~30 segments per 2,000 inserts versus 0 at the default, until the next VACUUM. The 'only trades performance' premise holds per session but the segment count is shared state. Tracked in #310.

## Addendum (2026-10-05, PRs #331-#350)

Decided for #310 SURFACE-08 (D25): the lowering direction of the USERSET `seal_threshold`
is accepted on the same "bounded and VACUUM-recoverable" premise as the raising direction.
The damage is bounded by the inserting role's own write volume, corrupts nothing, and a
VACUUM-time merge (or `bm25_merge()`) consolidates the small segments. The 64 kB floor is
not raised: a 1 MB floor would still leave a 4x lever and would break every suite and
bulk-load recipe that uses a smaller value, and `PGC_SUSET` would take the legitimate
bulk-load raise away from non-superusers. The registration in `_PG_init` now carries this
reasoning (PR #349). Comment only; no behaviour changed.
