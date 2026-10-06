---
id: 0015
title: Honor SK_ISNULL, asymmetrically — an empty result for a NULL @@@ key, all matching rows for a NULL &@@ key
date: 2026-07-29
status: Accepted
summary: bm25_rescan tests SK_ISNULL before touching a key datum; a NULL @@@ key short-circuits to an empty result (bm25_match is STRICT), while a NULL &@@ order-by key falls back to the unordered membership scan so the qualifying rows are still returned.
---

# 0015. Honor SK_ISNULL, asymmetrically — an empty result for a NULL `@@@` key, all matching rows for a NULL `&@@` key

## Context

When a runtime index key evaluates to NULL, the executor sets
`sk_argument = (Datum) 0`, raises `SK_ISNULL`, and calls `index_rescan` anyway.
Testing that flag is the access method's job, and every core AM does it —
nbtree sets `qual_ok = false`, hash returns `false`, GIN sets `isVoidRes`.

`grep -rn SK_ISNULL src/` returned zero hits across the whole extension.
`bm25_rescan` fed `sk_argument` straight to `DatumGetTextPP` /
`DatumGetJsonbP`, so a NULL parameter dereferenced address 0 inside
`pg_detoast_datum_packed`: SIGSEGV, and the whole cluster restarted into crash
recovery, killing every other session. Reachable by any unprivileged user with
`SELECT` on the table (review ref C2, issue #33).

It had gone unnoticed because a *literal* `@@@ NULL` never reaches the AM —
`bm25_match` is STRICT, so the planner const-folds the qual away. It takes a
runtime key: a generic plan (`PREPARE p(text) … EXECUTE p(NULL)`), or a
parameterized nested-loop inner scan whose outer row is NULL.

The remaining question was what a NULL key *means*, and the two key kinds do not
mean the same thing.

## Decision

We will test `SK_ISNULL` before touching either key's datum, and short-circuit
the two cases **differently**:

- **NULL `@@@` (scan) key** — `bm25_match` is STRICT, so no row can satisfy the
  qual. Latch the lazy loaders shut and return an empty result, exactly as
  nbtree treats `x = NULL`.

- **NULL `&@@` (order-by) key** — the matching rows *still qualify*: the `@@@`
  key is what selects them, and `amoptionalkey = false` guarantees one is
  present. Only their ordering value is unknown, and `bm25_distance` is STRICT
  so the executor's resjunk is NULL for every row regardless. Fall through to
  the ordinary unordered membership scan driven by `keyData[0]` and let the rows
  sort to the end.

`bm25_rescan` additionally re-initializes `xs_orderbynulls` to all-true, because
`bm25_beginscan` doing it once is not enough: a prior *scoring* rescan of the
same descriptor leaves `xs_orderbynulls[0] == false` beside a stale
`xs_orderbyvals[0]`.

## Alternatives considered

- **Return an empty result for both keys** (the original report's suggested
  fix) — correct for `@@@`, but a silent wrong answer for `&@@`:
  `WHERE body @@@ 'alpha' ORDER BY body &@@ $1 LIMIT 3` with `$1` NULL would
  return zero rows where a sequential scan returns three. Trading a crash for a
  wrong answer is not a fix.
- **`ereport(ERROR)` on a NULL order-by key** — loud, and consistent with the
  project's fail-loud stance elsewhere, but it makes the index path *error* on a
  query the non-index path answers fine. The plan chosen must not change the
  answer, and "error" is a different answer.
- **Compute a real distance for the NULL key** — there is nothing to compute
  from; the query text *is* the key.

The `&@@` behaviour is not invented: GiST does exactly this for a NULL KNN
ordering key — it assumes the distance is null, sorts the row last, and returns
it. Verified against a live GiST index (`ORDER BY p <-> $1` with `$1` NULL
returns every row).

## Consequences

- A NULL query parameter is now an ordinary, boring result instead of a
  cluster-wide outage.
- `@@@` and `&@@` are deliberately asymmetric in `bm25_rescan`. Anyone adding a
  third key kind must decide which side it falls on rather than copying either
  branch.
- The `&@@`-NULL path emits rows in *unspecified* order. `sql/60_null_scankey`
  therefore re-sorts by `id` before asserting; a suite that pinned the emission
  order there would be pinning an implementation detail.
- This fix reads only `keyData[0]` / `orderByData[0]`, matching the surrounding
  code. Keys 1..n-1 being ignored entirely is a separate defect
  (review ref C8, issue #39) and is addressed on its own.
