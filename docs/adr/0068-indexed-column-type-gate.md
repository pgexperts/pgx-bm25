---
id: 0068
title: Indexed column types are gated at the top of both build and insert, on the base type
date: 2026-08-20
status: Accepted
summary: bm25_check_indexed_column_types refuses an indexed key column whose BASE type is not text/varchar, called at the top of bm25_build and bm25_insert rather than from bm25_resolve_fields, which on the insert path runs after the tokenize loop has already dereferenced the Datum.
---

# 0068. Indexed column types are gated at the top of both build and insert, on the base type

## Context

`bm25_build_callback` and `bm25_insert` both do `DatumGetTextPP(values[f])` on
every indexed key column with no type test at all. A non-text column therefore
reaches `PG_DETOAST_DATUM_PACKED` on a Datum that is not a varlena: a by-value type
like `int4` dereferences the integer itself as a pointer (42 → SIGSEGV →
postmaster crash-restart of every backend), and a by-reference type like `uuid`
reads its first bytes as a varlena header and yields a garbage `VARSIZE`.

`bm25_validate` read `opcintype` and used it only to test member coverage, never to
test what the type IS — which is `amvalidate`'s entire stated purpose, and the
canary suite's six malformed opclasses are all `FOR TYPE text`, so the
wrong-input-type shape went unexercised.

Reachability is narrower than it first appears, and worth stating so the gate is
not over-credited: core's own default-opclass resolution refuses an ordinary
`CREATE INDEX ... (int_col)` before the AM sees it, because this AM declares a
default opclass only for `text`. The crash requires an explicitly-named opclass
declared `FOR TYPE` something else, which satisfies core's resolution. Issue #150
(HDL-02).

## Decision

`bm25_check_indexed_column_types` refuses an indexed KEY column whose type this AM
cannot read as text, and is called at the TOP of `bm25_build` and at the TOP of
`bm25_insert` — before anything dereferences a Datum.

It compares the BASE type (`getBaseType`), consulted only when the
`TEXTOID`/`VARCHAROID` fast path misses.

`bm25_validate` additionally reports a non-text `opcintype` (INFO + `result = false`),
but that half is reporting-only: DDL never invokes `amvalidate` — only the
`amvalidate()` SQL function does, which `opr_sanity` drives — so it cannot stop a
malformed opclass reaching CREATE INDEX.

## Alternatives considered

- **Put the check in `bm25_resolve_fields`' per-column loop** — where it first lived,
  and it is correct on the build path (resolve precedes the callback). **Useless on
  the insert path**: there `bm25_resolve_fields` is called only after the tokenize
  loop has already run `DatumGetTextPP`, because it is needed for `key_field`, which
  is resolved later. Adversarial review demonstrated an index over a non-text column
  built by a binary predating the check still dying with signal 11 on its first
  INSERT against the "fixed" binary.
- **Compare the raw `atttypid`** — rejected after review demonstrated it as a
  regression: a DOMAIN over text passes core's opclass resolution (binary-coercible)
  and its Datum genuinely IS a text varlena, so such indexes worked. Comparing the
  raw type broke `CREATE INDEX` on them and, worse, broke every INSERT into an
  already-built domain index after a pure binary upgrade — a working index killed
  with no DDL involved.
- **Rely on `amvalidate` alone** — rejected; see above, DDL never calls it.

## Consequences

`bpchar` and `text[]` are refused by core's opclass resolution before reaching the
AM, so the gate neither sees nor needs to handle them. `varchar` is accepted
deliberately: it is binary-coercible to text and its Datum is a real varlena.

The insert-path half is defence in depth that a fresh regression run cannot
exercise — once `CREATE INDEX` refuses, there is no index over a non-text column to
insert into. It matters only for an index built by a binary predating the check, and
it was verified by building exactly that, with the build-path guard removed.

The same change gives both index-open gates (`bm25_index_open_owned` /
`bm25_index_open_readable`) a `RELKIND_HAS_STORAGE` test, since `index_open` accepts
`RELKIND_PARTITIONED_INDEX` and a partitioned index's relcache entry carries
`rd_indam`, so it passed the AM-identity check and callers then read block 0 of a
relation with a zeroed `rd_locator`. AM identity is tested FIRST: neither check
reads a page, so the order costs nothing and is purely about which message is true —
a partitioned index of another access method fails both, and the storage message's
hint would send the caller to leaves that are not bm25 indexes either.
