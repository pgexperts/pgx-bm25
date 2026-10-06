---
id: 0090
title: AddressSanitizer gates CI
date: 2026-09-21
status: Accepted
summary: A gating asan job runs the full installcheck (SQL + TAP) with the extension built under -fsanitize=address and libasan preloaded into a non-instrumented cassert PostgreSQL, reversing the "NO ASan" rejection ADR 0056 recorded.
---

# 0090. AddressSanitizer gates CI

## Context

ADR 0056 (2026-08-11) lists "add an AddressSanitizer job" among its rejected
alternatives, "excluded by standing user decision", and leans on the review
verifier's argument that `--enable-cassert` already defines
`CLOBBER_FREED_MEMORY` and `MEMORY_CONTEXT_CHECKING`, so the `hardening` job
catches much of the use-after-free and overrun class on palloc'd memory.

The next review round (REVIEW-2026-08-16, finding BLD-02; issue #142) kept finding
exactly that class anyway: an unvalidated `field_count` bounding a 32-element
STACK array (#133, a backend crash on demand once a corrupt page reaches it), an
unbounded `reachable[]` write (#134), and an unbounded pending append (#136). The
cassert argument does not reach them. `MEMORY_CONTEXT_CHECKING` writes a sentinel
byte after a palloc chunk only when the request is smaller than the chunk it lands
in -- always for a large (dedicated-block) chunk, never for a small power-of-two
request of 8 bytes or more such as `palloc(64)` (`aset.c`) -- and inspects it when
the chunk is freed,
reallocated or its context checked. So it sees SOME writes past a palloc'd chunk,
late, and sees neither a READ past the end nor any overrun of a stack array. UBSan, the other
half of `hardening`, instruments a disjoint class -- signed overflow, misaligned
access, invalid enum values, all within a valid object.

No SQL suite can stand in. `sql/69_decode_boundary`,
`sql/78_trust_boundary_bounds` and `sql/79_page_content_bounds` exist because a
regression suite cannot fabricate a corrupt page; they drive `bm25_debug_*`
probes instead. A real corrupt-page overread is caught by ASan or by a crash,
never by `pg_regress` diffing output.

The job landed in e8ca59a (PR #168, closing #142) on 2026-08-17 without a
decision record, so ADR 0056 went on stating the opposite. The #67 triage
(2026-09-21) found the drift; this record closes it retroactively.

## Decision

`.github/workflows/ci.yml` carries a gating `asan` job:

- It reuses the `hardening` job's source-built cassert PostgreSQL 18 **and its
  cache key**, so whichever of the two runs first pays the build.
- Only the EXTENSION is instrumented (`PG_CFLAGS=-fsanitize=address
  -fno-omit-frame-pointer`, `PG_LDFLAGS=-fsanitize=address`). PostgreSQL is not,
  so `libasan` is `LD_PRELOAD`ed into the postmaster before it allocates, and the
  preload is EXPORTED for the whole step so the TAP tier's own
  `PostgreSQL::Test::Cluster` postmasters inherit it too.
- `ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0`. Leak detection
  is off deliberately: PostgreSQL frees by resetting contexts and by exiting, so
  it would report the server's design and bury the bounds errors.
- A CANARY runs before any suite: the shipped module must export `__asan_*`
  symbols, or the job fails. "installcheck passed under ASan" is equally
  satisfied by a build that silently is not instrumented.
- The server log, not `regression.diffs`, is the diagnostic it prints on failure:
  an ASan report kills the backend, which `pg_regress` shows only as a lost
  connection.

## Alternatives considered

- **Keep the ADR 0056 rejection.** Lost on evidence: three memory-safety defects
  in one review round, one of them a stack array cassert cannot see at all.
- **Rely on `hardening` alone.** Same reason. UBSan and cassert's allocator
  instrumentation cover other classes; neither observes an out-of-bounds read.
- **Instrument PostgreSQL itself.** Not done. Preloading the runtime into a
  non-instrumented host is the configuration ASan's own diagnostic asks for, and
  it lets the job share `hardening`'s cached build instead of paying a second
  source build.
- **Leak detection on.** Rejected, for the reason given in the decision.

## Consequences

- ADR 0056's alternatives section is now historically true and currently false;
  its addendum points here.
- Any new C code runs under ASan on every PR. An ASan report makes the job fail
  even when every suite's output matched.
- The canary, not the green result, is what proves the job is testing anything.
  A change to the build step that drops `PG_CFLAGS`/`PG_LDFLAGS` has to trip it.
- Coverage is exactly what the suites execute. ASan cannot see a corrupt-page
  path no probe reaches, so the `bm25_debug_*` trust-boundary probes remain the
  way to drive decoders over hostile bounds.
- **What ASan sees here is narrower than "every out-of-bounds access", because
  its view of the heap is at `malloc` granularity and PostgreSQL 18 poisons no
  palloc-chunk or buffer boundary for it** (`memdebug.h` and `mcxt.c` carry only
  Valgrind client requests). Instrumenting PostgreSQL itself would not change
  that. It sees stack objects (the #133 class, and every `PGAlignedBlock` page
  copy) and the edges of each `malloc`'d block, which includes a dedicated
  large-chunk block such as a `palloc(BLCKSZ)` page copy. It does NOT see: (a) an
  overrun that stays inside an AllocSet block -- palloc chunks up to the
  allocChunkLimit are carved from larger `malloc`'d blocks, so a neighbouring
  chunk is addressable memory; or (b) a decoder reading past one page into the
  next inside `shared_buffers`, which is a single addressable mapping ASan does
  not track. The practical consequence: the copy-then-unlock discipline (ADR
  0063, ADR 0083) is also what makes a decoder ASan-visible, since it decodes a
  stack or palloc'd page copy with a redzone at its end; a reader that decodes a
  shared buffer in place gets no such protection.

## Addendum (2026-10-05, PRs #331-#350)

The asan job now runs as a matrix over PG 17.11 and 18.6 instead of PG 18.0 alone, for the
reason ADR 0008's addendum of this date gives (#309 CI-06, PR #348). The PG19-beta cassert
leg is a residual (D28).
