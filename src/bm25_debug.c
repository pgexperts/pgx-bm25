/* bm25_debug.c -- the SQL-callable debug/introspection surface: every
 * bm25_debug_* SRF that exposes scan internals to the regression suites, plus
 * the helpers only those SRFs use.
 *
 * Why this file exists (#69.5): these SRFs and their helpers were physically
 * interleaved with the access method's scan callbacks in bm25_scan.c, sitting
 * between the ranking dispatcher and beginscan/rescan/gettuple/endscan, so the
 * AM's lifecycle could not be read contiguously. Roughly a quarter of a
 * 5,210-line file was test-only code. Splitting is a pure code move: nothing
 * here changed behaviour, and the SQL-visible signatures are untouched.
 *
 * What lives here:
 *   - the PG_FUNCTION_INFO_V1 debug probes (bm25_debug_* plus bm25_wand_stats,
 *     which is a debug probe despite the name; not every one returns a set);
 *   - their private helpers: wand_debug_ctx_and_block, term_contrib_debug_cb,
 *     and the four bm25_query_render* AST printers;
 *   - bm25_global_stats, whose only caller is bm25_debug_global_stats.
 *
 * What deliberately did NOT move: the three scan-start helpers these probes call
 * (bm25_scan_corpus_stats, bm25_term_idf, bm25_field_corpus_stats). Those have
 * production callers too, and the probes are only useful as REFERENCES if they run
 * the exact code the real scan runs -- so they stayed in the scanner (bm25_scan.c
 * then, bm25_scan_rank.c since #228) and are reached through bm25_scan.h. A probe
 * that recomputed corpus stats its own way would silently stop witnessing anything,
 * which is precisely what sql/43_wand_parity depends on it not doing. (bm25_term_idf
 * has since moved down to bm25_stats.c with #67.13 and is reached through
 * bm25_stats.h. The probe still runs the same function the scorer and the WAND driver
 * run, so the argument holds unchanged.)
 *
 * bm25_fingerprint_gate was a fourth until #188, which gave it a write-path caller
 * and moved it to bm25_analyzer.c (where both of its operands already lived).
 * bm25_debug_fingerprint_gate still drives the REAL function -- the rule above did
 * not change, only the translation unit and the header: it now arrives via bm25.h.
 *
 * Privilege note: these functions are REVOKEd from PUBLIC in the extension
 * script (see sql/63_debug_privileges and sql/71_maintenance_privileges) -- they
 * take relation OIDs and walk index internals, so they are superuser-ish
 * surface, not user-facing API. Adding an SRF here means adding its REVOKE.
 */
#include "postgres.h"

#include "bm25.h"
#include "bm25_scan.h"          /* the two shared scan-start helpers */
                                /* bm25_fingerprint_gate arrives via bm25.h (#188) */
#include "bm25_stats.h"         /* bm25_term_idf, the idf every ranked path uses (#67.13) */
#include "bm25_wand.h"          /* M2b Task 4/5: BM25WandCtx/bm25_block_ub, BM25TopK, and
                                 * BM25_WAND_TOP_K_MAX for the validator below. It used to
                                 * arrive early through bm25_scan.h, which no longer
                                 * includes it (#67.13). */
#include "funcapi.h"            /* Materialize-mode SRF machinery */
#include "miscadmin.h"          /* CHECK_FOR_INTERRUPTS */
#include "utils/array.h"        /* M2b Task 5: bm25_debug_topk's float8[]/int[] args */
#include "utils/builtins.h"
#include "utils/uuid.h"         /* pg_uuid_t / UUID_LEN / UUIDPGetDatum (M5 key decode) */
#include "utils/jsonb.h"        /* M6 Task 2: the jsonb query-tree probes */
#include "utils/tuplestore.h"   /* tuplestore_begin_heap/putvalues -- no longer pulled
                                 * in transitively via funcapi.h as of PG19 */
#include "bm25_query.h"         /* M6 Task 2: BM25Query AST + bm25_query_parse/glob_match */

/* Bound a debug SRF's user-supplied k before it reaches bm25_topk_create (HDL-03).
 *
 * bm25_topk_create did `MemoryContextAlloc(cxt, sizeof(BM25Scored) * k)` behind an
 * Assert, which is compiled out in exactly the builds these probes ship in. All three
 * callers below take k straight from a SQL argument and rejected only k <= 0, so the
 * multiplication was unbounded above. What that actually costs depends on the platform,
 * and it is worth being exact rather than reaching for the scarier story: on LP64 the
 * product is computed in a 64-bit size_t and cannot overflow for any int k, so the
 * pre-fix behaviour was palloc's own anonymous "invalid memory alloc request size"
 * error, or a successful allocation of up to 1 GB. On ILP32 it can genuinely wrap and
 * turn a huge request into a small allocation the heap writes then overrun. The guard
 * is worth having on both -- an unbounded SQL argument reaching an allocation size is
 * the defect, whichever way it fails -- but only the second is a memory-safety event.
 *
 * QRY-10 has since made bm25_topk_create enforce the same ceiling itself, with real
 * ereports rather than an Assert, so this is no longer the ONLY thing between a SQL
 * argument and an allocation size. It stays because it names the probe in the error
 * and reads the same to a suite; the ceiling is now shared rather than restated --
 * BM25_WAND_TOP_K_MAX (bm25_wand.h), derived the same MaxAllocSize / sizeof(...) way
 * as the BM25_MAX_DOC_TOKENS idiom in the tokenizer's growth guard.
 *
 * Deliberately NOT a "sensible" top-k: bm25_native.wand_top_k is PGC_USERSET on the
 * work_mem bargain and carries the identical bound (HDL-04), and a debug probe should
 * not be more opinionated than the real knob. This is about making the arithmetic
 * total, not about policy.
 *
 * fname must be the CALLING SQL function's own name as declared in
 * bm25_native--1.0.sql, never a C routine's: it becomes the message prefix, and the
 * house errmsg rule (ARCHITECTURE.md) allows a function-name prefix only when it names
 * the SQL function the user actually called. A shared helper that could not be told
 * its caller would have to use the plain "bm25: " prefix instead.
 */
static void
bm25_debug_topk_k_validate(int k, const char *fname)
{
    int max_k = BM25_WAND_TOP_K_MAX;

    if (k <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: k must be positive", fname)));
    if (k > max_k)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("%s: k must not exceed %d", fname, max_k),
                 errdetail("A larger top-k heap than one allocation can hold.")));
}

/* bm25_debug_fingerprint_gate(index, claimed_fp) -- run the SAME scan-start gate
 * with a CALLER-SUPPLIED query fingerprint so the ERROR/WARNING behavior is
 * testable deterministically (no need to fabricate a second analyzer). Returns
 * true when claimed_fp matches the index's stored fingerprint; raises ERROR (or
 * WARNING then returns false) on mismatch, exactly as the scan-start gate. */
PG_FUNCTION_INFO_V1(bm25_debug_fingerprint_gate);
Datum
bm25_debug_fingerprint_gate(PG_FUNCTION_ARGS)
{
    Oid              relid = PG_GETARG_OID(0);
    uint32           claimed = (uint32) PG_GETARG_INT64(1);
    Relation         index = bm25_index_open_readable(relid, AccessShareLock);
    BM25ScanSnapshot snap;
    bool             matched;

    bm25_scan_snapshot(index, &snap);
    matched = (claimed == snap.analyzer_fingerprint);
    /* BM25_GATE_SCAN, not INGEST: this probe exists to pin the READ path's behavior,
     * and it writes nothing. The ingest wording is pinned behaviorally by
     * sql/102_ingest_fingerprint_gate instead, driving a real INSERT. */
    bm25_fingerprint_gate(index, claimed, snap.analyzer_fingerprint,
                          BM25_GATE_SCAN);   /* may ERROR/WARN */
    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(matched);
}

/*
 * bm25_debug_rank -- SRF: (tid tid, score float8) rows in descending score.
 *
 * Calls bm25_scan_build_ranking against a zero-initialized BM25ScanOpaqueData
 * so ranking can be tested without a real executor scan.  Uses the standard
 * Materialize-SRF guard identical to bm25_debug_terms.
 *
 * SQL: CREATE FUNCTION bm25_debug_rank(index regclass, query text,
 *        OUT tid tid, OUT score float8) RETURNS SETOF record ...
 */
PG_FUNCTION_INFO_V1(bm25_debug_rank);
Datum
bm25_debug_rank(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi    = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid  = PG_GETARG_OID(0);
    text               *query  = PG_GETARG_TEXT_PP(1);
    Relation            index;
    BM25ScanOpaqueData  so_buf;     /* stack-allocated; lives for this call */
    BM25ScanOpaque      so     = &so_buf;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
    MemoryContext       oldctx;
    uint32              i;

    /* Standard Materialize-mode SRF guard (same pattern as bm25_debug_terms). */
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    /* Tuplestore must be created in the per-query memory context so it
     * lives long enough for the caller to drain it. */
    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);

    /*
     * Zero-initialise the opaque.  Set scanctx to the SRF's per-query context:
     * ranked/scores are palloc'd there by bm25_scan_build_ranking and freed
     * when that context is reset -- no explicit pfree needed here.
     */
    MemSet(so, 0, sizeof(BM25ScanOpaqueData));
    so->scanctx = rsi->econtext->ecxt_per_query_memory;
    /* C4: this debug SRF bypasses bm25_rescan (which would set qfield), so the
     * MemSet leaves qfield == 0 (a valid field_id!). Force all-fields scope so
     * bm25_debug_rank scores BM25F across every field, matching its contract. */
    so->qfield = BM25_FIELD_ALL;

    /*
     * force_exhaustive=true is LOAD-BEARING, not belt-and-braces (#67.4): this
     * SRF is the uncapped reference sql/43_wand_parity diffs WAND against, so it
     * must never take the WAND branch itself. Passing false also happens to work
     * today, but only because the MemSet above leaves so->scoring false and the
     * dispatcher's D7 gate tests it -- an implicit coupling that silently turns
     * the parity suite into WAND-vs-WAND (green, proving nothing) the moment
     * anyone sets so->scoring here. Say it at the call site instead.
     */
    bm25_scan_build_ranking(index,
                            so,
                            VARDATA_ANY(query),
                            VARSIZE_ANY_EXHDR(query),
                            /* force_exhaustive= */ true);

    /* Emit (tid, score) rows in the already-sorted (descending) order. */
    for (i = 0; i < so->nranked; i++)
    {
        Datum  vals[2];
        bool   nulls[2] = {false, false};

        vals[0] = PointerGetDatum(&so->ranked[i].tid);
        vals[1] = Float8GetDatum(so->scores[i]);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/*
 * bm25_debug_wand_rank -- SRF: (tid tid, score float8) in descending score,
 * produced by the M2b Block-Max WAND driver (bm25_wand_build_ranking) with an
 * explicit k, into a throwaway BM25ScanOpaqueData. It does NOT touch the live
 * scan seam (that wiring landed separately, in the dispatcher
 * bm25_scan_build_ranking_once): its whole job is to let sql/43_wand_parity
 * diff WAND against the exhaustive bm25_debug_rank and prove they are bit-
 * identical (same tids, same order, same score bits) -- the milestone's core
 * exactness gate (D1/D8).
 *
 * The corpus-stats prologue mirrors bm25_scan_build_ranking_once's bag-of-words
 * path (WAND applies only to bag-of-words, D7); the phrase/AND machinery is
 * skipped (this path never sets qphrase). qfield is forced BM25_FIELD_ALL, so
 * this SRF scores BM25F across every field exactly like bm25_debug_rank -- the
 * two are therefore comparable (field:term parsing lives in bm25_rescan, off
 * this debug path). Development/regression only; not on the query path.
 *
 * SQL: CREATE FUNCTION bm25_debug_wand_rank(index regclass, query text, k int,
 *        OUT tid tid, OUT score float8) RETURNS SETOF record ...
 */
PG_FUNCTION_INFO_V1(bm25_debug_wand_rank);
Datum
bm25_debug_wand_rank(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi   = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    text               *query = PG_GETARG_TEXT_PP(1);
    int32               k     = PG_GETARG_INT32(2);
    Relation            index;
    BM25ScanOpaqueData  so_buf;         /* stack-allocated; lives for this call */
    BM25ScanOpaque      so    = &so_buf;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
    MemoryContext       oldctx;
    uint32              i;
    /* corpus-stats prologue (the shared one _once's bag-of-words path runs). */
    BM25ScanSnapshot    snap;
    BM25AnalyzerConfig  qcfg;
    BM25Token          *qtoks;
    int                 nq;
    uint64              live_ndocs;
    BM25FieldConfig     fcfg[BM25_MAX_FIELDS];
    double              avgdl_f[BM25_MAX_FIELDS];
    uint64              fld_ndocs[BM25_MAX_FIELDS];

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in context that cannot accept a set")));
    /* k is the heap's logical capacity straight from SQL: reject non-positive here
     * so the error names this probe (bm25_topk_create rejects it too, since QRY-10,
     * but with the generic message). k == 0 is "WAND disabled" on the real seam,
     * but a debug driver with no heap has nothing to return, so error. */
    bm25_debug_topk_k_validate(k, "bm25_debug_wand_rank");
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);

    MemSet(so, 0, sizeof(BM25ScanOpaqueData));
    so->scanctx = rsi->econtext->ecxt_per_query_memory;
    so->qfield  = BM25_FIELD_ALL;   /* BM25F across every field, like bm25_debug_rank */

    /* Shared prologue: one atomic snapshot + the scan-start fingerprint gate,
     * then the global/per-field corpus stats -- literally the same call _once
     * makes, so this SRF scores off the same values (see bm25_scan_corpus_stats).
     * No store_pos: WAND never reads positions. */
    bm25_scan_corpus_stats(index, &snap, &qcfg, NULL,
                           &live_ndocs, fcfg, avgdl_f, fld_ndocs);

    nq = bm25_analyze(&qcfg, VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query), &qtoks);

    bm25_wand_build_ranking(index, so, &snap, qtoks, nq, live_ndocs,
                            avgdl_f, fld_ndocs, fcfg, BM25_FIELD_ALL, k, NULL);

    /* Emit (tid, score) in the already-sorted (descending) ranked order. */
    for (i = 0; i < so->nranked; i++)
    {
        Datum  vals[2];
        bool   nulls[2] = {false, false};

        vals[0] = PointerGetDatum(&so->ranked[i].tid);
        vals[1] = Float8GetDatum(so->scores[i]);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/*
 * bm25_debug_rank_key -- like bm25_debug_rank, but emits the ranked row's decoded
 * key_field value instead of its ctid, proving the scorer stashed key[docid] beside
 * the TID. Columns: (key_int4, key_int8, key_uuid, key_text, score); the column that
 * matches ranked_key_type is populated, the rest NULL. A keyless index yields all-
 * NULL key columns (ctid-only). A pending-only ranked row (no KEYMAP entry yet) also
 * yields NULL key columns. Development/regression introspection only.
 *
 * SQL: CREATE FUNCTION bm25_debug_rank_key(index regclass, query text,
 *        OUT key_int4 int, OUT key_int8 bigint, OUT key_uuid uuid,
 *        OUT key_text text, OUT score float8) RETURNS SETOF record ...
 */
PG_FUNCTION_INFO_V1(bm25_debug_rank_key);
Datum
bm25_debug_rank_key(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi    = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid  = PG_GETARG_OID(0);
    text               *query  = PG_GETARG_TEXT_PP(1);
    Relation            index;
    BM25ScanOpaqueData  so_buf;
    BM25ScanOpaque      so     = &so_buf;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
    MemoryContext       oldctx;
    uint32              i;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);

    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);

    MemSet(so, 0, sizeof(BM25ScanOpaqueData));
    so->scanctx = rsi->econtext->ecxt_per_query_memory;
    so->qfield = BM25_FIELD_ALL;    /* bare RHS: BM25F across every field */

    /* force_exhaustive=true for the same reason as bm25_debug_rank (#67.4): the
     * uncapped reference must not be able to drift onto the WAND path. */
    bm25_scan_build_ranking(index, so,
                            VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query),
                            /* force_exhaustive= */ true);

    for (i = 0; i < so->nranked; i++)
    {
        Datum  vals[5];
        bool   nulls[5] = {true, true, true, true, false};
        /* ranked_key_present gates the slot for the same reason bm25_score_key does:
         * an unresolved slot is all-zero, and reporting that as the key would print
         * 0 for a row whose key this scan never resolved. Unresolved -> NULL column.
         * (A SQL NULL key_field still prints 0: it IS a zero key, by design.) */
        const unsigned char *kp = (so->ranked_keys != NULL &&
                                   so->ranked_key_present[i])
            ? so->ranked_keys + (Size) i * so->ranked_key_size : NULL;

        if (kp != NULL)
        {
            switch (so->ranked_key_type)
            {
                case BM25_KEY_INT4:
                    {
                        int32       v;

                        memcpy(&v, kp, sizeof(v));
                        vals[0] = Int32GetDatum(v);
                        nulls[0] = false;
                        break;
                    }
                case BM25_KEY_INT8:
                    {
                        int64       v;

                        memcpy(&v, kp, sizeof(v));
                        vals[1] = Int64GetDatum(v);
                        nulls[1] = false;
                        break;
                    }
                case BM25_KEY_UUID:
                    {
                        pg_uuid_t  *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));

                        memcpy(u->data, kp, UUID_LEN);
                        vals[2] = UUIDPGetDatum(u);
                        nulls[2] = false;
                        break;
                    }
                case BM25_KEY_TEXT:
                    {
                        int         len = 0;

                        while (len < so->ranked_key_size && kp[len] != '\0')
                            len++;
                        vals[3] = PointerGetDatum(cstring_to_text_with_len((const char *) kp, len));
                        nulls[3] = false;
                        break;
                    }
                default:
                    break;
            }
        }
        vals[4] = Float8GetDatum(so->scores[i]);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_field_stats(index) -- one (field_id, ndocs_field, total_len_field) row
 * per field, the SQL-visible witness for the BM25F corpus-stat accessor
 * (bm25_field_corpus_stats): per-field sumdoclen and N_field summed across the live
 * segments + pending, exactly as the scorer computes avgdl_field. Regression
 * introspection only; not on the query path. */
PG_FUNCTION_INFO_V1(bm25_debug_field_stats);
Datum
bm25_debug_field_stats(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi   = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
    MemoryContext       oldctx;
    BM25ScanSnapshot    snap;
    uint64              fld_len[BM25_MAX_FIELDS];
    uint64              fld_ndocs[BM25_MAX_FIELDS];
    uint32              f;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);

    /* Snapshot allocates snap.segs in CurrentMemoryContext; the per-query context
     * is current here and is reset after the SRF drains, so no explicit pfree. */
    bm25_scan_snapshot(index, &snap);
    bm25_field_corpus_stats(index, &snap, fld_len, fld_ndocs);

    for (f = 0; f < snap.field_count; f++)
    {
        Datum   vals[3];
        bool    nulls[3] = {false, false, false};

        vals[0] = Int32GetDatum((int32) f);
        vals[1] = Int64GetDatum((int64) fld_ndocs[f]);
        vals[2] = Int64GetDatum((int64) fld_len[f]);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_global_stats -- global corpus stats from the metapage cache.
 *
 * The sealer/merger/bulkdelete keep ndocs/total_len live and tombstone-adjusted,
 * so one metapage read gives the corpus-wide N and sumdoclen for a single global
 * avgdl and idf computation.
 *
 * static, and declared nowhere: bm25_debug_global_stats just below is the only
 * caller there has ever been. It was extern in bm25.h until the split (ADR 0053)
 * gave it a home where that is visible. */
static void
bm25_global_stats(Relation index, uint64 *ndocs, uint64 *total_len)
{
    BM25MetaPageData meta;

    bm25_meta_read(index, &meta);
    *ndocs = meta.ndocs;
    *total_len = meta.total_len;
}

/* bm25_debug_global_stats -- SQL-callable wrapper returning (ndocs, total_len)
 * as a single composite row (OUT parameters, not a SETOF SRF). */
PG_FUNCTION_INFO_V1(bm25_debug_global_stats);
Datum
bm25_debug_global_stats(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    Relation    index;
    uint64      ndocs,
                total_len;
    TupleDesc   tupdesc;
    Datum       vals[2];
    bool        nulls[2] = {false, false};
    HeapTuple   tuple;

    /* Resolve the result type before opening the index (matches the debug-SRF
     * idiom; nothing to release if the type check errors). */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_global_stats(index, &ndocs, &total_len);
    vals[0] = Int64GetDatum((int64) ndocs);
    vals[1] = Int64GetDatum((int64) total_len);
    tuple = heap_form_tuple(tupdesc, vals, nulls);
    index_close(index, AccessShareLock);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* -------------------------------------------------------------------------
 * M2b Task 4: WAND bound debug probes
 * -------------------------------------------------------------------------
 * bm25_debug_block_ub / bm25_debug_term_contrib share one setup helper so the
 * bound and the "true" per-posting contribution the safety test compares it
 * against are computed from IDENTICAL scan-time stats (idf/avgdl/k1/b/boost).
 * That identity is the whole point: comparing the bound to a value computed
 * from different stats would prove nothing (see the task's escalation note).
 *
 * Scope is deliberately narrow -- ONE term, ONE block (the first live segment
 * that carries the term, at its first block) -- because proving the bound
 * formula sound needs only one block, not a full multi-segment/pending WAND
 * walk. That walk is the production driver's job (bm25_wand_build_ranking,
 * reached from the seam dispatcher bm25_scan_build_ranking_once), not this
 * probe's. Pending-list postings are out of scope here
 * (a term's pending copies aren't organized into blocks/impact tables at
 * all -- there is nothing for bm25_block_ub to bound); both probes assume the
 * corpus under test has been sealed, exactly like sql/43_wand_parity.sql.
 */

/* Runs the SAME shared prologue and per-term stat computation as the scorer
 * (bm25_scan_corpus_stats + bm25_term_idf), for exactly one term and unscoped to
 * any field (qfield == BM25_FIELD_ALL) -- sharing them, rather than recomputing
 * alongside them, is what makes the bound comparison meaningful. Errors if the
 * term is absent from every live segment: all three debug functions built on
 * this require a hit to have anything to report. out_df (Task 6, nullable -- the
 * block_ub/term_contrib probes have no use for it) is that FIRST live segment's
 * df for the term, the same (post_root, post_off, df) triple
 * bm25_wand_cursor_open needs.
 */
static void
wand_debug_ctx_and_block(Relation index, const char *term, int termlen,
                         BM25WandCtx *ctx, BM25SegmentHeader *out_seg,
                         BlockNumber *out_blk, uint16 *out_off,
                         BM25BlockHeader *out_hdr, BM25BlockImpact *out_imp,
                         uint32 *out_df)
{
    BM25ScanSnapshot   snap;
    BM25AnalyzerConfig qcfg;
    BM25FieldConfig    fcfg[BM25_MAX_FIELDS];
    double             avgdl_f[BM25_MAX_FIELDS];
    double             idf_f[BM25_MAX_FIELDS];
    uint64             fld_ndocs[BM25_MAX_FIELDS];
    uint64             live_ndocs;
    uint32             si;
    bool               found_block = false;

    /* Shared scan-start prologue (bm25_scan_corpus_stats): snapshot, section I
     * fingerprint gate, then global and per-field corpus stats -- pending included
     * so a not-yet-sealed doc is never silently excluded from the stats the bound
     * is checked against. This probe used to omit the fingerprint gate; running
     * the shared prologue is what makes all five sites uniform. qcfg is gated and
     * then unused: the caller hands in an already-analyzed term, so there is no
     * query left to tokenize. No store_pos -- this path reads no positions. */
    bm25_scan_corpus_stats(index, &snap, &qcfg, NULL,
                           &live_ndocs, fcfg, avgdl_f, fld_ndocs);

    /* df + idf for this one term, summed across every live segment + pending
     * (idf is corpus-GLOBAL, never per-segment -- the task's escalation note).
     * Unscoped and unboosted, so bm25_term_idf's C4 zeroing and M6 boost are
     * both no-ops here. Its return is deliberately discarded: a term absent
     * everywhere leaves idf all-zero and is reported by the not-found ERROR
     * below, which is the contract all three probes are built on. */
    (void) bm25_term_idf(index, &snap, term, termlen,
                         live_ndocs, fld_ndocs, BM25_FIELD_ALL, 1.0, idf_f, NULL);

    bm25_wand_ctx_build(ctx, snap.field_count, idf_f, avgdl_f, fcfg, BM25_FIELD_ALL);

    /* First live segment carrying the term, its first block. */
    for (si = 0; si < snap.nsegs && !found_block; si++)
    {
        BlockNumber post_root;
        uint16      post_off;
        uint32      seg_df;
        BlockNumber next_blk;
        uint16      next_off;

        /* HDL-09 (#139). Each iteration does a full dict lookup (a whole DICT
         * chain walk) on a term that may be absent from every segment, so this
         * loop is unbounded in work even though nsegs bounds its trip count.
         * Nothing is locked here -- bm25_seg_dict_lookup takes and releases its
         * own page locks -- so the check fires. */
        CHECK_FOR_INTERRUPTS();

        bm25_seg_header_read(index, snap.segs[si].header_blkno,
                             snap.segs[si].gen, out_seg);
        if (!bm25_seg_dict_lookup(index, out_seg, term, termlen,
                                  &post_root, &post_off, &seg_df, NULL, NULL))
            continue;
        bm25_seg_block_header_read(index, post_root, post_off, out_seg->gen,
                                   out_hdr, out_imp, &next_blk, &next_off);
        *out_blk = post_root;
        *out_off = post_off;
        if (out_df != NULL)
            *out_df = seg_df;
        found_block = true;
    }

    if (!found_block)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("bm25: term not found in any live (sealed) segment")));
}

/* bm25_debug_block_ub(index regclass, qterm text) -- the safe upper bound
 * (bm25_wand.c:bm25_block_ub) for qterm's FIRST block, using live scan-time
 * stats. Paired with bm25_debug_term_contrib below in sql/43_wand_parity.sql
 * to prove the bound is never less than a real per-posting contribution.
 * bm25_block_ub itself IS on the live query path: bm25_wand_cursor_block_max
 * and the straddle bound (the pivot deep check), and through its per-field terms
 * (wand_field_ub) wand_cursor_sweep_global_ub, run unconditionally by
 * bm25_wand_cursor_open -- all in bm25_wand.c. Only this SRF is
 * development/regression surface -- it exposes the bound to SQL so it can be
 * compared against a real contribution row by row. */
PG_FUNCTION_INFO_V1(bm25_debug_block_ub);
Datum
bm25_debug_block_ub(PG_FUNCTION_ARGS)
{
    Oid             relid = PG_GETARG_OID(0);
    text           *qterm = PG_GETARG_TEXT_PP(1);
    Relation        index;
    BM25WandCtx     ctx;
    BM25SegmentHeader seg;
    BlockNumber     blk;
    uint16          off;
    BM25BlockHeader hdr;
    BM25BlockImpact imp;
    double          ub;

    index = bm25_index_open_readable(relid, AccessShareLock);
    wand_debug_ctx_and_block(index, VARDATA_ANY(qterm), VARSIZE_ANY_EXHDR(qterm),
                             &ctx, &seg, &blk, &off, &hdr, &imp, NULL);
    ub = bm25_block_ub(&imp, &ctx);
    index_close(index, AccessShareLock);
    PG_RETURN_FLOAT8(ub);
}

/* Per-posting callback for bm25_debug_term_contrib: the EXACT real-scorer
 * formula (bm25_termscore, matching seg_posting_cb's contrib computation
 * above) evaluated with the SAME ctx bm25_debug_block_ub was checked
 * against, over the SAME block's postings. Skips a field outside the ctx
 * (defensive; cannot happen for a block just read via that same ctx's
 * snapshot) and tombstoned docs, exactly like the real scorer. */
typedef struct
{
    BM25SegmentHeader *seg;
    /* Forward cursors over *seg's LIVEDOCS/NORMS chains, the same shape and reasons as
     * bm25_scan_rank.c's TermScoreCtx.rdr -- which is the very callback this one is written
     * to mirror, so it reads the segment the same way too. A debug SRF is still a
     * per-posting path: the one-shot accessors re-walked each chain from its root per
     * posting and took a SEGREAD-11 RelationGetNumberOfBlocks apiece. Opened on the
     * caller's segment header just before the scan; one segment per call. It supplies
     * the Relation, so this struct no longer carries one. */
    BM25SegReader      rdr;
    const BM25WandCtx *ctx;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
} TermContribDebugState;

static void
term_contrib_debug_cb(uint32 local_docid, uint32 tf, uint32 field_id, void *state)
{
    TermContribDebugState *s = (TermContribDebugState *) state;
    uint32                 doclen_f;
    double                 contrib;
    Datum                  vals[2];
    bool                   nulls[2] = {false, false};

    if (field_id >= s->ctx->field_count || s->ctx->idf_f[field_id] == 0.0)
        return;
    if (!bm25_seg_reader_doc_is_live(&s->rdr, local_docid))
        return;

    doclen_f = bm25_seg_reader_doclen_field(&s->rdr, local_docid, field_id);
    contrib  = s->ctx->boost_f[field_id] *
               bm25_termscore(s->ctx->idf_f[field_id], tf, doclen_f,
                              s->ctx->avgdl_f[field_id],
                              s->ctx->k1_f[field_id], s->ctx->b_f[field_id]);

    vals[0] = Int64GetDatum((int64) local_docid);
    vals[1] = Float8GetDatum(contrib);
    tuplestore_putvalues(s->ts, s->tupdesc, vals, nulls);
}

/* bm25_debug_term_contrib(index regclass, qterm text) -- one row per posting
 * in qterm's FIRST block (the exact same block bm25_debug_block_ub bounds),
 * (local_docid, contrib) with contrib the real per-posting BM25 contribution
 * computed from the identical ctx. max(contrib) is the "true_max" the bound
 * must dominate. Development/regression only. */
PG_FUNCTION_INFO_V1(bm25_debug_term_contrib);
Datum
bm25_debug_term_contrib(PG_FUNCTION_ARGS)
{
    ReturnSetInfo         *rsi   = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                    relid = PG_GETARG_OID(0);
    text                  *qterm = PG_GETARG_TEXT_PP(1);
    Relation               index;
    BM25WandCtx            ctx;
    BM25SegmentHeader      seg;
    BlockNumber            blk;
    uint16                 off;
    BM25BlockHeader        hdr;
    BM25BlockImpact        imp;
    TermContribDebugState  state;
    Tuplestorestate       *ts;
    TupleDesc              tupdesc;
    MemoryContext          oldctx;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);
    wand_debug_ctx_and_block(index, VARDATA_ANY(qterm), VARSIZE_ANY_EXHDR(qterm),
                             &ctx, &seg, &blk, &off, &hdr, &imp, NULL);

    state.seg     = &seg;
    bm25_seg_reader_init(&state.rdr, index, &seg);  /* opened on the segment just read */
    state.ctx     = &ctx;
    state.ts      = ts;
    state.tupdesc = tupdesc;
    bm25_seg_scan_postings(index, blk, off, hdr.ndocs, seg.gen,
                           term_contrib_debug_cb, &state,
                           InvalidBlockNumber, 0, NULL, NULL, NULL, 0);

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/*
 * bm25_debug_topk(k int, scores float8[], tids int[]) -- SRF over the M2b
 * Task 5 bounded top-k heap (BM25TopK, bm25_wand.c): offers each
 * (scores[i], tid(0, tids[i])) in array order, then drains the heap into
 * (rank, score, tid) rows starting at rank 1. No index, no real corpus --
 * this exercises the heap's own comparator in isolation, so
 * sql/43_wand_parity.sql can pin the drained order (including score-tied
 * entries) against hand-picked scores/tids rather than depending on a real
 * scan producing a tie by chance. Development/regression only.
 *
 * tids become ItemPointerData via ItemPointerSet(&tid, 0, tids[i]) --
 * synthetic (block 0), matching bm25_debug_accum's synthetic-TID idiom
 * elsewhere in this AM; only the tid's ORDER under ItemPointerCompare
 * matters for this probe, not any real heap page.
 */
PG_FUNCTION_INFO_V1(bm25_debug_topk);
Datum
bm25_debug_topk(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    int32            k = PG_GETARG_INT32(0);
    ArrayType       *scores_arr = PG_GETARG_ARRAYTYPE_P(1);
    ArrayType       *tids_arr   = PG_GETARG_ARRAYTYPE_P(2);
    Datum           *score_elems,
                    *tid_elems;
    bool            *score_nulls,
                    *tid_nulls;
    int              nscores,
                     ntids;
    BM25TopK        *heap;
    BM25Scored      *out;
    int              n,
                     i;
    TupleDesc        tupdesc;
    Tuplestorestate *ts;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    /* k is the heap's logical capacity (bm25_topk_create); a non-positive k has no
     * valid heap, and unlike an internal caller this SRF's k comes straight from a
     * SQL argument, so it is checked here too -- for the probe-named error message,
     * not because bm25_topk_create would let it through (QRY-10). */
    bm25_debug_topk_k_validate(k, "bm25_debug_topk");
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    deconstruct_array_builtin(scores_arr, FLOAT8OID, &score_elems, &score_nulls, &nscores);
    /* Explicit int4 descriptor via the general deconstruct_array: INT4OID was
     * not added to deconstruct_array_builtin's switch until PG 18.2, so the
     * wrapper errors "type 23 not supported" on stock 18.0/18.1 (floor PG17). */
    deconstruct_array(tids_arr, INT4OID, sizeof(int32), true, TYPALIGN_INT,
                      &tid_elems, &tid_nulls, &ntids);
    if (nscores != ntids)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bm25_debug_topk: scores and tids must have equal length")));

    heap = bm25_topk_create(k, CurrentMemoryContext);
    for (i = 0; i < nscores; i++)
    {
        ItemPointerData tid;

        int32           off;

        if (score_nulls[i] || tid_nulls[i])
            continue;               /* no NULL-handling contract here; just skip */
        /* Issue #313 SURFACE-10: the int is an offset number. 0 is not a valid one
         * (it tripped Assert(ItemPointerIsValid) on a cassert build), and anything
         * above MaxOffsetNumber used to truncate silently into a different tid. */
        off = DatumGetInt32(tid_elems[i]);
        if (off < FirstOffsetNumber || off > MaxOffsetNumber)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25_debug_topk: tid offset %d is outside [%d, %d]",
                            off, FirstOffsetNumber, MaxOffsetNumber)));
        ItemPointerSet(&tid, 0, (OffsetNumber) off);
        /* Pure heap-comparator probe: no segment context, so no key projection
         * to carry (Invalid/0/0 sentinel, same as a pending-only candidate). */
        (void) bm25_topk_offer(heap, DatumGetFloat8(score_elems[i]), &tid,
                               InvalidBlockNumber, 0, 0);
    }

    /* Sized by the heap's live count, not by k: the heap itself grows on demand
     * now (QRY-10), so sizing this by the SQL-supplied k would leave the probe as
     * the one remaining place where a large k costs memory regardless of input. */
    out = palloc(sizeof(BM25Scored) * Max(bm25_topk_count(heap), 1));
    n = bm25_topk_drain_sorted(heap, out);
    for (i = 0; i < n; i++)
    {
        Datum vals[3];
        bool  nulls[3] = {false, false, false};

        vals[0] = Int32GetDatum(i + 1);
        vals[1] = Float8GetDatum(out[i].score);
        vals[2] = Int32GetDatum((int32) ItemPointerGetOffsetNumber(&out[i].tid));
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }

    return (Datum) 0;
}

/*
 * bm25_debug_cursor_scan(index regclass, qterm text) -- M2b Task 6: drive the
 * per-term per-segment pull cursor (BM25WandCursor) linearly to exhaustion
 * over qterm's FIRST live segment (the same segment wand_debug_ctx_and_block
 * locates for the probes above), emitting (tid, cursor_score) per doc.
 *
 * The gate this exists to feed (sql/43_wand_parity.sql): cursor_score must be
 * BIT-IDENTICAL to the exhaustive scorer's per-doc, per-term score for the
 * same term (bm25_debug_rank restricted to a single-term query, joined by
 * tid) -- see bm25_wand_cursor_score_doc's comment for why every gate,
 * the termscore formula, and the summation order are all made to match the
 * real scorer exactly rather than merely approximate it. The cursor itself is
 * the production BMW driver's primitive -- bm25_wand_segment (bm25_wand.c)
 * opens one per in-scope query term per sealed segment. Only this SRF is
 * development/regression surface: it exposes the cursor to SQL so one term's
 * walk can be verified in isolation from the pivot machinery.
 */
PG_FUNCTION_INFO_V1(bm25_debug_cursor_scan);
Datum
bm25_debug_cursor_scan(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi   = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    text               *qterm = PG_GETARG_TEXT_PP(1);
    Relation            index;
    BM25WandCtx         ctx;
    BM25SegmentHeader   seg;
    BlockNumber         blk;
    uint16              off;
    BM25BlockHeader     hdr;
    BM25BlockImpact     imp;
    uint32              df;
    BM25WandCursor     *cur;
    /* Forward cursor over seg's DOCMAP for the per-doc TID resolve in the loop below,
     * mirroring the production WAND driver (bm25_wand_segment_scan), which holds one of
     * its own for exactly this. Opened on `seg` once it is filled; one segment here. */
    BM25SegReader       rdr;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
    MemoryContext       oldctx;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);
    wand_debug_ctx_and_block(index, VARDATA_ANY(qterm), VARSIZE_ANY_EXHDR(qterm),
                             &ctx, &seg, &blk, &off, &hdr, &imp, &df);

    bm25_seg_reader_init(&rdr, index, &seg);
    cur = bm25_wand_cursor_open(index, &seg, blk, off, df, &ctx, CurrentMemoryContext, NULL, false);

    while (bm25_wand_cursor_docid(cur) != BM25_DOCID_MAX)
    {
        uint32          docid = bm25_wand_cursor_docid(cur);
        double          score = 0.0;
        bool            contributed = false;
        ItemPointerData tid;
        Datum           vals[2];
        bool            nulls[2] = {false, false};

        CHECK_FOR_INTERRUPTS();

        /* Task 8 changed score_doc to fold postings into a caller-provided
         * running double (bit-exact multi-field accumulation); a single-term
         * single call from 0.0 reproduces the old per-doc return value exactly.
         * contributed is unused here (this probe emits every doc the cursor
         * visits, scored or not -- it is proving the SCORE matches, not the
         * seam's offer-gating, which 43_wand_parity's field-scope cases cover
         * via the real bm25_wand_build_ranking path instead). */
        bm25_wand_cursor_score_doc(cur, &score, &contributed);   /* advances past docid */
        tid = bm25_seg_reader_docid_to_tid(&rdr, docid);

        vals[0] = PointerGetDatum(&tid);
        vals[1] = Float8GetDatum(score);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/*
 * bm25_debug_cursor_skip(index regclass, qterm text, target int) -- M2b Task 7:
 * drive bm25_wand_cursor_next_geq(target) over qterm's FIRST live segment (the
 * same segment/term lookup wand_debug_ctx_and_block uses for every other WAND
 * probe) and report whether it landed correctly, plus how many blocks it
 * bypassed header-only.
 *
 * landed_ok is deliberately proven against a SECOND, independently-driven
 * cursor rather than asserted from internal state: a fresh cursor over the
 * IDENTICAL (post_root, post_off, df) is walked posting-by-posting via
 * next() (the linear path Task 6 already pinned bit-exact against the
 * exhaustive scorer) until ITS OWN docid reaches target. landed_ok is true
 * iff the two cursors agree on where they land -- equal real docids, or both
 * exhausted (BM25_DOCID_MAX) if target lies past every posting. This is
 * exactly the property sql/43_wand_parity.sql needs to check across many
 * targets ("does the skip path visit the identical position the decode-all
 * path would have"), so one SQL-level assertion per target is enough; no
 * separate comparison machinery has to live in the test file itself.
 *
 * Plain composite return (not a tuplestore SRF) -- one (target) in, one row
 * out, same shape as bm25_debug_global_stats. Development/regression only;
 * not on the query path.
 */
PG_FUNCTION_INFO_V1(bm25_debug_cursor_skip);
Datum
bm25_debug_cursor_skip(PG_FUNCTION_ARGS)
{
    Oid                relid  = PG_GETARG_OID(0);
    text              *qterm  = PG_GETARG_TEXT_PP(1);
    int32              target = PG_GETARG_INT32(2);
    Relation           index;
    BM25WandCtx        ctx;
    BM25SegmentHeader  seg;
    BlockNumber        blk;
    uint16             off;
    BM25BlockHeader    hdr;
    BM25BlockImpact    imp;
    uint32             df;
    BM25WandCursor    *skip_cur,
                      *lin_cur;
    uint32             skip_docid,
                       lin_docid;
    TupleDesc          tupdesc;
    Datum              vals[2];
    bool               nulls[2] = {false, false};
    HeapTuple          tuple;

    if (target < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_cursor_skip: target must be >= 0")));

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    index = bm25_index_open_readable(relid, AccessShareLock);
    wand_debug_ctx_and_block(index, VARDATA_ANY(qterm), VARSIZE_ANY_EXHDR(qterm),
                             &ctx, &seg, &blk, &off, &hdr, &imp, &df);

    skip_cur = bm25_wand_cursor_open(index, &seg, blk, off, df, &ctx, CurrentMemoryContext, NULL, false);
    bm25_wand_cursor_next_geq(skip_cur, (uint32) target);
    skip_docid = bm25_wand_cursor_docid(skip_cur);

    /* Ground truth: an independent cursor over the identical block run,
     * advanced ONE posting at a time until it reaches target itself. */
    lin_cur = bm25_wand_cursor_open(index, &seg, blk, off, df, &ctx, CurrentMemoryContext, NULL, false);
    while (bm25_wand_cursor_docid(lin_cur) != BM25_DOCID_MAX &&
           bm25_wand_cursor_docid(lin_cur) < (uint32) target)
    {
        /* HDL-09 (#139). target is an unbounded caller-supplied int32, so
         * bm25_debug_cursor_skip('idx','the',2147483647) walks the term's ENTIRE
         * posting run one posting at a time in this loop. It terminates (df bounds
         * it) and the function is REVOKEd from PUBLIC, so this is responsiveness
         * only -- but the identically-shaped cursor loop in bm25_debug_cursor_scan
         * above has had the check since ADR 0053 and this one was simply missed. */
        CHECK_FOR_INTERRUPTS();

        bm25_wand_cursor_next(lin_cur);
    }
    lin_docid = bm25_wand_cursor_docid(lin_cur);

    index_close(index, AccessShareLock);

    vals[0] = BoolGetDatum(skip_docid == lin_docid);
    vals[1] = Int32GetDatum((int32) bm25_wand_cursor_blocks_skipped(skip_cur));
    tuple = heap_form_tuple(tupdesc, vals, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * bm25_wand_stats(index regclass, query text, k int) -- M2b Task 11 (C-TEST):
 * runs a REAL WAND build (bm25_wand_build_ranking, the same path
 * bm25_debug_wand_rank exercises) with a live BM25WandStats accumulator
 * threaded through every cursor it opens, and reports the totals
 * (blocks_examined, blocks_skipped, docs_scored, deep_check_skips).
 *
 * This is the anti-neuter gate the bit-exact parity suite (43_wand_parity.sql)
 * cannot provide: a "WAND" that always decodes every block and never actually
 * calls next_geq's header-only skip still produces the exact right ranked
 * set (cf. the M2a hollow-reclaim finding -- passing parity is not proof a
 * mechanism ran). blocks_skipped > 0 for a genuinely selective query over a
 * large corpus witnesses that SOME next_geq skip fired, but that includes
 * plain WAND pivot alignment; deep_check_skips > 0 is the narrower witness
 * that the block-max deep check's OWN multi-cursor shallow-skip fired --
 * see BM25WandStats's comment and sql/44_wand_skip.sql Gate 1 vs Gate 1b.
 *
 * Corpus-stats prologue mirrors bm25_debug_wand_rank's exactly (same reason:
 * WAND applies to bag-of-words scans, so this drives the identical prologue,
 * BM25F across every field, that the real seam's bag-of-words path uses).
 * Plain composite return (four ints), not a tuplestore SRF -- one query in,
 * one totals row out, same shape as bm25_debug_cursor_skip. Development and
 * regression testing only; not on the query path.
 *
 * SQL: CREATE FUNCTION bm25_wand_stats(index regclass, query text, k int,
 *        OUT blocks_examined int, OUT blocks_skipped int, OUT docs_scored int,
 *        OUT deep_check_skips int)
 *        RETURNS record ...
 */
PG_FUNCTION_INFO_V1(bm25_wand_stats);
Datum
bm25_wand_stats(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    text               *query = PG_GETARG_TEXT_PP(1);
    int32               k     = PG_GETARG_INT32(2);
    Relation            index;
    BM25ScanOpaqueData  so_buf;         /* stack-allocated; lives for this call */
    BM25ScanOpaque      so    = &so_buf;
    BM25WandStats       stats = {0, 0, 0, 0};
    TupleDesc           tupdesc;
    Datum               vals[4];
    bool                nulls[4] = {false, false, false, false};
    HeapTuple           tuple;
    /* corpus-stats prologue (the shared one bm25_debug_wand_rank runs). */
    BM25ScanSnapshot    snap;
    BM25AnalyzerConfig  qcfg;
    BM25Token          *qtoks;
    int                 nq;
    uint64              live_ndocs;
    BM25FieldConfig     fcfg[BM25_MAX_FIELDS];
    double              avgdl_f[BM25_MAX_FIELDS];
    uint64              fld_ndocs[BM25_MAX_FIELDS];

    bm25_debug_topk_k_validate(k, "bm25_wand_stats");
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    index = bm25_index_open_readable(relid, AccessShareLock);

    MemSet(so, 0, sizeof(BM25ScanOpaqueData));
    so->scanctx = CurrentMemoryContext;   /* no cross-call scan; the ranked set
                                            * this throws away can live in the
                                            * SQL call's own context */
    so->qfield  = BM25_FIELD_ALL;

    /* Shared prologue (see bm25_scan_corpus_stats) -- the identical call the real
     * seam's bag-of-words path makes, which is the point of this probe. No
     * store_pos: WAND never reads positions. */
    bm25_scan_corpus_stats(index, &snap, &qcfg, NULL,
                           &live_ndocs, fcfg, avgdl_f, fld_ndocs);

    nq = bm25_analyze(&qcfg, VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query), &qtoks);

    bm25_wand_build_ranking(index, so, &snap, qtoks, nq, live_ndocs,
                            avgdl_f, fld_ndocs, fcfg, BM25_FIELD_ALL, k, &stats);

    /* so->ranked/scores/ranked_keys landed in so->scanctx (this call's own
     * context, above) and are thrown away with it on return -- only the
     * counters accumulated into stats are wanted here. */
    index_close(index, AccessShareLock);

    vals[0] = Int32GetDatum((int32) stats.blocks_examined);
    vals[1] = Int32GetDatum((int32) stats.blocks_skipped);
    vals[2] = Int32GetDatum((int32) stats.docs_scored);
    vals[3] = Int32GetDatum((int32) stats.deep_check_skips);
    tuple = heap_form_tuple(tupdesc, vals, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}


/* -------------------------------------------------------------------------
 * M6 Task 2: jsonb query tree debug/introspection
 * -------------------------------------------------------------------------
 * bm25_query_render/_group/render_leaf_text/render_field print a parsed
 * BM25Query as canonical text -- the AST-shape witness bm25_debug_query_parse
 * exposes to SQL. They belong in THIS file rather than bm25_query.c because
 * rendering is a pure debug/test concern, like everything else here;
 * bm25_query.c stays scoped to the functions the real scan path calls
 * (parse/flatten/eval/glob).
 */
static void bm25_query_render(const BM25Query *n, StringInfo buf);

static void
render_field(StringInfo buf, int32 field_id)
{
    if (field_id == BM25_FIELD_ALL)
        appendStringInfoString(buf, "fALL");
    else
        appendStringInfo(buf, "f%d", field_id);
}

static void
render_leaf_text(StringInfo buf, const char *tag, int32 field_id,
                 const char *text, int textlen)
{
    appendStringInfo(buf, "%s(", tag);
    render_field(buf, field_id);
    appendStringInfoChar(buf, ',');
    appendStringInfoChar(buf, '"');
    appendBinaryStringInfo(buf, text, textlen);
    appendStringInfoString(buf, "\")");
}

/* Renders "label=[child, child, ...]" iff the group is non-empty, prefixing
 * ", " when a PRIOR group already rendered something -- so a node with only
 * a "must" array (should/must_not empty) prints just "must=[...]", matching
 * the builders' habit of defaulting the other two arrays to empty. */
static void
bm25_query_render_group(StringInfo buf, const char *label,
                       BM25Query * const *nodes, int n, bool *need_comma)
{
    int i;

    if (n == 0)
        return;
    if (*need_comma)
        appendStringInfoString(buf, ", ");
    appendStringInfo(buf, "%s=[", label);
    for (i = 0; i < n; i++)
    {
        if (i > 0)
            appendStringInfoString(buf, ", ");
        bm25_query_render(nodes[i], buf);
    }
    appendStringInfoChar(buf, ']');
    *need_comma = true;
}

static void
bm25_query_render(const BM25Query *n, StringInfo buf)
{
    switch (n->kind)
    {
        case BM25Q_MATCH:
            render_leaf_text(buf, "MATCH", n->field_id, n->text, n->textlen);
            break;
        case BM25Q_TERM:
            render_leaf_text(buf, "TERM", n->field_id, n->text, n->textlen);
            break;
        case BM25Q_WILDCARD:
            render_leaf_text(buf, "WILDCARD", n->field_id, n->text, n->textlen);
            break;
        case BM25Q_PHRASE:
            appendStringInfoString(buf, "PHRASE(");
            render_field(buf, n->field_id);
            appendStringInfoChar(buf, ',');
            appendStringInfoChar(buf, '"');
            appendBinaryStringInfo(buf, n->text, n->textlen);
            appendStringInfo(buf, "\",slop=%d,ordered=%s)",
                             n->slop, n->ordered ? "true" : "false");
            break;
        case BM25Q_BOOLEAN:
            {
                bool need_comma = false;

                appendStringInfoString(buf, "BOOLEAN(");
                bm25_query_render_group(buf, "must", n->must, n->nmust, &need_comma);
                bm25_query_render_group(buf, "should", n->should, n->nshould, &need_comma);
                bm25_query_render_group(buf, "must_not", n->must_not, n->nmust_not, &need_comma);
                appendStringInfoChar(buf, ')');
                break;
            }
        case BM25Q_BOOST:
            appendStringInfo(buf, "BOOST(weight=%g,", n->boost);
            bm25_query_render(n->child, buf);
            appendStringInfoChar(buf, ')');
            break;
    }
}

/*
 * bm25_debug_query_parse(index regclass, query jsonb) -> text -- parse `query`
 * against `index`'s baked field config (opened read-only, same snapshot
 * discipline bm25_scan_snapshot everywhere else uses) and render the
 * resulting BM25Query as canonical text. Regression/introspection only --
 * not on the query path (bm25_rescan is the real caller of bm25_query_parse).
 */
PG_FUNCTION_INFO_V1(bm25_debug_query_parse);
Datum
bm25_debug_query_parse(PG_FUNCTION_ARGS)
{
    Oid              relid = PG_GETARG_OID(0);
    Jsonb           *jb    = PG_GETARG_JSONB_P(1);
    Relation         index;
    BM25ScanSnapshot snap;
    BM25FieldConfig  fields[BM25_MAX_FIELDS];
    BM25Query       *tree;
    MemoryContext    parsectx;
    StringInfoData   buf;
    text            *result;

    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_scan_snapshot(index, &snap);

    if (snap.field_config_blkno != InvalidBlockNumber)
    {
        BM25FieldConfigHeader fhdr;

        bm25_fieldcfg_read(index, snap.field_config_blkno, &fhdr, fields, NULL);
    }
    else
    {
        /* No field-config page: the single-default-field convention used
         * throughout the scorer (bm25_scan_build_ranking_exhaustive et al) --
         * one field, id 0, unnamed, so a jsonb node naming any field is simply
         * "unknown" (there is nothing to resolve against). */
        MemSet(fields, 0, sizeof(fields));
        fields[0].field_id = 0;
    }

    /* The parsed tree only needs to live long enough to be rendered; a fresh
     * child context (deleted right after) keeps that scratch out of the
     * caller's context rather than leaking it for the rest of the query. */
    parsectx = AllocSetContextCreate(CurrentMemoryContext,
                                     "bm25 debug query parse",
                                     ALLOCSET_SMALL_SIZES);
    tree = bm25_query_parse(jb, fields, snap.field_count, parsectx);

    initStringInfo(&buf);
    bm25_query_render(tree, &buf);

    MemoryContextDelete(parsectx);
    index_close(index, AccessShareLock);

    result = cstring_to_text_with_len(buf.data, buf.len);
    pfree(buf.data);

    PG_RETURN_TEXT_P(result);
}

/*
 * bm25_debug_query_flatten(index regclass, query jsonb) -> text -- parse +
 * bm25_query_flatten, rendering one line per leaf in leaf_bit order:
 * "[bit] boost=W negated=B <rendered leaf>". Exercises the flatten DFS (leaf_bit
 * assignment, top-down boost folding through nested BOOST, and must_not
 * negation propagation through nested BOOLEANs) in isolation.
 * bm25_query_flatten's production callers are both on the query path:
 * bm25_scan_build_ranking_exhaustive (bm25_scan_rank.c) and bm25_rescan_parse_jsonb
 * (bm25_scan.c). Regression/introspection only.
 */
PG_FUNCTION_INFO_V1(bm25_debug_query_flatten);
Datum
bm25_debug_query_flatten(PG_FUNCTION_ARGS)
{
    Oid              relid = PG_GETARG_OID(0);
    Jsonb           *jb    = PG_GETARG_JSONB_P(1);
    Relation         index;
    BM25ScanSnapshot snap;
    BM25FieldConfig  fields[BM25_MAX_FIELDS];
    BM25Query       *tree;
    BM25Query       *leaves[BM25_QUERY_MAX_LEAVES];
    int              nleaves,
                     i;
    MemoryContext    parsectx;
    StringInfoData   buf;
    text            *result;

    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_scan_snapshot(index, &snap);

    if (snap.field_config_blkno != InvalidBlockNumber)
    {
        BM25FieldConfigHeader fhdr;

        bm25_fieldcfg_read(index, snap.field_config_blkno, &fhdr, fields, NULL);
    }
    else
    {
        MemSet(fields, 0, sizeof(fields));
        fields[0].field_id = 0;
    }

    parsectx = AllocSetContextCreate(CurrentMemoryContext,
                                     "bm25 debug query flatten",
                                     ALLOCSET_SMALL_SIZES);
    tree    = bm25_query_parse(jb, fields, snap.field_count, parsectx);
    nleaves = bm25_query_flatten(tree, leaves, BM25_QUERY_MAX_LEAVES);

    initStringInfo(&buf);
    for (i = 0; i < nleaves; i++)
    {
        if (i > 0)
            appendStringInfoChar(&buf, '\n');
        appendStringInfo(&buf, "[%d] boost=%g negated=%s ",
                         leaves[i]->leaf_bit, leaves[i]->boost,
                         leaves[i]->negated ? "t" : "f");
        bm25_query_render(leaves[i], &buf);
    }

    MemoryContextDelete(parsectx);
    index_close(index, AccessShareLock);

    result = cstring_to_text_with_len(buf.data, buf.len);
    pfree(buf.data);

    PG_RETURN_TEXT_P(result);
}

/*
 * bm25_debug_glob_match(pattern text, term text) -> bool -- SQL-visible unit
 * check for bm25_glob_match, whose production caller is the wildcard dict
 * expansion bm25_dict_expand_wildcard (bm25_seg_dict.c), reached on the query
 * path from bm25_scan_build_ranking_exhaustive. Regression/introspection only.
 */
PG_FUNCTION_INFO_V1(bm25_debug_glob_match);
Datum
bm25_debug_glob_match(PG_FUNCTION_ARGS)
{
    text *pattern = PG_GETARG_TEXT_PP(0);
    text *term    = PG_GETARG_TEXT_PP(1);

    PG_RETURN_BOOL(bm25_glob_match(VARDATA_ANY(pattern), (int) VARSIZE_ANY_EXHDR(pattern),
                                  VARDATA_ANY(term), (int) VARSIZE_ANY_EXHDR(term)));
}

/* -------------------------------------------------------------------------
 * Cancellable-work counter (issue #156)
 * -------------------------------------------------------------------------
 * The counter itself and its two SQL accessors. See bm25.h's BM25_WORK_UNIT()
 * header for the design -- why it is a C global (it has to survive the abort of
 * the statement being measured, so a PL/pgSQL exception handler can read it),
 * why reset is explicit, and what one unit means.
 *
 * It lives HERE, with the rest of the introspection surface, rather than in one
 * of the files that increments it: no production path reads it, and the file
 * that owns the readers should own the state. The increment sites reach it
 * through bm25.h like any other shared declaration.
 *
 * The definition is at file scope and deliberately initialised: a backend that
 * has never called bm25_debug_work_reset() still reports a well-defined 0.
 */
uint64 bm25_work_units = 0;

/*
 * bm25_debug_work_reset() -> void -- zero the counter.
 *
 * The ONLY reset point. Call it immediately before the work to be measured;
 * anything bm25-related executed in between is counted too, because the counter
 * is one global and not a per-statement or per-scan tally.
 *
 * Not transactional: a rollback does not restore the previous value, which is
 * precisely what makes the pairing with bm25_debug_work_units() usable from an
 * exception handler.
 */
PG_FUNCTION_INFO_V1(bm25_debug_work_reset);
Datum
bm25_debug_work_reset(PG_FUNCTION_ARGS)
{
    bm25_work_units = 0;
    PG_RETURN_VOID();
}

/*
 * bm25_debug_work_units() -> bigint -- read the counter.
 *
 * Returned as int64. The counter is uint64, but one unit is a page decode or a
 * dictionary entry, so reaching INT64_MAX would require more work than a backend
 * can perform in the life of the hardware; no saturation logic is warranted.
 */
PG_FUNCTION_INFO_V1(bm25_debug_work_units);
Datum
bm25_debug_work_units(PG_FUNCTION_ARGS)
{
    PG_RETURN_INT64((int64) bm25_work_units);
}

/*
 * bm25_debug_seg_reader_diff(index regclass, seg int, what text, docids int[])
 *   -> SETOF (local_docid bigint, tid tid, reader_bytes bytea, oneshot_bytes bytea,
 *             reader_int bigint, oneshot_int bigint)
 * -- issue #225 differential probe: a BM25SegReader accessor against its one-shot
 * root-walk counterpart, over one segment and a caller-chosen docid ORDER.
 *
 * Issue #225 moved per-row and per-posting callers off the one-shot accessors (which
 * start from the chain root on every call) onto reader accessors (which resume from
 * wherever the previous call landed). The two must agree on every cell for every
 * access order, and the order is the point: ascending access is what the converted
 * callers mostly do, but a descending or scattered order is what makes a cursor jump
 * BACKWARD and fall back to a root walk -- the branch a regression in the resume test
 * would corrupt. So ONE reader is carried across the whole array in the given order,
 * and the one-shot form is called fresh for each element -- FIRST, so that on a corrupt
 * chain the one-shot walk is the one that meets the bad link and raises. The reader
 * form's bound has callers of its own that reach it on a real path (both ranking
 * builders, the merge, bm25_debug_seg_keymap); the one-shot form has only this.
 *
 *   what = 'doclen'   bm25_seg_reader_doclen  vs  bm25_seg_doclen
 *   what = 'key'      bm25_seg_reader_key     vs  bm25_seg_key
 *
 * tid comes from a SEPARATE reader's DOCMAP cursor, so the suite can join each row to
 * its heap tuple and check both columns against a value computed from the table
 * itself -- equality of two readings alone would pass if both were wrong the same way.
 * reader_int/oneshot_int carry the value as a number; reader_bytes/oneshot_bytes carry
 * the exact bytes (for doclen, the uint32 as stored; for a key, the key_size bytes
 * the KEYMAP holds, with the _int columns decoded only for an int4/int8 key_field and
 * NULL otherwise). A keyless segment returns NULL for all four. TEST-ONLY, read-only;
 * REVOKEd from PUBLIC with every bm25_debug_*.
 */
PG_FUNCTION_INFO_V1(bm25_debug_seg_reader_diff);
Datum
bm25_debug_seg_reader_diff(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi    = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid  = PG_GETARG_OID(0);
    int32               seg    = PG_GETARG_INT32(1);
    char               *what   = text_to_cstring(PG_GETARG_TEXT_PP(2));
    ArrayType          *arr    = PG_GETARG_ARRAYTYPE_P(3);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;
    BM25SegReader       rdr;        /* the reader under test, carried across the array */
    BM25SegReader       tidrdr;     /* DOCMAP only: the join key, not under test */
    TupleDesc           tupdesc;
    Tuplestorestate    *ts;
    MemoryContext       oldctx;
    Datum              *elems;
    bool               *nullp;
    int                 nelems;
    int                 i;
    bool                want_key;
    uint8               km_type = BM25_KEY_NONE;
    uint16              km_size = 0;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    /* Arguments before the index is opened (97_debug_probe_arguments). */
    if (strcmp(what, "doclen") != 0 && strcmp(what, "key") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_seg_reader_diff: unknown accessor \"%s\"", what),
                 errhint("Use doclen or key.")));
    want_key = (strcmp(what, "key") == 0);
    /* Explicit int4 descriptor, for the PG 17 / 18.0 reason given at
     * bm25_debug_topk's deconstruct_array above. */
    deconstruct_array(arr, INT4OID, sizeof(int32), true, TYPALIGN_INT,
                      &elems, &nullp, &nelems);
    for (i = 0; i < nelems; i++)
        if (nullp[i] || DatumGetInt32(elems[i]) < 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25_debug_seg_reader_diff: docid at index %d is NULL or negative",
                            i)));

    oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
    ts = tuplestore_begin_heap(true, false, work_mem);
    MemoryContextSwitchTo(oldctx);
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult  = ts;
    rsi->setDesc    = tupdesc;

    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_segcat_read(index, &segs, &nsegs);
    if (seg < 0 || (uint32) seg >= nsegs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    }
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);
    /* Range-checked against the segment, not left to the accessors: they raise
     * ERRCODE_INDEX_CORRUPTED for an out-of-range docid, which is the right answer
     * for a page-supplied value and the wrong one for a caller's typo. */
    for (i = 0; i < nelems; i++)
        if ((uint64) DatumGetInt32(elems[i]) >= h.ndocs)
        {
            index_close(index, AccessShareLock);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25_debug_seg_reader_diff: docid %d out of range (segment has "
                            UINT64_FORMAT " docs)", DatumGetInt32(elems[i]), h.ndocs)));
        }

    if (want_key)
        (void) bm25_seg_keymeta(index, &h, &km_type, &km_size);

    bm25_seg_reader_init(&rdr, index, &h);
    bm25_seg_reader_init(&tidrdr, index, &h);
    for (i = 0; i < nelems; i++)
    {
        uint32          d = (uint32) DatumGetInt32(elems[i]);
        Datum           vals[6];
        bool            nulls[6] = {false, false, false, false, false, false};
        ItemPointerData tid;
        unsigned char   rbuf[Max(BM25_KEY_MAX_SIZE, sizeof(uint32))];
        unsigned char   obuf[Max(BM25_KEY_MAX_SIZE, sizeof(uint32))];
        uint16          rlen = 0;
        uint16          olen = 0;
        bool            have = true;
        bytea          *rb;
        bytea          *ob;

        CHECK_FOR_INTERRUPTS();

        tid = bm25_seg_reader_docid_to_tid(&tidrdr, d);
        if (want_key)
        {
            bool    got_o = bm25_seg_key(index, &h, d, obuf, &olen);
            bool    got_r = bm25_seg_reader_key(&rdr, d, rbuf, &rlen);

            /* The two agree on keylessness by construction (both test keymap_root
             * first); a disagreement would be the finding, so it is reported rather
             * than asserted away. */
            if (got_r != got_o)
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bm25_debug_seg_reader_diff: reader and one-shot disagree "
                                "on whether docid %u has a key", d)));
            have = got_r;
        }
        else
        {
            uint32  via_o = bm25_seg_doclen(index, &h, d);
            uint32  via_r = bm25_seg_reader_doclen(&rdr, d);

            memcpy(rbuf, &via_r, sizeof(uint32));
            memcpy(obuf, &via_o, sizeof(uint32));
            rlen = olen = sizeof(uint32);
        }

        vals[0] = Int64GetDatum((int64) d);
        vals[1] = ItemPointerGetDatum(&tid);
        if (!have)
        {
            nulls[2] = nulls[3] = nulls[4] = nulls[5] = true;
            tuplestore_putvalues(ts, tupdesc, vals, nulls);
            continue;
        }
        rb = (bytea *) palloc(VARHDRSZ + rlen);
        SET_VARSIZE(rb, VARHDRSZ + rlen);
        memcpy(VARDATA(rb), rbuf, rlen);
        ob = (bytea *) palloc(VARHDRSZ + olen);
        SET_VARSIZE(ob, VARHDRSZ + olen);
        memcpy(VARDATA(ob), obuf, olen);
        vals[2] = PointerGetDatum(rb);
        vals[3] = PointerGetDatum(ob);

        if (!want_key)
        {
            uint32  vr;
            uint32  vo;

            memcpy(&vr, rbuf, sizeof(uint32));
            memcpy(&vo, obuf, sizeof(uint32));
            vals[4] = Int64GetDatum((int64) vr);
            vals[5] = Int64GetDatum((int64) vo);
        }
        else if (km_type == BM25_KEY_INT4 && rlen == sizeof(int32) && olen == sizeof(int32))
        {
            int32   vr;
            int32   vo;

            memcpy(&vr, rbuf, sizeof(int32));
            memcpy(&vo, obuf, sizeof(int32));
            vals[4] = Int64GetDatum((int64) vr);
            vals[5] = Int64GetDatum((int64) vo);
        }
        else if (km_type == BM25_KEY_INT8 && rlen == sizeof(int64) && olen == sizeof(int64))
        {
            int64   vr;
            int64   vo;

            memcpy(&vr, rbuf, sizeof(int64));
            memcpy(&vo, obuf, sizeof(int64));
            vals[4] = Int64GetDatum(vr);
            vals[5] = Int64GetDatum(vo);
        }
        else
            nulls[4] = nulls[5] = true;
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}
