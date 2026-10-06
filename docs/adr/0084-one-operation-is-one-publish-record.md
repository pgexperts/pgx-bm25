---
id: 0084
title: Bound the accumulator by maintenance_work_mem; one operation stays one publish record
date: 2026-08-23
status: Accepted
summary: The build, merge and pending-drain accumulators are cut at maintenance_work_mem (autovacuum_work_mem in an AV worker) and seal a segment per chunk, but every chunk of one operation is published in a SINGLE WAL record, because publishing them one at a time leaves committed states in which a doc exists in both a new segment and its still-live source and is therefore double-scored.
---

# 0084. Bound the accumulator by maintenance_work_mem; one operation stays one publish record

## Context

`BM25Accum` is a pure in-memory inverted index: no spill to disk, no `tuplesort`,
no incremental flush. Until now nothing bounded its residency at all. A build held
the whole heap's postings; a merge re-accumulated every live document of up to
`BM25_MERGE_MAX_INPUTS` segments (a cap on the segment COUNT, not on the documents
in them); a drain held the whole pending chain. The file's own header said so —
"an accumulator that is already unbounded by `maintenance_work_mem`" — and ADR 0033
recorded the bound as explicitly out of scope, with `amusemaintenanceworkmem` left
`false`.

Doing nothing was not an option because of WHERE these run. `bm25_vacuumcleanup`
seals and then merges, so both feed this accumulator inside an **autovacuum
worker** — a process with no user-visible failure path, so an OOM there does not
surface as a failed statement; the index simply stops being maintained. The
opportunistic seal runs inside a user backend's INSERT. And the tree already
documents the shape that makes the pending list arbitrarily large: raise
`bm25_native.seal_threshold`, bulk load, and wait for a VACUUM or a manual
`bm25_seal()`.

Below that ceiling sat a second, sharper failure: every growth site used plain
`repalloc`, which caps at `MaxAllocSize`. `terms[]` at 40 B/term, a hot term's
`post[]` at 32 B/posting, `docs[]`/`keys[]`, and the postings scratch can each cross
1 GB on a large corpus, and the result was `invalid memory alloc request size N` —
a message naming no relation, no operation and no tuning knob — raised mid-seal.

The obvious fix is the GIN/nbtree one: accumulate to a budget, seal a partial
segment, continue. The format already supports many segments and the merge ladder
already consolidates them. What is NOT obvious, and is the substance of this
record, is what that does to the single-record seal invariant (ADR-era D-SEAL/C2).

## Decision

**Bound the accumulator, and publish every chunk of one operation in ONE record.**

Concretely:

1. `bm25_maintenance_budget_bytes()` returns `autovacuum_work_mem` when the process
   is an autovacuum worker and one is configured, else `maintenance_work_mem` —
   `ginInsertCleanup`'s precedence. `bm25_accum_over_budget()` measures
   `MemoryContextMemAllocated(recurse=true)` over the accumulator's single context,
   minus a baseline captured before the first document, so the budget means
   "document data on top of fixed overhead". All three feeders check it and, when
   over, seal what they hold and start a fresh accumulator.

2. **Budget exhaustion seals; it never errors.** An error out of `amvacuumcleanup`
   leaves the index permanently un-maintained; an error out of the opportunistic
   seal fails every subsequent INSERT past the threshold. Both are the disease
   rather than the cure.

3. **One operation is one atomic publish record — not one segment.**
   `bm25_segcat_publish_append` takes an entry array and appends all of them in the
   record that also resets the pending anchor; `bm25_segcat_publish_swap` takes an
   array of new entries and publishes all of them in the record that retires every
   input and flips `segcat_root`.

4. Chunk boundaries are placed where one TID cannot land in two segments: the merge
   cuts only between INPUT SEGMENTS, the drain only between pending PAGES (and
   never under a content lock), and the build per document.

5. `repalloc_huge` on the four corpus-scaling growth sites, and a refusal of the
   `uint32` capacity wrap at each doubling site.

6. A byte estimator (`bm25_accum_estimate_bytes`, living beside the structs it sums
   `sizeof`s of) drives a selection trim that keeps only merge sets which can reduce
   the segment count; `bm25_merge_execute` returns PROGRESS rather than "merged
   something".

7. `bm25_native.debug_budget` (KB, `PGC_SUSET`, 0 = off) overrides the budget, as a
   TEST LEVER only.

8. `amusemaintenanceworkmem` becomes `true`, correcting ADR 0033's deferral.

## Alternatives considered

- **N output segments published by N independent swaps** (the shape the issue
  proposed, on the grounds that each swap is atomic so crash safety is unaffected).
  **Rejected: it is wrong, not merely slower.** Atomicity was never the property
  doing the work — consistency of the intermediate states is. Between swap *i* and
  swap *i+1* the catalog is a fully committed, WAL-durable state other backends
  read. `bm25_scan_snapshot` captures `pending_head` and the whole live catalog
  under ONE metapage SHARE lock, and `bm25_scores_add` SUMS per-TID contributions
  with no cross-segment dedup, so a partial output published while its inputs are
  live double-scores every document in it and double-RETURNS it on the `@@@` path —
  durably, if a crash lands there. The mirror-image interleaving, retiring inputs
  before their remaining documents are published, deletes live documents. No
  ordering of per-chunk swaps over an arbitrary cut avoids both. (The one form in
  which "N swaps" IS correct is at input-segment granularity, where a swap
  publishing an output that fully owns inputs 1..i and dropping exactly those is
  consistent. Its only advantage over N-outputs-one-record is that partial progress
  survives a crash, which is worthless for a restartable maintenance operation, and
  it costs a catalog-chain rebuild and a retired-list page per swap.)

- **`ERROR` when the budget is exceeded**, naming `maintenance_work_mem` (the
  issue's own "honest interim"). Rejected: see decision 2. The issue itself calls
  seal-and-restart "the real fix".

- **Mid-segment doc-range windowing in the merge**, so a single oversized input
  could be split. Expressible — the id remap already skips documents — but it costs
  a full dictionary and postings replay per window, i.e. O(windows x segment bytes)
  of read amplification, and it exists only to serve the shrinking legacy case that
  overshoot + WARNING + REINDEX already covers. Deferred; it stacks cleanly on the
  N-outputs publish if a real corpus ever needs it.

- **Capping `BM25_MERGE_MAX_INPUTS` by total `live_ndocs`** (the issue's interim).
  Rejected as the permanent answer: document count is a proxy for bytes that is
  wrong by orders of magnitude across doclen, position and term-shape variation; it
  says nothing about a single input exceeding the budget; and it needs a 2-input
  floor to avoid stalling the ladder, with which it can still exceed the budget. Its
  descendant — the byte estimator driving the selection trim — is half of the
  permanent answer, and seal-and-restart is the other half.

- **Charging the accumulator's fixed baseline against the budget** (no baseline
  subtraction). Rejected: the accumulator pre-allocates ~1.2 MB before the first
  document, and `maintenance_work_mem`'s legal floor is 1 MB, so a floor-configured
  system would seal one segment per document.

- **A streaming k-way segment merge** needing O(1) memory, which this format permits
  in principle (sorted DICT chains, docid-ascending postings, an order-preserving
  remap). This is the right long-term shape and would also dissolve the ladder floor
  below — but it is a new segment builder, not a bound on the existing one. Recorded
  as the successor, not attempted here.

- **Making `bm25_native.debug_budget` `PGC_USERSET`, or documenting it as a tuning
  knob.** Rejected on a stronger ground than the wildcard guardrails' (ADR 0025):
  those decide whether a query may run, whereas this changes the PHYSICAL LAYOUT of
  shared on-disk state — a lowered budget makes the next seal or merge publish more,
  smaller segments that every other session then scans — and it is the one knob that
  can make the segment count grow without limit. `maintenance_work_mem` is the
  contract.

## Consequences

- **The segment-count ladder gains a memory floor, and it is operator-visible.**
  `BM25_TARGET_SEGMENT_COUNT` (8) is unreachable for a corpus whose accumulator
  footprint exceeds 8x the budget; the steady state floors at roughly
  footprint/budget segments and scan cost grows with it. The remedy is documented:
  raise `maintenance_work_mem` (or `autovacuum_work_mem`) and run `bm25_merge()`.

- **`bm25_merge()` needed a termination guard it did not previously need.** With
  outputs capped at the budget, a rung of budget-sized segments merges into the same
  number of budget-sized segments and the force loop would re-select it forever. The
  selection trim makes that set unattractive; the executor's progress return makes
  termination independent of the estimator's quality. This is why the estimator's
  accuracy is a performance concern and never a correctness one.

- **A single input segment that alone exceeds the budget is an accepted overshoot**,
  logged as a WARNING naming the index (counts in `errdetail`, so `VERBOSITY terse`
  keeps output stable). It cannot be split, and REINDEX re-buckets it. It has no
  regression coverage: the trim refuses infeasible sets except the lone tombstoned
  segment it deliberately exempts, and VACUUM's own cleanup merge normally consumes
  that state before a test can observe it.

- **`bm25_upgrade`'s full rewrite now emits several segments** on a large index
  rather than one, with the format re-stamp still riding the single publish record.

- **No on-disk format change.** Multi-segment catalogs, multi-page SEGCAT chains,
  the swap record and the retired list are all existing shapes; chunking only changes
  how many entries existing writers emit. A standby replays ordinary Generic WAL and
  an old binary reads a normal multi-segment catalog.

- **One existing suite changed.** `sql/80_maintenance_interrupts`' half-million
  document pending list is the only regression corpus large enough to cross a chunk
  boundary at the DEFAULT budget; its per-segment listing became a total, because the
  count there depends on `maintenance_work_mem` and on struct padding. Everywhere
  else the default still yields exactly one segment.

- **`bm25_segment_build_and_commit_swap` is gone**, having become a wrapper with no
  callers once the merge moved to the entry-array form; `bm25_segcat_publish_swap`
  is the name to cite.

- **The `MaxAllocSize` cliff needs both halves.** Below a 1 GB budget the seal fires
  before any array reaches it; above one — and `maintenance_work_mem` legally
  exceeds 1 GB — only `repalloc_huge` keeps it unreachable. Those paths are not
  testable at suite scale and are covered by review, not by a test.

- **`amusemaintenanceworkmem = true` is inert today**, because
  `amparallelvacuumoptions` is `VACUUM_OPTION_NO_PARALLEL` and `vacuumparallel.c`
  consults the flag only for indexes that participate in parallel vacuum. It is set
  because it is the true answer, and so the budget is not silently multiplied across
  workers the day this AM gains a parallel vacuum option.

## Addendum (2026-08-23)

Adversarial review of the implementation, before it landed, corrected three things
in the decision above. Recorded because two of them are the kind of mistake the
decision itself warns against.

**The append publish must have NO ceiling on N.** The first implementation refused a
batch larger than one SEGCAT page (~203 entries), reasoning that only a pathological
`bm25_native.debug_budget` could produce that many chunks from one drain. Wrong on
both halves. `maintenance_work_mem` is `PGC_USERSET` with a 1 MB floor, and a few
thousand large documents drained at that floor really do produce a thousand chunks --
reproduced. And the failure was not a clean refusal: the check ran *after* every
chunk had been built and WAL-logged as orphan pages, the abort left the pending
anchor untouched so the next seal did the same thing, and `bm25_vacuumcleanup` seals
BEFORE it reclaims orphans, so the sweep that would have freed them was never
reached. Measured: 344 MB of stranded pages after two attempts, nothing published,
latching -- the identical "index stops being maintained" wedge this record exists to
remove, reached through a different door. A batch too large for one page is now laid
on a fresh orphan CHAIN and prepended by pointing its tail at the old root inside the
flip record, which is the indirection `bm25_segcat_publish_swap` had all along.

**The estimator has to predict the quantity the cut measures.** `bm25_accum_over_budget`
reads `MemoryContextMemAllocated` -- blocks, including allocator slack --
while `bm25_accum_estimate_bytes` sums `sizeof()`s, which measured ~1.6x low. Since
the selection trim compares predicted chunks against input count, a ladder of
budget-floored segments looked half as expensive as it is: the trim declared an
infeasible set feasible, and `bm25_merge_execute` computes progress only *after* the
swap, so the whole index was read, rewritten and retired for nothing, on every
autovacuum, forever. That is exactly the regime the trim exists to prevent, so the
estimate now carries a slack factor -- set above the measured ratio, because
over-estimating costs a refused merge while under-estimating costs a no-op rewrite of
the index.

**The overshoot WARNING needed a threshold, not a predicate.** Every segment the
chunker produces lands just past the budget by construction -- that is why it was cut
there -- so warning on "exceeds the budget" fired on the ordinary steady state, once
per tombstone rewrite, into the autovacuum log, with a hint recommending a REINDEX
that would reproduce the same segment. It now warns at twice the budget, which
separates "this index is at its budget floor" (normal, documented) from "this segment
predates the budget" (actionable).

## Addendum (2026-08-24)

**The trim's shed order has to rank tombstone picks last, not by estimate alone.**
Review of the record above found the exemption unreachable in the one case it exists
for. When a heavily-tombstoned oversized segment is bundled with a permanently
budget-floored rung -- both triggers fire, so `bm25_merge_select` picks them together
-- the set is infeasible and the trim sheds largest-estimate-first. The tombstoned
giant *is* the largest estimate, so it went first; the rung was then whittled to a
lone non-tombstone survivor, which the trim refuses; and the pass returned 0. Every
pass. The giant's dead space was never reclaimed while that rung persisted, which is
precisely what the exemption's own comment promised could not happen.

The trim now ranks a non-tombstone candidate ahead of a tombstone one and only then
by estimate, so a tombstone pick is shed only when nothing else remains to shed. The
cost of the ordering is that an infeasible set sheds one or two more members before
reaching feasibility; the benefit is that the exemption is reachable from the
selector's real output rather than only from a hand-constructed single-segment set.

`sql/105` pins it as reclaimed dead space (`max(ndocs - live_ndocs)` across the
catalog: 2700 before, 0 after) rather than as "the trim kept something" -- a trim that
kept an arbitrary rung member would also be non-zero, and that is the wrong answer.
The row count is asserted alongside and is unchanged in both directions, which is the
control: this is a space-reclamation defect, not an answer defect.

That fixture also reaches the accepted-overshoot WARNING deterministically, which the
suite header had recorded as unobservable because VACUUM's cleanup merge consumes the
state first. The path is now exercised; the message text is still not asserted.

## Addendum (2026-10-05, PRs #331-#350)

The "~1.2 MB before the first document" figure in the rejected "charge the baseline" option
no longer holds. With the full-term map key (ADR 0125) `BM25Accum.baseline` measures about
130 KB on a 64-bit build, below `maintenance_work_mem`'s 1 MB floor, so the specific argument
given (a floor-configured system would seal one segment per document) is no longer true as
stated. Subtracting the baseline is still the better rule (the budget then means "document
data"), and nothing changed in the code; the `BM25Accum.baseline` comment was corrected in PR
#349.
