/* bm25_meta.c -- the metapage (the LSM directory) + Generic WAL page helpers.
 *
 * No format-version number is spelled out in prose in this file:
 * BM25_FORMAT_VERSION (bm25_format.h) is the single source of truth for what
 * this build stamps, and a copy of that number in a comment rots at the next
 * bump. The layout a current reader must tolerate is in any case a RANGE, not
 * one version -- bm25_meta_validate accepts format_version from
 * BM25_OLDEST_READABLE up, so an index stamped by an older binary keeps its own
 * version and is read as-is, no REINDEX. A version number below is therefore
 * only ever a HISTORICAL note about which version introduced something.
 *
 * The metapage (block 0) anchors the whole index: pending-list head/tail, the
 * segment-catalog root, the global stats cache, and the retired-free-list root.
 * Almost every mutation goes through Generic WAL so crash recovery and physical
 * replication come from core; the ONE exception is INIT_FORKNUM's block 0
 * (bm25_meta_finish), which uses log_newpage_buffer instead -- GenericXLog gates
 * WAL on RelationNeedsWAL, which is false for an unlogged relation's init fork
 * too, so it would silently skip WAL exactly where this page needs it
 * unconditionally. See bm25_meta_finish's header comment for the full story.
 * After ANY metapage write we advance pd_lower past BM25MetaPageData (GIN
 * ginfast.c:419) -- Generic WAL's page-hole compression silently drops bytes
 * above pd_lower otherwise. That advance must RAISE, never assign: see
 * bm25_meta_set_pd_lower. */
#include "postgres.h"

#include "bm25.h"
#include "funcapi.h"
#include "access/table.h"       /* table_open/table_close (heap, D-ALLOC/M6) */
#include "access/xloginsert.h" /* log_newpage_buffer -- INIT_FORKNUM's unconditional WAL (bm25_meta_finish) */
#include "catalog/index.h"      /* IndexGetRelation -- heap behind an index */
#include "catalog/pg_class.h"   /* RelationRelationId -- entry-point ownership check */
#include "storage/indexfsm.h"  /* GetFreeIndexPage/RecordFreeIndexPage -- FSM-freed page reuse */
#include "utils/acl.h"         /* object_ownercheck / aclcheck_error */
#include "utils/lsyscache.h"   /* get_rel_name, get_rel_relkind */
#include "utils/rls.h"         /* check_enable_rls -- readable gate under row-level security */
#include "utils/snapmgr.h"     /* GlobalVisCheckRemovableFullXid -- horizon-gated reuse */

/*
 * bm25_index_open_owned -- the ONE way ANY entry point may open a user-supplied
 * regclass for mutation. Was bm25_debug_open_index; renamed when the maintenance
 * functions (bm25_seal, bm25_merge, bm25_upgrade) started routing through it too,
 * because "debug" in the name no longer described who calls it.
 *
 * index_open() validates relkind and NOTHING else: not the access method, not the
 * ACL, not ownership. So a plain `SELECT bm25_debug_stamp_version(<any oid>, ...)`
 * used to open ANY index in the database and write block 0 of it under a Generic
 * WAL window -- durable, replayed on every standby. Aimed at a btree,
 * BM25PageGetMeta resolves to PageGetContents, so BM25MetaPageData.format_version
 * lands exactly on BTMetaPageData.btm_version; pointed at pg_class_oid_index that
 * makes every OID lookup fail with "version mismatch", on the primary and on every
 * standby at once. None of these functions is on the query path.
 *
 * The same gap existed on the PUBLIC maintenance surface, where a REVOKE is not an
 * option because those functions are meant to be callable: bm25_seal, bm25_merge
 * and bm25_upgrade each took a regclass and index_open()ed it, so any role that
 * could connect could seal, merge, re-stamp and (with the synthetic-transform
 * toggle) fully re-emit every segment of an index it has no rights to -- repeatable
 * as a cheap WAL-amplification DoS. A bare numeric OID cast to regclass needs no
 * schema privileges at all, so nothing upstream filtered it either.
 *
 * All three checks must happen BEFORE any page is touched:
 *   - ownership, so the regclass cannot name someone else's relation;
 *   - not in recovery, since every caller writes and a standby cannot (#307);
 *   - AM identity, so it cannot name an index this code does not understand.
 *
 * Ownership rather than an ACL bit: PostgreSQL has no privilege that means "may
 * maintain this index", and core's closest precedent, gin_clean_pending_list,
 * uses object_ownercheck for exactly this shape of function.
 *
 * AM identity is tested by comparing the resolved handler's ambuild against our
 * own bm25_build (bm25_handler.c sets amr->ambuild = bm25_build). That is exact
 * and needs no catalog lookup or AM-name string, so renaming the access method
 * cannot silently disarm it.
 */
/* The shared half of both gates: everything that must hold before a page is read,
 * checked in the order that lets each message be accurate.
 *
 * HDL-05 added the relkind test. Both gates checked ownership/ACL and AM identity
 * and nothing else, but index_open accepts RELKIND_PARTITIONED_INDEX as well as
 * RELKIND_INDEX, and a partitioned index's relcache entry DOES carry rd_indam --
 * its relam is set -- so `rd_indam->ambuild == bm25_build` holds for one. Callers
 * then went straight to block 0 (bm25_meta_read's ReadBuffer) on a relation with no
 * storage: RelationInitPhysicalAddr returns early for a storage-less relkind,
 * leaving rd_locator zeroed, so the best case was an internal-looking
 * `could not open file "base/0/0"` instead of the clean ERRCODE_WRONG_OBJECT_TYPE
 * these helpers exist to produce, and a zeroed RelFileLocator reaching smgropen is
 * a plausible assertion trip under --enable-cassert. bm25_upgrade additionally took
 * the write lock and ran a full seal before failing.
 *
 * AM identity is tested FIRST. Neither check reads a page, so the order costs
 * nothing and is purely about which message is accurate: a partitioned index of
 * ANOTHER access method fails both, and reporting it as "a partitioned index; only
 * its leaf partitions hold bm25 data" would send the caller to a leaf that is not a
 * bm25 index either. Identity first means each relation gets the message that is
 * true of it, and a partitioned BM25 index -- which passes identity -- still falls
 * through to the storage check below rather than reaching a page read. */
static void
bm25_index_gate_validate(Relation index, LOCKMODE lockmode)
{
    if (index->rd_indam == NULL || index->rd_indam->ambuild != bm25_build)
    {
        char *relname = pstrdup(RelationGetRelationName(index));

        index_close(index, lockmode);
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("bm25: \"%s\" is not a bm25_native index", relname)));
    }

    if (!RELKIND_HAS_STORAGE(index->rd_rel->relkind))
    {
        char *relname = pstrdup(RelationGetRelationName(index));

        index_close(index, lockmode);
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("bm25: \"%s\" has no storage", relname),
                 errdetail("It is a partitioned index; only its leaf partitions "
                           "hold bm25 data."),
                 errhint("Name one of the partitions' indexes instead.")));
    }
}

/* Issue #313 META-06/SURFACE-05: resolve the OID's relkind before anything that
 * assumes it names an index. Without it, a typo or a table's OID reached
 * IndexGetRelation (open_readable: "cache lookup failed for index N") or index_open
 * (open_owned as superuser: "could not open relation with OID N"), both XX000 -- an
 * internal-error SQLSTATE from a PUBLIC entry point on an ordinary argument mistake.
 * get_rel_relkind takes no lock; a relation dropped between here and index_open still
 * gets index_open's own error, which is the race every unlocked OID lookup in core
 * accepts. Partitioned indexes pass here and are refused by bm25_index_gate_validate
 * with its more specific message. pg_class is world-readable, so reporting the
 * relkind before the ownership or ACL check discloses nothing. */
static void
bm25_index_relkind_check(Oid relid)
{
    char        relkind = get_rel_relkind(relid);

    if (relkind == '\0')
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("bm25: index with OID %u does not exist", relid)));
    if (relkind != RELKIND_INDEX && relkind != RELKIND_PARTITIONED_INDEX)
        ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                 errmsg("bm25: \"%s\" is not an index", get_rel_name(relid))));
}

Relation
bm25_index_open_owned(Oid relid, LOCKMODE lockmode)
{
    Relation    index;

    bm25_index_relkind_check(relid);

    if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, get_rel_name(relid));

    /* META-07 (#307): every caller of this gate writes pages, and a standby cannot.
     * Without this check they reached the buffer write and failed at XLogBeginInsert
     * with XX000 -- after bm25_debug_alloc_unknown_page had already extended the
     * standby's relation file. Core refuses INSERT, VACUUM and REINDEX during
     * recovery itself, so these SQL entry points were the only way in. Core's own
     * helper, so the SQLSTATE (25006) and wording match every other write a standby
     * refuses. After the ownership check, so a non-owner gets the same 42501 on either
     * node, and before index_open, so no lock is taken and no page is read. */
    PreventCommandDuringRecovery("bm25 index maintenance");

    index = index_open(relid, lockmode);
    bm25_index_gate_validate(index, lockmode);

    return index;
}

/*
 * bm25_index_open_readable -- the read-only sibling, for entry points that report
 * on an index rather than mutate it: bm25_stats, bm25_wand_stats, and (H13) every
 * read-only bm25_debug_* SRF. The count is deliberately not stated -- it was "33" here
 * and in two other files while the true figure moved through 35 and beyond, and a
 * number in a comment beside a set that grows every release is a liability with no
 * upside. sql/63_debug_privileges pins the surface by name.
 *
 * Ownership would be wrong here: reading an index's statistics is a reasonable
 * thing for a non-owner to do. But it must not be a way around table privileges,
 * so the gate is SELECT on the INDEXED TABLE -- the same privilege that would let
 * the caller see the underlying rows. AM identity is checked for the same reason
 * as in the owned variant: these functions read bm25 page layouts.
 *
 * The debug readers were revoked from PUBLIC by the install script (docs/adr/0020)
 * but still called index_open() directly, so an explicit EXECUTE grant -- a plausible
 * thing to hand an ops role -- was enough to dump the dictionary, postings, positions
 * and per-doc key values of ANY bm25 index in the database, including indexes on
 * tables the caller could not SELECT. The REVOKE bounds who can call; this bounds
 * what they can aim it at. Pointed at a foreign AM they also read the special area
 * as a 24-byte BM25PageOpaque where btree's is 16 -- past pd_special -- which the AM
 * identity check now refuses before any page is read.
 */
Relation
bm25_index_open_readable(Oid relid, LOCKMODE lockmode)
{
    Relation    index;
    Oid         heaprelid;
    AclResult   aclresult;

    bm25_index_relkind_check(relid);
    heaprelid = IndexGetRelation(relid, false);

    aclresult = pg_class_aclcheck(heaprelid, GetUserId(), ACL_SELECT);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, OBJECT_TABLE, get_rel_name(heaprelid));

    /* SURFACE-04 (#310): table SELECT is not "may see every row" when a row-level
     * security policy applies to the caller, and everything behind this gate reports
     * on the WHOLE index -- corpus statistics, or term-level dumps of rows the policy
     * hides. So the gate refuses outright, the way pg_stats omits RLS tables rather
     * than filtering them. Leaf heap only, unlike D8's ancestor walk in bm25_score.c:
     * this gate's ACL is leaf-only too, and a parent's policy does not apply to
     * direct access to the leaf. noError for the same reason as D8: with
     * row_security = off, check_enable_rls would raise its own error for a caller the
     * policy covers; RLS_ENABLED comes back instead, so both settings get this one
     * message. The owner (absent FORCE), superusers and BYPASSRLS roles get
     * RLS_NONE / RLS_NONE_ENV and pass. */
    if (check_enable_rls(heaprelid, InvalidOid, true) == RLS_ENABLED)
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("bm25: cannot report on index \"%s\": row-level security is enabled "
                        "for table \"%s\"", get_rel_name(relid), get_rel_name(heaprelid)),
                 errdetail("This function reports on every row the index covers, "
                           "including rows the table's policies hide from the current role.")));

    index = index_open(relid, lockmode);
    bm25_index_gate_validate(index, lockmode);

    return index;
}

/* Initialize a page's header + BM25PageOpaque in place. Does NOT open a Generic
 * WAL window -- the caller owns the GenericXLogStart/RegisterBuffer window and
 * must call this on the registered page copy (with GENERIC_XLOG_FULL_IMAGE, since
 * PageInit rewrites the whole page). */
void
bm25_page_init(Page page, uint16 flags)
{
    BM25PageOpaque *op;
    PageInit(page, BLCKSZ, sizeof(BM25PageOpaque));
    op = BM25PageGetOpaque(page);
    op->flags = flags;
    op->unused = 0;
    op->nextblk = InvalidBlockNumber;
    op->retire_xid = InvalidFullTransactionId;
    op->seg_gen = 0;            /* option (d): non-segment default; the segment builder
                                 * overwrites this with the segment's gen on each page it
                                 * writes (segment pages are validated by the reader), and
                                 * the pending appender with its chain epoch (#291). */
}

/* Raise the metapage's pd_lower to at least the end of THIS build's
 * BM25MetaPageData. The single chokepoint every metapage writer must use.
 *
 * WHY Max() and not assignment (this is load-bearing for docs/adr/0009's additive
 * contract, not a style choice): the metapage grows by APPENDING struct fields --
 * historically at v3->v4 and again at v5->v6 (v7 appended nothing here), and that is
 * how any future version would grow it too. Appending is ADDITIVE,
 * so the floor gate lets an OLDER binary accept a NEWER index. That older binary
 * still WRITES the metapage on any insert/seal/merge (stats bump), and its
 * sizeof(BM25MetaPageData) ends BELOW the newer tail. A plain assignment would drop
 * that tail into the page hole, and GenericXLogFinish's delta path ZEROES
 * [pd_lower, pd_upper) on apply -- to the live buffer and on WAL redo alike. The
 * newer binary's fields (possibly chain roots) would be destroyed by a binary the
 * contract calls safe. Raising leaves the unknown tail below pd_lower, hence
 * preserved verbatim; the cost is only that the hole is smaller than this build
 * needs it to be, which is free.
 *
 * The reader side already ignores the tail by construction (bm25_meta_read copies a
 * fixed sizeof(BM25MetaPageData) and never consults pd_lower), so a raised pd_lower
 * never makes unknown bytes visible to this build. */
void
bm25_meta_set_pd_lower(Page page)
{
    uint16 end = (uint16) (((char *) BM25PageGetMeta(page) + sizeof(BM25MetaPageData))
                           - (char *) page);

    ((PageHeader) page)->pd_lower = Max(((PageHeader) page)->pd_lower, end);
}

/* bm25_meta_fill -- populate a freshly bm25_page_init'd metapage's BM25MetaPageData
 * fields (page must already be BM25_PAGE_META). Shared by bm25_meta_init (bare/
 * sentinel identity fields, for bm25_build's MAIN_FORKNUM block 0 -- analyzer
 * config isn't resolved yet at that point) and bm25_buildempty (the SAME base
 * fields but with the analyzer identity already resolved, so its INIT_FORKNUM
 * metapage gets ONE write with the real values instead of a bare write followed
 * by a restamp -- see bm25_buildempty's header comment for why that distinction
 * matters for INIT_FORKNUM specifically). Does not touch pd_lower or WAL/durability
 * -- callers own both, since bm25_meta_init and bm25_buildempty need different
 * discipline for the latter (see bm25_meta_finish). */
static void
bm25_meta_fill(Page page, uint32 analyzer_fingerprint, BlockNumber field_config_blkno,
              uint32 field_count, uint32 feature_flags)
{
    BM25MetaPageData *meta = BM25PageGetMeta(page);

    meta->magic             = BM25_MAGIC;
    meta->format_version    = BM25_FORMAT_VERSION;
    meta->k1                = BM25_DEFAULT_K1;
    meta->b                 = BM25_DEFAULT_B;
    meta->pending_head      = InvalidBlockNumber;
    meta->pending_tail      = InvalidBlockNumber;
    meta->pending_tail_free = 0;
    meta->pending_npages    = 0;
    meta->pending_ndocs     = 0;
    meta->segcat_root       = InvalidBlockNumber;
    meta->nsegs             = 0;
    meta->ndocs             = 0;
    meta->total_len         = 0;
    meta->retired_head      = InvalidBlockNumber;
    /* D-GEN / C1: initialize next_gen to 1 so the first sealed segment's gen is
     * >= 1 and can never collide with the seg_gen 0 sentinel. PageInit zeroing
     * would otherwise leave next_gen = 0 here. */
    meta->next_gen          = 1;

    /* Analyzer identity (the three fields v4 appended). Build overwrites
     * analyzer_fingerprint + field_config_blkno + field_count
     * once the analyzer config + field-config page are written (the bare 0/Invalid/1
     * sentinels from bm25_meta_init; field_count becomes the index's key-attribute
     * count there -- bm25_build) OR the caller already knows them (bm25_buildempty).
     * PageInit zeroing would leave field_config_blkno = 0 (a VALID block number!),
     * so an explicit InvalidBlockNumber sentinel matters when the caller has none
     * yet. */
    meta->analyzer_fingerprint = analyzer_fingerprint;
    meta->field_config_blkno   = field_config_blkno;
    meta->field_count          = field_count;

    /* Format negotiation (the two fields v6 appended, which is why v6 is called the
     * negotiation baseline). A fresh index demands nothing of its reader, so its
     * floor starts at the lowest one there is, BM25_OLDEST_READABLE. That floor is
     * NOT frozen at create time: bm25_pending_append_multi raises it (to
     * BM25_MIN_READ_PENDING_SPAN) the first time this index actually writes a
     * spanning pending record, so min_read_version tracks capabilities USED, not the
     * version that stamped the page. feature_flags is 0 (bare) or the caller's
     * resolved value. */
    meta->min_read_version = BM25_OLDEST_READABLE;
    meta->feature_flags    = feature_flags;

    /* Orphan-sweep evidence (issue #300). Zero, which PageInit already left, spelled
     * out because zero is load-bearing: swept_epoch == 0 means "never swept", so the
     * first VACUUM of a new index sweeps once (bm25_reclaim_orphans). */
    meta->reserved_tail_pad = 0;
    meta->orphan_ops_begun  = 0;
    meta->orphan_ops_done   = 0;
    meta->swept_epoch       = 0;
}

/* bm25_meta_extend -- extend the given fork by exactly one page (asserted block 0)
 * and return it EXCLUSIVE-locked, uninitialized (PageIsNew). Split out of
 * bm25_meta_init so a caller that needs to know ANOTHER page's block number
 * before finishing this one's content can interleave that work between extending
 * and filling: bm25_buildempty extends the metapage here, then extends and writes
 * the field-config page (learning its block number), THEN calls bm25_meta_finish
 * on this buffer with that now-known field_config_blkno -- one write, full
 * identity, instead of a bare write followed by a restamp. Ordinary callers with
 * nothing to interleave use bm25_meta_init, which wraps extend+finish back into
 * one call. */
static Buffer
bm25_meta_extend(Relation index, ForkNumber forknum)
{
    Buffer buf = ReadBufferExtended(index, forknum, P_NEW, RBM_NORMAL, NULL);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    Assert(BufferGetBlockNumber(buf) == BM25_METAPAGE_BLKNO);   /* invariant */
    return buf;
}

/*
 * bm25_meta_finish -- PageInit + fill + WAL-log an already-extended metapage
 * buffer (from bm25_meta_extend), then unlock/release it. forknum drives the
 * WAL/durability discipline, not just the fill values:
 *
 * MAIN_FORKNUM (and any other ordinary fork): GenericXLog, as every other page
 * write in this extension uses. RelationNeedsWAL correctly gates WAL on/off per
 * the relation's own persistence -- an unlogged relation's MAIN_FORKNUM is
 * disposable (reset from the init fork on every crash restart by core), so
 * skipping WAL for it is correct, not a bug.
 *
 * INIT_FORKNUM: MUST be durable and replicated regardless of the relation's
 * persistence, which GenericXLog cannot provide here -- RelationNeedsWAL is
 * ALSO false for an unlogged relation's init fork (RelationNeedsWAL just tests
 * RelationIsPermanent, with no fork-specific exception), so GenericXLogFinish
 * would apply the image to the buffer and mark it dirty but emit no WAL record
 * at all. The buffer becomes durable only at the next checkpoint; a crash
 * before one leaves the on-disk init fork as the zero pages smgrzeroextend
 * wrote at P_NEW time, no WAL to replay it from, so core's post-crash
 * ResetUnloggedRelations copies zeros over the main fork instead of a real
 * metapage -- every subsequent query fails bm25_meta_validate's magic gate. A
 * standby never even gets that far: it receives core's log_smgrcreate (an
 * empty file) but no page contents over the WAL stream, so promotion reads
 * "could not read block 0" until REINDEX. This was a PRE-EXISTING hole in this
 * function (both bm25_build's block 0 and, historically, bm25_buildempty's
 * restamp inherited it) masked in ordinary testing by a checkpoint always
 * landing between CREATE INDEX and any crash.
 *
 * The fix matches core's ginbuildempty/brinbuildempty exactly: PageInit + fill
 * the buffer directly (no GenericXLogRegisterBuffer indirection) inside a
 * critical section, MarkBufferDirty, then log_newpage_buffer -- log_newpage is
 * an unconditional XLOG_FPI record via a direct XLogInsert, independent of
 * RelationNeedsWAL, so it is emitted (and replicated) no matter the relation's
 * persistence. That is what makes this page both crash-recoverable (WAL replay
 * reconstructs it before the next checkpoint even runs) and replicated (a
 * standby now receives it over the stream it otherwise never would, having no
 * other path to this file's content). No smgrimmedsync: FPI replay is what
 * makes this page durable, not a synchronous flush at creation time -- the
 * previous fix's smgrimmedsync call papered over the missing WAL record with a
 * synchronous disk flush that only helped if no crash intervened before it ran,
 * which is exactly the failure mode being fixed here.
 */
static void
bm25_meta_finish(Relation index, ForkNumber forknum, Buffer buf,
                 uint32 analyzer_fingerprint, BlockNumber field_config_blkno,
                 uint32 field_count, uint32 feature_flags)
{
    Page page = BufferGetPage(buf);

    if (forknum == INIT_FORKNUM)
    {
        bm25_page_init(page, BM25_PAGE_META);
        bm25_meta_fill(page, analyzer_fingerprint, field_config_blkno,
                      field_count, feature_flags);
        /* Fresh page (no prior tail), so the raise is a plain set here --
         * routed through the shared helper anyway so every metapage writer
         * reads identically. */
        bm25_meta_set_pd_lower(page);

        START_CRIT_SECTION();
        MarkBufferDirty(buf);
        log_newpage_buffer(buf, true);   /* true: standard pd_lower/pd_upper layout */
        END_CRIT_SECTION();
    }
    else
    {
        GenericXLogState *state = GenericXLogStart(index);
        Page              wpage = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);

        bm25_page_init(wpage, BM25_PAGE_META);
        bm25_meta_fill(wpage, analyzer_fingerprint, field_config_blkno,
                      field_count, feature_flags);
        bm25_meta_set_pd_lower(wpage);
        GenericXLogFinish(state);
    }

    UnlockReleaseBuffer(buf);
}

/* Create block 0 as the metapage in the given fork, stamped with whatever
 * BM25_FORMAT_VERSION this build is (bm25_meta_fill does the stamping; the
 * number is deliberately not repeated in prose), and with the bare/sentinel
 * identity fields bm25_build overwrites once the analyzer config is resolved
 * (MAIN_FORKNUM) -- see bm25_meta_extend/bm25_meta_finish for the split this
 * wraps, and bm25_meta_finish for why INIT_FORKNUM needs different WAL
 * discipline than every other page write in this extension. */
void
bm25_meta_init(Relation index, ForkNumber forknum)
{
    Buffer buf = bm25_meta_extend(index, forknum);

    bm25_meta_finish(index, forknum, buf, 0, InvalidBlockNumber, 1, 0);
}

/* One metapage block pointer, for bm25_meta_validate: see the #302.A note there. */
static void
meta_blkno_validate(const char *name, BlockNumber blk)
{
    if (blk == BM25_METAPAGE_BLKNO)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: metapage %s points at the metapage", name),
                 errdetail("Block %u is the metapage itself.", blk)));
}

/* The two-directional format gate (magic tag, then version compatibility).
 * Forward: min_read_version > BM25_FORMAT_VERSION -- an index that REQUIRES a
 * capability this build lacks is refused. Backward: format_version <
 * BM25_OLDEST_READABLE -- an index from a generation whose reader we dropped is
 * refused. Both are clean ERRORs; neither ever mis-parses.
 *
 * So what this build accepts is a RANGE, floored at BM25_OLDEST_READABLE and
 * open upward for anything that does not demand a newer reader: an index stamped
 * by an older binary keeps its own format_version and is read AS-IS, no REINDEX.
 * Do not restate that range as a single version anywhere -- the two constants
 * above are its only definition.
 *
 * HISTORICAL, and still load-bearing here: this gate arrived at v6 along with the
 * min_read_version/feature_flags tail, replacing an exact-equality check. An index
 * predating that tail reads min_read_version == 0 out of the zeroed bytes, which
 * is <= BM25_FORMAT_VERSION for every build -- "legacy, always readable" -- so
 * such indexes pass the forward half by construction rather than by special case.
 *
 * Factored out of bm25_meta_read so bm25_scan_snapshot (bm25_seg_read.c) --
 * which reads metapage fields directly under its own SHARE lock as the scan's
 * one-and-only metapage touch, instead of calling bm25_meta_read -- enforces
 * the SAME gate. Without this, a too-old/too-new index would scan silently:
 * bm25_meta_read only gates introspection/build/maintenance callers, none of
 * which run on the per-query read path. */
void
bm25_meta_validate(const BM25MetaPageData *out)
{
    if (out->magic != BM25_MAGIC)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: corrupt or uninitialized index")));
    if (out->min_read_version > BM25_FORMAT_VERSION)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: index requires extension format >= %u; this build is %u",
                        out->min_read_version, BM25_FORMAT_VERSION),
                 errhint("Upgrade the bm25_native extension binary.")));
    if (out->format_version < BM25_OLDEST_READABLE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: index is format version %u; this build reads >= %u",
                        out->format_version, BM25_OLDEST_READABLE),
                 errhint("REINDEX the index (it predates this build's supported range).")));

    /* field_count arrives by raw memcpy off the metapage and from here on is used
     * as a LOOP BOUND / array index against BM25_MAX_FIELDS-sized STACK arrays by every
     * caller that reads it off *out (bm25_merge.c's dlbf[BM25_MAX_FIELDS], this file's
     * store_pos[BM25_MAX_FIELDS], and bm25_scan_snapshot's out->field_count, which in
     * turn bounds tok_by_field/seen/doclen_by_field/df_by_field[BM25_MAX_FIELDS]
     * throughout bm25_stats.c and bm25_scan_rank.c). This is the ONE gate all of those
     * consumers reach through (bm25_meta_read / bm25_scan_snapshot both call this
     * before handing field_count to a caller), so bounding it here is what makes every
     * BM25_MAX_FIELDS-sized array indexed by it safe by construction. */
    if (out->field_count == 0 || out->field_count > BM25_MAX_FIELDS)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: metapage field_count %u is invalid (maximum is %d)",
                        out->field_count, BM25_MAX_FIELDS)));

    /* Issue #302.A. Scope (the on-disk validation contract, decision D1 of the
     * 2026-10-05 grind): a corrupt bm25 page, checksum-valid and of any origin, must
     * produce ERRCODE_INDEX_CORRUPTED rather than a crash, an out-of-bounds access, an
     * unbounded or uncancellable wait, a write into a page of another kind, or the
     * freeing of a reachable page. A silently wrong answer from in-range values is
     * caught only where the decode boundary has a cheap structural invariant (count,
     * order, kind, gen, span); otherwise it is a documented residual.
     *
     * Block 0 is never a legitimate value for any of the five block pointers: each is
     * InvalidBlockNumber (bm25_meta_fill) or a page allocated after the metapage. And
     * 0 is the one value that turns a corrupt pointer into a hang rather than an
     * error. Several callers dereference these pointers while holding this page's
     * content lock (the catalog appender and the pending appender EXCLUSIVE, the scan
     * snapshot and bm25_segcat_first_entry SHARE), and ReadBuffer + LockBuffer on
     * block 0 then takes that lock a second time. Over a held EXCLUSIVE it waits on
     * itself with interrupts held, so the backend can be neither cancelled nor
     * terminated and every writer queues behind it; a second SHARE is a double lock PG
     * 19 asserts on. Refusing 0 here, at the gate every metapage reader passes, covers
     * every such dereference at once; it replaces the hand-rolled tests the pending
     * appender and bm25_segcat_first_entry carried. Where the pointer leads after that
     * (kind, role, extent) is the dereferencing site's check. */
    meta_blkno_validate("pending_head", out->pending_head);
    meta_blkno_validate("pending_tail", out->pending_tail);
    meta_blkno_validate("segcat_root", out->segcat_root);
    meta_blkno_validate("retired_head", out->retired_head);
    meta_blkno_validate("field_config_blkno", out->field_config_blkno);
}

/* Decode-boundary probe (trust-boundary review, 2026-08): field_count is
 * fixed by the index's own column list at CREATE INDEX time (bounded well
 * inside BM25_MAX_FIELDS by the reloption validation the comment on BM25_MAX_FIELDS
 * refers to), so a regression suite has no legitimate way to build an index
 * whose metapage carries an out-of-range field_count, and no DDL path
 * reaches it either. This runs bm25_meta_validate itself (not a
 * reimplementation) over a fully-formed, otherwise-valid metapage struct
 * built on the stack, varying only field_count, so the magic/version gates
 * that run first never mask the check under test. TEST-ONLY: a pure function
 * of its scalar argument, no relation touched. */
PG_FUNCTION_INFO_V1(bm25_debug_meta_field_count_validate);
Datum
bm25_debug_meta_field_count_validate(PG_FUNCTION_ARGS)
{
    int32             field_count = PG_GETARG_INT32(0);
    BM25MetaPageData  m;

    memset(&m, 0, sizeof(m));
    m.magic           = BM25_MAGIC;
    m.format_version  = BM25_FORMAT_VERSION;
    m.min_read_version = BM25_OLDEST_READABLE;
    m.field_count     = (uint32) field_count;
    /* Zeroed block pointers would name the metapage, which the gate refuses after
     * field_count; Invalid is what an empty index carries. */
    m.pending_head = m.pending_tail = InvalidBlockNumber;
    m.segcat_root = m.retired_head = m.field_config_blkno = InvalidBlockNumber;

    bm25_meta_validate(&m);
    PG_RETURN_INT32(field_count);
}

/* Copy the metapage into *out under a SHARE lock, then validate. The copy is
 * taken before the buffer is released so the version gate (and every caller)
 * operates on stable memory, never the live buffer. */
void
bm25_meta_read(Relation index, BM25MetaPageData *out)
{
    Buffer  buf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    Page    page;

    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    memcpy(out, BM25PageGetMeta(page), sizeof(BM25MetaPageData));
    UnlockReleaseBuffer(buf);

    bm25_meta_validate(out);
}

/* Copy the metadata from an already-EXCLUSIVE-locked metabuffer WITHOUT re-locking,
 * then run the SAME full format gate bm25_meta_read runs. Used by the pending
 * append/seal paths that already hold the metabuffer exclusive across a write window
 * and must read the current directory state without releasing and re-taking the lock
 * (which would let a concurrent append wedge in). The caller owns the lock; this never
 * touches it.
 *
 * This used to be a bare magic check, with the format gate left to the CALLER: the
 * seal (bm25_seg_build.c) ran bm25_meta_validate on the next line, the pending appender
 * never did. So aminsert -- the one path that writes without reading a validated
 * metapage first -- reached the pending list without the forward/backward version gate
 * bm25_format.h describes as universal, and its Generic WAL page writes survived the
 * abort that the first validated read (bm25_pending_should_seal, AFTER the append)
 * raised. Both callers wanted the whole gate, so it is folded in here rather than
 * repeated: a third caller cannot forget it.
 *
 * Safe from an index's very first write: bm25_meta_fill stamps magic, format_version =
 * BM25_FORMAT_VERSION, min_read_version = BM25_OLDEST_READABLE, and a field_count of at
 * least bm25_meta_init's 1 sentinel, which satisfies every arm of bm25_meta_validate --
 * so CREATE INDEX (and CREATE INDEX CONCURRENTLY, whose concurrent inserts land here)
 * are unaffected. bm25_meta_validate is a pure function of the copied struct; an
 * ereport out of it unwinds cleanly as long as the caller has no WAL window open, which
 * is true of every call site (bm25_upgrade's identity path became one for exactly that
 * reason, issue #313 META-10). */
void
bm25_meta_read_locked(Buffer metabuf, BM25MetaPageData *out)
{
    memcpy(out, BM25PageGetMeta(BufferGetPage(metabuf)), sizeof(BM25MetaPageData));
    bm25_meta_validate(out);
}

/* Drained-head accessor (seal/merge): report the current pending-list head. Thin
 * wrapper over bm25_meta_read so the seal records exactly which head it drained
 * (for the post-commit page recycle and any future partial drain). */
void
bm25_meta_read_pending_head(Relation index, BlockNumber *out)
{
    BM25MetaPageData meta;

    bm25_meta_read(index, &meta);
    *out = meta.pending_head;
}

/* Overwrite the metapage with *in, WAL-logged. The delta image (flag 0) is
 * correct here -- block 0 already exists, so unlike bm25_meta_init this is not a
 * fresh-page FULL_IMAGE. pd_lower is re-asserted past the struct (see file header). */
void
bm25_meta_write(Relation index, const BM25MetaPageData *in)
{
    Buffer              buf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    Page                page;
    GenericXLogState   *state;

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    memcpy(BM25PageGetMeta(page), in, sizeof(BM25MetaPageData));
    /* MANDATORY: raise pd_lower so page-hole compression keeps the struct (and any
     * newer binary's tail above it). */
    bm25_meta_set_pd_lower(page);
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
}

/* Predicate: does this index store position data for at least one field? Reads
 * back the same per-field store_positions flags the build persists on the
 * field-config page (D12) -- NOT a fresh reloption resolution, so it stays
 * correct for bm25_upgrade (Task 5) reading an already-built index where no
 * reloption context is in scope. A pre-M4 field-config page (no trailing flag
 * array) leaves store_pos all-zero, which correctly reads as "no positions". */
static bool
bm25_index_stores_positions(Relation index, const BM25MetaPageData *meta)
{
    BM25FieldConfigHeader  fchdr;
    BM25FieldConfig        fcfg[BM25_MAX_FIELDS];
    uint8                  store_pos[BM25_MAX_FIELDS] = {0};
    uint32                 i;

    if (meta->field_config_blkno == InvalidBlockNumber)
        return false;               /* not yet written (mid-build); no positions to report */

    bm25_fieldcfg_read(index, meta->field_config_blkno, &fchdr, fcfg, store_pos);
    for (i = 0; i < meta->field_count; i++)
        if (store_pos[i])
            return true;
    return false;
}

/* Derive the informational feature_flags bitmap from an index's resolved content.
 * WAND impacts have been mandatory since v6, so BM25_FEAT_WAND_IMPACTS is set
 * unconditionally for every index this build stamps; multifield/positions come from
 * the field config. Never gates readability -- diagnostics + bm25_upgrade only.
 * Shared by bm25_build (stamps at build time) and bm25_upgrade (Task 5, re-derives
 * for an existing index) so the derivation lives in exactly one place.
 *
 * BM25_FEAT_SEGCAT_TOKENS IS DELIBERATELY NOT PRODUCED HERE, and must never be.
 * Every bit above is a property of index CONTENT, derivable by looking. Bit 3 is a
 * property of the WRITER -- it asserts that no catalog entry carries pre-ADR-0074
 * stack residue in the bytes total_tokens now occupies -- and no amount of reading an
 * existing index can establish that. Deriving it here would silently bless residue on
 * every bm25_upgrade, which is precisely the trap the bit exists to avoid; bm25_build
 * ORs it in separately because only a fresh build can honestly promise it. ADR 0088. */
uint32
bm25_derive_feature_flags(Relation index, const BM25MetaPageData *meta)
{
    uint32 flags = BM25_FEAT_WAND_IMPACTS;

    if (meta->field_count > 1)
        flags |= BM25_FEAT_MULTIFIELD;
    if (bm25_index_stores_positions(index, meta))
        flags |= BM25_FEAT_POSITIONS;
    return flags;
}

/* bm25_next_gen_check -- refuse to draw from an exhausted or corrupt counter (issue
 * #313 META-08). Called by both draw sites (bm25_next_gen below and the pending
 * chain start in bm25_pending_append_multi) on the value about to be drawn, BEFORE
 * either opens its Generic WAL window (ADR 0083).
 *
 * next_gen is a plain uint32 that advances once per segment build and once per
 * pending chain start. Drawing PG_UINT32_MAX would store 0 back -- the "skip
 * validation" sentinel (D-GEN/C1) -- and every later draw would repeat a gen already
 * stamped on pages, defeating option (d)'s no-ABA premise. So the last value ever
 * handed out is PG_UINT32_MAX - 1. Skipping 0 instead of refusing would let gens
 * repeat, which is the thing to prevent. REINDEX rebuilds the metapage with next_gen
 * = 1, so the hint is a real remedy. Practically unreachable (2^32 draws on one
 * index); the check is a guard, not a feature.
 *
 * 0 cannot come from a wrap any more, and every readable format has carried next_gen
 * initialized to 1 (bm25_meta_fill), so 0 means the metapage is corrupt. */
void
bm25_next_gen_check(Relation index, uint32 next_gen)
{
    if (next_gen == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: index \"%s\" has a segment generation counter of 0",
                        RelationGetRelationName(index)),
                 errdetail("The counter starts at 1 and is never reset in place."),
                 errhint("REINDEX the index.")));
    if (next_gen == PG_UINT32_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: segment generation counter of index \"%s\" is exhausted",
                        RelationGetRelationName(index)),
                 errdetail("Every segment build and pending chain start draws one "
                           "generation, and %u have been drawn.", PG_UINT32_MAX - 1),
                 errhint("REINDEX the index to reset the counter.")));
}

/* Read-and-bump the metapage segment-generation counter, returning the value to
 * assign to the next sealed segment's BM25SegCatEntry.gen / BM25SegmentHeader.gen
 * and to stamp on every one of its pages (option (d) reuse-safety). The bump is a
 * single Generic WAL record on the metapage (delta image -- block 0 already
 * exists). next_gen is initialized to 1 in bm25_meta_init and bm25_next_gen_check
 * refuses the draw that would wrap it, so the returned value is always >= 1 and
 * can never collide with the seg_gen 0 "skip validation" sentinel (D-GEN/C1).
 * pd_lower is re-asserted past the struct so page-hole compression keeps it.
 *
 * Not the counter's only consumer: bm25_pending_append_multi draws a pending
 * chain's epoch from the same next_gen (#291), bumping it in its own append record
 * rather than through this function. Sharing one counter is what lets a scan bound
 * both kinds of reuse with the single next_gen it captured, so segment gens are
 * unique and increasing but no longer consecutive. */
uint32
bm25_next_gen(Relation index)
{
    Buffer              buf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    Page                page;
    GenericXLogState   *state;
    BM25MetaPageData   *m;
    uint32              gen;

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    /* Checked on the locked buffer before the window opens; the registered copy below
     * holds the same bytes. */
    bm25_next_gen_check(index, BM25PageGetMeta(BufferGetPage(buf))->next_gen);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    m = BM25PageGetMeta(page);
    gen = m->next_gen;          /* in [1, PG_UINT32_MAX - 1]: bm25_next_gen_check */
    m->next_gen += 1;
    bm25_meta_set_pd_lower(page);
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
    return gen;
}

/* Orphan-op brackets (issue #300). VACUUM's orphan sweep (bm25_reclaim_orphans) is
 * gated on durable evidence that an orphan can exist, and these are how a maintenance
 * op that can leave one says so. bm25_orphan_op_begin bumps orphan_ops_begun in a
 * metapage WAL record the caller writes BEFORE the op's first page allocation (or, for
 * bm25_reclaim_retired, before its first descriptor compaction); bm25_orphan_op_end
 * bumps orphan_ops_done once the op has finished without leaving any. While the two
 * differ, the next VACUUM sweeps, and a completed sweep sets them equal.
 *
 * WHY "BEFORE THE FIRST ALLOCATION" MAKES THIS CRASH-PROOF: the begin record precedes,
 * in LSN order, every page-init record the op writes, and a page reaches disk only
 * after its WAL is flushed (WAL before data). So any orphan whose bytes survive a crash
 * has its begin replayed too. (A P_NEW extension's zero page needs no WAL to exist on
 * disk; a crash is what strands one, and the crash epoch catches that.)
 *
 * WHY COUNTERS AND NOT A BIT: an op that dies leaves begun ahead of done, and the next
 * successful op's own begin/end pair moves both by one, so the gap -- the evidence --
 * survives. A set-then-clear bit would be cleared by that next op. Every caller holds
 * the seal/merge singleton EXCLUSIVE across its whole bracket, and the sweep holds it
 * in Share, so no bracket is ever open while the sweep decides or completes: whatever
 * gap it sees belongs to ops that died.
 *
 * An op that leaves orphans BY DESIGN on its success path -- a merge or upgrade swap,
 * which orphans the old catalog chain -- calls begin and never end, so the sweep that
 * follows it reclaims that chain (bm25_segcat_publish_swap).
 *
 * Same in-place shape as bm25_next_gen: lock, register, modify the registered copy,
 * finish. Nothing in it throws once the window is open. */
static void
bm25_orphan_op_bump(Relation index, bool begin)
{
    Buffer              buf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    Page                page;
    GenericXLogState   *state;
    BM25MetaPageData   *m;

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    m = BM25PageGetMeta(page);
    if (begin)
        m->orphan_ops_begun += 1;
    else
        m->orphan_ops_done += 1;
    bm25_meta_set_pd_lower(page);
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
}

void
bm25_orphan_op_begin(Relation index)
{
    bm25_orphan_op_bump(index, true);
}

void
bm25_orphan_op_end(Relation index)
{
    bm25_orphan_op_bump(index, false);
}

/* Page allocator (stamp-and-gate): hand back an FSM page ONLY if it proves itself
 * free by carrying an on-page BM25_PAGE_DELETED mark (or is a brand-new zero page);
 * otherwise extend by one zeroed page. Returns the buffer EXCLUSIVE-locked. The
 * caller PageInit()s the returned page (under a Generic WAL window, with
 * GENERIC_XLOG_FULL_IMAGE) and WAL-logs it, so a reused page's stale contents are
 * fully overwritten/replicated and its DELETED mark is cleared -- a reused page can
 * never leak old data into a new segment.
 *
 * WHY the on-page mark and not the FSM alone: the index FSM fork is NOT crash-safe.
 * After recovery GetFreeIndexPage can return a page that was freed, reused, and is
 * now LIVE again (the nbtree safexid / bloom BLOOM_DELETED hazard). Every free path
 * therefore stamps BM25_PAGE_DELETED (bm25_page_mark_deleted) BEFORE
 * RecordFreeIndexPage, and we reject any returned page lacking the mark as
 * still-live. This is what keeps reuse correct once retired (still-referenceable)
 * ranges enter the free list -- and why the orphan/pending freers had to start
 * stamping too (else their pages would be rejected here and the relation would grow
 * unboundedly, 18_vacuum_reclaim).
 *
 * Horizon gate: a DELETED page from a RETIRED segment carries a valid retire_xid; it
 * is reusable only once that xid is removable cluster-wide
 * (GlobalVisCheckRemovableFullXid), so a pre-swap scan snapshot can never still
 * reference it. A DELETED page from a DRAINED PENDING chain carries one too, for the
 * same reason and with less margin for error -- a scan captures pending_head once and
 * walks the chain long afterwards, and on the primary the horizon is what stands
 * between that scan and a recycled page (issue #135). A hot standby without feedback
 * does not hold this horizon back; there the pending page's chain epoch in seg_gen,
 * checked by bm25_pending_walk_read, turns the reuse into a 40001 (issue #291). Every
 * other orphan carries InvalidFullTransactionId (reusable now). heaprel supplies the horizon and MUST be
 * pre-opened by the caller -- never table_open()ed under a buffer lock (D-ALLOC/M6);
 * with heaprel == NULL a valid-retire_xid page is skipped, so the path degrades to
 * extend-only -- correct, just no horizon-gated reuse there. That NULL form dates
 * from the M1 builder, which is gone: every production caller passes a heaprel now,
 * and the only NULL caller left in the tree is the TEST-ONLY
 * bm25_debug_alloc_unknown_page (bm25_fsm.c). */
/* Rejection budget for ONE bm25_page_alloc call: after this many popped-and-
 * rejected FSM candidates, stop scanning and extend the relation instead.
 *
 * Why a cap exists at all (issue #135). GetFreeIndexPage calls
 * RecordUsedIndexPage as a side effect, so within one call the pool only shrinks
 * and the loop makes strict progress -- but the rejected candidates are handed
 * straight back to the FSM after the loop, so the NEXT call pops the same ones
 * again. With N horizon-blocked pages resident in the FSM, EVERY allocation
 * therefore walked all N (a buffer read + a ConditionalLockBuffer each) before
 * reaching the extend it was always going to reach. That was tolerable while the
 * only horizon-blocked pages came from retired segments; once a drained pending
 * chain is horizon-stamped too, one seal at the default 4 MB threshold frees
 * ~512 pages at once, and k seals under a long-lived snapshot leave ~512k
 * blocked entries -- making the next seal's own appends quadratic in k.
 *
 * 32 is far above any plausible run of transient rejections (lock contention on
 * a page a concurrent ChainWriter holds, a handful of not-yet-clear retired
 * pages) and far below the cliff. Capping costs only reuse opportunity, never
 * correctness: the after-the-loop requeue preserves every rejected candidate for
 * a later call, exactly as it does when the loop runs to FSM exhaustion, and the
 * fallback -- extending by one page -- is the same thing an exhausted FSM does.
 *
 * "Consecutive" and "total" coincide here: the loop only continues at all while
 * it is rejecting, since accepting a candidate breaks out of it. */
#define BM25_ALLOC_MAX_REJECTS  32

/* Append blkno to a growable requeue list (doubling), used only by
 * bm25_page_alloc below to defer RecordFreeIndexPage until after its loop
 * exits -- see that function's header comment on *requeue for why immediate
 * requeuing was a livelock, not merely a missed optimization. */
static void
bm25_page_alloc_requeue_append(BlockNumber **arr, int *n, int *cap, BlockNumber blkno)
{
    if (*n == *cap)
    {
        *cap = *cap ? *cap * 2 : 8;
        *arr = (*arr == NULL)
            ? (BlockNumber *) palloc(sizeof(BlockNumber) * *cap)
            : (BlockNumber *) repalloc(*arr, sizeof(BlockNumber) * *cap);
    }
    (*arr)[(*n)++] = blkno;
}

Buffer
bm25_page_alloc(Relation index, Relation heaprel)
{
    /* Fix (adversarial review, 2026-08): candidates this loop rejects but
     * could still validly reuse later (see the two call sites below) are
     * collected here and only handed back to RecordFreeIndexPage AFTER the
     * loop exits, not immediately at the point of rejection.
     *
     * The immediate-RecordFreeIndexPage version of this fix (the FIRST pass)
     * traded an FSM leak for a livelock: requeuing a candidate mid-loop hands
     * it straight back to the VERY NEXT GetFreeIndexPage call in the SAME
     * invocation of this loop, which can spin without making progress:
     *   - contention (ConditionalLockBuffer failure): a block legitimately
     *     double-listed in the FSM while a seal's ChainWriter holds it
     *     EXCLUSIVE across an entire tail-page fill -- a concurrent allocator
     *     pops it, fails to lock it, requeues it, pops it again, spinning for
     *     the seal's whole page hold, where pre-first-fix it fell through to
     *     extension instead;
     *   - DELETED-but-not-yet-reusable with heaprel == NULL (the no-horizon-check
     *     form, whose one caller today is the TEST-ONLY
     *     bm25_debug_alloc_unknown_page): any
     *     valid-retire_xid DELETED page in the FSM can never be accepted OR
     *     dropped, so immediate requeuing spins on it forever -- cancellable
     *     via CHECK_FOR_INTERRUPTS, but not terminating on its own.
     * Deferring the requeue to after the loop restores strict progress: within
     * ONE call to this function, GetFreeIndexPage's pool only ever shrinks (a
     * rejected candidate stays OUT of it for the REST of this call, only
     * reappearing for a future, separate bm25_page_alloc call), so the loop
     * cannot revisit a block it already saw this call -- while still
     * preserving every rejected-but-reusable-later candidate for that future
     * call, which is the FSM-preservation goal the first pass was after. */
    BlockNumber *requeue = NULL;
    int          nrequeue = 0;
    int          requeue_cap = 0;
    int          nrejected = 0;            /* candidates popped and refused this call */
    Buffer       result = InvalidBuffer;   /* sentinel: loop found no buffer to return yet */
    int          i;

    for (;;)
    {
        BlockNumber     blkno;
        Buffer          buf;
        Page            page;
        BM25PageOpaque *op;

        /* The rejection budget below bounds this loop, but a full budget's worth of
         * buffer reads is still work; every backend that writes a page goes through
         * this allocator, so it is on the maintenance side of every
         * build/seal/merge/pending-append write path. */
        CHECK_FOR_INTERRUPTS();

        /* Rejection budget (BM25_ALLOC_MAX_REJECTS above): give up on the FSM and
         * extend rather than re-walk a large resident population of
         * horizon-blocked pages on every single allocation. */
        if (nrejected >= BM25_ALLOC_MAX_REJECTS)
            break;

        blkno = GetFreeIndexPage(index);
        if (blkno == InvalidBlockNumber)
            break;                          /* FSM exhausted: extend below */

        buf = ReadBuffer(index, blkno);
        if (!ConditionalLockBuffer(buf))    /* never block on a page in active use */
        {
            /* We don't yet know whether this page is even reusable (couldn't
             * check its DELETED mark, just lost the race for its lock), so
             * dropping it permanently would discard a plausibly-still-free
             * candidate. Collect it for the after-the-loop requeue instead of
             * requeuing now (see the livelock reasoning above). */
            bm25_page_alloc_requeue_append(&requeue, &nrequeue, &requeue_cap, blkno);
            ReleaseBuffer(buf);
            nrejected++;
            continue;
        }
        page = BufferGetPage(buf);

        if (PageIsNew(page))
        {
            result = buf;                   /* crashed-extend zero page; caller PageInit()s */
            break;
        }

        /* Issue #302.D: the DELETED test below reads the opaque at pd_special. A page
         * whose pd_special is not where bm25 puts it is corrupt, and its "flags" would
         * be the next buffer's bytes, which could pass as DELETED and hand a live page
         * out for re-init. The page is only an FSM hint here, so it is dropped like an
         * unmarked page (out of the FSM for good) rather than raised: the allocation
         * itself is not wrong, the hint is. After the PageIsNew acceptance, since a
         * zero page has pd_special 0. */
        if (!bm25_page_special_ok(page))
        {
            UnlockReleaseBuffer(buf);
            nrejected++;
            continue;
        }

        op = BM25PageGetOpaque(page);
        if ((op->flags & BM25_PAGE_DELETED) &&
            (!FullTransactionIdIsValid(op->retire_xid) ||
             (heaprel != NULL &&
              GlobalVisCheckRemovableFullXid(heaprel, op->retire_xid))))
        {
            result = buf;                   /* reusable: caller FPI-re-inits + re-stamps seg_gen */
            break;
        }

        /* Two rejection cases, matching what "stays in/out of the FSM" always
         * meant to describe:
         *   - DELETED (a real, previously-freed page, just not reusable YET --
         *     either its retire_xid horizon hasn't cleared, or heaprel == NULL):
         *     collect it for the after-the-loop requeue so a LATER
         *     bm25_page_alloc call -- once the horizon clears, or with a real
         *     heaprel in hand -- can find and reuse it. This is the entire point
         *     of horizon-gated reuse; without requeuing it eventually, a page's
         *     horizon could clear years before anything ever looks at it again.
         *   - unmarked (live data, or a stale/bogus FSM hint that was never
         *     validly free): genuinely stays OUT, permanently, not just for
         *     this call. Re-adding a live page's block would be harmless in
         *     itself (the DELETED check above is authoritative and would
         *     reject it again next time), but there is no reuse opportunity to
         *     preserve here, so don't manufacture a hint for a page that was
         *     never a valid candidate. */
        if (op->flags & BM25_PAGE_DELETED)
            bm25_page_alloc_requeue_append(&requeue, &nrequeue, &requeue_cap, blkno);
        UnlockReleaseBuffer(buf);
        nrejected++;
    }

    /* result is still the InvalidBuffer sentinel when the loop ended WITHOUT
     * finding a candidate -- either FSM exhaustion (blkno == InvalidBlockNumber)
     * or the BM25_ALLOC_MAX_REJECTS cap-break -- never via one of the two
     * found-a-buffer breaks above (ReadBuffer never returns InvalidBuffer for a
     * successful read). The cap-break is the second way in, added with the
     * pending-recycle horizon: it fires BEFORE GetFreeIndexPage, so no candidate
     * is popped-and-dropped, and the requeue below still replays every rejected
     * block. */
    if (result == InvalidBuffer)
    {
        Buffer buf = ReadBufferExtended(index, MAIN_FORKNUM, P_NEW, RBM_NORMAL, NULL);

        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        result = buf;
    }

    for (i = 0; i < nrequeue; i++)
        RecordFreeIndexPage(index, requeue[i]);
    if (requeue != NULL)
        pfree(requeue);
    return result;
}

/*
 * bm25_buildempty: called by core when building an unlogged index (operates on
 * the INIT fork, which is copied over the main fork on crash recovery for unlogged
 * relations -- core's ResetUnloggedRelations, not anything this AM controls). For
 * ordinary (logged) relations, bm25_build handles block 0 directly.
 *
 * Fix (2026-08, crash/replica-safety pass): this used to write only a bare
 * metapage (analyzer_fingerprint = 0, field_config_blkno = Invalid, field_count =
 * 1, the bm25_meta_init sentinels). After a crash on an unlogged index, core
 * resets MAIN_FORKNUM from exactly that INIT_FORKNUM image -- so every subsequent
 * query recomputed a real (non-zero) fingerprint from the index's live reloptions,
 * compared it against the stored 0, and bm25_fingerprint_gate ERRORed (require_
 * analyzer_match defaults true), on every query, forever (only REINDEX or ALTER
 * INDEX ... SET (require_analyzer_match = false) recovered it). A genuinely empty
 * index must read as EMPTY, not error.
 *
 * The fix stamps the INIT_FORKNUM metapage with the SAME analyzer identity a
 * normal build stamps on MAIN_FORKNUM (analyzer_fingerprint, a real field-config
 * page, field_count, feature_flags) rather than widening bm25_fingerprint_gate
 * with an empty-index exemption: matching bm25_build's own on-disk state keeps
 * there being exactly one code path scan-time reads ever have to trust, instead of
 * a second "is this index empty" special case with its own blast radius (e.g. a
 * post-crash multi-field index would otherwise present field_count = 1, silently
 * wrong for any per-field-targeted query reissued against it before the first
 * post-reset seal).
 *
 * bm25_resolve_fields resolves purely off the INDEX's own tuple descriptor +
 * reloptions (key_field is matched against RelationGetDescr(index), never the
 * heap's) -- heaprel is opened anyway, D-ALLOC/M6 style, to mirror bm25_build's own
 * call shape exactly rather than lean on that being true forever.
 *
 * feature_flags is derived locally from the just-resolved field_count/store_pos
 * rather than via bm25_derive_feature_flags: that helper's bm25_index_stores_
 * positions reads back the field-config page through bm25_fieldcfg_read, which
 * hardcodes MAIN_FORKNUM (ReadBuffer's default fork) -- at buildempty time
 * MAIN_FORKNUM already holds the REAL build's own field-config page at a
 * numerically unrelated block, so calling it here would read the wrong fork's
 * data. BM25_FEAT_WAND_IMPACTS/MULTIFIELD/POSITIONS are cheap to derive directly
 * from data already in hand.
 *
 * Second fix (adversarial review, same pass): the first cut of this function
 * still went through bm25_meta_init's original GenericXLog-based write for block
 * 0, then reopened it to restamp identity in a SECOND GenericXLog window.
 * GenericXLog gates WAL on RelationNeedsWAL, which is false for an UNLOGGED
 * relation's init fork exactly as much as for its main fork -- so neither write
 * emitted any WAL record, and an smgrimmedsync call was added believing it
 * covered durability. It didn't: the pages were modified in shared buffers and
 * never smgrwritten, so the sync flushed the zero pages smgrzeroextend wrote at
 * P_NEW time, not the real content, which reached disk only at the next
 * checkpoint. A crash before one left the on-disk init fork zeroed, and a
 * standby never received the content at all (no WAL record to stream). See
 * bm25_meta_finish's header comment for the full failure mode and the fix
 * (log_newpage_buffer, matching core's ginbuildempty/brinbuildempty).
 *
 * That fix also collapses the metapage to ONE write instead of bare-then-
 * restamp: bm25_meta_extend reserves block 0 (uninitialized) without writing
 * it, the field-config page is written to block 1 (learning its block number),
 * and bm25_meta_finish then fills block 0 with the now-fully-known identity in
 * its one and only write. This ordering is why the metapage must be reserved
 * (extended) before the field-config page: the metapage is contractually block
 * 0 (BM25_METAPAGE_BLKNO), and P_NEW only ever returns the next sequential
 * block, so extending field-config first would put IT at block 0 instead.
 */
void
bm25_buildempty(Relation index)
{
    Relation             heaprel;
    BM25AnalyzerConfig   cfg;
    BM25FieldConfig      fields[BM25_MAX_FIELDS];
    uint32               fpvec[BM25_MAX_FIELDS];
    uint8                store_pos[BM25_MAX_FIELDS];
    uint32               field_count;
    uint8                key_type;
    uint16               key_size;
    int                  key_attno;
    uint32               idx_fp;
    uint32               feature_flags;
    uint32               i;
    BlockNumber          fc_blkno;
    Buffer               metabuf;

    heaprel = table_open(IndexGetRelation(RelationGetRelid(index), false),
                         AccessShareLock);

    bm25_analyzer_config(index, &cfg);
    idx_fp = bm25_analyzer_fingerprint(&cfg);

    bm25_resolve_fields(index, heaprel, &cfg, fields, &field_count,
                        &key_type, &key_size, &key_attno, store_pos);
    for (i = 0; i < field_count; i++)
        fpvec[i] = idx_fp;

    table_close(heaprel, AccessShareLock);

    /* BM25_FEAT_SEGCAT_TOKENS belongs here too, and leaving it out would be a quiet
     * bug rather than a missing optimisation: an unlogged index reset by a crash
     * rebuilds from this fork, and without the bit it would spend the rest of its
     * life estimating merges from run counts even though every entry it holds was
     * written by a counter-aware binary. Derived locally with the rest for the
     * fork-hazard reason in this function's header. */
    feature_flags = BM25_FEAT_WAND_IMPACTS | BM25_FEAT_SEGCAT_TOKENS;
    if (field_count > 1)
        feature_flags |= BM25_FEAT_MULTIFIELD;
    for (i = 0; i < field_count; i++)
        if (store_pos[i])
        {
            feature_flags |= BM25_FEAT_POSITIONS;
            break;
        }

    /* Reserve block 0 (the metapage) WITHOUT writing it yet -- see this
     * function's header comment for why the reservation must come before the
     * field-config page and the write must come last. Everything above runs
     * first because bm25_meta_extend returns block 0 EXCLUSIVE-locked, and none of
     * it touches either fork: the heap open, the catalog lookups behind the
     * config and field resolution, and the fingerprint's dictionary probe (#296)
     * would otherwise all run under a buffer content lock, which the house rule
     * forbids and which would leave the probe uncancellable for its duration. */
    metabuf = bm25_meta_extend(index, INIT_FORKNUM);

    /* Second INIT_FORKNUM page: the field-config chain root, same layout a real
     * build writes on MAIN_FORKNUM (bm25_fieldcfg_write), via the INIT_FORKNUM
     * sibling that cannot reuse bm25_page_alloc (MAIN_FORKNUM-only FSM reuse).
     * Lands at block 1: block 0 was already reserved (not yet written) above.
     * #292: it carries the same key-identity stamp too, so an unlogged index reset
     * from this fork after a crash still refuses a changed key_field at INSERT. */
    fc_blkno = bm25_fieldcfg_write_init(index, fields, field_count, fpvec, store_pos,
                                        key_type, key_size, key_attno);

    /* Now write block 0's one and only image, with the full identity already
     * known -- no bare-then-restamp. */
    bm25_meta_finish(index, INIT_FORKNUM, metabuf, idx_fp, fc_blkno,
                     field_count, feature_flags);
}

/* -------------------------------------------------------------------------
 * bm25_stats(index regclass) -- SQL-callable introspection of the metapage.
 * Returns 13 columns (R1 order):
 *   ndocs, total_len, format_version, nsegs, pending_ndocs, k1, b, sealed_avgdl,
 *   analyzer_fingerprint, field_config_blkno, field_count,
 *   min_read_version, feature_flags.
 * values[0..7] are the original 8 columns (order preserved for existing tests);
 * values[8..10] are the v4 additions; values[11..12] are the v6 additions.
 * -------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(bm25_stats);
Datum
bm25_stats(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    /* Read-only, so SELECT on the indexed table rather than ownership: this
     * reports ndocs/sealed_avgdl/segment counts for an index the caller may legitimately
     * not own, but it must not be a way around table privileges. */
    Relation            index = bm25_index_open_readable(relid, AccessShareLock);
    BM25MetaPageData    meta;
    TupleDesc           tupdesc;
    Datum               values[13];
    bool                nulls[13] = {0};
    HeapTuple           tuple;

    bm25_meta_read(index, &meta);
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    values[0] = Int64GetDatum((int64) meta.ndocs);
    values[1] = Int64GetDatum((int64) meta.total_len);
    values[2] = Int32GetDatum((int32) meta.format_version);
    values[3] = Int32GetDatum((int32) meta.nsegs);
    values[4] = Int64GetDatum((int64) meta.pending_ndocs);
    /* k1/b are LIVE reloptions (spec 2026-07-16), not the metapage's write-once
     * copy: honest introspection reads the current index-wide default, which can
     * be changed after CREATE INDEX (Task 2's live ALTER). The metapage fields
     * stay untouched (vestigial write-once, per spec section 2). */
    values[5] = Float8GetDatum(index->rd_options != NULL
                               ? ((BM25Options *) index->rd_options)->k1
                               : BM25_DEFAULT_K1);
    values[6] = Float8GetDatum(index->rd_options != NULL
                               ? ((BM25Options *) index->rd_options)->b
                               : BM25_DEFAULT_B);
    /* sealed_avgdl (HDL-13, issue #154). meta.total_len and meta.ndocs are the
     * SEALED-segment totals -- every published segment's contribution, decremented
     * as VACUUM tombstones documents -- so this quotient is the average document
     * length over sealed, tombstone-adjusted documents and nothing else.
     *
     * WHAT IT IS NOT: the scorer's avgdl. The ranked path recomputes corpus stats
     * live and ADDS the pending list's un-sealed documents to both the numerator
     * and the denominator (bm25_stats.c's corpus-stat pass), so the scorer's avgdl
     * equals this column only when the pending list is empty -- i.e. immediately
     * after a seal. Any other time the two differ by exactly the un-sealed
     * documents, and a query's scores were produced by the OTHER number.
     *
     * The column reports the sealed quantity rather than the scorer's on purpose:
     * it is metapage introspection, and the metapage is what holds these totals.
     * The name is what changed in #154 -- `avgdl` read as a promise the value
     * never made. The computation is deliberately unchanged. */
    values[7] = Float8GetDatum(meta.ndocs > 0
                               ? (double) meta.total_len / (double) meta.ndocs
                               : 0.0);
    values[8]  = Int64GetDatum((int64) meta.analyzer_fingerprint);
    values[9]  = Int64GetDatum((int64) meta.field_config_blkno);
    values[10] = Int32GetDatum((int32) meta.field_count);
    values[11] = Int32GetDatum((int32) meta.min_read_version);
    values[12] = Int64GetDatum((int64) meta.feature_flags);   /* uint32 widened to bigint (avoid int4 sign) */

    tuple = heap_form_tuple(tupdesc, values, nulls);
    index_close(index, AccessShareLock);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* -------------------------------------------------------------------------
 * bm25_debug_stamp_version(index regclass, version int, min_read_version int,
 * feature_flags bigint) -- TEST-ONLY.
 * WAL-logs a forced (format_version, min_read_version, feature_flags) triple onto
 * the metapage so the negotiation suite can exercise both gate directions and the
 * feature-flags bitmap without on-disk fixtures for every generation. Mutates
 * block 0 under a Generic WAL window (delta image -- block 0 exists), re-asserting
 * pd_lower past the struct exactly like bm25_meta_write. Reads the metapage via
 * the registered page copy WITHOUT bm25_meta_read's version gate (a deliberate raw
 * poke), so it can stamp an otherwise-rejected combination and later restore a
 * valid one. Replica replay carries the poke (the GenericXLog delta includes the
 * changed bytes), which the replica/crash TAP relies on. Not part of the query
 * path; exposed only for regression testing.
 * -------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(bm25_debug_stamp_version);
Datum
bm25_debug_stamp_version(PG_FUNCTION_ARGS)
{
    Oid                 relid    = PG_GETARG_OID(0);
    int32               version  = PG_GETARG_INT32(1);
    int32               minread  = PG_GETARG_INT32(2);
    int64               flags    = PG_GETARG_INT64(3);
    Relation            index = bm25_index_open_owned(relid, RowExclusiveLock);
    Buffer              buf;
    Page                page;
    GenericXLogState   *state;
    BM25MetaPageData   *m;

    buf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    m = BM25PageGetMeta(page);
    m->format_version   = (uint32) version;     /* raw poke: no gate here */
    m->min_read_version = (uint32) minread;
    m->feature_flags    = (uint32) flags;
    bm25_meta_set_pd_lower(page);
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
    index_close(index, RowExclusiveLock);
    PG_RETURN_VOID();
}

/* -------------------------------------------------------------------------
 * bm25_debug_write_optional_region(index regclass) -- TEST-ONLY.
 * Emulates an additive on-disk change made by a format version NEWER than this
 * build's BM25_FORMAT_VERSION -- one this build has no reader for and must read
 * through anyway: writes a synthetic [uint16 len][len bytes] blob directly
 * after the metapage struct and ORs in a synthetic feature_flags bit (1u << 31,
 * never assigned to a real capability). Stated relative to BM25_FORMAT_VERSION
 * rather than by version number on purpose: this was originally written as "v7
 * vs. a v6 binary", which stopped being true the moment v7 shipped.
 *
 * pd_lower MUST be advanced past the blob, even though that looks backwards for
 * a region we want a v6 reader to "not see": GenericXLogFinish's delta path
 * treats [pd_lower, pd_upper) as an assumed-empty hole and zeroes exactly that
 * range when applying the record -- to the live buffer as well as on WAL redo
 * (src/backend/access/transam/generic_xlog.c, mirrored by generic_redo()). A
 * blob written above pd_lower without advancing it is therefore discarded
 * immediately, before this function even returns, let alone across a WAL replay
 * -- verified empirically with pageinspect's get_raw_page() (the bytes read back
 * as zero in the same session). So the region has to be OUTSIDE the hole to
 * exist at all. What still makes it invisible to THIS build's reader is that
 * bm25_meta_read() copies a hardcoded sizeof(BM25MetaPageData) bytes and never
 * consults pd_lower to size that copy (unlike the pending/field-config pages
 * elsewhere in this file, which use pd_lower as their flat-page end-of-data
 * marker) -- so advancing pd_lower here does not make the region visible to it.
 * That is the real structural-ignorance property the additive read-through
 * tests rely on, not the position of pd_lower.
 *
 * The region PERSISTS across subsequent metapage writes: they all raise pd_lower
 * via bm25_meta_set_pd_lower rather than assign it, so none can retract this
 * region back into the zeroed hole. That is precisely the v7-tail-preservation
 * property this function is standing in for, so the emulation is faithful -- and it
 * is why sql/55 case (C) no longer depends on the region write being the last
 * metapage write before the read.
 * -------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(bm25_debug_write_optional_region);
Datum
bm25_debug_write_optional_region(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index = bm25_index_open_owned(relid, RowExclusiveLock);
    Buffer              buf;
    Page                page;
    GenericXLogState   *state;
    BM25MetaPageData   *m;
    char               *region;
    uint16              len = 8;

    buf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    state = GenericXLogStart(index);
    page = GenericXLogRegisterBuffer(state, buf, 0);
    m = BM25PageGetMeta(page);

    region = (char *) m + sizeof(BM25MetaPageData);
    memcpy(region, &len, sizeof(len));
    memset(region + sizeof(len), 0xAB, len);
    m->feature_flags |= (1u << 31);   /* synthetic optional-capability bit */

    /* Advance past [len][bytes] so the region is real page content (see the
     * hole-zeroing note above), not silently-dropped hole. NOT bm25_meta_set_pd_lower:
     * this writer deliberately raises FURTHER than the struct end (that is the whole
     * point -- it emulates a v7 writer laying down a tail this build does not know), so
     * it satisfies the helper's "never below the struct end" invariant a fortiori. */
    ((PageHeader) page)->pd_lower =
        (region + sizeof(len) + len) - (char *) page;
    GenericXLogFinish(state);
    UnlockReleaseBuffer(buf);
    index_close(index, RowExclusiveLock);
    PG_RETURN_VOID();
}

/* -------------------------------------------------------------------------
 * bm25_debug_check_optional_region(index regclass) -> bool -- TEST-ONLY.
 * Reads back the region bm25_debug_write_optional_region laid down and reports
 * whether it is still INTACT (the length prefix reads 8 and all 8 payload bytes
 * are still 0xAB) AND still covered by pd_lower.
 *
 * This is the discriminating probe for the pd_lower-RAISE invariant
 * (bm25_meta_set_pd_lower): if any metapage writer ASSIGNED pd_lower back to the
 * end of this build's struct, GenericXLogFinish would zero the region as page hole
 * and this returns false. sql/55 case (G) writes the region, then drives real
 * production metapage writers (aminsert, seal) over it, then calls this. Reading
 * the raw page from SQL would otherwise need pageinspect, which the gating
 * cassert+UBSan CI job does not have (it builds PostgreSQL from source without
 * contrib), so the probe lives here.
 * -------------------------------------------------------------------------*/
PG_FUNCTION_INFO_V1(bm25_debug_check_optional_region);
Datum
bm25_debug_check_optional_region(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index = bm25_index_open_readable(relid, AccessShareLock);
    Buffer              buf;
    Page                page;
    const char         *region;
    uint16              len;
    bool                ok;
    int                 i;

    buf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    page = BufferGetPage(buf);
    region = (const char *) BM25PageGetMeta(page) + sizeof(BM25MetaPageData);
    memcpy(&len, region, sizeof(len));

    /* pd_lower must still cover [len][payload]: a retracted pd_lower would leave the
     * bytes inside the assumed-empty hole even if a delta record had not yet zeroed
     * them, so check the coverage, not just the bytes. */
    ok = (len == 8) &&
         (((PageHeader) page)->pd_lower >= (region + sizeof(len) + len) - (char *) page);
    for (i = 0; ok && i < len; i++)
        ok = ((uint8) region[sizeof(len) + i] == 0xAB);

    UnlockReleaseBuffer(buf);
    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(ok);
}
