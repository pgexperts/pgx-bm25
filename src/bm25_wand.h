/* bm25_wand.h -- block-max WAND engine: cursor, top-k heap, BMW driver.
 *
 * M2b Task 4 landed the first slice of this subsystem: the scoring context
 * (BM25WandCtx) and the safe per-block score upper bound (bm25_block_ub).
 * Task 5 adds the second: BM25Scored/BM25TopK, the bounded top-k min-heap the
 * BMW driver drains into. Task 6 adds the third: BM25WandCursor, the per-term
 * per-segment pull cursor that decodes a term's blocks and scores a doc bit-
 * exactly with the exhaustive scorer -- walked LINEARLY only (next()). Task 7
 * adds the fourth: bm25_wand_cursor_next_geq, the block-SKIP jump built on
 * top of that same cursor -- it reads a skipped block's HEADER ONLY (via
 * bm25_seg_block_header_read, Task 3's reader) to decide whether the block
 * can be bypassed entirely, never varbyte-decoding a posting the query does
 * not need. Task 8/9 landed the driver itself: bm25_wand_build_ranking,
 * declared at the bottom of this header, which bm25_scan_build_ranking_once
 * (bm25_scan_rank.c) takes by DEFAULT for ranked scans -- see bm25_wand.c's file
 * header for the gate that selects it.
 * Getting the bound provably safe first, in isolation, with its own
 * regression coverage (sql/43_wand_parity.sql), is deliberate: every later
 * piece trusts it without re-deriving the monotonicity argument.
 *
 * Given its own header (unlike most of this AM's internals, which share the
 * one bm25.h) because the WAND engine is a distinct, largely self-contained
 * subsystem bolted onto the existing scorer at a single seam
 * (bm25_scan_build_ranking_once) -- worth keeping its interface separate from
 * the AM-wide grab bag.
 */
#ifndef BM25_WAND_H
#define BM25_WAND_H

#include "bm25.h"       /* BM25_MAX_FIELDS, BM25FieldConfig, BM25BlockImpact, BM25_FIELD_ALL */
#include "utils/hsearch.h"   /* HTAB: the driver's pending-TID dedupe set (Task 8) */

/*
 * Per-term WAND scoring context: the same live corpus statistics
 * (idf, avgdl, k1, b, boost) the exact scorer (bm25_scan_rank.c:seg_posting_cb)
 * uses for this term, captured once so bm25_block_ub can be compared
 * apples-to-apples against a real per-posting contribution. idf/avgdl are
 * scan-time (snapshot) stats, NOT baked into the on-disk impact table
 * (bm25_format.h:BM25FieldImpact comment) -- they drift as the index grows,
 * so every ctx is built fresh per scan/term, never cached across scans.
 *
 * field_in_query mirrors the C4 field-scope gate (a field-scoped query
 * forces idf_f[f] = 0 for fields outside the scope in the real scorer);
 * bm25_block_ub honors it the same way so a field-scoped bound cannot count
 * a field the query never touches.
 */
typedef struct BM25WandCtx
{
    uint32  field_count;
    double  idf_f[BM25_MAX_FIELDS];
    double  avgdl_f[BM25_MAX_FIELDS];
    double  k1_f[BM25_MAX_FIELDS];
    double  b_f[BM25_MAX_FIELDS];
    double  boost_f[BM25_MAX_FIELDS];
    bool    field_in_query[BM25_MAX_FIELDS];
} BM25WandCtx;

/*
 * bm25_block_ub -- safe upper bound on a term's contribution to any doc in the
 * block described by *imp, FROM THE POSTINGS THAT BLOCK HOLDS (see bm25_wand.c
 * for the monotonicity proof). That is the doc's whole contribution except for a
 * doc whose postings straddle a block boundary -- possible on a multi-field
 * index in a segment written before the writer cut blocks at document
 * boundaries (issue #289) -- which draws the rest from the neighbouring block.
 * The driver covers that case itself: the global bound sums per-field maxima,
 * and the deep check adds a straddling last doc's continuation (its postings in
 * the next block, scored exactly). A
 * caller that needs a bound on a doc's WHOLE contribution must do likewise; this
 * value alone is not one.
 *
 * It owes DOMINATION, not bit-exactness with the scorer's arithmetic: the
 * driver's prune sites regroup and reorder these bounds in ways WAND itself
 * requires, so they widen their threshold comparison by the worst-case
 * regrouping error (bm25_wand.c:wand_widen_ub) rather than assume the sums
 * agree bit-for-bit. This does NOT touch the bit-exactness the scan actually
 * promises -- bm25_wand_cursor_score_doc's per-posting fold and the BIT-EXACT
 * CONTRACT on BM25TopK below -- both of which remain exact.
 */
extern double bm25_block_ub(const BM25BlockImpact *imp, const BM25WandCtx *ctx);

/*
 * bm25_wand_ctx_build -- pure data assembly, no I/O: pack already-computed
 * per-field idf_f/avgdl_f (the caller's stat computation, however it got
 * them) and fcfg (k1/b/boost) into a BM25WandCtx, deriving field_in_query
 * from qfield (BM25_FIELD_ALL, or one dense field id). Deliberately split out
 * from the stat computation itself: the bm25_debug_block_ub /
 * bm25_debug_term_contrib probes (bm25_debug.c) compute idf_f/avgdl_f for one
 * term, while the WAND driver gets a ctx per query term from
 * bm25_wand_prepare_terms (bm25_wand.c), which calls this once per term. Both
 * funnel through this one assembly point instead of duplicating the
 * struct-packing.
 */
extern void bm25_wand_ctx_build(BM25WandCtx *ctx, uint32 field_count,
                                const double *idf_f, const double *avgdl_f,
                                const BM25FieldConfig *fcfg, int32 qfield);

/*
 * BM25WandStats -- instrumentation counters for one WAND build, threaded through
 * as an OPTIONAL (nullable) out-param: the real scan seam and bm25_debug_wand_rank
 * pass NULL (a null check is the only overhead), while the bm25_wand_stats debug
 * SRF (M2b Task 11) passes a live struct to prove the block-skip path actually
 * runs rather than silently decoding every block. Bit-exact parity against the
 * exhaustive scorer (sql/43_wand_parity.sql) cannot distinguish a genuinely
 * pruning WAND from a "neutered" one that decodes every block and never skips --
 * both produce the identical right answer. blocks_skipped is the only direct
 * witness that next_geq's header-only skip fired at all.
 *
 *   blocks_examined -- every block header TOUCHED exactly once over the cursor's
 *     lifetime: a full decode (cursor open, a linear next() block crossing, or
 *     the block a skip run lands on) or a next_geq header-only peek that gets
 *     bypassed. A block that gets peeked and then decoded as the landing block
 *     is counted ONCE (at the peek), not twice.
 *   blocks_skipped -- the subset of blocks_examined bypassed via a HEADER-ONLY
 *     read (bm25_seg_block_header_read), never varbyte-decoded. Deliberately
 *     EXCLUDES the cursor's already-resident block when a skip run begins on it
 *     (that block was decoded before the skip started, so it was never bypassed
 *     header-only -- counting it here would overstate real header-only skips).
 *     The per-cursor bm25_wand_cursor_blocks_skipped accessor applies the SAME
 *     rule, so this field equals the sum of that accessor over every cursor the
 *     build opened -- see its comment for the invariant.
 *   docs_scored -- one increment per bm25_wand_cursor_score_doc call in
 *     bm25_wand_segment, i.e. once per (query term, pivot candidate) pair
 *     actually decoded and scored. A driver that never prunes trends toward one
 *     entry per (term, doc) in the corpus; genuine pruning keeps it far below.
 *   deep_check_skips -- incremented ONLY when the BLOCK-MAX DEEP CHECK itself
 *     (bm25_wand_segment's bsum-vs-theta branch, summing every ALIGNED
 *     cursor's CURRENT block_max, plus the continuation for a cursor whose last
 *     doc straddles into the next block (#289); the comparison is widened by wand_widen_ub,
 *     which does not change WHEN this counter fires, only how close to theta
 *     bsum must land to avoid the skip) decides to shallow-skip. blocks_skipped
 *     alone cannot distinguish this from plain WAND pivot ALIGNMENT (the
 *     `cur[0] < pivot` branch's own next_geq call, which every disjunctive
 *     WAND needs regardless of any block-max extension) -- a corpus whose
 *     query terms are mutually exclusive can drive blocks_skipped > 0 through
 *     alignment alone while the deep check's multi-cursor bsum sum NEVER
 *     actually engages (see sql/44_wand_skip.sql Gate 1 vs Gate 1b). This
 *     counter is the only direct witness that the deep check's OWN shallow-skip
 *     fired, not merely that some next_geq somewhere in the driver did.
 *
 * NOTE: the cursor-open sweep for global_ub (wand_cursor_sweep_global_ub) reads
 * every block's header up front, unconditionally, regardless of whether WAND
 * prunes anything at scan time. It is an open-time cost linear in the term's
 * block count (measured: under 1% of a WAND build on an ordinary corpus, 7.8% on
 * one built to favour skipping it -- ADR 0096), not part of the runtime
 * block-skip behavior this struct exists to witness, so it is deliberately NOT
 * counted here. The deep check's straddle reads (issue #289: the next block's
 * lead, peeked at most once per resident block when it is on another page, and
 * the straddling doc's doclens) are left out on the same reasoning, and so that a
 * block peeked there and decoded later is still counted once.
 */
typedef struct BM25WandStats
{
    uint32  blocks_examined;
    uint32  blocks_skipped;
    uint32  docs_scored;
    uint32  deep_check_skips;
} BM25WandStats;

/*
 * BM25Scored -- one scored result (tid, score) plus (M2b Task 9) the segment
 * source of whichever posting last touched it: (src_hdr, src_gen, src_docid),
 * InvalidBlockNumber for a pending-only candidate. Mirrors bm25_stats.h's
 * BM25AccEnt/BM25ExhScored src_* fields exactly, for exactly the same reason --
 * the seam's M5 key projection (bm25_wand_build_ranking's finalize) needs to
 * resolve a WAND-ranked TID's user key the SAME way the exhaustive path does
 * (a KEYMAP lookup on src_hdr/src_gen, src_docid), and there is no cheap reverse
 * TID->docid lookup to recover this after the fact (see bm25_handler.c's
 * bm25_bulkdelete comment on why that reverse index doesn't exist). The
 * src_* fields do NOT affect ranking order or the heap's worse() comparator
 * -- only tid/score drive that -- they are carried along purely so the
 * top-k survivors can be key-projected once drained.
 */
typedef struct BM25Scored
{
    ItemPointerData tid;
    double          score;
    BlockNumber     src_hdr;
    uint32          src_gen;
    uint32          src_docid;
} BM25Scored;

/*
 * BM25TopK -- bounded top-k min-heap; opaque outside bm25_wand.c so its
 * array/sift internals can change without touching callers.
 *
 * BIT-EXACT CONTRACT: bm25_topk_drain_sorted's output order MUST equal the
 * existing exhaustive scan's comparator (bm25_scan_rank.c:scored_desc) -- score
 * DESCENDING, ties broken by ItemPointerCompare(tid) ASCENDING -- because
 * Task 8's WAND driver has to produce results indistinguishable from the
 * exhaustive scan it replaces. That is why the heap's "worse" predicate and
 * the drain step's sort comparator (both in bm25_wand.c) are defined
 * directly in terms of that exact ordering rather than anything more
 * heap-convenient (e.g. tid-first tiebreaks would be simpler but wrong).
 */
typedef struct BM25TopK BM25TopK;

/*
 * BM25_WAND_TOP_K_MAX -- the largest top-k capacity this heap can represent: the
 * point at which the drained BM25Scored array would no longer fit one palloc.
 *
 * Derived, not chosen, matching the BM25_MAX_DOC_TOKENS idiom (MaxAllocSize /
 * sizeof(BM25Token)) the tokenizer's growth guard uses. It is the bound on
 * bm25_native.wand_top_k (HDL-04) and the bound bm25_topk_create enforces
 * (QRY-10), so the knob and the allocator cannot disagree: an out-of-range k is
 * refused by the GUC machinery with the parameter named, instead of surfacing as
 * palloc's anonymous XX000 "invalid memory alloc request size" from inside a scan.
 *
 * It is a CAPACITY ceiling, not a policy one. The thing that stops a large
 * wand_top_k from costing memory on a small corpus is the grow-on-demand array in
 * bm25_topk_create, which only ever reaches the number of candidates actually
 * offered; before that, the heap allocated sizeof(BM25Scored) * k up front, so
 * `SET bm25_native.wand_top_k = 33554431` cost ~1 GB per ranked scan on a ten-row
 * table. Both halves are needed: growth handles the plausible values, the ceiling
 * handles the implausible ones.
 */
#define BM25_WAND_TOP_K_MAX  ((int) (MaxAllocSize / sizeof(BM25Scored)))

extern BM25TopK *bm25_topk_create(int k, MemoryContext cxt);
extern bool      bm25_topk_full(BM25TopK *h);
/* Live candidate count (0 <= n <= k). The size a bm25_topk_drain_sorted output
 * buffer actually needs -- sizing it by k instead re-introduces the eager
 * k-sized allocation the heap itself no longer makes. */
extern int       bm25_topk_count(BM25TopK *h);

/*
 * bm25_topk_threshold -- the score a candidate must BEAT to be worth
 * offering: the current k-th-best score once the heap is full, or -DBL_MAX
 * (accept anything) before then. A block-max driver compares this against a
 * block's safe upper bound (bm25_block_ub) to skip blocks that cannot
 * possibly improve the top-k -- the entire point of WAND -- so this must
 * always read the heap's live root, never a value cached from an earlier
 * offer (the threshold rises monotonically as the heap fills/improves).
 */
extern double    bm25_topk_threshold(BM25TopK *h);
/* src_hdr/src_gen/src_docid: the candidate's segment source for later M5 key
 * projection (InvalidBlockNumber/0/0 for a pending candidate or a caller with
 * no key-projection use for this heap, e.g. the bm25_debug_topk probe). */
extern bool      bm25_topk_offer(BM25TopK *h, double score, ItemPointer tid,
                                 BlockNumber src_hdr, uint32 src_gen,
                                 uint32 src_docid);
extern int       bm25_topk_drain_sorted(BM25TopK *h, BM25Scored *out);

/* -------------------------------------------------------------------------
 * M2b Task 6: per-term, per-segment pull cursor
 * -------------------------------------------------------------------------
 * A cursor walks ONE term's block-encoded postings run within ONE sealed
 * segment, one posting at a time. It is the primitive the Task 7 block-SKIP
 * jump (next_geq, below) and the multi-segment BMW fan-out
 * (bm25_wand_build_ranking) sit on top of. Task 6 landed it advanced LINEARLY
 * only, so its correctness could be pinned against the exhaustive scorer in
 * isolation (sql/43_wand_parity.sql) before any skip logic existed to get
 * wrong; the driver now advances it both ways -- score_doc's linear
 * per-posting fold and next_geq's block skips.
 *
 * BM25_DOCID_MAX is the cursor's exhaustion sentinel for bm25_wand_cursor_docid:
 * local docids are dense from 0, so this value can never collide with a real
 * one -- a segment would need 2^32 documents for that, far past any real size.
 */
#define BM25_DOCID_MAX  PG_UINT32_MAX

typedef struct BM25WandCursor BM25WandCursor;   /* opaque; see bm25_wand.c */

/*
 * bm25_wand_cursor_open -- position at the term's first posting in this
 * segment (post_root/post_off/df from bm25_seg_dict_lookup, seg already
 * header-read) and precompute global_ub (see bm25_wand_cursor_global_ub)
 * by a header-only sweep of every block up front -- one buffer round trip per
 * block, see wand_cursor_sweep_global_ub. ctx is captured by
 * POINTER, not copied -- it is query-wide (built once per query term, shared
 * across every segment's cursor for that term), so the caller must keep it
 * alive for at least the cursor's lifetime. Allocated in cxt.
 *
 * stats is captured by POINTER too, NOT owned (same lifetime contract as
 * ctx) -- every cursor opened for one WAND build shares the SAME stats
 * struct, so counts accumulate across every term/segment. NULL disables
 * counting entirely (the real scan seam's normal case).
 *
 * all_live must be the result of bm25_seg_reader_init_checked on this same segment
 * in the same call (the WAND driver checks the LIVEDOCS bitmap once per segment and
 * hands the result to each cursor, ADR 0100). true skips the per-posting liveness
 * read; false reads the bitmap per posting, as the debug probes do.
 */
extern BM25WandCursor *bm25_wand_cursor_open(Relation index, BM25SegmentHeader *seg,
                                             BlockNumber post_root, uint16 post_off,
                                             uint32 df, const BM25WandCtx *ctx,
                                             MemoryContext cxt, BM25WandStats *stats,
                                             bool all_live);

/* The current posting's local docid, or BM25_DOCID_MAX once all df postings
 * have been consumed. */
extern uint32  bm25_wand_cursor_docid(BM25WandCursor *cur);

/* Advance exactly ONE posting, decoding the next block on demand when the
 * current one is exhausted (df, not chain end, bounds a term's run -- see
 * bm25_wand.c). A no-op once already exhausted. */
extern void    bm25_wand_cursor_next(BM25WandCursor *cur);

/*
 * bm25_wand_cursor_next_geq -- the Task 7 block-SKIP jump: advance the cursor
 * to the first posting with local docid >= target, bypassing whole blocks
 * that cannot contain it via a HEADER-ONLY read (no docid/tf/field-RLE
 * decode) -- the entire performance point of block-max WAND. A no-op if the
 * cursor is already exhausted or already positioned at or past target (never
 * moves backward). Marks the cursor exhausted (docid() -> BM25_DOCID_MAX) if
 * target lies beyond the term's last posting in this segment. See
 * bm25_wand.c for the state machine and the df-bound that stops a skip at
 * this term's own last block.
 */
extern void    bm25_wand_cursor_next_geq(BM25WandCursor *cur, uint32 target);

/* Cumulative count of blocks bm25_wand_cursor_next_geq has bypassed
 * header-only (never posting-decoded) on this cursor so far -- the debug
 * probe's witness that a skip actually happened rather than a disguised
 * linear walk.
 *
 * Counts on exactly the rule BM25WandStats.blocks_skipped uses (see its
 * paragraph above, which is the authoritative statement of what "header-only"
 * excludes): the already-resident block a skip run STARTS on is NOT counted,
 * because it was fully decoded before the run began -- walking off its tail
 * bypasses nothing. The two counters are therefore the same quantity at
 * different scopes, and the invariant that ties them is
 *
 *     SUM over every cursor of bm25_wand_cursor_blocks_skipped(cursor)
 *         == BM25WandStats.blocks_skipped
 *
 * for one WAND build. Keep them in lockstep: they are incremented together in
 * bm25_wand.c's next_geq, under one !cand_is_resident guard, for this reason.
 * Without that exclusion a value of 1 would be indistinguishable from a plain
 * linear block crossing (peek one header, then decode that same block), which
 * would make the debug probe's "> 0" assertions witness nothing. */
extern uint32  bm25_wand_cursor_blocks_skipped(BM25WandCursor *cur);

/* The CURRENT decoded block's skip key (BM25BlockHeader.last_docid) and safe
 * score upper bound (bm25_block_ub over that block's impact table) -- the
 * per-block quantities a block-skip driver compares against its top-k
 * threshold before deciding whether to decode.
 *
 * Both are now plain field reads: block_max's bm25_block_ub is evaluated once at
 * block load, not per call (QRY-11, issue #153), because the driver asks for it
 * once per aligned cursor per PIVOT ITERATION while the block it describes changes
 * only per BLOCK. The value is unchanged -- bm25_block_ub is a pure function of the
 * impact table and the ctx, neither of which moves while a block is resident. */
extern uint32  bm25_wand_cursor_block_last(BM25WandCursor *cur);
extern double  bm25_wand_cursor_block_max(BM25WandCursor *cur);

/* The term's global upper bound in this segment, precomputed once at open (never
 * changes afterward): for each field, the max over blocks of that field's term of
 * bm25_block_ub, summed over fields. Not the max of the per-block sums, which
 * under-bounds a doc whose postings straddle two blocks (issue #289). */
extern double  bm25_wand_cursor_global_ub(BM25WandCursor *cur);

/*
 * bm25_wand_cursor_score_doc -- fold every field-posting at the CURRENT docid
 * (a doc's postings for one term are adjacent, ascending field_id -- see
 * bm25_accum.c) into *acc ONE POSTING AT A TIME, in the same field-stream
 * (decode) order the exhaustive scorer accumulates them in, then leave the
 * cursor positioned at the next distinct docid (or exhausted). Accumulating
 * into a caller-provided running double (rather than returning a per-term
 * subtotal) is a bit-exactness REQUIREMENT of the BMW driver, not a style
 * choice: the exhaustive scorer does acc[D] += contrib per posting, so a doc
 * matching a term in two fields must fold as ((r+a)+b), never r+(a+b) -- the
 * two differ by a ULP under IEEE-754 and would fail the parity gate. The
 * driver therefore sums a candidate's whole score into ONE running double
 * across all its terms (in query-term order), which is exactly reproduced by
 * calling this per term with the same acc. MUST equal seg_posting_cb's
 * (bm25_scan_rank.c) per-doc, per-term contribution bit-for-bit -- see bm25_wand.c.
 * Caller must check docid() != BM25_DOCID_MAX before calling.
 *
 * *contributed is set to true (never cleared -- OR-accumulate it across the
 * same doc's query terms exactly like *acc) whenever at least one posting
 * cleared the SAME live/in-scope/nonzero-idf gate seg_posting_cb uses before
 * calling bm25_scores_add. This is NOT the same question as "is *acc nonzero
 * afterward": a genuinely scored posting can legitimately contribute exactly
 * 0.0 (e.g. a zero-boost field), and the exhaustive accumulator still inserts
 * that TID (bm25_scores_add ran). Conversely a doc whose ONLY postings for
 * this term are gated out (field-scoped query, C4; tombstoned; a ubiquitous
 * term with idf==0) must NOT be treated as a match even though *acc stays
 * numerically unchanged. A driver deciding whether to OFFER a candidate must
 * test *contributed, never "*acc != 0.0" or "score > threshold" -- the WAND
 * seam's field-scope gate (33_keymap/35_m5_acceptance/32_field_query) depends
 * on this distinction: without it, a doc whose term match lands entirely
 * outside the query's field scope still gets offered with score 0.0 into a
 * heap that isn't yet full, and wrongly appears in the ranked result the
 * exhaustive path would have excluded outright.
 */
extern void    bm25_wand_cursor_score_doc(BM25WandCursor *cur, double *acc,
                                          bool *contributed);

/* -------------------------------------------------------------------------
 * M2b Task 8: the Block-Max WAND driver
 * -------------------------------------------------------------------------
 * The engine (per-segment BMW under one shared global top-k heap) lives here.
 * The per-term idf and pending-arm scoring it needs are in bm25_stats.c
 * (bm25_term_idf, bm25_pending_score_term, the BM25AccEnt accumulator), the
 * layer below both ranking builders, so the driver reuses the exhaustive
 * scorer's exact idf/pending code without calling into the scanner (#67.13, ADR
 * 0093). Two static adapters in bm25_wand.c, bm25_wand_prepare_terms and
 * bm25_wand_score_pending, fit that layer to the driver's per-term context and
 * top-k heap. Nothing in this header is defined anywhere but bm25_wand.c.
 */

/*
 * bm25_wand_build_ranking -- produce the exact top-`wand_top_k` ranked set into
 * so->ranked[]/scores[]/nranked/rcur (+ so->wand_capped), BIT-IDENTICAL to the
 * exhaustive path's top-k prefix (D1/D8). Reuses the caller's already-computed
 * scan snapshot + corpus stats (no new snapshot). Pending is scored first as a
 * non-prunable arm (D6); each segment then runs BMW under the shared heap, theta
 * rising across segments (D5). Also projects so->ranked_keys[] (M5 key_field,
 * Task 9) from each drained candidate's carried segment source exactly like
 * the exhaustive path's finalize; NULL/BM25_KEY_NONE for a keyless index.
 * A capped result also pins its scoring inputs in so->stats_pin (#268), so the
 * over-pull tail rebuild scores under the same ones; NULL when not capped.
 *
 * stats (M2b Task 11), if non-NULL, accumulates blocks_examined/blocks_skipped/
 * docs_scored/deep_check_skips across every segment's BMW pass -- see
 * BM25WandStats. NULL (the real scan seam's and bm25_debug_wand_rank's case)
 * disables counting.
 */
extern void    bm25_wand_build_ranking(Relation index, BM25ScanOpaque so,
                                       const BM25ScanSnapshot *snap,
                                       BM25Token *qtoks, int nq,
                                       uint64 live_ndocs,
                                       const double *avgdl_f,
                                       const uint64 *fld_ndocs,
                                       const BM25FieldConfig *fcfg,
                                       int32 qfield, int wand_top_k,
                                       BM25WandStats *stats);

#endif                          /* BM25_WAND_H */
