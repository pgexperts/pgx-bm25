---
id: 0048
title: bm25_snippet escapes its field text by default
date: 2026-08-10
status: Accepted
summary: The snippet builder HTML-escapes the field text it copies into the excerpt, with an `escape` argument as the opt-out, deliberately diverging from ts_headline because the function's own defaults declare HTML as the intended rendering target.
---

# 0048. bm25_snippet escapes its field text by default

## Context

`bm25_snippet(field, start_tag, end_tag, max_num_chars)` returns an excerpt of the
passed column value with each query-term occurrence wrapped in tags. Its tag defaults
are `'<mark>'` and `'</mark>'`, and the file header described the contract as wrapping
occurrences "in `<mark>`…`</mark>`". The function's own defaults therefore declare HTML
as the intended rendering target.

The builder copied the original field bytes into the output verbatim on all three of
its paths — the leading context, the hit span, and the trailing context. Markup stored
in the indexed column passed straight through. A search UI doing the obvious thing —
`SELECT bm25_snippet(body)` into `innerHTML`, because the function's defaults are HTML
tags — would render whatever markup the corpus contained.

Review finding #67.10 filed this as a security defect. The verifier **refuted that
framing** and downgraded it from medium to low, on platform precedent: PostgreSQL's own
highlighter behaves identically. `ts_headline` on PG 18 with an `<img src=x
onerror=...>` prefix returns the tag intact — HTML tag defaults, source markup passed
through, no escaping option, and no warning in the core documentation. The verifier's
suggested fix was a documentation caveat, and it called an `encoder` argument "a
nice-to-have, not a fix for a defect."

The repo owner overrode that conclusion and directed escape-by-default. The reasoning
that carries the override, rather than the precedent: `ts_headline`'s behaviour explains
how PostgreSQL got here, but it is not a justification. `ts_headline` shipped in 2007
with an installed base that a change would break. This extension has no installed base —
version 1.0 is untagged, and ADR 0046 had already forced a REINDEX for every existing
index in the same release. The window for choosing the safe default is open exactly now,
and it costs one argument.

## Decision

The field text is HTML/XML-escaped by default. `bm25_snippet` gains a fifth argument,
`escape boolean DEFAULT true`; `escape => false` restores byte-verbatim field text.

Five characters are escaped — `&` `<` `>` `"` `'` — as `&amp;` `&lt;` `&gt;` `&quot;`
`&#39;`. This is the `htmlspecialchars(ENT_QUOTES)` set. `&#39;` rather than `&apos;`
because the latter is XML and HTML5 but not HTML 4.

The caller's **tags are never escaped**. They are markup by contract — escaping them
would render `<mark>` as visible text — so tags remain trusted input the caller owns,
and passing user-controlled tags is still an injection. That asymmetry is the contract
and is stated in the SQL comment, the file header, and `ARCHITECTURE.md`.

A NULL `escape`, and a catalog entry predating the argument, both select escaping: not
saying anything picks the safe direction.

Escaping is measured against the ORIGINAL text, never the output — see
[0049](0049-snippet-budget-counts-characters.md), which decides the budget's units. The
two are independent by construction: an `&` spends one unit of budget and emits five
bytes, so `escape` cannot change which window is selected, only how it renders.

## Alternatives considered

- **Document the hazard and change nothing** (the reviewer's suggested fix, and
  `ts_headline` parity) — rejected by the owner. The caveat would be accurate and would
  leave every caller one forgotten escape call away from rendering stored markup.
- **Always escape, no opt-out** — rejected. It would make the function unusable for
  non-HTML consumers: a plain-text excerpt in a CLI or a JSON API would show `&amp;`
  where the corpus has `&`, with no way back.
- **`encoder text DEFAULT 'html'`**, the Elasticsearch/Solr spelling — rejected. An enum
  with exactly two members needs its own validation and its own error path for a
  misspelled value; a boolean is self-validating. A third encoding, if ever wanted, can
  be a new argument then.
- **Escape only `&` `<` `>`** (Lucene's `SimpleHTMLEncoder`, minus the quotes) — rejected.
  The two quote characters are what make the result safe inside a quoted attribute, and
  a snippet in a `title=` tooltip is an ordinary use.
- **Escape the tags too** — rejected as incoherent; it defeats the function's purpose.

## Consequences

- **This is a behaviour change.** A caller rendering the result as HTML now gets correct
  output with no change on their side. A caller consuming the result as plain text sees
  entities appear and must pass `escape => false`.
- **It diverges from `ts_headline`**, which is what a PostgreSQL user will expect. Stated
  explicitly in `README.md`, `ARCHITECTURE.md`, and the SQL comment rather than left as a
  surprise.
- **The tags stay a trust boundary the caller owns.** Nothing here protects a caller who
  interpolates user input into `start_tag`.
- **One direction of catalog skew is safe and the other is not, and C offers no fix.** A
  library NEWER than the catalog — a rebuilt `.so` against a 4-argument `CREATE FUNCTION`
  — is handled: `PG_NARGS() > 4` avoids reading past the `fcinfo` and escaping stays on.
  The reverse — a 5-argument catalog with a STALE library, which is this repo's known
  "forgot to `rm` the dylib before `make`" failure mode — silently ignores `escape` and
  emits raw markup, with no error to notice. A C function cannot detect that its own
  binary is older than its declaration. Recorded so the failure is recognized rather than
  debugged: unexpected raw markup in a snippet means a stale build, not a broken fix.
- **Escaping the hit span is unreachable today.** A hit is a whole analyzer word run, and
  `bm25_is_word_byte` admits only alnum bytes and (in a multibyte encoding) high-bit
  bytes, so none of the five ASCII metacharacters can fall inside one. The hit span is
  routed through the escaper anyway, so that widening the word-character predicate — to
  admit the apostrophe in "don't", say — cannot silently open a hole in a path nobody
  re-examined.
- **No existing regression suite moved.** None of the five snippet-touching suites has a
  corpus containing any of the five metacharacters, so all 88 stayed green and only the
  unrelated `errmsg` line in `expected/39_snippet.out` changed. That is precisely why
  `sql/84_snippet_escaping_and_char_budget.sql` exists: before it, nothing in the tree
  would have noticed escaping being removed again.
