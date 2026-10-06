/* bm25_seg_build.c -- sealed-segment builder + the varbyte codec.
 *
 * The varbyte codec (unsigned LEB128) is the pure, unit-testable base used by
 * posting blocks. Task 5 adds the block encoder, the orphan-page-chain writer
 * (ChainWriter), and the two-phase commit: build the DOCMAP/NORMS/LIVE/POST/DICT
 * chains + the segment header as ORPHAN pages (each its own <=1-buffer Generic WAL
 * record, metapage untouched), then publish with ONE final record that locks the
 * catalog page + metapage, appends the BM25SegCatEntry(s), updates global stats,
 * and resets the pending anchor. That single record is the linearization point
 * (D-SEAL): a crash can never leave a segment published while the pending list
 * still anchors the same docs.
 *
 * ONE OPERATION, ONE RECORD -- NOT one segment (BUILD-04). Since the accumulator
 * became budget-bounded, a build, seal or merge can produce SEVERAL segments, and
 * they are published together: bm25_segcat_publish_append takes an entry array, and
 * bm25_segcat_publish_swap takes an array of new entries alongside the dropped
 * gens. Publishing them one record at a time would be crash-durably wrong, not
 * merely slower -- see bm25_segcat_publish_swap's header for the double-scoring
 * argument. The per-record buffer budget is unchanged by N in both forms. */
#include "postgres.h"

#include "bm25.h"
#include "funcapi.h"            /* used by the debug roundtrip wrappers below */
#include "storage/bufmgr.h"
#include "access/transam.h"     /* ReadNextFullTransactionId (merge swap, Task 23) */
#include "catalog/pg_type_d.h"  /* INT4OID (field-RLE roundtrip SRF) */
#include "utils/array.h"        /* ArrayType / deconstruct_array_builtin (RLE SRF) */
#include "utils/memutils.h"     /* AllocSetContextCreate (BUILD-03 builder scratch) */
#include "utils/tuplestore.h"   /* tuplestore_begin_heap/putvalues -- no longer pulled
                                 * in transitively via funcapi.h as of PG19 */

/* Unsigned LEB128: low 7 bits per byte, high bit set on all but the last.
 * Returns bytes written (1..5). out must have room for 5 bytes. */
int
bm25_varbyte_encode(uint32 v, uint8 *out)
{
    int n = 0;
    do
    {
        uint8 b = (uint8) (v & 0x7F);
        v >>= 7;
        if (v != 0)
            b |= 0x80;
        out[n++] = b;
    } while (v != 0);
    return n;
}

/* Inverse of bm25_varbyte_encode. Returns bytes read (1..BM25_VARBYTE_MAX_BYTES).
 *
 * `end` is one past the last readable byte of the run being decoded. This used to
 * take no bound and its own comment conceded it "will read past the buffer on a
 * truncated/corrupt stream", justified by "the only producer is
 * bm25_varbyte_encode into segment pages" -- which assumes the pages read back are
 * the pages written. On a torn page or bit flip a tail of 0x80 bytes walked the
 * cursor forward indefinitely, and `shift` passed 31 on the fifth byte, which is
 * undefined behaviour before it is a wrong answer.
 *
 * Both are now bounded: running out of input, or a sequence wider than a uint32
 * can hold, is a corrupt page and fails loud. The encoder never emits either. */
int
bm25_varbyte_decode(const uint8 *in, const uint8 *end, uint32 *v)
{
    uint32  result = 0;
    int     shift = 0;
    int     n = 0;
    uint8   b;

    do
    {
        if (in + n >= end)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: varbyte sequence runs past the end of its run")));
        if (n == BM25_VARBYTE_MAX_BYTES)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: varbyte sequence wider than %d bytes",
                            BM25_VARBYTE_MAX_BYTES)));
        b = in[n++];
        result |= ((uint32) (b & 0x7F)) << shift;
        shift += 7;
    } while (b & 0x80);

    *v = result;
    return n;
}

PG_FUNCTION_INFO_V1(bm25_debug_varbyte_roundtrip);
Datum
bm25_debug_varbyte_roundtrip(PG_FUNCTION_ARGS)
{
    int64       arg = PG_GETARG_INT64(0);
    uint32      v;
    uint8       buf[5];
    int         nenc;
    uint32      decoded;
    int         ndec PG_USED_FOR_ASSERTS_ONLY;
    TupleDesc   tupdesc;
    Datum       values[2];
    bool        nulls[2] = {0};
    HeapTuple   tuple;

    if (arg < 0 || arg > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_varbyte_roundtrip: value out of uint32 range")));
    v = (uint32) arg;

    nenc = bm25_varbyte_encode(v, buf);
    ndec = bm25_varbyte_decode(buf, buf + sizeof(buf), &decoded);
    Assert(nenc == ndec);       /* invariant */
    Assert(decoded == v);       /* invariant: encode/decode round-trips exactly */

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    values[0] = Int64GetDatum((int64) decoded);
    values[1] = Int32GetDatum(nenc);
    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * Decode-boundary probes. The validators added for review ref H6 only fire on a
 * page that is already corrupt, which a regression suite cannot produce -- so
 * these three run the SAME validators over caller-supplied bytes, giving the
 * negative paths real coverage without poking a live page.
 *
 * TEST-ONLY, like the rest of the bm25_debug_* surface: pure functions of their
 * argument, no relation touched, no privilege to escalate. They are covered
 * automatically by the install script's REVOKE loop (ADR 0020) because they match
 * the bm25_debug_ prefix, and 63_debug_privileges asserts that.
 */
PG_FUNCTION_INFO_V1(bm25_debug_varbyte_decode_bytes);
Datum
bm25_debug_varbyte_decode_bytes(PG_FUNCTION_ARGS)
{
    bytea      *raw = PG_GETARG_BYTEA_PP(0);
    const uint8 *in = (const uint8 *) VARDATA_ANY(raw);
    int          len = (int) VARSIZE_ANY_EXHDR(raw);
    uint32       v;

    (void) bm25_varbyte_decode(in, in + len, &v);
    PG_RETURN_INT64((int64) v);
}

/* Validate a block header built from its FIELDS rather than raw bytes: a bytea of
 * a native struct would make the suite depend on host endianness and padding, and
 * nothing else in the tree does. `avail` is how many bytes the notional page has
 * left from the header onward, i.e. what pend - cur would be. Returns the
 * validated block length. */
PG_FUNCTION_INFO_V1(bm25_debug_block_validate);
Datum
bm25_debug_block_validate(PG_FUNCTION_ARGS)
{
    BM25BlockHeader hdr;
    int32           avail = PG_GETARG_INT32(5);
    char            base = 0;

    memset(&hdr, 0, sizeof(hdr));
    hdr.ndocs           = (uint16) PG_GETARG_INT32(0);
    hdr.docid_bytes     = (uint16) PG_GETARG_INT32(1);
    hdr.tf_bytes        = (uint16) PG_GETARG_INT32(2);
    hdr.field_rle_bytes = (uint16) PG_GETARG_INT32(3);
    hdr.impact_bytes    = (uint16) PG_GETARG_INT32(4);

    /* &base stands in for the header's page position; only the DIFFERENCE
     * (pend - cur) is examined, so no real page is needed. */
    PG_RETURN_INT64((int64) bm25_block_validate(&hdr, &base, &base + avail));
}

PG_FUNCTION_INFO_V1(bm25_debug_impact_decode_bytes);
Datum
bm25_debug_impact_decode_bytes(PG_FUNCTION_ARGS)
{
    bytea          *raw = PG_GETARG_BYTEA_PP(0);
    BM25BlockImpact imp;

    bm25_decode_impact_table((const uint8 *) VARDATA_ANY(raw),
                             (uint16) VARSIZE_ANY_EXHDR(raw), &imp);
    PG_RETURN_INT32((int32) imp.nfields);
}

/* Encode a per-block field_ids[] as (field_id, run_length) varbyte pairs into out;
 * return bytes written. Consecutive equal ids collapse into one run. Shared by
 * encode_block AND the bm25_debug_field_rle_roundtrip SRF so the run logic lives in
 * ONE place; decode_field_rle is its exact inverse. Worst case (every posting its
 * own run) is n * 2 * BM25_VARBYTE_MAX_BYTES bytes. */
static Size
encode_field_rle(const uint32 *field_ids, uint32 n, uint8 *out)
{
    uint8  *p = out;
    uint32  i = 0;

    while (i < n)
    {
        uint32 fid = field_ids[i];
        uint32 run = 1;
        while (i + run < n && field_ids[i + run] == fid)
            run++;
        p += bm25_varbyte_encode(fid, p);
        p += bm25_varbyte_encode(run, p);
        i += run;
    }
    return p - out;
}

/* Inverse of encode_field_rle: expand `nbytes` of (field_id, run_length) varbyte
 * pairs into out_field_ids[0..n-1]. The caller guarantees the run lengths sum to
 * exactly n (they do by construction -- encode_field_rle covered every posting).
 * A single-field block has nbytes == 0 and the caller fills field 0 itself.
 * NON-static: both on-page readers -- bm25_seg_scan_postings (bm25_seg_chain.c) and
 * wand_cursor_load_block (bm25_wand.c) -- share this ONE decoder so the RLE parse
 * is never re-implemented (declared in bm25_format.h). */
void
bm25_field_rle_decode(const uint8 *in, Size nbytes, uint32 *out_field_ids, uint32 n)
{
    const uint8 *p = in;
    const uint8 *end = in + nbytes;
    uint32       idx = 0;

    while (p < end && idx < n)
    {
        uint32 fid, run, r;
        p += bm25_varbyte_decode(p, end, &fid);
        p += bm25_varbyte_decode(p, end, &run);
        /* The decoded field_id reaches the posting callback, which uses it to index
         * BM25_MAX_FIELDS-wide per-field arrays (idf_f, avgdl_f, boosts). Bounding it
         * here covers every reader, since this is the only RLE parse in the tree. */
        if (fid >= BM25_MAX_FIELDS)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: posting block field id %u exceeds BM25_MAX_FIELDS %d",
                            fid, BM25_MAX_FIELDS)));
        for (r = 0; r < run && idx < n; r++)
            out_field_ids[idx++] = fid;
    }

    /* The caller's contract is that the run lengths sum to EXACTLY n, consuming
     * EXACTLY nbytes (true by construction of encode_field_rle: it never emits a
     * short run and never leaves an unconsumed trailing pair). Checking only
     * `idx < n` caught UNDERSHOOT (stream runs out before n ids are produced) but
     * missed OVERSHOOT: a run whose length sums to more than n makes the inner
     * loop's `idx < n` guard stop early, `idx` reaches `n`, the outer `while`
     * exits on that -- and any trailing bytes (the rest of that run, or entirely
     * separate leftover pairs) were silently ignored rather than treated as the
     * corrupt encoding they are. Checking both directions catches either. */
    if (p != end || idx != n)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block field-id run-length encoding produced %u of %u ids",
                        idx, n)));
}

/* Decode-boundary probe (2026-08 page-content-bytes sweep) for bm25_field_rle_decode's
 * symmetric tail check. bm25_debug_field_rle_roundtrip (below) always encodes and
 * decodes the SAME n (the input array's length), so it can only ever exercise the
 * well-formed round trip -- neither an undershoot nor an overshoot is reachable
 * through it. This probe decouples the two: it decodes caller-supplied raw bytes
 * (which the caller can hand-build shorter or longer than a real encoding of `n`
 * ids would be) against a caller-chosen `n`, exactly like
 * bm25_debug_varbyte_decode_bytes and bm25_debug_impact_decode_bytes decode
 * caller-supplied bytes rather than round-tripping their own encoder's output.
 * Returns the decoded field_ids on success. TEST-ONLY: a pure function of its
 * arguments, no relation touched, covered by the install script's REVOKE loop
 * like every other bm25_debug_* function. */
PG_FUNCTION_INFO_V1(bm25_debug_field_rle_decode_bytes);
Datum
bm25_debug_field_rle_decode_bytes(PG_FUNCTION_ARGS)
{
    bytea      *raw = PG_GETARG_BYTEA_PP(0);
    int32       n    = PG_GETARG_INT32(1);
    const uint8 *in  = (const uint8 *) VARDATA_ANY(raw);
    Size         nbytes = (Size) VARSIZE_ANY_EXHDR(raw);
    uint32      *out;
    Datum       *elems;
    ArrayType   *result;
    int          i;

    if (n < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_field_rle_decode_bytes: n must be non-negative")));

    out = palloc(sizeof(uint32) * (Size) Max(n, 1));
    bm25_field_rle_decode(in, nbytes, out, (uint32) n);

    elems = palloc(sizeof(Datum) * (Size) Max(n, 1));
    for (i = 0; i < n; i++)
        elems[i] = Int32GetDatum((int32) out[i]);
    result = construct_array(elems, n, INT4OID, sizeof(int32), true, TYPALIGN_INT);
    PG_RETURN_ARRAYTYPE_P(result);
}

/* Serialize a BM25BlockImpact as [uint8 nfields][{uint8 field_id, uint32 max_tf,
 * uint32 min_doclen} x nfields]; return bytes written (1 + 9*nfields). Fixed-width
 * host byte order (native, no conversion) via memcpy rather than varbyte: the
 * impact table is read once per block (skip decision), not once per posting, so the
 * codec favors simplicity over density here. The uint32s land on the page exactly
 * as the host holds them, like the rest of the on-disk format (page structs are
 * memcpy'd verbatim in both directions); nothing here byte-swaps. */
Size
bm25_encode_impact_table(const BM25BlockImpact *imp, uint8 *out)
{
    uint8 *p = out;
    int    i;

    *p++ = imp->nfields;
    for (i = 0; i < imp->nfields; i++)
    {
        *p++ = imp->fields[i].field_id;
        memcpy(p, &imp->fields[i].max_tf, sizeof(uint32));
        p += sizeof(uint32);
        memcpy(p, &imp->fields[i].min_doclen, sizeof(uint32));
        p += sizeof(uint32);
    }
    return (Size) (p - out);
}

/* Inverse of bm25_encode_impact_table.
 *
 * nbytes == 0 used to return an all-zero table "defensively". That directly
 * contradicted bm25_format.h, which specifies impact_bytes is never 0, and the
 * tolerance was not harmless: a zero-field table makes bm25_block_ub return 0.0 and
 * the WAND driver prune the whole block, so the defensive branch turned corruption
 * into silently missing search results. bm25_block_validate now rejects
 * impact_bytes == 0 up front, which is the single declared decode boundary both
 * on-page readers route through (bm25_seg_chain.c, bm25_wand.c), so the case cannot
 * reach here from a page any more. The early return is kept solely so
 * bm25_debug_impact_decode_bytes -- the one caller that supplies its own buffer, a
 * caller-chosen bytea that may be empty -- returns 0 fields instead of reading a byte
 * that is not there. bm25_encode_impact_table always emits at least the 1-byte
 * nfields count, so no well-formed block reaches it either.
 *
 * This is a live decode boundary, not a debug-only helper: it runs once per posting
 * block on every ranked scan -- wand_cursor_load_block (bm25_wand.c) on the WAND
 * block-max path, and bm25_seg_block_header_read (bm25_seg_chain.c) on the
 * header-only path -- over bytes read straight off a shared-buffer page. Both
 * inputs are therefore page-derived and untrusted, so nfields and nbytes are
 * VALIDATED below (both raise ERRCODE_INDEX_CORRUPTED) rather than asserted; see
 * the note on the nfields check. It stays here beside the encoder so the pair
 * reads as one unit.
 *
 * Residuals (issue #303.E): only the table's SHAPE is validated; its values are
 * trusted. bm25_block_ub (bm25_wand.c) bounds a block by each field's max_tf and
 * min_doclen, so an entry that understates a real tf (or overstates a real doclen),
 * or a table missing a field the block holds postings in, gives a bound below the
 * block's real scores, and WAND prunes true top-k documents with no error. A
 * load-time cross-check in wand_cursor_load_block (every decoded posting's field has
 * an entry and tf <= its max_tf) was built and measured, and per the #303.E decision
 * (land the check only if it is free on the hot path) it was not landed, because it
 * is not. In an interleaved crossover A/B
 * at -O2 on 120-150k-document corpora whose ranked LIMIT 10/100 queries skip blocks
 * (bm25_wand_stats, with identical counters and results before and after), a
 * per-posting walk cost about 2.7% on a two-field index in both arms. A vectorized
 * max-tf reduction for single-field blocks measured +0.4% and +1.2%, inside the noise.
 * Even with the check, a block that is never decoded escapes it: the pivot test's
 * global_ub and next_geq's skips read header-only peeks. The doclen half could only
 * be checked per scored document, in bm25_wand_cursor_score_doc. */
void
bm25_decode_impact_table(const uint8 *in, uint16 nbytes, BM25BlockImpact *out)
{
    const uint8 *p = in;
    int          i;

    memset(out, 0, sizeof(*out));
    if (nbytes == 0)
        return;
    out->nfields = *p++;
    /* Was an Assert, i.e. absent in a production build, while the writes below go
     * into a caller's STACK BM25BlockImpact: nfields is a raw on-page byte, so 255
     * fields wrote ~2.7 KB past a 388-byte struct on the WAND open path. The length
     * cross-check is the stronger of the two -- it catches a plausible-looking
     * nfields that the surrounding block header does not actually account for. */
    if (out->nfields > BM25_MAX_FIELDS)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: impact table claims %u fields, maximum is %d",
                        out->nfields, BM25_MAX_FIELDS)));
    if (nbytes != 1 + BM25_IMPACT_FIELD_BYTES * (uint16) out->nfields)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: impact table is %u bytes but declares %u fields (expected %u)",
                        nbytes, out->nfields,
                        1 + BM25_IMPACT_FIELD_BYTES * (uint16) out->nfields)));
    for (i = 0; i < out->nfields; i++)
    {
        out->fields[i].field_id = *p++;
        memcpy(&out->fields[i].max_tf, p, sizeof(uint32));
        p += sizeof(uint32);
        memcpy(&out->fields[i].min_doclen, p, sizeof(uint32));
        p += sizeof(uint32);
    }
}

/*
 * bm25_block_validate -- the decode boundary for a posting block.
 *
 * Every reader memcpy's a BM25BlockHeader straight off a shared-buffer page and
 * then uses its uint16 counts as loop bounds, memcpy sizes and cursor offsets.
 * bm25_seg_page_validate does NOT cover any of that: it compares the page opaque's
 * seg_gen and nothing else, so a torn page, a bit flip, or a hostile page image in
 * a restored data directory passes it with an arbitrary block header. The readers
 * guarded these with Assert, which is compiled out in exactly the build where it
 * matters.
 *
 * `cur` points at the header, `pend` one past the last valid content byte
 * (page + pd_lower). Returns the block's total on-page length so the caller can
 * advance without recomputing it.
 *
 * Every bound below is satisfied by construction in encode_block: it asserts
 * 0 < n <= BM25_POSTINGS_PER_BLOCK, and varbyte-encodes exactly n docid deltas and
 * n tfs, each at least one byte and at most BM25_VARBYTE_MAX_BYTES. So the run
 * lengths pin down what a well-formed block can look like tightly enough that a
 * corrupt header cannot produce an in-range one by accident.
 */
Size
bm25_block_validate(const BM25BlockHeader *hdr, const char *cur, const char *pend)
{
    Size    block_len;

    if (hdr->ndocs == 0 || hdr->ndocs > BM25_POSTINGS_PER_BLOCK)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block claims %u docs, limit is %d",
                        hdr->ndocs, BM25_POSTINGS_PER_BLOCK)));

    if (hdr->docid_bytes < hdr->ndocs ||
        hdr->docid_bytes > (uint32) hdr->ndocs * BM25_VARBYTE_MAX_BYTES ||
        hdr->tf_bytes < hdr->ndocs ||
        hdr->tf_bytes > (uint32) hdr->ndocs * BM25_VARBYTE_MAX_BYTES)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block with %u docs has implausible run lengths "
                        "(docid_bytes %u, tf_bytes %u)",
                        hdr->ndocs, hdr->docid_bytes, hdr->tf_bytes)));

    /* impact_bytes == 0 (SEGREAD-08). Until now this quantity appeared only as a
     * summand in block_len, so a too-LARGE value was caught incidentally by the
     * overrun check below while zero passed straight through -- and zero is the
     * damaging value, on the default path. An all-zero table gives imp->nfields == 0,
     * bm25_block_ub (bm25_wand.c) then loops zero times and returns ub = 0.0, and the
     * WAND driver prunes every posting in the block: silently missing rows from an
     * ordinary ranked query, no error anywhere. bm25_format.h is the authority for the
     * on-disk contract and states impact_bytes is "never 0 -- every block has at least
     * a 1-byte nfields count", so this is corruption, not a tolerable shape. The
     * minimum is that 1-byte count; the maximum is one count plus a full field table. */
    if (hdr->impact_bytes == 0 ||
        hdr->impact_bytes > 1 + (uint32) BM25_MAX_FIELDS * BM25_IMPACT_FIELD_BYTES)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block has an invalid impact table length %u",
                        hdr->impact_bytes),
                 errdetail("Expected between 1 and %d bytes.",
                           1 + BM25_MAX_FIELDS * BM25_IMPACT_FIELD_BYTES)));

    block_len = sizeof(BM25BlockHeader) + hdr->docid_bytes + hdr->tf_bytes +
                hdr->field_rle_bytes + hdr->impact_bytes;

    /* The four uint16 run lengths sum to at most 262140, so without this the
     * decode cursors could be placed ~250 KB past an 8 KB page. */
    if (cur + block_len > pend)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block of %zu bytes overruns the page by %zu bytes",
                        block_len, (Size) (cur + block_len - pend))));

    return block_len;
}

/* Cross-check a block header's last_docid against the run that was actually decoded
 * from it (QRY-08, and the wider half of QRY-07).
 *
 * WHY THIS IS NOT IN bm25_block_validate, which is where it belongs by topic: that
 * function runs on the HEADER, before the varbyte deltas are expanded, so the value
 * it would need does not exist yet.
 *
 * Only ONE of the three block readers calls this, and that is not an oversight:
 *   - wand_cursor_load_block materializes the whole run into cur->docids[] and is
 *     the reader whose consumers trust last_docid, so it calls this. It is also the
 *     default ranked path, i.e. where a wrong last_docid actually bites.
 *   - bm25_seg_block_header_read never expands the deltas at all -- it is the
 *     header-only PEEK next_geq uses to choose a landing block. Nothing to compare.
 *   - bm25_seg_scan_postings streams each posting to a callback and never
 *     materializes the run, but after a block's last posting its running `prev` IS
 *     the run's last docid, so it makes the same equality check inline (issue #293),
 *     with the same message, instead of through this array-taking form.
 *
 * The peek is therefore still unvalidated by construction, and that has a residual
 * cost worth stating plainly: a peeked-and-SKIPPED block whose last_docid UNDERSTATES
 * its true maximum has its postings bypassed with no error -- silently missing rows
 * under corruption. Validating it would require decoding the very block the peek
 * exists to avoid decoding. What the check here does close is every use of a
 * last_docid that has been DECODED: the landing scan cannot run off the run, and the
 * deep check's skip_target cannot stall on a value the block does not support.
 *
 * last_docid is the one BM25BlockHeader field no reader validated, and two separate
 * WAND behaviours trust it:
 *   - bm25_wand_cursor_next_geq picks its landing block because the PEEKED
 *     hdr.last_docid >= target, then scans `while (pos < ndocs && docids[pos] <
 *     target)`. If last_docid overstates the run, pos walks off docids[] -- which
 *     for ndocs == BM25_POSTINGS_PER_BLOCK aliases tfs[0], and for a shorter run
 *     yields a stale docid left by the previously decoded block, which then drives
 *     score_doc to read tfs[]/fields[] from that same stale window. A wrong score on
 *     an ordinary ranked query, not a crash.
 *   - the block-max deep check computes skip_target = min_last + 1 and relies on it
 *     exceeding the pivot, an invariant that holds only if last_docid really is the
 *     block's maximum. A value that UNDERSTATES the maximum makes every next_geq
 *     return at its "already at/past target" guard, nothing advances, and the driver
 *     loop spins until the user cancels.
 * Requiring exact equality with the decoded maximum closes both, and is the strongest
 * available statement: the encoder writes last_docid = docids[n-1] by construction. */
void
bm25_block_last_docid_validate(const BM25BlockHeader *hdr, const uint32 *docids)
{
    if (hdr->ndocs == 0)        /* bm25_block_validate already rejected this */
        return;

    if (docids[hdr->ndocs - 1] != hdr->last_docid)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting block header claims last docid %u but its run ends at %u",
                        hdr->last_docid, docids[hdr->ndocs - 1])));
}

/* bm25_block_lead_decode -- issue #289: the postings of a block's first document.
 * See BM25BlockLead (bm25_format.h) for what they are for.
 *
 * Bounds: the caller validated *hdr against this block's page, so the docid, tf and
 * field-RLE streams all lie inside it, and each varbyte decode below is bounded by
 * its own stream's end. The RLE is expanded whole by the one shared decoder rather
 * than partially by a second parser, so its corruption checks apply here too; it is
 * at most BM25_POSTINGS_PER_BLOCK entries, and this runs at most once per block a
 * WAND cursor loads -- and past the first docid only when that docid is
 * match_docid, i.e. only for an actual straddle, which the current writer never
 * produces. */
void
bm25_block_lead_decode(const BM25BlockHeader *hdr, const char *cur, uint32 match_docid,
                       BM25BlockLead *lead)
{
    const uint8 *dp   = (const uint8 *) (cur + sizeof(BM25BlockHeader));
    const uint8 *dend = dp + hdr->docid_bytes;
    const uint8 *tp   = dend;
    const uint8 *tend = tp + hdr->tf_bytes;
    uint32       fields[BM25_POSTINGS_PER_BLOCK];
    uint32       i;

    dp += bm25_varbyte_decode(dp, dend, &lead->docid);     /* first delta is from 0 */
    lead->n = 0;
    if (lead->docid != match_docid)
        return;             /* not the document the caller asked about: docid only */
    lead->n = 1;
    while (lead->n < hdr->ndocs)
    {
        uint32 delta;

        dp += bm25_varbyte_decode(dp, dend, &delta);
        if (delta != 0)
            break;
        if (lead->n == BM25_MAX_FIELDS)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: posting block holds more than %d postings for document %u",
                            BM25_MAX_FIELDS, lead->docid)));
        lead->n++;
    }
    for (i = 0; i < lead->n; i++)
        tp += bm25_varbyte_decode(tp, tend, &lead->tf[i]);

    if (hdr->field_rle_bytes > 0)
    {
        bm25_field_rle_decode(tend, hdr->field_rle_bytes, fields, hdr->ndocs);
        for (i = 0; i < lead->n; i++)
            lead->field_id[i] = fields[i];
    }
    else
        for (i = 0; i < lead->n; i++)
            lead->field_id[i] = 0;
}

/* Decode-boundary probe for bm25_block_last_docid_validate. Only the final element of
 * the run is examined, so the probe builds a one-element array and sets ndocs to 1
 * rather than materializing a whole block -- the check under test is the equality, not
 * the decode. Same pure-function shape as bm25_debug_block_validate beside it. */
PG_FUNCTION_INFO_V1(bm25_debug_block_last_docid_validate);
Datum
bm25_debug_block_last_docid_validate(PG_FUNCTION_ARGS)
{
    int64           last_docid = PG_GETARG_INT64(0);
    int64           run_last   = PG_GETARG_INT64(1);
    BM25BlockHeader hdr;
    uint32          docids[1];

    if (last_docid < 0 || last_docid > (int64) PG_UINT32_MAX ||
        run_last < 0 || run_last > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_block_last_docid_validate: docids must fit uint32")));

    memset(&hdr, 0, sizeof(hdr));
    hdr.ndocs      = 1;
    hdr.last_docid = (uint32) last_docid;
    docids[0]      = (uint32) run_last;

    bm25_block_last_docid_validate(&hdr, docids);
    PG_RETURN_BOOL(true);
}

/* Issue #289: a document holds at most one posting per field for a term, so with
 * BM25_MAX_FIELDS <= BM25_POSTINGS_PER_BLOCK every document fits in one block. That
 * is what lets the slicing loop in bm25_segment_build_orphans always find a
 * document boundary to cut at without exceeding the block limit, and what bounds an
 * OLD (count-sliced) segment's straddler to two blocks, which is all the WAND
 * reader's straddle handling (bm25_wand.c:wand_cursor_straddle_ub) covers. */
StaticAssertDecl(BM25_MAX_FIELDS <= BM25_POSTINGS_PER_BLOCK,
                 "a document's postings for one term must fit in one posting block");

/* Serialize one block (<=128 postings) into out; return bytes written. docids are
 * ascending local doc-ids; deltas are first-minus-0 then successive differences.
 * When field_count > 1, a per-block field-id RLE stream (section 3.5) is appended AFTER
 * the tf stream and hdr.field_rle_bytes records its byte length; when field_count
 * == 1 the RLE is omitted and field_rle_bytes stays 0 -- byte-identical to M3.
 * A per-field impact table (max tf, min doclen over the block's postings) is then
 * appended and hdr.impact_bytes records its length; this is written for EVERY
 * block, including single-field ones, since the WAND scorer needs it regardless of
 * field_count. out must hold a worst-case block (header + docid + tf + field-id RLE
 * worst case + impact-table worst case). */
static Size
encode_block(const uint32 *docids, const uint32 *tfs, const uint32 *field_ids,
             const uint32 *doclens, uint32 n, uint32 field_count, uint8 *out)
{
    BM25BlockHeader hdr;
    BM25BlockImpact imp;
    uint8          *p;
    uint32          prev = 0;
    uint32          i;
    Size            doc_start, tf_start, rle_start, rle_len = 0, imp_len;

    /* field_ids is NOT optional despite the historical `field_ids ? ... : 0`
     * guard this function's impact-table loop used to carry: the sole caller
     * (bm25_segment_build_orphans, this file) passes `field_ids + off`, a
     * per-block slice of the array bm25_accum_term_postings returns -- reused
     * accumulator scratch (NOT a fresh allocation: see that function's lifetime
     * contract), sized Max(npost, 1) so it is never NULL even for a zero-posting
     * term. "NULL + off" is a wild pointer, not NULL, so the
     * guard never actually fired; that same caller's POS-chain loop over the
     * identical array (`field_ids[j]`, no guard at all) already assumed this. */
    Assert(field_ids != NULL);   /* checked: the sole caller passes field_ids + off */
    Assert(n > 0 && n <= BM25_POSTINGS_PER_BLOCK);   /* checked: the caller's block slicing */
    p = out + sizeof(BM25BlockHeader);      /* header backfilled last */
    doc_start = p - out;
    for (i = 0; i < n; i++)
    {
        uint32 delta = docids[i] - prev;    /* first delta is docids[0]-0 */
        p += bm25_varbyte_encode(delta, p);
        prev = docids[i];
    }
    tf_start = p - out;
    for (i = 0; i < n; i++)
        p += bm25_varbyte_encode(tfs[i], p);

    /* Field-id RLE tail: present only for a multi-field segment. Single-field
     * stays zero-RLE for v4 back-compat (the on-page bytes are the M3 layout). */
    rle_start = p - out;
    if (field_count > 1)
    {
        rle_len = encode_field_rle(field_ids, n, p);
        p += rle_len;
    }

    /* Impact table: fold this block's postings into a per-field (max_tf, min_doclen)
     * summary. These are raw ingredients, not a baked score -- idf/avgdl are supplied
     * at SCAN time (see the format-h comment on BM25BlockImpact), so no k1/b/idf math
     * belongs here even though this runs at seal/merge. */
    memset(&imp, 0, sizeof(imp));
    for (i = 0; i < n; i++)
    {
        uint32 f = field_ids[i];
        int    slot = -1;
        int    j;

        for (j = 0; j < imp.nfields; j++)
            if (imp.fields[j].field_id == (uint8) f)
            {
                slot = j;
                break;
            }
        if (slot < 0)
        {
            slot = imp.nfields++;
            Assert(imp.nfields <= BM25_MAX_FIELDS);   /* invariant */
            imp.fields[slot].field_id   = (uint8) f;
            imp.fields[slot].max_tf     = tfs[i];
            imp.fields[slot].min_doclen = doclens[i];
        }
        else
        {
            if (tfs[i] > imp.fields[slot].max_tf)
                imp.fields[slot].max_tf = tfs[i];
            if (doclens[i] < imp.fields[slot].min_doclen)
                imp.fields[slot].min_doclen = doclens[i];
        }
    }
    imp_len = bm25_encode_impact_table(&imp, p);
    p += imp_len;

    /* C5: zero the whole header before populating it. BM25BlockHeader has 2 bytes
     * of compiler padding between ndocs (uint16) and last_docid (uint32); the
     * memcpy below copies sizeof(hdr) bytes, so uninitialized stack padding would
     * be written below pd_lower and WAL-logged, making page images
     * non-deterministic (replica/crash-image divergence, valgrind noise). The
     * field ORDER is part of the on-disk contract and must NOT be reordered to
     * close the gap. */
    memset(&hdr, 0, sizeof(hdr));
    hdr.ndocs = (uint16) n;
    hdr.last_docid = docids[n - 1];
    hdr.docid_bytes = (uint16) (tf_start - doc_start);
    hdr.tf_bytes = (uint16) (rle_start - tf_start);
    hdr.field_rle_bytes = (uint16) rle_len;  /* 0 when field_count == 1 (M3 layout) */
    hdr.impact_bytes = (uint16) imp_len;
    memcpy(out, &hdr, sizeof(hdr));
    return p - out;
}

/* bm25_debug_field_rle_roundtrip(int[]) -- encode a synthetic per-block field_ids[]
 * through the SAME encode_field_rle/decode_field_rle codec encode_block and
 * bm25_seg_scan_postings use, then return the recovered (idx, field_id). Proves the
 * pair round-trips independently of a full segment build. Development/regression
 * only; not on the query path. */
PG_FUNCTION_INFO_V1(bm25_debug_field_rle_roundtrip);
Datum
bm25_debug_field_rle_roundtrip(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    ArrayType      *arr = PG_GETARG_ARRAYTYPE_P(0);
    Datum          *elems;
    bool           *nullp;
    int             nelems, i;
    uint32         *in;
    uint32         *out;
    uint8          *buf;
    Size            nbytes;
    TupleDesc       tupdesc;
    Tuplestorestate *ts;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    /* Use the general deconstruct_array with an explicit int4 type descriptor
     * rather than deconstruct_array_builtin(INT4OID): INT4OID was only added
     * to that wrapper's supported-type switch in PostgreSQL 18.2 (back-patched
     * to 17.x), so the builtin errors "type 23 not supported" on a stock 18.0/
     * 18.1 backend.  The general form takes the descriptor directly and is
     * byte-for-byte equivalent on every supported version (floor PG17). */
    deconstruct_array(arr, INT4OID, sizeof(int32), true, TYPALIGN_INT,
                      &elems, &nullp, &nelems);
    if (nelems == 0)
        return (Datum) 0;

    in  = palloc(sizeof(uint32) * nelems);
    out = palloc(sizeof(uint32) * nelems);
    for (i = 0; i < nelems; i++)
    {
        if (nullp[i])
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("bm25_debug_field_rle_roundtrip: NULL field_id at index %d", i)));
        in[i] = (uint32) DatumGetInt32(elems[i]);
    }

    /* Worst case (every posting its own run): nelems * 2 varbytes. */
    buf = palloc((Size) nelems * 2 * BM25_VARBYTE_MAX_BYTES);
    nbytes = encode_field_rle(in, (uint32) nelems, buf);
    bm25_field_rle_decode(buf, nbytes, out, (uint32) nelems);

    for (i = 0; i < nelems; i++)
    {
        Datum   vals[2];
        bool    nulls[2] = {0};

        vals[0] = Int32GetDatum(i);
        vals[1] = Int32GetDatum((int32) out[i]);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    return (Datum) 0;
}

/* bm25_segheader_write_lenfields -- append the per-field length arrays onto the
 * header page, immediately AFTER the BM25SegmentHeader struct, and return the new
 * pd_lower OFFSET (bytes from page start) the caller must store. The struct must
 * already be written at PageGetContents(); this writes the array(s) at
 * PageGetContents()+sizeof(BM25SegmentHeader). The values are NOT MAXALIGN-strided
 * between each other (packed uint64 runs). The byte-for-byte memcpy below makes
 * the array's start alignment irrelevant (no aligned load is performed on the
 * page), so no alignment invariant on sizeof(BM25SegmentHeader) is assumed.
 * Caller sets pd_lower to the returned value so page-hole compression keeps both
 * the struct and the array(s).
 *
 * Layout:
 *   field_count == 1: [total_len_by_field[0]]                  (M3/C-BUILD bytes,
 *                                                               UNCHANGED)
 *   field_count  > 1: [total_len_by_field[fc]][ndocs_by_field[fc]]
 *
 * BACK-COMPAT: ndocs_by_field[] is written ONLY when field_count > 1 so an
 * existing single-field v4 index reads byte-identically (no REINDEX). For a
 * single field the scorer derives N_field[0] from hdr->ndocs (every live doc
 * has the one field), so nothing is lost by omitting the array. */
Size
bm25_segheader_write_lenfields(Page pg, const BM25SegmentHeader *hdr,
                               const uint64 *total_len_by_field,
                               const uint64 *ndocs_by_field)
{
    char   *base = (char *) PageGetContents(pg);
    char   *arr  = base + sizeof(BM25SegmentHeader);
    uint32  i;

    for (i = 0; i < hdr->field_count; i++)
        memcpy(arr + i * sizeof(uint64), &total_len_by_field[i], sizeof(uint64));
    arr += (Size) hdr->field_count * sizeof(uint64);

    if (hdr->field_count > 1)
    {
        for (i = 0; i < hdr->field_count; i++)
            memcpy(arr + i * sizeof(uint64), &ndocs_by_field[i], sizeof(uint64));
        arr += (Size) hdr->field_count * sizeof(uint64);
    }

    return arr - (char *) pg;
}

/* Append bytes to an orphan page chain rooted at w->root (BM25_PAGE_* kind).
 * Opens a fresh page when the current one lacks room. Each page is its own
 * <=1-buffer GenericXLog record; the metapage is never registered here. Returns
 * the (block, content-offset) where the bytes start. Content offsets are measured
 * from PageGetContents (== page + SizeOfPageHeaderData).
 *
 * On any ereport(ERROR) inside an open window, the open buffer is held EXCLUSIVE
 * and registered in w->state; the only error sources here are palloc/bufmgr,
 * which abort the transaction and run resowner cleanup (the open GenericXLog is
 * discarded and the buffer released by the abort path), so there is no partially
 * linked live structure to unwind -- nothing is reachable until the final publish
 * record. */
typedef struct ChainWriter
{
    Relation        index;
    Relation        heaprel;        /* D-ALLOC/M6: pre-opened heap threaded to bm25_page_alloc */
    uint16          kind;
    uint32          gen;            /* option (d): stamp every page with the segment's gen */
    BlockNumber     root;
    Buffer          tailbuf;        /* currently-open EXCL-locked tail, or Invalid */
    Page            tailpage;       /* registered page in the open xlog state */
    GenericXLogState *state;
} ChainWriter;

static void
chain_open(ChainWriter *w, Relation index, Relation heaprel, uint16 kind, uint32 gen)
{
    w->index = index;
    w->heaprel = heaprel;
    w->kind = kind;
    w->gen = gen;
    w->root = InvalidBlockNumber;
    w->tailbuf = InvalidBuffer;
    w->tailpage = NULL;
    w->state = NULL;
}

/* flush the open tail page's xlog record (call before locking the next page). */
static void
chain_flush(ChainWriter *w)
{
    if (w->state != NULL)
    {
        GenericXLogFinish(w->state);
        UnlockReleaseBuffer(w->tailbuf);
        w->state = NULL;
        w->tailbuf = InvalidBuffer;
        w->tailpage = NULL;
    }
}

/* Content bytes a freshly initialized chain page can hold. `need` above this can
 * NEVER be satisfied, on a fresh page or any other. */
#define CHAIN_PAGE_CAPACITY \
    ((Size) (BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BM25PageOpaque))))

/* ensure an open tail page with at least `need` contiguous content bytes. */
static void
chain_ensure(ChainWriter *w, Size need)
{
    Page    pg = w->tailpage;

    /* Trust boundary, not an Assert: the fresh-page branch below cannot satisfy a
     * `need` larger than one page, and it used to return anyway -- leaving the caller
     * to memcpy past the end of an 8 KB shared buffer. Record sizes here are derived
     * from user data (a dictionary record carries its term inline), so this must hold
     * in a production build, where Asserts are compiled out. */
    if (need > CHAIN_PAGE_CAPACITY)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: segment record of %zu bytes exceeds the %zu-byte page capacity",
                        need, CHAIN_PAGE_CAPACITY)));

    if (pg != NULL)
    {
        Size avail = ((PageHeader) pg)->pd_upper - ((PageHeader) pg)->pd_lower;
        if (avail >= need)
            return;
    }
    {
        BlockNumber        oldblk = (w->tailpage != NULL)
                                    ? BufferGetBlockNumber(w->tailbuf)
                                    : InvalidBlockNumber;
        Buffer             newbuf;
        BlockNumber        newblk;
        GenericXLogState  *st;
        Page               np;

        /* Finish-and-fully-release the old tail BEFORE allocating the new one,
         * rather than the old allocate-then-flush order. Why this matters:
         * LWLockAcquire calls HOLD_INTERRUPTS() (miscadmin.h's
         * INTERRUPTS_CAN_BE_PROCESSED gates on InterruptHoldoffCount == 0), so
         * CHECK_FOR_INTERRUPTS is a silent no-op for as long as ANY buffer
         * content lock is held. The old order held the OLD and NEW tail buffers
         * locked SIMULTANEOUSLY at every rotation (new acquired before old was
         * released, to learn newblk for the old page's forward link), and held
         * one or the other continuously the rest of the time -- there was never
         * an instant with zero buffers held, so every CHECK_FOR_INTERRUPTS this
         * pass originally placed in the CALLERS of chain_write/chain_write_stream
         * was dead code (caught in review). This is the one place that fix
         * belongs: restructured so there IS a genuine lock-free instant, once per
         * page rotation, covering every chain this file writes (DOCMAP, NORMS,
         * LIVE, POST, POS, DICT) through this one shared function.
         *
         * The cost: the old page's forward link can no longer ride the SAME
         * record as its content, because newblk isn't known until AFTER the old
         * page is released. So the old page is flushed with nextblk still
         * Invalid (bm25_page_init's default), and a SECOND, tiny Generic WAL
         * record re-opens it below, once newblk is known, to backfill just that
         * one field. One extra WAL record per PAGE (not per chain_write call --
         * a DOCMAP page holds ~1300 records before it rotates), in exchange for
         * cancellability that actually works. Safe under a crash between the two
         * records: these are orphan pages, unreachable until the segment
         * publishes far downstream, so a chain that ends up one link short after
         * a crash is exactly as harmless as any other abandoned orphan page. */
        chain_flush(w);

        /* One unit of cancellable work per page rotation (#156). This rotation gap is
         * the build path's ONLY cancellation point (ADR 0041), so it is also the only
         * place the build phase of a seal can be counted. Counted beside the check,
         * not by it: moving this check back inside a chain_write loop would make it
         * dead again while leaving the rotations happening, and the count -- and so
         * sql/80_maintenance_interrupts' assertion -- is what notices. */
        BM25_WORK_UNIT();
        CHECK_FOR_INTERRUPTS();

        newbuf = bm25_page_alloc(w->index, w->heaprel);   /* EXCL-locked */
        newblk = BufferGetBlockNumber(newbuf);

        if (oldblk != InvalidBlockNumber)
        {
            /* Backfill the link the old page's own record could not carry (see
             * above). Opened and closed entirely within this function, on a page
             * no other writer touches. */
            Buffer              oldbuf = ReadBuffer(w->index, oldblk);
            GenericXLogState   *ost;
            Page                opg;

            LockBuffer(oldbuf, BUFFER_LOCK_EXCLUSIVE);
            ost = GenericXLogStart(w->index);
            opg = GenericXLogRegisterBuffer(ost, oldbuf, 0);
            BM25PageGetOpaque(opg)->nextblk = newblk;
            GenericXLogFinish(ost);
            UnlockReleaseBuffer(oldbuf);
        }

        st = GenericXLogStart(w->index);
        np = GenericXLogRegisterBuffer(st, newbuf, GENERIC_XLOG_FULL_IMAGE);
        bm25_page_init(np, w->kind);
        BM25PageGetOpaque(np)->seg_gen = w->gen;   /* option (d): stamp the segment gen */
        w->state = st;
        w->tailbuf = newbuf;
        w->tailpage = np;
        if (w->root == InvalidBlockNumber)
            w->root = newblk;
    }
}

/* write `len` bytes; return start (blk, off within content). */
static void
chain_write(ChainWriter *w, const void *data, Size len,
            BlockNumber *out_blk, uint16 *out_off)
{
    Page    pg;
    char   *dst;

    /* "the whole write fits one page" is enforced (not merely asserted) inside
     * chain_ensure -- see CHAIN_PAGE_CAPACITY. */
    chain_ensure(w, len);
    pg = w->tailpage;
    dst = (char *) pg + ((PageHeader) pg)->pd_lower;
    *out_blk = BufferGetBlockNumber(w->tailbuf);
    /* Not a trust boundary like the reader-side pd_lower - SizeOfPageHeaderData
     * sites (bm25_page_content_bytes): w->tailpage was bm25_page_init'd by this
     * same writer earlier in this call, so pd_lower is under our own control, not
     * read back off an untrusted page. */
    *out_off = (uint16) (((PageHeader) pg)->pd_lower - SizeOfPageHeaderData);
    memcpy(dst, data, len);
    ((PageHeader) pg)->pd_lower += len;
    /* Content offset is measured from PageGetContents == page + SizeOfPageHeaderData. */
}

/* M4 POS chain: append `len` bytes that MAY span page boundaries (a position frame,
 * or even a single deltapos varbyte, can be larger than the room left on the tail page).
 * Unlike chain_write (whole-write-fits-one-page), this fills the current tail to the
 * brim, opens continuation pages, and keeps going -- so the reader must follow nextblk
 * mid-frame (which bm25_seg_scan_postings' POS cursor does). Returns the (blk, off) of
 * the FIRST byte written, which is the frame's start the DICT entry records. */
static void
chain_write_stream(ChainWriter *w, const void *data, Size len,
                   BlockNumber *out_blk, uint16 *out_off)
{
    const char *src = (const char *) data;
    Size        remaining = len;
    bool        first = true;

    /* Guarantee an open tail so we can report the start position even for len==0
     * (an empty frame never occurs -- tf>=1 -- but keep the writer total). */
    chain_ensure(w, 1);
    do
    {
        Page    pg = w->tailpage;
        Size    avail = ((PageHeader) pg)->pd_upper - ((PageHeader) pg)->pd_lower;
        Size    span;

        if (avail == 0)
        {
            chain_ensure(w, 1);             /* open a fresh continuation page */
            pg = w->tailpage;
            avail = ((PageHeader) pg)->pd_upper - ((PageHeader) pg)->pd_lower;
        }
        span = Min(avail, remaining);
        if (first)
        {
            *out_blk = BufferGetBlockNumber(w->tailbuf);
            /* Not a trust boundary (same reasoning as chain_write above): this
             * page's pd_lower was just set by this writer, either fresh from
             * bm25_page_init or by our own advance a few lines below, never read
             * back off an untrusted page. */
            *out_off = (uint16) (((PageHeader) pg)->pd_lower - SizeOfPageHeaderData);
            first = false;
        }
        if (span > 0)
        {
            memcpy((char *) pg + ((PageHeader) pg)->pd_lower, src, span);
            ((PageHeader) pg)->pd_lower += span;
            src += span;
            remaining -= span;
        }
    } while (remaining > 0);
}

/* bm25_segment_build_orphans -- Phase-1 of the two-phase commit, FACTORED OUT of
 * bm25_segment_build_and_commit so the seal publish and the merge swap publish
 * (Task 23) share ONE segment-page writer. It writes the DOCMAP/NORMS/LIVE/POST/
 * DICT chains + the segment header as ORPHAN pages (metapage untouched), stamps a
 * fresh monotonic gen on every page (option (d)), fills *hdr with the segment's
 * stats + chain roots + gen, and returns the header block. Nothing it writes is
 * reachable from a live structure: the CALLER's publish record is the
 * linearization point. Returns InvalidBlockNumber for an empty accumulator
 * (caller skips the publish).
 *
 * PRECONDITION (same as the publish caller): must run inside a transaction that
 * ABORTS on error. The Generic WAL windows here are NOT throw-free: the dict tail
 * window stays open across terms while palloc runs, and the chain writer's
 * chain_ensure raises PROGRAM_LIMIT_EXCEEDED for an oversized record. That is
 * accepted because every page is an unpublished orphan, so a throw discards private
 * state only; the builder relies on resource-owner cleanup to release the in-flight
 * buffer and discard the GenericXLogState rather than unwinding itself. ADR 0083's
 * "no throwing call in a window" rule is scoped to windows over PUBLISHED structure
 * (the publish and swap records below) by its addendum for #312, not this one.
 *
 * MEMORY (BUILD-03): everything this function allocates is scratch, and it used to
 * be palloc'd straight into the CALLER's context -- es_query_cxt on the aminsert
 * seal path, the SQL function's context on the merge path -- with no matching
 * frees, so each invocation stranded ~16 bytes x nterms plus a ~3 KB block buffer
 * plus the grown doclens/posbuf for the caller's whole lifetime. A private context
 * owns all of it now and is deleted on the normal return; the only things that
 * escape are *hdr's scalar fields and the returned block number, all by value.
 * The context is a child of CurrentMemoryContext, so the abort-on-error
 * precondition above still reclaims it on the throwing paths.
 *
 * This became load-bearing rather than merely tidy once the maintenance memory
 * budget landed: a budgeted build/merge/drain calls this function once per CHUNK
 * instead of once per operation, so the per-call leak multiplied with the chunk
 * count exactly when the operation was already large enough to be near a memory
 * ceiling. */
BlockNumber
bm25_segment_build_orphans(Relation index, Relation heaprel, BM25Accum *a,
                           BM25SegmentHeader *hdr)
{
    MemoryContext   buildcxt;       /* BUILD-03: owns every allocation below */
    MemoryContext   oldcxt;
    uint32          ndocs, nterms, i;
    uint32          field_count;
    uint32          gen;
    BlockNumber     docmap_root, norms_root, live_root, dict_root, header_blk;
    BlockNumber     posts_root;     /* D-POST: root of the single shared POST chain */
    BlockNumber     pos_root;       /* M4: root of the single shared POS chain (Invalid when off) */
    BlockNumber     keymap_root;    /* M5: docid->key chain root (Invalid when no key_field) */
    ChainWriter     wd, wn, wl, wp, wdict, wpos;
    BlockNumber    *term_post_root;
    uint16         *term_post_off;
    BlockNumber    *term_pos_root;  /* M4: per-term first-frame POS-chain block */
    uint16         *term_pos_off;   /* M4: per-term first-frame POS-chain content offset */
    uint32         *term_df;
    uint8          *blkbuf;
    uint8          *posbuf;         /* M4: scratch for one position frame */
    Size            poscap;         /* M4: current posbuf capacity */
    uint32         *doclens;        /* v5: one term's per-posting (doc,field) lengths */
    uint32          doclencap;      /* current doclens capacity, in elements */
    const uint8    *store_pos;      /* M4: per-field gate (NULL => all fields on) */
    bool            any_positions;  /* M4: false => every field off => no POS chain */
    /* #289: read once, so one segment is never written under both slicing rules. */
    bool            count_slicing = bm25_debug_count_slicing;

    bm25_accum_sort(a);
    ndocs = bm25_accum_ndocs(a);
    nterms = bm25_accum_nterms(a);
    field_count = bm25_accum_field_count(a);

    if (ndocs == 0)
        return InvalidBlockNumber;      /* empty build: leave 0 segments */

    /* BUILD-03: switch AFTER the empty bail-out (nothing has been allocated yet on
     * that path) and after bm25_accum_sort, which allocates nothing at all -- it
     * qsorts the accumulator's own array in place. Everything from here to the
     * hdr fill belongs to buildcxt, including the transient GenericXLogState each
     * chain writer pallocs and pfrees, and the scratch the helpers below take. */
    buildcxt = AllocSetContextCreate(CurrentMemoryContext,
                                     "bm25 segment builder scratch",
                                     ALLOCSET_DEFAULT_SIZES);
    oldcxt = MemoryContextSwitchTo(buildcxt);

    /* Option (d): assign this segment a fresh monotonic generation (read-and-bump
     * meta->next_gen). Every orphan page below is stamped with it so the reader can
     * detect a reclaimed-and-reused page; gens never repeat, so there is no ABA. */
    gen = bm25_next_gen(index);

    term_post_root = palloc(sizeof(BlockNumber) * Max(nterms, 1u));
    term_post_off  = palloc(sizeof(uint16) * Max(nterms, 1u));
    term_pos_root  = palloc(sizeof(BlockNumber) * Max(nterms, 1u));
    term_pos_off   = palloc(sizeof(uint16) * Max(nterms, 1u));
    term_df        = palloc(sizeof(uint32) * Max(nterms, 1u));
    blkbuf = palloc(sizeof(BM25BlockHeader) +
                    BM25_POSTINGS_PER_BLOCK * BM25_VARBYTE_MAX_BYTES * 2 +   /* docid + tf streams */
                    BM25_POSTINGS_PER_BLOCK * BM25_VARBYTE_MAX_BYTES * 2 +   /* field-id RLE worst case */
                    (1 + BM25_IMPACT_FIELD_BYTES * BM25_MAX_FIELDS));             /* v5 impact table worst case */

    /* M4: decide whether this segment stores positions at all. store_pos[field] gates
     * per posting; if EVERY field is off there is no POS chain (pos_root Invalid -- the
     * pre-M4 shape, so a phrase query degrades per D7). A NULL gate means all-on. */
    store_pos = bm25_accum_store_positions(a);
    any_positions = false;
    if (store_pos == NULL)
        any_positions = true;
    else
    {
        uint32 f;
        for (f = 0; f < field_count; f++)
            if (store_pos[f])
            {
                any_positions = true;
                break;
            }
    }
    /* One frame is [tf-count varbyte][deltapos varbyte x tf]; tf <= ndocs's field length,
     * unbounded a priori, so the frame is streamed page-spanningly. The scratch buffer
     * holds one BLOCK's worth of positions at a time is NOT enough (tf can exceed a
     * block); size it generously and grow per-posting if a posting's tf is larger. */
    posbuf = NULL;
    poscap = 0;

    /* Same grow-on-demand discipline for the POST pass's per-term doclens scratch:
     * ONE buffer reused across all nterms, grown to the largest term. It used to be
     * palloc'd fresh per term and never freed, which stranded 4 bytes per posting
     * for the whole build/seal/merge -- in the CALLER's context, back when this
     * function had none of its own (BUILD-03 gave it buildcxt above). The reuse is
     * still worth having: buildcxt bounds the lifetime, this bounds the peak. */
    doclens = NULL;
    doclencap = 0;

    /* ---- Phase 1: orphan pages (metapage untouched) ---- */

    /* DOCMAP: packed ItemPointerData[docid] */
    chain_open(&wd, index, heaprel, BM25_PAGE_DOCMAP, gen);
    /* No per-item CHECK_FOR_INTERRUPTS here: chain_write holds wd's tail buffer
     * content-locked across many iterations by design, and LWLockAcquire calls
     * HOLD_INTERRUPTS() -- a check anywhere in this loop would be a silent
     * no-op (caught in review; see chain_ensure's header comment for the actual
     * fix). Cancellability for every chain this function writes now comes from
     * chain_ensure's own check, once per page rotation. */
    for (i = 0; i < ndocs; i++)
    {
        BlockNumber b;
        uint16      o;

        chain_write(&wd, bm25_accum_doc_tid(a, i), sizeof(ItemPointerData), &b, &o);
    }
    docmap_root = wd.root;
    chain_flush(&wd);

    /* NORMS: packed per-field uint32 doclen, row-major by docid -- cell
     * [docid*field_count + field_id]. field_count == 1 collapses to the M3
     * layout (one uint32 per doc, indexed by docid). C3 reads per-field
     * doclen_f from this; a doc that lacks a field has a 0 cell. */
    chain_open(&wn, index, heaprel, BM25_PAGE_NORMS, gen);
    /* Same reasoning as the DOCMAP loop above: no per-item check, wn's tail stays
     * locked across iterations. */
    for (i = 0; i < ndocs; i++)
    {
        uint32 f;

        for (f = 0; f < field_count; f++)
        {
            uint32 dl = bm25_accum_doc_len_field(a, i, f);
            BlockNumber b;
            uint16      o;

            chain_write(&wn, &dl, sizeof(uint32), &b, &o);
        }
    }
    norms_root = wn.root;
    chain_flush(&wn);

    /* LIVE: ceil(ndocs/8) bytes, all bits set (all live at seal) */
    chain_open(&wl, index, heaprel, BM25_PAGE_LIVE, gen);
    {
        uint32  nbytes = (ndocs + 7) / 8;
        uint8  *bits = palloc(nbytes);
        BlockNumber b;
        uint16      o;

        memset(bits, 0xFF, nbytes);
        if (ndocs % 8 != 0)             /* clear pad bits in last byte */
            bits[nbytes - 1] &= (uint8) ((1u << (ndocs % 8)) - 1);
        /* LIVE bitmap may exceed one page for huge segments; chain_write splits
         * only on page boundaries, so write in page-sized spans. */
        {
            uint32 maxspan = BLCKSZ - SizeOfPageHeaderData -
                             MAXALIGN(sizeof(BM25PageOpaque));
            uint32 off = 0;
            while (off < nbytes)
            {
                uint32 span = Min(maxspan, nbytes - off);
                chain_write(&wl, bits + off, span, &b, &o);
                off += span;
            }
        }
        pfree(bits);
    }
    live_root = wl.root;
    chain_flush(&wl);

    /* POST: per term, block-encode postings into a chain shared by all terms. */
    chain_open(&wp, index, heaprel, BM25_PAGE_POST, gen);
    for (i = 0; i < nterms; i++)
    {
        const uint32   *docids;
        const uint32   *tfs;
        const uint32   *field_ids;   /* C2.5 feeds this into encode_block */
        uint32          n, off, cnt;
        int             termlen;
        uint32          df;
        bool            first = true;

        /* Pre-set the term's posting pointer to a defined "no blocks" value (BUILD-07).
         * term_post_root/_off are palloc'd, not palloc0'd, and are assigned ONLY inside
         * the block loop below under `first`. A term with zero postings never enters
         * that loop, so both stayed uninitialized and were copied into the DICT entry's
         * post_root/post_off -- onto a WAL-logged page, making the entry differ between
         * two byte-identical builds and putting stack residue in the WAL stream.
         * InvalidBlockNumber is the right sentinel: it is what every reader already
         * tests for "this term has no chain here", and it is what the sibling POS pass
         * in this same function writes for a term that produced no frame. */
        term_post_root[i] = InvalidBlockNumber;
        term_post_off[i]  = 0;

        /* No per-term/per-block CHECK_FOR_INTERRUPTS in this loop nest: wp's tail
         * buffer stays content-locked across many terms' worth of blocks (same
         * reasoning as DOCMAP/NORMS above), which also makes the doclens-fill
         * loop below dead despite touching no buffer of its own -- it runs while
         * a PRIOR chain_write's tail lock from an earlier term is still held.
         * chain_ensure's own check now covers the whole POST chain. */
        (void) bm25_accum_term(a, i, &termlen, &df);
        term_df[i] = df;
        /* The three arrays are the accumulator's REUSED scratch, valid only until
         * the next bm25_accum_term_postings call -- fine here, because this iteration
         * consumes them fully (only BlockNumbers escape into term_post_root/_off).
         * field_ids is never NULL, even for a zero-posting term: the accumulator
         * floors its capacity at 1 element. Same non-NULL producer contract
         * encode_block asserts below. */
        bm25_accum_term_postings(a, i, &docids, &tfs, &field_ids, &n);

        if (Max(n, 1u) > doclencap)
        {
            doclencap = Max(n, 1u);
            doclens = (doclens == NULL)
                ? palloc(sizeof(uint32) * doclencap)
                : repalloc(doclens, sizeof(uint32) * doclencap);
        }
        for (off = 0; off < n; off++)
            doclens[off] = bm25_accum_doc_len_field(a, docids[off], field_ids[off]);

        /* Issue #289: end each block at a DOCUMENT boundary, so one document's
         * postings for this term (adjacent, one per field) never straddle two
         * blocks. A block-max bound covers only the postings its own block holds,
         * so a straddler was under-bounded by both of its blocks and WAND could
         * prune it unscored. Cutting every BM25_POSTINGS_PER_BLOCK postings, as
         * this loop used to, split a multi-field document whenever the boundary
         * fell inside it.
         *
         * The cut backs off to the start of the document the count boundary would
         * split. A block therefore holds between BM25_POSTINGS_PER_BLOCK -
         * (BM25_MAX_FIELDS - 1) and BM25_POSTINGS_PER_BLOCK postings (the last
         * block of the run, fewer): never more than 128, which every reader
         * already accepts (bm25_block_validate) and no reader assumes is full, so
         * this is not a format change. Allowing a block to grow past 128 to finish
         * the document instead WOULD be one (ndocs > 128 is refused as corrupt).
         *
         * Old segments keep their straddles until a merge rewrites them, and the
         * reader cannot tell the layouts apart, so bm25_wand.c handles straddles on
         * every segment regardless; bm25_native.debug_count_slicing restores the
         * old rule so the suite can still build one. */
        for (off = 0; off < n; off += cnt)
        {
            Size        blen;
            BlockNumber b;
            uint16      o;

            cnt = Min(BM25_POSTINGS_PER_BLOCK, n - off);
            if (!count_slicing)
            {
                /* A block starts at a document boundary (the previous cut put it
                 * there), so backing off cannot pass its first document unless
                 * that document alone holds more than BM25_POSTINGS_PER_BLOCK
                 * postings for the term -- impossible, one posting per field. */
                while (cnt > 0 && off + cnt < n && docids[off + cnt - 1] == docids[off + cnt])
                    cnt--;
                if (cnt == 0)
                    elog(ERROR, "bm25: document %u holds more than %d postings for one term",
                         docids[off], BM25_POSTINGS_PER_BLOCK);
            }

            blen = encode_block(docids + off, tfs + off, field_ids + off,
                                doclens + off, cnt, field_count, blkbuf);
            chain_write(&wp, blkbuf, blen, &b, &o);
            if (first)
            {
                term_post_root[i] = b;
                term_post_off[i] = o;
                first = false;
            }
        }
    }
    chain_flush(&wp);
    posts_root = wp.root;       /* D-POST: the single shared POST chain root */

    /* POS (M4): a SEPARATE chain, structurally parallel to POST. One frame per
     * position-bearing posting -- same posting order the POST scan emits -- laid
     * consecutively for a term's df postings. A frame is [tf-count varbyte][deltapos
     * varbyte x tf]; deltapos resets per frame (per (doc,field)), first delta = pos[0].
     * A posting on a positions-off field emits NO frame; the reader gates identically
     * on the field's store_positions bit, so the streams stay in lockstep. term_pos_*
     * records the FIRST frame of the term's run (Invalid if the term is only ever in
     * off-fields -- its pos_post_root then stays Invalid). Skipped entirely when
     * any_positions is false (no field stores positions => no POS chain, pos_root
     * Invalid => the pre-M4 shape). */
    pos_root = InvalidBlockNumber;
    if (any_positions)
    {
        chain_open(&wpos, index, heaprel, BM25_PAGE_POS, gen);
        for (i = 0; i < nterms; i++)
        {
            const uint32   *docids;
            const uint32   *tfs;
            const uint32   *field_ids;
            uint32          n, j;
            bool            first = true;

            /* No per-term/per-posting check here either, same reasoning as the
             * POST loop above -- wpos's tail stays locked across many postings
             * and terms; chain_ensure covers the POS chain too. */
            term_pos_root[i] = InvalidBlockNumber;
            term_pos_off[i]  = 0;
            /* Reused accumulator scratch again (see the POST loop). Safe: this pass
             * runs only AFTER the POST pass has fully drained nterms, and this
             * iteration consumes the arrays before requesting the next term. The
             * `positions` array below is NOT this scratch -- it points at the
             * posting's own pos[] inside the accumulator, so it is unaffected. */
            bm25_accum_term_postings(a, i, &docids, &tfs, &field_ids, &n);

            for (j = 0; j < n; j++)
            {
                uint32          fid = field_ids[j];
                const uint32   *positions;
                uint32          npos, k, prev, needbytes;
                uint8          *p;
                BlockNumber     b;
                uint16          o;

                /* Per-field gate: an off-field posting carries no frame (D12). */
                if (store_pos != NULL && !store_pos[fid])
                    continue;

                positions = bm25_accum_posting_positions(a, i, j, &npos);
                /* D2 self-check on the WRITE side: one position per token occurrence. */
                Assert(npos == tfs[j]);   /* invariant */

                /* Grow the scratch frame buffer to worst case for this posting:
                 * (tf-count + npos varbytes) * 5 bytes each. */
                needbytes = (npos + 1) * BM25_VARBYTE_MAX_BYTES;
                if ((Size) needbytes > poscap)
                {
                    poscap = (Size) needbytes;
                    posbuf = (posbuf == NULL) ? palloc(poscap) : repalloc(posbuf, poscap);
                }

                p = posbuf;
                p += bm25_varbyte_encode(npos, p);      /* tf-count */
                prev = 0;
                for (k = 0; k < npos; k++)
                {
                    uint32 delta = positions[k] - prev; /* first delta = pos[0] - 0 */
                    p += bm25_varbyte_encode(delta, p);
                    prev = positions[k];
                }
                chain_write_stream(&wpos, posbuf, (Size) (p - posbuf), &b, &o);
                if (first)
                {
                    term_pos_root[i] = b;
                    term_pos_off[i]  = o;
                    first = false;
                }
            }
        }
        chain_flush(&wpos);
        pos_root = wpos.root;
    }

    /* DICT: sorted BM25DictEntry + inline term, MAXALIGN-strided. */
    chain_open(&wdict, index, heaprel, BM25_PAGE_DICT, gen);
    for (i = 0; i < nterms; i++)
    {
        int             termlen;
        uint32          df;
        const char     *term;
        Size            esz;
        char           *rec;
        BM25DictEntry  *e;
        BlockNumber     b;
        uint16          o;

        /* No per-term check: wdict's tail stays locked across many terms, same
         * reasoning as the other chain_write loops above. */
        term = bm25_accum_term(a, i, &termlen, &df);
        esz = MAXALIGN(sizeof(BM25DictEntry) + termlen);
        rec = palloc(esz);
        e = (BM25DictEntry *) rec;

        memset(rec, 0, esz);
        e->df = term_df[i];
        e->post_root = term_post_root[i];
        e->post_off = term_post_off[i];
        e->termlen = (uint16) termlen;
        /* M4: the term's POS-chain entry (first frame). Invalid when the segment
         * stores no positions OR the term appears only in positions-off fields (no
         * frame was written). 0 is a valid block number, so set Invalid explicitly. */
        e->pos_post_root = any_positions ? term_pos_root[i] : InvalidBlockNumber;
        e->pos_post_off  = any_positions ? term_pos_off[i]  : 0;
        e->dict_pad      = 0;
        memcpy(rec + sizeof(BM25DictEntry), term, termlen);
        chain_write(&wdict, rec, esz, &b, &o);
        pfree(rec);
    }
    dict_root = wdict.root;
    chain_flush(&wdict);

    /* KEYMAP: docid->key flat array (one more orphan chain), only when a key_field
     * is configured (the accumulator carries key_type/key_size and the dense keys[]).
     * Returns InvalidBlockNumber when key_type==NONE, giving the ctid-fallback path
     * with zero pages written -- byte-identical to a keyless index. The merge inherits
     * this for free because it routes through the same orphan builder (its accumulator
     * carries the source keys, read via bm25_seg_reader_key). The writer stays inside the
     * 4-buffer cap -- at most TWO buffers registered, in the one record that links a
     * full page to its successor and inits that successor. (PEND-10, issue #145: this
     * comment used to say "one buffer registered at a time (the writer flushes each
     * page before the next)", repeating a claim the writer's own header made and did
     * not honour -- it allocated the successor, EXCL-locked, inside the predecessor's
     * still-open Generic WAL window. Both comments were corrected together; see
     * bm25_keymap_write's header for the record sequence it actually emits.) */
    keymap_root = bm25_keymap_write(index, heaprel, gen,
                                    bm25_accum_key_type(a),
                                    bm25_accum_key_size(a),
                                    bm25_accum_doc_key(a, 0), ndocs);

    /* Segment header page (the segment "root"). */
    {
        Buffer              buf = bm25_page_alloc(index, heaprel);
        GenericXLogState   *st = GenericXLogStart(index);
        Page                pg = GenericXLogRegisterBuffer(st, buf, GENERIC_XLOG_FULL_IMAGE);
        BM25SegmentHeader  *h;

        bm25_page_init(pg, BM25_PAGE_SEGCAT);   /* header is segcat-kind page */
        BM25PageGetOpaque(pg)->seg_gen = gen;    /* option (d): stamp the header page too */
        h = (BM25SegmentHeader *) PageGetContents(pg);
        h->gen = gen;                            /* this segment's generation (matches catalog) */
        h->ndocs = ndocs;
        h->total_len = bm25_accum_total_len(a);
        /* Runs above, TOKENS here -- different quantities since analyzer revision 5
         * (ADR 0087). Saturating, because the field is a uint32 living in what used
         * to be padding: a segment past 4.29e9 tokens is refused by the merge budget
         * on either number, and the estimator's Max(stored, total_len) keeps a
         * saturated value from ever charging LESS than the run count. */
        h->total_tokens = (uint32) Min(bm25_accum_total_tokens(a),
                                       (uint64) PG_UINT32_MAX);
        h->nterms = nterms;
        h->dict_root = dict_root;
        h->norms_root = norms_root;
        h->livedocs_root = live_root;
        h->docmap_root = docmap_root;
        h->posts_root = posts_root;     /* D-POST: the single shared POST chain root */

        /* ---- v4/M5 field dimension (M4 fills pos_root) ---- */
        h->field_count  = field_count;
        h->pos_root     = pos_root;             /* M4: POS chain root, Invalid when no field stores positions */
        h->keymap_root  = keymap_root;          /* M5: real root when key_field, else Invalid */

        /* Length-prefixed per-field arrays immediately after the struct (section H):
         * field_count uint64 per-field sumdoclen, then (when field_count > 1) the
         * field_count uint64 per-field N_field (live docs that HAVE the field --
         * BM25F avgdl_field = sumlen_field / N_field). The helper loops
         * h->field_count and returns the pd_lower offset covering the struct AND
         * the array(s). field_count == 1 writes exactly one uint64 == total_len and
         * NO ndocs_by_field[] (M3/C-BUILD byte layout, no REINDEX). */
        {
            uint64 total_len_by_field[BM25_MAX_FIELDS];
            uint64 ndocs_by_field[BM25_MAX_FIELDS];
            bm25_accum_total_len_by_field(a, total_len_by_field);
            bm25_accum_ndocs_by_field(a, ndocs_by_field);
            ((PageHeader) pg)->pd_lower =
                bm25_segheader_write_lenfields(pg, h, total_len_by_field,
                                               ndocs_by_field);
        }
        header_blk = BufferGetBlockNumber(buf);
        GenericXLogFinish(st);
        UnlockReleaseBuffer(buf);
    }

    /* Hand the segment's stats + chain roots back to the caller's publish record.
     * hdr->gen is the gen we stamped above; the caller (seal or swap) reuses it in
     * the catalog entry so the entry gen matches the page stamps (option (d)). */
    hdr->gen           = gen;
    hdr->ndocs         = ndocs;
    hdr->total_len     = bm25_accum_total_len(a);
    hdr->total_tokens  = (uint32) Min(bm25_accum_total_tokens(a),
                                      (uint64) PG_UINT32_MAX);   /* see the on-page copy above */
    hdr->nterms        = nterms;
    hdr->dict_root     = dict_root;
    hdr->norms_root    = norms_root;
    hdr->livedocs_root = live_root;
    hdr->docmap_root   = docmap_root;
    hdr->posts_root    = posts_root;
    hdr->field_count   = field_count;
    hdr->pos_root      = pos_root;              /* M4: POS chain root, Invalid when no field stores positions */
    hdr->keymap_root   = keymap_root;           /* M5: real root when key_field, else Invalid */

    /* BUILD-03: every field written above is a scalar copied by value, and
     * header_blk is a BlockNumber, so nothing in buildcxt is reachable after this
     * point. Switch back BEFORE the delete so the caller is never left standing in
     * a destroyed context. */
    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(buildcxt);
    return header_blk;
}

/* bm25_segcat_entry_from_hdr -- fill ONE catalog entry from a freshly built
 * segment's header. The single constructor for a BM25SegCatEntry, shared by the
 * append (seal/build) publish and the swap (merge) publish.
 *
 * MEMSET BEFORE FILL (XCUT-01/BUILD-02). BM25SegCatEntry used to carry a 4-byte
 * hole at offset 4 -- BlockNumber header_blkno at 0, then uint64 ndocs needing
 * 8-alignment -- that no assignment touched; ADR 0088 named it total_tokens, so every
 * byte is now assigned below and the memset is defence in depth against a member
 * reorder reintroducing padding (bm25_format.h, next to the size pin). It stays
 * because the stakes have not changed: both publish paths get this struct onto a
 * WAL-logged page by byte-exact memcpy, and every catalog reader copies the full
 * sizeof, so any padding byte would reach disk, be carried into the snapshot array
 * and be re-emitted onto the next merge's fresh chain for the life of the segment.
 * Zeroing keeps the entry a deterministic function of the header, which is also what
 * keeps 96_wal_page_determinism honest.
 *
 * live_ndocs == ndocs unconditionally: a freshly built segment has no tombstones,
 * whether it came from a build chunk, a drained pending chain, or a merge (which
 * drops dead docs as it re-accumulates). */
void
bm25_segcat_entry_from_hdr(BM25SegCatEntry *e, BlockNumber header_blkno,
                           const BM25SegmentHeader *hdr)
{
    memset(e, 0, sizeof(*e));
    e->header_blkno = header_blkno;
    /* Copied from the header's immutable master. The entry's copy is the one the
     * tombstone path decays; the header's is not (ADR 0088). */
    e->total_tokens = hdr->total_tokens;
    e->ndocs        = hdr->ndocs;
    e->live_ndocs   = hdr->ndocs;
    e->total_len    = hdr->total_len;
    e->nterms       = hdr->nterms;
    /* Option (d): reuse the gen bm25_segment_build_orphans already stamped on every
     * orphan page (hdr->gen) so the catalog entry's gen matches the page stamps;
     * recomputing it here would make the reader reject every read. */
    e->gen          = hdr->gen;
}

/* BM25SegCatEntry slots on one BM25_PAGE_SEGCAT page (about 203 at BLCKSZ 8192).
 * Derived, never a literal -- see the same arithmetic in
 * bm25_segcat_build_orphan_chain, which this now shares. */
static int
bm25_segcat_entries_per_page(void)
{
    return (int) ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData)
                   - MAXALIGN(sizeof(BM25PageOpaque)))
                  / MAXALIGN(sizeof(BM25SegCatEntry)));
}

/* bm25_segcat_publish_append -- Phase 2 of the seal/build commit: ONE record that
 * appends `n` already-built catalog entries AND resets the pending anchor.
 *
 * PRECONDITION: must run inside a transaction that ABORTS on error -- there must be
 * no enclosing PG_TRY/PG_CATCH that swallows an error and resumes. The open Generic
 * WAL window here contains no throwable calls, so on error this relies on
 * resource-owner cleanup to release the in-flight buffers and discard the
 * GenericXLogState rather than unwinding the window itself; a resuming PG_CATCH
 * would leak the EXCL-locked buffer for the rest of the txn.
 *
 * WHY N ENTRIES AND NOT N RECORDS (BUILD-04). A budgeted drain produces several
 * chunk segments from ONE pending chain, and publishing them one record at a time
 * would mean committed states in which some chunks are segments while the whole
 * chain is still anchored -- and a scan snapshots pending_head and the catalog under
 * one metapage lock and then SUMS per-TID contributions with no cross-source dedup
 * (bm25_scores_add, bm25_stats.c), so every such state double-scores and
 * double-returns the already-published docs. Durably, if a crash lands there. One
 * record for all N is the same D-SEAL/C2 argument the single-entry version made,
 * generalized: the linearization point is one record, not one segment.
 *
 * Cheap, too, and unbounded in N: entries that fit one catalog page are written
 * straight into this record, and a batch too large for one page is laid on a fresh
 * orphan CHAIN beforehand and prepended by pointing its tail at the old root. Either
 * way the record registers metapage + one catalog page = 2 buffers regardless of N.
 *
 * n == 0 publishes nothing and only detaches the drained chain (issue #131's C1):
 * the recycle that follows the caller is gated on whether a chain was drained, not
 * on whether anything survived it, so "publish skipped, recycle not skipped" is
 * exactly the dangerous shape and the anchor must be reset either way. */
void
bm25_segcat_publish_append(Relation index, Relation heaprel,
                           const BM25SegCatEntry *entries, int n,
                           BlockNumber drained_head)
{
    BM25MetaPageData    meta;
    Buffer              metabuf;
    Buffer              catbuf;
    GenericXLogState   *st;
    Page                metapage;
    Page                catpage;
    BM25MetaPageData   *m;
    BlockNumber         chain_head = InvalidBlockNumber;
    BlockNumber         chain_tail = InvalidBlockNumber;
    bool                fresh_page;     /* this record initializes catbuf's page */
    bool                link_old_root;  /* catbuf's page must point at the old root */
    bool                append_here;    /* this record writes the entries itself */
    uint64              add_ndocs = 0;
    uint64              add_total_len = 0;
    int                 i;

    Assert(n >= 0);   /* checked: callers pass a counted array length */
    if (n == 0)
    {
        /* Nothing to publish. Gated on drained_head so the ambuild caller (pending
         * already empty, drained_head Invalid) does not emit a pointless metapage
         * record. */
        if (drained_head != InvalidBlockNumber)
            bm25_pending_reset_anchor(index);
        return;
    }

    /* THERE IS NO CEILING ON n, and there must not be. An earlier version of this
     * function refused n greater than one page's worth of entries, on the reasoning
     * that only a pathological bm25_native.debug_budget could produce that many
     * chunks out of one drain. That was wrong twice over. maintenance_work_mem is
     * PGC_USERSET with a 1 MB floor, and a few thousand large documents drained at
     * that floor really does produce a thousand chunks -- reproduced. And the
     * failure was not merely an error: by the time it fired, every one of those
     * chunks had already been built and WAL-logged as orphan pages, the abort left
     * the pending anchor untouched so the next seal did exactly the same thing, and
     * bm25_vacuumcleanup seals BEFORE it reclaims orphans, so the sweep that would
     * have freed them was never reached. Measured: 344 MB of stranded pages after
     * two attempts, zero segments published, latching. That is precisely the
     * "index silently stops being maintained" wedge the memory budget exists to
     * remove, reached through a different door.
     *
     * So when the batch cannot fit ONE catalog page, lay it on a FRESH ORPHAN CHAIN
     * and prepend the WHOLE CHAIN -- the same indirection the merge swap has always
     * used, which is why the machinery was already here. Built BEFORE any lock is
     * taken: each page is its own small record, and holding the metapage across a
     * run of them is the lock-hold discipline ADR 0083 forbids. The flip record
     * below still registers exactly 2 buffers (metapage + the chain's TAIL, whose
     * nextblk is pointed at the old root there), so the record's cost is independent
     * of n in this case exactly as it is in the others.
     *
     * Deliberately not clever: this path does not first fill whatever room the
     * current root page has. A batch this size is already the exceptional case, and
     * a partial fill would mean writing entries in two records. */
    if (n > bm25_segcat_entries_per_page())
        chain_head = bm25_segcat_build_orphan_chain(index, heaprel, (BM25SegCatEntry *) entries,
                                                    n, &chain_tail);

    /* Global-stat deltas come from the entry array either way, so they are summed
     * here rather than inside the record's append loop, which the chain case skips. */
    for (i = 0; i < n; i++)
    {
        add_ndocs     += entries[i].ndocs;
        add_total_len += entries[i].total_len;
    }

    /* ---- ONE record publishes the segments AND advances the pending head
     * (linearization point) ---- *
     * Append n BM25SegCatEntry to the segment-catalog chain, update global stats,
     * AND reset the pending-list anchor (so the just-drained docs leave the
     * pending list) -- all in a single Generic XLog finish. The metapage is
     * already exclusive in this record, so advancing pending_head here is free.
     *
     * D-SEAL / C2: publish-then-truncate as two separate WAL records is a
     * correctness bug. A crash between them would leave the docs counted in BOTH
     * the published segment and the still-anchored pending list (double-counted
     * global stats; a re-seal would double-publish). Unlike GIN's pending-list
     * redo, re-sealing here is NOT idempotent (segment publish is additive, not
     * a bitmap OR), so the seal MUST be one record. Resetting pending_head to
     * the drained snapshot is safe because the seal caller holds the GIN-style
     * LockPage singleton across drain+build+commit (D-SEAL / M5), so no
     * concurrent append can land between the drain and this record.
     *
     * At CREATE INDEX time pending_head is already Invalid, so the reset below is a
     * no-op; at seal time it discards the drained pending chain (its pages become
     * orphans reclaimed by Phase-3/4 VACUUM, exactly as bm25_pending_truncate used
     * to do -- but now atomically with the publish). */
    {
        /*
         * INDEX-WIDE BUFFER LOCK ORDER: metapage BEFORE segment-catalog page.
         *
         * bm25_scan_snapshot holds the metapage SHARE across its whole catalog
         * walk, taking each SEGCAT page SHARE inside it -- it has to, because that
         * held metapage lock is exactly what stops a publish moving the chain
         * under the copy. Buffer content locks are LWLocks: no deadlock detector,
         * not cancel-interruptible. This record used to take the catalog page
         * EXCLUSIVE first and the metapage second, so an ordinary concurrent
         * INSERT-that-seals and SELECT could wedge both backends until SIGKILL.
         * The reader's direction is the one that cannot change, so the writers
         * move. Take the metapage first and read the anchor from under it.
         */
        metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        /* _locked runs the full format gate itself (magic, then the version floor
         * this site used to re-check with its own bm25_meta_validate call, now folded
         * in so the pending-append path cannot bypass it). It is a pure function of
         * the copied struct, and no WAL window is open yet, so an ereport out of it
         * just unwinds and drops the buffer lock. */
        bm25_meta_read_locked(metabuf, &meta);

        /*
         * Do all n new entries fit on the current root page?
         *
         * NOTHING bounds the segment count between VACUUMs: bm25_merge_maybe runs
         * only from amvacuumcleanup and the manual bm25_merge() SQL, and
         * BM25_TARGET_SEGMENT_COUNT is a merge-selection target, not a seal-time
         * gate. So a root page that holds 203 entries genuinely fills -- by repeated
         * bm25_seal(), or by the opportunistic aminsert seal on an insert-heavy
         * table with autovacuum lagging -- and the 204th entry used to be written
         * straight through BM25PageOpaque and past the end of the buffer, leaving
         * pd_lower > pd_upper for GenericXLogFinish to WAL-log. The test is against
         * n entries, not one, since a budgeted drain publishes a batch.
         *
         * When they do not fit, PREPEND a fresh page: the new page becomes the
         * root, its nextblk points at the old root, and meta->segcat_root flips --
         * all inside this one record. Prepending (rather than walking to the tail)
         * keeps the record at 2 buffers and never touches the old root at all. The
         * pre-window check above guarantees all n fit the fresh page.
         * Catalog order carries no meaning: readers copy entries in chain order
         * until they have nsegs of them (bm25_seg_read.c), and the merge already
         * rebuilds the chain wholesale via bm25_segcat_build_orphan_chain.
         */
        if (chain_head != InvalidBlockNumber)
        {
            /* The chain already carries every entry and every page is already
             * initialized; this record only has to link its far end to the old root
             * and flip. Locked AFTER the metapage, per the order above -- the pages
             * are unreachable orphans so nothing can contend, but the rule is the
             * rule. */
            catbuf = ReadBuffer(index, chain_tail);
            LockBuffer(catbuf, BUFFER_LOCK_EXCLUSIVE);
            fresh_page    = false;
            link_old_root = true;
            append_here   = false;
        }
        else
        {
        fresh_page = (meta.segcat_root == InvalidBlockNumber);
        if (!fresh_page)
        {
            Page    rootpage;

            /* Issue #302.A. segcat_root is not block 0 (bm25_meta_read_locked's gate
             * refuses it: ReadBuffer + LockBuffer would wait forever on the metapage
             * lock held above). It may still name a page of another kind, or a segment
             * header page, which is SEGCAT too; the entries would then be written into
             * that page and WAL-logged. So the root's content bounds, kind and catalog
             * role (seg_gen == 0) are checked here, under its lock and before
             * GenericXLogStart, which also makes the capacity test below compare a
             * validated pd_lower. */
            catbuf = ReadBuffer(index, meta.segcat_root);
            LockBuffer(catbuf, BUFFER_LOCK_EXCLUSIVE);
            rootpage = BufferGetPage(catbuf);
            (void) bm25_segcat_page_validate(rootpage, BM25_SEGCAT_ROLE_CATALOG,
                                             meta.segcat_root);
            if (((PageHeader) rootpage)->pd_lower
                + (Size) n * MAXALIGN(sizeof(BM25SegCatEntry))
                > ((PageHeader) rootpage)->pd_upper)
            {
                UnlockReleaseBuffer(catbuf);
                fresh_page = true;
            }
        }
        link_old_root = fresh_page;
        append_here   = true;
        if (fresh_page)
        {
            /* Allocating under the held metapage lock is sanctioned (D-ALLOC/M6):
             * the stamp-and-gate allocator only touches index buffers via
             * ReadBuffer + ConditionalLockBuffer and never waits on another
             * buffer's content lock -- the same reason bm25_pending_append_multi
             * can call it under this lock. (XCUT-09, issue #145: "never blocks"
             * was too strong as a general claim, and the allocator takes no
             * relation-extension lock either -- P_NEW skips it -- so it is the
             * seal singleton this path already holds, not any lock inside the
             * allocator, that keeps relation extends single-threaded. See
             * bm25_pending_append_multi's header for the full statement.) */
            catbuf = bm25_page_alloc(index, heaprel);    /* fresh orphan catalog page */
        }
        }

        /* Metapage registered FIRST: generic_redo locks the blocks EXCLUSIVE in
         * registration order and holds them until the record is applied, so on a
         * standby this order IS the lock order, and the in-place branch appends to
         * the live segcat_root a replica scan may hold SHARE under the metapage
         * (ADR 0018, issue #240). */
        st = GenericXLogStart(index);
        metapage = GenericXLogRegisterBuffer(st, metabuf, 0);
        catpage = GenericXLogRegisterBuffer(st, catbuf,
                    fresh_page ? GENERIC_XLOG_FULL_IMAGE : 0);

        if (fresh_page)
            bm25_page_init(catpage, BM25_PAGE_SEGCAT);
        if (link_old_root)
            /* Link the rest of the chain behind the new root. Invalid when this is
             * the very first catalog page, which is what page_init already set.
             * Read from the LOCKED metapage, not from the unlocked bm25_meta_read
             * copy above -- the seal singleton makes them equal today, but taking
             * the authoritative one costs nothing. In the chain case this is the
             * chain's TAIL, so the whole chain is prepended in front of the live
             * catalog by this one assignment. */
            BM25PageGetOpaque(catpage)->nextblk =
                BM25PageGetMeta(metapage)->segcat_root;

        /* Append the entries at pd_lower, MAXALIGN-strided (the on-page stride; the
         * array-indexed readers agree only because bm25_format.h pins the struct
         * MAXALIGN-clean). Byte-exact memcpy, so every byte reaching disk is one
         * bm25_segcat_entry_from_hdr assigned or zeroed -- see that function.
         * Skipped in the chain case: bm25_segcat_build_orphan_chain already wrote
         * them, as one bulk array copy that lands on the same offsets for the same
         * reason. */
        if (append_here)
            for (i = 0; i < n; i++)
            {
                memcpy((char *) catpage + ((PageHeader) catpage)->pd_lower,
                       &entries[i], sizeof(BM25SegCatEntry));
                ((PageHeader) catpage)->pd_lower += MAXALIGN(sizeof(BM25SegCatEntry));
            }

        m = BM25PageGetMeta(metapage);
        /* Flip the root whenever this record created or prepended a page -- the
         * first catalog page, a single prepended page, or the head of a prepended
         * chain (whose TAIL is the page registered in this record). */
        if (chain_head != InvalidBlockNumber)
            m->segcat_root = chain_head;
        else if (fresh_page)
            m->segcat_root = BufferGetBlockNumber(catbuf);
        m->nsegs += n;
        m->ndocs += add_ndocs;
        m->total_len += add_total_len;
        /* D-SEAL / C2: advance the pending head in THIS record. The drained docs
         * are now in the published segments, so they must leave the pending list
         * atomically with the publish -- never as a separate truncate record.
         * No-op at CREATE INDEX (pending already empty); at seal time it drops
         * the drained chain (pages become orphans reclaimed by VACUUM).
         *
         * Resetting FULLY (rather than to whatever the drain actually consumed) is
         * safe only because no append can have raced the drain, and that holds only
         * because bm25_pending_append_multi takes the same LockPage singleton in
         * ShareLock mode. Until it did, this reset silently DISCARDED any document
         * appended after the drain walked past the tail page: the metapage buffer
         * lock the appender held is a different lock manager from LockPage and never
         * conflicted with it. If the append-side lock is ever removed, this reset
         * must become a partial one -- see the note there. */
        m->pending_head = InvalidBlockNumber;
        m->pending_tail = InvalidBlockNumber;
        m->pending_tail_free = 0;
        m->pending_npages = 0;
        m->pending_ndocs = 0;
        bm25_meta_set_pd_lower(metapage);

        GenericXLogFinish(st);
        UnlockReleaseBuffer(metabuf);
        UnlockReleaseBuffer(catbuf);
    }
}

/* Build one sealed segment from the accumulator and publish it -- the N == 1 case
 * of the two-phase commit, kept as its own entry point because it is what the
 * ambuild chunk publish and the single-chunk seal call. PRECONDITION: as
 * bm25_segcat_publish_append's.
 *
 * D-SEAL: Phase 1 always drains the WHOLE pending list, so the publish record
 * resets pending_head/tail to Invalid unconditionally; the drained_head argument
 * records exactly which head was drained, for the caller's post-commit
 * bm25_pending_truncate page-recycle. The Phase-1 full reset does not consult it --
 * the ONLY use is the empty-build bail-out, which needs to know whether a chain was
 * detached. */
void
bm25_segment_build_and_commit(Relation index, Relation heaprel, BM25Accum *a,
                              BlockNumber drained_head)
{
    BM25SegmentHeader   hdr;
    BlockNumber         header_blk;
    BM25SegCatEntry     entry;
    int                 n = 0;

    /* ---- Phase 1: build the segment as orphan pages (shared with the merge). */
    header_blk = bm25_segment_build_orphans(index, heaprel, a, &hdr);
    if (header_blk != InvalidBlockNumber)
    {
        bm25_segcat_entry_from_hdr(&entry, header_blk, &hdr);
        n = 1;
    }
    else
    {
        /* Empty build: leave 0 segments. BUILD-01 (issue #131): the publish is
         * skipped, and with it the pending-anchor reset that lives inside it --
         * while the caller goes on to recycle the drained chain regardless. It is
         * here because the dangerous shape is precisely "publish skipped, recycle
         * not skipped", and that must not be reintroducible by a change to Phase 1
         * alone.
         *
         * THIS IS A LIVE PATH SINCE BUILD-04, not the defence in depth it used to
         * be. bm25_build publishes its final chunk unconditionally so that nothing
         * the heap scan accumulated can be dropped, and when the budget fires on the
         * very last document that final accumulator holds zero docs. The seal path
         * still gates on ndocs > 0. Either way publish_append handles n == 0, and
         * on the build path drained_head is Invalid so it does not even emit the
         * anchor-reset record. The zeroing below keeps `entry` from being read
         * uninitialized by a future change. */
        memset(&entry, 0, sizeof(entry));
    }

    /* ---- Phase 2: ONE record publishes the segment AND advances the pending
     * head (the linearization point). */
    bm25_segcat_publish_append(index, heaprel, &entry, n, drained_head);
}

/* bm25_segcat_build_orphan_chain -- lay `n` BM25SegCatEntry onto a FRESH
 * BM25_PAGE_SEGCAT chain of orphan pages and return the first block. The metapage
 * is NOT touched here; the caller flips meta->segcat_root to the returned block in
 * its own (later) record, which is the linearization point. Each catalog page is
 * its own small (<=2-buffer) Generic WAL record -- they are unreachable orphans
 * until the root flip, so a crash before the flip simply leaves them for the
 * Phase-3 orphan sweep. Pages are allocated via bm25_page_alloc(index, heaprel)
 * (D-ALLOC/M6: heaprel pre-opened by the caller, never opened under a buffer lock),
 * initialized with bm25_page_init(.., BM25_PAGE_SEGCAT), stamped seg_gen = 0
 * (catalog pages are non-segment so the option-(d) reader never validates them),
 * and chained via op->nextblk. Generalizes the Phase-1 single-page catalog writer
 * (bm25_segment_build_and_commit's Phase 2) to an arbitrary-length chain. */
BlockNumber
bm25_segcat_build_orphan_chain(Relation index, Relation heaprel,
                               BM25SegCatEntry *entries, int n,
                               BlockNumber *out_tail)
{
    int                 per_page = bm25_segcat_entries_per_page();
    BlockNumber         head = InvalidBlockNumber;
    BlockNumber         prev = InvalidBlockNumber;
    int                 done = 0;

    do
    {
        Buffer            buf = bm25_page_alloc(index, heaprel);   /* EXCL-locked */
        GenericXLogState *st  = GenericXLogStart(index);
        Page              pg  = GenericXLogRegisterBuffer(st, buf, GENERIC_XLOG_FULL_IMAGE);
        BM25SegCatEntry  *arr;
        int               batch = Min(per_page, n - done);
        BlockNumber       this_blk = BufferGetBlockNumber(buf);

        bm25_page_init(pg, BM25_PAGE_SEGCAT);    /* sets nextblk=Invalid, seg_gen=0 */
        arr = (BM25SegCatEntry *) PageGetContents(pg);
        /* ONE bulk copy lays the batch at the ARRAY stride, sizeof(BM25SegCatEntry),
         * while pd_lower below -- and every sequential catalog reader -- use the
         * MAXALIGN stride. The two coincide only because the struct is MAXALIGN-clean,
         * and bm25_format.h pins exactly that relationship with a StaticAssertDecl; a
         * struct change that broke it would fail to compile rather than land entry 1
         * onward short of where readers look for it. */
        if (batch > 0)
            memcpy(arr, &entries[done], sizeof(BM25SegCatEntry) * batch);
        ((PageHeader) pg)->pd_lower =
            (((char *) arr) + MAXALIGN(sizeof(BM25SegCatEntry)) * batch) - (char *) pg;
        GenericXLogFinish(st);
        UnlockReleaseBuffer(buf);

        if (head == InvalidBlockNumber)
            head = this_blk;
        /* link the previous page to this one in a follow-up record (kept separate
         * so each record stays <= 2 buffers; chain links are not yet reachable). */
        if (prev != InvalidBlockNumber)
        {
            Buffer            pbuf = ReadBuffer(index, prev);
            GenericXLogState *pst;
            Page              ppg;

            LockBuffer(pbuf, BUFFER_LOCK_EXCLUSIVE);
            pst = GenericXLogStart(index);
            ppg = GenericXLogRegisterBuffer(pst, pbuf, 0);
            BM25PageGetOpaque(ppg)->nextblk = this_blk;
            GenericXLogFinish(pst);
            UnlockReleaseBuffer(pbuf);
        }
        prev  = this_blk;
        done += batch;
    } while (done < n);

    /* The LAST page, for a caller that needs to link the chain's far end to
     * something -- bm25_segcat_publish_append prepends the whole chain in front of
     * the live catalog by pointing this page's nextblk at the old root inside its
     * flip record. The swap caller replaces the chain wholesale and passes NULL. */
    if (out_tail != NULL)
        *out_tail = prev;
    return head;
}

/* bm25_segcat_publish_swap -- Phase 2 of the swap: publish `nnew` already-built
 * catalog entries and drop `ndrop` generations, in ONE linearizing record.
 *
 * WHY ALL N OUTPUTS GO IN ONE RECORD (BUILD-04), stated because the obvious
 * alternative is wrong. A budget-bounded merge produces several output segments
 * from one chosen input set, and "N outputs published by N swaps, each swap
 * independently atomic" does NOT preserve crash safety. Atomicity was never the
 * invariant that matters -- CONSISTENCY OF THE INTERMEDIATE STATES is. Between swap
 * i and swap i+1 the catalog is a fully committed, WAL-durable state other backends
 * read; with a partial output published while its inputs are still live, every doc
 * in it exists in two places, and a scan snapshots pending_head plus the whole live
 * catalog under ONE metapage SHARE lock and then SUMS per-TID contributions with no
 * cross-segment dedup (bm25_scores_add, bm25_stats.c) -- so those docs are
 * double-scored, and double-RETURNED on the membership path. A crash right there
 * makes it permanent until some later merge happens to fix it. The mirror-image
 * interleaving, dropping inputs in swap i before their unprocessed docs are
 * published, deletes live documents outright. There is no ordering of per-chunk
 * swaps over an arbitrary cut that avoids both.
 *
 * So: N outputs built as orphans, all published and all inputs dropped in ONE
 * record. The file-header invariant generalizes from "one merge = ONE new segment"
 * to "one merge = one atomic publish record", which is what it always meant.
 *
 * It costs nothing, which is why it is available at all: the new catalog chain is
 * laid down beforehand as its own orphan records (bm25_segcat_build_orphan_chain),
 * so however many entries it carries, the flip record registers only the metapage +
 * one retired-list tail page = 2 buffers. The retire-budget contract
 * (ndropped <= BM25_RETIRED_PER_PAGE) constrains INPUTS, not outputs.
 *
 * Crash before the flip: every chunk segment is an unreachable orphan reclaimed by
 * the Phase-3 sweep -- exactly today's Phase-1 story, with more orphan pages. */
void
bm25_segcat_publish_swap(Relation index, Relation heaprel,
                         const BM25SegCatEntry *new_entries, int nnew,
                         uint32 *drop_gens, int ndrop,
                         const BM25FormatRestamp *restamp)
{
    GenericXLogState  *state;
    Buffer             metabuf;
    Page               metapage;
    BM25MetaPageData  *meta;
    BM25SegCatEntry   *catarr = NULL;     /* snapshot of current catalog (palloc'd) */
    uint32             nlive_u = 0;
    int                nlive;
    int                i;
    uint64             new_ndocs = 0;
    uint64             new_total_len = 0;

    Assert(nnew >= 0);   /* checked: callers pass a counted array length */

    /* Phase 2: build a BRAND-NEW orphan catalog chain (survivors + the new entry),
     * then flip the SINGLE meta->segcat_root pointer in ONE Generic WAL record
     * (D-SWAP). We never rewrite a live catalog page in place. This is the
     * indirection trick: the linearization point is the one-pointer flip, and it
     * is multi-page-catalog safe because however long the new chain is, the final
     * record touches only the metapage (1 buffer) + the retired tail. The OLD
     * catalog chain becomes orphan pages on the flip; they are reclaimed by the
     * Phase-3 orphan sweep (mark_chain walks the NEW live chain only). That makes a
     * SUCCESSFUL merge or upgrade an orphan source, so both callers open an orphan
     * bracket and never close it (issue #300): the sweep is gated on that evidence,
     * and without it these pages would leak, one chain per merge. They are
     * NOT placed on the retired list, because no production reader can still be
     * walking them when the sweep frees them. bm25_scan_snapshot and
     * bm25_segcat_first_entry read the catalog under the metapage SHARE, which this
     * flip's EXCLUSIVE excludes, and keep only a copy. bm25_segcat_read_locked's
     * callers, and bm25_segcat_locate_entry's (bm25_livedocs_clear, under
     * bm25_bulkdelete's ShareLock), hold the seal/merge singleton across the whole
     * walk. That excludes this swap, so any such walker took the singleton after the
     * flip and reads the NEW root -- including one running beside the share-mode
     * sweep (issue #300), which frees only the chain this flip orphaned;
     * bm25_segcat_read's own metapage lock covers only the root read (issue #270).
     * Only the debug SRFs walk unguarded. Horizon-gating
     * is therefore unnecessary for these pages (it IS necessary for the SEGMENT data
     * pages -- see bm25_retire_segment). */

    /* --- Step A: snapshot the current catalog into a palloc'd survivor array ---
     * (read-only; the merge's singleton, not a metapage lock, keeps the chain still
     * for the walk). Drop the dropped gens; keep the rest. */
    bm25_segcat_read_locked(index, &catarr, &nlive_u);
    nlive = (int) nlive_u;
    {
        BM25SegCatEntry *survivors = palloc(sizeof(BM25SegCatEntry) *
                                            Max(nlive + nnew, 1));
        int              nsurv = 0;
        BlockNumber      new_root;
        BM25SegCatEntry *dropped = palloc(sizeof(BM25SegCatEntry) * Max(nlive, 1));
        BM25SegmentHeader *dropped_hdr =                /* chain roots harvested BEFORE the window */
            palloc(sizeof(BM25SegmentHeader) * Max(nlive, 1));
        int              ndropped = 0;
        FullTransactionId retire_xid;

        for (i = 0; i < nlive; i++)
        {
            bool drop = false;
            int  j;
            for (j = 0; j < ndrop; j++)
                if (catarr[i].gen == drop_gens[j])
                {
                    drop = true;
                    break;
                }
            if (!drop)
                /* Byte-exact for the same reason: these survivors were read off the
                 * old catalog page and are about to be written to a new one, so their
                 * padding must survive the round trip verbatim. */
                memcpy(&survivors[nsurv++], &catarr[i], sizeof(BM25SegCatEntry));
            else
                dropped[ndropped++] = catarr[i];   /* keep for the in-swap retire */
        }

        /* Issue #302.G. Every gen in drop_gens must have matched exactly one entry.
         * The caller chose them from a catalog read under the singleton ExclusiveLock
         * it still holds, so no swap can have removed one since, and the catalog
         * copy above refuses a duplicate gen; a mismatch is a bug in this AM, not
         * on-disk corruption (hence elog). Publishing anyway would leave a vanished
         * input live beside the outputs that already hold its documents, which then
         * score twice. Before the WAL window, like every check in this function. */
        if (ndropped != ndrop)
            elog(ERROR, "bm25: merge swap matched %d of its %d input generations "
                 "in the catalog", ndropped, ndrop);

        /* Throw-free-window prep (m2a.md:6692): harvest every dropped segment's
         * chain roots HERE, before GenericXLogStart, so bm25_retire_segment inside
         * the window is pure memory. We read the header at its REAL gen (option (d)
         * validation stays on): the segment is still in the live catalog at this
         * point -- the swap record below is what removes it -- so its pages legitimately
         * carry that gen and a mismatch is a real corruption signal worth raising
         * (an ereport here is fine; we are NOT yet inside the window). */
        for (i = 0; i < (uint32) ndropped; i++)
            bm25_seg_header_read(index, dropped[i].header_blkno,
                                 dropped[i].gen, &dropped_hdr[i]);

        /* From here to the flip, survivors[] is a COPY that Step C publishes
         * verbatim, so a tombstone landing in between would have its catalog and
         * metapage decrements overwritten (#241). None can: bm25_bulkdelete holds
         * the singleton ShareLock for its whole pass, which our caller's
         * ExclusiveLock excludes. t/020 parks here to prove it. */
        bm25_debug_pause_point("swap_after_snapshot");

        /* The merge's own contributions. Zero of them is legal and means a pure
         * DROP -- the caller decides that; bm25_merge.c's chunked accumulate simply
         * contributes no entry for a chunk whose docs were all tombstoned. Each
         * entry was zeroed and filled by bm25_segcat_entry_from_hdr, and is copied
         * here by byte-exact memcpy rather than struct assignment: a struct
         * assignment is not required by C to copy padding, so on a compiler that
         * scalarizes the copy the zeroed hole would never reach survivors[] --
         * which is palloc, not palloc0 -- and would be uninitialized again by the
         * time bm25_segcat_build_orphan_chain memcpy's the array onto the page.
         * Zeroing the source only helps if the copy that carries it is byte-exact. */
        for (i = 0; i < nnew; i++)
            memcpy(&survivors[nsurv++], &new_entries[i], sizeof(BM25SegCatEntry));

        /* --- Step B: write the survivor set onto a FRESH orphan SEGCAT chain ---
         * (metapage untouched; each catalog page is its own <=2-buffer record).
         * Returns the first block of the new chain. */
        new_root = bm25_segcat_build_orphan_chain(index, heaprel, survivors, nsurv,
                                                  NULL);

        /* recompute global stats from the survivor set we just built */
        new_ndocs = 0;
        new_total_len = 0;
        for (i = 0; i < nsurv; i++)
        {
            new_ndocs     += survivors[i].live_ndocs;
            new_total_len += survivors[i].total_len;
        }

        /* --- Step C: the SINGLE linearizing record (R4) ---
         * ONE GenericXLogFinish atomically: (a) flips segcat_root to the new
         * orphan chain + updates stats; (b) captures retire_xid INSIDE the
         * exclusive-lock window (after the metapage exclusive lock serializes out
         * any scan that could still read the OLD catalog, so retire_xid bounds
         * exactly those scans -- the metapage lock is what orders this, not any
         * critical section); and (c) appends ONE retired-list RANGE entry per
         * dropped segment to a fresh retired-list tail page -- all in this same
         * record. There is NO separate post-swap retire WAL record. Buffer budget:
         * this flip record touches only the metapage + one retired-list-tail page
         * = 2 buffers.
         *
         * THROW-FREE WINDOW (m2a.md:6692): ALL buffer acquisition and ALL segment-
         * header reads happen BEFORE GenericXLogStart. The dropped headers were read
         * above; the metapage and (when ndropped > 0) the fresh retired tail are
         * acquired+locked here, still before Start. Between Start and Finish there is
         * ZERO ReadBuffer/ReadBufferExtended/LockBuffer/palloc/ereport/bm25_page_alloc/
         * bm25_seg_*read* -- only RegisterBuffer, ReadNextFullTransactionId (non-throwing),
         * the pure-memory metapage + retired-page mutations, and Finish. So a throw can
         * never escape mid-window and no PG_TRY/CATCH is needed. */
        {
            Buffer       rbuf = InvalidBuffer;
            Page         rpage = NULL;
            BlockNumber  tailblk = InvalidBlockNumber;
            int          n;

            /* Pre-acquire the retired tail (a fresh raw-extended page; the reason it is
             * not a bm25_page_alloc page is not recorded) BEFORE opening the window.
             * It is not about handing back a page the merge is about to retire: the
             * allocator takes only DELETED pages, and the merge inputs are still live.
             * A merge drops at most BM25_MERGE_MAX_INPUTS segments, all of which fit on
             * one page (BM25_RETIRED_PER_PAGE is in the hundreds), so one tail page
             * suffices and no overflow handling is needed inside the record. PageInit
             * happens via the REGISTERED copy inside the window, so here we only
             * acquire + EXCL-lock the raw buffer.
             *
             * CALLER CONTRACT: ndropped MUST be <= BM25_RETIRED_PER_PAGE. Only the
             * merge caller's BM25_MERGE_MAX_INPUTS cap guarantees this implicitly;
             * NON-merge callers (bm25_upgrade's bm25_merge_rewrite_all, which drops
             * ALL segments) must guard the count themselves before calling -- else
             * bm25_retire_segment's in-window Assert(n < BM25_RETIRED_PER_PAGE) trips
             * mid-record. A real large-index transform will need a BATCHED rewrite
             * (re-stamp folded into only the final batch's swap) to lift the bound. */
            if (ndropped > 0)
            {
                rbuf = ReadBufferExtended(index, MAIN_FORKNUM, P_NEW, RBM_NORMAL, NULL);
                LockBuffer(rbuf, BUFFER_LOCK_EXCLUSIVE);
            }

            metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
            LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

            /* ---- window opens: nothing throwable from here to GenericXLogFinish ---- */
            state = GenericXLogStart(index);
            metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
            meta = BM25PageGetMeta(metapage);

            /* (b) horizon captured inside the exclusive-lock window (R4). */
            retire_xid = ReadNextFullTransactionId();

            /* (a) the indirection flip + stats.
             * INVARIANT: meta->ndocs / meta->total_len cache the SEGMENT-ONLY live
             * totals; the current pending contribution is folded in by the stats
             * reader at scan time. */
            meta->segcat_root = new_root;          /* indirection flip: ONE pointer */
            meta->nsegs       = nsurv;
            meta->ndocs       = new_ndocs;         /* segment-only; pending folded in by stats reader */
            meta->total_len   = new_total_len;     /* segment-only; pending sumlen folded in by stats reader */

            /* Optional version re-stamp (bm25_upgrade rewrite path only). Folded
             * into THIS record so the metapage's format fields flip in lockstep
             * with segcat_root: a crash can never expose the freshly re-emitted
             * (new-format) segments under a stale old-format version. Pure in-window
             * memory writes on the already-registered page copy -- throw-free and
             * add no buffer to the record. NULL on the merge/seal path, which is
             * therefore byte-for-byte unchanged. */
            if (restamp != NULL)
            {
                meta->format_version   = restamp->format_version;
                /* RAISE the floor, never assign it (src/bm25_format.h:57-63).
                 * min_read_version is monotonic because a reader can hold a
                 * snapshot across the change, and it is raised LAZILY by writers
                 * that have nothing to do with an upgrade --
                 * bm25_pending_append_multi stamps BM25_MIN_READ_PENDING_SPAN the
                 * first time a document spans pending pages. The caller derives
                 * restamp->min_read_version from a PRE-lock snapshot, so the Max
                 * has to be taken HERE, against the registered page copy: a raise
                 * that landed between that snapshot and this window would
                 * otherwise be silently rewritten back down, advertising the
                 * index to a binary with no reader for what it now contains.
                 * Reading the registered copy is pure memory -- still throw-free,
                 * still no extra buffer in the record. */
                meta->min_read_version = Max(meta->min_read_version,
                                             restamp->min_read_version);
                meta->feature_flags    = restamp->feature_flags;
            }

            bm25_meta_set_pd_lower(metapage);

            /* (c) append one RANGE entry per dropped segment, all into the single
             * pre-acquired tail page registered in THIS record (crash-atomic with the
             * flip). The retired page is PageInit'd via its REGISTERED copy here. */
            if (ndropped > 0)
            {
                tailblk = meta->retired_head;
                rpage = GenericXLogRegisterBuffer(state, rbuf, GENERIC_XLOG_FULL_IMAGE);
                bm25_page_init(rpage, BM25_PAGE_RETIRED);
                BM25PageGetOpaque(rpage)->nextblk = tailblk;
                meta->retired_head = BufferGetBlockNumber(rbuf);   /* prepend new tail */
                n = 0;
                for (i = 0; i < (uint32) ndropped; i++)
                    bm25_retire_segment(rpage, &n, &dropped[i],
                                        &dropped_hdr[i], retire_xid);
            }

            GenericXLogFinish(state);
            /* ---- window closed ---- */

            if (rbuf != InvalidBuffer)
                UnlockReleaseBuffer(rbuf);
            UnlockReleaseBuffer(metabuf);
        }

        pfree(dropped);
        pfree(dropped_hdr);
        pfree(survivors);
        if (catarr != NULL)
            pfree(catarr);
    }
}
