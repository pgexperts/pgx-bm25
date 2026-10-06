/* bm25_upgrade.c -- online, no-REINDEX format upgrade.
 *
 * Role in the system: rewrites an index from its on-disk format generation to
 * this build's BM25_FORMAT_VERSION without a heap rescan, riding the merge
 * atomic-swap machinery. A per-(from,to) transform registry drives any segment
 * rewrite; today the registry is empty, so every accepted gap (legacy or
 * >= BM25_OLDEST_READABLE) up to BM25_FORMAT_VERSION is a pure metapage
 * re-stamp. Runs on the primary only; standbys converge via WAL replay.
 *
 * Two paths, chosen by the registry lookup below: a MATCHED transform re-emits
 * every segment through the merge accumulate + atomic-swap machinery (the re-stamp
 * folded into the swap's own WAL record); an unmatched gap is the IDENTITY path -- a
 * standalone metadata-only re-stamp. The registry ships with no real entries, so
 * every gap this build accepts is additive/identity today; the test-only synthetic
 * flag forces a match so the rewrite path is exercised, not hypothetical.
 */
#include "postgres.h"

#include "bm25.h"               /* umbrella: format structs, generic_xlog, genam, bufmgr, miscadmin */
#include "utils/builtins.h"     /* cstring_to_text */

/* What this registry ACTUALLY is, stated honestly: a set of on-disk generations
 * whose gap to the current format is BREAKING, i.e. needs a full segment re-emit
 * rather than a metadata-only re-stamp. It is a per-from_gen flag and nothing
 * more.
 *
 * It used to carry a `to_gen` field and a `rewrite_segment` function pointer.
 * Both were dead: to_gen was never read anywhere, and rewrite_segment was only
 * ever NULL-tested as a sentinel, never invoked -- the matched path calls
 * bm25_merge_rewrite_all, whose re-emit is semantically identity. A registered
 * {5, 6, rewrite_v5_to_v6} would therefore have selected the rewrite path and
 * then silently NOT run rewrite_v5_to_v6, which is worse than having no hook at
 * all. Removed rather than wired up: nothing needs a per-segment transform yet,
 * and a hook that lies about being called is a trap for whoever adds the first
 * real one. Wire it deliberately, with a caller, when there is something to run.
 *
 * READ THIS BEFORE REGISTERING THE FIRST REAL ENTRY: a registered entry means the
 * gap is BREAKING, and a breaking change by definition RAISES the floor -- but
 * both upgrade paths below stamp min_read_version = Max(current,
 * BM25_OLDEST_READABLE), because with an all-additive registry no gap in the
 * registry needs a floor above that. (The Max is about a floor a WRITER raised,
 * not about transforms; it does not stand in for carrying a transform's own
 * floor.) A real entry must therefore ALSO carry its resulting min_read_version
 * (add the field here and thread it into BM25FormatRestamp) and bump
 * BM25_OLDEST_READABLE, or the rewrite will re-emit new-format segments under a
 * floor claiming an old binary can still read them -- exactly the silent mis-read
 * the v6 gate exists to prevent. AND it must arrange for the per-segment
 * transform to actually run, which today nothing does. */
typedef struct BM25UpgradeTransform
{
    uint32  from_gen;
    bool    needs_segment_rewrite;
} BM25UpgradeTransform;

/* Empty of real entries today: every gap in [OLDEST_READABLE, FORMAT_VERSION] is
 * additive (identity). The {0,false} sentinel keeps the array (and the lookup
 * below) well-formed with zero live entries. */
static const BM25UpgradeTransform bm25_upgrade_transforms[] = { { 0, false } };

/* Build-time trap enforcing the paragraph above. Wiring the registry dispatch traded
 * a loud runtime ERROR on a matched transform for a silent wrong floor, so the
 * reminder cannot live in a comment alone: adding an entry breaks the build here and
 * forces the author to deal with min_read_version first. Bump the expected length as
 * part of that work, once the floor is actually carried AND a per-segment transform
 * is actually dispatched. */
StaticAssertDecl(lengthof(bm25_upgrade_transforms) == 1,
                 "adding a real transform: carry min_read_version into BM25FormatRestamp, "
                 "raise the stamped floor, and dispatch the per-segment transform "
                 "(nothing calls one today) before registering an entry");

/* TEST-ONLY backend-local flag: forces the registry lookup below to report a match,
 * so bm25_upgrade takes the segment-rewrite path exactly as a real registered
 * transform would. It stands in for a real per-(from,to) entry so t/014 and sql/55
 * case (F) can exercise the rewrite+swap+restamp path end to end with the registry
 * still empty of real transforms. Never persisted; never consulted by the query
 * path. */
static bool bm25_debug_synthetic_transform = false;

/* Registry lookup: does some entry transform THIS index's on-disk generation? A
 * match means the gap is breaking (segments must be re-emitted); no match means the
 * gap is additive and the upgrade is a metadata-only re-stamp. A false
 * needs_segment_rewrite marks the sentinel entry, never a match. */
static bool
bm25_upgrade_transform_matches(uint32 from_version)
{
    uint32 i;

    if (bm25_debug_synthetic_transform)
        return true;
    for (i = 0; i < lengthof(bm25_upgrade_transforms); i++)
        if (bm25_upgrade_transforms[i].needs_segment_rewrite &&
            bm25_upgrade_transforms[i].from_gen == from_version)
            return true;
    return false;
}

PG_FUNCTION_INFO_V1(bm25_debug_enable_synthetic_transform);
Datum
bm25_debug_enable_synthetic_transform(PG_FUNCTION_ARGS)
{
    bm25_debug_synthetic_transform = PG_GETARG_BOOL(0);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bm25_upgrade_sql);
Datum
bm25_upgrade_sql(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index;
    BM25MetaPageData    meta;       /* decision snapshot: which upgrade path to take */
    uint32              from_version;
    bool                already_current;
    uint32              derived_feature_flags;
    char                msg[128];

    /* RowExclusiveLock, not ShareUpdateExclusiveLock: matches bm25_merge_sql and
     * bm25_seal_sql (the two existing maintenance entry points this rides
     * alongside) exactly. It does not self-conflict, so concurrent inserts and
     * concurrent bm25_upgrade/bm25_merge/bm25_seal calls all proceed; they're
     * serialized against each other by the finer-grained locks each takes on
     * the metapage buffer / seal singleton, not by the relation lock. */
    /* Ownership + AM identity before any page is touched. This was the widest of
     * the three gaps: with bm25_debug_enable_synthetic_transform(true) first, an
     * unprivileged role could force the rewrite path, re-emitting every live
     * segment of an index it has no rights to and leaving the old pages retired
     * until a later merge or VACUUM reclaims them -- loopable as cheap WAL
     * amplification plus bloat. Without the toggle it still re-stamped
     * format_version/min_read_version on someone else's index.
     *
     * The gate also refuses during recovery (25006, core's wording). This function
     * used to carry its own standby check with SQLSTATE 55000; it went when META-07
     * (#307) moved the check into the gate, where every write entry point gets it. */
    index = bm25_index_open_owned(relid, RowExclusiveLock);

    /* What the stamped floor DOES and DOES NOT mean, since the paragraph above is
     * easy to over-read: min_read_version tracks the METAPAGE generation, not the
     * shape of the segments the running binary happens to write. bm25_upgrade is
     * not the sole gatekeeper -- the seal on the next line publishes segments built
     * by THIS binary into an index whose metapage still advertises the OLD floor,
     * and so does an ordinary insert. That is safe only while the generations in
     * flight are shape-compatible.
     *
     * Where the segment-shape invariant is actually enforced today: at the WRITER,
     * lazily, per feature. bm25_pending_append_multi (the writer aminsert calls;
     * bm25_pending_append is only its field-0 wrapper) raises the floor to
     * BM25_MIN_READ_PENDING_SPAN in the same WAL record as the first continuation
     * record it writes, because that record is the thing an older reader would
     * mis-group. A future breaking segment shape needs the same treatment at its
     * own writer; it does not get it for free from here. */
    bm25_seal_index(index);   /* drain pending into sealed segments first; this
                                * also enforces the version gate via its own
                                * internal bm25_meta_read of the pending head. */

    /* Decision snapshot. bm25_meta_read takes the metapage share lock, enforces
     * the two-directional version gate, and returns a struct copy. format_version
     * only ever moves forward, and only by a bm25_upgrade whose own atomic window
     * (the identity re-stamp below, or the swap on the rewrite path) serializes the
     * change, so reading it here to pick the path is sound. */
    bm25_meta_read(index, &meta);
    from_version    = meta.format_version;
    already_current = (from_version >= BM25_FORMAT_VERSION);

    /* Fix (2026-08, crash/replica-safety pass): derive feature_flags here, once,
     * on this pre-lock decision snapshot, for BOTH non-already-current paths
     * below. feature_flags is a structural, build-time-only field (like
     * field_config_blkno/field_count) that a concurrent aminsert never touches,
     * so deriving it from this snapshot is sound even though the identity path
     * below re-reads meta fresh under its own lock for the OTHER (pending-list)
     * fields. bm25_derive_feature_flags reaches ANOTHER buffer
     * (bm25_index_stores_positions -> bm25_fieldcfg_read) -- computing it here
     * keeps that second buffer acquisition outside both the identity path's
     * open GenericXLog window and its EXCLUSIVE metapage lock, matching what
     * the rewrite path already did (it derived pre-lock; only the identity
     * path derived it inside the window). A legacy index carries feature_flags
     * == 0; a real v6+ index already carries its stamped value, so this is a
     * no-op read in that case, not a re-derivation.
     *
     * CONSTRAINT FOR THE FIRST NON-IDENTITY TRANSFORM: this derivation reads the
     * PRE-rewrite index state, and the rewrite path below stamps the result into
     * the swap that publishes the POST-rewrite segments. Correct today only
     * because every rewrite is semantically identity (the registry is empty and
     * the synthetic transform replays each doc unchanged), so no feature can
     * appear or disappear across it. A transform whose re-emit CHANGES which
     * features are present must derive its flags from what the rewrite produced
     * -- after bm25_merge_rewrite_all's accumulate, before its swap -- not from
     * here. Unlike min_read_version below, a Max() does not rescue this: flags
     * can legitimately be CLEARED by a rewrite that drops a feature. */
    derived_feature_flags = meta.feature_flags;
    if (derived_feature_flags == 0)
        derived_feature_flags = bm25_derive_feature_flags(index, &meta);

    if (already_current)
    {
        ereport(NOTICE, (errmsg("bm25: index already at format version %u", from_version)));
        snprintf(msg, sizeof(msg), "already current (v%u)", BM25_FORMAT_VERSION);
    }
    else if (bm25_upgrade_transform_matches(from_version))
    {
        /* SEGMENT-REWRITE PATH. A matched transform -- a real per-(from,to) entry in
         * bm25_upgrade_transforms[], or the test-only synthetic stand-in that forces
         * a match -- re-emits EVERY segment through the merge
         * accumulate + atomic-swap machinery. The version re-stamp is folded into
         * the swap's own catalog-flip WAL record (the BM25FormatRestamp handed to
         * bm25_segcat_publish_swap), so the new-format segments and the
         * new metapage version commit as ONE record. There is deliberately NO
         * separate metapage write here: a crash can never split them.
         *
         * The re-stamp values are computed on the decision snapshot (every gap
         * this build accepts is additive, so the upgrade demands no floor above
         * BM25_OLDEST_READABLE; a legacy zeroed feature_flags is
         * derived exactly as the identity path derives it). They are pure values
         * carried into the swap's throw-free WAL window.
         *
         * min_read_version is the one field the swap does NOT assign: it is a
         * monotonic floor, and this value is a pre-lock snapshot, so the swap
         * takes Max(on-disk, this) against the registered metapage copy inside
         * its own window. Read rs.min_read_version as "the floor this upgrade
         * requires AT LEAST", not "the floor to write". */
        BM25FormatRestamp   rs;
        int                 nrewritten;

        rs.format_version   = BM25_FORMAT_VERSION;
        rs.min_read_version = BM25_OLDEST_READABLE;     /* a minimum; the swap raises to it */
        rs.feature_flags    = derived_feature_flags;   /* pre-lock; see above */

        nrewritten = bm25_merge_rewrite_all(index, &rs);
        snprintf(msg, sizeof(msg), "upgraded %d segments: v%u -> v%u",
                 nrewritten, from_version, BM25_FORMAT_VERSION);
    }
    else
    {
        /* IDENTITY / METADATA-ONLY PATH. The registry matched no transform for this
         * gap, i.e. it is additive: no segment needs rewriting, so
         * re-stamp version/floor/flags as ONE Generic WAL record: the
         * metapage buffer's EXCLUSIVE content lock is held continuously from the
         * snapshot read through the write. Two separate calls -- bm25_meta_read()
         * then, later, bm25_meta_write() -- would drop the lock in between, and
         * RowExclusiveLock does not close that gap: a concurrent aminsert
         * (bm25_pending_append_multi) mutates this SAME metapage buffer directly
         * under its own LockBuffer(EXCLUSIVE) content lock, so a stale snapshot
         * written back after such an insert would silently revert its pending-list
         * linkage (pending_head/tail/npages/ndocs) -- an orphaned, unreachable
         * document. This shape (lock once, read the registered page copy, modify in
         * place, finish, unlock) is the same one bm25_next_gen and
         * bm25_debug_stamp_version already use in bm25_meta.c for the identical
         * reason; it is not a new protocol. */
        Buffer              metabuf;
        Page                metapage;
        GenericXLogState   *state;
        BM25MetaPageData   *m;

        metabuf = ReadBuffer(index, BM25_METAPAGE_BLKNO);
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

        /* Copy and re-validate the freshest snapshot BEFORE the window opens (issue
         * #313 META-10, ADR 0083: no throwable call inside a Generic WAL window).
         * bm25_meta_validate is an ereport site. The registered copy below is taken
         * under the same EXCLUSIVE lock, so it holds exactly the bytes validated
         * here. */
        bm25_meta_read_locked(metabuf, &meta);

        state = GenericXLogStart(index);
        metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
        m = BM25PageGetMeta(metapage);

        /* No segment moved: re-stamp version, floor, and flags in this one WAL
         * record. Every gap this build accepts is additive, so the UPGRADE itself
         * demands no floor above BM25_OLDEST_READABLE -- but the floor the index
         * already carries may legitimately be higher, so the stamp RAISES to that
         * minimum rather than assigning it (see below). feature_flags uses the
         * value derived pre-lock above (not a fresh bm25_derive_feature_flags
         * call here, which would reach the field-config buffer while metabuf is
         * EXCLUSIVE-locked and this GenericXLog window is open -- see the
         * derivation's own comment). Gated on THIS fresh read being legacy
         * (0), not the pre-lock snapshot's: if a concurrent bm25_upgrade already
         * stamped real flags between the two reads, derived_feature_flags is
         * byte-identical to what it stamped (feature_flags is a deterministic
         * function of structural state neither read's window can change), so
         * either branch of this condition lands on the same value. */
        meta.format_version   = BM25_FORMAT_VERSION;
        /* RAISE the floor, never assign it. min_read_version is documented as
         * monotonic in two places (src/bm25_format.h:57-63 and the raise site at
         * src/bm25_pending.c) because a reader can hold a snapshot across the
         * change. Critically, the floor is raised by writers that have nothing to
         * do with an upgrade: bm25_pending_append_multi stamps
         * BM25_MIN_READ_PENDING_SPAN the first time a document spans pending
         * pages, and leaves format_version alone. A plain assignment here
         * therefore rewrote a legitimately-raised floor back DOWN -- v6 body,
         * floor lifted to 7 by such an insert, then bm25_upgrade advertising the
         * index as readable by a binary with no reader for the continuation
         * records it still contains. `meta` is the copy re-read under this
         * EXCLUSIVE lock, so Max() is taken against the current on-disk floor,
         * not the stale pre-lock decision snapshot. */
        meta.min_read_version = Max(meta.min_read_version, BM25_OLDEST_READABLE);
        if (meta.feature_flags == 0)
            meta.feature_flags = derived_feature_flags;

        memcpy(m, &meta, sizeof(BM25MetaPageData));
        bm25_meta_set_pd_lower(metapage);
        GenericXLogFinish(state);
        UnlockReleaseBuffer(metabuf);

        snprintf(msg, sizeof(msg), "upgraded 0 segments: v%u -> v%u",
                 from_version, BM25_FORMAT_VERSION);
    }

    index_close(index, RowExclusiveLock);
    PG_RETURN_TEXT_P(cstring_to_text(msg));
}
