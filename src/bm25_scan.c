/* bm25_scan.c -- index scan: resolve the query term to its postings and stream
 * the matching heap TIDs back to the executor.
 *
 * Flow:
 *   bm25_beginscan  -- allocate BM25ScanOpaqueData + per-scan MemoryContext.
 *   bm25_rescan     -- reset scanctx (freeing prior allocations), parse the
 *                     scan's query (the order-by key, else the first scan key)
 *                     and every other scan key it must still apply (#290).
 *   bm25_load_if_needed -- on first bm25_gettuple call (non-scoring path),
 *                         builds the @@@ match set: unions live TIDs across
 *                         every source (all segments + pending) under one
 *                         snapshot, deduplicated by TID, store in scanctx (see
 *                         the multi-source paragraph further down for what a
 *                         flat OR-union cannot decide).
 *   bm25_gettuple   -- scoring path: lazily build the full ranking on first
 *                     call, then stream TIDs in descending score order;
 *                     non-scoring path: stream the match set built by
 *                     bm25_load_if_needed -- TID-ascending from the flat union,
 *                     score-descending when the build was delegated to the
 *                     exhaustive scorer (phrase, multi-leaf tree, several keys).
 *   bm25_endscan    -- deregister from the active scored-scan registry
 *                     (bm25_deregister_scored_scan); delete scanctx.
 *
 * Per-scan MemoryContext (scanctx):
 *   All scan-persistent state -- qterm, postings, ranked, scores -- is allocated
 *   in so->scanctx so it survives across gettuple calls regardless of the
 *   executor's current context.  MemoryContextReset on rescan frees the old
 *   allocations atomically; MemoryContextDelete in endscan tears it all down.
 *
 * Scoring iterator (Task 11/12; M2b Task 9 WAND wiring). All three functions below
 * are in bm25_scan_rank.c (#228); this file calls only bm25_scan_build_ranking:
 *   bm25_scan_build_ranking_once -- the SEAM dispatcher. Decides, per D7, between
 *     the Block-Max WAND driver (bm25_wand_build_ranking, default on -- see
 *     bm25_wand.c) and bm25_scan_build_ranking_exhaustive (phrase/
 *     proximity/AND-fallback queries, or bm25_native.wand_top_k = 0). Either path fills
 *     so->ranked/scores/nranked/rcur (+ WAND also so->wand_capped/ranked_keys).
 *   bm25_scan_build_ranking_exhaustive -- the multi-source OR-sum scorer (Task 12;
 *     what WAND replaces for the common case, and what a WAND-capped scan's
 *     gettuple over-pull tail fallback -- Task 10 -- reruns for the full order).
 *     Takes ONE bm25_scan_snapshot, then per query term computes one corpus-wide
 *     idf (df summed over ALL segments + pending) and accumulates each doc's
 *     bm25_termscore into a TID-keyed dynahash from EVERY source -- the pending
 *     list first (registering its TIDs), then each segment (skipping tombstoned
 *     docs and TIDs pending already owns: dedupe-by-TID, pending wins) -- then
 *     qsort descending. Scratch lives in a short-lived child context; ranked[]/
 *     scores[] are written into so->scanctx. The contract (fill so->ranked/
 *     scores/nranked in descending order) must not change -- the dispatcher's two
 *     branches and the tail fallback all depend on it being interchangeable.
 *   bm25_scan_build_ranking -- the public entry: a bounded subtransaction retry
 *     wrapper (D-HORIZON/C4) that catches ONLY the option-(d) seg_gen reuse abort
 *     (ERRCODE_T_R_SERIALIZATION_FAILURE) and re-runs the dispatcher with a fresh
 *     snapshot, restoring CurrentMemoryContext/CurrentResourceOwner on every path.
 *   gettuple registers so on the active-scored-scan registry (scoring path)
 *     so bm25_score() can resolve a TID to its score without knowing which
 *     scan is active.
 *
 * The @@@ boolean (non-scoring) path (bm25_load_if_needed) is ALSO multi-source:
 * it unions live TIDs across all segments + pending under one snapshot, so a doc
 * living only in a later segment is never dropped from the match set. It is the
 * SOLE filter for @@@ (amgetbitmap is NULL and gettuple sets xs_recheck = false),
 * so any predicate a flat OR-union cannot decide -- a text phrase / proximity
 * query, a multi-leaf jsonb tree -- is delegated to the exhaustive scorer rather
 * than approximated (#132).
 *
 * Debug surface: the bm25_debug_* SRFs that expose this module's internals to
 * the regression suites live in bm25_debug.c (#69.5), not here. Two scan-start
 * helpers they share with the real scan path -- bm25_scan_corpus_stats and
 * bm25_field_corpus_stats -- are in bm25_scan_rank.c and declared in bm25_scan.h. A
 * probe is only a reference if it runs the code the scan runs, so resist
 * reimplementing any of them over there. (bm25_fingerprint_gate was another until
 * #188 gave it a write-path caller and moved it to bm25_analyzer.c, where both of
 * its operands already lived. bm25_term_idf was another until #67.13 moved it to
 * bm25_stats.c. The same "drive the real one" rule applies to both.)
 *
 * Statistics layer: per-term df -> idf (bm25_term_idf), the scorer's pending arm
 * (bm25_pending_score_term), the TID-keyed accumulator with its match-set budget,
 * and the pending-list key backfill are in bm25_stats.c (#67.13, ADR 0093),
 * because the WAND driver needs the same code. bm25_scan_rank.c calls into that
 * layer and into bm25_wand_build_ranking; this file uses only its match-set
 * budget, for the @@@ union collector. Neither calls back into the scanner.
 *
 * File layout (#228, ADR 0101). The scanner is three files:
 *   - bm25_scan.c (this file): the AM callbacks -- beginscan, rescan and its query
 *     parsing, gettuple, endscan -- and the @@@ match-set build
 *     (bm25_load_if_needed) with its TID collector;
 *   - bm25_scan_rank.c: the corpus prologue, the exhaustive scorer, the D7
 *     dispatcher and the retry wrapper bm25_scan_build_ranking;
 *   - bm25_scan_match.c: the phrase/proximity position stash and recheck, and the
 *     AND-of-terms fallback, which the exhaustive scorer drives.
 * bm25_scan.h holds what crosses between them, under the narrow rule it states.
 */
#include "postgres.h"

#include "bm25.h"
#include "bm25_scan.h"          /* bm25_qtree_is_multileaf */
#include "bm25_stats.h"         /* the match-set budget the @@@ union collector charges */
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "utils/jsonb.h"        /* M6 Task 1 fix: JSONBOID, to guard rescan against jsonb RHS below */
#include "bm25_query.h"         /* M6 Task 2: BM25Query AST + bm25_query_parse/glob_match */
#include "utils/float.h"        /* get_float8_infinity (cur_orderby_dist init) */
#include "access/tableam.h"     /* table_index_fetch_tuple_check (cassert tail check, #268) */
#include "utils/hsearch.h"      /* the cassert tail check's emitted-row set (#268) */
#include "catalog/pg_type.h"    /* TEXTOID/VARCHAROID (SURFACE-09 key-type gate) */
#include "utils/builtins.h"     /* format_type_be (SURFACE-09 message) */
#include "utils/lsyscache.h"    /* getBaseType (SURFACE-09: a domain over text) */
#include "parser/scansup.h"     /* scanner_isspace: the text-RHS whitespace class (#306) */

/* One @@@ union collector entry, per (term, document): the array doubles, so at the
 * growth step it holds the old and the new copy at once, and the survivors are
 * copied out to so->postings before the scratch context dies. 3x covers all three. */
#define BM25_MATCH_BYTES_PER_TID    (3 * sizeof(BM25Posting))

/* The most entries one plain (non-huge) allocation may hold: MaxAllocSize / 16, i.e.
 * 2^26-1. Because cap doubles from 64, the check in tid_collector_add refuses the
 * doubling that would pass it, so the collector stops at 2^25 entries (512 MB). The
 * match-set budget does not keep it under that: 2^25 entries charge 1.5 GB, and
 * max_match_memory goes far higher (SCAN-08, issue #305). */
#define BM25_MATCH_MAX_TIDS     ((uint32) (MaxAllocSize / sizeof(BM25Posting)))

/* TID collector for the non-scoring @@@ path: a growable BM25Posting scratch
 * array (only .tid is consumed downstream) plus the segment header so the seg
 * callback can resolve docid->TID under the live-docs gate. */
typedef struct
{
    /* The current segment as forward cursors over its LIVEDOCS/DOCMAP chains --
     * TermScoreCtx.rdr's shape and reasons, on the membership path. seg_tid_cb runs per
     * posting, so the one-shot accessors it replaces re-walked each chain from its root
     * per posting AND took a RelationGetNumberOfBlocks apiece for the SEGREAD-11 bound,
     * which lseeks on every call outside recovery.
     *
     * This collector outlives one segment -- it accumulates TIDs across every segment
     * and every query token -- so unlike TermScoreCtx it is not constructed per segment
     * and the reader is re-opened at the one site that moves it to a new one. It
     * replaces the `seg` header pointer and the `index` this struct used to carry
     * separately: the reader holds both, and one field cannot fall out of step with
     * itself. */
    BM25SegReader       rdr;
    BM25Posting        *arr;
    uint32              n;
    uint32              cap;
    int32               qfield;    /* C4: field scope, or BM25_FIELD_ALL */
    BM25MatchBudget    *budget;   /* #62.5 shared materialization total */
} TidCollector;

static void
tid_collector_add(TidCollector *tc, ItemPointer tid)
{
    /* #62.5: charged per ENTRY, and an entry here is one (term, document) posting,
     * not one document -- this collector appends per term and dedupes only afterwards
     * (the qsort + unique below). An earlier version of this bound counted documents
     * here, which made the effective limit scale as 1/nterms: a ten-term query
     * errored at a tenth of the matches a one-term query allowed, reporting a
     * document count the query had never reached. Charging the bytes the array
     * actually holds is both correct and self-describing. Tested every add rather
     * than at the doubling boundaries, so the stop is exact instead of rounded up to
     * the next power of two.
     *
     * The budget does NOT keep `cap *= 2` clear of MaxAllocSize: a budget above about
     * 1.5 GB lets n reach 2^25, where the doubled array is one byte past it, and a
     * plain repalloc then fails with the anonymous "invalid memory alloc request
     * size" (XX000) this budget exists to replace. So the doubling checks the ceiling
     * itself and raises a program-limit error naming the scan. A huge allocation is
     * not the answer (ADR 0047 rejects it). The check also keeps the doubling from
     * overflowing cap's uint32. */
    bm25_match_charge(tc->budget, BM25_MATCH_BYTES_PER_TID);

    if (tc->n == tc->cap)
    {
        if (tc->cap > BM25_MATCH_MAX_TIDS / 2)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bm25: query matches more postings than a \"@@@\" scan can collect"),
                     errdetail("The scan holds one entry per matching term and document, "
                               "at most %u of them.", tc->cap),
                     errhint("Make the query more selective. Raising "
                             "bm25_native.max_match_memory does not raise this limit.")));
        tc->cap *= 2;
        tc->arr = repalloc(tc->arr, sizeof(BM25Posting) * tc->cap);
    }
    tc->arr[tc->n].tid    = *tid;
    tc->arr[tc->n].tf     = 0;       /* unused; only .tid matters on this path */
    tc->arr[tc->n].doclen = 0;
    tc->n++;
}

static void
seg_tid_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    TidCollector   *tc = (TidCollector *) state;
    ItemPointerData tid;

    (void) tf;
    /* C4: under a field scope, a doc is a hit only if it posts the term in the
     * queried field; skip postings from other fields. A bare query (all fields)
     * keeps the field-agnostic "hit if ANY field posts the term" behavior. */
    if (tc->qfield != BM25_FIELD_ALL && (int32) field_id != tc->qfield)
        return;
    if (!bm25_seg_reader_doc_is_live(&tc->rdr, local_docid))
        return;
    tid = bm25_seg_reader_docid_to_tid(&tc->rdr, local_docid);
    tid_collector_add(tc, &tid);
}

/*
 * Ascending-TID comparator for a BM25Posting array.
 *
 * Deliberately a real qsort-signature function rather than a cast of
 * ItemPointerCompare itself. Passing ItemPointerCompare through a double cast
 * would be undefined behaviour (C11 6.3.2.3p8 -- calling a function through a
 * pointer to an incompatible type), and it would additionally bake in a silent
 * dependency on tid remaining BM25Posting's first member: the comparator would
 * receive element pointers reinterpreted as ItemPointer, which is only correct
 * at offset 0. Reordering the struct would then sort on tf/doclen bytes and
 * corrupt the dedup compaction below with no compiler diagnostic. Both hazards
 * disappear by doing the member access here, as scored_desc (bm25_scan_rank.c)
 * already does.
 */
static int
posting_tid_cmp(const void *a, const void *b)
{
    return ItemPointerCompare((ItemPointer) &((const BM25Posting *) a)->tid,
                              (ItemPointer) &((const BM25Posting *) b)->tid);
}

/* -------------------------------------------------------------------------
 * Index scan callbacks
 * -------------------------------------------------------------------------*/

/*
 * bm25_scanctx_deregister_cb -- scanctx memory-context callback: unlinks so
 * from the active-scored-scan registry.
 *
 * Why this exists (not just the explicit bm25_deregister_scored_scan call in
 * bm25_endscan): PortalCleanup() skips ExecutorEnd (hence our amendscan)
 * entirely for a portal already marked PORTAL_FAILED -- the comment there
 * reads "other mechanisms will take care of releasing executor resources".
 * A target-list error inside a scored scan (see 41_scored_scan_error.sql)
 * takes exactly that path: the failing statement's portal is torn down by
 * the NEXT statement's CreatePortal("", true, true), which unconditionally
 * MemoryContextDelete()s the portal's heap -- including this scanctx -- with
 * bm25_endscan never having run. Without this callback the registry would
 * keep a node pointing at that about-to-be-freed so, and the next scored
 * scan's bm25_register_scored_scan (which walks the list to move itself to
 * head) would dereference freed memory. Registering the callback here means
 * unlink-on-teardown holds for BOTH the normal endscan path and the abort
 * path, via whichever MemoryContextDelete/Reset happens to run first --
 * bm25_deregister_scored_scan is idempotent, so running it twice (once
 * explicitly in endscan, once via this callback when endscan then deletes
 * scanctx) is harmless.
 */
static void
bm25_scanctx_deregister_cb(void *arg)
{
    bm25_deregister_scored_scan((BM25ScanOpaque) arg);
}

/*
 * bm25_scanctx_arm_deregister_cb -- (re-)register the callback above on
 * so->scanctx.
 *
 * MUST be called after EVERY MemoryContextReset(so->scanctx), not only once
 * in beginscan: PostgreSQL's per-context reset-callback list is one-shot --
 * MemoryContextCallResetCallbacks() pops and invokes each entry at most once
 * per reset/delete, emptying the list as it goes. A callback armed only in
 * beginscan is already consumed by the mandatory bm25_rescan that always
 * follows beginscan (required by the index AM calling convention) -- long
 * before the scan is ever registered, leaving nothing armed for the scan's
 * real teardown. Re-arming here, right after bm25_rescan's own
 * MemoryContextReset, keeps protection live for the rest of the scan's
 * lifetime (including across repeated rescans of the same opaque).
 */
static void
bm25_scanctx_arm_deregister_cb(BM25ScanOpaque so)
{
    MemoryContextCallback *cb = MemoryContextAlloc(so->scanctx, sizeof(MemoryContextCallback));

    cb->func = bm25_scanctx_deregister_cb;
    cb->arg  = so;
    MemoryContextRegisterResetCallback(so->scanctx, cb);
}

/* #242: source of BM25ScanOpaqueData.scan_serial. See bm25_beginscan. */
static uint64 bm25_next_scan_serial = 0;

IndexScanDesc
bm25_beginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc  scan = RelationGetIndexScan(index, nkeys, norderbys);
    BM25ScanOpaque so   = palloc0(sizeof(BM25ScanOpaqueData));

    /*
     * RelationGetIndexScan allocates keyData and orderByData, but the AM is
     * responsible for allocating xs_orderbyvals and xs_orderbynulls when it
     * supports amcanorderbyop.  It palloc's (not palloc0's) the descriptor and
     * then assigns its members individually (see RelationGetIndexScan in
     * genam.c), so xs_recheck, xs_recheckorderby, xs_orderbyvals and
     * xs_orderbynulls are left unset and the AM must initialize them;
     * otherwise the executor may see garbage and call the order-by operator
     * directly before the first amgettuple.
     *
     * Initialize xs_orderbynulls to all-true so that any un-filled slot is
     * treated as NULL (matches the GiST convention for "no value yet").
     */
    scan->xs_recheckorderby = false;
    if (norderbys > 0)
    {
        scan->xs_orderbyvals  = palloc0(sizeof(Datum) * norderbys);
        scan->xs_orderbynulls = palloc(sizeof(bool) * norderbys);
        memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
    }

    so->scanctx = AllocSetContextCreate(CurrentMemoryContext,
                                        "bm25 scan",
                                        ALLOCSET_SMALL_SIZES);
    /* Guarantees the registry never retains a dangling entry even if
     * bm25_endscan is skipped (aborted portal) -- see
     * bm25_scanctx_arm_deregister_cb above. The AM calling convention always
     * runs bm25_rescan (which re-arms) before the first gettuple, so this
     * copy is consumed harmlessly by that call; it is here only so scanctx
     * is never briefly unprotected between beginscan and the first rescan. */
    bm25_scanctx_arm_deregister_cb(so);
    so->loaded  = false;
    so->qtree   = NULL;     /* M6: no jsonb parsed yet; palloc0 already zeroed this,
                             * explicit for the same reason so->loaded is above */
    /* Rank-collapse fix: defined value before the first gettuple stashes a
     * real distance, so &@@ degrades to +inf (not palloc0's 0.0) if somehow
     * projected before any tuple is returned. */
    so->cur_orderby_dist = get_float8_infinity();
    so->cur_ranked_idx = BM25_NO_CUR;   /* R3: not positioned on any row yet */
    /* #242: pre-increment so 0 stays free to mean "unbound" in the score
     * accessors' call-site cache. Backend-local and 64-bit, so it cannot wrap in
     * a backend's lifetime and a stale binding can never match a later scan. */
    so->scan_serial = ++bm25_next_scan_serial;
    scan->opaque = so;
    return scan;
}

/*
 * bm25_field_by_name -- resolve a query field name (the left side of "field:term")
 * to a dense field_id against the index's BAKED field config (R3: the field_name
 * frozen at CREATE INDEX, NOT the live catalog attname -- the index may outlive a
 * column rename). Sets *found and returns the field_id on a hit; *found=false and
 * BM25_FIELD_ALL otherwise (the caller raises the user ERROR so the message cites
 * the offending token). `len` is the trimmed left-substring length.
 *
 * ponytail: linear over <= BM25_MAX_FIELDS attnames; exact match, no case-fold -- field
 * names are attnames and the RHS names them verbatim.
 *
 * Extern (M6): bm25_query.c, a separate translation unit, resolves a jsonb
 * query node's "field" through this SAME function so a node written as
 * bm25_term('nosuch','x') errors identically to a "field:term" RHS naming an
 * unknown column. Declared in bm25.h.
 */
int32
bm25_field_by_name(const BM25FieldConfig *fields, uint32 field_count,
                   const char *name, int len, bool *found)
{
    uint32 f;

    *found = false;
    if (len <= 0)
        return BM25_FIELD_ALL;      /* empty field name (":term") -> no match */
    for (f = 0; f < field_count; f++)
    {
        /* field_name is NUL-padded to BM25_FIELD_NAME_LEN; require an exact
         * length match so "title" does not match a "titles" column prefix. */
        if ((int) strnlen(fields[f].field_name, BM25_FIELD_NAME_LEN) == len &&
            strncmp(fields[f].field_name, name, (Size) len) == 0)
        {
            *found = true;
            return (int32) fields[f].field_id;
        }
    }
    return BM25_FIELD_ALL;
}

/*
 * bm25_query_isspace -- THE whitespace class of the text-RHS micro-parser (#306).
 *
 * Every whitespace decision in bm25_query_field_prefix, bm25_query_phrase_offset
 * and bm25_rescan_parse_phrase goes through this one test. They used to test only
 * ' ', each on its own, so a leading tab silently turned `"a b"` into an OR query
 * (and slipped past bm25_match's #132 phrase refusal), while a trailing space or
 * newline after the closing quote ERRORed. PostgreSQL's SQL-lexer class (space,
 * tab, newline, CR, form feed) is the natural one for text pasted from a form.
 */
static inline bool
bm25_query_isspace(char c)
{
    return scanner_isspace(c);
}

/*
 * bm25_rescan_parse_field -- the "field:term" micro-parse (R9). Recognises a scope
 * with bm25_query_field_prefix (#306: the same rule bm25_match refuses on), resolves
 * the name to a dense field_id against the baked field-config page (root from the
 * metapage -- R3), and rewrites so->qterm in place to the text after the colon so
 * downstream tokenization sees only the term. No scope => so->qfield stays
 * BM25_FIELD_ALL. An unknown name is literal text when the colon is followed by
 * '/' or a digit, and an ERROR otherwise (see the policy comment below).
 *
 * Runs for BOTH the scoring (ORDER BY) and boolean (@@@) paths -- both populated
 * so->qterm before this call. No buffer lock is held in rescan, so reading the
 * field-config page (bm25_meta_read + bm25_fieldcfg_read) is safe here.
 * NOTE: field:term is resolved and tokenized ENTIRELY inside the index scan; it
 * is NEVER routed through bm25_match (which has no index Relation, hardcodes the
 * english analyzer, and cannot know field_id) -- R7. xs_recheck stays false.
 *
 * CONSEQUENCE (documented limitation): because the scope lives in the index scan,
 * it is honored only when the bm25 index actually answers the query. The ranked
 * form, `col @@@ q ORDER BY col &@@ q`, is the reliable way to get that (only the
 * bm25 index computes &@@). The @@@ is required: with amoptionalkey false, an &@@
 * ORDER BY alone is not an index path and every row's distance is +inf (#245).
 *
 * When the planner applies `col @@@ q` as a Filter instead -- enable_indexscan off,
 * an `@@@ ... OR ...` qual (no amgetbitmap), an RLS table (bm25_match is not
 * LEAKPROOF, so it is not pushed below the policy qual), a cheaper competing index
 * -- bm25_match evaluates it on the LHS value alone. It does NOT match all fields:
 *   - a scoped RHS (bm25_query_field_prefix) raises feature_not_supported (#298),
 *     because the only alternative is searching the field NAME as a term in the LHS
 *     column, wrong on every configuration;
 *   - a BARE RHS is matched against the LHS column only, while this scan matches
 *     every field of a multi-column index (ADR 0004). That divergence is documented,
 *     not refused: bm25_match has no index handle to tell the two cases apart.
 */
static void
bm25_rescan_parse_field(IndexScanDesc scan, BM25ScanOpaque so)
{
    int colon;
    int name_start = 0;

    if (so->qterm == NULL || so->qtermlen <= 0)
        return;                         /* nothing to parse; qfield stays ALL */

    /* The scope is bm25_query_field_prefix's: leading whitespace, then a run with
     * no whitespace and no double quote, ended by a colon (#306). This split runs
     * before the phrase parser, which is the right order -- `field:"a b"` needs the
     * field resolved first.
     *
     * The rule's stop at '"' is what keeps a colon INSIDE a phrase from being read
     * as a scope (SCAN-05: `'"error: connection refused"'` used to raise
     * `unknown search field ""error"`): a quote opens phrase syntax, and everything
     * from there on belongs to the phrase parser. Its stop at whitespace is #306:
     * the old split took everything before the first colon, so `meeting 10:30`,
     * `' body:cat'` and `note to self: "a b"` named fields `meeting 10`, ` body`
     * and `note to self` and ERRORed here while the filter path answered them. The
     * cost, accepted: a field name containing whitespace (or a quote) cannot be
     * scoped in text syntax; the jsonb builders (bm25_term etc.) name any field. */
    if (!bm25_query_field_prefix(so->qterm, so->qtermlen, &colon))
        return;                         /* bare RHS (or a leading phrase): BM25F over all fields */
    while (name_start < colon && bm25_query_isspace(so->qterm[name_start]))
        name_start++;

    {
        int                     lhslen = colon - name_start;
        int                     rhslen = so->qtermlen - colon - 1;
        bool                    found;
        BM25MetaPageData        meta;
        BM25FieldConfigHeader   fhdr;
        BM25FieldConfig         fields[BM25_MAX_FIELDS];
        uint32                  field_count = 0;

        /* Resolve the LEFT side against the BAKED field-config page (R3): the
         * field_name frozen at CREATE INDEX, not the live catalog attname. A plain
         * metapage read gives the config root -- no bm25_scan_snapshot (which would
         * palloc a catalog copy we do not need here). An index built without a
         * field-config page (should not happen post-C1) has no named fields ->
         * every field name is unknown. */
        /* Deliberately NOT routed through bm25_scan_load_fieldcfg (Task 2): this
         * resolves field NAME -> field_id only, never k1/b/boost, so the live
         * resolver has nothing to overlay -- the build-era stamp is exactly what
         * field-name resolution wants (R3, above). Same for bm25_rescan's other
         * jsonb-query field_count resolve below. */
        bm25_meta_read(scan->indexRelation, &meta);
        if (meta.field_config_blkno != InvalidBlockNumber)
        {
            bm25_fieldcfg_read(scan->indexRelation, meta.field_config_blkno,
                               &fhdr, fields, NULL);
            field_count = fhdr.field_count;
        }

        so->qfield = bm25_field_by_name(fields, field_count,
                                        so->qterm + name_start, lhslen, &found);
        if (!found)
        {
            /* Unknown-name policy (#306, decision D16). A colon followed by '/' or a
             * digit is literal text: URLs (`http://x`), times (`10:30`), ratios
             * (`3:1`) -- shapes no one types as a field scope, so the whole RHS stays
             * a bare query over every field (qfield is already BM25_FIELD_ALL and
             * qterm is untouched). Every other unknown name ERRORs, because the
             * alternative -- "unknown name means plain text" -- would turn the typo
             * `titel:foo` into a silent `titel OR foo`. That includes a whitespace,
             * quote or end of input after the colon (`Note: x`, `x:"a b"`, `x:`) and
             * the empty name (`:running`, pinned by sql/32); `:30` follows the
             * digit rule like any other unknown name.
             *
             * Off the index, bm25_match cannot tell a known name from an unknown one
             * and refuses EVERY scope-shaped RHS, including the ones accepted here as
             * literal text: refuse or agree, never silently differ (see
             * bm25_query_field_prefix).
             *
             * Residual: a field literally named like a URL scheme or a number
             * (`http`, `10`) scopes as usual, so the literal reading applies only to
             * names the index does not have. */
            char    follower = (colon + 1 < so->qtermlen) ? so->qterm[colon + 1] : '\0';

            if (follower == '/' || (follower >= '0' && follower <= '9'))
                return;
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_COLUMN),
                     errmsg("bm25: unknown search field \"%.*s\"",
                            lhslen, so->qterm + name_start),
                     errhint("A leading word followed by a colon names a field of the bm25 index. "
                             "To search for text of this shape, split it into words and pass each as "
                             "bm25_term(field, word) under bm25_boolean(should => ...).")));
        }

        /* Advance qterm past "field:"; the term is the right substring. Rewrite in
         * place (shift left) so downstream tokenization sees only the term. rhslen
         * may be 0 (e.g. "title:") -> empty term, which tokenizes to zero tokens
         * and matches nothing (accepted per R9, NOT an error). */
        memmove(so->qterm, so->qterm + colon + 1, (Size) rhslen);
        so->qtermlen = rhslen;
    }
}

/*
 * bm25_query_field_prefix -- does this raw @@@ RHS open with a `field:` scope?
 *
 * The rule: after leading whitespace, a run containing no whitespace and no
 * double quote, ended by a colon (whitespace is bm25_query_isspace). So
 * `body:cat`, `body: "a b"` and `:x` (empty name) are scope-SHAPED; `meeting
 * 10:30` (space first) and `"a: b"` (quote first) are not. It is not
 * identifier-only: `10:30` and `http://x` are scope-shaped too.
 *
 * ONE definition, three consumers: bm25_rescan_parse_field (the index path's
 * split), bm25_query_phrase_offset (to skip a scope before looking for the phrase
 * quote) and bm25_match, which refuses any scope-shaped RHS (#298) because it has
 * no index to resolve the name against and would otherwise search a field NAME as
 * a term in the LHS column.
 *
 * INVARIANT (#306): off the index, bm25_match refuses or agrees, it never
 * silently differs. The index path, seeing the same shape, resolves the name: a
 * field scopes, an unknown name before '/' or a digit is literal text, any other
 * unknown name ERRORs. bm25_match cannot make that distinction, and answering
 * `10:30` as text would mean answering `title:2024` (a scope on the index) as
 * text too -- the #298 bug. So it refuses every shape, including those the index
 * answers unscoped (`http://x`, `10:30`): the same asymmetry a phrase has.
 *
 * On true, *out_colon (when non-NULL) receives the colon's offset.
 */
bool
bm25_query_field_prefix(const char *s, int len, int *out_colon)
{
    int i = 0;

    if (s == NULL || len <= 0)
        return false;
    while (i < len && bm25_query_isspace(s[i]))
        i++;
    while (i < len && s[i] != ':' && s[i] != '"' && !bm25_query_isspace(s[i]))
        i++;
    if (i >= len || s[i] != ':')
        return false;
    if (out_colon != NULL)
        *out_colon = i;
    return true;
}

/*
 * bm25_query_phrase_offset -- does this raw @@@ / &@@ RHS name a quoted phrase,
 * and if so where does the quote start?
 *
 * ONE definition, shared by the two places that must agree about it: the scan's
 * micro-parser below (which turns a phrase into a positional predicate) and
 * bm25_match (the OFF-index evaluator, which cannot answer one and must refuse).
 * #132 is exactly what a second, drifting copy costs -- the index path learned to
 * filter phrases while the filter path went on unioning their tokens, and nothing
 * downstream could tell, because xs_recheck is deliberately false.
 *
 * Deliberately SYNTACTIC and permissive: it looks past leading whitespace and an
 * optional `field:` prefix for an opening quote and validates nothing else. A
 * malformed phrase must still reach the real parser below, which owns the error
 * messages for an unterminated quote and a bad slop suffix.
 */
bool
bm25_query_phrase_offset(const char *s, int len, bool allow_field_prefix,
                         int *out_quote)
{
    int i = 0;

    if (s == NULL || len <= 0)
        return false;

    /* Skip leading whitespace so ' "a b"' (or a tab/newline before the quote, #306)
     * still reads as a phrase (the analyzer would discard it anyway, but the
     * opening quote must be found to detect one). */
    while (i < len && bm25_query_isspace(s[i]))
        i++;

    /*
     * Optional `field:` scope prefix, and allow_field_prefix is NOT decoration.
     * The two callers see the RHS at DIFFERENT stages: bm25_match is handed the
     * raw value and must skip a scope prefix itself, while the scan's phrase
     * micro-parser runs on a remainder from which bm25_rescan_parse_field has
     * ALREADY removed one. Skipping unconditionally made the scan strip a second
     * prefix, so `body:x:"a b"` silently swallowed the unvalidated `x:` and became
     * a phrase where it used to be a bare term -- a behaviour change that reached
     * the scored &@@ path too. Pass false from the scan; true from bm25_match.
     *
     * The scope is recognised by bm25_query_field_prefix, the predicate bm25_match
     * also uses to refuse a scope (#298). Its stop at '"' keeps a colon INSIDE a
     * phrase ("a:b") from being mistaken for a scope separator.
     */
    if (allow_field_prefix)
    {
        int colon;

        if (bm25_query_field_prefix(s, len, &colon))
        {
            i = colon + 1;
            /* Re-skip whitespace AFTER the colon. Without this the two paths disagreed
             * on `body: "a b"` (a space after the scope separator): the scan strips
             * `body:` and its parser then skips the space and sees a phrase, while
             * bm25_match stopped at the space and called it a bare term -- and so
             * silently returned the OR-union for a query the index path filters
             * positionally. That is #132's own failure mode, on a shape the index
             * path accepts. */
            while (i < len && bm25_query_isspace(s[i]))
                i++;
        }
    }

    if (i >= len || s[i] != '"')
        return false;                       /* not a quoted phrase: a bare term */
    if (out_quote != NULL)
        *out_quote = i;
    return true;
}

/*
 * bm25_rescan_parse_phrase -- the M4 quoted-phrase / proximity micro-parse (D5, D6).
 *
 * Runs AFTER the field split (bm25_rescan_parse_field), on whatever remains in
 * so->qterm -- a bare (colon-less) "a b" reaches here too, so this is a STAGE that
 * runs regardless of the field split (D6). If the term (after trimming whitespace)
 * is a double-quoted string, it is a phrase:
 *   "a b c"    -> EXACT   (ordered, slop 0)
 *   "a b c"~n  -> UNORDERED W/n
 *   "a b c"~>n -> ORDERED  PRE/n
 * The quoted text is left in so->qterm (quotes + slop suffix stripped) so the scorer's
 * existing bm25_analyze(so->qterm) produces the phrase's stemmed term sequence IN
 * ORDER -- the position stash keys on qtoks[0..nq-1] by token ordinal qi, which
 * phrase_recheck_tid then groups into per-source-word SLOTS before calling the matcher
 * (issue #184). A non-quoted RHS is untouched (plain bare-term / field:term; qphrase
 * stays false).
 *
 * PARSE PRECEDENCE (load-bearing, D5): the ordered marker "~>" MUST be tested before
 * "~", so "a b"~>3 is ordered slop 3, not unordered ">3". A "~" (or "~>") with a
 * non-numeric or empty tail is a PARSE ERROR (fail loud) -- never silently treated as
 * an exact phrase. Any characters after the closing quote other than a valid slop
 * suffix are likewise an error.
 */
static void
bm25_rescan_parse_phrase(BM25ScanOpaque so)
{
    char *s = so->qterm;
    int   len = so->qtermlen;
    int   start,
          i,
          inner_start,
          inner_len;

    if (!bm25_query_phrase_offset(s, len, false, &start))
        return;                             /* not a quoted phrase: leave as bare term */

    /* Trailing whitespace is not "text after the phrase" (#306): `"a b" ` and
     * `"a b"~2\n` parse as `"a b"` / `"a b"~2`, the mirror of the leading
     * whitespace bm25_query_phrase_offset skips. Trimming the whole RHS is safe:
     * a whitespace last byte lies either after the closing quote or inside an
     * unterminated one, which still fails below. */
    while (len > start && bm25_query_isspace(s[len - 1]))
        len--;

    /* Find the closing quote. A phrase has exactly one quoted run; an unterminated
     * quote is a parse error (a lone '"' is not a valid bare term either). */
    inner_start = start + 1;
    for (i = inner_start; i < len && s[i] != '"'; i++)
        ;
    if (i >= len)
        ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                 errmsg("bm25: unterminated phrase quote in query")));
    inner_len = i - inner_start;

    /* Parse the optional slop suffix immediately after the closing quote. */
    {
        int   suf = i + 1;                  /* first char after the closing quote */
        bool  ordered = true;               /* default: exact phrase (ordered, slop 0) */
        int   slop = 0;

        if (suf < len)                      /* something follows the closing quote */
        {
            if (s[suf] != '~')
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("bm25: unexpected text after phrase; "
                                "use \"...\", \"...\"~n, or \"...\"~>n")));
            suf++;
            /* PRECEDENCE: test "~>" before "~". */
            if (suf < len && s[suf] == '>')
            {
                ordered = true;             /* ~>n : ORDERED PRE/n */
                suf++;
            }
            else
                ordered = false;            /* ~n : UNORDERED W/n */

            /* The remaining tail MUST be a non-empty run of digits (the slop). An
             * empty or non-numeric tail is a hard error -- never silently exact. */
            if (suf >= len)
                ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR),
                         errmsg("bm25: phrase proximity operator '~' requires a "
                                "numeric slop (e.g. \"a b\"~3 or \"a b\"~>3)")));
            slop = 0;
            for (; suf < len; suf++)
            {
                if (s[suf] < '0' || s[suf] > '9')
                    ereport(ERROR,
                            (errcode(ERRCODE_SYNTAX_ERROR),
                             errmsg("bm25: invalid phrase slop; expected a number "
                                    "after '~'")));
                slop = slop * 10 + (s[suf] - '0');
                if (slop > BM25_MAX_PHRASE_SLOP)
                    ereport(ERROR,
                            (errcode(ERRCODE_SYNTAX_ERROR),
                             errmsg("bm25: phrase slop too large (max %d)",
                                    BM25_MAX_PHRASE_SLOP)));
            }
        }

        so->qphrase         = true;
        so->qphrase_ordered = ordered;
        so->qslop           = slop;

        /* Rewrite qterm to just the inside-quotes text so downstream tokenization
         * yields the phrase's ordered term sequence. Shift left in place. */
        memmove(so->qterm, s + inner_start, (Size) inner_len);
        so->qtermlen = inner_len;
    }
}

/*
 * bm25_stash_orderby_rhs -- record this scan's ORDER BY key RHS verbatim (#138).
 *
 * The distance projection resolves the owning scan by comparing the expression's
 * own RHS against this copy, so it must be the bytes the CALLER wrote, taken
 * before any micro-parser has touched them. Lives in scanctx, so a rescan's
 * MemoryContextReset frees it and the fresh capture re-stashes -- which is also
 * what keeps a per-row (correlated / parameterised) ORDER BY RHS correct: the
 * stash tracks whatever value this rescan cycle was given.
 */
static void
bm25_stash_orderby_rhs(BM25ScanOpaque so, bool is_jsonb, const char *rhs, int rhslen)
{
    so->orderby_rhs_jsonb = is_jsonb;
    so->orderby_rhs_len   = rhslen;
    so->orderby_rhs       = MemoryContextAlloc(so->scanctx, Max(rhslen, 1));
    memcpy(so->orderby_rhs, rhs, rhslen);
}

/*
 * bm25_rescan_parse_jsonb -- M6 Task 3: the jsonb counterpart of
 * bm25_rescan_parse_field/_parse_phrase. Parses+flattens a (text,jsonb) @@@/&@@
 * RHS into so->qtree, against the SAME baked field-config snapshot
 * bm25_rescan_parse_field reads for "field:term" text-RHS resolution -- so an
 * unknown field name errors identically on both surfaces.
 *
 * ORDERING DEP (Task 2 report, amended by #68): flatten must still run
 * immediately after parse, but no longer because it is the only place the
 * 64-leaf cap is enforced. It is not: bm25_query_count_leaf now fires from
 * parse_leaf, so a 65-leaf tree is rejected DURING parse and never reaches here.
 * flatten_recurse keeps its own copy of the check as a guard for a future caller
 * (it bounds the leaves[] write and the leaf_bit later shifted into a uint64
 * mask), but a tree parsed here has already passed the identical count, so that
 * copy cannot currently fire (ADR 0099). Flatten itself is still required:
 * skipping it would leave leaf_bit unassigned for the shift
 * `UINT64CONST(1) << leaf_bit` downstream.
 * The flattened leaf array itself isn't needed yet (Task 4 re-walks qtree), so
 * it is scratch, not stored.
 *
 * A single MATCH/TERM leaf is made text-equivalent: its field_id is already
 * resolved (parse_field_id ran bm25_field_by_name), and its raw text is copied
 * straight into so->qterm/so->qtermlen -- EXACTLY the state bm25_rescan_parse_field
 * leaves a (text,text) RHS in. The text micro-parsers are deliberately NOT called
 * on this path (a MATCH/TERM leaf's text has no ':'-field-scope or "phrase" quoting
 * of its own -- those are separate query-tree node kinds -- so re-running them here
 * would misparse a term that happens to contain ':' or start with '"'). Every
 * downstream consumer (the scorer's bm25_analyze, bm25_load_if_needed's filter
 * union) then runs the byte-identical code path a (text,text) query would.
 *
 * M6 wires BOTH scan paths to the SAME multi-leaf evaluator, so a jsonb tree parses
 * identically regardless of scoring:
 *   - SCORED (&@@): a BOOLEAN/BOOST/PHRASE/WILDCARD root leaves so->qterm NULL and
 *     bm25_gettuple's ranking build reads so->qtree (expanding each WILDCARD leaf over
 *     the dict; Task 4/6/7).
 *   - FILTER-ONLY (@@@, no ORDER BY): bm25_load_if_needed reuses that SAME exhaustive
 *     membership computation for the surviving-TID set, so the @@@ filter set equals
 *     the ranked set by construction (D12, Task 8).
 * A single MATCH/TERM root is still copied into so->qterm (text-equivalent) for both.
 * The ONE unwired leaf shape -- a must_not (negated) PHRASE leaf -- is rejected here,
 * eagerly over the flattened leaves (any depth), for both paths: this single choke
 * point both bm25_rescan branches funnel through, before any gettuple, is why an
 * unwired shape can never slip past as a silent mis-score.
 */
static void
bm25_rescan_parse_jsonb(IndexScanDesc scan, BM25ScanOpaque so, Jsonb *jb)
{
    BM25MetaPageData      meta;
    BM25FieldConfigHeader fhdr;
    BM25FieldConfig       fields[BM25_MAX_FIELDS];
    uint32                field_count = 0;
    BM25Query            *leaves[BM25_QUERY_MAX_LEAVES];

    bm25_meta_read(scan->indexRelation, &meta);
    if (meta.field_config_blkno != InvalidBlockNumber)
    {
        bm25_fieldcfg_read(scan->indexRelation, meta.field_config_blkno,
                           &fhdr, fields, NULL);
        field_count = fhdr.field_count;
    }

    so->qtree = bm25_query_parse(jb, fields, field_count, so->scanctx);

    {
        int nleaves = bm25_query_flatten(so->qtree, leaves, BM25_QUERY_MAX_LEAVES);
        int i;

        /* Both scan paths now drive the SAME multi-leaf evaluator (scored via
         * bm25_gettuple's ranking build; filter-only via bm25_load_if_needed reusing
         * it for membership -- Task 8/D12), so the ONE leaf shape neither path can honor
         * is rejected here for both, eagerly over the flattened leaf set (a leaf at any
         * depth is caught) before the scan builds anything -- a clean ERROR rather than
         * a silent mis-score:
         *   - A must_not (negated) PHRASE leaf: its per-doc position stash is
         *     non-idempotent (positions are APPENDED), so it cannot reuse the
         *     pending-wins dedup a negated presence bit gets for free (a bit OR is
         *     idempotent; appending a stale segment doc's positions on top of the live
         *     pending doc's is not). A must_not term/match/wildcard leaf is unaffected
         *     (presence-only). Wiring must_not phrase needs the pending-TID dedup the
         *     positive path gets from bm25_pending_score_term -- deferred. */
        for (i = 0; i < nleaves; i++)
        {
            if (leaves[i]->kind == BM25Q_PHRASE && leaves[i]->negated)
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bm25: a phrase leaf inside must_not is not yet "
                                "supported in a jsonb query tree")));
        }
    }

    /* A single MATCH/TERM root is made text-equivalent (Task 3): its resolved field
     * and raw text are copied into so->qfield/so->qterm so the plain OR/text scorer
     * (and WAND) run it byte-for-byte like a (text,text) query. A BOOLEAN/BOOST root
     * on the scored path leaves so->qterm NULL -- bm25_scan_build_ranking_exhaustive
     * detects the tree (bm25_qtree_is_multileaf) and scores off so->qtree directly. */
    if (so->qtree->kind == BM25Q_MATCH || so->qtree->kind == BM25Q_TERM)
    {
        so->qfield   = so->qtree->field_id;
        so->qtermlen = so->qtree->textlen;
        so->qterm    = MemoryContextAlloc(so->scanctx, so->qtermlen);
        memcpy(so->qterm, so->qtree->text, so->qtermlen);

        /* QRY-05, second enforcement site. This shortcut discards the leaf and hands the
         * raw text to the plain text scorer, so the multi-token check in the tree path
         * (bm25_scan_build_ranking_exhaustive's leaf loop) never sees a single-leaf
         * TERM query. Checked here instead, which is also the earliest point it can be:
         * scan->indexRelation gives the analyzer, and erroring now costs nothing
         * downstream. */
        if (so->qtree->kind == BM25Q_TERM)
        {
            BM25AnalyzerConfig  tcfg;
            BM25Token          *ttoks;
            int                 ntok;
            MemoryContext       tcxt = AllocSetContextCreate(CurrentMemoryContext,
                                                             "bm25 term-leaf check",
                                                             ALLOCSET_SMALL_SIZES);
            MemoryContext       told = MemoryContextSwitchTo(tcxt);

            bm25_analyzer_config(scan->indexRelation, &tcfg);
            ntok = bm25_analyze(&tcfg, so->qterm, so->qtermlen, &ttoks);
            MemoryContextSwitchTo(told);

            if (ntok > 1)
            {
                int   n = so->qtermlen;
                char *t = so->qterm;

                MemoryContextDelete(tcxt);
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: \"term\" query \"%.*s\" analyzes to %d terms; "
                                "\"term\" accepts exactly one", n, t, ntok),
                         errhint("Use \"match\" for OR semantics over several terms, "
                                 "or \"phrase\" to require them adjacent.")));
            }
            MemoryContextDelete(tcxt);
        }
    }
}

/*
 * bm25_free_if_detoasted -- release a detoasted varlena, if one was made (SCAN-07).
 *
 * Both return the argument's own pointer when no copy was needed, and a FRESH
 * palloc in CurrentMemoryContext when one was. WHEN that is differs between them,
 * and the difference is worth stating because it decides which sites can actually
 * leak: DatumGetTextPP is PG_DETOAST_DATUM_PACKED, which copies only for an
 * externally-stored or compressed datum and hands back a SHORT-header varlena
 * as-is -- that is the point of the _PACKED variant -- while DatumGetJsonbP is the
 * full PG_DETOAST_DATUM and copies a short-header datum too.
 *
 * Comparing the returned pointer against the original datum covers both without
 * having to know which -- it is exactly what PG_FREE_IF_COPY does; that macro is
 * unusable here because these datums come off a ScanKey rather than fcinfo.
 *
 * WHY IT MATTERS ON THIS PATH SPECIFICALLY. bm25_rescan runs from
 * ExecReScanIndexScan with CurrentMemoryContext == estate->es_query_cxt:
 * ExecIndexEvalRuntimeKeys switches to the per-tuple context and back BEFORE
 * index_rescan, so nothing here is reclaimed until end of query. A parameterized
 * nested loop rescans once per outer row, so a toasted or compressed RHS left one
 * detoasted copy per outer row in the query context. Small for ordinary query
 * strings, unbounded in principle -- the RHS is user-supplied text/jsonb. This is
 * the same leak class the H17 comments here and in bm25_scan_rank.c measured at
 * 1.85 GB and fixed for the ranking builder's scratch; the rescan call sites' OWN
 * allocation was left behind then, and this is it.
 *
 * The scan keeps no pointer into the freed copy: the text paths memcpy into
 * so->scanctx before the release, and the jsonb paths release only after
 * bm25_rescan_parse_jsonb has returned (the parsed tree is palloc'd in
 * so->scanctx, and bm25_stash_orderby_rhs copies the identity bytes).
 */
static void
bm25_free_if_detoasted(void *p, Datum orig)
{
    if ((Pointer) p != DatumGetPointer(orig))
        pfree(p);
}

/*
 * bm25_query_rhs_type_ok / bm25_check_key_type -- SURFACE-09.
 *
 * Every reader of sk_argument below decodes a non-jsonb subtype as a text
 * varlena, with no other test. The shipped opclass registers only (text,text) and
 * (text,jsonb), but CREATE OPERATOR CLASS never calls amvalidate, so a superuser
 * can register, say, a (text,int4) operator; the planner then builds an index
 * scan on it and DatumGetTextPP dereferences the int4 VALUE as a pointer (backend
 * SIGSEGV, cluster restart). The scan key is the only point every such opclass
 * must pass, so the type gate is here: any key whose subtype is not one this AM
 * decodes is a clean ERROR before its argument is touched.
 *
 * A domain over text/varchar is accepted (its Datum genuinely is a text varlena,
 * the same rule as bm25_check_indexed_column_types). A domain over jsonb is NOT:
 * the dispatch tests sk_subtype == JSONBOID, so it would be misread as text.
 * getBaseType is consulted only when the fast path misses, so the shipped
 * operators cost at most two integer compares.
 */
bool
bm25_query_rhs_type_ok(Oid typid)
{
    Oid     base;

    if (typid == JSONBOID || typid == TEXTOID || typid == VARCHAROID ||
        typid == InvalidOid)
        return true;
    base = getBaseType(typid);
    return base == TEXTOID || base == VARCHAROID;
}

static void
bm25_check_key_type(ScanKey key)
{
    if (!bm25_query_rhs_type_ok(key->sk_subtype))
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bm25: a query argument of type %s cannot be used with "
                        "this access method",
                        format_type_be(key->sk_subtype)),
                 errdetail("The operator's right-hand type must be text, varchar "
                           "or jsonb."),
                 errhint("Check the index's operator class with amvalidate().")));
}

/*
 * bm25_rescan_parse_key -- read one non-NULL scan key's RHS into so's query fields.
 *
 * The one parse every key goes through: the scan's own query (the ORDER BY key, or
 * the first WHERE key when there is none) and, since #290, each WHERE key the scan
 * must still apply, parsed into its own opaque. One path is what makes an invalid
 * WHERE RHS fail exactly as the same text fails as the scan's query.
 *
 * sk_subtype is the operator's actual RHS type OID (M6: Task 1 registered the
 * (text,jsonb) operators into text_bm25_ops): jsonb takes the AST path, and the
 * (text,text) path is subtype TEXTOID or 0/untyped.
 *
 * stash_orderby records the scan's identity for the distance projection (#138). It
 * must see the bytes the CALLER wrote, so it runs before any micro-parser: jsonb's
 * binary form is canonical per value, and text needs a second, pristine copy
 * because the field-scope and phrase micro-parsers rewrite so->qterm in place.
 */
static void
bm25_rescan_parse_key(IndexScanDesc scan, BM25ScanOpaque so, ScanKey key,
                      bool stash_orderby)
{
    text *q;

    bm25_check_key_type(key);   /* SURFACE-09: before the argument is decoded */
    if (key->sk_subtype == JSONBOID)
    {
        Jsonb *jb = DatumGetJsonbP(key->sk_argument);

        if (stash_orderby)
            bm25_stash_orderby_rhs(so, true, (const char *) VARDATA_ANY(jb),
                                   VARSIZE_ANY_EXHDR(jb));
        bm25_rescan_parse_jsonb(scan, so, jb);
        /* SCAN-07: released AFTER the parse, which reads through jb. */
        bm25_free_if_detoasted(jb, key->sk_argument);
        return;
    }

    q = DatumGetTextPP(key->sk_argument);
    so->qtermlen = VARSIZE_ANY_EXHDR(q);
    so->qterm = MemoryContextAlloc(so->scanctx, so->qtermlen);
    memcpy(so->qterm, VARDATA_ANY(q), so->qtermlen);
    if (stash_orderby)
        bm25_stash_orderby_rhs(so, false, so->qterm, so->qtermlen);
    bm25_free_if_detoasted(q, key->sk_argument);   /* SCAN-07 */

    /* C4/M4: the text micro-parsers (":"-field-scope split, quoted-phrase
     * detection) apply only to a (text,text) RHS's own mini-syntax, so the jsonb
     * branch above returned before reaching them. A jsonb leaf's field is already
     * resolved and it has no quoting convention of its own (bm25_rescan_parse_jsonb
     * copied its text straight into qterm) -- running these on it would misparse a
     * term that happens to contain ':' or start with '"'. */

    /* C4: split a leading `field:` scope (bm25_query_field_prefix, #306) from the
     * term. Sets so->qfield and shrinks so->qterm to the post-colon term; a bare
     * RHS, or an unknown name read as literal text, leaves qfield == ALL. */
    bm25_rescan_parse_field(scan, so);

    /* M4 (C-MATCH): after the field split, parse the remaining term for a quoted
     * "phrase" with an optional ~n / ~>n slop suffix (D5/D6). Runs regardless of the
     * colon so a bare "a b" is a valid phrase. Sets so->qphrase and shrinks so->qterm
     * to the inside-quotes text; a non-quoted RHS leaves qphrase == false. */
    bm25_rescan_parse_phrase(so);
}

/*
 * bm25_scankeys_same_rhs -- do two non-NULL scan keys carry the byte-identical
 * query? The same test the distance projection uses to identify a scan (#138): the
 * operator family must agree (a text and a jsonb RHS never match), then the payload
 * bytes. jsonb's binary form is canonical per value, so equal values compare equal.
 *
 * #290 uses it to skip a WHERE key whose set is, by construction, the scan's own
 * query's set. The column (sk_attno) is deliberately not compared: the scan never
 * reads it, so `title @@@ q` and `body @@@ q` name the same set (all fields).
 */
static bool
bm25_scankeys_same_rhs(ScanKey a, ScanKey b)
{
    bool    a_jsonb = (a->sk_subtype == JSONBOID);
    bool    same;

    /* SURFACE-09: bm25_rescan calls this as (where_key, primary) BEFORE the
     * WHERE key is parsed, so this is the first reader of a's argument (the
     * load-bearing check; sql/144 crashes without it). b, the primary, already
     * passed bm25_rescan_parse_key; it is checked again so the function does
     * not depend on its callers' argument order. */
    bm25_check_key_type(a);
    bm25_check_key_type(b);
    if (a_jsonb != (b->sk_subtype == JSONBOID))
        return false;
    if (a_jsonb)
    {
        Jsonb  *ja = DatumGetJsonbP(a->sk_argument);
        Jsonb  *jb = DatumGetJsonbP(b->sk_argument);

        same = VARSIZE_ANY_EXHDR(ja) == VARSIZE_ANY_EXHDR(jb) &&
            memcmp(VARDATA_ANY(ja), VARDATA_ANY(jb), VARSIZE_ANY_EXHDR(ja)) == 0;
        bm25_free_if_detoasted(ja, a->sk_argument);
        bm25_free_if_detoasted(jb, b->sk_argument);
    }
    else
    {
        text   *ta = DatumGetTextPP(a->sk_argument);
        text   *tb = DatumGetTextPP(b->sk_argument);

        same = VARSIZE_ANY_EXHDR(ta) == VARSIZE_ANY_EXHDR(tb) &&
            memcmp(VARDATA_ANY(ta), VARDATA_ANY(tb), VARSIZE_ANY_EXHDR(ta)) == 0;
        bm25_free_if_detoasted(ta, a->sk_argument);
        bm25_free_if_detoasted(tb, b->sk_argument);
    }
    return same;
}

/*
 * bm25_where_opaque -- a fresh opaque to parse one WHERE key into (#290).
 *
 * Only the query fields and scanctx are used (see BM25ScanOpaqueData.where): the
 * filtered builder hands it to the exhaustive scorer for its membership set, the
 * same computation the @@@ path delegates a phrase or a boolean tree to. palloc0
 * leaves every array NULL and every count 0; the rest is the state bm25_rescan
 * gives the scan's own query before parsing it. It shares the scan's scanctx, so
 * the parse's allocations die with the next rescan's reset, and it is never put on
 * the scored-scan registry.
 */
static BM25ScanOpaque
bm25_where_opaque(BM25ScanOpaque so)
{
    BM25ScanOpaque w = MemoryContextAllocZero(so->scanctx, sizeof(BM25ScanOpaqueData));

    w->scanctx = so->scanctx;
    w->qfield = BM25_FIELD_ALL;
    w->cur_ranked_idx = BM25_NO_CUR;
    w->cur_orderby_dist = get_float8_infinity();
    w->nqtoks = -1;
    return w;
}

void
bm25_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
            ScanKey orderbys, int norderbys)
{
    BM25ScanOpaque so = (BM25ScanOpaque) scan->opaque;
    bool           scoring_key_isnull;
    bool           where_key_isnull = false;
    ScanKey        primary;
    int            i;

    /*
     * Free all prior scan-persistent allocations (qterm, postings, ranked,
     * scores).  After the reset, the pointers are dangling -- null them so no
     * code accidentally reads stale values before the next allocation.
     */
    MemoryContextReset(so->scanctx);
    /* The reset above also consumed scanctx's one-shot deregister callback
     * (see bm25_scanctx_arm_deregister_cb); re-arm it so the registry stays
     * protected through this scan's next load/registration cycle. */
    bm25_scanctx_arm_deregister_cb(so);
    /* #138: the stash lives in scanctx, so the reset above freed it. NULL it
     * here rather than only in the scoring branch below: a rescan that goes from
     * a scoring key to a non-scoring one takes the `else` path and would
     * otherwise leave this pointing into freed memory for the distance resolver
     * to memcmp. Same class of bug as the qtermlen reset documented below. */
    so->orderby_rhs       = NULL;
    so->orderby_rhs_len   = 0;
    so->orderby_rhs_jsonb = false;
    so->qterm    = NULL;
    so->qtermlen = 0;      /* M6: a multi-leaf jsonb tree leaves qterm NULL and never
                            * sets qtermlen; without this reset a stale nonzero length
                            * from a PRIOR single-leaf/text rescan of this same opaque
                            * (a parameterized @@@/&@@ $1::jsonb plan alternating single-
                            * and multi-leaf values) would pair a NULL qterm with a >0
                            * len at the scorer's bm25_analyze(so->qterm, so->qtermlen)
                            * -> NULL deref. The parse re-sets it for every path that
                            * uses it, so zeroing here is otherwise inert. */
    so->postings = NULL;
    so->ranked   = NULL;
    so->scores   = NULL;
    so->score_by_tid = NULL;   /* R3 Task 4: freed by the MemoryContextReset above (scanctx) */
    so->score_by_key = NULL;
    so->qtree    = NULL;   /* M6: died with the reset above; re-parsed below if jsonb */
    so->where    = NULL;   /* #290: the WHERE opaques died with the reset too */
    so->nwhere   = 0;
    so->where_has_orderby = false;
    /*
     * M5 key projection: ranked_keys is palloc'd in scanctx too, so it dangles
     * exactly like the pointers above and belongs in this list. The stride and
     * type travel with it -- bm25_resolve_score_key gates on the POINTER, not on
     * nranked (bm25_score.c: "if (s->ranked_keys == NULL || type mismatch) ...")
     * and then indexes at ranked_key_size, so a stale non-NULL pointer paired
     * with a stale nonzero stride would be load-bearing rather than inert.
     *
     * No reachable failure today, and by construction rather than luck: the
     * MemoryContextReset above fires scanctx's deregister callback, which
     * unlinks this scan from the registry bm25_score.c walks before any field
     * here is nulled, and the ranking builder reassigns all three before
     * bm25_register_scored_scan re-links it. This keeps the block honest to the
     * comment above it -- and keeps that "by construction" argument from being
     * the only thing standing between a future early-return in a builder and a
     * read of freed memory at a stale stride.
     */
    so->ranked_keys     = NULL;
    so->ranked_key_present = NULL;   /* same scanctx lifetime; travels with ranked_keys */
    so->ranked_key_type = BM25_KEY_NONE;
    so->ranked_key_size = 0;

    /* Reset cursor state; postings/ranking will be loaded on next gettuple. */
    so->loaded  = false;
    so->cur     = 0;
    so->npost   = 0;
    so->nranked = 0;
    so->rcur    = 0;
    so->cur_ranked_idx = BM25_NO_CUR;   /* R3: rescan un-positions the scan */
    /* Rank-collapse fix: a stale distance from a PRIOR rescan of this same
     * opaque must not leak into &@@ projections before the new scan's first
     * gettuple stashes a real one. */
    so->cur_orderby_dist = get_float8_infinity();
    /* M2b Task 10: a stale wand_capped/wand_tail_done from a PRIOR rescan of
     * this same opaque (e.g. a parameterized nested-loop inner scan) must not
     * leak into the new scan -- bm25_gettuple's over-pull tail fallback keys
     * off exactly these two flags. Cleared here rather than relying on the
     * beginscan-time palloc0 zero-init, which only covers the FIRST scan. */
    so->wand_capped    = false;
    so->wand_tail_done = false;
    so->stats_pin      = NULL;      /* #268: in scanctx, freed by the reset above */
    so->qfield  = BM25_FIELD_ALL;   /* C4: bare-RHS default; overwritten by the parse */
    so->qphrase = false;            /* M4: set by bm25_rescan_parse_phrase below */
    so->qphrase_ordered = false;
    so->qslop   = 0;
    so->qcfg_valid = false;         /* M4 snippet: qcfg re-resolved lazily when scoring publishes */
    /* TEXT-12: the cached query tokens are only meaningful under qcfg_valid and
     * live in the scanctx this rescan already reset above, so they are cleared in
     * the same breath -- leaving the pointer set would leave a dangling read
     * reachable the moment the next gettuple republished the scan. */
    so->qtoks      = NULL;
    so->nqtoks     = -1;         /* not analyzed yet; see the field comment in bm25.h */

    if (keys && nkeys > 0)
        memmove(scan->keyData, keys, nkeys * sizeof(ScanKeyData));
    if (orderbys && norderbys > 0)
        memmove(scan->orderByData, orderbys, norderbys * sizeof(ScanKeyData));

    /* No order value has been produced for this scan yet. beginscan sets this
     * once, which is not enough: a PRIOR scoring rescan of this same descriptor
     * leaves xs_orderbynulls[0] == false next to a stale xs_orderbyvals[0]. */
    if (scan->numberOfOrderBys > 0 && scan->xs_orderbynulls != NULL)
        memset(scan->xs_orderbynulls, true,
               sizeof(bool) * scan->numberOfOrderBys);

    /*
     * ARITY. At most one ORDER BY key: two rank keys have no defined combined
     * order, so a second one cannot be honored and is refused rather than
     * dropped. WHERE keys have no such limit (#290): every one is applied below.
     */
    if (norderbys > 1)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: an index scan supports only one bm25 ORDER BY key"),
                 errdetail("This scan was given %d ORDER BY bm25 keys; ranking by "
                           "several queries has no defined combined order.",
                           norderbys),
                 errhint("Rank by one query, combining the conditions into it with "
                         "bm25_boolean(should => ARRAY[...]).")));

    /*
     * SK_ISNULL. A runtime key that evaluates to NULL arrives as
     * sk_argument == (Datum) 0 with SK_ISNULL raised, and index_rescan is called
     * anyway -- testing the flag is the AM's job, and every core AM does it
     * (nbtree qual_ok=false, hash returns false, gin isVoidRes). Dereferencing
     * (Datum) 0 in DatumGetTextPP/DatumGetJsonbP below takes SIGSEGV and restarts
     * the whole cluster. A literal `@@@ NULL` never gets here because bm25_match
     * is STRICT and const-folds away; the runtime-key path is not folded.
     *
     * WHERE and ORDER BY keys are NOT symmetric, so they short-circuit differently:
     *
     *  - A NULL @@@ key, ANY of them: bm25_match is STRICT, so no row can satisfy
     *    it, and the WHERE clause is their AND. Empty result, exactly as nbtree
     *    treats `x = NULL`.
     *
     *  - NULL &@@ order-by key: the matching rows still qualify -- the @@@ keys are
     *    what select them, and amoptionalkey=false guarantees one is present --
     *    only their ordering value is unknown, and bm25_distance being STRICT
     *    makes the executor's resjunk NULL for every row anyway. So run the
     *    ordinary unordered membership scan and let them sort to the end. This
     *    is what GiST does with a NULL KNN key (assume the distance is null,
     *    sort last); returning nothing instead would be a silent wrong answer.
     */
    scoring_key_isnull = (norderbys > 0 &&
                          (scan->orderByData[0].sk_flags & SK_ISNULL) != 0);
    for (i = 0; i < nkeys; i++)
        if ((scan->keyData[i].sk_flags & SK_ISNULL) != 0)
            where_key_isnull = true;

    if (where_key_isnull || (scoring_key_isnull && nkeys == 0))
    {
        /* Nothing can match. Latch the lazy loaders shut; the counters reset
         * above (nranked/npost == 0) then make both gettuple paths return false. */
        so->scoring = (norderbys > 0 && !scoring_key_isnull);
        so->loaded  = true;
        return;
    }

    /*
     * THE SCAN'S OWN QUERY. With an ORDER BY key, that key: it is what ranks, and
     * so the scan is scoring. Without one, the first WHERE key, answered by the
     * unordered @@@ membership path.
     */
    so->scoring = (norderbys > 0 && !scoring_key_isnull);
    primary = so->scoring ? &scan->orderByData[0]
        : (nkeys > 0 ? &scan->keyData[0] : NULL);
    if (primary != NULL)
        bm25_rescan_parse_key(scan, so, primary, so->scoring);

    /*
     * THE WHERE KEYS (#290). The result is what SQL says it is, whatever the plan:
     * the rows satisfying EVERY @@@ key, ordered by the &@@ distance when there is
     * an ORDER BY key. bm25_gettuple sets xs_recheck = false and the planner has
     * removed these clauses from the qual list, so a key not applied here is
     * applied by nothing. (Recheck is not the way out: it would defer to
     * bm25_match, which re-tokenizes with the english DEFAULT analyzer and drops
     * correct matches on any other -- see the note in bm25_gettuple.)
     *
     * It used to be otherwise. The ORDER BY query was THE query and the WHERE key
     * was never read, so `WHERE body @@@ 'cat' ORDER BY body &@@ 'dog'` returned
     * the 'dog' rows, and a second WHERE key was refused outright. A ranked query
     * that wants its ranking scoped to one field now says so in the WHERE as well
     * (`WHERE title @@@ 'body:"a b"' ORDER BY title &@@ 'body:"a b"'`); with the
     * scope in the ORDER BY alone, the unscoped WHERE rows the scoped query does
     * not match follow the ranked ones at distance +Infinity.
     *
     * A key whose RHS is byte-identical to the scan's own query needs no set of its
     * own -- its set IS that query's set -- so it is not parsed again, and the common
     * `WHERE col @@@ q ORDER BY col &@@ q` keeps exactly its old plan and cost
     * (WAND included). Its CONDITION still applies: when other keys send the scan to
     * the filtered build, where_has_orderby makes that build intersect the ORDER BY
     * query's set into the WHERE set, so `WHERE @@@ 'dog' AND @@@ 'cat' ORDER BY &@@
     * 'dog'` is the dog-and-cat rows, with no unmatched tail (an unscored scan's own
     * query is the first WHERE key and is always intersected). Every other key is
     * parsed NOW, into its own opaque, so an
     * invalid RHS (an unknown field, a malformed phrase, a must_not phrase leaf)
     * raises here exactly as it would as the scan's own query. The sets are
     * computed later, by the ranking build, under one snapshot (bm25_scan_rank.c).
     *
     * sk_attno is ignored, as it is for the scan's own query: a bare RHS searches
     * every field of the index, so several keys are an AND of all-field matches.
     */
    for (i = 0; i < nkeys; i++)
    {
        ScanKey         key = &scan->keyData[i];
        BM25ScanOpaque  w;

        if (key == primary)
            continue;
        if (bm25_scankeys_same_rhs(key, primary))
        {
            if (so->scoring)
                so->where_has_orderby = true;
            continue;
        }
        if (so->where == NULL)
            so->where = MemoryContextAlloc(so->scanctx,
                                           sizeof(BM25ScanOpaque) * nkeys);
        w = bm25_where_opaque(so);
        bm25_rescan_parse_key(scan, w, key, false);
        so->where[so->nwhere++] = w;
    }
}

/*
 * Compute the @@@ filter's matching TID set into so->postings (in scanctx).
 * Called at most once per rescan via the so->loaded guard.
 *
 * Two shapes, and which one a query takes is the whole correctness story here:
 *
 *   (a) DELEGATED to the exhaustive scorer -- a multi-leaf jsonb tree, OR a text
 *       phrase / proximity query (so->qphrase). Both need a per-document test the
 *       flat OR-union cannot make: the boolean formula for a tree, the positional
 *       recheck for a phrase. See the delegation block below.
 *
 *   (b) FLAT OR-UNION -- a bare term / field:term (and a single MATCH/TERM jsonb
 *       leaf, which bm25_rescan_parse_jsonb already made text-equivalent).
 *       Tokenize the query, union the postings for ALL tokens (OR semantics), and
 *       store the deduplicated result. OR semantics match bm25_scan_build_ranking
 *       (scored path) and bm25_match (recheck). Deduplication: gather all
 *       BM25Posting records, sort by TID via ItemPointerCompare, then compact --
 *       only the first occurrence of each TID is kept (only .tid is used in the
 *       non-scoring emit; tf/doclen are irrelevant).
 *
 * (b) requests NO positions anywhere: bm25_seg_dict_lookup is called with NULL
 * pos_root/pos_off, bm25_seg_scan_postings with an entirely Invalid/NULL position
 * cursor, and the pending walker strides past te->pos_bytes without decoding it.
 * That is why a phrase cannot be rechecked in place here and takes (a) instead --
 * doing it here would mean standing up a second position pipeline alongside the
 * one the scorer already owns.
 */
static void
bm25_load_if_needed(IndexScanDesc scan)
{
    Relation           index = scan->indexRelation;
    BM25ScanOpaque     so = (BM25ScanOpaque) scan->opaque;
    BM25ScanSnapshot   snap;
    BM25AnalyzerConfig qcfg;
    BM25Token         *toks;
    int              ntok,
                     qi;
    uint32           si;
    TidCollector     col;   /* scratch union array (scratchctx, below) */
    BM25MatchBudget  colbudget;  /* #62.5: this path's own running total */
    uint32           ntmp,
                     pi;
    MemoryContext    scratchctx,
                     oldctx;

    if (so->loaded)
        return;
    so->loaded = true;
    so->npost  = 0;
    so->cur    = 0;

    /* DELEGATION (M6 D12, #132). Two query shapes cannot be answered by the flat-OR
     * union below, and both reuse the SAME exhaustive membership computation the
     * scored path runs. Its surviving-TID set IS the filter set, which is what makes
     * @@@ == &@@ TRUE BY CONSTRUCTION for both:
     *
     *   (1) A multi-leaf jsonb tree (BOOLEAN/BOOST/PHRASE/WILDCARD) leaves so->qterm
     *       NULL -- there is no single term to OR-union, and a flat OR cannot honor
     *       per-leaf field scope / boost / must_not exclusion (it would WRONGLY
     *       INCLUDE the very docs a must_not clause removes). The scorer runs the
     *       per-leaf presence mask + bm25_query_eval instead.
     *
     *   (2) A TEXT phrase / proximity query (so->qphrase). #132: this disjunct was
     *       missing, so `col @@@ '"a b"'` with no ORDER BY fell through to the flat-OR
     *       union and silently returned the OR of the phrase's tokens -- no error, no
     *       recheck. so->qterm is ALREADY the inside-quotes text (bm25_rescan_parse_phrase
     *       rewrote it in place), which is exactly what the scorer tokenizes into the
     *       phrase's ordered term sequence; the positional recheck reads so->qfield /
     *       so->qphrase_ordered / so->qslop straight off this same opaque. Pending
     *       (unsealed) docs come along for free: the scorer's pending_phrase_stash
     *       supplies their true positions with the pending-wins dedup already applied.
     *
     * The two disjuncts are MUTUALLY EXCLUSIVE by construction: so->qphrase is set only
     * by the text micro-parser (bm25_rescan runs it only when the RHS was not jsonb) and
     * so->qtree only by bm25_rescan_parse_jsonb.
     *
     * so->scoring is false on this path, so the WAND gate is already bypassed (D10) --
     * the build goes straight to the exhaustive scorer, which fills so->ranked/so->nranked
     * (scores unused here). Copy its TIDs into so->postings, the array the non-scoring
     * emit (bm25_gettuple) iterates.
     *
     * Residual (#314 SCAN-11, left deliberately; unmeasured). This membership path pays
     * for the whole ranked build -- key discovery, the keymap fill, the score sort --
     * and the copy below keeps only the TIDs, so the key projection and the score
     * ordering are discarded work. #290 widened its reach to any scan with several
     * WHERE keys. It is a constant factor over walks the ranking does anyway, not a
     * defect, and nobody has measured it; a membership-only build would have to keep
     * whichever retry wrapper bm25_scan_build_ranking uses (#307) identical.
     *
     * Two intended consequences of routing a phrase through here rather than the
     * flat-OR path: rows are emitted score-descending instead of TID-ascending (the
     * copy below preserves the ranked order), and the phrase now spends the exhaustive
     * scorer's match-memory budget instead of the flat-OR per-TID one -- so @@@ and &@@
     * on the same literal spend the same budget and fail identically, still naming
     * bm25_native.max_match_memory. The D7 position-degradation gate likewise now
     * applies here: a phrase on a positions-off index ERRORs (or WARNs and falls back to
     * AND-of-terms under phrase_fallback='and') exactly as the &@@ form already did.
     *
     * A single MATCH/TERM jsonb leaf was copied into so->qterm (bm25_qtree_is_multileaf
     * false), and a non-quoted text RHS never sets qphrase; both fall through to the
     * byte-identical flat-OR path below.
     *
     * (3) #290: SEVERAL WHERE keys (so->nwhere > 0). The answer is the intersection of
     *     their sets, and the ranking build's filtered branch computes every one of
     *     them -- this scan's own query and each so->where entry -- through the same
     *     exhaustive membership computation, under one snapshot, and leaves the
     *     intersection in so->ranked in TID order. Whatever shape each key has, it is
     *     decided by the code that decides it for a single key. */
    if (so->nwhere > 0 || so->qphrase ||
        (so->qtree != NULL && bm25_qtree_is_multileaf(so->qtree)))
    {
        uint32 i;

        bm25_scan_build_ranking(index, so, so->qterm, so->qtermlen,
                                /* force_exhaustive = */ false);

        oldctx = MemoryContextSwitchTo(so->scanctx);
        so->postings = palloc(sizeof(BM25Posting) * Max(so->nranked, 1));
        for (i = 0; i < so->nranked; i++)
        {
            CHECK_FOR_INTERRUPTS();

            so->postings[i].tid    = so->ranked[i].tid;
            so->postings[i].tf     = 0;   /* unused; only .tid is read on this path */
            so->postings[i].doclen = 0;
        }
        so->npost = so->nranked;
        MemoryContextSwitchTo(oldctx);
        return;
    }

    if (!so->qterm)
        return;

    /* H17: everything from here to the dedupe is per-call scratch, and none of it was
     * being reclaimed. bm25_scan_snapshot palloc's a copy of the whole live catalog and
     * its contract (bm25.h) puts freeing it on the caller; bm25_analyze palloc's the
     * lowercased copy, the token array and a copy per lexeme. Only col.arr was pfree'd.
     * On this path CurrentMemoryContext is estate->es_query_cxt, reclaimed only at end
     * of QUERY, so a parameterized nested loop with a bm25 @@@ scan on the inner side
     * leaked all of it once per outer row: measured growing monotonically to 1.85 GB
     * over 120 s on a 4M-row outer scan, with no plateau, and terminated rather than
     * completed. A scratch context deleted at every exit bounds it to one call's worth.
     *
     * Deliberately created AFTER the early returns above, which allocate nothing, and
     * the deduplicated result is copied into scanctx before the delete. */
    scratchctx = AllocSetContextCreate(CurrentMemoryContext,
                                       "bm25 @@@ filter scratch",
                                       ALLOCSET_SMALL_SIZES);
    oldctx = MemoryContextSwitchTo(scratchctx);

    /* Atomic scan-start snapshot (D-SNAP/C3): ONE metapage SHARE lock captures
     * pending_head and the WHOLE live catalog together (bm25_scan_snapshot). The
     * @@@ boolean filter must union ALL segments + pending -- the same multi-source
     * treatment as the scored path -- or a doc living only in a later segment is
     * dropped from the match set. Only distinct TIDs matter here (the union is
     * deduped at the end), so per-source dedupe during collection is unnecessary:
     * a TID appearing in both pending and a stale segment posting collapses in the
     * sort-and-compact. Live-docs tombstone gating still applies per segment
     * (seg_tid_cb). There is no heap recheck of @@@ (xs_recheck stays false, see
     * bm25_gettuple); the executor checks only heap visibility. */
    bm25_scan_snapshot(index, &snap);

    /* section I scan-start gate (boolean @@@ path): a mismatched analyzer would silently
     * mis-filter, so gate here too, before any token collection. qcfg is reused to
     * tokenize the query term against the analyzer it was just gated against. */
    bm25_analyzer_config(index, &qcfg);
    bm25_fingerprint_gate(index, bm25_analyzer_fingerprint(&qcfg),
                          snap.analyzer_fingerprint, BM25_GATE_SCAN);

    /* Same analyzer as the index/build/insert paths: stem the query term so it
     * matches the STEMMED postings the segment stored. */
    ntok = bm25_analyze(&qcfg, so->qterm, so->qtermlen, &toks);
    if (ntok == 0)
    {
        MemoryContextSwitchTo(oldctx);
        MemoryContextDelete(scratchctx);
        return;
    }

    /*
     * Collect TIDs for every query token into a scratch array.  This is scratchctx,
     * so it goes away with the snapshot and the tokens once the deduplicated result
     * has been copied into scanctx below.
     * col.rdr is opened per-segment inside the loop (see below).
     */
    /* Zeroed, not opened: no segment yet. Same reasoning as the phrase stash's own
     * MemSet -- "no segment" spelled as a value the read path recognizes. */
    MemSet(&col.rdr, 0, sizeof(col.rdr));
    col.cap    = 64;
    col.n      = 0;
    col.qfield = so->qfield;    /* C4: field scope threaded into seg_tid_cb */
    bm25_match_budget_init(&colbudget);         /* #62.5 */
    col.budget = &colbudget;
    col.arr    = palloc(sizeof(BM25Posting) * col.cap);

    for (qi = 0; qi < ntok; qi++)
    {
        CHECK_FOR_INTERRUPTS();

        /* (a) sealed segments: collect this term's live docs' TIDs from EVERY
         * segment in the snapshot, not just segment 0. */
        for (si = 0; si < snap.nsegs; si++)
        {
            BM25SegmentHeader   h;
            BlockNumber         post_root;
            uint16              post_off;
            uint32              seg_df;

            CHECK_FOR_INTERRUPTS();

            bm25_seg_header_read(index, snap.segs[si].header_blkno,
                                 snap.segs[si].gen, &h);
            if (bm25_seg_dict_lookup(index, &h, toks[qi].ptr, toks[qi].len,
                                     &post_root, &post_off, &seg_df, NULL, NULL))
            {
                /* RE-OPENED here, not carried: col outlives one segment, so a reader
                 * kept from the previous one would name a header that &h has since been
                 * refilled over. (SEGREAD-14's root test keeps such a cursor from
                 * producing a WRONG answer, but nothing would make it point at a live
                 * header, and nothing would make it fast.) &h aliases this iteration's
                 * stack header, which is safe because seg_tid_cb reads through col.rdr
                 * only synchronously inside the scan below, while h is in scope.
                 *
                 * Liveness (issue #246): checked once per (term, segment) against the
                 * LIVEDOCS bitmap, gated on seg_df, instead of one LIVEDOCS read per
                 * posting. ADR 0100's argument covers this collector: the check runs
                 * at the first gettuple, after the scan's MVCC snapshot, and the TID
                 * set is built once and returned later without re-reading a bit --
                 * the per-lookup read was already a sample taken before the return.
                 * A tombstone landing after the check therefore leaves the same dead
                 * tuple the heap visibility check drops, and a line pointer reused
                 * after that tombstone holds a tuple the snapshot cannot see. */
                (void) bm25_seg_reader_init_checked(&col.rdr, index, &h, seg_df);
                /* Issue #267: a DOCMAP page image for the per-posting TID lookups, in
                 * scratchctx, released when this (term, segment) is done so one image
                 * is live at a time. */
                bm25_seg_reader_cache_pages(&col.rdr, scratchctx);
                /* seg_df bounds the decode to this term's run within the shared,
                 * undelimited segment-wide postings chain (D-POST). */
                bm25_seg_scan_postings(index, post_root, post_off, seg_df, h.gen,
                                       seg_tid_cb, &col,
                                       InvalidBlockNumber, 0, NULL, NULL, NULL, 0);
                bm25_seg_reader_release_pages(&col.rdr);
            }
        }

        /* (b) pending list: collect this term's live docs' TIDs. */
        {
            BlockNumber     blk = snap.pending_head;
            PGAlignedBlock  copy;
            BM25PendingWalk w;

            /* #291: the validated walk, bounded by the snapshot's next_gen. On this
             * @@@ path a recycled page's 40001 reaches the user, by decision: no retry
             * (XCUT-04, closed by #307/D22) -- the primary's horizon rules the reuse
             * out, and on a standby the ranked paths now report it the same way. */
            bm25_pending_walk_init(&w, index, snap.next_gen);
            while (blk != InvalidBlockNumber)
            {
                Buffer          buf;
                Page            pg;
                BM25PendingIter it;
                BlockNumber     next;

                CHECK_FOR_INTERRUPTS();
                /* pinned and SHARE-locked; extent, cycle, epoch and kind checked */
                buf = bm25_pending_walk_read(&w, blk);

                /* Fix (2026-08, crash/replica-safety pass): copy-then-unlock, same
                 * shape as pending_phrase_stash/pending_and_stash (bm25_scan_match.c) and
                 * bm25_pending_score_term (bm25_stats.c). tid_collector_add repallocs col.arr when it grows past
                 * col.cap -- unbounded by this one page's size -- and used to run
                 * while the page was still locked. */
                memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
                UnlockReleaseBuffer(buf);

                pg = (Page) copy.data;
                bm25_pending_iter_begin(&it, pg);
                while (bm25_pending_iter_next(&it))
                {
                    BM25PendingDocHeader *dh = it.cur;
                    char   *p = (char *) dh + bm25_pending_doc_entries_off(dh);
                    uint32  k;

                    if (!ItemPointerIsValid(&dh->tid))
                        continue;
                    for (k = 0; k < dh->ndocterms; k++)
                    {
                        BM25PendingTermEntry *te = (BM25PendingTermEntry *) p;
                        /* C4: honor the field scope for pending docs too -- pending
                         * entries carry field_id, so a scoped @@@ is exact even on
                         * unsealed rows, not just post-seal. */
                        if ((so->qfield == BM25_FIELD_ALL ||
                             (int32) te->field_id == so->qfield) &&
                            te->termlen == toks[qi].len &&
                            memcmp(p + sizeof(BM25PendingTermEntry),
                                   toks[qi].ptr, toks[qi].len) == 0)
                        {
                            tid_collector_add(&col, &dh->tid);
                            break;
                        }
                        p += MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
                    }
                }
                bm25_pending_iter_end(&it);
                next = BM25PageGetOpaque(pg)->nextblk;   /* read off the unlocked copy */
                blk = next;

                /* Between two pages of the captured chain, holding no buffer: the
                 * point where t/025 parks a standby scan while the primary drains,
                 * recycles and reuses the pages it has not read yet (#291). */
                if (blk != InvalidBlockNumber)
                    bm25_debug_pause_point("scan_pending_page");
            }
        }
    }

    ntmp = col.n;
    if (ntmp == 0)
    {
        MemoryContextSwitchTo(oldctx);
        MemoryContextDelete(scratchctx);
        return;
    }

    /*
     * Sort by TID so duplicates (a doc matching multiple query tokens) are
     * adjacent; then compact in-place keeping the first occurrence.
     */
    qsort(col.arr, ntmp, sizeof(BM25Posting), posting_tid_cmp);

    /* Compact: skip any TID already written as the previous output entry. */
    pi = 0;     /* write cursor into the deduplicated output */
    for (qi = 0; qi < (int) ntmp; qi++)
    {
        if (pi == 0 ||
            ItemPointerCompare(&col.arr[qi].tid, &col.arr[pi - 1].tid) != 0)
            col.arr[pi++] = col.arr[qi];
    }
    ntmp = (uint32) pi;

    /* Copy deduplicated result into scanctx for lifetime across gettuple. Must happen
     * BEFORE the scratch delete below -- col.arr lives in scratchctx. */
    MemoryContextSwitchTo(so->scanctx);
    so->postings = palloc(sizeof(BM25Posting) * ntmp);
    memcpy(so->postings, col.arr, sizeof(BM25Posting) * ntmp);
    so->npost = ntmp;

    MemoryContextSwitchTo(oldctx);
    MemoryContextDelete(scratchctx);
}

/*
 * bm25_tail_resume_pos -- the over-pull tail's resume point (#268): the index of
 * the first entry of the rebuilt ranking that sorts STRICTLY after (last_score,
 * last_tid) in scored_desc order (score descending, TID ascending), or nranked if
 * none does. The ranking is sorted by exactly that comparator, so a binary search
 * finds it. Resuming by key rather than by the last row's position is what keeps the
 * tail exact when that row is no longer ranked; see the invariant at the caller.
 */
static uint32
bm25_tail_resume_pos(BM25ScanOpaque so, double last_score, ItemPointer last_tid)
{
    uint32 lo = 0;
    uint32 hi = so->nranked;

    while (lo < hi)
    {
        uint32 mid = lo + (hi - lo) / 2;
        double s = so->scores[mid];
        bool   at_or_before;

        at_or_before = (s > last_score) ||
            (s == last_score && ItemPointerCompare(&so->ranked[mid].tid, last_tid) <= 0);
        if (at_or_before)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

#ifdef USE_ASSERT_CHECKING
/* One emitted row of the capped ranking, for bm25_tail_check_emitted. `tid` must
 * be first: dynahash compares the leading keysize bytes. */
typedef struct BM25TailEmitted
{
    ItemPointerData tid;
    double          score;
} BM25TailEmitted;

/*
 * bm25_tail_check_emitted -- cassert-only check of the over-pull tail invariant
 * (#268), both of its halves:
 *   - SCORE: every row the capped ranking emitted that the rebuilt ranking still
 *     holds carries a bit-identical score there;
 *   - MEMBERSHIP (#289): every row the rebuilt ranking places at or before the
 *     resume key (the last emitted (score, TID)) was emitted. The tail resumes
 *     strictly after that key, so such a row is otherwise never returned at all:
 *     a capped build that dropped a row it should have ranked turns into a row
 *     missing from an unlimited result. The score half alone could not see this,
 *     since it only compares rows present in both builds.
 * Hash the `nemitted` emitted rows (bounded by wand_top_k), then one pass over the
 * rebuilt array: O(nranked + k).
 *
 * Either kind of mismatch is a violation only when the scan's snapshot can see the
 * row. For membership: a row the snapshot can see was committed before it, so its
 * index entry was already there for the capped build to rank, under the same pinned
 * statistics and so the same score. For score: a TID
 * names a document only while its heap line pointer lives: an emitted row the
 * executor discarded as dead can be vacuumed between the builds and its line pointer
 * reused by a new row, which then appears in the rebuilt ranking under the same TID
 * with its own, legitimately different, score. A row the snapshot can see cannot
 * have been vacuumed, so its TID still names the document that was emitted. The heap
 * probe runs only on a mismatch.
 */
static void
bm25_tail_check_emitted(IndexScanDesc scan, const BM25Posting *old_ranked,
                        const double *old_scores, uint32 nemitted)
{
    BM25ScanOpaque   so = (BM25ScanOpaque) scan->opaque;
    HASHCTL          ctl;
    HTAB            *emitted;
    uint32           resume;
    uint32           i;

    if (nemitted == 0)
        return;
    /* The same position the caller resumes at, from the same key. */
    resume = bm25_tail_resume_pos(so, old_scores[nemitted - 1],
                                  (ItemPointer) &old_ranked[nemitted - 1].tid);

    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(ItemPointerData);
    ctl.entrysize = sizeof(BM25TailEmitted);
    ctl.hcxt      = CurrentMemoryContext;
    emitted = hash_create("bm25 tail emitted rows", (long) nemitted, &ctl,
                          HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    for (i = 0; i < nemitted; i++)
    {
        BM25TailEmitted *e;

        e = (BM25TailEmitted *) hash_search(emitted, &old_ranked[i].tid, HASH_ENTER, NULL);
        e->score = old_scores[i];
    }
    for (i = 0; i < so->nranked; i++)
    {
        BM25TailEmitted *e;
        bool             all_dead;

        e = (BM25TailEmitted *) hash_search(emitted, &so->ranked[i].tid, HASH_FIND, NULL);
        if (e == NULL ? i >= resume : e->score == so->scores[i])
            continue;
        Assert(!table_index_fetch_tuple_check(scan->heapRelation, &so->ranked[i].tid,
                                              scan->xs_snapshot, &all_dead));   /* invariant */
    }
    hash_destroy(emitted);
}
#endif                          /* USE_ASSERT_CHECKING */

bool
bm25_gettuple(IndexScanDesc scan, ScanDirection dir)
{
    BM25ScanOpaque so = (BM25ScanOpaque) scan->opaque;

    if (so->scoring)
    {
        /* On the first call, build the full descending-score ranking. */
        if (!so->loaded)
        {
            bm25_scan_build_ranking(scan->indexRelation, so,
                                    so->qterm, so->qtermlen,
                                    /* force_exhaustive= */ false);
            so->loaded = true;
            /* M4 snippet (D11): cache the resolved analyzer config on the scan so
             * bm25_snippet can re-analyze the projected field VALUE with the SAME
             * config the scorer used for the query terms. qcfg is a pure function of
             * the index reloptions (already resolved + fingerprint-gated inside the
             * ranking build); re-resolving it here is cheap and keeps it out of the
             * retry subtransaction that owns the ranking build's own qcfg. */
            bm25_analyzer_config(scan->indexRelation, &so->qcfg);
            /* TEXT-12 (issue #153): bm25_snippet's query hit set is built once per
             * scan, not once per projected row, and lazily: nqtoks = -1 here and
             * bm25_snippet builds it on its first call (from qterm, or from qtree's
             * non-negated leaves on the jsonb path, #308). It goes in scanctx (where
             * qterm, qtree and qcfg already live) so it lasts exactly as long as the
             * parse that produced it -- bm25_rescan's MemoryContextReset frees it and
             * clears the two fields next to qcfg_valid. The query is frozen from this
             * point until that rescan, so one build serves every row. */
            so->qtoks  = NULL;
            so->nqtoks = -1;
            so->qcfg_valid = true;
            /* Publish the full ranking so bm25_score(ctid) can resolve scores.
             * Registers so at the registry head; see bm25_score.c. #301: this
             * also records the current user id as the scan's owner, and this is
             * the point where that id is the one running the query: the executor
             * begins an index scan lazily, at the first fetch. */
            bm25_register_scored_scan(so, scan->indexRelation);
        }

        if (so->rcur >= so->nranked)
        {
            /* M2b Task 10: over-pull tail fallback. WAND filled only the top
             * wand_top_k rows (so->wand_capped); if the executor wants more --
             * a LIMIT beyond k, or no LIMIT at all (e.g. count(*)/array_agg
             * over an ordered subquery) -- lazily rebuild the FULL exhaustive
             * ranking and resume from here. Latched by wand_tail_done: the
             * rebuild is itself exhaustive, so it only ever needs to run once per
             * scan.
             *
             * The rebuild goes through bm25_scan_build_ranking (the retry
             * wrapper), NOT a direct call to bm25_scan_build_ranking_exhaustive:
             * this re-reads segment pages under a fresh snapshot exactly like the
             * scan's first build did, so it is just as exposed to the option-(d)
             * seg_gen reuse abort (Hot Standby + concurrent merge reusing pages
             * between the first build and this tail rebuild) -- going around the
             * wrapper would let that abort surface as a user-visible serialization
             * error instead of retrying silently. force_exhaustive=true is required
             * (not the plain wrapper): the dispatcher would otherwise see
             * wand_capped still true from the first build and re-run WAND, which
             * reproduces the same capped top-k and silently drops rows k+1..N.
             * so->qterm/so->qtermlen are exactly the text this scan's WAND build
             * ran on (bm25_rescan sets them once, before any gettuple call, and
             * nothing mutates them afterward), so the rebuild reproduces the
             * identical query.
             *
             * INVARIANT (#268): a rebuild may only EXTEND the emitted prefix; the
             * scores of rows already emitted are immutable. Two things make it hold.
             *
             *   1. The rebuild scores under the capped build's corpus statistics
             *      (so->stats_pin, consumed by the forced build), not the fresh
             *      snapshot's. Those statistics count every valid pending TID,
             *      tombstone and merge with no visibility check, so any INSERT --
             *      uncommitted, aborted, or this transaction's own -- or a VACUUM or
             *      merge between the two builds used to move avgdl/idf and re-score
             *      the tail on a different scale. Under the pin a document's score
             *      depends only on its own tf and doclen, so a document both builds
             *      saw scores bit-identically in both.
             *   2. The tail resumes by KEY, not by position: the first entry
             *      strictly after (last emitted score, last emitted TID) in
             *      scored_desc order. Every row the scan's snapshot can see that was
             *      not emitted sat after that key in the first build and, with its
             *      score unchanged, sits after it now; every emitted row sits at or
             *      before it. This is exact even when the last emitted row has since
             *      vanished, which the old resume-by-TID could not do (it fell back
             *      to the ordinal, one slot off).
             *
             * What the fresh snapshot still contributes is membership: it may hold
             * rows the scan's snapshot cannot see (another transaction's insert, this
             * transaction's later ones), placed by their pinned-scale scores, and the
             * executor's visibility check drops them exactly as it drops any dead
             * index entry. A cassert build checks the invariant below. */
            /* #290: a scan with WHERE keys of its own (so->nwhere > 0) is built by the
             * filtered builder, which is always exhaustive and never caps, so it never
             * reaches this rebuild -- and the rebuild could not serve it: it would
             * replace the filtered ranking with the bare ORDER BY one. */
            Assert(so->nwhere == 0 || !so->wand_capped);   /* invariant */
            if (so->wand_capped && !so->wand_tail_done)
            {
                uint32          emitted = so->rcur;
                bool            have_last = (emitted > 0 && emitted <= so->nranked);
                ItemPointerData last_tid;
                double          last_score = 0.0;
#ifdef USE_ASSERT_CHECKING
                /* The capped arrays stay allocated in scanctx after the rebuild
                 * reassigns the pointers, so the check below can still read them. */
                const BM25Posting *old_ranked = so->ranked;
                const double      *old_scores = so->scores;
#endif

                ItemPointerSetInvalid(&last_tid);
                if (have_last)
                {
                    last_tid   = so->ranked[emitted - 1].tid;
                    last_score = so->scores[emitted - 1];
                }

                /* SCAN-03/TEXT-02: the rebuild below replaces ranked, scores and
                 * ranked_keys with freshly palloc'd arrays sized to a NEW survivor
                 * count, and sets nranked to it. cur_ranked_idx still points into
                 * the OLD arrays -- at emitted - 1, i.e. the old capped nranked - 1.
                 * If a concurrent DELETE+VACUUM shrank the match set, the new
                 * nranked can be smaller than that, and this scan stays registered
                 * until bm25_endscan, so a later bm25_score(ctid)/bm25_score_key()
                 * in the same query (the resolvers walk EVERY registered scan, not
                 * just the one that owns the row) reads past the end of all three.
                 * Clear it here: there is no row being emitted during the rebuild,
                 * which is exactly what the sentinel means. The resolvers also
                 * bound-check it now, for any future site that rebuilds without
                 * coming through here. */
                so->cur_ranked_idx = BM25_NO_CUR;

                bm25_scan_build_ranking(scan->indexRelation, so,
                                       so->qterm, so->qtermlen,
                                       /* force_exhaustive= */ true);
                so->wand_tail_done = true;
                so->wand_capped    = false;
                so->rcur           = 0;     /* nothing emitted: start at the top */

#ifdef USE_ASSERT_CHECKING
                bm25_tail_check_emitted(scan, old_ranked, old_scores, emitted);
#endif

                if (have_last)
                {
                    so->rcur = bm25_tail_resume_pos(so, last_score, &last_tid);
                    /* Re-position on the entry just before the resume point. When
                     * the last emitted row is still ranked that entry IS it (its key
                     * is unchanged). If the rebuild has nothing after it -- a capped
                     * build that already held every match, e.g. exactly wand_top_k of
                     * them -- the scan ends below without emitting, and with the
                     * sentinel left in place it would read as never positioned: the
                     * resolvers that require a positioned scan (the &@@ distance
                     * projection and the query-qualified score accessors,
                     * bm25_scan_ranks_query) would skip it, so a projection above a
                     * Sort got NULL (#253). An uncapped scan that runs out keeps its
                     * last row current in the same way.
                     *
                     * When the last row has vanished (deleted and vacuumed between the
                     * builds) the entry before the resume point is an earlier row;
                     * positioning there keeps the scan positioned, which is true --
                     * it has emitted rows -- and the resolvers' row-identity checks
                     * (ranked[cur_ranked_idx].tid) keep it from answering for any row
                     * but that one. */
                    if (so->rcur > 0)
                        so->cur_ranked_idx = so->rcur - 1;
                }
            }
            if (so->rcur >= so->nranked)
                return false;
        }

        scan->xs_heaptid = so->ranked[so->rcur].tid;
        so->cur_ranked_idx = so->rcur;   /* R3: the row being emitted (owner-identity resolution) */
        bm25_scored_scan_emitted(so);    /* #242: see bm25_score.c */
        /* The ctid score accessor needs this heap to follow a HOT chain back to its
         * root; stash it here because the accessor is reached from a target-list
         * expression that has no IndexScanDesc. See bm25_score.c. */
        so->heaprel = scan->heapRelation;
        /* No recheck: the index match is authoritative (see the non-scoring path
         * below for the full rationale). A recheck would defer to bm25_match, which
         * tokenizes with the english DEFAULT analyzer and silently drops correct
         * matches on a non-english index. */
        scan->xs_recheck = false;
        scan->xs_recheckorderby = false; /* index supplies the order value */
        if (scan->numberOfOrderBys > 0)
        {
            /* Ascending "distance" = -score so higher relevance sorts first. A WHERE
             * row the ORDER BY query does not match (#290) has score -Infinity, so
             * its distance is +Infinity, the off-index value, and the scan stays
             * positioned on it for the Merge Append resolver (#252). */
            scan->xs_orderbyvals[0] = Float8GetDatum(-so->scores[so->rcur]);
            scan->xs_orderbynulls[0] = false;
            so->cur_orderby_dist = -so->scores[so->rcur];   /* stash for the &@@ per-row projection */
        }
        so->rcur++;
        return true;
    }

    /* Non-scoring: Task 6 term-match path (unordered @@@ matching). */
    bm25_load_if_needed(scan);

    if (so->cur >= so->npost)
        return false;

    scan->xs_heaptid = so->postings[so->cur].tid;
    so->cur++;

    /*
     * No recheck -- the index match is authoritative, which requires that this AM
     * apply the WHOLE @@@ predicate, not just term membership. amgetbitmap is NULL,
     * so bm25_load_if_needed above is the only way a TID reaches here and is
     * therefore the sole filter; it delegates any shape it cannot decide itself (a
     * text phrase / proximity query, a multi-leaf jsonb tree) to the exhaustive
     * scorer rather than approximating it. #132 is precisely what this claim looks
     * like when it is false: the phrase delegation was missing, the union of the
     * phrase's tokens was emitted, and xs_recheck = false meant nothing downstream
     * could catch it.
     *
     * Note what this does NOT say. It is a claim about the AM, and it is not on
     * its own a licence to disable recheck -- the recheck TARGET, bm25_match,
     * could not decide a phrase either (no positions, no index), so routing there
     * would have traded one wrong answer for another. That is why bm25_match now
     * REFUSES a phrase outright rather than approximating it (#132,
     * bm25_handler.c): both paths agree a phrase is index-only, one by answering
     * it and one by failing loud, and neither returns a plausible wrong set.
     *
     * Given that, the scan tokenized the query with the INDEX's analyzer and matched
     * postings built (at insert/build) from that same analyzer, so a returned TID
     * genuinely satisfies the query. The executor's heap-visibility (MVCC) check runs
     * independently of xs_recheck, so dead/aborted/invisible tuples are filtered
     * regardless. Stale postings cannot yield a LIVE false positive: VACUUM's
     * bm25_bulkdelete tombstones a dead TID's posting (bm25_livedocs_clear) BEFORE
     * its heap line pointer may be reused (the ambulkdelete contract), and the scan
     * skips tombstoned docs (bm25_seg_doc_is_live); a reused line pointer's new tuple
     * carries its own correct posting.
     *
     * Critically, xs_recheck=true would defer the recheck to bm25_match, which has
     * no index Relation and tokenizes with the english DEFAULT analyzer. On a
     * non-english index that re-tokenization diverges from the index's analyzer and
     * silently drops correctly-matched rows (false negatives) -- so the recheck must
     * stay OFF here.
     */
    scan->xs_recheck = false;

    return true;
}

void
bm25_endscan(IndexScanDesc scan)
{
    BM25ScanOpaque so = (BM25ScanOpaque) scan->opaque;

    /* Deregister before freeing so; order matters (a stale registry entry
     * would let a later projection dereference freed scanctx). */
    bm25_deregister_scored_scan(so);

    /* Deleting scanctx frees qterm, postings, ranked, scores in one shot. */
    MemoryContextDelete(so->scanctx);
}
