---
id: 0029
title: Every regclass entry point checks privileges — ownership to mutate, SELECT on the indexed table to report
date: 2026-07-29
status: Accepted
summary: bm25_seal, bm25_merge and bm25_upgrade route through the ownership + AM-identity helper the debug surface already used, and bm25_stats / bm25_wand_stats through a read-only sibling gated on SELECT of the indexed table.
---

# 0029. Every regclass entry point checks privileges — ownership to mutate, SELECT on the indexed table to report

## Context

[0020](0020-debug-surface-privileges.md) closed this hole for the `bm25_debug_*`
surface, with two layers: a `REVOKE ... FROM PUBLIC` loop in the install script, and
`bm25_debug_open_index` (ownership + AM identity) on the mutating ones.

It did not cover the **public maintenance surface**, and a REVOKE is not the answer
there — `bm25_seal`, `bm25_merge`, `bm25_upgrade`, `bm25_stats` and `bm25_wand_stats`
are meant to be callable. Each took a `regclass` and `index_open()`ed it, and
`index_open()` validates `relkind` and nothing else: not the access method, not the
ACL, not ownership.

So any role that could connect could seal, merge and re-stamp any index in the
database. `bm25_upgrade` was the widest: with
`bm25_debug_enable_synthetic_transform(true)` first it takes the full-rewrite path,
re-emitting every live segment through `bm25_merge_rewrite_all` and retiring the old
ones — which never calls `bm25_reclaim_retired`, so the old pages persist until a
later merge or VACUUM. Loopable as cheap WAL amplification plus bloat against
someone else's index.

The input cannot be filtered upstream. A schema-qualified `regclass` literal does hit
`LookupExplicitNamespace`'s USAGE check, but a bare **numeric OID** cast to
`regclass` needs no schema privileges at all, and `pg_class` is world-readable. So
the check has to be in the function.

Findings H5 (issue #45) and the body of issue #42 describe the same gap; #45's own
suggested fix says the honest form is "one shared owner-check helper applied to
every mutating entry point, not just this one."

## Decision

**Mutating entry points require ownership.** `bm25_seal`, `bm25_merge` and
`bm25_upgrade` now open through the helper the debug surface already used, renamed
`bm25_debug_open_index` → **`bm25_index_open_owned`** because "debug" no longer
described who calls it. Ownership rather than an ACL bit: PostgreSQL has no
privilege meaning "may maintain this index", and core's closest precedent,
`gin_clean_pending_list`, uses `object_ownercheck` for exactly this shape.

**Reporting entry points require SELECT on the indexed table.** `bm25_stats` and
`bm25_wand_stats` open through a new **`bm25_index_open_readable`**. Ownership would
be wrong — reading an index's statistics is reasonable for a non-owner — but it must
not become a side channel around table privileges, so the gate is the privilege that
would let the caller see the underlying rows.

Both helpers also check **AM identity**, by comparing the resolved handler's
`ambuild` against our own rather than looking up an AM name, so renaming the access
method cannot disarm it. `bm25_merge_sql`'s hand-rolled
`get_index_am_oid("bm25_native")` check is subsumed and removed.

## Alternatives considered

- **`REVOKE ... FROM PUBLIC` on the maintenance functions**, as for the debug
  surface. Wrong tool: these are meant to be callable by their owner without a
  superuser first granting EXECUTE to every role that owns a bm25 index.
- **`ACL_MAINTAIN` on the table** (PG 17+). Closer in spirit than ownership, and it
  is what `VACUUM`/`ANALYZE` use — but it grants over the *table*, while these
  functions name an *index*, and the resulting rule ("may seal an index if you may
  maintain its table") is harder to state than ownership and has no precedent for
  index-level maintenance functions. Worth revisiting if core ever gives indexes
  their own maintenance privilege.
- **Ownership for the reporters too.** Simpler and one helper less, but it would
  deny an ordinary reporting role a statistic it is otherwise entitled to infer, and
  the finding's own recommendation is SELECT for readers.
- **Check in each function rather than a shared helper.** Five copies of a
  three-line check, and the debug surface already proved how that ends — the
  original gap existed because each entry point open-coded `index_open`.

## Consequences

- A non-owner can no longer seal, merge or upgrade an index they do not own, and a
  role without SELECT on the table can no longer read its bm25 statistics. **This is
  a behavior break on a 1.0 surface**, and it is the point.
- Any monitoring role that reads `bm25_stats` now needs SELECT on the indexed table.
  That is the same privilege it would need to see the rows the statistics describe.
- Aiming any of the five at a btree now fails with "is not a bm25_native index"
  rather than reading or writing bm25 structures on its pages — verified for the
  index's **owner**, since ownership alone was never sufficient.
- `bm25_debug_enable_synthetic_transform` needs no separate treatment: it matches
  `bm25_debug_%`, so [0020](0020-debug-surface-privileges.md)'s REVOKE loop already
  covers it. `sql/71_maintenance_privileges` asserts that rather than assuming it.
- `sql/71_maintenance_privileges` pins the owner path still working, the AM-identity
  refusal, the non-owner refusals, the SELECT-gated reporter split, and that a bare
  numeric OID cast does not bypass any of it.

## Addendum (2026-10-05, PRs #331-#350)

`bm25_index_open_owned` now runs, in order: the relkind check (42809 for a non-index, 42704
for a dangling OID; #313, PR #345), the ownership check, core's
`PreventCommandDuringRecovery("bm25 index maintenance")`, then `index_open` and the AM
identity check (D23, #307 META-07, PR #333). On a hot standby every write entry point
(`bm25_seal`, `bm25_merge`, `bm25_upgrade` and every mutating `bm25_debug_*` lever) used to
run until `XLogBeginInsert` failed with XX000, and `bm25_debug_alloc_unknown_page` extended
the standby's relation file first. They now fail with 25006 and core's "cannot execute ...
during recovery" wording, after the ownership check (so a non-owner gets the same 42501 on
either node) and before any lock or page read. `bm25_upgrade`'s private check is gone, so its
standby SQLSTATE moves from 55000 to 25006.

Release note: a seal or merge called on a standby with no work to do now raises 25006 instead
of returning as a no-op. Pinned by `t/034_standby_write_gates`; the `sql/63` completeness
check is described in ADR 0020's addendum.
