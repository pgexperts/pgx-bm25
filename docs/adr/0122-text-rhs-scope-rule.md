---
id: 0122
title: The text-RHS micro-parser has one scope rule and one whitespace class, and off-index refuses or agrees, never silently differs
date: 2026-10-05
status: Accepted
summary: The index path recognizes a field scope with bm25_query_field_prefix, the rule bm25_match refuses on; for a name that is not a field of the index, a colon followed by '/' or a digit is literal text and any other unknown name is an ERROR with a builder hint; all three text parsers share bm25_query_isspace; off the index every scope-shaped RHS is still refused.
---

# 0122. The text-RHS micro-parser has one scope rule and one whitespace class, and off-index refuses or agrees, never silently differs

## Context

A text `@@@` / `&@@` right-hand side can carry a `field:` scope and a `"phrase"~n`.
Before this change the index path (`bm25_rescan_parse_field`) took everything before the
first colon ahead of any quote as a field name, so ordinary text such as
`'meeting 10:30'`, `' body:cat'`, a URL, or `'note to self: "red car"'` raised "unknown
search field" whenever the index answered, while the filter path answered or refused
(#306 SCAN-04). #298 (ADR 0004's and 0005's addenda of 2026-10-05) had just introduced
`bm25_query_field_prefix` for the off-index path: leading whitespace, then a run with no
whitespace and no quote, ended by a colon. That addendum recorded that #306 had to move
both consumers together.

The three parsers also disagreed on whitespace (QUERY-06/SCAN-10): only a space was
skipped, so a leading tab turned a phrase into an OR query on the index path and slipped
past the off-index phrase refusal (#132), and trailing whitespace after `"..."` or `~n`
was a syntax error.

## Decision

Decided by the user as D16.

- **One scope rule.** The index path recognizes a scope with `bm25_query_field_prefix`.
- **Unknown names, index path only.** For a name that is not a field of the index, a colon
  followed by `/` or a digit is literal text (URLs, times, ratios); any other unknown name
  still ERRORs, so typos in field names are caught. The empty prefix (`':running'`) still
  ERRORs. The error carries a hint that says what a leading word and colon mean and how to
  search text of that shape: split it into words and pass each as `bm25_term(field, word)`
  under `bm25_boolean(should => ...)`. No message recommends `bm25_match` (the #298 rule).
- **Off the index, unchanged.** `bm25_match` keeps refusing every scope-shaped RHS. It
  cannot tell a known field name from an unknown one, so letting `/`-or-digit through
  there would make `'title:2024'` silently search the text `title 2024` (the #298 bug).
- **The invariant becomes** "off-index refuses or agrees, never silently differs", the
  asymmetry phrases already had. The #298 invariant ("the off-index refusal never rejects a
  query the index answers unscoped") is dropped: `'http://x'` answers on the index and is
  refused off it.
- **One whitespace class**, `bm25_query_isspace` (the SQL lexer's), in
  `bm25_query_field_prefix`, `bm25_query_phrase_offset` and the phrase suffix parser, and
  trailing whitespace is trimmed before the suffix parse.
- **Field names containing whitespace or a quote** cannot be scoped in text syntax; the
  jsonb builders name any field.

Landed in PR #336 (#306).

## Alternatives considered

- **Share the `/`-or-digit escape with the off-index path** (the issue's suggestion).
  Unsafe: `bm25_match` cannot see field names, so it would silently mis-answer
  `'title:2024'`-shaped scopes, reopening #298.
- **Treat every unknown name as literal text.** Loses typo detection for `titel:cat`, which
  would silently search all fields for two words.
- **Keep the first-colon split on the index path.** Leaves ordinary text erroring on one
  plan and answering on another.

## Consequences

- User-visible: `http://x` and `10:30` are literal text on the index path and refused off
  it; `E'\t"a b"'` is a phrase (it was an OR query on the index path and answered off it).
- Pinned by `sql/145_text_rhs_micro_parser`, which runs each case on both the index and the
  filter path; the README documents the text syntax.
- The ADR 0004 and ADR 0005 addenda of this date restate the #298 invariant.
