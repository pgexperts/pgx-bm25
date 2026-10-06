---
id: 0005
title: TRADEOFFS
date: 2026-07-11
status: Accepted
summary: Design tradeoffs forced by the hard constraints — Generic-WAL-only with no custom rmgr, the 4-buffer cap driving orphan-build/pointer-flip swaps, pd_lower discipline, approximate doclen accounting on delete, and the v3->v4->v5 format-break history.
---

# 0005. TRADEOFFS

- **Generic WAL only, no custom rmgr**: a custom rmgr would need `RegisterCustomRmgr` (preload)
  and a recovery-conflict hook `generic_redo` lacks. Rules out the nbtree `xl_btree_reuse_page`
  mechanism and forces option (d) (seg_gen re-validation). Preload-free + no bg workers are hard
  product constraints.
- **4-buffer cap** (`MAX_GENERIC_XLOG_PAGES`): the reason for orphan-build + pointer-flip
  catalog swap, RANGE-not-per-page retire, and two-phase install. Per-page work (stamping,
  unlinking) is deferred to reclaim, never done in the linearizing swap record.
- **pd_lower discipline**: Generic WAL silently drops bytes in the page hole, so every flat-page
  mutation must advance `pd_lower` past the written region — a silent-data-loss footgun. (The
  keymap/RLE/ndocs_by_field/pos-frame/impact-table regions all memset-before-fill for replica
  byte-identity.)
- **Approximate Σdoclen on delete**: `bm25_livedocs_clear` decrements total_len by the
  segment-average doclen; exact only after merge. Extends to per-field avgdl (exact only
  post-seal/merge, approximate under tombstones — same as corpus avgdl).
- **Format breaks (REINDEX) — v3→v4→v5, now complete**: v4 baked the analyzer + reserved the
  M4/M5/M2b surface; M5 and M4 filled the field/keymap and position surfaces with NO break; **M2b
  breaks v4→v5 (REINDEX required)** to fill the last slot (`max_impact` → per-block impact table),
  because that slot could not be filled in place (a safe scan-time WAND bound needs per-field raw
  ingredients, not the reserved `float4`). After v5 the format has NO reserved-but-unused field —
  the reserve-then-fill arc is done. **M6 adds NO format break** — it is a query-language layer on
  v5. A v4 index is rejected with a REINDEX ereport at scan; a pre-M4 (bag-of-words) index still
  needs no REINDEX for scoring but has no positions, so phrase/proximity fail loud (D7) until REINDEX.
- **`store_positions` absent ⇒ off (M4)**: a pre-M4 field-config page has no per-field flag
  array, so it is read as positions-OFF — an upgraded (no-REINDEX) index is uniformly
  position-less (phrase fails loud) rather than risking a mixed-state merge that would write POS
  frames for position-less source postings (an `npos=0` vs `tf` desync). Positions are populated
  only for indexes built/reindexed under M4.
- **Pending position footprint (M4)**: the pending record now carries per-token positions
  (`pos_bytes` posblob), so a very large single INSERTed document's pending footprint grows —
  bounded by the pre-existing "one doc must fit one pending page" limit.
- **`key_field` requires an INCLUDE column**: a non-text key (int/uuid) has no bm25 opclass, so
  it can't be an indexed attribute; `INCLUDE (id) WITH (key_field='id')` is the idiomatic way to
  carry its value to the build callback. text keys truncate to 16 B; a NULL key stores the zero
  sentinel and projects as key 0 (there is no null flag, so a NULL key and a genuine 0 are
  indistinguishable — "unspecified identity", like a non-unique key). ctid fallback applies to a
  keyless index (keymap_root Invalid), not to a sealed NULL-key row.
- **`BM25RetiredEntry` grows per reserved-fill**: it gained `keymap_root` (M5) and `pos_root`
  (M4) so a merged-away segment's keymap and position chains are reclaimed (else they leak) —
  the transient retired descriptor is the one on-disk struct each fill grows. (M2b adds no new
  retired-chain field — the impact table rides inside each posting block, reclaimed with it.
  M6 adds no on-disk struct.)
- **Exhaustive OR-sum scan is now the fallback/reference (M2b)**: block-max WAND is the default
  ranked path behind the same `bm25_scan_build_ranking` contract; the exhaustive OR-sum stays as
  the bit-exact reference and is taken for phrase/AND/`@@@`, `wand_top_k=0`, the over-pull tail,
  and every M6 jsonb tree. Chosen EXACT-not-approximate: WAND returns identical results, so it
  defaults on with zero behavior change.
- **Over-pull tail takes a SECOND snapshot (M2b, known limitation)**: WAND builds the top-`k`
  under one snapshot; if the consumer pulls PAST `wand_top_k` (an un-`LIMIT`ed or `LIMIT >
  wand_top_k` ranked scan), the tail rebuild runs the exhaustive scorer under a FRESH snapshot
  (through the retry wrapper, so seg_gen aborts stay non-user-visible). A concurrent commit
  landing in that window can make the tail's stats/membership differ from the top-k's, so scores
  can drift across the k-boundary. Narrow (never on the default `LIMIT ≤ 100` path),
  non-corrupting, and consistent with the AM's live-committed-stats model. Candidate fix: cache
  the top-k build's snapshot on the scan and reuse it for the tail rebuild.
- **Scored-scan target-list errors (was a FATAL; fixed, PR #10)**: a runtime error in the
  target list of a `&@@` scored scan used to FATAL the backend, because `bm25_scan_build_ranking`
  `return`ed from inside its `PG_TRY` (skipping `PG_END_TRY`, leaving a stale `PG_exception_stack`
  that the later projection error longjmp'd into). Pre-existing since M2a; fixed by returning only
  after `PG_END_TRY`, guarded by the mutation-verified `41_scored_scan_error`. The in-scan
  subtransaction-retry mechanism is otherwise sound.
- **Boolean scoring is bag-of-words (M6, D9/D11)**: the boolean tree filters MEMBERSHIP exactly,
  but SCORE is the single-pass BM25F OR-sum of every leaf's constituent terms — so a should-PHRASE
  or should-WILDCARD whose own adjacency/expansion predicate FAILS still contributes its terms'
  score to a document that a sibling clause keeps. Only the RANKING of already-kept docs is
  approximate; the result SET is exact. This is inherited by-design from the M4/D9 filter-only
  phrase-scoring contract (single scoring pass, phrase is a post-filter), not a regression. D11's
  "Σ over matched leaf contributions" wording is loose against this architecture and is documented
  as such rather than implemented as a second scoring pass.
- **must_not PHRASE leaves are rejected (M6, clean ERROR not silent-wrong)**: a phrase leaf reached
  through a `must_not` edge raises "a phrase leaf inside must_not is not yet supported in a jsonb
  query tree" (any depth, both `@@@` and `&@@`). The negated presence-only decode appends into a
  non-idempotent position stash and cannot reuse the pending-wins dedup the positive phrase gather
  relies on (negated leaves skip `pending_score_term`, so their pending TIDs never enter
  `pending_tids`); wiring it correctly needs its own dedup path. Deferred rather than shipped
  silent-wrong. must_not TERM/MATCH/WILDCARD leaves work.
- **`ORDER BY x &@@ q, <secondary key>` — rank-collapse FIXED (post-M6, both operators)**: a
  pre-existing whole-index bug (since M1) collapsed BM25 rank onto the secondary key. **Corrected
  root cause:** `&@@` is projected PER ROW as a resjunk column on the Index Scan node's OWN target
  list (a secondary sort key, or a direct `SELECT` of the column, needs the value), on EVERY ranked
  query — NOT gated by `xs_recheckorderby` (that flag only gates PG's internal reorder-queue in
  `nodeIndexscan.c`). The functions returned a constant `+inf`, so a secondary-key Incremental Sort
  saw every row tie on `+inf` and sorted by the secondary key alone. (This is why the
  originally-planned FAIL-LOUD fix — ERROR when `bm25_active_scored_scan != NULL` — was INFEASIBLE:
  the fn is invoked on every ranked query's projection, so erroring breaks them all.) **Fix
  (score-stash):** `bm25_gettuple` stashes each returned tuple's `-score` on the scan opaque
  (`so->cur_orderby_dist`, set alongside `xs_orderbyvals[0]`), and `bm25_distance`/
  `bm25_distance_jsonb` return that stash instead of `+inf` while a scored scan is active. The
  per-row projection immediately follows the `bm25_gettuple` that produced the tuple — a strict 1:1
  `gettuple → project` pairing, so the stash is never stale (verified across multiseg+pending / WAND
  top-k / over-pull tail / rescan; mark/restore is unreachable since the AM leaves
  `ammarkpos`/`amrestrpos` NULL → the planner interposes a buffering Sort over already-projected
  tuples). So `ORDER BY x &@@ q, <tiebreak>` now sorts correctly and `x &@@ q` is a usable
  score-distance projection (`= -bm25_score(ctid)`). Gating suite `50_orderby_dist`. **PG16 dropped
  (min PG17):** this fix works on PG 17/18; PostgreSQL 16 was dropped because its planner does NOT
  build the incremental-sort-over-`amcanorderbyop` path this relies on (16 full-sorts an unordered
  scan → no active scored scan → `&@@`=`+inf` → the secondary-key form still collapses). Enforced by
  the `src/bm25.h` `#if PG_VERSION_NUM < 170000 #error` floor. **Two residual caveats (pre-existing
  single-global-slot limits, ordering UNCHANGED by the fix):** (1) `&@@` must be the LEADING
  order-by key — a non-leading `ORDER BY other, x &@@ q` runs an unordered scan (no active scored
  scan) so `&@@` is `+inf` and still collapses; (2) correct only for the ONE active scored scan —
  `bm25_active_scored_scan` is a single global slot (shared with `bm25_score`/`bm25_snippet`), so
  two concurrently-active bm25 scored scans (a correlated/joined query ranking on both) make a
  non-driving scan's `&@@` project the driving scan's distance (a wrong FINITE value — the same
  nested-scored-scan limit `bm25_score` already has, now a plausible-wrong finite instead of the
  old obvious `+inf`).
- **Wildcard expansion is a linear dict skip (M6, bounded ceiling)**: `bm25_seg_dict_iter_begin`
  has no seek, so the prefix range scan linearly skips every dict entry `< prefix` per segment
  (O(dict)); the `bm25.wildcard_max_expansions` cap is checked after the union is materialized.
  Both are bounded by the GUC guardrails (min-prefix ≥ 3, max-expansions ≤ 1000 by default) and
  fine at M6 scale; the upgrade path (a seek-to-`≥ prefix` iterator + a running-size check during
  accumulation) is a later optimization, not a blocker.
- **BriefBank §5 conformance is proven — test-only, no code/format change**: `sql/51_briefbank_conformance.sql`
  maps every documented BriefBank grammar form (default-fields fan-out, per-field boost, field scope
  `ti/su/te`, boolean AND-NOT/OR/nested, phrase-vs-bag, proximity `PRE/n`/`W/n`/`W/S`, wildcard
  truncation, injection-safety, relevance score positive/strictly-descending/no-Sort top-N, query-time
  boost, snippet) to a native call on the "§1a" multi-field/boost/`key_field`/positions index, each with
  a *discriminating* assertion on a 28-row synthetic corpus; the grammar-form → native-call contract is
  `docs/briefbank-grammar-mapping.md`, and `t/008_briefbank_conformance.pl` proves a ranked fan-out +
  phrase + snippet query is replica-byte-identical. The suite is self-contained (it runs the SQL
  BriefBank's compiler *would* emit; the Django compiler-backend swap stays a BriefBank-side task). It
  surfaced two real native-side limits, worked around by using the ranked/single-leaf forms throughout:
  **(F1)** a bare `@@@` jsonb filter could silently return 0 rows when the planner answered `col @@@ jsonb`
  OFF the bm25 index — **now FIXED (fail-loud; see the dedicated bullet below + `52_jsonb_filter_index_only`)**.
  The earlier "priming-dependent / cold-scan" framing was a MISDIAGNOSIS: the real trigger is the planner
  demoting `@@@` to a filter qual (a competing `ORDER BY`, a cheaper index, a cost/stats flip) → the inert
  `bm25_match_jsonb`, which now ERRORs instead of returning false. **(F2)** `bm25_snippet`
  highlights only single-leaf/text scans — under a multi-leaf `bm25_boolean(...)`/fan-out scan it returns
  NULL for every row (it reads the single-leaf `so->qterm` slot, which a boolean scan never sets); to
  highlight a boolean result, re-issue the snippet under a single-leaf `bm25_term` scan for the term.
- **jsonb `@@@` off-index FAILS LOUD (post-conformance fix, `52_jsonb_filter_index_only`)**: `bm25_match_jsonb`
  (the `(text,jsonb)` `@@@` operator's procedure) used to `PG_RETURN_BOOL(false)`, so any plan that evaluated
  `col @@@ jsonb` as a filter / recheck / seqscan qual SILENTLY returned zero rows: a competing `ORDER BY` that
  steals another index (e.g. the PK for `ORDER BY id`, deterministic under `enable_sort=off`), a cheaper
  alternative index, or a cost/stats flip that made even a bare `count(*)` intermittently return 0 (the symptom
  originally mis-filed as an F1 "cold scan" bug). It cannot match a field-scoped jsonb query from one heap
  column value (no index handle/segments/analyzer), and the correct index path never reaches it (`xs_recheck=false`;
  no `amgetbitmap`/`amcanreturn` — independently verified in the fix's adversarial review), so it now
  `ereport(ERROR, FEATURE_NOT_SUPPORTED)` with a hint to force the bm25 index (`enable_seqscan=off`, anchor on
  the first indexed column, no competing `ORDER BY`; `col &@@ q` for ranked retrieval). Fail-loud over
  silent-wrong, matching the project philosophy. Gate `52_jsonb_filter_index_only` (seqscan / pkey-Filter /
  direct-call ERROR; the bm25 Index Scan path still returns the correct set — deterministic on PG17/18 via the
  `disabled_nodes` planner). The `(text,text)` `@@@` (`bm25_match`) is intentionally UNCHANGED — it does a real
  default-english match on the seqscan fallback (field-blind on a non-english index), so it is not silently-zero.

## Addendum (2026-07-13)

The **"Format breaks (REINDEX) — v3→v4→v5, now complete"** bullet's closing
stance — treat the format as throwaway, REINDEX is expected on every future
change — is **superseded by `docs/adr/0009-format-stability.md`**. That record
replaces the exact-equality version gate with a two-directional
`min_read_version` floor and ships a v6 negotiation baseline: additive format
changes (a new optional trailing region, a new page type, a filled sentinel)
no longer require a REINDEX at all, and even a genuinely breaking change gets
an online `bm25_upgrade(regclass)` path when a transform is registered for it.
The bullet's historical content (why v3→v4→v5 broke, what M4/M5/M2b each did)
remains accurate as a record of what already happened and is left as written;
only the forward-looking "any future change is a hard break, treat the index
as throwaway" framing no longer holds. See 0009 for the current contract.

## Addendum (2026-09-29, the over-pull tail is worse than score drift)

The **"Over-pull tail takes a SECOND snapshot"** bullet calls the seam narrow,
non-corrupting, and a matter of scores drifting across the k-boundary. That
understates what the code allows (issue #268). The mechanism below was verified by
reading `bm25_gettuple`; it has not been reproduced, and the consequences marked
"inferred" have not been traced through the executor.

When the executor pulls past `wand_top_k`, `bm25_gettuple` rebuilds the full
exhaustive ranking under a fresh snapshot, which recomputes `ndocs`, `avgdl` and df
from the current index and therefore every score. It then finds the last emitted TID
in the NEW ordering and continues after it (ADR 0030 records that resume-by-TID). The
new ordering is sorted by the new scores, and nothing ties it to the scores the first
`wand_top_k` rows were already emitted under. If a write commits between the two
builds (another session, or the same transaction) and moves those statistics so that
the last emitted row now scores lower than it did, rows after it in the new ordering
can score higher than rows already handed to the executor. For an index-ordered scan
(`amcanorderbyop`) the executor takes the order values from the index and does not
re-sort, so the client would see a sequence that is not in score order (inferred).
Two further effects are also inferred from the code: a row the new ordering places
before the last emitted row but that was never emitted is skipped, and a row already
emitted that the new ordering places after it is emitted again. Membership can also
change between the builds. If the last emitted row was deleted and vacuumed, its TID
is not found, resume falls back to the ordinal, and if nothing follows at that
ordinal the scan ends never positioned, so accessors that require a positioned scan
return NULL or `+inf` (never a wrong finite value).

The default `LIMIT <= 100` path never reaches the tail, and all of this needs writes
between the first `wand_top_k` rows and the executor's next pull, but "narrow" should
not be read as "scores only drift". The candidate fix in the bullet (reuse the first
build's snapshot for the tail rebuild) has a cost the bullet did not mention: the retry
wrapper's seg_gen-reuse abort relies on taking a fresh snapshot, so reuse needs either a
pinned mechanism or a defined failure mode. A cheaper alternative is to keep the fresh
snapshot and make the tail a strict continuation (drop rows scoring above the last
emitted score, skip TIDs already emitted). Neither is done; #268 tracks it.

## Addendum (2026-10-04, the over-pull tail fixed; corrections to the 2026-09-29 addendum)

Issue #268 was reproduced deterministically, and four statements in the 2026-09-29
addendum (and the bullet it amends) are wrong:

- **"The default `LIMIT <= 100` path never reaches the tail."** It does. Under a filter
  qual the executor discards rows the index emitted, so `LIMIT 5` at the default
  `wand_top_k` of 100 pulls past the capped 100 whenever the filter rejects most of them
  (reproduced: five qualifying rows ranked 50th to 300th).
- **That it takes a commit** ("A concurrent commit landing in that window" in the
  bullet; "If a write commits between the two builds" in the addendum). It does not.
  The corpus statistics the rebuild recomputed (`bm25_scan_corpus_stats`) count every
  valid pending TID with no visibility check, so an uncommitted insert in another session, an aborted insert, and the
  scanning transaction's own later inserts all moved them, as did VACUUM tombstones and
  merges. All three insert kinds were reproduced.
- **"Consistent with the AM's live-committed-stats model."** The statistics are not
  committed-only; see the previous point. They are live in the sense of "whatever is in
  the index", visibility aside.
- **Skip and repeat "inferred".** Both were reproduced, together with out-of-order
  distances: a same-transaction insert of short rows (avgdl shrinks) re-emitted an
  already-emitted row at a different distance; one of long rows (avgdl grows) skipped a
  row never emitted. The ordinal fallback used when the last emitted TID was missing from
  the new ranking also resumed one slot off.

**Fix.** The capped WAND build pins the inputs a document's score is computed from in
`so->stats_pin`: per-field avgdl, the per-field k1/b/boost, and each query token's
per-field idf. The tail rebuild still takes a fresh snapshot for postings and
membership, and still runs `bm25_term_idf` for its dictionary lookups, but scores under
the pinned values, so a document both builds saw scores bit-identically in both. The
tail then resumes at the first entry strictly after the last emitted (score, TID) in
`scored_desc` order, replacing ADR 0030's resume-by-TID and the ordinal fallback; that
is exact even when the last emitted row has vanished. The invariant: a rebuild may only
extend the emitted prefix, and emitted rows' scores are immutable. A cassert build checks
it after the tail rebuild, for every emitted row the scan's snapshot can still see.

The bounded claim: for a WAND-capped scan pulled past its k rows, the tail emits every
row visible to the scan's snapshot exactly once, in non-decreasing distance under one
consistent score computed with the first build's statistics. Rows the snapshot cannot
see can appear in the rebuilt ranking and are filtered by the executor as before. The
earlier candidate fix (reuse the first build's snapshot) was not taken: it needs the
segment pages behind that snapshot held against reuse for the life of the cursor,
while pinning the statistics needs nothing held, and the seg_gen retry of the tail
rebuild keeps working unchanged.

The per-field `k1_<col>`, `b_<col>` and `boost_<col>` reloptions are not registered
options, so `ALTER INDEX ... SET` takes only ShareUpdateExclusiveLock for them and they
can change under an open scan (the index-wide `k1`/`b` and the analyzer options take
AccessExclusiveLock and cannot). The pin covers them for the tail.

## Addendum (2026-10-04, the lock level of the index-wide reloptions)

The 2026-10-04 addendum above says the index-wide `k1`/`b` and the analyzer options
take AccessExclusiveLock "and cannot" change under an open scan. That is true only in a
backend that has already registered those options. bm25 registers them lazily, on the
first `bm25_options` call in a backend, and core derives an ALTER's lock level from the
options registered in the backend running it, so from a backend that has not loaded a
bm25 index's relcache entry the ALTER takes only ShareUpdateExclusiveLock for them too.
The statement about the per-field knobs is unaffected, and the pin still covers k1, b and
boost for the tail. The analyzer is not pinned: a change that makes the query analyze to
a different token count between the builds, by such an ALTER or by ALTER TEXT SEARCH
DICTIONARY (which takes no index lock), raises XX000 in the tail rebuild. See [0012](0012-live-k1b-reloptions.md)'s
2026-10-04 addendum and [0108](0108-a-wand-rebuild-may-only-extend-the-emitted-prefix.md).

## Addendum (2026-10-04)

The fresh-eyes review of this date reproduced disjoint index-vs-filter results for field-scoped and multi-column `@@@` queries (#298) and the FATAL escalation of the subtransaction retry on hot standby (#307). See those issues; the accepted tradeoffs here should be revisited when they are decided.

## Addendum (2026-10-05)

The `(text,text)` `@@@` (`bm25_match`) was recorded above as intentionally unchanged off
the index. That no longer holds for a field-scoped RHS: since #298 (decided 2026-10-05) it
raises `ERRCODE_FEATURE_NOT_SUPPORTED`, through the prefix predicate described in ADR 0004's
addendum of this date. A bare RHS off the index still matches the LHS column only, and the
trade stays accepted and documented. The first of the two points in the 2026-10-04 addendum
is therefore decided; the second (#307) is not. #306 must change the off-index scope rule and
the index path's together.

## Addendum (2026-10-05, PRs #331-#350)

- **F2 is replaced.** `bm25_snippet` now highlights any query tree from its non-negated
  leaves, with wildcard patterns globbed against the field's own tokens, so a boolean,
  boost, phrase or wildcard scan no longer returns NULL on every row (ADR 0123, PR #337,
  #308). The workaround F2 describes is no longer needed. F1 and the rest of this record
  stand.
- **The #298 off-index trade, restated.** Since #306 the index path and `bm25_match` share
  the scope predicate, and the documented invariant is "off-index refuses or agrees, never
  silently differs": `'http://x'` answers on the index and is refused off it (ADR 0122). The
  second point of the 2026-10-04 addendum (#307) is decided by ADR 0121.
- **The tail rebuild's analyzer-drift error is 55000, not XX000.** The 2026-10-04 addendum on
  index-wide reloption lock levels says a query that analyzes to a different token count
  between the builds "raises XX000 in the tail rebuild". It is reachable by concurrent DDL, so
  it now raises `ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE` (55000) (#313 XCUT-12, PR #345).
