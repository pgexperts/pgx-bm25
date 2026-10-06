---
id: 0106
title: CI runs on manual dispatch only
date: 2026-10-04
status: Accepted
summary: The CI workflow's only trigger is workflow_dispatch, run once a block of work has landed on main, because per-PR runs on a serially landed stack were superseded within minutes by the next merge; nothing runs automatically, branch protection cannot require the checks, and the macOS leg's promotion criterion counts manual main runs only.
---

# 0106. CI runs on manual dispatch only

## Context

ADR 0070 scoped the push trigger to `main` and kept `pull_request`, so a branch with
an open PR ran the matrix once, on the merge result, and `main` ran it again after the
merge. That record also fixed the concurrency group (cancel in progress for
pull_request events, never for `main`) because this repository lands stacked PRs in
bursts.

The cost showed up in the 2026-10-04 grind, which landed a serial stack of PRs: each
PR's checks were superseded minutes later by the next merge, and the stack was
verified as a whole on `main` afterwards anyway. The per-PR runs bought little for the
matrix they spent (eleven jobs in the first manual run below, including the legs that
build PostgreSQL from source on a cache miss; ADR 0070).

## Decision

`.github/workflows/ci.yml` triggers on `workflow_dispatch` only. CI runs once, at the
end of a block of work, after a stack of PRs has landed on `main`:

```
gh workflow run CI --ref main
```

The concurrency group (`github.workflow` plus `github.ref`) now serializes manual
runs on a ref and never cancels one, with `cancel-in-progress: false`. A second
`gh workflow run` on `main` queues behind the first. A run cancelled mid-hardening
skips the cache-save post-step, which leaves the shared cassert PostgreSQL cache cold
for longer, and a cancelled run reads in `gh` like a plain failure with no sign it
never ran a test (the same observation ADR 0070 made about superseded runs).

This amends ADR 0070 and does not supersede it. 0070 also decided that the two
wall-clock interrupt-latency suites are excluded from the macOS leg (lifted by its
2026-09-22 addendum), and that decision does not depend on the trigger. Only the
trigger scoping and the concurrency rule it describes are replaced.

## Alternatives considered

- **Keep the scoped `push: [main]` plus `pull_request` pair of ADR 0070.** This is the
  previous state. Rejected for the reason above: on a serially landed stack its
  per-PR runs were superseded before anyone read them. Restoring automatic runs means
  restoring that scoped pair, not a bare `push:`, which runs the matrix twice for a
  branch with an open PR (ADR 0070 explains why a concurrency key alone cannot dedupe
  the two).
- No other trigger shape (a schedule, a label-gated PR run) is recorded as having been
  considered.

## Consequences

- Nothing runs CI automatically. A PR merged without a manual run afterwards is
  untested in CI. The local gates stand in for per-PR checks: the build, the SQL
  regression suites and the TAP suites on PG 17, 18 and 19.
- Branch protection cannot require these checks, since PRs no longer produce them.
- The macOS job's promotion criterion, observed green on `main`, counts manual `main`
  runs only. The first manual run on `main` (run 37258437391, commit 5cb24e3) passed
  every other job, including every gating leg; the report-only macOS leg failed
  `66_scan_interrupts`, so it did not count as a green observation.
- ADR 0070's remark that `workflow_dispatch:` is the escape hatch for a one-off run
  no longer describes a second option, since it is now the only trigger.
- README.md and bench/README.md describe the manual trigger. The workflow's header
  comment records the consequences above and how to restore the scoped pair.
