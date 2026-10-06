---
id: 0070
title: Push CI is scoped to main, and the interrupt-latency suites are excluded from the macOS leg
date: 2026-08-20
status: Accepted
summary: The push trigger is limited to main so a branch with an open PR stops running the whole matrix twice, and the two wall-clock cancellation suites are excluded from the report-only macOS leg while remaining gating on Linux, so that leg's result means something.
---

# 0070. Push CI is scoped to main, and the interrupt-latency suites are excluded from the macOS leg

## Context

Two unrelated CI defects surfaced while landing the #131/#147/#150 stack.

**Duplicate runs.** With a bare `push:` alongside `pull_request:`, every push to a
branch with an open PR started the entire matrix TWICE on the same commit — six
jobs, including the hardening leg that builds PostgreSQL from source on a cache
miss. Worse than the waste, the duplicates queue behind each other and a cancelled
superseded one is reported by `gh pr checks` as a plain "fail" with no sign it never
ran a test; one sat 30 minutes in apt before cancellation and read exactly like a
real failure while the identical commit was green elsewhere.

**A red report-only leg.** The macOS job (ADR: added for the BSD/glibc ctype
divergence `sql/82_encoding_aware_tokens` documents) was permanently red because
`sql/66_scan_interrupts` intermittently exceeds its wall-clock boundary there —
observed failing on one run while the identical commit passed on a concurrent one.
The job has `continue-on-error: true` at job level, so it never gated anything; but
a report-only job that always cries wolf gets ignored, and its failure had nothing
to do with the divergence the leg exists to find.

The project's CI policy is that only correctness gates the pipeline and that
benchmark-shaped numbers from shared runners must not.

## Decision

`push:` is scoped to `branches: [main]`; branch work is covered by the
`pull_request` run, which tests the merge result. A `concurrency` group is added
with `cancel-in-progress` enabled for pull_request events and DISABLED for main.

The two interrupt-latency suites are excluded from the macOS leg only, via a new
`make print-regress` target, and remain gating on every Linux leg.

## Alternatives considered

- **A concurrency block alone, to dedupe push-vs-PR** — the obvious-looking fix, and
  it does not work: `github.ref` is `refs/heads/<branch>` for a push but
  `refs/pull/<N>/merge` for a pull_request, so the two runs land in different groups
  and never cancel each other however the key is written. Scoping the trigger is what
  removes the duplicate.
- **`cancel-in-progress: true` on main as well** — rejected after review. This repo
  lands stacked PRs in bursts, so a merge train would have each merge cancel the
  previous commit's main run — and the macOS leg's own promotion criterion is
  "observed green on main", while a run cancelled mid-hardening skips the cache-save
  post-step. Validated on the very next merge train: six merges, six completed main
  runs, none cancelled.
- **Widen the latency boundaries for macOS** — rejected: pg_regress handles
  per-platform expected output poorly, and a boundary wide enough for the worst
  shared runner stops detecting the regression.
- **Move the latency assertions to a non-gating job everywhere** — rejected: it would
  weaken the Linux legs, where the margins have held and the suites are the only thing
  pinning cancellability.
- **Grep `REGRESS` out of the Makefile** — rejected after review found a silent hole:
  splitting the (1,900-character) definition into `REGRESS = ...` plus a
  `REGRESS += ...` continuation defeats a line-anchored grep WHILE leaving the
  drop-count assertion self-consistently satisfied, so the leg would silently run only
  the first line's suites, green. `make` evaluates the variable and survives any
  reformat.

## Consequences

A branch pushed with NO open PR gets no CI until the PR exists. That suits a workflow
where branch and PR are created together; `workflow_dispatch:` is the cheaper escape
hatch for a one-off. Naming branches also means TAG pushes no longer trigger CI —
inert today (no tags, no tag-driven release), and the thing to remember when one
appears.

The `REGRESS` filter is asserted, not trusted, because its failure mode is silent:
`REGRESS=` with an empty value runs ZERO suites and exits 0. Extraction failure and an
unexpected drop count both fail loudly; all paths were exercised verbatim under the
shell GitHub actually uses.

That shell is `bash -e {0}`, NOT `bash -eo pipefail`, unless a step sets `shell:` —
a claim this workflow previously made about itself and got wrong. Without pipefail a
pipeline takes its LAST command's status, so a `grep -c` returning 1 inside one does
not abort the step. Corrected in place rather than deleted (ADR 0058).

The exclusion is recorded on issue #156 so it does not become permanent by default:
re-including the suites needs them made runner-speed-independent — asserting that
cancellation happened and bounding work done, rather than wall-clock elapsed. The
change helps that goal by decoupling "promote the macOS leg to gating on its other 98
suites" from "fix 66/80", which were previously chained. First run after landing:
98 of 100 suites, all passing — the leg's first green.

## Addendum (2026-09-22)

**The exclusion is lifted.** `sql/66_scan_interrupts` and
`sql/80_maintenance_interrupts` now run on the macOS leg with the rest of the
`REGRESS` list, and the `SKIP=` variable, the `make print-regress` call and the
drop-count assertion that implemented the subsetting are gone from
`.github/workflows/ci.yml` — not left as a vestigial no-op. The `print-regress`
target stays in the Makefile for the next leg that needs a subset. Nothing about
what GATES changed: the macOS leg is still `continue-on-error: true` at job level,
and both suites are still gating on every Linux leg.

What made it possible is the prerequisite this record named: all three wall-clock
boundaries in those two files were replaced with bounds on WORK DONE (issue #156).
A backend-local `uint64` counter is incremented on the interrupt-checked loops —
POST pages decoded, POS pages crossed, dictionary entries replayed, pending pages
drained, build page rotations — and read back through `bm25_debug_work_units()`.
It is a C global specifically so it survives the abort of the statement being
measured and can be read from a PL/pgSQL `WHEN query_canceled` handler; a table
written inside the cancelled block would be rolled back before the handler runs.
Each suite then asserts `cancelled_work * 10 < full_work`, where the denominator is
the SAME operation run to completion moments later — a run each suite already
performed for other reasons, so the ratio costs no extra fixture time.

The alternative this record rejected, "widen the latency boundaries for macOS",
stays rejected for the reason given. What is new is that the failure DIRECTION
inverts: a slow or descheduled runner burns wall clock without advancing a loop
iteration, so it moves these assertions further from failing. The residual
sensitivity is to a much FASTER machine, and the remedy is to raise the fixture's
row count — which now widens the bound automatically, because the bound is a ratio
against that fixture's own uncancelled work rather than a constant to re-derive.

Two things measured during the conversion are worth recording because they
contradict claims made in the suites at the time:

- `sql/66_scan_interrupts`' phrase assertion had **no failing side at all** on the
  development machine. Its `elapsed < 300 ms` boundary was documented as carrying a
  one-to-two-orders-of-magnitude margin; the uncancelled query measures **142 ms**,
  i.e. inside the boundary, so a completely uncancellable build would have returned
  `t`. The 2026-08-18 A/B that reported `f` predates #184's phrase rework.
- `sql/80_maintenance_interrupts`' seal A/B no longer reproduces as recorded either:
  deleting the drain's delay point and `chain_ensure`'s check — the documented
  pre-fix condition, which once landed a 10 ms timeout at ~660 ms — now cancels at
  1,124 work units of 15,822 (~65 ms). #146 put chunked seals inside the drain's own
  page loop, so `bm25_page_alloc`'s and the accumulator's checks are on that path
  too. Defense in depth deepened; the deletion A/B stopped separating the trees.

Both suites were re-verified by induced failure against the new form, using the
model the defect class actually takes (a buffer content lock held across the work,
reproduced with a `HOLD_INTERRUPTS`/`RESUME_INTERRUPTS` pair) rather than by
deleting a check: cancelled work goes 11→263 of 526, 245→14,995 of 15,822, and
5→251 of 251, each failing its bound by 5x, 9.5x and 10x, and each flipping exactly
one line of expected output.

`sql/66_scan_interrupts`' fixture changed shape to get that margin: 500,000 rows
instead of 100,000, the never-queried per-row md5 term dropped, and
`bm25_native.debug_budget` raised so the corpus lands in ONE segment. The last is
load-bearing and was found by measurement, not reasoning:
`bm25_seg_scan_postings` runs once per (term, SEGMENT), so the pre-fix work figure
is `full/(terms × segments)` and does NOT grow with the corpus — at 400k rows in
seven chunked segments the defeated tree passed the same bound the fixed tree
passed.

## Addendum (2026-10-04)

The trigger decided here is amended by [0106](0106-ci-runs-on-manual-dispatch.md): CI
now runs on `workflow_dispatch` only, once a block of work has landed, and the scoped
`push: [main]` plus `pull_request` pair and the concurrency rule described above no
longer apply. The remark in Consequences that `workflow_dispatch:` is the cheaper
escape hatch for a one-off run is now the whole story, since it is the only trigger.
This record is not superseded: its decision to exclude the two interrupt-latency suites
from the macOS leg, and the 2026-09-22 addendum that lifted that exclusion, do not
depend on the trigger. Its macOS promotion criterion, observed green on `main`, now
counts manual runs of `main` only.

## Addendum (2026-10-06)

**The 2026-09-22 addendum's claim that the failure direction inverted was wrong.**
Converting the bounds to work done fixed the MEASUREMENT, but all three cancels in
the two suites were still DELIVERED by a 10 ms `statement_timeout`. The work a timed
cancel reports is the loop's work rate multiplied by the moment the timer is actually
serviced, and the addendum reasoned only about the first factor: a slow runner does
fewer units per millisecond. It assumed the timer arrives at 10 ms. On the shared
macOS VM it does not, and a late timer adds work, because the backend keeps running
its loop, checks live, until the cancel is finally pending.

Observed: `sql/66_scan_interrupts`' `phrase_cancelled_early` failed on five of the
last six `main` runs of the macOS leg (37416900341, 37410189321, 37375824380,
37258437391, 36603687757), and `sql/80_maintenance_interrupts`'
`seal_cancelled_early` failed on PR #352's run (37421409050). A diagnostic run of
macos-latest (37427419953, branch `diag/macos-cancel-latency`, 25 repetitions of each
timed cancel) measured why:

| Case | Timer serviced at | Cancelled units | Bound | Over |
|---|---|---|---|---|
| sql/66 phrase | 13–51 ms (median 22) | 22–88 | < 52.6 | 9 of 25 |
| sql/80 seal | 17–56 ms (median 47) | 963–1,605 | < 1,584 | 2 of 25 |

The phrase run's count kept climbing with the service time (~1.8 units/ms), so the
checks were live throughout. The cancel arrived late; the code did not service it
late. The second factor drifted too: on the development machine the cancelled phrase
count went from 11–13 (2026-09-22) to 30–34, and the seal's from 241–245 to 586–652,
with neither suite changed, because the scan and seal got faster. The `sql/80`
SEGREAD-10 case had already failed the same way on 2026-09-27, and its fixture was
tripled to tolerate a cancel landing at ~125 ms. Raising the row count, the remedy
this record's previous addendum prescribed, only moves the line.

**Decision: the cancel is injected at a fixed step, mid-loop, not timed.**

- Three pause points are appended to the `debug_pause`/`debug_cancel_at` table in
  `src/bm25_handler.c`: 17 `scan_post_page` (`bm25_seg_scan_postings`' POST page
  loop), 18 `drain_pending_page` (`bm25_pending_drain`'s page loop) and 19
  `debug_dict_entry` (`bm25_debug_postings`' DICT entry loop). Each sits immediately
  ahead of its loop's work unit and interrupt check, where no buffer content lock is
  held.
- A new `PGC_SUSET` integer, `bm25_native.debug_cancel_after` (default 0, the first
  hit), holds a `debug_cancel_at` cancel back until the backend's
  `bm25_debug_work_units()` counter has reached it. The threshold is on the work
  counter rather than a per-point hit count because the suites already reset and read
  that counter, so arrival and measurement share one scale.
- Each wrapper sets both GUCs with `set_config(..., true)` inside the block it
  cancels, so the abort that services the cancel also reverts them and the
  uncancelled run cannot inherit them. That is `sql/148`'s pattern (#305). At the
  point the backend makes `StatementCancelHandler`'s assignments on itself.
- Arrivals: 50 units (phrase), 500 (seal), 200 (SEGREAD-10), each inside the first
  window a held lock would span and below each suite's non-vacuity floor.
- The assertion counts from the arrival on both sides: `(cancelled - arrival) * 10 <
  (full - arrival)`. Once the cancel was pending, the operation did under a tenth of
  the work it had left. A counter wired to nothing cannot pass it: the counter never
  reaches the arrival, the cancel never fires, and the wrapper RAISEs.

**Why mid-loop and not the first hit.** The first version of this change fired at
each point's first hit, and CI ran green on it, macOS included (37428679114).
Adversarial review then showed that a first-hit cancel proves only that the loop's
FIRST check is live. Interrupts held from that first check to the loop's end (the
shape of a content lock taken during the first iteration and kept) gave counts of
1 / 1 / 1 and passed all three assertions, while the timed form had caught it. The
mid-loop arrival closes that gap.

Verified on the development machine (PG 18.6, cassert), three runs of each, every
defect model applied alone. Each model flips exactly its own assertion and no other
line of either suite:

| Case | Fixed tree | Hold across loop | Hold from 1st check | Bound on cancelled |
|---|---|---|---|---|
| sql/66 phrase (arrival 50) | 52 | 263 | 263 | < 97.6 |
| sql/80 seal (arrival 500) | 501 | 14,899 | 14,899 | < ~2,033 |
| sql/80 SEGREAD-10 (arrival 200) | 206 | 1,932 | 1,932 | < 373.2 |

For `sql/66` a third model, interrupts held only while the POS cursor is open (the
pre-#139 defect exactly), also gives 263. Nothing in any column reads a clock.

What the suites do NOT catch is a single DELETED check. A neighbouring check
services the cancel within a page or so: `chain_read_at`'s per-posting check for a
POST page, the nested scan's check for a DICT entry. The drain is the exception:
deleting its delay point leaves the cancel to the next chunk boundary
(`bm25_page_alloc`'s and the accumulator's checks), which serviced it at 1,286 units in
both the first-hit version (bound ~1,582) and the mid-loop one (arrival 500, bound
~2,033), still under the bound each time. That was equally true of the timed form. `ci.yml`'s `CHECK_FOR_INTERRUPTS` grep floors are what
catch a deletion. The suites catch a check going dead under a held lock, which is the
defect class ADR 0041 and #139 were about. Comments that claimed otherwise
(`src/bm25.h`'s work-counter header, `bm25_seg_scan_postings`) are corrected.

Alternatives rejected:

- **A tighter bound on the timed form, or a longer timeout.** Either still multiplies
  by the timer's service time, which no constant in the suite controls.
- **Re-excluding the suites from the macOS leg.** The leg would go green, but these
  suites would still be one runner-speed or code-speed change away from failing on
  Linux, which gates.
- **A per-point hit count (`name@N`) instead of a work threshold.** It would work,
  but it needs its own parser and check hook on a GUC that shares `debug_pause`'s, and
  it would put the arrival on a different scale from the measurement.
- **Raising `SIGINT` with `kill(MyProcPid, ...)`.** POSIX delivers a self-signal
  before `kill` returns, so the result would be identical. Setting the handler's two
  flags directly is what `debug_cancel_at` already does, and it adds no dependence on
  the platform's signal path.

Consequences:

- **The real timer is still exercised.** `sql/66`'s three 1 ms outcome checks (`@@@`,
  WAND and exhaustive ranked) assert only that the query is cancelled, so a late
  timer can fail them only by letting the query finish first. The diagnostic run did
  not time those queries on macOS. Their margin is the ranked queries' runtime
  (~46 ms on the development machine) against timer service times of up to ~56 ms.
  They have not failed. If they do, raising the row count is the right lever there,
  since an outcome check needs the query to outlast the timer.
- **The SEGREAD-10 fixture is oversized.** Its 1,200,000 rows were bought only for
  timer tolerance. It could shrink to any size that keeps ONE DICT page, keeps the
  200-unit arrival inside the walk, and lowers the 480-unit canary to match. That is
  left for a later edit so this one changes only the delivery mechanism.
- **Cost on the hot path.** `bm25_debug_pause_point` now returns after two
  first-byte tests when both GUCs are empty. It runs once per POST page on every
  scan.
- **Documentation.** The pause-point table's long description and ARCHITECTURE.md
  now list nineteen points, and ARCHITECTURE.md describes `debug_cancel_after`.
