---
id: 0123
title: bm25_snippet highlights any query tree, from its non-negated leaves, with wildcards globbed against the field's own tokens
date: 2026-10-05
status: Accepted
summary: The snippet hit set is built from every non-negated leaf of the scan's query tree -- MATCH, TERM and PHRASE text analyzed under the scan's config, WILDCARD patterns folded as the expander folds them and globbed against the field's analyzed tokens -- so PHRASE, WILDCARD, BOOLEAN and BOOST roots no longer return NULL on every row; this replaces ADR 0005's F2 limitation.
---

# 0123. bm25_snippet highlights any query tree, from its non-negated leaves, with wildcards globbed against the field's own tokens

## Context

ADR 0005's F2 item records that `bm25_snippet` highlights only single-leaf or text scans:
it read its hit set from `so->qterm`, which the scan fills for a text query or a single
MATCH/TERM jsonb root. A PHRASE, WILDCARD, BOOLEAN or BOOST root returned NULL on every
row, which looks exactly like "no hit" (#308 TEXT-04). A single PHRASE or WILDCARD leaf was
inside F2's own "single-leaf" wording, so the limitation was also a defect.

Capturing the scan's dictionary expansion for wildcards would have meant changing
`bm25_scan_rank.c`, the ranking builder, for a presentation feature.

## Decision

Decided by the user as D17. At the first snippet call of a scan the hit set is built from
`so->qtree`:

- **Non-negated leaves only.** A `must_not` subtree is skipped whole: highlighting a term
  the query excludes would mark text the row was returned despite, not because of.
- **MATCH, TERM and PHRASE** text goes through `bm25_analyze` under the scan's `qcfg`, the
  call the exhaustive scorer makes per leaf, so each stems identically.
- **WILDCARD** keeps its pattern, folded by `bm25_fold_term` exactly as
  `bm25_dict_expand_wildcard` folds it, and each field token is tested with
  `bm25_glob_match`. The field is analyzed with the index's own analyzer, so its tokens are
  the dictionary bytes the row was indexed under, and a glob match on one is membership in
  the expansion for any row the scan returned. The expansion cap cannot make them differ:
  exceeding it ERRORs the scan rather than truncating. `bm25_scan_rank.c` is unchanged.
- **Field scope is ignored**, as it always was for a text `field:term` query: the function
  is handed a value, not a column.

The hit set is sorted once per scan and probed with `bsearch` over `bm25_token_cmp` (now
shared from `bm25_handler.c`), and the hit loop checks for interrupts per field token
(XCUT-06/REGR-04, the ADR 0034 remedy applied to the snippet). Landed in PR #337 (#308,
#305).

## Alternatives considered

- **Capture each wildcard's dictionary expansion in the ranking build** and hand it to the
  snippet. Correct by construction, but changes the ranking builder for a presentation
  feature, and adds per-scan memory proportional to the expansion.
- **ERROR for a tree the snippet cannot highlight** (the fallback the diagnosis offered).
  Cheaper, but leaves every builder-made query without highlighting.
- **Keep F2 and document it.** It was already documented; the NULL-on-every-row trap was
  the problem.

## Consequences

- Snippets are non-NULL for phrase, wildcard, boolean and boost trees whenever a
  non-negated leaf hits the field.
- **Residuals**, recorded at `snippet_add_leaves`: re-analysis approximates the scan's
  matcher rather than replaying it. (1) Field scope is ignored. (2) A phrase marks its terms
  wherever they occur in the field, not only where they form the phrase (the text-syntax
  `"..."` path always did). (3) A text that was never indexed (a literal, another column) is
  matched against the query's terms and patterns as given, so a wildcard can mark a word
  the expansion never contained.
- `sql/146_snippet_query_trees` carries a wall-clock bound (`clock_timestamp() - t0 <
  interval '10 seconds'`, eight rows, about 23 s before the fix and well under a second
  after). It departs from the project's "timing is not asserted" practice (sql/74); the
  reviewer kept it and it was surfaced for the user rather than replaced with a work
  count.
- ADR 0005's F2 item is replaced by this record (ADR 0005 addendum); ADR 0049 records the
  budget carry and ADR 0034 the sorted probe. The same PR replaced `pg_mblen` with
  `bm25_mblen_bounded`, recorded on ADR 0049.
