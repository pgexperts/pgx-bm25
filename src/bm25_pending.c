/* bm25_pending.c -- GIN-fastupdate-style WAL-logged pending list.
 *
 * aminsert appends each document's postings here (append-only; sealed segments
 * are never touched), under a short metabuffer+tail-page exclusive lock -- the
 * ginHeapTupleFastInsert template. A seal drains the chain into a BM25Accum and
 * publishes a segment via the Task-5 two-phase commit, then truncates (recycles)
 * the drained pages. A document's entries are stored consecutively so a drain
 * reassembles it without random access (design section 3.3) -- since v7 that means consecutive
 * RECORDS rather than one record on one page: a document too large for a single page is
 * written as several same-tid parts, the later ones flagged BM25_PENDING_DOC_CONT, and
 * the drain accumulates them into one accumulator doc (#57, docs/adr/0038).
 *
 * Every pending page carries its CHAIN EPOCH in seg_gen (#291): drawn from the
 * metapage's next_gen when a chain starts and copied onto each later page. A scan
 * walks the chain it captured with bm25_pending_walk_read, which rejects a page whose
 * epoch is at or above the snapshot's next_gen -- a page recycled and re-initialized
 * after the capture, which a hot standby without feedback can reach. */
#include "postgres.h"

#include "bm25.h"
#include "access/table.h"      /* table_open/table_close (heap, D-ALLOC/M6) */
#include "access/transam.h"    /* ReadNextFullTransactionId (drained-page horizon) */
#include "catalog/index.h"     /* IndexGetRelation (D-ALLOC/M6) */
#include "common/hashfn.h"     /* hash_bytes/hash_combine -- append de-dup HTAB */
#include "storage/bufmgr.h"
#include "storage/freespace.h" /* FreeSpaceMapVacuumRange (truncate recycler, PEND-07) */
#include "storage/indexfsm.h"  /* RecordFreeIndexPage (truncate recycler) */
#include "storage/lmgr.h"      /* LockPage/UnlockPage seal singleton (D-SEAL/M5) */

int bm25_seal_threshold_kb = 4096;      /* 4 MB default, mirrors gin_pending_list_limit */

/* Defined below, next to the reasoning for why the cap is a counter rather than a
 * visited bitmap. Forward-declared because all three writer-side pending-chain
 * walkers and the scan-side bm25_pending_walk_read use it, and the first of them
 * precedes the definition. */
static void bm25_pending_cycle_cap_validate(uint64 visited, BlockNumber nblocks,
                                            BlockNumber blk);

#define PENDING_PAGE_CAPACITY \
    (BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BM25PageOpaque)))

/* Shared errhint for the two limits a document can still hit. The old
 * "document too large for a pending page" ceiling -- ~380 distinct 7-byte stems at the
 * default BLCKSZ, which CREATE INDEX did not share -- is GONE as of v7 (#57): a document
 * now spans pages as same-tid continuation records. What survives is narrower and both
 * cases fail loudly at INSERT:
 *
 *   1. total token count must fit uint16, because BM25PendingTermEntry.tf is 16 bits
 *      on disk (this is also what keeps H7/#47's tf-wrap unreachable);
 *   2. one (field, term) entry plus a doc header must fit one page, since an entry's
 *      term bytes and position blob are contiguous. Only reachable with a very large
 *      position blob, i.e. one term repeated tens of thousands of times in one field. */
#define BM25_PENDING_TOO_LARGE_HINT \
    "Since format v7 a document may span pending pages, so this is no longer the old " \
    "one-page ceiling. What remains is a token-count limit (16-bit term frequency) and " \
    "a requirement that one (field, term) entry with its positions fit a single page. " \
    "Reduce the document's token count, or split it across rows."

/* De-dup key for the append below: (field_id, term bytes). Keyed by CONTENT with
 * no length cap, and holding a POINTER rather than an inline copy -- the bytes
 * live in the caller's token arrays, which outlive the HTAB. Same shape as
 * bm25_seg_dict.c's wildcard dedup set, for the same reason (a fixed inline key
 * buffer would impose a cap, and truncating it would false-merge two distinct
 * long terms -- here that would merge their tf and positions). */
typedef struct BM25PendKey
{
    uint32          field;
    const char     *ptr;
    int             len;
} BM25PendKey;

typedef struct BM25PendEnt
{
    BM25PendKey     key;
    int             idx;        /* index into the e_* parallel arrays */
} BM25PendEnt;

static uint32
bm25_pendkey_hash(const void *key, Size keysize)
{
    const BM25PendKey *k = (const BM25PendKey *) key;

    return hash_combine(hash_uint32(k->field),
                        hash_bytes((const unsigned char *) k->ptr, k->len));
}

static int
bm25_pendkey_match(const void *a, const void *b, Size keysize)
{
    const BM25PendKey *ka = (const BM25PendKey *) a;
    const BM25PendKey *kb = (const BM25PendKey *) b;

    if (ka->field != kb->field || ka->len != kb->len)
        return 1;               /* unequal => nonzero */
    return memcmp(ka->ptr, kb->ptr, ka->len);
}

static void *
bm25_pendkey_copy(void *dst, const void *src, Size keysize)
{
    /* Copy the (field,ptr,len) struct, NOT the bytes. */
    memcpy(dst, src, sizeof(BM25PendKey));
    return (char *) dst + sizeof(BM25PendKey);
}

/* ---- drain part buffer (v7 multi-part documents, #57) ----
 *
 * A document may arrive as several consecutive same-tid records, so the drain cannot feed
 * the accumulator record by record. bm25_accum_add_field_tokens ASSIGNS
 * doclen_by_field[field] and bumps ndocs_by_field[field], so calling it twice for one
 * (doc, field) would overwrite the length with the last part's count and count the
 * document twice in that field's N -- silently wrong idf and avgdl, not a crash. It must
 * be called exactly ONCE per (doc, field), which means buffering a document's parts until
 * it is complete.
 *
 * Term bytes must be COPIED, not referenced: BM25Token.ptr would otherwise point into
 * the drain's one page image, which the NEXT page's copy overwrites before a later part
 * is read. One copy per ENTRY (not per occurrence) -- an entry's tf tokens all share it.
 *
 * The single-part case goes through the same path rather than keeping a separate
 * zero-copy fast path: seal is a bulk operation and the accumulator copies every term
 * into its own context regardless, so one extra memcpy per entry is noise next to
 * accum_find_or_add_term, and one code path cannot drift from the other. */
typedef struct DrainDoc
{
    bool                active;
    ItemPointerData     tid;
    uint8               key_type;
    uint16              key_size;
    unsigned char       key[BM25_KEY_MAX_SIZE];
#ifdef USE_ASSERT_CHECKING
    /* v8: the per-field doclens the APPEND stored, taken from the document's first
     * part. The drain does not need them -- it rebuilds the tokens with their true
     * positions, so bm25_accum_add_field_tokens re-derives the identical value -- and
     * keeping them is purely so drain_doc_flush can assert the two agree. That
     * equality IS the pre-seal/post-seal score identity this format version exists to
     * guarantee, so it is worth checking where both numbers are in hand rather than
     * inferring it from a score three layers up. false for a pre-v8 record.
     *
     * Compiled out of a production build ON PURPOSE, struct members included: this is
     * the bulk seal path, and copying up to 4 * BM25_MAX_FIELDS bytes per document to
     * feed an assertion nobody evaluates is work, not insurance. */
    bool                have_fieldlens;
    uint32              nfieldlens;
    uint32              fieldlens[BM25_MAX_FIELDS];
#endif
    BM25Token          *toks[BM25_MAX_FIELDS];   /* per-field, one token per occurrence */
    int                 ntok[BM25_MAX_FIELDS];
    int                 cap[BM25_MAX_FIELDS];
    MemoryContext       cxt;                /* token arrays + term copies; reset per doc */
} DrainDoc;

static void
drain_doc_init(DrainDoc *d, uint32 field_count)
{
    uint32 f;

    d->active = false;
    d->cxt = AllocSetContextCreate(CurrentMemoryContext,
                                   "bm25 pending drain doc",
                                   ALLOCSET_SMALL_SIZES);
    for (f = 0; f < field_count; f++)
    {
        d->toks[f] = NULL;
        d->ntok[f] = 0;
        d->cap[f] = 0;
    }
}

static void
drain_doc_fini(DrainDoc *d)
{
    MemoryContextDelete(d->cxt);
}

static void
drain_doc_begin(DrainDoc *d, const BM25PendingDocHeader *dh)
{
    uint32          f;

    /* Everything the previous document buffered has been flushed, so reclaim it in one
     * go rather than tracking individual allocations. */
    MemoryContextReset(d->cxt);
    for (f = 0; f < BM25_MAX_FIELDS; f++)
    {
        d->toks[f] = NULL;
        d->ntok[f] = 0;
        d->cap[f] = 0;
    }
    d->active   = true;
    d->tid      = dh->tid;
    d->key_type = dh->key_type;
    d->key_size = dh->key_size;
#ifdef USE_ASSERT_CHECKING
    {
        const uint32 *fl = bm25_pending_doc_fieldlens(dh);

        d->have_fieldlens = (fl != NULL);
        d->nfieldlens = d->have_fieldlens ? dh->nfieldlens : 0;  /* bounded by iter_next */
        if (d->have_fieldlens)
            memcpy(d->fieldlens, fl, sizeof(uint32) * d->nfieldlens);
    }
#endif
    memcpy(d->key, dh->key, BM25_KEY_MAX_SIZE);
}

/*
 * bm25_pending_term_entry_span -- decode boundary for one on-page pending term entry
 * (ADR 0027/0039's shape, extended to the pending list). Bounds the fixed header fit,
 * then the MAXALIGN'd (header + termlen + pos_bytes) span, against `end` before
 * termlen/pos_bytes -- both raw on-page uint16s -- are trusted as a memcpy length or a
 * cursor stride. Returns the entry's total on-page length so callers advance without
 * recomputing it, matching bm25_dictentry_validate's validate-then-return-the-stride
 * shape. Shared by every walker over this identical layout: the iterator's stride
 * loop (bm25_pending_iter_next), the drain's per-part re-decode (below), and the
 * query-path wildcard expander's re-walk of an iterator-returned doc
 * (bm25_dict_expand_wildcard, bm25_seg_dict.c) -- one struct, one stride rule, one
 * place it is checked. EXTERN, not static, for that third caller (SEGREAD-13, issue
 * #154): it used to open-code the same MAXALIGN expression, which is exactly the
 * second place bm25_format.h's pending-record comment promises does not exist. */
Size
bm25_pending_term_entry_span(const char *cur, const char *end)
{
    const BM25PendingTermEntry *te;
    Size    entry_len;

    if (cur + sizeof(BM25PendingTermEntry) > end)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending term entry header overruns the page")));

    te = (const BM25PendingTermEntry *) cur;
    entry_len = MAXALIGN(sizeof(BM25PendingTermEntry) + te->termlen + te->pos_bytes);
    if (cur + entry_len > end)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending term entry of length %zu overruns the page",
                        entry_len)));
    return entry_len;
}

/* Decode-boundary probe (2026-08 page-content-bytes sweep). bm25_pending_term_entry_span
 * only fires on an already-corrupt pending page, which a regression suite cannot
 * produce, so this drives the SAME (static) function over a caller-chosen
 * (termlen, pos_bytes, avail) triple. termlen/pos_bytes fill a REAL
 * BM25PendingTermEntry on the C stack (a struct assignment, not a raw bytea, so
 * the suite stays host-endian/padding independent -- same reasoning as
 * bm25_debug_dictentry_validate). `avail` stands in for (end - cur), the notional
 * page bytes left from this entry onward. TEST-ONLY: a pure function of its
 * scalar arguments, no relation touched, covered by the install script's REVOKE
 * loop like every other bm25_debug_* function. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_term_entry_span);
Datum
bm25_debug_pending_term_entry_span(PG_FUNCTION_ARGS)
{
    int32                 termlen   = PG_GETARG_INT32(0);
    int32                 pos_bytes = PG_GETARG_INT32(1);
    int32                 avail     = PG_GETARG_INT32(2);
    BM25PendingTermEntry  hdr;

    if (termlen < 0 || termlen > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_term_entry_span: termlen out of uint16 range")));
    if (pos_bytes < 0 || pos_bytes > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_term_entry_span: pos_bytes out of uint16 range")));

    /* And bound `avail`, which had no check at all (PEND-17). It is used below to form
     * `(char *) &hdr + avail`, where hdr is an 8-byte stack object -- so any value past
     * sizeof(hdr) builds an out-of-range pointer, undefined on formation, and a large
     * one points arbitrarily far into the stack. The probe's own comment calls avail
     * "the notional page bytes left", i.e. a page-scale quantity, which is precisely the
     * magnitude that is wrong here: the real caller passes a pointer into a real page,
     * while this passes an offset from a local.
     *
     * BLCKSZ is the bound rather than sizeof(hdr) because the span function's job is to
     * decide whether a record FITS the bytes it is given, and a suite must be able to
     * ask about a page-sized budget as well as a too-small one. What it must not do is
     * form a pointer outside the object -- so the buffer below is a real BLCKSZ
     * allocation rather than the 8-byte local, which makes every avail in range a
     * pointer into a live object. */
    if (avail < 0 || avail > BLCKSZ)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_term_entry_span: avail must be within [0, %d]",
                        BLCKSZ)));

    memset(&hdr, 0, sizeof(hdr));
    hdr.termlen   = (uint16) termlen;
    hdr.pos_bytes = (uint16) pos_bytes;

    /* The end pointer is derived from a real BLCKSZ buffer, not from &hdr, so no
     * out-of-range pointer is ever formed however large avail is. The header the
     * function reads is copied into the head of that buffer. */
    {
        char   *buf = (char *) palloc0(BLCKSZ);
        int64   span;

        memcpy(buf, &hdr, sizeof(hdr));
        span = (int64) bm25_pending_term_entry_span(buf, buf + avail);
        pfree(buf);
        PG_RETURN_INT64(span);
    }
}

/* Append one record's entries to the buffered document. `end` is the same page
 * bound (it.end_ptr) the iterator already validated this doc's term entries
 * against -- passed through rather than re-derived so this second walk checks the
 * identical bound instead of trusting the iterator's earlier pass blindly. */
static void
drain_doc_add_part(DrainDoc *d, const BM25PendingDocHeader *dh, uint32 field_count,
                    const char *end)
{
    MemoryContext   old = MemoryContextSwitchTo(d->cxt);
    const char     *scan = (const char *) dh + bm25_pending_doc_entries_off(dh);
    uint32          k;

    for (k = 0; k < dh->ndocterms; k++)
    {
        Size            entry_len = bm25_pending_term_entry_span(scan, end);
        const BM25PendingTermEntry *te = (const BM25PendingTermEntry *) scan;
        const char     *term = scan + sizeof(BM25PendingTermEntry);
        const uint8    *pb = (const uint8 *) (term + te->termlen);
        const uint8    *pbend = pb + te->pos_bytes;
        uint32          fld = te->field_id;
        char           *termcopy;
        /* uint64, not uint32, so the running sum cannot WRAP past the bound below.
         * bm25_varbyte_decode yields a full 32-bit delta, so a hostile or torn blob
         * could carry `prev` around 2^32 and land back under PG_UINT16_MAX with a
         * non-monotonic position that the check then waves through. Widening the
         * accumulator makes the bound mean what it says; the value is still narrowed
         * to int only after it has been bounded. */
        uint64          prev = 0;
        uint16          r;

        /* field_id comes off a WAL-logged page written by this AM; a value past
         * field_count would index the arrays below out of bounds. Issue #303.H: this
         * used to skip the entry, which sealed the document without those postings,
         * silently -- the opposite of ADR 0027's decode-boundary discipline, which is
         * to raise. The merge path already raises on the same value class
         * (bm25_accum_add_posting); the drain now matches it, and so blocks the seal
         * until REINDEX rather than losing postings.
         *
         * Residual: the query-side pending readers (pending df and score passes in
         * bm25_stats.c and bm25_scan_rank.c, the phrase stash in bm25_scan_match.c)
         * still skip such an entry without raising. None of them indexes out of
         * bounds, and the seal that would make the drop permanent now refuses.
         * Scope: the D1 on-disk validation contract. */
        if (fld >= field_count)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: pending posting field %u exceeds field count %u",
                            fld, field_count),
                     errhint("REINDEX the index.")));

        termcopy = palloc(Max(te->termlen, 1));
        memcpy(termcopy, term, te->termlen);

        if (d->ntok[fld] + (int) te->tf > d->cap[fld])
        {
            int want = Max(d->cap[fld] * 2, d->ntok[fld] + (int) te->tf);

            d->cap[fld] = Max(want, 16);
            d->toks[fld] = d->toks[fld] == NULL
                ? palloc(sizeof(BM25Token) * d->cap[fld])
                : repalloc(d->toks[fld], sizeof(BM25Token) * d->cap[fld]);
        }

        /* Decode the term's TRUE positions from its delta-varbyte blob
         * (D-POS-PENDING): tf ascending positions, first delta relative to 0 -- the
         * inverse of the append-side encode and of the seg-builder frame codec. Expand
         * one token per occurrence at its real source position so a repeated term
         * ('alpha bravo alpha' -> alpha@{0,2}) seals with correct positions instead of
         * dict-order ordinals. */
        for (r = 0; r < te->tf; r++)
        {
            uint32 delta;

            pb += bm25_varbyte_decode(pb, pbend, &delta);

            /* Issue #303.F: the list strictly ascends (ADR 0087 dedups within a run),
             * so a zero delta after the first is corruption. Sealing it would write a
             * repeated position into the segment, where the sealed reader's own
             * ascent check would then refuse every phrase over it. No wrap check
             * is needed: `prev` is uint64 and bounded to PG_UINT16_MAX by the check
             * below on every step. Raising here blocks the seal until REINDEX, as the
             * merge path's own decode checks do (bm25_accum.c, ADR 0027). */
            if (r > 0 && delta == 0)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: pending position list of field %u does not ascend: "
                                "delta 0 after position " UINT64_FORMAT,
                                fld, prev),
                         errhint("REINDEX the index.")));
            prev += delta;

            /* Bound the decoded POSITION, not just the blob's byte extent (which
             * bm25_pending_term_entry_span already did). This became load-bearing when
             * doclen stopped being the token count: bm25_accum_add_field_tokens now
             * derives the field's length as max(position) + 1, so a torn or hostile
             * delta run could set doclen_by_field / total_len_by_field to an arbitrary
             * uint32 and skew the sealed segment's avgdl for the whole corpus, where
             * before it was implicitly capped by the record's own entry count.
             *
             * PG_UINT16_MAX is the right ceiling and not an arbitrary one: both ingest
             * paths reject a document above PG_UINT16_MAX tokens (#158, ADR 0079), a
             * position is a per-field ordinal over that document's SOURCE RUNS, and the
             * analyzer increments it once per run (analyzer revision 5, ADR 0087) -- so
             * a legitimately written position never reaches this bound. The bound held
             * a fortiori when position advanced per lexeme, since runs <= tokens; it is
             * only looser now, never tighter. It also keeps `prev` inside
             * int range for the (int) cast below. */
            if (prev > PG_UINT16_MAX)
                ereport(ERROR,
                        (errcode(ERRCODE_INDEX_CORRUPTED),
                         errmsg("bm25: pending position " UINT64_FORMAT
                                " exceeds the %u-token document limit",
                                prev, (unsigned) PG_UINT16_MAX)));

            d->toks[fld][d->ntok[fld]].ptr = termcopy;
            d->toks[fld][d->ntok[fld]].len = te->termlen;
            d->toks[fld][d->ntok[fld]].pos = (int) prev;
            d->ntok[fld]++;
        }

        scan += entry_len;
    }
    MemoryContextSwitchTo(old);
}

/* Register the buffered document and hand each field its whole token list. */
static void
drain_doc_flush(DrainDoc *d, BM25Accum *a, uint32 field_count)
{
    uint32  docid;
    uint32  f;

    if (!d->active)
        return;

    docid = bm25_accum_add_doc_multi(a, &d->tid);
    /* Carry the row's key into the rebuilt keymap whenever this index has one
     * (key_type != BM25_KEY_NONE) -- including a NULL-key row, whose pending record
     * already carries the zero sentinel bm25_insert wrote (bm25_build.c). Sealing it
     * here bakes that in as key 0, NOT a ctid fallback (bm25_build.c's key_field
     * handling: a NULL key and a genuine 0 are indistinguishable). Ctid fallback is
     * for a genuinely keyless index, where this branch never runs and a->keys stays
     * NULL. */
    if (d->key_type != BM25_KEY_NONE)
        bm25_accum_set_doc_key(a, docid, d->key, d->key_size);

    /* EVERY field, including ones with no tokens: add_field_tokens assigns
     * doclen_by_field[f] and a skipped field would keep whatever the array was
     * initialized to rather than an explicit 0. */
    for (f = 0; f < field_count; f++)
        bm25_accum_add_field_tokens(a, docid, f, d->toks[f], d->ntok[f]);

    /* The invariant that makes a seal score-neutral: what the SCAN read out of the
     * pending record (d->fieldlens, written by the append) is what the SEAL is about to
     * write into NORMS (re-derived from the same positions by add_field_tokens). They
     * are computed by different code from different inputs -- an explicit array versus a
     * max over decoded positions -- so this is a real cross-check, not a tautology.
     * Assert-level because a mismatch is a coding error in this AM, not user-reachable
     * corruption, and because it is per (doc, field) on the bulk seal path.
     *
     * Gated on the array being exactly field_count wide. nfieldlens is the WRITING
     * index's field_count and field_count is the READING one; they are the same
     * metapage value, fixed at CREATE INDEX, so a difference means a corrupt record --
     * NOT something bm25_pending_iter_next rejects, because that boundary bounds
     * nfieldlens against BM25_MAX_FIELDS and has no access to field_count at all.
     * Comparing a short array element-by-element would assert on whatever the other
     * fields happened to hold, so skip the check rather than report a false site. */
#ifdef USE_ASSERT_CHECKING
    if (d->have_fieldlens && d->nfieldlens == field_count)
        for (f = 0; f < field_count; f++)
            Assert(bm25_accum_doc_len_field(a, docid, f) == d->fieldlens[f]);   /* invariant */
#endif

    d->active = false;
}

/*
 * bm25_pending_tail_space_check -- the trust boundary between the metapage's CACHED
 * pending_tail_free counter and the tail page that counter is supposed to describe.
 *
 * The append budgets each on-page record against PENDING_PAGE_CAPACITY and against
 * meta.pending_tail_free, then writes at the tail page's pd_lower. Until this check,
 * nothing ever compared the record against the LOCKED PAGE's own pd_upper - pd_lower:
 * counter and page were kept in step by convention alone (`pending_tail_free -=
 * runneed` mirroring `pd_lower += runneed`). A counter that overstates the page -- a
 * torn or hostile metapage, or a stale counter left behind when the tail page was
 * recycled and re-inited as something else -- turns the append's memcpy loop into an
 * unbounded write: past pd_upper, over the BM25PageOpaque special area, and past the
 * end of the 8 KB GenericXLogState scratch image the record is staged in. That image
 * is not the last thing in its own allocation either: GenericXLogState begins with a
 * contiguous array of page images and the tail page is registered FIRST, so the first
 * thing an overrun destroys is the metapage image registered SECOND in the same
 * record. pd_lower is then stored back as a uint16, so a value in [BLCKSZ, 16336]
 * survives intact and > BLCKSZ, and Generic WAL's delta computation walks it with
 * only an Assert bounding its fixed BLCKSZ-sized delta buffer -- a second overrun,
 * invisible in a production build.
 *
 * OPERAND ORDER IS LOAD-BEARING. bm25_page_content_bytes runs FIRST and ERRORs (it
 * does not clamp) on pd_lower < SizeOfPageHeaderData, pd_lower > pd_upper, or
 * pd_upper > BLCKSZ. Only because it has already passed is the raw
 * `pd_upper - pd_lower` in the second operand safe: that is an int subtraction of two
 * uint16s, so on an INVERTED header it is negative and the (Size) cast would turn it
 * into a huge positive that satisfies any runneed silently -- the exact shape of
 * failure this check exists to prevent. The first operand also rejects a page whose
 * content already runs past where a pending page's special area starts (content >
 * PENDING_PAGE_CAPACITY), which pd_lower <= pd_upper alone permits.
 *
 * Returns the page's real free byte count (so bm25_debug_pending_tail_space can report
 * it); the append itself only cares that this returned at all.
 */
static Size
bm25_pending_tail_space_check(Page tailpage, Size runneed, uint32 claimed_free,
                              BlockNumber tailblk)
{
    PageHeader  ph = (PageHeader) tailpage;

    if (bm25_page_content_bytes(tailpage) > PENDING_PAGE_CAPACITY ||
        (Size) (ph->pd_upper - ph->pd_lower) < runneed)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending tail page cannot hold the record the metapage budgeted"),
                 errdetail("Block %u has pd_lower = %u, pd_upper = %u (a pending page holds "
                           "%zu content bytes); the record needs %zu bytes and the metapage "
                           "claims %u bytes free.",
                           tailblk, (uint32) ph->pd_lower, (uint32) ph->pd_upper,
                           (Size) PENDING_PAGE_CAPACITY, runneed, claimed_free)));

    return (Size) (ph->pd_upper - ph->pd_lower);
}

static void bm25_pending_page_flags_validate(uint16 flags, BlockNumber blk);

/*
 * bm25_pending_tail_page_validate -- the kind check for a page the appender is about to
 * write as the pending list's tail (issue #302.B), whether it appends in place or links
 * a new page behind it.
 *
 * meta.pending_tail arrives off the metapage, which bm25_meta_validate has already
 * kept off block 0 (#302.A). It may still name any other page. Every pending READER
 * checks the kind (bm25_pending_walk_read, bm25_pending_iter_begin); the appender, the
 * one pending consumer that writes, used to check only the byte budget, so a tail
 * naming a catalog, DICT or POST page with room took a pending record or a forward
 * link into it, WAL-logged and replicated. On the link path the old tail's seg_gen also
 * became the new page's chain epoch (#291), so a segment page would donate a segment
 * gen. A DELETED page is refused too: bm25_page_mark_deleted leaves BM25_PAGE_PENDING
 * set, so the kind test passes a freed pending page, and the live tail is never freed
 * (the seal resets pending_tail in the record that drains it, and the singleton
 * ShareLock this append holds keeps the seal out).
 *
 * Order as bm25_pending_iter_begin's: bm25_page_content_bytes first, so an all-zero
 * page or a misplaced special area (#302.D) is refused before the opaque read. Called
 * after LockBuffer and before GenericXLogStart and bm25_page_alloc, so an ERROR
 * unwinds with nothing written and nothing allocated.
 */
static void
bm25_pending_tail_page_validate(Page pg, BlockNumber blk)
{
    uint16      flags;

    (void) bm25_page_content_bytes(pg);
    flags = BM25PageGetOpaque(pg)->flags;
    bm25_pending_page_flags_validate(flags, blk);
    if (flags & BM25_PAGE_DELETED)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending list tail block %u is a freed page", blk),
                 errdetail("Its flags are 0x%x, which include BM25_PAGE_DELETED.",
                           flags)));
}

/* Append one document's per-field de-duplicated postings to the pending list.
 *
 * A term is de-duplicated WITHIN each field: the same term appearing in two
 * indexed columns produces TWO term entries (each tagged with its field_id), so a
 * drain reconstructs the exact per-(term,field) postings a build would.
 *
 * Per-field doclen is STORED (v8), as a uint32 array between the doc header and the
 * term entries, and the header's doclen is its sum. It used to be reconstructed by the
 * readers as sum-of-tf over a field's entries, which is exact only while doclen counts
 * TOKENS; doclen counts emitting source RUNS (max position + 1, see
 * bm25_accum_add_field_tokens), and the two part company as soon as an analyzer emits
 * several lexemes for one run. Storing it is what keeps a pending document's score
 * equal to the same document's score after a seal. See src/bm25_format.h's pending
 * region comment for the record layout and the self-describing flag.
 *
 * Concurrency / WAL discipline: the metabuffer is held EXCLUSIVE for the whole
 * append. Every part is ONE Generic WAL record: the metapage, plus the tail page
 * the part lands on, plus -- when the part needs a fresh page -- the old tail whose
 * forward link points at it (three buffers, inside the 4-buffer cap). It used to be
 * three records on the new-page path, and the gaps between them leaked pages only
 * the orphan sweep could recover; see "ONE RECORD" at the allocation below for why
 * that no longer suffices now the sweep is gated (issue #300). heaprel is pre-opened by the
 * caller and threaded into bm25_page_alloc (D-ALLOC/M6); the stamp-and-gate allocator
 * only ReadBuffer/ConditionalLockBuffers index pages and consults the in-memory
 * horizon, so calling it under the metapage lock is safe.
 *
 * WHAT "SAFE" MEANS HERE, PRECISELY (XCUT-09, issue #145). Two wrong versions of
 * this paragraph are on record and both are kept here so neither is reintroduced:
 * it originally said the allocator "never table_opens or blocks", and the first
 * attempt to narrow that replaced it with "it may wait on the relation-extension
 * lock", which is false in the other direction. What is actually true, verified
 * against the PG 18 source rather than reasoned about:
 *
 *   - It never table_opens. Unchanged, and still why heaprel is threaded in.
 *   - Its FSM candidates go through ConditionalLockBuffer, so a page another
 *     backend holds is skipped, not waited on; and the eviction a ReadBuffer miss
 *     may trigger takes the victim's content lock with LWLockConditionalAcquire
 *     (GetVictimBuffer, bufmgr.c) and retries another victim on failure. The one
 *     content lock it can WAIT for is the P_NEW page's own, which a share-mode
 *     orphan sweep (issue #300) can hold briefly after sampling nblocks past the
 *     extend; that sweep holds no other content lock meanwhile, so no cycle can
 *     form. This is the property that makes nesting the allocator under the
 *     metapage LWLock (and, on the new-page path, the old tail's) deadlock-free.
 *   - It does NOT take the relation-extension lock. ReadBuffer_common passes
 *     EB_SKIP_EXTENSION_LOCK for a P_NEW blockNum, so ExtendBufferedRelShared
 *     skips LockRelationForExtension outright. ReadBufferExtended's own header
 *     states the consequence: "Caller is responsible for ensuring that only one
 *     backend tries to extend a relation at the same time!" -- without that lock
 *     the smgrnblocks/smgrzeroextend pair is not atomic, so two concurrent
 *     extenders can both claim the same block.
 *
 * THE METAPAGE LOCK IS WHAT DISCHARGES THAT OBLIGATION, which makes it
 * load-bearing for relation-extension correctness and not merely for the pending
 * anchor. Every maintenance writer that can extend -- seal, merge, drain, VACUUM
 * reclaim -- holds LockPage(BM25_METAPAGE_BLKNO, ExclusiveLock); this appender
 * holds LockPage(..., ShareLock) plus the metapage BUFFER content lock EXCLUSIVE.
 * ShareLock does not self-conflict, so it is the buffer lock that serializes two
 * concurrent appenders against each other; ShareLock DOES conflict with the
 * singleton's ExclusiveLock, so an appender and a seal can never be extending at
 * the same moment. Do not narrow either lock's scope without putting something
 * else in place that serializes extends.
 *
 * Core nbtree extends the same way, under its own page's write lock
 * (_bt_allocbuf, called from _bt_split), so the nesting shape itself is standard;
 * what is specific to this AM is which lock supplies the one-extender-at-a-time
 * guarantee. Other sites cite this comment to justify their own lock nesting --
 * see PEND-19 at the call site below. */
void
bm25_pending_append_multi(Relation index, Relation heaprel, ItemPointer tid,
                          BM25Token **toks_by_field, const int *ntok_by_field,
                          uint32 field_count,
                          uint8 key_type, uint16 key_size,
                          const unsigned char *key /* key_size bytes, or NULL */)
{
    BM25MetaPageData    meta;
    Buffer              metabuf;
    Buffer              tailbuf;
    GenericXLogState   *state;
    Page                metapage;
    Page                tailpage;
    BM25MetaPageData   *m;
    char               *dst;
    BM25PendingDocHeader hdr;
    /* Distinct (field, term) entries, in first-seen order, de-duplicated through
     * an HTAB keyed on (field_id, term bytes). This used to be a linear scan over
     * all previously-seen entries, justified by "docs are short relative to the
     * corpus" -- an unenforced precondition on a value the user supplies. */
    HTAB               *seen;
    HASHCTL             hctl;
    int                 total_tok = 0;
    uint16             *e_field;    /* field_id of each distinct entry */
    uint16             *e_tf;       /* aggregated tf of each distinct entry */
    int                *e_tokidx;   /* index into the entry's field token list */
    uint32             *e_fld;      /* which field's toks[] e_tokidx indexes */
    /* M4: per distinct entry, the TRUE ascending occurrence positions (collected in
     * token order during de-dup) and the delta-varbyte encoding of that list. The
     * blob is what the drain feeds to the accumulator so an inserted-then-sealed doc
     * carries real positions (not fabricated ordinals) into the POS chain. */
    uint32            **e_pos;      /* ascending positions of each distinct entry */
    int                *e_poscap;   /* allocated slots in e_pos[j] (grows by doubling) */
    uint8             **e_posblob;  /* delta-varbyte encoding of e_pos[j] */
    uint16             *e_posbytes; /* byte length of e_posblob[j] */
    int                 ndistinct = 0;
    uint32              doclen = 0;
    /* v8: the per-field doclens written into every record of this document. */
    uint32              fieldlen[BM25_MAX_FIELDS];
    Size                hdrspan;    /* header + doclen array, MAXALIGN'd */
    uint32              f;
    int                 j;
    int                 j2;         /* entry cursor within the part being written */
    int                 part;       /* 0-based record ordinal for this document */

    /* Callers pass meta.field_count, which bm25_meta_validate has already bounded to
     * 1..BM25_MAX_FIELDS; the fixed-width array above (and DrainDoc's, on the read
     * side) index on it. */
    Assert(field_count >= 1 && field_count <= BM25_MAX_FIELDS);   /* checked: bm25_meta_validate */

    for (f = 0; f < field_count; f++)
    {
        int     i;
        uint32  fl = 0;

        total_tok += ntok_by_field[f];

        /* doclen is the count of emitting source runs, i.e. max position + 1 -- the
         * SAME expression bm25_accum_add_field_tokens uses on the build path and on
         * this document's own eventual drain, which is what makes a pending score and
         * a sealed score equal. Deliberately not ntok_by_field[f]: that is the token
         * count, which only coincides with the run count while the analyzer emits one
         * token per run. A max-scan rather than the last token's position because
         * nothing in this function's contract promises the caller's array is ordered
         * by position (the analyzer's is; the drain's rebuilt one is not). */
        for (i = 0; i < ntok_by_field[f]; i++)
            if ((uint32) toks_by_field[f][i].pos + 1 > fl)
                fl = (uint32) toks_by_field[f][i].pos + 1;

        fieldlen[f] = fl;
        doclen += fl;
    }
    hdrspan = bm25_pending_doc_header_size(field_count);

    /* Reject a document with too many TOKENS before doing any per-token work.
     *
     * This used to be a page-capacity check (need >= MAXALIGN(header) + total_tok, since
     * every token contributes at least one varbyte byte to some entry's position blob),
     * which is what capped an INSERT at one page's worth of document. v7 lets a document
     * span pages, so that bound is gone -- but the check itself cannot simply be deleted,
     * because H7/#47 leaned on it for a second, unrelated reason:
     *
     *   e_tf below is uint16, and so is BM25PendingTermEntry.tf ON DISK. A term occurring
     *   more than 65535 times in one field wraps the counter to 0 during de-dup, which
     *   yields a zero-length position blob, a small `need`, and an ACCEPTED document
     *   carrying tf = 0 -- present in the table and unfindable through its index, with the
     *   loss landing at seal time. The old page bound capped total_tok far below 65535 as
     *   a side effect, so the wrap was unreachable.
     *
     * Keep that guarantee explicitly rather than by accident: a document's total token
     * count must stay inside uint16. That is ~65k tokens, roughly 170x the old ~380-entry
     * ceiling and far past any natural prose document (a 2000-word article is ~2000
     * tokens), so it bounds the per-token work below without being a limit real content
     * meets. Widening tf to uint32 would be an on-disk change for a case no corpus has. */
    if (total_tok > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: document has too many tokens for the pending list"),
                 errdetail("The document has %d tokens; the limit is %d, because a "
                           "pending term entry stores its term frequency in 16 bits.",
                           total_tok, PG_UINT16_MAX),
                 errhint("%s", BM25_PENDING_TOO_LARGE_HINT)));

    e_field  = palloc0(sizeof(uint16) * Max(total_tok, 1));
    e_tf     = palloc0(sizeof(uint16) * Max(total_tok, 1));
    e_tokidx = palloc(sizeof(int) * Max(total_tok, 1));
    e_fld    = palloc(sizeof(uint32) * Max(total_tok, 1));
    e_pos      = palloc(sizeof(uint32 *) * Max(total_tok, 1));
    e_poscap   = palloc(sizeof(int) * Max(total_tok, 1));
    e_posblob  = palloc(sizeof(uint8 *) * Max(total_tok, 1));
    e_posbytes = palloc0(sizeof(uint16) * Max(total_tok, 1));

    MemSet(&hctl, 0, sizeof(hctl));
    hctl.keysize   = sizeof(BM25PendKey);
    hctl.entrysize = sizeof(BM25PendEnt);
    hctl.hash      = bm25_pendkey_hash;
    hctl.match     = bm25_pendkey_match;
    hctl.keycopy   = bm25_pendkey_copy;
    /* PEND-07: HASH_CONTEXT, and it is not optional here.
     *
     * Without hcxt, dynahash creates the table's own context under
     * TopMemoryContext, which survives statement AND transaction abort. This is a
     * per-inserted-row path, and the loop between this create and the
     * hash_destroy below can throw -- hash_search(HASH_ENTER) on OOM, and the
     * position-list repalloc further down -- with no PG_TRY in between, so every
     * such failure stranded the table (64 buckets plus everything grown) for the
     * life of the backend. bm25_insert's per-row scratch context exists precisely
     * to stop per-row accumulation, and this allocation escaped it.
     *
     * CurrentMemoryContext here IS that scratch context (bm25_insert switches into
     * it before calling), so the table now dies with the row's other scratch on
     * every path, thrown or not. This was also the one hash_create in the tree
     * without the flag, and the comment on BM25PendKey above claims this HTAB has
     * the "same shape" as bm25_seg_dict.c's wildcard dedup set -- which sets hcxt.
     * The claim is true now; it was a doc mismatch before. */
    hctl.hcxt      = CurrentMemoryContext;
    seen = hash_create("bm25 pending append dedup", 64, &hctl,
                       HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_KEYCOPY |
                       HASH_CONTEXT);

    for (f = 0; f < field_count; f++)
    {
        BM25Token  *toks = toks_by_field[f];
        int         ntok = ntok_by_field[f];
        int         i;

        for (i = 0; i < ntok; i++)
        {
            BM25PendKey     k;
            BM25PendEnt    *ent;
            bool            found;

            /* De-dup is per FIELD, so field_id is part of the key: the same term
             * in two columns is two entries. */
            k.field = f;
            k.ptr   = toks[i].ptr;
            k.len   = toks[i].len;
            ent = (BM25PendEnt *) hash_search(seen, &k, HASH_ENTER, &found);

            if (!found)
            {
                ent->idx            = ndistinct;
                e_fld[ndistinct]    = f;
                e_tokidx[ndistinct] = i;
                e_field[ndistinct]  = (uint16) f;
                e_tf[ndistinct]     = 1;
                /* Start small and double. Pre-sizing every entry to this field's
                 * WHOLE token count made a document with D distinct terms among T
                 * tokens cost D*T*4 bytes -- 3.1 GB for a 200 KB value. Geometric
                 * growth makes the total across all entries O(T). */
                e_poscap[ndistinct] = 8;
                e_pos[ndistinct]    = palloc(sizeof(uint32) * e_poscap[ndistinct]);
                e_pos[ndistinct][0] = (uint32) toks[i].pos;
                ndistinct++;
            }
            else
            {
                int     d = ent->idx;

                if (e_tf[d] == e_poscap[d])
                {
                    e_poscap[d] *= 2;
                    e_pos[d] = repalloc(e_pos[d], sizeof(uint32) * e_poscap[d]);
                }
                e_pos[d][e_tf[d]] = (uint32) toks[i].pos;
                e_tf[d]++;
            }
        }
    }

    hash_destroy(seen);

    /* Delta-varbyte-encode each entry's ascending position list into its posblob.
     * The analyzer emits tokens in position order per field, so e_pos[j] is already
     * ascending; the first delta is pos[0] itself (relative to 0), matching the
     * seg-builder's frame codec. */
    for (j = 0; j < ndistinct; j++)
    {
        uint32  prev = 0;
        uint16  r;
        uint8  *pp;

        e_posblob[j] = palloc((Size) e_tf[j] * BM25_VARBYTE_MAX_BYTES);
        pp = e_posblob[j];
        for (r = 0; r < e_tf[j]; r++)
        {
            pp += bm25_varbyte_encode(e_pos[j][r] - prev, pp);
            prev = e_pos[j][r];
        }

        /* Check the narrowing rather than assuming it (PEND-13). e_posbytes is uint16
         * and this cast is silent, so an overlong blob would wrap to a small length,
         * the entry would be written with a byte count that does not describe it, and
         * every later reader would stride by the wrong amount -- a desync produced at
         * WRITE time, which no read-side trust boundary can catch.
         *
         * It is currently unreachable, but by a chain with EXACTLY ZERO margin: the
         * token cap rejects a document above PG_UINT16_MAX tokens, so a field holds at
         * most 65535, the deltas of an ascending sequence sum to at most 65534, and a
         * varbyte run over them is at most 65535 bytes -- the largest value that fits,
         * hit exactly when a single term occupies every token slot. Any future change
         * to the cap, the codec, or the ascending precondition consumes margin that
         * does not exist. Cheap enough to assert once per entry at insert time. */
        if ((Size) (pp - e_posblob[j]) > PG_UINT16_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bm25: position blob of %zu bytes exceeds the %u-byte "
                            "pending-entry limit",
                            (Size) (pp - e_posblob[j]), (unsigned) PG_UINT16_MAX)));

        e_posbytes[j] = (uint16) (pp - e_posblob[j]);
    }

    /* v7: the whole-document byte need is no longer computed or checked here. It used to
     * gate the single-page write; the per-part write loop below now budgets each record
     * against page capacity as it packs it, which is the same arithmetic applied per
     * record instead of per document. Position blobs are included there, as they must be
     * -- a document that fits by term bytes alone would overflow once its posblobs land.
     *
     * A document too large for one page is written as several
     * same-tid parts (see the write loop below). What still cannot be split is a SINGLE
     * entry: its term bytes and position blob are contiguous, so one entry plus a doc
     * header has to fit one page. Check it here, before any page is touched, so an
     * impossible document fails at the same point it always did rather than partway
     * through a multi-part write. Reachable only with a huge position blob, i.e. one
     * term repeated tens of thousands of times in a single field. */
    {
        Size    hdrneed = hdrspan;

        for (j = 0; j < ndistinct; j++)
        {
            Size esz = MAXALIGN(sizeof(BM25PendingTermEntry) +
                                toks_by_field[e_fld[j]][e_tokidx[j]].len +
                                e_posbytes[j]);

            if (hdrneed + esz > PENDING_PAGE_CAPACITY)
                ereport(ERROR,
                        (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                         errmsg("bm25: pending term entry too large for a page"),
                         errdetail("One (field, term) entry needs %zu bytes (term %d "
                                   "bytes, %d positions in %d bytes) and a document "
                                   "header needs %zu; a page holds %zu.",
                                   esz, toks_by_field[e_fld[j]][e_tokidx[j]].len,
                                   (int) e_tf[j], (int) e_posbytes[j], hdrneed,
                                   (Size) PENDING_PAGE_CAPACITY),
                         errhint("%s", BM25_PENDING_TOO_LARGE_HINT)));
        }
    }

    /*
     * D-SEAL: exclude seals for the duration of the append.
     *
     * The metapage BUFFER lock below is NOT enough. LockPage is the lmgr
     * heavyweight LOCKTAG_PAGE lock and LockBuffer is the buffer-content LWLock --
     * different lock managers, no conflict between them -- so the seal singleton
     * did not exclude an appender at all. The production aminsert path appends
     * here and only THEN tries ConditionalLockPage (bm25_build.c), so the append
     * ran entirely outside the singleton, and the invariant the publish record
     * relies on ("no append can land between the drain snapshot and the publish")
     * was simply false: a document appended after the drain walked past the tail
     * page was published by nobody, then had its page reset to Invalid and
     * recycled by bm25_pending_truncate. Committed, heap-visible, and findable
     * through the index only after a REINDEX.
     *
     * ShareLock, not ExclusiveLock: it conflicts with the sealer's ExclusiveLock
     * (so appends and seals genuinely exclude each other) while ShareLock does not
     * conflict with itself, so concurrent inserters still append in parallel.
     * Heavyweight before buffer, as everywhere else in this AM. Released below;
     * an ereport in between unwinds it via transaction abort.
     *
     * ponytail: one lmgr acquire/release per inserted row. Cheap next to the
     * tokenizing already done above, and it makes an existing invariant true
     * instead of reworking the publish protocol. If it ever shows up in an
     * insert-heavy profile, the alternative is to have the publish record reset
     * the anchor to what the drain actually consumed rather than to Invalid/0 --
     * which needs per-document drain granularity, because a partially-drained
     * tail page must be neither re-drained (BM25 scores are additive: the
     * single-record-seal invariant) nor truncated.
     */
    LockPage(index, BM25_METAPAGE_BLKNO, ShareLock);

    metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    /* Copy under the held lock. This also runs the format gate (bm25_meta_validate,
     * folded into _locked), which is still the right thing to do here: it must hold
     * under THIS lock, and the seal path reaches this function without passing through
     * aminsert at all.
     *
     * It is no longer aminsert's FIRST validated metapage read, though. #188 put a
     * bm25_meta_read (which validates) in bm25_insert ahead of the analyzer fingerprint
     * gate, so on the insert path the version floor is now met earlier still -- before
     * tokenization, and before the all-NULL early-out. One visible consequence: an
     * all-NULL row into an index below BM25_OLDEST_READABLE used to return false without
     * ever validating, and now errors. That is the correct answer either way; it just
     * arrives for a row that previously slipped past. */
    bm25_meta_read_locked(metabuf, &meta);

    /* v7 multi-part write. One iteration = one on-page record. A document that fits a
     * page produces exactly one; a larger one produces as many as it needs, all carrying
     * this tid, parts after the first flagged BM25_PENDING_DOC_CONT. (v7 made the
     * single-part case byte-identical to v6. v8 no longer is -- every record now carries
     * the per-field doclen array -- which is what BM25_MIN_READ_PENDING_FIELDLENS is
     * for.)
     *
     * Parts do NOT need their own pages, only to be CONSECUTIVE RECORDS in chain order,
     * which they are: the metapage buffer is held EXCLUSIVE for the whole loop and every
     * appender takes it, so no other document can interleave. The drain reassembles by
     * accumulating consecutive same-tid parts.
     *
     * ATOMICITY comes from the transaction, not from the WAL. Each part is its own
     * GenericXLog record (staying inside the 4-buffer cap), so a crash between parts
     * leaves a partial document on the chain -- and Generic WAL page writes survive abort,
     * so it stays there. That is already a state this design tolerates: the inserting
     * transaction did not commit, so the tid is invisible, and the partial postings are
     * exactly the aborted-insert orphans the pending list already carries until a seal
     * tombstones them. A partially-written document is therefore indistinguishable from a
     * fully-written one whose insert rolled back. (This is why the ADR's earlier
     * build-as-orphans-then-link scheme was unnecessary.) */
    part = 0;
    j = 0;
    while (j < ndistinct || part == 0)      /* always emit at least one record */
    {
        Size    runneed = hdrspan;
        int     jstart = j;
        Buffer  oldtailbuf = InvalidBuffer;  /* the tail a new page is linked from */
        bool    newtail = false;             /* this part allocated its page */
        uint32  new_epoch = 0;               /* #291 epoch stamped on that page */

        /* Greedily take entries while they fit this record's page. The
         * one-entry-per-page check above guarantees progress, so this cannot spin. */
        while (j < ndistinct)
        {
            Size esz = MAXALIGN(sizeof(BM25PendingTermEntry) +
                                toks_by_field[e_fld[j]][e_tokidx[j]].len +
                                e_posbytes[j]);

            if (runneed + esz > PENDING_PAGE_CAPACITY)
                break;
            runneed += esz;
            j++;
        }

        /* No block-0 test here: meta came through bm25_meta_read_locked, whose
         * bm25_meta_validate refuses a pending_tail of 0 (issue #302.A). That matters
         * because ReadBuffer of block 0 would hand back the metapage buffer this call
         * holds EXCLUSIVE, and the LockBuffer after it would wait on itself with
         * interrupts held. Within this loop pending_tail only ever becomes a page
         * bm25_page_alloc returned, never block 0. The tail's kind is checked after
         * each LockBuffer below (bm25_pending_tail_page_validate). */

    /* choose/allocate a tail page with room.
     *
     * ONE RECORD (issue #300). When this part needs a new page, the page's init, the
     * old tail's forward link and the metapage update (new pending_tail, and
     * pending_head plus the epoch's next_gen bump on a chain start) all ride the
     * single append record below, with this part's data already on the new page.
     * They used to be three records -- init the new page, link the old tail, then
     * the data+metapage record -- and every gap between them was an orphan source:
     * a crash or an ERROR after the init left an initialised PENDING page nothing
     * linked, and one after the link left the page hanging off the old tail past
     * meta.pending_tail, where the NEXT append's link overwrote the pointer and
     * orphaned it then, on a successful path. The orphan sweep was the only thing
     * that ever freed those pages, and the sweep is now gated on durable evidence
     * that orphans can exist (bm25_reclaim_orphans) -- which an appender could not
     * provide without writing evidence on every allocation. With one record there
     * is nothing to provide: a crash or an ERROR between the allocation and the
     * record leaves at most a zero page from P_NEW or a DELETED page popped out of
     * the FSM, both free-space drift the crash epoch (or the next dead-op sweep)
     * recovers, never an initialised page nothing reaches. Three buffers (metapage,
     * old tail, new page), inside Generic WAL's four. */
    if (meta.pending_tail == InvalidBlockNumber || meta.pending_tail_free < runneed)
    {
        /* D-ALLOC/M6: the stamp-and-gate allocator only touches index buffers
         * (ReadBuffer + ConditionalLockBuffer) and the in-memory visibility horizon --
         * it never table_opens a heap, and it never waits on a content lock that a
         * backend holding a lock we need could be waiting behind -- so calling it
         * here under the metapage exclusive lock and the old tail's is
         * deadlock-free. ("Never waits on a content lock at all" would be false: the
         * P_NEW branch's LockBuffer(EXCLUSIVE) on the freshly extended page can wait
         * on a share-mode orphan sweep that sampled nblocks after the extend and
         * locked the zero page. That sweep holds no other content lock while it
         * does, and it only records the page free -- harmless, by stamp-and-gate.)
         * heaprel was pre-opened by the caller (never table_open under a buffer lock)
         * and is threaded down so the allocator can horizon-gate a retired-page
         * reuse. (XCUT-09: it takes NO relation-extension lock either -- P_NEW skips
         * it -- which is why the metapage lock this call sits under is what keeps
         * extends single-threaded. See this function's header.)
         *
         * PEND-19 (issue #145) -- WHY THE FSM WORK IS NESTED IN THE METAPAGE LOCK ON
         * PURPOSE, AND WHY THE ALTERNATIVE WAS REJECTED. bm25_page_alloc consults
         * GetFreeIndexPage, may extend the relation, and re-records rejected
         * candidates with RecordFreeIndexPage -- all of it here inside block 0's
         * EXCLUSIVE content lock, which every concurrent inserter serializes behind.
         * The obvious alternative -- pre-allocate the pages this document needs
         * BEFORE taking the metapage lock -- does not work, for three independent
         * reasons, so this is deliberate rather than an oversight:
         *
         *  1. The page count is not knowable outside the lock. The PART count is
         *     (it falls out of ndistinct/e_posbytes above), but how many parts need
         *     a NEW page depends on meta.pending_tail_free, which lives on the
         *     metapage and is only stable under this very lock. Estimating from an
         *     unlocked read and falling back under the lock just makes the nesting
         *     rarer, not gone.
         *  2. Surplus pre-allocations cannot be handed back cheaply. bm25_page_alloc
         *     accepts only pages stamped BM25_PAGE_DELETED and permanently drops
         *     unmarked ones from the FSM (see its header), so returning an unused
         *     page means a Generic WAL record to stamp it -- an extra record and FSM
         *     churn on EVERY insert, including the overwhelmingly common one-part
         *     insert that lands in the existing tail's free space.
         *  3. Releasing the metapage BETWEEN parts (the other half of that proposal)
         *     is a correctness regression, not a trade-off. The multi-part contract
         *     documented above requires a document's parts to be CONSECUTIVE RECORDS
         *     in chain order, and the held metapage lock is the only thing providing
         *     that; the drain reassembles a document by accumulating consecutive
         *     same-tid parts, so an interleaved appender would split one document
         *     into two and mis-attribute the postings.
         *
         * What we do instead is keep the hold BOUNDED: the allocator's FSM scan is
         * capped (BM25_ALLOC_MAX_REJECTS) and its candidate locks are conditional. */
        uint32 epoch;

        /* #291: the chain epoch the new page is stamped with.
         *
         * A chain START draws next_gen and bumps it in the local metapage copy. The
         * bump reaches disk in this part's append record -- the SAME record that
         * inits the page and publishes the new pending_head -- so no reader can see
         * the new head without also seeing next_gen past its epoch. That pairing is
         * the whole argument: a scan that captured (pending_head, next_gen) under one
         * metapage lock has every page of its chain below next_gen, and any page
         * re-inited after the capture, as a page of a chain started later or as a
         * segment page (whose gen bm25_next_gen also draws later), at or above it. No
         * separate record and no new metapage field. Because the init rides the same
         * record, an epoch is never stamped on a page without the bump reaching disk
         * too, so no two chains ever share one.
         *
         * Any later page copies the epoch off the current tail, which is locked
         * EXCLUSIVE here for the link and so cannot change under the read. A tail
         * written by an older binary carries 0 and so does the rest of its chain: 0
         * is "no epoch", which the walker passes, so a mixed-binary chain is
         * unprotected rather than spuriously rejected. */
        if (meta.pending_tail != InvalidBlockNumber)
        {
            oldtailbuf = ReadBuffer(index, meta.pending_tail);
            LockBuffer(oldtailbuf, BUFFER_LOCK_EXCLUSIVE);
            /* Before the epoch read and before the allocation (#302.B). */
            bm25_pending_tail_page_validate(BufferGetPage(oldtailbuf), meta.pending_tail);
            epoch = BM25PageGetOpaque(BufferGetPage(oldtailbuf))->seg_gen;
        }
        else
        {
            /* The same exhaustion guard as bm25_next_gen (issue #313 META-08), before
             * the window below opens. */
            bm25_next_gen_check(index, meta.next_gen);
            epoch = meta.next_gen;
            meta.next_gen += 1;
        }

        /* Hoisted above the allocation: GenericXLogStart pallocs, and from the moment
         * bm25_page_alloc hands back a page to GenericXLogFinish nothing may throw, or
         * the page is stranded (see the ONE RECORD note above). It registers nothing
         * and locks nothing, so opening it early is free. */
        state = GenericXLogStart(index);
        tailbuf = bm25_page_alloc(index, heaprel);     /* EXCL-locked */
        newtail = true;
        /* Test lever (t/028): a page is allocated and nothing is written yet. Parked
         * here the backend holds three content locks, so it cannot be cancelled; the
         * suite only ever ends it with an immediate shutdown. */
        bm25_debug_pause_point("pending_append_alloc");
        new_epoch = epoch;

        if (meta.pending_head == InvalidBlockNumber)
            meta.pending_head = BufferGetBlockNumber(tailbuf);
        meta.pending_tail = BufferGetBlockNumber(tailbuf);
        meta.pending_tail_free = PENDING_PAGE_CAPACITY;
        meta.pending_npages += 1;
    }
    else
    {
        tailbuf = ReadBuffer(index, meta.pending_tail);
        LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
        bm25_pending_tail_page_validate(BufferGetPage(tailbuf), meta.pending_tail);  /* #302.B */

        /* Budget this record against the PAGE now that it is locked, not against the
         * metapage's cached counter (see bm25_pending_tail_space_check). Placed after
         * the LockBuffer -- the page header is only stable under the content lock --
         * and before GenericXLogStart, so no WAL window is open: an ereport here just
         * unwinds, and the abort drops both buffer locks and the seal ShareLock with
         * nothing written. Inside the per-part loop so EVERY part landing on an
         * existing tail is checked. A part that just allocated its page needs no
         * check: the page is fresh, and the greedy loop above never takes more than
         * PENDING_PAGE_CAPACITY (asserted below). */
        bm25_pending_tail_space_check(BufferGetPage(tailbuf), runneed,
                                      meta.pending_tail_free, meta.pending_tail);
        state = GenericXLogStart(index);
    }
    Assert(runneed <= PENDING_PAGE_CAPACITY);   /* checked: the greedy take above */

    /* Metapage registered FIRST: generic_redo locks the blocks EXCLUSIVE in
     * registration order, so this order is the standby's lock order and must
     * follow the index-wide metapage-first rule (ADR 0018, issue #240). The old
     * tail comes next and the new page last, the order this backend locked them in
     * and the order a chain walker meets them. */
    metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
    if (BufferIsValid(oldtailbuf))
    {
        Page op = GenericXLogRegisterBuffer(state, oldtailbuf, 0);

        BM25PageGetOpaque(op)->nextblk = BufferGetBlockNumber(tailbuf);
    }
    tailpage = GenericXLogRegisterBuffer(state, tailbuf,
                                         newtail ? GENERIC_XLOG_FULL_IMAGE : 0);
    if (newtail)
    {
        bm25_page_init(tailpage, BM25_PAGE_PENDING);
        BM25PageGetOpaque(tailpage)->seg_gen = new_epoch;
    }

    dst = (char *) tailpage + ((PageHeader) tailpage)->pd_lower;
    /* WAL determinism: zero the whole header REGION on the page -- the struct's own
     * trailing/interior padding, and the MAXALIGN slack after the v8 doclen array.
     * These bytes land below pd_lower on a Generic-WAL-logged page, so leaving any of
     * them uninitialized makes the page image non-deterministic (replica byte
     * divergence). The staged `hdr` is memset for the same reason before fill, matching
     * the term-entry discipline below and encode_block's BM25BlockHeader. */
    MemSet(dst, 0, hdrspan);
    memset(&hdr, 0, sizeof(hdr));
    hdr.tid = *tid;
    hdr.nfieldlens = (uint16) field_count;
    hdr.ndocterms = (uint32) (j - jstart);
    /* Every part carries the WHOLE document's length. The drain takes it from the
     * first part (the one that registers the docid) and ignores it on continuations,
     * so a part's own entry count and the doc's length stay independent. */
    hdr.doclen = doclen;
    /* v8: every record carries the doclen array, and says so. Repeated on
     * continuations for the same reason key/doclen are -- each record stays
     * self-describing, so a walker that meets a stranded continuation (ADR 0069) still
     * strides it correctly. */
    hdr.flags = BM25_PENDING_DOC_FIELDLENS | ((part > 0) ? BM25_PENDING_DOC_CONT : 0);
    /* M5 key_field: carry the row's key so the next seal writes it into a KEYMAP. The
     * memset above already zeroed key_type/key[]; for a NULL key VALUE those zero
     * bytes stay (key_type below is still this index's real type), and the drain
     * later seals them as key 0, NOT a ctid fallback -- a NULL key and a genuine 0
     * are indistinguishable (bm25_build.c's key_field handling). Ctid fallback is for
     * a genuinely KEYLESS index (key_type == BM25_KEY_NONE), where the drain never
     * calls bm25_accum_set_doc_key at all. Repeated on every part so each
     * record stays self-describing (and so a VACUUM sweep that invalidates one part's
     * tid leaves the others structurally intact). */
    hdr.key_type = key_type;
    hdr.key_size = key_size;
    if (key_type != BM25_KEY_NONE && key != NULL)
        memcpy(hdr.key, key, key_size);
    memcpy(dst, &hdr, sizeof(hdr));
    memcpy(dst + sizeof(hdr), fieldlen, sizeof(uint32) * field_count);
    dst += hdrspan;
    for (j2 = jstart; j2 < j; j2++)
    {
        BM25PendingTermEntry te;
        BM25Token *tk = &toks_by_field[e_fld[j2]][e_tokidx[j2]];
        int tl = tk->len;
        /* WAL determinism: zero the whole entry (incl. any struct padding around
         * field_id/pos_bytes) before fill so the on-page bytes are deterministic. */
        memset(&te, 0, sizeof(te));
        te.termlen = (uint16) tl;
        te.tf = e_tf[j2];
        te.field_id = e_field[j2];
        te.pos_bytes = e_posbytes[j2];
        memcpy(dst, &te, sizeof(te));
        memcpy(dst + sizeof(te), tk->ptr, tl);
        /* Position blob follows the term bytes; the drain reads it back at
         * the same [sizeof(te) + termlen] offset. */
        memcpy(dst + sizeof(te) + tl, e_posblob[j2], e_posbytes[j2]);
        dst += MAXALIGN(sizeof(BM25PendingTermEntry) + tl + e_posbytes[j2]);
    }
    ((PageHeader) tailpage)->pd_lower = dst - (char *) tailpage;

    m = BM25PageGetMeta(metapage);
    memcpy(m, &meta, sizeof(meta));
    m->pending_tail_free -= runneed;
    if (part == 0)
        m->pending_ndocs += 1;      /* once per DOCUMENT, not per record */

    /* Lazy floor raise, in the SAME Generic WAL record as the record that needs it, so
     * the floor is durable before any reader can observe that record. Same
     * Max()-not-assign, monotonic discipline as bm25_meta_set_pd_lower: the floor is
     * NOT lowered when a seal drains these records, because a reader may hold a
     * snapshot across the seal.
     *
     * Two capabilities, one floor. v8's per-field doclen array is on EVERY record and
     * would make a v7 reader stride from the header straight into the array's bytes as
     * a term entry; v7's continuation records would make a v6 reader register one docid
     * per part. The v8 floor dominates today, which is exactly why both are named here
     * rather than folded into one literal -- the span floor is still the correct answer
     * to "what does a continuation record require", and it must not silently vanish if
     * the doclen array is ever retired. */
    {
        uint32  floor_needed = BM25_MIN_READ_PENDING_FIELDLENS;

        if (part > 0)
            floor_needed = Max(floor_needed, BM25_MIN_READ_PENDING_SPAN);
        if (m->min_read_version < floor_needed)
            m->min_read_version = floor_needed;
    }
    bm25_meta_set_pd_lower(metapage);

    GenericXLogFinish(state);
    UnlockReleaseBuffer(tailbuf);
    if (BufferIsValid(oldtailbuf))
        UnlockReleaseBuffer(oldtailbuf);

    /* Carry the metapage changes into our local copy so the next part sees the new
     * tail/free/head, then loop. metabuf stays EXCLUSIVE-locked across every part. */
    bm25_meta_read_locked(metabuf, &meta);
    part++;
    }
    UnlockReleaseBuffer(metabuf);

    /* The document is durably on the pending chain and counted in the metapage, so
     * any seal from here on will see it. Release before the caller's opportunistic
     * ConditionalLockPage(ExclusiveLock). */
    UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);

    for (j = 0; j < ndistinct; j++)
    {
        pfree(e_pos[j]);
        pfree(e_posblob[j]);
    }
    pfree(e_field);
    pfree(e_tf);
    pfree(e_tokidx);
    pfree(e_fld);
    pfree(e_pos);
    pfree(e_posblob);
    pfree(e_posbytes);
}

/* Single-field convenience: append `toks` under field 0. Every single-field caller
 * (bm25_debug_pending_append, tests) goes through the identical code path as the
 * multi-field one -- which since v8 means its records carry a one-element doclen array
 * like everyone else's, so this is path-identical to the M3 path but no longer
 * byte-identical to it. */
void
bm25_pending_append(Relation index, Relation heaprel, ItemPointer tid,
                    BM25Token *toks, int ntok)
{
    bm25_pending_append_multi(index, heaprel, tid, &toks, &ntok, 1,
                              BM25_KEY_NONE, 0, NULL);
}

/*
 * Read the key_field configuration off the PENDING list: the (key_type, key_size)
 * pair carried by its first keyed document header. Returns false (leaving the
 * outputs untouched) for an empty pending list or a keyless one.
 *
 * WHY A SCAN NEEDS THIS. The ranked-scan builders discover the key config from the
 * first live SEGMENT carrying a KEYMAP, which is correct whenever one exists -- all
 * segments of an index share the config. But an index built on an empty table and
 * then INSERTed into has no such segment, so the config was BM25_KEY_NONE and the
 * scan took the keyless path: ranked_keys was never allocated and bm25_score_key
 * returned NULL for every row, not merely for the pending ones (#204).
 *
 * Validates the tag rather than trusting it, for the same reason the drain does
 * (PEND-09): both bytes come off a pending page, and this pair becomes the stride
 * a scan later indexes ranked_keys by.
 *
 * Takes the caller's CAPTURED head rather than re-reading the metapage. A ranked
 * scan pins its view of the pending list once, under the metapage lock, and every
 * other pending read on that path walks the captured chain (the recycle horizon
 * keeps it walkable afterwards on the primary; on a standby the epoch check in
 * bm25_pending_walk_read turns a recycled page into a 40001, so epoch_bound is the
 * snapshot's next_gen). Re-reading here would let a concurrent seal -- another
 * backend's INSERT crossing seal_threshold -- move the head between the snapshot and
 * this call, and report "keyless" for an index that is not.
 */
bool
bm25_pending_keymeta(Relation index, BlockNumber pending_head,
                     uint32 epoch_bound,
                     uint8 *out_type, uint16 *out_size)
{
    BlockNumber     blk = pending_head;
    PGAlignedBlock  copy;
    BM25PendingWalk w;

    bm25_pending_walk_init(&w, index, epoch_bound);
    while (blk != InvalidBlockNumber)
    {
        Buffer          buf;
        Page            pg;
        BM25PendingIter it;
        BlockNumber     next;

        CHECK_FOR_INTERRUPTS();
        buf = bm25_pending_walk_read(&w, blk);

        memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg   = (Page) copy.data;
        next = BM25PageGetOpaque(pg)->nextblk;

        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;

            if (!ItemPointerIsValid(&dh->tid) || dh->key_type == BM25_KEY_NONE)
                continue;

            bm25_seg_keymeta_validate(dh->key_type, dh->key_size);
            *out_type = dh->key_type;
            *out_size = dh->key_size;
            bm25_pending_iter_end(&it);
            return true;
        }
        bm25_pending_iter_end(&it);

        blk = next;
    }
    return false;
}

bool
bm25_pending_should_seal(Relation index)
{
    BM25MetaPageData meta;
    Size used;
    bm25_meta_read(index, &meta);
    if (meta.pending_npages == 0)
        return false;
    used = (Size) meta.pending_npages * PENDING_PAGE_CAPACITY -
           (Size) meta.pending_tail_free;
    return used >= (Size) bm25_seal_threshold_kb * 1024;
}

/* PEND-03/PEND-08 (H6): the decode boundary for one page reached via a
 * pending-list chain -- it must carry BM25_PAGE_PENDING.
 *
 * Modelled on bm25_retired_page_flags_validate (bm25_fsm.c), which guards all
 * three retired-list walkers with the same rationale: "a corrupt or stray nextblk
 * landing on an unrelated live page would otherwise be read straight through".
 * The pending chain was the one family with no such gate, across eleven walkers,
 * and it is also the one family recycled with no XID horizon -- so the stale-page
 * read was not hypothetical, and its failure mode was a DICT or POST page's bytes
 * decoded as BM25PendingDocHeader records (ItemPointerIsValid(&dh->tid) filters
 * some noise; arbitrary page bytes frequently look like a valid ItemPointerData).
 *
 * NOT a cycle guard, and it must not be mistaken for one: bm25_page_mark_deleted
 * ORs BM25_PAGE_DELETED and LEAVES BM25_PAGE_PENDING set, so a page revisited by a
 * looping walk passes this check every time round. See bm25_pending_truncate.
 *
 * blk == InvalidBlockNumber selects the block-less message, for the callers that
 * hold only a Page (bm25_pending_iter_begin) and for the debug probe below; the
 * chain walkers that know where they are pass their block number. Extracted so
 * that probe exercises the identical check the production walkers run. */
static void
bm25_pending_page_flags_validate(uint16 flags, BlockNumber blk)
{
    if (flags & BM25_PAGE_PENDING)
        return;

    if (blk == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending-list page is not a pending page (flags 0x%x)",
                        flags)));
    else
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending-list chain reached block %u, which is not "
                        "a pending page (flags 0x%x)",
                        blk, flags)));
}

/* Decode-boundary probe. No bm25_debug_* lever can aim a pending chain at a
 * wrong-kind page (that needs a corrupt nextblk), so -- exactly like
 * bm25_debug_retired_page_bounds -- this drives the SAME check over a
 * caller-chosen flags word. blk is fixed at InvalidBlockNumber so the message
 * carries no block number and the expected output stays portable. TEST-ONLY: a
 * pure function of its scalar argument, no relation touched. Returns flags on
 * success. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_page_flags_validate);
Datum
bm25_debug_pending_page_flags_validate(PG_FUNCTION_ARGS)
{
    int32   flags = PG_GETARG_INT32(0);

    if (flags < 0 || flags > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_page_flags_validate: flags out of uint16 range")));

    bm25_pending_page_flags_validate((uint16) flags, InvalidBlockNumber);
    PG_RETURN_INT32(flags);
}

/* #291: the chain-epoch half of a captured-chain page's validation.
 *
 * WHY THE HORIZON IS NOT ENOUGH. bm25_pending_truncate and bm25_reclaim_orphans stamp
 * a drained page with a retire_xid, and bm25_page_alloc reuses it only once
 * GlobalVisCheckRemovableFullXid passes -- on the PRIMARY. A standby query's snapshot
 * does not hold that horizon back unless hot_standby_feedback is on (default off, and
 * asynchronous), and nothing on this path raises a recovery conflict: Generic WAL redo
 * carries no snapshotConflictHorizon. So the primary can drain, recycle and re-init a
 * page while a standby scan that captured the old pending_head is between two pages
 * of it. Re-inited as a new PENDING page it passed the kind gate, and the scan
 * followed the new chain and silently missed the old chain's documents; re-inited as
 * a segment page it failed the kind gate as XX002 on a healthy index.
 *
 * THE RULE. page_epoch == 0 passes: a chain an older binary started, which carries no
 * epoch (bm25_page_init's default), so mixing binaries leaves such a chain
 * unprotected rather than rejecting it with a 40001 no retry would clear. Otherwise
 * the page must predate the snapshot: page_epoch < epoch_bound, the next_gen captured
 * with pending_head (bm25_pending_append_multi draws a chain's epoch and publishes the
 * chain in one metapage record; a segment page's seg_gen comes from the same
 * counter). >= rather than an exact match, because a scan also walks pages appended
 * to its chain after the capture, and those carry the chain's own (older) epoch.
 *
 * NOT COVERED: a page re-initialized as a SEGCAT or RETIRED page. Catalog and retired-
 * descriptor pages carry seg_gen = 0 (bm25_page_init), which passes here, so the kind gate
 * in bm25_pending_page_flags_validate fires instead and a healthy standby index raises
 * XX002, not 40001. Loud, never silent, and narrow: a merge or a retire has to take the
 * recycled page inside the window. Turning it into a 40001 would mask real corruption on
 * a standby as retryable, so it is a recorded residual (docs/adr/0110).
 *
 * ERRCODE_T_R_SERIALIZATION_FAILURE, like the segment reader's gen check and like
 * core's own recovery-conflict cancellations: the index is healthy and a fresh query
 * succeeds. The ranked path's existing retry absorbs it; @@@ reports it. Checked
 * BEFORE the page kind, for the reason bm25_seg_page_validate_kind gives: a page
 * reused as another kind fails both, and only this answer is the true one.
 *
 * blk == InvalidBlockNumber selects the block-less detail, for the debug probe. */
static void
bm25_pending_epoch_validate(uint32 page_epoch, uint32 epoch_bound, BlockNumber blk)
{
    if (page_epoch == 0 || page_epoch < epoch_bound)
        return;

    if (blk == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                 errmsg("bm25: pending list recycled concurrently; retry"),
                 errdetail("The page carries generation %u; this scan's snapshot "
                           "admits generations below %u.", page_epoch, epoch_bound)));
    else
        ereport(ERROR,
                (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
                 errmsg("bm25: pending list recycled concurrently; retry"),
                 errdetail("Pending-list block %u carries generation %u; this "
                           "scan's snapshot admits generations below %u, so the "
                           "page was re-initialized after the scan began.",
                           blk, page_epoch, epoch_bound),
                 errhint("On a hot standby, hot_standby_feedback = on makes this "
                         "rarer.")));
}

/* Decode-boundary probe for the epoch rule above, in the family of
 * bm25_debug_pending_page_flags_validate: the real trigger needs a standby and a
 * concurrent recycle (t/025), so this drives the SAME check over caller-chosen
 * values. TEST-ONLY: a pure function of its scalar arguments, no relation touched.
 * Returns page_epoch on success. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_epoch_validate);
Datum
bm25_debug_pending_epoch_validate(PG_FUNCTION_ARGS)
{
    int64   page_epoch = PG_GETARG_INT64(0);
    int64   epoch_bound = PG_GETARG_INT64(1);

    if (page_epoch < 0 || page_epoch > PG_UINT32_MAX ||
        epoch_bound < 0 || epoch_bound > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_epoch_validate: argument out of uint32 range")));

    bm25_pending_epoch_validate((uint32) page_epoch, (uint32) epoch_bound,
                                InvalidBlockNumber);
    PG_RETURN_INT64(page_epoch);
}

/* ---- BM25PendingWalk: the validated walk of a captured chain (#291) ----
 *
 * The scan side's counterpart of the checks the writer-side walkers (drain, truncate,
 * VACUUM's sweep) already carry: extent (bm25_blk_in_extent, re-sampling on a would-be
 * violation, because appenders keep extending and linking while a scan walks -- the
 * "Pending extent bound" note below), the visited-count cycle cap, then the epoch and
 * the kind. The scan walkers had only the kind gate; a link out of the relation
 * reached ReadBuffer's own short-read error and an in-extent cycle spun until
 * cancelled. One helper so the eleven walkers cannot drift apart again.
 *
 * Returns the buffer pinned and SHARE-locked: some walkers copy the page and release
 * at once (copy-then-unlock, docs/adr/0063), others decode in place; this keeps both
 * shapes without a forced copy on the hot per-term walks. Every check reads the locked
 * page, so the copy a caller then takes is the page that was checked. */
void
bm25_pending_walk_init(BM25PendingWalk *w, Relation index, uint32 epoch_bound)
{
    w->index = index;
    w->nblocks = 0;             /* the first bm25_blk_in_extent samples */
    w->visited = 0;
    w->epoch_bound = epoch_bound;
}

Buffer
bm25_pending_walk_read(BM25PendingWalk *w, BlockNumber blk)
{
    Buffer      buf;
    Page        pg;

    /* No CHECK_FOR_INTERRUPTS here: every caller runs one at the top of its page
     * loop, just before this call, where it holds no buffer (and where the
     * interrupt-check floors count it). */
    if (!bm25_blk_in_extent(w->index, blk, &w->nblocks))
        bm25_seg_chain_extent_validate(blk, w->nblocks, "pending-list chain");
    bm25_pending_cycle_cap_validate(w->visited, w->nblocks, blk);
    w->visited++;

    buf = ReadBuffer(w->index, blk);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);

    /* bm25_page_content_bytes first: it rejects an all-zero page before the opaque
     * read below, for the reason bm25_pending_iter_begin gives. Error exits here
     * leave the buffer locked; transaction abort releases it. */
    (void) bm25_page_content_bytes(pg);
    bm25_pending_epoch_validate(BM25PageGetOpaque(pg)->seg_gen, w->epoch_bound, blk);
    bm25_pending_page_flags_validate(BM25PageGetOpaque(pg)->flags, blk);
    return buf;
}

/* ---- BM25PendingIter: the ONE canonical in-page pending walker ----
 * Used by bm25_pending_drain (below) AND bm25_pending_mark_dead (Phase 3) so the
 * per-doc record stride is defined in exactly one place. Operates on a Page whose
 * contents region holds physically-consecutive {BM25PendingDocHeader + term
 * entries} groups up to pd_lower. Holds no resources; _end is a no-op kept for
 * source-compatibility with a possible future buffered variant. */
void
bm25_pending_iter_begin(BM25PendingIter *it, Page page)
{
    Size content_bytes;

    /* PEND-03: this is the ONE canonical entry point for every pending walker, so
     * the page-kind gate its peer chain families already carry belongs here and
     * nowhere else.
     *
     * THE PRECONDITION, stated as a property rather than a census: every caller
     * hands us a page image that carries its SPECIAL AREA, so BM25PageGetOpaque
     * below is valid. Three provenances satisfy that -- BufferGetPage on a pinned,
     * content-locked buffer; a full-BLCKSZ PGAlignedBlock memcpy of such a page
     * (whose owner then also reads BM25PageGetOpaque(pg)->nextblk off that same
     * copy after the walk, rather than off the released buffer); and
     * GenericXLogRegisterBuffer's registered copy. A partial or contents-only image
     * would not, and must not be passed here. This used to enumerate the call sites
     * by count and provenance; the count drifted twice (it said eleven against
     * thirteen sites) and issue #145 moved another site between provenances, so the
     * property is what is maintained now.
     *
     * ORDER: bm25_page_content_bytes runs FIRST, deliberately. It is the only
     * thing standing between an all-zeroes page and the opaque read: a zero page
     * has pd_special == 0, which trips PageGetSpecialPointer's
     * (pd_special >= SizeOfPageHeaderData) assertion inside BM25PageGetOpaque on a
     * cassert build and reads the page HEADER as the opaque on a production one.
     * bm25_page_content_bytes rejects pd_lower == 0 first, so the flags read below
     * is unreachable for such a page. Both derivations still happen after both
     * checks, which is what PEND-03 asks for. */
    content_bytes = bm25_page_content_bytes(page);
    bm25_pending_page_flags_validate(BM25PageGetOpaque(page)->flags,
                                     InvalidBlockNumber);

    /* end_ptr is the bound every check bm25_pending_iter_next and
     * bm25_pending_term_entry_span make is measured against -- so it cannot itself
     * be a bare, unvalidated pd_lower read. A corrupt pd_lower in
     * [0, SizeOfPageHeaderData) would otherwise put end_ptr BEFORE cur_ptr
     * (PageGetContents), which makes the very next call's `cur_ptr >= end_ptr`
     * loose check true and iter_next return false immediately -- every walker
     * (drain, VACUUM mark_dead, the membership/score/phrase scans in bm25_scan.c,
     * bm25_scan_rank.c and bm25_scan_match.c, the wildcard walk in bm25_seg_dict.c)
     * would then treat a corrupt page as EMPTY rather than erroring: documents
     * silently vanish instead of the loud ERRCODE_INDEX_CORRUPTED every other trust
     * boundary in this branch raises. bm25_page_content_bytes bounds
     * pd_lower before either pointer is derived from it. */
    it->cur_ptr = (char *) PageGetContents(page);
    it->end_ptr = (char *) PageGetContents(page) + content_bytes;
    it->cur = NULL;
}

bool
bm25_pending_iter_next(BM25PendingIter *it)
{
    BM25PendingDocHeader *dh;
    char                 *p;
    uint32                k;

    if (it->cur_ptr >= it->end_ptr)
        return false;

    /* Bound the header itself before dh->ndocterms (or any other field) is read
     * below: the loose cur_ptr < end_ptr check above only proves at least one byte
     * remains, not a whole header's worth. A torn or hostile page can leave less,
     * and ndocterms is what the loop below trusts as a stride count next. */
    if (it->cur_ptr + MAXALIGN(sizeof(BM25PendingDocHeader)) > it->end_ptr)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending doc header overruns the page")));

    dh = (BM25PendingDocHeader *) it->cur_ptr;

    /* v8 decode boundary, and THE one for nfieldlens: this iterator is the single
     * gateway every pending walker goes through, so bounding the count here is what
     * lets bm25_pending_doc_entries_off state "nfieldlens has been bounded" as a
     * precondition instead of re-checking at eleven call sites. A raw on-page uint16 is
     * about to become a stride; a v8 record must name at least one field and no more
     * than the format allows. (A record WITHOUT the flag never reads the count at all
     * -- on a pre-v8 record those two bytes are struct padding, and while the append
     * path has always zeroed them, nothing is read that would depend on it.) */
    if ((dh->flags & BM25_PENDING_DOC_FIELDLENS) != 0 &&
        (dh->nfieldlens == 0 || dh->nfieldlens > BM25_MAX_FIELDS))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending doc header declares %u per-field doclens",
                        (uint32) dh->nfieldlens),
                 errdetail("A pending record's doclen array holds 1..%d entries.",
                           BM25_MAX_FIELDS)));

    /* The header REGION (header + doclen array) is wider than the fixed struct the
     * check above bounded, and it is the next thing used as a stride. */
    if (it->cur_ptr + bm25_pending_doc_entries_off(dh) > it->end_ptr)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending doc header region overruns the page")));

    it->cur = dh;
    /* advance cur_ptr past this whole doc group (header region + all term entries);
     * M4: each entry is strided by term bytes AND its trailing position blob.
     * bm25_pending_term_entry_span bounds ndocterms/termlen/pos_bytes -- all raw
     * on-page counts -- against it->end_ptr before any of them contributes to the
     * stride (ADR 0027/0039's shape: validate before a value becomes a bound). */
    p = it->cur_ptr + bm25_pending_doc_entries_off(dh);
    for (k = 0; k < dh->ndocterms; k++)
        p += bm25_pending_term_entry_span(p, it->end_ptr);
    it->cur_ptr = p;
    return true;
}

void
bm25_pending_iter_end(BM25PendingIter *it)
{
    (void) it;          /* no resources held; present for symmetry/future use */
}

/* Cheap pre-scan (no WAL window): does this pending page hold any live entry whose
 * TID the callback marks dead? bm25_pending_mark_dead (Phase 3) calls this before
 * opening a GenericXLog window so pages with no deletions are skipped. */
bool
bm25_pending_page_has_dead(Page page, IndexBulkDeleteCallback cb, void *cb_state)
{
    BM25PendingIter it;

    bm25_pending_iter_begin(&it, page);
    while (bm25_pending_iter_next(&it))
    {
        if (ItemPointerIsValid(&it.cur->tid) && cb(&it.cur->tid, cb_state))
        {
            bm25_pending_iter_end(&it);
            return true;
        }
    }
    bm25_pending_iter_end(&it);
    return false;
}

/* ---- Pending extent bound (issue #243) ----
 *
 * Extent bound for a pending-chain walker whose extent sample can go stale while it
 * walks. The pending walkers below share the SEGCAT walkers' helper,
 * bm25_blk_in_extent (bm25_seg_read.c; ADR 0095 addendum): the re-sample is the same,
 * the reason it is needed differs.
 *
 * VACUUM's sweep holds the seal singleton in ShareLock mode, and appenders
 * (bm25_pending_append_multi) take the SAME mode, so inserts run for the whole sweep.
 * Each one that fills the tail allocates a page with bm25_page_alloc, which can extend
 * the relation, and links it in as the new tail. A sweep that reaches that link finds
 * a healthy block at or past its sample. Sampling later does not help, because
 * appends continue after any sample; so the sample is re-read on a would-be violation
 * instead. The appender extends BEFORE it links, and the walker read the link under
 * the page's buffer lock, so a sample taken now covers every block the link can name.
 * A block still at or past it is out of the index for real.
 *
 * Steady-state cost stays one lseek per walk: a healthy sweep re-samples only when it
 * actually meets a page added after its sample. Each re-sample follows a distinct
 * link past the previous extent, so the number of re-samples is bounded by the
 * number of extensions during the walk. */

/* bm25_pending_mark_dead -- VACUUM's pending-list sweep (Phase 3, Task 17).
 *
 * Walk the pending page chain; for every live entry whose TID the callback marks
 * dead, set its slot's TID invalid in place so drain/scan skip it (both already
 * treat an invalid-TID slot as a hole -- see bm25_pending_drain). Returns the count
 * cleared so bm25_bulkdelete can fold it into stats->tuples_removed.
 *
 * WAL discipline (two records per dirtied page, never straddling):
 *   1. Per-page slot invalidation rides one Generic WAL record on the registered
 *      copy of THAT page only. The cheap bm25_pending_page_has_dead pre-scan runs
 *      first (no WAL window) so clean pages never open one -- matching the GIN-style
 *      "don't dirty a page you won't change" rule.
 *   2. The metapage counter decrement is a SEPARATE Generic WAL record, taken only
 *      when a page actually changed, with pd_lower re-asserted past the struct so
 *      page-hole compression keeps it -- the same reason ginfast.c's
 *      ginHeapTupleFastInsert re-sets pd_lower past GinMetaPageData before it
 *      WAL-logs the metapage (checked against PG 18.3; cited by function name
 *      because a line number into another tree rots with every release).
 * Keeping the two records distinct stays within the 4-buffer Generic WAL cap and
 * means no metapage lock is held across the per-page window. The metapage is read
 * once up front only to find pending_head; each decrement re-reads the live struct
 * from its own registered page copy, so concurrent counter changes are not clobbered.
 *
 * Fix (2026-08, crash/replica-safety pass): this used to decrement meta->ndocs
 * alongside pending_ndocs. meta->ndocs is documented SEGMENT-ONLY (see the
 * "segment-only; pending folded in by stats reader" comment on its publish-record
 * assignment in bm25_seg_build.c) and is only ever incremented at seal -- a pending
 * doc was never added to it, so tombstoning one here decremented a counter it had
 * no claim on, corrupting the sealed-document count that feeds every avgdl/idf
 * computation (bm25_score.c) for segment-resident documents, silently skewing
 * every score, not just the tombstoned pending docs' own. pending_ndocs is the
 * correct (and now sole) counter for this path; the corpus-stats reader never
 * consults it anyway -- pending_global_stats (bm25_scan_rank.c) recomputes the live
 * pending doc count and sumdoclen by walking the pending list and testing
 * ItemPointerIsValid, the exact flag this function clears, so pending_ndocs is
 * administrative bookkeeping (the opportunistic-seal threshold, bm25_stats
 * reporting) rather than a value the scorer trusts directly.
 *
 * sumdoclen (meta->total_len) is not touched here either, and there is nothing to
 * adjust: a pending document never contributes to meta->total_len. The append
 * touches only the metapage's pending-list fields, next_gen and min_read_version,
 * never ndocs or total_len. total_len is written by seal publish and merge swap
 * (bm25_seg_build.c) and decremented by bulkdelete's tombstone of SEGMENT docs; no
 * pending path touches it. The corpus-stats reader adds the live pending doclens
 * itself (pending_global_stats), so a tombstoned pending document drops out of avgdl
 * by being skipped in that walk.
 *
 * `nblocks` is the caller's extent sample and only a starting point: see
 * the "Pending extent bound" note above for why this walk re-samples it. The public entry point
 * (bm25_pending_mark_dead, after this function) takes the sample itself;
 * bm25_debug_pending_sweep passes a stale one to drive the re-sample path. `npages`,
 * when not NULL, receives the number of chain pages visited, so the probe can tell a
 * walk that completed from one that stopped short. */
static uint64
pending_mark_dead_sampled(Relation index, IndexBulkDeleteCallback cb, void *cb_state,
                          BlockNumber nblocks, uint64 *npages)
{
    BM25MetaPageData    meta;
    BlockNumber         blk;
    uint64              removed = 0;
    uint64              visited = 0;

    /* PEND-06: take the seal singleton for the WHOLE sweep.
     *
     * Without it this walker and bm25_pending_drain can leapfrog each other on a
     * multi-part document, because both go head-to-tail releasing each page before
     * reading the next, and the ONLY thing that made the drain's per-page SHARE
     * locks add up to a stable snapshot was the seal's LockPage(ExclusiveLock) --
     * a heavyweight lock this sweep never asked for. The interleaving:
     *
     *   sealer:  drain reads P1 SHARE, buffers part 0 of spanning document D
     *            (TID still valid), releases P1
     *   vacuum:  takes P1 EXCLUSIVE, invalidates D's part 0; then P2 EXCLUSIVE,
     *            invalidates D's part 1
     *   sealer:  reads P2 SHARE, sees part 1 with an invalid TID, `continue`s --
     *            and later flushes D carrying ONLY part 0's tokens
     *
     * D is then published into a segment with a truncated token list and a wrong
     * per-field doclen, which feeds wrong total_len_by_field[] / ndocs_by_field[]
     * into the segment header. D's heap tuple is dead so it is never RETURNED, but
     * its corpus-statistics contribution skews every other document's score until
     * a merge rewrites the segment. The comment on the drain's skip asserts the
     * opposite -- that TID-equality invalidation takes every part of a multi-part
     * document together, so no continuation can be orphaned -- which is true only
     * if the two passes are serialized. This is what serializes them.
     *
     * ShareLock, not ExclusiveLock: it does not conflict with itself, so it does
     * not serialize this sweep against a concurrent appender (which holds the same
     * mode) -- but it DOES conflict with the sealer's ExclusiveLock, which is the
     * whole requirement. Released on error by transaction abort, the same
     * discipline bm25_pending_append_multi uses for its own acquisition. */
    LockPage(index, BM25_METAPAGE_BLKNO, ShareLock);

    bm25_meta_read(index, &meta);
    blk = meta.pending_head;
    /* blk < nblocks is the same corruption backstop bm25_fsm.c's mark_chain and
     * bm25_reclaim_orphans use on the FSM's own chains -- the pending list is an
     * ordinary singly-linked nextblk
     * chain and had no bound of its own. The check sits here, before this
     * iteration's ReadBuffer and before bm25_pending_page_has_dead's cheap
     * pre-scan opens anything, so a cancel never lands inside the per-page
     * GenericXLog window further down (that window is opened and closed within
     * one iteration of THIS loop, never spanning two).
     *
     * #243: the bound goes through bm25_blk_in_extent, which re-samples before it
     * calls a link out of range. nblocks is therefore not constant across the loop,
     * and the cycle cap below reads the refreshed value. */
    while (blk != InvalidBlockNumber && bm25_blk_in_extent(index, blk, &nblocks))
    {
        Buffer              buf;
        Page                pg;
        BM25PageOpaque     *op;
        GenericXLogState   *state;
        Page                wpg;
        BlockNumber         next;
        int                 changed = 0;
        BM25PendingIter     it;          /* Phase-1 in-page entry iterator */

        /* PEND-11: the THROTTLING form, not a bare interrupt check. This walker is
         * reached only from bm25_bulkdelete -- the very caller whose own per-doc
         * loop was converted for this reason -- and it does per-page I/O and
         * per-page WAL across the entire pending chain, so leaving it on
         * CHECK_FOR_INTERRUPTS silently exempted the sweep from autovacuum's cost
         * budget. vacuum_delay_point() calls CHECK_FOR_INTERRUPTS internally and is
         * inert when VacuumCostActive is false. Safe here for the same reason the
         * bare check was: the previous iteration already released its buffer, so
         * this is a genuinely lock-free instant. */
        BM25_VACUUM_DELAY_POINT();

        /* PEND-08, applied to this walker too: the extent bound below stops a
         * nextblk that points OUT of the relation, but not an in-extent cycle.
         * bm25_pending_truncate has carried this cap since #137; drain and this
         * sweep were left with the bound alone, i.e. merely cancellable rather
         * than bounded. Checked before any page is touched this iteration. */
        bm25_pending_cycle_cap_validate(visited, nblocks, blk);
        visited++;

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        pg = BufferGetPage(buf);
        op = BM25PageGetOpaque(pg);
        next = op->nextblk;

        /* Cheap pre-scan: skip pages with nothing dead so we never open a WAL
         * window (and never dirty a page) we don't have to. */
        if (!bm25_pending_page_has_dead(pg, cb, cb_state))
        {
            UnlockReleaseBuffer(buf);
            blk = next;
            continue;
        }

        state = GenericXLogStart(index);
        wpg = GenericXLogRegisterBuffer(state, buf, 0);
        bm25_pending_iter_begin(&it, wpg);
        while (bm25_pending_iter_next(&it))
        {
            if (ItemPointerIsValid(&it.cur->tid) &&
                cb(&it.cur->tid, cb_state))
            {
                ItemPointerSetInvalid(&it.cur->tid);
                /* Invalidate EVERY part (they all carry this tid, so the callback matches
                 * each one), but count DOCUMENTS: `changed` is what the metapage
                 * decrement below subtracts from pending_ndocs, and counting parts
                 * would over-decrement a spanning document to the point of clamping
                 * the counter at 0 (v7, #57). */
                if ((it.cur->flags & BM25_PENDING_DOC_CONT) == 0)
                    changed++;
            }
        }
        bm25_pending_iter_end(&it);
        GenericXLogFinish(state);
        UnlockReleaseBuffer(buf);

        if (changed > 0)
        {
            /* Decrement the pending doc count in its own metapage record. */
            Buffer              mbuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
            GenericXLogState   *mstate;
            Page                mpg;
            BM25MetaPageData   *mp;

            LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);
            mstate = GenericXLogStart(index);
            mpg = GenericXLogRegisterBuffer(mstate, mbuf, 0);
            mp = BM25PageGetMeta(mpg);
            mp->pending_ndocs = (mp->pending_ndocs >= (uint64) changed)
                                ? mp->pending_ndocs - changed : 0;
            /* mp->ndocs is SEGMENT-ONLY (bm25_seg_build.c's publish record) and is
             * never incremented for a pending doc, so it must never be decremented
             * for one either -- see this function's header comment. */

            /* pd_lower discipline so page-hole compression keeps the struct. */
            bm25_meta_set_pd_lower(mpg);
            GenericXLogFinish(mstate);
            UnlockReleaseBuffer(mbuf);
            removed += changed;
        }
        blk = next;
    }
    /* blk < nblocks stops the walk EARLY on a corrupt/out-of-extent nextblk --
     * see bm25_analyzer.c's bm25_fieldcfg_read for the model this follows. A
     * pending chain always terminates naturally at InvalidBlockNumber; landing
     * here with blk still holding a real (but out-of-bound) block number means
     * the bound cut the walk short, not that the chain legitimately ended. Silent
     * about that would let VACUUM skip dead entries whose heap TIDs then get
     * recycled -- a later seal would still carry them, and the index would
     * return arbitrary wrong rows. Loud beats silently wrong. Since #243 the loop
     * exits here only once blk is past an extent sampled AFTER its link was read, so
     * a page an appender linked mid-sweep no longer reaches this error. */
    if (blk != InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending-list chain exceeds the relation's extent at block %u",
                        blk)));

    if (npages != NULL)
        *npages = visited;

    /* PEND-06: normal exit. Every error path out of this walk -- the extent check
     * above, the cycle cap, bm25_pending_iter_begin's page-kind validation -- exits
     * by ereport, where transaction abort releases the lock, so there is no path
     * that leaves it held. Matches bm25_pending_append_multi's discipline. */
    UnlockPage(index, BM25_METAPAGE_BLKNO, ShareLock);
    return removed;
}

/* VACUUM's entry point (bm25_bulkdelete). The sample is taken before the seal
 * singleton, as it always was; since #243 that order is harmless, because the walk
 * re-samples before it trusts a failed bound. */
uint64
bm25_pending_mark_dead(Relation index, IndexBulkDeleteCallback cb, void *cb_state)
{
    return pending_mark_dead_sampled(index, cb, cb_state,
                                     RelationGetNumberOfBlocks(index), NULL);
}

/* A fresh chunk accumulator for the drain, configured exactly like every other
 * chunk's (BUILD-04). Both settings are silent when omitted: without the keymeta a
 * chunk writes no KEYMAP and its rows revert from key identity to ctid, and with a
 * NULL positions gate the builder emits POS frames for tokens that carry none and
 * desyncs the D2 tf-count == tf lockstep at read time. km_type is threaded in
 * rather than rediscovered because the drain learns it from the FIRST keyed doc
 * header it sees, which may have been in an earlier chunk. */
static BM25Accum *
bm25_drain_accum_new(uint32 field_count, const uint8 *store_pos,
                     uint8 km_type, uint16 km_size)
{
    BM25Accum *a = bm25_accum_begin_multi(field_count);

    bm25_accum_set_store_positions(a, store_pos);
    if (km_type != BM25_KEY_NONE)
        bm25_accum_set_keymeta(a, km_type, km_size);
    return a;
}

/* Seal one chunk into orphan pages and record its catalog entry. An accumulator
 * holding no live docs writes nothing and contributes no entry -- the case where
 * every doc on the drained pages had been tombstoned by VACUUM's pending sweep. */
static void
bm25_drain_flush_chunk(Relation index, Relation heaprel, BM25Accum *acc,
                       BM25SegCatEntry **ents, int *nent, int *capent)
{
    BM25SegmentHeader hdr;
    BlockNumber       header_blk;

    header_blk = bm25_segment_build_orphans(index, heaprel, acc, &hdr);
    if (header_blk == InvalidBlockNumber)
        return;
    if (*nent == *capent)
    {
        *capent *= 2;
        *ents = repalloc(*ents, sizeof(BM25SegCatEntry) * (*capent));
    }
    bm25_segcat_entry_from_hdr(&(*ents)[(*nent)++], header_blk, &hdr);
}

int
bm25_pending_drain(Relation index, Relation heaprel, Size budget,
                   BM25SegCatEntry **out_entries)
{
    BM25MetaPageData    meta;
    uint32              field_count;
    BM25Accum          *a;
    BlockNumber         blk;
    BlockNumber         nblocks = RelationGetNumberOfBlocks(index);
    uint64              visited = 0;
    DrainDoc            doc;    /* v7: buffers a document's consecutive same-tid parts */
    PGAlignedBlock      copy;   /* the page being decoded; see the copy below */
    uint8               store_pos[BM25_MAX_FIELDS];
    uint8               km_type = BM25_KEY_NONE;   /* carried across chunks */
    uint16              km_size = 0;
    /* #292: the key config of the first live record, keyless included, that every
     * later live record must repeat -- see the mixed-chain refusal below. */
    bool                kc_seen = false;
    uint8               kc_type = BM25_KEY_NONE;
    uint16              kc_size = 0;
    BM25SegCatEntry    *ents;
    int                 nent = 0;
    int                 capent = 8;

    bm25_meta_read(index, &meta);
    field_count = meta.field_count;
    ents = palloc(sizeof(BM25SegCatEntry) * capent);

    /* M4: carry the per-field store_positions gate into the drained accumulator so a
     * sealed-from-pending segment writes POS frames on exactly the same fields a
     * CREATE INDEX would. The flags live on the field-config page (D12 trailing
     * array); read them there (no heap needed). The pending list retains each term
     * entry's TRUE per-occurrence positions (posblob, D-POS-PENDING), so the
     * reconstructed tokens carry real source positions -- phrase search is correct on
     * inserted-then-sealed docs without a REINDEX.
     *
     * Back-compat (D12): an ABSENT flag array means the field-config PREDATES
     * positions (an M5-built index read by an M4 binary without REINDEX) -- treat it
     * as positions OFF. Pre-init store_pos all-0 and read real bits only when the
     * flag array is present; an M4-built page overwrites the 0s. And call
     * bm25_accum_set_store_positions UNCONDITIONALLY: a NULL gate on the accumulator
     * is treated as all-ON, which would make the builder emit POS frames for tokens
     * that carry none (legacy pending) and desync the tf-count==tf lockstep (D2).
     * An explicit all-0 gate keeps the sealed segment position-less, matching D13. */
    {
        BM25FieldConfigHeader   fchdr;
        BM25FieldConfig         fcfg[BM25_MAX_FIELDS];
        uint32                  f;

        for (f = 0; f < BM25_MAX_FIELDS; f++)
            store_pos[f] = 0;               /* absent flag array / legacy index => positions OFF */
        if (meta.field_config_blkno != InvalidBlockNumber)
            bm25_fieldcfg_read(index, meta.field_config_blkno, &fchdr, fcfg, store_pos);
    }
    a = bm25_drain_accum_new(field_count, store_pos, km_type, km_size);

    drain_doc_init(&doc, field_count);
    blk = meta.pending_head;
    /* Same corruption backstop as bm25_pending_mark_dead above. */
    while (blk != InvalidBlockNumber && blk < nblocks)
    {
        Buffer  buf;
        Page    pg;
        BM25PendingIter it;
        BlockNumber next;

        /* PEND-11: throttling form. This walker is shared -- reached from
         * bm25_vacuumcleanup -> bm25_seal_index AND from the user-backend
         * opportunistic seal -- but vacuum_delay_point() is a no-op whenever
         * VacuumCostActive is false, so it is correct on both paths and only
         * engages the cost budget on the one that has one.
         *
         * One unit of cancellable work per pending page drained (#156): the unit
         * sql/80_maintenance_interrupts' seal assertion measures, since a seal is
         * drain-then-build and the drain is what a small timeout lands in. Counted
         * beside the delay point rather than by it -- see BM25_WORK_UNIT() in bm25.h.
         * sql/80 injects its cancel at the pause point ahead of both, so the seal's
         * cancellation is measured from a fixed step rather than from a timer. */
        bm25_debug_pause_point("drain_pending_page");
        BM25_WORK_UNIT();
        BM25_VACUUM_DELAY_POINT();

        /* PEND-08 for this walker: see bm25_pending_mark_dead. */
        bm25_pending_cycle_cap_validate(visited, nblocks, blk);
        visited++;

        /*
         * BUILD-04: the chunk boundary. HERE, at the top of the page loop, and
         * NOT under a content lock.
         *
         * Sealing a chunk allocates pages and writes WAL. Doing that while holding
         * a pending page's content lock would be the lock-hold defect ADR 0041 and
         * ADR 0083 are about -- ProcessInterrupts defers while an LWLock is held,
         * so the whole seal would become uncancellable, and the allocator would be
         * nested under an unrelated page's lock. This point is genuinely lock-free:
         * the previous iteration released its buffer and this one has not taken
         * one.
         *
         * A DOCUMENT IN PROGRESS IS NOT SPLIT. v7 lets a document span pages as
         * same-TID continuation records, and `doc` buffers its parts in its own
         * memory context -- nothing reaches the accumulator until drain_doc_flush
         * runs at the NEXT document header. So a doc still being assembled here has
         * contributed nothing to the accumulator being sealed, and lands whole in
         * the next chunk. Its buffered parts survive the accumulator swap because
         * DrainDoc owns its own context and copies every term's bytes out of the
         * page (see its header comment) -- it does not point into either the page
         * or the accumulator.
         *
         * NO DOC IS DUPLICATED: drain_doc_flush feeds a document to exactly one
         * accumulator, exactly once, and a sealed accumulator is freed and never
         * fed again. NO DOC IS LOST: the tail flush after the loop is
         * unconditional. NOTHING OBSERVES A PARTIAL STATE: the chunks are orphan
         * pages, and the caller's ONE publish record adds every entry AND detaches
         * the pending chain together -- publishing chunk k while the chain is still
         * anchored would double-score every doc in it, durably, because a scan
         * captures pending_head and the catalog under one metapage lock and sums
         * per-TID with no cross-source dedup. Crash before that record: orphan
         * segment pages swept by VACUUM, pending list untouched, nothing lost.
         *
         * AN ERROR AFTER CHUNK 1 IS NOT THE SAME AS A CRASH, and the difference is
         * worth stating because the crash case is the reassuring one. Chunks 1..k-1
         * are committed orphan pages by then, and while bm25_reclaim_orphans will
         * sweep them, it only runs from a bm25_vacuumcleanup whose SEAL SUCCEEDED
         * FIRST (and, since #300, whose reclaim_retired and merge passes did too).
         * For a deterministic thrower -- the mixed-key-chain refusal in the
         * key-sizing pass below (a chain admitted before the build-time key stamp
         * existed, issue #292), or the extent bound on a corrupt chain -- that
         * never happens, so each attempt strands another set. Both wedge the seal;
         * what chunking adds is that the wedged index also grows. The cure is
         * REINDEX (reverting key_field does not unmix a chain that is already
         * mixed), and reordering vacuumcleanup to sweep
         * before it seals would fix the growth but is a change to a documented
         * Phase-4 ordering and belongs to its own decision.
         *
         * THE BUDGET IS NOT THE WHOLE PEAK, either. DrainDoc buffers a document's
         * parts in its OWN context -- which is what makes the accumulator swap above
         * safe -- so those bytes are invisible to bm25_accum_over_budget. Bounded,
         * not unbounded: a document is capped at 65535 tokens on both ingest paths
         * (ADR 0079), so the true ceiling here is budget + accumulator baseline +
         * one document's expanded tokens, the same shape the build path has.
         */
        if (bm25_accum_over_budget(a, budget))
        {
            bm25_drain_flush_chunk(index, heaprel, a, &ents, &nent, &capent);
            bm25_accum_free(a);
            a = bm25_drain_accum_new(field_count, store_pos, km_type, km_size);
        }

        /* Copy-then-unlock (#67, ADR 0083 addendum), the shape every other pending
         * reader already has. The loop below runs drain_doc_flush -- accumulator
         * inserts, i.e. dynahash growth and palloc unbounded by this page's size --
         * and drain_doc_add_part's palloc/repalloc of a document's parts, and both
         * used to run with this page's content lock held. No production path can
         * WRITE the page meanwhile (the seal singleton excludes every appender and
         * VACUUM's pending sweep; the owner-only bm25_debug_pending_invalidate_page
         * takes no singleton, and the SHARE lock at copy time serialises it exactly as
         * it used to), so this was never a contention bug; what the lock did cost was
         * interrupt holdoff across the whole page's accumulator work, and an
         * exception to a rule the rest of the tree now keeps without one. Decoding a
         * copy changes nothing else: DrainDoc already copies every byte it keeps out
         * of the page, so no pointer into `copy` outlives this iteration, and the
         * next link is read off the copy, as bm25_pending_iter_begin's precondition
         * describes. */
        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        memcpy(copy.data, BufferGetPage(buf), BLCKSZ);
        UnlockReleaseBuffer(buf);

        pg = (Page) copy.data;
        bm25_pending_iter_begin(&it, pg);
        while (bm25_pending_iter_next(&it))
        {
            BM25PendingDocHeader *dh = it.cur;

            /* Skip slots a VACUUM sweep invalidated (bm25_pending_mark_dead). It
             * invalidates by TID equality, so every part of a multi-part document is
             * invalidated together and a surviving continuation cannot be orphaned by
             * it -- but the continuation branch below still tolerates that rather than
             * erroring, since a seal must not be the thing that fails. */
            if (!ItemPointerIsValid(&dh->tid))
                continue;

            /* #292: refuse a MIXED chain rather than seal it. Every record carries the
             * key_field configuration its INSERT resolved, so a chain is homogeneous
             * unless key_field changed between two of its rows. The insert gate keeps
             * that from happening on any index this binary built (the key stamp), but
             * not on an unstamped one with no segment yet, and not for chains a
             * pre-#292 binary already mixed. Sealing such a chain is never right:
             * a keyless record in a keyed chain is sealed with key 0, a keyed record
             * in a keyless-first chain likewise, and a same-width type change (uuid
             * vs text) seals one type's bytes under the other's tag -- all silent.
             * bm25_accum_set_doc_key's own guard sees only a width change, and only
             * for keyed records. Erroring wedges the seal for exactly those indexes,
             * which is the accepted trade: their pending rows are already
             * mis-keyed, and REINDEX (which rebuilds from the heap) is the cure.
             * Same errcode as that width guard: DDL-reachable, not storage damage.
             * Compared as the full (type, width) pair, keyless included. */
            if (!kc_seen)
            {
                kc_seen = true;
                kc_type = dh->key_type;
                kc_size = dh->key_size;
            }
            else if (dh->key_type != kc_type || dh->key_size != kc_size)
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bm25: pending list of index \"%s\" mixes key_field "
                                "configurations",
                                RelationGetRelationName(index)),
                         errdetail("A pending row carries key type %u width %u; an earlier "
                                   "one carries key type %u width %u.",
                                   dh->key_type, dh->key_size, kc_type, kc_size),
                         errhint("key_field was changed after rows were inserted. REINDEX "
                                 "the index; restoring key_field does not repair rows "
                                 "already queued.")));

            /* M5 key_field: size the accumulator's key store from the first keyed doc
             * header (all docs of a keyed index share the config). Must precede the
             * first add_doc_multi so its key slot is allocated. Idempotent-guarded on
             * key_type so a keyless index (NONE) never allocates. */
            if (dh->key_type != BM25_KEY_NONE &&
                bm25_accum_key_type(a) == BM25_KEY_NONE)
            {
                /* Validate the TAG, not just its width (PEND-09). Both values come
                 * off a pending page and land in the KEYMAP header of the segment
                 * this seal is about to publish; bm25_accum_set_keymeta checks
                 * key_size alone, so an unrecognized key_type was written through to
                 * disk and only rejected later, by the reader, as a corrupt segment
                 * nobody wrote deliberately. The merge half of this same call is
                 * already safe -- it sources the pair from bm25_seg_keymeta, which
                 * validates -- so this closes the one remaining unvalidated producer.
                 *
                 * ADR 0067's insert-time gate does NOT cover it: that runs against the
                 * index's CONFIGURED key type, which is always a valid tag by
                 * construction, and never against the byte on the page. */
                bm25_seg_keymeta_validate(dh->key_type, dh->key_size);
                bm25_accum_set_keymeta(a, dh->key_type, dh->key_size);
                /* Remembered so every LATER chunk's accumulator is created with it
                 * (BUILD-04). Without this, a chunk boundary before the next keyed
                 * header would leave that chunk keyless and silently revert its
                 * rows to ctid identity. The validation above is not repeated: the
                 * pair has already been checked, and re-deriving it per chunk would
                 * make a keyed drain's behaviour depend on where the cuts fell. */
                km_type = dh->key_type;
                km_size = dh->key_size;
            }

            if ((dh->flags & BM25_PENDING_DOC_CONT) == 0)
            {
                /* A new document starts here, so whatever was buffered is complete. */
                drain_doc_flush(&doc, a, field_count);
                drain_doc_begin(&doc, dh);
            }
            else if (!doc.active ||
                     !ItemPointerEquals(&doc.tid, &dh->tid))
            {
                /* A continuation with nothing to continue. Drop the fragment; a seal
                 * must not be the thing that fails.
                 *
                 * This used to carry Assert(false) on the belief that only corruption
                 * could produce the state. It is reachable from an ORDINARY cancelled
                 * VACUUM, and the assertion PANICked an --enable-cassert build (which
                 * the hardening CI job builds) on a state this branch exists to
                 * tolerate. Measured: 600 multi-part documents, DELETE, VACUUM under
                 * statement_timeout = 15ms, then bm25_seal ->
                 *   TRAP: failed Assert("false") ... in bm25_pending_drain
                 *   client backend was terminated by signal 6: Abort trap: 6
                 *
                 * WHY IT IS REACHABLE. Two CONSECUTIVE parts of one document never
                 * share a page. Not because a part starts a fresh one -- part 0 lands
                 * in the tail page's leftover room whenever it fits, beside whatever
                 * other documents' records are already there (bm25_format.h's
                 * BM25_PENDING_DOC_CONT note) -- but because the append loop above
                 * budgets EVERY part against the whole PENDING_PAGE_CAPACITY: the
                 * entry that ended part k is, by that break condition, larger than the
                 * room part k leaves behind, so part k+1 always allocates a page.
                 * bm25_pending_mark_dead then commits ONE GenericXLog record per page
                 * with its delay/interrupt point at the top of the next iteration. So a
                 * cancel landing between two pages leaves part 0 invalidated and part
                 * 1 untouched -- permanently, because PostgreSQL has no undo and an
                 * aborted transaction does not roll back physical page changes. The
                 * drain then skips part 0 on its invalid TID and arrives here holding
                 * either no active document or a different one. Autovacuum
                 * cancellation is routine (any conflicting lock request does it), so
                 * this is an expected steady-state occurrence, not a rarity.
                 *
                 * DROPPING IS CORRECT, not merely tolerable. Reaching this branch
                 * means part 0 was invalidated, which happens only when the bulkdelete
                 * callback judged that TID dead -- so the document must not be indexed
                 * at all, and discarding the surviving fragment is the right answer
                 * rather than a lossy compromise. The stranded pages are recycled with
                 * the rest of the drained chain.
                 *
                 * NO WARNING, deliberately: the trigger is a routine cancelled
                 * autovacuum and the outcome is correct, so logging here would emit
                 * one line per stranded fragment during ordinary maintenance. The
                 * genuinely corrupt version of this state is indistinguishable from
                 * the benign one at this point, and the loud checks that DO discriminate
                 * (page-kind validation, the extent bound, the cycle cap) already run
                 * on this walk. */
                continue;
            }

            drain_doc_add_part(&doc, dh, field_count, it.end_ptr);
        }
        bm25_pending_iter_end(&it);
        next = BM25PageGetOpaque(pg)->nextblk;
        blk = next;
    }
    /* Same corruption backstop as bm25_pending_mark_dead above, and higher
     * stakes here: bm25_seal_index's publish resets pending_head on the premise
     * that this drain always consumes the WHOLE chain (see the NOTE (D-SEAL /
     * C2) below on bm25_pending_truncate, which explains why THAT function must
     * not touch the pending anchor itself -- this drain is the one place that
     * does). A silent early stop here would publish a partial accumulator,
     * advance the anchor past pending docs that were never read, and
     * bm25_pending_truncate would then detach and free the rest -- committed,
     * heap-visible rows becoming unfindable until REINDEX, exactly what that
     * restructuring was meant to prevent. */
    if (blk != InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending-list chain exceeds the relation's extent at block %u",
                        blk)));
    drain_doc_flush(&doc, a, field_count);      /* last document on the chain */
    drain_doc_fini(&doc);
    bm25_drain_flush_chunk(index, heaprel, a, &ents, &nent, &capent);
    bm25_accum_free(a);

    *out_entries = ents;
    return nent;
}

/* PEND-08: the cycle cap for bm25_pending_truncate's walk, extracted so the debug
 * probe can prove it fires (a real in-extent cycle needs a corrupt nextblk, which
 * no SQL lever can produce).
 *
 * A visited-COUNT cap, NOT a reachable[] bitmap, for two independent reasons:
 *
 * 1. The page-kind check does not double as a cycle guard the way it might look
 *    like it does. bm25_page_mark_deleted ORs BM25_PAGE_DELETED and leaves
 *    BM25_PAGE_PENDING set, so this loop's own stamp does not change the kind and
 *    a page revisited on the second lap passes bm25_pending_page_flags_validate
 *    exactly as it did on the first. Without a cap, an in-extent cycle re-stamps
 *    and re-records the same pages forever -- cancellable via CHECK_FOR_INTERRUPTS,
 *    but non-terminating.
 *
 * 2. This runs on the aminsert path, once per seal. mark_chain can afford a
 *    bit-per-block bitmap over the whole extent because it runs under VACUUM; here
 *    that is ~1.6 MB allocated per seal on a 100 GB index to guard a walk that is
 *    typically a few hundred pages. nblocks is a strict upper bound on the number of DISTINCT
 *    blocks the walk can legally visit (the loop's other bound is blk < nblocks),
 *    so exceeding it proves a repeat without recording which one.
 *
 * Reports the block the walk was standing on when the cap tripped. */
static void
bm25_pending_cycle_cap_validate(uint64 visited, BlockNumber nblocks, BlockNumber blk)
{
    if (visited >= (uint64) nblocks)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: pending chain revisits blocks; stopped at "
                        "block %u after " UINT64_FORMAT " pages (relation has %u)",
                        blk, visited, nblocks)));
}

/* Probe for the cap above -- see bm25_debug_pending_page_flags_validate for why
 * these exist at all. TEST-ONLY: pure function of its scalar arguments, no
 * relation touched. Returns visited on success. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_cycle_cap_validate);
Datum
bm25_debug_pending_cycle_cap_validate(PG_FUNCTION_ARGS)
{
    int64   visited = PG_GETARG_INT64(0);
    int64   nblocks = PG_GETARG_INT64(1);

    if (visited < 0 || visited > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_cycle_cap_validate: visited out of uint32 range")));
    if (nblocks < 0 || nblocks > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_cycle_cap_validate: nblocks out of uint32 range")));

    /* blk 0 in the message: the probe has no real page, only the check is under
     * test. The suite normalises the block number away regardless. */
    bm25_pending_cycle_cap_validate((uint64) visited, (BlockNumber) nblocks, 0);
    PG_RETURN_INT64(visited);
}

/* NOTE (D-SEAL / C2): this is NO LONGER part of the seal linearization. The
 * seal now advances pending_head INSIDE the segment-publish Generic WAL record
 * (Task 5 Phase-2), so publish+truncate are one atomic record and a crash can
 * never strand the drained docs in both the segment and the pending list. This
 * standalone truncate is retained only as a POST-COMMIT page-recycler: it walks
 * the already-detached drained chain starting at drained_head (the pending head the
 * seal drained -- the walk's START, not an end bound) and returns its pages to the
 * FSM. It MUST NOT touch the pending anchor (pending_head/tail/ndocs) -- that
 * was reset in the publish record. It is intentionally unused for correctness
 * (a crash before/after it just leaves orphans for VACUUM); it only avoids
 * letting the drained pages sit as orphans until the next VACUUM. Phase 1 always
 * drains all, so the chain from drained_head is the whole drained list. */
void
bm25_pending_truncate(Relation index, BlockNumber drained_head)
{
    BlockNumber       blk = drained_head;
    BlockNumber       nblocks = RelationGetNumberOfBlocks(index);
    FullTransactionId retire;
    uint64            visited = 0;
    BlockNumber       freed_lo = InvalidBlockNumber;   /* [freed_lo, freed_hi]: */
    BlockNumber       freed_hi = 0;                    /* the blocks recorded free */

    /* Recycle the detached drained pages to the FSM. These pages are no longer
     * reachable from the (already-advanced) pending anchor, so freeing them takes
     * no metapage lock and is not a linearization point.
     *
     * Stamp-and-gate: each drained page still carries BM25_PAGE_PENDING. Stamp it
     * BM25_PAGE_DELETED + a retire horizon BEFORE RecordFreeIndexPage, so the gated
     * bm25_page_alloc accepts it as proven-free once that horizon clears; an
     * unstamped page reads as live and is rejected, which would grow the relation
     * unboundedly. We hold the page EXCLUSIVE for the stamp and release it BEFORE
     * RecordFreeIndexPage (FSM fork only -- never nest the FREED PAGE'S OWN lock
     * with the FSM update that publishes it).
     *
     * Scope of that rule, stated exactly (PEND-19, issue #145 -- an earlier wording,
     * "never nest the page lock with the FSM update", read as a blanket ban and was
     * cited as one). It governs the FREE side: the page being handed to the FSM must
     * not still be locked when it is handed over, so a concurrent bm25_page_alloc
     * that pops it can take its content lock and inspect the stamp. It says nothing
     * about the ALLOCATE side, where bm25_page_alloc's FSM traffic runs under the
     * metapage lock by design at three call sites (bm25_pending_append_multi, and
     * two in bm25_seg_build.c) -- see the reasoning at the append's call site.
     *
     * WHY A HORIZON AND NOT InvalidFullTransactionId (issue #135). This stamp used
     * to read InvalidFullTransactionId -- "reusable now: a detached pending page has
     * no live references". That premise was FALSE. Scanners hold only
     * AccessShareLock and take no part in the seal singleton: bm25_scan_snapshot
     * captures pending_head under ONE metapage SHARE lock and releases it, then
     * walks the chain LATER, page by page, releasing each buffer before reading the
     * next. So a scan that started before the seal is still holding block numbers
     * out of this very chain while we free them. With an Invalid stamp
     * bm25_page_alloc short-circuits its horizon test and hands the page to the next
     * writer, which FPI-re-inits it as a DICT/POST page -- and nothing downstream
     * notices, because bm25_page_init leaves seg_gen = 0 on pending pages, which
     * makes bm25_seg_page_validate a documented no-op there. Segment pages survive
     * the same race on three independent layers (per-page seg_gen, the retired RANGE
     * list, this horizon gate); the pending chain had none of the first two, so the
     * horizon was its only protection -- and the horizon is the PRIMARY's. A standby
     * query does not hold it back unless hot_standby_feedback is on, and no recovery
     * conflict fires for Generic WAL, so a standby scan could still be walking this
     * chain when its pages were reused (#291). Since #291 every pending page carries
     * its chain's epoch in seg_gen (bm25_pending_append_multi), and the scan's walk
     * (bm25_pending_walk_read) rejects a page re-inited after its snapshot with a
     * 40001. This stamp stays: on the primary it is what keeps that error from ever
     * firing.
     *
     * ONE value for the whole chain: these pages were all detached at a single
     * instant (the publish record), so one horizon describes them exactly, and a
     * per-page ReadNextFullTransactionId would only take XidGenLock n times for a
     * strictly later -- i.e. strictly more conservative, never more correct -- xid.
     * Read it here, before the loop, because ReadNextFullTransactionId acquires
     * XidGenLock and must never be called under a buffer lock. */
    retire = ReadNextFullTransactionId();

    while (blk != InvalidBlockNumber && blk < nblocks)
    {
        Buffer      buf;
        Page        pg;
        BlockNumber next;

        /* PEND-11: throttling form. Shared with the insert path exactly as the
         * drain is, and inert there for the same reason. */
        BM25_VACUUM_DELAY_POINT();

        /* Cycle cap BEFORE any page is touched this iteration, so a looping chain
         * cannot free one more page than the extent could ever hold. */
        bm25_pending_cycle_cap_validate(visited, nblocks, blk);
        visited++;

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        pg = BufferGetPage(buf);
        next = PageIsNew(pg) ? InvalidBlockNumber : BM25PageGetOpaque(pg)->nextblk;
        if (!PageIsNew(pg))
        {
            uint16 flags = BM25PageGetOpaque(pg)->flags;

            /* UNKNOWN-KIND guard, matching bm25_reclaim_orphans' (bm25_fsm.c):
             * LEAK rather than free a page carrying a flag bit outside
             * BM25_PAGE_ALL_KNOWN. ADR 0009 classifies a new page kind as
             * ADDITIVE, so this (older) binary can be handed an index whose newer
             * chains it cannot interpret; freeing such a page stamps it
             * InvalidFullTransactionId (immediate reuse) and hands a live
             * structure to the next bm25_page_alloc. Checked BEFORE the
             * pending-kind test below so a legitimately newer kind is leaked, not
             * misreported as corruption. The walk continues -- exactly the sweep's
             * `continue` -- and stays bounded by the extent check and the cycle
             * cap above. */
            if (flags & ~((uint16) BM25_PAGE_ALL_KNOWN))
            {
                UnlockReleaseBuffer(buf);
                blk = next;
                continue;
            }

            /* PEND-08: this is the ONLY chain walker in the extension that
             * DESTROYS pages, and it had nothing but the `blk < nblocks` extent
             * bound. A stray nextblk landing on a live DICT/POST/SEGCAT page
             * marked it BM25_PAGE_DELETED with InvalidFullTransactionId --
             * immediately reusable -- and recorded it free: committed, heap-visible
             * rows becoming unfindable until REINDEX. bm25_reclaim_retired errors
             * out in exactly this situation (bm25_fsm.c); so does this now. Release
             * the buffer before the ereport -- error cleanup would drop the lock
             * anyway, but every other explicit-error path in this file does it
             * here, and it keeps the abort path free of a held content lock. */
            if (!(flags & BM25_PAGE_PENDING))
            {
                UnlockReleaseBuffer(buf);
                /* Always ERRORs -- the bit was just proved clear. Reusing the
                 * shared validator keeps errcode and message identical to
                 * bm25_pending_iter_begin's. */
                bm25_pending_page_flags_validate(flags, blk);
                pg_unreachable();
            }

            bm25_page_mark_deleted(index, buf, retire);
        }
        UnlockReleaseBuffer(buf);
        RecordFreeIndexPage(index, blk);
        if (freed_lo == InvalidBlockNumber || blk < freed_lo)
            freed_lo = blk;
        if (blk > freed_hi)
            freed_hi = blk;
        blk = next;
    }

    /* RecordFreeIndexPage updates only each block's FSM leaf; the upper levels,
     * which GetFreeIndexPage searches from the root, learn of the space only from
     * an FSM vacuum. Vacuum just the range this walk freed (issue #300, PEND-07),
     * not the whole map: this runs under the seal singleton, including from
     * aminsert's opportunistic seal, where every document-adding insert waits for
     * the hold to end. IndexFreeSpaceMapVacuum visited every FSM page of the
     * relation; the range form visits only the FSM pages covering
     * [freed_lo, freed_hi], plus their ancestors. What it no longer does as a side
     * effect is propagate leaves OUTSIDE those FSM pages that another path wrote
     * without a vacuum (bm25_page_alloc's requeue); those wait for the next VACUUM,
     * whose cleanup vacuums the whole map on every run (bm25_vacuumcleanup) --
     * unconditionally, not as a side effect of the orphan sweep, which is gated
     * (issue #300) and usually does not run. fsm_search only corrects a stale-HIGH
     * parent downward, so a leaf nobody vacuums stays invisible to
     * GetFreeIndexPage: if that whole-map vacuum ever becomes conditional, these
     * requeued pages leak and the relation grows. Below about 4,000 blocks every
     * block shares one FSM leaf page, so there the two calls do the same work. The
     * singleton is NOT released before this walk to make room for the vacuum: a
     * VACUUM orphan sweep running in that gap could free a detached page ahead of
     * the cursor, an appender re-init it as a live pending page, and this walk
     * then free it. */
    if (freed_lo != InvalidBlockNumber)
        FreeSpaceMapVacuumRange(index, freed_lo, freed_hi + 1);
}

/* bm25_pending_reset_anchor -- detach the whole pending chain from the metapage
 * in one metapage-only Generic WAL record, writing exactly the five anchor fields
 * bm25_segcat_publish_append's publish record writes.
 *
 * WHY THIS EXISTS (issue #131, C1). The anchor reset used to live ONLY inside the
 * publish record, and the publish record runs only when the drain produced at
 * least one live document. The page recycle that follows it (bm25_pending_truncate)
 * is gated on something else entirely -- whether a chain was drained at all. Those
 * two conditions come apart exactly when VACUUM's bm25_pending_mark_dead has
 * invalidated every doc's TID: bm25_pending_drain skips every invalidated slot, so
 * it yields no catalog entries at all (it returned an accumulator with ndocs == 0
 * before the drain became chunked), no segment is published, the anchor is never
 * touched -- and the truncate still frees the entire chain to the FSM.
 * The metapage is then left pointing pending_head/pending_tail at pages the FSM
 * has handed back, with pending_tail_free still nonzero, so the next insert
 * appends pending-record bytes over whatever the allocator has since made of them.
 * The state LATCHES: nothing ever repairs the anchor, so every later insert keeps
 * writing to reclaimed pages.
 *
 * The caller MUST hold the seal singleton (LockPage(BM25_METAPAGE_BLKNO,
 * ExclusiveLock)). That is what makes a FULL reset correct rather than a partial
 * one: bm25_pending_append_multi takes the same lock in ShareLock mode, so no
 * append can have landed between the drain snapshot and this record. This is the
 * identical argument the publish record relies on -- see the long note there.
 *
 * ORDERING against the recycle is the whole point: this record must COMMIT before
 * bm25_pending_truncate frees the pages. A crash in between leaves the chain
 * detached but unrecycled, i.e. ordinary orphans for VACUUM to reclaim -- the same
 * benign outcome the publish path already has. The reverse order is the bug. */
void
bm25_pending_reset_anchor(Relation index)
{
    Buffer              metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    GenericXLogState   *state;
    Page                metapage;
    BM25MetaPageData   *m;

    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    state = GenericXLogStart(index);
    metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
    m = BM25PageGetMeta(metapage);

    m->pending_head = InvalidBlockNumber;
    m->pending_tail = InvalidBlockNumber;
    m->pending_tail_free = 0;
    m->pending_npages = 0;
    m->pending_ndocs = 0;

    /* pd_lower discipline so page-hole compression keeps the struct. */
    bm25_meta_set_pd_lower(metapage);
    GenericXLogFinish(state);
    UnlockReleaseBuffer(metabuf);
}

/* bm25_validate_key_config_for_insert -- refuse a row whose resolved key_field
 * configuration disagrees with the key identity this index was built with.
 *
 * BUILD-05; the full rationale, including why this is neither a DDL-time nor a
 * seal-time check, is at the call site in bm25_insert (src/bm25_build.c).
 *
 * #292: THE STAMP IS THE AUTHORITY. Every build by this binary leaves a BM25KeyStamp
 * (key_type, key_size, key_attno) on the field-config page, and every row is compared
 * with all three. That closes two gaps the segment-0 comparison below cannot:
 *
 *   - THE EMPTY CATALOG. Segment 0 does not exist until the first seal, and an index
 *     created on an empty table and then loaded -- the common order -- spends its
 *     early life there. Every key_field change was admitted into the pending chain:
 *     a width change then failed every seal and VACUUM (and every INSERT once the
 *     chain crossed seal_threshold) with no recovery short of REINDEX, and every
 *     other change sealed silently mis-keyed rows (keys of 0, or of another column).
 *   - THE KEY COLUMN. A KEYMAP header records type and width, not which column the
 *     keys came from, so re-pointing key_field at another column of the same type
 *     passed even with segments present (ADR 0067's accepted residual).
 *
 * The stamp is immutable for the life of the relfilenode, so no lock is needed to
 * read it consistently -- unlike a "first pending record" comparison, which would
 * race a concurrent ALTER (it takes only ShareUpdateExclusiveLock in a backend that
 * has not registered the reloptions; see bm25_insert) and still miss the column.
 *
 * FALLBACK: an index built before #292 has no stamp, and keeps exactly the pre-#292
 * check below -- type and width against segment 0, nothing while there is none.
 * REINDEX stamps it. What follows describes that fallback.
 *
 * BOTH DIRECTIONS ARE CHECKED, and that is load-bearing. An earlier version
 * returned early on BM25_KEY_NONE and `continue`d past keyless segments, which left
 * two bypasses that each produce exactly the mixed-KEYMAP state this exists to
 * prevent:
 *
 *   - `ALTER INDEX ... RESET (key_field)` on a KEYED index. The next row resolves
 *     NONE, the early return let it through, and the seal published a KEYLESS
 *     segment into a keyed catalog.
 *   - `ALTER INDEX ... SET (key_field = ...)` on a KEYLESS index. Skipping keyless
 *     segments left nothing to compare against, so the check passed vacuously and
 *     the seal published a KEYED segment into a keyless catalog. The next merge --
 *     which bm25_merge_maybe fires on its own at autovacuum cadence -- then takes
 *     its key config from chosen[0] (bm25_merge.c), i.e. from the keyless segment,
 *     and the committed row's key is silently dropped while key_field is still set.
 *
 * The early return was justified in a comment claiming a keyed index legitimately
 * resolves NONE for a row whose key column is NULL. That was simply false:
 * key_type comes from bm25_resolve_fields, i.e. from index-level reloption state,
 * and isnull[] gates only key EXTRACTION at the call site -- bm25_pending_append_multi
 * stamps hdr.key_type with the configured type even for a NULL-key row. A keyed
 * index never resolves NONE for any row; only a configuration change does. So the
 * hole protected no legitimate case.
 *
 * bm25_seg_keymeta sets (BM25_KEY_NONE, 0) before it returns false, so a keyless
 * segment yields a directly comparable pair and needs no special case: it matches a
 * keyless resolution and mismatches every keyed one.
 *
 * COVERAGE OF THE FALLBACK, stated honestly. With segments present it catches any
 * key_type or key_size change, including to and from keyless. It does NOT catch
 * re-pointing key_field at a DIFFERENT column of the SAME type and width (two int4
 * INCLUDE columns), and with no segment yet it catches nothing at all. Both gaps are
 * what the stamp above closes; for an unstamped index the drain's mixed-chain refusal
 * (bm25_pending_drain) turns the type/width half of the second gap into a loud seal
 * error rather than a silently mis-keyed segment, and REINDEX is the cure.
 *
 * COST. The stamped path: one SHARE read of the write-once field-config page. The
 * fallback: one catalog-root read plus one segment-header read, and only when the
 * index already has segments. It stops at the FIRST segment, which is sound by induction
 * now that both directions are checked: every row admitted since has been validated
 * against that same set, so the set cannot have become heterogeneous through this
 * path. (An index already mixed by a binary that predates this check is not
 * repaired by it -- REINDEX is the cure, as the hint says.) Measured worst case on
 * bulk INSERT of deliberately tiny documents: ~0.5 us/row, ~6-8%; on realistic
 * documents tokenization dominates and it disappears. The allocation lands in
 * bm25_insert's per-row scratch context. */
void
bm25_validate_key_config_for_insert(Relation index, BlockNumber field_config_blkno,
                                    uint8 key_type, uint16 key_size, int key_attno)
{
    BM25SegCatEntry     first;
    BM25KeyStamp        ks;

    /* Invalid root: no field-config page at all, so certainly no stamp. Every
     * format this binary reads has one, but the drain tolerates its absence too, and
     * the insert gate must not be the stricter of the two. */
    if (field_config_blkno != InvalidBlockNumber &&
        bm25_fieldcfg_read_keystamp(index, field_config_blkno, &ks))
    {
        if (ks.key_type != key_type || ks.key_size != key_size ||
            (int) ks.key_attno != key_attno)
        {
            TupleDesc   itd = RelationGetDescr(index);

            /* Names, not attnos, in the detail: the attno is the INDEX column
             * position, which is not what anyone wrote in key_field. Both attnos
             * are inside itd (the stamp reader bounds its own, bm25_resolve_fields
             * resolved the live one against this same descriptor). */
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bm25: key_field does not match the key column this index "
                            "was built with"),
                     errdetail("This row resolves key_field to %s%s%s (key type %u, width %u); "
                               "the index was built with %s%s%s (key type %u, width %u).",
                               key_attno >= 0 ? "column \"" : "no key column",
                               key_attno >= 0 ? NameStr(TupleDescAttr(itd, key_attno)->attname) : "",
                               key_attno >= 0 ? "\"" : "",
                               key_type, key_size,
                               ks.key_attno >= 0 ? "column \"" : "no key column",
                               ks.key_attno >= 0 ? NameStr(TupleDescAttr(itd, ks.key_attno)->attname) : "",
                               ks.key_attno >= 0 ? "\"" : "",
                               ks.key_type, ks.key_size),
                     errhint("key_field is structural, not a tunable knob. Restore the "
                             "previous key_field, or REINDEX after changing it.")));
        }
        return;
    }

    /* Issue #270: entry 0 under the metapage SHARE, not bm25_segcat_read. This runs
     * holding no singleton, and that walk releases the metapage before it reads the
     * catalog, so a merge plus a VACUUM in between could free and reuse the root and
     * turn this healthy INSERT into XX002. The header read below is safe after the
     * release; bm25_segcat_first_entry's header says why. */
    if (bm25_segcat_first_entry(index, &first))
    {
        BM25SegmentHeader   h;
        uint8               seg_type;
        uint16              seg_size;

        /* No buffer is held here. */
        bm25_debug_pause_point("insert_keycheck");
        bm25_seg_header_read(index, first.header_blkno, first.gen, &h);
        /* Return value deliberately ignored: false means "keyless", and the
         * out-params are already set to the (NONE, 0) pair that represents it. */
        (void) bm25_seg_keymeta(index, &h, &seg_type, &seg_size);

        if (seg_type != key_type || seg_size != key_size)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bm25: key_field does not match the configuration this "
                            "index's existing segments were written with"),
                     errdetail("This row resolves to key type %u width %u; the "
                               "existing segments use key type %u width %u.",
                               key_type, key_size, seg_type, seg_size),
                     errhint("key_field is structural, not a tunable knob. Restore the "
                             "previous key_field, or REINDEX after changing it.")));
    }
}

/* bm25_seal_pending_locked -- the drain + publish + recycle core. The caller has
 * already acquired the seal singleton (LockPage(BM25_METAPAGE_BLKNO,
 * ExclusiveLock), conditionally in the opportunistic case) and holds an open heap
 * Relation.
 *
 * Two DIRECT callers acquire the singleton themselves: bm25_seal_index and
 * bm25_insert's opportunistic seal. Everything else that seals -- amvacuumcleanup,
 * the merge, bm25_upgrade -- reaches this through bm25_seal_index, so this is the
 * single drain/publish/recycle implementation for the whole extension.
 *
 * PEND-16: bm25_seal_index and bm25_insert's opportunistic seal used to carry
 * byte-identical copies of this body, which is how issue #131's defect came to
 * exist in two places at once. One copy now, so the next fix lands once.
 *
 * There is no publish/reset CHOICE here any more, and that is the point: this
 * function calls bm25_segcat_publish_append unconditionally, and n == 0 is the case
 * where that record does only the anchor detach. The two branches used to live here
 * as an if/else, which is a shape where the recycle below can outlive a skipped
 * reset -- issue #131's C1 defect exactly. Making it one call makes the exhaustive
 * argument structural rather than a property of two conditions agreeing. Only after
 * that record may the pages be recycled. */
void
bm25_seal_pending_locked(Relation index, Relation heaprel)
{
    BlockNumber      drained_head;
    BM25SegCatEntry *entries;
    int              nent;

    bm25_meta_read_pending_head(index, &drained_head);  /* head being drained */
    /* Orphan bracket (issue #300): an ERROR or crash from here to the end of the
     * truncate leaves orphans -- built-but-unpublished segment pages, or a detached
     * chain only partly recycled -- that only VACUUM's orphan sweep frees, and that
     * sweep runs only on durable evidence. Opened BEFORE the drain's first
     * allocation (bm25_orphan_op_begin's header says why that ordering is what makes
     * the evidence crash-proof) and closed after the truncate. An empty chain builds,
     * publishes and frees nothing, so it opens no bracket and writes no WAL: VACUUM
     * on an idle index stays WAL-free. */
    if (drained_head != InvalidBlockNumber)
        bm25_orphan_op_begin(index);
    /* BUILD-04: the drain seals a chunk and starts a fresh accumulator whenever it
     * crosses the maintenance budget, so it hands back one entry per output rather
     * than one accumulator. */
    nent = bm25_pending_drain(index, heaprel, bm25_maintenance_budget_bytes(),
                              &entries);
    /* ONE record: publishes EVERY chunk AND advances pending_head. All of them
     * together, never one record each -- a committed state holding some chunks as
     * segments while the whole chain is still anchored double-scores and
     * double-returns every doc in them (see bm25_segcat_publish_append).
     *
     * nent == 0 means nothing survived to publish -- every doc on the drained pages
     * had been tombstoned by VACUUM's pending sweep -- and publish_append then does
     * only the anchor detach, so the recycle below is never freeing pages the
     * metapage still points at (issue #131's C1). That branch used to live here as
     * an explicit bm25_pending_reset_anchor call; it is the n == 0 case of the
     * publish now, which is the same record either way. */
    bm25_segcat_publish_append(index, heaprel, entries, nent, drained_head);
    pfree(entries);

    /* Post-commit, idempotent page recycle of the now-orphaned drained pages.
     * NOT part of the linearization point (pending_head is already advanced, by
     * one branch or the other above); a crash here just leaves orphans for VACUUM,
     * which the open bracket (below) tells VACUUM to look for. */
    if (drained_head != InvalidBlockNumber)
    {
        bm25_pending_truncate(index, drained_head);
        /* Bracket closed: every page the drain built is published and every drained
         * page is recycled, so this seal left no orphan. */
        bm25_orphan_op_end(index);
    }
}


/* bm25_seal_index -- the seal core, factored out of bm25_seal_sql so it can be
 * reused by amvacuumcleanup (Task 18) without duplicating a (proven, correctly-
 * signed, singleton-protected) drain+build+commit+truncate. Takes an OPEN index
 * Relation; opens the heap internally so the VACUUM path need not (and portably
 * cannot, across all PG versions) depend on IndexVacuumInfo.heaprel.
 *
 * No-ops cleanly when the CHAIN is empty: bm25_pending_drain publishes no entries,
 * and drained_head is InvalidBlockNumber, which is what makes
 * bm25_segcat_publish_append skip its record entirely and bm25_pending_truncate
 * not run. Repeated calls (manual seal then VACUUM) are therefore safe. "No live
 * docs recovered" is NOT the same state: a chain whose docs VACUUM has all
 * tombstoned also yields zero entries, but drained_head is valid, so the
 * anchor-detach record is still written and the pages are still recycled.
 *
 * Error handling uses PG_TRY/PG_FINALLY (re-throw), NOT PG_CATCH: PG_FINALLY runs
 * the singleton-unlock + heap-close cleanup and then RE-THROWS on error, so the
 * builder's documented precondition (must abort the transaction on error; no
 * enclosing PG_CATCH that swallows-and-resumes) still holds. */
void
bm25_seal_index(Relation index)
{
    Relation    heaprel;

    /* D-ALLOC/M6: open the heap ONCE, before any buffer lock, and thread it into
     * the allocator/build path. Never table_open while holding a buffer lock. */
    heaprel = table_open(IndexGetRelation(RelationGetRelid(index), false),
                         AccessShareLock);

    /* D-SEAL/M5: the GIN-style heavyweight LockPage(BM25_METAPAGE_BLKNO) singleton
     * is held across the ENTIRE drain+build+commit so two concurrent sealers cannot
     * both drain the same docs and publish duplicate segments, and no append can
     * land between the drain snapshot and the publish record (which is what makes
     * the full pending-head reset in the publish record safe -- D-SEAL/C2).
     *
     * The second half of that only became true when bm25_pending_append_multi
     * started taking the same lock in ShareLock mode. Before it did, the appender
     * held only the metapage BUFFER lock -- a different lock manager entirely, which
     * this ExclusiveLock does not conflict with -- so an append could and did land
     * mid-drain and be discarded by the reset. Do not weaken either side. */
    LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
    PG_TRY();
    {
        bm25_seal_pending_locked(index, heaprel);
    }
    PG_FINALLY();
    {
        UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
        table_close(heaprel, AccessShareLock);
    }
    PG_END_TRY();
}

/* bm25_seal(regclass) -- manual seal: open the index and run the seal core. */
PG_FUNCTION_INFO_V1(bm25_seal_sql);
Datum
bm25_seal_sql(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    /* Ownership + AM identity before any page is touched. index_open() checks
     * relkind only, so this used to seal ANY index in the database for any role
     * that could connect. */
    Relation    index = bm25_index_open_owned(relid, RowExclusiveLock);

    bm25_seal_index(index);
    index_close(index, RowExclusiveLock);
    PG_RETURN_VOID();
}

/* bm25_debug_seal_unpublished(index regclass) -> bigint -- TEST-ONLY.
 * Run a seal's BUILD phase and stop before its publish record: drain the pending
 * list into orphan segment pages exactly as bm25_seal_pending_locked does, then
 * discard the catalog entries instead of handing them to
 * bm25_segcat_publish_append. Returns the first built segment's header block (a
 * BM25_PAGE_SEGCAT page nothing links to), or NULL when the drain built none.
 *
 * This is the on-disk state a seal or merge leaves when the server dies between
 * the orphan build and the one record that publishes it (the two-phase install,
 * ADR 0004) -- the state bm25_reclaim_orphans exists to recover. The crash suites
 * could not produce it: a TAP harness cannot stop a backend inside the C build,
 * and the "fire the operation asynchronously and stop('immediate')" approach they
 * used never lands there either. bm25's maintenance calls assign no xid, so their
 * commits do not flush WAL, and the immediate stop discards everything past the
 * last flush -- the whole in-flight operation, not a prefix of it (issue #226).
 * With this, a suite builds the orphans, flushes WAL with pg_switch_wal() (a
 * bare txid_current() commit does not flush: it has written no WAL of its own),
 * and crashes; recovery then replays every orphan page and nothing links them.
 *
 * Takes the seal singleton for the same reason the real seal does (the drain
 * must not race an append), and changes nothing reachable: the pending anchor,
 * the catalog and the stats are untouched, so a later bm25_seal publishes the
 * same documents normally. Its metapage writes are the orphan bracket's begin
 * (issue #300) and bm25_next_gen's generation bump per built segment, which a real
 * seal that died at this point would also have made -- so, like that seal, it
 * leaves the evidence that sends the next VACUUM's gated sweep after its pages.
 * Ownership-gated like every write-side debug helper. */
PG_FUNCTION_INFO_V1(bm25_debug_seal_unpublished);
Datum
bm25_debug_seal_unpublished(PG_FUNCTION_ARGS)
{
    Oid                     relid = PG_GETARG_OID(0);
    Relation                index = bm25_index_open_owned(relid, RowExclusiveLock);
    Relation                heaprel;
    volatile BlockNumber    first = InvalidBlockNumber;

    /* Heap first, before any lock, as bm25_seal_index does (D-ALLOC/M6). */
    heaprel = table_open(IndexGetRelation(relid, false), AccessShareLock);
    LockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
    PG_TRY();
    {
        BM25SegCatEntry    *entries;
        int                 nent;

        /* Open the orphan bracket and never close it (issue #300): this is a seal
         * that "died" after its build, and the evidence is what sends the next
         * VACUUM's gated sweep after the pages it leaves. */
        bm25_orphan_op_begin(index);
        nent = bm25_pending_drain(index, heaprel, bm25_maintenance_budget_bytes(),
                                  &entries);
        if (nent > 0)
            first = entries[0].header_blkno;
        pfree(entries);
    }
    PG_FINALLY();
    {
        UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
        table_close(heaprel, AccessShareLock);
    }
    PG_END_TRY();

    index_close(index, RowExclusiveLock);
    if (first == InvalidBlockNumber)
        PG_RETURN_NULL();
    PG_RETURN_INT64((int64) first);
}

/* bm25_debug_pending_nth_page(index regclass, n int) -> bigint -- TEST-ONLY.
 * The n'th block of the pending chain in CHAIN order (n = 0 is pending_head), or
 * NULL when the chain is shorter than n + 1.
 *
 * Chain order is append order, which is what a test needs to target a specific
 * document's part: bm25_debug_pending_head alone can only reach the FIRST document
 * in the chain, so a suite using it can only ever strand a continuation with NO
 * preceding active document. Reaching the other shape -- an orphaned continuation
 * arriving while a DIFFERENT document is active -- means invalidating a page partway
 * down the chain, hence this. Deliberately not derived from block numbering: pending
 * pages happen to be allocated sequentially in a fresh index, so `head + 1` would
 * pass today and rot silently the first time the allocator hands back a recycled
 * page out of order.
 *
 * The extent is sampled once up front and read through bm25_blk_in_extent (#243),
 * so a page an appender links after the sample is followed rather than reported as
 * the end of the chain. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_nth_page);
Datum
bm25_debug_pending_nth_page(PG_FUNCTION_ARGS)
{
    Oid             relid = PG_GETARG_OID(0);
    int32           n = PG_GETARG_INT32(1);
    Relation        index = bm25_index_open_readable(relid, AccessShareLock);
    BlockNumber     nblocks = RelationGetNumberOfBlocks(index);
    BlockNumber     blk;
    int32           i;

    if (n < 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_pending_nth_page: n must not be negative")));

    bm25_meta_read_pending_head(index, &blk);
    for (i = 0; i < n && blk != InvalidBlockNumber &&
                bm25_blk_in_extent(index, blk, &nblocks); i++)
    {
        Buffer  buf = ReadBuffer(index, blk);
        Page    pg;

        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg = BufferGetPage(buf);
        blk = PageIsNew(pg) ? InvalidBlockNumber : BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
    }
    /* Tested before index_close: the n'th link itself was never checked by the loop,
     * and a re-sample needs the relation open. */
    if (blk != InvalidBlockNumber && !bm25_blk_in_extent(index, blk, &nblocks))
        blk = InvalidBlockNumber;
    index_close(index, AccessShareLock);

    if (blk == InvalidBlockNumber)
        PG_RETURN_NULL();
    PG_RETURN_INT64((int64) blk);
}

/* bm25_debug_pending_sweep(index regclass, nblocks bigint) RETURNS bigint
 *   -- issue #243 probe for VACUUM's pending sweep and its stale extent sample.
 *
 * bm25_pending_mark_dead walks under the seal singleton in ShareLock mode, which
 * appenders share, so an INSERT can extend the relation and link a new tail page
 * after the sweep sampled the extent. No suite can land that append inside a live
 * sweep deterministically, but the race only changes the value of the sample, so this
 * runs the REAL sweep (pending_mark_dead_sampled) with a caller-chosen sample instead.
 * nblocks = 1 (the metapage alone) puts every pending page past the sample, so the
 * very first link -- the head -- is a would-be violation, exactly as in the race.
 * Take the re-sample out of bm25_blk_in_extent and a healthy chain raises
 * "pending-list chain exceeds the relation's extent": the mutation this exists to
 * catch. nblocks = -1 takes a fresh sample, as the public entry point does: the
 * control.
 *
 * The callback never reports a TID dead, so the sweep writes nothing: no page is
 * dirtied and no WAL is emitted (bm25_pending_page_has_dead's pre-scan skips every
 * page). Returns the number of chain pages the sweep visited, so a walk that quietly
 * stopped short cannot pass. TEST-ONLY; REVOKEd from PUBLIC with every bm25_debug_*. */
static bool
pending_sweep_never_dead(ItemPointer itemptr, void *state)
{
    return false;
}

PG_FUNCTION_INFO_V1(bm25_debug_pending_sweep);
Datum
bm25_debug_pending_sweep(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    int64       nblocks = PG_GETARG_INT64(1);
    Relation    index;
    BlockNumber extent;
    uint64      visited = 0;

    /* Arguments before the index is opened (97_debug_probe_arguments). 0 is allowed:
     * it is as stale as a sample can be, and the re-sample must cope with it. */
    if (nblocks < -1 || nblocks > (int64) PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_pending_sweep: nblocks " INT64_FORMAT
                        " is not a block count", nblocks)));

    index = bm25_index_open_readable(relid, AccessShareLock);
    extent = RelationGetNumberOfBlocks(index);
    /* A sample can be stale but never ahead of the relation: an index only grows
     * while it is open, so a sweep's sample is at most the current extent. Accepting
     * a larger one would let a suite pass on a sample the race cannot produce --
     * one that switches the bound off instead of exercising the re-sample. */
    if (nblocks > (int64) extent)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_pending_sweep: nblocks is past the relation's extent"),
                 errdetail("The nblocks argument is " INT64_FORMAT "; the relation has %u blocks.",
                           nblocks, extent)));
    }
    (void) pending_mark_dead_sampled(index, pending_sweep_never_dead, NULL,
                                     nblocks < 0 ? extent : (BlockNumber) nblocks,
                                     &visited);
    index_close(index, AccessShareLock);
    PG_RETURN_INT64((int64) visited);
}

/* bm25_debug_pending_invalidate_page(index regclass, blkno int) -> int -- TEST-ONLY.
 * Invalidate the TID of every pending record on ONE page, exactly as
 * bm25_pending_mark_dead does, and return how many were changed.
 *
 * This exists to make a PARTIALLY SWEPT chain reproducible without racing a clock.
 * The state matters -- it is what strands a continuation record whose parent header
 * is gone, the case bm25_pending_drain tolerates -- but reaching it in the field
 * requires a VACUUM to be cancelled between two specific pages. Driving that from a
 * regression suite means a statement_timeout tuned against the sweep's runtime, and
 * a suite that only sometimes strands a fragment passes vacuously the rest of the
 * time (measured: 30ms stranded nothing, 15ms stranded and PANICked a cassert
 * build). Invalidating one chosen page reproduces the identical on-disk state
 * deterministically.
 *
 * Deliberately does NOT touch pending_ndocs: mark_dead's metapage decrement is a
 * separate record, and the drain's behaviour under test does not read that counter.
 * Callers should treat the counter as meaningless afterwards. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_invalidate_page);
Datum
bm25_debug_pending_invalidate_page(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               blkno = PG_GETARG_INT32(1);
    Relation            index = bm25_index_open_owned(relid, RowExclusiveLock);
    Buffer              buf;
    GenericXLogState   *state;
    Page                wpg;
    BM25PendingIter     it;
    int32               changed = 0;

    if (blkno <= 0 || (BlockNumber) blkno >= RelationGetNumberOfBlocks(index))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_pending_invalidate_page: block %d out of range",
                        blkno)));

    buf = ReadBuffer(index, (BlockNumber) blkno);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    state = GenericXLogStart(index);
    wpg = GenericXLogRegisterBuffer(state, buf, 0);
    /* Validates the page kind, so a mistargeted block errors instead of scribbling. */
    bm25_pending_iter_begin(&it, wpg);
    while (bm25_pending_iter_next(&it))
    {
        if (ItemPointerIsValid(&it.cur->tid))
        {
            ItemPointerSetInvalid(&it.cur->tid);
            changed++;
        }
    }
    bm25_pending_iter_end(&it);
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);

    index_close(index, RowExclusiveLock);
    PG_RETURN_INT32(changed);
}

/* bm25_debug_pending_append(regclass, text) -- drive bm25_pending_append from SQL
 * with a synthetic, unique-ish TID (Task 6 added the machinery; Task 7 wired the
 * real aminsert path, which reaches bm25_pending_append_multi directly from
 * bm25_insert). For regression testing only -- single-session use.
 *
 * The comment that used to stand here said this helper, unlike the aminsert path,
 * "does NOT take the seal LockPage singleton" -- the authors believed aminsert did.
 * It did not: nothing took it on the append side at all. Both go through
 * bm25_pending_append_multi, which now takes it in ShareLock mode, so both are
 * covered and neither can be drained out from under a concurrent seal.
 * heaprel is opened before any buffer lock and threaded into the allocator (D-ALLOC/M6). */
PG_FUNCTION_INFO_V1(bm25_debug_pending_append);
Datum
bm25_debug_pending_append(PG_FUNCTION_ARGS)
{
    Oid             relid = PG_GETARG_OID(0);
    text           *doc = PG_GETARG_TEXT_PP(1);
    Relation        index = bm25_index_open_owned(relid, RowExclusiveLock);
    Relation        heaprel = table_open(IndexGetRelation(relid, false), AccessShareLock);
    BM25Token      *toks;
    int             ntok = bm25_tokenize(VARDATA_ANY(doc), VARSIZE_ANY_EXHDR(doc), &toks);
    ItemPointerData tid;
    BM25MetaPageData meta;

    bm25_meta_read(index, &meta);
    /* synthetic but unique-ish TID from current pending count */
    ItemPointerSet(&tid, 1, (OffsetNumber) (meta.pending_ndocs + 1));
    bm25_pending_append(index, heaprel, &tid, toks, ntok);
    table_close(heaprel, AccessShareLock);
    index_close(index, RowExclusiveLock);
    PG_RETURN_VOID();
}

/* Tail-space probe (#136). bm25_pending_tail_space_check only fires when the
 * metapage's cached pending_tail_free disagrees with the tail page it describes, and a
 * regression suite has no legitimate way to produce that: every bm25_debug_* function
 * was enumerated. The two that poke the metapage directly (bm25_debug_stamp_version,
 * bm25_debug_write_optional_region) leave the pending fields alone, and the two that
 * reach it through production code (bm25_debug_pending_append's append,
 * bm25_debug_seal_unpublished's generation bump) keep it consistent, so nothing can
 * falsify the counter from SQL. This therefore drives the SAME
 * function over a caller-chosen (pd_lower, pd_upper, runneed, claimed_free) tuple
 * against a synthetic page header built on the stack -- a struct assignment, not a raw
 * bytea, so the suite stays host-endian/padding independent (same reasoning as
 * bm25_debug_page_content_bytes, whose guard this one runs first, and
 * bm25_debug_block_validate's header-by-fields approach).
 *
 * The range checks below carry their OWN messages, distinct from the guard's, so a test
 * can tell "the argument never reached the C guard" from "the guard fired" -- sql/79's
 * stated rationale. tailblk is diagnostic only (it appears in the errdetail and nowhere
 * else); the probe has no real page and passes InvalidBlockNumber.
 *
 * Returns the page's real free byte count. TEST-ONLY: a pure function of its scalar
 * arguments, no relation opened, covered automatically by the install script's
 * bm25_debug_% REVOKE loop like every other probe. */
PG_FUNCTION_INFO_V1(bm25_debug_pending_tail_space);
Datum
bm25_debug_pending_tail_space(PG_FUNCTION_ARGS)
{
    int32           pd_lower     = PG_GETARG_INT32(0);
    int32           pd_upper     = PG_GETARG_INT32(1);
    int64           runneed      = PG_GETARG_INT64(2);
    int64           claimed_free = PG_GETARG_INT64(3);
    PageHeaderData  hdr;

    if (pd_lower < 0 || pd_lower > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_tail_space: pd_lower out of uint16 range")));
    if (pd_upper < 0 || pd_upper > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_tail_space: pd_upper out of uint16 range")));
    /* runneed is a Size in the append and claimed_free a uint32 on the metapage. Bound
     * both here so a negative or oversized SQL argument cannot reach the guard as a
     * wrapped value and have the wrap mistaken for the guard's own verdict. */
    if (runneed < 0 || runneed > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_tail_space: runneed out of uint32 range")));
    if (claimed_free < 0 || claimed_free > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_pending_tail_space: claimed_free out of uint32 range")));

    memset(&hdr, 0, sizeof(hdr));
    hdr.pd_lower = (LocationIndex) pd_lower;
    hdr.pd_upper = (LocationIndex) pd_upper;
    hdr.pd_special = (LocationIndex) BM25_PAGE_SPECIAL_OFF;   /* #302.D: as on a real page */

    PG_RETURN_INT64((int64) bm25_pending_tail_space_check((Page) &hdr, (Size) runneed,
                                                          (uint32) claimed_free,
                                                          InvalidBlockNumber));
}
