/* bm25_keymap.c -- per-segment docid->key flat array (BM25_PAGE_KEYMAP, M5).
 *
 * Role in the system: fills the v4-reserved KEYMAP family so a scan can turn a
 * dense local docid into the user's key_field value (int4/int8/uuid/text<=16B),
 * returning/scoring by the user key instead of the ctid. The writer is one more
 * orphan chain built inside bm25_segment_build_orphans (like DOCMAP/NORMS): each
 * page is its own <=1-buffer Generic WAL record stamped with the segment gen, so
 * nothing is reachable until the caller's publish record. The reader validates
 * seg_gen on every followed page (option (d)) exactly like bm25_seg_docid_to_tid,
 * which it clones with a key_size stride.
 *
 * Layout: root page = [BM25KeymapHeader][key[k*key_size]...]; continuation pages
 * (nextblk) carry raw key[] bytes only. key[docid] lives at byte offset
 * docid*key_size in the flat stream that begins AFTER the header on the root page.
 * There is no key->docid reverse map (spec section 9): id-restricted re-query is SQL.
 *
 * Only the index Relation is touched by the reader/SRF; each page is released
 * before the next is read (no table_open under a buffer lock). Adding KEYMAP
 * needed no format-version bump: it fills the BM25_PAGE_KEYMAP flag that v4
 * reserved (bm25_format.h), and a keyless index simply omits the chain
 * (keymap_root Invalid). BM25_FORMAT_VERSION has advanced since, for unrelated
 * reasons -- read the current value from BM25_FORMAT_VERSION itself rather than
 * from a number quoted here; KEYMAP remains a fill-in-place, not a break.
 */
#include "postgres.h"

#include "bm25.h"
#include "access/generic_xlog.h"
#include "storage/bufmgr.h"
#include "utils/uuid.h"          /* pg_uuid_t / UUID_LEN */
#include "catalog/pg_type_d.h"   /* INT4OID / INT8OID / UUIDOID / TEXTOID / VARCHAROID */
#include "funcapi.h"             /* Materialize-mode SRF (bm25_debug_seg_keymap) */
#include "mb/pg_wchar.h"         /* pg_mbcliplen -- key_text display clip */
#include "utils/builtins.h"      /* cstring_to_text_with_len */
#include "utils/hsearch.h"       /* BM25SegKeyCache's per-segment reader table */
#include "utils/rel.h"
#include "utils/tuplestore.h"    /* tuplestore_begin_heap/putvalues -- no longer pulled
                                  * in transitively via funcapi.h as of PG19 */

/* Fixed-width key extraction: turn a heap Datum for the key column into key_size
 * bytes. text is copied and right-NUL-padded to key_size; a text value LONGER than
 * key_size (16 B) is TRUNCATED to the first key_size bytes -- the row is still
 * indexed and keyed, never dropped or errored (documented: a text key_field must be
 * <=16 B to have a distinct identity; two rows sharing a 16-byte prefix collide,
 * which is the same "unspecified identity" as any non-unique key). The cut is by
 * BYTES and can split a multibyte character; it must stay that way, since these
 * bytes are already on disk in KEYMAPs and every key probe re-extracts with this
 * function (ADR 0009). Only bm25_debug_seg_keymap's key_text column clips to a
 * character boundary for display (issue #313 META-09). int4/int8 are
 * stored native-endian (the SAME backend reads them back -- the keymap never travels
 * between architectures except via base-backup, which copies byte order faithfully).
 * uuid is its 16 raw bytes. */
void
bm25_key_extract(uint8 key_type, uint16 key_size, Datum value,
                 unsigned char *out /* >= key_size */)
{
    memset(out, 0, key_size);
    switch (key_type)
    {
        case BM25_KEY_INT4:
        {
            int32 v = DatumGetInt32(value);
            memcpy(out, &v, sizeof(v));
            break;
        }
        case BM25_KEY_INT8:
        {
            int64 v = DatumGetInt64(value);
            memcpy(out, &v, sizeof(v));
            break;
        }
        case BM25_KEY_UUID:
        {
            pg_uuid_t *u = DatumGetUUIDP(value);
            memcpy(out, u->data, UUID_LEN);
            break;
        }
        case BM25_KEY_TEXT:
        {
            text *t = DatumGetTextPP(value);
            int   len = VARSIZE_ANY_EXHDR(t);
            if (len > key_size)
                len = key_size;                 /* truncate to key_size (do NOT drop/error) */
            memcpy(out, VARDATA_ANY(t), len);   /* right-NUL-padded by the memset */
            break;
        }
        default:
            elog(ERROR, "bm25: unexpected key_type %u", key_type);
    }
}

/* Map a PG type OID to the fixed-width bm25 key encoding. See bm25.h. Shared by
 * bm25_build (key_field column type -> ERROR on unsupported) and bm25_score_key
 * (SQL argument type -> NULL on unsupported / cross-type). */
bool
bm25_key_type_from_oid(Oid typid, uint8 *out_key_type, uint16 *out_key_size)
{
    switch (typid)
    {
        case INT4OID:
            *out_key_type = BM25_KEY_INT4;
            *out_key_size = 4;
            return true;
        case INT8OID:
            *out_key_type = BM25_KEY_INT8;
            *out_key_size = 8;
            return true;
        case UUIDOID:
            *out_key_type = BM25_KEY_UUID;
            *out_key_size = 16;
            return true;
        case TEXTOID:
        case VARCHAROID:
            *out_key_type = BM25_KEY_TEXT;
            *out_key_size = BM25_KEY_MAX_SIZE;
            return true;
        default:
            return false;
    }
}

/* The fixed width a BM25_KEY_* type implies, keyed by the on-disk key_type byte
 * rather than a catalog OID. Mirrors bm25_key_type_from_oid's per-type widths
 * (4/8/16/BM25_KEY_MAX_SIZE) exactly -- kept as the SAME correspondence, entered
 * from the other side, rather than a second hardcoded table that could drift
 * from it. Used to validate an on-disk key_type/key_size PAIR (bm25_seg_keymeta):
 * unlike an OID, which always tells the truth, key_type is untrusted bytes off a
 * page, so an unrecognized value must be rejected rather than silently mapped. */
static bool
bm25_key_type_width(uint8 key_type, uint16 *out_width)
{
    switch (key_type)
    {
        case BM25_KEY_INT4:
            *out_width = sizeof(int32);
            return true;
        case BM25_KEY_INT8:
            *out_width = sizeof(int64);
            return true;
        case BM25_KEY_UUID:
            *out_width = UUID_LEN;
            return true;
        case BM25_KEY_TEXT:
            *out_width = BM25_KEY_MAX_SIZE;
            return true;
        default:
            return false;
    }
}

/* The decode boundary for an on-disk (key_type, key_size) PAIR, as read off a
 * KEYMAP header by bm25_seg_keymeta: key_type must be a recognized BM25_KEY_*
 * and key_size must equal EXACTLY the fixed width that type implies. Extracted
 * to its own function (rather than left inline in bm25_seg_keymeta) so the
 * debug probe below exercises the identical check a real KEYMAP header runs
 * through -- the same "one validator, multiple callers" shape as
 * bm25_dictentry_validate.
 *
 * No longer static (PEND-09): the pending drain writes an on-PAGE key_type straight
 * into a sealed segment's KEYMAP header via bm25_accum_set_keymeta, whose own check
 * covers key_SIZE only. That made the tag a write-side hole in an otherwise
 * read-side-validated field -- corruption produced by the seal rather than merely
 * tolerated by a reader. Exporting the existing check was the right fix over adding a
 * second one: two validators for one on-disk fact is how they drift apart. */
void
bm25_seg_keymeta_validate(uint8 key_type, uint16 key_size)
{
    uint16 expect_size;

    if (!bm25_key_type_width(key_type, &expect_size) || key_size != expect_size)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: keymap key_type %u / key_size %u is invalid",
                        key_type, key_size)));
}

/* The decode boundary for the KEYMAP walk's (seg_key_cur's) root-page header step: key_size must
 * land in [1, BM25_KEY_MAX_SIZE] (every caller's `out` buffer is a fixed
 * kbuf[BM25_KEY_MAX_SIZE] stack array), and pagebytes -- a uint32 derived from
 * pd_lower, only lower-bounded by PageIsVerifiedExtended's pd_lower >=
 * SizeOfPageHeaderData -- must be large enough to hold the header BEFORE it is
 * subtracted from, or the subtraction wraps to just under UINT32_MAX instead
 * of erroring. Both checks run once, on the KEYMAP root page, before key_size
 * or the header-adjusted pagebytes drive any further arithmetic. */
static void
bm25_seg_key_header_validate(uint16 key_size, uint32 pagebytes)
{
    if (key_size == 0 || key_size > BM25_KEY_MAX_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: keymap key_size %u is invalid (maximum is %d)",
                        key_size, BM25_KEY_MAX_SIZE)));
    if (pagebytes < sizeof(BM25KeymapHeader))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: keymap root page has only %u bytes, header needs %zu",
                        pagebytes, sizeof(BM25KeymapHeader))));
}

/* The decode boundary for the KEYMAP walk's per-page span check: this key's local
 * span, starting at local_off (== slot - seen, i.e. the key's byte offset
 * within the CURRENT page's key[] stream), must stay inside the pagebytes
 * available on that page (already header-adjusted on the root page). Catches a
 * corrupt trailing partial-key run that the page-fit check above does not: that
 * check bounds the page's TOTAL byte count, not that any one key's span stays
 * inside it. */
static void
bm25_seg_key_span_validate(Size local_off, uint16 key_size, uint32 pagebytes)
{
    if (local_off + key_size > pagebytes)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: keymap entry at offset %zu overruns its page "
                        "(key_size %u, %u bytes available)",
                        local_off, key_size, pagebytes)));
}

/* Write the dense docid->key array as an orphan KEYMAP chain. Mirrors the DOCMAP
 * chain in bm25_segment_build_orphans, but with a header on the root page and a
 * key_size stride.
 *
 * WAL WINDOW / BUFFER DISCIPLINE (PEND-10, issue #145). This header used to claim
 * "at most ONE buffer registered at a time (each page is flushed before the next is
 * locked)", and BOTH halves were false: the continuation branch called
 * bm25_page_alloc -- which can ereport, and which EXCLUSIVE-locks the page it
 * returns -- while the tail page's Generic WAL window was still open, so the next
 * page was locked before the previous was flushed AND a throw-capable call ran
 * inside an open window. That is precisely what bm25_fsm.c states as a hard rule
 * ("The swap's Generic WAL window must be throw-free: no
 * ReadBuffer/LockBuffer/palloc/ereport between GenericXLogStart and
 * GenericXLogFinish") and what bm25_pending_append_multi already obeys.
 *
 * What it actually does now, per continuation boundary:
 *   1. GenericXLogFinish the tail's own record -- no window open -- and UNLOCK the
 *      tail, keeping its pin;
 *   2. BM25_WORK_UNIT and CHECK_FOR_INTERRUPTS, with no buffer content lock held;
 *   3. bm25_page_alloc for the successor (may throw; nothing to corrupt if it does,
 *      beyond an orphan page the reclaim path already handles);
 *   4. re-lock the old tail and write ONE small record registering it (to set
 *      nextblk) and the new page (init + seg_gen stamp), then release the old tail;
 *   5. open the new page's own window and keep streaming.
 * So: at most TWO buffers registered (well inside the 4-buffer Generic-WAL cap), at
 * most two held at once, and every window contains nothing but memory writes. The
 * cost is one extra WAL record per continuation page -- the same "correctness over a
 * single combined record" trade bm25_pending.c's appender makes.
 *
 * Step 1's unlock is what makes step 2 a real cancellation point (XCUT-05, issue
 * #305). The tail used to stay EXCLUSIVE-locked from its record through the
 * allocation to the link record, and the next page's lock was taken before it was
 * released, so some content lock was held at every instant of the chain: interrupts
 * were held the whole way and the allocator's CHECK_FOR_INTERRUPTS never acted. That
 * is the shape ADR 0041 removed from chain_ensure, and this is the same remedy. The
 * pin keeps the old tail resident for the re-lock. Nothing else writes it meanwhile:
 * the chain is an orphan until the segment publishes, and a concurrent allocator that
 * pops it from a double-listed FSM entry rejects it as not DELETED.
 *
 * Returns InvalidBlockNumber when there is no key_field (key_type==NONE) or the
 * segment is empty -- the caller then leaves keymap_root Invalid (ctid fallback). */
BlockNumber
bm25_keymap_write(Relation index, Relation heaprel, uint32 gen,
                  uint8 key_type, uint16 key_size,
                  const unsigned char *keys, uint32 ndocs)
{
    BlockNumber        root = InvalidBlockNumber;
    Buffer             tailbuf;
    Page               tailpage;
    GenericXLogState  *state;
    Size               off;                 /* bytes of key[] written so far */
    Size               total = (Size) ndocs * key_size;

    if (key_type == BM25_KEY_NONE || ndocs == 0)
        return InvalidBlockNumber;          /* no key_field, or empty segment */

    Assert(keys != NULL);   /* checked: the key_type/ndocs gate above */

    /* Root page: write the header, then let the loop below stream key[] into the
     * space that remains. pd_lower is advanced past every byte so page-hole
     * compression keeps them in the FPI. */
    {
        BM25KeymapHeader hdr;
        Buffer  buf = bm25_page_alloc(index, heaprel);    /* EXCL-locked */
        Page    pg;

        state = GenericXLogStart(index);
        pg = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
        bm25_page_init(pg, BM25_PAGE_KEYMAP);
        BM25PageGetOpaque(pg)->seg_gen = gen;             /* option (d) stamp */

        memset(&hdr, 0, sizeof(hdr));                     /* WAL determinism (incl. pad0) */
        hdr.key_type = key_type;
        hdr.key_size = key_size;
        hdr.ndocs    = ndocs;
        memcpy((char *) PageGetContents(pg), &hdr, sizeof(hdr));
        ((PageHeader) pg)->pd_lower += sizeof(hdr);

        root = BufferGetBlockNumber(buf);
        tailbuf = buf;
        tailpage = pg;
    }

    /* Write the dense key[] stream in WHOLE-KEY spans, opening continuation pages as
     * needed. Each page holds an integral number of keys: a key is NEVER split across
     * a page boundary. This is required so the reader can fetch key[docid] with a
     * single memcpy from ONE page (bm25_seg_key). The trailing < key_size bytes of a
     * page are left as unused slack (pd_lower not advanced over them), so the reader's
     * per-page byte count (pd_lower - header) is always a whole-key multiple and its
     * `seen`/`slot` accounting stays key-aligned. See the header comment for this
     * loop's WAL window and buffer discipline. */
    off = 0;
    while (off < total)
    {
        Size avail = ((PageHeader) tailpage)->pd_upper -
                     ((PageHeader) tailpage)->pd_lower;
        Size usable = avail - (avail % key_size);   /* whole keys only */
        Size span;

        if (usable == 0)
        {
            /* Not even one whole key fits in the tail: open a continuation page.
             *
             * PEND-10: close the tail's window FIRST. bm25_page_alloc searches the
             * FSM, may extend the relation, and can ereport -- none of which may
             * happen between GenericXLogStart and GenericXLogFinish. The tail page
             * is complete except for its nextblk link, which the record below adds;
             * a crash between the two records leaves a full but unlinked tail plus
             * an orphan successor, i.e. exactly the orphan state an aborted seal
             * already leaves and the reclaim path already collects. A cancel taken
             * at the check below leaves the same state.
             *
             * XCUT-05: then UNLOCK the tail, keeping the pin, so the check runs with
             * no content lock held and can act (see the header comment). */
            Buffer  nb;
            BlockNumber nblk;
            GenericXLogState *st;
            Page    np;
            Page    op;

            GenericXLogFinish(state);
            /* tailpage now dangles -- it addressed the finished state's scratch
             * image. It is re-derived from the register below before any use. */
            LockBuffer(tailbuf, BUFFER_LOCK_UNLOCK);

            /* One unit of cancellable work per KEYMAP page, counted beside the check
             * for the reason chain_ensure gives: if the check goes dead again under a
             * held lock, the work still happens and the count still climbs, which is
             * what sql/148 notices. */
            bm25_debug_pause_point("keymap_rotation");
            BM25_WORK_UNIT();
            CHECK_FOR_INTERRUPTS();

            nb = bm25_page_alloc(index, heaprel);         /* EXCL-locked, no window */
            nblk = BufferGetBlockNumber(nb);

            /* One record: link old tail -> new AND init the new page. Two buffers,
             * within the 4-buffer cap; the window itself is pure memory writes. The
             * old tail is re-locked here, after the new page's lock: the order
             * chain_ensure takes them in, and safe because no other backend locks
             * either page of an unpublished chain while waiting on the other. */
            LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
            st = GenericXLogStart(index);
            op = GenericXLogRegisterBuffer(st, tailbuf, 0);
            np = GenericXLogRegisterBuffer(st, nb, GENERIC_XLOG_FULL_IMAGE);
            BM25PageGetOpaque(op)->nextblk = nblk;
            bm25_page_init(np, BM25_PAGE_KEYMAP);
            BM25PageGetOpaque(np)->seg_gen = gen;         /* option (d) stamp */
            GenericXLogFinish(st);
            UnlockReleaseBuffer(tailbuf);

            /* The new page becomes the tail; open its own window for the streaming
             * below. Delta-logged (not FULL_IMAGE): the record above already wrote
             * its full image, so this one only has to carry the key bytes. */
            state = GenericXLogStart(index);
            tailbuf = nb;
            tailpage = GenericXLogRegisterBuffer(state, nb, 0);

            avail = ((PageHeader) tailpage)->pd_upper -
                    ((PageHeader) tailpage)->pd_lower;
            usable = avail - (avail % key_size);
        }
        span = Min(usable, total - off);
        memcpy((char *) tailpage + ((PageHeader) tailpage)->pd_lower,
               keys + off, span);
        ((PageHeader) tailpage)->pd_lower += span;
        off += span;
    }

    GenericXLogFinish(state);
    UnlockReleaseBuffer(tailbuf);
    return root;
}

/* Key bytes bm25_keymap_write leaves on every KEYMAP page but the last (issue #303).
 * The writer fills a page with as many WHOLE keys as `pd_upper - pd_lower` holds and
 * opens the next page only when not one more fits, so a non-final page carries
 * floor(avail / key_size) keys: avail is the page's content capacity, less the
 * BM25KeymapHeader on the root. The same capacity expression as bm25_chain_full_span. */
Size
bm25_keymap_full_span(uint16 key_size, bool root)
{
    Size    cap = BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BM25PageOpaque));

    Assert(key_size > 0);       /* checked: bm25_seg_key_header_validate */
    if (root)
        cap -= sizeof(BM25KeymapHeader);
    return cap - cap % key_size;
}

/* Read a segment's KEYMAP {key_type, key_size} from the chain root header. The merge
 * uses it to learn the source segments' key config so the merged segment re-seals an
 * identical keymap. Returns false (out_type=NONE) when there is no key_field. */
bool
bm25_seg_keymeta(Relation index, BM25SegmentHeader *h,
                 uint8 *out_type, uint16 *out_size)
{
    Buffer      buf;
    BM25KeymapHeader hdr;
    BM25SegWalk w;
    Size        pagebytes;

    *out_type = BM25_KEY_NONE;
    *out_size = 0;
    if (h->keymap_root == InvalidBlockNumber)
        return false;

    /* One page, read through the walker for block 0, the content bound and gen/kind
     * (issue #303: a keymap_root of 0 read the metapage and raised a retryable 40001).
     * No extent sample: this runs once per INSERT row on a keyed index
     * (bm25_validate_key_config_for_insert), and RelationGetNumberOfBlocks is an lseek
     * outside recovery, the cost ADR 0071 declines per pointer. InvalidBlockNumber as
     * nblocks leaves the extent unbounded for this one read -- the only block it would
     * reject is InvalidBlockNumber itself, excluded above -- so a root past the end
     * reaches ReadBuffer's own short-read error instead (a residual, see BM25SegWalk). */
    bm25_seg_walk_init(&w, index, InvalidBlockNumber, h->gen, BM25_PAGE_KEYMAP, false,
                       "segment KEYMAP chain");
    buf = bm25_seg_walk_read(&w, h->keymap_root, &pagebytes);
    memcpy(&hdr, PageGetContents(BufferGetPage(buf)), sizeof(hdr));
    UnlockReleaseBuffer(buf);

    /* This is the ONE place callers (merge, the debug SRF, the scan-start key
     * discovery in bm25_scan_rank.c/bm25_wand.c) learn a segment's key config, and it
     * runs unconditionally against the FIRST keyed segment header -- unlike
     * bm25_seg_key, it is not gated on any docid actually being ranked, so a
     * corrupt key_size here reaches so->ranked_key_size and a dynahash keysize
     * WITHOUT bm25_seg_key's own bound ever running. Reject anything but a known
     * key_type whose key_size equals the fixed width that type implies. */
    bm25_seg_keymeta_validate(hdr.key_type, hdr.key_size);

    *out_type = hdr.key_type;
    *out_size = hdr.key_size;
    return true;
}

/* Read key[local_docid] into out (>= key_size bytes), resuming from `cur`. A
 * near-clone of chain_read_at (bm25_seg_chain.c) with the root-page header skipped and a
 * key_size stride; it is a separate loop rather than a chain_read_at call because the
 * header makes the byte origin differ from every other dense chain's, and the header's
 * own validation has to run where it is decoded. Returns false when keymap_root is
 * Invalid (no key_field: caller uses ctid).
 *
 * Issue #225. This walk used to restart from keymap_root on EVERY call, with no extent
 * bound, and its comment declined the bound on cost: one RelationGetNumberOfBlocks per
 * call, on a path that runs once per ranked row and once per document in the merge. ADR
 * 0095 had already answered that objection for the other dense chains -- capture the
 * extent once, when a reader opens -- and the KEYMAP chain was the one left over. With
 * a cursor the per-row cost is one lseek per (loop, segment) instead of per row, the
 * forward-walking callers resume instead of re-walking, and the bound is affordable, so
 * an out-of-extent link now raises ERRCODE_INDEX_CORRUPTED naming the chain instead of
 * reaching ReadBuffer's short-read error.
 *
 * Resuming needs key_size before any page is read (to turn local_docid into a byte
 * offset), and only the root page carries it, so it comes from *key_size_cache: set by
 * whichever call last decoded the root header, and consulted only when the cursor's
 * root matches this chain -- the same identity test chain_read_at applies (SEGREAD-14),
 * which keeps a cursor carried to another KEYMAP chain from resuming on this one's
 * offsets. A call that resumes on the root page itself re-reads the header, as a call
 * from the root always does.
 *
 * `cur` is never NULL: the one-shot bm25_seg_key passes a fresh stack cursor. */
static bool
seg_key_cur(Relation index, BM25SegmentHeader *h, BM25ChainCursor *cur,
            uint16 *key_size_cache, uint32 local_docid,
            unsigned char *out, uint16 *out_size)
{
    BlockNumber   root = h->keymap_root;
    BlockNumber   blk;
    BlockNumber   nblocks;
    BM25SegWalk   w;
    Size          seen;              /* key[] bytes on the pages BEFORE blk (no header) */
    Size          slot = 0;
    uint16        key_size = 0;
    bool          got = false;

    if (root == InvalidBlockNumber)
        return false;                /* no key_field: caller falls back to ctid */

    /* A real check, not the Assert this used to be: the scan callers pass a docid
     * decoded off a POST page (the running delta sum), which no upstream check bounds
     * -- the same provenance, and the same reasoning, as seg_docid_to_tid_cur's check in
     * bm25_seg_chain.c. Out of range would otherwise walk to the chain's end and report
     * the misleading "ended before local_docid" below. */
    if (local_docid >= h->ndocs)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: local docid %u in a keymap lookup exceeds the "
                        "segment's document count " UINT64_FORMAT,
                        local_docid, h->ndocs)));

    if (cur->blk != InvalidBlockNumber && cur->blk != BM25_METAPAGE_BLKNO &&
        cur->root == root && *key_size_cache != 0 &&
        cur->seen <= (Size) local_docid * *key_size_cache)
    {
        blk      = cur->blk;
        seen     = cur->seen;
        key_size = *key_size_cache;
        slot     = (Size) local_docid * key_size;
    }
    else
    {
        blk  = root;
        seen = 0;
    }

    /* From the cursor when it has one (the reader form), else once for this call (the
     * one-shot form) -- never per page. */
    nblocks = (cur->nblocks != 0) ? cur->nblocks : RelationGetNumberOfBlocks(index);
    bm25_seg_walk_init(&w, index, nblocks, h->gen, BM25_PAGE_KEYMAP, false,
                       "segment KEYMAP chain");
    w.root = root;              /* a resume starts past it */

    while (blk != InvalidBlockNumber)
    {
        Buffer  buf;
        Page    pg;
        uint32  pagebytes;
        Size    walkbytes;
        char   *content;
        BlockNumber next;
        bool    at_root = (blk == root);

        CHECK_FOR_INTERRUPTS();

        /* Block 0, the extent, the revisit test and the cap run before any ReadBuffer,
         * inside bm25_seg_walk_get, and are loud rather than a quiet stop: stopping
         * early lands on the `!got` ERROR below anyway, but naming the chain and the
         * block is the attribution ADR 0095 gives every other segment chain. The
         * content bound and gen + kind are checked on every page, read or served from
         * the cursor's page image (issue #267). Everything below reads only `pg`, so
         * the root page's header is decoded and validated from the image exactly as
         * from the buffer.
         *
         * The content bound matters to the header check below: without it a corrupt
         * pd_lower below SizeOfPageHeaderData would wrap pagebytes to just under
         * SIZE_MAX, which bm25_seg_key_header_validate's `pagebytes < sizeof(hdr)`
         * would read as "plenty of room". The validated value is always < BLCKSZ, so
         * the narrowing cast to uint32 (the two validators' parameter type) loses
         * nothing. */
        pg = bm25_seg_walk_get(&w, cur, blk, &buf, &walkbytes);
        content   = (char *) PageGetContents(pg);
        pagebytes = (uint32) walkbytes;

        if (at_root)                          /* root page carries the header */
        {
            BM25KeymapHeader hdr;
            memcpy(&hdr, content, sizeof(hdr));
            key_size = hdr.key_size;
            /* key_size is a raw on-page value and every caller passes a FIXED unsigned
             * char kbuf[BM25_KEY_MAX_SIZE] stack buffer for `out` (bm25_scan_rank.c,
             * bm25_wand.c, bm25_merge.c). A corrupt/oversized key_size turns the memcpy
             * below into a stack overflow, so it must be bounded at the point it is
             * decoded, before it drives any offset math -- including the cached copy a
             * later resume trusts. bm25_seg_key_header_validate's remaining job -- now
             * that pagebytes is already range-checked above -- is the header-fit check:
             * pagebytes must hold at least sizeof(hdr) before it is subtracted below. */
            bm25_seg_key_header_validate(key_size, pagebytes);
            *key_size_cache = key_size;
            content   += sizeof(hdr);
            pagebytes -= sizeof(hdr);
            slot = (Size) local_docid * key_size;   /* now that key_size is known */
        }

        if (slot < seen + pagebytes)
        {
            /* The page-fit check above (pd_lower - header) bounds pagebytes, but not
             * that THIS key's span, starting at (slot - seen), stays inside it -- a
             * corrupt trailing partial-key run on this page would otherwise carry the
             * memcpy past pd_lower into page slack / the next page's bytes. */
            bm25_seg_key_span_validate(slot - seen, key_size, pagebytes);
            memcpy(out, content + (slot - seen), key_size);
            *out_size = key_size;
            got = true;
            /* All three together, as in chain_read_at: blk and seen mean nothing
             * against any other chain. */
            cur->blk  = blk;
            cur->seen = seen;
            cur->root = root;
            bm25_chain_page_keep(cur, blk, pg, buf, BM25_PAGE_KEYMAP);
        }
        seen += pagebytes;
        next = BM25PageGetOpaque(pg)->nextblk;
        bm25_chain_page_done(buf);
        if (got)
            break;
        /* Walking past this page: it must hold the writer's full key span (issue #303,
         * ADR 0111's dense-chain rule). Without it a cycle through pages with no keys
         * never advanced `seen` and spun until cancelled, and a short page would shift
         * every later docid onto another document's key. key_size is known here: a walk
         * from the root decoded it above, and a resume took it from the cache. After the
         * release, on values already copied out, and only on a page transition. */
        if (next != InvalidBlockNumber &&
            pagebytes != bm25_keymap_full_span(key_size, at_root))
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: keymap chain page at block %u holds %u key bytes, not "
                            "the %zu every page but the last carries",
                            blk, pagebytes, bm25_keymap_full_span(key_size, at_root)),
                     errhint("REINDEX the index.")));
        blk = next;
    }
    /* Model: bm25_analyzer.c's bm25_fieldcfg_read (`got != field_count`). local_docid
     * < h->ndocs is checked above and keymap_root is non-Invalid on this path, so an
     * uncorrupted KEYMAP chain always sets got=true before running out. A false
     * return used to mean exactly that -- corruption -- but every caller (bm25_scan_rank.c,
     * bm25_wand.c, bm25_merge.c) reads a plain `false` as "no key_field" and silently
     * falls back to ctid. Loud beats a silently wrong scored result or a merged
     * segment with missing keys. */
    if (!got)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: KEYMAP chain for segment gen %u ended before local_docid %u",
                        h->gen, local_docid)));
    return got;
}

/* One-shot form: a fresh cursor, so a walk from the root and one extent capture per
 * call. Kept for single lookups and as the root-walk reference sql/110 checks the
 * reader form against; no per-row or per-document caller uses it any more. The
 * capture is left to seg_key_cur (nblocks = 0), so a keyless segment returns before
 * paying for it. */
bool
bm25_seg_key(Relation index, BM25SegmentHeader *h, uint32 local_docid,
             unsigned char *out /* >= key_size */, uint16 *out_size)
{
    BM25ChainCursor cur;
    uint16          key_size = 0;

    bm25_chain_cursor_init(&cur, h->keymap_root, 0);
    return seg_key_cur(index, h, &cur, &key_size, local_docid, out, out_size);
}

/* Reader form (issue #225): the KEYMAP cursor and key_size cache live in the reader, and
 * the extent was captured once by bm25_seg_reader_init. */
bool
bm25_seg_reader_key(BM25SegReader *r, uint32 local_docid,
                    unsigned char *out /* >= key_size */, uint16 *out_size)
{
    return seg_key_cur(r->index, r->h, &r->keymap, &r->key_size, local_docid,
                       out, out_size);
}

/* ---- BM25SegKeyCache: per-segment readers for a ranked-row loop ----
 *
 * The ranked-row finalizers resolve one key per row, and the row's winning segment
 * varies row to row, so a single BM25SegReader cannot serve the loop. Each used to pay
 * a bm25_seg_header_read (bm25_scan_rank.c's and bm25_wand.c's finalizers both did) plus a
 * KEYMAP walk from the root per row. The cache keeps one header + reader per distinct
 * segment for the loop's duration. Rows arrive in score order, not docid order, so a
 * caller that resolves them in that order falls back to a root walk on every backward
 * docid jump within a segment; the finalizers therefore go through
 * bm25_seg_key_cache_fill, which visits a multi-page KEYMAP's rows in docid order.
 *
 * Keyed on (header_blkno, gen) rather than header_blkno alone: within one ranking the
 * two always travel together, but a key that names the generation cannot hand a row
 * a reader opened on some other segment's header even if that ever stops being true. */
typedef struct BM25SegKeyCacheKey
{
    BlockNumber header_blkno;
    uint32      gen;
} BM25SegKeyCacheKey;

typedef struct BM25SegKeyCacheEntry
{
    BM25SegKeyCacheKey key;         /* hash key: MUST be first */
    BM25SegmentHeader  h;           /* the reader's `h` points here; dynahash never moves it */
    BM25SegReader      r;
} BM25SegKeyCacheEntry;

void
bm25_seg_key_cache_init(BM25SegKeyCache *c, Relation index, MemoryContext cxt)
{
    HASHCTL ctl;

    memset(&ctl, 0, sizeof(ctl));
    ctl.keysize   = sizeof(BM25SegKeyCacheKey);
    ctl.entrysize = sizeof(BM25SegKeyCacheEntry);
    ctl.hcxt      = cxt;
    c->index = index;
    c->segs  = hash_create("bm25 ranked-row key readers", 16, &ctl,
                           HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    c->last  = NULL;
    c->cxt   = cxt;
    c->nimages = 0;
}

/* How many of the cache's segment readers may keep a KEYMAP page image (issue #267):
 * 64 x 8 KB = 512 KB at most, in the cache's context, freed with it. Unlike the
 * scorers' readers, which live for one (term, segment), the cache keeps one reader per
 * segment the ranking touches for the whole finalizer, and it needs them all at once:
 * rows of one-page KEYMAPs are resolved in rank order, hopping between segments, and
 * their image is what turns one buffer access per row into one per segment. The
 * segment count is not bounded by anything else here, so the images are capped; a
 * reader past the cap reads per lookup, as every reader did before. 64 covers the
 * 53-segment bench corpus (bench/wand_global_ub.sh) with room to spare. */
#define BM25_KEYCACHE_MAX_IMAGES    64

/* The entry for (header_blkno, gen), created on first sight -- which reads the header. */
static BM25SegKeyCacheEntry *
key_cache_entry(BM25SegKeyCache *c, BlockNumber header_blkno, uint32 gen)
{
    BM25SegKeyCacheEntry *e = c->last;

    if (e == NULL || e->key.header_blkno != header_blkno || e->key.gen != gen)
    {
        BM25SegKeyCacheKey key;

        memset(&key, 0, sizeof(key));       /* HASH_BLOBS hashes every byte of the key */
        key.header_blkno = header_blkno;
        key.gen = gen;
        e = (BM25SegKeyCacheEntry *) hash_search(c->segs, &key, HASH_FIND, NULL);
        if (e == NULL)
        {
            BM25SegmentHeader hdr;
            BM25SegReader     r;

            /* Everything that can raise runs BEFORE the entry exists, so a caught
             * error (bm25_scan_build_ranking retries on a serialization failure) can
             * never leave a half-built entry for a later lookup to find. The reader is
             * then re-pointed at the entry's own header copy, which is where it must
             * point for the entry's life. */
            bm25_seg_header_read(c->index, header_blkno, gen, &hdr);
            bm25_seg_reader_init(&r, c->index, &hdr);
            if (c->nimages < BM25_KEYCACHE_MAX_IMAGES)
                bm25_seg_reader_cache_pages(&r, c->cxt);
            e = (BM25SegKeyCacheEntry *) hash_search(c->segs, &key, HASH_ENTER, NULL);
            if (r.keymap.img_cxt != NULL)
                c->nimages++;       /* counted once the entry exists */
            e->h = hdr;
            e->r = r;
            e->r.h = &e->h;
        }
        c->last = e;
    }
    return e;
}

/* True when a segment's whole KEYMAP fits on its root page, by the writer's geometry
 * (bm25_keymap_write): the root page's content area after BM25KeymapHeader, in whole
 * keys. Such a chain costs one access per lookup in any order -- a restart from the
 * root lands on the only page -- so sorting its rows buys nothing. Used only to decide
 * whether to sort; a wrong answer (a corrupt header, a key_size that disagrees with
 * the stride) costs time, never a different key. */
static bool
keymap_fits_root_page(const BM25SegmentHeader *h, uint16 key_size)
{
    Size    room = BLCKSZ - MAXALIGN(sizeof(BM25PageOpaque)) - SizeOfPageHeaderData -
                   sizeof(BM25KeymapHeader);

    return key_size != 0 && h->ndocs <= (uint64) (room / key_size);
}

/* Resolve one request through its segment's reader into its slot. The copy is the one
 * the finalizers did inline: km_size is the caller's stride, discovered from the first
 * keyed segment, and ksz is what this row's segment reports. They agree on a
 * well-formed index, but nothing upstream guarantees it for a corrupt or mixed one,
 * and kbuf past ksz is uninitialized stack, so only ksz bytes are copied and the rest
 * of the slot is zeroed explicitly. present[slot] is set only on a true return (false
 * means the segment has no KEYMAP), because a zero slot is also a valid key for id = 0. */
static void
key_req_resolve(BM25SegKeyCacheEntry *e, const BM25KeyReq *req,
                unsigned char *keys, bool *present, uint16 km_size)
{
    unsigned char kbuf[BM25_KEY_MAX_SIZE];
    uint16        ksz;

    if (bm25_seg_reader_key(&e->r, req->local_docid, kbuf, &ksz))
    {
        Size           klen = Min((Size) ksz, (Size) km_size);
        unsigned char *dst = keys + (Size) req->slot * km_size;

        memcpy(dst, kbuf, klen);
        if (klen < km_size)
            memset(dst + klen, 0, km_size - klen);
        present[req->slot] = true;
    }
}

/* (segment, docid) order for bm25_seg_key_cache_fill. slot is the last key only to make
 * the order total; one ranking never holds two rows for the same (segment, docid). */
static int
key_req_cmp(const void *a, const void *b)
{
    const BM25KeyReq *x = (const BM25KeyReq *) a;
    const BM25KeyReq *y = (const BM25KeyReq *) b;

    if (x->header_blkno != y->header_blkno)
        return (x->header_blkno < y->header_blkno) ? -1 : 1;
    if (x->gen != y->gen)
        return (x->gen < y->gen) ? -1 : 1;
    if (x->local_docid != y->local_docid)
        return (x->local_docid < y->local_docid) ? -1 : 1;
    if (x->slot != y->slot)
        return (x->slot < y->slot) ? -1 : 1;
    return 0;
}

/* Issue #246. The finalizers used to look each row's key up one at a time in rank
 * order, and seg_key_cur resumes its cursor only when the target is at or past it, so
 * every backward docid jump inside a segment re-walked that segment's KEYMAP chain from
 * the root: ~one access per 2,000 docids of the row's position, 78% of a rare-term
 * k=1000 query on a 100k-document segment (ADR 0100). Visiting a segment's rows sorted
 * by docid makes its walk one forward pass.
 *
 * Only rows whose segment's KEYMAP spans more than one page are sorted. On a one-page
 * KEYMAP a backward jump costs nothing (the restart lands on the only page), and the
 * sort is then pure CPU: measured on a 51-segment index of ~2,000-document segments,
 * sorting all ~100k rows of an exhaustive ranking added ~15% to the query and saved no
 * access. So a first pass, in rank order, resolves the rows of one-page segments at
 * once, exactly as before, and compacts the rest to the front of reqs; only those are
 * sorted and resolved forward.
 *
 * Output is unchanged by construction: a key is a pure function of (segment, docid),
 * each is written to its row's own slot, and the slots never overlap, so the order the
 * rows are visited in cannot change a byte. Errors (a corrupt chain) can surface at a
 * different row than rank order would reach first; which rows are looked up, and so
 * whether the build errors, is unchanged.
 *
 * Accepted residual (#267, item 5): this resolves a key for EVERY ranked row, while
 * under ORDER BY ... LIMIT k the executor returns only the first k; an exhaustive
 * ranking of ~100k rows still resolves ~100k keys. Since #267's page images a key on
 * the page the previous lookup landed on costs no buffer access, so what is left is
 * mostly per-row CPU. Resolving lazily (on emit, or per block of rows) is entangled
 * with three things that each want every key up front: bm25_build_score_index builds
 * the score-by-key hash over all of ranked_keys; the query-qualified
 * bm25_score_key(key, query) accessors from #263 read that hash; and the WAND over-pull
 * tail rebuild (#268) replaces the ranking mid-scan, so keys resolved after it must
 * come from the rebuilt ranking. Any lazy scheme has to fill the remaining keys before
 * the hash is built and re-key on a rebuild. */
void
bm25_seg_key_cache_fill(BM25SegKeyCache *c, BM25KeyReq *reqs, uint32 n,
                        unsigned char *keys, bool *present, uint16 km_size)
{
    uint32 i;
    uint32 ndefer = 0;

    for (i = 0; i < n; i++)
    {
        BM25SegKeyCacheEntry *e;

        CHECK_FOR_INTERRUPTS();

        e = key_cache_entry(c, reqs[i].header_blkno, reqs[i].gen);
        if (keymap_fits_root_page(&e->h, km_size))
            key_req_resolve(e, &reqs[i], keys, present, km_size);
        else
            reqs[ndefer++] = reqs[i];       /* ndefer <= i: compacts in place */
    }

    if (ndefer > 1)
        qsort(reqs, ndefer, sizeof(BM25KeyReq), key_req_cmp);
    for (i = 0; i < ndefer; i++)
    {
        CHECK_FOR_INTERRUPTS();

        key_req_resolve(key_cache_entry(c, reqs[i].header_blkno, reqs[i].gen),
                        &reqs[i], keys, present, km_size);
    }
}

/* bm25_debug_seg_keymap(index regclass, seg int) -- per-segment KEYMAP introspection.
 * When the segment has a key_field: one row per dense local docid, exposing the raw
 * key bytes and a type-decoded convenience column (key_int4/key_int8/key_uuid/
 * key_text -- only the one matching key_type populated). When there is NO key_field
 * (keymap_root Invalid): exactly one row with local_docid NULL, key_type=0,
 * key_size=0, keymap_root=<Invalid=4294967295>, all key columns NULL -- the ctid-
 * fallback witness. Development/regression introspection only; not on the query path.
 *
 * Race note (adversarial review, 2026-08): this calls bm25_segcat_read without the
 * metapage singleton (see bm25_handler.c's bm25_bulkdelete for a caller that DOES
 * take it, and why; bm25_fsm.c's bm25_reclaim_orphans and bm25_merge.c's
 * bm25_merge_rewrite_all hold it across their own bm25_segcat_read too). On a
 * healthy index under concurrent merge/reclaim, that
 * now means bm25_segcat_read's own short/truncated-chain ERROR (fix 3) is reachable
 * here where it wasn't before -- a benign snapshot race that used to read as an
 * (silently wrong) undercount now surfaces as ERRCODE_INDEX_CORRUPTED instead. Still
 * a strict improvement (loud beats silently wrong), but a debug SRF that fails
 * transiently under concurrency is worth knowing about if it's ever seen in the wild.
 *
 * SQL: bm25_debug_seg_keymap(index regclass, seg int,
 *        OUT local_docid bigint, OUT key_type int, OUT key_size int,
 *        OUT keymap_root bigint, OUT key_bytes bytea,
 *        OUT key_int4 int, OUT key_int8 bigint, OUT key_uuid uuid, OUT key_text text)
 */
PG_FUNCTION_INFO_V1(bm25_debug_seg_keymap);
Datum
bm25_debug_seg_keymap(PG_FUNCTION_ARGS)
{
    ReturnSetInfo      *rsi   = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                 relid = PG_GETARG_OID(0);
    int32               seg   = PG_GETARG_INT32(1);
    Relation            index;
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   h;
    BM25SegReader       rdr;
    TupleDesc           tupdesc;
    Tuplestorestate    *ts;
    MemoryContext       oldctx;
    uint8               km_type = BM25_KEY_NONE;
    uint16              km_size = 0;
    uint32              d;

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
    bm25_segcat_read(index, &segs, &nsegs);
    if (seg < 0 || (uint32) seg >= nsegs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: segment %d out of range (%u live)", seg, nsegs)));
    }
    bm25_seg_header_read(index, segs[seg].header_blkno, segs[seg].gen, &h);
    (void) bm25_seg_keymeta(index, &h, &km_type, &km_size);

    if (h.keymap_root == InvalidBlockNumber)
    {
        /* No key_field: one header-only row (local_docid NULL, keymap_root Invalid). */
        Datum vals[10];
        bool  nulls[10];
        memset(nulls, true, sizeof(nulls));
        nulls[1] = false;
        vals[1] = Int32GetDatum(0);                             /* key_type */
        nulls[2] = false;
        vals[2] = Int32GetDatum(0);                             /* key_size */
        nulls[3] = false;
        vals[3] = Int64GetDatum((int64) InvalidBlockNumber);    /* keymap_root */
        tuplestore_putvalues(ts, tupdesc, vals, nulls);
    }
    else
    {
        /* Ascending docids, so one reader makes this one forward pass over the KEYMAP
         * chain instead of a walk from the root per docid (issue #225). */
        bm25_seg_reader_init(&rdr, index, &h);
        for (d = 0; d < h.ndocs; d++)
        {
            Datum         vals[10];
            bool          nulls[10];
            unsigned char kbuf[BM25_KEY_MAX_SIZE];
            uint16        ksz = 0;
            bytea        *raw;

            /* This loop was O(ndocs x chain length) while bm25_seg_key restarted from
             * keymap_root on every call; the reader's KEYMAP cursor (issue #225) makes
             * it linear. The interrupt check stays: ndocs is unbounded either way. */
            CHECK_FOR_INTERRUPTS();

            memset(nulls, true, sizeof(nulls));
            if (!bm25_seg_reader_key(&rdr, d, kbuf, &ksz))
                continue;   /* unreachable: keymap_root valid => every docid resolves */

            nulls[0] = false;
            vals[0] = Int64GetDatum((int64) d);                 /* local_docid */
            nulls[1] = false;
            vals[1] = Int32GetDatum((int32) km_type);           /* key_type */
            nulls[2] = false;
            vals[2] = Int32GetDatum((int32) km_size);           /* key_size */
            nulls[3] = false;
            vals[3] = Int64GetDatum((int64) h.keymap_root);     /* keymap_root */

            raw = (bytea *) palloc(VARHDRSZ + ksz);
            SET_VARSIZE(raw, VARHDRSZ + ksz);
            memcpy(VARDATA(raw), kbuf, ksz);
            nulls[4] = false;
            vals[4] = PointerGetDatum(raw);                     /* key_bytes */

            switch (km_type)
            {
                case BM25_KEY_INT4:
                    {
                        int32       v;

                        memcpy(&v, kbuf, sizeof(v));
                        nulls[5] = false;
                        vals[5] = Int32GetDatum(v);
                        break;
                    }
                case BM25_KEY_INT8:
                    {
                        int64       v;

                        memcpy(&v, kbuf, sizeof(v));
                        nulls[6] = false;
                        vals[6] = Int64GetDatum(v);
                        break;
                    }
                case BM25_KEY_UUID:
                    {
                        pg_uuid_t  *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));

                        memcpy(u->data, kbuf, UUID_LEN);
                        nulls[7] = false;
                        vals[7] = UUIDPGetDatum(u);
                        break;
                    }
                case BM25_KEY_TEXT:
                    {
                        int         len = 0;

                        while (len < ksz && kbuf[len] != '\0')
                            len++;
                        /* Issue #313 META-09: bm25_key_extract truncates by bytes, so
                         * a long multibyte key can end mid-character, and a text
                         * Datum holding that tail is invalid in the server encoding.
                         * Clip to the last whole character; key_bytes keeps the
                         * stored bytes exactly. */
                        len = pg_mbcliplen((const char *) kbuf, len, len);
                        nulls[8] = false;
                        vals[8] = PointerGetDatum(cstring_to_text_with_len((const char *) kbuf, len));
                        break;
                    }
                default:
                    break;
            }
            tuplestore_putvalues(ts, tupdesc, vals, nulls);
        }
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* bm25_debug_keymap_cursor_crosstalk(index regclass, seg_a int, seg_b int, docid bigint)
 *   RETURNS bytea -- issue #225, the KEYMAP counterpart of
 *   bm25_debug_chain_cursor_crosstalk (bm25_seg_chain.c).
 *
 * Deliberate misuse: a reader opened on segment seg_a reads seg_a's key 0, which parks
 * its KEYMAP cursor on seg_a's root page with seen = 0 and caches seg_a's key_size;
 * then the SAME reader, re-pointed at seg_b's header, reads seg_b's key `docid`. No
 * caller does this -- each reader is opened on one segment and kept there -- so
 * without a probe the `cur->root == root` clause of seg_key_cur's resume test would
 * have no test at all. With the clause the lookup walks seg_b's chain from its root
 * and returns seg_b's real key. Without it, seen = 0 satisfies the offset test for any
 * docid, the walk resumes on seg_a's page, and the gen check rejects that page as
 * "reclaimed concurrently" (seg_a and seg_b carry different gens) -- or, for two
 * same-gen chains, would read the wrong segment's bytes.
 *
 * Returns the key bytes so the suite can compare them with seg_b's key read through
 * the one-shot form. TEST-ONLY, read-only; REVOKEd from PUBLIC with every bm25_debug_*. */
PG_FUNCTION_INFO_V1(bm25_debug_keymap_cursor_crosstalk);
Datum
bm25_debug_keymap_cursor_crosstalk(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    int32               seg_a = PG_GETARG_INT32(1);
    int32               seg_b = PG_GETARG_INT32(2);
    int64               docid = PG_GETARG_INT64(3);
    Relation            index = bm25_index_open_readable(relid, AccessShareLock);
    BM25SegCatEntry    *segs;
    uint32              nsegs;
    BM25SegmentHeader   ha;
    BM25SegmentHeader   hb;
    BM25SegReader       r;
    unsigned char       kbuf[BM25_KEY_MAX_SIZE];
    uint16              ksz = 0;
    bytea              *out;

    bm25_segcat_read(index, &segs, &nsegs);
    if (seg_a < 0 || (uint32) seg_a >= nsegs || seg_b < 0 || (uint32) seg_b >= nsegs ||
        seg_a == seg_b)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_keymap_cursor_crosstalk: segments %d and %d must be "
                        "two different live segments (%u live)", seg_a, seg_b, nsegs)));
    }
    bm25_seg_header_read(index, segs[seg_a].header_blkno, segs[seg_a].gen, &ha);
    bm25_seg_header_read(index, segs[seg_b].header_blkno, segs[seg_b].gen, &hb);
    if (ha.keymap_root == InvalidBlockNumber || hb.keymap_root == InvalidBlockNumber ||
        ha.ndocs == 0)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_keymap_cursor_crosstalk: both segments must carry a "
                        "KEYMAP, and segment %d at least one document", seg_a)));
    }
    if (docid < 0 || (uint64) docid >= hb.ndocs)
    {
        index_close(index, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25_debug_keymap_cursor_crosstalk: docid " INT64_FORMAT
                        " out of range for segment %d (" UINT64_FORMAT " documents)",
                        docid, seg_b, hb.ndocs)));
    }

    bm25_seg_reader_init(&r, index, &ha);
    (void) bm25_seg_reader_key(&r, 0, kbuf, &ksz);     /* park on seg_a's root page */
    r.h = &hb;                                          /* the misuse under test */
    (void) bm25_seg_reader_key(&r, (uint32) docid, kbuf, &ksz);

    out = (bytea *) palloc(VARHDRSZ + ksz);
    SET_VARSIZE(out, VARHDRSZ + ksz);
    memcpy(VARDATA(out), kbuf, ksz);
    index_close(index, AccessShareLock);
    PG_RETURN_BYTEA_P(out);
}

/* ------------------------------------------------------------------------
 * Keymap decode-boundary probes (trust-boundary review, 2026-08). The three
 * validators above only fire on an already-corrupt KEYMAP page, which a
 * regression suite cannot produce, so these run the SAME validators over
 * caller-supplied values -- the same reasoning 69_decode_boundary's probes
 * apply to the h6 decode boundary. TEST-ONLY, like the rest of the
 * bm25_debug_* surface: pure functions of their scalar arguments, no relation
 * touched, no privilege to escalate; covered by the install script's REVOKE
 * loop (matched by the bm25_debug_ prefix) same as every other probe.
 * ------------------------------------------------------------------------ */

/* bm25_seg_keymeta's key_type/key_size PAIR check. */
PG_FUNCTION_INFO_V1(bm25_debug_keymeta_validate);
Datum
bm25_debug_keymeta_validate(PG_FUNCTION_ARGS)
{
    int32 key_type = PG_GETARG_INT32(0);
    int32 key_size = PG_GETARG_INT32(1);

    if (key_type < 0 || key_type > PG_UINT8_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_keymeta_validate: key_type out of uint8 range")));
    if (key_size < 0 || key_size > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_keymeta_validate: key_size out of uint16 range")));

    bm25_seg_keymeta_validate((uint8) key_type, (uint16) key_size);
    PG_RETURN_VOID();
}

/* The KEYMAP walk's (seg_key_cur's) three checks, run in the SAME order and against the SAME
 * derived quantities as the real per-page decode loop: the header-step bound
 * (key_size width + the pagebytes-underflow guard), then the span check
 * against the header-adjusted pagebytes. `pagebytes` is the notional page's
 * TOTAL content bytes (pd_lower - page header) -- what the real code has
 * BEFORE subtracting sizeof(BM25KeymapHeader) -- matching the `pagebytes`
 * local in seg_key_cur at the point its header check runs; `local_off` is
 * the key's byte offset within the page's key[] stream (slot - seen). Returns
 * key_size on success, so a well-formed call demonstrates the expected width,
 * like bm25_debug_block_validate returning the validated block length. */
PG_FUNCTION_INFO_V1(bm25_debug_seg_key_bounds);
Datum
bm25_debug_seg_key_bounds(PG_FUNCTION_ARGS)
{
    int32   key_size  = PG_GETARG_INT32(0);
    int64   pagebytes = PG_GETARG_INT64(1);
    int64   local_off = PG_GETARG_INT64(2);

    if (key_size < 0 || key_size > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_key_bounds: key_size out of uint16 range")));
    if (pagebytes < 0 || pagebytes > PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_key_bounds: pagebytes out of uint32 range")));
    if (local_off < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_seg_key_bounds: local_off must be non-negative")));

    bm25_seg_key_header_validate((uint16) key_size, (uint32) pagebytes);
    bm25_seg_key_span_validate((Size) local_off, (uint16) key_size,
                               (uint32) (pagebytes - (int64) sizeof(BM25KeymapHeader)));
    PG_RETURN_INT32(key_size);
}
