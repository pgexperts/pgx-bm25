/* bm25_merge.c -- tiered merge engine.
 *
 * Role: decide WHICH segments to merge (this task) and EXECUTE the merge as a
 * two-phase atomic catalog swap (Task 23). Policy is a size-layered ladder
 * (ParadeDB-style) plus a target-segment-count cap and a per-segment
 * tombstone-fraction trigger. The selection is pure (no I/O) so it is unit-
 * testable via bm25_debug_merge_plan and reusable by both the manual
 * bm25_merge() path and the autovacuum-cadence bm25_vacuumcleanup path.
 *
 * Invariant carried into Task 23, in the form it now holds: ONE MERGE IS ONE
 * ATOMIC PUBLISH RECORD. The chosen set is merged into one or more new segments,
 * and the single record that publishes ALL of them is also the record that removes
 * ALL the chosen entries, so the catalog is never observed in a partially-merged
 * state. It used to say "into ONE new segment", which was the same statement while
 * the accumulator was unbounded; BUILD-04 made the merge cut its accumulator at
 * bm25_maintenance_budget_bytes, so a large merge now yields several outputs.
 *
 * The count changed; the atomicity did not, and must not. Publishing chunk k while
 * its inputs are still live is not a transient window -- see the double-scoring
 * argument on bm25_segcat_publish_swap. Chunk boundaries are also INPUT-SEGMENT
 * GRANULAR for a related reason spelled out on bm25_accum_from_segments: cutting
 * mid-segment splits a document's postings across two outputs.
 *
 * The visible consequence, stated honestly: BM25_TARGET_SEGMENT_COUNT is
 * unreachable for a corpus whose accumulator footprint exceeds 8x the budget. The
 * steady-state segment count floors at roughly footprint/budget and scan cost grows
 * with it. Raising maintenance_work_mem and running bm25_merge() is the operator
 * remedy; a streaming k-way segment merge, which this format's sorted dicts and
 * ascending docids would permit at O(1) memory, is the real cure and is recorded as
 * future work in ADR 0084. */
#include "postgres.h"

#include "bm25.h"
#include "funcapi.h"
#include "access/genam.h"
#include "access/table.h"       /* table_open / table_close (merge heap pin) */
#include "catalog/index.h"
#include "storage/lmgr.h"       /* LockPage / ConditionalLockPage (merge singleton) */
#include "utils/tuplestore.h"   /* tuplestore_begin_heap/putvalues -- no longer pulled
                                 * in transitively via funcapi.h as of PG19 */
#include <math.h>
#include <stdint.h>          /* SIZE_MAX (overshoot-warning threshold guard) */

int
bm25_merge_layer_of(uint64 ndocs)
{
    /* Floor log base FANOUT. ndocs==0 (degenerate) lands in layer 0. */
    if (ndocs < 2)
        return 0;
    return (int) floor(log((double) ndocs) / log((double) BM25_MERGE_LAYER_FANOUT));
}

/* Is this segment tombstoned enough to be worth rewriting on its own? (BUILD-11)
 *
 * `ndocs - live_ndocs` is unsigned subtraction on two fields that arrive by raw
 * memcpy off a SEGCAT page. Nothing validates them: bm25_segcat_read and
 * bm25_scan_snapshot bound the entry COUNT against the page's content length but
 * check no field of any entry, and both bm25_merge_select callers feed straight from
 * bm25_segcat_read. So live_ndocs > ndocs -- a torn or corrupt catalog page -- wraps
 * `dead` to something near 2^64, the fraction lands astronomically above the
 * threshold, and every such segment is selected for merge on every check. That is a
 * self-sustaining rewrite loop over a segment whose entry is already wrong, not a
 * one-off misjudgement.
 *
 * Ordering the comparison as live_ndocs >= ndocs first makes the arithmetic total:
 * the subtraction below cannot underflow once that has returned. In-repo writers
 * never produce the state (the builders assign live_ndocs = ndocs and the tombstone
 * path decrements under `if (live_ndocs > 0)`), so this is the ADR 0040 on-disk trust
 * boundary rather than a reachable internal bug -- treated the same way as the rest
 * of that boundary: refuse to act on it rather than compute with it.
 *
 * Extracted because the identical test existed verbatim at two sites in this file and
 * only one of them would ever have been found by a reader looking for the first. */
static bool
bm25_merge_tombstone_trigger(const BM25SegCatEntry *seg)
{
    uint64 dead;

    if (seg->ndocs == 0 || seg->live_ndocs >= seg->ndocs)
        return false;

    dead = seg->ndocs - seg->live_ndocs;
    return (double) dead / (double) seg->ndocs >= BM25_MERGE_TOMBSTONE_FRAC;
}

/* Decode-boundary probe for bm25_merge_tombstone_trigger. The underflow it guards
 * needs live_ndocs > ndocs in a SEGCAT entry, which no debug lever can write (the
 * catalog is rebuilt wholesale by seal and merge), so the suite drives the predicate
 * directly -- the same pure-function shape the rest of the trust-boundary probes use.
 * Returns whether the segment would be selected for a tombstone rewrite. */
PG_FUNCTION_INFO_V1(bm25_debug_merge_tombstone_trigger);
Datum
bm25_debug_merge_tombstone_trigger(PG_FUNCTION_ARGS)
{
    int64           ndocs      = PG_GETARG_INT64(0);
    int64           live_ndocs = PG_GETARG_INT64(1);
    BM25SegCatEntry seg;

    if (ndocs < 0 || live_ndocs < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_merge_tombstone_trigger: counts must be non-negative")));

    memset(&seg, 0, sizeof(seg));
    seg.ndocs      = (uint64) ndocs;
    seg.live_ndocs = (uint64) live_ndocs;

    PG_RETURN_BOOL(bm25_merge_tombstone_trigger(&seg));
}

int
bm25_merge_select(const BM25SegCatEntry *segs, uint32 nsegs, bool *chosen)
{
    int         nchosen = 0;
    uint32      i;
    int         layer_count[64];
    int         best_layer = -1;
    int         best_count = 0;
    int         l;

    for (i = 0; i < nsegs; i++)
        chosen[i] = false;
    if (nsegs < 2)
        return 0;

    memset(layer_count, 0, sizeof(layer_count));

    /* (a) Per-segment tombstone-fraction trigger: any segment >= the threshold
     * is chosen outright (merge rewrites it, dropping the dead docs). */
    for (i = 0; i < nsegs; i++)
    {
        if (bm25_merge_tombstone_trigger(&segs[i]))
        {
            chosen[i] = true;
            nchosen++;
        }
    }

    /* (b) Size-layer ladder: find the most-populated layer at/above fanout. */
    for (i = 0; i < nsegs; i++)
    {
        l = bm25_merge_layer_of(segs[i].ndocs);
        if (l >= 0 && l < 64)
            layer_count[l]++;
    }
    for (l = 0; l < 64; l++)
    {
        if (layer_count[l] >= BM25_MERGE_LAYER_FANOUT && layer_count[l] > best_count)
        {
            best_count = layer_count[l];
            best_layer = l;
        }
    }
    if (best_layer >= 0)
    {
        for (i = 0; i < nsegs; i++)
        {
            if (!chosen[i] && bm25_merge_layer_of(segs[i].ndocs) == best_layer)
            {
                chosen[i] = true;
                nchosen++;
            }
        }
    }

    /* (c) target_segment_count cap: if still over budget and nothing else fired,
     * merge the two segments with the smallest total ndocs to make progress. */
    if (nchosen == 0 && nsegs > BM25_TARGET_SEGMENT_COUNT)
    {
        uint32  s0 = 0, s1 = 1;
        if (segs[s1].ndocs < segs[s0].ndocs)
        {
            uint32  t = s0;

            s0 = s1;
            s1 = t;
        }
        for (i = 2; i < nsegs; i++)
        {
            if (segs[i].ndocs < segs[s0].ndocs)
            {
                s1 = s0;
                s0 = i;
            }
            else if (segs[i].ndocs < segs[s1].ndocs)
            {
                s1 = i;
            }
        }
        chosen[s0] = true;
        chosen[s1] = true;
        nchosen = 2;
    }

    /* Cap inputs so one merge record's Phase-2 work stays bounded. */
    if (nchosen > BM25_MERGE_MAX_INPUTS)
    {
        int kept = 0;
        for (i = 0; i < nsegs; i++)
        {
            if (chosen[i])
            {
                if (kept >= BM25_MERGE_MAX_INPUTS)
                    chosen[i] = false;
                else
                    kept++;
            }
        }
        nchosen = BM25_MERGE_MAX_INPUTS;
    }

    /* A single chosen segment is only worth merging if it is the tombstone case;
     * the ladder/cap paths require >= 2 to actually reduce segment count. */
    if (nchosen == 1)
    {
        for (i = 0; i < nsegs; i++)
            if (chosen[i])
                break;
        if (!bm25_merge_tombstone_trigger(&segs[i]))
        {
            chosen[i] = false;
            nchosen = 0;
        }
    }

    return nchosen;
}

/* bm25_debug_merge_plan -- SRF introspection: one row per catalog entry with
 * (gen, ndocs, live_ndocs, layer, chosen), where chosen reflects bm25_merge_select.
 * Uses the Materialize-mode tuplestore idiom with the per-query memory context
 * (C7: must switch to ecxt_per_query_memory before tuplestore_begin_heap, or
 * PG 18 raises "invalid tuplestore state" when the executor drains after return).
 * Mirrors bm25_debug_segcat / bm25_debug_tombstone in bm25_seg_debug.c exactly. */
PG_FUNCTION_INFO_V1(bm25_debug_merge_plan);
Datum
bm25_debug_merge_plan(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid             relid = PG_GETARG_OID(0);
    Relation        index;
    BM25SegCatEntry *segs = NULL;
    uint32          nsegs = 0;
    bool           *chosen;
    Tuplestorestate *ts;
    TupleDesc        tupdesc;
    uint32          i;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    index = bm25_index_open_readable(relid, AccessShareLock);
    tupdesc = BlessTupleDesc(tupdesc);
    /* C7: the Materialize-mode tuplestore MUST live in the per-query context, not
     * the (short-lived) current context, or PG 18 raises "invalid tuplestore
     * state" when the executor drains it after this call returns.  Mirrors
     * bm25_debug_segcat / bm25_debug_tombstone in bm25_seg_debug.c. */
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    bm25_segcat_read(index, &segs, &nsegs);     /* Phase-1/2 snapshot helper */
    chosen = (bool *) palloc(sizeof(bool) * Max(nsegs, 1));
    (void) bm25_merge_select(segs, nsegs, chosen);

    for (i = 0; i < nsegs; i++)
    {
        Datum   vals[5];
        bool    nulls[5] = {false, false, false, false, false};
        vals[0] = Int32GetDatum((int32) segs[i].gen);
        vals[1] = Int64GetDatum((int64) segs[i].ndocs);
        vals[2] = Int64GetDatum((int64) segs[i].live_ndocs);
        vals[3] = Int32GetDatum(bm25_merge_layer_of(segs[i].ndocs));
        vals[4] = BoolGetDatum(chosen[i]);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* ---- Task 23: merge executor + atomic catalog swap ---- *
 *
 * Re-accumulate only the LIVE docs of the chosen segments into a fresh BM25Accum,
 * reassigning dense local doc-ids in catalog-then-old-id order, then publish the
 * merged segment via the single-record atomic swap. Per matched (local_docid, tf)
 * we need each doc's per-term tf; rather than transpose the stored postings into
 * per-doc token lists (O(terms*docs)), we register every live doc up front
 * (add_doc_blank, fixing its new id + doclen from NORMS) and then stream each
 * term's postings straight into the accumulator (add_posting), keyed by the
 * remapped doc-id. Because all of one segment's live docs are registered before
 * any of the next segment's, the new ids stay ascending within every term, so the
 * accumulator's postings remain in the docid order the segment builder requires. */
typedef struct
{
    BM25Accum  *out;            /* destination accumulator */
    uint32     *idmap;          /* old local_docid -> new local_docid, or UINT32_MAX */
    char       *cur_term;       /* current dict term being streamed (iter-owned) */
    int         cur_termlen;
    /* M4 position replay: the reader fires post_cb THEN (for a position-bearing
     * field, in lockstep) pos_cb for the SAME posting. merge_post_cb records the
     * just-added posting's field here so merge_pos_cb attaches its positions to it.
     * cur_field == UINT32_MAX means the last posting was tombstoned/skipped, so a
     * following pos_cb (the reader still emits one for an on-field posting) is a
     * no-op -- the doc is not in the merged segment. */
    uint32      cur_field;
    /* Source segment's doc count, i.e. the size idmap was allocated with
     * (Max(h.ndocs, 1), see the caller below). Bounds old_docid before it
     * indexes idmap -- see merge_post_cb. */
    uint64      ndocs;
} MergeFeedState;

/* The decode boundary for a merge source posting's docid. Extracted so the
 * debug probe below exercises the identical check merge_post_cb runs against
 * a real corrupt/oversized old_docid. */
static void
bm25_merge_docid_validate(uint32 old_docid, uint64 ndocs)
{
    if (old_docid >= ndocs)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: merge source posting docid %u exceeds segment ndocs "
                        UINT64_FORMAT,
                        old_docid, ndocs)));
}

static void
merge_post_cb(uint32 old_docid, uint32 tf, uint32 field_id, void *state)
{
    MergeFeedState *st = (MergeFeedState *) state;
    uint32          newid;

    /* old_docid is decoded straight off the source segment's POST pages
     * (bm25_seg_scan_postings) and is otherwise unbounded here: bm25_block_validate
     * bounds a per-BLOCK ndocs count, but never the CUMULATIVE decoded docid
     * against the segment's actual doc count, and idmap[] is allocated with
     * exactly that count. Left unchecked, a corrupt/oversized old_docid would
     * index idmap arbitrarily far past its allocation. */
    bm25_merge_docid_validate(old_docid, st->ndocs);

    newid = st->idmap[old_docid];

    if (newid == UINT32_MAX)
    {
        st->cur_field = UINT32_MAX;     /* skipped: a trailing pos_cb must no-op */
        return;                 /* tombstoned doc: skip */
    }
    /* Thread the decoded field_id straight into the accumulator so a merged
     * segment preserves each posting's field. For a single-field source this is
     * always 0 (no RLE on the page); C6 makes the merge accumulator/NORMS fully
     * field-aware for multi-field merges. */
    bm25_accum_add_posting(st->out, st->cur_term, st->cur_termlen,
                           field_id, newid, tf);
    st->cur_field = field_id;           /* the posting merge_pos_cb targets */
}

/* Decode-boundary probe (trust-boundary review, 2026-08). bm25_merge_docid_validate
 * only fires on an already-corrupt source segment (a POST-page docid past the
 * segment's own decoded ndocs), which a regression suite cannot produce, so this
 * runs the SAME check over caller-chosen values. TEST-ONLY: a pure function of
 * its scalar arguments, no relation touched. */
PG_FUNCTION_INFO_V1(bm25_debug_merge_docid_validate);
Datum
bm25_debug_merge_docid_validate(PG_FUNCTION_ARGS)
{
    int64 old_docid = PG_GETARG_INT64(0);
    int64 ndocs     = PG_GETARG_INT64(1);

    if (old_docid < 0 || old_docid > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_merge_docid_validate: old_docid out of uint32 range")));
    if (ndocs < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_merge_docid_validate: ndocs must be non-negative")));

    bm25_merge_docid_validate((uint32) old_docid, (uint64) ndocs);
    PG_RETURN_INT64(old_docid);
}

/* M4: the reader fires this immediately after merge_post_cb for the same posting
 * when the posting's field bears positions. Attach the replayed positions to the
 * posting merge_post_cb just added (bm25_accum_add_positions_to_last enforces the
 * D2 npos==tf invariant). A tombstoned/skipped posting left cur_field UINT32_MAX,
 * so its trailing pos_cb is dropped -- the doc is not in the merged accumulator. */
static void
merge_pos_cb(uint32 local_docid, uint32 field_id, const uint32 *positions,
             uint32 npos, void *state)
{
    MergeFeedState *st = (MergeFeedState *) state;

    (void) local_docid;         /* the target posting is fixed by merge_post_cb */
    if (st->cur_field == UINT32_MAX)
        return;                 /* the paired post_cb skipped a tombstoned doc */
    Assert(field_id == st->cur_field);   /* checked: add_positions_to_last ereports it */
    bm25_accum_add_positions_to_last(st->out, st->cur_term, st->cur_termlen,
                                     field_id, positions, npos);
}

/* MergeConfig -- the index-wide facts a merge accumulator has to be re-initialized
 * with, read ONCE per merge instead of per chunk.
 *
 * Every chunk's fresh accumulator must get the same store_positions gate and the
 * same keymeta, and both are silent when wrong: an accumulator without the keymeta
 * writes no KEYMAP, so that chunk's rows revert from key identity to ctid, and one
 * with a NULL positions gate makes the builder emit POS frames against sources that
 * carry none and desync the D2 tf-count == tf lockstep at read time. Carrying them
 * in a struct filled by one function is what keeps the chunk re-init from drifting.
 * It also feeds the byte estimator, which needs field_count, whether positions are
 * stored, and the key width. */
typedef struct MergeConfig
{
    uint32  field_count;
    uint8   store_pos[BM25_MAX_FIELDS];             /* accumulator gate (0/1 per field) */
    bool    field_store_positions[BM25_MAX_FIELDS]; /* reader gate, same bits as bool */
    bool    any_positions;
    uint8   km_type;
    uint16  km_size;
    /* Whether this index's BM25SegCatEntry.total_tokens may be believed (ADR 0088).
     * Read from the metapage's feature_flags, not derived from the entries: the field
     * occupies what used to be padding, and on an index whose catalog was ever written
     * by a pre-ADR-0074 binary those bytes can hold stack residue. Only a fresh build
     * stamps the bit, so it certifies the WRITER rather than the content. */
    bool    trust_segcat_tokens;
} MergeConfig;

static void
bm25_merge_config_read(Relation index, const BM25MetaPageData *meta,
                       const BM25SegCatEntry *segs, int nsegs, MergeConfig *cfg)
{
    uint32 f;

    memset(cfg, 0, sizeof(*cfg));
    cfg->field_count = meta->field_count;
    cfg->km_type     = BM25_KEY_NONE;
    cfg->trust_segcat_tokens =
        (meta->feature_flags & BM25_FEAT_SEGCAT_TOKENS) != 0;

    /* M4 (C-POS-MERGE): the merge REPLAYS positions so a merged segment keeps its
     * phrase-searchability. store_positions is a per-INDEX config identical for the
     * source and merged segments, so read the per-field bits off the field-config
     * page (D12 trailing flag array) -- the SAME pattern bm25_pending_drain uses.
     *
     * Back-compat (D12): an ABSENT flag array means the field-config PREDATES
     * positions (an M5-built index read by an M4 binary without REINDEX, or the
     * no-config single-field legacy shape) -- treat it as positions OFF. So we
     * pre-init store_pos all-0 and only read real bits when the flag array is
     * present; an M4-built page overwrites the 0s with its true bits. Critically,
     * bm25_accum_set_store_positions is then called UNCONDITIONALLY on every chunk
     * accumulator (even when there is no field-config page): a NULL gate on the
     * accumulator is treated as all-ON, which would make the builder set
     * any_positions=true and enter the POS-emit loop against sources whose pos_root
     * is Invalid -- no pos_cb ever fires, every posting decodes npos=0, and the
     * tf-count==tf lockstep (D2) desyncs. Passing an explicit all-0 gate keeps a
     * legacy/absent source position-less (no frames), so the merged segment stays
     * position-less too, matching D13. */
    if (meta->field_config_blkno != InvalidBlockNumber)
    {
        BM25FieldConfigHeader   fchdr;
        BM25FieldConfig         fcfg[BM25_MAX_FIELDS];

        bm25_fieldcfg_read(index, meta->field_config_blkno, &fchdr, fcfg,
                           cfg->store_pos);
    }
    for (f = 0; f < BM25_MAX_FIELDS; f++)
    {
        cfg->field_store_positions[f] = (cfg->store_pos[f] != 0);
        if (cfg->field_store_positions[f])
            cfg->any_positions = true;
    }

    /* M5 key_field (R10): if the index is keyed, every merged chunk MUST rebuild a
     * KEYMAP or the first VACUUM-cadence merge silently reverts results from key
     * back to ctid. The key config is index-wide, so ANY live segment is
     * representative -- segs[0] here, which also lets this run before selection has
     * chosen anything. Keyless index => keymeta stays NONE and every chunk's
     * set_keymeta is a no-op. */
    if (nsegs > 0)
    {
        BM25SegmentHeader h0;

        bm25_seg_header_read(index, segs[0].header_blkno, segs[0].gen, &h0);
        (void) bm25_seg_keymeta(index, &h0, &cfg->km_type, &cfg->km_size);
    }
}

/* A fresh chunk accumulator, configured identically to every other chunk's. */
static BM25Accum *
bm25_merge_accum_new(const MergeConfig *cfg)
{
    BM25Accum *acc = bm25_accum_begin_multi(cfg->field_count);

    bm25_accum_set_store_positions(acc, cfg->store_pos);
    if (cfg->km_type != BM25_KEY_NONE)
        bm25_accum_set_keymeta(acc, cfg->km_type, cfg->km_size);
    return acc;
}

/* Seal one chunk accumulator into orphan pages and record its catalog entry.
 * An accumulator holding no live docs writes nothing and contributes no entry --
 * the pure-DROP degenerate case (every input doc tombstoned) that must not
 * publish a 0-doc segment. */
static void
bm25_merge_flush_chunk(Relation index, Relation heaprel, BM25Accum *acc,
                       BM25SegCatEntry *entries, int *nent)
{
    BM25SegmentHeader hdr;
    BlockNumber       header_blk;

    header_blk = bm25_segment_build_orphans(index, heaprel, acc, &hdr);
    if (header_blk != InvalidBlockNumber)
        bm25_segcat_entry_from_hdr(&entries[(*nent)++], header_blk, &hdr);
}

/* bm25_accum_from_segments -- re-accumulate the LIVE docs of `chosen[0..nchosen)`,
 * remapping to dense ascending local doc-ids (segment-then-old-id order) and
 * replaying per-field doclens, keys, and positions exactly, then seal the result
 * as ORPHAN segments. Writes one BM25SegCatEntry per output into `entries` and
 * returns how many; the caller publishes them all in one swap record.
 *
 * Factored out of bm25_merge_execute so the bm25_upgrade segment-rewrite path
 * (bm25_merge_rewrite_all) reuses the identical, subtle accumulation instead of
 * duplicating it. Every SEGMENT page it writes is an orphan until the caller's
 * record -- but it is NOT true that it touches nothing live, and the distinction
 * matters to anyone reasoning about the swap's atomicity: bm25_segment_build_orphans
 * calls bm25_next_gen, which takes the metapage EXCLUSIVE and commits its own record
 * bumping next_gen, once per CHUNK now rather than once per merge. That is required,
 * not incidental -- each output needs a distinct gen, or a later swap matching one
 * gen in drop_gens would retire two segments -- and it is durable independently of
 * whether the publish ever happens, which is exactly the property gen monotonicity
 * needs.
 *
 * ONE output unless `budget` is crossed, in which case the accumulator is sealed
 * and restarted at the next INPUT-SEGMENT boundary -- see the long note at the cut
 * for why the boundary cannot be finer, and why the outputs must nevertheless be
 * published together. `entries` must have room for nchosen of them, which is the
 * hard maximum (at most one cut per input segment, and the last cannot cut).
 *
 * nchosen == 0 accumulates nothing and returns 0. */
static int
bm25_accum_from_segments(Relation index, Relation heaprel, const MergeConfig *cfg,
                         const BM25SegCatEntry *chosen, int nchosen,
                         Size budget, BM25SegCatEntry *entries)
{
    BM25Accum  *acc;
    uint32      field_count = cfg->field_count;
    const bool *field_store_positions = cfg->field_store_positions;
    uint32      i;
    uint32      f;
    int         nent = 0;
    int         segs_in_chunk = 0;
    bool        over;

    acc = bm25_merge_accum_new(cfg);

    /* For each chosen segment, build old->new idmap over LIVE docs, copy TIDs +
     * per-field doclens, then stream every term's postings into the accumulator. */
    for (i = 0; i < (uint32) nchosen; i++)
    {
        BM25SegmentHeader h;
        uint32           *idmap;
        uint64            d;
        BM25SegReader     rdr;      /* H16: forward cursors over h's dense chains */
        MergeFeedState    st;
        char             *term;
        int               termlen;
        BlockNumber       post_root;
        uint16            post_off;
        uint32            df;
        BlockNumber       pos_post_root;
        uint16            pos_post_off;
        void             *dictiter;
        /* Fix (2026-08, crash/replica-safety pass): caller-owned copy of the
         * current term, filled before bm25_seg_dict_iter_unlock each iteration
         * -- see that function's header comment (bm25_seg_dict.c). Sized to
         * BLCKSZ: bm25_dictentry_validate (inside the iterator) already bounds
         * termlen to fit within one page's content area. */
        char              termbuf[BLCKSZ];

        bm25_seg_header_read(index, chosen[i].header_blkno, chosen[i].gen, &h);
        /* H16: this loop is the heaviest reader of the dense per-docid chains --
         * ndocs x (live + tid + field_count doclens) lookups, each of which used to
         * re-walk its chain from the root, so the replay was quadratic in segment
         * size. d ascends and one docid's NORMS cells are contiguous, so the reader's
         * cursors turn the whole loop into one forward pass per chain. */
        bm25_seg_reader_init(&rdr, index, &h);
        idmap = palloc(sizeof(uint32) * Max(h.ndocs, 1));
        /* Not in the original enumeration, but the same shape: ndocs scales with
         * corpus size (the ADR 0035 header cites a 1M-doc segment), the reader
         * cursors above make each lookup cheap, but the loop itself had no
         * interrupt point at all. */
        for (d = 0; d < h.ndocs; d++)
        {
            CHECK_FOR_INTERRUPTS();

            if (bm25_seg_reader_doc_is_live(&rdr, (uint32) d))
            {
                ItemPointerData tid = bm25_seg_reader_docid_to_tid(&rdr, (uint32) d);
                uint32 dlbf[BM25_MAX_FIELDS];
                uint32 newid;
                /* Read the doc's per-field doclens from the source segment's packed
                 * per-field NORMS so the merged segment reproduces them exactly.
                 * Single-field collapses to one cell (dlbf[0]). */
                for (f = 0; f < field_count; f++)
                    dlbf[f] = bm25_seg_reader_doclen_field(&rdr, (uint32) d, f);
                newid = bm25_accum_add_doc_blank(acc, &tid, dlbf, field_count);
                idmap[d] = newid;
                /* M5 key_field (R10): carry the source doc's key into the merged
                 * segment's KEYMAP, through rdr's KEYMAP cursor (issue #225): d ascends,
                 * so this is one forward pass over the source's KEYMAP chain where the
                 * one-shot bm25_seg_key re-walked it from the root for every document.
                 * bm25_seg_reader_key returns false only when the SOURCE
                 * segment itself has no keymap at all (keyless); for a keyed source it
                 * returns every docid's stored key bytes, including the zero sentinel a
                 * NULL-key row carries -- bm25_accum_set_doc_key below then bakes that
                 * in as key 0 in the merged segment, NOT a ctid fallback (same
                 * reasoning as bm25_build.c's key_field handling).
                 *
                 * Ctid fallback survives the merge in exactly ONE case: acc itself is
                 * keyless, so the guard below skips this entirely and the output
                 * segment is written with keymap_root Invalid. A keyed acc over a
                 * KEYLESS source is not a second case -- the false return would only
                 * leave this doc's zero sentinel untouched, and a keyed acc seals a
                 * real KEYMAP, so the doc would read back as key 0, never as ctid.
                 * It should not arise at all, either: key config is index-wide (acc
                 * takes it from segs[0] in bm25_merge_config_read) and
                 * bm25_validate_key_config_for_insert rejects any row whose resolved
                 * config disagrees with segs[0], in BOTH directions including to and
                 * from keyless, so no current binary can make the segment set
                 * heterogeneous. An index already mixed by a binary predating that
                 * check is REINDEX territory, as that check's own hint says. */
                if (bm25_accum_key_type(acc) != BM25_KEY_NONE)
                {
                    unsigned char kbuf[BM25_KEY_MAX_SIZE];
                    uint16        ksz;
                    if (bm25_seg_reader_key(&rdr, (uint32) d, kbuf, &ksz))
                        bm25_accum_set_doc_key(acc, newid, kbuf, ksz);
                }
            }
            else
                idmap[d] = UINT32_MAX;
        }

        st.out = acc;
        st.idmap = idmap;
        st.cur_term = NULL;
        st.cur_termlen = 0;
        st.cur_field = UINT32_MAX;
        st.ndocs = h.ndocs;

        /* Iterate every dict term in this segment, streaming its postings. The
         * source segment is still in the live catalog here, and the caller holds the
         * metapage singleton, so decoding at expected_gen = 0 would be sound; we pass
         * h.gen anyway (it is in hand) as defense-in-depth -- it restores the option-(d)
         * safety net so a page reclaimed and reused out from under us is caught rather
         * than silently mis-decoded. */
        dictiter = bm25_seg_dict_iter_begin(index, &h);
        /* bm25_seg_dict_iter_next holds its current DICT page SHARE-locked
         * across every entry on that page (it only releases between pages,
         * inside its own fresh-page branch), so a check right at the top of
         * this loop would be dead most of the time -- LWLockAcquire holds off
         * interrupts for as long as the lock is held (caught in review).
         * bm25_seg_dict_iter_next itself checks right before it acquires each
         * new page, a genuine lock-free instant.
         *
         * (Issue #145 corrected what that per-page check covers, and it is now
         * this loop's alone. It never covered the debug SRFs -- they open-code
         * their own DICT walk and have never used this iterator -- and the
         * wildcard expander, formerly the only other caller, was moved onto a
         * per-DICT-page copy because the unlock/relock pair below is unsound for
         * a reader on a hot standby without feedback. This merge feed is the
         * iterator's sole remaining caller; see bm25_seg_dict_iter_unlock's
         * header before adding another.)
         *
         * This loop gets a SECOND, per-TERM check below (fix 5, adversarial
         * review): bm25_seg_dict_iter_unlock drops the DICT lock before each
         * term's postings replay, which is itself a genuine lock-free instant
         * this loop didn't used to have, so a large merge is now cancellable
         * per-term instead of only per-DICT-page (ADR 0041). */
        while (bm25_seg_dict_iter_next(dictiter, &term, &termlen,
                                       &post_root, &post_off, &df,
                                       &pos_post_root, &pos_post_off))
        {
            /* MINIMAL fix (2026-08, crash/replica-safety pass; full 3-file
             * restructure of bm25_seg_scan_postings' own contract is out of
             * scope -- see bm25_seg_dict_iter_unlock's header comment in
             * bm25_seg_dict.c for the complete reasoning and safety argument).
             * `term` points into the DICT iterator's currently SHARE-locked
             * page; copy it out BEFORE dropping that lock, since termbuf is
             * what the postings replay below (and the accumulator underneath
             * it) reads once the page is unlocked. termlen is already bounded
             * < BLCKSZ by bm25_dictentry_validate inside the iterator. */
            memcpy(termbuf, term, (size_t) termlen);
            st.cur_term = termbuf;
            st.cur_termlen = termlen;

            /* Drop the DICT page's lock before the postings replay, which opens
             * its OWN POST/POS locks and runs the accumulator callback
             * (allocates) nested underneath them -- without this, that whole
             * nested operation ran with the DICT page ALSO still locked, for
             * every term on the page. Re-acquire immediately after so the next
             * bm25_seg_dict_iter_next call finds its expected locked state.
             *
             * Update (issue #139): the "nested underneath them" half of that no
             * longer holds -- bm25_seg_scan_postings now copies each POST page
             * (and each POS page) and unlocks BEFORE decoding, so the callback
             * runs with nothing locked. This unlock/relock pair stays anyway:
             * the DICT page would otherwise be held across the whole replay, and
             * the lock-free instant it opens is where this loop's per-term
             * interrupt check lives. */
            bm25_seg_dict_iter_unlock(dictiter);
            /* Genuine lock-free instant (fix 5): the DICT page is unlocked and
             * the POST/POS locks below haven't been taken yet. Placed here,
             * not before _unlock, so it never fires while any content lock is
             * held (ADR 0041's dead-check lesson). */
            CHECK_FOR_INTERRUPTS();
            /* M4 (C-POS-MERGE): replay positions. The reader opens the term's POS
             * cursor at (pos_post_root, pos_post_off) and -- for each posting whose
             * field bears positions (field_store_positions gate) -- fires merge_pos_cb
             * right after merge_post_cb, so the position list attaches to the posting
             * just added. A source with no positions has pos_post_root Invalid, so the
             * reader opens no cursor and merge_pos_cb never fires (the merged segment
             * stays position-less, per D13). The gate keys on the posting's decoded
             * field_id and MUST match the accumulator's store_positions so writer and
             * reader agree per field. */
            bm25_seg_scan_postings(index, post_root, post_off, df, h.gen,
                                   merge_post_cb, &st,
                                   pos_post_root, pos_post_off,
                                   merge_pos_cb, &st,
                                   field_store_positions, field_count);
            bm25_seg_dict_iter_relock(dictiter);
        }
        bm25_seg_dict_iter_end(dictiter);
        pfree(idmap);
        segs_in_chunk++;

        /* NOT PART OF THE BUDGET, stated so the ceiling is not over-claimed: idmap
         * is 4 bytes per SOURCE-segment doc and lives in the caller's context, not
         * the accumulator's, so bm25_accum_over_budget never sees it. Its lifetime
         * is one input segment (freed just above), and post-fix a segment's own
         * ndocs is itself budget-bounded, so in the steady state it is a small
         * fraction of the budget. It is only material for the legacy oversized
         * segment of the overshoot case below -- where, note, the accumulator is not
         * cut either, because a one-input merge has nothing to cut before. */

        /*
         * BUILD-04: the chunk boundary. INPUT-SEGMENT GRANULAR -- never mid-segment.
         *
         * WHY THE CUT MUST RESPECT SEGMENT BOUNDARIES. The loop above processes one
         * chosen segment completely before the next: it registers ALL that segment's
         * live docs, then streams ALL its terms' postings. The postings stream
         * PER TERM ACROSS DOCS, so a cut in the middle leaves every doc of that
         * segment holding postings for terms A..K and none for L..Z; completing it
         * in the next chunk would register the same TID in a second output segment,
         * and the exhaustive scorer sums per-TID contributions across segments with
         * no dedup -- a permanent double-score and a double-return on the membership
         * path. A doc-range window is expressible (the idmap already skips docs) but
         * costs a full dict+postings replay per window; refused, see ADR 0084.
         *
         * NO DOC IS DUPLICATED: a cut here happens only between two source segments,
         * and every doc of segments 0..i is fully accumulated into this chunk and
         * never touched again -- the next chunk starts at segment i+1. NO DOC IS
         * LOST: the loop's final chunk is flushed below unconditionally, so the
         * union of the chunks is exactly the live docs of the chosen set. NOTHING
         * OBSERVES A PARTIAL STATE: these are orphan pages until the caller's single
         * swap record publishes every entry and retires every input together.
         *
         */
        over = bm25_accum_over_budget(acc, budget);

        /* A single input segment that alone exceeds the budget is an ACCEPTED
         * OVERSHOOT -- it cannot be split (above), and erroring out of
         * amvacuumcleanup would leave the index permanently un-maintained, which is
         * the disease and not the cure. It is reachable for a segment built before
         * the budget existed, under a since-lowered maintenance_work_mem, or as the
         * lone tombstone rewrite the selection trim deliberately exempts; REINDEX
         * re-buckets it. Warned about SEPARATELY from the cut below, because the
         * commonest instance of it -- that lone tombstone pick -- is a one-input
         * merge, where there is no next segment to cut before. Counts go in
         * errdetail so `\set VERBOSITY terse` keeps regression output stable.
         *
         * TWICE the budget, not merely over it, and the factor is load-bearing.
         * EVERY segment the chunker produces is by construction one whose replay
         * lands just past the budget -- that is why it was cut there -- so warning
         * on plain `over` would fire on the ordinary steady state, once per
         * tombstone rewrite, into the autovacuum log, with a hint telling the
         * operator to REINDEX a segment that is already exactly the size REINDEX
         * would produce. The threshold separates "this index is at its budget
         * floor", which is normal and is documented in ARCHITECTURE.md as a
         * consequence rather than a fault, from "one segment here predates the
         * budget", which is the actionable case.
         *
         * KNOWN GAP, stated rather than papered over: a chunk of {small, huge} has
         * segs_in_chunk == 2 and is not warned about even though the huge one is a
         * genuine overshoot. Widening the test would cost the message its ability to
         * name the segment, and the overshoot is reported by the memory it uses
         * either way. */
        if (segs_in_chunk == 1 &&
            budget <= SIZE_MAX / 2 && bm25_accum_over_budget(acc, budget * 2))
            ereport(WARNING,
                    (errmsg("bm25: a single segment of index \"%s\" exceeds the "
                            "maintenance memory budget during merge",
                            RelationGetRelationName(index)),
                     errdetail("Segment generation %u holds " UINT64_FORMAT
                               " live documents; the budget is " UINT64_FORMAT
                               " bytes.",
                               chosen[i].gen, chosen[i].live_ndocs,
                               (uint64) budget),
                     errhint("This segment predates the current budget: REINDEX to "
                             "rebuild the index in budget-sized segments, or raise "
                             "maintenance_work_mem.")));

        /* Not on the LAST chosen segment: the tail flush below covers it, and
         * cutting there would emit an empty final chunk. */
        if (over && i + 1 < (uint32) nchosen)
        {
            bm25_merge_flush_chunk(index, heaprel, acc, entries, &nent);
            bm25_accum_free(acc);
            acc = bm25_merge_accum_new(cfg);
            segs_in_chunk = 0;
        }
    }

    bm25_merge_flush_chunk(index, heaprel, acc, entries, &nent);
    bm25_accum_free(acc);
    return nent;
}

/*
 * bm25_merge_predicted_chunks -- how many output segments the chosen set is
 * predicted to produce, by simulating the chunker's own greedy sequential packing
 * over the per-segment estimates. Mirrors bm25_accum_from_segments' cut condition
 * exactly -- add a segment, and if the running total is over budget and another
 * chosen segment follows, cut -- because a total that is merely wrong is absorbed
 * by the real budget check at run time, whereas a MODEL that disagreed with the
 * loop it predicts would not be. Hence a simulation and not a ceil(total/budget)
 * shortcut.
 *
 * It counts CHUNKS, which is an upper bound on output SEGMENTS: a chunk whose docs
 * were all tombstoned writes no pages and contributes no catalog entry. Erring high
 * makes the trim more conservative, which is the safe direction -- the cost is an
 * occasional refused merge, never a wrong answer.
 */
static int
bm25_merge_predicted_chunks(const bool *chosen, const uint64 *est, uint32 nsegs,
                            uint64 budget)
{
    int     chunks = 1;
    uint64  cur = 0;
    uint32  i;
    int     seen = 0,
            nchosen = 0;

    for (i = 0; i < nsegs; i++)
        if (chosen[i])
            nchosen++;
    if (nchosen == 0)
        return 0;
    if (budget == 0)
        return 1;               /* no budget: one accumulator, as before */

    for (i = 0; i < nsegs; i++)
    {
        if (!chosen[i])
            continue;
        cur += est[i];
        seen++;
        if (cur > budget && seen < nchosen)
        {
            chunks++;
            cur = 0;
        }
    }
    return chunks;
}

/*
 * bm25_merge_trim_to_budget -- drop chosen segments until merging them can actually
 * REDUCE the segment count under the budget. Returns the surviving count.
 *
 * WHY THIS IS REQUIRED AND NOT AN OPTIMIZATION. With outputs capped at the budget,
 * merging a rung of four budget-sized segments yields four budget-sized outputs --
 * the same layer population. bm25_merge_select chooses them again on the next pass,
 * and bm25_merge_maybe(force) loops "until a pass finds nothing to merge", so the
 * manual bm25_merge() would never return and every autovacuum would burn a full
 * rewrite for nothing. This is the brace; the executor's progress check is the belt,
 * and it is what makes termination independent of the estimate's quality.
 *
 * Largest-estimate-first, because that is the segment most likely to be a chunk of
 * its own -- dropping it is what turns an infeasible set into a feasible one with
 * the fewest removals. But TOMBSTONE-TRIGGER SEGMENTS ARE SHED LAST, whatever their
 * estimate: a tombstoned segment is the one pick that makes progress on its own, so
 * spending it to make some other rung feasible trades the only reclaimable work in
 * the set for none. Without that ordering the exemption below is unreachable in the
 * case that needs it most -- a heavily-tombstoned oversized segment bundled with a
 * permanently budget-floored rung is also the LARGEST estimate, so it was shed on
 * the first pass, the rung was then whittled to a lone non-tombstone survivor, and
 * the trim returned 0. Every pass. The giant's dead space was never reclaimed while
 * that rung persisted, which is precisely what the paragraph below promises cannot
 * happen.
 *
 * THE LAST ONE STANDING IS KEPT ONLY IF IT IS THE TOMBSTONE REWRITE, which is
 * bm25_merge_select's own final rule, restated because the trim can arrive at a
 * single-segment set that the selector never would. A lone tombstoned segment makes
 * progress by dropping dead docs no matter how large it is, so trimming it away
 * would leave a heavily-tombstoned oversized segment permanently un-rewritten --
 * exactly what the tombstone trigger exists to prevent. The merge overshoots the
 * budget for it instead, with the WARNING bm25_accum_from_segments raises. Any
 * OTHER lone survivor would be a 1-input-1-output rewrite: pure cost, no progress,
 * and the executor would report no progress anyway, so it is refused here.
 *
 * MUTATES `chosen` IN PLACE -- that is its output, not a side effect; the return
 * value is only the resulting count. No I/O: it decides from (segs, chosen, est,
 * budget) alone, which is what keeps it unit-testable through
 * bm25_debug_merge_budget_plan the way bm25_merge_select is through
 * bm25_debug_merge_plan.
 */
static int
bm25_merge_trim_to_budget(const BM25SegCatEntry *segs, bool *chosen,
                          const uint64 *est, uint32 nsegs, uint64 budget)
{
    for (;;)
    {
        uint32  i;
        int     nchosen = 0;
        uint32  worst = 0;
        uint32  only = 0;
        uint64  worst_est = 0;
        bool    have_worst = false;
        bool    worst_is_tomb = false;

        for (i = 0; i < nsegs; i++)
            if (chosen[i])
            {
                bool tomb = bm25_merge_tombstone_trigger(&segs[i]);

                nchosen++;
                only = i;

                /* Rank non-tombstone ahead of tombstone first, and only then by
                 * estimate. A tombstone pick is shed only when nothing else is
                 * left to shed. */
                if (!have_worst ||
                    (worst_is_tomb && !tomb) ||
                    (worst_is_tomb == tomb && est[i] > worst_est))
                {
                    worst = i;
                    worst_est = est[i];
                    worst_is_tomb = tomb;
                    have_worst = true;
                }
            }

        if (nchosen == 0 || budget == 0)
            return nchosen;
        if (nchosen == 1)
        {
            if (bm25_merge_tombstone_trigger(&segs[only]))
                return 1;
            chosen[only] = false;
            return 0;
        }
        if (bm25_merge_predicted_chunks(chosen, est, nsegs, budget) < nchosen)
            return nchosen;

        chosen[worst] = false;  /* infeasible: shed the biggest and re-test */
    }
}

/* Per-entry accumulator-residency estimates for the whole catalog. Kept separate
 * from the trim so the debug SRF can report the estimates alongside the decision. */
static void
bm25_merge_estimates(const BM25SegCatEntry *segs, uint32 nsegs,
                     const MergeConfig *cfg, uint64 *est)
{
    uint32 i;

    for (i = 0; i < nsegs; i++)
        est[i] = bm25_accum_estimate_bytes(segs[i].ndocs, segs[i].live_ndocs,
                                           segs[i].total_len,
                                           /* 0 means "no trustworthy count": the
                                            * estimator then falls back to total_len and
                                            * reproduces its pre-ADR-0088 arithmetic
                                            * exactly, which is what an index built
                                            * before this change must keep getting. */
                                           cfg->trust_segcat_tokens
                                               ? (uint64) segs[i].total_tokens : 0,
                                           segs[i].nterms,
                                           cfg->field_count, cfg->any_positions,
                                           cfg->km_size);
}

/* bm25_debug_merge_budget_plan -- bm25_debug_merge_plan's budget-aware sibling: one
 * row per catalog entry with (gen, est_bytes, chosen, kept), where `chosen` is
 * bm25_merge_select's raw pick and `kept` is what survives the budget trim at the
 * CURRENT bm25_maintenance_budget_bytes. predicted_chunks reports what the chunker
 * is expected to emit for the kept set.
 *
 * A NEW SRF rather than extra columns on bm25_debug_merge_plan: widening that row
 * type would churn sql/19 and sql/20's expected files for a question they do not
 * ask. Mirrors its Materialize-mode tuplestore idiom exactly (C7: switch to
 * ecxt_per_query_memory before tuplestore_begin_heap).
 *
 * est_bytes is a sum of sizeof()s, so its VALUE is platform-dependent -- assert
 * relationships over it, never a number. */
PG_FUNCTION_INFO_V1(bm25_debug_merge_budget_plan);
Datum
bm25_debug_merge_budget_plan(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid              relid = PG_GETARG_OID(0);
    Relation         index;
    BM25SegCatEntry *segs = NULL;
    uint32           nsegs = 0;
    bool            *chosen;
    bool            *kept;
    uint64          *est;
    MergeConfig      cfg;
    BM25MetaPageData meta;
    Tuplestorestate *ts;
    TupleDesc        tupdesc;
    uint64           budget;
    int              predicted;
    uint32           i;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    index = bm25_index_open_readable(relid, AccessShareLock);
    tupdesc = BlessTupleDesc(tupdesc);
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    bm25_meta_read(index, &meta);
    bm25_segcat_read(index, &segs, &nsegs);
    chosen = (bool *) palloc(sizeof(bool) * Max(nsegs, 1));
    kept   = (bool *) palloc(sizeof(bool) * Max(nsegs, 1));
    est    = (uint64 *) palloc0(sizeof(uint64) * Max(nsegs, 1));

    bm25_merge_config_read(index, &meta, segs, (int) nsegs, &cfg);
    bm25_merge_estimates(segs, nsegs, &cfg, est);
    (void) bm25_merge_select(segs, nsegs, chosen);
    memcpy(kept, chosen, sizeof(bool) * Max(nsegs, 1));
    budget = (uint64) bm25_maintenance_budget_bytes();
    (void) bm25_merge_trim_to_budget(segs, kept, est, nsegs, budget);
    predicted = bm25_merge_predicted_chunks(kept, est, nsegs, budget);

    for (i = 0; i < nsegs; i++)
    {
        Datum   vals[5];
        bool    nulls[5] = {false, false, false, false, false};

        vals[0] = Int32GetDatum((int32) segs[i].gen);
        vals[1] = Int64GetDatum((int64) est[i]);
        vals[2] = BoolGetDatum(chosen[i]);
        vals[3] = BoolGetDatum(kept[i]);
        vals[4] = Int32GetDatum(predicted);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/*
 * bm25_merge_execute -- one merge pass. Returns whether it made PROGRESS, which is
 * not the same as whether it merged anything.
 *
 * The distinction is what makes bm25_merge_maybe(force)'s drain loop terminate now
 * that a merge can produce several outputs. A pass that consumed N inputs and
 * emitted N outputs of the same shapes has changed nothing the selector can see, so
 * the next pass would choose the same rung forever. Progress means either the
 * segment count actually fell, or dead documents were dropped -- and the latter is
 * knowable before the merge runs, from the chosen entries' own ndocs vs live_ndocs.
 * Belt to the selection trim's braces: even with a badly wrong estimate, the loop
 * stops.
 */
bool
bm25_merge_execute(Relation index, Relation heaprel)
{
    BM25SegCatEntry *segs = NULL;
    uint32           nsegs = 0;
    bool            *chosen;
    int              nchosen;
    uint64          *est;
    BM25SegCatEntry *entries;
    int              nentries;
    uint32          *remove_gens;
    int              nremove = 0;
    BM25SegCatEntry *chosen_copy;
    uint32           i;
    BM25MetaPageData meta;
    MergeConfig      cfg;
    uint64           chosen_ndocs = 0;
    uint64           chosen_live = 0;
    Size             budget;
    bool             progress;

    bm25_meta_read(index, &meta);
    bm25_segcat_read_locked(index, &segs, &nsegs);
    chosen = (bool *) palloc(sizeof(bool) * Max(nsegs, 1));
    nchosen = bm25_merge_select(segs, nsegs, chosen);
    if (nchosen == 0)
    {
        pfree(chosen);
        if (segs != NULL)
            pfree(segs);
        return false;           /* nothing to merge */
    }

    /* Index-wide config, read once: it re-initializes every chunk's accumulator AND
     * feeds the byte estimator. Deliberately AFTER the selection, so the common
     * "nothing to merge" pass above costs no segment-header read. */
    bm25_merge_config_read(index, &meta, segs, (int) nsegs, &cfg);

    /* BUILD-04: keep only a set whose merge can actually reduce the segment count
     * under the budget -- otherwise the force loop below re-selects the same rung
     * forever. See bm25_merge_trim_to_budget. */
    est = (uint64 *) palloc0(sizeof(uint64) * Max(nsegs, 1));
    bm25_merge_estimates(segs, nsegs, &cfg, est);
    /* ONE read for the whole operation, per the doctrine in bm25.h and as the build
     * and seal paths already do: the trim decides which set is feasible and the cut
     * decides where that set breaks, so judging them against two different numbers
     * would be a contradiction waiting for a config reload to expose it. */
    budget  = bm25_maintenance_budget_bytes();
    nchosen = bm25_merge_trim_to_budget(segs, chosen, est, nsegs, (uint64) budget);
    if (nchosen == 0)
    {
        pfree(est);
        pfree(chosen);
        if (segs != NULL)
            pfree(segs);
        return false;
    }

    /* Snapshot the chosen entries. The swap re-snapshots the catalog and matches
     * them by gen; the singleton ExclusiveLock held from this selection through the
     * swap keeps every one of them in the catalog, so the swap treats a gen it cannot
     * find as an internal error (bm25_segcat_publish_swap, issue #302.G). */
    remove_gens  = palloc(sizeof(uint32) * nchosen);
    chosen_copy  = palloc(sizeof(BM25SegCatEntry) * nchosen);
    for (i = 0; i < nsegs; i++)
        if (chosen[i])
        {
            remove_gens[nremove]  = segs[i].gen;
            chosen_copy[nremove]  = segs[i];
            chosen_ndocs         += segs[i].ndocs;
            chosen_live          += segs[i].live_ndocs;
            nremove++;
        }

    /* Re-accumulate the chosen segments' LIVE docs (per-field doclens, keys, and
     * positions replayed exactly) and seal them as orphan segments -- one, or
     * several if the budget is crossed at an input-segment boundary. At most one
     * output per input, so nremove slots is the hard maximum. */
    entries  = palloc(sizeof(BM25SegCatEntry) * nremove);
    /* Orphan bracket (issue #300), opened here -- after both `return false` exits
     * above, so a pass that merges nothing leaves no evidence -- and BEFORE the
     * accumulate's first page allocation. Deliberately never closed: even a merge
     * that succeeds leaves orphans, because the swap below rebuilds the catalog as a
     * fresh chain and orphans the old one (bm25_segcat_publish_swap), and VACUUM's
     * orphan sweep is the only thing that frees it. The open bracket is what makes
     * that gated sweep run; cleanup runs it right after its own merge. */
    bm25_orphan_op_begin(index);
    nentries = bm25_accum_from_segments(index, heaprel, &cfg, chosen_copy, nremove,
                                        budget, entries);

    /* Phase 2: ONE record publishes every output AND retires every input. heaprel
     * (pre-opened by bm25_merge_maybe before any buffer lock) is threaded down to
     * bm25_page_alloc (D-ALLOC/M6). R4: retirement is crash-atomic with the swap --
     * there is no post-swap retire loop. The swap re-snapshots its own catalog and
     * drops the entries whose gen is in remove_gens, appending one retired-list
     * RANGE entry per dropped segment in the SAME record that flips segcat_root.
     * NULL restamp: a merge never changes the on-disk format version.
     *
     * All N outputs in this one record, never one record each -- the intermediate
     * catalog states of the N-record shape are crash-durably wrong, not transient.
     * See bm25_segcat_publish_swap. */
    bm25_debug_pause_point("merge_preswap");
    bm25_segcat_publish_swap(index, heaprel, entries, nentries,
                             remove_gens, nremove, NULL);

    progress = (nentries < nremove) || (chosen_ndocs > chosen_live);

    pfree(entries);
    pfree(est);
    pfree(remove_gens);
    pfree(chosen_copy);
    pfree(chosen);
    pfree(segs);
    return progress;
}

/* bm25_merge_rewrite_all -- the bm25_upgrade segment-rewrite publish path. Re-emit
 * EVERY live segment through the same accumulate + atomic-swap machinery a merge
 * uses, but fold `restamp` into the swap's single catalog-flip WAL record so the
 * metapage version re-stamp is crash-atomic with the publish (a crash can never
 * expose the freshly re-emitted new-format segments under an old-format version).
 *
 * It takes the per-index seal/merge singleton (LockPage, blocking -- bm25_upgrade
 * is a deliberate maintenance op) so two catalog swaps never race on the segcat
 * snapshot, and opens the heap itself (D-ALLOC/M6: bm25_page_alloc needs it),
 * mirroring bm25_merge_maybe(force) -- heap FIRST, then the singleton, for the
 * reason given there. The re-emit is semantically identity -- every
 * live doc's tid/key/doclen/postings are replayed -- so query results are unchanged;
 * merging all segments into one only changes physical layout, not scores.
 * Returns the number of segments CONSUMED -- which is what bm25_upgrade reports, and
 * which stopped being the same as the number re-emitted when the accumulator became
 * budget-chunked: a large index is re-emitted as several segments. 0 for an empty
 * index, which still gets its version re-stamped by the swap. */
int
bm25_merge_rewrite_all(Relation index, const BM25FormatRestamp *restamp)
{
    Oid       heaprelid = IndexGetRelation(RelationGetRelid(index), false);
    Relation  heaprel;
    int       nrewritten = 0;

    heaprel = table_open(heaprelid, AccessShareLock);
    LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
    PG_TRY();
    {
        BM25SegCatEntry  *segs = NULL;
        uint32            nsegs = 0;
        BM25MetaPageData  meta;
        MergeConfig       cfg;
        BM25SegCatEntry  *entries;
        int               nentries;
        uint32           *gens;
        uint32            i;

        bm25_meta_read(index, &meta);
        bm25_segcat_read_locked(index, &segs, &nsegs);

        /* The swap retires every dropped segment as ONE range entry on a SINGLE
         * pre-acquired retired-list page, and bm25_retire_segment only Asserts room
         * (n < BM25_RETIRED_PER_PAGE) -- a caller MUST cap its drop count. The merge
         * caller is bounded by BM25_MERGE_MAX_INPUTS; this caller drops ALL segments,
         * so an index with more than one page's worth would overrun that page INSIDE
         * the swap's throw-free WAL window (Assert-abort mid-record under cassert, or
         * page corruption in production). Convert that into a clean pre-window error
         * with an operator path BEFORE opening any window. (A real large-index
         * transform will instead rewrite in batches, folding the re-stamp into only
         * the FINAL batch's swap, to lift this bound -- future work.) */
        if (nsegs > BM25_RETIRED_PER_PAGE)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bm25: bm25_upgrade cannot rewrite an index with %u segments "
                            "in one atomic step (limit %d)",
                            nsegs, (int) BM25_RETIRED_PER_PAGE),
                     errhint("Run bm25_merge() to reduce the segment count first, then re-run bm25_upgrade().")));

        /* Re-accumulate ALL live docs of ALL segments, and drop ALL their gens: the
         * swap builds fresh segments carrying every doc and retires the old ones,
         * exactly as a full merge would, plus the version re-stamp in the same
         * record. Budget-chunked like any other merge (BUILD-04), so a large index
         * is re-emitted as several segments rather than one unbounded accumulator;
         * the re-stamp still rides the single publish record, so a crash can never
         * expose the new-format segments under a stale version. NOT trimmed: this
         * caller must rewrite everything, and its own retired-list guard above is
         * what bounds it. */
        bm25_merge_config_read(index, &meta, segs, (int) nsegs, &cfg);
        entries  = palloc(sizeof(BM25SegCatEntry) * Max(nsegs, 1));
        /* Orphan bracket (issue #300), never closed, for the same reason as
         * bm25_merge_execute's: the swap orphans the old catalog chain on success. */
        bm25_orphan_op_begin(index);
        nentries = bm25_accum_from_segments(index, heaprel, &cfg, segs, (int) nsegs,
                                            bm25_maintenance_budget_bytes(), entries);
        gens = palloc(sizeof(uint32) * Max(nsegs, 1));
        for (i = 0; i < nsegs; i++)
            gens[i] = segs[i].gen;

        bm25_segcat_publish_swap(index, heaprel, entries, nentries,
                                 gens, (int) nsegs, restamp);
        nrewritten = (int) nsegs;

        pfree(entries);
        pfree(gens);
        if (segs != NULL)
            pfree(segs);
    }
    PG_FINALLY();
    {
        UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
        table_close(heaprel, AccessShareLock);
    }
    PG_END_TRY();

    return nrewritten;
}

void
bm25_merge_maybe(Relation index, bool force)
{
    /* Singleton guard: only one sealer/merger at a time (spec section 9).
     * Conditional for the opportunistic path; blocking for `force`. Inserts do NOT
     * continue while it is held: bm25_pending_append_multi takes the same lock in
     * ShareLock mode (ADR 0022), so every insert that adds a document to this index
     * waits for the whole merge pass below, forced or opportunistic. The conditional
     * acquire keeps the opportunistic path from queueing behind another holder or
     * waiter -- a merger, a sealer, VACUUM's sweep, or any in-flight insert's
     * ShareLock -- and returns instead; it does not shorten this hold once granted.
     * D-ALLOC/M6: open the heap BEFORE any buffer lock and thread it through to
     * bm25_page_alloc; table_close in PG_FINALLY.
     *
     * Lock order (issue #300, XCUT-11): heap lock FIRST, then the singleton -- the
     * order bm25_seal_index and every other singleton taker already use. The other
     * order let bm25_merge() hold the singleton while it waited for the heap:
     * bm25_merge's bm25_seal_index drops its heap lock on the way out, so the merge
     * arrives here holding none, and a DDL lock queued on the heap in between (a
     * waiting ALTER TABLE's AccessExclusiveLock) then left the merge holding the
     * singleton while it queued behind that DDL. New inserts queue behind the DDL
     * either way; the extra victims were transactions already holding their heap
     * lock that insert again, which then waited on the singleton (and formed a
     * cycle with the DDL that only the deadlock detector's soft-edge rearrangement
     * resolved). With the heap first, a merge that must wait for the heap does so
     * holding nothing that inserts need. The VACUUM caller already holds a heap
     * lock, so its open here never waits. On the conditional path a failed acquire
     * now costs a table_open/table_close pair; that is the price of the uniform
     * order. */
    Relation heaprel;
    Oid      heaprelid = IndexGetRelation(RelationGetRelid(index), false);
    volatile bool singleton_held;

    /* Test lever: no heap or singleton lock held yet (t/027's lock-order check). */
    bm25_debug_pause_point("merge_start");

    heaprel = table_open(heaprelid, AccessShareLock);
    if (force)
        LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
    else if (!ConditionalLockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock))
    {
        table_close(heaprel, AccessShareLock);
        return;                 /* held elsewhere: skip rather than queue (see above) */
    }

    /* Whether this backend holds the singleton right now: the forced loop below
     * releases it between passes, and the reclaim runs after the last release. */
    singleton_held = true;
    PG_TRY();
    {
        /* Rewritten from `while (bm25_merge_execute(...))` so the interrupt check
         * runs before EVERY pass, including the first -- a `force` drain can chain
         * several full merge passes back to back (one per ladder rung) with no
         * check between them otherwise.
         *
         * The loop condition is now PROGRESS, not "merged something" (BUILD-04).
         * Once a merge can emit as many segments as it consumed -- which it can, as
         * soon as the chosen set does not fit the memory budget -- "stop when a pass
         * finds nothing to merge" never becomes true: the selector picks the same
         * rung, the merge rewrites it into the same shapes, forever. bm25_merge_execute
         * therefore returns false when a pass changed nothing observable, and that is
         * what terminates this loop independently of how good the byte estimator is. */
        for (;;)
        {
            CHECK_FOR_INTERRUPTS();
            if (!bm25_merge_execute(index, heaprel))
                break;
            if (!force)
                break;          /* opportunistic: one pass; manual: drain to target */

            /* BOUNDED HOLD (issue #300): release the singleton between forced ladder
             * passes, so a bm25_merge() that drains several rungs blocks inserts for
             * one pass at a time, not for all of them. Safe because nothing is
             * carried across the gap: the pass just swapped (its outputs are
             * published and its inputs retired in one record), and the next pass
             * re-reads the metapage and the catalog from scratch
             * (bm25_merge_execute). A bulkdelete that takes its whole-pass ShareLock
             * in the gap is simply waited for on re-acquire, so it still never sees
             * a swap mid-pass (ADR 0102). The heap stays open, so the order is still
             * heap, then singleton (XCUT-11). The opportunistic single pass cannot be
             * bounded this way -- a pass is the unit that must not be split, and its
             * build must stay hidden from the orphan sweep (ADR 0019) -- so VACUUM's
             * merge_maybe(false) still holds the singleton for O(its inputs): the
             * residual issue #300 documents. */
            UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
            singleton_held = false;
            /* Test lever (t/032): between two forced passes, no singleton held. */
            bm25_debug_pause_point("merge_between_passes");
            LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
            singleton_held = true;
        }
        UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
        singleton_held = false;

        /* Frees retired segments whose retire_xid has cleared the horizon: compacts
         * the descriptor chain, then the pages (bm25_fsm.c, bm25_reclaim_retired).
         * After the release, not under it: bm25_reclaim_retired takes the singleton
         * itself one descriptor page at a time (issue #300), and the singleton is
         * re-entrant, so calling it under this hold would hold it across every
         * chunk. The heap is still open, so its locks are still taken heap-first. */
        bm25_reclaim_retired(index, heaprel);
    }
    PG_FINALLY();
    {
        if (singleton_held)
            UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
        table_close(heaprel, AccessShareLock);
    }
    PG_END_TRY();
}

/* ---- Task 27: bm25_merge(regclass) SQL function ---- *
 *
 * Forces a drain of the pending list into a segment AND a merge-to-target. Opens
 * the index RowExclusiveLock (it mutates it) and returns void. (bm25_seal, owned
 * by Phase 1, forces a drain + an opportunistic merge.) */
PG_FUNCTION_INFO_V1(bm25_merge_sql);
Datum
bm25_merge_sql(PG_FUNCTION_ARGS)
{
    Oid       relid = PG_GETARG_OID(0);
    /* Ownership + AM identity before any page is touched; this used to merge ANY
     * index for any role that could connect. The helper's AM check compares the
     * resolved ambuild against our own, which subsumes (and is stricter than) the
     * get_index_am_oid("bm25_native") name lookup that stood here: renaming the
     * access method cannot disarm it. */
    Relation  index = bm25_index_open_owned(relid, RowExclusiveLock);

    /* Drain pending first so the merge sees all docs as segments, then merge to
     * target (force=true drains the ladder in a loop and runs reclaim). The drain
     * reuses the Phase-1 seal PRIMITIVE bm25_seal_index (which holds the
     * drain+build+commit singleton correctly); it does NOT call the bm25_seal_sql
     * SQL wrapper. bm25_seal_index is a true no-op only when the CHAIN IS EMPTY
     * (pending_head Invalid, so drained_head is Invalid: no publish record, no
     * truncate). "No live docs recovered" is a different state and NOT a no-op --
     * a chain whose every doc VACUUM already tombstoned drains to nent == 0, which
     * still writes the anchor-detach publish record and still recycles the drained
     * pages. Either way this call is unconditional and correct; the distinction
     * matters only to a reader reasoning about what it costs. The merge then
     * acquires the same metapage singleton, so the drain has fully committed
     * before it runs. */
    bm25_seal_index(index);
    bm25_merge_maybe(index, true);   /* force: drain ladder to target + reclaim */

    index_close(index, RowExclusiveLock);
    PG_RETURN_VOID();
}
