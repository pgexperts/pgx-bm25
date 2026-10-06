/* bm25_stats.h -- interface of bm25_stats.c, the statistics layer shared by the
 * exhaustive scorer (bm25_scan_rank.c) and the Block-Max WAND driver (bm25_wand.c).
 *
 * This header exists to hold one layering (#67.13, ADR 0093):
 *
 *     bm25_scan_rank.c ----> bm25_wand.c ----> bm25_stats.c
 *
 * and all three of the scanner's files (bm25_scan.c, bm25_scan_rank.c,
 * bm25_scan_match.c; one file, bm25_scan.c, until #228) also call this layer
 * directly. Nothing declared here may be DEFINED in the scanner or bm25_wand.c, and
 * bm25_stats.c may call nothing that either of them defines. A helper that needs the
 * scanner's or the WAND engine's symbols belongs in that file instead. CI's
 * "Stats-layer call direction" step checks this on the object files after every
 * build. Before this layer existed, the WAND driver reached the same machinery by
 * calling back into bm25_scan.c, the file that had just called it.
 *
 * Not to be confused with the bm25_stats() SQL function, which is defined in
 * bm25_meta.c.
 *
 * Declare a bm25_stats.c symbol here only when another translation unit calls it.
 * Everything else in that file stays static: pending_df, pending_df_by_field,
 * field_df_cb, pending_score_flush and bm25_match_budget_kb.
 */
#ifndef BM25_STATS_H
#define BM25_STATS_H

#include "bm25.h"
#include "utils/hsearch.h"      /* HTAB: the score accumulator and the pending-TID set */

/*
 * BM25ExhScored -- the exhaustive scorer's sort element (bm25_scan_rank.c's
 * scored_desc): tid/score drive the order, src_* carry the M5 key lookup along
 * for the ride. Only the exhaustive scorer in bm25_scan_rank.c builds and sorts these.
 * It is declared here because bm25_stats.c takes its size: the match-set budget's
 * per-document charge (BM25_MATCH_BYTES_PER_DOC) includes one drain-array slot of
 * it. Named distinctly from bm25_wand.h's BM25Scored (the WAND top-k heap's plain
 * {tid,score} element, M2b Task 5) because the two are NOT interchangeable -- this
 * one carries the extra segment-source fields below that the heap has no use for
 * -- and both are visible in any file that includes this header and bm25_wand.h,
 * so a shared name would collide.
 */
typedef struct
{
    ItemPointerData tid;
    double          score;
    /* M5 key_field: the segment source of a scored posting so the fill loop can
     * resolve docid->key from its KEYMAP. src_hdr == InvalidBlockNumber for a
     * pending-only row (no keymap entry -> NULL key). */
    BlockNumber     src_hdr;
    uint32          src_gen;
    uint32          src_docid;
} BM25ExhScored;

/* BM25AccEnt: dynahash entry -- key is the TID, value is the running score.
 * The TID-keyed score accumulator is exactly the M1 structure; only the posting
 * SOURCE changed in Task 7 (segment NORMS + pending instead of the M1 in-place
 * postings). It has header scope because three files use it: the exhaustive
 * scorer in bm25_scan_rank.c (it sizes the accumulator's entries with it and drains the
 * accumulator through it), bm25_stats.c's upsert bm25_scores_add (which every
 * scoring callback, segment or pending, goes through), and bm25_wand.c's pending
 * arm. It was the file-static AccEnt in bm25_scan.c until #67.13. */
typedef struct
{
    ItemPointerData key;
    double          score;
    /* M5 key_field: segment source (header_blkno/gen/local_docid) of the LAST
     * segment posting that scored this TID. All segment postings for one TID resolve
     * to the same user key, so last-wins is exact. src_hdr == InvalidBlockNumber
     * means pending-only (no keymap entry) -> the fill loop leaves the zero key. */
    BlockNumber     src_hdr;
    uint32          src_gen;
    uint32          src_docid;
} BM25AccEnt;

/* -------------------------------------------------------------------------
 * Match-set memory bound (#62.5)
 * -------------------------------------------------------------------------
 * The design (why the bound counts BYTES, and why one running total is shared by
 * every structure a scan materializes) is described at the definitions in
 * bm25_stats.c. The per-structure charges that only the scan makes
 * (BM25_MATCH_BYTES_PER_PRESENCE, BM25_MATCH_BYTES_PER_TID) are in
 * bm25_scan_match.c and bm25_scan.c.
 */

/* dynahash per-element overhead: the HASHELEMENT header (a link pointer plus a
 * uint32 hash value, 16 bytes once padded on a 64-bit build) PLUS the ~8-byte
 * bucket-directory slot that a fill factor of 1 amortizes to one per entry. Both,
 * not just the header -- an earlier 16 made the presence hash the one charge that
 * UNDER-estimated, which would have quietly falsified the "every charge is an
 * over-estimate" property the whole bound rests on. Caught in review. */
#define BM25_HASH_ENTRY_OVERHEAD    24

/* A scan's running materialization total. bm25_match_charge does no accounting when
 * handed NULL or a budget <= 0. The WAND pending arm uses NULL, because the pending
 * list already bounds that accumulator; bm25_match_budget_init never produces 0,
 * since work_mem cannot go below 64 kB. */
typedef struct
{
    int64   budget;     /* bytes this scan may materialize; 0 = unbounded */
    int64   charged;    /* bytes committed so far */
} BM25MatchBudget;

/* Start a fresh budget from bm25_native.max_match_memory (or work_mem when that is
 * 0), read once at scan start. */
extern void bm25_match_budget_init(BM25MatchBudget *b);

/* Charge `bytes` against the budget; ERRORs, naming the governing GUC, once the
 * running total passes it. b == NULL or an unbounded budget is a no-op. */
extern void bm25_match_charge(BM25MatchBudget *b, int64 bytes);

/* Upsert `contrib` into the accumulator entry for `tid` (OR semantics: sum each
 * term's contribution), recording the segment source when there is one. Charges
 * the per-document cost on first insertion; budget == NULL skips accounting. */
extern void bm25_scores_add(HTAB *acc, ItemPointer tid, double contrib,
                            BlockNumber src_hdr, uint32 src_gen, uint32 src_docid,
                            BM25MatchBudget *budget);

/*
 * BM25TermSegLoc -- one term's dictionary entry in one segment, as bm25_term_idf's
 * df pass found it (issue #246). found == false means the term is not in that
 * segment's dictionary; the other fields are then zero and must not be read. The POS fields are the
 * entry's own (Invalid for a pre-M4 segment or a term only in positions-off fields);
 * they cost nothing extra to copy, and a phrase scorer needs them.
 */
typedef struct BM25TermSegLoc
{
    bool        found;
    BlockNumber post_root;
    uint16      post_off;
    uint32      df;
    BlockNumber pos_post_root;
    uint16      pos_post_off;
} BM25TermSegLoc;

/*
 * bm25_term_idf -- the shared per-term df->idf computation. Sums ONE term's df
 * across every live segment plus pending, then writes the per-field idf row.
 * Returns false when the term is absent corpus-wide (df == 0).
 *
 * Called by the exhaustive scorer, by the WAND driver's term preparation, and by
 * the debug probes. All three must get the same idf, so none may compute its own.
 *
 * idf_f must be sized [BM25_MAX_FIELDS] and is zeroed here, so a caller's row is
 * never left holding stack garbage past field_count.
 *
 * out_locs, when not NULL, must be sized [snap->nsegs]: entry si receives this
 * term's dictionary lookup in snap->segs[si], so a scorer on the SAME snapshot can
 * open the term's postings without walking that dictionary a second time. Every
 * entry is written, on every path, whatever the return value.
 */
extern bool bm25_term_idf(Relation index, const BM25ScanSnapshot *snap,
                          const char *term, int termlen,
                          uint64 live_ndocs, const uint64 *fld_ndocs,
                          int32 qfield, double boost,
                          double *idf_f /* [BM25_MAX_FIELDS], zeroed here */,
                          BM25TermSegLoc *out_locs /* [snap->nsegs] or NULL */);

/*
 * bm25_pending_score_term -- score one query term's live pending documents into
 * the TID-keyed accumulator `scores` (entries are BM25AccEnt), registering each
 * scored TID in pending_tids (when non-NULL) so the segment passes dedupe against
 * it: pending wins. The exhaustive scorer and the WAND pending arm both call it,
 * so a pending document's score is bit-identical on either path (D8). budget ==
 * NULL leaves the accumulator unaccounted (the WAND arm, which the pending list
 * already bounds).
 *
 * Renamed from the file-static pending_score_term when it gained header scope.
 */
extern void bm25_pending_score_term(Relation index, BlockNumber pending_head,
                                    uint32 epoch_bound,
                                    const char *term, int termlen,
                                    uint32 field_count, const double *idf_f,
                                    const double *avgdl_f,
                                    const BM25FieldConfig *fcfg, HTAB *scores,
                                    HTAB *pending_tids, BM25MatchBudget *budget);

/* Ranked-key backfill from the pending list, called by BOTH ranking builders
 * (exhaustive and WAND) at the end of their key projection. A pending-only row
 * has no segment to resolve its key from, so without this the newest rows in an
 * index score NULL through bm25_score_key (#100, #204). */
extern void bm25_ranked_keys_fill_from_pending(Relation index, BM25ScanOpaque so,
                                               BlockNumber pending_head,
                                               uint32 epoch_bound);

/* The index-wide key config both ranking builders project ranked-row keys with:
 * the #292 key stamp when the index carries one, else discovered from the first
 * keyed segment, else from the pending records (#204, #303.I). */
extern void bm25_ranked_key_config(Relation index, const BM25ScanSnapshot *snap,
                                   uint8 *km_type, uint16 *km_size);

#endif                          /* BM25_STATS_H */
