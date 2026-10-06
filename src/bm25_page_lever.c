/* bm25_page_lever.c -- the raw page lever the corruption suites use: bm25_debug_poke_page
 * writes arbitrary bytes onto one page of a bm25 index under Generic WAL, and
 * bm25_debug_layout reports the offsets and sizes of the on-disk structs it addresses.
 *
 * Why a raw lever. The bm25_debug_stamp_* family (bm25_seg_debug.c, bm25_pending.c,
 * bm25_meta.c) forges one named field each, after a validated walk to its target. The
 * corrupt-pointer tests of issues #302/#303/#309 need about twenty more fields forged
 * (metapage pointers, retired-descriptor roots, catalog-entry gen, fieldcfg field_id,
 * pending field_id, DICT/POS nextblk, impact max_tf, pd_special), and one raw writer
 * plus a layout table costs less than twenty semantic levers and keeps the test in the
 * SQL suites, where gcov sees it, rather than in a TAP that patches relation files and
 * has to recompute checksums.
 *
 * Kept out of bm25_seg_debug.c because neither function uses the segment reader that
 * file is organized around. Both are REVOKEd from PUBLIC by the install script's
 * allowlist loop like every bm25_debug_* function (sql/63_debug_privileges), and the
 * writer is also gated on index ownership. Exercised by sql/136_page_lever.
 */
#include "postgres.h"

#include "access/xlog.h"     /* XLogFlush */
#include "bm25.h"
#include "funcapi.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"   /* not pulled in by funcapi.h as of PG19 */
/* -------------------------------------------------------------------------
 * bm25_debug_poke_page(index regclass, blkno bigint, off int, bytes bytea)
 *   RETURNS bytea
 * -- TEST-ONLY raw corruption lever (issues #302, #303, #309).
 *
 * Writes `bytes` at byte offset `off` of block `blkno` and returns the bytes it
 * replaced. The stamp levers above each forge one named field after a validated walk to
 * their target; this one forges any bytes on any page, which is what the remaining
 * corrupt-pointer tests need (metapage pointers, retired-descriptor roots, catalog-entry
 * gen, fieldcfg field_id, pending field_id, DICT/POS nextblk, impact max_tf, pd_special)
 * without one more lever per field. Suites take offsets from bm25_debug_layout below,
 * never from literals. Any block below the relation's size is a target, block 0
 * included: the metapage pointers are among the values the tests most need to corrupt.
 *
 * It validates the range and that the RESULT is still a page PostgreSQL can carry, and
 * nothing about bm25 kind, structure or content, because forging bad structure is its
 * job. The refusals, each for a poke that would not do what the caller asked:
 *   - The range must lie in the block and start at or after pd_flags. pd_lsn is
 *     overwritten by the record's LSN and pd_checksum is recomputed when the buffer is
 *     written, so bytes there would be silently replaced.
 *   - The result's header must pass the header half of core's PageIsVerified (pd_upper
 *     nonzero, no unknown pd_flags bits, pd_lower <= pd_upper <= pd_special <= BLCKSZ,
 *     pd_special MAXALIGNed). A header that fails it reads fine while the buffer stays
 *     cached and raises "invalid page" once it is evicted and re-read, so a suite's
 *     outcome would depend on buffer pressure; and pd_lower > pd_upper would PANIC,
 *     because GenericXLogFinish memsets the hole inside its critical section.
 *   - pd_lower must not point inside the page header. Core's check allows it, but the
 *     hole would then start inside the header, GenericXLogFinish would zero pd_upper and
 *     pd_special in the buffer, and the buffer would no longer be the page validated
 *     here: it would hold pd_lower > pd_upper, the PANIC precondition the check above
 *     exists to exclude (measured: pd_lower 14 was accepted, and the next read hit
 *     PageGetSpecialPointer's assertion on a cassert build).
 *   - No poked byte may land in the result's hole [pd_lower, pd_upper), and the hole
 *     must already be all zero. GenericXLogFinish zeroes the hole on the primary and
 *     replay zeroes it on a standby, so poked bytes there would vanish, and a poke that
 *     lowers pd_lower over live content would destroy that content irrecoverably
 *     (measured: 266 bytes, which poking the old pd_lower back could not restore).
 *   - pd_special is accepted anywhere core accepts it, including values that leave less
 *     than a BM25PageOpaque before BLCKSZ (pd_special == BLCKSZ among them): 302.D's
 *     tests need exactly that page. Such a page makes every bm25 reader of it read past
 *     the block, so no suite may poke pd_special except the 302.D cluster's own test.
 * Caveats:
 *   - It cannot leave an all-zero, never-initialized page: the record stamps an LSN,
 *     and pd_upper == 0 is refused above. A test that needs a link to one must produce
 *     a genuinely never-initialized page (t/028's pending_append_alloc pause plus an
 *     immediate shutdown leaves one).
 *   - It holds only the target's EXCLUSIVE content lock, no metapage lock or singleton,
 *     so it is for a quiescent index, not one under concurrent maintenance.
 *   - The index is left as corrupt as the bytes make it. Drop the table afterwards, or
 *     poke the returned bytes back. Because no accepted poke can zero live bytes (the
 *     hole refusal above), that restores every byte but pd_lsn and pd_checksum; if a
 *     later poke wrote into what the restore would turn back into hole, the restore is
 *     refused rather than lossy.
 *
 * Logged as a Generic WAL FULL_IMAGE record, so the forged page reaches standbys and
 * survives crash recovery, and the checksum computed when the buffer is written covers
 * it. The record is flushed before returning: the caller's transaction usually has no
 * xid, so its commit would not flush it, and a TAP that pokes and then stops the server
 * with -m immediate would recover the UNPOKED page and test nothing (measured: both
 * pokes were gone after an immediate stop before the flush was added). Every check runs
 * before the WAL window opens. Ownership + AM identity via
 * bm25_index_open_owned like every write-side lever; REVOKEd from PUBLIC by the install
 * script's allowlist loop (sql/63_debug_privileges).
 * -------------------------------------------------------------------------*/
static void
poke_image_validate(const char *fn, Page image, int32 off, int64 len)
{
    PageHeader  ph = (PageHeader) image;
    uint16      i;

    if (ph->pd_upper == 0 ||
        (ph->pd_flags & ~PD_VALID_FLAG_BITS) != 0 ||
        ph->pd_lower < SizeOfPageHeaderData ||
        ph->pd_lower > ph->pd_upper ||
        ph->pd_upper > ph->pd_special ||
        ph->pd_special > BLCKSZ ||
        ph->pd_special != MAXALIGN(ph->pd_special))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: the poked page would fail PostgreSQL's page header check", fn),
                 errdetail("The result has pd_flags 0x%x, pd_lower %u, pd_upper %u, pd_special %u.",
                           ph->pd_flags, ph->pd_lower, ph->pd_upper, ph->pd_special)));
    if (ph->pd_lower < ph->pd_upper && off < ph->pd_upper && off + len > ph->pd_lower)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: bytes [%d, " INT64_FORMAT ") overlap the page hole [%u, %u)",
                        fn, off, off + len, ph->pd_lower, ph->pd_upper),
                 errdetail("Generic WAL zeroes the hole between pd_lower and pd_upper, so "
                           "these bytes would be lost.")));
    /* A plain loop: pg_memory_is_all_zeros is PG18+, and this runs once per poke. */
    for (i = ph->pd_lower; i < ph->pd_upper; i++)
        if (((const char *) image)[i] != 0)
            break;
    if (i < ph->pd_upper)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: the poked page's hole [%u, %u) holds nonzero bytes",
                        fn, ph->pd_lower, ph->pd_upper),
                 errdetail("Generic WAL zeroes the hole between pd_lower and pd_upper, so a "
                           "poke that lowers pd_lower over live bytes would destroy them.")));
}

PG_FUNCTION_INFO_V1(bm25_debug_poke_page);
Datum
bm25_debug_poke_page(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int64               blkno = PG_GETARG_INT64(1);
    int32               off   = PG_GETARG_INT32(2);
    bytea              *bytes = PG_GETARG_BYTEA_PP(3);
    int64               len   = VARSIZE_ANY_EXHDR(bytes);
    const char         *fn    = "bm25_debug_poke_page";
    const int32         first = (int32) offsetof(PageHeaderData, pd_flags);
    Relation            index;
    BlockNumber         nblocks;
    Buffer              buf;
    PGAlignedBlock      image;
    bytea              *old;
    GenericXLogState   *state;
    Page                page;
    XLogRecPtr          lsn;

    /* Arguments before the index is opened (97_debug_probe_arguments). */
    if (blkno < 0 || blkno > (int64) MaxBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: block " INT64_FORMAT " is not a block number", fn, blkno)));
    if (len == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: no bytes to write", fn)));
    if (off < first || (int64) off + len > BLCKSZ)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: bytes [%d, " INT64_FORMAT ") are outside the writable range "
                        "[%d, %d) of a page", fn, off, (int64) off + len, first, BLCKSZ),
                 errdetail("The page header's pd_lsn and pd_checksum are rewritten when the page is logged "
                           "and written.")));

    index = bm25_index_open_owned(relid, RowExclusiveLock);
    nblocks = RelationGetNumberOfBlocks(index);
    if ((BlockNumber) blkno >= nblocks)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("%s: block " INT64_FORMAT " is past the end of the index (%u blocks)",
                        fn, blkno, nblocks)));

    buf = ReadBuffer(index, (BlockNumber) blkno);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    memcpy(image.data, BufferGetPage(buf), BLCKSZ);
    old = (bytea *) palloc(VARHDRSZ + len);
    SET_VARSIZE(old, VARHDRSZ + len);
    memcpy(VARDATA(old), image.data + off, len);
    memcpy(image.data + off, VARDATA_ANY(bytes), len);
    poke_image_validate(fn, (Page) image.data, off, len);

    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
    memcpy((char *) page + off, VARDATA_ANY(bytes), len);  /* the deliberate corruption */
    lsn = GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
    if (!XLogRecPtrIsInvalid(lsn))         /* Invalid for an unlogged index */
        XLogFlush(lsn);
    index_close(index, RowExclusiveLock);
    PG_RETURN_BYTEA_P(old);
}

/* -------------------------------------------------------------------------
 * bm25_debug_layout() RETURNS SETOF (struct text, field text, off int, size int)
 * -- the on-disk layout bm25_debug_poke_page addresses.
 *
 * One row per member of each struct a page carries, from offsetof and sizeof, so a
 * suite names a field instead of hard-coding an offset that a later format change would
 * silently re-aim. Each struct also gets a field '*' row at offset 0 with sizeof(struct):
 * the stride where it is an array element (BM25SegCatEntry, BM25RetiredEntry,
 * BM25FieldConfig). Offsets are relative to the struct; where the struct sits on its
 * page is the format's business, and most start at the 'page' row's 'contents'.
 *
 * Three pseudo-structs, lower case so they cannot be mistaken for C types:
 *   page          'contents' (PageGetContents) and 'special' (where every bm25 page's
 *                 BM25PageOpaque starts); '*' is the block.
 *   impact_table  the packed per-block impact encoding, which is not a C struct on the
 *   impact_entry  page: one nfields byte, then BM25_IMPACT_FIELD_BYTES per field
 *                 (bm25_encode_impact_table). Unaligned; offsets written out by hand,
 *                 pinned by the StaticAssertDecl below.
 * Members are listed in declaration order. A member added to one of these structs needs
 * a row here; sql/136 prints the table, so the expected output shows the gap.
 * -------------------------------------------------------------------------*/
typedef struct BM25LayoutRow
{
    const char *structname;
    const char *field;
    int32       off;
    int32       size;
} BM25LayoutRow;

#define LAYOUT_MEMBER(S, F) {#S, #F, (int32) offsetof(S, F), (int32) sizeof(((S *) 0)->F)}
#define LAYOUT_WHOLE(S)     {#S, "*", 0, (int32) sizeof(S)}

StaticAssertDecl(sizeof(uint8) + 2 * sizeof(uint32) == BM25_IMPACT_FIELD_BYTES,
                 "impact_entry rows in bm25_debug_layout no longer match the encoding");

static const BM25LayoutRow bm25_layout_rows[] = {
    LAYOUT_WHOLE(PageHeaderData),
    LAYOUT_MEMBER(PageHeaderData, pd_lsn),
    LAYOUT_MEMBER(PageHeaderData, pd_checksum),
    LAYOUT_MEMBER(PageHeaderData, pd_flags),
    LAYOUT_MEMBER(PageHeaderData, pd_lower),
    LAYOUT_MEMBER(PageHeaderData, pd_upper),
    LAYOUT_MEMBER(PageHeaderData, pd_special),
    LAYOUT_MEMBER(PageHeaderData, pd_pagesize_version),
    LAYOUT_MEMBER(PageHeaderData, pd_prune_xid),

    {"page", "*", 0, BLCKSZ},
    {"page", "contents", (int32) MAXALIGN(SizeOfPageHeaderData),
     (int32) (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(BM25PageOpaque)))},
    {"page", "special", (int32) (BLCKSZ - MAXALIGN(sizeof(BM25PageOpaque))),
     (int32) MAXALIGN(sizeof(BM25PageOpaque))},

    LAYOUT_WHOLE(BM25PageOpaque),
    LAYOUT_MEMBER(BM25PageOpaque, flags),
    LAYOUT_MEMBER(BM25PageOpaque, unused),
    LAYOUT_MEMBER(BM25PageOpaque, nextblk),
    LAYOUT_MEMBER(BM25PageOpaque, retire_xid),
    LAYOUT_MEMBER(BM25PageOpaque, seg_gen),

    LAYOUT_WHOLE(BM25MetaPageData),
    LAYOUT_MEMBER(BM25MetaPageData, magic),
    LAYOUT_MEMBER(BM25MetaPageData, format_version),
    LAYOUT_MEMBER(BM25MetaPageData, k1),
    LAYOUT_MEMBER(BM25MetaPageData, b),
    LAYOUT_MEMBER(BM25MetaPageData, pending_head),
    LAYOUT_MEMBER(BM25MetaPageData, pending_tail),
    LAYOUT_MEMBER(BM25MetaPageData, pending_tail_free),
    LAYOUT_MEMBER(BM25MetaPageData, pending_npages),
    LAYOUT_MEMBER(BM25MetaPageData, pending_ndocs),
    LAYOUT_MEMBER(BM25MetaPageData, segcat_root),
    LAYOUT_MEMBER(BM25MetaPageData, nsegs),
    LAYOUT_MEMBER(BM25MetaPageData, ndocs),
    LAYOUT_MEMBER(BM25MetaPageData, total_len),
    LAYOUT_MEMBER(BM25MetaPageData, retired_head),
    LAYOUT_MEMBER(BM25MetaPageData, next_gen),
    LAYOUT_MEMBER(BM25MetaPageData, analyzer_fingerprint),
    LAYOUT_MEMBER(BM25MetaPageData, field_config_blkno),
    LAYOUT_MEMBER(BM25MetaPageData, field_count),
    LAYOUT_MEMBER(BM25MetaPageData, min_read_version),
    LAYOUT_MEMBER(BM25MetaPageData, feature_flags),
    LAYOUT_MEMBER(BM25MetaPageData, reserved_tail_pad),
    LAYOUT_MEMBER(BM25MetaPageData, orphan_ops_begun),
    LAYOUT_MEMBER(BM25MetaPageData, orphan_ops_done),
    LAYOUT_MEMBER(BM25MetaPageData, swept_epoch),

    LAYOUT_WHOLE(BM25SegCatEntry),
    LAYOUT_MEMBER(BM25SegCatEntry, header_blkno),
    LAYOUT_MEMBER(BM25SegCatEntry, total_tokens),
    LAYOUT_MEMBER(BM25SegCatEntry, ndocs),
    LAYOUT_MEMBER(BM25SegCatEntry, live_ndocs),
    LAYOUT_MEMBER(BM25SegCatEntry, total_len),
    LAYOUT_MEMBER(BM25SegCatEntry, nterms),
    LAYOUT_MEMBER(BM25SegCatEntry, gen),

    LAYOUT_WHOLE(BM25RetiredEntry),
    LAYOUT_MEMBER(BM25RetiredEntry, header_blkno),
    LAYOUT_MEMBER(BM25RetiredEntry, dict_root),
    LAYOUT_MEMBER(BM25RetiredEntry, norms_root),
    LAYOUT_MEMBER(BM25RetiredEntry, livedocs_root),
    LAYOUT_MEMBER(BM25RetiredEntry, docmap_root),
    LAYOUT_MEMBER(BM25RetiredEntry, posts_root),
    LAYOUT_MEMBER(BM25RetiredEntry, keymap_root),
    LAYOUT_MEMBER(BM25RetiredEntry, pos_root),
    LAYOUT_MEMBER(BM25RetiredEntry, gen),
    LAYOUT_MEMBER(BM25RetiredEntry, retire_xid),

    LAYOUT_WHOLE(BM25SegmentHeader),
    LAYOUT_MEMBER(BM25SegmentHeader, gen),
    LAYOUT_MEMBER(BM25SegmentHeader, total_tokens),
    LAYOUT_MEMBER(BM25SegmentHeader, ndocs),
    LAYOUT_MEMBER(BM25SegmentHeader, total_len),
    LAYOUT_MEMBER(BM25SegmentHeader, nterms),
    LAYOUT_MEMBER(BM25SegmentHeader, dict_root),
    LAYOUT_MEMBER(BM25SegmentHeader, norms_root),
    LAYOUT_MEMBER(BM25SegmentHeader, livedocs_root),
    LAYOUT_MEMBER(BM25SegmentHeader, docmap_root),
    LAYOUT_MEMBER(BM25SegmentHeader, posts_root),
    LAYOUT_MEMBER(BM25SegmentHeader, field_count),
    LAYOUT_MEMBER(BM25SegmentHeader, pos_root),
    LAYOUT_MEMBER(BM25SegmentHeader, keymap_root),

    LAYOUT_WHOLE(BM25KeymapHeader),
    LAYOUT_MEMBER(BM25KeymapHeader, key_type),
    LAYOUT_MEMBER(BM25KeymapHeader, pad0),
    LAYOUT_MEMBER(BM25KeymapHeader, key_size),
    LAYOUT_MEMBER(BM25KeymapHeader, ndocs),

    LAYOUT_WHOLE(BM25DictEntry),
    LAYOUT_MEMBER(BM25DictEntry, df),
    LAYOUT_MEMBER(BM25DictEntry, post_root),
    LAYOUT_MEMBER(BM25DictEntry, post_off),
    LAYOUT_MEMBER(BM25DictEntry, termlen),
    LAYOUT_MEMBER(BM25DictEntry, pos_post_root),
    LAYOUT_MEMBER(BM25DictEntry, pos_post_off),
    LAYOUT_MEMBER(BM25DictEntry, dict_pad),

    LAYOUT_WHOLE(BM25BlockHeader),
    LAYOUT_MEMBER(BM25BlockHeader, ndocs),
    LAYOUT_MEMBER(BM25BlockHeader, last_docid),
    LAYOUT_MEMBER(BM25BlockHeader, docid_bytes),
    LAYOUT_MEMBER(BM25BlockHeader, tf_bytes),
    LAYOUT_MEMBER(BM25BlockHeader, field_rle_bytes),
    LAYOUT_MEMBER(BM25BlockHeader, impact_bytes),

    {"impact_table", "*", 0, (int32) (sizeof(uint8) + BM25_IMPACT_FIELD_BYTES * BM25_MAX_FIELDS)},
    {"impact_table", "nfields", 0, (int32) sizeof(uint8)},
    {"impact_table", "entries", (int32) sizeof(uint8),
     (int32) (BM25_IMPACT_FIELD_BYTES * BM25_MAX_FIELDS)},
    {"impact_entry", "*", 0, BM25_IMPACT_FIELD_BYTES},
    {"impact_entry", "field_id", 0, (int32) sizeof(uint8)},
    {"impact_entry", "max_tf", (int32) sizeof(uint8), (int32) sizeof(uint32)},
    {"impact_entry", "min_doclen", (int32) (sizeof(uint8) + sizeof(uint32)), (int32) sizeof(uint32)},

    LAYOUT_WHOLE(BM25FieldConfigHeader),
    LAYOUT_MEMBER(BM25FieldConfigHeader, field_count),
    LAYOUT_MEMBER(BM25FieldConfigHeader, per_field_fingerprint),

    LAYOUT_WHOLE(BM25FieldConfig),
    LAYOUT_MEMBER(BM25FieldConfig, field_id),
    LAYOUT_MEMBER(BM25FieldConfig, field_name),
    LAYOUT_MEMBER(BM25FieldConfig, k1),
    LAYOUT_MEMBER(BM25FieldConfig, b),
    LAYOUT_MEMBER(BM25FieldConfig, boost),
    LAYOUT_MEMBER(BM25FieldConfig, tokenizer_type),
    LAYOUT_MEMBER(BM25FieldConfig, stemmer_name),

    LAYOUT_WHOLE(BM25KeyStamp),
    LAYOUT_MEMBER(BM25KeyStamp, magic),
    LAYOUT_MEMBER(BM25KeyStamp, key_type),
    LAYOUT_MEMBER(BM25KeyStamp, pad0),
    LAYOUT_MEMBER(BM25KeyStamp, key_size),
    LAYOUT_MEMBER(BM25KeyStamp, key_attno),
    LAYOUT_MEMBER(BM25KeyStamp, pad1),

    LAYOUT_WHOLE(BM25PendingDocHeader),
    LAYOUT_MEMBER(BM25PendingDocHeader, tid),
    LAYOUT_MEMBER(BM25PendingDocHeader, nfieldlens),
    LAYOUT_MEMBER(BM25PendingDocHeader, ndocterms),
    LAYOUT_MEMBER(BM25PendingDocHeader, doclen),
    LAYOUT_MEMBER(BM25PendingDocHeader, flags),
    LAYOUT_MEMBER(BM25PendingDocHeader, key_type),
    LAYOUT_MEMBER(BM25PendingDocHeader, key_pad0),
    LAYOUT_MEMBER(BM25PendingDocHeader, key_size),
    LAYOUT_MEMBER(BM25PendingDocHeader, key),

    LAYOUT_WHOLE(BM25PendingTermEntry),
    LAYOUT_MEMBER(BM25PendingTermEntry, termlen),
    LAYOUT_MEMBER(BM25PendingTermEntry, tf),
    LAYOUT_MEMBER(BM25PendingTermEntry, field_id),
    LAYOUT_MEMBER(BM25PendingTermEntry, pos_bytes),
};

PG_FUNCTION_INFO_V1(bm25_debug_layout);
Datum
bm25_debug_layout(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Tuplestorestate    *ts;
    TupleDesc           tupdesc;
    Size                i;

    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context that cannot accept a set")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);
    /* Per-query context, as in bm25_debug_segcat (C7). */
    {
        MemoryContext oldctx = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

        ts = tuplestore_begin_heap(true, false, work_mem);
        MemoryContextSwitchTo(oldctx);
    }
    rsi->returnMode = SFRM_Materialize;
    rsi->setResult = ts;
    rsi->setDesc = tupdesc;

    for (i = 0; i < lengthof(bm25_layout_rows); i++)
    {
        Datum   vals[4];
        bool    nulls[4] = {0};

        vals[0] = CStringGetTextDatum(bm25_layout_rows[i].structname);
        vals[1] = CStringGetTextDatum(bm25_layout_rows[i].field);
        vals[2] = Int32GetDatum(bm25_layout_rows[i].off);
        vals[3] = Int32GetDatum(bm25_layout_rows[i].size);
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    return (Datum) 0;
}
