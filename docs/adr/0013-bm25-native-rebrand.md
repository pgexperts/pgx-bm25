---
id: 0013
title: Rebrand to bm25_native as a fresh 1.0 lineage
date: 2026-07-17
status: Accepted
summary: Rename the extension, access method, GUC prefix, and library from pg_bm25_index to bm25_native for 1.0 to escape a hard name clash with pg_search's own bm25 access method; keep the schema-qualified SQL API unchanged.
---

# 0013. Rebrand to bm25_native as a fresh 1.0 lineage

## Context

ParadeDB's `pg_search` registers an index access method also named `bm25`.
PostgreSQL access methods live in `pg_am`, which has **no namespace column** —
an AM name is global to the cluster. `relocatable=true` on our extension only
moves the *schema-qualified* SQL objects (functions, operators, opclass); it
cannot move or rename the access method, so it does nothing to avoid the clash.
The concrete failure is total: `CREATE EXTENSION pg_bm25_index` fails with a
duplicate-access-method error on any database where `pg_search` is already
installed, and vice versa. The two extensions cannot coexist.

The rename also has a correctness consequence inside our own code.
`src/bm25_merge.c`'s merge guard resolved the access method by string literal —
`get_index_am_oid("bm25", ...)` with `missing_ok=true` — and errored on any index
whose `relam` did not match. Because an AM name is unique per database, this literal
could never mis-resolve to a rival's AM (a second AM named `bm25` cannot be created
where ours already exists), so it was not a silent wrong-backend hazard. It is,
however, load-bearing for the rename: once the access method becomes `bm25_native`,
the old literal resolves to `InvalidOid`, and the guard would then reject every
legitimate `bm25_native` index. The literal must — and does — move in lockstep with
the AM name.

This blocks the two things 1.0 exists to enable: the cadc/BriefBank dual-backend
trial (running our engine and `pg_search` side by side in one database for a
same-schema head-to-head comparison), and any `pg_search` user who wants to
migrate incrementally with both extensions installed at once. At this point the
project is pre-1.0 with **zero external users**, so the cost of a rename is
paid entirely by us and never by an installed base.

## Decision

We will fully rebrand to `bm25_native` for the 1.0 release: the extension name,
the access method (`CREATE ... USING bm25_native`), the GUC prefix
(`bm25_native.seal_threshold`, `bm25_native.wand_top_k`,
`bm25_native.wildcard_min_prefix`, `bm25_native.wildcard_max_expansions`), and
the shared library. This is a fresh **1.0 lineage**, not a continuation of the
0.x version chain.

The SQL API names are deliberately **kept**: the `bm25_*` functions, the `@@@`
and `&@@` operators, and the `text_bm25_ops` operator class. These are all
schema-qualified catalog objects, so they do not collide with `pg_search`
(whose own API lives under its `paradedb` schema) — coexistence needs no API
rename. `bm25_` also reads as a generic "BM25 API" prefix rather than a product
name, so keeping it costs nothing in clarity and saves every caller a rewrite.

There is **no on-disk format change** — format v6 is carried across the rename
unchanged. There is **no 0.x -> 1.0 upgrade path**: PostgreSQL has no
`ALTER ACCESS METHOD ... RENAME`, so no `ALTER EXTENSION ... UPDATE` script can
carry an existing index across an access-method rename. The former 0.1 -> 0.2 ->
0.3 upgrade lineage is folded into a single consolidated `bm25_native--1.0.sql`
(catalog state identical to 0.3). The (zero) existing 0.x installs migrate by
dropping the old extension, creating `bm25_native`, and reindexing.

## Alternatives considered

- **Rename only the access method, leave the extension named `pg_bm25_index`** —
  rejected. It resolves the AM clash but leaves a permanent, confusing mismatch
  between the extension name and the AM/GUC/library identity, and every doc and
  error message would have to explain the split. A clean single identity is
  worth more than preserving the extension name for an installed base that does
  not exist.
- **Vendor a patched build inside cadc only** — rejected. It unblocks the
  in-house trial but creates a maintained divergence between cadc's copy and
  upstream, and upstream `pg_bm25_index` stays uninstallable next to `pg_search`
  for everyone else, including future migrators. The clash is upstream's to fix,
  not cadc's to paper over.
- **Run the trial in a separate database** (our engine in one DB, `pg_search`
  in another) — rejected. It sidesteps the clash but defeats the point of the
  trial: heavier ops (two databases, cross-DB result plumbing) and, crucially,
  not a same-database comparison, so index/query behavior is no longer measured
  under identical planner, cache, and data conditions.

## Consequences

- The cadc/BriefBank dual-backend head-to-head is unblocked: both extensions
  install in one database and can be compared on the same schema and data.
- Every future `pg_search` migrator can install `bm25_native` alongside
  `pg_search` and cut over incrementally, rather than choosing one or the other
  per cluster.
- The `bm25_merge.c` name-lookup hazard is closed: with a unique AM name, the
  lookup can no longer bind ParadeDB's AM.
- The format-upgrade machinery from ADR 0009 (`bm25_upgrade`,
  `min_read_version`, the transform registry) resumes cleanly from the 1.0
  baseline; format v6 and all query/scoring behavior are unchanged, so the
  rebrand carries no functional risk.
- All prose docs, the consolidated SQL script, and the regression/TAP suites now
  carry the `bm25_native` identity. The one deliberate exception is historical
  references to the pre-1.0 `pg_bm25_index` lineage, which keep the old name
  because that is what those artifacts were actually called.
- The 1.0 feature set is the sum of the prior milestones' decisions, still in
  force under the new identity: score-accessor concurrency (ADR 0007), format
  stability and online upgrade (ADR 0009), blind-constant planner estimates
  (ADR 0010), ranking-quality benchmarks (ADR 0011), and live k1/b reloptions
  (ADR 0012).
- Committed cost: the rename is irreversible in practice for anyone who adopts
  1.0, and because there is no upgrade script, the (currently empty) 0.x install
  base has only a drop-and-reindex migration.

## Addendum (2026-10-05, PRs #331-#350)

The control file now says `relocatable = false` (D26, #311 CI-16, PR #340). As the Context
above says, relocation cannot move the access method, so it buys nothing for the name clash,
and no test had ever relocated the extension, so `true` was an untested claim. `CREATE
EXTENSION ... SCHEMA x` still works; only a later `ALTER EXTENSION ... SET SCHEMA` is
refused. The packaging gate (`test/check_packaging_identity.py`) pins `relocatable =
false`, the META.json PostgreSQL prerequisite (17.0.0, equal to the `#error` floor in
`bm25.h`) and the Makefile's header-dependency rule.
