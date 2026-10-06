---
id: 0119
title: A corrupt page must produce INDEX_CORRUPTED, and a silently wrong answer is caught only where a cheap structural invariant exists
date: 2026-10-05
status: Accepted
summary: The on-disk validation contract in one sentence -- a checksum-valid corrupt page of any origin must raise ERRCODE_INDEX_CORRUPTED rather than crash, read or write out of bounds, wait without bound or uncancellably, write into a page of another kind, or free a reachable page; a wrong answer from in-range values is caught only at a decode boundary with a cheap count, order, kind, gen or span invariant, and is otherwise a named residual.
---

# 0119. A corrupt page must produce INDEX_CORRUPTED, and a silently wrong answer is caught only where a cheap structural invariant exists

## Context

The project had pieces of an on-disk validation policy but no statement of it.
ARCHITECTURE.md said every on-disk quantity is bounded where it is first trusted, which
covers values that size a memory access. ARCHITECTURE.md and THEORY.md said the threat
model is on-disk corruption only. ADR 0071 gates block pointers before `ReadBuffer`, ADR
0095 bounds chain walks by the extent, ADR 0111 states the validated-walker contract
(count, kind, gen, extent, full span) for the POST and dense chains, and ADR 0110 applies
it to the pending chain. Nothing said what the write paths owe, whether a cycle is a
defect, or which in-range-but-wrong values must be caught.

The fresh-eyes review of 2026-10-04 filed 16 on-disk findings (#302, #303) and about 20 hygiene
items (#313) against that gap. Without a scope, each finding reopened the same argument:
is a silently wrong answer from a corrupt page a bug? Every validator is an approximate
check measured against "any corruption", so the honest answer has to say which
approximations are accepted.

## Decision

The scope, verbatim, decided by the user on 2026-10-05 (decision D1 of the grind on
#302-#314) and cited at the checks that implement it:

> A corrupt bm25 page, checksum-valid and of any origin, must produce
> ERRCODE_INDEX_CORRUPTED rather than a crash, an out-of-bounds access, an unbounded or
> uncancellable wait, a write into a page of another kind, or the freeing of a reachable
> page. A silently wrong answer from in-range values is caught only where the decode
> boundary has a cheap structural invariant (count, order, kind, gen, span); otherwise it
> is a documented residual.

"Of any origin" means the check may not assume the corruption is random: a forged page
(the `bm25_debug_poke_page` lever, ADR 0126) is in scope. "Cheap" means no hash probe,
visited set, allocation or syscall on a per-posting or per-document path; where a check
would cost the hot path, it is measured in the regime it affects before it lands, and it
lands only if that measurement is clean (D5, ADR 0072's addendum of this date).

How the 2026-10-05 findings classify under it:

- **First sentence (must ERROR):** every #302 item. A `segcat_root` of 0 waited forever
  on the metapage lock its own backend held (#302.A, PR #341); a wrong-kind `pending_tail`
  took a pending record (#302.B, #341); a corrupt retired range freed reachable pages
  (#302.C, PR #342); a `pd_special` near `BLCKSZ` read the opaque past the block (#302.D,
  #341); retired-list cycles held the singleton (#302.E, #342); `mark_chain` read a
  never-initialized page's header as the opaque (#302.F, #342). From #303: the sealed-chain
  spins (303.A) and the block-0 links that raised a retryable 40001 (303.B), both closed by
  the walker (ADR 0120, PR #346); the catalog entry with gen 0 (303.C, #341); fieldcfg
  `field_id` out of range (303.D), zero position deltas (303.F), a segment `field_count`
  that disagrees with the index (303.G) and an out-of-range pending `field_id` at the drain
  (303.H), all in PR #343.
- **Second sentence, invariant exists (caught):** DICT cross-page order and non-empty
  pages, POST `(docid, field)` order and progress, WAND header order (ADR 0120); the
  catalog/header role told apart by `seg_gen` (ADR 0062's addendum); `field_id == i` in
  the field-config page (303.D).
- **Second sentence, no cheap invariant (residual):** listed under Consequences.

An error the contract requires blocks the operation until REINDEX where the corrupt value
sits on a write path. 303.F and 303.H at the drain, and a corrupt retired range at VACUUM,
now refuse rather than drop data silently (D6, D3); that matches what the merge path
already did on the same value classes (ADR 0027).

## Alternatives considered

- **No written scope, decide per finding.** What the project did before. Every review
  pass re-litigated the same question, and the answers drifted between walkers (ADR 0111
  and 0110 each stated part of a contract the others did not follow).
- **Catch every wrong answer.** Not achievable at acceptable cost. A cycle among
  same-gen, same-kind, full-span pages of one dense chain answers from the wrong page,
  and the only general detector is a per-lookup visited set on the per-posting path.
- **Errors only for crashes and out-of-bounds access.** Leaves hangs, wrong-kind writes
  and frees of reachable pages, which damage more than the corrupt page itself (a freed
  live chain is overwritten under its readers), outside the contract.

## Consequences

- Reviewers and fix authors classify an on-disk finding mechanically: first-sentence
  shapes are defects; second-sentence shapes are defects only where a cheap invariant
  exists.
- Residuals under the second sentence, as of PRs #331-#350:
  - **303.A(f), narrowed** by the walker's revisit test: in LIVE, DOCMAP, NORMS, KEYMAP
    and POS, a link to another page of the same chain that is not the walk's first page,
    the previous page or the root can answer from the wrong page within the offset
    budget. On `bm25_livedocs_locate`'s path the wrong page is written (a tombstone bit
    on another document of the same segment).
  - **DICT within-page order** is not checked by `bm25_seg_dict_lookup` or the wildcard
    expander, only by the merge iterator and the dumps (ADR 0120).
  - **POST in-block disorder** across a cycle of two or more pages whose blocks are each
    internally out of order passes every cross-block test (ADR 0120).
  - **A KEYMAP root past the extent** reaches `ReadBuffer`'s own short-read error
    (`ERRCODE_DATA_CORRUPTED`, XX001), not XX002, because `bm25_seg_keymeta` takes no
    extent sample on the per-row INSERT path (ADR 0071's reasoning).
  - **303.E: impact-table values are trusted.** A load-time `tf <= max_tf` check cost
    about 2.7% on a two-field index in the pruning regime and was not landed (PR #344,
    ADR 0072's addendum). A never-decoded block's understated table can lower
    `global_ub`, and no load-time check could see it.
  - **Cross-segment links rely on two invariants checked by inspection only:** gens
    never repeat, and a segment is retired (its catalog entry removed) before any of its
    pages is reclaimed. The gen arm's XX002 versus 40001 decision rests on them (ADR 0120).
  - **The query-side pending readers** (the pending df and score passes in
    `bm25_stats.c` and `bm25_scan_rank.c`, the phrase stash in `bm25_scan_match.c`)
    skip an out-of-range `field_id` without raising. None indexes out of bounds, and the
    drain that would make the drop permanent refuses.
  - **A catalog entry whose `header_blkno` names a catalog page** raises 40001, not
    XX002: the header reader checks gen before role so the reclaim race stays retryable
    (ADR 0062), and a catalog page's gen 0 mismatches first.
  - **Debug and owner-only surfaces** are outside the strict reading: the
    `bm25_debug_page_flags`/`_seg_gen`/`_retire_xid_valid` probes read the opaque without
    a `pd_special` check, the retired-list debug SRFs take no singleton (a concurrent
    VACUUM can give them a spurious XX002), and the stamp levers other than the poke do
    not `XLogFlush`.
  - **`bm25_fieldcfg_write_init` runs under block 0's lock** on the INIT fork (#313
    META-11, otherwise fixed): that is the documented reservation order, on a fork no
    other backend can see.
- Pinned by `sql/136`-`sql/140` and `sql/149`-`sql/151`, `t/035`, and the pre-existing
  corruption suites.
