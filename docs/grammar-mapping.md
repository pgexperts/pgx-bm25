# Query grammar → bm25_native native call (conformance contract)

This is a worked mapping of a rich Lexis/Westlaw-style search grammar onto native
`bm25_native` calls, for anyone porting a legal-research-style query compiler off
`pg_search`. Each documented query-grammar form maps to a native `bm25_native` call.
Grounded in the documented Lexis/Westlaw-style grammar (ROADMAP §1b/§5), NOT
reverse-engineered from any particular application's compiler. Conformance is proven by
`sql/51_conformance.sql` and `t/008_conformance.pl`.

## The index shape ("§1a")

```sql
CREATE INDEX conf_bm25 ON conf_brief USING bm25_native (title, summary, body_plain)
  INCLUDE (id)
  WITH (key_field = 'id', language = 'english',
        boost_title = 5, boost_summary = 3, boost_body_plain = 1);
-- store_positions defaults ON per field -> phrase / proximity / snippet all work.
-- key_field='id' -> scoring/highlight keyed by the user id via bm25_score_key(id).
```

## Anchoring rule

Every predicate anchors on the FIRST indexed column: `title @@@ <query>` and
`title &@@ <query>`. The field a clause SCOPES is set inside the jsonb tree
(`bm25_term('summary', ...)`), independent of the anchor column. Anchoring on a
later column cannot use the index, so a jsonb `@@@` there ERRORs ("can only be
evaluated by a bm25 index scan") rather than returning zero rows.

## Text query syntax

The table below maps onto the jsonb builder calls. A plain-text query (`col @@@ 'red car'`,
`col @@@ 'body:cat'`, `col @@@ '"red car"~2'`) is a separate, smaller syntax: words (OR),
an optional leading `field:` scope and an optional leading quoted phrase with a `~n` /
`~>n` slop suffix. It has no AND/NOT or nesting; use the builders for those. See the
"Text query syntax" entry in the README's usage section for the exact rules.

## Mapping table

| Query grammar form | Native call |
|---|---|
| plain stemmed term, no field scope: `negligence` | `bm25_boolean(should => ARRAY[bm25_term('title',t), bm25_term('summary',t), bm25_term('body_plain',t)])` — the index's per-field boosts (5/3/1) weight the fan-out automatically (a title hit outranks a body hit). |
| `A AND B` | `bm25_boolean(must => ARRAY[<A>, <B>])` |
| `A OR B` / juxtaposition-OR | `bm25_boolean(should => ARRAY[<A>, <B>])` |
| `A NOT B` / `A AND NOT B` | `bm25_boolean(must => ARRAY[<A>], must_not => ARRAY[<B>])` |
| nested boolean `(A OR B) AND C` | compose: `bm25_boolean(must => ARRAY[bm25_boolean(should => ARRAY[<A>,<B>]), <C>])` |
| exact phrase: `"product liability"` | `bm25_phrase('field','product liability')` (default `slop=0, ordered=true` — adjacent, in order) |
| proximity unordered: `A W/n B` | `bm25_phrase('field','a b', n, false)` |
| proximity ordered: `A PRE/n B` | `bm25_phrase('field','a b', n, true)` |
| `A W/S B` (same sentence) | `bm25_phrase('field','a b', 25, false)` — documented slop convention (the engine has token-distance slop, no sentence boundary; 25 approximates a sentence). |
| `A W/P B` (same paragraph) | `bm25_phrase('field','a b', 75, false)` — documented slop convention (75 approximates a paragraph). |
| field scope `ti(x)` / `su(x)` / `te(x)` | set the builder's `field` argument to `'title'` / `'summary'` / `'body_plain'`. |
| truncation: Lexis `judg!` / Westlaw `judg*` / embedded `wom*n` | `bm25_wildcard('field','judg*')` / `bm25_wildcard('field','wom*n')` — both Lexis `!` and Westlaw `*` map to our `*` glob; matched (lowercased) against the STEMMED dictionary; min-prefix of at least 3 bytes, cap `bm25_native.wildcard_max_expansions`. |
| per-clause query-time boost | `bm25_boost(weight, <leaf>)` — multiplies on top of the index's per-field boost. |
| match predicate (filter) | `title @@@ <query>` (anchor on the first indexed column). |
| relevance score | `bm25_score_key(id)` — the `paradedb.score(id)` analogue, keyed by `key_field`; read inside an ordered scan. |
| ranked top-N | `... WHERE title @@@ <query> ORDER BY title &@@ <query> LIMIT n` — single-key, plans to `Limit → Index Scan` (no Sort). The `@@@` is required: `ORDER BY` alone is a Seq Scan + Sort with every distance `+inf`. |
| highlight / snippet | `bm25_snippet(field, start_tag, end_tag, max_num_chars, escape)` — NULL when the field has no query hit for the row; reads the active scored scan. `max_num_chars` counts CHARACTERS of the original text; `escape` (default `true`) HTML-escapes the field text but never the tags. |

## Injection-safety (§1c)

A user-supplied term whose VALUE is literally an operator or wildcard is DATA, never
parsed: `bm25_term('body_plain','AND')` searches for the token `and` (a stopword →
matches nothing), and `bm25_term('body_plain','judg*')` searches for the literal
token — a strict subset of the expansion `bm25_wildcard('body_plain','judg*')`. The
grammar operators live in the tree STRUCTURE (`must`/`should`/`must_not`, the `*`
inside `bm25_wildcard`), never in a term's value string.

## Known conventions & limits

- `W/S`→25, `W/P`→75 are token-distance approximations (no sentence/paragraph model).
- Wildcard requires a literal prefix of at least 3 bytes (the unit the parse-time and
  expander checks both measure; a multibyte character counts as several) and is capped;
  both raise a clear ERROR.
- All matching is over stemmed tokens (English Snowball), so `negligence`/`negligent`/
  `negligently` are one term; `judgment`/`judgement` do NOT share the `judge` stem
  (`judg`), so a wildcard `judg*` is strictly broader than the term `judge`.
- **A jsonb `@@@` filter must be answered by the bm25 index.** `col @@@ <jsonb>` can only be
  evaluated by a bm25 index scan; if the planner instead applies it as a filter / seqscan qual
  (a competing `ORDER BY` that steals another index, a cheaper index, a cost/stats flip) it now
  ERRORs (fail-loud) rather than silently returning 0 rows. Always issue match + ranking together
  (`WHERE title @@@ q ORDER BY title &@@ q`, anchored on the first indexed column, with
  `SET enable_seqscan = off`) — the `@@@` predicate is what makes the index usable (an `&@@`
  order-by alone does not force it; every row then ranks `+inf`), which is how ranked
  queries are issued anyway. (The conformance suite uses the ranked form throughout; the fail-loud fix + its
  regression live in `sql/52_jsonb_filter_index_only`.)
- **`bm25_snippet` highlights the query's positive leaves, whatever the tree.** Every
  non-`must_not` leaf contributes: match/term/phrase text as analyzed terms (a phrase
  marks its terms wherever they occur, not only in-phrase), a wildcard by globbing the
  field's analyzed tokens. Field scope is ignored -- the function sees a value, not a
  column -- so a `title`-scoped leaf also marks the word in `bm25_snippet(body)`. (Until
  #308 only a text query or a single match/term leaf highlighted; every other jsonb root
  returned NULL on every row.)
