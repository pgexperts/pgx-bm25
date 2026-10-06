---
id: 0006
title: PHILOSOPHY
date: 2026-07-11
status: Accepted
summary: Engineering philosophy — correctness and crash/replica safety first, complete each layer against spec before moving on, near-100% discriminating test coverage, and adversarial cluster-by-cluster review before code is declared ready.
---

# 0006. PHILOSOPHY

Correctness and crash/replica safety first: the index is just pages, so core WAL + streaming
replication must Just Work, verified by cassert builds, crash-recovery TAP, and replica-equality
TAP. Complete a layer fully against the design spec before moving on (completeness over
tracer-bullets). Keep test coverage near 100% and make every assertion *discriminating* (a test
that passes with the feature neutered is a bug). New or significantly modified code gets
adversarial review before it is declared ready — M5, M4, M2b, and M6 were each implemented
cluster-by-cluster, every cluster adversarially reviewed (find → refute-by-default verify). That
process caught real defects before merge: for M5 a page-spanning key corruption and a
version-dependent field:term plan; for M4 a pending fabricated-positions silent-wrong-answer, a
merge desync on the no-REINDEX upgrade path, a mixed-index bare-phrase D7-degradation gap, and a
snippet UTF-8 heap overread (found only under AddressSanitizer — hence the standing note that
CI should gain a cassert/valgrind/ASan job); for M2b (block-max WAND) two silent unsafe-prunes
(an uncapped block-max shallow-skip that dropped a qualifying top-k doc, and a 3+-field bound
whose field-sum order could round below a real score), a field-scope over-offer exposed only when
WAND went live on the full suite, and an FMA-contraction score divergence between the WAND and
exhaustive paths (caught only by the bit-exact parity gate, which motivated pinning
`-ffp-contract=off`); and for M6 (boolean + wildcard) a `boost.weight = 0` silent-drop (a
zero-boost positive leaf zeroed its idf so the doc never entered the accumulator, yet its presence
bit was set → the doc vanished; fixed by rejecting `weight ≤ 0` at parse) and a bound-parameter
NULL-deref that CRASHED the backend (a cached `$1::jsonb` plan reused across a single-leaf →
multi-leaf transition kept a stale `qtermlen`, so `bm25_analyze(NULL, len>0)` derefed; fixed by
resetting `qtermlen` on rescan). M6 review ALSO uncovered the pre-existing whole-index
secondary-key rank-collapse bug — found, empirically confirmed on both operators, and fixed on a
scoped post-merge branch rather than smuggled into M6. That fix's own design+review taught three
lessons: the originally-approved FAIL-LOUD ERROR was found INFEASIBLE mid-implementation (the `&@@`
operator is projected per-row on EVERY ranked query, not just on re-eval, so erroring would break
them all) and pivoted to the score-stash (see TRADEOFFS), verified across every scan path before
merge; the stash then worked on PG 17/18 but NOT PG 16 (its planner won't incremental-sort an
`amcanorderbyop` index → the secondary-key form full-sorts an unordered scan and still collapses),
so PG16 was DROPPED (min PG17, `#error` floor) rather than ship version-inconsistent ordering; and
a struct field added to a shared header rebuilt with only `rm dylib && make` (no `make clean`)
produced a mixed-`.o` build whose disagreeing struct offsets surfaced as a spurious
`cache lookup failed for text search dictionary 0` — a 100%-deterministic BUILD artifact, not a
code bug (PGXS does not track header deps; struct/header changes need a full `make clean`). A
recurring lesson across milestones: bit-exact parity alone is NOT sufficient (a WAND that never
prunes is still bit-exact), so anti-neuter gates (`blocks_skipped`/`deep_check_skips > 0`; a
wildcard double-count discriminator; a boolean must_not that must EXCLUDE, not merely not-add;
rank-collapse fixtures whose true order is the REVERSE of id) witness that the headline behavior
actually fires. Bugs are fixed at root cause and reproduced first (the scored-scan FATAL traced
with `backtrace_functions`; the boost-0 and NULL-deref both RED-confirmed before the fix).
The BriefBank §5 conformance suite (test-only, see TRADEOFFS) was executed subagent-driven — a
fresh implementer plus a two-verdict spec/quality review per grammar-form task, then a 4-lens
adversarial whole-branch review (assertion-correctness / version-portability / corpus-consistency /
TAP+docs) with an opus synthesis — and that discipline again caught real issues before merge: a
corpus fixture where row 7's `here` is a stopword, tying two documents' doclen and neutering the
strictly-descending-score assertion (caught when the score task blocked on the tie, not by a green
run); a replica-TAP phrase query with a `, id` secondary key that silently rank-collapsed to
id-order — testing nothing about ranked replication — (caught by the final review); and a
final-review pass that hardened a weakly-discriminating per-field-boost assertion (the `[17,18]`
order alone doesn't isolate the 5× boost from length-norm → pinned `score(17) > 3·score(18)`) and
a vacuously-satisfiable injection assertion (empty term-set `<@` anything → pinned the literal
token set `= {10}`). The two native-side limits it surfaced (F1 bare-`@@@`
off-index silent-zero — since ROOT-CAUSED to a planner-index-choice + inert `bm25_match_jsonb` and
FIXED fail-loud, see TRADEOFFS + `52_jsonb_filter_index_only`; F2 snippet-single-leaf-only) are
documented in the grammar-mapping doc and worked around by the suite rather than papered over. Documentation (ARCHITECTURE.md, THEORY.md) and this ADR are kept
in sync with each pushed change.
