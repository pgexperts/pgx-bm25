# ARCHITECTURE

Cold-start map of `bm25_native` for a worker with no prior context. Read it
whole before touching code. It states **what the system is** and **what will
break if you get it wrong** — not how to build it (see [README.md](README.md))
and not why the design is shaped this way (see [THEORY.md](THEORY.md)).

The rationale behind every invariant below lives in THEORY.md; individual
design decisions, with the alternatives rejected, are recorded in
[docs/adr/](docs/adr/).

## What this is

A PostgreSQL **custom index access method** (`USING bm25_native`) that implements Okapi
BM25 ranked full-text search. Pure C against stock PostgreSQL 17/18 server
headers via PGXS. The entire index lives in the index relation's own pages, so
crash recovery and physical replication come from core. Three hard constraints
shape everything (see THEORY §2): **Generic WAL only** (no custom rmgr), **no
background workers**, **preload-free**.

The **on-disk format** is a segmented LSM with a per-index analyzer. Its current version
is whatever **`BM25_FORMAT_VERSION`** (`src/bm25_format.h`) says; this document does not
repeat that number in prose, because a prose copy of it rots at the next bump. A version
number appearing below is a historical statement about what THAT version introduced, not
a claim about the build. An index does not have to carry the current number to be
readable either: the gate is a two-directional floor (`bm25_meta_validate`, see below),
so anything from `BM25_OLDEST_READABLE` up is read as-is with no REINDEX. v3→v4 was a hard
break — at the time, the gate was an exact-equality check that refused any
`format_version` other than the build's own with `ERRCODE_FEATURE_NOT_SUPPORTED` and a
REINDEX hint. That exact-equality gate is **gone** as of v6; the floor gate that
replaced it refuses a too-old index with "bm25: index
is format version %u; this build reads >= %u" instead, and v3/v4 now fall below
`BM25_OLDEST_READABLE` rather than being rejected for inequality.
**M5 (multi-field / BM25F / `key_field`)** and **M4 (positions /
phrase / snippets)** both FILLED into the reserved v4 surface with NO further break,
NO version bump, and NO forced REINDEX for existing bag-of-words use (a v4 index
built by M3/M5 still reads — its bag-of-words bytes are unchanged; `pos_root`/
`pos_post_root`/`BM25_PAGE_POS` simply stay Invalid until REINDEX enables positions).
**M2b (block-max WAND)** could not be filled the same way: the one remaining reserved
slot, `BM25BlockHeader.max_impact` (a seal-time float4), was the wrong SHAPE for a
safe scan-time bound — it needed a per-field `(max_tf, min_doclen)` table instead.
That forced the second hard break, v4→**v5** (`REINDEX required`); after v5 the
reserved v4 surface is **fully and finally consumed** — the format now carries no
reserved-but-unused fields. **M6 (boolean / wildcard jsonb query trees)** adds NO
on-disk change (still v5): it is a query-layer feature — new SQL builder functions,
`(text,jsonb)` operator overloads, and a `src/bm25_query.c` AST evaluated entirely at
scan time. **Roadmap #4 (format stability + upgrade story)** then bumped v5→**v6** —
not another hard break, but the format-negotiation baseline: the exact-equality
version gate is replaced by a `min_read_version` floor (see "On-disk layout"
below), so a v5 index reads through unchanged and future *additive* changes need
no REINDEX at all. See "Current state" and the metapage section.

## Current state

- **Branch `feat/format-stability` (roadmap #4): on-disk format stability + upgrade
  story COMPLETE.** The v4→v5 exact-equality version gate — any format bump forced a
  hard break and a full REINDEX, and after v5 the format had no reserved-but-unused
  fields left to fill in place (see the "fully consumed" note above) — is replaced by
  a two-directional `min_read_version` **floor** gate (`bm25_meta_validate`,
  `src/bm25_meta.c`), checked on the query hot path via `bm25_scan_snapshot`
  (`src/bm25_seg_read.c`) as well as on every build/maintenance `bm25_meta_read`.
  That branch bumped the format to **v6**, a metadata-only negotiation
  baseline (`BM25_FORMAT_VERSION` has moved on since; the gate, not the number, is the
  contract): a legacy v5 index reads its unwritten `min_read_version`/`feature_flags`
  as `0` from the zeroed metapage tail and is accepted REINDEX-free (`BM25_OLDEST_READABLE
  = 5`, so v4 and older are still refused exactly as before). Going forward, an
  **additive** change (a length-prefixed trailing region, a new page type, a filled
  sentinel) never raises the floor and needs no REINDEX; only an existing field's
  shape changing, or a *packed multi-record struct* (`BM25DictEntry`,
  `BM25BlockHeader`, the segcat entry) growing a field, is **breaking** and raises the
  floor. A new **`bm25_upgrade(regclass)`** SQL function gives a breaking change an
  online, no-heap-rescan migration path (seal → gate-validate → per-segment transform
  dispatch, identity today → atomic merge-swap publish with the version re-stamp
  folded into the SAME WAL record as the catalog flip); the transform registry ships
  empty, proven via a test-only synthetic transform. Packaging got the project's
  first extension upgrade script (the former `pg_bm25_index--0.1--0.2.sql`,
  `default_version` 0.1→0.2, since folded into the consolidated
  `bm25_native--1.0.sql`, which is the ONLY script today and is **frozen** since the
  1.0.0 release, so `ALTER EXTENSION ... UPDATE` has no path to follow until the first
  update script, e.g. `bm25_native--1.0--1.1.sql`, ships); the designed rollout is binaries-first, then
  `ALTER EXTENSION ... UPDATE`, then `bm25_upgrade` on the primary only (standbys
  converge via WAL replay). **No query
  or scoring capability change** — `docs/adr/0009-format-stability.md`;
  `sql/55_format_compat` (61 SQL suites), `t/013_format_compat_replica.pl`,
  `t/014_upgrade_crash.pl`.
- **Branch `feat/score-robustness` (R3, post rank-collapse fix): score-accessor
  concurrency FIXED (ROADMAP §3).** The single backend-global
  `bm25_active_scored_scan` pointer — silently wrong whenever two bm25 scored scans
  were concurrently live in one query (a correlated subquery / self-join, each side
  ranking: whichever scan's `bm25_gettuple` ran most recently clobbered the other's
  slot) — is replaced by an intrusive REGISTRY of active scored scans
  (`bm25_register_scored_scan`/`_deregister_scored_scan`/`_scored_scan_head`/
  `_scored_scan_count` (the count was removed later, `docs/adr/0064`), linked via `next_active`). `bm25_score`/`bm25_score_key` now
  resolve the projected row to its OWNING scan by ROW IDENTITY and were taken to be fully
  concurrent-correct (bounded since: `docs/adr/0103`; `docs/adr/0105` adds overloads that
  name the scan); `bm25_snippet` (no row identity to resolve against) FAILS LOUD
  instead of guessing; `bm25_distance`/`bm25_distance_jsonb` (which double as the
  `&@@` sort-key's per-row resjunk projection on EVERY ranked query, so they cannot
  fail loud) HEAD-RESOLVE instead — unchanged externally from the pre-R3 behavior
  for that one accessor pair. A `MemoryContextCallback` on the scan's `scanctx`
  deregisters it even on the aborted-portal path where `bm25_endscan` is skipped.
  **No format or SQL-signature change** (still v5). See "Score accessor concurrency
  (R3)" below. Adds `sql/53_concurrent_scored_scans` (**59 SQL suites**).
- **Branch `fix/orderby-rank-collapse` (post-M6): rank-collapse FIXED.** The pre-existing
  whole-index bug (since M1, identical for text and jsonb) where `ORDER BY x &@@ q, <secondary
  key>` silently collapsed BM25 rank onto the secondary key is closed, with **no format
  change** (still v5). `bm25_gettuple` now stashes each returned tuple's `-score` on the scan
  opaque (`so->cur_orderby_dist`, set alongside `xs_orderbyvals[0]`), and `bm25_distance`/
  `bm25_distance_jsonb` return that stash instead of a constant `+inf` while a scored scan is
  active — `+inf` remains the degradation off the index (seqscan / no active scored scan). So
  `ORDER BY x &@@ q, <tiebreak>` now sorts CORRECTLY, and `x &@@ q` is a usable score-distance
  projection (`= -bm25_score(ctid)`). See the corrected root cause and the stash-pairing
  invariant in Landmines. Adds `sql/50_orderby_dist.sql` (**56 SQL suites**).
- **Branch `impl/m6`:** **M6 (boolean + wildcard jsonb query trees) COMPLETE.** A second
  RHS type for `@@@`/`&@@`: **jsonb** query objects built by SQL functions
  (`bm25_term`/`bm25_match_terms`/`bm25_phrase`/`bm25_wildcard`/`bm25_boolean`/`bm25_boost`),
  parsed into a `BM25Query` AST (`src/bm25_query.c`) and evaluated as a boolean
  must/should/must_not formula over a per-doc presence bitmask at drain. Boolean
  **membership is EXACT**; the score stays the single-pass BM25F OR-sum (**bag-of-words** —
  see the section + Landmines). Wildcards expand over the stemmed dict (prefix range + glob),
  **bypassing the stemmer**. jsonb VALUES are data, never a parsed DSL — structurally
  injection-proof. **NO on-disk change (still v5)**: new builders, `(text,jsonb)` operator
  overloads on `text_bm25_ops` (strategies 1/2), new GUCs
  (`bm25_native.wildcard_min_prefix`/`_max_expansions`), and the new `bm25_query.c`. The
  `(text,text)` path is byte-identical. Multi-leaf jsonb bypasses WAND (exhaustive); a single
  MATCH/TERM leaf is text-identical (WAND-eligible). **56 SQL suites** green on PG 17/18
  (M6 adds `46_m6_boolean`/`47_m6_wildcard`/`48_m6_builders`/`49_m6_acceptance`; the post-M6
  rank-collapse fix adds `50_orderby_dist`, see above). See the
  "Boolean + wildcard query trees (M6)" section.
- **Branch `impl/m2b`:** **M2b (block-max WAND) COMPLETE.** Ranked (`ORDER BY col &@@ q
  LIMIT n`) scans run **Block-Max WAND by default**, skipping whole posting blocks whose
  best-possible score cannot enter the current top-k, while returning results
  **bit-identical** to the exhaustive OR-sum scorer it replaces. This is the LAST fill of
  the reserved v4 format surface: `BM25BlockHeader.max_impact` (the wrong shape for a safe
  scan-time bound) is dropped and replaced by a per-block, per-field impact table, forcing
  the sole remaining hard break to format **v5** (`REINDEX required`). See the "Block-max
  WAND (M2b)" section. **51 SQL suites + 14 TAP suites** green on PG 17/18.
- **M4 (positions → phrase / proximity / snippets), merged into this branch's base:**
  Positions flow to a SEPARATE per-segment `BM25_PAGE_POS` chain (the Lucene
  `.doc`/`.pos` split), read back by an optional lockstep cursor, replayed through merge,
  and reclaimed by the FSM. A phrase/proximity query surface (`"a b"`, `"a b"~n`,
  `"a b"~>n`) matches via a post-accumulation recheck over the ranked TID set;
  `bm25_snippet(field, …)` highlights query terms by re-analyzing the passed text. Per-field
  `store_positions` (default true) rides a trailing flag array on the field-config page.
  See the "Positions / phrase / snippets (M4)" section. **46 SQL suites + 13 TAP suites**
  green on PG 17/18 at M4 completion.
- **M5 (merged into this branch's base):** **multi-field / BM25F / `key_field`.** A
  multi-column `CREATE INDEX ... USING bm25_native (title, body)` makes each indexed column a
  dense **field** (`field_id`) with its own `k1`/`b`/`boost`
  (`boost_<col>`/`k1_<col>`/`b_<col>` reloptions). The scorer is **BM25F** —
  `Σ_field boost_f · idf_f · termscore(tf, doclen_f, avgdl_f, k1_f, b_f)` in the single
  scorer pass, with exact per-field `avgdl` (per-field `Σdoclen`/`N` in the segment
  header). A query can **scope to one field** via `field:term` on the ranked `&@@` path.
  A **`key_field`** (an `INCLUDE` column) records a docid→key map so results return the
  user key (ctid fallback). See the "Multi-field / BM25F / key_field (M5)" section.
- **Format v4 + M3 analyzer (merged base):** the `ts_lexize` Snowball analyzer (lowercase
  → stopword → stemmer), baked at CREATE INDEX as a 32-bit FNV-1a `analyzer_fingerprint`;
  build/insert/query tokenize through one `bm25_analyze`; scan-start fingerprint gate
  (ERROR by default, WARNING under `require_analyzer_match = false`). The bm25 index scan
  is authoritative — `xs_recheck=false`; the `@@@` operator `bm25_match` is the
  default-english bare-filter/seqscan fallback and must not re-check the index path
  (it refuses a phrase or a `field:` scope, #132/#298, and sees only the LHS column).
- **M2a (merged base):** pending list, two-phase seal into immutable segments,
  multi-segment dedupe-by-TID scored union, tombstone deletes + VACUUM, tiered merge
  engine (single-record atomic catalog swap + in-swap segment retire), XID-horizon
  retired-segment reclamation, and the stamp-and-gate page-reuse allocator.
- **Hardening CI (merged):** a gating job source-builds a **cassert + debug** PostgreSQL (17
  and 18 since #309, at their current minors, with `wal_consistency_checking` on)
  and the extension with **UBSan**, then runs the full `installcheck` (SQL + TAP) — catching
  the assert / undefined-behavior / misaligned-access class the PGDG `build-and-test` matrix
  cannot see (`docs/adr/0008-cassert-ubsan-ci.md`). It surfaced two real pre-existing bugs on
  bring-up (a misaligned field-config page copy; an `INT4` array deconstruct unsupported below
  PG 18.2).
- **Format stability + upgrade story (roadmap #4) COMPLETE** — see the
  `feat/format-stability` bullet above. This was the last cross-cutting production/ops
  gate on the roadmap; "treat the format as throwaway, expect REINDEX" no longer holds
  (superseded, `docs/adr/0005-tradeoffs.md` addendum → `docs/adr/0009-format-stability.md`).

## Module map

The arrows below give the main direction of calls. They are not a strict layering: at the
object-file level several pairs of modules call each other directly (for example `meta` ↔
`build`, `scan` ↔ `query` through `bm25_field_by_name`, `handler` ↔ `tokenize`, the segment
reader ↔ `seg_build`, now across files since `docs/adr/0101`: `seg_build` calls `seg_read`, `seg_chain` calls `seg_build`). The one such pair that was a design seam, `wand` → `scan` → `wand`, is gone
(#67.13, `docs/adr/0093`). The code both ranking builders share lives in `stats`, so
`scan` → `wand` → `stats` and `scan` → `stats`, with no call running back up.
`handler` → `build`/`scan` →
`pending`/`accum` → `seg_build`/`seg_read` → `meta` + `format`; `vacuumcleanup`
also drives `merge` → `seg_build`. `seg_build`/`seg_read`/`merge`/`scan` call
`keymap` for the docid→key map; `scan` calls `phrase` (the pure matcher),
`snippet` (the highlighter), (M2b) `wand` (the block-max WAND engine, which
itself calls back into the segment reader, `seg_read`/`seg_dict`/`seg_chain`, for header-only block reads, liveness, key
projection), `stats` (per-term idf, the pending scoring arm and the score accumulator,
shared with `wand`), and (M6) `query` (the jsonb query-tree parser/evaluator) + `seg_dict`'s
`bm25_dict_expand_wildcard`; `tokenize` resolves tokens through `analyzer`; (roadmap #4)
`upgrade` drives `merge`'s rewrite-and-swap entry point (`bm25_merge_rewrite_all`) and
reads/writes `meta` directly for the identity re-stamp; `analyzer`, `keymap`, `phrase`,
`query`, and `format` are leaves.

**M5 shape (multi-field / BM25F / key_field), threaded across the files below:** a
multi-column index makes each column a dense `field_id`; the accumulator, posting
blocks (per-block field-id RLE), NORMS (per-field doclen), and the segment header
(`total_len_by_field[]` + `ndocs_by_field[]`) all carry the field dimension; the
scorer sums per-field `idf·boost·termscore`; `bm25_rescan` parses `field:term`; a
`key_field` INCLUDE column is written to a `BM25_PAGE_KEYMAP` chain and survives
merge + reclaim. Single-field / no-key indexes are byte-identical to M3.

**M4 shape (positions / phrase / snippets), threaded across the files below:** the
accumulator retains each posting's token positions instead of discarding them; the
seg builder writes them to a SEPARATE per-segment `BM25_PAGE_POS` orphan chain (rooted
at `header.pos_root`, per-term at `dict.pos_post_root`/`pos_post_off` — the structural
twin of `post_root`/`post_off`), one `[tf-count][Δpos × tf]` frame per position-bearing
posting; the seg reader gains an OPTIONAL lockstep `pos_cb` cursor (bag-of-words scans
pass NULL and never fault a POS page); merge replays positions and reclaim mirrors every
`keymap_root` site with a `pos_root` twin; the pending path carries true positions as a
delta-varbyte blob per entry (`BM25PendingTermEntry.pos_bytes`). On top of the stream,
`bm25_scan.c` parses the phrase/proximity RHS and `bm25_scan_match.c` runs the
`bm25_phrase.c` matcher as a post-accumulation recheck; `bm25_snippet.c` re-analyzes the passed field text. A per-field
`store_positions` bit (default true) rides a trailing flag array on the field-config page.
An index built pre-M4 (positions off) is byte-identical to M5 and bag-of-words-only.

**M6 shape (boolean / wildcard jsonb query trees), query-layer only — NO on-disk change:** a
second RHS type for `@@@`/`&@@` — jsonb query objects (SQL builders → jsonb) parsed by
`src/bm25_query.c` into a `BM25Query` AST, flattened to per-leaf `leaf_bit`s (≤ 64), scored in
the ONE exhaustive scorer pass with each positive leaf's terms ORed under its bit and its
presence recorded in the REUSED `and_presence` bitmask, then filtered at drain by
`bm25_query_eval` (must/should/must_not over the uint64 mask). Wildcard leaves expand over the
byte-sorted STEMMED dict (`bm25_dict_expand_wildcard`, `bm25_seg_dict.c`), pattern matched
raw-lowercased (stemmer bypassed). A single MATCH/TERM jsonb leaf is copied to `so->qterm` and
runs the `(text,text)` scan path verbatim (byte-identical, WAND-eligible); anything multi-leaf
takes the exhaustive scorer. See the "Boolean + wildcard query trees (M6)" section.

| File | Role |
|------|------|
| `src/bm25_format.h` | The on-disk contract: every page/record struct, page-kind flags (incl. `BM25_PAGE_FIELDCFG`, `BM25_PAGE_KEYMAP`, `BM25_PAGE_POS`), constants (`BM25_POSTINGS_PER_BLOCK=128`, **`BM25_FORMAT_VERSION`** — the SINGLE SOURCE OF TRUTH for the current format version, deliberately not restated as a number anywhere in this document or in that header's prose, `BM25_OLDEST_READABLE=5`, `BM25_MAX_PHRASE_SLOP=100000`), the `BM25FieldConfigHeader`/`BM25FieldConfig` structs, the length-prefixed `total_len_by_field` segment-header serializers, the M4-filled position fields (`pos_root`/`pos_post_root`/`pos_post_off`, `BM25PendingTermEntry.pos_bytes`), the **v8** pending per-field doclen array (`BM25PendingDocHeader.nfieldlens` + the `BM25_PENDING_DOC_FIELDLENS` flag, with `bm25_pending_doc_entries_off`/`bm25_pending_doc_fieldlens` as the ONE stride rule every walker uses instead of a bare `MAXALIGN(sizeof(BM25PendingDocHeader))`), the M5 `keymap_root`/`field_rle_bytes`, the **M2b**-filled `BM25BlockHeader.impact_bytes` + trailing `BM25BlockImpact` (`bm25_encode_impact_table`/`bm25_decode_impact_table`) replacing the dropped `max_impact` float4, the still-reserved `dict_pad` (an explicit memset-able tail pad for WAL determinism — NOT an alignment pad and not a feature slot; see the reserved-surface section), the **v6** `min_read_version`/`feature_flags` metapage fields + `BM25FormatRestamp` (the non-on-disk carrier threading a version re-stamp into the merge swap's WAL record), the **ADR 0088** `total_tokens` field named into `BM25SegCatEntry`'s and `BM25SegmentHeader`'s existing 4-byte padding holes (`sizeof` unchanged at 40/64 — ADR 0009's additive shape (iii), no `min_read_version` bump) plus its trust gate **`BM25_FEAT_SEGCAT_TOKENS`** (feature_flags bit 3, stamped ONLY by a fresh build — see `bm25_meta.c` below), **`BM25_PAGE_ALL_KNOWN`** (the OR of every page kind this build knows — the orphan sweep refuses to free anything outside it, which is what makes "a new page type" an ADDITIVE change; **a new `BM25_PAGE_*` flag MUST be added to it**), and accessor inlines (`BM25PageGetOpaque`, `BM25PageGetMeta`). |
| `src/bm25.h` | Cross-module prototypes and opaque typedefs. |
| `src/bm25_wand.h` | **M2b** the block-max WAND engine's public interface: `BM25WandCtx` (per-term scan-time stats), `bm25_block_ub` (the safe per-block bound), `BM25WandStats` (blocks_examined/skipped, docs_scored, deep_check_skips instrumentation), the opaque `BM25TopK` bounded top-k heap and `BM25Scored` result struct, the opaque `BM25WandCursor` (open/docid/next/next_geq/block_last/block_max/global_ub/score_doc), and the driver entry point `bm25_wand_build_ranking`. Given its own header (unlike most internals, which share `bm25.h`) because WAND is a distinct, largely self-contained subsystem bolted onto the scorer at one seam. Declares only what `bm25_wand.c` defines. The driver's per-term idf and pending scoring come from `bm25_stats.h`, through two static adapters in `bm25_wand.c` (`bm25_wand_prepare_terms`/`bm25_wand_score_pending`). Those used to be defined in `bm25_scan.c`, so the driver called back into the scanner that called it; `docs/adr/0093` removed that. |
| `src/bm25_query.h` | **M6** the jsonb query-tree AST public interface: the `BM25Query` node (MATCH/TERM/PHRASE/WILDCARD/BOOLEAN/BOOST), `bm25_query_parse` (jsonb→AST + validation), `bm25_query_flatten` (assign `leaf_bit`, fold `boost`, propagate `negated`, enforce `BM25_QUERY_MAX_LEAVES`=64 so `1<<leaf_bit` fits a uint64), `bm25_query_eval` (boolean formula over a uint64 presence mask), `bm25_glob_match`. A separate header (like `bm25_wand.h`) because the query AST is a self-contained subsystem. |
| `src/bm25_handler.c` | AM registration (`bm25_handler` → `IndexAmRoutine`); `ambulkdelete`/`amvacuumcleanup`; the `@@@` recheck operator `bm25_match` (tokenizes via the default analyzer — no index handle, so it cannot read reloptions; intersects the two token sets by sorting the SMALLER and binary-searching it, `CHECK_FOR_INTERRUPTS` per probe — the old nested scan was O(nd × nq) over unbounded user text with no interrupt point, i.e. days of uncancellable CPU from one expression, `docs/adr/0033`); `amcostestimate`, `amvalidate`; `amoptions` = `bm25_options` (the `analyzer`/`language`/`stopwords`/`tokenizer`/`require_analyzer_match`/**`store_positions`**/**`phrase_fallback`** reloption parser, + the per-field `store_positions_<col>` knob threaded through `bm25_strip_field_knobs`). Every string reloption but `key_field` carries a `validate_string` callback, so a bad value fails at `CREATE INDEX`/`ALTER INDEX` rather than later: `bm25_validate_analyzer` (reserved surface -- only its default `english`; the value selects nothing, so accepting others was accepting a knob that did nothing) and `bm25_validate_language` (in `bm25_analyzer.c`, since it must fold and resolve `<language>_stem` the way the resolver does). That one resolves under a RESTRICTED search_path on purpose: `DefineIndex` calls `RestrictSearchPath()` before `index_reloptions(..., validate=true)`, so `CREATE INDEX` already validates against `pg_catalog` alone, while `ATExecSetRelOptions` restricts nothing -- so the validator restricts for itself and both DDL paths answer the same question, *will the BUILD find this dictionary*. It does not pin anything for INSERT or scan, which run unrestricted; that divergence is the analyzer-fingerprint gate's job (`docs/adr/0081`), alongside the pre-existing `bm25_validate_stopwords`/`bm25_validate_tokenizer`/`bm25_validate_phrase_fallback`. `key_field` cannot have one: its check needs a `Relation`, which `bm25_options` does not have. Registers the `bm25_native.seal_threshold` GUC. Also registers
`bm25_native.debug_pause` (`PGC_SUSET`, empty string = off): names a
comma-separated list of pause points, so one backend can park twice. Nineteen exist, in
this order (1 to 19): `bulkdelete_start`, `bulkdelete_segment`, `bulkdelete_pending`,
`merge_preswap`, `swap_after_snapshot`, `insert_keycheck`, `scan_pending_page`,
`orphan_sweep_start`, `merge_start`, `pending_append_alloc`, `orphan_sweep_marked`,
`reclaim_retired_compacted`, `reclaim_retired_between_chunks`, `merge_between_passes`,
`rank_build_attempt`, `keymap_rotation`, `scan_post_page`, `drain_pending_page` and
`debug_dict_entry`. The last three exist for `bm25_native.debug_cancel_at`: they sit
just ahead of the interrupt checks that `sql/66` and `sql/80` measure, so those suites
inject their cancels instead of timing them (`docs/adr/0070`, 2026-10-06 addendum).
They are checked against a table by `bm25_debug_pause_check`, and the table is
append-only, since N is the position. At a named point
the backend takes and releases `pg_advisory_lock(BM25_DEBUG_PAUSE_LOCKKEY, N)` so
the TAP suites (`t/020`, `t/022`, the standby pending-recycle test of #291 -- a
standby `@@@` scan parked between two pending pages -- and the cleanup, sweep and
bounded-hold tests of #300, `t/027` to `t/032`) can park it there deterministically, no
injection points needed
(`docs/adr/0102`). Test lever only, `PGC_SUSET` because a parked VACUUM or
merge holds the seal/merge singleton across the whole pause — setting it is a
way to stall every writer of the index (`docs/adr/0020`'s addendum).
`bm25_native.debug_cancel_at` (`PGC_SUSET`, empty string = off; issue #305) takes the
same point names and makes the backend cancel its own query at them, as a
`pg_cancel_backend` arriving at that step would, so a single-session suite
(`sql/148`, and the latency assertions of `sql/66` and `sql/80`) can show where a
cancel takes effect. Use it, not `statement_timeout`, for any assertion about WHEN a
cancel is serviced: a timer's service time belongs to the runner (`docs/adr/0070`).
`bm25_native.debug_cancel_after` (`PGC_SUSET`, 0 = the first hit) holds such a cancel
back until the backend's `bm25_debug_work_units()` counter reaches it, so it lands
mid-loop. A first-hit cancel proves only that the loop's first check is live.
`bm25_native.debug_count_slicing` (`PGC_SUSET`, off; issue #289): makes the
segment writer cut posting blocks every 128 postings as it used to, instead of at
document boundaries, so `sql/125_wand_straddle` can still build the straddling
layout old segments carry and test the WAND reader on it. Test lever only,
`PGC_SUSET` on `debug_budget`'s ground (it changes shared on-disk layout).
`MarkGUCPrefixReserved("bm25_native")` runs after every `bm25_native.*` GUC is
registered, so a typo'd parameter name cannot be silently accepted as a
placeholder `USERSET` GUC that no-ops any of the seven `PGC_SUSET` guardrails. **M6:** the jsonb anchors for the `(text,jsonb)` operators — `bm25_match_jsonb` (**ERRORs off-index** — it cannot match a jsonb tree from a single heap column value; only reachable when the planner demotes `@@@` to a filter/seqscan qual, since `bm25_gettuple` sets `xs_recheck=false` and there is no `amgetbitmap`/`amcanreturn`, so erroring only surfaces that misuse — was `→false`, silently 0 rows; `52_jsonb_filter_index_only`) and `bm25_distance_jsonb` (→ the active scored scan's per-row distance stash, `+inf` only off the index — see the rank-collapse fix in Landmines); registers the `bm25_native.wildcard_min_prefix`/`bm25_native.wildcard_max_expansions` GUCs. Also owns `bm25_native.max_match_memory` (ADR 0047) — `PGC_USERSET` like `seal_threshold`/`wand_top_k`, NOT `PGC_SUSET` like the wildcard pair, because raising it only lets a session consume what it consumed before the bound existed. |
| `src/bm25_selfuncs.c` | **Roadmap #4 planner estimates** (new module) `bm25_matchsel`/`bm25_matchjoinsel`: the `@@@` operator's `RESTRICT`/`JOIN` selectivity estimators, both returning the compile-time constant `BM25_MATCH_SEL = 0.05` regardless of RHS shape (`Const`, `Param`, or correlated `Var` alike) — deliberately blind, never touching the index or table statistics. See "Planner interface" above and `docs/adr/0010-planner-estimates.md`. |
| `src/bm25_build.c` | `ambuild` (`bm25_build`: heap scan → `BM25Accum` → seal; stamps `analyzer_fingerprint` + writes the field-config page) and `aminsert` (`bm25_insert`: append to pending list, conditional seal). Build + insert tokenize via `bm25_analyze`. **M5:** `bm25_resolve_fields` maps each indexed column to a dense `field_id` (`field_count = natts`) with per-field `k1`/`b`/`boost` (validated) + resolves the `key_field` INCLUDE column; the build/insert callbacks tokenize EACH field and extract the key. **M4:** `bm25_resolve_fields` also resolves each field's `store_positions` bit into its `BM25FieldConfig` resolution. **Scratch contexts:** neither entry point may tokenize in `CurrentMemoryContext` — for `ambuild` that is the statement's (the table AM resets `econtext`'s per-tuple context but does not switch into it, hence GIN's `funcCtx`), and for `aminsert` it is `es_query_cxt`. `BM25BuildState.tupcxt` is reset per callback invocation and `bm25_insert` deletes a per-row scratch at both exits; the opportunistic seal runs AFTER that delete so the drain's accumulator is not parented on it. **#146 (BUILD-04):** the callback seals the accumulator and starts a fresh one whenever it crosses `bm25_maintenance_budget_bytes`, so ONE BUILD IS NOT ONE SEGMENT — a fresh chunk must be re-given the keymeta and the `store_positions` gate (`bm25_build_accum_new`), and `index_tuples` is summed across chunks rather than read off the last accumulator. Resetting under the accumulator is safe only because it `memcpy`s terms into `a->cxt` and copies positions by value (`docs/adr/0032`). |
| `src/bm25_tokenize.c` | `bm25_analyze`: the live tokenizer that runs the resolved analyzer (`ts_lexize` Snowball pipeline) over text, used identically at build, insert, and query time. Deterministic (a pure function of the resolved dict OID + database default collation + token) so WAL replay and standbys tokenize byte-identically. **Splits by CHARACTER, folds at EMIT (`docs/adr/0046`):** runs are measured with `bm25_next_char` over the ORIGINAL bytes and only the emitted term is folded, in that order for two reasons — a byte-wise `tolower` rewrites a multibyte lead byte outright (`0xC3`→`0xE3` under a UTF-8 `LC_CTYPE` on BSD/macOS, which made `CREATE INDEX` over accented text fail inside `ts_lexize`), and no fold is byte-length preserving, so folding first would invalidate the `src_off`/`src_len` spans measured against the original. The working copy survives only for its NUL terminator: `stem_dict_oid` is a user-chosen dictionary and must not be handed a bare varlena payload. The token array's UP-FRONT size is a clamped GUESS (`BM25_TOKCAP_INITIAL_MAX`, 1024 entries), not a bound: it used to be `textlen/2+1` entries at 24 bytes each — twelve bytes of scratch per input byte — so the tokenizer died on `MaxAllocSize` at ~89.5 MB of input against `text`'s own ~1 GB (ADR 0047). For INDEXING that ceiling sat above the tighter 65,535-token document limit (16-bit `tf`, reported clearly by the pending append), so the fix mostly corrects which error a huge document gets; the ceiling was OPERATIVE on the paths with no token limit — query-side `bm25_match` over unbounded user text, `bm25_snippet`, and the debug SRFs. It grows geometrically through the one shared `tok_array_grow`, whose ceiling names the DOCUMENT rather than the allocator. Growth is not optional: `ts_lexize` returns a lexeme ARRAY, so a compound splitter or thesaurus turns one run into several and the run count never bounded the token count. Also hosts the `bm25_debug_tokenize` SRFs (1-arg default-config + 4-arg explicit-config), and `bm25_tokenize`, the M0/M1 tokenizer now reached only by two debug SRFs (it shares the same character rules so a probe cannot disagree with the real tokenizer). |
| `src/bm25_analyzer.c` | M3 analyzer engine: `BM25AnalyzerConfig`, the reloption→config resolver (`bm25_analyzer_config`), the Snowball dict OID lookup (resolved by NAME, e.g. `english_stem`, on EVERY call — there is no cache here; the syscache underneath is what makes that cheap), `bm25_stemmer_identity` (the name-derived stemmer id the fingerprint stores, `docs/adr/0080`), the FNV-1a `bm25_analyzer_fingerprint` (whose seventh component, `bm25_analyzer_probe_hash`, is the one cached thing here: a per-backend dictionary-OID → probe-hash table in `CacheMemoryContext`, dropped wholesale by a `TSDICTOID` syscache callback, #296), and the per-index field-config page reader/writer (`bm25_fieldcfg_write`/`bm25_fieldcfg_read`, and `bm25_fieldcfg_read_keystamp` for the #292 key-identity stamp tail, with its `bm25_debug_keystamp`/`bm25_debug_clear_keystamp` probes). **M4:** the writer appends a trailing `uint8 store_positions[field_count]` flag array AFTER the config records (no `BM25FieldConfig` struct growth → existing M5 pages still parse); the reader returns it when present past `pd_lower`, absent ⇒ positions off. `bm25_require_analyzer_match` reads the reloption for the scan gate. **`docs/adr/0046`:** also the single definition of "word character" and "lowercase" for the whole extension — `bm25_is_word_byte`, `bm25_next_char` and `bm25_fold_term` (`str_tolower` under the database default collation, the same fold `dict_snowball` applies internally). Four sites previously each carried their own byte-wise approximation and disagreed. `bm25_lower_ascii` is a true `'A'..'Z'` fold, NOT `tolower()`, because its result feeds both the dict-name lookup and the on-disk fingerprint and so must be a mapping every server reproduces. |
| `src/bm25_sb_wordclass.c` | **#295** (GENERATED by `ci/gen_sb_wordclass.py`; never edit by hand) `bm25_sb_wordclass(encoding)`: a 16-byte bitmap per single-byte server encoding saying which high bytes (0x80-0xFF) are letters or digits, i.e. word bytes for `bm25_is_word_byte`. Pinned to PG 18.6's conversion maps and its `pg_u_isalnum` at Unicode 16.0 so run splitting cannot drift with a server's Unicode tables across majors; unmapped bytes are separators; `SQL_ASCII` has no table (all high bytes are word bytes). Regenerating with different bytes is an analyzer revision bump. The generator's `--check FILE` mode verifies the committed file against a PG source tree; CI does not run it (CI has no PG source, and regenerating from a moving tree is exactly the drift being prevented). |
| `src/bm25_merge.c` | Tiered merge engine: pure size-layered selection policy (`bm25_debug_merge_plan`) + the two-phase atomic catalog-swap executor. `bm25_merge_select` (`bm25_merge.c`) fires on any of three triggers, not one: (a) any segment >= 15% tombstoned (`BM25_MERGE_TOMBSTONE_FRAC`), given >= 2 segments total; (b) a size layer — `floor(log_FANOUT(ndocs))`, `BM25_MERGE_LAYER_FANOUT = 4` — holding >= 4 same-layer segments; (c) only if neither (a) nor (b) chose anything, `nsegs > BM25_TARGET_SEGMENT_COUNT` (8), which merges just the two smallest segments (by `ndocs`) to make progress. So a fixture with no tombstones and <= 8 segments spread across mixed layers never merges — it needs >= 4 segments landing in the SAME size layer (or a tombstone rewrite, or > 8 segments total) to exercise a real merge rather than a seal. **#146 (BUILD-04):** the re-accumulation is cut at `bm25_maintenance_budget_bytes` — at INPUT-SEGMENT boundaries only, since postings stream per term across docs and a mid-segment cut would put one TID in two outputs — and ALL outputs are published, with ALL inputs retired, in the ONE swap record. A byte estimator (`bm25_accum_estimate_bytes`) plus a selection trim keeps only sets whose merge can reduce the segment count, and `bm25_merge_execute` returns PROGRESS rather than "merged something", which is what stops `bm25_merge()`'s force loop spinning on a rung it can no longer consolidate (`bm25_debug_merge_budget_plan` exposes both). Driven by both manual `bm25_merge()` and the vacuum-cadence path in `bm25_vacuumcleanup`. **M5:** the merge re-accumulation is field-aware (per-field doclen/`field_id`) and reads each surviving doc's key from the source keymap so the merged segment rebuilds its per-field postings AND keymap (else the first merge would revert to flat/ctid). **M4:** position support is discovered from the chosen segments (all must agree); a `merge_post_cb`/`merge_pos_cb` pair replays each surviving posting's positions into the accumulator (`bm25_accum_add_positions_to_last`) so the merged segment re-emits its POS chain — a source whose `pos_root` is Invalid fires no `pos_cb`. |
| `src/bm25_keymap.c` | **M5** `key_field` docid→key map (`BM25_PAGE_KEYMAP`): the writer (`bm25_keymap_write`, an orphan chain built inside `bm25_segment_build_orphans`, whole keys packed per page — never split across a page boundary; **#145 (`docs/adr/0083`)**: its continuation path now FINISHES the tail's WAL record before calling `bm25_page_alloc`, then writes link + init in one small follow-up record — it used to allocate inside the open window, which its own header comment claimed it did not), the one-shot reader `bm25_seg_key` (walks from the root and captures the extent on every call; the per-segment reader form, resuming from a cached `BM25SegReader`'s KEYMAP cursor, is `bm25_seg_reader_key`, added by issue #225 / `docs/adr/0095`'s addendum), `bm25_key_extract` (Datum → fixed-width bytes per `BM25_KEY_*` type; text truncates to ≤16 B; a NULL key → zero sentinel, projected as key 0 — there is no null flag, so NULL and a genuine 0 are indistinguishable, "unspecified identity"; ctid fallback is for a keyless index only), and `bm25_debug_seg_keymap`. **#246 item 1 (`docs/adr/0100` addendum):** `bm25_seg_key_cache_fill` resolves a ranked build's keys: rows of a segment whose KEYMAP spans several pages are sorted by (segment, docid), resolved in one forward pass and scattered back to their rank slots; rows of a one-page KEYMAP are resolved in rank order, since sorting them cost more than it saved. Both ranked-row finalizers go through it. **#267 (`docs/adr/0100` addendum):** the cache's per-segment readers keep a KEYMAP page image (`bm25_seg_reader_cache_pages`), for at most `BM25_KEYCACHE_MAX_IMAGES` (64) readers; the residual that a key is still resolved for EVERY ranked row is recorded in this function's header. |
| `src/bm25_upgrade.c` | **Roadmap #4** (new module) `bm25_upgrade(regclass)`: seals pending, re-reads and gate-validates the metapage, then consults a static transform registry ONCE for the index's stamped generation (`bm25_upgrade_transform_matches`). That registry is today a `{from_gen, needs_segment_rewrite}` FLAG and nothing more — the former `to_gen` field and `rewrite_segment` function pointer are GONE (`to_gen` was never read; `rewrite_segment` was only ever NULL-tested as a sentinel, never invoked, so a registered entry would have silently not run it). It holds no real entries — every gap this build accepts is additive/identity — and a `StaticAssertDecl` on its length breaks the build if one is added, forcing the author to carry `min_read_version` and actually dispatch a per-segment transform (nothing calls one today) first. No match ⇒ identity path: re-stamp `format_version`/`min_read_version`/`feature_flags` as a single standalone Generic WAL record. A match ⇒ the WHOLE index is re-emitted via `bm25_merge_rewrite_all` (`bm25_merge.c`), which folds the re-stamp into the SAME record as the atomic catalog-swap — never a separate write, so a crash cannot strand new-format segments under an old-format metapage. Refused on a standby with 25006 by the owned gate it opens through (`bm25_index_open_owned`, #307; it had its own 55000 check before) and idempotent (already-current is a no-op `NOTICE`). `bm25_debug_enable_synthetic_transform` routes a test through the rewrite path with no real transform registered, proving the plumbing ahead of the first real breaking change. |
| `src/bm25_meta.c` | Metapage (block 0) lifecycle + Generic WAL page helpers: `bm25_meta_init/read/write/read_locked`, the extracted gate `bm25_meta_validate` (two-directional: forward `min_read_version > BM25_FORMAT_VERSION` → "requires extension format >= N"; backward `format_version < BM25_OLDEST_READABLE` → "reads >= N", REINDEX hint), `bm25_page_alloc`, `bm25_page_init`, `bm25_next_gen`, `bm25_buildempty` (also stamps **`BM25_FEAT_SEGCAT_TOKENS`**, since an unlogged index rebuilt from this fork after a crash is exactly as counter-aware as one built by `ambuild`), `bm25_derive_feature_flags` (deliberately NEVER produces that bit — it is a property of the WRITER, not of content, so `bm25_upgrade`'s re-derive-on-existing-index call cannot honestly set it; ADR 0088), **`bm25_meta_set_pd_lower`** (the one chokepoint EVERY metapage writer routes through — it RAISES `pd_lower` to `Max(current, end-of-struct)`; assigning would let `GenericXLog` zero a newer binary's appended tail as page hole, breaking the additive contract), the `bm25_stats` SRF (13 columns), and the test-only levers `bm25_debug_stamp_version` (pokes all three negotiation fields) / `bm25_debug_write_optional_region` + `bm25_debug_check_optional_region` (write and verify a synthetic v7-style tail — the discriminating probe for the `pd_lower`-raise invariant). The metapage carries `analyzer_fingerprint`, `field_config_blkno`, `field_count`, and (v6) `min_read_version`/`feature_flags`; `bm25_stats` surfaces all of them. Single source of truth for LSM directory state. **#302/#313:** `bm25_meta_validate` refuses block 0 in all five metapage block pointers (a `segcat_root` of 0 made the catalog appender wait on the metapage lock it already held, uncancellably); `bm25_next_gen_check` guards both `next_gen` draw sites. **The two SQL gates** live here: `bm25_index_open_owned` (relkind, then ownership, then `PreventCommandDuringRecovery` = 25006 on a standby, then AM identity; every writing entry point) and `bm25_index_open_readable` (relkind, table SELECT, then 42501 when row-level security applies to the caller on the leaf heap; `bm25_stats` and every read-only debug SRF). `test/check_owned_gate_coverage.py` keeps `sql/63` listing every owned-gate caller. |
| `src/bm25_accum.c` | In-memory drain accumulator for seal/merge: assigns dense local doc-ids (0..N−1) in TID order, accumulates per-term sorted postings, sorts terms lexicographically for the builder. **#146 (BUILD-04/PEND-12):** bounded at last — `bm25_maintenance_budget_bytes` (`autovacuum_work_mem` in an AV worker when set, else `maintenance_work_mem`, else the `bm25_native.debug_budget` test lever) and `bm25_accum_over_budget`, which measures `MemoryContextMemAllocated(recurse=true)` over the one context everything lives in, minus a baseline captured before the first document. The four corpus-scaling growth sites use `repalloc_huge`: the budget makes the 1 GB `MaxAllocSize` cliff unreachable BELOW 1 GB budgets, the HUGE flag makes it unreachable above them, and `maintenance_work_mem` legally exceeds 1 GB. **M4:** each `(doc, field)` posting retains its token position list (no longer discarded); `bm25_accum_add_positions_to_last` attaches a replayed position list to the just-added posting on the merge path. **`bm25_accum_term_postings` returns REUSED per-accumulator scratch, not fresh arrays — valid only until the next call on the same accumulator** (`docs/adr/0044`); both merge-replay entry points validate `field_id` against `field_count` with a real `ERRCODE_INDEX_CORRUPTED` error rather than an Assert, since the id is decoded off a source segment. **ADR 0088:** a `total_tokens` counter, sibling to `total_len_by_field` but summing tokens rather than runs, fed at both ingest shapes — `bm25_accum_add_field_tokens` adds `ntok`, `bm25_accum_add_posting` adds `tf` (counted BEFORE its repeat-merge early return, so a `tf` folded into an existing posting is not lost) — and sealed into the segment header. `bm25_accum_estimate_bytes` now charges its postings and positions terms from `Max(stored_tokens, total_len)` -- the clamp, not an unconditional switch, is what makes a missing, untrusted or saturated count degrade to the run count instead of under-charging. **#305 (`docs/adr/0125`, supersedes 0076):** the term map keys on `(pointer, length)` over the whole term (`HASH_FUNCTION \| HASH_COMPARE`, the pointer re-aimed at the `AccumTerm`'s own copy right after `HASH_ENTER`), so there is no collision fallback; `bm25_accum_sort` leaves every entry's `termidx` stale, so `a->frozen` makes a sorted accumulator refuse further term inserts. |
| `src/bm25_pending.c` | GIN-fastupdate-style WAL-logged pending list: `bm25_pending_append`, `bm25_pending_drain`, `bm25_pending_truncate`, the seal core `bm25_seal_index`, in-page iterator, `bm25_pending_mark_dead`. **#146 (BUILD-04):** the drain cuts at the memory budget too, at a PAGE boundary and never under a content lock; a document mid-continuation has contributed nothing to the accumulator yet (its parts are buffered in `DrainDoc`'s own context) so it lands whole in the next chunk, and the keymeta learned from the first keyed header is carried across chunks. All chunks publish in the ONE record that also detaches the chain. Owns `bm25_seal_threshold_kb`. **M4:** each `BM25PendingTermEntry` carries a delta-varbyte position blob (`pos_bytes`) after its term bytes, so an inserted-then-sealed doc gets true positions (not a positionless placeholder). **#57 (`docs/adr/0038`, format v7):** a document's pending records used to have to fit ONE page (~380 distinct 7-byte stems at the default BLCKSZ) while `ambuild`/`REINDEX` had no page budget, so a rebuild accepted rows INSERT refused. A document too large for a page is now written as several CONSECUTIVE records carrying the same TID, later ones flagged `BM25_PENDING_DOC_CONT` (the flag lives in pre-existing `MAXALIGN` slack, so `sizeof` 36->40 leaves the 40-byte stride and every term-entry offset unchanged; a `StaticAssertDecl` pins that). Parts need only be consecutive, not to own pages — the metapage buffer is held EXCLUSIVE across the whole append. Atomicity comes from the TRANSACTION, not the WAL: a crash mid-append leaves postings for an uncommitted TID, which is the aborted-insert orphan case the pending list already tolerates. **Every walker that aggregates per DOCUMENT had to learn about it** — the drain (`add_field_tokens` ASSIGNS `doclen_by_field` and bumps `ndocs_by_field`, so it must be called once per (doc,field), which needs a part buffer with COPIED term bytes), `pending_global_stats`, `pending_stats_by_field` and `bm25_pending_score_term`; all four produced silently wrong scores, not errors. Entry-walking collectors (`@@@`, AND mask, phrase stash, wildcard) are correct unchanged. **#195:** `pending_df` / `pending_df_by_field` are the awkward pair — entry-walking, but aggregating per DOCUMENT. v7 spanning alone left them correct (a document's entries are PARTITIONED across its parts, so each `(field, term)` entry lives in exactly one part; `pending_df` is additionally called only when `field_count == 1`, where that means each TERM does too), and a STRANDED continuation did not: they counted the orphan as a document containing each of its terms, inflating `df` while `pending_global_stats` left `N` alone. They now carry the drain's tid-matching guard. Note the guard, not the cheaper "skip every `BM25_PENDING_DOC_CONT`" — measured on such a build, a term living only in a legitimate continuation lost its `df` entirely and `sql/77`'s spanning phrase stopped matching. **#184 (`docs/adr/0086`, format v8):** per-field doclen is no longer RECONSTRUCTED by the readers as a sum of `tf` -- the record stores it, as a `uint32 doclen_by_field[]` array between the doc header and the term entries, announced by `BM25_PENDING_DOC_FIELDLENS` in the header's own `flags` (`nfieldlens` reuses the 2 bytes of pre-existing `tid` padding, so `sizeof` stays 40 and a second `StaticAssertDecl` pins that). Sum-of-`tf` is a TOKEN count; doclen is a RUN count (`bm25_accum_add_field_tokens`), and they part company the moment an analyzer emits several lexemes for one run -- at which point a pending document would score differently from its own sealed self. Self-describing rather than keyed on the index's stamped version, so a list holding both shapes (records written before this binary attached, or appended by a concurrent inserter inside `bm25_upgrade`'s seal-then-re-stamp window) reads correctly either way; `bm25_pending_doc_entries_off` is the ONE stride rule and every walker uses it. `min_read_version` rises to 7 LAZILY in the record that lands the first continuation, and to 8 LAZILY in the record that lands the first doclen array -- so an index that is only ever built (`CREATE INDEX` writes no pending records) keeps its old floor and stays readable by an older binary. Residual loud limits: ~65k tokens/doc (16-bit `tf`, also what keeps H7's wrap unreachable) and one (field,term) entry must fit a page. **#226:** `bm25_debug_seal_unpublished` (TEST-ONLY) runs a seal's BUILD phase — drains the pending list into orphan segment pages exactly as `bm25_seal_pending_locked` does — and stops before the one record that publishes them, discarding the catalog entries instead. This is the on-disk state a real seal/merge leaves if the server dies between the orphan build and its publish record (the two-phase install, `docs/adr/0004`), which the crash TAP suites could not otherwise produce, for two separate reasons: a harness cannot stop a backend mid-C-function, so firing the operation asynchronously and then calling `stop('immediate')` loses it outright — the maintenance call's own commit is unflushed (see the Generic-WAL/no-xid landmine below); and a bare `SELECT txid_current()` does not fix that, because its own commit cannot be relied on to flush either (see the Generic-WAL/no-xid landmine below). Takes the seal singleton like a real seal, so it does not race an append, and leaves the pending anchor/catalog/stats untouched — a later real seal still publishes the same documents normally. **#243 (`docs/adr/0095` addendum):** `bm25_pending_mark_dead` (VACUUM's sweep, which holds the singleton in ShareLock mode, so appenders extend the relation while it walks) and `bm25_debug_pending_nth_page` bound their walk with `bm25_blk_in_extent`, which re-samples the extent once before calling a link out of range; `bm25_pending_drain` runs under the ExclusiveLock singleton and keeps the plain sampled bound. The test-only `bm25_debug_pending_sweep(index, nblocks)` runs the real sweep with a caller-chosen (stale) sample. |
| `src/bm25_seg_build.c` | Varbyte (LEB128) codec + block encoder + the two-phase `bm25_segment_build_and_commit` (build orphan chains, then ONE metapage-flip record). **M4:** a parallel `BM25_PAGE_POS` orphan-chain writer emits one `[tf-count][Δpos × tf]` frame per position-bearing posting in lockstep with the POST writer, stamping `dict.pos_post_root`/`pos_post_off` and `header.pos_root` (Invalid when `store_positions` is off for every field). Δpos reuse the varbyte encoder; the POS chain is orphan-built (D14) so it adds ZERO buffers to the 4-buffer publish window. **Issue #289:** the POST slicing loop ends each block at a DOCUMENT boundary (97..128 postings, never more — no format change), so one document's postings for a term never straddle two blocks; `StaticAssertDecl(BM25_MAX_FIELDS <= BM25_POSTINGS_PER_BLOCK)` pins that a document always fits. Older segments still straddle, so the WAND reader handles straddles regardless. |
| `src/bm25_seg_read.c` | Segment reader: `bm25_scan_snapshot` (Model-A catalog copy), segment header read, `bm25_seg_page_validate_kind` (seg_gen AND page-kind check; the 2-arg `bm25_seg_page_validate` is the `want_kind == 0` wrapper) and the other page/offset/block-number/content-bytes/extent validators with the probes that wrap them, and the LIVEDOCS bitmap code (`bm25_livedocs_locate`/`_clear`, `seg_livedocs_all_set`, `bm25_seg_reader_init_checked`/`_known`, sharing the static inline `bm25_livedocs_bits_per_page`) — see the `#228` note below for what split OFF this file to the three rows after it. **#137 (`docs/adr/0062`):** gen answers WHICH SEGMENT and cannot answer WHICH CHAIN inside it -- every page of a segment carries the same gen -- so each call site now passes the kind it expects. The gen check runs FIRST, deliberately: a page reclaimed and re-inited as another kind fails both, and kind-first would report that benign race as non-retryable `ERRCODE_INDEX_CORRUPTED` and break `bm25_scan_build_ranking`'s retry. **#139 (`docs/adr/0063`):** `bm25_seg_scan_postings` and the POS cursor now COPY the page (`palloc(BLCKSZ)`) and unlock BEFORE decoding -- `PosCursor` holds no buffer at all between calls. A content lock is an LWLock and `LWLockAcquire` holds off interrupts, so the loop's `CHECK_FOR_INTERRUPTS` (and `chain_read_at`'s, killed separately by the POST lock) were dead on every phrase query. Both are live now, with the interrupt COUNT unchanged -- which is why only a latency harness can see this fix. **Created once (Task 5), only modified after** (INV-SINGLE-CHAIN-OWNER). **M4:** `bm25_seg_scan_postings` takes a nullable `pos_cb` + the term's `pos_post_root`/`pos_post_off`; when non-NULL it opens a second cursor and decodes one frame per posting in lockstep, self-checking the frame's decoded `tf-count == tf` (a mismatch is a hard ERROR — stream desync). A bag-of-words caller passes `pos_cb == NULL` so the POS chain is never faulted. `bm25_debug_seg_positions` dumps the round-trip. **M6:** `bm25_dict_expand_wildcard` — a wildcard leaf's prefix range-scan + `*`-glob over the byte-sorted STEMMED dict, per sealed segment + pending, deduped into ONE distinct term set (a term in N segments must score once under the leaf's single bit); the pattern is matched RAW-lowercased, NOT stemmed. **#145 (`docs/adr/0083`):** the expander no longer holds a page lock across `bm25_glob_match`/`bm25_wild_add`. BOTH passes now take a `PGAlignedBlock` page copy under the SHARE lock and decode the copy; the segment pass was moved OFF `bm25_seg_dict_iter_*` and open-codes its own DICT page walk, which is what let it carry the segment's REAL `seg_gen` (the iterator then passed `expected_gen = 0`; since #303 it carries the real gen too) and an extent bound that ERRORs rather than terminating. The unlock/relock pair is NOT used here and must not be: its precondition is the metapage singleton, and the “an ordinary MVCC snapshot holds the horizon back” substitute a draft relied on is void on a hot standby without `hot_standby_feedback`. Same restructure gave `bm25_debug_segterms`' entry loop a live interrupt check. **H16 (`docs/adr/0034`):** the three dense per-docid chain readers (`bm25_seg_doc_is_live`/`docid_to_tid`/`doclen_field`) each re-walked from the chain root PER SCORED POSTING, so scoring was quadratic in segment size (~1.2e8 ReadBuffer+LWLock pairs for one df=500k term on a 1M-doc segment). `BM25SegReader` holds one FORWARD cursor per chain and resumes where the last lookup landed — valid because every hot caller walks docids ascending (D-ACCUM posting order, the merge's `for d in 0..ndocs`, WAND's forward-only skip) and a docid's `field_count` NORMS cells are contiguous. A cursor is a pure optimization: it supplies only a start point, restarts from the root on a backward jump, and every page READ is still `seg_gen`-validated. The one-shot public functions keep their signatures (they pass a NULL cursor), so only the three hot loops changed. **SEGREAD-11/14 (`docs/adr/0095`):** the four walkers still missing an extent bound -- `bm25_seg_dict_lookup`, `bm25_seg_dict_iter_next`, `chain_read_at`, `bm25_seg_scan_postings` -- now validate each block against `RelationGetNumberOfBlocks` before `ReadBuffer` and ERROR `ERRCODE_INDEX_CORRUPTED` rather than under-answering; `bm25_seg_scan_postings` also gained a `post_root` validity check it had lacked, closing a silent zero-postings hole. `BM25ChainCursor` gained `root`, so a resume against the wrong chain falls back to a fresh walk instead of reading a foreign chain at a meaningless offset. Giving `chain_read_at` a per-call extent capture would have cost an `lseek` per posting (the ADR 0071 clause already declined once), so the six remaining per-posting callers with no reader -- the WAND driver's per-candidate resolve, the `@@@` TID collector, the BM25F df pass, the phrase and AND-fallback callbacks, and VACUUM's per-document tombstone sweep -- were each given a `BM25SegReader` instead, extending the H16 pattern rather than reopening its cost tradeoff. Measured on a 300k-doc fully-cached index: WAND top-10 487ms to 50ms, `@@@` membership 479ms to 23ms, VACUUM over a 5%-deleted index 1774ms to 168ms, with WAND-shape buffer hits down from 6.65M to 591k; all three trees still return bit-identical scores and TIDs (`43_wand_parity`). **#228 (`docs/adr/0101`):** split four ways as a pure move. What stayed: `bm25_scan_snapshot`, `bm25_segcat_read`, `bm25_segcat_read_locked` and `bm25_segcat_first_entry` (both added by #270, below), `bm25_segcat_find_entry` (test/debug-only: its one caller is `bm25_debug_segcat_walk`), `bm25_segcat_locate_entry`, and their probes `bm25_debug_segcat_walk`/`bm25_debug_seg_lenfields` — all still defined in THIS file. Everything the paragraphs above describe as belonging to `bm25_seg_scan_postings`/the `PosCursor` POS cursor/`chain_read_at` (the #139 and M4 paragraphs) moved to `bm25_seg_chain.c`; the DICT lookup and the wildcard expander (the M6 paragraph) moved to `bm25_seg_dict.c`; the per-docid chain readers (H16: `bm25_seg_doc_is_live`/`_docid_to_tid`/`_doclen_field`, plus `bm25_seg_reader_init` and its doc/tid/doclen accessors) are in `bm25_seg_chain.c` (`bm25_seg_reader_key` is in `bm25_keymap.c`); the extent-bound history (SEGREAD-11/14) now spans `bm25_seg_chain.c` and `bm25_seg_dict.c`, with its validator `bm25_seg_chain_extent_validate` still here, its SEGREAD-11 probes (`bm25_debug_seg_chain_extent`, `bm25_debug_seg_postings_count`) in `bm25_seg_debug.c`, and its SEGREAD-14 probe (`bm25_debug_chain_cursor_crosstalk`) in `bm25_seg_chain.c`; and the probes `bm25_debug_seg_positions` (M4 paragraph) and `bm25_debug_segterms` (M6 paragraph) moved to `bm25_seg_debug.c`. **#244 (`docs/adr/0095` addendum):** the four SEGCAT walkers (`bm25_scan_snapshot`, `bm25_segcat_read`, `bm25_segcat_find_entry`, `bm25_segcat_locate_entry`) validate link SHAPE as well as extent (page kind, a zero-entry page that links onward, an `nsegs + 1` visit cap before `ReadBuffer`, and, for the two copying walkers, no generation listed twice) and raise `ERRCODE_INDEX_CORRUPTED`; the extent test is the exported `bm25_blk_in_extent` (formerly `segcat_blk_in_extent`), which the pending walkers in `bm25_pending.c` share. See the SEGCAT landmines. **#270 (`docs/adr/0107`):** `bm25_segcat_read_locked` is the form production callers use and asserts the metapage singleton under cassert; `bm25_segcat_first_entry` copies catalog entry 0 under the metapage SHARE (the INSERT key check's source) and applies the extent re-sample, the page-kind check and the empty-page-that-links-onward check to that one page. |
| `src/bm25_seg_dict.c` | **`docs/adr/0101`** (#228, split from `bm25_seg_read.c`) the DICT reader: `bm25_seg_dict_lookup`, the `BM25DictIter` merge-feed iterator (`bm25_seg_dict_iter_*`), `bm25_dict_expand_wildcard` with its private dedup table, `bm25_dictentry_validate` and its probe. |
| `src/bm25_seg_chain.c` | **`docs/adr/0101`** (#228, split from `bm25_seg_read.c`) the chain readers: `bm25_seg_scan_postings` with the `PosCursor` POS cursor, `bm25_seg_block_header_read` (and `bm25_seg_block_header_read_lead`, which also decodes the block's first document's postings via `bm25_block_lead_decode`, issue #289), `chain_read_at` and `bm25_chain_span_validate`, the one-shot per-docid lookups, and `BM25SegReader` (`bm25_seg_reader_init` and accessors). Keeps the two probes that call its statics (`bm25_debug_chain_span_validate`, `bm25_debug_chain_cursor_crosstalk`). **#267 (`docs/adr/0100` addendum):** the optional per-cursor page image on `BM25ChainCursor` (`bm25_seg_walk_get`/`bm25_chain_page_keep`, `bm25_seg_reader_cache_pages`/`_release_pages`); see the page-image landmine. **#303:** `BM25SegWalk` (`bm25_seg_walk_init`/`_read`/`_get`/`_check`), the validated reader every sealed-chain walk in the tree reads through -- block 0, extent, revisit (first page, previous page, chain root), visit cap, content bytes, then gen (a mismatch goes to `bm25_seg_gen_mismatch`: XX002 while the segment is live, 40001 once it has left the catalog) and kind; its header comment lists the per-family cycle invariants and the residuals. **#293/#294:** the validated-walker contract for POST, LIVE, DOCMAP and NORMS (see the landmine row of that name); `bm25_chain_full_span` is the per-kind full-page span, and `seg_docid_to_tid_cur` is the single docid -> TID chokepoint that rejects an impossible heap TID. |
| `src/bm25_seg_debug.c` | **`docs/adr/0101`** (#228, split from `bm25_seg_read.c`) the segment-reader debug SRFs that use only the reader's API (catalog, dict, postings, positions, liveness, lengths, and the `bm25_debug_stamp_*` corruption levers). Probes that call a file-static stayed beside it (`bm25_debug_segcat_walk`, `bm25_debug_seg_lenfields` in `bm25_seg_read.c`). |
| `src/bm25_page_lever.c` | The raw corruption lever for the corrupt-pointer suites (#302/#303/#309): `bm25_debug_poke_page` writes arbitrary bytes at an offset of any existing block (metapage included) under a Generic WAL full image, flushed so a poke survives an immediate stop, and refuses only a range outside the page or below `pd_flags`, a result that fails core's page-header check, or bytes in the `pd_lower..pd_upper` hole; `bm25_debug_layout` gives `(struct, field, off, size)` from `offsetof`/`sizeof` so suites never hard-code offsets. Owner-gated and REVOKEd like the stamp levers. |
| `src/bm25_scan.c` | AM lifecycle: `bm25_beginscan`/`bm25_rescan` (+ its RHS parsers)/`bm25_gettuple`/`bm25_endscan`, plus `bm25_load_if_needed` (the `@@@` filter path) with its static TID collector. The ranking itself — the retry wrapper `bm25_scan_build_ranking` (bounded subtransaction retry on a seg_gen abort; on a hot standby one direct attempt with no subtransaction, so a recovery conflict cancels the statement instead of FATALing the session, #307), the seam dispatcher `bm25_scan_build_ranking_once`, and the exhaustive scorer — lives in `bm25_scan_rank.c` (`docs/adr/0101`, #228); this file only calls it. **M2b:** the dispatcher (`bm25_scan_rank.c`) chooses the WAND driver (`bm25_wand_build_ranking`, default) or the (renamed) exhaustive OR-sum scorer `bm25_scan_build_ranking_exhaustive`, multi-source dedupe-by-TID; `bm25_gettuple`'s over-pull tail fallback lazily reruns the wrapper with `force_exhaustive=true` past `wand_top_k`. **M5:** the scorer (`bm25_scan_rank.c`) is BM25F (per-field `idf`/`avgdl`/`boost`/`doclen`, still one pass, still dedupe-by-TID — `m7`: ranked on TID, keymap is output-only); `bm25_rescan_parse_field` parses `field:term` against the baked field-config page (`so->qfield`, via `bm25_field_by_name`), honored on the index scan (`&@@` forces it — NOT via `bm25_match`); each ranked row stashes its user key (`bm25_scan_rank.c`/`bm25_wand.c`, via the per-segment `BM25SegKeyCache`). **M4:** `bm25_rescan_parse_phrase` parses a `"..."` / `"..."~n` / `"..."~>n` phrase (`so->qphrase`/`qslop`/`qphrase_ordered`); the scorer (`bm25_scan_rank.c`) passes the lockstep `pos_cb` (`seg_phrase_pos_cb`, `bm25_scan_match.c`) to stash per-`(TID,field)` positions, then runs the `bm25_phrase.c` matcher as a post-accumulation recheck, dropping non-matching TIDs (filter-only, the WAND seam and `bm25_scan_build_ranking` contract do NOT move; a bare phrase ORs across fields — D6). The D7 degradation gate (`pos_root` Invalid or a `store_positions=false` field ⇒ ERROR, or WARNING + AND-of-terms under `phrase_fallback='and'`) lives in `bm25_scan_build_ranking_exhaustive` (`bm25_scan_rank.c`, both for the text path and, per phrase leaf, the jsonb path) — a DIFFERENT predicate from `bm25_scan_build_ranking_once`'s own WAND gate, which decides WAND-vs-exhaustive, not ERROR-vs-degrade. The D7 predicate itself is shared by the text and jsonb surfaces via `bm25_phrase_fields_have_positions`/`_segments_have_positions`, while the ERROR-vs-degrade POLICY stays per-surface — the jsonb path cannot degrade because `PhraseAndEnt`'s single mask encodes either phrase-term bits or leaf bits, never both. The scan-start prologue (snapshot + fingerprint gate + corpus stats) and the per-term df→idf block are each ONE shared helper — `bm25_scan_corpus_stats` (`bm25_scan_rank.c`, declared in `bm25_scan.h` for `bm25_debug.c`'s sake) and `bm25_term_idf` (in `bm25_stats.c` since `docs/adr/0093`) — rather than five and three hand-copies, which is what makes the fingerprint gate uniform across the scoring and WAND-debug paths (`docs/adr/0045`) and caches `qcfg` for `bm25_snippet`. **M6:** `bm25_rescan_parse_jsonb` parses a jsonb RHS into `so->qtree` (single MATCH/TERM leaf copied to `so->qterm` → text-identical; anything multi-leaf takes the exhaustive scorer, `bm25_qtree_is_multileaf`); the exhaustive scorer (`bm25_scan_rank.c`) generalizes the per-term OR-loop to a flat `BM25TermWork[]` (leaves × tokens), marking each positive leaf's `leaf_bit` in `and_presence` and draining through `bm25_query_eval`; `bm25_load_if_needed` reuses the SAME builder so the filter set == the `&@@` ranked set by construction (D12); `bm25_rescan` resets `so->qtermlen=0` on every rescan so a rebound single-leaf→multi-leaf plan can't `bm25_analyze(NULL, stale_len)`. **`docs/adr/0053`:** the 15 `bm25_debug_*` SRFs that used to sit BETWEEN the dispatcher and the callback lifecycle (1,218 lines, a quarter of the file) moved to `bm25_debug.c`; this file holds zero `PG_FUNCTION_INFO_V1` and the AM's callback flow reads contiguously. **`docs/adr/0093` (#67.13):** what the exhaustive scorer shares with WAND moved down to `bm25_stats.c` — `bm25_term_idf`, `bm25_pending_score_term`, the `BM25AccEnt` accumulator and `bm25_scores_add`, the #62.5 match-set budget, and `bm25_ranked_keys_fill_from_pending`. The exhaustive scorer itself stayed in this file by decision, at the time. **#228 (`docs/adr/0101`):** split three ways as a pure move — the corpus prologue, the exhaustive scorer, the dispatcher and `bm25_scan_build_ranking` moved to `bm25_scan_rank.c`; the phrase stash/recheck and AND fallback moved to `bm25_scan_match.c`. This file now holds only the AM lifecycle (beginscan, rescan and its parsers, gettuple, endscan) and `bm25_load_if_needed` with its static TID collector. |
| `src/bm25_scan_rank.c` | **`docs/adr/0101`** (#228, split from `bm25_scan.c`) the ranked-scan builders: `bm25_scan_corpus_stats` and its pending walkers, `bm25_field_corpus_stats`, `bm25_scan_build_ranking_exhaustive` (`seg_posting_cb`, `TermScoreCtx`, the phrase-position gates), `bm25_scan_build_ranking_once` (D7) and the retry wrapper `bm25_scan_build_ranking`. The only scanner file that calls `bm25_wand_build_ranking`. |
| `src/bm25_scan_match.c` | **`docs/adr/0101`** (#228, split from `bm25_scan.c`) the M4 match filters the exhaustive scorer drives: the phrase position stash (`seg_phrase_pos_cb`, `pending_phrase_stash`), `phrase_recheck_tid`, and the AND-of-terms presence set (`phrase_and_mark`, `seg_and_cb`, `pending_and_stash`). Calls nothing in the other two scanner files. |
| `src/bm25_scan.h` | **`docs/adr/0053`** (new header) the narrow internal seam created by that split. Declares ONLY the symbols a SECOND translation unit genuinely calls, across the three scanner files (`bm25_scan.c`/`bm25_scan_rank.c`/`bm25_scan_match.c`): the two shared scan-start helpers `bm25_scan_corpus_stats`/`bm25_field_corpus_stats` (defined in `bm25_scan_rank.c`; used by both the real scan path and the debug probes — a probe is only a REFERENCE if it runs the code the scan runs, which is what `sql/43_wand_parity` depends on), the five phrase-stash/AND-presence types and the prototypes of the six `bm25_scan_match.c` functions `bm25_scan_rank.c` calls, and `bm25_qtree_is_multileaf` as a static inline (read by `bm25_scan.c` and `bm25_scan_rank.c`). Included by the three scanner files and `bm25_debug.c`. **`docs/adr/0093`:** `bm25_term_idf` was declared here too until it moved to `bm25_stats.c`, and the two `bm25_wand_*` stat helpers that `docs/adr/0053` had relocated here from `bm25_wand.h` are now statics in `bm25_wand.c`. **#188:** `bm25_fingerprint_gate` was another scan-start helper here until it gained a write-path caller (`bm25_insert`) and moved to `src/bm25_analyzer.c`, declared in `bm25.h` — a predicate both the read and write paths run does not belong behind a scan-private header, and `bm25_build.c` would otherwise have had to include this file. The narrowness is the point: each entry costs a `static` the compiler can no longer prove is unexported, so anything used solely inside one scanner file stays static there and anything used solely by the probes lives in `bm25_debug.c`. **#228 (`docs/adr/0101`):** this narrowness rule was extended to all three scanner files when `bm25_scan.c` split. |
| `src/bm25_stats.c` | **`docs/adr/0093`** (#67.13, new module) the statistics layer both ranking builders stand on: `bm25_term_idf` (per-term df→idf over segments + pending, with its static walkers `pending_df`/`pending_df_by_field`/`field_df_cb`); `bm25_pending_score_term` (the pending arm of scoring — the SAME code on the exhaustive and WAND paths is what keeps them bit-identical, D8); the TID-keyed accumulator upsert `bm25_scores_add`; the #62.5 match-set budget (`bm25_match_budget_init`/`bm25_match_charge`, ADR 0047); and `bm25_ranked_keys_fill_from_pending`. It exists to remove the WAND driver's call back into the scanner. Nothing here calls into the scanner (`bm25_scan.c`, `bm25_scan_rank.c`, `bm25_scan_match.c` since `docs/adr/0101`) or `bm25_wand.c`. CI's "Stats-layer call direction" step enforces this on the object files: it fails if any undefined symbol in `bm25_stats.o` is defined in the three scanner objects or `bm25_wand.o`, or if `bm25_wand.o` uses any symbol a scanner object defines. The two WAND adapters over this layer stay in `bm25_wand.c` because they call `bm25_wand_ctx_build`/`bm25_topk_offer`. The scan-start corpus prologue (`bm25_scan_corpus_stats` and its pending walkers) did not move. Not the home of the `bm25_stats()` SQL function, which is in `bm25_meta.c`. **#246 item 3 (`docs/adr/0100` addendum):** `bm25_term_idf` optionally records each (term, segment) dictionary lookup (`BM25TermSegLoc`: found, postings root and offset, df, POS root and offset), and the WAND driver and the exhaustive scorer read those instead of walking the dictionary again; both run on the snapshot the df pass used, and a segment holding no query term is skipped before its header is read. |
| `src/bm25_stats.h` | Interface of `bm25_stats.c`: `BM25AccEnt` (the accumulator entry), `BM25MatchBudget`, `BM25_HASH_ENTRY_OVERHEAD`, `BM25ExhScored` (the exhaustive scorer's sort element, declared here only because the per-document budget charge includes one slot of it) and the six functions above. Declares only what a second file calls. |
| `src/bm25_debug.c` | **`docs/adr/0053`** (new module) the SQL-callable debug/introspection surface: all 15 `bm25_debug_*` SRFs formerly interleaved with the scan callbacks (`_rank`, `_wand_rank`, `_rank_key`, `_field_stats`, `_global_stats`, `_block_ub`, `_term_contrib`, `_topk`, `_cursor_scan`, `_cursor_skip`, `_fingerprint_gate`, `_query_parse`, `_query_flatten`, `_glob_match`, plus `bm25_wand_stats` — a debug probe despite the name), their private helpers (`wand_debug_ctx_and_block`, `term_contrib_debug_cb`, the four `bm25_query_render*` AST printers), and `bm25_global_stats`. Split out because a quarter of `bm25_scan.c` being test-only code is exactly where the duplicated stat prologues PR-E consolidated had hidden. Purely a code move — no behaviour changed, and the proof is that `43_wand_parity`'s pre-existing expected output is byte-identical. Every SRF here is `REVOKE`d from `PUBLIC` in the extension script -- including `bm25_wand_stats`, which it was NOT until #149: the loop matched `bm25\_debug\_%` and that name does not carry the prefix, so the one debug probe in this file named differently was the one that shipped PUBLIC-executable. The loop is now an ALLOWLIST of deliberately-public names, so adding an SRF here means doing nothing at all and adding a public function means an explicit, reviewable entry (`docs/adr/0075`). `sql/63_debug_privileges` pins the resulting public surface by name rather than re-running the script's own predicate, which is what let the blind spot survive a suite named for it. |
| `src/bm25_score.c` | BM25 math: `bm25_idf` (Lucene "+1" variant, pure — the caller passes per-field N/df), `bm25_termscore` (pure six-arg); `bm25_distance` (`&@@`, resolves by ORDER BY query identity against the registry — ADR 0061 — and among same-query sibling scans by emit recency, `docs/adr/0104`); `bm25_score(tid)` + **M5** `bm25_score_key(key)` (score-by-user-key) accessors. **R3:** hosts the active-scored-scan REGISTRY (`bm25_register_scored_scan`/`_deregister_scored_scan`/`_scored_scan_head`/`_sole_scored_scan`, replacing the old single `bm25_active_scored_scan` pointer; the `bm25_scored_scan_count` that once sat beside them is gone, its raw count test replaced by `bm25_sole_live_scored_scan`, `docs/adr/0064`; since #301, `docs/adr/0115`, every walker sees only the scans whose `owner_userid` is the caller's current user id and the row-addressed accessors also apply `bm25_probe_acl_compute`) and the row-IDENTITY resolvers `bm25_resolve_score_tid`/`_key` (current-row O(1) hot path, then a lazily-built per-scan hash — `bm25_build_score_index` — for the decoupled/arbitrary-id fallback when exactly one scan is active). See "Score accessor concurrency (R3)". **#253 (`docs/adr/0105`):** the query-qualified overloads `bm25_score(tid, query [, regclass])` and `bm25_score_key(key, query)` (`bm25_score_query`/`_jsonb`, `bm25_score_key_query`/`_jsonb`) resolve through `bm25_resolve_by_query`, which asks each scan ranking the byte-identical query (`bm25_scan_ranks_query`, shared with the `&@@` distance projection) whether its whole-ranking hash holds the row, instead of reading current rows. |
| `src/bm25_wand.c` | **M2b** (new module) the block-max WAND engine: `bm25_block_ub` (the safe per-block upper bound, over live scan-time idf/avgdl + the block's raw impact table); `BM25TopK` (the bounded top-k min-heap, drain order pinned to the exhaustive comparator); `BM25WandCursor` (per-term per-segment pull cursor — `next`/`next_geq` header-only block skip/`score_doc`, bit-exact to `bm25_scan_rank.c:seg_posting_cb`); and the driver `bm25_wand_build_ranking` (per-segment Ding–Suel block-max WAND under one shared global top-k heap, pending scored first exhaustively to prime θ). Calls back into the segment reader (`seg_chain` for header-only block reads and liveness, `seg_read` for reader init and page validation, `seg_dict` for lookup) and `keymap` for key projection. Its per-term idf and pending arm come from `bm25_stats.c` through two static adapters, `bm25_wand_prepare_terms`/`bm25_wand_score_pending` (moved here from `bm25_scan.c` by `docs/adr/0093`); it calls nothing in `bm25_scan.c`. |
| `src/bm25_query.c` | **M6** (new module) the jsonb query-tree engine, PURE (no buffer access): `bm25_query_parse` (jsonb object → `BM25Query`, validation ERRORs — unknown field, a key outside the node kind's closed key set, fractional `slop`, more than `BM25_QUERY_MAX_NODES`=1024 nodes, `must_not`-only, non-positive `boost.weight`), `bm25_query_flatten` (ONE DFS assigning `leaf_bit` from a single 0.. counter over ALL leaves, folding each `boost` weight as a PRODUCT into its enclosing leaves' boost, propagating `negated` down `must_not` edges, ERROR if leaves > 64 — since #68 the SAME cap also fires during parse (`bm25_query_count_leaf`, from `parse_leaf`), so `bm25_debug_query_parse` is bounded too and flatten's own check cannot currently fire through any caller, kept only as a guard for a future one (ADR 0099); also ERROR if a POSITIVE leaf's FOLDED product lies outside [`BM25_QUERY_MIN_BOOST`, `BM25_QUERY_MAX_BOOST`] = [1e-6, 1e6], since parse's guard is per-weight and `1e-300 * 1e-300` folds to exactly `0.0` — the state that guard exists to reject, reached by a path it cannot see — and a single extreme weight still under- or overflows `boost*idf`; NOT checked for a NEGATED leaf, whose boost is never read (`bm25_pending_score_term`/`seg_posting_cb` are both `if (!w->negated)`-guarded and presence marking runs through `seg_and_cb`, which ignores idf), ADR 0047), `bm25_query_eval` (must=AND / should=OR-required-only-when-no-must / must_not=AND-NOT, over a uint64 presence mask), `bm25_glob_match`. jsonb VALUES are data, never a parsed DSL (injection-proof). The matching debug SRFs (`bm25_debug_query_parse`/`_flatten`/`_glob_match`) and the `bm25_query_render*` AST printers live in `bm25_debug.c`, deliberately NOT here: this module stays scoped to what the real scan path calls. |
| `src/bm25_phrase.c` | **M4** the pure phrase / proximity matcher (`bm25_phrase_match`): decides whether the phrase's N SLOTS co-occur within one field under the required order/slop. `ordered_match` handles EXACT (slop 0) + ordered PRE/n (strictly increasing, `(p_last−p₁)−(N−1) ≤ slop`); `unordered_match` handles W/n (a distinct-position assignment with window span `≤ (N−1)+slop`). **#184:** a slot is one source WORD, not one lexeme — `bm25_phrase_slot_map` derives the token→slot grouping from the analyzer's positions and `bm25_phrase_merge_lists` folds a slot's members into one deduplicated list. That widening cost `unordered_match` its KEY FACT (different terms' position sets are disjoint): it now CLASSIFIES each call with an O(m) adjacent-equal scan of the sorted tagged stream, runs the old counting sweep when the sets are disjoint, and an exact incremental bipartite-matching sweep (`sdr_sweep`, Hall) when they overlap — the counting sweep answers `S₀={5}, S₁={5,9}, slop 0` with a false positive. `ordered_match` needed no change; strict increase already does the distinctness work. No PostgreSQL dependency — compile-standalone selftest under `-DBM25_PHRASE_SELFTEST` (see its header for the exact command; it carries its own backend stubs), which was the ONLY harness for the overlap paths until analyzer revision 5 (issue #184) landed the co-positioned data that makes them reachable at all: `sql/107_per_run_positions` PART TWO is the first SQL to construct genuinely overlapping (not merely equal) slot sets — an *unordered* (`~n`) phrase over `ispell_sample` — and reach `sdr_sweep`, plus a fuzz-vs-brute-force check. Reused verbatim by the scan recheck. |
| `src/bm25_snippet.c` | **M4** `bm25_snippet(field, start_tag, end_tag, max_num_chars, escape)`: RE-ANALYZES the passed field text (never the stored positions — post-stem ordinals can't map to char spans), marks tokens whose stem ∈ the query stem set (any query tree since #308 TEXT-04: every non-negated leaf's analyzed terms, wildcard patterns globbed against the field's tokens; built once per scan, sorted, probed by bsearch), picks the densest ≤`max_num_chars` window, wraps matched surface spans in the tags with original casing (via each token's `src_off`/`src_len`, D10), and snaps window edges to character boundaries. **#151 (TEXT-04):** that snapping is `snap_left`/`snap_right`'s own continuation-byte test, NOT `pg_mblen`/`IS_HIGHBIT_SET` as this row used to claim -- the character length (`bm25_mblen_bounded`, which replaced the deprecated `pg_mblen` in #308 TEXT-08 and ERRORs on a truncated sequence) is used only by the `max_num_chars` counters and `IS_HIGHBIT_SET` only in a comment. The test `(b & 0xC0) == 0x80` identifies a continuation byte in UTF-8 and nothing else, so it is now gated on `PG_UTF8` rather than on any multibyte encoding (EUC_JP and friends were being walked off a character boundary rather than onto one). **PR-H:** the budget counts CHARACTERS, not the bytes it measured through M4 (`docs/adr/0049` — hit-edge character offsets resolved in one forward pass by `snippet_hit_charpos`, converted back to byte bounds by the forward-only `snippet_char_to_byte`, since a character length is read from a lead byte and no server encoding steps backward; leftover budget a side cannot use carries to the other side, #308 TEXT-06); and the FIELD text is HTML-escaped by default while the caller's TAGS are not (`docs/adr/0048` — deliberately unlike `ts_headline`, because the tag defaults declare HTML as the rendering target; `escape => false` opts out, and both NULL and a pre-argument catalog entry select escaping). The budget measures ORIGINAL characters and escaping expands only the OUTPUT, so the two are independent by construction. **R3:** sources its scan (query terms + analyzer config) from `bm25_sole_scored_scan()` instead of a single slot — FAILS LOUD (`ERRCODE_FEATURE_NOT_SUPPORTED`) if a scan behind the registry head could still emit a future row, since a snippet carries no row identity to disambiguate; returns SQL NULL on NULL field / NULL max_num_chars / no active scan / empty query / zero hits; ERRORs on `max_num_chars <= 0`. |
| `src/bm25_fsm.c` | Page reclamation: `bm25_reclaim_orphans` mark-and-sweep (+ retired-RANGE marking), `mark_chain`, the stamp-and-gate helper `bm25_page_mark_deleted`, and `bm25_reclaim_retired` (horizon-gated per-segment freeing). **M5:** `BM25RetiredEntry` gains `keymap_root` so a merged-away segment's KEYMAP chain is retired + freed (the field-config chain + live-segment keymap were already marked reachable). **M4:** the complete `keymap_root` → `pos_root` mirror — `BM25RetiredEntry.pos_root`, marked reachable at both the live-segment and retired-segment sites, copied in `bm25_retire_segment`, and in `reclaim_one_range`'s `roots[]` — so a merged-away POS chain is retired + freed and never leaks. **Roadmap #4:** the sweep skips (LEAKS, never frees) any page carrying a flag bit outside `BM25_PAGE_ALL_KNOWN` — its root set is hardcoded, so a newer binary's page kind is unreachable out of ignorance, and freeing it would hand a live chain to the next `bm25_page_alloc` (a non-pending orphan's `InvalidFullTransactionId` = immediate reuse); this guard is what makes "a new page type" ADDITIVE per ADR 0009. Test-only `bm25_debug_alloc_unknown_page` / `bm25_debug_page_flags` are the discriminating probe for it. **#135:** a `BM25_PAGE_PENDING` orphan is stamped with a real `ReadNextFullTransactionId()` horizon instead — a drained pending chain reaches the FSM through this sweep whenever the opportunistic `bm25_pending_truncate` is skipped, and pending pages carry `seg_gen = 0` so nothing else catches a stale reader; `bm25_debug_page_retire_xid_valid` is the probe (`sql/89_pending_recycle_horizon`). **#67 (`docs/adr/0019` addendum):** the sweep's reachable set is one BIT per block (`reach_bytes`/`reach_test`/`reach_set`), not a `bool` — a bool per block hit `MaxAllocSize` at ~1G blocks (~8 TB), so VACUUM on a larger index errored and reclaimed nothing; a bit covers the whole `BlockNumber` range in 512 MB. **#302 (`docs/adr/0032`, `0041` addenda):** `reclaim_one_range` validates every page before it stamps it (`reclaim_page_validate`: not block 0, content bounds incl. `pd_special`, the chain's kind, `seg_gen` equal to the descriptor's gen, not already DELETED) and raises XX002 after the descriptor's compaction, so the error fires once and the orphan sweep recovers the rest; every retired-list walk caps visits at `nblocks`, `bm25_reclaim_retired` also refuses a walk back to the head (it restarts per chunk), and `mark_chain` refuses a link to a never-initialized page. The stamp-and-gate allocator `bm25_page_alloc` lives in `bm25_meta.c`. |
| `src/bm25_segment.c` | **Legacy** M0/M1 in-place builder/ops, largely retired; some debug SRFs re-pointed at the catalog. Do not extend; new work goes through `seg_build`/`seg_read`. **#145 (`docs/adr/0083`):** both surviving SRFs (`bm25_debug_terms`, `bm25_debug_postings`) now COPY each DICT page under its SHARE lock and decode the copy — `bm25_debug_postings` used to run an entire nested postings replay, and both used to run a `palloc` + `hash_search(HASH_ENTER)`, per entry with the DICT page still locked. Deliberately NOT converted onto `bm25_seg_dict_iter_*`, whose extent bound ERRORs where a dump stops. **#303:** both read through a quiet `BM25SegWalk` (lazy extent sample, taken after the catalog read) with the DICT order and count checks. |

## On-disk layout

```
block 0  METAPAGE  (BM25MetaPageData)  — the LSM directory
  magic, format_version (the BM25_FORMAT_VERSION this index was BUILT under, or last
                         moved to by bm25_upgrade — NOT necessarily this build's), k1, b
  analyzer_fingerprint  (FNV-1a of the build-time analyzer config; gated at scan start)
  field_config_blkno ────────► FIELDCFG page (per-index, single copy)
  field_count          (the index's key-attribute count, written by bm25_build;
                        bounded 1..BM25_MAX_FIELDS by bm25_meta_validate because
                        every reader uses it as a loop bound. bm25_meta_init's
                        sentinel of 1 is overwritten by the build — it is not a
                        claim that the index has one field)
  pending_head/tail, pending_npages, pending_ndocs
  segcat_root  ──────────────► SEGCAT chain
  ndocs, total_len, nsegs        (global stats cache)
  retired_head ──────────────► RETIRED free-list chain (Phase 4)
  next_gen     (monotonic generation counter, starts at 1: segment gens AND
                pending-chain epochs, #291, so segment gens are not consecutive)
  min_read_version     (v6: the FLOOR — oldest BM25_FORMAT_VERSION that can read
                        this index; a legacy v5 metapage has no such field, so a
                        newer reader sees 0 from the zeroed page tail == "legacy,
                        always readable")
  feature_flags        (v6: bitmap of optional capabilities present.
                        Bits 0-2 (multifield/positions/WAND-impacts) are
                        informational, 0 on a legacy index until bm25_upgrade
                        derives and stamps them. Bit 3, BM25_FEAT_SEGCAT_TOKENS
                        (ADR 0088), is a trust signal the merge estimator
                        consults for the segment catalog's total_tokens field;
                        stamped ONLY by a fresh build, NEVER by bm25_upgrade.
                        No bit is consulted by the readability gate.)

FIELDCFG page   BM25_PAGE_FIELDCFG, seg_gen = 0, written once at CREATE INDEX,
  read-only after: [BM25FieldConfigHeader][BM25FieldConfig × field_count]
  [store_positions[field_count]][BM25KeyStamp]. One BM25FieldConfig per indexed column
  (field_id 0..N-1, field_name = attname, per-field k1/b/boost); M3/single-field writes
  exactly one. The two tails are optional and detected by pd_lower: the flag array
  (M4) and the #292 key-identity stamp (magic, key_type, key_size, key_attno), which
  bm25_insert checks every row against. A page written before #292 has no stamp.
PENDING chain   flat array of BM25PendingDocHeader (+ the doc's key_field value) + N
  BM25PendingTermEntry per doc (each tagged with its field_id)
SEGCAT chain    flat array of BM25SegCatEntry (one per sealed segment; each entry
  carries a tombstone-decayed total_tokens copy since ADR 0088, sibling to total_len)
  each entry.header_blkno ──► SEGMENT HEADER (BM25SegmentHeader): gen, ndocs,
      total_tokens (ADR 0088 — the immutable master the catalog entry's copy decays
      from; sum of tf, a strictly-since-analyzer-revision-5 different quantity from
      the run-counting total_len below), total_len, nterms, field_count (= natts),
      and the chain roots. The serialized page
      is [BM25SegmentHeader][uint64 total_len_by_field[field_count]] and, WHEN
      field_count > 1, [uint64 ndocs_by_field[field_count]] (per-field N for exact
      avgdl_field). Single-field is byte-identical to M3 (no ndocs_by_field written).
      pos_root (M4) is valid iff the index stores positions (some field has
      store_positions=true) — Invalid on a pre-M4 or all-positions-off index;
      keymap_root is valid iff key_field is set.
        dict_root     ─► DICT  chain (BM25DictEntry, sorted by term; df = per-term total
                                over ALL fields; M4 pos_post_root/off point to the term's
                                POS-chain frames, Invalid/0 when positions are off; dict_pad)
        posts_root    ─► POST  chain (ONE segment-wide chain; 128-doc blocks,
                                BM25BlockHeader + varbyte δ-docids + varbyte tf +
                                per-block field-id RLE when field_count > 1 (else
                                field_rle_bytes == 0, byte-identical to M3) + a
                                trailing v5 per-field IMPACT table (impact_bytes,
                                never 0 — see below))
        norms_root    ─► NORMS chain (per-doc length, PER FIELD: doclen[docid*fc + field])
        livedocs_root ─► LIVE  chain (tombstone bitmap; bit set = live)
        docmap_root   ─► DOCMAP chain (local_docid → heap ItemPointerData)
        keymap_root   ─► KEYMAP chain (BM25_PAGE_KEYMAP; packed key[docid], whole keys
                                per page; only when key_field is set — else Invalid)
        pos_root      ─► POS   chain (BM25_PAGE_POS; SEPARATE per-segment chain, one
                                [tf-count][Δpos × tf] frame per position-bearing posting;
                                only when the index stores positions — else Invalid)
```

**v5 block impact table (M2b).** Every POST block trailer carries a `BM25BlockImpact`:
`[uint8 nfields][{uint8 field_id, uint32 max_tf, uint32 min_doclen} × nfields]`
(`BM25BlockHeader.impact_bytes`, never 0 — every block has at least the 1-byte `nfields`
count). This REPLACES the v4 `BM25BlockHeader.max_impact` float4 (dropped, not merely
superseded — the struct layout changed, hence the hard v4→v5 break). The values are RAW
per-field `(max_tf, min_doclen)`, never a baked score: idf/avgdl are scan-time,
index-wide statistics that drift as the index grows (deletes, merges), so a score baked
at seal time would go stale; the WAND scorer (`bm25_block_ub`, `src/bm25_wand.c`)
combines this table with LIVE idf/avgdl at scan time to derive a safe upper bound (see
the "Block-max WAND (M2b)" section). Written by the one segment encoder (`encode_block`,
`src/bm25_seg_build.c`) at both seal and merge — merge re-encodes every surviving block,
so impacts are never carried forward stale across a merge. The v6 gate's backward check
(`format_version < BM25_OLDEST_READABLE`, next paragraph) still rejects any index at
`format_version=4` — a pre-M2b index must be rebuilt before WAND has anything to prune
against; this is unchanged from the old exact-equality gate for that boundary.

**v6 format-negotiation gate (roadmap #4).** `bm25_meta_validate` (`src/bm25_meta.c`,
called from both `bm25_meta_read` and `bm25_scan_snapshot` — i.e. on every scan, not
only build/maintenance entry points) replaced the old exact-equality
`format_version != BM25_FORMAT_VERSION` check with a two-directional **floor** test:
forward, `min_read_version > BM25_FORMAT_VERSION` ERRORs "index requires extension
format >= N" (this binary is too old for a capability the index requires); backward,
`format_version < BM25_OLDEST_READABLE` ERRORs "this build reads >= N" with a REINDEX
hint (this binary has dropped the reader for that generation). Between the two bounds
the index is accepted — a legacy v5 metapage has no `min_read_version`/`feature_flags`
on disk, so a newer reader sees `0`/`0` from the zeroed page tail
(`0 <= BM25_FORMAT_VERSION`, format `5 >= BM25_OLDEST_READABLE(5)`), accepted with **no
REINDEX**. **Neither half of the gate is an equality test**, which is why "the format is
vN" is never a safe thing to say about a given index: an index stamped 5 or 6 is read
without REINDEX by a build whose `BM25_FORMAT_VERSION` is higher, and it keeps its own
older stamp on disk. Going forward: an
**additive** change (a length-prefixed trailing region, a brand-new page type, or a
previously-zero sentinel being filled) leaves `min_read_version` unchanged and needs no
REINDEX; a **breaking** change (an existing field's shape/semantics changing, OR a
*packed multi-record* struct — `BM25DictEntry`, `BM25BlockHeader`, the segcat entry —
gaining a field, since those pack many records per page with no per-record length
prefix and an old reader would desync at the second record) raises `min_read_version`
and is the only case that still needs a hard version bump.

**Two mechanisms MAKE the additive rule true — it is not a documentation-only
promise.** "Additive" means an older binary keeps accepting, reading, *and writing* a
newer index; both of the following are what stop an older binary from destroying newer
data it cannot see, and both are load-bearing enough that removing either silently
corrupts (see Landmines):

- **The orphan sweep never frees an unknown page kind.** `bm25_reclaim_orphans` marks
  from a HARDCODED compile-time root set, so a newer binary's page type is unreachable
  to an older one *out of ignorance*, not because it is dead — and a freed orphan of
  an unknown kind is stamped `BM25_PAGE_DELETED` + `InvalidFullTransactionId`, which
  `bm25_page_alloc` reuses **immediately** (no horizon wait; `BM25_PAGE_PENDING`
  orphans are the one kind that gets a real `retire_xid` instead — issue #135, ADR
  0019's addendum). The sweep therefore skips any page whose
  `flags` carry a bit outside **`BM25_PAGE_ALL_KNOWN`** (`bm25_format.h`, the OR of
  every kind this build knows): it leaks the page rather than freeing it. Adding a
  `BM25_PAGE_*` flag REQUIRES extending that mask.
- **Metapage writers RAISE `pd_lower`, never assign it.** The metapage grows by
  appending struct fields (v3→v4 and v5→v6 both did; neither v7 nor v8 did — both are
  pending-record changes and appended nothing to the metapage — but a future version
  may). An older binary's
  `sizeof(BM25MetaPageData)` ends *below* a newer tail, and `GenericXLogFinish` ZEROES
  `[pd_lower, pd_upper)` on apply — live buffer and WAL redo alike — so an assigning
  writer would destroy the newer fields on the first stats bump. Every metapage writer
  therefore goes through **`bm25_meta_set_pd_lower`** (`Max(current, end-of-struct)`).

`min_read_version >= 5` and
`BM25_OLDEST_READABLE >= 5` are invariant forever (v5's per-block impacts are
mandatory). `feature_flags` never gates readability. Bits 0-2 are informational,
derived by `bm25_derive_feature_flags` when read as `0`. Bit 3
(`BM25_FEAT_SEGCAT_TOKENS`, ADR 0088) narrows that contract without breaking it: it
IS consulted, by the merge estimator, as a trust signal for an optional segment-catalog
field, while staying invisible to `bm25_meta_validate`, which still decides readability
from `format_version`/`min_read_version` alone — and it is deliberately excluded from
`bm25_derive_feature_flags` because it asserts a fact about the writer, not about
content. All four bits are surfaced via `bm25_stats`. A breaking
change gets an online migration path via `bm25_upgrade(regclass)` (seal → gate-validate
→ per-segment transform dispatch — identity today, since the transform registry ships
empty — → atomic merge-swap publish with the version re-stamp folded into the SAME
`GenericXLog` record as the catalog flip); see `docs/adr/0009-format-stability.md` for
the full contract, alternatives, and the rollout procedure (binaries on every node →
`ALTER EXTENSION ... UPDATE` → `bm25_upgrade` on the primary only; the UPDATE step
does nothing until the first `bm25_native--X--Y.sql` script ships). **Released install
scripts are frozen:** `bm25_native--1.0.sql` is pinned by sha256 in
`test/check_packaging_identity.py`, so any SQL change after 1.0.0 goes into a new
`bm25_native--1.0--X.sql` update script, never into the 1.0 script. That release also
adds the script to the Makefile's `DATA`, bumps `default_version` and META.json's
`version`/`provides.version` together (the same packaging check ties them), and adds
every script it ships to `RELEASED_SCRIPTS` (ADR 0009 addendum of 2026-10-06). The
check runs in CI, which is dispatched manually (ADR 0106), so an edit fails the next
CI run, not the commit.

`BM25_PAGE_KEYMAP` (M5) is written when `key_field` is set; `BM25_PAGE_POS` (M4) is
written when the index stores positions (some field's `store_positions=true`). Both are
orphan-built chains outside the 4-buffer publish window; a bag-of-words scan never faults a
POS page (the reader opens the POS cursor only for a phrase query). Every page carries
`BM25PageOpaque` in its
special area: `flags` (page kind),
`nextblk` (intra-chain link), `retire_xid` (set when `BM25_PAGE_DELETED`), and
`seg_gen` (the segment's generation, stamped on every segment page; `0` =
non-segment / skip-validation sentinel). Sealed segment pages are **immutable**
and reachable only through a live `BM25SegCatEntry`. Orphans (crashed-seal
leftovers, drained pending pages) are reclaimed to the FSM by mark-and-sweep.

## Analyzer (M3) and the (fully consumed) reserved surface

The analyzer is baked at CREATE INDEX and never changes without REINDEX. The pipeline
is lowercase → stopword-drop → Snowball stemmer (`ts_lexize` over the dict resolved by
NAME, e.g. `english_stem`), run by `bm25_analyze` identically at build, insert, and
query time. The fingerprint is
`FNV1a32( LE32(stemmer_id) ‖ language_bytes(NUL-terminated) ‖ LE32(stopword_set_hash) ‖ LE32(tokenizer_type) ‖ LE32(analyzer_revision) ‖ LE32(database_encoding) ‖ LE32(probe_hash) )`
(offset basis `0x811c9dc5`, prime `0x01000193`); the concatenation order and the
basis/prime are part of the on-disk contract (compared across machines for replica
equality) — a new component is APPENDED, never inserted. `analyzer_revision`
(`BM25_ANALYZER_REVISION`, now 6) covers the cases the other components cannot: a
change to the analyzer's CODE that alters its output for reloptions that did not
change, or a change to how an existing component's value is ENCODED. Without it,
ADR 0046's byte-wise→character-wise change would have left every pre-0046 index
passing the gate and silently returning nothing for its non-ASCII terms. Bump it on
any change to `bm25_analyze`'s output for fixed reloptions, or on a re-encoding, never
for a pure refactor (a needless bump costs every user a REINDEX). `stemmer_id` is
**not** the dict OID: it is `FNV1a32` over the resolved dictionary's schema-qualified
name plus its template's (`bm25_stemmer_identity`, `docs/adr/0080`), because the OID is
an initdb/OID-counter allocation — recording it made `pg_upgrade` look like an analyzer
change (a logical dump/restore never did: `pg_dump` emits `CREATE INDEX`, so the
restore re-stamps). It was never a *detection* weakness — `pg_ts_dict.oid` is unique,
so a shadowing dictionary was always caught; the OID simply could not tell a different
dictionary from a renumbered one. A name says nothing about **behaviour**, though, and
`pg_upgrade` can change behaviour under an unchanged name: PG18's new Snowball sources
stem six English words differently from PG17 (`added`: `ad` → `add`; `egging`, `erring`,
`offing`). So `probe_hash` (#296, `bm25_analyzer_probe_hash`) hashes, for each word of a
fixed probe list, `bm25_fold_term`'s fold and the resolved dictionary's raw lexize output
(raw-cased input, as `bm25_analyze` passes it; a stoplist drop is zero lexemes; the
`stopwords` reloption is NOT applied — it is component 3). Non-ASCII probes are used only
in UTF-8 databases (no `pg_conversion` dependency; the encoding is component 6). It is
cached per backend by dictionary OID and dropped by a `TSDICTOID` syscache callback, so
the per-row ingest gate stays a hash lookup and an `ALTER TEXT SEARCH DICTIONARY` moves
the fingerprint at the next statement of the same session. Consequences: any change to a
probe word's fold or stem — a 17→18 `pg_upgrade`, an `ALTER … (StopWords = …)`, an ICU/
glibc upgrade that refolds a probe word — trips the gate (REINDEX); a change confined to
words outside the list does not. The probe lists are on-disk contract: editing them is a
revision bump. Tests still assert fingerprint
*determinism/differentiation*, never a pinned integer (component 6 varies with the
database encoding). The fingerprint is stored in
`meta->analyzer_fingerprint` and `BM25FieldConfigHeader.per_field_fingerprint[0]`.

The **fingerprint gate** (`bm25_fingerprint_gate`, `src/bm25_analyzer.c`, contract §I)
re-resolves the analyzer from the index's reloptions, fingerprints it, and compares to
the metapage's stored value. On mismatch it ERRORs (`ERRCODE_FEATURE_NOT_SUPPORTED`) by
default, or WARNs and proceeds under `require_analyzer_match = false`. The guarded
scenario is a stored fingerprint that no longer matches what the analyzer would now
produce — e.g. a post-build `ALTER INDEX ... SET (language=…)` without REINDEX, or a
session whose `search_path` resolves `<lang>_stem` to a dictionary the build never
bound. (A `pg_upgrade` that shifts the Snowball dict OID used to trip it too; since
`docs/adr/0080` it does not, which is the point of that record. A `pg_upgrade` whose
stemmer changes a probe word's output DOES trip it, since #296 — and should: the stored
terms were stemmed by the old major. So does an in-place `ALTER TEXT SEARCH DICTIONARY`
that changes a probe word's output.)

It runs at **two** sites, and the difference between them matters:

- **Scan start** (`BM25_GATE_SCAN`), at *two* sites against the fingerprint captured
  in that path's own snapshot: the ranked prologue `bm25_scan_corpus_stats` and the
  boolean `@@@` filter path `bm25_load_if_needed`. Both are genuine BM25 index scans; a
  plain `@@@` filter the planner answers with *another* index instead runs `bm25_match`,
  which uses the default analyzer and never reaches the gate at all.
- **Per inserted row** (`BM25_GATE_INGEST`), from `bm25_insert`, before it tokenizes —
  `docs/adr/0081`, closing #188. `INSERT` is not one of the maintenance commands PG17's
  `RestrictSearchPath` covers, so it resolves under the *caller's* path; and a
  `language` reloption edit reaches the same place with no shadowing at all. Proceeding
  here is strictly more expensive than on the scan side — the row is committed with
  terms the index's own analyzer does not produce and is unfindable until REINDEX — so
  the ingest site carries its own, starker message text. One function, one comparison,
  one `require_analyzer_match` decision; only the wording is per-site.

**Filled by M4 (were reserved in v4):** `BM25SegmentHeader.pos_root` (valid iff the index
stores positions); `BM25DictEntry.pos_post_root`/`pos_post_off` (the term's POS-chain entry,
Invalid/0 when positions are off); `BM25_PAGE_POS` pages (the separate position chain);
`BM25PendingTermEntry.pos_bytes` (the pending-path position blob). Positions are read-gated:
a field's positions are consulted iff `pos_root` is valid AND its `store_positions` flag is
set, so a pre-M4 (position-less) index — where `pos_root` is Invalid — is byte-for-byte
unchanged and the missing `store_positions` flag array is never consulted.

**Filled by M5 (were reserved in v4):** `field_count` = natts; `keymap_root` (valid iff
`key_field`); `BM25BlockHeader.field_rle_bytes` > 0 for multi-field; `ndocs_by_field[]`
appended after `total_len_by_field[]` for multi-field. Single-field on-disk bytes are
unchanged from M3.

**Filled by M2b (the reserved v4 slot, only fillable via a hard break to v5):**
`BM25BlockHeader.max_impact` (a single seal-time float4) is DROPPED, not filled in
place — its shape could not carry a safe scan-time bound (see the on-disk layout
section above), so it is replaced by the trailing per-field `BM25BlockImpact` table
(`impact_bytes`). This is the ONLY v4→v5 change; nothing else in the format moved. This
was the LAST reserved slot — after M2b the format carries no reserved-but-unused fields.
This is no longer a problem for future growth: roadmap #4 (v6) replaced "reserve a slot
at the last hard break, fill it later" with a general contract — a length-prefixed
trailing region or a new page type can be added additively at any time, with no reserved
slot needed in advance (see the "v6 format-negotiation gate" paragraph above and
`docs/adr/0009-format-stability.md`). Reserved-slot fill-in-place was a v3/v4-era
technique, superseded, not a pattern to keep using.

**Remaining (written as sentinels, never read/built) — do not treat as bugs:**
- `BM25DictEntry.dict_pad` is an explicit TAIL pad, not a feature slot — and **not an
  alignment pad**. Every member of `BM25DictEntry` is a uint32/uint16, so the struct's
  alignment is 4 and its `sizeof` is 20 with or without the field (and `MAXALIGN(20)` is
  24, so the field cannot be supplying MAXALIGN either). Its purpose is to turn what
  would otherwise be implicit trailing padding into a NAMED field the writer memset-0s,
  so the WAL image is deterministic. The record's MAXALIGN comes from the
  `MAXALIGN(sizeof(BM25DictEntry) + termlen)` stride applied at the write site
  (`bm25_seg_build.c`) and the read site (`bm25_seg_dict.c`), not from this field.
- Per-field analyzer overrides are DEFERRED: `BM25FieldConfig.tokenizer_type`/`stemmer_name`
  and `BM25FieldConfigHeader.per_field_fingerprint[]` are written as CLONES of the single
  index analyzer (all fields share one analyzer; the format carries divergence, M5 does
  not tokenize differently per field).

`bm25_reclaim_orphans` marks the field-config chain (`meta->field_config_blkno`) and each
live segment's `keymap_root` AND `pos_root` chains reachable; a merged-away segment's keymap
and POS chains are retired via `BM25RetiredEntry.keymap_root`/`pos_root` and freed by
`bm25_reclaim_retired` — so page reuse never frees a live config/keymap/POS page and neither
chain leaks under churn.

## Multi-field / BM25F / key_field (M5)

A multi-column `CREATE INDEX ... USING bm25_native (title, body)` makes each indexed column a
dense **field** (`field_id` = index attnum, `field_name` = attname, ≤ `BM25_MAX_FIELDS`=32).
Per-field `k1`/`b`/`boost` come from `k1_<col>`/`b_<col>`/`boost_<col>` reloptions
(read from the raw `pg_class.reloptions` array — the names are only known at CREATE INDEX
— and range-validated: `k1≥0`, `b∈[0,1]`, `boost≥0`, erroring on a bad value; the upper
caps `k1_<col>≤1e30` and `boost_<col>≤1e6` bind at CREATE/ALTER only, so a stored value
never wedges a scan or INSERT; a suffix naming no key column draws a WARNING at
CREATE INDEX/REINDEX), defaulting
to the index-wide `k1`/`b` reloptions when no per-field override is present (real
reloptions, `k1 ∈ [0, 1e30]` default `1.2`, `b ∈ [0, 1]` default `0.75`; boost has no
index-wide knob and defaults to `1.0`).

**k1/b/boost are LIVE** (`docs/adr/0012-live-k1b-reloptions.md`): both the global and
per-field reloptions are re-resolved from `pg_class.reloptions` at every scan start, so
`ALTER INDEX ... SET`/`RESET` on any of them takes effect on the very next scan, no
`REINDEX`. This is safe only because nothing in the postings bakes these three values —
`store_positions` and the analyzer configuration remain build-stamped and require
`REINDEX` (the analyzer additionally fingerprint-gates a stale stamp at scan time, since
its tokens ARE the postings).

**BM25F scoring** happens in the ONE existing scorer pass (no second pass, still
dedupe-by-TID): each posting contributes
`boost_f · bm25_idf(N_field_f, df_field_f) · bm25_termscore(tf, doclen_f, avgdl_f, k1_f, b_f)`,
summed into the doc's single score. `df_field` is derived by partitioning the term's
`dict.df` via the per-block field-id RLE (`Σ_field df_field == dict.df`); `avgdl_f =
total_len_by_field[f] / ndocs_by_field[f]` (`N_field` counts only docs that HAVE the
field). A single-field index collapses to the exact M3 formula (boost 1.0, `N_field =
ndocs`) so its scores are byte-identical.

**Field scoping:** a query RHS `field:term` (scope = `bm25_query_field_prefix`, a whitespace-free
run ended by a colon; name resolved against the baked field-config page; an unknown name is
literal text before `/` or a digit, else ERROR (#306); no scope = all fields)
scopes scoring/matching to that field. It is honored ONLY on the genuine index scan —
use the ranked form, `WHERE col @@@ 'field:term' ORDER BY col &@@ 'field:term'`. The `@@@`
predicate is what makes the bm25 index path eligible; `ORDER BY … &@@` alone does not
(`amoptionalkey = false`: it plans as Seq Scan + Sort and every row ranks `+inf`). When the
planner applies `@@@` as a **Filter** instead (`enable_indexscan = off`, `@@@ … OR …` — no
`amgetbitmap` —, a non-owner on a table with a non-trivial RLS policy, since `bm25_match` is not
`LEAKPROOF`), `bm25_match` sees only the LHS value and no index: a scope-shaped RHS (no whitespace or
quote before the colon, `bm25_query_field_prefix`) raises `feature_not_supported` (#298) instead of
searching the field name as a term; a bare RHS matches the LHS column only, where the index
matches every field — documented, not detectable there (ADR 0004). Default-english analyzer
too; the ranked form is the reliable entry. The rule across the two paths is "off-index refuses
or agrees, never silently differs" (`docs/adr/0122`): `bm25_match` cannot tell a known field name
from an unknown one, so it must keep refusing every scope-shaped RHS, including the `/`-or-digit
shapes the index path reads as literal text; letting those through would reopen #298 for
`'title:2024'`. All three text parsers use one whitespace class, `bm25_query_isspace`.

**key_field** names an **INCLUDE** column (`... INCLUDE (id) WITH (key_field='id')`) — a
non-text column can't be a bm25 key attribute, so INCLUDE is how its value reaches the
build callback. Its type (`int4`/`int8`/`uuid`/`text`, text truncated to 16 B; NULL →
zero sentinel — projected as key 0, "unspecified identity" vs a genuine 0, no null flag;
non-unique → unspecified identity) is recorded in a `BM25_PAGE_KEYMAP` docid→key array so
a scored query returns the user key (`bm25_score_key`; ctid fallback only when NO key_field
is set, i.e. keymap_root Invalid). The key threads through the accumulator, survives seal + merge, and its
pages are reclaimed. The ranking dynahash still keys on TID (dedupe); the keymap is an
output-projection layer only (a non-unique key never collides in the accumulator).

## Positions / phrase / snippets (M4)

**Position stream — a SEPARATE `BM25_PAGE_POS` chain (the Lucene `.doc`/`.pos` split).**
Positions are NEVER inline in the POST block. Each segment roots its position chain at
`header.pos_root`; a term's frames begin at `dict.pos_post_root`/`pos_post_off` — the exact
structural parallel of the `post_root`/`post_off` docid/tf blocks. One frame is emitted per
position-bearing posting (per `(doc, field)`, not per doc), encoded
`[tf-count varbyte][Δpos varbyte × tf]` with the Δpos accumulator reset at each frame. The
reader decodes frames in **lockstep** with the POST scan through an OPTIONAL nullable
`pos_cb` cursor, self-verifying that each frame's decoded `tf-count == tf` (a mismatch is a
hard ERROR — stream desync / corruption). The whole point of a separate chain is that a
**bag-of-words scan passes `pos_cb = NULL` and never faults a POS page** into the buffer
cache; only a phrase query opens the second cursor. The chain is orphan-built like KEYMAP,
so it adds ZERO buffers to the 4-buffer publish window; merge replays it and the FSM
reclaims it via the complete `pos_root` twin of every `keymap_root` reclaim site.

**Per-field `store_positions` (default true).** A `store_positions` index-wide reloption plus
per-field `store_positions_<col>` knobs resolve into each `BM25FieldConfig`. The per-field bit
does NOT grow `BM25FieldConfig` — that would misparse an existing M5 field-config page (read at
every scan, no struct-version gate). Instead it rides a **trailing `uint8[field_count]` flag
array appended after the config records** on the field-config page; the reader picks it up when
present past `pd_lower`. An absent flag array (a legacy / pre-M4 index) ⇒ positions OFF, which
is only ever reached when `pos_root` is already Invalid, so the absence is harmless. An index
is uniformly position-less until REINDEX enables positions. A field with `store_positions=false`
writes no frames for its postings, shrinking the POS chain.

**Pending path carries true positions.** Each `BM25PendingTermEntry` stores a delta-varbyte
position blob (`pos_bytes`) after its term bytes, so a doc inserted then sealed gets correct
positions — not a positionless placeholder that a later phrase query would silently miss.

**Phrase / proximity query surface.** The RHS grammar (parsed in `bm25_rescan`, composing with
the M5 `field:` prefix) is: `"a b"` = EXACT phrase (ordered, slop 0); `"a b"~n` = unordered
W/n; `"a b"~>n` = ordered PRE/n. `~>` is tested before `~` (so `"a b"~>3` is not misread as
unordered slop `>3`); a `~` with a non-numeric or empty tail is a hard parse ERROR (never
silently exact), and slop is capped at `BM25_MAX_PHRASE_SLOP` (100000) **on both surfaces** —
the jsonb `bm25_phrase(slop => …)` builder enforces the same ceiling as the `~n` text suffix,
which it did not before ADR 0047 (the matchers cast `(nterms − 1) + slop` to `uint32`, so an
unbounded value wrapped and the span test degenerated to "any co-occurrence"). The RHS is analyzed to
the same stemmed terms as everything else. Matching is a **post-accumulation recheck** (`bm25_phrase.c`), NOT a per-posting gate: the scorer stashes each phrase term's per-`(TID, field)`
position list via the `pos_cb`, and after the full scan drops any ranked TID that fails the
positional test — filter-only scoring (survivors keep the BM25F score they already accumulated
over the constituent terms). This is why the recheck **does not move the WAND seam or the
`bm25_scan_build_ranking` contract**. A bare (unscoped) phrase ORs across fields (adjacency must
hold within some single field — positions reset per field); `field:"a b"` scopes to one field.

**Slots, not tokens (#184, `docs/adr/0085`).** The unit of the phrase is one source WORD of the
query, called a *slot*. A compound-splitting or thesaurus dictionary turns one word into several
lexemes, and any one of them at a document position satisfies that slot (OR-per-slot — the
query-side equivalent of core's `<0>` distance inside a `phraseto_tsquery` alternative). The span
arithmetic therefore counts source words. `bm25_phrase_slot_map` derives the grouping from the
analyzer's own token positions (a new slot wherever `.pos` changes); `phrase_recheck_tid` gathers
each slot's member lists per field, disqualifies the field only when a slot has NO member present
(per-slot ANY-of, replacing the old per-token all-of), borrows the pointer for the single-member
case and merges + dedups otherwise. The position STASH is unchanged — still one list per
`(token, field)`. **Live since analyzer revision 5** (`docs/adr/0087`, issue #184: `bm25_analyze`
now stamps one position per source RUN instead of per lexeme, so a compound run's lexemes
genuinely share a slot instead of each owning one). Two divergences from core remain, deliberately:
a slot is satisfied by ANY lexeme its run produced, where core ANDs the lexemes of one variant and
ORs the variants — recall-only, because `bm25_analyze` discards `TSLexeme.nvariant` and cannot
reconstruct the chains, so this never drops a match core would find, only adds ones it wouldn't;
and under `stopwords = default` a dropped stopword consumes no position here, where core lets it
consume one. Both are named, not hidden, in `docs/adr/0087`.

**D7 degradation (fail loud, never silent).** A phrase query touching a position-less segment
(`pos_root` Invalid, e.g. a pre-M4 index) or landing on a `store_positions=false` field raises
`ERROR: positions not present … REINDEX to enable phrase/proximity search`. The
`phrase_fallback = 'and'` reloption (default `'error'`) relaxes this to a `WARNING` + AND-of-terms
semantics — mirroring the `require_analyzer_match = false` → WARNING precedent. Silent
AND-fallback and silent empty results are both rejected (false positives / hidden index-state
are unacceptable for legal search).

**`phrase_fallback` is honoured on the TEXT phrase surface only.** A `bm25_phrase(...)` leaf in a
**jsonb** query tree ERRORs *regardless* of `phrase_fallback` whenever any of its leaves lands on a
position-less segment or a `store_positions=false` field. That covers a bare `bm25_phrase` root as
well as one nested inside `bm25_boolean`/`bm25_boost`, on both `@@@` and `&@@`: only a single
MATCH/TERM root is rewritten into the text-equivalent `so->qterm` path, so any PHRASE root goes
through the jsonb gate. That gate raises `ERROR: bm25: positions not present in this index; REINDEX
to enable phrase/proximity search in a jsonb query tree` without ever consulting the reloption;
`bm25_phrase_fallback_is_and` has exactly one call site tree-wide, and it is the text gate.
This is a **capability** limit, not an oversight. The AND-of-terms fallback rides `PhraseAndEnt`'s
single `uint64` mask, whose two uses are mutually exclusive: in `fallback_and` mode a bit indexes a
PHRASE TERM, in `boolean_mode` it indexes a LEAF. Degrading one leaf of a boolean tree would need
per-leaf, per-term presence *simultaneously* with the leaf bits, i.e. a second presence structure.
Until that exists this surface fails loud rather than silently disagreeing with the text surface
about what the same phrase matches. Both gates do share the *predicate*
(`bm25_phrase_fields_have_positions`/`_segments_have_positions`); only the ERROR-vs-degrade policy
is per-surface. Workaround: `REINDEX` with positions, or express the phrase through the text RHS
grammar.

**`bm25_snippet(field, start_tag DEFAULT '<mark>', end_tag DEFAULT '</mark>', max_num_chars
DEFAULT 300)`.** It **re-analyzes the passed text** — it does NOT read the stored position
chain (post-stem ordinals have no retained mapping to original character spans, so the position
stream and snippets are fully independent consumers). It marks tokens whose stem is in the query
stem set, which since #308 is built from every NON-NEGATED leaf of the scan's query tree, any
shape (`docs/adr/0123`): MATCH/TERM/PHRASE text analyzed under the scan's config, WILDCARD patterns
folded as the expander folds them and globbed against the field's own tokens; built once per scan,
sorted, probed by `bsearch`, with an interrupt check per field token. Field scope is ignored and a
phrase marks its terms anywhere in the field (documented residuals). It selects the densest ≤`max_num_chars` window, wraps matched surface runs in the tags
with ORIGINAL casing preserved (each token carries its `src_off`/`src_len` byte span in the
source text), and is UTF-8-safe (window edges snap to char boundaries). Like
`bm25_score`, it only works when the bm25 index scan actually runs; unlike `bm25_score` it
carries no row identity, so under concurrency it FAILS LOUD rather than resolving (**R3**, see
"Score accessor concurrency" below) instead of a single shared slot. It returns SQL NULL on a
NULL field, a NULL `max_num_chars`, no active scan, an empty query, or zero hits (callers typically
drop NULL snippets), and ERRORs on `max_num_chars <= 0` (trust boundary) and on a truncated
multibyte sequence (`bm25_mblen_bounded`). The truncation ellipsis is converted to the
SERVER ENCODING rather than emitted as hardcoded UTF-8 (`docs/adr/0050`) — it is the only
non-ASCII literal the extension writes itself, so it was the one site ADR 0046's sweep did
not reach; it was mojibake in `LATIN1` and an outright `invalid byte sequence` error in
`EUC_JP`.

**It escapes the field text by default, and this DIVERGES from `ts_headline`** (`docs/adr/0048`).
The tag defaults are HTML, so the documented way to consume the result is to render it as markup,
and markup stored in the indexed column would otherwise reach the browser verbatim. `escape`
(the fifth argument, default `true`) rewrites `& < > " '` in the FIELD text; the caller's TAGS are
never escaped, because they are markup by contract and escaping them would render `<mark>` as
visible text. Tags therefore remain a trust boundary the caller owns. `escape => false` restores
byte-verbatim field text for non-HTML consumers. A NULL `escape` escapes, as does a catalog entry
predating the argument (`PG_NARGS()`): not saying anything picks the safe direction.

**`max_num_chars` counts CHARACTERS of the original field text** (`docs/adr/0049`) — not bytes,
which is what it measured through M4, and not output length. The two facts compose: an escaped
`&` spends one unit of budget and emits five bytes, so `escape` changes how the excerpt renders
and never which window is selected. Hit-edge character offsets are resolved in ONE forward pass
(`snippet_hit_charpos`), and the per-side character allowance is converted back to a byte bound by
a forward walk (`snippet_char_to_byte`) — forward-only because a character length is read from its lead byte, so no
general server encoding can be stepped backward. Budget a side cannot use (a hit near a field
edge) carries to the other side, in characters, before that conversion (#308 TEXT-06).

**Two honest limitations.**
- **Phrase / snippet are index-scan-only.** They are honored only on the genuine ranked `&@@`
  scan — the same limitation class as `field:term` scoping and the analyzer recheck. A bare
  boolean `@@@` the planner may answer via `bm25_match` (LHS-column-only, position-blind,
  default-english) refuses a phrase or a `field:` scope and cannot produce a snippet;
  discriminating tests force the index with `&@@`.
- **Scored-scan robustness (ROADMAP §3) — now substantially addressed (R3).** A runtime error
  raised in the TARGET LIST of a `&@@` scored scan used to be able to FATAL the backend (a
  `PG_TRY`/`return` bug in `bm25_scan_build_ranking`, fixed separately — see the TRADEOFFS ADR,
  gated by `41_scored_scan_error`). The other half of ROADMAP §3 — the per-backend SINGLE-SLOT
  scored-scan plumbing silently returning the WRONG scan's number under concurrency — is fixed
  by the registry + row-identity resolution in "Score accessor concurrency (R3)" below;
  `bm25_snippet` keeps a narrower documented residual there; `bm25_distance` resolves by
  ORDER BY query identity (`docs/adr/0061`) and its residual is narrower still.

## Block-max WAND (M2b)

**What it accelerates.** A ranked scan (`ORDER BY col &@@ q LIMIT n`) no longer always
runs the exhaustive OR-sum scorer over every posting. Block-Max WAND
(`src/bm25_wand.c`/`.h`, a new module) skips whole 128-doc posting blocks whose
best-POSSIBLE contribution cannot raise a doc into the current top-k, while still
returning the top-k **bit-identical** to the exhaustive scan it replaces — this is
acceleration, never a different answer.

**The safe bound.** `bm25_block_ub` (`src/bm25_wand.c`) sums, over every field present
in BOTH the block's impact table and the query, `boost_f · bm25_termscore(idf_f,
max_tf_f, min_doclen_f, avgdl_f, k1_f, b_f)` — the block's RAW impact values combined
with the SCAN-TIME `idf`/`avgdl` (never seal-time-baked; see the on-disk layout
section). `bm25_termscore` is monotone increasing in `tf` and decreasing in `doclen`, so
substituting a block's max-over-postings `tf` and min-over-postings `doclen` into the
EXACT scoring formula is a true, provable over-estimate of any real posting's
contribution in that block — not a heuristic approximation. `sql/43_wand_parity.sql`
pins this against `bm25_debug_term_contrib` (the real per-posting formula) on a corpus
engineered so the block's `(max_tf, min_doclen)` combination is not any single real
doc's pair.

What the bound does **not** owe is bit-exactness with the scorer's summation. It
cannot: the deep check accumulates over docid-sorted cursors while the score is built
in query-term order, and `bm25_block_ub` returns a per-term subtotal while
`score_doc` folds every `(term, field)` posting flat — so the two regroup relative to
each other whenever a term after the first matches more than one field. Rather than
chase a fold order two of the three accumulation sites structurally cannot honour,
the bound owes only DOMINATION and every prune comparison is widened by
`wand_widen_ub(ub, nsummands)` — a relative slack of `(2n+4)·DBL_EPSILON`, derived
from the standard `γ_n` error bound for summing `n` non-negative values. Both
widenings are conservative (strictly more candidates scored, never fewer), so the
delivered ranking is unchanged. See `docs/adr/0043`; this is a different contract
from WAND ≡ exhaustive below, which governs the SCORE path and is unaffected.

**The cursor.** `BM25WandCursor` (opaque, `src/bm25_wand.c`) walks one query term's
blocks within one segment, one posting at a time (`bm25_wand_cursor_next`), or jumps
directly to the first posting with local docid ≥ target (`bm25_wand_cursor_next_geq`)
by reading SKIPPED blocks' HEADERS ONLY (`bm25_seg_block_header_read`) — never
varbyte-decoding a block the query doesn't need. `global_ub` (the term's bound in
this segment: per field, the max over blocks of that field's bound, summed over fields
-- not the max of per-block sums, which under-bounds a document whose postings straddle
two blocks, issue #289) is precomputed once at cursor open by a header-only sweep of
every block -- header-only saves the decode, not the walk: one `ReadBuffer`/lock/release
PER BLOCK, linear in the term's block count, paid before anything is pruned. Measured
rather than assumed cheap (QRY-03, issue #153): under 1% of a WAND build's buffer
accesses on an ordinary 100k-doc corpus, 7.8% on one built so later segments quit at
the pivot test right after this sweep. Storing the bound's inputs in the DICT record
at seal time was built and rejected on those numbers (`docs/adr/0096`) -- DICT chains
grew 43-50% in pages and rare-term queries touched 11-15% more buffers. That under-1% share
was measured before #267's page images; after them the sweep is 15-36% of the buffer accesses of
an ordinary query's build at `LIMIT` 10, up to 43% on a merged layout (`bench/README.md`), and the `docs/adr/0096` addendum records the
changed premise without reversing the decision. Where the
rest of a build's buffer accesses go was profiled chain by chain in `docs/adr/0100`
(`bench/wand_global_ub.sh` reports the totals): each NORMS, LIVEDOCS and DOCMAP
lookup is one `ReadBuffer`, and LIVEDOCS reads against tombstone-free segments were
the largest share until the ranked readers began checking each segment's bitmap once
at init instead (see the `live_ndocs` landmine row). What remained was one NORMS and one
DOCMAP access per pair, plus key projection for rare-term, large-k queries; #267's page
images (below) removed most of both. Rows arrive
in score order, so within a segment the docids jump backward; the finalizers therefore
resolve keys per segment in one forward KEYMAP pass (#246 item 1) rather than
re-walking the chain from its root on each jump. The same follow-up made the exhaustive
scorer, the phrase and boolean passes and the `@@@` collector check LIVEDOCS once per
segment, gated as the df pass is (item 4) and had the df pass hand its dictionary lookups to both scorers
(item 3); the measurements are in `docs/adr/0100`'s addendum. #267 then served same-page
lookups without a buffer access: a cursor that opts in through `bm25_seg_reader_cache_pages`
keeps a `BLCKSZ` copy of the page its last lookup landed on and answers later lookups on
that block from it. Only NORMS, DOCMAP and KEYMAP cursors may, never LIVEDOCS (see the
page-image landmine). Owners and their bounds: WAND's cursors and driver reader live in
its per-segment context (one copy per query term plus one); the exhaustive scorer's three
readers and the `@@@` collector release theirs after each (term, segment); the ranked-row
key cache gives copies to at most 64 segment readers (512 KB). None of it is charged to
`max_match_memory`. Not done, recorded as residuals: keys are still resolved for every
ranked row, not only the rows returned (the score-by-key hash, the query-qualified score
accessors and the tail rebuild each want every key up front; see
`bm25_seg_key_cache_fill`), and the merge replay does not use images (unmeasured).
`bm25_wand_cursor_score_doc` folds the current docid's field-postings
into a caller-owned running accumulator ONE POSTING AT A TIME, in the same order
(query-term order, then ascending `field_id`) the exhaustive scorer accumulates in —
required for bit-exactness, not style (see below).

**The top-k heap.** `BM25TopK` (opaque, `src/bm25_wand.c`) is a bounded min-heap whose
drain order (`bm25_topk_drain_sorted`) matches the exhaustive comparator exactly —
score DESCENDING, ties broken by `ItemPointerCompare(tid)` ASCENDING — because the
WAND result has to be indistinguishable from what it replaces, tie-break included.

**The driver (`bm25_wand_build_ranking`, Ding–Suel block-max WAND).** Runs per segment
under ONE shared global top-k heap, so θ (the current k-th-best score) rises
monotonically across segments processed in sequence. Pending is scored FIRST,
exhaustively (blockless — it has no posting blocks to prune), both to prime θ before
any segment runs and to register every pending TID in a dedupe set (`pending_tids`) so
a segment's postings for a doc already resolved by pending are skipped (pending wins,
matching the exhaustive scorer's dedupe-by-TID). Each segment then runs the classic
WAND pivot (align every term cursor, find the pivot term whose cumulative upper bound
first clears θ) with the block-max extension as a DEEP CHECK before scoring the pivot
candidate: sum every ALIGNED cursor's CURRENT block-level bound and shallow-skip
(advance past the block without scoring) when that sum can't reach θ. The shallow-skip
target is capped at `min(min_last+1, next_docid)` (never skip past the next candidate
the pivot logic itself would have picked). A candidate that survives the deep check is
scored via `bm25_wand_cursor_score_doc` per term, deduped against `pending_tids` and
live-checked (`bm25_seg_doc_is_live`) before being offered to the heap.

**Live wiring (`src/bm25_scan_rank.c`).** `bm25_scan_build_ranking_once` is now a
DISPATCHER: WAND runs iff `so->scoring && !so->qphrase && bm25_native.wand_top_k > 0` (a
boolean `@@@` scan never reaches this function — `bm25_gettuple` routes it to the
unordered postings path before ranking is considered — and `phrase_fallback='and'`'s
AND-downgrade is provably unreachable whenever `!so->qphrase`, so it needs no separate
conjunct; see the function's header comment). Everything else — phrase/proximity
queries, the AND-fallback downgrade, and `bm25_native.wand_top_k = 0` — falls through to the
(renamed) `bm25_scan_build_ranking_exhaustive`. WAND is **ON BY DEFAULT**:
`bm25_native.wand_top_k` (GUC, `PGC_USERSET`, default 100, `0` disables — `src/bm25_handler.c`)
bounds how many top rows WAND fills directly. The WAND result path populates the M5
`ranked_keys` projection identically to the exhaustive finalize.

**Over-pull tail fallback (`bm25_gettuple`).** WAND only fills the top `wand_top_k`
rows (`so->wand_capped`). If the executor pulls past that (a `LIMIT` beyond
`wand_top_k`, or no `LIMIT` at all — e.g. `count(*)` over an ordered subquery),
`bm25_gettuple` lazily rebuilds the FULL exhaustive ranking through the retry wrapper
(`bm25_scan_build_ranking`, `force_exhaustive=true` threaded through to `_once`) —
routing through the wrapper (not calling the exhaustive scorer directly) keeps this
rebuild exposed to the same seg_gen-abort-and-retry safety net as the scan's first
build, instead of surfacing a page-reuse race as a user-visible error. `force_exhaustive`
exists because `so->wand_capped` is still true at this point from the first build, and
without it the dispatcher would re-run WAND and reproduce the same capped top-k,
silently dropping rows k+1..N instead of recovering them. **Invariant (#268): a rebuild
may only extend the emitted prefix; emitted rows' scores are immutable.** The rebuild
takes a fresh snapshot (postings and membership), but scores under the capped WAND
build's corpus statistics, pinned in `so->stats_pin` (`BM25PinnedStats`, `src/bm25.h`:
per-field avgdl and k1/b/boost, per-token idf); `bm25_term_idf` still runs, for its
dictionary lookups, and only its idf output is replaced. A document both builds saw
therefore scores bit-identically in both. The tail then resumes at the first entry
strictly after the last emitted (score, TID) in `scored_desc` order
(`bm25_tail_resume_pos`), which is exact even when the last emitted row has since
vanished. A cassert build checks the invariant after the tail rebuild
(`bm25_tail_check_emitted`). Only the forced tail rebuild consumes the pin; a non-forced
build clears it, and the seg_gen retry of the tail rebuild keeps it.

**Match-set memory budget (`bm25_native.max_match_memory`, ADR 0047).** The exhaustive
scorer and the non-scoring `@@@` union both hold the WHOLE match set — one `BM25AccEnt` per
matching doc, then a flat drain array, then `ranked`/`scores`/`ranked_keys` — and neither
can spill: a global sort by score needs the whole set before it can emit row one. Both were
unbounded, so a common term over a large corpus consumed several GB and then died on
`MaxAllocSize` with a message naming neither the query nor a knob. The budget is a GUC
(`PGC_USERSET`, `GUC_UNIT_KB`, default **256 MB**, `0` = follow `work_mem`). Crossing it is an
**ERROR**; there is nothing to degrade to.

Accounted in BYTES, in ONE running total shared by every structure the build materializes —
both properties forced by review after a first version counted DOCUMENTS PER STRUCTURE.
Counting documents is wrong wherever the structure is not per-document: the `@@@` collector
takes one entry per (term, doc) and dedupes only afterwards, so a doc limit there is really a
posting limit, and a 10-term query errored at a tenth of the matches a 1-term query allowed —
reporting a doc count the query had never reached. Counting per structure lets two per-doc
structures each spend the whole budget (a boolean query fills both `acc` and the leaf-presence
hash). Charges are taken at the INSERTION points — `bm25_scores_add`, `phrase_and_mark`,
`tid_collector_add`, `phrase_stash_add` — not at the allocations, because the dynahash growth
is both the larger consumer and the earlier one; each charge is a deliberate over-estimate;
none is ever released (within one build the total IS the peak, and every rebuild starts fresh
alongside its fresh scratch context).

`phrase_and_mark` is charged on its own account rather than assumed to track `acc`:
`pending_tids` is written only for a doc that scored and a `must_not` PHRASE is rejected
before execution, so those DO track `acc` — but the leaf-presence hash also marks leaves
carrying NO score. `phrase_stash_add` is charged because bytes are what a doc count could
never have covered: a stashed doc holds up to `tf` positions and `tf` reaches 65535.

Interacts with the over-pull tail above: a ranked scan escapes materialization only while the
consumer stays INSIDE `wand_top_k`. Reading past it triggers the rebuild, which re-runs the
exhaustive scorer and lands back at the bound — which is why the error's HINT says
`LIMIT n`, not merely "use a ranked scan", and why `83_query_limits_memory_bounds` asserts
both halves.

**Bit-exact discipline.** WAND scores a candidate posting-at-a-time in query-term
order, then ascending `field_id` within a term — the exact order the exhaustive scorer
accumulates. Both `bm25_block_ub` and `bm25_wand_cursor_score_doc`/`seg_posting_cb`
compute `contrib = boost·termscore` into a NAMED intermediate (a rounding barrier)
before `+=`-ing it into the running total; the Makefile pins `PG_CFLAGS
= -ffp-contract=off` (`Makefile`) so no compiler fuses that multiply-add into a
single-rounding FMA — which would let the bound's and the exhaustive scorer's
structurally-identical expressions round DIFFERENTLY (1 ULP apart at the coincidence
boundary) on different compilers, passing on one CI compiler and failing on another.
This combination is what makes WAND == exhaustive **bit-identical**, not merely close,
and portable across CI's PG 17/18 × compiler matrix.

**Tests.** `sql/43_wand_parity.sql` (the bound dominates the real per-posting
contribution; WAND == exhaustive incl. BM25F multi-term + over-pull);
`sql/44_wand_skip.sql` (anti-neuter: `bm25_wand_stats(...).blocks_skipped`/
`deep_check_skips` > 0 prove the header-only skip AND the block-max deep check
specifically fire, not just pivot alignment; + stats-drift-after-delete +
merge-parity gates); `sql/45_m2b_acceptance.sql` (a 6000-doc realistic multi-field BM25F
corpus; all four query shapes — single-term, multi-term, field-scoped, multi-field
co-occurring — prune AND match the exhaustive scan bit-for-bit, including over-pull);
`t/012_v5_wand.pl` (crash + replica: WAND == exhaustive AND pruning still fires
post-recovery/on a standby — parity alone can't rule out a driver that silently fell
back to decoding every block).

## Boolean + wildcard query trees (M6)

**Query representation — jsonb, not a DSL.** A second RHS type for `@@@`/`&@@`: jsonb query
objects built by the SQL functions `bm25_term`/`bm25_match_terms`/`bm25_phrase`/
`bm25_wildcard`/`bm25_boolean`/`bm25_boost` (each `RETURNS jsonb`), consumed via `(text,jsonb)`
overloads of `@@@`/`&@@` on `text_bm25_ops` (strategy 1 = `@@@`, strategy 2 = `&@@`). User
input is a bound jsonb VALUE, never parsed as a query language — structurally injection-proof.
The `(text,text)` path (`field:term`, `"phrase"~n`) is UNTOUCHED and byte-identical; the
opfamily just gained a second operator per strategy. NO on-disk change (still v5).

**AST + flow.** `bm25_rescan_parse_jsonb` (`src/bm25_scan.c`) parses the jsonb into a
`BM25Query` tree (`src/bm25_query.c`) via `bm25_query_parse` (validation ERRORs; since #304
each node kind's args are a CLOSED key set, slop must be integral and a tree has at most
`BM25_QUERY_MAX_NODES` = 1024 nodes, `docs/adr/0124`), then
`bm25_query_flatten` assigns each leaf a `leaf_bit` from ONE 0.. counter over all leaves
(≤ `BM25_QUERY_MAX_LEAVES`=64, so `1<<leaf_bit` fits a `uint64`), folds each `boost` weight as
a product into its enclosing leaves' boost (a scoring leaf's product must lie in [1e-6, 1e6];
`must_not` leaves are exempt), and propagates `negated` down `must_not` edges. A
single MATCH/TERM leaf is copied straight to `so->qterm`/`qfield` and runs the EXISTING text
scan path verbatim (WAND-eligible); any multi-leaf tree takes the exhaustive scorer.

**Scoring + membership.** In the ONE exhaustive scorer pass, each positive leaf's terms score
into `acc` (per-term OR-sum, scaled by the leaf's folded `boost` via `idf_f *= boost` — exact,
since `bm25_termscore` is linear in idf) AND mark the leaf's bit in the REUSED `and_presence`
HTAB (`PhraseAndEnt.mask`, `phrase_and_mark(cur_qi=leaf_bit)` — NOT a new `BM25AccEnt` field). A
`must_not` leaf is presence-only (gated decode, NO score). At drain, `bm25_query_eval(mask)`
(must=AND, should=OR-required-only-when-no-must, must_not=AND-NOT) filters the accumulated
TIDs. **Membership is EXACT; the SCORE is a bag-of-words approximation** — a should-PHRASE or
should-WILDCARD whose own adjacency/expansion predicate fails still leaves its terms' score in
a doc a sibling clause keeps (the single-pass accumulator can't retract a leaf's terms). This
is by-design, inherited from the M4 phrase filter-only contract, and reconciles the design
doc's D11 "Σ over matched leaf contributions" wording with the implementation; see Landmines.
The non-scoring `@@@` filter path (`bm25_load_if_needed`) reuses the SAME builder, so the
filter set == the ranked set by construction (D12), for every leaf type — and, since #132,
for a TEXT phrase too (`so->qphrase` is the second disjunct of that path's delegation guard;
the two are mutually exclusive, since only the text micro-parser sets `qphrase` and only the
jsonb parser sets `qtree`). The exhaustive scorer depends on that exclusivity without re-deriving
it — its dispatch chains take the first true arm, so phrase + boolean at once would silently drop
`must_not` — and since #67 it `Assert`s the three modes (`phrase`, `fallback_and`,
`boolean_mode`) pairwise exclusive, so a cassert build traps a future jsonb PHRASE root that set
`qphrase`.

**Wildcards (`bm25_wildcard('field','judg*')`).** `bm25_dict_expand_wildcard`
(`src/bm25_seg_dict.c`) expands the pattern by a prefix range-scan + embedded-`*` glob over the
byte-sorted STEMMED dict, per sealed segment + pending, deduped into ONE distinct set (else a
term present in N segments double-counts under the leaf's single bit). The pattern is matched
RAW-lowercased — it BYPASSES the stemmer (stemming `judg*` would be nonsense). Guardrails:
`bm25_native.wildcard_min_prefix` (default 3, enforced at PARSE, path-independent) and
`bm25_native.wildcard_max_expansions` (default 1000, ERROR — never truncate). Both are
**`PGC_SUSET`**, unlike `seal_threshold`/`wand_top_k`: they are the only bound on how much work
one pattern may demand, and a guardrail the constrained user can lower is not a guardrail
(`SET …_min_prefix=0; SET …_max_expansions=1000000;` then `bm25_wildcard('body','*')`). This
blocks tightening by a non-superuser too; delegate with
`GRANT SET ON PARAMETER bm25_native.wildcard_min_prefix TO <role>` (ADR 0025, `67_wildcard_guc_privileges`).
A wildcard leaf consumes
ONE `leaf_bit` regardless of expansion count. Ceiling: the dict iterator has no seek, so the
prefix scan is a linear O(dict) skip per segment; the cap is checked post-materialize — bounded
by the guardrails, fine at M6 scale.

**WAND / operator anchors.** Any multi-leaf/boolean/phrase/wildcard/boost jsonb tree bypasses
block-max WAND (`bm25_qtree_is_multileaf` → exhaustive); only a single MATCH/TERM leaf is
WAND-eligible (and the filter path also bypasses via `so->scoring == false`). `bm25_match_jsonb`
exists so the `@@@ jsonb` operator resolves; it is never invoked on the index path
(`bm25_gettuple` sets `xs_recheck=false`; EXPLAIN shows no Recheck Cond) and **ERRORs
(fail-loud) whenever it IS reached** — i.e. when the planner demoted `@@@` to a filter/seqscan
qual (it cannot match a jsonb tree from a heap value; it used to return `false` there,
silently dropping every row). The jsonb tree is parsed and evaluated elsewhere
(`bm25_rescan_parse_jsonb`/`bm25_query_eval`), never by this
function. `bm25_distance_jsonb` is NOT inert: like `bm25_distance`, it IS invoked per row as the
`&@@ jsonb` projection on the Index Scan node's own target list. Both call the shared
`bm25_distance_for_query`, which resolves the OWNING scan by matching the projected
expression's own ORDER BY RHS against the RHS each scoring scan stashed — NOT by taking the
active-scored-scan registry's HEAD, which is what #138 fixed (under a correlated subquery the
inner scan registers during the outer's projection, so the head is the wrong scan). `+inf` only
off the index, or when no live scan ranked that query — see the rank-collapse fix in Landmines,
"Score accessor concurrency (R3)", and `docs/adr/0061`.

## Score accessor concurrency (R3)

**Problem this replaces.** Through M6 + the rank-collapse fix, `bm25_score`,
`bm25_score_key`, `bm25_snippet`, and `bm25_distance`/`bm25_distance_jsonb` all read
one backend-global pointer, `bm25_active_scored_scan` — "whichever scored scan most
recently called `bm25_gettuple`." Two bm25 scored scans concurrently alive in the
SAME query (a correlated subquery, or a self-join with both sides ranked) shared
that one pointer: whichever scan's `bm25_gettuple` ran most recently silently
overwrote it, so an accessor evaluated against the OTHER scan's row returned that
other scan's number — a **silent wrong answer**, never a crash or a NULL. An
empirical repro (a correlated inner query ranking `'bar'`, correlated to an outer
ranking `'foo'`) showed the inner scan is legitimately the MOST-RECENTLY-registered
scan even though the OUTER's score is the one being projected — ruling out "most
recent wins" as a general resolution strategy (see the ADR's Alternatives).

**The registry.** `bm25_register_scored_scan`/`bm25_deregister_scored_scan`/
`bm25_scored_scan_head` (`src/bm25_score.c`) replace the
single pointer with a backend-local intrusive singly-linked list
(`BM25ScanOpaqueData.next_active`), head = most-recently-registered. A scan
registers on its first scoring `bm25_gettuple` (idempotent — a rescanned inner scan
re-loads and re-registers, by first removing any prior entry, so it can never
appear twice) and deregisters in `bm25_endscan`. List depth is query-nesting depth
(typically 1-2), so a linear walk is cheap.

**Registry ownership and the accessors' privilege check (#301, `docs/adr/0115`).** The list is per
backend, not per role, and the accessors are executable by PUBLIC, so a
`SECURITY DEFINER` function's still-open scan (a `LANGUAGE sql` SRF in the caller's
target list; a refcursor) was probeable by the caller, whole ranking included.
Registration records `owner_userid = GetUserId()` and the index relation; every
walker filters through `bm25_owned_from` (directly via `bm25_next_owned`, or via
`bm25_visible_from`/`bm25_next_visible`, which add the privilege check), so a scan
registered under another user id is invisible (`bm25_scored_scan_head` included). Registration is the capture point that
matters: the executor calls `index_beginscan` lazily at the first fetch, so a
definer-opened refcursor's scan registers under the CALLER's id. That shape is closed
by the second check, made only by the row-addressed accessors (`bm25_score*`,
`bm25_score_key*`, `bm25_snippet`): `bm25_probe_acl_compute` requires no
`check_enable_rls == RLS_ENABLED` on the heap or any PARTITION ancestor, and
`SELECT` on the heap, on a partition ancestor (a partition has exactly its parent's
columns, so a grant on a partitioned parent covers the partitions each child scan
reads; a legacy `INHERITS` parent's grant does not, because a child can add columns
the parent's grant does not reach), or on every heap column the index reads. The
result is cached per call site in `fn_extra` (`BM25ProbeFnCache`, keyed on index and
user id; the user id is what keeps it correct across SET ROLE / SECURITY DEFINER,
while a same-user REVOKE can go unseen for a PL/pgSQL simple expression's lifetime).
**A refused scan is invisible**, exactly like another role's: the probe walkers
(`bm25_visible_from`/`bm25_next_visible`) test it before consulting its ranking or
current row, and it is never a candidate, a match or a sole-live rival. Testing it
only after a lookup hit made the resulting NULL a membership oracle whenever the
caller's own scan ranked the same query. The only exception is `bm25_snippet`'s
`ERRCODE_INSUFFICIENT_PRIVILEGE` error when the caller has no readable scan but owns
a refused one, which depends on the scan's existence, not its contents.
The `&@@` distance is deliberately owner-filtered only: it is the ORDER BY key Merge
Append merges on, and a role ranking through a granted view (no base-table `SELECT`)
must still get real distances. Pinned by `sql/131_scored_scan_owner`.

**Row-identity resolution (`bm25_score`/`bm25_score_key`) — bounded concurrent-correct,
via call-site attribution (#242, `docs/adr/0103`, supersedes `docs/adr/0007`'s
recency-first clause).** Each scan stamps `cur_ranked_idx` (a `BM25_NO_CUR`
sentinel when unpositioned) on every row `bm25_gettuple` emits, and a backend-wide
monotonic `bm25_emit_seq` counter is stamped onto a scan (`last_emit_seq`) every
time it emits. The resolvers (`bm25_resolve_score_tid`/`_key`) walk the registry
from the head: a scan whose `cur_ranked_idx` row matches the projected ctid/key is
the head-first pick. But a rescanned correlated inner scan re-heads itself on every
outer row, and when its current row coincides with the outer's (its top hit, never
rejected by the correlation), head-first alone returns the WRONG scan's score for
the outer's own row. Each textual `bm25_score(...)`/`bm25_score_key(...)` call site
therefore caches a binding (`BM25ScoreCallSite {bound_serial, last_call_seq}`) in
its `FmgrInfo->fn_extra` (the `cs` member of `BM25ProbeFnCache` since #301), by `scan_serial` rather than pointer (a freed opaque's
address can be reused). The head-first pick is overridden by the bound scan only
when: the bound scan's current row matches the probe, the head-first pick is a
DIFFERENT scan, and BOTH have emitted since this call site's last call
(`last_emit_seq > last_call_seq`) — requiring the head-first pick to be fresh too
is what stops a hash join's stale finished-build-side row from being wrongly
overridden by a probe-side binding. When exactly one scored scan can still emit
(ADR 0064's `bm25_sole_live_scored_scan` gate), a lazily-built per-scan hash
(`score_by_tid`/`score_by_key`, built once on first fallback probe by
`bm25_build_score_index`, freed with `scanctx` on rescan/endscan) answers a
DECOUPLED projection (the id read after its row was emitted, or in arbitrary
order) in O(1). `bm25_resolve_score_key` additionally width-checks a candidate
scan's `ranked_key_size` against the argument's before any byte comparison, so a
narrower concurrently-active key (e.g. an `int` `key_field` scan alongside a
`uuid` one) can never be read out of bounds.

**The contract is bounded, not universal, and code/SQL comments say so instead of
claiming "never a wrong number."** Projected directly on its own scan's emitted
rows, the accessor returns that scan's score even when another live scan's current
row coincides. Two residuals remain, where the accessor CAN still return another
concurrent scan's score: an **unattributable collision** (the call site never saw
a probe matching exactly one scan alone — e.g. a join on the key with the accessor
above the join — so it has no binding and falls back to the head-first pick), and a
**decoupled projection** (a PL/pgSQL `FOR` loop prefetching rows, a
`Sort`/`Materialize` above the scan, a cursor — where a binding learned from a
coincidental match can be the wrong scan). `sql/53` pins a constructed
PL/pgSQL-prefetch shape as a documented residual, measurably worse than the plain
walk on that one row.

**Query-qualified overloads name the scan (#253, `docs/adr/0105`).** Those residuals,
and a ctid repeated across the children of a Merge Append, are not fixable by any rule
over (row identity, registry state), so `bm25_score(tid, query [, regclass])` and
`bm25_score_key(key, query)` (text or jsonb `query`; `regclass` is `tableoid`) take the
scan's identity from the caller. `bm25_resolve_by_query` (`src/bm25_score.c`) walks the
registry for scans that rank the byte-identical query (`bm25_scan_ranks_query`, the
predicate the `&@@` distance projection shares), narrowed to the given heap, and asks
each whether its WHOLE-ranking hash (`score_by_tid`/`score_by_key`) holds the row. It
never reads the current row, so a decoupled projection resolves for as long as the scan
is registered and not rescanned. One holder gives its score; several holders with the
same score give that score; several with different scores give NULL, and so does a key
shared by two ranked rows with different scores (a conflict flag in the key hash entry,
written by `bm25_build_score_index`). Do NOT add the ADR 0104 emit-recency tiebreak here:
the overloads exist for the shapes where the newest emitter is not the owner. The
one-argument accessors ignore the flag and are unchanged. Limits: after a rescan the
answer comes from the new ranking, and past `wand_top_k` the WAND tail rebuild re-ranks
under a fresh snapshot but the first build's statistics (the #268 landmine, below), so a
row both builds saw keeps its score exactly; rows past the cap that the scan never emitted
read NULL. That rebuild
now re-positions `cur_ranked_idx` on the last emitted row (a capped build holding exactly
`wand_top_k` matches used to leave the scan reading as never positioned), which also
changes the one-argument accessors in that shape: `bm25_score` above a `Sort` answers
instead of NULL, and `&@@` in a later statement over a fully fetched cursor reads the
last emitted row's distance instead of `+inf`. Suite: `sql/116_score_query_overloads`.

**What identity means (ADR 0089).** Both resolvers match a row's identity, not the
particular encoding the ranking happened to capture — two ordinary situations make
those differ, and each used to return a silent SQL NULL (#100, #204):

- **A pending row has no segment**, so `bm25_seg_key` cannot supply its key. The key
  is instead backfilled from the PENDING record, which already carries it
  (`bm25_ranked_keys_fill_from_pending`, called by BOTH ranking builders — WAND's
  over-pull tail rebuild swaps one builder's ranking for the other's mid-scan, so a
  one-sided fix would be discarded past `wand_top_k`). The walk takes the scan's
  CAPTURED `snap.pending_head`, never a fresh metapage read, or a concurrent seal
  could move the head mid-ranking. Key-config discovery falls back to the pending
  list's own validated metadata when no sealed segment carries a KEYMAP — without
  which an index built on an empty table and then loaded projects NULL for EVERY row.
- **A HOT update moves the tuple, not the index entry.** The index holds the chain
  ROOT; the executor projects the descendant's ctid. A missed probe is retried
  against `heap_get_root_tuples`'s root. The block is bounds-checked against
  `RelationGetNumberOfBlocks` BEFORE any `ReadBuffer` — an out-of-range read would
  EXTEND the relation, making a read-only accessor a writer — and the mapping uses
  only the probed scan's own heap (`so->heaprel`, stashed at emit).

**`ranked_key_present` is not decoration.** An unresolved key slot is `palloc0` fill
and int4/int8 keys are stored raw, so a genuine `id = 0` is byte-identical to it;
presence is therefore tracked out of band, in an array parallel to `ranked_keys` and
nulled with it at every reset site. Both consumers (the current-row `memcmp` and the
`score_by_key` build) must honour it or an unresolved row re-enters the lookup under
a key it does not have. This is separate from a SQL NULL `key_field`, which is a real
zero key by design and still projects as `0`. Invalid ItemPointers (`ip_posid == 0`,
e.g. `'(0,0)'::tid` — a legal `tid` value) are rejected at `bm25_resolve_score_tid`'s
entry: the tid accessors `Assert` validity, so probing one aborted the backend on a
cassert build, which the hardening CI leg is.

**Fail-loud (`bm25_snippet`).** A snippet carries no row identity to resolve
against, so `bm25_sole_scored_scan()` (`src/bm25_score.c`) raises
`ERROR (ERRCODE_FEATURE_NOT_SUPPORTED)` instead of guessing which scan's excerpt to
return. The check is deliberately NOT "is more than one scan registered": a
finished scan lingers in the registry until its OWN `bm25_endscan` runs, which for
a `UNION ALL` sibling (or any other `Append`/`SubqueryScan` child) fires only at
`ExecutorEnd` — long after that sibling returned its last row — so a raw count
would make every `UNION ALL` of ranked snippet queries spuriously ambiguous
(`39_snippet` Part 4 is exactly this shape). Instead it walks the registry BEHIND
the head and errors only when a scan there could still emit a FUTURE row
(`rcur < nranked`, or WAND-capped with the exhaustive tail not yet rebuilt) —
mirroring `bm25_gettuple`'s own "return false" condition without adding new scan
state.

**Query-identity resolve, not fail-loud (`bm25_distance`/`bm25_distance_jsonb`).**
These two are ALSO the functions PostgreSQL calls to materialize the `&@@` sort key
as a per-row resjunk column on EVERY ranked query (a secondary `ORDER BY` key, or a
direct `SELECT` of the distance, forces the materialization) — making them fail
loud would break any statement with two coexisting ranked scans, including the
overwhelmingly common single-scan case (e.g. one wrapped in a view referenced
twice). So they cannot fail loud, and cannot use `bm25_snippet`'s strategy.

They used to read `bm25_scored_scan_head()`'s stashed `cur_orderby_dist`. That was
WRONG under nesting and is fixed as of `docs/adr/0061`. The head is the most
recently REGISTERED scan and registration happens at LOAD, so under a correlated
subquery the inner scan becomes head DURING the outer row's projection and the
outer row was projected with the inner's distance. They now resolve by ORDER BY
QUERY IDENTITY: each scoring scan stashes its ORDER BY RHS verbatim, and the
projection matches its own RHS against those stashes to find the owning scan.

**Same-query siblings (#252, `docs/adr/0104`).** A ranked `ORDER BY body &@@ q` on a
partitioned or inheritance parent plans as a Merge Append over one index scan per
child, every one with the same RHS, so the RHS match leaves several positioned
candidates. `bm25_distance_for_query` returns the one with the highest
`last_emit_seq` (the #242 stamp above), not the first found from the registry head;
a sole candidate is unchanged. The head-first rule projected every child other than the
last-registered positioned one with that child's current distance, so Merge Append saw a
constant key and drained them out of score order (wrong `LIMIT k`). This is correct
only because the `&@@` resjunk is projected on the scan's own target list right
after its emit; it is not the "most recently emitted wins" that 0061 rejected,
because it runs only after the RHS match. Suite: `sql/112_merge_append_distance`.

Two claims that used to stand here are FALSE and were removed rather than softened:
that only the materialized VALUE was affected while row ORDER stayed
index-correct — sorting on the clobbered value ties every row and collapses the
ranking to the tiebreak, measured — and that this matched the pre-R3 single-slot
behaviour harmlessly. Note also that `xs_orderbyvals`, which the AM does fill, is
NOT an escape route: core consumes it only inside `IndexNextWithReorder`, never for
projection, so the registry is the only channel available (ADR 0061).

**Teardown.** A `MemoryContextCallback` on `so->scanctx`
(`bm25_scanctx_arm_deregister_cb`, armed in `bm25_beginscan` and RE-ARMED after
every `bm25_rescan`'s `MemoryContextReset` — PostgreSQL's per-context
reset-callback list is one-shot, so a callback armed only in `beginscan` is already
consumed by the mandatory post-beginscan `bm25_rescan` before the scan ever
registers, leaving nothing armed for the real teardown) deregisters the scan even
when `bm25_endscan` is skipped entirely — the aborted-portal path
(`PORTAL_FAILED`), where a LATER statement's portal teardown
`MemoryContextDelete`s `scanctx` without PostgreSQL ever calling `amendscan` for
the failed one. Without this callback the registry would keep a node pointing at
freed memory, and the next scored scan's `bm25_register_scored_scan` (which walks
the list to move itself to head) would dereference it.
`bm25_deregister_scored_scan` is idempotent, so running it both via this callback
and via the explicit call in `bm25_endscan` is harmless.

**Residual limitations (documented, not fixed — see the ADR's Consequences).**
- `bm25_snippet`: a scan projecting its ONLY/FINAL matched row (`rcur==nranked`)
  while sitting BEHIND a still-live head can return a wrong excerpt silently — a
  strict improvement over pre-R3 (wrong for every concurrent case), unreached by
  the test suite, and unfixable without threading row identity through snippet
  (a resjunk score column or custom scan — considered and declined as out of
  scope; see the ADR).
- `bm25_snippet`: a WAND-capped, `LIMIT`-stopped sibling scan can spuriously
  ERROR — the safe direction (loud, never silently wrong).
- `bm25_score`/`bm25_score_key` (one-argument): bounded by call-site attribution
  (`docs/adr/0103`) to rows projected directly on their own scan's emitted rows. Two named
  residuals remain — an unattributable collision (no call-site binding, falls back to the
  head-first pick) and a decoupled projection (PL/pgSQL prefetch, `Sort`/
  `Materialize`, a cursor) — in both of which the accessor can still return
  another concurrent scan's score. The query-qualified overloads (`docs/adr/0105`) avoid
  both; their own limits are in the paragraph above.
- `bm25_distance`/`bm25_distance_jsonb`: resolve by ORDER BY query identity
  (`docs/adr/0061`), and among scans ranking the BYTE-IDENTICAL query by emit
  recency (`docs/adr/0104`) — correct when `&@@` is projected on the scan's own
  target list right after its emit (Append / Merge Append children). Residuals, in
  which the newest emitter need not be the owner: a decoupled projection (a join's
  target list evaluating two same-query scans' `&@@` after both emitted, a
  same-query subquery run ahead of the resjunk, `Sort`/`Materialize`, a cursor); the
  same-RHS correlated shape (right for the inner's `&@@`, wrong for the outer's);
  and an inheritance child with NO bm25 index (a Sort over a Seq Scan under the
  Merge Append). Its rows get `+inf` if that Sort drains before any sibling index scan
  has emitted, otherwise the newest emitter's current distance, one constant for all of
  them, so they sort as if tied at a sibling's score. Observed 2026-09-29 on the fixed
  code (PG 18.6); by the code the head-first rule likewise resolved such rows to a
  sibling scan (not A/B'd), so this predates #252, and `sql/112` does not cover it.

See `docs/adr/0007-score-accessor-concurrency.md` for the fuller Context/Decision/
Alternatives/Consequences record; `docs/adr/0061-distance-resolves-by-query-identity.md`,
which supersedes its distance-family decision; and
`docs/adr/0103-score-accessor-call-site-attribution.md`, which supersedes its
current-row-collision clause and "never a wrong number" claim for `bm25_score`/
`bm25_score_key`; `docs/adr/0104-distance-among-same-query-siblings-resolves-by-emit-recency.md`,
which changes 0061's byte-identical-RHS residual (registration recency to emit
recency); and `docs/adr/0105-query-qualified-score-accessors-name-the-scan.md`, which
amends 0103 (the one-argument accessors still follow it) with the overloads.

## AM entry points

| Callback | Function (file) | Note |
|----------|-----------------|------|
| `ambuild` | `bm25_build` (build) | Heap scan → `BM25Accum` → two-phase seal of one segment. |
| `aminsert` | `bm25_insert` (build) | Append one doc to the pending list; conditional opportunistic seal at `bm25_native.seal_threshold`. |
| `amgettuple` | `bm25_gettuple` (scan) | Non-scoring: stream postings unordered. Scoring (`norderbys>0`): lazily build the full ranking on first call, then emit in descending score. |
| `ambulkdelete` | `bm25_bulkdelete` (handler) | Iterate every live segment's docs, test each heap TID against core's callback, tombstone via `bm25_livedocs_clear`; sweep pending for dead entries. |
| `amvacuumcleanup` | `bm25_vacuumcleanup` (handler) | Seal pending (`bm25_seal_index`), then `bm25_reclaim_retired`, then the opportunistic `bm25_merge_maybe(false)`, and `bm25_reclaim_orphans` **last** (issue #300, `docs/adr/0118`): the O(index) sweep is the pass most likely to be cancelled, and a cancelled sweep must not also skip the only automatic merge and retired reclaim. The sweep must still follow the seal, and following the merge lets the same cleanup free the catalog chain the merge's swap orphaned. The sweep is gated on evidence and runs in ShareLock (see "The orphan sweep runs only on durable evidence"). Then `IndexFreeSpaceMapVacuum` on EVERY cleanup: the truncate's range-only FSM vacuum (PEND-07) and the allocator's requeue rely on it, and fsm_search only corrects a stale parent downward. |
| `amcanorderbyop` | (true, in handler) | `WHERE col @@@ 'q' ORDER BY col &@@ 'q' LIMIT k` → ordered index scan, no Sort. The `@@@` qual is required (`amoptionalkey = false`). |

## Planner interface

Three numbers the extension feeds the core planner, none of them derived
from live index or table state: the `@@@` operator's `oprrest`/`oprjoin`
(`bm25_matchsel`/`bm25_matchjoinsel`, `src/bm25_selfuncs.c`, a compile-time
`BM25_MATCH_SEL = 0.05` blind to the RHS); `COST 5000` on
`bm25_match(text,text)` and `bm25_match_jsonb(text,jsonb)`
(`bm25_native--1.0.sql`); and `bm25_costestimate`'s stub output
(`src/bm25_handler.c`: `*su = 1.0`, `*tot = 10.0 + 0.01 × tuples`, `*sel =
0.05`, `*corr = 0.0`, `*pages = 1.0`). All three are constants, not
estimators, because a df-reading estimator cannot see a correlated-`Var`
RHS and would add plan-time index I/O and ERROR risk for no benefit on
the query shape that matters most — see `docs/adr/0010-planner-estimates.md`
for the full derivation and rejection record. The two selectivity paths
above (`oprrest` and `bm25_costestimate`'s `*sel`) are independent code
that merely share a value today; a future cost pass that raises
`bm25_costestimate`'s `*su` toward honesty must not do so in isolation —
combined with a much higher effective `@@@` selectivity it can re-open the
plan-flip ADR 0010 measured closed (§7 there).

The other numbers the planner reads from this extension are the per-function
declaration properties in `bm25_native--1.0.sql`. Most declarations there still take
the conservative defaults (`VOLATILE`, `PARALLEL UNSAFE`), correctly — an explicit
property in that file means the default was WRONG for that function, which is the
audit `docs/adr/0082-declaration-defaults-that-misrepresent-are-made-explicit.md`
records. What it changed:
`COST 5000` also on `bm25_snippet` (it re-analyzes the whole field per row) and on
the four probes that drive a full ranking build (`bm25_debug_rank`,
`bm25_debug_wand_rank`, `bm25_debug_rank_key`, `bm25_wand_stats`); a `ROWS`
estimate on every one of the 23 set-returning probes, in place of the flat
`prorows = 1000` default; `PARALLEL SAFE` on the six jsonb builders, without which
one folded-away builder call de-parallelised the entire plan
(`max_parallel_hazard` runs on the raw parse tree, before constant folding); and
`VOLATILE` on `bm25_snippet`, matching `bm25_score` over the same backend-local
scan state. `sql/103_declaration_properties` pins all of it.

## Binding invariants

Violating any of these is a correctness or crash-safety bug, often invisible in
non-assert CI. THEORY §3–§5 explain *why* each holds.

| Invariant | Statement | What breaks if violated |
|-----------|-----------|-------------------------|
| **Single-record seal** | A seal publishes the new segment **and** advances `pending_head`/stats in ONE Generic WAL record; `bm25_pending_truncate` is a post-commit page-recycler that never touches `pending_head`. | BM25 scores are **additive**, not GIN-bitmap-idempotent: a publish-before-truncate crash window double-scores the redrained docs and double-counts global stats. |
| **Two-phase install** | Every seal/merge builds all new pages as orphans (many ≤4-buffer records, metapage untouched), then ONE final record flips `segcat_root` + writes stats + (seal) resets the pending anchor / (merge) appends the retire RANGE. Never link a page into a live structure before the final record. | A multi-record commit has a crash window referencing a half-built segment; straddling metapage locks lets a concurrent seal/merge corrupt the snapshot. |
| **4-buffer cap** | One Generic WAL record modifies ≤4 buffers (`MAX_GENERIC_XLOG_PAGES`). The linearizing record budget is segcat-root + metapage + ≤1 retired-list page. | This cap is *the* reason for orphan-build + pointer-flip (catalog swap), RANGE-not-per-page retire, and two-phase segment install. Exceeding it fails `GenericXLogFinish`. |
| **pd_lower discipline** | After every flat-page mutation (META, SEGCAT, RETIRED, PENDING) manually advance `pd_lower` past the written region; readers parse `PageGetContents`..`pd_lower`. | Generic WAL page-hole compression **silently drops** bytes between `pd_lower` and `pd_upper` (GIN `ginfast.c:419`) — silent data loss. |
| **WAL window** | `GenericXLogStart` → `RegisterBuffer` (FULL_IMAGE for a freshly `PageInit`'d page) → modify ONLY the registered copy → `GenericXLogFinish`, holding the buffer EXCLUSIVE throughout. Use `pd_upper − pd_lower` for flat-page free space, not `PageGetFreeSpace`. | Modifying the original (not the registered copy) or dropping the lock mid-window produces WAL that mismatches the page → corruption on replay/standby. |
| **Abort-in-window** | Any `ereport(ERROR)` inside an open Generic WAL window must `GenericXLogAbort(state)` and `UnlockReleaseBuffer` every registered buffer first. | Otherwise the WAL state + buffer pins/locks leak and wedge the backend. |
| **An open Generic WAL window is throw-free** | Between `GenericXLogStart` and `GenericXLogFinish`: no `ReadBuffer`, no `LockBuffer`, no `palloc`, no `ereport` — memory writes on the registered copies only. Every allocation a writer needs is done BEFORE the window opens; where that is impossible mid-stream (a chain writer discovering it needs a continuation page), the writer FINISHES the current record first, allocates with no window open, and opens a fresh small record to write the link — `bm25_pending_append_multi`'s three-small-records new-tail path and `bm25_keymap_write`'s continuation path both have this shape. | The whole reason the rule can be stated so bluntly is that it makes the abort case above unreachable rather than merely handled: `bm25_page_alloc` can `ereport` (FSM walk, relation extension) and returns a page already EXCLUSIVE-locked, so calling it inside a window both throws in the window and locks the next page before the previous is flushed. Stated at `bm25_fsm.c`'s swap helper; the keymap writer's header claimed it and violated it until `docs/adr/0083`. **Scope (D27, `docs/adr/0083`'s 2026-10-05 addendum):** the rule binds windows over PUBLISHED structure (publish and swap records, the pending append, the catalog appender, the levers). The segment builder's orphan windows are NOT throw-free (the DICT tail window stays open across `palloc`, `chain_ensure` raises `PROGRAM_LIMIT_EXCEEDED`); that is accepted because every page they write is unreachable until the caller's publish record and the builder runs in a transaction that aborts on error. Do not extend that exception to a window over a reachable page. |
| **seg_gen validation** | Every segment page is stamped with its `seg_gen` (≥1; re-stamped on reuse via FULL_IMAGE); the reader validates `op->seg_gen == expected_gen` after locking each followed page. `next_gen` inits to 1; gen `0` is the skip-validation sentinel; gens never repeat (no ABA): both draw sites call `bm25_next_gen_check`, which refuses the draw that would wrap the counter (`54000`, REINDEX; #313). **Pending pages (#291)** carry their chain's epoch in `seg_gen`, drawn from the same `next_gen` in the append record that publishes a new `pending_head` and copied onto later pages; `BM25ScanSnapshot.next_gen` is captured under the same metapage lock as `pending_head`, and `bm25_pending_walk_read` (every scan-side pending walker) rejects `seg_gen != 0 && seg_gen >= snap.next_gen` (`docs/adr/0110`). | This is the reuse-safety mechanism (option d). A stale pointer to a reclaimed-then-reused page is caught and cleanly aborted (`40001`; retried on the ranked path, reported on `@@@`) instead of reading recycled bytes. Zero-initializing `next_gen` would make the first segment skip its own validation. Drawing a pending epoch anywhere other than the record that publishes the head, or from a different counter, breaks the bound: a scan could then see a chain whose epoch is not below its captured `next_gen` (a false `40001` on the primary), or a reused page whose epoch is. |
| **Block independence** | Each 128-doc posting block is varbyte-δ-encoded from a **zero base**; the decoder MUST reset its `prev` accumulator at every block boundary. | Carrying `prev` across blocks inflates local doc-ids past `ndocs` for every block after the first → backend abort under cassert, invalid TIDs / undercounts otherwise. (Real bug, caught only by a multi-block term in CI.) |
| **df-bounded decode** | The POST chain is **one segment-wide chain with no inter-term delimiter** (D-POST); `bm25_seg_scan_postings` takes an explicit `df` bound. | Without the bound the decoder over-reads into the next term's postings. |
| **Position lockstep (M4)** | When a POS cursor is open, the reader decodes exactly one `[tf-count][Δpos]` frame per position-bearing posting, in lockstep with the POST scan, and asserts the frame's decoded `tf-count == tf`. Whether a posting bears a frame is decided by its field's `store_positions` bit (keyed on the decoded `field_id`), so a mixed on/off index stays unambiguous. | A misaligned frame silently returns wrong positions (false phrase hits/misses); the `tf-count == tf` self-check turns a desync into a hard ERROR instead of a corrupt match. |
| **POS reader is opt-in** | A bag-of-words scan passes `pos_cb = NULL`; the POS chain is opened ONLY for a phrase query. | Faulting POS pages on every ranked scan defeats the whole `.doc`/`.pos` split — the reason positions are a separate chain, not inline in the POST block. |
| **Score each doc once** | Honor the scan-start single-metapage-lock snapshot (ignore segments sealed after scan start) AND dedupe by heap TID, pending-wins. | Additive scores → a doc in both pending and a stale segment posting (the UPDATE case) is double-scored, corrupting ranking. |
| **Appends exclude seals** | `bm25_pending_append_multi` takes `LockPage(index, BM25_METAPAGE_BLKNO, ShareLock)` before the metapage buffer lock. ShareLock conflicts with the sealer's ExclusiveLock but not with itself, so concurrent inserters still run in parallel. This is what makes the publish record's UNCONDITIONAL pending-anchor reset safe. | `LockPage` (lmgr, LOCKTAG_PAGE) and `LockBuffer` (LWLock) are different lock managers and do not conflict, so the metapage buffer lock alone excluded nothing — and `aminsert` appends BEFORE it tries `ConditionalLockPage`. A document appended during a seal was published by nobody and had its page recycled: committed, heap-visible, findable only after REINDEX. Reproduced pre-fix as a backend CRASH. `docs/adr/0022`. |
| **`bm25_bulkdelete` holds the singleton for its WHOLE pass** | `bm25_bulkdelete` takes `LockPage(index, BM25_METAPAGE_BLKNO, ShareLock)` from before its catalog snapshot (`bm25_segcat_read`) through `bm25_pending_mark_dead`'s pending sweep — one critical section covering the catalog read, every segment's tombstone loop, and the sweep. ShareLock, so it still does not serialize against concurrent appenders (same mode, ADR 0022) or against its own re-acquisition in the sweep — but it conflicts with every writer that can change which documents live where: the seal, the merge, `bm25_upgrade`'s rewrite, and both reclaims, all ExclusiveLock. None of those can run inside the pass. | Originally only the pending sweep held the singleton (`docs/adr/0066`); the tombstone loop did not. A seal draining a dead pending doc into a segment the snapshot never saw was missed by the sweep; a merge replaying an input's LIVEDOCS bits before a tombstone landed republished the document live (resurrection after heap line-pointer reuse) or, if it swapped mid-loop, left VACUUM tombstoning a retired segment (`segment header N not found in catalog`); a merge's pre-flip catalog copy silently overwrote a survivor's counters and VACUUM's decrements between the copy and the flip. `docs/adr/0102` (issues #239, #241); extends `docs/adr/0066`'s trade to the whole pass. A `bm25_native.debug_pause` (PGC_SUSET) test lever plus `t/020_vacuum_merge_race.pl` drive all four interleavings deterministically. Operator-visible effects (`docs/adr/0102` addendum, README maintenance section): the stall exists only in VACUUM's index-vacuuming pass (PG runs it only when the heap scan found dead items, and can skip or bypass it), not in every VACUUM; `bm25_vacuumcleanup`'s seal and both reclaims take the singleton with a BLOCKING ExclusiveLock (only `bm25_merge_maybe(false)` skips), so a cleanup can wait behind an explicit seal, merge or upgrade; a waiting cleanup is never cancelled, but once an autovacuum cleanup HOLDS the lock, an insert waiting on it cancels that autovacuum after `deadlock_timeout` (never an anti-wraparound one); and explicit `bm25_merge` and a rewriting `bm25_upgrade` block inserts for their whole hold, releasing the lock between their seal and their second phase (`docs/adr/0022` addendum). |
| **A drained chain is detached whether or not anything was published** | The anchor reset lives in the segment-publish record, which runs only when the drain yielded ≥1 live doc — but the recycle is gated on whether ANY chain was drained. When VACUUM has tombstoned every doc, `bm25_pending_reset_anchor` writes the same five fields in its own metapage-only record, BEFORE `bm25_pending_truncate`. Both seal sites share one core (`bm25_seal_pending_locked`). | Otherwise the metapage names FSM-free pages with `pending_tail_free` still set, and the next INSERT writes over pages the allocator has reissued. The state LATCHES — nothing repairs the anchor. Reachable from one ordinary VACUUM (its sweep runs before its seal); measured, 36 of 100 rows inserted afterwards were silently unfindable. `docs/adr/0065`. |
| **Page-lifecycle ops hold the singleton** | Every operation that decides a page's fate holds `LockPage(index, BM25_METAPAGE_BLKNO, ...)` (re-entrant; released on abort): the seal for its whole drain-build-publish-truncate, each merge pass across its build and swap, and `bm25_reclaim_retired` across each descriptor page's compact-free-splice, all ExclusiveLock; `bm25_reclaim_orphans` for its whole mark-and-sweep in **ShareLock** (issue #300), which still excludes every builder and swapper but admits appenders. Forced merges release between passes and `bm25_reclaim_retired` between descriptor pages, never mid-unit (the descriptor is compacted before its ranges are freed). Heavyweight before buffer locks, always. | "Unreachable ⇒ orphan" is only true while nothing is BUILDING: a segment under construction is unreachable orphan pages until its publish record commits, and `chain_flush` releases each page before allocating the next. An unlocked sweep stamps a live in-flight page `BM25_PAGE_DELETED` and frees it; the next `bm25_page_alloc` hands it back and the caller FPI-re-inits it — silent corruption from ordinary concurrent INSERT + autovacuum (`ShareUpdateExclusiveLock` does not conflict with `RowExclusiveLock`). `docs/adr/0019` (the reachability argument; its Exclusive mode is superseded by `docs/adr/0117`), `docs/adr/0118`. |
| **Metapage before segment-catalog page** | The index-wide buffer content-lock order. `bm25_scan_snapshot` holds the metapage SHARE across its whole catalog walk and MUST — that held lock is what stops a publish moving the chain under the copy — so the reader's direction is fixed and every writer conforms: the seal publish and `bm25_livedocs_clear` both take the metapage EXCLUSIVE first. `bm25_segcat_locate_entry` holds neither across the other. | Buffer content locks are LWLocks: no deadlock detector, not cancel-interruptible, and `AccessShare` vs `RowExclusive`/`ShareUpdateExclusive` do not conflict, so nothing else serializes the two. An inverted writer plus an ordinary ranked SELECT wedges both backends until SIGKILL/restart. `docs/adr/0018`. |
| **The orphan sweep runs only on durable evidence** | `bm25_reclaim_orphans` sweeps only when `meta.orphan_ops_begun != meta.orphan_ops_done`, `meta.swept_epoch` differs from `bm25_crash_epoch()` (a nonce in a named DSM segment, re-created with shared memory, so it changes on a crash-restart too), or `swept_epoch == 0` (never swept: new or legacy index). Every maintenance op that can leave orphans brackets itself with `bm25_orphan_op_begin` (a metapage WAL record BEFORE its first allocation) and `bm25_orphan_op_end`: the seal (drain through truncate), `bm25_reclaim_retired` (lazily, before its first compaction), and each merge pass and `bm25_merge_rewrite_all` — which never call end, because a successful swap orphans the old catalog chain. A completed sweep sets done = begun and records the epoch. The pending append writes its new page, the old tail's link and the metapage in ONE record, so it is not an orphan source. Under ShareLock the sweep skips an unreachable PENDING page whose epoch is 0 or `>= min(next_gen, head epoch, tail epoch)`: that is the page a concurrent appender may own. | A new orphan source without a bracket leaks silently: no test sees space that is never reclaimed until something measures relation growth. Closing a bracket on a path that leaves orphans BY DESIGN (a swap) leaks one catalog chain per merge (`t/029` (C), `sql/134`). Splitting the pending append back into several records makes a crash or ERROR between them an unbracketed orphan (t/028). Dropping the epoch-0 clause lets the sweep stamp pages an older binary's chain propagates (t/031 (C)). Residual: an ERROR inside `bm25_page_alloc` after a pop or a P_NEW extension strands one free page until the next sweep. `docs/adr/0116` (the gate), `0117` (share mode and the per-page rule), `0118` (bounded holds). |
| **Generic WAL registration order is the standby lock order** | On replay `generic_redo` locks every registered block EXCLUSIVE in registration (block_id) order and holds them all until the record is applied. So any multi-buffer Generic WAL record that includes the metapage must call `GenericXLogRegisterBuffer` on the metapage FIRST, whatever order the primary acquired the buffer locks in. `bm25_livedocs_clear` (meta, LIVE, catalog), `bm25_segcat_publish_append` (meta, catalog) and `bm25_pending_append_multi` (meta, tail; or meta, old tail, new page when a part needs a fresh page, one record since issue #300) all do. Adding a multi-buffer record that includes the metapage means extending `t/021_generic_wal_meta_first.pl`'s workload to emit it. | On the primary registration takes no locks, so the ADR 0018 fix was invisible there and three records kept the old order. On a standby a record that registers a reachable catalog page ahead of the metapage lets the startup process hold the catalog page EXCLUSIVE while waiting for the metapage that a `scan_snapshot_sampled` reader holds SHARE across its catalog walk: the same uninterruptible LWLock cycle. Derived from reading the server source (issue #240) and never reproduced; the only guard is `t/021` reading block order out of `pg_waldump`, not an `Assert`. PG 19 moved buffer content locks off LWLocks, but `BufferLockAcquire` in REL_19_STABLE's `bufmgr.c` still calls `HOLD_INTERRUPTS()` before waiting (read 2026-09-29), so the hazard class persists. `docs/adr/0018` (addendum), `docs/adr/0100`. |
| **A catalog entry's `live_ndocs` is not evidence that a segment has no tombstones** | Readers opened with `bm25_seg_reader_init_checked` (the ranked WAND path, `bm25_term_idf`'s df pass, the exhaustive scorer and the `@@@` collector; the phrase stash and the boolean presence pass take the scorer's answer through `bm25_seg_reader_init_known`) skip per-lookup LIVEDOCS reads only after checking the bitmap itself at init; the catalog count is not consulted. Keep it that way. Any reader that must see a tombstone made after it was opened (bulkdelete, the merge replay) must use plain `bm25_seg_reader_init`. | `bm25_segcat_publish_swap` copies surviving entries before its flip; `bm25_bulkdelete` now holds the singleton across its whole tombstone loop (`docs/adr/0102`), which closes the interleaving that used to leave a survivor's bit cleared while the published entry still said `live_ndocs == ndocs` (`docs/adr/0100`, #241). The bitmap check is kept anyway rather than downgraded to trusting the count now that the race is closed: the count was never load-bearing for this check, only additional evidence that would have been wrong, and a reader trusting counters as a shortcut reintroduces exactly the class of bug this row exists to warn about. |
| **The unlocked segment-catalog walkers need the metapage singleton** | A production caller of `bm25_segcat_read` (through `bm25_segcat_read_locked`) or `bm25_segcat_locate_entry` (through `bm25_livedocs_clear`) holds `LockPage(index, BM25_METAPAGE_BLKNO, ShareLock)` or stronger; `bm25_segcat_read_locked` and `bm25_livedocs_clear` assert it under cassert with `LockHeldByMe`. Callers today: `bm25_bulkdelete` (ShareLock), `bm25_reclaim_orphans`, `bm25_merge_execute`, `bm25_merge_rewrite_all` and `bm25_segcat_publish_swap`. `bm25_scan_snapshot` is outside the rule because it holds the metapage SHARE across its whole walk. A caller holding no singleton that needs one entry uses `bm25_segcat_first_entry` (entry 0 under the metapage SHARE; the INSERT key check does). The debug SRFs keep the unasserted `bm25_segcat_read`. | The walk releases the metapage after reading `segcat_root`, and a catalog chain orphaned by a merge has no `retire_xid`, so the next VACUUM's `bm25_reclaim_orphans` frees it for immediate reuse; only the singleton keeps that swap and sweep out of the walk. A walker without it can read a page that has become another kind and fail a healthy INSERT with XX002 (#270, reproduced by `t/022_insert_keycheck_race.pl`). The assert is cassert-only, and on the fixed build `t/022` cannot catch a return to the unlocked walk (its window runs under a buffer lock). `docs/adr/0107`. |
| **A chain page image is never taken of LIVEDOCS** | `BM25ChainCursor` keeps a `BLCKSZ` copy of its last page only after `bm25_seg_reader_cache_pages` opts it in, and only for NORMS, DOCMAP and KEYMAP. LIVEDOCS is excluded twice: the opt-in never gives a LIVEDOCS cursor a context, and `chain_page_cacheable` (used by `bm25_seg_walk_get`/`bm25_chain_page_keep`) refuses `BM25_PAGE_LIVE` regardless. Every owner passes a context that outlives the reader and bounds the copies, and a loop that re-inits a reader calls `bm25_seg_reader_release_pages` first (a re-init turns images off and would leave one behind per iteration). The merge replay and bulkdelete use plain readers. | `bm25_livedocs_clear` flips bits in place after seal, so a copy of a bitmap page can miss a tombstone a fresh read sees, and VACUUM's bulkdelete and the merge replay decide what to drop from exactly those reads. The other three chains are written once, before the publish record, and gens never repeat, so a same-page lookup returns the bytes a fresh read would. A hit needs the block number and the gen to match, and the kind check re-runs on it; if the page has been reclaimed and reused meanwhile, a fresh read would raise the gen failure and a ranked build would retry, while the copy keeps returning the segment's own bytes. A copy rather than the pin ADR 0100 sketched: the buffer is released before every lookup returns, so no reader needs resource-owner handling, and the copy is ordinary memory freed with its context on ERROR. Images are not charged to `max_match_memory`. `docs/adr/0100` (addendum). |
| **No `table_open` under a buffer lock** | `bm25_page_alloc(index, heaprel)` takes the pre-opened heap relation; open it at the top of each maintenance entry point, before any buffer lock. | `table_open` under a buffer lock inverts buffer-lock → heavyweight-lock order → deadlock. |
| **The metapage lock is what serializes relation EXTENSION; `bm25_page_alloc` takes no extension lock** | `ReadBuffer_common` passes `EB_SKIP_EXTENSION_LOCK` for a `P_NEW` blockNum, so `ExtendBufferedRelShared` never calls `LockRelationForExtension`, and `ReadBufferExtended`'s header puts the obligation on us: *"Caller is responsible for ensuring that only one backend tries to extend a relation at the same time!"* Every maintenance writer that can extend holds `LockPage(BM25_METAPAGE_BLKNO, ExclusiveLock)`; the pending appender holds `LockPage(…, ShareLock)` **plus** the metapage buffer content lock EXCLUSIVE. `ShareLock` does not self-conflict, so the buffer lock is what serializes two appenders; `ShareLock` does conflict with the singleton, so an appender and a seal never extend at once. Two callers hold neither half, both safely: `bm25_build` (`ambuild`) — no other backend can reach the index while it runs, including under CREATE INDEX CONCURRENTLY, because `index_concurrently_build` calls `index_build` and only *afterwards* sets `indisready`, so `aminsert` is not yet possible; and the test-only `bm25_debug_alloc_unknown_page`, which is `heaprel == NULL`/extend-only and REVOKEd from PUBLIC. | Without the extension lock, `smgrnblocks` and `smgrzeroextend` are not atomic and two concurrent extenders can claim the same block. So the metapage lock is load-bearing for extension correctness, not only for the pending anchor — narrowing either half needs a replacement that keeps extends single-threaded. What `bm25_page_alloc` **does** guarantee is that it never `table_open`s and never WAITS on another buffer's content lock (FSM candidates via `ConditionalLockBuffer`; eviction via `GetVictimBuffer`'s `LWLockConditionalAcquire`), which is what makes nesting it under the metapage LWLock deadlock-free. Two wrong versions of this are on record — "never blocks" (too strong) and "it may wait on the relation-extension lock" (false in the other direction, and briefly shipped) — both kept in the source comment so neither returns. `docs/adr/0083`. |
| **The "release the page lock before `RecordFreeIndexPage`" rule is about the FREE side only** | A page being handed to the FSM must not still be held: stamp it `BM25_PAGE_DELETED` under its EXCLUSIVE lock, release, THEN `RecordFreeIndexPage` (`bm25_pending_truncate`, `bm25_fsm.c`'s `reclaim_one_range`). | So a concurrent `bm25_page_alloc` that pops the block can take its content lock and read the stamp. It is NOT a blanket ban on FSM traffic under a page lock: the ALLOCATE side deliberately runs `bm25_page_alloc` (`GetFreeIndexPage` + `RecordFreeIndexPage` requeue) under the metapage lock, because the page count a document needs is not knowable outside that lock, surplus pre-allocations cannot be returned without an extra WAL record per insert, and releasing the metapage between a document's parts would break the consecutive-records contract the drain reassembles on. `docs/adr/0083`. |
| **Single chain owner** | The segment reader was created once (Task 5) and only modified after; since `docs/adr/0101` (#228) it is FOUR files — `bm25_seg_read.c`, `bm25_seg_dict.c`, `bm25_seg_chain.c`, `bm25_seg_debug.c` — the three new ones created once by that split, and all four only modified after. `OBJS`/`REGRESS` lines are append-only, each entry added once. No "catalog must fit one page" guard exists (the catalog is chained + `segcat_root`-swapped; the seal publish PREPENDS a page when the root fills — `docs/adr/0017` — and, since one seal can publish many segments at once, a whole CHAIN when one page is not enough). A guard of that shape was briefly reintroduced by #146's N-entry publish and had to be removed: it fired only after every chunk was already on disk, and the abort left the pending anchor untouched so the next seal repeated it, stranding orphan pages that `bm25_vacuumcleanup` never reached because it seals BEFORE it reclaims. `docs/adr/0084`. | Re-creating any of the four reader files duplicates symbols (link failure); a stray one-page-catalog guard wrongly rejects a chained catalog. |
| **`key_field` is structural; a change is refused at INSERT** | `bm25_insert` calls `bm25_validate_key_config_for_insert` BEFORE the row reaches the pending chain, erroring when the resolved (`key_type`, `key_size`, `key_attno`) disagrees with the **key-identity stamp** the build wrote on the field-config page (`BM25KeyStamp`, #292, `docs/adr/0112`; both `ambuild` and `ambuildempty` write it, so every build path — CONCURRENTLY and REINDEX included — stamps). The stamp is checked whether or not the index has segments, and names the column, so it covers the empty-catalog window (create-then-load) and the same-type column swap. An UNSTAMPED index (built before #292) falls back to the old check: type/width against the first segment's KEYMAP, nothing while there is none. Backstop: `bm25_pending_drain` ERRORs (REINDEX hint) on a pending chain whose live records disagree on (type, width), keyless included, rather than seal it — that wedges seal/VACUUM for an index already mixed, by decision, since its queued rows are mis-keyed anyway. | `AccessExclusiveLock` on the reloption sets the LOCK LEVEL for `ALTER INDEX ... SET` (only once the backend running the ALTER has registered the option; from a cold backend it takes just `ShareUpdateExclusiveLock`, `docs/adr/0012` addendum), not a prohibition, and `key_field` was re-resolved from LIVE reloptions every row — so post-ALTER rows carried a new key type into the chain and the next seal published a KEYMAP disagreeing with every existing segment, which the merge then silently re-keyed to whichever it read first. NOT at DDL time (`amoptions` never receives the index, so it cannot compare) and NOT at seal time (the divergent rows are queued by then, so erroring WEDGES the index — every later seal, autovacuum's included, fails identically with REINDEX the only exit). Residual (unstamped indexes only): a same-type same-width column swap, and any change before the first seal short of the drain's type/width refusal; REINDEX stamps. `docs/adr/0067`. |
| **Indexed columns are type-gated before any Datum is touched** | `bm25_check_indexed_column_types` runs at the TOP of `bm25_build` and the TOP of `bm25_insert`, comparing the BASE type against text/varchar. | Both paths do `DatumGetTextPP(values[f])` with no type test, so a non-text column dereferences a non-varlena — `int4` treats the integer as a pointer (SIGSEGV, postmaster crash-restart). Core's opclass resolution blocks the ordinary case; the reachable one is an explicitly-named opclass `FOR TYPE` something else. NOT inside `bm25_resolve_fields`: on the insert path that runs AFTER the tokenize loop has already dereferenced. BASE type, because a DOMAIN over text is legitimately indexable and comparing the raw `atttypid` broke existing domain indexes on a pure binary upgrade. `docs/adr/0068`. |
| **A stranded pending continuation is tolerated, not asserted** | `bm25_pending_drain` drops a `BM25_PENDING_DOC_CONT` part whose parent header is absent, and does NOT assert. Every scan-side walker that reassembles a document across its parts drops it the same way, by the same test (does this continuation's TID match the record that opened the document?) — `pending_stats_by_field`, `bm25_pending_score_term`, `pending_df`, `pending_df_by_field`. `pending_global_stats` aggregates per document too but needs no TID: every part repeats the whole document's `doclen`, so a blanket "skip `BM25_PENDING_DOC_CONT`" is exactly right there and exactly wrong in the other four. | An ordinary cancelled VACUUM produces it: parts live on separate pages, `mark_dead` commits one WAL record per page with its interrupt point at the top of the NEXT iteration, and PostgreSQL has no undo. The old `Assert(false)` PANICked the cassert `hardening` build on a state the branch exists to tolerate (reproduced: signal 6). Dropping is CORRECT — invalidation proceeds in chain order, so a surviving continuation implies the whole document was judged dead. `docs/adr/0069`. |
| **Every WHERE key is applied; the result is the SQL answer (#290)** | `bm25_gettuple` sets `xs_recheck = false` and the planner removes index clauses from `qpqual`, so a key the scan does not apply is applied by nothing. `bm25_rescan` parses every `@@@` key (an invalid RHS raises there); a key byte-identical to the scan's own query (the ORDER BY key, else the first WHERE key) gets no set of its own, the rest go to `so->where`; a skipped key's condition still applies (`so->where_has_orderby` makes the filtered build intersect the ORDER BY set into the WHERE set). With `so->nwhere > 0` the ranking build takes `bm25_scan_build_filtered` (`bm25_scan_rank.c`): one snapshot, the exhaustive scorer for every set (never WAND, so never capped and never tail-rebuilt), result = WHERE intersection in ORDER BY order, then the WHERE rows the ORDER BY query does not match at score `-Infinity` (distance `+Infinity`; `bm25_score_is_unmatched`, the score accessors answer NULL for them). Any NULL WHERE key = no rows. `norderbys > 1` still ERRORs. | The old contract read only `keyData[0]`/`orderByData[0]`: ORDER BY won over a different WHERE (a plan-dependent wrong answer, `{2,3}` vs the seqscan's `{1,3}`), and `nkeys > 1` was refused. `xs_recheck = true` is not a fix (english DEFAULT analyzer). Plain intersection is not the SQL answer (drops the unmatched WHERE rows). The field-scope idiom now scopes in the WHERE too (`sql/38_phrase`). `docs/adr/0109` (supersedes 0021). |
| **`SK_ISNULL` before any key datum** | A runtime key that evaluates to NULL arrives as `sk_argument == (Datum) 0` with `SK_ISNULL` raised, and `index_rescan` is called anyway. `bm25_rescan` tests the flag first, ASYMMETRICALLY: a NULL `@@@` key means no row can match (`bm25_match` is STRICT) → empty result; a NULL `&@@` key means the rows still qualify and only their order value is unknown → fall through to the unordered membership scan, exactly as GiST treats a NULL KNN key. | Dereferencing `(Datum) 0` in `DatumGetTextPP`/`DatumGetJsonbP` is a SIGSEGV that restarts the whole cluster, from unprivileged user SQL. A literal `@@@ NULL` const-folds away (STRICT), so only a runtime key — generic plan, or a parameterized nested-loop inner scan — reaches it. Short-circuiting the `&@@` case to empty instead would trade the crash for a silent wrong answer. `docs/adr/0015`. |
| **The analyzer identity is re-checked at INSERT, not just at scan start** | `bm25_insert` reads the metapage and calls `bm25_fingerprint_gate` with `BM25_GATE_INGEST` BEFORE it tokenizes, comparing the analyzer it just resolved against `meta.analyzer_fingerprint`. ERROR by default; WARNING-and-proceed under `require_analyzer_match = false`, memoized once per `(index, transaction)` so the deferral path does not emit a warning per row. | `INSERT` is not one of the maintenance commands PG17's `RestrictSearchPath` covers, so it resolved `<lang>_stem` through the CALLER's `search_path` — and `ALTER INDEX ... SET (language=…)` reaches the same place with no shadowing at all. (`language` gained a validator in `docs/adr/0082`, which does NOT close this: it rejects a name whose dictionary does not resolve, and a legal edit to a real language still moves the analyzer under a built index.) Either way the row was committed with terms the index's own analyzer does not produce and stayed unfindable *until REINDEX*: durable, unlike the scan side's one-session wrong answer. A restricted `search_path` for ingest would have covered only the first vector. Consequence to know: an un-migrated index is read-only, not merely unreadable, and the format-version floor now binds before the all-NULL early-out. `docs/adr/0081`. |
| **Term length is capped at ingest** | A term is stored INLINE in its `BM25DictEntry`, and `chain_write` requires the whole record to fit one page. `bm25_analyze` drops any run or lexeme longer than `BM25_MAX_TERM_BYTES` (2047, core FTS's own limit) with a NOTICE — checked on the raw run AND on each emitted lexeme, since a user-chosen `stem_dict_oid` (thesaurus/synonym) can EXPAND. `chain_ensure` re-checks `CHAIN_PAGE_CAPACITY` as a hard `ERROR`, not an `Assert`. | An uncapped alphanumeric run (base64/hex blob, DNA string) makes the DICT writer `memcpy` past the end of an 8 KB shared buffer and WAL-log `pd_lower > pd_upper` — out-of-bounds write from plain `CREATE INDEX`. The `Assert` that used to "guard" it is compiled out of every production build. `docs/adr/0014`. |
| **On-disk validation scope (D1)** | A checksum-valid corrupt page of any origin must produce `ERRCODE_INDEX_CORRUPTED` (XX002), never a crash, an out-of-bounds access, an unbounded or uncancellable wait, a write into a page of another kind, or the freeing of a reachable page. A silently wrong answer from in-range values is caught only where the decode boundary has a cheap structural invariant (count, order, kind, gen, span); otherwise it is a residual named in `docs/adr/0119`. "Cheap" means nothing per posting or per document that costs a hash probe, a visited set, an allocation or a syscall; a hot-path check lands only after an A/B in the regime it affects is clean. | The first sentence is what a fix must deliver; the second is what keeps a reviewer from re-litigating every in-range wrong answer. A new on-disk value or walker that cannot meet the first sentence is a defect; one that leaves a second-sentence gap must name it at the code and in ADR 0119. |
| **Every sealed-chain walk reads through `BM25SegWalk`** | DICT, POST, POS, LIVE, DOCMAP, NORMS and KEYMAP walks call `bm25_seg_walk_read`/`_get` (or `_check` for a block handed on unread): block 0, extent, revisit (first, previous or root page), visit cap at `nblocks`, content bytes before the opaque, gen, kind. Each family adds its invariant: DICT non-empty pages in strict cross-page term order (plus within-page order and `nterms` on the merge feed and dumps), POST progress and `(docid, field)` order, KEYMAP full span. Outside it by design: `chain_read_at`'s single-page image hit (re-checks content, gen and kind on the image), and the POST header reader and WAND decoder, which step `(blk, off)` pairs rather than walk pages and apply block 0, gen (through `bm25_seg_gen_mismatch`), kind and the header-order rules themselves. `docs/adr/0120`. | A walker with its own hand-rolled checks drifts: before #303, block 0 read the metapage and raised a retryable 40001, cycles spun until cancelled, and a link back to a dense chain's root answered from the wrong page. |
| **A gen mismatch on a live segment is corruption** | `bm25_seg_gen_mismatch` raises XX002 when the walk's own segment is still in the catalog and 40001 only when it has left it. It reads the catalog through `bm25_scan_snapshot` (metapage SHARE held across the walk), never through `bm25_segcat_find_entry`, and skips the read when the backend holds the singleton. It must be called with no content lock held. | Retirement removes a catalog entry before any page is reclaimed and gens never repeat, so a live segment never owns a page with another gen. A catalog read without the metapage lock can meet a reused catalog page on a standby and turn the legitimate 40001 into XX002 (`docs/adr/0107`). |
| **No subtransaction on a standby read path** | In recovery, `bm25_scan_build_ranking` calls `bm25_scan_build_ranking_once` directly; only the primary wraps it in `BeginInternalSubTransaction` and retries (`docs/adr/0121`). A read path that runs on a standby must not add an internal subtransaction around code with `CHECK_FOR_INTERRUPTS`, and must not catch 40001 to retry. | Core resolves a recovery conflict with an ERROR only outside a subtransaction; inside one it terminates the session (FATAL). Core's own conflict cancel is 40001, so a catch-and-retry would swallow exactly what core escalates to prevent. |
| **Every on-disk quantity is bounded where it is first trusted** | `bm25_meta_validate` bounds the METAPAGE's `field_count` (`[1, BM25_MAX_FIELDS]` — a struct distinct from the segment header's own `field_count`, which the posting-block decode boundary bounds separately); `bm25_dictentry_validate` (shared by all five DICT walkers, incl. the two `bm25_segment.c` debug SRFs) bounds a DICT entry's header fit and `MAXALIGN(sizeof+termlen)` term span; `bm25_seg_keymeta_validate` bounds a KEYMAP header's `key_type`/`key_size` PAIR to the exact width the type implies; `bm25_seg_key_header_validate`/`bm25_seg_key_span_validate` bound `key_size`, the `pagebytes < sizeof(hdr)` underflow, and each key's per-page span; `bm25_accum_set_keymeta` bounds the accumulator's `key_size` magnitude before it sizes a `palloc0`; `bm25_accum_set_doc_key` rejects a later doc's disagreeing `key_size` (`ERRCODE_FEATURE_NOT_SUPPORTED` + REINDEX hint — DDL-reachable via `ALTER INDEX ... SET (key_field=...)` between inserts, not corruption; since #292 the drain's mixed-chain refusal fires first for pending input and this is the memcpy-bounding backstop); `bm25_merge_docid_validate` bounds a merge source posting's `old_docid` against the segment's own `ndocs` before it indexes `idmap[]`; `bm25_retired_page_flags_validate`/`bm25_retired_count_validate` bound a retired-list page's kind and entry count; `bm25_accum_add_posting`/`bm25_accum_add_positions_to_last` bound the merge-replay `field_id` against the accumulator's `field_count` (the field RLE decode bounds it to `BM25_MAX_FIELDS`, but the per-doc arrays are only `field_count` wide, so `[field_count, BM25_MAX_FIELDS)` is the gap). Since #137 the page KIND is validated too (`bm25_seg_page_validate_kind`, `bm25_pending_iter_begin`, and `bm25_pending_truncate`'s kind + unknown-kind + cycle guards) -- a different axis from all of the above, which bound VALUES decoded off a page rather than asking whether the page is the kind the walker expected. `docs/adr/0062`. Since #143 four more quantities are bounded: `bm25_seg_blkno_validate` gates every on-disk `BlockNumber` BEFORE `ReadBuffer` (`InvalidBlockNumber` IS `P_NEW`, so an unchecked corrupt pointer EXTENDS the relation from a read-only scan -- it deliberately does NOT bound against `RelationGetNumberOfBlocks`, `docs/adr/0071`); `bm25_seg_page_off_validate` bounds a DICT entry's `post_off`/`pos_post_off` before either becomes a page pointer; `bm25_block_validate` now bounds `impact_bytes` on both sides (`0` decodes to a zero-field table, which makes `bm25_block_ub` return 0.0 and the WAND driver prune the whole block -- silently missing rows, `docs/adr/0072`); and `bm25_block_last_docid_validate` cross-checks a block header's `last_docid` against its DECODED run, which `bm25_block_validate` cannot do because it runs before the deltas are expanded (`docs/adr/0073`). `bm25_segheader_span_validate` bounds a segment-header page against the struct AND its trailing per-field arrays before either is read, and `bm25_seg_keymeta_validate` is now called on the pending drain's WRITE path, not only by readers. | Each of these sizes a stack array, a `memcpy` length, a `palloc0`, or an array index straight off bytes read from a page — a torn page, a bit flip, or a hostile restored data directory turns any one of them into a stack/heap overflow or a `uint32` underflow wraparound instead of a clean `ERROR`. `docs/adr/0039`. |
| **Page content length is bounded before anything subtracts from it** | `bm25_page_content_bytes(Page pg)` is the single read-side derivation of a page's content byte count (`pd_lower - SizeOfPageHeaderData`) for every chain reader, DICT/SEGCAT/fieldcfg/retired-list walker, and POST-block reader in the tree — `chain_read_at`, `bm25_seg_dict_lookup`/`_iter_next`, `bm25_seg_key`, `bm25_pending_iter_begin`, `pos_cursor_open`/`_next_page`, `bm25_scan_snapshot`, `bm25_segcat_read`/`_find_entry`/`_locate_entry`, `bm25_fieldcfg_read`, all four retired-list walkers, and the three POST-block readers (`bm25_seg_scan_postings`, `bm25_seg_block_header_read`, `wand_cursor_load_block`). `pd_lower == SizeOfPageHeaderData` returns 0 and is NOT an error — that is `PageIsEmpty`'s own definition. The write-side chain/page builders reading back a page they themselves just initialized are the one deliberate exception. `chain_read_at`'s memcpy also gained a length-past-offset span check (the pre-existing start-offset bound alone did not cover it), and `bm25_pending_term_entry_span` closes the identical header+span gap for one on-page pending term entry. | `PageIsVerifiedExtended` bounds `pd_lower` from ABOVE only (`pd_lower <= pd_upper <= pd_special <= BLCKSZ`) — PostgreSQL deliberately admits `pd_lower <= SizeOfPageHeaderData`, down to 0, as a normal `PageIsEmpty`/uninitialized state. Every one of ADR 0027's and ADR 0039's decode boundaries was measured against an `end`/`pagebytes` derived from this UNCHECKED subtraction, so a corrupt `pd_lower` below `SizeOfPageHeaderData` underflowed it to near `SIZE_MAX` and made every downstream bound read "plenty of room" — silently dropping documents, terms, or postings rather than erroring: memory-safe, but answer-wrong. `docs/adr/0040`. |
| **A segment-catalog entry has ONE stride, pinned by assertion** | `BM25SegCatEntry` is addressed at the MAXALIGN on-page stride (`bm25_segcat_entries_per_page`, both writers' `pd_lower`, the `bm25_scan_snapshot` / `bm25_segcat_read` cursors) AND as a plain C array (`bm25_segcat_build_orphan_chain`'s single bulk `memcpy`, `bm25_segcat_find_entry` / `bm25_segcat_locate_entry`'s `ents[i]`, `bm25_livedocs_clear`'s `[catidx]`). `bm25_format.h` pins `sizeof == MAXALIGN(sizeof)` next to the size pin. A new site may use either form. | The two agree only while the struct is MAXALIGN-clean, which today holds because its `uint64` members give it `MAXIMUM_ALIGNOF` alignment and C pads `sizeof` to that -- adding a member cannot break it, NARROWING the counters to `uint32` can (on a 64-bit target, 28 bytes on a 32-byte stride: entry *k* read 4*k* bytes short). The size pin cannot see that, because a deliberate format change updates the number with the struct; the relationship pin fails to compile instead. `BM25RetiredEntry` needs no such pin: every site strides it by plain `sizeof`. `docs/adr/0094`. |
| **idf is strictly positive; `idf == 0.0` is a sentinel** | `bm25_idf` clamps `df` to `ndocs` (the single-field path sums RAW dict df against a LIVE `ndocs`, so `df > N` after enough deletes). Clamp the **df**, never the result: `idf_f[field] == 0.0` is the in-band "term absent from this field" sentinel that `seg_posting_cb`, `bm25_block_ub` and `bm25_wand_cursor_score_doc` all skip on. Every idf must come from `bm25_idf` — never re-derived inline. | A negative idf INVERTS `bm25_termscore`'s monotonicity in `tf`/`doclen`, so `bm25_block_ub` evaluated at `(max_tf, min_doclen)` returns the block MINIMUM: the "upper" bound sinks below real scores and WAND breaks out of the segment, silently truncating the ranking. Clamping the RESULT to 0 instead lands a present term on the absent-sentinel and empties the ranked scan entirely. `docs/adr/0016`. |
| **WAND bound safety (M2b)** | `bm25_block_ub` must be a TRUE upper bound on any real posting's contribution in its block: raw per-field impacts (never seal-time-baked) and the EXACT `bm25_termscore` formula (not an approximation). The bound owes **domination, not bit-exactness** — it is NOT required to reproduce the scorer's fold order, because two of the three accumulations provably cannot: the deep check sums over docid-sorted cursors, and the pivot sum folds `global_ub`, precomputed per-field maxima over blocks. **A per-block bound covers only the postings its block holds** (issue #289, `docs/adr/0113`): a document whose postings for a term straddle a block boundary (any count-sliced, i.e. pre-#289, multi-field segment) is dominated only by the sum of both blocks' bounds, so `global_ub` sums per-field maxima and the deep check adds, for a cursor whose block's last document continues into the next block and lies in the skip window, that document's postings there scored exactly (`wand_cursor_straddle_ub`; the next block's lead, `BM25BlockLead`, is read at block load when it is on the same page). The writer no longer produces straddles, but nothing on disk distinguishes old segments, so both reader measures stay for every segment. Instead both prune comparisons are widened by `wand_widen_ub`, a relative slack proportional to the summand count, always in the conservative direction. The ascending-`field_id` fold order is kept as a slack MINIMISER. | An unsafe bound (a baked score gone stale, or a regrouping that rounds below the real sum) lets a prune shallow-skip a block that actually contains a top-N doc — silent, undetectable data loss in ranked results. The widening's soundness depends on every summand being NON-NEGATIVE (see the idf row above); relaxing that would invert it into a narrowing. `docs/adr/0043`. |
| **WAND ≡ exhaustive (M2b)** | The WAND driver's output must be bit-identical to `bm25_scan_build_ranking_exhaustive`'s top-k prefix: the same named-`contrib` rounding barrier, `-ffp-contract=off`, the same accumulation order. | Any divergence — even 1 ULP — breaks the "acceleration, never a different answer" contract; `43_wand_parity`/`44_wand_skip`/`45_m2b_acceptance`/`t/012_v5_wand.pl` exist to catch exactly this. |
| **Phrase/AND/`@@@` never take the WAND path (M2b)** | `bm25_scan_build_ranking_once`'s gate excludes `so->qphrase` (and, transitively, `phrase_fallback='and'`) and `bm25_native.wand_top_k = 0`; a boolean `@@@` scan never reaches the ranking dispatcher at all. | WAND has no notion of positional recheck or AND-fallback; running it on a phrase query would silently skip the post-accumulation phrase match, returning matches the phrase operator never approved. |
| **Leaf cap → uint64 mask (M6)** | `bm25_query_flatten` assigns `leaf_bit` from one 0.. counter over ALL leaves and ERRORs if leaves > `BM25_QUERY_MAX_LEAVES`=64. Since #68 the same cap ALSO fires during parse (`bm25_query_count_leaf`, from `parse_leaf`), so a wide tree is rejected at the 65th leaf instead of after the whole tree is materialized; flatten keeps its own check as a guard for a future caller (it bounds the `leaves[]` write and `leaf_bit`), though every tree that reaches it has already passed the parse-time count, so it cannot currently fire (ADR 0099). The scan path must still call flatten (not just parse) before any `1<<leaf_bit` / `bm25_query_eval` — for `leaf_bit`, not for the cap. | 65+ leaves make `1<<leaf_bit` shift-UB; skipping flatten leaves `leaf_bit` unassigned and the presence mask garbage. |
| **Presence via `and_presence`, not `BM25AccEnt` (M6)** | The per-doc leaf-presence bitmask reuses the EXISTING `and_presence` HTAB / `PhraseAndEnt.mask` / `phrase_and_mark(cur_qi=leaf_bit)` (`and_presence` created whenever `fallback_and || boolean_mode`); no field is added to `BM25AccEnt`. | A parallel `BM25AccEnt.mask` would desync from the phrase/AND machinery that already owns the same bitmask; drain would evaluate a stale mask. |
| **Filter == ranked by construction (M6, #132)** | The non-scoring `@@@` path (`bm25_load_if_needed`) drives the SAME `bm25_scan_build_ranking` builder as `&@@` and copies its drained TIDs; it does not reimplement membership. Its delegation guard is `so->nwhere > 0 \|\| so->qphrase \|\| bm25_qtree_is_multileaf(so->qtree)` — a TEXT phrase / proximity query delegates for the same reason a tree does, and several WHERE keys (#290) delegate so every key's set comes from the same computation under one snapshot. Only a bare term / `field:term` (and a single MATCH/TERM jsonb leaf, made text-equivalent at parse) keeps the flat OR-union. | A separate filter evaluator (a flat OR union) diverges from the ranked set on `must_not`/nested trees AND on any phrase — `@@@` and `&@@` disagree on which rows match. #132 was exactly this: the guard tested only `so->qtree`, so `col @@@ '"a b"'` with no `ORDER BY` returned the OR of the phrase's tokens, silently, with `xs_recheck = false` leaving nothing downstream to catch it. The flat-OR path requests NO positions anywhere (NULL `pos_root`/`pos_off`, an Invalid position cursor, a pending walker that strides past `pos_bytes`), so a phrase can only be delegated, never rechecked in place. |
| **jsonb multi-leaf bypasses WAND (M6)** | The WAND gate excludes `bm25_qtree_is_multileaf(so->qtree)`; only a single MATCH/TERM jsonb leaf (text-identical) may take WAND. | WAND score-prunes before the drain-time `bm25_query_eval` runs, so it would drop docs a boolean/must_not formula keeps (or keep docs a must_not excludes) — a different answer, silently. |
| **Wildcard single-count dedup (M6)** | `bm25_dict_expand_wildcard` unifies matches across all segments + pending into ONE distinct term set before scoring under the leaf's single `leaf_bit`. | A term present in N segments scored N times under one bit inflates that leaf's contribution N-fold. |
| **Wildcards bypass the stemmer (M6)** | The wildcard pattern is matched RAW-folded against the byte-sorted STEMMED dict; it is never run through `bm25_analyze`. It IS folded through `bm25_fold_term`, the analyzer's own fold, and every length downstream of that fold (`memchr` for `*`, `prefixlen`, `pure_prefix`, both `bm25_glob_match` calls) is derived from the FOLDED length — the fold is not byte-length preserving, so pairing the folded buffer with the caller's original length measures the wrong bytes (`docs/adr/0046`). | Stemming `judg*` (or its prefix) produces a nonsense pattern that matches nothing or the wrong terms. Folding differently from the analyzer is the same class of silent miss: the byte-wise `tolower` this replaced agreed with the analyzer for ASCII and nowhere else, so an accented prefix matched zero rows. |
| **One definition of "word character" and "lowercase"** | `bm25_is_word_byte` / `bm25_next_char` / `bm25_fold_term` in `bm25_analyzer.c` are the only implementations; the analyzer, the wildcard expander and the snippet edge snapper all call them rather than testing bytes themselves. A multibyte character is a word character unconditionally (locale-independent), and so is every high byte in `SQL_ASCII`; in any other single-byte encoding a high byte is a word byte iff the generated per-encoding table `bm25_sb_wordclass` (`src/bm25_sb_wordclass.c`, written by `ci/gen_sb_wordclass.py` from PG 18.6's conversion maps and `pg_u_isalnum` at Unicode 16.0; unmapped bytes are separators) says so — never `LC_CTYPE`, never the server's own Unicode tables, never `pg_conversion`. Changing that table's bytes is an analyzer revision bump (#295, `docs/adr/0114`). | The index and the query must agree byte-for-byte or a term silently fails to match, and an excerpt cut where the tokenizer would not cut shows a word fragment. Four sites each had their own byte-wise version and had drifted apart; on BSD/macOS under a UTF-8 `LC_CTYPE` the disagreement escalated from a wrong answer to `ERROR: invalid byte sequence for encoding "UTF8"` at `CREATE INDEX`. Non-ASCII corpora need a `REINDEX` across this change. `docs/adr/0046`. |
| **`CHECK_FOR_INTERRUPTS` must sit in a genuinely lock-free instant** | `LWLockAcquire` calls `HOLD_INTERRUPTS()`, so a check anywhere a buffer content lock is held across iterations (the `ChainWriter`'s tail buffer, `bm25_seg_dict_iter_next`'s current DICT page) is dead code — verified by an adversarial review that caught 9 such checks in the first draft of the maintenance-interrupts pass. The same trap was still live at three DICT-entry loops until `docs/adr/0083` — the wildcard expander's per-entry check and the two `bm25_segment.c` debug SRFs' per-page checks — and all three were fixed the same way: the loop now decodes a page COPY with no content lock held, so the check fires. (The expander was NOT put on `bm25_seg_dict_iter_unlock`; see the row above for why that pair is unsound for a reader.) None of those three changed the interrupt COUNT, which is why the CI grep floor cannot witness the fix and `sql/80_maintenance_interrupts`' `bm25_debug_postings` latency harness pins it instead. `chain_ensure` (`bm25_seg_build.c`) finishes-and-releases the OLD tail page BEFORE allocating the NEW one, creating one real lock-free instant per page rotation; that is the ONLY cancellation point for every chain this file writes (DOCMAP/NORMS/LIVE/POST/POS/DICT). Never unlock a buffer still registered in an open `GenericXLogState` — the old page's own record is `Finish`ed before the backfill record's `GenericXLogStart` opens a new window. | A check placed inside a locked per-item loop instead compiles, links, and passes every test, while never actually firing — a large seal/merge/build then runs to completion regardless of `statement_timeout`/`pg_cancel_backend`, the exact defect ADR 0024 fixed for scans, silently reopened on the maintenance side. `docs/adr/0041`. |
| **One maintenance OPERATION is one atomic publish RECORD — not one segment** | The build, merge and drain accumulators are all cut at `bm25_maintenance_budget_bytes`, so each can produce several segments. Every one of those chunks is published in a SINGLE Generic WAL record: `bm25_segcat_publish_append` (seal/build — appends the whole entry array and resets the pending anchor together) or `bm25_segcat_publish_swap` (merge — publishes every new entry and retires every input together). Budget exhaustion SEALS; it never errors. Chunk boundaries are input-segment-granular in the merge and page-granular in the drain — never mid-document. | Atomicity was never the property that mattered; consistency of the INTERMEDIATE states is. `bm25_scan_snapshot` captures `pending_head` plus the whole live catalog under ONE metapage SHARE lock, and the exhaustive scorer SUMS per-TID contributions with no cross-source dedup (`bm25_scores_add`), so any committed state holding a partial output alongside its still-live source double-scores every doc in it and double-RETURNS it on the `@@@` path — durably, if a crash lands there. The mirror-image ordering (retire inputs first) deletes live documents. Erroring instead of sealing would wedge the index: an error out of `amvacuumcleanup` leaves it permanently un-maintained, and one out of the opportunistic seal fails every subsequent INSERT past the threshold. `docs/adr/0084`. |
| **The segment-count ladder has a memory floor, and it is operator-visible** | `BM25_TARGET_SEGMENT_COUNT` (8) is unreachable for a corpus whose accumulator footprint exceeds 8× the budget. The steady state floors at roughly footprint/budget segments and scan cost grows with it; `bm25_merge()` stops when a pass makes no progress rather than spinning. Remedy: raise `maintenance_work_mem` (or `autovacuum_work_mem`) and run `bm25_merge()`. | The alternative is the OOM this bound exists to remove — in an AUTOVACUUM WORKER, which has no user-visible failure path, so the index would simply stop being maintained. The real cure is a streaming k-way segment merge needing O(1) memory, which this format's sorted DICT chains and docid-ascending postings permit; it is a new segment builder, not a bug fix, and is recorded as future work. `docs/adr/0084`. |
| **A segment chain walk is held to the length its header implies (validated walker)** | `bm25_seg_scan_postings` and `wand_cursor_sweep_global_ub` hold a term's POST run to exactly the DICT `df`: a block claiming more postings than the run has left, a run ending (Invalid `nextblk`, or no block left on the last page) before `df`, a block whose decoded last docid differs from its header, and a block whose first `(docid, field)` does not sort strictly after the previous block's last `(docid, field)` all ERROR `ERRCODE_INDEX_CORRUPTED` with a REINDEX hint; `bm25_wand_cursor_next_geq` ERRORs on an Invalid peek link while postings remain. `chain_read_at` (LIVE, DOCMAP, NORMS) ERRORs when the walk reaches the chain's end before the requested offset, and on any page it walks past that does not hold `bm25_chain_full_span(kind)` bytes; `seg_livedocs_all_set` and `bm25_livedocs_locate` apply the same two rules, `bm25_livedocs_clear` bounds its byte by the page's content, `bm25_segheader_validate` rejects a missing LIVE/DOCMAP/NORMS root on a segment with documents, and `seg_docid_to_tid_cur` rejects a TID with block `InvalidBlockNumber` or an offset outside `1..MaxOffsetNumber` (not `MaxHeapTuplesPerPage`: bm25 does not restrict the table access method). The cross-block order check is lexicographic on `(docid, field)` and strict (#303, `docs/adr/0120`); on docid alone it must stay `>=`. WAND's header-only sweep and peek allow an equal `last_docid` only for a run's final block holding at most `BM25_MAX_FIELDS` postings (a straddle-only block). Checks run per block, run or page transition, never per posting; the TID check is a few comparisons per TID resolved. | The builder writes every one of these chains to an exact length, so a short one only ever means corruption, and each old fallback was a silently wrong answer: a short posting list on every non-WAND path, a tombstoned row read as live (and returned at its reused TID, `@@@` being authoritative), an invalid TID the heap fetch reads as `P_NEW` and extends the heap with from a read-only query, doclen 0. The merge replay reads through the same functions, so a merge used to launder the damage into a healthy-looking segment no validator could fault. Docid alone is `>=` because old count-sliced segments cut blocks every 128 postings and a multi-field document has a posting per field, so a document can straddle two blocks; a straddle continues the same document at a higher field, which is why the `(docid, field)` rule is exact for both layouts. `ItemPointerIsValid` alone is not the TID check: it tests only the offset. `sql/124_strict_chain_walkers`. Issues #293/#294; `docs/adr/0111`, `0095`, `0073`, `0100`. |
| **A chain-walk extent bound is not a cycle guard, and must fail loud, not silent** | `blk < nblocks` (added to several previously-unbounded chain walks: `bm25_pending_drain`/`_mark_dead`, `bm25_fsm.c`'s retired-list walkers, `bm25_analyzer.c`'s `bm25_fieldcfg_read`) stops a corrupt `nextblk` from walking PAST the relation's own extent; it does nothing for an in-extent CYCLE. Cycles are ended by visit caps and shape checks layered on top: every sealed-chain walk reads through `BM25SegWalk` (revisit test, visit cap at `nblocks`, family order invariants; #303, `docs/adr/0120`), the pending walkers through `bm25_pending_walk_read` (`docs/adr/0110`), the SEGCAT walkers cap visits and check link shape (see the landmines below), `bm25_fsm.c`'s retired-list walkers cap visits at `nblocks` and `bm25_reclaim_retired` also refuses a walk back to the list head, and `reclaim_one_range`'s refusal of an already-DELETED page stops a cycle in a freed range (#302). A walker whose extent sample can go stale while it runs must test the bound through `bm25_blk_in_extent`, which re-samples once before calling a link out of range: `bm25_pending_mark_dead` does (VACUUM's sweep holds the singleton in ShareLock mode, which appenders share, so they can extend the relation and link a new tail page after the sample, #243), as do the SEGCAT walkers. `bm25_pending_drain` runs under the ExclusiveLock singleton, which excludes appenders, and keeps the plain sampled bound. `bm25_pending_drain`/`bm25_pending_mark_dead` follow the bound with a loud `ereport(ERROR, ERRCODE_INDEX_CORRUPTED)` if the loop exits with `blk` still valid — the bound was hit, not a natural end-of-chain. | `bm25_seal_index`'s publish record resets `pending_head` on the premise that the drain it publishes consumed the WHOLE pending chain. A silently-truncated drain would publish a partial accumulator, advance the anchor past unread pending docs, and let `bm25_pending_truncate` free them — committed, heap-visible rows unfindable until REINDEX. Same loud-vs-silent principle as `docs/adr/0040`'s `pd_lower` underflow. `docs/adr/0041`. The same bound was extended to the four walkers in `src/bm25_seg_dict.c` and `src/bm25_seg_chain.c` that `bm25_segment.c`'s own comment had wrongly claimed were already covered: `bm25_seg_dict_lookup`, `bm25_seg_dict_iter_next`, `chain_read_at`, and `bm25_seg_scan_postings` (SEGREAD-11, `docs/adr/0095`) -- each ERRORs `ERRCODE_INDEX_CORRUPTED` rather than under-answering (a term reported absent, a short merge dictionary, a false-LIVE doc, a truncated posting list). `BM25ChainCursor` also gained a `root` field (SEGREAD-14) so a cursor resumed against the wrong chain falls back to a fresh root walk instead of reading a foreign chain at a meaningless offset. |
| **`INIT_FORKNUM` is written with `log_newpage_buffer`, never GenericXLog** | `bm25_meta_finish` branches on forknum: `MAIN_FORKNUM` keeps GenericXLog; `INIT_FORKNUM` uses `START_CRIT_SECTION` / fill / `MarkBufferDirty` / `log_newpage_buffer(buf, true)` / `END_CRIT_SECTION`, per page, matching core's `ginbuildempty`/`brinbuildempty`. `bm25_fieldcfg_write_init` does the same for the field-config page `bm25_buildempty` writes. No `smgrimmedsync` — FPI replay is the durability mechanism. | `GenericXLogStart` takes `isLogged` from `RelationNeedsWAL`, which requires `RelationIsPermanent` — FALSE for an unlogged index — so GenericXLog emits NO WAL record for an init fork, and `smgrimmedsync` flushes the file while the content is still dirty in shared buffers, i.e. the zeros `smgrzeroextend` wrote. `CREATE UNLOGGED TABLE` + `CREATE INDEX` + crash before the next checkpoint therefore left `ResetUnloggedRelations` copying zero pages over the main fork: every query failed `bm25_meta_validate`'s magic gate, and a promoted standby got "could not read block 0". Verified with `pg_walinspect`: 2 `XLOG/FPI` records now, 0 before. `t/016_unlogged_crash.pl` (crash reset) and `t/017_unlogged_replica.pl` (standby promotion) exercise the init fork, so this is now gated. `docs/adr/0042`. |
| **Nothing that can allocate, hash, or take a second buffer runs under a buffer content lock** | Two shapes, applied at every site. **(a) Copy-then-unlock — the default, and what every READER uses**: the four pending-page walkers (`pending_phrase_stash`, `pending_and_stash`, `bm25_pending_score_term`, `bm25_load_if_needed`'s inline block), the seal's own reader `bm25_pending_drain` (the one exception until #67 — its accumulator inserts ran under each page's lock; harmless only because the seal singleton excludes every production pending-page writer, a remote justification the rule does not need; ADR 0083 addendum), *both* passes of the wildcard expander (`bm25_dict_expand_wildcard`), and all three DICT-walking debug SRFs (`bm25_debug_terms`, `bm25_debug_postings`, `bm25_debug_segterms`) take one bounded `BLCKSZ` copy under the SHARE lock, release it, then run unchanged logic against the copy. **(b) Unlock/relock** (`bm25_seg_dict_iter_unlock`/`_relock`, term bytes copied out first — the iterator yields `term` pointing INTO the locked page): **the merge feed only**. `bm25_upgrade` derives `feature_flags` BEFORE taking the metapage lock, not inside an open GenericXLog window. | `palloc`/`repalloc`/dynahash growth is unbounded work, and an OOM inside it `ereport`s under the lock; holding a pending page's SHARE lock across it also stalls the pending appends and seals that contend for exactly that page. Nesting a postings replay (POST+POS locks) inside a held DICT lock stacks three content locks on one call path. The unlock/relock form is safe only while nothing can FREE AND REUSE the segment's pages mid-iteration — the pin prevents eviction, not content change — and it revalidates **nothing** on resume (the gen and page-kind checks run only at page crossings). Its precondition is the **metapage singleton and nothing weaker**. "The caller holds an ordinary MVCC snapshot covering the segment" is NOT a substitute: that argument rests on `GlobalVisCheckRemovableFullXid`, evaluated on the primary, so on a hot standby without `hot_standby_feedback` the startup process can replay retire→stamp→reuse over the pinned page inside the window and the resumed cursor decodes rewritten bytes. A draft of #145 made that mistake; the reader that prompted it now copies the page instead, which is what any new caller should do. `docs/adr/0042`, `docs/adr/0083`. |
| **A rejected FSM candidate is requeued after the loop, never inside it** | `bm25_page_alloc` collects rejected-but-reusable blocks in a growable array and calls `RecordFreeIndexPage` once, after the search loop exits. | Requeueing immediately hands the block straight back to the next `GetFreeIndexPage`. Pre-fix, every rejection strictly SHRANK the FSM, and that is what guaranteed termination. Immediate requeue busy-spins when a seal's `ChainWriter` holds a double-listed page EXCLUSIVE across a whole tail fill, and never terminates at all for `heaprel == NULL` callers with a valid-`retire_xid` DELETED page present. Not requeueing at all (the original bug) drains the FSM so the index extends instead of reusing. `docs/adr/0042`. |

## Landmines

- **The two ingest paths must agree on what is indexable, and one of them is easy to
  forget.** `INSERT` goes through the pending list; `CREATE INDEX` and `REINDEX` bypass it
  entirely and feed the accumulator. A limit added to one is not a limit. The per-document
  token ceiling (`PG_UINT16_MAX`, because `BM25PendingTermEntry.tf` is `uint16` on disk)
  lived only on the pending path until #158, so the same row was accepted or rejected
  depending on index-creation order — durably, since `REINDEX` rebuilt the bad state
  rather than surfacing it (`docs/adr/0079`).


- **A jsonb `term` leaf reaches the scorer by TWO paths, and a check on one is not a
  check.** `bm25_rescan_parse_jsonb` short-circuits a single MATCH/TERM root into
  `so->qterm` (the text-equivalent shortcut) and DISCARDS the leaf, so the leaf/token loop
  in `bm25_scan_build_ranking_exhaustive` never sees it. QRY-05's single-term rule had to
  be enforced at both. Anything else that must hold "for every leaf" has the same trap
  (`docs/adr/0078`).

- **Analyzer output changes are REINDEX events, and the constant that says so is
  `BM25_ANALYZER_REVISION`.** It is now 6. Revision 3 (`docs/adr/0077`): single-byte run
  splitting no longer consults `LC_CTYPE`, and the fingerprint gained the database
  ENCODING as a sixth component, because run splitting branches on it and a dump/restore
  into a differently-encoded database previously kept a fingerprint claiming its terms
  were still valid. Revision 4 (`docs/adr/0080`): `stemmer_id` is now a hash of the
  dictionary's schema-qualified name and template rather than its OID — output is
  byte-identical across that one, so the REINDEX it forces is conservative rather than
  corrective. Revision 5 (`docs/adr/0087`, issue #184): `bm25_analyze` advances position
  once per source RUN instead of once per emitted LEXEME, and deduplicates a run's
  repeated lexemes — the change revision 3 did not make (see below). Snowball English
  emits one lexeme per run, so an English-only index tokenizes byte-identically across
  this bump; a compound-splitting dictionary (`ispell`/`hunspell`) does not. That is the
  only affected class in practice: a thesaurus is the other multi-lexeme template, but
  `bm25_analyze` calls lexize with a NULL `DictSubState`, so one fails loud (`forbidden
  call of thesaurus or nested call`) rather than indexing. **This is a genuine
  tokenization change, unlike revision 4's re-encoding, and that distinction bounds what
  `require_analyzer_match = false` may safely defer.** The reloption is a sound way to
  defer REINDEX across revision 4 specifically because output was byte-identical there
  (ADR 0080). It is NOT safe across revision 5 for an index over a multi-lexeme
  dictionary: deferring means scoring and phrase-matching against an index whose stored
  terms and positions disagree with the analyzer answering the query, which is wrong
  results, not merely stale ones. REINDEX is the only correct response. Revision 6
  (#295 + #296, one bump so users pay one REINDEX): in a single-byte database a high
  byte the encoding maps to a letter or digit is now a word byte (`SQL_ASCII`: every
  high byte), a genuine tokenization change there and nowhere else; and the fingerprint
  gained the dictionary probe as a seventh component, which moves every stored value.
  The probe runs the dictionary over its word list whenever a fingerprint is computed
  uncached, so a dictionary that cannot lexize at all (a thesaurus as `<lang>_stem`, see
  above) now fails at `CREATE INDEX` and at every gate site even with no rows. Residual: a
  combining mark the generator does not classify as alphabetic stays a separator in
  WIN1258 and WIN874, so decomposed text splits there (`ci/gen_sb_wordclass.py`'s Rules,
  `docs/adr/0114`). Bump
  `BM25_ANALYZER_REVISION` on ANY change to `bm25_analyze`'s output for fixed reloptions,
  or on any re-encoding of an existing component's value; do not bump for a refactor that
  provably preserves both.

- **A shadowing `<lang>_stem` is DETECTED, never prevented.** Resolution is by
  unqualified name through the caller's `search_path`; since PG17
  `CREATE INDEX`/`REINDEX` run under a restricted `pg_catalog, pg_temp` path, so the
  build side always binds pg_catalog's dictionary while a scan or an insert binds
  whatever the session reaches. `bm25_analyze` still tokenizes through the RESOLVED
  OID; the gate refusing first is the whole defence. `INSERT` had no such gate until
  #188 (`docs/adr/0081`) — it is not a maintenance command, so it resolved under the
  caller's path and stored wrong-dictionary terms silently, and the row was then
  invisible to a correct query *until REINDEX*, which is why that half was the durable
  one. Note the fix is a fingerprint comparison rather than a restricted `search_path`
  for ingest: a post-build `ALTER INDEX ... SET (language = …)` reaches the identical
  corruption with no shadowing anywhere, and name-resolution pinning cannot see it.
  What is still NOT detected either way is a dictionary's OPTIONS changing under a
  stable name/template — see the "STILL NOT COVERED" list in `src/bm25_analyzer.c`.

- **Co-positioning a compound run's lexemes to match core FTS could not land as a
  standalone change — it had to ship together with its query-side counterpart, and it
  has been tried the other way and silently broke phrase search.** `bm25_analyze` used
  to advance position once per LEXEME where core advances once per RUN, so positions
  genuinely drifted from `to_tsvector`'s under a compound-splitting dictionary — but the
  document side is only half the pipeline. The query side used to expand a phrase into
  one matcher term per query lexeme, and `bm25_phrase.c` requires strictly increasing
  document positions, so N co-positioned doc lexemes could never fill N slots: phrase
  queries over compound words returned ZERO ROWS (`docs/adr/0077`; the divergence and the
  phrase guard are both pinned in `sql/99_query_semantics`'s history).
  **Landed as issue #184, in the order the 0077 lesson forced.** The query-side slot
  model shipped first, deliberately inert (`docs/adr/0085`): a phrase slot is one source
  WORD, `bm25_phrase_slot_map` groups the query's lexemes by the analyzer's own
  positions, and `phrase_recheck_tid` merges each slot's member lists before calling the
  matcher. Run-count doclen shipped second, likewise inert (`docs/adr/0086`): a field's
  length is `max(position) + 1`, computed in the one place
  `bm25_accum_add_field_tokens`, and the pending list STORES it per field (format v8)
  instead of re-deriving it as a sum of `tf` — needed so a seal cannot silently move a
  compound document's score by switching it from a lexeme count to a run count. Both were
  inert only because position still advanced per lexeme, making every slot one token and
  the pending-vs-sealed doclen identity hold trivially. The third PR made them live: the
  emission line itself (position stamped once per run, bumped only if the run emitted a
  token), within-run dedup by exact term bytes (a compound's variants repeat lexemes —
  scoped to the run, not the document, so the same term in two different source words is
  still two occurrences), and the `BM25_ANALYZER_REVISION` 4→5 bump, all in one change so
  users pay for one REINDEX (`docs/adr/0087`). See "Slots, not tokens" above for the
  landed semantics and the two divergences from core that remain deliberately out of
  scope, and the analyzer-revision entry above for the `require_analyzer_match`
  consequence.

- **Redefining doclen as a run count silently invalidated what the merge memory-budget
  estimator was charging for, and unwinding it took two further changes -- the first
  carrying the on-disk field while deliberately leaving the estimator alone, so that
  "no estimate moved" was provable before the estimate moved.**
  `bm25_accum_estimate_bytes` predicts accumulator residency for replaying one segment,
  so the merge selection trim can refuse a candidate set that will not fit
  `maintenance_work_mem`; its dominant term charged `total_len × live_frac ×
  (sizeof(AccumPosting) + (has_positions ? 4 : 0))` — 32 bytes per posting, 4 bytes per
  stored position, one position per token. Runs and tokens coincided until the previous
  entry's revision-5 flip made `total_len` a run count while both charged quantities
  still scale with tokens, so a compound-splitting dictionary (tokens ≈5x runs for
  `ispell_sample`) under-estimated by that factor; Snowball English, one lexeme per run,
  was and is unaffected. Bounded rather than dangerous — `BM25_ACCUM_SLACK_FACTOR`
  absorbs the first 2x, `bm25_accum_over_budget` measures actual residency rather than
  trusting the estimate, and the merge executor's progress check backstops termination
  regardless of the estimate's quality — so the visible cost was one no-op index rewrite
  per autovacuum for a compound-dictionary index sitting at the merge budget floor, which
  is why `docs/adr/0087` recorded it as follow-up rather than folding a third format
  change into an already three-stage landing.

  **Resolved by `docs/adr/0088`, in two units.** This unit supplies the missing
  quantity: `BM25SegCatEntry` and `BM25SegmentHeader` each already carried a 4-byte
  padding hole (see the `memcpy`/padding entry below), now named `total_tokens` with
  `sizeof` unchanged (40/64) — ADR 0009's additive shape (iii), no `min_read_version`
  bump. The header's copy is the immutable master; the catalog entry's copy decays per
  tombstone exactly as `total_len` already does. The accumulator counts it at both
  ingest entry points (`bm25_accum_add_field_tokens` adds `ntok`, `bm25_accum_add_posting`
  adds `tf`, counted before its repeat-merge early return so a folded-in token is not
  lost). **Trust is gated by `BM25_FEAT_SEGCAT_TOKENS` (feature_flags bit 3), stamped
  ONLY by a fresh build** (`ambuild`/`ambuildempty`) — **never by `bm25_upgrade` and
  never derivable by `bm25_derive_feature_flags`**, because the bit asserts a fact about
  the WRITER (every catalog entry this index will ever hold came from a counter-aware
  binary), not about content, and `bm25_upgrade`'s empty transform registry means an
  accepted version gap takes an in-place restamp with no segment rewrite — conferring the
  bit there would bless pre-ADR-0074 stack residue as a genuine token count. REINDEX,
  not `bm25_upgrade`, is the only path that earns an existing index the bit.
  **The estimator was left UNCHANGED in that first unit** — `bm25_accum_estimate_bytes`
  still charged from `total_len` alone, so the field landed inert and no estimate moved,
  with `expected/104_build_memory_budget.out`/`105_merge_memory_budget.out` as the
  byte-identical proof. The second unit made it consume `Max(stored_tokens, total_len)`,
  and `Max` is the safe direction given the error is asymmetric: over-estimating
  costs a refused merge that would have fit, under-estimating costs the rewrite loop this
  work exists to remove — which is also why the counter sums tokens (`tf`) rather than
  postings: tokens over-estimate the postings term (tokens ≥ postings) but are exact on
  the positions term, while postings would invert that trade. Existing indexes gain
  nothing until REINDEX — deliberate, and cheaper than it looks, since analyzer revision
  5 already forces REINDEX on exactly the compound dictionaries the estimate matters for.

  **A third defect in the same expression, found by review and fixed separately: the
  live-document discount was applied TWICE.** `bm25_livedocs_clear` — the only tombstone
  path — decays the catalog entry's `total_len` (and, since 0088, `total_tokens`) in
  place in the same WAL record that decrements `live_ndocs`, so what the estimator reads
  has already had the dead documents removed; it then multiplied by `live_ndocs / ndocs`
  on top. Measured at ~10% low on a segment 90% live, worsening as `live_ndocs` fell, and
  in the expensive direction. **The fix had to be on the estimator side**: the entry's
  `total_len` is folded into the metapage's corpus-wide `total_len`, which is `avgdl`'s
  numerator, so a segment that stopped decaying would leave dead documents' lengths in a
  scoring statistic — a wrong ranking answer traded for a tighter memory estimate. That
  makes the entry's counters being **live-scaled by contract** an invariant the estimator
  now leans on, and `sql/105` asserts that premise explicitly next to the equality that
  pins it (two indexes with identical live content, one tombstoned, must cost the same).

- **A truncated hash key makes collisions selectable by whoever supplies the input.** The
  accumulator keyed terms on their first 255 bytes, so N terms sharing a 255-byte prefix
  collapsed onto one entry and drove an O(N^2) linear fallback that had no interrupt check
  anywhere in the file. `BM25_MAX_TERM_BYTES` (2047) does not bound it — it is eight times
  the key width, so the whole collision class fits inside a legal document. ADR 0076 then
  keyed long terms on a 32-bit `hash_bytes` of the full term, and a birthday search aimed
  that too (#305), so the map now keys on (pointer, length) over the whole term with no
  fallback at all. The same shape to watch for anywhere else: a fixed-width key filled
  from variable-length user input — whether by truncation or by a short hash.


- **A `memcpy` of a stack struct onto a page carries padding that an in-place field fill
  never writes.** This was the whole of #144's one real leak: the seal built its
  `BM25SegCatEntry` in place on a `PageInit`'d page and was clean, while the merge built
  the same struct on the stack and `memcpy`'d it — undoing the page's zeroing for the
  4-byte hole at offset 4, below `pd_lower`, inside a `GENERIC_XLOG_FULL_IMAGE` record.
  `palloc0` on the destination array does NOT fix it; the whole-struct assignment
  overwrites the zeroed padding. The `memset` must be on the SOURCE — **and every copy
  that carries it must be a `memcpy`**, because C does not require a struct assignment to
  copy padding at all, so a compiler that scalarizes the copy would drop the zeros again.
  Every on-page struct built ON THE STACK gets a `memset`, and every hop from there to the
  page is a `memcpy` (`docs/adr/0074`); the in-place writers (the seal's catalog entry, the
  segment header, `bm25_meta_fill`) deliberately do neither and rely on the `PageInit` in
  their own WAL window instead. **ADR 0088 retires this class for `BM25SegCatEntry`
  rather than merely guarding it**: its 4-byte hole is now the named `total_tokens`
  field, assigned by every writer including the merge's stack copy, so there is no
  longer an implicit hole for residue to hide in — the `memset`/`memcpy` discipline above
  stays as defence against a future member reorder reintroducing one, not as the primary
  defence. `BM25RetiredEntry` still has its hole (offset 36) and still depends on that
  discipline entirely. `sizeof(BM25SegCatEntry)` / `sizeof(BM25RetiredEntry)` stay
  `StaticAssertDecl`-pinned regardless — `BM25SegCatEntry` because it is a packed
  fixed-stride array with no per-record length prefix (an old reader desyncs at record
  two), `BM25RetiredEntry` for that reason AND because it still has no named pad member;
  `BM25_RETIRED_PER_PAGE` is derived from the latter. `sql/96_wal_page_determinism`'s
  segcat probe changed accordingly, from asserting these four bytes are zero to asserting
  they equal `bm25_debug_segcat`'s reported `total_tokens` for the same entry — strictly
  stronger, since zero-checking could only ever catch a writer that left them alone, and
  this catches one that leaves them wrong.


- **`InvalidBlockNumber` is `P_NEW`, so an unvalidated on-disk block pointer GROWS the
  relation instead of erroring.** This is the reason `bm25_seg_blkno_validate` exists and
  the reason it must be called *before* `ReadBuffer`, not after. Anywhere a `BlockNumber`
  comes off a page — a `BM25SegCatEntry.header_blkno`, a DICT `post_root`, a page-opaque
  `nextblk` — reading it unchecked is a silent relation extension from a read-only scan,
  followed by a *retryable* serialization error (the fresh zero page fails the gen half of
  the kind check) that `bm25_scan_build_ranking` swallows and retries up to three times.
  The symptom is a growing index and a misleading "segment reclaimed concurrently"
  message, neither of which points at the corrupt pointer. The validator does not bound
  `blkno` against `RelationGetNumberOfBlocks`, for cost rather than correctness: that call
  `lseek`s on every invocation in a normal backend (`smgrnblocks_cached` consults its cache
  only under `InRecovery`), and this validator runs once per posting-block load. If you do
  want the extent bound, copy `pos_cursor_load`'s shape — capture `nblocks` once at cursor
  open — rather than paying a syscall per block (`docs/adr/0071`).
- **A SEGCAT walker samples `nblocks` BEFORE the chain's root is even read, so its first
  extent check can be a false positive — it must RE-SAMPLE once, not error immediately
  (#225, `docs/adr/0095`).** Only `bm25_scan_snapshot`, `bm25_segcat_read` and `bm25_segcat_first_entry` raise
  `ERRCODE_INDEX_CORRUPTED` (SQLSTATE `XX002`, "segment catalog chain leaves the index")
  when a page falls outside the extent, via the shared `segcat_extent_validate` helper.
  `bm25_segcat_find_entry` (test/debug-only) and `bm25_segcat_locate_entry` stop their walk quietly instead
  (they fall out of the loop and hit their own ordinary "not found" path) — WITHOUT the
  re-sample, a stale-low sample would turn a healthy catalog into a spurious `find_entry`
  "gen not live" (false), or a `locate_entry` "not found in catalog" error, for an entry that is
  actually there. But a seal/merge
  extends the relation with `bm25_page_alloc` and only THEN flips `segcat_root` under the
  metapage EXCLUSIVE, so a walker that captured `nblocks` before that flip can see a
  healthy, freshly-published root past its own stale sample. `bm25_blk_in_extent`
  therefore re-samples `nblocks` exactly once on a would-be violation and errors only if
  the block is still out of range against the FRESH extent — ordered so the re-sample
  follows the root read, which follows the flip, which follows the extension. Getting the
  order wrong (erroring on the first sample, or re-sampling before the root read) turns an
  ordinary concurrent seal into a spurious corruption error on every ranked query that
  races it. `bm25_debug_segcat_walk` drives each walker with a caller-chosen stale sample
  to exercise exactly this path. The helper is shared: the pending walkers in
  `bm25_pending.c` use it too, for a different reason (appenders keep extending the
  relation while VACUUM's sweep walks, #243, `docs/adr/0095` addendum).
- **The SEGCAT walkers check link SHAPE, not only extent, and the checks rest on the
  catalog's packing (#244, `docs/adr/0095` addendum).** All four walkers raise
  `ERRCODE_INDEX_CORRUPTED` for: a page of the wrong kind (checked on every page read); a
  page with no entries that links onward; a walk longer than `nsegs + 1` pages (checked
  before `ReadBuffer`); and, in the two copying walkers, a generation listed twice. Each
  catches a shape the others miss, so do not drop one as redundant: a cycle through
  entry-bearing pages copies duplicates until the count is met and a scan answers from
  duplicated segments with no error; a cycle through empty pages in `bm25_scan_snapshot`
  spins under the metapage SHARE lock, whose `LWLockAcquire` holds interrupts, so it cannot
  be cancelled and every writer needing the metapage EXCLUSIVE queues behind it; a packed
  root linked to itself passes both of those and only the duplicate-generation check sees
  it. The zero-entry check and the cap assume every linked catalog page has at least one
  entry except a lone empty root or, after a chained publish prepends in front of it, the
  chain's last page; a change to how catalog pages are packed must revisit them. The
  threat model is on-disk corruption only. A link to a segment HEADER page (also SEGCAT
  kind) is refused by the catalog role check, `seg_gen == 0` (`bm25_segcat_page_validate`,
  #302/#303). Not caught, a residual (#276): a corrupt `meta.nsegs`, which every guard
  trusts as its bound and as an allocation size. The unlocked walk that #270 found is no
  longer reachable from INSERT: the key check
  reads entry 0 through `bm25_segcat_first_entry` under the metapage SHARE, and production
  callers of the unlocked walkers must hold the seal/merge singleton (the landmine row
  above, `docs/adr/0107`). `first_entry` reads one page, so it has no visit cap or duplicate
  check; the INSERT path therefore no longer notices catalog corruption beyond the root page.
- **Never `ReadBuffer` a block whose content lock this backend already holds.**
  `bm25_scan_snapshot` holds the metapage content lock across the whole catalog walk, so
  it refuses a link to block 0 before reading it, with the kind check's message. PG 18's
  LWLock-based content locks happen to let a second SHARE through; PG 19 reworked buffer
  locking and asserts on the re-lock (`bufmgr.c`, `entry->data.lockmode ==
  BUFFER_LOCK_UNLOCK`), and nothing promises a production build more than a
  self-deadlock. The kind check that follows `ReadBuffer` is too late. A new walker that
  keeps a buffer locked across iterations needs the same pre-check.
  `bm25_segcat_first_entry` holds the metapage while it reads the root and refuses a root
  at block 0 the same way. The other three SEGCAT walkers release each page before the
  next and are not exposed.

- **A validator that runs on a header cannot check a fact about the decoded run.**
  `bm25_block_validate` runs before the varbyte deltas are expanded, which is why
  `last_docid` is checked by a *separate* function called after the decode loop, and why
  only `wand_cursor_load_block` calls it — that is the one reader which materializes a
  `docids[]` array. `bm25_seg_block_header_read` never expands the deltas at all, and
  `bm25_seg_scan_postings` streams each posting to a callback with only a running `prev`,
  so neither has a run to compare against (`docs/adr/0073`).

- **The PGDG matrix is non-cassert; the `hardening` job covers the gap.** The main
  `build-and-test` matrix builds against PGDG packages (no `--enable-cassert`), so an
  assertion-only bug passes it. The orphan sweep's `PageIsNew` guard exists because a
  never-initialized zero page (crash after relation-extend, before the page-init WAL
  commits) has `pd_special == 0`, which trips `PageGetSpecialPointer`'s assert inside
  `BM25PageGetOpaque` — only on a cassert build. The `hardening` CI job (source-built
  cassert + UBSan PG 17 and 18, full SQL + TAP `installcheck`) now exercises exactly these
  reclamation/crash paths under asserts; still test them against a cassert PostgreSQL
  locally when touching page lifecycle (`docs/adr/0008-cassert-ubsan-ci.md`).
- **Retired pages are unflagged at swap (handled).** The in-swap retire helper
  writes only a compact RANGE descriptor (4-buffer cap) — it does **not** stamp
  `BM25_PAGE_DELETED` per page and does **not** touch `nextblk` (per-page work
  cannot fit the single linearizing record). The orphan sweep therefore marks every
  retired-RANGE page **reachable** (so a pre-swap scan can still walk it), and the
  actual `BM25_PAGE_DELETED` stamping + freeing is deferred to `bm25_reclaim_retired`
  once the page's `retire_xid` clears the cluster horizon. The stamp is what the reuse
  allocator gates on (see the stamp-and-gate landmine above).
- **Custom rmgr is unbuildable here.** The nbtree/GiST reuse-conflict records
  (`xl_btree_reuse_page`) are replayed by a per-AM rmgr; `generic_redo` has no
  `ResolveRecoveryConflictWithSnapshotFullXid` hook and `RegisterCustomRmgr`
  needs preload. This is the wall that forces option (d).
- **`SELECT txid_current()` cannot be relied on to flush a maintenance operation's
  WAL ahead of a crash test's `stop('immediate')` — use `SELECT pg_switch_wal()`
  instead.** Two separate facts, not one mechanism: (a) the maintenance TRANSACTION
  itself — `bm25_seal()`/`bm25_merge()`/`bm25_upgrade()` — assigns no xid, so
  `markXidCommitted` is false (`xact.c` ~1318/1354) and its commit is
  asynchronous (`XLogSetAsyncXactLSN`, ~1523; the `forceSyncCommit`/`nrels > 0` escapes do not apply to these calls), regardless of what
  `wrote_xlog` says; and (b) a transaction that only ever assigns an xid, such as a
  bare `SELECT txid_current()`, samples `wrote_xlog = (XactLastRecEnd != 0)`
  (`xact.c` ~1348) BEFORE it writes its own commit record, so ITS commit is ALSO
  asynchronous and flushes nothing on its own — UNLESS that same transaction
  happens to write some other WAL first, such as a heap-page prune triggered
  against `pg_catalog.pg_proc`, which makes ITS OWN eventual commit synchronous and
  flushes everything back to the LSN it targets, the maintenance record included (a
  flush is not scoped to "its own" record). Whether that incidental prune happens
  is not something a test controls (PRs #231/#232/#237). `SELECT pg_switch_wal()` (`xlog.c`
  ~981-984) is a deliberate flush instead: its `XLOG_SWITCH` record IS
  `XLogFlush`'d, with no checkpoint, so redo still replays the maintenance record
  ahead of it. Any new crash TAP suite that needs a maintenance op's WAL durable
  before `stop('immediate')` needs this call, not `txid_current()`.
- **`GlobalVis` staleness (Phase 4).** Before `GlobalVisCheckRemovableFullXid`
  in reclaim, force a `GetOldestNonRemovableTransactionId(heaprel)` refresh (as
  `_bt_pendingfsm_finalize` does, `nbtpage.c:3033`); the per-backend horizon is
  lazily advanced and otherwise defers reclamation forever.
- **Page-reuse safety = stamp-and-gate (Phase 4, ratified).** The index FSM fork
  is not crash-safe: after recovery `GetFreeIndexPage` can hand back a page that
  was freed, reused, and is now *live* again. To make a returned page provably
  safe on inspection, **every** free path stamps the page `BM25_PAGE_DELETED` in
  its opaque before `RecordFreeIndexPage` (the opaque sits in the special area,
  outside Generic WAL's `[pd_lower, pd_upper)` hole, so the stamp is a tiny delta
  record, no `FULL_IMAGE`): the orphan sweep stamps `retire_xid = Invalid`
  (reusable now) for every kind EXCEPT `BM25_PAGE_PENDING`; `bm25_reclaim_retired`
  stamps the dropped segment's pages with the RANGE's `retire_xid` (reusable once
  the horizon clears); and the pending recycle — `bm25_pending_truncate`, plus the
  sweep's `BM25_PAGE_PENDING` case — stamps a real `ReadNextFullTransactionId()`
  too. A scan captures `pending_head` once and walks the chain long afterwards,
  holding no lock any writer takes, so on the primary the horizon is what keeps
  that walk off a recycled page (issue #135, ADR 0019's addendum). A
  `hot_standby_feedback=off` standby holds no horizon back, so since #291 a pending
  page also carries its chain's epoch in `seg_gen` (drawn from `next_gen` at chain
  start, copied onto later pages), and every scan-side walker reads the chain
  through `bm25_pending_walk_read`, which raises `40001` for a page whose epoch is
  at or above the `next_gen` its `BM25ScanSnapshot` captured — the pending chain's
  form of the option-(d) second line below. Epoch 0 (a chain an older binary
  started) is passed.
  `bm25_page_alloc` reuses a candidate **only** if `PageIsNew` **or**
  (`BM25_PAGE_DELETED` and (`!FullTransactionIdIsValid(retire_xid)` or
  `GlobalVisCheckRemovableFullXid`)), under a *conditional* lock; an unmarked page
  is treated as live and rejected. This is the nbtree `safexid` / bloom
  `BLOOM_DELETED` idiom. Option (d) (`seg_gen` re-stamp on the caller's FPI
  re-init) is the second line: a stale pointer into a reused page fails its
  `seg_gen` check and aborts cleanly. Stamping is per-page work, so it lives in
  reclaim/recycle (never the swap, which is 4-buffer-capped).
- **Σdoclen on delete is approximate.** `bm25_livedocs_clear` decrements
  `total_len` by the **segment-average** doclen (per-doc length is not consulted
  on the delete path); it becomes exact only at merge. Acceptable IDF/avgdl
  drift, self-healing at merge — but a known approximation.
- **Seal-vs-scan lock order — FIXED, was never latent.** Seal locked
  segcat→meta while the scan locks meta→segcat. The note that used to sit here
  called this "latent, resolve when the merge path adds concurrent segcat
  writes"; it was live all along — the seal publish and `bm25_livedocs_clear`
  are both concurrent segcat writers, and either could wedge a ranked SELECT.
  Both now take the metapage first; see the "Metapage before segment-catalog
  page" invariant and `docs/adr/0018`.
- **`-ffp-contract=off` is load-bearing (M2b).** Not a style preference — removing it
  lets a compiler fuse WAND's or the exhaustive scorer's `contrib = boost*termscore;
  acc += contrib` into a single-rounding FMA, and the two paths' structurally-identical
  expressions can then round DIFFERENTLY on different compilers (bit-exact on one
  toolchain, off by 1 ULP on another). Do not remove this flag to "clean up" the
  Makefile without re-verifying `43_wand_parity`/`45_m2b_acceptance` on every CI
  compiler first.
- **Over-pull tail rebuild takes a SECOND scan-catalog snapshot, but NOT second
  statistics (M2b; #268).** `bm25_gettuple`'s tail fallback re-enters
  `bm25_scan_build_ranking` (the retry wrapper), which re-snapshots the catalog — it
  does NOT reuse the first snapshot's pages. Before #268 it also recomputed the corpus
  statistics from that snapshot, and those count every valid pending TID, tombstone and
  merge with no visibility check, so ANY insert between the builds (committed,
  uncommitted, aborted, or the cursor's own transaction's), a VACUUM or a merge
  re-scored the tail on a different scale: out-of-order distances, and repeated or
  skipped rows (reproduced, including at the default `wand_top_k` under `LIMIT 5` with a
  selective filter qual). The tail now scores under the first build's pinned statistics
  and resumes by (score, TID) key — see the invariant under "Over-pull tail fallback"
  and `sql/120_wand_tail_stats_pin`. Do not "simplify" the tail back to fresh statistics
  or to a positional/TID resume. What remains fresh is membership: the rebuilt ranking
  can hold rows the scan's snapshot cannot see, and the executor's visibility check
  drops them. Note that the per-field `k1_<col>`/`b_<col>`/`boost_<col>` reloptions are
  not registered options, so `ALTER INDEX ... SET` takes only ShareUpdateExclusiveLock
  for them and they can change under an open scan; the pin covers them for the tail. The
  registered options are no safer than that: core takes the lock level from the options
  registered in the backend running the ALTER and bm25 registers its options lazily, so
  from a backend that has not yet loaded a bm25 index's relcache entry even `k1`, `b`,
  `analyzer` and `key_field` take only ShareUpdateExclusiveLock (`docs/adr/0012`
  addendum). The analyzer is not pinned: the rebuild compares the query's token count with
  the pin's and raises 55000 (object_not_in_prerequisite_state) on a mismatch, which a cold-backend ALTER or an
  `ALTER TEXT SEARCH DICTIONARY` (no index lock at all) can cause.
- **`max_impact` was the LAST reserved format surface (M2b) — superseded by the v6 floor
  gate (roadmap #4, FIXED).** After v5, the on-disk format carried NO reserved-but-unused
  fields (`BM25DictEntry.dict_pad` is an explicit memset-able tail pad for WAL
  determinism, not an alignment pad and not a feature slot — see the
  reserved-surface section), which used to mean any future feature needing new on-disk
  surface would force a fresh hard break. That is no longer the constraint: v6 replaced
  the exact-equality gate with a `min_read_version` floor, so an **additive** change (a
  length-prefixed trailing region, a new page type, a filled sentinel) is free — no
  reserved slot needed in advance, no REINDEX. Only a **breaking** change (an existing
  field reshaping, or a *packed multi-record* struct — `BM25DictEntry`, `BM25BlockHeader`,
  the segcat entry — gaining a field, since those pack many records per page with no
  per-record length prefix and an old reader desyncs at the second record) still raises
  the floor, and now has an online path via `bm25_upgrade(regclass)` rather than only
  REINDEX. See "v6 format-negotiation gate" in the on-disk layout section and
  `docs/adr/0009-format-stability.md`.
- **The additive contract rests on TWO guards — do not "simplify" either away.** The
  contract says an OLDER binary keeps accepting, reading, AND WRITING a NEWER index.
  That is only true because of these, and both fail SILENTLY (data loss, no error) if
  removed. (1) **`BM25_PAGE_ALL_KNOWN`** (`bm25_format.h`): `bm25_reclaim_orphans` is a
  closed-world sweep off a hardcoded compile-time root set, so a newer binary's page
  kind looks like an orphan to an older one — and an orphan is freed with an *invalid*
  `retire_xid`, which `bm25_page_alloc` reuses IMMEDIATELY (no horizon wait), handing a
  live chain to the next seal. The sweep therefore leaks (never frees) any page carrying
  a flag bit outside the mask. **Adding a `BM25_PAGE_*` flag REQUIRES adding it to the
  mask** — it lives directly under the flag defines for that reason. (2)
  **`bm25_meta_set_pd_lower`** (`bm25_meta.c`): every metapage writer must RAISE
  `pd_lower` to `Max(current, end-of-struct)`, NEVER assign it. The metapage grows by
  appending fields, so an older binary's struct ends below a newer tail; assigning drops
  that tail into the page hole and `GenericXLogFinish` zeroes it on apply (live buffer
  AND WAL redo), destroying a future version's fields on the first insert/seal/merge.
  Both are pinned by discriminating checks in `sql/55_format_compat.sql` cases (G)/(H) —
  neuter either guard and (H) reports flags `8704` instead of `8192`, or (G)/(C) report
  `f`.
- **Boolean scoring is bag-of-words (M6).** Boolean MEMBERSHIP (`bm25_query_eval` over the
  presence bitmask) is exact, but the SCORE is the single-pass BM25F OR-sum of every positive
  leaf's constituent terms. A should-PHRASE or should-WILDCARD whose OWN adjacency/expansion
  predicate fails still leaves its terms' score in the accumulator, so it can influence the
  RANK of a doc a sibling clause keeps. Only the ranking of already-kept docs is approximate;
  this is by-design (inherited from the M4/D9 filter-only phrase-scoring contract) and
  reconciles the design doc's D11 "Σ over matched leaf contributions" wording with the code.
  Do NOT "fix" it by retracting a failed leaf's score without a second scorer pass.
- **`must_not`-PHRASE leaves are rejected (M6).** A phrase leaf inside `must_not` raises a
  clean ERROR ("a phrase leaf inside must_not is not yet supported"), on BOTH the scored and
  filter paths — the negated presence-only decode can't reuse the pending-wins dedup (the
  position stash is append/non-idempotent). Deferred, never silent-wrong.
- **`ORDER BY x &@@ q, <secondary key>` — rank-collapse FIXED (post-M6).** A pre-existing
  whole-index bug (since M1, NOT M6-specific, identical for text and jsonb) used to collapse
  BM25 rank onto the secondary key. **Corrected root cause** (an earlier version of this entry,
  and of the `bm25_distance` code comment, had it wrong): `&@@` is projected PER ROW as a
  resjunk column on the Index Scan node's OWN target list — the plan materializes it whenever a
  secondary sort key or a direct `SELECT` of the column needs the value — and this projection
  runs on EVERY ranked query, independent of `xs_recheckorderby` (that flag only gates PG's
  internal reorder-queue re-verification in `nodeIndexscan.c`; it does NOT gate this
  projection). `bm25_distance`/`bm25_distance_jsonb` used to return a constant `+inf` for that
  projection, so a secondary-key Incremental Sort saw every row tie on `+inf` and sorted by the
  secondary key alone, discarding the real scores (`xs_orderbyvals`). **Fix:** `bm25_gettuple`
  stashes each returned tuple's `-score` on the scan opaque (`so->cur_orderby_dist`, set
  alongside `xs_orderbyvals[0]`), and both operator functions return that stash instead of
  `+inf` whenever a scored scan is active (`bm25_scored_scan_head() != NULL` — the registry
  head, post-R3; a single global pointer at the time this fix originally landed); `+inf` remains
  the degradation for a seqscan / non-index plan, where there is no per-row score to report.
  **#151 (TEXT-01):** that degradation is the contract on BOTH fall-through routes, and both are
  pinned as decisions -- `sql/50_orderby_dist` for "no scored scan registered at all",
  `sql/87_distance_scan_identity` for "scans exist but none owns this query", the latter marked a
  DELIBERATE BEHAVIOUR CHANGE because `+inf` replaced returning a DIFFERENT query's finite
  distance. The finding proposed erroring "like the jsonb sibling"; `bm25_distance`'s actual jsonb
  sibling is `bm25_distance_jsonb`, which calls the same resolver and behaves identically. The
  function it was being compared against is `bm25_match_jsonb`, the `@@@` FILTER, which errors
  because a jsonb tree cannot be evaluated off-index at all -- a different operator with a
  different obligation, since `&@@` is projected as a resjunk column on every ranked query and
  must not error. So
  `ORDER BY x &@@ q, <tiebreak>` now sorts CORRECTLY (score order, then tiebreak), and
  `x &@@ q` is a usable score-distance projection (`= -bm25_score(ctid)`) in the target list of
  a ranked scan that is the single active scored scan driving the index order. **Two caveats**
  (both pre-existing single-global-slot limitations — this fix changes neither's ORDERING vs.
  before): (1) **`&@@` must be the LEADING order-by key.** `ORDER BY x &@@ q, tiebreak` works;
  `ORDER BY other_col, x &@@ q` (non-leading) does not trigger the index's ordered scan — the
  scan is unordered (`so->scoring=false`, no active scored scan), so `&@@` degrades to `+inf` and
  that form still collapses. (2) **Head-resolved, not self-resolved, under concurrent scored
  scans — FIXED, see `docs/adr/0061`.** `&@@` used to resolve to the registry HEAD's stashed
  distance. That is right only when one scan is live: the head is the most recently REGISTERED
  scan and registration happens at LOAD, so a correlated inner scan becomes head DURING the
  outer row's projection and the outer row was projected with the inner's distance — and,
  contrary to what this note used to claim, that corrupted row ORDER too, collapsing the
  ranking to the tiebreak. Both distance functions now resolve by ORDER BY query identity
  through one shared resolver. Residual: two live scans ranking the byte-identical query
  resolved recency-first (by registration) — since #252, by emit recency, see
  `docs/adr/0104` — and a projection naming a query nothing is ranking yields `+inf`
  (deliberate — it used to yield another query's finite distance). **The stash-pairing invariant this rests on:** the per-row projection
  immediately follows the `bm25_gettuple` call that produced the tuple being projected — a
  strict, in-order 1:1 `gettuple → project` pairing, so the stash is never stale. Verified
  across multi-segment + pending, WAND top-k, the over-pull tail, and rescan/bound-parameter
  re-runs. Mark/restore (an inner side of a mergejoin/nestloop) cannot break this pairing
  because it is structurally unreachable here: the AM leaves `ammarkpos`/`amrestrpos` NULL, so
  the planner interposes a buffering Sort/Materialize over the bm25 scan, and THAT node
  re-emits its own already-projected (already-frozen) tuples rather than re-invoking the
  operator against a live scan. Gating suite: `sql/50_orderby_dist.sql`. Same family,
  unaffected by the fix: a bare `&@@ <jsonb>` with NO `@@@` anchor under a seqscan still
  returns +inf (no active scored scan off-index). It is no longer blind to the tree, though
  (#245): on that fall-through `bm25_distance_jsonb` parses it structurally
  (`bm25_query_validate`, no field resolution) and raises the index path's error for a
  malformed or over-cap tree; an unknown field name is still accepted there
  (`sql/115_orderby_jsonb_validate`). Residuals (`docs/adr/0061` addendum): the
  validator skips the `must_not`-phrase rejection the index path applies (#273, closed
  as a residual), its per-call-site cache keys on the tree's bytes AND the three
  SUSET wildcard GUCs that also decide validity (#272, fixed: a cached valid verdict is reused
  only while the bytes and all three GUCs are unchanged; `sql/115` has one case per GUC),
  and the text form, and a valid jsonb tree, still return `+inf` in arbitrary order with no
  signal (#271, decided as documented and closed: the fall-through stays, and a NOTICE and a
  planner check were rejected, `docs/adr/0061` addenda).
- **The jsonb `@@@` operator needs a real index scan — it now FAILS LOUD off-index (M6 +
  fix).** Boolean/phrase/wildcard/`field:term` scoping is honored ONLY on the genuine bm25
  index scan; the jsonb tree is parsed and evaluated in `bm25_rescan_parse_jsonb`/
  `bm25_query_eval`, never by the operator functions. `bm25_match_jsonb` **ereports an ERROR**
  (`52_jsonb_filter_index_only`) whenever it is reached — which is ONLY when the planner
  applies `col @@@ jsonb` as a filter/recheck/seqscan qual (a competing `ORDER BY` that steals
  another index, a cheaper alternative index, a cost/stats flip). It cannot match a
  field-scoped jsonb query from one heap column value (no index handle/segments/analyzer);
  it used to return `false`, silently dropping every row (a 0-row lie), and the intermittent
  version of that (a `count(*)` that sometimes flips to the pkey+Filter plan) was mis-reported
  as a "cold scan" bug. The correct index path never calls it (`xs_recheck=false`; no
  `amgetbitmap`/`amcanreturn`), so erroring can only surface the misuse. `bm25_distance_jsonb`
  is NOT fail-loud for a valid tree: it returns the owning scored scan's stashed per-row
  distance, degrading to `+inf` when no scan ranks its query. `ORDER BY` alone does NOT force
  the index: with `amoptionalkey = false`, `ORDER BY col &@@ q` with no `WHERE col @@@ q` plans
  as Seq Scan + Sort even under `enable_seqscan = off`, and every row is `+inf` (an arbitrary
  order presented as a ranked one). On that fall-through the jsonb tree is validated
  structurally (#245), so a malformed or over-cap tree errors as it would on the index; field
  names are not resolved there. Issue ranked queries as `WHERE col @@@ q ORDER BY col &@@ q`.
  Force the index for a bare jsonb `@@@` filter (`SET enable_seqscan=off`, anchor on the
  first indexed column, no competing `ORDER BY`/index); for ranked retrieval use
  `col @@@ q ORDER BY col &@@ q`, as above.
  The `(text,text)` `@@@` (`bm25_match`) does a real default-english match on the seqscan
  fallback (LHS column only, may diverge from a non-english index), so it is not silently-zero
  and answers a bare RHS; it refuses only what it structurally cannot decide, a phrase (#132)
  and a `field:` scope (#298).
- **The `pg_catalog.` prefixes inside the six jsonb builders are load-bearing, not noise.**
  Those bodies are `LANGUAGE sql` **strings**, so they are re-parsed at CALL time under the
  CALLER's `search_path`. `pg_catalog` being implicit does not save them: it is only a tiebreak
  among candidates with IDENTICAL argument-type lists, and once the lists differ
  `func_select_candidate` ranks by exact-input-type-match count — a user
  `jsonb_build_object(text,text,text,text)` scores 2 to the variadic builtin's 0 and wins
  regardless of schema order. Stripping the prefixes reintroduces CVE-2018-1058 and is invisible
  under any ordinary test path; `sql/85_builder_search_path` is the only thing that sees it, and
  it checks both behaviorally (against shadow functions, with canaries proving the shadows are
  eligible) and statically (over `pg_proc.prosrc`). A **seventh** builder must be added to that
  suite's Part 2 name list or it is unguarded. `docs/adr/0051-builders-qualify-pg-catalog.md`.
- **`META.json` and `.pgx-build.yml` are read by nothing in this repo**, which is why both rotted
  through a rebrand and a PG-floor change with CI fully green. They are now gated by
  `test/check_packaging_identity.py` (first step of `build-and-test`), which DERIVES the expected
  identity from `Makefile` (`EXTENSION`/`DATA`), `bm25_native.control` (`default_version`) and the
  `#if PG_VERSION_NUM < …` `#error` guard in `src/bm25.h` — so a version or floor bump fails the
  gate until the manifests follow. Its rebrand-residue scan is deliberately scoped to the
  packaging files: the old identity is preserved on purpose in ADR bodies, in the upgrade-script
  reference below, and in the plan archives under `docs/`.
  `docs/adr/0052-packaging-identity-is-gated.md`.
- **The files PostgreSQL parses must stay 7-bit ASCII**, and `test/check_source_ascii.py`
  (second step of `build-and-test`) is what enforces it. Not a style rule: `CREATE EXTENSION`
  validates the WHOLE of `bm25_native--1.0.sql` against the SERVER ENCODING before running any
  of it, so 11 comment lines of em dashes and a Σ meant the extension **could not be installed
  in an EUC_JP database at all** (`0xE2 0x80` is not a legal EUC_JP sequence). LATIN1 accepts
  those bytes — every byte is legal LATIN1 — and every gate here is C/UTF8, so nothing saw it;
  the broken and fixed scripts are byte-identical under everything CI runs. Declaring
  `encoding = 'UTF8'` in the control file does NOT fix this: it converts instead of validating,
  and U+2014 has no equivalent in either LATIN1 or EUC_JP (PostgreSQL maps the JIS dash to
  U+2015). The gate derives its file set from `Makefile` `EXTENSION`/`DATA` plus `*.control`,
  reads every `DATA` assignment form, and scans on `\n` only — `str.splitlines()` CONSUMES
  U+2028/U+2029/U+0085, which would blind it to `E2 80 A8`, the very prefix it exists to catch.
  The gate also covers `src/*.{c,h}`, where non-ASCII really was only cosmetic — swept together
  so the tree has one rule instead of a per-file judgement call. `sql/` is deliberately NOT
  covered and should not be: several suites carry intentional multibyte test DATA.
  `t/018_snippet_encoding.pl` is the behavioural half.
  `docs/adr/0054-shipped-script-is-seven-bit-ascii.md`.
- **`BM25_MAX_FIELDS`, never `MAX_FIELDS`.** It was the only unprefixed macro with header scope,
  exported by `bm25_format.h` — which every translation unit pulls in transitively alongside
  `postgres.h` and a dozen PG headers — and it sizes on-disk structs
  (`BM25FieldConfigHeader.per_field_fingerprint[]`, `BM25BlockImpact.fields[]`). No transitional
  `#define MAX_FIELDS BM25_MAX_FIELDS` was left behind: an alias would reintroduce exactly the
  collision surface the rename removes. ADR bodies still say `MAX_FIELDS` **on purpose** —
  Accepted records are frozen and are a historical record of what was decided when, the same
  discipline that kept the pre-rebrand identity in them.
- **`amvalidate` is a real check, and `sql/86`'s CANARIES are what prove it.** `bm25_validate`
  returned `true` for any OID at all through M0–M6. It now walks the opfamily: strategy 1 a
  boolean SEARCH member, strategy 2 a float8 ORDER BY member whose sort family can sort float8,
  nothing outside `[1,2]`, no support procs (`amsupport` is 0), and both members present for the
  opclass's own `opcintype` (NOT the whole family — cross-type members like `(text,jsonb)` are
  legitimate, and rejecting them would fail `opr_sanity` for a user who did nothing wrong).
  Because `amstrategies` is 0, the opclass DDL is the ONLY place the strategy contract is
  written down, which is what makes this check the thing that reads it. Note `CREATE OPERATOR
  CLASS` does **not** invoke `amvalidate` — core calls it only from the SQL function — which is
  both why a broken opclass is accepted silently and why suite 86 can construct its six
  canaries. Two branches have no canary on purpose (core rejects a non-boolean SEARCH operator
  and any support proc at DDL time, so they cannot be injected). Use
  `SearchSysCache1(OPFAMILYOID)`, never `get_opfamily_name()` — that helper is PG 18+ and the
  floor here is 17. `docs/adr/0055-amvalidate-checks-the-opclass.md`.
- **SQLSTATE is API surface, and no suite sees it unless a suite prints it.** Every
  `ereport(ERROR)` call site in `src/` carries an `errcode()`, and `test/check_source_style.py`
  now fails CI if one does not (see "C source house style" below); five once did not, and
  so reported as `XX000` internal_error — including two reachable from ordinary user SQL by
  passing a bad segment number to a debug SRF. A wrong SQLSTATE changes nothing about the message text, so
  pg_regress compared identical output before and after. House rule: user-argument errors get
  `ERRCODE_INVALID_PARAMETER_VALUE`, on-disk corruption gets `ERRCODE_INDEX_CORRUPTED`, and
  `sql/86` Part 1 pins the two reachable ones by printing the SQLSTATE *and* proving a
  PL/pgSQL handler keyed on `invalid_parameter_value` fires — a bare `WHEN OTHERS` would stay
  green under `XX000`. Since #307/#310/#313: a non-index OID is `42809` and a dangling one `42704`
  (the gates resolve the relkind first), a write on a standby is `25006`, a whole-index report
  under row-level security is `42501`, a state concurrent DDL can produce is `55000` (not
  `XX000`), and every corruption ereport bm25 raises is `XX002` (the one `DATA_CORRUPTED` site was fixed; a KEYMAP root past the extent still reaches core's own short-read `XX001`, a residual in `docs/adr/0119`). `40001`
  is reserved for the reclaim race, never for corruption (`docs/adr/0120`), except the residuals
  `docs/adr/0119` lists (a catalog entry whose `header_blkno` names a catalog page reaches the
  header reader's gen check first and gets `40001`). `sql/149` pins these.
- **The page lever forges bytes, not structure, and its contract is narrower than "any
  bytes".** `bm25_debug_poke_page` (`docs/adr/0126`) refuses a result PostgreSQL could not carry
  (core's header check, `pd_lower` inside the header) or that would lose bytes (any poked byte in
  the hole, or a hole that is not already all zero), so every accepted poke can be undone by
  poking the returned bytes back. It cannot leave an all-zero page (`t/035` gets one from a crash
  mid-extend), holds no singleton (quiescent indexes only), and accepts any `pd_special` core
  accepts, so only the #302 D cases may poke `pd_special`. Aim it with `bm25_debug_layout()`,
  never literal offsets, and A/B each new assertion against the build without its check: an
  earlier guard often fires first. Tests written against an earlier version of the lever broke
  when the hole refusal landed.

## C source house style

`src/` follows a documented house style rather than pgindent's (ADR 0092, issue #66). The part a
script can decide without judgement is gated by `test/check_source_style.py`, which runs in
`build-and-test` right after the ASCII gate; the rest is a review rule.

Gated:

- **No tab characters, LF line endings, no trailing whitespace.** Indentation is 4 spaces per
  level; the gate enforces only the absence of tabs, so the width itself is a review rule.
- **No line over 120 columns.** Comment text wraps at about 88 by habit; 120 is the hard ceiling.
  Split a long message across adjacent string literals (C concatenates them) and move a trailing
  comment that would push a line past it onto its own line above.
- **`#include "postgres.h"` is the literal first `#include` of every `.c`, and no header includes
  it** — headers assume it, as in core. `bm25.h`'s PG-floor `#error` reads `PG_VERSION_NUM`, which
  a `.c` receives only through `postgres.h` (it is `pg_config.h`'s, reached via `c.h`), so a `.c`
  that forgot it would fail with a misleading message.
- **Every `ereport` that can raise an error carries `errcode()`** — level `ERROR`/`FATAL`/`PANIC`,
  or any non-literal level (`errcode_for_file_access()` / `errcode_for_socket_access()` count; an
  errcode hidden in a project macro does not). Checked inside the call's own parentheses with
  comments and literals blanked, so a message that merely mentions `errcode` does not count.
- **Every `hash_create` passes `HASH_CONTEXT`** (and sets `ctl.hcxt`), so a table is never
  built in whatever context happens to be current. Read from the call's fourth top-level
  argument across line breaks, not line-wise: `HASH_CONTEXT` sits on a continuation line at
  *every one* of the call sites, so a line-oriented grep reports all of them as violations.
- **Every `Assert()` carries a classification comment** — `/* invariant */` when this code's
  own construction guarantees the condition, or `/* checked: <where> */` when it was validated
  elsewhere and the Assert is a cassert-only restatement. The point is that the difference
  cannot be lost silently: an Assert over data this AM does not itself guarantee, notably
  anything read off a page, is compiled out of production and wants a loud
  `ereport(ERROR, errcode(ERRCODE_INDEX_CORRUPTED))` instead.
  `elog()` is exempt: it is the can't-happen internal-error path, where `XX000` is the right answer.
  Which code to use is the SQLSTATE rule in Landmines above.
- **7-bit ASCII** is the separate `test/check_source_ascii.py` gate; see the Landmines bullet on
  the files PostgreSQL parses.

Reviewed, not gated (each needs a C parser or a judgement call):

- **One statement per line**, braces on their own lines, and a function's return type on its own
  line above its name. The one exception is a lookup-table `switch`, which may write
  `case LABEL: return <expr>;` on one line. A declaration list such as `uint32 s0 = 0, s1 = 1;` is
  one statement.
- **Declarations at the top of a block**, and no C99 `for (int i = 0; ...)` declarations. PG's
  `-Wdeclaration-after-statement` catches a declaration after a statement but not a for-init one.
- **Both comment-opener forms are accepted**: text on the `/*` line, or a bare `/*` with the text
  starting on the next ` * ` line. Neither is to be normalized tree-wide.
- **Each file opens with a short prose header** naming its bare basename (`bm25_scan.c -- ...`,
  never `src/bm25_scan.c`) and its role in the system. There is no PG-core copyright /
  `IDENTIFICATION` block; the root `LICENSE` is the license statement.
- **Error-message prefixes (R1–R5):**
  1. Every `ereport` primary message (`errmsg`) starts with a component prefix.
  2. The default prefix is `"bm25: "`.
  3. A message raised directly by a SQL-callable function about its own arguments or target may
     instead start `"<sql_function>: "`, where the name is exactly one declared in
     `bm25_native--1.0.sql` through which the user reached that code — the name the user typed.
     PostgreSQL's message style guide forbids naming the internal C routine, which users never
     see. A shared helper may use this form only when each caller passes its own SQL name in, as
     `bm25_debug_topk_k_validate` does.
  4. Never name a C routine that is not a SQL-visible function name. A helper shared by several
     SQL functions uses `"bm25: "`, as does a function whose C symbol differs from its SQL name
     (`bm25_upgrade_sql` is SQL `bm25_upgrade`). A SQL name may still appear in the message body.
     What decides between 3 and 4 is what the message is ABOUT: the `"<sql_function>: "` form is
     only for a message about the caller's own argument or target. A message about anything else
     -- query shape, server state, index contents -- uses `"bm25: "` even when it is raised on
     behalf of one SQL function, as `bm25_sole_scored_scan`'s ambiguity error does.
  5. Exempt: the opclass validator's (`amvalidate`) INFO messages, which mirror core's amvalidate
     phrasing, and `elog()`.
- **`errdetail` strings are complete sentences**: capitalized, ending in a period, per
  PostgreSQL's message style guide ("The page's flags are 0x%x; expected one of 0x%x.", not
  "flags 0x%x, expected 0x%x"). Twenty-six fragments were rewritten in #313 (`docs/adr/0092`'s
  addendum). A review rule, not gated: the lexer can find the literal but not judge the sentence.

**Why not pgindent.** This is an out-of-tree PGXS extension, not a core patch. A pgindent pass
re-indents every line with tabs and needs a maintained typedefs list to run at all, and meeting
its 79-column width would mean hand-reflowing thousands of lines on top — a whole-tree rewrite of
`git blame` with no behavioural gain. The house style keeps what makes review easier and gates
what can be gated; everything else is a documented review rule.

## Build & test (for acting, not setup — see README for detail)

- `pg_config` is not assumed on PATH. Pass it: `make PG_CONFIG=/path/to/pg_config`.
  Local dev here uses `/usr/local/pgsql/bin/pg_config` (PostgreSQL 18.3); a
  running cluster is reached via `PGHOST=<repo>/_localtest PGPORT=55432`.
- Build / install: `make PG_CONFIG=… [install]`.
- SQL regression: `PGHOST=… PGPORT=… make PG_CONFIG=… installcheck`.
  Always confirm the trailing `# All N tests passed.` — that line is the authority on how
  many suites there are, which is why neither this bullet nor `README.md` states a count:
  a written-down figure is stale the next time a suite is added, and was (it said 64 while
  the `REGRESS` line held 88). The per-milestone counts in the branch history above are a
  different thing — historical records of what was green when each milestone landed, not
  claims about today, and deliberately not maintained. The `REGRESS` line is the single
  source of which `sql/NN_*.sql` run (PGXS does not auto-discover `sql/`); append each
  new suite there exactly once. M4 adds `36_positions` (round-trip through seal),
  `37_merge_positions`, `38_phrase`, `39_snippet`, and `40_m4_acceptance` (the
  realistic multi-field end-to-end bars, incl. the bag-of-words-does-not-fault-POS proof).
  M2b adds `42_format_v5` (the v4→v5 migration gate), `43_wand_parity`, `44_wand_skip`,
  and `45_m2b_acceptance` (see the "Block-max WAND (M2b)" section). M6 adds `46_m6_boolean`
  (boolean/must_not/phrase/nested + filter==ranked), `47_m6_wildcard` (expansion, caps,
  single-count dedup), `48_m6_builders` (builder validation + boost reorder + bound-param
  crash lock), and `49_m6_acceptance` (multi-field BM25F acceptance + DSL-safety + backward-compat).
  The post-M6 rank-collapse fix adds `50_orderby_dist` (secondary-key ordering now correct,
  `&@@` real-distance projection, single-key unchanged, cross-path stash verification across
  multiseg+pending/WAND/over-pull-tail/mark-restore, seqscan degradation unchanged).
  The §5 conformance suite adds `51_conformance` (a 28-row synthetic
  discriminating corpus on the "§1a" multi-field/boost/`key_field`/positions index, mapping
  every documented query-grammar form — default-fields fan-out, per-field boost, field
  scope, boolean AND-NOT/OR/nested, phrase-vs-bag, proximity `PRE/n`/`W/n`/`W/S`, wildcard
  truncation, injection-safety, relevance score positive/strictly-descending/no-Sort top-N,
  query-time boost, snippet — to a native call with a discriminating assertion). The
  grammar→native-call contract lives in `docs/grammar-mapping.md`. Two known limits
  the suite works around, documented there: a bare `@@@` filter answered OFF the bm25 index now
  ERRORs (fail-loud — see the jsonb-`@@@`-fail-loud fix below; the earlier "priming/cold-scan"
  framing was a misdiagnosis), and, until #308, `bm25_snippet` returned NULL under a multi-leaf
  boolean scan — so the suite uses the ranked/single-leaf forms throughout.
  The jsonb-`@@@`-fail-loud fix adds `52_jsonb_filter_index_only` (a `col @@@ jsonb` planned as a
  filter/seqscan/pkey-Filter qual ERRORs instead of silently returning 0 rows; the bm25 Index
  Scan path still returns the correct set).
  The score-accessor concurrency work (R3) adds `53_concurrent_scored_scans` (a portable
  plan-shape guard forces two bm25 Index Scan nodes genuinely live at once — a correlated
  subquery, both sides ranking — then asserts the outer's `bm25_score_key` matches its
  single-scan baseline for every row; before the fix every row diverged, since the single
  global slot silently handed back the inner scan's score instead). The #242 call-site-attribution
  fix (`docs/adr/0103`) made the suite plan-stable (`ANALYZE`, materialized single-scan CTAS
  baselines, so it no longer depends on which plan the planner happens to pick) and extended it:
  per-row nested assertions for both `bm25_score_key`/`bm25_score` that fail on pre-fix main with
  autovacuum both on and off, the two-subquery JOIN/`UNION ALL`/LIMIT-branch shapes, and the two
  named residual shapes (an unattributable collision; a decoupled PL/pgSQL-prefetch projection),
  pinned with their values labelled NOT correct rather than silently passing.
  `t/020_vacuum_merge_race.pl` (#239, #241, `docs/adr/0102`) adds the VACUUM/merge singleton
  test: `bm25_native.debug_pause` parks a backend at a named pause point (the `src/bm25_handler.c`
  entry above lists them all; this suite uses the first five,
  `bulkdelete_start`/`bulkdelete_segment`/`bulkdelete_pending`/`merge_preswap`/
  `swap_after_snapshot`) behind an ordinary `pg_advisory_lock`, and the suite drives all four
  interleavings — pending sweep vs. seal, merge between segments, merge replaying before the loop
  and swapping after it, and the counter-drift race — asserting the live count, the metapage, the
  LIVEDOCS bits and the heap agree and no reused line pointer is returned; 49/49 assertions pass
  with the fix, 19/49 fail on pre-fix locking.
  `t/021_generic_wal_meta_first.pl` (#240, `docs/adr/0018` addendum) runs a workload
  emitting the three multi-buffer Generic WAL record shapes that carry the metapage
  (`bm25_pending_append_multi`, the in-place branch of `bm25_segcat_publish_append`,
  `bm25_livedocs_clear`) and asserts with `pg_waldump` that each such record has the
  metapage as block reference #0; each shape must be observed, so the suite cannot
  pass vacuously. It uses `pg_waldump` (core) rather than `pg_walinspect` because the
  source-built cassert/UBSan/ASan legs install no contrib. It checks WAL block order,
  not the standby deadlock itself, which has no test.
  `sql/112_merge_append_distance` (#252, `docs/adr/0104`) runs a ranked `ORDER BY &@@`
  over a partitioned parent and an `INHERITS` parent (whose parent has its own rows and
  index) and checks each row's distance, non-decreasing order and `LIMIT 5` against
  single-child baselines. `sql/111_wand_livedocs_per_reader` now drops its tables and the
  extension at its end, because it used to be the only suite that left them installed.
  The 2026-09-29 grind added `sql/113` to `sql/119`: `113_pending_sweep_extent` (#243:
  the real sweep run with a stale extent sample through `bm25_debug_pending_sweep`, and a
  genuinely out-of-extent pending link still raising), `114_segcat_link_corruption` (#244:
  every SEGCAT walker against each corrupt-link shape, with legitimate empty-page negative
  controls, through `bm25_debug_stamp_segcat_empty` and the `find_absent`/`locate_absent`
  modes of `bm25_debug_segcat_walk`), `115_orderby_jsonb_validate` (#245),
  `116_score_query_overloads` (#253: each residual class against single-scan baselines,
  also recording that the one-argument accessor misattributes there), and, for #246,
  `117_exhaustive_livedocs_checked` and `119_dict_lookup_handoff` (each pins its saving
  as buffer accesses rather than time) and `118_ranked_keys_forward_pass` (every ranked row
  carries its own key across multi-page and one-page KEYMAPs).
  The 2026-10-04 grind added `sql/120_wand_tail_stats_pin` (#268: a cursor driven across a
  same-transaction change, an aborted insert and a mid-scan seal must return the reference
  ids, each once, at the reference distances; exact float equality against a `wand_top_k = 0`
  ranking) and `sql/121_chain_page_image` (#267: every ranked path on a one-segment,
  multi-page shape against an identical table whose rows are all pending, with a bound on
  buffer accesses per scored pair, plus a 72-segment keyed case). It extended `sql/115` (#272:
  one case per wildcard GUC), `sql/110` and `sql/114` (the `bm25_segcat_first_entry` guards
  through `bm25_debug_segcat_walk`) and left residual notes in `sql/113` and `sql/114`
  (#269 T1-T3). Three TAP suites arrived with it: `t/022_insert_keycheck_race.pl` (#270, the
  `insert_keycheck` pause point, the sixth in the table in `bm25_handler.c`: an INSERT parked
  after its catalog read, then a merge, a VACUUM and inserts until the old catalog root is
  reused as a pending page; the INSERT must succeed), `t/023_slow_cursor_tid_reuse.pl` (#269:
  five REPEATABLE READ cursors sample LIVEDOCS, VACUUM tombstones a document and an INSERT
  reuses its heap line pointer; the reused row must never appear, which is ADR 0100's safety
  argument) and `t/024_wand_tail_vanished_last.pl` (#268: the last emitted row, dead, is
  vacuumed by another session before the tail rebuild, and the next row must not be skipped).
  The 2026-10-05 grind added `sql/122` to `sql/134` and `t/025` to `t/032`.
  `122_reloption_registration` (#297: a failed registration leaves the next attempt whole),
  `123_build_all_null` (#299: `CREATE INDEX`, `REINDEX` and incremental insert agree on
  all-NULL rows), `124_strict_chain_walkers` (#293, #294, `docs/adr/0111`: each walker against
  a chain that ends early or a page short of its full span, and the TID chokepoint, through the new
  `bm25_debug_stamp_*` levers), `125_wand_straddle` (#289, `docs/adr/0113`: straddling
  documents on a count-sliced layout, with and without LIMIT, against the exhaustive scorer),
  `126_key_identity_stamp` and `127_key_stamp_fallback` (#292, `docs/adr/0112`: every
  `key_field` change refused on an empty catalog, every build path stamping, the unstamped
  fallback and the drain's mixed-chain refusal), `128_pending_chain_epoch` (#291,
  `docs/adr/0110`), `129_where_orderby_sql_semantics` (#290, `docs/adr/0109`, against a
  seqscan ground truth), `130_offindex_field_scope` (#298: the filter shapes, with plan
  guards), `131_scored_scan_owner` (#301, `docs/adr/0115`), `132_dictionary_probe` (#296:
  stoplist and stemmer `ALTER`s trip the gate in the same session),
  `133_pending_truncate_fsm` (#300: pages an insert-path seal recycles are found by the
  allocator with no VACUUM between) and `134_orphan_sweep_gate` (#300, `docs/adr/0116`: the
  gate, behaviourally). It extended `sql/38`, `60` and `64` (the new WHERE semantics),
  `sql/43` (a three-field straddle case), and `t/016` (a keyed unlogged index's init-fork
  stamp) and `t/021` (the three-block append record). The TAP suites: `t/025_standby_pending_recycle.pl`
  (#291: a standby `@@@` scan parked at `scan_pending_page` while the primary recycles the
  next page, as a segment page and as a new pending page), `t/026_single_byte_encodings.pl`
  (#295: WIN1251, KOI8R, LATIN1, SQL_ASCII and a UTF-8 control), `t/027_cleanup_order.pl`
  (#300: a VACUUM cancelled in its sweep has already merged and reclaimed; `bm25_merge`
  holds no singleton while it waits for the heap), `t/028_append_one_record_crash.pl` (an
  INSERT parked after its allocation, then a crash: no `PENDING` page outside the chain),
  `t/029_orphan_sweep_evidence.pl` (evidence across a crash, a crash-restart with the
  postmaster surviving, an ERROR between `bm25_reclaim_retired`'s compaction and its frees,
  a merge's open bracket), `t/030_sweep_insert_concurrency.pl` (INSERTs during a manual and
  an autovacuum sweep, with no autovacuum cancel), `t/031_sweep_appender_race.pl`
  (appenders racing the share-mode sweep, including an older binary's epoch-0 page before
  and after the mark) and `t/032_bounded_maintenance_holds.pl` (an INSERT with a short
  `lock_timeout` succeeds while a reclaim or a forced merge is parked between its units).
  The second 2026-10-05 grind (#302-#314) added `sql/136` to `sql/151` (no `141`) and
  `t/033` to `t/037` (no `036`). The corruption suites forge pages with the raw lever
  (`docs/adr/0126`) and assert the call site's own message: `136_page_lever` (the lever and
  `bm25_debug_layout` themselves), `137_page_roles` (#302 A/B/D, #303 C: metapage pointers,
  catalog versus header role, the pending tail, `pd_special`), `138_retired_reclaim_validation`
  (#302 C/E), `139_seg_walk` (#303 A/B, `docs/adr/0120`: every walker and family invariant,
  the gen arm), `140_decode_value_bounds` (#303 D/F/G/H/I) and `151_block_header_offset`
  (#312 item 3). The rest: `142_readable_gate_rls` (#310), `143_query_args_strict` (#304,
  `docs/adr/0124`), `144_reloption_opclass_surface` (#304), `145_text_rhs_micro_parser` (#306,
  each case on the index and the filter path, `docs/adr/0122`), `146_snippet_query_trees`
  (#308, `docs/adr/0123`; it carries a 10 s wall-clock bound, an exception to the no-timing
  rule), `147_accum_full_term_key` (#305, a hard-coded colliding pair, `docs/adr/0125`),
  `148_keymap_cancel_analyze_memory` (#305, a cancel injected with `debug_cancel_at`),
  `149_hygiene_313` (#313) and `150_error_sites` (#309 CI-04: the user-reachable,
  non-corruption ERROR sites a gcov run found unexecuted, and merge rule (c)). `sql/61` now
  reads its regime off the index and checks WAND parity while WAND prunes. The TAP suites:
  `t/033_standby_rank_conflict.pl` (#307: a lock conflict against a ranked scan parked at
  `rank_build_attempt` on a standby is a 40001 cancel and the session survives; every
  ranking-build caller on the standby branch), `t/034_standby_write_gates.pl` (#307: 25006
  for every write entry point, relation size unchanged), `t/035_mark_chain_uninitialized.pl`
  (#302 F: a real zero page left by a crash mid-extend) and `t/037_reclaim_unlink_order.pl`
  (#50: the unlink-path compaction order, the only suite that fails with it reverted).
- **TAP suites (`t/*.pl`) need the PG source tree's Perl modules** — they use
  `PostgreSQL::Test::Cluster`, which an installation built without
  `--enable-tap-tests` does not ship. They still run locally: put the matching source
  tree's `src/test/perl` (plus `IPC::Run`) on `PERL5LIB`, point `TEMP_CONFIG` at a file
  setting `extension_control_path`/`dynamic_library_path` to the repo, and run `prove`.
  A direct `prove` writes node logs to `./log` and APPENDS across runs; every suite's
  closing `bm25_check_logs` (`t/lib/Bm25LogCheck.pm`, #309) scans only what each node
  wrote in the current run, so a stale failure there does not carry over.
  `TAP_TESTS=1` auto-discovers every `t/*.pl`; do NOT add a `PROVE_TESTS` line (it
  shadows auto-discovery and silently drops suites). CI runs every one (39 files as of
  the second 2026-10-05 grind; `ci/check_tap_ran.sh` fails build-and-test unless prove's
  `Files=N` matches `t/*.pl`), including the M4
  positions crash/replica suites `t/010_m4_crash.pl` and `t/011_m4_replica.pl` (mirroring
  the M5 `t/008_m5_crash.pl`/`t/009_m5_replica.pl` on a position-bearing index), the M2b WAND
  crash/replica suite `t/012_v5_wand.pl` (WAND == exhaustive AND pruning still fires
  post-recovery), and the conformance replica suite `t/008_conformance.pl`
  (a §1a-shape primary→standby asserting a ranked fan-out + phrase + snippet query is
  byte-identical on both nodes).
- CI (`.github/workflows/ci.yml`) runs build + SQL regression + TAP on PG 17/18
  via `pg_virtualenv`, with `COPT=-Werror` so a compiler warning fails the build
  (`docs/adr/0056`); a `hardening` job repeats `installcheck` under cassert +
  UBSan (`docs/adr/0008`), as a matrix over the current minor of PG 17 and 18 (17.11 and
  18.6; bump `pg_ver` when a minor ships), with `wal_consistency_checking = 'all'`: TAP
  nodes get it through `TEMP_CONFIG`, and the pg_regress cluster streams to a synchronous
  standby (`ci/wal_check_standby.sh`, `remote_apply`, a script-owned slot cap and a
  watchdog) that replays the whole run and must end without "inconsistent page found"
  (`docs/adr/0074`'s 2026-10-05 addendum); an `asan` job, on the same two minors, repeats it (SQL + TAP) with the
  extension built under AddressSanitizer and `libasan` preloaded into the
  non-instrumented cassert postmaster, after a canary proves the module really
  is instrumented (`docs/adr/0090` — UBSan and cassert's allocator checks cannot
  see an out-of-bounds READ or a stack-array overrun, the class the hand-written
  decoders are exposed to; ASan in turn cannot see an overrun that stays inside a
  palloc block or a shared buffer, since PostgreSQL poisons no such boundary for
  it, so a decoder is ASan-visible when it decodes a stack or palloc'd page copy
  and not when it decodes a shared buffer in place); a
  `folding-collation` job repeats the SQL half
  against a `C.UTF-8` cluster, since every other gate is C-locale and the
  analyzer folds nothing non-ASCII there (`docs/adr/0057`, mechanism in
  `docs/adr/0046`). That job's TAP half is NOT folding coverage —
  `PostgreSQL::Test::Cluster->init` passes no locale argument, so t/*.pl clusters
  take the runner's environment locale. A `macos` job runs the SQL half on the
  BSD ctype table, REPORT-ONLY (`continue-on-error`) until observed green on
  `main` (`docs/adr/0091`). The workflow declares
  `permissions: contents: read` and a `timeout-minutes` per job, and every
  `actions/checkout`/`actions/cache` call site is pinned to a commit SHA rather
  than a floating major tag (`docs/adr/0097`). A `bench` job runs the `bench/`
  scripts scaled down; a `coverage` job builds with gcc `--coverage` (gcov) instrumentation, summarised by lcov, and
  summarizes one instrumented `installcheck`; a `static-analysis` job runs
  `cppcheck` over `src/*.c` and uploads its report. All three are REPORT-ONLY
  (`continue-on-error` at job level, nothing downstream depends on them, no
  stored baseline compared against) -- a shared CI runner is not where a
  trustworthy performance or coverage number comes from (`docs/adr/0098`;
  `bench/README.md` for the bench job's own scaling). An `autovacuum-off` job
  (#242) reruns the SQL suites with autovacuum disabled, so a check whose
  correctness depends on which plan the planner picks (a stats-less table
  plans differently from a stats-having one) cannot hide behind CI's default
  autovacuum-on plan the way `sql/53` did before the fix. A non-blocking
  `build-and-test (19, experimental)` leg (issue #236; `continue-on-error`
  keyed off `matrix.experimental`, with an explicit job `name:` so the 17/18
  check names cannot shift) runs the same build and `installcheck` (SQL + TAP)
  as 17/18 against apt.postgresql.org's PG19 pre-GA package: it cannot fail
  the required-checks gate, since a pre-GA beta can regress for reasons
  unrelated to this extension. `sql/70_distance_volatility` used to fail on
  it: PG19 added `parser_errposition()` to the "functions in index expression
  must be marked IMMUTABLE" ereport in `indexcmds.c`, so the pinned output
  lacked PG19's LINE/caret annotation. The suite now renders those two errors
  through `pg_temp.err_of()` (SQLSTATE and message, identical on every major;
  `\set VERBOSITY terse` does not work, libpq still appends " at character N",
  #251). That one `installcheck` invocation runs the SQL suites first, so a
  diff in any of them stops `make` before the TAP suites run on this leg. PG19 dropped two transitive includes `-Werror` depends on:
  `funcapi.h` no longer pulls in `utils/tuplestore.h` (nine files calling
  `tuplestore_begin_heap`/`putvalues` now include it directly), and
  `storage/lmgr.h` no longer pulls in `storage/lock.h` (needed in
  `bm25_handler.c` for the `debug_pause` advisory-lock calls); a file compiled
  clean on 17/18 by relying on either transitive include silently breaks the
  PG19 leg. `PointerIsValid` was also removed from `c.h`; the portable
  replacement is `DatumGetPointer(x) != NULL`. Build, SQL regression, TAP,
  `hardening`, `asan`, `folding-collation` and `autovacuum-off` all gate the
  workflow run — note `main` has no branch-protection rule, so a red run does
  not mechanically block a merge. `build-and-test (19, experimental)` never gates it. Static steps
  in build-and-test also gate: the interrupt-check floors and `ci/check_scan_scratch.py`
  count calls with comments and strings blanked (`ci/count_calls.py`), and
  `test/check_owned_gate_coverage.py` fails if `sql/63` misses an owned-gate caller (or
  lists a function that no longer is one). Every TAP suite that creates its nodes at file scope (all but `t/018`) ends with
  `bm25_check_logs` (`t/lib/Bm25LogCheck.pm`), which fails on `TRAP`, `PANIC`, a crashed process, an invalid
  page or a UBSan "runtime error:" in any node's log; a suite that kills a backend on
  purpose exempts that PID only, in both PG17's and PG18's wording.
  **Minimum supported major is PostgreSQL 17**: PG16's
  planner will not build an incremental-sort path over an `amcanorderbyop`
  index, so the `&@@` ranked-tiebreak ordering (`ORDER BY x &@@ q, <tiebreak>`)
  collapses onto the tiebreak column on 16; `src/bm25.h` enforces this with a
  build-time `#error` below `PG_VERSION_NUM 170000`. PG19 is tested
  non-blocking, not yet supported.

## Phase 4 design notes (implemented)

1. **Merge swap** is a single `segcat_root` indirection-pointer flip over an
   orphan-rebuilt catalog chain + one RANGE retire entry per dropped segment, all in
   ONE Generic WAL record (`bm25_segcat_publish_swap`); `retire_xid` is
   captured **inside the metapage's exclusive-lock window** — the metapage lock is what
   orders this, NOT a critical section. There is no critical section on this path:
   `GenericXLogStart` opens none, and the only one covering the swap is the one inside
   `GenericXLogFinish` itself. Taking the metapage exclusive lock serializes out any scan
   that could still read the OLD catalog, so `retire_xid` bounds exactly those scans.
   The window is separately throw-free (all buffer acquisition + the dropped-header
   harvest happen before `GenericXLogStart`).
2. **`bm25_reclaim_orphans`** marks every retired-RANGE page reachable so a pre-swap
   scan can still walk it; freeing is deferred to reclaim. It also stamps the orphans it
   frees `BM25_PAGE_DELETED` for the gated allocator. (`BM25_PAGE_DELETED` + `PageIsNew`
   guards remain.)
3. **`bm25_reclaim_retired`** force-refreshes the horizon, then per RANGE entry whose
   `retire_xid` is removable, stamps + frees the segment's pages and drops the entry.
4. **`bm25_page_alloc`** is the stamp-and-gate allocator (see the landmine): reuse only
   `PageIsNew` or `BM25_PAGE_DELETED` pages whose `retire_xid` is invalid or horizon-clear.

5. **Retired-descriptor reclamation** — `bm25_reclaim_retired` unlinks + frees a
   descriptor page once it drains to zero entries (predecessor `nextblk` rewrite; an
   emptied HEAD is left for the next pass to bound the chain to O(1) pages with no
   metapage write). It holds the metapage SINGLETON for the pass (re-entrant) so the
   unlink cannot race a concurrent swap or another reclaim. (`bm25_debug_retired_pages`
   probes the chain length; `21_retired_descriptors` gates it.) The dropped entries are
   rewritten out of the descriptor BEFORE any range is freed, on the unlink path too —
   the free is effective immediately (horizon already clear, so the next allocator, which
   holds the singleton like every live-index allocator, may re-hand the page) while
   Generic WAL writes survive abort, so an
   interruption after the free must not leave entries aliasing reallocated pages. It
   leaks pages for the orphan sweep instead (`docs/adr/0031`).

**Still open (not an M2a blocker):**
- ~~**Seal-vs-scan lock-order inversion**~~ — **FIXED** (`docs/adr/0018`). The merge
  builds its catalog on a fresh orphan chain and locks only the metapage, so it was
  never part of the inversion; the seal publish and `bm25_livedocs_clear` were, and
  both now take the metapage before the catalog page. The same rule was later found
  to apply to Generic WAL REGISTRATION order, which is the standby's lock order
  (#240, addendum to `docs/adr/0018`).
