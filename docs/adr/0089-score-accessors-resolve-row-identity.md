---
id: 0089
title: Score accessors resolve a row's identity, not just the shape the ranking stored it in
date: 2026-08-24
status: Accepted
summary: bm25_score_key backfills a pending row's key from the pending record and bm25_score maps a projected ctid to its HOT-chain root, so both accessors answer for a row whatever storage shape it is currently in, with an out-of-band presence flag distinguishing an unresolved key from the key zero.
---

# 0089. Score accessors resolve a row's identity, not just the shape the ranking stored it in

## Context

`bm25_score(ctid)` and `bm25_score_key(key)` look up a projected row in the
active scan's ranking. Both did so by exact match against whatever the ranking
happened to hold, which quietly assumed that the identifier the executor
projects is byte-identical to the one the scan stored. Two ordinary situations
break that assumption, and each produced a silent SQL NULL where a number
belongs (#100, #204):

- **A pending row has no segment.** A ranked row's user key was resolved from
  the winning posting's segment via `bm25_seg_key`. The pending scorer has no
  segment to name, so it passes `InvalidBlockNumber` and the key slot stayed
  empty. A row is pending exactly between its `INSERT` and the next seal, so
  the rows without a score were reliably the newest ones — an application that
  inserted and then searched got NULL for precisely the content it had just
  added. Worse, an index created on an empty table and then loaded has *no*
  sealed segment carrying a KEYMAP, so key-config discovery reported "keyless"
  and every row of such an index projected a NULL key.

- **A HOT update moves the tuple but not the index entry.** That is the point
  of HOT: the new tuple is heap-only, `aminsert` is not called, and the index
  keeps pointing at the chain's ROOT. So the scan ranks the root TID while the
  executor projects the ctid of the tuple it actually fetched, the descendant.
  Sealing does not repair this, because sealing does not change which TID the
  index holds.

The two compose: a row that was both pending and HOT-updated appeared in the
result set, correctly ordered, with NULL from both accessors — no working
accessor at all. Neither failure is visible from the ranking, which is why
`@@@` matching and `&@@` ordering were correct throughout, and why the
regression suites missed it (`54_score_key_keytype` inserts only before
`CREATE INDEX`; `16_pending_ryw` scores by ctid and updates an *indexed*
column, which is by definition not a HOT update).

Doing nothing was not an option: `docs/grammar-mapping.md` recommends
`bm25_score_key(id)` as the `paradedb.score(id)` analogue with no caveat, and
callers cannot distinguish "no score yet" from "score is zero".

## Decision

**A score accessor resolves the row's identity, not merely the encoding the
ranking captured.** Concretely:

1. A ranked row whose key no segment can supply is backfilled from the
   **pending record**, which already carries the key (`bm25_pending_append`
   writes it so the next seal can put it in a KEYMAP). This is a read of the
   value the seal will later write, not a recomputation, so the two cannot
   disagree. Key-config discovery falls back to the pending list's own
   metadata when no sealed segment carries a KEYMAP, validating the on-page
   tag rather than trusting it.

2. A probed TID that matches nothing is retried against its **HOT-chain root**,
   obtained with `heap_get_root_tuples` — the idiom
   `heapam_index_validate_scan` uses.

3. Whether a ranked row's key is known is tracked **out of band**, in a
   `ranked_key_present` array parallel to `ranked_keys`.

Both ranking builders — exhaustive and WAND — do (1) identically. This is
load-bearing, not tidy: WAND's over-pull tail rebuild replaces a WAND ranking
with an exhaustive one mid-scan, so a fix in only one would be discarded the
moment an executor pulls past `wand_top_k`.

Point (3) exists because the obvious encoding is unsound. An unresolved key
slot is left at its `palloc0` fill, and int4/int8 keys are stored raw, so a
genuine `id = 0` encodes to exactly the zero pattern. Before the flag the two
were indistinguishable and `bm25_score_key(0)` — what `WHERE id = 0` encodes
to, not an exotic probe — resolved to whichever unresolved row was in the
ranking. **An absent value cannot be signalled by a value drawn from the same
domain as the real data.**

This deliberately does *not* change SQL NULL `key_field`, which is stored as a
real zero key with no null flag and projects as `0` by design; that ambiguity
is a format property, recorded where it is implemented.

## Alternatives considered

- **Thread the key through the scorer** instead of a second pass. Rejected:
  it widens the accumulator entry, `BM25ExhScored`, and the WAND top-k heap
  element — the hottest path in the scan, including WAND's dedupe — to serve
  a projection most queries never ask for. The separate pass walks the pending
  chain once per ranking and only when a slot is actually missing, so a
  fully-sealed index pays nothing.
- **Re-read the pending head in the backfill.** Rejected in review: every
  other pending read on the ranked path walks the head captured under the
  metapage lock, and the recycle horizon exists so it stays walkable. Re-reading
  would let a concurrent seal move the head between the snapshot and the
  backfill, reproducing the very symptom being fixed as a race.
- **Map the HOT root inside the existing exact-compare loop.** Rejected: that
  loop runs for every projected row of every registered scan, so mapping there
  puts a heap page read behind each of its ordinary misses. The mapping is
  reached only after the cheap answer has failed everywhere.
- **Document the NULLs as the contract** rather than fixing them, updating
  `docs/grammar-mapping.md` with a caveat. Rejected: the ordering and matching
  paths already read pending rows correctly, so the accessor was the outlier,
  and the shape it failed on (newest content) is the shape most likely queried.

## Consequences

- `bm25_score_key` and `bm25_score` now answer for any row the active scan
  produced, sealed or pending, HOT-updated or not. Sealing is invisible to the
  score — asserted directly, by comparing the same rows across a `bm25_seal()`
  rather than by pinning float literals.
- Reading a heap page from a SQL-callable accessor is new, and the accessor
  takes an arbitrary user tid. The block is bounds-checked against
  `RelationGetNumberOfBlocks` **before** any `ReadBuffer`, because a read on an
  out-of-range block would extend the relation — turning a read-only accessor
  into a writer. The mapping is confined to the probed scan's own heap, so a
  concurrent scan on a different heap cannot resolve this one's row.
- An invalid `ItemPointer` is rejected at the resolver's single entry point.
  `(0,0)` is a legal value of type `tid` but not a valid ItemPointer, and the
  tid accessors assert validity, so probing with it aborted the backend on an
  `--enable-cassert` build — SQL-triggerable by any user, against a gating
  cassert CI leg. This was reachable before any of this work, through
  `ItemPointerEquals` on a positioned scan; the guard covers that too.
- The scan opaque carries the heap relation, set at emit. The AM exposes no
  `amgetbitmap`, so every scored scan is a plain index scan and
  `IndexScanDesc.heapRelation` is always available there.
- A pre-existing aliasing property is widened slightly: `bm25_score()` takes a
  bare tid, which carries no relation identity, so a tid from another table
  that collides with a ranked one already returned a score. HOT descendants
  now join that collision set. Unfixable within the API's signature.
- Covered by `sql/109_score_key_pending`, which exercises every case under
  **both** scorers because the key projection exists in two places.
