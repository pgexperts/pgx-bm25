---
id: 0076
title: The accumulator keys long terms on a hash of the full term, not a truncated prefix
date: 2026-08-20
status: Superseded
superseded_by: 0125
summary: Keying the term hash on the first 255 bytes made collisions selectable by whoever supplies the document text — N terms sharing a prefix collapsed onto one entry and drove an O(N^2) uncancellable linear scan; long terms now key on a hash of their full bytes, and the fallback that remains carries an interrupt check (immediate on build and merge; deferred to the page boundary on the drain, which holds a content lock).
---

# 0076. The accumulator keys long terms on a hash of the full term, not a truncated prefix

## Context

`bm25_accum`'s term lookup is a dynahash over a fixed 256-byte key
(`ACCUM_KEY_MAX`). `accum_make_key` built that key as a one-byte length prefix
followed by the first 255 bytes of the term.

For any term longer than 255 bytes that key is a **prefix**, which makes collisions
something the person supplying the text can choose. N distinct terms sharing a
255-byte prefix all hash to one entry; each lookup past the first falls through to
the collision fallback, which is a linear `memcmp` over the entire term list. Indexing
N such terms is therefore O(N²) — and `grep -c CHECK_FOR_INTERRUPTS src/bm25_accum.c`
returned **0**, so it was also unkillable. A single `CREATE INDEX` or `INSERT` could
pin a backend with no way to cancel it short of a restart.

The later `BM25_MAX_TERM_BYTES` cap (2047) does not help, and it is worth being precise
about why: it is eight times the key width, so the entire collision class sits inside
what a legal document may contain. The cap bounds each `memcmp`; it does nothing to the
number of them.

There is in-tree corroboration that this was known and routed around rather than fixed.
`src/bm25_handler.c` records that the `bm25_match` fix deliberately avoided a hash
"because it would need the truncate-plus-linear-collision-fallback that bm25_accum
carries."

## Decision

Split the key by length, and never truncate.

- A term of **at most 254 bytes** is stored verbatim behind its length prefix. The key
  is exact, two distinct short terms can never share one, and this is the path
  essentially every real term takes. Unchanged from before.
- A term of **255 bytes or more** keys on `hash_bytes` over its **full** bytes, tagged
  with a `0xFF` first byte that no verbatim key can carry, so the two key spaces are
  disjoint by construction.

254 and not 255, and the off-by-one is load-bearing rather than conservative: the tag
byte is `0xFF` = 255, so admitting a 255-byte verbatim term would write `buf[0] = 255`
and alias the tag exactly. A later "cleanup" widening that branch to `<=` would silently
merge a 255-byte term with a hashed one. An earlier draft of this record, the file
header, and the suite's boundary fixture all said 255/256 — which would have left the
verbatim side of the real boundary untested. Adversarial review caught it; the fixture
now covers 253/254/255/256.

Two long terms now collide only when their 32-bit hashes collide, which a chosen prefix
cannot arrange. The linear fallback stays — a hash collision is still possible — and
gains a `CHECK_FOR_INTERRUPTS`, with the caveat recorded under Consequences that it is
deferred on one of the three feeder paths.

`hash_bytes` is the function the pending-list dedup HTAB (`bm25_pendkey_hash`) and the
segment dedup HTAB already key on, so this is the file adopting a convention the rest of
the tree already follows rather than inventing one.

## Alternatives considered

- **Add only the interrupt check.** Makes the quadratic loop cancellable and leaves it
  quadratic. That treats a denial-of-service as an ergonomics problem: the backend still
  burns O(N²) until someone notices and cancels it.
- **Switch to a pointer+length key with custom hash/match/keycopy**, the shape
  `bm25_pending.c`'s dedup HTAB uses. Strictly better — no collisions at all, not merely
  unselectable ones — and the right destination. Rejected for this fix on risk: it
  changes key storage and lifetime in the accumulator, which is on the `CREATE INDEX`,
  merge and drain hot paths, for a finding whose remaining severity after the hash change
  is a hash collision nobody can aim.
- **Raise `ACCUM_KEY_MAX` above `BM25_MAX_TERM_BYTES`** so every term fits verbatim. That
  is a 2 KB key per hash entry, and it ties two constants together such that raising the
  term cap silently multiplies accumulator memory.
- **Cap term length below the key width.** Changes what is indexable to work around an
  implementation detail of the hash.

## Consequences

- Collisions are no longer selectable from document text. The fallback is reachable only
  by a genuine 32-bit hash collision, and is cancellable when it is.
- `sql/98_unbounded_input_loops` guards the new key's correctness — 300 terms sharing a
  300-byte prefix stay distinct and individually findable, a same-prefix term never
  indexed does not match, and all four lengths around the verbatim/hashed boundary
  round-trip.
- **Every assertion in it passes byte-identically against the pre-fix code**, verified by
  A/B build, and no SQL assertion could do otherwise. The pre-fix defect was never
  corruption: both the hash-hit path and the linear fallback full-`memcmp` the whole
  term, so colliding terms were always resolved to the right entry. It was
  correct-but-quadratic, and unkillable. The suite is a regression guard on the thing the
  FIX could plausibly have broken, not evidence the fix was needed, and it says so.
- The cancellability claim is **two-thirds true**, which the code now records: on
  `ambuild` (no buffer lock) and merge replay (page copied and unlocked before the
  callback fires) the interrupt check is immediate; on the pending drain it is not,
  because `bm25_pending_drain` holds the page's content lock across `drain_doc_flush`
  and `ProcessInterrupts` defers under an LWLock (ADR 0041). Cancellation there waits
  for the page boundary.
- `bm25_debug_query_parse` renders a tree without flattening, so it had **no** leaf cap
  before and now shares the parse-time one. That is the single place the "same queries
  legal, same rejected" claim does not hold, and it is asserted in the suite rather than
  left to be discovered.
- The parse-time cap reuses flatten's SQLSTATE as well as its message. The first draft
  used `ERRCODE_PROGRAM_LIMIT_EXCEEDED`, which would have changed `22023` to `54000` for
  the same condition — the same message with a different code is the worst of both, and
  no suite pinned the errcode.
- The file header described the key as having "a full-term spill check for longer
  terms", which was not what the code did. Corrected.

## Addendum (2026-09-27)

The query-parser half of this same PR (the parse-time leaf cap, noted above only in the
Consequences bullets about `bm25_debug_query_parse` gaining a cap and about the
parse-time check reusing flatten's SQLSTATE) has its own dedicated record now: ADR 0099
(`docs/adr/0099-leaf-cap-enforced-at-parse-time.md`), written because issue #227 found
that decision undocumented under any title that names it -- this record's title and
Context are about the accumulator's hash key, not the query parser, so those two bullets
were not discoverable by anyone looking for the leaf-cap decision.

While writing ADR 0099, the Consequences bullet claiming "Every assertion in it passes
byte-identically against the pre-fix code, ... and no SQL assertion could do otherwise"
was checked against the whole of `sql/98_unbounded_input_loops.sql` as it reads today,
and does not hold for the file as a whole. It holds for the accumulator-key assertions
that bullet is describing (the 253/254/255/256-byte boundary and the
300-shared-prefix-term checks earlier in the file), which is what "it" meant in context.
It does not hold for the `bm25_debug_query_parse` assertions the same PR added later in
that file (the pair at the end of its "jsonb leaf cap timing" block): against the
pre-fix build (`6513e8c^`, which has no parse-time leaf counter), the 65-leaf case does
not error at all -- it renders the tree as text, same as the 64-leaf case -- so it
produces a different result than the `ERROR: bm25: query has more than 64 leaf clauses
(must + should + must_not)` pinned in `expected/98_unbounded_input_loops.out`. That is
exactly the effect the `bm25_debug_query_parse` bullet above already describes; this
addendum flags the byte-identical bullet's wording as too broad, not the underlying
finding.

The SQLSTATE bullet above is right that a tree invalid on leaf count alone keeps 22023.
ADR 0099 additionally records that a tree invalid for more than one reason can now
report the leaf cap where it used to report a different error, in some cases under a
different SQLSTATE (42703, 54000, or 22003 before; 22023 now).

## Addendum (2026-10-04)

The fresh-eyes review of this date found colliding long-term pairs in about one second of SQL, so the collision fallback can be aimed: a crafted document set turned index construction into O(occurrences x terms) work (289 ms -> 1,313 ms CREATE INDEX on a small corpus), and on the seal path the same work runs while holding the seal singleton, stalling INSERTs. The record's statement that the drain holds the page lock across the flush is also stale (it copies and unlocks first). Tracked in #305 and #312.

## Addendum (2026-10-05, PRs #331-#350)

Superseded by ADR 0125 (D19, PR #338, #305). This record's premise, that nobody can aim a
32-bit hash collision, is false: a birthday search finds colliding long-term pairs in about a
second of SQL, which made the linear fallback reachable on demand. The map now keys on
`(pointer, length)` over the whole term, the "right destination" the Alternatives above named
and deferred, and the fallback is deleted.

Two statements above were already stale and are not carried forward: the Consequences say
`bm25_pending_drain` holds the page's content lock across `drain_doc_flush`, but the drain
copies the page and unlocks it before the callback (copy-then-unlock, ADR 0083), so the
interrupt check was immediate there too (the `bm25_accum.c` comment was corrected in PR #349);
and the 254/255 verbatim/hashed boundary no longer exists.
