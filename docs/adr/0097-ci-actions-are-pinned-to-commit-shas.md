---
id: 0097
title: CI actions are pinned to commit SHAs, not version tags
date: 2026-09-23
status: Accepted
summary: Every action the workflow uses is referenced by the full commit SHA its version tag resolved to, with the version as a trailing comment, so the code a CI job runs cannot change without a change to ci.yml.
---

# 0097. CI actions are pinned to commit SHAs, not version tags

## Context

Issue #156 (BLD-05): `actions/checkout` and `actions/cache` were referenced by floating
major tags (`@v4`, `@v5`) at every call site. A tag is mutable: whoever controls the
action's repository can move it, and the code that runs inside the job changes with no
change to this repository and no trace in its history. The practical exposure was low --
both are first-party GitHub actions, the workflow's top-level `permissions:` block is
`contents: read`, and no secret is referenced -- but low exposure is not none, and the
workflow was about to grow jobs that upload artifacts.

## Decision

Every `uses:` in `.github/workflows/` names a full 40-character commit SHA, followed by a
comment giving the version tag it was resolved from (`# v5.1.0`). That holds for all 13
call sites across `actions/checkout`, `actions/cache` and `actions/upload-artifact`, and
for any action added later. Updating an action means resolving its new tag to a SHA and
changing both the SHA and the comment in one edit.

## Alternatives considered

- **Keep the major tags.** Rejected: it is the one configuration in which the code a job
  runs can change without a commit here.
- **Pin to exact version tags (`@v5.1.0`).** Rejected: an exact tag is still a movable
  ref; it narrows the window without closing it.
- **Dependabot or Renovate to manage the pins.** Not adopted now. It would automate the
  SHA bumps, but it is another integration with write access to open PRs, for three
  actions that change rarely. Nothing here prevents adding it later.

## Consequences

- Action updates are manual: a newer release does nothing until someone re-resolves the
  tag and edits ci.yml. Security fixes in an action arrive only that way.
- The trailing version comment is documentation, not enforcement; a SHA edited without
  its comment would silently mislabel itself. Review has to check both.
- The same rule applies to new jobs: the report-only legs of ADR 0098 were added pinned.
