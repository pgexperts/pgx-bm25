---
id: 0078
title: A "term" query means exactly one term, and bm25_match(text,text)'s analyzer divergence is documented rather than fixed
date: 2026-08-20
status: Accepted
summary: bm25_term was byte-for-byte bm25_match and silently OR'd every token of a multi-word value; it now errors, enforced at the two places tokens exist. bm25_match(text,text)'s hard-coded english analyzer is structural — it receives two bare text Datums and cannot discover the index — so it is documented and pinned by a test instead.
---

# 0078. A "term" query means exactly one term, and bm25_match(text,text)'s analyzer divergence is documented rather than fixed

## Context

Two findings in #151 are about a query surface promising one thing and doing another.

**QRY-05.** `BM25Q_TERM` and `BM25Q_MATCH` took the same arm in `flatten_recurse`, in
`bm25_query_eval` and in `bm25_qtree_is_multileaf`; the only differences were the jsonb
key read (`"value"` vs `"terms"`) and a debug label. Both analyzed their value and OR'd
every token. So `bm25_term('body','red car')` matched a document containing only "red"
— OR semantics from a builder named for a single term, with `README.md` documenting it
as single-term. A silently *wider* match set than the name promises is the worst of the
available behaviours: it over-returns, quietly, and the user has no signal.

**SQL-12.** `bm25_match(text,text)` memsets a config and hard-codes english, default
stopwords and the standard tokenizer, whatever the index's reloptions say. When the
qual lands as a Filter rather than an Index Cond, the off-index evaluation therefore
tokenizes differently from the index that would have answered it.

## Decision

**`bm25_term` requires exactly one term**, and errors otherwise with a hint naming
`match` (for OR) and `phrase` (for adjacency).

Enforced where the tokens exist, which is *two* places and not one — a fact worth
recording because the second is easy to miss:

- `bm25_scan_build_ranking_exhaustive`'s leaf/token loop, for a TERM leaf inside a
  boolean or boost tree;
- `bm25_rescan_parse_jsonb`'s single-leaf text-equivalent shortcut, which copies the
  leaf's raw text into `so->qterm` and discards the leaf, so the first check never sees
  it.

It cannot be enforced in `bm25_query.c`: "how many terms is this?" is an analyzer
question and parse time has no `Relation`.

**`bm25_match(text,text)` keeps its hard-coded analyzer**, documented in the C function
and the install script, and pinned by `sql/99_query_semantics` — which asserts the
agreement on an english index *and* the divergence on a german one (index path finds
the row, filter path does not).

## Alternatives considered

- **Delete `BM25Q_TERM` and make `bm25_term` a SQL alias for a `match` node.** The
  verification's own recommendation, and genuinely lower-risk: zero of the 189 existing
  call sites pass multi-word text, so nothing would break, and it removes a redundant
  AST kind rather than papering over it. Not chosen — the product decision was that
  "term" should mean term. The cost of that decision is recorded below.
- **Take only the first token of a multi-word value.** Silently discards user input;
  strictly worse than the OR it replaces, which at least over-returns rather than
  under-returns.
- **Make `bm25_match(text,text)` error off-index, like `bm25_match_jsonb`.** The
  comparison does not hold. `bm25_match_jsonb` refuses because a jsonb query tree
  *cannot* be evaluated without the index — refusing is the only correct answer it has.
  `bm25_match` can evaluate, and on an english index (the default) its answer is right.
  Erroring would trade a narrow documented divergence for a broad certain breakage.
- **Give `bm25_match` a signature that can see the index.** There is no such signature.
  It receives two bare `text` Datums; changing them breaks the `@@@` operator; and a
  planner-support rewrite cannot help either, because at plan time it is not yet known
  whether the qual lands as an Index Cond (where this code never runs) or a Filter.

## Consequences

- **The `bm25_term` error is analyzer-dependent, and this is the accepted cost of the
  decision.** A value that stems to one token is legal and one that does not is not, so
  the same builder call can be legal on one index and illegal on another. A value whose
  only token is a stopword yields zero tokens — a different case, left alone, because a
  term that analyzes away matches nothing, which is already what an empty leaf does.
- Existing callers passing multi-word text to `bm25_term` now get an error where they
  previously got OR semantics. No in-tree caller does; an external one would.
- `sql/99_query_semantics` pins the SQL-12 divergence with numbers rather than prose, so
  it stays a known, tested property. If someone later makes the filter path
  index-aware, that test is where the change announces itself.

## Addendum (2026-10-05)

#298 (2026-10-05) adds a second documented divergence of the same kind as the one recorded
here: off the index, `bm25_match` sees only the LHS column's value, so a bare RHS on a
multi-column index matches that column where the index matches every field. It cannot be
fixed for the reason this record gives, since the operator receives two bare text values and
cannot discover the index, so it is documented and not fixed, and the field-scoped case that
used to be silent now raises (ADR 0004's addendum of this date). `sql/130_offindex_field_scope`
pins the filter shapes with plan guards.
