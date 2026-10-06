---
id: 0023
title: The analyzer's token array grows geometrically instead of trusting a word-run estimate
date: 2026-07-29
status: Accepted
summary: bm25_analyze sizes its BM25Token array from the word-run count but emits one token per lexeme, so the array now doubles on demand rather than treating the estimate as a bound.
---

# 0023. The analyzer's token array grows geometrically instead of trusting a word-run estimate

## Context

`bm25_analyze` allocated its output array once, at `textlen / 2 + 1` entries, under
the comment "Upper bound on token count: alternating word/non-word." That is a
correct bound on the number of **word runs** — a run needs at least one word
character and runs are separated by at least one non-word character, so at most
`(textlen+1)/2` runs exist.

It is not a bound on the number of **tokens**. The emit loop appends one
`BM25Token` per lexeme returned by `ts_lexize`, and `ts_lexize` returns an array:

```c
for (j = 0; res[j].lexeme != NULL; j++) { ... toks[ntok] ... ntok++; }
```

with no capacity test. The function's own comment acknowledges the mismatch
("compound splitters emit several"). Any dictionary whose output exceeds 0.5
lexemes per input byte overruns the allocation — the smallest shape is a
two-character word yielding two lexemes, e.g. `'ab cd ef'`: 8 bytes, old capacity
5, 6 tokens emitted. The overrun writes 24-byte `BM25Token` structs over the
following palloc chunk header.

The dictionary is not fixed by the extension. `language` is a reloption registered
with a NULL validate callback, resolved as `"<language>_stem"` by name, so the
stemmer can be any text-search dictionary, including a compound-splitting ispell
dictionary or a thesaurus that expands rather than contracts.

`bm25_analyze` is shared by the debug tokenizer, `bm25_build` and `bm25_insert`, so
this is a build/insert-path memory-safety defect, not a debug-only one.

### Reachability, as measured

The original report (review ref H1, issue #41) described this as reachable by "a
non-superuser with CREATE on any schema". On the supported versions it is not.
Measured on PG 18.3:

- PG 17+ runs `CREATE INDEX` with `search_path` restricted to
  `pg_catalog, pg_temp`, and non-relation catalog lookups skip the temp namespace.
  A dictionary in a user schema is therefore **not** resolvable at build time even
  with that schema in the session `search_path`; `pg_temp` does not work either.
  Only a dictionary in `pg_catalog` resolves — superuser only.
- The debug tokenizer resolves under the caller's own `search_path`, but the entire
  `bm25_debug_*` surface is revoked from `PUBLIC` (see [0020](0020-debug-surface-privileges.md)).

So the defect is superuser/owner-reachable rather than unprivileged. It is still a
heap overflow on the build and insert paths, and it is one branch to close.

## Decision

Track a capacity alongside `ntok` and double the array with `repalloc` before any
write that would fill it. The initial allocation keeps `textlen / 2 + 1` as an
**estimate** — sized so the one-lexeme-per-run common case never repallocs — and
the comment now says so explicitly instead of calling it an upper bound.

The guard is placed at both append sites (the `stopwords = none` surface-form keep
and the per-lexeme loop), even though only the second can outrun the estimate
today. A future edit that adds a third emit site inherits a uniform pattern rather
than a rule about which sites are "safe".

This matches `bm25_wild_add` (`src/bm25_seg_read.c`), which already uses the same
inline `if (n == capacity) { capacity *= 2; repalloc }` shape, so the file's house
style is unchanged.

## Alternatives considered

- **Compute a true upper bound up front** — there is none. `ts_lexize`'s output
  size is a property of a user-chosen dictionary; nothing short of calling it tells
  you how many lexemes a run produces.
- **Cap the token count and drop the excess** — silently truncates a document's
  token stream, which corrupts scoring (doclen, tf) rather than crashing. A wrong
  answer is worse than the allocation.
- **`ereport` when the estimate is exceeded** — turns a legitimate configuration
  (a compound-splitting dictionary, which the code comments already anticipate)
  into a hard error at INSERT time.
- **Assert instead of a runtime guard** — compiled out in production, which is
  exactly the build where the overflow matters.
- **Validate the `language` reloption to Snowball dictionaries only** — narrows a
  documented feature (any TS dictionary) to fix a bug that is really about the
  allocation, and would not help the debug tokenizer's explicit-config overload.

## Consequences

- The build and insert paths are memory-safe for any dictionary, whatever its
  lexeme-per-run ratio.
- One predictable branch per emitted token on the tokenize path, and a possible
  `repalloc` for dictionaries that expand. The common Snowball case (one lexeme per
  run) still allocates exactly once, so the hot path is unchanged.
- `sql/65_multilexeme_tokens` pins the per-lexeme emit path with an ispell
  compound-splitting dictionary, on both the debug tokenizer and a real
  build + insert + scan cycle.
- **The suite does not reproduce the pre-fix overflow, and says so in its header.**
  Every dictionary PostgreSQL ships tops out near 0.4 lexemes per byte
  (`footballklubber` → 6 lexemes / 16 bytes); exceeding 0.5 needs a dictionary file
  in `$SHAREDIR/tsearch_data`, which a regression suite cannot install. The tests
  are therefore characterization tests of the emit path, not a red/green
  reproduction. A cassert CI build ([0008](0008-cassert-ubsan-ci.md)) would catch
  the clobbered chunk header if such a dictionary ever appeared in a test fixture.
- The measured `search_path` facts above are recorded here because they are not
  obvious from the code and they bound the severity of this whole class of
  `language`-reloption findings.

## Addendum (2026-08-23)

Two statements in this record have been overtaken by ADR 0082 (#148 HDL-07), which
gave the `language` reloption a `validate_string` callback. Neither changes the
decision; both would mislead a reader who lands here first.

**The Context's "registered with a NULL validate callback" is no longer true.**
`language` is now validated at `CREATE INDEX` / `ALTER INDEX`. What that validator
checks is only that `"<language>_stem"` RESOLVES, under the same restricted
`search_path` the build uses. It says nothing about what the dictionary DOES.

**So the rejected alternative below is narrower than what shipped, not the same
thing.** "Validate the `language` reloption to Snowball dictionaries only" — the
option this record rejected — would have narrowed a documented feature (any TS
dictionary) to fix an allocation bug. That is still rejected and still the right
call. What shipped is an EXISTENCE check: an ispell compound splitter and a
thesaurus both pass it, keep expanding one run into several lexemes, and keep
exercising the array growth this record is about. The premise of this record — the
dictionary is not fixed by the extension, so the token array must grow — is
untouched.

The second half of that rejected bullet, "would not help the debug tokenizer's
explicit-config overload", remains exactly right for `language`: that overload takes
a language argument directly and no reloption validator can reach it. (Its SECOND
argument, separately, was renamed from `analyzer` to `tokenizer` and is now validated
by ADR 0082 — a different knob that merely shared a name.)
