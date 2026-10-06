---
id: 0075
title: The REVOKE loop is an allowlist of deliberately-public names, and the test that gates it pins the public surface instead of re-running its predicate
date: 2026-08-20
status: Accepted
summary: A prefix-matching REVOKE loop excused every debug function not named bm25_debug_*, and exactly one is — bm25_wand_stats shipped PUBLIC-executable; the loop is now an allowlist so the default is revoked, and sql/63_debug_privileges enumerates and pins the actual public surface rather than reusing the install script's own WHERE clause, which is why the blind spot survived a suite named for it.
---

# 0075. The REVOKE loop is an allowlist of deliberately-public names, and the test that gates it pins the public surface instead of re-running its predicate

## Context

PostgreSQL grants `EXECUTE` on a new function to `PUBLIC` by default, so the install
script ends with a `DO` block that revokes the debug surface. It selected
`proname LIKE 'bm25\_debug\_%'`.

That predicate excuses, silently, every debug function whose name does not carry the
prefix — and one does not. `bm25_wand_stats` is a debug probe by its own header
comment, by its SQL comment ("Development and regression testing only; not on the
query path"), and by living in `src/bm25_debug.c`, which no other public function
does. It was `PUBLIC`-executable from the day the loop was written.

The reason that survived is the more interesting half. `sql/63_debug_privileges.sql`
asserted the loop's result with a `WHERE` clause **character-for-character identical**
to the loop's own:

```sql
 WHERE p.proname LIKE 'bm25\_debug\_%'
   AND n.nspname = current_schema()
   AND l.lanname = 'c'
```

A test that reuses the implementation's predicate can only ever confirm that the
predicate is self-consistent. It cannot notice that the predicate is the *wrong*
predicate. So the hole sat inside a suite named `debug_privileges`, green, for as long
as the loop existed. Its sanity companion (`count(*) > 40`) could not rescue it either:
that only proved the pattern matched *something*, with enough headroom that the surface
could have halved and stayed green.

A third problem compounded both: the loop's comment claimed it replaced "46
hand-written REVOKE lines". The true figure was 61 when the finding was filed, 71 when
it was verified, and 77 by the time it was fixed — the fix itself added six more. Three
other files carried their own stale counts of the same surface.

## Decision

**Invert the loop to an allowlist.** It now selects every C-language `bm25%` function in
the install schema *except* an explicit list of 14 deliberately-public names — the
operator and AM anchors, the query-path functions, and the documented maintenance and
reporting entry points. The default flips: a new debug function is covered whether or
not whoever adds it thinks about privileges, and a new public function requires a
visible, reviewable entry.

**Make the test enumerate and pin, not re-derive.** `sql/63_debug_privileges` now lists
the entire `PUBLIC`-executable surface by name and pins it in expected output. Adding a
function that is public — deliberately or by forgetting — changes that output and fails
the suite, as a diff a reviewer reads rather than a count they must recompute. A second,
independent assertion states the specific regression directly: nothing matching
`bm25_debug_%`, and not `bm25_wand_stats`, may be public.

**Stop stating counts in prose.** The loop's comment no longer claims a number, and
neither does `bm25_index_open_readable`'s. A count beside a loop that computes it is a
maintenance liability with no upside; the suite pins the membership, which is the thing
worth pinning.

**Correct the two-tier privilege claim.** README said the debug surface "is owner-only
on top of that". It is not, and deliberately so: the few probes that *write* pages
require ownership, while the much larger read-only set requires `SELECT` on the indexed
table — the same rule `bm25_stats` follows, for the same reason. The docs now describe
both tiers.

## Alternatives considered

- **Rename `bm25_wand_stats` to `bm25_debug_wand_stats`** — the one-line fix, and it
  addresses this instance while leaving the mechanism intact. The next function named
  outside the convention reopens the hole, and the test still could not detect it.
- **Keep the prefix match and add `OR proname = 'bm25_wand_stats'`** — same objection,
  with the added cost of a special case that reads like an accident.
- **Tighten the read-only probes to owner-only so README becomes true** — considered
  and rejected on its merits, not on effort: reading an index's contents is a reasonable
  thing for a non-owner who can already read the underlying rows, which is exactly the
  argument `bm25_stats` is built on. Making the docs describe the code was the correct
  direction here; the code is right.
- **Assert an exact count of revoked functions in the test** — the failure mode this
  record exists to remove. Every count in this area has been wrong at least once.

## Residual, stated rather than implied closed

The loop still selects on a **name prefix** (`bm25%`), so a C function this extension
installed under some other name would escape the loop, the pinned list, and the suite's
sanity floor — all three, which is the same shape as the bug this record replaces, one
notch further out. Every function installed today matches. The honest closure is to key
on `pg_depend` extension membership rather than on a name; that is a larger change and
is not made here.

Two smaller notes on the same theme. Defaulting to closed cuts the other way from the
old comment: any unrelated C function named `bm25%` co-installed in this schema now has
its `PUBLIC` grant revoked at `CREATE EXTENSION` time. And the pinned expected output
contains a platform-dependent constant or two (a `MaxAllocSize`-derived `k` ceiling, a
`BLCKSZ`-derived bound), which is the count-rot pattern relocated into a place where at
least CI checks it.

## Consequences

- `bm25_wand_stats` is revoked from `PUBLIC`. It remains reachable by an explicitly
  granted role, and its C-level gate (`SELECT` on the indexed table, plus AM identity)
  is unchanged — this closes the second layer, which was the one missing.
- The pinned list makes the public surface a reviewed artifact. It is 17 declarations
  over 14 names today; that number appears in expected output, where it is checked, and
  nowhere in prose, where it would rot.
- Adding a public function now requires editing both the install script's allowlist and
  the expected output. That is friction by design.
- `ARCHITECTURE.md`'s claim that every SRF in `bm25_debug.c` is revoked was false when
  written; it is true as a consequence of this change, and now says by what mechanism.
