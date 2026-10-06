---
id: 0033
title: Tokenizer scratch lives in a context the caller resets, not the statement's
date: 2026-07-29
status: Accepted
summary: bm25_build_callback and bm25_insert allocated the lowercased copy, the token array and every lexeme copy in a statement-lifetime context that is never reset per row; both now run that work in a scratch context reset per tuple / deleted per row, matching gininsert. The maintenance_work_mem bound on the accumulator is explicitly not addressed.
---

# 0033. Tokenizer scratch lives in a context the caller resets, not the statement's

## Context

`bm25_analyze` documents that its output "point[s] into palloc'd storage in
CurrentMemoryContext". Neither of its two indexing callers gave it a context worth
pointing into (review ref H12, issue #51).

**ambuild.** `bm25_build_callback` allocated into whatever `CurrentMemoryContext` was
during `table_index_build_scan`, which is the `CREATE INDEX` statement's context.
`heapam_index_build_range_scan` resets `econtext->ecxt_per_tuple_memory` but never
switches into it before invoking the callback — which is precisely why
`ginBuildCallback` keeps and resets its own `buildstate->funcCtx`. So every row's
lowercased copy (one byte per input byte), token array (24 bytes per `BM25Token`, one
per ~2 input bytes) and per-lexeme copies stayed live for the entire heap scan: roughly
13x the scanned text bytes, on top of the accumulator's own footprint, with nothing to
reclaim it until the statement ended.

**aminsert.** `bm25_insert` runs with `CurrentMemoryContext == estate->es_query_cxt`,
and `ExecInsertIndexTuples` does not switch to a per-tuple context around
`index_insert`. It leaked the same tokenizer scratch plus a fresh
`untransformRelOptions` List per row (built by `bm25_resolve_fields`), freeing only the
two small `toks_by_field` / `ntok_by_field` arrays. A single multi-row
`INSERT ... SELECT` therefore accumulated every row's scratch for the whole statement.

## Decision

**Give each path a scratch context and match the GIN precedent.**

`BM25BuildState` gains a `tupcxt`, created before `table_index_build_scan` and deleted
after. The callback switches into it on entry and does
`MemoryContextSwitchTo(old); MemoryContextReset(bs->tupcxt);` on exit — reset, not
delete, so the arena's first block is reused across rows.

`bm25_insert` creates a scratch context, switches in, and deletes it at both exits (the
all-NULL early return and the normal path) before the opportunistic seal block. The seal
stays outside deliberately: `bm25_pending_drain`'s accumulator must not be parented on a
context about to be freed.

**Resetting under the accumulator is safe, and that is a load-bearing property.** The
callback resets scratch while the accumulator still holds every term it was handed.
That works only because `accum_find_or_add_term` does `palloc` + `memcpy` of the term
bytes into `a->cxt`, positions are copied by value, and `bm25_accum_add_doc_multi` /
`bm25_accum_add_field_tokens` switch to `a->cxt` themselves. `BM25AnalyzerConfig` is a
flat POD struct, so `cfg` surviving the context delete by value is fine too.

**No `PG_TRY`.** The report suggested `PG_TRY`/`PG_FINALLY` for the insert path.
`gininsert` does not use one, for the reason that applies here: the scratch context is a
child of the query context, so an error discards it with its parent. Adding one would
also force restructuring the all-NULL early return, since a `return` may not cross a
`PG_TRY` block.

**The memory bound is out of scope.** `BM25Accum` still holds the whole inverted index
in RAM for a build, `maintenance_work_mem` is still consulted nowhere, and
`amusemaintenanceworkmem` is still `false`. That is the other half of the finding and a
much larger change — it needs the accumulator to track its byte usage and seal an
intermediate segment when it crosses the budget. This decision fixes only the scratch
leak layered on top of it.

## Alternatives considered

- **Switch into `econtext->ecxt_per_tuple_memory` in the build callback rather than
  owning a context.** Shorter, and the reset would come for free from
  `heapam_index_build_range_scan`. Rejected: the callback signature does not carry the
  `ExprContext`, the reset cadence would then be the table AM's business rather than
  ours, and GIN's choice to own `funcCtx` is the load-bearing precedent for exactly this
  callback contract.
- **`pfree` the individual allocations instead of using a context.** `bm25_analyze`
  returns a token array whose entries point at separately-palloc'd lexeme copies, so this
  means walking `ntok` tokens per field per row to free them, and it silently breaks the
  next time the tokenizer allocates something new. An arena reset cannot go stale.
- **One scratch context created once per `bm25_insert` caller and threaded down.** Would
  avoid the per-row create/delete, but aminsert has no per-statement state to hang it on
  (`indexInfo->ii_Context` is the wrong lifetime), and the create/delete of a
  never-more-than-one-block arena is not measurable next to `ts_lexize`.

## Consequences

- Build and insert memory is now bounded by the largest single document rather than by
  the statement's total text volume.
- No output changes anywhere, which is why this carries a CI floor rather than a
  differential test: `sql/73_tokenizer_scratch` cannot separate pre- from post-fix on
  output, so the "Tokenizer scratch-context floor" step in `ci.yml` is what fails if
  either context is dropped in a refactor.
- What `sql/73_tokenizer_scratch` *does* pin is the hazard this fix introduces: 200 rows
  x 40 distinct stems through the build path, the same corpus through the insert path,
  the two compared term for term, plus a mid-`INSERT` opportunistic seal and a
  `key_field` index. If the accumulator ever stops copying terms out of scratch, the
  symptom is wrong search results rather than a crash, and those assertions are what
  catch it.
- A `CREATE INDEX` on a table whose text volume exceeds RAM can still OOM, via the
  accumulator. That is unchanged and untracked by any GUC — see the out-of-scope note
  above.

## Addendum (2026-08-23)

The deferral recorded above has been discharged. `bm25_maintenance_budget_bytes` /
`bm25_accum_over_budget` bound the accumulator, all three feeders (build, merge,
pending drain) seal a chunk and restart when they cross it, and
`amusemaintenanceworkmem` is now `true`. The statement "`maintenance_work_mem` is
still consulted nowhere, and `amusemaintenanceworkmem` is still `false`" describes
the tree as of this record's date only. See `docs/adr/0084`, which also explains why
the publish stayed a single WAL record per operation rather than one per chunk.
