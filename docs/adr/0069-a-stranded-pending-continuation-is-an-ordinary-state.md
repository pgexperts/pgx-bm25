---
id: 0069
title: A stranded pending continuation is an ordinary state, not an assertion failure
date: 2026-08-20
status: Accepted
summary: bm25_pending_drain drops a continuation record whose parent header is absent without asserting, because an ordinary cancelled VACUUM produces that state and the Assert(false) guarding it PANICked assertion-enabled builds on a condition the branch exists to tolerate.
---

# 0069. A stranded pending continuation is an ordinary state, not an assertion failure

## Context

Since v7 (#57) a document too large for one pending page is written as several
same-tid parts, the later ones flagged `BM25_PENDING_DOC_CONT`, each starting on a
fresh page. `bm25_pending_drain` reassembles a document from consecutive same-tid
parts and has a branch for "a continuation with nothing to continue", which drops
the fragment rather than failing the seal — on the standing principle that a seal
must not be the thing that fails.

That branch carried `Assert(false)`, on the belief that only a corrupt chain could
reach it. The belief was wrong, and the assertion turned a deliberately tolerated
condition into a PANIC on any `--enable-cassert` build — which is what the
`hardening` CI job builds.

Reproduced directly: 600 multi-part documents, DELETE, VACUUM under
`statement_timeout = 15ms`, then `bm25_seal` →
`TRAP: failed Assert("false") ... bm25_pending_drain`, backend terminated by signal 6.

The mechanism is three facts in combination: a multi-part document's parts each
start on a fresh page; `bm25_pending_mark_dead` commits ONE Generic WAL record per
page with its delay/interrupt point at the top of the NEXT iteration; and
PostgreSQL has no undo, so physical page changes already committed when a cancel
lands are permanent. A cancel between two pages therefore leaves part 0 invalidated
and part 1 live, and the drain skips part 0 on its invalid TID and arrives at part 1
with no matching active document. Autovacuum cancellation is routine — any
conflicting lock request causes it — so this is steady state, not a rarity.

Found during adversarial review of #131; fixed separately.

## Decision

The assertion is removed and drop-and-continue is kept, with the reachability
argument and the measured trap recorded at the branch.

Dropping is CORRECT, not merely tolerable: reaching the branch means part 0 was
invalidated, and in production only `bm25_pending_mark_dead` writes an invalid TID,
gated on the bulkdelete callback judging that TID dead. Invalidation proceeds in
chain order, so invalidated parts form a PREFIX, and a surviving continuation
implies the whole document was judged dead.

No WARNING is emitted: the trigger is a routine cancelled autovacuum and the outcome
is correct, so logging would emit one line per stranded fragment during ordinary
maintenance. The checks that DO discriminate genuine corruption — page-kind
validation, the extent bound, the cycle cap — already run on this walk.

## Alternatives considered

- **Keep the assertion and document why it is unreachable** — the right answer if the
  premise had held. It did not; the state was reproduced from an ordinary cancelled
  VACUUM.
- **Emit a WARNING** — rejected for the log-spam reason above. The genuinely corrupt
  version of this state is indistinguishable from the benign one at this point, so a
  WARNING would carry no information the surrounding validators do not already give.
- **Make the sweep atomic across a whole document** — would remove the state at its
  source, but requires holding every page of a spanning document under one WAL record,
  which is unbounded by construction (a document may own many pages).

## Consequences

`sql/92_pending_stranded_continuation` reproduces the state deterministically with a
new test-only `bm25_debug_pending_invalidate_page`, rather than by cancelling a
VACUUM. Timing does not discriminate: at 600 documents a 30 ms timeout stranded
nothing and the seal completed, while 15 ms stranded a fragment and panicked — and
the boundary moves with the machine (a reviewer's box completed the sweep at 10 ms
and needed 4 ms). A suite built on that boundary passes vacuously whenever the cancel
lands outside the window, which is indistinguishable from passing correctly.

The branch has two disjuncts — no active document, or an active document with a
different TID — needing different chain shapes because chain order is append order.
Both are covered, the second by addressing the target page by CHAIN POSITION
(`bm25_debug_pending_nth_page`) rather than `head + 1`, which would depend on
sequential page allocation. A temporary per-disjunct probe verified each part
exercises the one it claims.

Standing lesson: assertions guarding states that are merely RARE rather than
IMPOSSIBLE are a liability in a codebase whose CI runs a cassert build. Several
findings in #65 were fixed by replacing a debug-only Assert with a real runtime
check; this is the same class seen from the other side.

## Addendum (2026-08-24)

This record was written about the SEAL side. The same state reaches every READ-side
pending walker, and tolerating it there needs the same test rather than merely not
crashing. #184 gave `pending_stats_by_field` and `pending_score_term` the drain's arm;
#195 (this addendum) gave it to `pending_df` and `pending_df_by_field`, which is the
last pair that reassembles a document across its parts. Recorded here rather than as a
new record because no decision changes — the decision above is that a stranded
continuation is an ordinary state to be dropped, and this is where its scope turned out
to reach.

`pending_df` mattered for a reason none of the others did. Every pending walker whose
output is a TID is corrected downstream for free: the fragment's tid belongs to a heap
tuple VACUUM was removing, so the heap recheck deletes whatever it produced. `df` has no
TID to filter on. It feeds `idf`, a corpus statistic, so counting the orphan inflated
`df` for every term the fragment carried — while `pending_global_stats` was already
skipping continuations, leaving `N` where it was. Measured on `sql/92`'s `strand4`: an
unrelated document's score for a term the fragment also carried read 0.633355 (df 2
against N 4) instead of the correct 1.100116 (df 1). Bounded to one fragment's terms and
self-healing at the next seal, hence low severity — but corpus-wide while it lasts.

The obvious simplification — skip every `BM25_PENDING_DOC_CONT` record, which needs no
per-document state at all — is NOT equivalent, and is worse. `bm25_pending_append_multi`
PARTITIONS a document's distinct `(field, term)` entries across its parts
(`hdr.ndocterms = j - jstart` over entries `[jstart, j)`), so an entry lives in exactly
one part and very often not part 0. A blanket skip therefore stops counting entries in
ordinary large documents. Built and measured on `sql/92`'s `strand3`, where nothing is
stranded and the big document legitimately carries the term in a continuation: its
unrelated neighbour's score went the other way, from the correct 1.474787 to 2.335308,
and `sql/77_pending_doc_ceiling`'s spanning phrase stopped matching entirely because the
term's `df` fell to zero and `bm25_term_idf` dropped it. That is a common state, not a
rare one. The tid-matching guard is the fix.

Neither df walker had a pre-existing SPANNING defect, for different reasons.
`pending_df_by_field` dedups per `(document, field)` and a `(field, term)` pair is unique
within a document, so its per-record `seen[]` reset already gave the per-document answer.
`pending_df` dedups per document ignoring field, where that argument does not hold — a
term in two columns is two entries and they can straddle a part boundary — and what
saved it is the caller: `bm25_term_idf` reaches it only under `field_count == 1`, where
each term has exactly one entry. Both were nonetheless moved onto explicit per-document
state, so the contract belongs to the walker rather than to a coincidence of its caller's
`field_count`.

Two other walkers are deliberately NOT given this arm. `pending_global_stats` skips every
continuation outright and is right to: each part repeats the whole document's `doclen`,
so there is nothing to reassemble. `bm25_dict_expand_wildcard` (`bm25_seg_read.c`) emits
a term SET rather than a TID and gates on `ItemPointerIsValid` alone, so a fragment's
terms still enlarge a wildcard expansion — and consume its `wildcard_max_expansions`
budget — until the next seal. After this fix those terms carry `df` 0 and are dropped
from scoring, which leaves the cap as the only residue; it is a different shape of
defect and is not addressed here.

## Addendum (2026-09-22)

The Context above says a multi-part document's parts "each start on a fresh page",
twice — once when introducing the v7 (#57) spanning format and again in the
three-fact mechanism paragraph. (Both occurrences are in Context; the Decision
section does not repeat the claim.) That is not what `bm25_pending_append_multi`
does, and the phrase survived a comment-accuracy sweep (#154 / PEND-14) because it
is line-wrapped here and a grep for it missed both occurrences. The packing
reality:

- Part 0 reuses the existing tail page whenever `meta.pending_tail_free >= runneed`,
  so it sits beside whatever other documents' records that page already holds.
- The last part's page keeps its unused remainder for subsequent inserts.
- A page therefore holds records from several different documents over time; an
  oversized document does not own whole pages.

The reachability argument is **unaffected**, and for a stronger reason than the
record gave. Two CONSECUTIVE parts of one document still never share a page, as a
consequence of the packing rather than as a rule: each part's `runneed` is budgeted
against the whole `PENDING_PAGE_CAPACITY`, and the inner loop breaks when
`runneed + esz > PENDING_PAGE_CAPACITY`, so the entry that ended part *k* is by
construction larger than the room part *k* leaves on its page. The append's
allocate-or-reuse branch is `if (meta.pending_tail == InvalidBlockNumber ||
meta.pending_tail_free < runneed)` — allocate when it is TRUE — and for part *k+1*
`pending_tail_free < runneed` is always TRUE, so part *k+1* always takes a fresh
page. A cancelled `bm25_pending_mark_dead` sweep, which commits one Generic WAL
record per page, can still land between a document's part 0 and part 1 exactly as
described above.

Nothing about the decision changes; only the stated mechanism is corrected.
`src/bm25_format.h` and the comment at the drain's drop branch now carry the same
wording.
