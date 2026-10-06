---
id: 0092
title: The C sources follow a documented house style, gated where a script can decide it
date: 2026-09-21
status: Accepted
summary: src/ keeps its own style instead of adopting pgindent; the style is written down in ARCHITECTURE.md and its mechanically decidable rules (whitespace, 120 columns, postgres.h first, errcode() in every error ereport) are enforced by test/check_source_style.py.
---

# 0092. The C sources follow a documented house style, gated where a script can decide it

## Context

Review finding #66 ("PostgreSQL coding conventions and formatting") measured
the tree against PostgreSQL core's conventions: 4-space rather than tab
indentation, no PG-core file-header blocks, thousands of lines past pgindent's
79 columns, both comment-opener forms in use, `postgres.h` included from
headers instead of first in each `.c`, multi-statement lines, primary messages
that named C routines, and a handful of `ereport(ERROR)` sites with no
`errcode()`.

The bug-grind plan (`docs/superpowers/plans/2026-08-04-bug-grind-58-69.md`)
settled #66 as a binding decision: fix a cheap subset (the errcodes,
`BM25_MAX_FIELDS`, `bm25_fsm.c`'s tabs, ASCII sources and the other items
PR-K landed) and keep the 79-column reflow, comment-opener normalization, PG
file headers and multi-statement lines out. ADR 0056 accordingly rejected a
pgindent or lint job, because such a job would have reopened those exclusions
by machinery.

That left the tree with a style that existed only as a consensus of its
existing files. Nothing stated it and nothing checked it, so the reviewer had
no rule to apply and a contributor had nothing to follow; the finding's own
recurring complaint was "no rule exists". Several of the conventions it cited
were real hazards rather than taste: an `ereport(ERROR)` without `errcode()`
reports SQLSTATE `XX000` and misleads any client that classifies errors, and
`bm25.h`'s PG-floor `#error` reads `PG_VERSION_NUM`, which a `.c` file receives
only through `postgres.h` (it is `pg_config.h`'s, reached via `c.h`).

## Decision

Keep the existing house style, write it down, and gate the part of it a script
can decide without judgement.

- `ARCHITECTURE.md` gains a "C source house style" section: 4-space indent and
  no tabs; comments wrap near 88 columns with a hard 120; both comment-opener
  forms accepted; a short prose file header naming the bare basename, with the
  root `LICENSE` (added in the same change) as the license statement rather
  than per-file PG-core blocks; `postgres.h` first in every `.c` and in no
  header; one statement per line with the single `case LABEL: return <expr>;`
  exception; declarations at the top of a block and no for-init declarations;
  `errcode()` in every error `ereport`; and an error-message prefix rule
  (R1-R5: every primary message starts with `"bm25: "`, or with the exact
  SQL-visible function name when a SQL function reports on its own arguments;
  never a C routine's name; amvalidate INFO messages and `elog()` exempt).
- The tree is brought into line with that section once: layout only in one
  commit (verified token-identical by the ADR 0059 method), includes and
  message prefixes in another. This lifts one of the original #66 exclusions:
  multi-statement lines are expanded. There were few enough that expanding
  them is a small, provably behaviour-free diff, unlike the 79-column reflow,
  and a documented one-statement-per-line rule is not credible over a tree
  that visibly breaks it.
- `test/check_source_style.py` runs in the `build-and-test` job after the ASCII
  gate and fails on: a tab, a CR, a line over 120 columns, trailing
  whitespace, a `.c` whose first `#include` is not `"postgres.h"`, a header
  that includes it, and an `ereport` whose level can be an error without
  `errcode()` inside its own balanced parentheses (comments and literals
  blanked first, so a message that mentions "errcode" cannot satisfy it).

## Alternatives considered

- **A full pgindent pass plus a pgindent-diff CI job.** Rejected, as the #66
  decision already did. This is an out-of-tree PGXS extension, not a core
  patch; pgindent's conventions bind core, not extensions. A pass would
  re-indent every line with tabs and needs a maintained typedefs list to run,
  and the 79-column
  target it implies would mean hand-reflowing thousands of lines. The result is
  a whole-tree rewrite of `git blame` that buys no behaviour and makes every
  archaeology session start with "skip the reformat commit".
- **Leave the style undocumented.** Rejected. That is the state #66 complained
  about, and it is not neutral: an undocumented style drifts at the rate of the
  least careful commit, and review cannot enforce a rule no one wrote down. The
  errcode and include-order rules in particular protect real behaviour.
- **Document the style but gate nothing.** Rejected. The whitespace, width,
  include-order and errcode rules are exactly the ones a reviewer misses and a
  script does not, and they were already violated in the tree when the review
  was run. A documented rule with no check is the same drift, one step slower.
- **Gate everything in the section, including one-statement-per-line and the
  message prefix rule.** Rejected for now. Statement layout needs a real C
  parser to judge without false positives, and the prefix rule's R3/R4 need the
  SQL script's name bindings plus knowledge of which SQL functions reach a
  helper. A gate that guesses either way is worse than a review rule: it trains
  people to ignore it or to contort code to satisfy it.
- **A substring test for `errcode(`.** Rejected after an earlier attempt was
  fooled by a message whose text contained "errcode". The gate checks the
  call's own balanced parentheses on literal- and comment-blanked text, and its
  negative controls include exactly that case.

## Consequences

- The style is now a rule a reviewer can cite, and the enforceable half cannot
  regress silently: CI fails, naming the file and line.
- **The 79-column question stays closed.** 120 is a readability ceiling for
  split diffs, not a step toward pgindent's width; do not tighten it in a
  whitespace-only sweep.
- **The ADR 0056 exclusion is narrowed, not reversed.** A style gate now
  exists, but it enforces this tree's own rules; it does not reintroduce
  pgindent, the reflow, comment-opener normalization or PG header blocks.
- **A future file is covered automatically** (the gate globs `src/*.{c,h}`).
  A header that genuinely needs something from `postgres.h` must still rely on
  its includer; that is the PostgreSQL convention and the gate enforces it.
- **Two parts remain review-only** and will drift unless reviewers hold them:
  one statement per line and the errmsg prefix rule. The latter's R3 form is
  easy to get subtly wrong (a C symbol that differs from its SQL name, or a
  helper shared by several SQL functions); ARCHITECTURE.md spells out both
  traps.
- The gate is a static check with no before-tree, so it cannot tell a new
  violation from an old one; it simply requires the whole tree to comply,
  which it does as of this change.

## Addendum (2026-09-22)

The gate covers two further rules, added for issue #157. Both are the same
shape as the ones above -- a script can decide them, so a reviewer should not
have to.

- **`hash_create` must pass `HASH_CONTEXT`.** Regression insurance: the tree
  was already clean, the single historical violation having been fixed when the
  append de-dup table was scoped to the row's scratch context.
- **Every `Assert()` carries `/* invariant */` or `/* checked: <where> */`.**
  All 44 sites were classified when the rule landed (17 invariant, 27 checked),
  so there is no allowlist and no grandfathered set.

The Assert rule is worth more than its mechanics suggest. Writing the tags
forced a reading of every Assert, and that surfaced four where a cassert-only
check is the wrong mechanism -- one of them a live defect, where a `field_id`
read off a pending page fails an `Assert(false)` in a cassert build and, in
production, silently drops a whole field's terms from the document being
sealed, while the merge path ereports `ERRCODE_INDEX_CORRUPTED` on the same
value class. The tag records what the code does; it does not endorse it.

Two limits carry over unchanged: the gate still cannot tell a new violation
from an old one, so it requires the whole tree to comply; and it still decides
only what a lexer can decide -- no dataflow rule from #157's list was
implementable at an acceptable false-positive rate, and the ones that looked
mechanical had already been closed by stronger means in ADRs 0071, 0074 and
0075.

## Addendum (2026-10-05, PRs #331-#350)

**errdetail style** (#313 REGR-06, D7, PR #345). Every `errdetail` is a complete,
capitalized sentence ending in a period, per PostgreSQL's message style guide. Twenty-six
strings in nine files were lowercase fragments with no final period ("block %u, relation has
%u blocks"); all were rewritten with the same values and no SQLSTATE or `errmsg` change, and
the strings the walker work of PR #346 added follow the same style. The rule is written in
ARCHITECTURE.md's house style; `test/check_source_style.py` does not enforce it (a lexer can
find an `errdetail` literal, but deciding "complete sentence" is not mechanical).
