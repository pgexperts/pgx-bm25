---
id: 0004
title: PATTERNS
date: 2026-07-11
status: Accepted
summary: Recurring implementation idioms — two-phase orphan-build-then-flip installs, single-record seals, stamp-and-gate page reuse, seg_gen re-validation, and subtransaction-wrapped in-scan retry.
---

# 0004. PATTERNS

- **Two-phase install**: every seal/merge builds all new pages as orphans (many ≤4-buffer
  Generic WAL records, metapage untouched), then ONE final record flips `segcat_root` + writes
  stats + (seal) resets the pending anchor / (merge) appends a retire RANGE. Never link a page
  into a live structure before the final record. (The keymap AND pos chains are further orphan
  chains built the same way — each adds zero buffers to the publish record.)
- **Single-record seal**: publish + advance pending_head + stats in ONE record (BM25 scores
  are additive, not GIN-idempotent — a publish-before-truncate crash window would double-score).
- **Stamp-and-gate page reuse** (nbtree `safexid` / bloom `BLOOM_DELETED` idiom): every free
  path stamps `BM25_PAGE_DELETED` before `RecordFreeIndexPage`; the allocator reuses a page
  only if `PageIsNew` or (`DELETED` and retire_xid invalid-or-horizon-clear).
- **seg_gen validation (option d)**: every segment page carries its generation; readers
  validate it after locking each followed page, so a stale pointer into a reused page aborts
  cleanly and retries instead of reading recycled bytes.
- **In-scan retry via subtransaction — leave PG_TRY only via PG_END_TRY**: the option-(d)
  `seg_gen` abort is a real `ereport`, so `bm25_scan_build_ranking` wraps the scorer in
  `BeginInternalSubTransaction` + `PG_TRY`/`PG_CATCH` and retries the retryable abort with a
  fresh snapshot. CRITICAL: never `return`/`goto`/`break` out of the `PG_TRY` body — only
  `PG_END_TRY` restores `PG_exception_stack`. The success path signals a flag and returns AFTER
  `PG_END_TRY` (a bare `return` from inside `PG_TRY` left a stale exception stack that a later
  target-list projection error longjmp'd into → backend FATAL; fixed + guarded by
  `41_scored_scan_error`). The M2b over-pull tail rebuild also goes THROUGH this wrapper (via a
  `force_exhaustive` flag threaded to the dispatcher) so seg_gen aborts stay non-user-visible.
- **Scan-start analyzer fingerprint gate**: scan re-resolves the query analyzer from
  reloptions, fingerprints it, and compares to the metapage value — ERROR by default, WARNING
  under `require_analyzer_match=false`. Fires only on the genuine bm25 index scan (ranked `&@@`
  path); a plain `@@@` filter answered by another index runs `bm25_match` (default analyzer).
- **Authoritative index match (xs_recheck=false)**: the bm25 index scan does NOT recheck its
  match. The scan tokenizes with the INDEX's analyzer against postings built from the same
  analyzer, so a returned TID genuinely matches; the executor's MVCC visibility check is
  independent of xs_recheck, and VACUUM tombstones a dead TID's posting before its line pointer
  can be reused (ambulkdelete contract + the scan skips tombstoned docs). Rechecking would defer
  to `bm25_match`, which has NO index handle and tokenizes with the default english analyzer —
  on a non-english index that diverges and silently drops correct matches. `bm25_match` is the
  default-english bare-filter/seqscan fallback only, never the index-path recheck.
- **BM25F in one pass, output-layer keymap (M5)**: multi-field scoring sums
  `boost_f · idf(N_f, df_f) · termscore(tf, doclen_f, avgdl_f, k1_f, b_f)` WITHIN each doc's
  single scoring (no second pass, still dedupe-by-TID). Per-field `df` is partitioned from
  `dict.df` via the per-block field-id RLE (`Σ df_field == dict.df`), NOT a second dict. The
  `bm25_idf`/`bm25_termscore` math stays pure (the field only selects which stats the caller
  feeds). The `key_field` docid→key map is an OUTPUT-projection layer: the ranking dynahash
  still keys on TID, so a non-unique key never collides the accumulator.
- **Separate position chain, opt-in read (M4)**: positions live in a SEPARATE per-segment
  `BM25_PAGE_POS` chain (the Lucene `.doc`/`.pos` split), addressed per-term by
  `dict.pos_post_root`/`off` (the structural parallel of `post_root`/`off`). Each posting has a
  frame `[tf-count varbyte][Δpos varbyte × tf]`, decoded in LOCKSTEP with the POST scan and
  self-verified by `tf-count == tf` (hard error on desync). A bag-of-words scan passes
  `pos_cb = NULL` and NEVER faults POS pages. Per-field `store_positions` (default on) rides a
  trailing `uint8[]` flag array on the field-config page (no `BM25FieldConfig` struct growth, so
  M5 pages still parse); an absent flag array (a pre-M4/legacy index) is treated as OFF. The
  pending record carries true per-token positions (`pos_bytes` posblob) so inserted-then-sealed
  docs get correct positions.
- **Phrase = recheck-and-filter, not re-score (M4)**: the phrase/proximity matcher
  (`src/bm25_phrase.c`, pure) runs as a POST-accumulation filter over the ranked TID set —
  positions are stashed per `(TID, field, phrase-term)` from BOTH sealed segments (lockstep
  `pos_cb`) and unsealed pending docs (posblob decode, read-your-writes); non-matching TIDs are
  dropped and survivors keep their accumulated BM25F score. This composes with the single-pass
  scorer and leaves the `bm25_scan_build_ranking` contract untouched, and is EXCLUDED from the
  WAND path (a phrase query takes the exhaustive scorer — WAND's score-pruning would drop a doc
  the post-accumulation filter would have kept). A bare phrase matches OR-across-fields (never
  spanning a field). **D7 degradation**: a phrase touching a position-less segment (`pos_root`
  Invalid) OR any in-scope `store_positions=false` field ERRORs ("REINDEX"), relaxed to WARNING +
  AND-of-terms by the `phrase_fallback='and'` reloption — never a silent under-return.
- **Snippets re-analyze the projected text (M4)**: `bm25_snippet(field, start_tag, end_tag,
  max_num_chars)` re-runs the analyzer on the passed column VALUE (the stored post-stemming
  ordinals cannot map to character spans), marks the query terms, and emits a bounded,
  original-cased, UTF-8-safe excerpt with tags (NULL on no hit). It sources query terms + config
  from the active scan slot like `bm25_score` — so it is index-scan-only (needs the ranked
  `&@@` path). `BM25Token` gained `src_off`/`src_len` (original-text byte span) for this,
  threaded through both analyzer emit sites; inert for every other caller.
- **Index-scan-only query semantics (field:term, phrase, snippet, non-english analyzer, jsonb tree)**:
  any semantics the standalone `@@@`/`bm25_match` operator can't carry (a non-english analyzer, a
  `field:term` scope, a phrase/proximity recheck, a snippet, an M6 jsonb boolean/wildcard tree) is
  honored ONLY when the bm25 index answers the query. The ranked `&@@` form forces the index; a
  bare boolean `@@@` filter the planner may satisfy via `bm25_match`/the jsonb match anchor
  (index-blind) — so the reliable entry for those semantics is the `&@@` ranked scan, and the
  `@@@`/`&@@` operators must be anchored on the index's FIRST column to plan as an Index Scan.
  (A secondary `&@@` sort key now sorts correctly — the rank-collapse fix, TRADEOFFS — but `&@@`
  must be the LEADING order key AND there must be a single active scored scan; a cross-join /
  bitmap / seq path with no active scored slot still degrades to `+inf`.)
- **Deterministic tokenization**: `bm25_analyze` is a pure function of the resolved dict OID +
  token, so build, insert, query, WAL replay, and standbys all tokenize byte-identically.
- **Block-max WAND: raw impacts, scan-time-evaluated safe bound (M2b)**: the per-block bound
  `bm25_block_ub = Σ_{f∈block∩query} boost_f · bm25_termscore(idf_f, max_tf_f, min_doclen_f,
  avgdl_f, k1_f, b_f)` is evaluated at SCAN time from RAW per-field `(max_tf, min_doclen)` — never
  a seal-time scalar. `bm25_termscore` is monotone (↑tf, ↓doclen), and `idf ≥ 0` (the Lucene
  "+1" variant), so `(max_tf, min_doclen)` dominates every posting ⇒ a provably safe upper bound
  that stays safe as tombstones/merges move idf/avgdl. The engine uses the EXACT `bm25_termscore`
  (not an inlined copy) so the bound equals a real score BIT-EXACTLY at the coincidence boundary
  (an inlined copy could round 1 ULP low and prune a top-k doc). BMW runs per segment under one
  shared global top-k heap (θ rises across sequential segments; pivot on the global `UB_t` gives
  safe termination; the block-max deep-check shallow-skip is CAPPED at `min(min_last+1,
  next_cursor_docid)` — an uncapped skip jumps a lagging term's matching docid and drops a top-k
  doc). Pending has no blocks → a non-prunable arm scored first to prime θ.
- **WAND == exhaustive, bit-for-bit (M2b)**: the WAND path returns the identical tids/order AND
  identical score bits as the exhaustive scorer. Achieved by: scoring a candidate doc
  posting-at-a-time in query-term order then field-stream order (matching the exhaustive
  accumulator's IEEE add sequence); a NAMED `contrib = boost*termscore` rounding barrier at BOTH
  score sites; folding fields in ascending `field_id` in both `score_doc` and `bm25_block_ub`;
  the top-k heap draining in the exact `scored_desc` order (score desc, tid asc); and the pinned
  `-ffp-contract=off` so FMA contraction can't defeat the barrier or diverge across compilers.
  The parity is enforced by `43_wand_parity`/`45_m2b_acceptance` (WAND vs exhaustive) AND by the
  anti-neuter `44_wand_skip` (`blocks_skipped`/`deep_check_skips > 0` — a WAND that never prunes
  is still bit-exact, so pruning must be witnessed separately).
- **jsonb query object, not a parsed DSL (M6)**: an M6 query is a **jsonb** structure whose
  string leaves are DATA — `bm25_term('body','AND')` searches for the token `and`, it is never
  parsed as an operator, so there is no injection/round-trip surface by construction. The SQL
  builders emit jsonb whose keys (`field`/`value`/`terms`/`pattern`/`phrase`/`slop`/`ordered`/
  `weight`/`query`/`must`/`should`/`must_not`) are consumed byte-for-byte by `bm25_query_parse`.
  Parse-time validation ERRORs (path-independent, so identical on `@@@` and `&@@`): unknown node/
  field, non-array must/should/must_not, must_not-only boolean (D6), >64 total leaves, recursion
  depth, wildcard with no `*` / prefix shorter than `bm25.wildcard_min_prefix`, `boost.weight ≤ 0`,
  `phrase.slop < 0`. `bm25_match_jsonb` **ERRORs (fail-loud)** if it is ever reached — which is
  ONLY off-index (the planner demoted `@@@` to a filter/seqscan qual); it cannot match a jsonb
  tree from a heap column value. The jsonb tree is parsed/evaluated on the index path in
  `bm25_rescan_parse_jsonb`/`bm25_query_eval` (EXPLAIN-verified no Recheck Cond; `xs_recheck=false`,
  no `amgetbitmap`/`amcanreturn`), so it never calls this fn (it returned `false` before —
  silently 0 rows; see TRADEOFFS + `52_jsonb_filter_index_only`). `bm25_distance_jsonb` returns the active
  scored scan's per-row distance stash (`+inf` only off the index — see the rank-collapse fix in
  TRADEOFFS), so a ranked jsonb `&@@` sorts correctly.
- **Boolean presence bitmask reuses `and_presence`, membership-only (M6, D5/D11)**: the multi-leaf
  scorer generalizes the M1 per-term OR loop into a flat `BM25TermWork[]` (leaves × tokens) so the
  single-level text path stays byte-identical. Each leaf owns a bit; the scorer marks it via the
  EXISTING M4 `and_presence` HTAB / `PhraseAndEnt.mask` / `phrase_and_mark(cur_qi = leaf_bit)` — NO
  new `AccEnt` field. Positive leaves score into `acc` (per-term OR-sum, scoped to the leaf's
  `field_id`, × the leaf's folded boost) AND mark their bit; **must_not (negated) leaves are
  presence-only** (a gated decode — live-docs + pending-dedup + field-scope, NO scoring) so a
  negated match never contributes score; the `negated` flag propagates down `must_not` edges at any
  depth. At drain, `bm25_query_eval(qtree, mask)` (must=AND, should=OR-required-only-when-no-must,
  must_not=AND-NOT) filters membership. **The boolean STRUCTURE filters membership only; the SCORE
  stays the single-pass BM25F OR-sum** — a should-PHRASE/WILDCARD whose own adjacency/expansion
  predicate fails still contributes its constituent terms' score to a doc a sibling clause keeps
  (bag-of-words ranking, exact membership; see TRADEOFFS).
- **`@@@` filter reuses the scorer for D12 consistency (M6)**: the non-scoring `@@@` filter path
  (`bm25_load_if_needed`) does NOT reimplement per-leaf presence — it drives the SAME
  `bm25_scan_build_ranking` the scored `&@@` path uses and emits the surviving TIDs (order
  irrelevant, scores discarded). So the `@@@` filter SET equals the `&@@` ranked SET **by
  construction** for every leaf type (D12); a divergence is structurally impossible. jsonb trees
  take the exhaustive scorer (WAND gate `!bm25_qtree_is_multileaf(so->qtree)`, D10) — a single
  MATCH/TERM leaf is copied to `so->qterm` and is text-identical (WAND-eligible).
- **Wildcard: prefix range + glob over the stemmed dict, single-count dedup (M6, D8)**:
  `bm25_dict_expand_wildcard` splits the pattern at the first `*`, then per live segment walks the
  byte-sorted dict (`bm25_seg_dict_iter_*`), accepts entries that start with the prefix (pure-prefix
  unconditionally, else `bm25_glob_match`), early-stops past the prefix range, and does a linear
  glob pass over pending. All matches across segments + pending dedup into ONE distinct set via a
  byte-keyed dynahash — **mandatory**, else a term living in N segments is scored N times under one
  leaf bit (double-count). The pattern is matched RAW-lowercased (NOT stemmed) against the STEMMED
  dict bytes — so `judg*` matches the stem `judg`. A wildcard leaf consumes ONE leaf_bit regardless
  of expansion count; field scope is applied at SCORING (the dict is not field-partitioned).

## Addendum (2026-08-16)

The "`@@@` filter reuses the scorer for D12 consistency" bullet above claimed the filter
set equals the ranked set "for every leaf type" and that "a divergence is structurally
impossible." That was true for jsonb leaves and false for the TEXT phrase surface, which
does not go through `so->qtree` at all (#132).

`bm25_load_if_needed`'s delegation guard read `so->qtree != NULL &&
bm25_qtree_is_multileaf(so->qtree)`. A text phrase leaves `so->qtree` NULL and sets
`so->qphrase` instead, so `col @@@ '"a b"'` with no `ORDER BY` fell through to the flat
OR-union and returned the union of the phrase's tokens — no error, no recheck, and
`xs_recheck = false` meant nothing downstream could catch it. `bm25_phrase()` (jsonb) was
never affected, because a PHRASE node is a tree and was already delegated.

The guard is now `so->qphrase || (so->qtree != NULL &&
bm25_qtree_is_multileaf(so->qtree))`. The two disjuncts are mutually exclusive by
construction: `so->qphrase` is set only by the text micro-parser (`bm25_rescan` runs it
only when the RHS was not jsonb), `so->qtree` only by `bm25_rescan_parse_jsonb`. The
delegation body needed no change — `so->qterm` already holds the inside-quotes text and
the recheck reads `so->qfield`/`qphrase_ordered`/`qslop` off the same scan opaque.

The pattern generalizes past "leaf type": **the flat OR-union is admissible only for a
predicate decidable from term membership alone.** It requests no positions anywhere (NULL
`pos_root`/`pos_off` into the dict lookup, an Invalid/NULL position cursor into the posting
scan, a pending walker that strides past `pos_bytes` without decoding), so any positional
predicate must be delegated, never rechecked in place — rechecking there would mean a
second position pipeline beside the scorer's, duplicating the pending stash, the off-field
gate and the pending-wins dedup.

Three consequences, all of them `&@@` behaviour the two surfaces should have shared: a bare
phrase on a `store_positions = false` index now raises the same D7 error (and honors
`phrase_fallback = 'and'` the same way) instead of quietly returning a union; rows are
emitted score-descending rather than TID-ascending; and the phrase spends the exhaustive
scorer's match-memory budget rather than the flat-OR per-TID one.

Why the suite never saw it: every phrase assertion pairs `@@@` with `ORDER BY … &@@` — a
deliberate portability convention — and `ORDER BY … &@@` is exactly what routes a query to
the scoring path, the one that always applied the recheck. The un-paired shape was
structurally invisible. `sql/40_m4_acceptance` (bar 6) and `sql/38_phrase` (part 10) now
write it, using `array_agg(id ORDER BY id)` so they do not depend on the emitted row order.

## Addendum (2026-08-23)

The two bullets above are unchanged in substance but say "publish" where the tree now
says "publish ALL of them". Since `docs/adr/0084` the build, merge and drain
accumulators are cut at `maintenance_work_mem`, so ONE operation can produce SEVERAL
segments -- and every one of them is published in the SAME single record. Read
**Two-phase install** as "then ONE final record flips `segcat_root` + writes stats +
(seal) resets the pending anchor / (merge) appends a retire RANGE **for every segment
the operation produced**", and **Single-record seal** as covering the whole entry
array rather than one entry.

That is not a widening of convenience. Publishing chunks in separate records leaves
committed, WAL-durable states in which a document exists in both a new segment and its
still-live source, and because a scan snapshots `pending_head` and the catalog together
and SUMS per-TID with no cross-source dedup, those states double-score and
double-return -- which is the same additive-scores argument the Single-record seal
bullet already rests on, applied one level up. `bm25_segcat_publish_append` and
`bm25_segcat_publish_swap` are the two entry points that enforce it.

## Addendum (2026-10-04)

The fresh-eyes review of this date found two entries here with consequences the record does not state. (1) The off-index `@@@` semantics (bm25_match evaluates only its own LHS column and treats a `field:` prefix as text) make results plan-dependent; filter plans arise under RLS policies and OR shapes, not only seqscans (#298). (2) In-scan retry via an internal subtransaction escalates a hot-standby recovery conflict from statement cancel to FATAL session termination, because PostgreSQL only downgrades the conflict outside a subtransaction (#307). Both are tracked for decision; this record is unchanged until a superseding record lands.

## Addendum (2026-10-05)

The off-index `@@@` entry above is narrowed by #298, decided 2026-10-05.

`bm25_match` now raises `ERRCODE_FEATURE_NOT_SUPPORTED` for a field-scoped RHS, as it
already did for a phrase (#132), instead of tokenizing the scope as text. The test is one
shared predicate, `bm25_query_field_prefix` (after leading spaces, a colon with no space or
quote before it), which `bm25_query_phrase_offset` now uses too. So `'meeting 10:30'` stays
a bare term, and `'http://x'` reads as a scope, as it does on the index path, and therefore
errors off the index. A field name containing a space (a quoted-identifier column) is read
as unscoped text off the index, because the space precedes the colon, but resolves as a
field on the index path. The index path is unchanged, including the unknown-field error, and
the jsonb forms already refused off the index.

What stays plan-dependent, accepted and documented (README, the `@@@` and `bm25_match`
COMMENTs, ARCHITECTURE, THEORY): a bare RHS off the index matches the LHS column only, where
the index matches every field of a multi-column index. `bm25_match` cannot detect that case.
Release note: a role under an RLS policy now gets an ERROR for field-scoped search instead of
wrong rows.

#306 has to change both consumers of the scope rule together. The index path still splits
with `bm25_rescan_parse_field`'s broader rule (the first colon before a quote). Today every
RHS the shared predicate calls a scope is also a scope on the index path, so the off-index
refusal never rejects something the index answers unscoped. If #306 narrows the index path
without moving the off-index rule, URL-shaped input becomes plan-dependent again. Pinned by
`sql/130_offindex_field_scope`. The second point of the 2026-10-04 addendum (#307) is
unchanged.

## Addendum (2026-10-05, PRs #331-#350)

Five entries here are partly replaced, corrected or qualified by work of this date; the rest of each entry
stands.

- **In-scan retry via subtransaction** now describes the primary only. In recovery
  `bm25_scan_build_ranking` runs the build once with no subtransaction and no retry, so a
  recovery conflict is core's ordinary 40001 statement cancel rather than FATAL (ADR 0121,
  PR #331, #307). The PG_TRY discipline the entry describes is unchanged on the primary.
- **The off-index invariant of the 2026-10-05 addendum above** ("the off-index refusal never
  rejects a query the index answers unscoped") is dropped. The index path now uses
  `bm25_query_field_prefix` too, and for a name that is not a field a colon followed by `/`
  or a digit is literal text there, while `bm25_match` still refuses every scope-shaped RHS.
  The invariant is now "off-index refuses or agrees, never silently differs" (ADR 0122, PR
  #336, #306).
- **Snippets re-analyze the projected text** no longer sources its terms from the
  single-leaf slot only: the hit set is built from every non-negated leaf of the query tree
  (ADR 0123, PR #337, #308).
- **Block-max WAND, "`bm25_termscore` is monotone".** True in exact arithmetic only. In
  IEEE-754 evaluation adjacent tf values can invert by a few `DBL_EPSILON` (at k1 = 0 for any
  tf; at ordinary k1 only far beyond real tf), and `wand_widen_ub`'s headroom absorbs it;
  "dominates" reads as "dominates to within that headroom" (the `bm25_wand.c` header was
  corrected in PR #349, #312). The bound is still safe; the WAND-equals-exhaustive entry is
  unaffected.
- **WAND == exhaustive, "pending wins".** The two builders define it differently for a TID
  live both in pending and in a segment: the exhaustive builder is term-major (per term,
  pending then segments), while WAND scores pending for every term first and then skips that
  TID's segment postings. The state is unreachable under the ambulkdelete contract (a TID is
  pending only after its old segment posting was tombstoned), so the results agree on every
  reachable state. Recorded as a comment at both builders (#314 SCORE-03, D20, PR #349), with
  no Assert, deliberately: it would test on-disk state that a corrupt index can violate.
