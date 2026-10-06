/* bm25_scan.h -- the internal seams of the scan module: between the three files the
 * scanner is split into (bm25_scan.c, the AM callbacks; bm25_scan_rank.c, the
 * ranked builders; bm25_scan_match.c, the phrase and AND-fallback filters), and
 * between the scanner and bm25_debug.c (the debug SRFs), which reuses its
 * scan-start machinery.
 *
 * This header exists because of splits, not a design: every declaration below
 * was file-static in bm25_scan.c until a split put its caller in a different file
 * (#69.5 moved the debug SRFs out; #228, ADR 0101, split the rest three ways).
 * Promoting a static to extern costs something real -- the compiler can no longer
 * prove the function has no outside callers -- so the rule for this header is
 * deliberately narrow:
 *
 *   ONLY declare a scan-module symbol here when a SECOND translation unit
 *   genuinely calls it. Anything used solely inside one of the three files stays
 *   static there, and anything used solely by the debug SRFs lives in bm25_debug.c.
 *
 * What is here, by the file that needs it:
 *   - bm25_debug.c: bm25_scan_corpus_stats and bm25_field_corpus_stats (defined
 *     in bm25_scan_rank.c; see below);
 *   - bm25_scan.c and bm25_scan_rank.c: bm25_qtree_is_multileaf, which
 *     bm25_load_if_needed reads to route a multi-leaf tree to the exhaustive
 *     scorer, and which the exhaustive scorer and the D7 dispatch read too. A
 *     one-line predicate, so it moved here as the static inline it already was
 *     rather than becoming an extern function;
 *   - bm25_scan_rank.c: the phrase-stash and AND-presence types and the six
 *     functions over them, defined in bm25_scan_match.c. The exhaustive scorer
 *     builds these contexts, reads their entries, and passes the two segment
 *     callbacks to bm25_seg_scan_postings.
 *
 * This is not the whole seam, and reading it as such will mislead you:
 * bm25_scan_build_ranking is also called from bm25_debug.c (and from
 * bm25_scan.c) but is declared in bm25.h, where it was already extern long before
 * any split. So the link-level dependency of bm25_debug.c on the scanner is THREE
 * symbols, of which the two corpus helpers below are the ones that split promoted.
 * (It was five, then four after #188 moved bm25_fingerprint_gate out of
 * bm25_scan.c -- see below. #67.13 then moved bm25_term_idf down to bm25_stats.c,
 * and bm25_debug.c reaches it through bm25_stats.h.)
 *
 * The two scan-start helpers below are shared because the debug probes must
 * score off the EXACT values the real scan path uses -- a debug SRF that
 * recomputed idf or corpus stats its own way would silently stop being a
 * reference, which is the whole reason sql/43_wand_parity can trust it.
 *
 * bm25_wand.c does NOT include this header. The WAND driver used to reach back
 * into bm25_scan.c for its per-term idf and pending scoring, through two helpers
 * that were declared here. That was a call-graph cycle, because the scanner
 * calls the driver. #67.13 moved the machinery both builders share into
 * bm25_stats.c, and the two helpers into bm25_wand.c as statics, so the WAND
 * driver no longer calls into the scanner (ADR 0093).
 */
#ifndef BM25_SCAN_H
#define BM25_SCAN_H

#include "bm25.h"
#include "bm25_query.h"         /* BM25Query, for bm25_qtree_is_multileaf */
#include "bm25_stats.h"         /* BM25MatchBudget, in both match contexts */

/*
 * bm25_scan_corpus_stats -- the shared scan-start prologue every ranked path
 * runs: ONE atomic bm25_scan_snapshot, the analyzer fingerprint gate, the
 * field-config load, and the live-ndocs / per-field avgdl corpus stats.
 *
 * Shared rather than copied because the four probes that reach it from
 * bm25_debug.c are only meaningful as references if they see byte-identical
 * inputs to the real scan; five hand-rolled copies of this prologue were what
 * PR-E (#59.5) consolidated in the first place.
 *
 * store_pos may be NULL (WAND and the stat probes never read positions).
 */
extern void bm25_scan_corpus_stats(Relation index, BM25ScanSnapshot *snap,
                                   BM25AnalyzerConfig *qcfg, uint8 *store_pos,
                                   uint64 *live_ndocs, BM25FieldConfig *fcfg,
                                   double *avgdl_f, uint64 *fld_ndocs);

/*
 * bm25_field_corpus_stats -- per-field sumdoclen (out_len) and N_field (out_ndocs)
 * summed across live segments plus pending. Both arrays must be sized
 * >= field_count.
 *
 * Renamed on promotion: the file-static was plain `field_corpus_stats`, and an
 * extern symbol in a shared module shares one namespace with every other
 * extension loaded into the backend. The other three promotions here were
 * already bm25_-prefixed; this one was not. (#228's six match-layer externs
 * kept their names; the note at their prototypes below says why.)
 */
extern void bm25_field_corpus_stats(Relation index, const BM25ScanSnapshot *snap,
                                    uint64 *out_len /* >= field_count */,
                                    uint64 *out_ndocs /* >= field_count */);

/*
 * bm25_fingerprint_gate was declared here while the scan was its only caller. It
 * moved to bm25.h with #188, which gave it a second caller on the WRITE path
 * (bm25_insert): a predicate both sides run does not belong behind a scan-private
 * header. bm25_debug.c reaches it through bm25.h now; nothing else changed about it.
 */

/* M6: does this parsed jsonb tree need the boolean multi-leaf evaluator?
 * A single MATCH/TERM root is copied into so->qterm by bm25_rescan_parse_jsonb
 * and scored as a plain text OR query (WAND-eligible, byte-identical to a
 * (text,text) query); EVERY other root needs the presence-mask scorer
 * (bm25_scan_build_ranking_exhaustive, bm25_scan_rank.c) and
 * must BYPASS WAND -- WAND computes only OR-of-terms, with no per-leaf field
 * scope, query boost, or must_not exclusion. That covers BOOLEAN/BOOST and,
 * for the same reason, a bare PHRASE or WILDCARD root: both are wired and run
 * end to end through this predicate (sql/47_m6_wildcard.sql exercises a bare
 * wildcard root, sql/51_conformance.sql a bare phrase root), and a phrase also
 * has to bypass WAND because its adjacency recheck is a POST-scoring filter
 * (D9 of M4) that score-based pruning could discard a surviving doc from.
 * NULL (a (text,text) query) is never multi-leaf. */
static inline bool
bm25_qtree_is_multileaf(const BM25Query *t)
{
    return t != NULL && t->kind != BM25Q_MATCH && t->kind != BM25Q_TERM;
}

/* -------------------------------------------------------------------------
 * M4 phrase / proximity recheck (C-MATCH, D4/D5/D6/D9)
 * -------------------------------------------------------------------------
 * A multi-term phrase cannot be decided at one posting, so the scorer runs the
 * bag-of-words pass unchanged (filling acc with candidate TIDs + BM25F scores) and
 * ADDITIONALLY, for a phrase query, stashes each phrase term's per-(TID, field)
 * position list. After the full scan, for each candidate TID the matcher (D5) runs
 * per field the doc holds all N terms in; a TID that matches within NO single field
 * is DROPPED from the ranked set (filter-only, D9 -- survivors keep their score). This
 * is a filter over the produced TID set; the bm25_scan_build_ranking contract and the
 * M2b-WAND seam are untouched (D4).
 *
 * The stash is a TID-keyed dynahash; each entry carries a flat [nq * field_count]
 * array of growable uint32 position lists (list index = qi * field_count + field,
 * qi == query TOKEN ordinal). Everything lives in the scan's scratch context, freed
 * with it at end of the ranking build. Bounded by the matching postings (only live,
 * non-deduped postings of the phrase terms are stashed).
 *
 * ALTERNATIVE GROUPS (issue #184) DO NOT MOVE THIS SEAM. A phrase slot is one source
 * WORD of the query and a compound-splitting dictionary can give one word several
 * lexemes; the token -> slot grouping (BM25PhraseSlotMap) is derived once from the
 * analyzer's own positions and consulted ONLY by phrase_recheck_tid, which folds a
 * slot's member lists together just before calling the matcher. The stash therefore
 * stays per-token, cur_qi stays the token ordinal, and both pos callbacks are
 * untouched. Unless a compound-splitting dictionary co-positions several lexemes of a
 * word (analyzer revision 5), each token IS its own slot and the fold is a pointer
 * borrow. */
typedef struct
{
    uint32 *pos;    /* ascending positions for one (TID, qi, field) */
    uint32  n;
    uint32  cap;
} PhrasePosList;

typedef struct
{
    ItemPointerData key;                    /* dynahash key: the doc TID */
    PhrasePosList  *lists;                   /* [nq * field_count], palloc0'd on first use */
} PhraseStashEnt;

/* Phrase-stash context: enough for the segment/pending pos callbacks to resolve
 * local_docid -> TID, honor the pending-wins dedupe, and route positions into the
 * right (TID, qi, field) list. cur_qi is set by the scorer loop before each term's
 * segment/pending pass. */
typedef struct
{
    Relation            index;
    HTAB               *stash;               /* TID -> PhraseStashEnt */
    MemoryContext       cxt;                 /* scratch context for the growable lists */
    uint32              nq;                  /* phrase term count (== query token count) */
    uint32              field_count;
    /* The current segment, as forward cursors over its LIVEDOCS/DOCMAP chains --
     * TermScoreCtx.rdr's shape and reasons (per-posting callback; the one-shot
     * accessors it replaces re-walked each chain from its root per posting and each took
     * a SEGREAD-11 RelationGetNumberOfBlocks, an lseek outside recovery).
     *
     * This REPLACES the separate `seg` pointer this struct used to carry beside it: the
     * reader already holds the header (rdr.h) and the Relation, so keeping both would be
     * two fields to re-point in step for one fact. Set per (segment) pass; left zeroed
     * for the pending pass, which has no segment. This ctx spans a whole query (one per
     * text phrase, one per boolean phrase leaf), so the reader is scoped by those
     * assignments rather than by its own declaration the way TermScoreCtx's is.
     *
     * `index` above stays: pending_phrase_stash reads pending pages directly, with no
     * segment and so no reader. */
    BM25SegReader       rdr;
    HTAB               *pending_tids;        /* TIDs pending owns (segment pass skips them) */
    uint32              cur_qi;              /* query TOKEN ordinal of the current term */
    const bool         *field_store_positions; /* [field_count]; gates the pending stash
                                              * to mirror the sealed reader (off fields
                                              * carry no positions). NULL => all fields on. */
    BM25MatchBudget    *budget;              /* #62.5 shared materialization total */
} PhraseStashCtx;

/* -------------------------------------------------------------------------
 * M4 AND-of-terms fallback (D7 phrase_fallback='and')
 * -------------------------------------------------------------------------
 * When a phrase query lands on a position-less index and phrase_fallback='and', we
 * cannot run the positional matcher, so we degrade to "all N phrase terms present in
 * the doc" (no order/proximity). This is a per-TID presence set: a bitmask of which
 * phrase-term-indices (qi) hit the doc; a TID survives iff its mask has all nq bits.
 * Populated by a plain bm25_post_cb (positions never touched) so it works on exactly
 * the position-less segments that triggered the fallback. */
typedef struct
{
    ItemPointerData key;
    uint64          mask;       /* bit qi set when phrase term qi hit this TID */
} PhraseAndEnt;

typedef struct
{
    Relation            index;               /* pending_and_stash only -- see rdr */
    HTAB               *presence;            /* TID -> PhraseAndEnt */
    /* The current segment as forward cursors, replacing a separate `seg` pointer for the
     * reasons PhraseStashCtx.rdr gives above, and set on the same schedule (per segment
     * pass; zeroed for the pending pass). seg_and_cb runs per posting, and the one-shot
     * accessors it replaces re-walked each chain from its root and took a SEGREAD-11
     * lseek apiece. */
    BM25SegReader       rdr;
    HTAB               *pending_tids;
    uint32              cur_qi;
    int32               qfield;              /* field scope, or BM25_FIELD_ALL */
    BM25MatchBudget    *budget;              /* #62.5 shared materialization total */
} PhraseAndCtx;

/*
 * The phrase / proximity stash and the AND-of-terms presence set (M4), defined in
 * bm25_scan_match.c and driven by bm25_scan_build_ranking_exhaustive in
 * bm25_scan_rank.c, which sets cur_qi and the reader before each term's pass and
 * reads the entries afterwards. The two seg_* functions are bm25_seg_scan_postings
 * callbacks. Names kept from when they were file-static: the build compiles with
 * -fvisibility=hidden, so they are not exported from the module.
 */
extern void seg_phrase_pos_cb(uint32 local_docid, uint32 field_id,
                              const uint32 *positions, uint32 npos, void *state);
extern void pending_phrase_stash(PhraseStashCtx *c, BlockNumber pending_head,
                                 uint32 epoch_bound, const char *term, int termlen);
extern void phrase_and_mark(PhraseAndCtx *c, ItemPointer tid);
extern void seg_and_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state);
extern void pending_and_stash(PhraseAndCtx *c, BlockNumber pending_head,
                              uint32 epoch_bound, const char *term, int termlen);
extern bool phrase_recheck_tid(const PhraseStashEnt *e, uint32 nq, uint32 field_count,
                               int32 qfield, bool ordered, int slop,
                               const BM25PhraseSlotMap *smap);

#endif                          /* BM25_SCAN_H */
