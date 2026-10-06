---
id: 0079
title: The builder enforces the same per-document token ceiling as the pending list, rather than widening tf to uint32
date: 2026-08-20
status: Accepted
summary: ambuild had no per-document token cap while INSERT rejects above PG_UINT16_MAX, so the same row was accepted or rejected depending on index-creation order — durably, since REINDEX takes the builder path; the builder now applies the same ceiling instead of the alternative of widening BM25PendingTermEntry.tf, which would be an on-disk format break for a case no corpus has.
---

# 0079. The builder enforces the same per-document token ceiling as the pending list, rather than widening tf to uint32

## Context

`bm25_pending_append_multi` rejects a document whose total token count exceeds
`PG_UINT16_MAX`, because `BM25PendingTermEntry.tf` is `uint16` on disk. A term
occurring more than 65535 times in one field would wrap the counter to zero and yield
an accepted document carrying `tf = 0` — present in the table, unfindable through its
index.

`ambuild` bypasses the pending list entirely and feeds the accumulator, where
`AccumPosting.tf` is `uint32`. The only ceiling on that path was the tokenizer's
array-growth guard (`MaxAllocSize / sizeof(BM25Token)`), orders of magnitude higher.

So the two entry points disagreed about what is indexable, and the disagreement was
observable as an ordering dependency rather than an abstract inconsistency:

- **Load then index.** `CREATE INDEX` over a table already holding a >65535-token
  document succeeded, producing a sealed segment carrying `tf > 65535` for some term.
- **Index then load.** `INSERT`ing that byte-identical row afterwards raised
  `ERRCODE_PROGRAM_LIMIT_EXCEEDED`.

And it was durable, not transient: `REINDEX` takes the builder path, so an index in
that state rebuilt into the same state rather than surfacing the limit. The `uint32`
`tf` survives to disk with no narrowing anywhere — verified through the varbyte
encoder, the impact table, merge and read-back — so this was a real stored state, not
a value truncated somewhere downstream.

## Decision

Apply the same `PG_UINT16_MAX` per-document ceiling on the `ambuild` path.

Checked **incrementally**, as each field is analyzed and before its tokens reach the
accumulator, so an oversized document costs one field's analysis rather than the whole
row's. Summed across fields, because the limit is a property of the document: two
40000-token columns is 80000 for the row and is refused, even though neither column
reaches the limit alone. A per-field check would have missed exactly that case.

**Not** widening `BM25PendingTermEntry.tf` to `uint32`. That is the honest alternative
if >65k-token documents are considered legitimate input, and it is an on-disk
record-layout change needing the ADR 0009 format-break treatment — for a case no
corpus has. ~65k tokens is far past any natural prose document; a 2000-word article is
about 2000 tokens.

## Alternatives considered

- **Widen `tf` to `uint32` and lift the cap entirely.** The other coherent answer, and
  the one to revisit if a real corpus ever meets the limit. Rejected on cost/benefit:
  a format break plus an upgrade path, to admit documents nobody has.
- **Leave the builder uncapped and let the pending path be the strict one.** This is
  the status quo, and it is the actual bug — not a missing check but two paths giving
  different answers for the same data, with which one you get depending on the order
  you did things in.
- **Cap per field rather than per document.** Cheaper to express and wrong: it admits
  a row whose fields sum past the ceiling, which is precisely the state the `uint16`
  `tf` cannot represent.
- **Warn instead of erroring on the builder path.** Leaves the inconsistency and adds
  noise. If the document cannot be represented on one path it should not be accepted
  on the other.

## Consequences

- `CREATE INDEX` and `REINDEX` now refuse a document the `INSERT` path would refuse.
  **This is a behaviour change for anyone whose corpus contains such a document**: a
  build that previously succeeded now errors, naming the row's token count and
  suggesting splitting the value or indexing a summary column.

  Be precise about what those users had, because it is *not* the pending path's failure
  mode: the builder stored an honest `tf > 65535`, not a wrapped one. Wrapping to zero
  is what `e_tf` (uint16) does on the pending path, and it is why that path has a cap.
  The builder's `tf` is `uint32` end to end, so a pre-fix index scored those documents
  with a correct, oversized `tf`. The defect being fixed is the disagreement between the
  paths, not silent data loss on the builder one.

- **Two upgrade consequences follow, and neither is signalled by a format bump.** A
  segment sealed by a pre-#158 binary keeps its `tf > 65535` indefinitely — there is no
  format-version change here and no read-side `tf` validation, which is fine because
  nothing downstream is bounded by it. But such an index now **fails its next
  `REINDEX`**, and a `pg_restore` of a dump containing such a row now **fails at
  `CREATE INDEX`**. Both are the intended consequence of making the paths agree; both
  will look like a regression to whoever hits them, so they belong in release notes.
- The two error messages differ in wording (the pending one names the pending list,
  the builder one does not) but share the errcode and the limit. Deliberate: the
  reason is the same, the context is not.
- `src/bm25_scan.c` carried two comments justifying the phrase stash's memory charge
  by saying position arrays "scale with term frequency, which reaches 65535". That
  bound held only for pending-path documents before this change; it now holds for
  anything *ingested* since, which is the qualifier both comments now carry — a segment
  sealed by an older binary is still readable and can still exceed it. **The charge itself was never wrong and must not be
  "fixed"** — it is levied per position actually appended, from the real decoded frame
  length, so the accounting tracked reality however large `tf` got. Only the sentence
  explaining it was overstating what bounded it.
- `sql/100_ingest_token_ceiling` pins both orderings, the multi-field sum, and a
  just-under-the-ceiling document indexing successfully on both paths. A/B verified:
  with the cap disabled, both `CREATE INDEX` statements succeed.

## Addendum (2026-10-05, PRs #331-#350)

- **Analysis garbage halved** (#305 TEXT-03(a), PR #339). `bm25_analyze` now adopts each
  kept lexeme instead of copying it and frees the dictionary's result array and every skipped
  lexeme, as core's `parsetext` does, roughly halving what a long document's analysis holds
  live (`sql/148` pins a bytes-per-word slope).
- **Residual (TEXT-03(b), D21): no early stop.** Every caller still analyzes a whole field
  before it can learn the document is over the 65,535-token ceiling, so a document certain to
  be rejected is analyzed in full first. Stopping early needs a caller-supplied limit and an
  overflow report from `bm25_analyze`, with each caller raising its existing error (the
  messages `sql/68`, `sql/77` and `sql/100` pin) and the insert path carrying the count across
  fields. Not done: it costs CPU and statement-scoped memory, no data, and
  `statement_timeout` bounds it.
