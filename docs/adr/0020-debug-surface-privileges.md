---
id: 0020
title: Mutating debug entry points check ownership and AM identity; the whole debug surface is revoked from PUBLIC
date: 2026-07-29
status: Accepted
summary: Every mutating bm25_debug_* function opens its regclass through bm25_debug_open_index (ownership + AM identity before any page is touched), and the install script revokes EXECUTE on all bm25_debug_* functions from PUBLIC.
---

# 0020. Mutating debug entry points check ownership and AM identity; the whole debug surface is revoked from PUBLIC

## Context

`index_open()` validates `relkind` and nothing else — not the access method, not
the ACL, not ownership. Four `bm25_debug_*` functions took a user-supplied
`regclass`, `index_open()`ed it, and mutated pages under a Generic WAL window:
`bm25_debug_stamp_version`, `bm25_debug_write_optional_region`,
`bm25_debug_alloc_unknown_page`, `bm25_debug_pending_append`. The only "guard"
was a `TEST-ONLY` comment in each header.

PostgreSQL grants `EXECUTE` on a new function to `PUBLIC` by default, and the
install script contained **zero** `REVOKE` statements. So all 46 `bm25_debug_*`
functions shipped callable by any user in any database with the extension
installed.

`BM25PageGetMeta` is `PageGetContents`, the same offset a btree's
`BTMetaPageData` occupies. So
`SELECT bm25_debug_stamp_version('pg_class_oid_index'::regclass, 0, 0, 0)` writes
`format_version` onto `btm_version`, and `_bt_getmeta`'s
`btm_version < BTREE_MIN_VERSION` check then fails on every `pg_class` OID
lookup. The write is Generic-WAL-logged, so it is durable and replays on every
standby (review ref C7, issue #38).

Reproduced on the pre-fix build: as the *owner* of an ordinary btree,
`bm25_debug_stamp_version`, `bm25_debug_write_optional_region` and
`bm25_debug_alloc_unknown_page` all **succeed** against it (the last one extends
the btree by a page and returns the block number). Only
`bm25_debug_pending_append` failed, and only incidentally — its `bm25_meta_read`
happened to catch the missing `BM25_MAGIC`.

## Decision

Two independent layers.

**1. A C-level gate.** All four mutating debug functions open their argument
through a new shared `bm25_debug_open_index(relid, lockmode)`, which — before any
page is touched — checks:

- **ownership**: `object_ownercheck(RelationRelationId, relid, GetUserId())`,
  failing with the standard `aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, …)`;
- **AM identity**: `index->rd_indam->ambuild != bm25_build` ⇒
  `ERRCODE_WRONG_OBJECT_TYPE`.

AM identity is tested by comparing the resolved handler's `ambuild` against our
own `bm25_build` (`bm25_handler.c` sets `amr->ambuild = bm25_build`). That is
exact, needs no catalog lookup or AM-name string, and cannot be disarmed by
renaming the access method.

**2. A script-level `REVOKE`.** The install script revokes `EXECUTE` on every
`bm25_debug_*` C function in the install schema from `PUBLIC`, via a loop rather
than 46 hand-written lines — so a debug function added later is covered
automatically. `sql/63_debug_privileges` asserts that no `bm25_debug_*` function
is PUBLIC-executable, which is what gates the loop's own correctness.

## Alternatives considered

- **`REVOKE` alone.** Cheapest, and it does close the reported attack. Rejected
  as sufficient: a superuser or a deliberately-granted role could still aim a
  mutating function at an arbitrary btree and corrupt it, and the function would
  be doing exactly what it was asked. The AM check is a correctness guard, not
  only a privilege one.
- **The C gate alone.** Leaves ~40 read-only introspection functions exposing
  another user's index contents to any user (that exposure is issue #52's scope;
  the `REVOKE` closes it here as a side effect).
- **Delete the debug functions from the 1.0 script.** They are load-bearing for
  the regression suite, including the cassert+UBSan CI job, which builds
  PostgreSQL from source without `contrib` and so has no `pageinspect`.
- **46 explicit `REVOKE` lines** instead of the loop — auditable at a glance, but
  it rots the first time someone adds a debug function and forgets. The loop plus
  a test asserting the outcome gives the same auditability without the rot.

## Consequences

- The debug functions are now owner-only and bm25-only. A regression suite runs
  as the index's owner, so nothing in the suite needed changing.
- Granting a non-owner `EXECUTE` on a mutating debug function is no longer
  enough to use it — the ownership check runs regardless. That is intended.
- `bm25_debug_open_index` is the single place these checks live. A new mutating
  debug function that calls `index_open()` directly silently reopens the hole;
  the function's own comment says so.
- **Out of scope, deliberately:** `bm25_seal(regclass)`, `bm25_merge(regclass)`
  and `bm25_upgrade(regclass)` also mutate a user-supplied index and remain
  PUBLIC-executable. They are documented user-facing maintenance functions, and
  restricting them is an API decision rather than a bug fix — raised in the PR
  rather than changed silently. They already fail safely on a non-bm25 index
  (`bm25_meta_read` rejects the missing `BM25_MAGIC`), so the block-0 corruption
  vector does not apply to them.

## Addendum (2026-07-29)

Both deferrals above are now closed, by separate changes.

**The read-only half (issue #52, review ref H13).** The "C gate alone" alternative
noted that ~40 read-only introspection functions were left exposed, and treated the
`REVOKE` as closing that "as a side effect". It does not close it fully: the `REVOKE`
bounds *who may call* the function, but a role explicitly granted `EXECUTE` — a
plausible thing to hand an ops or debugging role — could still aim any of them at any
bm25 index in the database, including indexes on tables it had no `SELECT` on, and
dump the dictionary, postings, positions, per-doc keys and live/dead counts. Nothing
bounded *what it could be aimed at*.

All 33 read-only `bm25_debug_*` SRFs now open through `bm25_index_open_readable`
(introduced by [0029](0029-mutating-entry-point-ownership.md) for `bm25_stats` /
`bm25_wand_stats`), so the same policy applies to the whole read-only surface:
`ACL_SELECT` on the indexed table, deliberately not ownership — a role that can
already read the rows learns nothing new from the dictionary — plus AM identity,
which also stops a foreign AM's 16-byte special area being read as a 24-byte
`BM25PageOpaque`. `sql/63_debug_privileges` gained coverage for both halves: a
reader refuses a btree, and a role holding `EXECUTE` but not `SELECT` on the table
is refused on the *table*.

The C gate and the `REVOKE` are now the same shape on both halves of the surface:
mutators require ownership, readers require table `SELECT`, and neither is reachable
by PUBLIC at all.

**The maintenance functions.** `bm25_seal` / `bm25_merge` / `bm25_upgrade` were
restricted to the index owner by [0029](0029-mutating-entry-point-ownership.md), which
also supplied the two helpers this addendum's change reuses.

**Note on issue #53 (review ref H14).** That report claims the 1.0 script contains no
`REVOKE` at all, citing `grep -c 'REVOKE|GRANT' bm25_native--1.0.sql` returning 0. The
command is missing `-E`, so it searched for the literal string `REVOKE|GRANT` and could
only ever return 0; the reviewer and the verifier ran the same broken command. The
`REVOKE` loop this record describes was already in the script, and
`sql/63_debug_privileges` was already asserting zero PUBLIC-executable debug
functions. The genuine remainder of that finding — the missing C-level ACL check on
the readers — is the change above.

## Addendum (2026-09-28)

The debug surface this record locks down gained a new entry that is neither
of the two kinds this record covers. `bm25_native.debug_pause`
([0102](0102-bulkdelete-holds-the-singleton-for-its-whole-pass.md)) is a GUC, not
a `bm25_debug_*` SQL function, so neither `bm25_debug_open_index` nor the
`REVOKE` loop applies to it. It is declared `PGC_SUSET`: settable by a
superuser, or by a role an administrator has granted `SET` on that parameter
(`GRANT SET ON PARAMETER`, PG 15+), the same protection the other `PGC_SUSET`
`bm25_native.*` GUCs use (six now: the wildcard guardrails, `debug_budget`,
and this one). Setting it parks nothing by itself. At a named pause point the
backend blocks acquiring a ShareLock on an advisory lock tag that a test
holds, and at the `bulkdelete_*` points it is holding the seal/merge singleton
while it waits, so a role able to set it can stall every writer of the index
for as long as it can hold that advisory lock. `MarkGUCPrefixReserved("bm25_native")`
still runs after every `bm25_native.*` GUC is registered, so a typo'd parameter
name cannot be silently accepted as a placeholder USERSET GUC that no-ops a
`PGC_SUSET` guardrail.

## Addendum (2026-10-05, PRs #331-#350)

- **The readable gate refuses under row-level security** (D24, #310 SURFACE-04, PR #333).
  `bm25_index_open_readable` fronts `bm25_stats` (PUBLIC) and every read-only
  `bm25_debug_*` SRF, and table SELECT is not "may see every row" when a policy applies.
  After the ACL check, the gate raises 42501 when `check_enable_rls` on the leaf heap returns
  `RLS_ENABLED` (with `noError`, so `row_security = off` gets the same message). Leaf
  only, unlike ADR 0115's ancestor walk: this gate's ACL check is leaf-only, and a parent's
  policy does not apply to direct access to a partition. It refuses outright, the way
  `pg_stats` omits RLS tables, where ADR 0115's row-addressed accessors return NULL:
  everything behind this gate reports on the whole index. Owners without FORCE, superusers
  and BYPASSRLS roles are unaffected. Pinned by `sql/142_readable_gate_rls`.
- **Both gates resolve the relkind first** (#313 META-06/SURFACE-05, PR #345): a table's OID
  gets 42809 and a dangling OID 42704, where `IndexGetRelation` or `index_open` gave XX000.
  The relkind read takes no lock; a drop between it and `index_open` still gets
  `index_open`'s own error.
- **The owned gate refuses during recovery** with 25006 (ADR 0029's addendum), which covers
  every mutating lever, including `bm25_debug_poke_page` (ADR 0126).
- **`sql/63` is now complete and checked.** Eight owned-gate writers added after this record
  were missing from its foreign-AM and non-owner checks; they and `bm25_seal`,
  `bm25_merge`, `bm25_upgrade` are now listed. `test/check_owned_gate_coverage.py` derives
  the gate's callers from `src/`, maps each to its SQL name through the install script, and
  fails (both directions) if `sql/63`'s ownership block and expected output disagree; it
  runs as a static CI step. The ownership block calls each writer as a non-owner without
  SELECT on the table, so a writer moved onto the readable gate changes the output.
- **`bm25_native.debug_cancel_at`** (PGC_SUSET, PR #339) names pause points at which the
  backend raises a query cancel against itself, the single-session counterpart of
  `debug_pause` (ADR 0102's addendum). It is reserved under the same prefix and settable
  only as `debug_pause` is.

## Addendum (2026-10-06)

**`bm25_native.debug_cancel_after`** (PGC_SUSET, default 0) joins the debug levers. It
holds a `debug_cancel_at` cancel back until the backend's `bm25_debug_work_units()`
counter reaches it, so `sql/66` and `sql/80` can land their injected cancels mid-loop
(ADR 0070's 2026-10-06 addendum). It qualifies `debug_cancel_at` and is settable only as
that GUC is. `sql/67`'s census lists it.
