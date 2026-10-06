# bm25_native

A native PostgreSQL **index access method** (`USING bm25_native`) for Okapi BM25 ranked
full-text search. Pure C against stock PostgreSQL via PGXS — no external search
engine, no separate runtime, no Rust. The whole index lives in the index
relation's own pages, so it inherits WAL, crash recovery, and physical
replication from core.

> **On-disk compatibility.** Starting with disk format version 6, the index
> format carries a forward/backward compatibility contract, and that contract
> applies to every disk format version from 6 onward: additive format changes
> do not require a `REINDEX` (see "Compatibility and upgrades" below). The
> current disk format version is later than 6; this file deliberately does not
> name it, because `BM25_FORMAT_VERSION` in `src/bm25_format.h` is the single
> source of truth and a number written down here would go stale at the next
> bump.

## Requirements

- **PostgreSQL 17 or 18**, with the matching server development headers
  (`postgresql-server-dev-NN` on Debian/Ubuntu, or a source/`pg_config`-visible
  install). CI builds and tests against both majors. A non-blocking CI leg also
  builds and tests against PostgreSQL 19 pre-GA (currently 19beta4); PG19 is
  tested but not yet a supported major.
- A C toolchain (`gcc`/`clang`, `make`) and `pg_config` for the target install.

Minimum PostgreSQL 17 — PG16 is not supported (its planner cannot order-then-tiebreak
an `amcanorderbyop` index scan; see the ranking notes).

## Build and install

The build uses PGXS, resolved through `pg_config`. If `pg_config` for your target
PostgreSQL is on `PATH`:

```sh
make
sudo make install      # installs into the cluster's pkglibdir / sharedir
```

If it is not on `PATH`, point at it explicitly (this repo's local dev install,
for example):

```sh
make         PG_CONFIG=/usr/local/pgsql/bin/pg_config
make install PG_CONFIG=/usr/local/pgsql/bin/pg_config
```

Then, in the database:

```sql
CREATE EXTENSION bm25_native;
```

The extension is not relocatable: `CREATE EXTENSION bm25_native SCHEMA x` works, but
a later `ALTER EXTENSION bm25_native SET SCHEMA` is refused. (The access method name
is global to the cluster whatever schema the functions live in.)

## Usage

Create a BM25 index on a `text` column and query it with the two operators:

```sql
CREATE TABLE docs (id int PRIMARY KEY, body text);
INSERT INTO docs VALUES
  (1, 'the quick brown fox'),
  (2, 'quick foxes leap'),
  (3, 'lazy dogs sleep');

CREATE INDEX docs_bm25 ON docs USING bm25_native (body);

-- @@@  : match — true if the document and query share any token (OR semantics)
SELECT id FROM docs WHERE body @@@ 'quick fox';

-- &@@  : rank — ORDER BY ... LIMIT yields documents in descending BM25 score
--        as an ordered index scan (no Sort node)
SELECT id, bm25_score(ctid) AS score
FROM   docs
WHERE  body @@@ 'quick fox'
ORDER  BY body &@@ 'quick fox'
LIMIT  10;
```

- **`@@@ (text, text)`** — match operator (OR over query tokens). Usable in
  `WHERE` and as the executor recheck. Since 0.3, `@@@` carries real planner
  selectivity and evaluation cost, so ranked queries choose the index path
  without `enable_seqscan` workarounds.
- **Text query syntax.** A text query is a set of words (OR) with two optional
  forms. A **field scope** is a run with no whitespace or `"`, ended by a colon,
  after optional leading whitespace: `'body:cat'`, `' body: "red car"'`. If the
  run names no field of the index, a colon followed by `/` or a digit is plain
  text (`'http://example.com'`, `'10:30'`) and anything else raises
  `unknown search field`, so a typo like `'titel:cat'` is caught. To search
  text of that shape, or a field whose name contains whitespace, split it into
  words and pass each as `bm25_term(field, word)` under
  `bm25_boolean(should => ...)` (builders below). A **phrase** is a quoted
  string at the start (after any scope), optionally followed by `~n` (up to n
  intervening tokens, any order) or `~>n` (the same, in order); surrounding
  whitespace is ignored, and a quote anywhere else is ordinary text.
- **`@@@` answered by the index vs. applied as a filter.** On a multi-column
  index, the bm25 index answers `title @@@ 'apple'` from *every* indexed field,
  and `title @@@ 'body:apple'` from the `body` field only. When the planner
  applies `@@@` as a filter instead, there is no index to consult — this happens
  with `enable_indexscan = off`, with `col @@@ q OR …` (the index has no bitmap
  scan), and on a table with row-level security for any role a policy applies
  to (unless the policy is the constant `true`). Then the operator sees only the left-hand column's value:
  - a bare query matches that column alone, so on a multi-column index it can
    return fewer rows than the index path;
  - a scope-shaped query (see the text syntax above) raises
    `feature_not_supported` rather than answering a different question. That
    includes `'http://example.com'` and `'10:30'`, which the index path answers
    as plain text: off the index they cannot be told from a real scope such as
    `'title:2024'`, so the operator refuses rather than guess;
  - a quoted phrase raises `feature_not_supported` as well;
  - the default English analyzer is used whatever the index's `language`.
- **`&@@ (text, text)`** — order-by operator; drives ranked top-N via
  `amcanorderbyop`. Pair it with a `WHERE … @@@ …` filter as shown: the filter is
  what lets the index answer the query. `ORDER BY body &@@ 'q'` on its own does not
  use the index (not even with `enable_seqscan = off`); it runs as Seq Scan + Sort
  and every row's distance is `Infinity`, so the order is arbitrary. The jsonb form
  still rejects a structurally malformed query tree in that case; it does not resolve
  field names or reject a phrase inside `must_not`, so those are not caught there.
- **`@@@` selects, `&@@` orders — and they may name different queries.** The index
  returns exactly the rows the `WHERE` selects, as any other plan would: every `@@@`
  condition applies, several are ANDed, and an `ORDER BY` never removes a row. They
  come back in `&@@` order, and the selected rows that the `&@@` query does not match
  come last, with distance `Infinity` and a NULL `bm25_score`. So
  `WHERE body @@@ 'cat' ORDER BY body &@@ 'dog'` returns every 'cat' document, the
  ones that also mention 'dog' first. To rank within one field of a multi-field
  index, scope the `WHERE` as well:
  `WHERE title @@@ 'body:"a b"' ORDER BY title &@@ 'body:"a b"'`. Performance: when
  every `@@@` query is byte-for-byte the `&@@` query (the usual form) the scan uses
  the WAND top-k path; when any differs it scores every matching document of each
  query (no WAND), which costs more on common terms. Each of those queries is held to
  `bm25_native.max_match_memory` on its own; they are evaluated one at a time, so
  such a scan peaks at about twice that setting, however many `@@@` queries it has.
- **`bm25_score(tid)`** — the BM25 score of the current scan's row (e.g.
  `bm25_score(ctid)` in the target list of a ranked query). See
  [Score accessors](#score-accessors) for the other forms and when to use them.
- **`@@@ (text, jsonb)` / `&@@ (text, jsonb)`** — the same two operators over a
  composable query tree, built by function call (`bm25_term`, `bm25_boolean`,
  `bm25_phrase`, `bm25_wildcard`, `bm25_boost`) so user input lands as a value,
  never as syntax.

### Score accessors

A score exists only while a ranked (`ORDER BY … &@@ …`) bm25 index scan is running,
so the accessors read it from that scan. There are two families:

| Accessor | Finds the scan by |
| --- | --- |
| `bm25_score(ctid)`, `bm25_score_key(key)` | the row alone |
| `bm25_score(ctid, query)`, `bm25_score(ctid, query, tableoid)`, `bm25_score_key(key, query)` | the query the scan ranks by, and optionally the table |

`bm25_score_key` takes the index's `key_field` value (`int`, `bigint`, `uuid` or
`text`) and needs an index with a `key_field`. `query` must be byte-identical to the
right-hand side of the scan's `&@@`. For `text` that means the same string: a
differently spelled query, even an equivalent one, matches no scan and gives NULL.
For `jsonb` the bytes compared are jsonb's stored form, so key order and whitespace
do not matter but, for example, a number's written scale (`1.0` against `1.00`)
does. Pass a jsonb query as `jsonb` (a builder call, or a literal cast with
`::jsonb`): an uncast literal is taken as `text` and matches no jsonb-ranked scan.

**The row-only forms** are correct in a query with one ranked scan, and when they
are projected directly on their own scan's rows, even with other ranked scans in
the same statement (a correlated subquery, for example). With several ranked scans
they can return **another scan's score**, a plausible wrong number with no error, in
three shapes:

- a join of two ranked subqueries on the row's key or ctid, with the accessor above
  the join;
- a projection decoupled from its scan: a PL/pgSQL `FOR` loop (which fetches rows
  ahead of the loop body), a `Sort` or `Materialize` above the scan, a cursor;
- a ranked query on a partitioned or inheritance parent, where the same ctid can
  exist in several children.

**The query-qualified forms** name the scan, so none of those shapes confuses them.
Each scan ranking `query` is asked whether its ranking holds the row. If one does,
or several do with the same score, that score is returned. If two hold it with
different scores, the result is NULL rather than a guess. On a partitioned or
inheritance parent a ctid repeats across children, so pass `tableoid` as well:

```sql
SELECT a.id,
       bm25_score(a.ctid, 'quick fox') AS fox_score,
       bm25_score(b.ctid, 'fox leap')  AS leap_score
FROM   (SELECT id, ctid FROM docs WHERE body @@@ 'quick fox'
        ORDER BY body &@@ 'quick fox' LIMIT 50) a
JOIN   (SELECT id, ctid FROM docs WHERE body @@@ 'fox leap'
        ORDER BY body &@@ 'fox leap' LIMIT 50) b USING (id);

SELECT id, bm25_score(ctid, 'quick fox', tableoid) AS score
FROM   docs_partitioned
WHERE  body @@@ 'quick fox'
ORDER  BY body &@@ 'quick fox'
LIMIT  10;
```

They answer from the scan's current ranking for as long as the scan exists, which
is until its statement or cursor finishes, so a projection above a `Sort` still
resolves after the scan has returned its last row. Three limits follow from reading
the ranking rather than the emitted value:

- A correlated subquery's scan re-ranks on every rescan. A row it emitted on an
  earlier rescan is answered from the new ranking, which holds it with the same
  score when the query does not depend on the outer row.
- Two ranked rows sharing a key, from a non-unique `key_field` or from `text` keys
  that agree in their first 16 bytes, give `bm25_score_key(key, query)` a NULL when
  their scores differ. Use the ctid form there.
- A ranked scan served by block-max WAND that is read past `bm25_native.wand_top_k`
  rows re-ranks the whole match set at that point. If the index changed in between
  (rows inserted in the same transaction, or another session's commit, even under
  `REPEATABLE READ`), rows already returned are scored under the new corpus
  statistics, which can differ from the value the scan returned. The row-only
  forms read the same ranking.

**Where possible, compute the score next to the scan.** Put the accessor in the
target list of the query that does the ranking and carry the column outward, rather
than calling an accessor above a join, a sort or a loop. `-(body &@@ 'quick fox')`
is the same score (the operator's distance, negated) and needs no ctid or key. The
query-qualified forms are for scores that must be computed away from the scan.

**Roles and privileges.** Every backend keeps one list of the ranked scans it has
open, and the accessors are executable by everyone. So that one role cannot read
another role's ranking through them (for example a `SECURITY DEFINER` search
function whose scan is still open while the caller's expressions run: a
`LANGUAGE sql` set-returning function in the caller's target list, or a refcursor
it returns), the accessors only see scans started under the caller's current user
id. The score, key and snippet accessors additionally require that the caller can
read the scanned table: `SELECT` on the table, on a partitioned table it is a
partition of (a grant on a partitioned table covers its partitions; a legacy
`INHERITS` parent's grant does not, since a child can add columns), or on every
column the index reads (column grants count only on the scanned table itself, not
on a partitioned table above it); and no row-level security policy in force for the caller
on the table or a partitioned table above it. A scan that fails the check is
ignored as if it were not open: `bm25_score` and `bm25_score_key` return NULL
unless another scan the caller may read answers, and `bm25_snippet` raises an
error when the only scans open are ones the caller may not read. Consequences for
ordinary use:

- A role that ranks through a view it was granted, without `SELECT` on the
  underlying table, gets NULL from the score accessors. `&@@` itself is not
  checked, so the ranking and `-(body &@@ query)` still work; use that for the
  score.
- Row-level security refuses the accessors even when the policy lets the caller
  see every row (a policy that folds to `true`, the one policy under which the
  caller's own `@@@` uses the index). The ranking itself is unaffected.
- A cursor's scan belongs to the role that first fetched from it. If the current
  role changes between fetches, the accessors on later rows return NULL (`&@@`
  degrades to `Infinity`).
- `&@@` is checked only for ownership, not privilege. A refcursor that a
  `SECURITY DEFINER` function opens and the caller fetches from belongs to the
  caller, so `&@@` in the caller's own query can still project that scan's
  current-row distance, which can be the score of a row the function filtered
  out. The caller cannot choose which row.

### Composable query builders

The `@@@` and `&@@` operators also accept a **jsonb query tree** on their
right-hand side. You build that tree by calling the functions below rather than by
assembling a query string, so a user-supplied value — even one that reads like
`AND`, `OR`, or a quote — can only ever land as data inside the tree, never as
query syntax. Each function returns one jsonb node; `bm25_boolean` and `bm25_boost`
take jsonb sub-queries and so nest to any depth. All are `IMMUTABLE PARALLEL SAFE`
and touch neither the catalog nor the tokenizer — they only assemble their
arguments into jsonb. `bm25_boost` records its weight as a `numeric` rather than a
`float8` so the rendering does not depend on `extra_float_digits`, which is what
makes the `IMMUTABLE` claim true; the weight is therefore kept to 15 significant
decimal digits. Every builder and operator described in this section carries a
`COMMENT`, as does the rest of the `PUBLIC`-executable surface, so `obj_description()`
and `\df+` give the same summary in-database. The `bm25_debug_*` probes deliberately
do not: they are test surface, not API, and `\df+ bm25_*` lists them too.

| Builder | Returns a node that… |
| --- | --- |
| `bm25_match_terms(field, terms)` | matches the analyzed OR of the terms of `terms` in `field` (the only OR-of-terms leaf) |
| `bm25_term(field, value)` | matches `value` as a single analyzed term in `field` |
| `bm25_phrase(field, phrase, slop int = 0, ordered bool = true)` | matches `phrase` as a phrase; `slop` allows up to that many intervening tokens (0 to 100000, the same range the text syntax's `~n` suffix accepts), `ordered` requires left-to-right order |
| `bm25_wildcard(field, pattern)` | matches a prefix wildcard such as `'judg*'` in `field` |
| `bm25_boolean(must jsonb[] = '{}', should jsonb[] = '{}', must_not jsonb[] = '{}')` | requires all of `must`, rewards any of `should`, and excludes anything matching `must_not` |
| `bm25_boost(weight float8, query)` | wraps `query`, scaling its contribution to the score by `weight`; the product of the weights enclosing each scoring leaf must lie in [1e-6, 1e6] (a leaf under `must_not` is exempt, since its weight is unused) |

The builders are `STRICT`: a NULL argument makes the builder return NULL, so
`col @@@ bm25_term(NULL, 'x')` matches nothing, and a NULL inside a `bm25_boolean`
array is an error naming the element. A leaf over **all** fields therefore needs raw
jsonb with `"field"` omitted, such as `'{"term": {"value": "red"}}'::jsonb`. Raw
jsonb is checked as strictly as builder output: each node accepts only the keys its
builder emits (`match`: field, terms; `term`: field, value; `phrase`: field, phrase,
slop, ordered; `wildcard`: field, pattern; `boolean`: must, should, must_not;
`boost`: weight, query), so a misspelled key is an error rather than silently
ignored; `slop` must be a whole number; and a tree may have at most 64 leaves and
1024 nodes in all.

On a multi-field index, anchor `@@@` and `&@@` on the index's **first indexed
column** (`title` below); the tree inside targets whatever fields its builders
name. Pass the same tree to both operators — `@@@` selects the rows, `&@@` orders
them; a different tree on each side is answered exhaustively (see above).

```sql
-- A multi-field index that scores and returns by your own key column:
CREATE INDEX docs_bm25 ON docs
  USING bm25_native (title, summary, body) INCLUDE (id) WITH (key_field = 'id');

-- Prefix wildcard: documents whose body has a term starting "judg".
SELECT id FROM docs
WHERE  title @@@ bm25_wildcard('body', 'judg*')
ORDER  BY title &@@ bm25_wildcard('body', 'judg*')
LIMIT  10;

-- Boolean: the body must match "tort" but must not match "battery".
SELECT id, bm25_score_key(id) AS score
FROM   docs
WHERE  title @@@ bm25_boolean(
                   must     => ARRAY[ bm25_term('body', 'tort') ],
                   must_not => ARRAY[ bm25_term('body', 'battery') ])
ORDER  BY title &@@ bm25_boolean(
                   must     => ARRAY[ bm25_term('body', 'tort') ],
                   must_not => ARRAY[ bm25_term('body', 'battery') ])
LIMIT  10;

-- Fan one term across fields, weighting the title hit 3× (query-time boost).
SELECT id, bm25_score_key(id) AS score
FROM   docs
WHERE  title @@@ bm25_boolean(should => ARRAY[
                   bm25_boost(3.0, bm25_term('title',   'mandamus')),
                   bm25_term('summary', 'mandamus'),
                   bm25_term('body',    'mandamus') ])
ORDER  BY title &@@ bm25_boolean(should => ARRAY[
                   bm25_boost(3.0, bm25_term('title',   'mandamus')),
                   bm25_term('summary', 'mandamus'),
                   bm25_term('body',    'mandamus') ])
LIMIT  10;

-- Phrase with proximity: "tort" and "negligence" within two tokens, any order.
SELECT id FROM docs
WHERE  title @@@ bm25_phrase('body', 'tort negligence', 2, false)
ORDER  BY title &@@ bm25_phrase('body', 'tort negligence', 2, false)
LIMIT  10;
```

`docs/grammar-mapping.md` maps each higher-level query form (Boolean,
proximity, field scoping, wildcards) to the builder call that expresses it.

### Beyond single-field keyword search

The same index also supports, without leaving PostgreSQL:

- **Multi-field / BM25F** — one index over several columns with per-field boosts and
  per-field length normalization. Add a `key_field` via `INCLUDE` to score and return
  by your own key (`bm25_score_key(id)`) instead of `ctid`.
- **Phrase, proximity, and snippets** — exact phrases and `~n` / `~>n` proximity, plus
  `bm25_snippet()` highlighted excerpts (on an index built with positions). Snippets
  HTML-escape the field text by default and size their window in characters — see
  **Snippet output** below.
- **Boolean and wildcard queries** — `must` / `should` / `must_not` trees and prefix
  wildcards, composed through the jsonb builders above.

See [ARCHITECTURE.md](ARCHITECTURE.md) and `docs/grammar-mapping.md` for the
full query surface.

BM25 parameters default to `k1 = 1.2` (reloption `k1`, range `[0, 1e30]`) and
`b = 0.75` (reloption `b`, range `[0, 1]`); either can be overridden per field with
`k1_<col>` / `b_<col>` / `boost_<col>` (boost defaults to `1.0`, no index-wide
knob). **k1/b/boost are live**: `ALTER INDEX ... SET`/`RESET` on any of them
takes effect on the next scan, with no `REINDEX`. Text is analyzed through
PostgreSQL's own Snowball dictionaries via `ts_lexize` — split → lowercase →
stopword removal → stemming — identically at index and query time, so a query
for `negligent` matches a document indexed as `negligence`. The analyzer is fixed at
`CREATE INDEX` (reloptions `language`, `stopwords`, `tokenizer`; defaults
`language = 'english'`, `stopwords = 'default'`). There is also an `analyzer`
reloption, which is **reserved surface and selects nothing** — there is one
analyzer pipeline — so it accepts only its default `'english'` and is rejected
otherwise rather than being silently ignored. Every one of these is validated at
`CREATE INDEX`/`ALTER INDEX` time, including `language`, whose dictionary is
resolved there so a name that does not exist fails at the statement rather than at
the next scan. The analyzer's fingerprint is
stamped into the index, so a scan whose analyzer no longer matches fails loud
rather than silently under-returning; the analyzer and `store_positions` both
still require a `REINDEX` to take effect, since (unlike k1/b/boost) they change
what a query can find rather than how a found document is scored.

`key_field` is fixed at `CREATE INDEX` too, and enforced: the build records which
column it keys on (its type, width and position), and after an
`ALTER INDEX ... SET (key_field = ...)` or `RESET (key_field)` every `INSERT` is
refused until the previous `key_field` is restored or the index is rebuilt with
`REINDEX`, which keys it on the new column. An index built by a version before this
record existed is checked only against its first sealed segment, so until its first
seal it admits a changed `key_field`; `REINDEX` it to get the full check. If such an
index already queued rows under two `key_field` settings, sealing it (including
VACUUM) fails with "pending list ... mixes key_field configurations"; `REINDEX`
is the cure, and `VACUUM (INDEX_CLEANUP off)` gets a table's VACUUM past it meanwhile.

`language` names a text-search dictionary as `<language>_stem`, so it is not
limited to the Snowball set — but PostgreSQL 17+ runs `CREATE INDEX` with
`search_path` restricted to `pg_catalog, pg_temp`, and non-relation catalog
lookups skip the temp namespace. A dictionary in a user schema is therefore not
resolvable at build time even with that schema in the session `search_path`;
only `pg_catalog` (the Snowball dictionaries, plus anything a superuser installs
there) resolves. A dictionary that emits several lexemes per word — an ispell
compound splitter — is supported: every distinct lexeme becomes its own
token, and every lexeme a given source word produces shares that word's position,
matching `to_tsvector` (a lexeme repeated within one word, such as ispell's `klubber`
in `footballklubber`, is dropped rather than double-counted). Two consequences follow,
both the deliberate cost of parity with core rather than a defect:

- **A phrase slot is satisfied by any lexeme its word produced.** `'"footballklubber"'`
  also matches a document containing only the separate words `football klubber`,
  because the phrase's one slot is filled by any lexeme the analyzer emitted for
  it — a recall gain, and the same match core's `phraseto_tsquery` alternatives give.
  The converse does not hold and is worth stating so it is not assumed: `'"football
  klubber"'` still matches only a document with those two words, never one whose sole
  occurrence is the compound, because the analyzer's own lexemes for `footballklubber`
  never place `klubber` at the slot after `football`'s. That was true before this change
  too — no query lost a match to it.
- **A word contributes 1 to BM25 length normalization, not its lexeme count.** A
  document is no longer length-penalised merely for containing a word its dictionary
  happens to decompose into several lexemes.

Two narrower divergences from core remain, deliberately, rather than by oversight: under
`stopwords = default` a dropped stopword consumes no position here, where core lets it
consume one; and a phrase slot ORs together every lexeme its word produced, where core
ANDs the lexemes of one variant and ORs the variants — `bm25_analyze` discards the
information needed to reconstruct the chains. Both are recall-only: neither can drop a
match core would find. See
[`docs/adr/0087`](docs/adr/0087-analyzer-revision-5-per-run-positions.md).

Words are split and case-folded by **character**, not by byte, so accented and
non-Latin text is indexed whole rather than shredded at each multibyte character.
In a single-byte server encoding (`LATIN1`, `WIN1251`, `KOI8R`, …) a non-ASCII
byte is a word character exactly when the encoding maps it to a letter or digit,
decided by a fixed table generated from PostgreSQL's own conversion maps at a pinned
Unicode version — not by `LC_CTYPE`, and not by the server's Unicode tables, which
change between major versions. In `SQL_ASCII` every non-ASCII byte is a word
character, so unchecked UTF-8 stored there is indexed whole too.
The fold is `str_tolower` under the database default collation — on PostgreSQL 18
the very call the Snowball dictionary makes on its own input, and the same one
`to_tsvector` gets. (On PostgreSQL 17 the dictionary folds through `LC_CTYPE`
instead of the collation's provider; the two agree for a database created the
ordinary way under libc. See [`docs/adr/0046`](docs/adr/0046-encoding-aware-tokenization-and-folding.md).)
Two consequences follow from it being the *database's* fold rather than a fixed
one. First, matching an accented term written in a different case works where the
collation folds non-ASCII and not under a `C` collation, which folds ASCII only;
this is exactly how `to_tsvector` behaves in the same database. Second, a
collation-provider upgrade is a `REINDEX` event for a bm25 index over non-ASCII
text, as it already is for a collated btree.

Note that wildcards bypass the stemmer by design, so an accented prefix reaches
only what the stemmer left intact. Under a folding collation `language = 'german'`
stems *and transliterates* `Ärger` to `arg`, and no accented prefix matches that —
the same way `runnin*` misses a document stored as `run`. Exact-term queries are
unaffected; they go through the stemmer on both sides.

> **Upgrading across this change: `REINDEX`.** Earlier builds split and folded
> byte-wise, which stored `Ärger` as `rger` on Linux and could not build the index
> at all on macOS with a UTF-8 `LC_CTYPE`. Stored terms therefore change, so the
> analyzer fingerprint changes with them and an older index is refused with
> `analyzer fingerprint mismatch ... REINDEX` rather than quietly
> returning nothing for its accented terms. The refusal covers pure-ASCII indexes
> too, whose content is in fact identical under both analyzers: a fingerprint cannot
> tell whether an index happens to contain a non-ASCII term, and a loud REINDEX for
> everyone is the better trade than a silent wrong answer for some. See
> [`docs/adr/0046`](docs/adr/0046-encoding-aware-tokenization-and-folding.md).
>
> **Size the window for a write outage, not just degraded reads.** Since
> [`docs/adr/0081`](docs/adr/0081-the-ingest-path-is-gated-by-the-analyzer-fingerprint.md)
> the same gate runs on `INSERT`, so an un-REINDEXed index refuses every insert and
> every non-HOT update as well as every scan. Only `REINDEX` restamps the fingerprint —
> `bm25_upgrade()` does not touch it.

> **Also `REINDEX`, and this one is only bookkeeping.** The fingerprint used to
> identify the stemming dictionary by its catalog OID, which is a number `initdb`
> allocates rather than a pinned constant — so a `pg_upgrade` into a cluster that
> numbered `english_stem` differently made every index in the database fail with
> `analyzer fingerprint mismatch ... REINDEX` even though nothing
> about the analyzer had changed. (A logical dump/restore was never affected:
> `pg_dump` emits `CREATE INDEX`, so the restore rebuilds and re-stamps.) The
> dictionary is now identified by its schema-qualified name and template, which
> `pg_upgrade` does not disturb. (Whether the dictionary *behaves* the same after a
> `pg_upgrade` is a separate question, and not always yes — see the next note.)
> Existing indexes are refused once, on the upgrade to this release, and a
> `REINDEX` clears it. Unlike the change above, tokenization here is
> byte-identical: the stored segments are already correct and the only stale thing
> is the number stamped on the metapage, so setting
> `require_analyzer_match = false` is a safe way to defer the reindex for *this*
> transition specifically. See
> [`docs/adr/0080`](docs/adr/0080-stemmer-identity-is-the-dictionary-name-not-its-oid.md).
>
> **The refusal covers writes too, which is what makes the deferral worth knowing
> about.** Since
> [`docs/adr/0081`](docs/adr/0081-the-ingest-path-is-gated-by-the-analyzer-fingerprint.md)
> the gate also runs per inserted row, so without either a `REINDEX` or
> `require_analyzer_match = false` an un-migrated index is effectively read-only:
> every `INSERT` and every non-HOT `UPDATE` is refused, not just every scan. On the
> deferral path the cost is one `WARNING` per index per transaction — deliberately
> memoized, so a bulk load emits one line rather than one per row. Because
> tokenization is byte-identical across *this* transition, rows written under the
> warning are correctly analyzed and remain findable; the warning hedges only because
> the gate compares fingerprints, not tokens, and cannot tell this transition from one
> that did change the stemmer.

> **`REINDEX` again, and this time `require_analyzer_match = false` is NOT a safe
> deferral.** `bm25_analyze` now advances position once per source word instead of once
> per emitted lexeme, and deduplicates a word's repeated lexemes — a genuine
> tokenization change, unlike the bookkeeping-only fingerprint move above. For an index
> whose dictionary emits one lexeme per word (the Snowball languages — `english` and
> friends) tokenization is byte-identical and nothing about this transition matters. For
> an index over a compound-splitting dictionary (`ispell`/`hunspell`) it is not: the stored terms and positions were written by the old analyzer and
> disagree with the new one answering the query. Setting `require_analyzer_match = false`
> here does not defer a bookkeeping refusal the way it safely does above — it queries a
> mismatched index and gets **wrong results**, not merely stale ones. `REINDEX` is the
> only correct response for a multi-lexeme dictionary crossing this boundary. See
> [`docs/adr/0087`](docs/adr/0087-analyzer-revision-5-per-run-positions.md).

> **`REINDEX` once more (analyzer revision 6), and in two cases it repairs real
> damage.** The fingerprint now also hashes what the stemming dictionary actually
> *does* to a fixed list of probe words, not only which dictionary it is. A dictionary
> can change behind an unchanged name: PostgreSQL 18 shipped new Snowball sources,
> and six English stems differ from PostgreSQL 17 (`added` and `adding` stem to `add`
> instead of `ad`; `egging`, `erring`, `offing` likewise), so an index carried across
> a 17 → 18 `pg_upgrade` silently stopped finding those words. The same probe catches
> `ALTER TEXT SEARCH DICTIONARY … (StopWords = …)` in place, in the same session. The
> new component moves every stored fingerprint, so every index is refused once after
> upgrading to this release, and `REINDEX` clears it.
>
> - **Single-byte databases** (`LATIN1`, `WIN1251`, …, and `SQL_ASCII`): tokenization
>   genuinely changes — non-ASCII letters used to be separators, which indexed nothing
>   for Cyrillic and split `Ärger` into fragments that matched `Bürger`. Rebuild;
>   `require_analyzer_match = false` would query the old terms.
> - **Indexes `pg_upgrade`d from PostgreSQL 17**: the rebuild re-stems under the new
>   stemmer, which is the repair.
> - Everything else (UTF-8, built on the current major): stored terms are unchanged and
>   the `REINDEX` only restamps.
>
> The probe has a new consequence worth planning for: a collation or library upgrade
> (ICU, glibc) that changes how a probe word folds, or a stemmer change that touches a
> probe word, makes the gate refuse the index until `REINDEX`. That is the gate doing
> its job — the stored terms were written under the old behaviour — but it can now
> happen on an OS or `pg_upgrade` upgrade with no change to the index itself. A change
> confined to words *outside* the probe list remains invisible to the gate.
>
> One consequence for custom dictionaries: a thesaurus dictionary used as the
> `<language>_stem` dictionary now fails at `CREATE INDEX`, even on an empty table,
> because the fingerprint lexizes it. It was already refused at `INSERT`.

### Maintenance and introspection

```sql
SELECT * FROM bm25_stats('docs_bm25');   -- ndocs, sealed_avgdl, k1/b, segment + pending
                                          -- counts, and format/analyzer metadata
SELECT bm25_seal('docs_bm25');           -- drain the pending list into a segment now
SELECT bm25_merge('docs_bm25');          -- force a tiered merge pass now
SELECT bm25_upgrade('docs_bm25');        -- online format upgrade (see below)
```

**Privileges.** `bm25_seal`, `bm25_merge` and `bm25_upgrade` mutate the index, so
they require **ownership** of it — the same rule core applies to
`gin_clean_pending_list`, and on a standby they fail with SQLSTATE 25006 like any
other write. `bm25_stats` only reports, so it requires **SELECT on the
indexed table** instead, which is the privilege that would let the caller see the
underlying rows anyway. If a row-level security policy applies to the caller on that
table, it is refused (42501): its whole-index figures would cover rows the policy
hides. `bm25_wand_stats` applies that same SELECT check, but it is
*not* callable by an ordinary user at all: it is a debug probe that happens not to
carry the `bm25_debug_` prefix, and the revoke block names it explicitly for that
reason. All of them additionally verify that the target really is a `bm25_native`
index, so aiming one at a btree fails cleanly rather than writing bm25 structures onto
its pages.

The `bm25_debug_*` surface is revoked from `PUBLIC` outright, and underneath that it
follows the **same two-tier split** as the functions above rather than being uniformly
owner-only: the handful of probes that *write* pages require ownership, while the much
larger read-only set requires SELECT on the indexed table, for exactly the reason
`bm25_stats` does — reading an index's contents is reasonable for someone who can
already read the rows. Every one of them verifies the AM as well. The authoritative
list of what is callable by whom is pinned, by name, in
`sql/63_debug_privileges.sql`; it is deliberately not restated here, because a count
in prose beside a loop that computes it goes stale silently and this one did.

Inserts land in a WAL-logged pending list and are sealed into immutable segments
automatically at a size threshold (the `bm25_native.seal_threshold` GUC, in KB) and
during `VACUUM`; `bm25_seal()` forces a seal immediately. `VACUUM` also tombstones
deleted rows and reclaims orphaned pages.

**Do not run `bm25_seal`, `bm25_merge` or `bm25_upgrade` while a `VACUUM` of a
write-heavy table is removing dead rows from its bm25 index.** That is `VACUUM`'s
index-vacuuming pass. PostgreSQL runs it only when the table scan found dead rows, and
skips it when `INDEX_CLEANUP` is off or once the wraparound failsafe has engaged. Under
the default `INDEX_CLEANUP auto` it can also bypass the pass, but only before any pass
has run in that `VACUUM`, and only when fewer than 2% of the table's pages hold dead
rows *and* the dead-row list takes under 32 MB. A `VACUUM` whose dead-row list
outgrows its memory runs the pass more than once. Each such pass holds the index's
seal/merge lock from start to finish, so an explicit
call waits for that pass, and while it waits, new `INSERT`s into the index that need
the same lock queue behind it (PostgreSQL's lock manager will not grant new shared
requests past a waiting exclusive one). A manual `bm25_merge()` issued during a long
index-vacuuming pass can therefore stall the table's insert traffic until the pass
ends. To spot it, look for `pg_locks` rows with `locktype = 'page'` on the index,
`page = 0` (the metapage) and `granted = false`, in mode `ExclusiveLock` for the
maintenance call and `ShareLock` for each queued insert. Cancelling the waiting call
(`pg_cancel_backend`) releases the inserts. The opportunistic seal that an insert
triggers when the pending list passes `seal_threshold`, and the opportunistic merge
that `VACUUM` runs at the end, skip rather than wait, so ordinary insert-driven
sealing does not cause this. If the `VACUUM` is an autovacuum, a waiting call
cancels it after `deadlock_timeout`, as with any lock conflict; a manual `VACUUM` or
an anti-wraparound autovacuum is not cancelled that way.

Inserts also wait while an explicit seal, merge or upgrade *holds* the lock, not only
while it waits for it: every insert that adds a document takes the lock in shared
mode, so it blocks while any of those calls holds it, `VACUUM` or no `VACUUM`. Each
call holds it for its seal of the pending list; `bm25_merge`, and a `bm25_upgrade`
that rewrites segments, take it again for the merge or rewrite, releasing it in
between (and `bm25_merge` again between merge passes). And apart from that opportunistic merge, `VACUUM`'s end-of-run cleanup does
not skip: its seal of the pending list and its two page reclaims wait for the lock, so
a `VACUUM` (autovacuum included) that reaches cleanup while one of those calls holds
the lock waits for that hold to end. The lock-conflict cancel does not reach a
cleanup while it waits, but once an autovacuum's cleanup holds the lock, an insert
that waits on it cancels that autovacuum after `deadlock_timeout` (again, never an anti-wraparound one). See
[`docs/adr/0102`](docs/adr/0102-bulkdelete-holds-the-singleton-for-its-whole-pass.md).

The sweep for orphaned pages is the one cleanup pass whose length grows with the whole
index: it visits every page, throttled by the cost-based vacuum delay. It runs only when
the index records evidence that orphaned pages can exist: after a crash or a restart
(including a backend crash that makes the server reinitialise in place), after a seal,
merge, upgrade or reclaim that failed or was interrupted part-way, after any merge or
segment-rewriting upgrade (its catalog swap leaves the old catalog pages for the sweep),
and once on an index this release has not yet swept. It holds the index's lock in
shared mode, so inserts proceed while it runs. A blocking `bm25_seal()`, `bm25_merge()`
or `bm25_upgrade()` issued during the sweep still waits for it, inserts then queue
behind that waiting call, and if the `VACUUM` is an autovacuum the waiting call cancels
it after `deadlock_timeout`; the insert-triggered seal skips rather than wait.

The cleanup holds that still block inserts are bounded: the seal (by the size of the
pending list), the reclaim of merged-away segments (one merge's worth of segments per
hold, releasing the lock in between), and each pass of an explicit `bm25_merge()` (it
releases the lock between passes). The opportunistic merge that `VACUUM` runs is a single
pass and holds the lock for its whole length, so on a large index with a steady insert
rate an autovacuum whose merge pass outlasts `deadlock_timeout` while an insert waits is
cancelled (`canceling autovacuum task`, with context `while cleaning up index`): that
run's freeze progress and statistics are lost, and autovacuum retries later. Cleanup
runs the merge after the seal and the reclaim of merged-away segments and before the
sweep, so a cancelled merge loses only itself. If the log shows these cancellations for
a bm25-indexed table, a manual `VACUUM` of that table, or a `bm25_merge()`, in a
low-insert window lets the merge finish.

**Restart the server after installing a new release of this extension.** Backends that
already loaded the previous library keep running its code until they exit, and an
older backend neither records the evidence the orphan sweep now depends on nor shares
its locking protocol with newer ones. Orphaned pages left in such a window are reclaimed
by the first `VACUUM` after the next restart.

Building, sealing and merging all accumulate in memory, and all three respect
**`maintenance_work_mem`** — or `autovacuum_work_mem`, when the work is being done by
an autovacuum worker and that setting is configured. Crossing the budget is not an
error: the operation publishes the segment it has and starts a new one, so a corpus
larger than the budget simply produces more segments. The consequence worth knowing is
that a persistently small budget puts a floor under the segment count, because merging
budget-sized segments produces budget-sized segments; if scans slow down as the segment
count grows, raise `maintenance_work_mem` and run `bm25_merge()`. A family of `bm25_debug_*` functions
exists for tests and introspection (see `bm25_native--1.0.sql`).

### Queries on a hot standby

A query on a hot standby can fail with SQLSTATE `40001` (serialization failure),
`bm25: segment reclaimed concurrently; retry` or `bm25: pending list recycled
concurrently; retry`. The primary reuses an index page once no query **on the
primary** can still need it. Without `hot_standby_feedback` a standby query does not
count, so the primary can drain, free and reuse pages that a long standby query is
still reading. The index detects this and cancels the query rather than return
results built from a reused page. The index is not damaged, and running the query
again succeeds. On a standby bm25 does not retry this internally: a retry needs a
subtransaction, and inside one PostgreSQL turns its own recovery-conflict cancel
into a dropped connection. Treat it like those cancellations, which use the same
SQLSTATE, and retry.

`hot_standby_feedback = on` on the standby makes this much rarer, because the
standby's queries then hold the primary's reuse horizon back. It is a mitigation, not
a guarantee: the feedback is asynchronous and is lost while the standby is
disconnected. The cost is the usual one, more bloat on the primary while long standby
queries run.

### Snippet output

```sql
bm25_snippet(field text,
             start_tag text DEFAULT '<mark>',
             end_tag   text DEFAULT '</mark>',
             max_num_chars int DEFAULT 300,
             escape    boolean DEFAULT true) RETURNS text
```

**The field text is HTML-escaped by default.** `& < > " '` in the indexed column become
entities, so a result rendered as HTML shows stored markup as text instead of executing
it. This is a deliberate departure from `ts_headline`, which escapes nothing: the tag
defaults above mean the documented way to consume this function is to render it as
markup, and a safe default costs less than a caveat every caller must remember. Pass
`escape => false` for byte-verbatim field text — the right choice for a plain-text
excerpt in a CLI or a JSON payload, where entities would be noise.

**Any query shape is highlighted.** The highlighted terms come from every leaf of the
scan's query that is not under `must_not`: plain, term and phrase text as the index
analyzes it, and wildcard patterns matched against the field's own words. A phrase's
words are marked wherever they occur in the field, not only where they form the phrase,
and a field scope in the query is not applied to the snippet (it is given a value, not a
column).

**The caller's tags are never escaped.** They are markup by contract; escaping them
would render `<mark>` as visible text. Tags are a trust boundary you own — do not build
one out of user input.

**`max_num_chars` counts characters of the original text**, not bytes and not output
length. The same budget yields the same excerpt length in Japanese as in English. Tags,
ellipses, and the expansion of escaped characters are all outside the budget: an `&`
spends one character of budget and emits five bytes, so `escape` never changes which
passage is selected, only how it renders.

### Document size limits

Since **disk format version 7**, a document's postings may span pending pages, so the old
"one document must fit one pending page" ceiling — roughly 380 distinct 7-byte stems at
the default `BLCKSZ`, which `CREATE INDEX` did not share — is gone. A row that a rebuild
indexes is now also insertable, and article-length text inserts normally.

Two narrower limits remain, and both fail loudly at `INSERT` with
`ERRCODE_PROGRAM_LIMIT_EXCEEDED`:

- **~65,000 tokens per document.** A pending term entry stores its term frequency in 16
  bits, so a document's total token count must fit `uint16`. A 2000-word article is around
  2000 tokens.
- **One `(field, term)` entry, with its position list, must fit a single page.** An
  entry's term bytes and positions are contiguous and cannot be split. This is reachable
  only with a very high-multiplicity term — tens of thousands of occurrences of the *same*
  term in one column — not with a long document.

An index's `min_read_version` — the floor it advertises to older binaries — tracks
capabilities it has actually USED, not what the writing binary can do. Two pending-record
capabilities raise it, both in the same WAL record as the write that needs them: the
page-spanning documents of disk format version 7 (floor 7), and the per-field
document-length array of disk format version 8, which every pending record carries
(floor 8). An index that is built and never written to keeps
whatever floor it had, because a plain `CREATE INDEX` writes no pending records at all.
(`CREATE INDEX CONCURRENTLY` is different: its validate phase inserts the rows the build
missed, so it can raise the floor by itself.) See
`docs/adr/0038-pending-document-spanning.md` and
`docs/adr/0086-doclen-is-a-source-run-count-stored-per-field-in-pending.md`.

### Query memory limits

Ranking by BM25 is a global sort by score, so a scan that is not answered from the
WAND top-k has to hold **every** matching document in memory before it can return the
first row — and there is nothing to spill to. `bm25_native.max_match_memory` bounds
that (in KB, default **256 MB**; `0` means follow `work_mem`). The budget covers
everything a scan materializes — the score accumulator, the leaf-presence bitmasks,
the `@@@` union collector, and phrase position lists — as one running total, so no
single structure can spend it twice. (The total is per query a scan evaluates: a scan
whose `@@@` queries differ from its `&@@` query evaluates each one in turn and can
peak at about twice the setting, see [Usage](#usage).) Roughly 1.7 million matching documents for a
plain term query at the default; a phrase query costs more per document, because it
also holds each document's term positions. Exceeding the budget is an error naming
the setting, rather than the unbounded growth and cryptic allocator failure it
replaced.

If you hit it, the options in order of preference are:

1. **Add a `LIMIT` no larger than `bm25_native.wand_top_k`** (default 100) to a ranked
   `ORDER BY … &@@ …` query whose `@@@` queries are all the `&@@` query. WAND then
   keeps only the top *k* and never materializes the match set at all. (A `WHERE`
   query that differs from the `&@@` query is always answered exhaustively.) Reading *past* `wand_top_k` — a larger `LIMIT`, or none —
   falls back to the exhaustive scorer and runs into the bound again. That fallback
   re-ranks from a fresh snapshot but scores under the first build's statistics and
   resumes after the last row returned, so the rest continues in score order even if
   the index changes (another session's insert, or rows inserted in your own
   transaction) between fetches; rows the scan's snapshot cannot see are dropped by
   the executor's visibility check (issue #268, fixed).
2. **Make the query more selective.** A term matching millions of documents carries
   almost no ranking signal anyway.
3. **Raise `bm25_native.max_match_memory`.** It is `PGC_USERSET`, so a session may
   raise it for one query.

See `docs/adr/0047-match-set-memory-budget.md`.

## Compatibility and upgrades

Every index publishes a `min_read_version` — the oldest extension format that
can correctly read it — in its metapage. A binary refuses cleanly (never
mis-parses) an index whose floor it doesn't meet, or one older than the
formats it still knows how to read; otherwise it reads the index, including
one built by an older extension version. In practice this means:

- **Additive releases need no `REINDEX`.** A new optional on-disk region (a
  new page type, a new trailing field) is skipped by older binaries and read
  by newer ones without raising the floor. An index built under the previous
  release keeps working unchanged after you upgrade the extension.
- **A breaking format change** (one that reshapes existing on-disk state)
  raises the floor and ships with an online migration path via
  `bm25_upgrade(regclass)` — a no-`REINDEX`, no-heap-rescan upgrade of one
  index in place — whenever an online path is possible. A binary too old for
  an index's floor is refused with a clear error naming the required version;
  an index too old for a binary's oldest-readable generation is refused with
  a `REINDEX` hint.

**Ranking change: all-NULL rows (issue #299).** `CREATE INDEX` and `REINDEX` used to count
a row whose indexed text columns are all NULL as a document, which an `INSERT` never did.
That inflated the document count and deflated the average document length, so the same
data ranked differently depending on how it reached the index. The build now skips such
rows. An index built before this release keeps them until it is rebuilt, so a `REINDEX`
of an index over a table with all-NULL rows is a one-time shift in scores, and possibly in
order.

**Query semantics change: `WHERE @@@` with a different `ORDER BY &@@` (issue #290).**
Earlier releases answered such a query from the `&@@` query alone and ignored the
`@@@` one, so the index could return rows the `WHERE` excludes and miss rows it
includes; a second `@@@` condition on the same index scan was an error. Now every
`@@@` condition is applied and the result is the `WHERE` set in `&@@` order (see
[Usage](#usage)). The documented field-scope idiom changes with it: a query that
scoped only its `ORDER BY` (`WHERE title @@@ '"a b"' ORDER BY title &@@ 'body:"a b"'`)
now also returns the unscoped matches, after the scoped ones at distance `Infinity`;
put the scope in the `WHERE` too. An invalid `@@@` query beside a valid `&@@` one
(an unknown field, a malformed phrase) now raises its error instead of being skipped.

**Query semantics change: a field-scoped `@@@` applied as a filter (issue #298).**
Earlier releases evaluated `title @@@ 'body:cat'` off the index as the two terms
`body` and `cat` in the `title` column — rows unrelated to the index's answer,
silently. It now raises `feature_not_supported` (see [Usage](#usage) for when
`@@@` runs as a filter). The case most likely to notice: on a table with
row-level security, a role the policy applies to gets the filter plan, so
**field-scoped search on an RLS table now errors for those roles** instead of
returning wrong rows. A bare `@@@` query is unaffected.

**Behaviour changes in the #302-#314 fixes.** Each of these used to run and do
something other than what was written, or fail with an internal error:

- **jsonb queries are checked as written.** An unknown key in a query node, a
  fractional `slop`, a scoring leaf whose combined `bm25_boost` weight falls outside
  [1e-6, 1e6], and a tree of more than 1024 nodes are now errors (SQLSTATE 22023).
- **Text queries with a colon.** On the index path `'http://x'` and `'10:30'` are
  plain text (they used to raise `unknown search field`); applied as a filter they
  are refused. A tab or newline before a quoted phrase no longer turns it into an
  OR query (`E'\t"a b"'` is a phrase), and trailing whitespace after a phrase is
  accepted.
- **Snippets** are no longer NULL for phrase, wildcard, boolean and boost queries.
- **On a hot standby,** `bm25_seal`, `bm25_merge`, `bm25_upgrade` and the debug
  writers fail with SQLSTATE 25006 (`bm25_upgrade` used to give 55000, and a seal or
  merge with nothing to do used to return quietly); a ranked query can now report
  the reuse `40001` described under "Queries on a hot standby" instead of retrying it,
  and keeps its connection when a recovery conflict cancels it.
- **Row-level security:** `bm25_stats` and the read-only debug functions refuse
  (42501) when a policy applies to the caller.
- **Per-field reloptions:** `k1_<col>` is capped at 1e30 and `boost_<col>` at 1e6 at
  `CREATE INDEX`/`ALTER INDEX SET`. An index that already stores a larger value keeps
  working, but a later `ALTER INDEX ... SET` and a plain dump and restore fail until
  that option is `RESET` (pg_upgrade is unaffected). A per-field option naming no
  indexed column now draws a WARNING at `CREATE INDEX`/`REINDEX`.
- **Errors:** passing a table's OID, or one that does not exist, to a bm25 function
  that takes an index gives 42809 / 42704 instead of an internal error. Many
  shapes of on-disk corruption that used to hang, return wrong rows, or report a
  retryable 40001 now raise `index_corrupted` (XX002); so does a missing field-config
  page, which used to report 55000. A corrupt pending record or retired range blocks
  seals or VACUUM until `REINDEX`, instead of being dropped silently.
- The extension is no longer relocatable (see "Build and install").

**Upgrading a running deployment** (a primary with physical streaming
replicas, e.g. an Ansible-managed fleet) is three ordered steps. 1.0 is the
first release and `bm25_native--1.0.sql` is its only script, so until a later
release ships a `bm25_native--1.0--X.sql` update script, step 2 has nothing to
do. The 1.0 script is frozen: later SQL changes arrive through update scripts,
never by rewriting it, so an existing install picks them up with step 2 rather
than a `DROP EXTENSION`. A change an update script cannot express (reshaping
the operator class, for one) would be called out in that release's notes.

1. **Install the new extension binary (`.so`) on every node** — primary and
   all standbys. A standby still on the old binary refuses to read any
   new-format data the primary produces, cleanly, until this step completes.
2. **`ALTER EXTENSION bm25_native UPDATE;`** on the database — applies the
   release's `bm25_native--X--Y.sql` update script (new or widened SQL
   functions). Skipping this step can make the next one fail with "function does
   not exist".
3. **`SELECT bm25_upgrade('my_index');` on the primary only.** Standbys
   receive the upgraded index purely by replaying the primary's WAL — do not
   run `bm25_upgrade` on a standby (it refuses with SQLSTATE 25006,
   `read_only_sql_transaction`, like any other write there).
   `bm25_upgrade` is idempotent; re-running it on an already-current index is
   a no-op.

   `bm25_upgrade` always begins with the same blocking seal as `bm25_seal`, even
   when the index is already current, so it waits for any `VACUUM` of that index
   and can queue inserts behind it; run it when no `VACUUM` is active on the index
   (see "Maintenance and introspection").

## Tests

The suite runs through PostgreSQL's regression harness against a **running**
cluster, so `pg_config` must point at the same install the cluster runs and the
cluster must be reachable.

```sh
# SQL regression suites
PGHOST=/path/to/socketdir PGPORT=5432 \
  make installcheck PG_CONFIG=/usr/local/pgsql/bin/pg_config TAP_TESTS=
```

Confirm the run ends with `# All N tests passed.`

The TAP suites under `t/` (`PostgreSQL::Test::Cluster`-based: crash recovery,
replica equality, crash-orphan reclamation) need the `PostgreSQL::Test` Perl
modules, which an installation built without `--enable-tap-tests` does not ship;
they run locally against a PostgreSQL source tree's `src/test/perl` (see the TAP
note in `ARCHITECTURE.md`). CI runs them via `pg_virtualenv` on both supported majors (PG 17 and
18; CI is triggered manually with `gh workflow run CI --ref main`, not by pushes or
pull requests, see `docs/adr/0106-ci-runs-on-manual-dispatch.md`), and compiles with `COPT=-Werror` so a warning fails the build instead of
scrolling past. Before building, the same job runs cheap static checks —
packaging identity, 7-bit ASCII sources, the C source house style
(`test/check_source_style.py`; the style itself is documented in
`ARCHITECTURE.md`), the interrupt-check and scratch-context floors, and the check
that `sql/63` exercises every function behind the index-ownership gate
(`test/check_owned_gate_coverage.py`). None needs a build or a cluster. After the
TAP tier the job fails if `prove` ran fewer files than `t/` holds, and the TAP
suites fail on a `TRAP`, `PANIC`, crashed process, bad page or UBSan report in any
node's log. A separate **hardening** job additionally runs the full
`installcheck` (SQL + TAP) against a source-built **cassert + UBSan**
PostgreSQL 17 and 18, catching the assert / undefined-behavior / misaligned-access class
the release-package build cannot see (see `docs/adr/0008-cassert-ubsan-ci.md`), with
`wal_consistency_checking` on and the SQL run replayed by a streaming standby.
An **asan** job, on the same two majors, repeats that `installcheck` with the extension built under
AddressSanitizer, which catches stack-array overruns and reads past a
`malloc`'d block that neither UBSan nor cassert can see. PostgreSQL poisons no
palloc-chunk or shared-buffer boundary for ASan, so an overrun that stays
inside a palloc block or a shared buffer is invisible to it (see
`docs/adr/0090-addresssanitizer-gates-ci.md`).
A **folding-collation** job runs the SQL suites against a `C.UTF-8`
cluster, because every other gate is C-locale — where the analyzer never folds a
non-ASCII character, and the fold is what decides whether the stemmer engages, so
the terms written to the index differ structurally between the two (see
`docs/adr/0057-ci-gates-on-a-folding-collation-cluster.md`, and
`docs/adr/0046-encoding-aware-tokenization-and-folding.md` for the analyzer
mechanism). That job's TAP half is *not* folding coverage: `PostgreSQL::Test`
initialises its own clusters with no locale argument, so they take the runner's
environment locale. A **macos** job runs the SQL suites on the BSD ctype table
and is report-only until it has been observed green on `main` in a manual run (see
`docs/adr/0091-a-report-only-macos-leg-covers-the-bsd-ctype-table.md`). An
**autovacuum-off** job reruns the SQL suites with autovacuum disabled, so a
check whose result depends on which plan the planner picks — a stats-less
table plans differently from one autovacuum has analyzed — cannot pass only
because CI's default autovacuum-on run happened to plan around a bug (see
`docs/adr/0103-score-accessor-call-site-attribution.md`). A non-blocking
**build-and-test (19, experimental)** leg builds and tests against PostgreSQL
19 pre-GA the same way as the 17/18 legs; a pre-GA beta can regress for
reasons unrelated to this extension, so it never fails the required-checks
gate. (`sql/70_distance_volatility` used to fail on it, because PG19 added an
error position to a message the suite pins; the suite now renders that message
through `pg_temp.err_of()`, which is identical on every major.) A
**bench** job runs the `bench/` scripts, scaled down, on every CI run (CI is triggered manually,
`gh workflow run CI --ref main`, once a block of work has landed), and
uploads their CSVs as a build artifact; a **coverage** job builds with `lcov`
instrumentation and uploads a coverage report; a **static-analysis** job runs
`cppcheck` over `src/*.c` and uploads its report. All three are report-only --
`continue-on-error` at job level, nothing depends on them, and none compares
against a stored baseline -- because a shared runner is not where a trustworthy
performance or coverage number comes from; see `bench/README.md` and
`docs/adr/0098`. Only the static checks, build, SQL regression, TAP, the
hardening job, the asan job, the folding-collation job and the autovacuum-off
job gate CI. ("Gate" here means the job fails the workflow run; `main` carries
no branch-protection rule that would block a merge on a red run.) The
`build-and-test (19, experimental)` leg never gates it.

## Documentation

- **[ARCHITECTURE.md](ARCHITECTURE.md)** — module map, the on-disk layout, and the
  binding invariants. Start here to work on the code.
- **[THEORY.md](THEORY.md)** — why the design is shaped this way: the
  segmented-LSM rationale, the three hard constraints, and the reuse-safety /
  reclamation model.
- **[docs/adr/](docs/adr/)** — the architecture decision log: one record per
  design decision, with context, alternatives considered, and consequences.

## License

The [PostgreSQL License](https://www.postgresql.org/about/licence/); the full text
is in [`LICENSE`](LICENSE). Maintainer: Christophe Pettus
&lt;christophe.pettus@pgexperts.com&gt;.
