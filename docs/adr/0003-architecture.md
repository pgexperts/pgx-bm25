---
id: 0003
title: ARCHITECTURE
date: 2026-07-11
status: Accepted
summary: A segmented LSM holding all state in the index relation — metapage directory, WAL-logged pending list sealed into immutable segments, tiered merge via atomic catalog swap, tombstone deletes, and a single-snapshot block-max-WAND scan over an exhaustive OR-sum reference path.
---

# 0003. ARCHITECTURE

A segmented **LSM** with all state in the index relation:
- **Metapage** (block 0) is the LSM directory: format version, k1/b, the v4 analyzer
  fingerprint + field-config root + `field_count`, the pending-list anchors, the segment
  catalog root, global stats cache, the retired free-list, and the monotonic `next_gen`.
- **Pending list** (GIN-fastupdate-style, WAL-logged) absorbs inserts; a **seal** drains it
  into one immutable **segment** (dict / postings / norms / livedocs / docmap [/ keymap] [/ pos]).
- A **tiered merge** engine consolidates same-layer segments via an atomic catalog swap.
  Deletes are tombstones; VACUUM tombstones dead TIDs, runs a vacuum-cadence merge, and
  reclaims orphan/retired pages.
- **Scan** takes ONE metapage-locked snapshot, then ranks the top-k. By default (**M2b**) it
  runs **block-max WAND** (skip whole posting blocks that can't enter the top-k); the exhaustive
  OR-sum union over all live segments + pending (dedup by heap TID, pending wins) is the fallback
  path (phrase/AND/`@@@`, `wand_top_k=0`, the over-pull tail, and **every M6 jsonb tree**) and the
  bit-exact reference.
- **Format v4** baked a per-index `ts_lexize` analyzer (FNV-1a fingerprint, gated at scan
  start). v4 was the last format break; it RESERVED the M4/M5 surface.
- **M5 (multi-field / BM25F / key_field)** FILLS the field/keymap reserved surface with NO
  break: each indexed column is a dense field (per-field `k1`/`b`/`boost`); the scorer is
  BM25F (per-field idf/avgdl/boost summed in the one scorer pass); `field:term` scopes a
  query; a `key_field` INCLUDE column is written to a `BM25_PAGE_KEYMAP` docid→key map so
  results return the user key. Single-field/no-key indexes are byte-identical to M3.
- **M4 (positions / phrase / snippets)** FILLS the LAST reserved surface with NO break: a
  SEPARATE per-segment `BM25_PAGE_POS` chain (`pos_root` + dict `pos_post_root`/`pos_post_off`)
  stores per-posting position frames; a phrase/proximity matcher rechecks-and-filters the
  ranked TID set; `bm25_snippet` highlights by re-analyzing the projected text. No
  format-version bump; existing bag-of-words use needs no REINDEX.
- **M2b (block-max WAND)** FILLS the LAST reserved slot (`BM25BlockHeader.max_impact`) and
  breaks the format **v4→v5 (REINDEX required)** — because a single seal-time scalar cannot be a
  safe scan-time bound (idf/avgdl are scan-time and drift with tombstones/merges) and BM25F needs
  per-field data. v5 replaces the reserved `max_impact` with a per-block **impact table**
  `[uint8 nfields][{uint8 field_id, uint32 max_tf, uint32 min_doclen}×nfields]` (raw ingredients,
  stamped in `encode_block` at seal, recomputed on merge). A new engine (`src/bm25_wand.c`) runs
  Ding–Suel BMW per segment under one shared global top-k heap; pending is a non-prunable arm
  scored first to prime the threshold; WAND is ON by default (`bm25.wand_top_k`, default 100) and
  its results are **bit-identical** to the exhaustive scorer. **After M2b the format has NO
  reserved-but-unused field left** — the reserve-then-fill arc (v3→v4→v5) is complete. See the
  "Block-max WAND (M2b)" ARCHITECTURE.md section.
- **M6 (boolean + wildcard query trees)** adds NO format change — it is a QUERY-LANGUAGE layer
  on top of v5. A query is a **jsonb** object built by SQL functions (`bm25_term`,
  `bm25_match_terms`, `bm25_phrase`, `bm25_wildcard`, `bm25_boolean`, `bm25_boost`) and run via
  `(text,jsonb)` `@@@`/`&@@` overloads added to the `text_bm25_ops` opfamily (same strategies
  1/2). `src/bm25_query.c` parses the jsonb into a `BM25Query` AST (MATCH/TERM/PHRASE/WILDCARD/
  BOOLEAN/BOOST), `bm25_query_flatten` assigns each leaf a bit in a **uint64 presence mask**
  (cap `BM25_QUERY_MAX_LEAVES`=64), the scorer marks each leaf's bit per hit (reusing the M4
  `and_presence` HTAB), and drain-time `bm25_query_eval` filters membership by the boolean
  formula. Wildcards expand over the per-segment dict (`bm25_dict_expand_wildcard` in
  `src/bm25_seg_read.c`). The `(text,text)` path is UNCHANGED. See the
  "Boolean + wildcard query trees (M6)" ARCHITECTURE.md section.
