/* bm25_format.h -- bm25_native on-disk format. The CURRENT version is whatever
 * BM25_FORMAT_VERSION says below; this comment deliberately does not repeat that
 * number in prose, because a prose copy of it rots at the next bump.
 *
 * Role in the system: the single source of truth for every byte written to an
 * 8 KB index page. All .c files include this (transitively via bm25.h) so no
 * struct is ever redefined.
 *
 * Version history -- each entry names the version that INTRODUCED the change.
 * Older stamps survive on disk, so a reader must tolerate every version from
 * BM25_OLDEST_READABLE up, not merely the current one:
 *  - v3: hard break from v2 (REINDEX required).
 *  - v4: second hard break (REINDEX required).
 *  - v5: third hard break (REINDEX required) -- drops the reserved
 *    BM25BlockHeader.max_impact float4 and replaces it with a trailing per-field
 *    impact table (BM25BlockImpact) for block-max WAND.
 *  - v6: the format-negotiation baseline, and NOT a hard break -- it only appends
 *    min_read_version + feature_flags to the metapage tail, replacing the former
 *    exact-equality version check with a two-directional floor gate (roadmap #4
 *    Task 1). A v5 index reads those fields as 0 (legacy, always readable) and
 *    needs no REINDEX.
 *  - v7: not a hard break either -- a pending document may span pages as same-TID
 *    continuation records (BM25_PENDING_DOC_CONT), and only an index that actually
 *    writes one raises its floor (BM25_MIN_READ_PENDING_SPAN, stamped lazily).
 *  - v8: not a hard break either -- a pending document record carries its per-field
 *    doclen explicitly, as a uint32 array between the doc header and the term entries
 *    (BM25_PENDING_DOC_FIELDLENS). Segments are untouched: NORMS has always stored
 *    doclen explicitly, so nothing sealed moves. Only an index that actually writes
 *    such a record raises its floor (BM25_MIN_READ_PENDING_FIELDLENS, stamped
 *    lazily), and the flag makes each record self-describing, so a pending list may
 *    legitimately hold a mix of v7 and v8 records and every reader gets both right.
 *
 * CAVEAT on "v8 reads v5", which is true of SEGMENTS but not of every pending
 * record. v7 INSERTED `flags` at the offset a v5/v6 record used for
 * key_type/key_pad0/key_size, so a pre-v7 KEYED pending record presents that key
 * metadata as flag bits: key_type 1 already read as BM25_PENDING_DOC_CONT under v7,
 * and key_type 2 now reads as BM25_PENDING_DOC_FIELDLENS, which makes
 * bm25_pending_iter_next raise ERRCODE_INDEX_CORRUPTED on the missing array. The
 * exposure is nil in practice -- no pre-v7 binary was ever released, and a pending
 * list is drained by the next seal rather than carried across an upgrade -- and
 * failing loudly beats v7's silent mis-grouping. Recorded because the floor's
 * one-line summary reads as a stronger promise than the pending path can keep.
 * See docs/adr/0009 (the floor gate and v6 baseline), 0038 (v7 continuation
 * records) and 0086 (v8 per-field doclen) for the reasoning behind each step.
 * bm25_meta_validate is what enforces the gate; it was factored out of
 * bm25_meta_read so that bm25_scan_snapshot -- which reads the metapage directly
 * as the query path's one metapage touch -- applies the identical check.
 *
 * Layout invariants:
 *  - Block 0 is always the metapage (BM25_METAPAGE_BLKNO). Its BM25MetaPageData
 *    lives at PageGetContents(); after any mutation pd_lower MUST be advanced
 *    past the struct or Generic WAL page-hole compression silently drops it.
 *  - Every page carries a BM25PageOpaque in its special space.
 *  - A sealed segment's header page and its DICT/POST/NORMS/DOCMAP/KEYMAP/POS
 *    pages are immutable once published; only the LIVE bitmap page and the
 *    segment's BM25SegCatEntry are mutated in place, by the tombstone path
 *    (bm25_livedocs_clear, which clears one bit and decrements the catalog
 *    entry's live_ndocs/total_len under one Generic WAL record).
 *  - A sealed segment's pages are reachable only from a live BM25SegCatEntry
 *    (the two-phase commit invariant).
 */
#ifndef BM25_FORMAT_H
#define BM25_FORMAT_H

#include "storage/block.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"   /* ItemPointerData -- used by the docid->TID map and the
                                * pending-record structs added in Tasks 5/6 */
#include "access/transam.h"    /* FullTransactionId for BM25PageOpaque.retire_xid */

/* ---- constants ---- */
#define BM25_FORMAT_VERSION     8   /* v8: a pending document record stores its per-field doclen explicitly */
#define BM25_OLDEST_READABLE    5   /* oldest format this build still has a reader for; v8 reads v5 */

/* The floor a pending CONTINUATION record demands of a reader (v7). Stamped LAZILY --
 * only into the metapage record that lands the first multi-part document, via
 * Max(current, this) -- so an index that never inserts an oversized document keeps
 * whatever floor it had and stays readable by a v6 binary. Monotonic: it is not lowered
 * when a seal drains those records, because a reader can hold a snapshot across the
 * seal. See docs/adr/0038. */
#define BM25_MIN_READ_PENDING_SPAN  7

/* The floor a pending record carrying an explicit per-field doclen array demands of a
 * reader (v8). Same lazy, monotonic Max(current, this) discipline as the span floor
 * above, stamped in the SAME Generic WAL record as the first such pending record, and
 * for the same reason: a v7 reader strides from the doc header straight to the first
 * term entry, so it would read the doclen array's bytes as a BM25PendingTermEntry and
 * desync mid-page. An index that is built and never written to therefore keeps its old
 * floor and stays readable by a v7 binary -- a plain CREATE INDEX writes no pending
 * records at all. (CREATE INDEX CONCURRENTLY is not "never written to": its validate
 * phase drives aminsert for the rows the build missed, so a CIC over a table taking
 * concurrent DML raises the floor. The same was already true of the span floor above.) */
#define BM25_MIN_READ_PENDING_FIELDLENS  8
#define BM25_METAPAGE_BLKNO     0
#define BM25_MAGIC              0x424D3235      /* "BM25" */
#define BM25_POSTINGS_PER_BLOCK 128             /* docs per posting block */
#define BM25_VARBYTE_MAX_BYTES  5               /* max LEB128 width of a uint32 (codec bound) */

#define BM25_DEFAULT_K1   1.2
#define BM25_DEFAULT_B    0.75

/* ---- v4 field/analyzer layout constants ---- */
#define BM25_MAX_FIELDS         32             /* reloption-validated at CREATE INDEX */
#define BM25_FIELD_NAME_LEN     64             /* fixed-width field_name storage */
#define BM25_STEMMER_NAME_LEN   32             /* fixed-width stemmer_name storage */

/* Query field-scope sentinel (C4: the RHS "field:term" micro-parse). A bare
 * (no-colon) RHS scopes to every field (BM25F with per-field boosts); a
 * "field:term" RHS resolves to a dense field_id in [0, BM25_MAX_FIELDS). Stored as
 * a signed int32 so the -1 sentinel is distinct from field_id 0. */
#define BM25_FIELD_ALL          (-1)

/* Max terms in a phrase / proximity query (M4 C-MATCH). Bounds the matcher's
 * per-term cursor arrays; a longer quoted phrase is rejected at parse time. A legal
 * brief-search phrase is a handful of words, so this cap is generous. */
#define BM25_PHRASE_MAX_TERMS   64
/* Max phrase proximity slop (the n in "a b"~n / ~>n). A parse-time cap so an absurd
 * value cannot overflow the accumulator; far larger than any sensible window. */
#define BM25_MAX_PHRASE_SLOP    100000

/* Tokenizer-type tags (BM25FieldConfig.tokenizer_type / fingerprint input). */
#define BM25_TOKENIZER_STANDARD 0              /* the only M3 tokenizer */

/* Stopword-set tags (feeds stopword_set_hash in the fingerprint). */
#define BM25_STOPWORDS_NONE     0
#define BM25_STOPWORDS_DEFAULT  1              /* Snowball language default stoplist */

/* ---- M5 key_field type tags (KEYMAP page {key_type, key_size} header, section 3.7) ---- */
#define BM25_KEY_NONE     0    /* no key_field: keymap_root Invalid, score/return by ctid */
#define BM25_KEY_INT4     1    /* 4 bytes  */
#define BM25_KEY_INT8     2    /* 8 bytes  */
#define BM25_KEY_UUID     3    /* 16 bytes */
#define BM25_KEY_TEXT     4    /* <= 16 bytes, right-NUL-padded/validated at build */
#define BM25_KEY_MAX_SIZE 16   /* widest fixed-width key */

/* page kinds (BM25PageOpaque.flags) */
#define BM25_PAGE_META      (1 << 0)
#define BM25_PAGE_PENDING   (1 << 1)   /* pending-list page */
#define BM25_PAGE_SEGCAT    (1 << 2)   /* segment-catalog page */
#define BM25_PAGE_DICT      (1 << 3)   /* segment dictionary page */
#define BM25_PAGE_POST      (1 << 4)   /* segment postings page */
#define BM25_PAGE_NORMS     (1 << 5)   /* segment norms page */
#define BM25_PAGE_LIVE      (1 << 6)   /* segment live-docs bitmap page */
#define BM25_PAGE_DOCMAP    (1 << 7)   /* segment docid->TID map page */
#define BM25_PAGE_RETIRED   (1 << 8)   /* retired-free-list page */
#define BM25_PAGE_DELETED   (1 << 9)   /* tombstoned page awaiting reclaim */
/*
 * New in v4: all three carry the existing BM25PageOpaque (incl. seg_gen).
 * FIELDCFG is a singleton page at seg_gen = 0 (not segment-scoped).
 * KEYMAP and POS were RESERVED when v4 landed; both are LIVE page kinds now and
 * are segment-scoped, so their seg_gen carries the segment generation. KEYMAP is
 * written by bm25_keymap_write (docid->key; only when the index has key_field and
 * the segment is non-empty); POS by the segment builder's position chain (only
 * when at least one field stores positions).
 */
#define BM25_PAGE_FIELDCFG  (1 << 10)  /* per-index field-config page (seg_gen = 0) */
#define BM25_PAGE_KEYMAP    (1 << 11)  /* per-segment docid->key map (M5) */
#define BM25_PAGE_POS       (1 << 12)  /* per-segment position chain  (M4) */

/* The OR of every page kind THIS build knows. MAINTENANCE: adding a BM25_PAGE_*
 * flag above REQUIRES adding it here -- the mask is what makes the additive
 * contract (docs/adr/0009) true, not merely documented.
 *
 * WHY it exists: "a new page type old binaries never follow a link to" is an
 * ADDITIVE change per ADR 0009, so an older binary ACCEPTS an index containing
 * one (the floor gate does not rise). But bm25_reclaim_orphans is a CLOSED-WORLD
 * mark-and-sweep off a compile-time root set: a page kind this build cannot reach
 * is, to this build, unreachable -- i.e. an orphan -- and the sweep would stamp it
 * DELETED with an invalid retire_xid, which bm25_page_alloc reuses IMMEDIATELY
 * (no horizon wait). An older binary vacuuming a newer index would thus hand the
 * newer binary's live chain out to the next seal: silent corruption. The sweep
 * therefore refuses to free any page carrying a bit outside this mask, which
 * fails SAFE (leaks a page; a later binary that knows the kind reclaims it). */
#define BM25_PAGE_ALL_KNOWN (BM25_PAGE_META | BM25_PAGE_PENDING | BM25_PAGE_SEGCAT | \
                             BM25_PAGE_DICT | BM25_PAGE_POST | BM25_PAGE_NORMS | \
                             BM25_PAGE_LIVE | BM25_PAGE_DOCMAP | BM25_PAGE_RETIRED | \
                             BM25_PAGE_DELETED | BM25_PAGE_FIELDCFG | BM25_PAGE_KEYMAP | \
                             BM25_PAGE_POS)

/* feature_flags bits. NEVER CONSULTED BY THE VERSION GATE -- bm25_meta_validate
 * decides readability from format_version and min_read_version alone, and that is
 * still true. Bits 0-2 are informational, derived from index content at
 * build/upgrade; 0 on a legacy v5 index.
 *
 * Bit 3 is different in kind and the distinction is deliberate: it is a TRUST
 * SIGNAL for an optional on-disk field, read by the merge estimator to decide
 * whether a byte range means anything. It is NOT derived -- deriving it would mean
 * proving that every catalog entry's padding was written by a counter-aware binary,
 * which is exactly what cannot be proven from content (ADR 0074's residue). It is
 * stamped at BUILD only, and bm25_upgrade neither confers nor clears it. An
 * existing index therefore gains it by REINDEX, which is the same event analyzer
 * revision 5 already forces on the dictionaries this matters for. See ADR 0088. */
#define BM25_FEAT_MULTIFIELD    (1u << 0)   /* field_count > 1 */
#define BM25_FEAT_POSITIONS     (1u << 1)   /* any field stores positions */
#define BM25_FEAT_WAND_IMPACTS  (1u << 2)   /* per-block impact tables present (always set at v6) */
#define BM25_FEAT_SEGCAT_TOKENS (1u << 3)   /* segcat entries carry a trustworthy total_tokens */

/* ---- per-page opaque (every bm25 page) ---- */
typedef struct BM25PageOpaque
{
    uint16              flags;          /* BM25_PAGE_* */
    uint16              unused;
    BlockNumber         nextblk;        /* intra-chain next page, or InvalidBlockNumber */
    FullTransactionId   retire_xid;     /* set when BM25_PAGE_DELETED; else InvalidFullTransactionId */
    uint32              seg_gen;        /* option (d) reuse-safety: the segment generation
                                         * (BM25SegCatEntry.gen) stamped on every segment page;
                                         * 0 == non-segment / unstamped. The reader validates it
                                         * after locking each followed page (mismatch => the page
                                         * was reclaimed-and-reused, abort cleanly). */
} BM25PageOpaque;

/* Issue #313 SEGREAD-08: sizeof(BM25PageOpaque) sets pd_special on every bm25 page, so a
 * member change silently repartitions every existing page. 24 bytes: retire_xid aligns
 * to 8, which leaves a 4-byte tail pad after seg_gen. */
StaticAssertDecl(sizeof(BM25PageOpaque) == 24 &&
                 offsetof(BM25PageOpaque, retire_xid) == 8 &&
                 offsetof(BM25PageOpaque, seg_gen) == 16,
                 "BM25PageOpaque changed layout; pd_special moved on every page");

/* ---- metapage (block 0) -- the LSM directory ---- */
typedef struct BM25MetaPageData
{
    uint32          magic;
    uint32          format_version;     /* The BM25_FORMAT_VERSION this index was BUILT under,
                                         * or last moved to by bm25_upgrade -- NOT necessarily
                                         * this build's: an index stamped by an older binary
                                         * legitimately keeps its older value and is read
                                         * as-is, which is the point of the gate.
                                         * bm25_meta_fill stamps it at create; bm25_upgrade
                                         * re-stamps it (on the segment-rewrite path the new
                                         * value rides the merge swap's own WAL record, via
                                         * BM25FormatRestamp). bm25_meta_validate rejects a
                                         * value below BM25_OLDEST_READABLE -- the BACKWARD
                                         * half of the gate; min_read_version below is the
                                         * forward half. */
    float8          k1;                 /* default 1.2 */
    float8          b;                  /* default 0.75 */

    /* pending list (GinMetaPageData analogue) */
    BlockNumber     pending_head;       /* first pending page, or Invalid */
    BlockNumber     pending_tail;       /* last pending page, or Invalid */
    uint32          pending_tail_free;  /* cached free bytes on tail */
    uint32          pending_npages;
    uint64          pending_ndocs;

    /* segment catalog: a chain of BM25_PAGE_SEGCAT pages of BM25SegCatEntry */
    BlockNumber     segcat_root;        /* root pointer to the current catalog chain
                                         * head; flipped atomically by the merge swap
                                         * (indirection), or Invalid */
    uint32          nsegs;              /* live segment count */

    /* Global stats cache: SEGMENT-ONLY live totals (tombstone-adjusted). The live
     * PENDING contribution is never stored here -- the scan-time stats reader
     * (bm25_scan_corpus_stats) adds it to its snapshot copy before deriving avgdl.
     * Writers: the seal/merge swap (bm25_segcat_publish_swap) recomputes
     * both from the catalog; the tombstone path (bm25_livedocs_clear) decrements
     * them; the pending append bumps pending_ndocs ONLY and leaves these alone. */
    uint64          ndocs;              /* live documents in sealed segments */
    uint64          total_len;          /* sum of their doc lengths -> avgdl */

    /* retired-segment pending-free list (chain of BM25_PAGE_RETIRED pages) */
    BlockNumber     retired_head;       /* or Invalid */

    /* segment generation counter (home of BM25SegCatEntry.gen /
     * BM25SegmentHeader.gen; bm25_next_gen() reads-and-bumps it). */
    uint32          next_gen;

    /* ---- v4 additions (appended after next_gen; on-disk order is contract) ---- */
    uint32          analyzer_fingerprint;  /* FNV-1a(stemmer_id || language ||
                                            * stopword_set_hash || tokenizer_type ||
                                            * analyzer_revision || database_encoding ||
                                            * probe_hash);
                                            * bm25_analyzer_fingerprint is authoritative
                                            * for the component list and its order.
                                            * 0 until bm25_build overwrites it */
    BlockNumber     field_config_blkno;    /* root of the per-index field-config chain;
                                            * InvalidBlockNumber => single default field */
    uint32          field_count;           /* number of dense fields == number of indexed key
                                            * columns == number of BM25FieldConfig records on
                                            * the field-config page. bm25_meta_init seeds 1 as
                                            * a sentinel; bm25_build overwrites it with the
                                            * count bm25_resolve_fields resolved. Every reader
                                            * uses it as a loop bound / array index, which is
                                            * why bm25_meta_validate bounds it to
                                            * 1..BM25_MAX_FIELDS before any caller sees it. */

    /* ---- v6 additions (appended after field_count; on-disk order is contract) ----
     * Both read as 0 from the zeroed page tail on a legacy v5 metapage (v5 never
     * wrote them), which the two-directional gate treats as "legacy, readable". */
    uint32          min_read_version;   /* floor: oldest BM25_FORMAT_VERSION that can
                                         * read this index; >=5 when stamped */
    uint32          feature_flags;      /* bitmap of optional capabilities present; never gates
                                         * READABILITY. Bits 0-2 informational; bit 3
                                         * (BM25_FEAT_SEGCAT_TOKENS) is a trust signal the
                                         * merge estimator consults -- see its define. */

    /* ---- orphan-sweep evidence (issue #300; appended past feature_flags) ----
     *
     * bm25_reclaim_orphans runs its mark-and-sweep only when one of these says an
     * orphan or a lost FSM entry can exist; see its header for the invariant.
     *
     * ADDITIVE without a version move (ADR 0009). The struct used to end at
     * feature_flags (offset 96), sizeof 104, so every byte from 100 on was either the
     * struct's trailing pad or past it. No older binary reads or writes past 104 (it
     * memcpy's sizeof(BM25MetaPageData) = 104), and bm25_meta_set_pd_lower only ever
     * RAISES pd_lower, so an older binary that writes this metapage carries the tail
     * through unchanged. On every metapage written before this field existed the bytes
     * are zero -- PageInit zeroed them, nothing ever wrote them, and FPI replay
     * restores the [pd_lower, pd_upper) hole as zeros -- and ZERO IS THE SAFE VALUE:
     * swept_epoch == 0 reads as "never swept by a binary that keeps this evidence", so
     * the first VACUUM sweeps. That is why there is no format_version, min_read_version
     * or feature_flags move and no read gate: nothing older needs to be told, and no
     * value an older binary can leave here makes a sweep skip that should have run.
     *
     * The pad at 100 is NAMED so that no new field ever lands in bytes whose history
     * nobody checked; it is never interpreted. */
    uint32          reserved_tail_pad;  /* @100: the implicit trailing pad through v8 */
    uint32          orphan_ops_begun;   /* @104: maintenance ops that can leave orphans,
                                         * bumped by bm25_orphan_op_begin in a WAL record
                                         * written BEFORE the op's first allocation */
    uint32          orphan_ops_done;    /* @108: bumped by bm25_orphan_op_end when such an
                                         * op finishes without leaving any; set equal to
                                         * orphan_ops_begun by a completed sweep */
    uint64          swept_epoch;        /* @112: bm25_crash_epoch() of the server lifetime
                                         * whose sweep last completed; 0 = never */
} BM25MetaPageData;

/* The evidence tail's offsets are on-disk contract (see the comment above). */
StaticAssertDecl(offsetof(BM25MetaPageData, feature_flags) == 96,
                 "metapage feature_flags moved; the evidence tail's additivity argument assumes offset 96");
StaticAssertDecl(offsetof(BM25MetaPageData, orphan_ops_begun) == 104 &&
                 offsetof(BM25MetaPageData, orphan_ops_done) == 108 &&
                 offsetof(BM25MetaPageData, swept_epoch) == 112 &&
                 sizeof(BM25MetaPageData) == 120,
                 "metapage orphan-sweep evidence tail must stay at offsets 104/108/112, size 120");

static inline BM25MetaPageData *
BM25PageGetMeta(Page p)
{
    return (BM25MetaPageData *) PageGetContents(p);
}

/* NOT on-disk: an optional (format_version, min_read_version, feature_flags)
 * re-stamp folded into the merge atomic swap (bm25_segcat_publish_swap).
 * NULL on the merge/seal path (no version change). Non-NULL only on the
 * bm25_upgrade segment-rewrite path, where the version re-stamp MUST commit in the
 * SAME GenericXLog record as the catalog-root flip -- otherwise a crash could leave
 * new-format segments published under a still-old-format metapage. This struct
 * just carries the values into the swap's throw-free WAL window.
 *
 * format_version and feature_flags are ASSIGNED there. min_read_version is not:
 * it is the monotonic floor described above, so the swap writes
 * Max(on-disk, min_read_version) against its registered metapage copy. Read this
 * field as "the floor the caller requires AT LEAST" -- the caller computes it
 * from a pre-lock snapshot, and a concurrent writer (bm25_pending_append_multi)
 * can raise the real floor in between. */
typedef struct BM25FormatRestamp
{
    uint32  format_version;
    uint32  min_read_version;
    uint32  feature_flags;
} BM25FormatRestamp;

static inline BM25PageOpaque *
BM25PageGetOpaque(Page p)
{
    return (BM25PageOpaque *) PageGetSpecialPointer(p);
}

/* ---- retired-segment free-list descriptor (D-RETIRE/M7) ----
 * A per-SEGMENT RANGE descriptor (not a per-page record): it captures a dropped
 * segment's header + chain roots plus the single retire_xid horizon, appended to
 * the persistent retired-list crash-atomically inside the merge swap's one Generic
 * WAL record. bm25_reclaim_retired re-walks the chains later to free each page.
 * Storing the range (not every page) keeps the in-swap append to ONE retired-list-
 * tail page, so the swap's single record stays within the 4-buffer cap.
 *
 * BM25_RETIRED_PER_PAGE (entries that fit on one such page) is therefore the hard
 * cap on how many segments one swap record may drop. It lives here so BOTH the
 * swap's retire budget (bm25_retire_segment / bm25_fsm.c) AND callers that must
 * respect that single-page cap before opening the window (bm25_merge caps inputs
 * at BM25_MERGE_MAX_INPUTS; bm25_merge_rewrite_all guards on this constant) read
 * the ONE value. */
typedef struct BM25RetiredEntry
{
    BlockNumber         header_blkno;   /* segment header; root of all chains */
    BlockNumber         dict_root;
    BlockNumber         norms_root;
    BlockNumber         livedocs_root;
    BlockNumber         docmap_root;
    BlockNumber         posts_root;     /* the one shared POSTINGS chain (D-POST/M1) */
    /* M5: the dropped segment's KEYMAP chain root (docid->key). Real (not Invalid)
     * only when the index has key_field. This is the ONE deliberate growth of the
     * retired descriptor M5 requires: keymap is now a real live chain that must be
     * marked reachable in the orphan sweep and freed by reclaim_one_range, exactly
     * like the five v3 roots. */
    BlockNumber         keymap_root;
    /* M4 (C-POS-MERGE): the dropped segment's POS chain root (BM25_PAGE_POS frames).
     * Real (not Invalid) only when the index stores positions AND the segment was
     * built/merged under M4; a pre-M4 or store_positions=off segment leaves it
     * Invalid, making every mark/free site below a no-op. Twinned one-for-one with
     * keymap_root: marked reachable in the orphan sweep and freed by reclaim_one_range,
     * or the POS pages leak (unreclaimed) / are freed while live (use-after-free). */
    BlockNumber         pos_root;
    uint32              gen;            /* dropped segment's gen (diagnostic) */
    FullTransactionId   retire_xid;
} BM25RetiredEntry;

/* Entries per retired page: contents area / sizeof(entry). */
#define BM25_RETIRED_PER_PAGE \
    ((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(BM25PageOpaque))) \
     / sizeof(BM25RetiredEntry))

/* ---- segment catalog entry ---- */
typedef struct BM25SegCatEntry
{
    BlockNumber     header_blkno;   /* the segment's BM25_PAGE seg-header page ("root") */
    /* Sum of tf over this segment's LIVE postings -- the token count, which since
     * analyzer revision 5 (ADR 0087) is a different quantity from total_len below:
     * total_len counts source word RUNS, this counts the tokens they emit, and a
     * compound-splitting dictionary makes the second about 5x the first.
     *
     * Occupies the 4-byte hole the compiler was already inserting between
     * header_blkno and the 8-aligned ndocs, so sizeof, the on-page stride and
     * entries-per-page are all unchanged -- ADR 0009's ADDITIVE shape (iii), "a
     * previously invalid/zero sentinel field being filled". GROWING this struct
     * would instead be a hard break: the catalog is a packed fixed-stride array
     * with no per-record length prefix, so an old reader desyncs at record two.
     *
     * TRUSTED ONLY WHEN BM25_FEAT_SEGCAT_TOKENS IS SET, and the reason is residue,
     * not layout. Before ADR 0074 (2026-08-20) the merge path copied an entry
     * built on the stack, carrying uninitialized padding onto the page and then
     * forward through every later merge -- so on an index built by such a binary
     * these four bytes hold garbage, not zero. The flag is stamped only at BUILD,
     * never conferred by bm25_upgrade (whose transform registry is empty, so its
     * accepted-gap path is an in-place restamp that would otherwise bless the
     * residue), so it means "every entry here was written by a counter-aware
     * binary".
     *
     * ONE READER: bm25_accum_estimate_bytes, reached from bm25_merge_estimates, which
     * passes this field only when the flag above certifies it and 0 otherwise. That
     * function clamps to Max(stored, total_len), which is what makes an absent,
     * saturated or garbage-low value degrade to the run count rather than under-charge
     * -- read its header before changing anything here, because the error direction is
     * the whole design. Saturates at PG_UINT32_MAX; a segment that large is refused by
     * the merge budget on either number. */
    uint32          total_tokens;
    uint64          ndocs;          /* docs in segment (incl. tombstoned, for layout) */
    uint64          live_ndocs;     /* non-tombstoned (for merge-trigger / stats) */
    uint64          total_len;      /* sum of doclen over live docs (approx until merge) */
    uint32          nterms;
    uint32          gen;            /* monotonically increasing segment generation id */
} BM25SegCatEntry;

/* Pin the on-disk descriptor sizes (#144).
 *
 * BM25SegCatEntry NO LONGER HAS A HOLE: its 4 bytes at offset 4 became the named
 * total_tokens field (ADR 0088), which is the cure the paragraph below wishes for --
 * it followed BM25DictEntry's dict_pad and BM25KeymapHeader's pad0 after all, and
 * without a format break, because naming a hole moves no other byte. There is now no
 * implicit padding in that struct for stack residue to hide in, so the #144 class is
 * RETIRED for it rather than merely guarded. bm25_segcat_entry_from_hdr's memset stays
 * as defence in depth against a future member reorder reintroducing one.
 *
 * BM25RetiredEntry still carries an implicit hole a member reorder would move silently:
 * 4 bytes at offset 36 (uint32 gen then an 8-aligned FullTransactionId). It is written
 * field-by-field into a memset slot, and every reader and writer strides it by plain
 * sizeof -- C array indexing, pd_lower advanced by sizeof, BM25_RETIRED_PER_PAGE and
 * the readers' entry counts both divided by sizeof -- so a size change silently
 * repartitions an existing page rather than failing to compile.
 *
 * Both sizes stay pinned. If one of these fires, the change is an ADR 0009 format
 * break -- these are packed fixed-stride arrays with no per-record length prefix, so
 * an old reader desyncs at the second record. */
StaticAssertDecl(sizeof(BM25SegCatEntry) == 40,
                 "BM25SegCatEntry changed size; its on-page stride repartitions the catalog page");
StaticAssertDecl(sizeof(BM25RetiredEntry) == 48,
                 "BM25RetiredEntry changed size; BM25_RETIRED_PER_PAGE repartitions silently");

/* BM25SegCatEntry is addressed at TWO strides that must coincide (#67, ADR 0094).
 * The on-page layout is MAXALIGN-strided: bm25_segcat_entries_per_page divides by
 * MAXALIGN(sizeof), the seal's in-window append and bm25_segcat_build_orphan_chain
 * advance pd_lower by it, and bm25_scan_snapshot / bm25_segcat_read step their cursor
 * by it. But several sites address the same page as a plain C array, which strides by
 * sizeof: the orphan-chain writer's single bulk memcpy, bm25_segcat_find_entry's and
 * bm25_segcat_locate_entry's ents[i] (each counts its entries at the MAXALIGN stride
 * and then indexes them at the array stride, in the same loop), and
 * bm25_livedocs_clear's [catidx]. Both are correct only while sizeof is already a
 * MAXALIGN multiple.
 *
 * Today that holds for a reason no line of code states: the uint64 members give the
 * struct the alignment MAXIMUM_ALIGNOF measures (8 on 64-bit targets, 4 on i386,
 * where int64 and double are both 4-aligned inside a struct), and C pads sizeof to its
 * alignment, so ADDING a member cannot break it (on a 64-bit target a uint32 more
 * makes 48, not 44). What breaks it is NARROWING -- say the three uint64 counters
 * become uint32 to save catalog space. On a 64-bit target the struct is then 4-byte
 * aligned, 28 bytes on a 32-byte page stride, and every array-indexed site reads entry
 * k from 4k bytes short of it. The size pin above cannot catch that,
 * because a deliberate format change updates the number along with the struct. This
 * pins the RELATIONSHIP, so the change fails to compile here instead: keep the struct
 * MAXALIGN-clean, or convert the array-indexed sites to the byte stride. */
StaticAssertDecl(sizeof(BM25SegCatEntry) == MAXALIGN(sizeof(BM25SegCatEntry)),
                 "BM25SegCatEntry is no longer MAXALIGN-clean; its array-indexed readers "
                 "and bulk-copy writer would disagree with the MAXALIGN on-page stride");

/* ---- segment header (the segment "root" page contents) ---- */
typedef struct BM25SegmentHeader
{
    uint32          gen;            /* matches catalog entry */
    /* Token count at SEAL time, in the 4-byte hole between gen and the 8-aligned
     * ndocs; sizeof stays 64. This is the IMMUTABLE master: the catalog entry's
     * copy is decayed per tombstone, this one is not, so the tombstone path can
     * derive a per-document average from it exactly as it already does for
     * total_len. Unlike the catalog entry's copy this needs no trust flag -- the
     * header page is filled field-by-field in place on a bm25_page_init'd page, so
     * the hole is genuinely zero on every index ever written, with no residue
     * path. See BM25SegCatEntry.total_tokens for the rest of the story. */
    uint32          total_tokens;
    uint64          ndocs;          /* dense local doc-ids are 0..ndocs-1 */
    uint64          total_len;
    uint32          nterms;
    BlockNumber     dict_root;      /* first BM25_PAGE_DICT page (chained, sorted) */
    BlockNumber     norms_root;     /* first BM25_PAGE_NORMS page */
    BlockNumber     livedocs_root;  /* first BM25_PAGE_LIVE page (tombstone bitmap) */
    BlockNumber     docmap_root;    /* first BM25_PAGE_DOCMAP page (docid->TID) */
    BlockNumber     posts_root;     /* first BM25_PAGE_POST page of the SINGLE
                                     * segment-wide postings chain (D-POST; kept in
                                     * sync with the other BM25SegmentHeader copy). */

    /* ---- v4 additions ---- */
    uint32      field_count;        /* == meta->field_count as of when this segment was
                                     * written; also the count for the arrays below */
    BlockNumber pos_root;           /* M4: BM25_PAGE_POS chain root; Invalid when the
                                     * index stores no positions (pre-M4 / all fields off) */
    BlockNumber keymap_root;        /* M5: BM25_PAGE_KEYMAP chain root (docid->key). Invalid
                                     * when the index has no key_field or the segment is
                                     * empty (bm25_keymap_write returns Invalid for both),
                                     * and readers then fall back to ctid. */
    /* total_len_by_field[field_count] is NOT a fixed struct member: it is serialized
     * immediately AFTER the struct on the header page (section H), with the struct's
     * field_count as the count. Its elements sum to total_len above, so a
     * single-field segment writes exactly one uint64 equal to it. The full on-page
     * layout (including the second, ndocs_by_field[] array, written only when
     * field_count > 1) is spelled out at bm25_segheader_write_lenfields below. */
} BM25SegmentHeader;

/* The segment header is not a packed array, but the per-field arrays (section H) are
 * serialized at base + sizeof(BM25SegmentHeader), so its size is equally load-bearing.
 * ADR 0088 added this pin; issue #313 pinned the remaining on-disk structs. */
StaticAssertDecl(sizeof(BM25SegmentHeader) == 64,
                 "BM25SegmentHeader changed size; the total_len_by_field[] tail moved");

/* ---- KEYMAP page header (BM25_PAGE_KEYMAP, M5 key_field, spec section 3.7) ----
 * The per-segment docid->key flat array. The chain root page carries this header
 * followed by the dense key[] bytes; continuation pages (nextblk) carry raw key[]
 * bytes only (DOCMAP/NORMS flat-chain convention). key[docid] lives at byte offset
 * docid*key_size in the flat stream that begins AFTER this header on the root page.
 * 8 bytes, MAXALIGN-clean; memset-0 before fill so the WAL image is deterministic. */
typedef struct BM25KeymapHeader
{
    uint8       key_type;       /* BM25_KEY_* */
    uint8       pad0;           /* explicit pad; memset-0 before fill (WAL determinism) */
    uint16      key_size;       /* fixed width in bytes (1..BM25_KEY_MAX_SIZE) */
    uint32      ndocs;          /* == segment ndocs (dense docid index bound) */
    /* unsigned char key[ndocs * key_size] follows the header on the root page and
     * may span nextblk continuation pages. */
} BM25KeymapHeader;

/* Issue #313 SEGREAD-08: the key[] stream starts at sizeof(BM25KeymapHeader). */
StaticAssertDecl(sizeof(BM25KeymapHeader) == 8,
                 "BM25KeymapHeader changed size; the key[] stream moved");

/* ---- dictionary entry (chained DICT pages, sorted by term) ---- */
typedef struct BM25DictEntry
{
    uint32          df;             /* postings for this term in THIS segment, one per
                                     * (doc, field); equals the document count only when
                                     * field_count == 1. It is also the exact decode bound
                                     * bm25_seg_scan_postings stops on -- the segment-wide
                                     * postings chain is undelimited, so a term's run of
                                     * blocks carries no end-of-term marker. */
    BlockNumber     post_root;      /* first BM25_PAGE_POST page for this term's blocks */
    uint16          post_off;       /* byte offset of first block within post_root */
    uint16          termlen;        /* unchanged; term bytes follow, MAXALIGN-strided */

    /* ---- v4 additions (live since M4; the builder fills them) ---- */
    BlockNumber     pos_post_root;  /* first BM25_PAGE_POS frame for this term; Invalid when
                                     * the segment stores no positions, or the term occurs
                                     * only in positions-off fields (no frame was written) */
    uint16          pos_post_off;   /* byte offset within pos_post_root; 0 when Invalid */
    uint16          dict_pad;       /* Explicit tail pad: it turns what would otherwise be
                                     * implicit trailing padding into a named field the
                                     * writer memset-0s, so the WAL image is deterministic.
                                     * It supplies NO alignment -- the struct's members are
                                     * all uint32/uint16, so its alignment is 4 and sizeof
                                     * is 20 with or without this field. MAXALIGN of the
                                     * whole record comes from the MAXALIGN(sizeof + termlen)
                                     * stride at the write site (bm25_segment_build_orphans)
                                     * and the read site (bm25_dictentry_validate). */
    /* char term[termlen] follows */
} BM25DictEntry;

/* Issue #313 SEGREAD-08: DICT records are strided as MAXALIGN(sizeof + termlen), so the
 * size is the stride's base. */
StaticAssertDecl(sizeof(BM25DictEntry) == 20,
                 "BM25DictEntry changed size; the DICT record stride moved");

/* ---- field-config page (BM25_PAGE_FIELDCFG, per-index, single copy) ----
 * Rooted at meta->field_config_blkno, written once at CREATE INDEX, read-only
 * thereafter. Flat-page convention: records begin at PageGetContents() and run up
 * to pd_lower. Page layout:
 *   [BM25FieldConfigHeader][BM25FieldConfig x field_count]
 *   [uint8 store_positions[field_count]]   (M4/D12, optional -- absent on an
 *                                           M5-era page written before M4)
 *   [BM25KeyStamp]                         (#292, optional -- absent on a page
 *                                           written before #292; see below)
 * One BM25FieldConfig per indexed column. The trailing store_positions array is
 * detected by the bytes remaining past the last config record below pd_lower
 * (bm25_fieldcfg_read); a page without it leaves the caller's flags all-zero, which
 * is correct because such a segment has pos_root Invalid. Those flags decide whether
 * POS frames are written and consumed: bm25_pending.c and bm25_merge.c feed them to
 * bm25_accum_set_store_positions, bm25_seg_chain.c gates the lockstep POS reader in
 * bm25_seg_scan_postings on them, bm25_scan_match.c gates the pending-side position
 * stash, bm25_scan_rank.c passes them down to that reader, and bm25_meta.c derives
 * BM25_FEAT_POSITIONS
 * from them. The writer/reader live in bm25_analyzer.c; these structs lock the
 * on-disk layout. */
typedef struct BM25FieldConfigHeader
{
    uint32  field_count;                        /* == meta->field_count */
    uint32  per_field_fingerprint[BM25_MAX_FIELDS];  /* [0..field_count-1] ALL carry the one
                                                 * index-wide analyzer fingerprint (per-field
                                                 * analyzer divergence is deferred, spec
                                                 * section 9); [field_count..] stay 0 */
} BM25FieldConfigHeader;

/* One record per indexed key column. Every member is resolved by bm25_resolve_fields
 * (bm25_build.c) at CREATE INDEX and never rewritten afterwards. */
typedef struct BM25FieldConfig
{
    uint32  field_id;                            /* dense, 0..field_count-1; the column's
                                                  * index attribute position */
    char    field_name[BM25_FIELD_NAME_LEN];     /* fixed 64 B, NUL-padded; the column attname */
    float8  k1;                                  /* BM25 k1: the index-wide k1 reloption
                                                  * (BM25_DEFAULT_K1 when the index carries no
                                                  * reloptions), overridden by k1_<attname> */
    float8  b;                                   /* BM25 b: same rule, b_<attname> overrides */
    float8  boost;                               /* index-time field boost; 1.0 unless
                                                  * boost_<attname> overrides it */
    uint32  tokenizer_type;                      /* BM25_TOKENIZER_*; cloned from the ONE index
                                                  * analyzer -- per-field analyzer divergence
                                                  * is deferred (spec section 9) */
    char    stemmer_name[BM25_STEMMER_NAME_LEN]; /* fixed 32 B, NUL-padded; the same index
                                                  * analyzer's language, cloned likewise */
} BM25FieldConfig;

/* ---- key-identity stamp (#292): the field-config page's optional tail ----
 * The key_field resolution CREATE INDEX / REINDEX made, persisted so every INSERT can
 * be checked against it. key_field is structural -- it fixes the type and width of
 * every KEYMAP and the meaning of every stored key -- but it is an ordinary reloption,
 * so `ALTER INDEX ... SET (key_field = ...)` is accepted, and bm25_insert re-resolves
 * it from the live reloptions on every row. Without a persisted identity the insert
 * gate could compare a row only against segment 0's KEYMAP header, which (a) does not
 * exist until the first seal, so an index loaded after CREATE INDEX admitted any
 * key_field change into its pending chain, and (b) carries no column, so re-pointing
 * key_field at another column of the same type passed even with segments present.
 *
 * Written once, by both build writers (bm25_fieldcfg_write and its INIT_FORKNUM
 * sibling), never rewritten: REINDEX writes a new page on a new relfilenode. It sits
 * immediately after the store_positions array, so its offset is
 *   sizeof(BM25FieldConfigHeader) + field_count * sizeof(BM25FieldConfig) + field_count
 * and a stamped page always carries the flag array (the writer emits zeros when it has
 * none, which reads exactly like an absent array).
 *
 * ADDITIVE (ADR 0009). An older binary sizes the flag array by field_count and never
 * looks past it, so it reads a stamped page unchanged. A page an older binary wrote
 * ends at the flag array (pd_lower is the flat-page end-of-data marker), so this
 * binary sees no stamp there and the insert gate falls back to the segment-0 check.
 * No format_version gate decides any of this -- a read gate is unsound while
 * bm25_upgrade's registry is empty -- presence is decided by pd_lower alone, and the
 * magic makes a present-but-wrong tail a corruption error rather than a plausible
 * identity. All fields are fixed-width integers and the record lands at an unaligned
 * offset, so readers and writers memcpy it. */
#define BM25_KEYSTAMP_MAGIC 0x4B45594Du     /* 'KEYM' */
typedef struct BM25KeyStamp
{
    uint32  magic;          /* BM25_KEYSTAMP_MAGIC */
    uint8   key_type;       /* BM25_KEY_*, as bm25_resolve_fields resolved it */
    uint8   pad0;           /* 0 (WAL-image determinism) */
    uint16  key_size;       /* 0 when key_type == BM25_KEY_NONE */
    int16   key_attno;      /* 0-based INDEX attno of the key column (what the insert
                             * callback subscripts values[] by); -1 when keyless */
    uint16  pad1;           /* 0 */
} BM25KeyStamp;

/* ---- posting block header (128 docs/block; varbyte streams follow) ---- *
 * On-page block layout (serialized), M3 (single field, field_rle_bytes == 0):
 *   BM25BlockHeader
 *   <docid_bytes of varbyte delta-encoded ascending local doc-ids>
 *   <tf_bytes of varbyte tf values>
 * No field-id RLE bytes are appended when field_rle_bytes == 0. When
 * field_count > 1 (post-M3), field_rle_bytes bytes of per-block field-id RLE
 * follow the tf stream. v5 appends a further impact_bytes of a per-field impact
 * table (BM25BlockImpact, see below) after the RLE tail.
 * Blocks for a term are consecutive, chained across BM25_PAGE_POST via nextblk. */
typedef struct BM25BlockHeader
{
    uint16          ndocs;
    uint32          last_docid;     /* max doc-id in block (skip key) */
    uint16          docid_bytes;
    uint16          tf_bytes;

    /* ---- v4 addition ---- */
    uint16          field_rle_bytes;    /* length of the per-block field-id RLE stream;
                                         * present only when field_count > 1.
                                         * M3 (single field): ALWAYS 0. */

    /* ---- v5 addition ---- */
    uint16          impact_bytes;   /* length of the trailing BM25BlockImpact encoding
                                     * (see bm25_encode_impact_table); never 0 -- every
                                     * block has at least a 1-byte nfields count. */
} BM25BlockHeader;

/* Issue #313 SEGREAD-08: 16 bytes with a 2-byte alignment hole at offset 2 (between
 * ndocs and last_docid). The hole is on disk; the builder memset-0s the header. */
StaticAssertDecl(sizeof(BM25BlockHeader) == 16 &&
                 offsetof(BM25BlockHeader, last_docid) == 4 &&
                 offsetof(BM25BlockHeader, impact_bytes) == 14,
                 "BM25BlockHeader changed layout; every postings block moved");

/* v5 per-block impact table: for each field present in the block, the max tf and the
 * min field-length over the block's postings. These are raw, per-field, un-normalized
 * quantities -- deliberately NOT a single baked float4 upper bound, because idf and
 * avgdl are index-wide statistics that drift as the index grows; baking them at seal
 * time would go stale. bm25_block_ub (bm25_wand.c) is the consumer: it combines this
 * table with the running scan's LIVE per-field idf/avgdl to derive the block's score
 * upper bound, evaluating the SAME bm25_termscore the real scorer uses at this
 * field's (max_tf, min_doclen). */
typedef struct BM25FieldImpact
{
    uint8   field_id;
    uint32  max_tf;
    uint32  min_doclen;
} BM25FieldImpact;

typedef struct BM25BlockImpact
{
    uint8           nfields;
    BM25FieldImpact fields[BM25_MAX_FIELDS];
} BM25BlockImpact;

/* Impact-table codec (defined in bm25_seg_build.c, the only writer). The decoder runs
 * on the block-max WAND query path: wand_cursor_load_block (bm25_wand.c) and
 * bm25_seg_block_header_read (bm25_seg_chain.c) each decode a block's table right after
 * bm25_block_validate has bounded the header. Its input is untrusted on-disk data and
 * its output is a caller stack BM25BlockImpact, so it validates before filling: nfields
 * must be <= BM25_MAX_FIELDS, and nbytes must equal exactly
 * 1 + BM25_IMPACT_FIELD_BYTES * nfields (either mismatch is ERRCODE_INDEX_CORRUPTED).
 * Wire format: [uint8 nfields][{uint8 field_id, uint32 max_tf, uint32 min_doclen} x
 * nfields], fixed-width and copied with memcpy -- so the uint32s land in HOST byte
 * order (native, no conversion), as with the rest of this format. */
/* On-disk width of one impact entry: uint8 field_id + uint32 max_tf +
 * uint32 min_doclen. A table is therefore exactly 1 + 9*nfields bytes, which is
 * what the decoder cross-checks its declared nfields against. */
#define BM25_IMPACT_FIELD_BYTES 9

/* The decode boundary for a posting block: validates a BM25BlockHeader read off a
 * page against what encode_block can produce, and returns the block's total
 * on-page length. `cur` points at the header, `pend` one past the last valid
 * content byte. ERRCODE_INDEX_CORRUPTED on anything inconsistent. Every reader that
 * decodes a block must call this before using any header field. */
extern Size bm25_block_validate(const BM25BlockHeader *hdr,
                                const char *cur, const char *pend);
/* The header half of the block contract that can only be checked AFTER the docid
 * deltas are expanded: last_docid must equal the decoded run's final docid. Every
 * reader that expands a block's docids must call this immediately afterwards --
 * bm25_block_validate cannot, the run does not exist when it runs. */
extern void bm25_block_last_docid_validate(const BM25BlockHeader *hdr,
                                           const uint32 *docids);

/* Issue #289: the postings of a block's FIRST document -- its docid (the first docid
 * varbyte, stored absolute), and the (tf, field_id) of each of its postings, which are
 * the leading postings sharing that docid (delta 0). On a segment written before the
 * writer cut blocks at document boundaries these can be the continuation of the
 * previous block's last document; the WAND deep check bounds that continuation with
 * them. A document holds at most one posting per field, so n <= BM25_MAX_FIELDS. */
typedef struct BM25BlockLead
{
    uint32  docid;
    uint32  n;
    uint32  tf[BM25_MAX_FIELDS];
    uint32  field_id[BM25_MAX_FIELDS];
} BM25BlockLead;

/* Decode a block's lead. `cur` points at the block's header, which the caller has
 * copied to *hdr and passed through bm25_block_validate against the same page, so
 * every stream read here is within the page. Always sets lead->docid; decodes the
 * postings (lead->n > 0) only when that docid equals match_docid, else n = 0 -- the
 * caller asks about one document, and most blocks do not start with it.
 * ERRCODE_INDEX_CORRUPTED if more than BM25_MAX_FIELDS postings share the docid. */
extern void bm25_block_lead_decode(const BM25BlockHeader *hdr, const char *cur,
                                   uint32 match_docid, BM25BlockLead *lead);

extern Size bm25_encode_impact_table(const BM25BlockImpact *imp, uint8 *out);
extern void bm25_decode_impact_table(const uint8 *in, uint16 nbytes, BM25BlockImpact *out);

/* varbyte codec (defined in bm25_seg_build.c) */
extern int  bm25_varbyte_encode(uint32 v, uint8 *out);       /* bytes written (1..5) */
/* `end` = one past the last readable byte of the run; overrunning it, or a
 * sequence wider than a uint32, is ERRCODE_INDEX_CORRUPTED. Returns bytes read. */
extern int  bm25_varbyte_decode(const uint8 *in, const uint8 *end, uint32 *v);

/* Per-block field-id RLE codec (defined in bm25_seg_build.c). The encoder is used
 * only by the segment builder (encode_block), so it stays file-local there; the
 * DECODER is shared -- bm25_seg_scan_postings (bm25_seg_chain.c) walks the RLE tail
 * in lockstep with the docid/tf streams. Expanding `nbytes` of (field_id,
 * run_length) varbyte pairs fills out_field_ids[0..n-1] (the run lengths sum to n
 * by construction). A single-field block has nbytes == 0; the caller fills field 0. */
extern void bm25_field_rle_decode(const uint8 *in, Size nbytes,
                                  uint32 *out_field_ids, uint32 n);

/* v4 segment-header length-prefixed field-array serialization (section H). The header
 * page layout is
 *   field_count == 1: [BM25SegmentHeader][uint64 total_len_by_field[1]]
 *   field_count  > 1: [BM25SegmentHeader][uint64 total_len_by_field[fc]]
 *                                        [uint64 ndocs_by_field[fc]]
 * field_count is read from hdr->field_count and is the prefix count. The
 * ndocs_by_field[] array is present ONLY when field_count > 1 -- a single-field v4
 * index keeps the M3/C-BUILD byte layout (no REINDEX), and the reader derives
 * N_field[0] from hdr->ndocs. ndocs_by_field may be NULL on write when field_count
 * == 1 (unused), and out_ndocs_by_field may be NULL on read when the caller only
 * wants sumdoclen. */
extern Size bm25_segheader_write_lenfields(Page pg, const BM25SegmentHeader *hdr,
                                           const uint64 *total_len_by_field,
                                           const uint64 *ndocs_by_field);
/* header_blkno is for the corruption message only -- this function validates
 * hdr->field_count itself, because that value is its loop bound over the caller's
 * arrays and a caller cannot be trusted to remember the check. */
extern void bm25_segheader_read_lenfields(Page pg, const BM25SegmentHeader *hdr,
                                          BlockNumber header_blkno,
                                          uint64 *out_total_len_by_field,
                                          uint64 *out_ndocs_by_field);

/* ---- pending-list on-page records (internal to bm25_pending.c) ---- *
 * A document writes one BM25PendingDocHeader followed by ndocterms
 * BM25PendingTermEntry records, each immediately followed by its term bytes
 * (termlen) THEN its position blob (pos_bytes), MAXALIGN-strided. The doc-group
 * region begins at PageGetContents() and extends up to pd_lower (the same flat-page
 * convention used for SEGCAT/RETIRED pages).
 *
 * v7 -- A DOCUMENT MAY SPAN PAGES. It used to be required to fit one page, which
 * capped an INSERT at ~380 distinct 7-byte stems while CREATE INDEX had no such
 * limit (issue #57). A document whose records do not fit one page is now written as
 * SEVERAL records that all carry the SAME tid, the second and later ones flagged
 * BM25_PENDING_DOC_CONT. Parts do NOT own whole pages; what the format requires is
 * only that they are CONSECUTIVE RECORDS in chain order, and the appender holds the
 * metapage buffer EXCLUSIVE for the whole multi-part write, so no other document's
 * record can interleave between this document's parts. A page freely holds records
 * from several different documents over time: part 0 reuses the existing tail page
 * when it has room, and the last part's page keeps whatever it did not fill for
 * later inserts. What DOES fall out of the packing, as a consequence rather than a
 * format rule, is that two consecutive parts of one document never share a page --
 * each part is budgeted against the whole PENDING_PAGE_CAPACITY, so the entry that
 * ended part k cannot fit the room part k left behind (bm25_pending.c's append
 * loop, and the reachability argument in its drain). Every part is a
 * structurally ordinary record -- the stride, the term-entry offsets and the
 * single-part byte layout are all unchanged -- so no walker had to change how it
 * DECODES a record. What every walker aggregating per DOCUMENT did have to learn is
 * that consecutive same-tid parts are one document: the drain (see its part buffer for
 * why the accumulator must be fed ONCE per (doc, field) rather than once per part),
 * pending_global_stats, pending_stats_by_field, bm25_pending_score_term, and -- since #195,
 * for the stranded-continuation state rather than for spanning itself -- pending_df and
 * pending_df_by_field. Walkers whose output is a TID (the @@@ collector, the AND mask,
 * the phrase stash) need nothing: they walk entries, each entry appears exactly once
 * wherever it lands.
 *
 * A reader that does not know about CONT would treat each part as its own document
 * and register one docid per part, over-counting the doc's length and per-field
 * document counts. That is why a continuation raises min_read_version to
 * BM25_MIN_READ_PENDING_SPAN rather than relying on the layout being byte-neutral.
 *
 * M4 positions: each term entry carries the TRUE per-occurrence positions of that
 * (term,field) as a delta-varbyte blob (pos_bytes long) appended after the term
 * bytes. This is what makes an inserted-then-sealed doc phrase-searchable without a
 * REINDEX: the drain feeds these real positions to the accumulator instead of
 * fabricating dict-order ordinals. Like field_id/key, this is a TRANSIENT layout
 * (the pending list is drained + page-recycled at every seal, so no segment-format
 * break) but it IS WAL-logged, so the append path memsets each entry before fill.
 *
 * v8 -- PER-FIELD DOCLEN IS STORED, NOT DERIVED. A uint32 doclen_by_field[nfieldlens]
 * array sits between the doc header and the first term entry, so the whole header
 * region is MAXALIGN(sizeof(header) + 4 * nfieldlens) bytes rather than
 * MAXALIGN(sizeof(header)). Use bm25_pending_doc_entries_off() -- never a bare
 * MAXALIGN(sizeof(BM25PendingDocHeader)) -- to find the first entry.
 *
 * WHY IT HAD TO BE STORED. Per-field doclen used to be RECONSTRUCTED by the readers as
 * sum-of-tf over a field's term entries. That identity holds only while doclen is a
 * count of TOKENS, because pending tf is token multiplicity. doclen is now defined as
 * the number of emitting source WORD RUNS (max stored position + 1), which is equal to
 * the token count for every analyzer revision that emits one token per run -- and stops
 * being equal the moment a dictionary emits several lexemes for one run. Deriving it
 * would then charge a compound word its lexeme count in the BM25 length denominator on
 * the pending side while the sealed side charged 1, i.e. a score that changes when a
 * seal runs. Storing it costs 4 bytes per field per pending record and makes the two
 * sides read the same number by construction.
 *
 * SELF-DESCRIBING, NOT VERSION-KEYED. The presence of the array is announced by the
 * record's own BM25_PENDING_DOC_FIELDLENS flag, not by the index's stamped
 * format_version. A pending list can legitimately hold both shapes at once -- a v7
 * index whose records were written before this binary attached, or a record appended by
 * a concurrent inserter in the window between bm25_upgrade's seal and its metapage
 * re-stamp -- and a version-keyed reader would mis-stride over exactly those. The
 * writer always emits the v8 shape and raises min_read_version to
 * BM25_MIN_READ_PENDING_FIELDLENS in the same record; the readers dispatch per record.
 * Records without the flag keep the sum-of-tf reconstruction, which is still exact for
 * them (they were written under an analyzer that emitted one token per run). */
/* BM25PendingDocHeader.flags bits. */
#define BM25_PENDING_DOC_CONT   0x0001u  /* v7: continues the preceding same-tid record */
#define BM25_PENDING_DOC_FIELDLENS 0x0002u  /* v8: nfieldlens uint32 doclens follow the header */

typedef struct BM25PendingDocHeader
{
    ItemPointerData tid;
    /* v8: how many uint32 per-field doclens follow this header, i.e. the writing
     * index's field_count. Meaningful ONLY when flags carries
     * BM25_PENDING_DOC_FIELDLENS; a reader must not treat it as a stride otherwise.
     * Placed here on purpose: tid is 6 bytes and ndocterms needs 4-byte alignment, so
     * this uint16 occupies the 2 bytes the compiler was already inserting as padding
     * (and the append path was already memset-ing for WAL determinism). sizeof and
     * every other member's offset are therefore unchanged -- pinned below. */
    uint16          nfieldlens;
    uint32          ndocterms;
    /* Whole-document length: the sum over fields of that field's doclen. Since v8 that
     * is a count of emitting source runs, not of tokens (see the region comment above);
     * the two coincide for one-lexeme-per-run analyzers, which is every analyzer
     * revision this format has seen so far. Feeds the scan's corpus-wide avgdl. */
    uint32          doclen;
    /* v7 (#57): see BM25_PENDING_DOC_CONT. Placed here deliberately -- the struct was
     * 36 bytes with a MAXALIGN stride of 40, so this uint32 lands in slack that was
     * already being written as padding. sizeof went 36 -> 40 and MAXALIGN(40) is still
     * 40, so v7 left every term entry at exactly the byte offset it had. (v8 then moved
     * them all anyway, by inserting the doclen array between the header and the first
     * entry -- deliberately, announced by a flag, and guarded by a floor. The
     * StaticAssertDecls below therefore pin the STRUCT's stride, which is a different
     * property from the entry offsets they used to imply.) */
    uint32          flags;
    /* M5 key_field: the row's fixed-width key, carried through the pending list so a
     * post-build INSERT keeps its user key after the next seal writes it into a
     * KEYMAP (without this, sealed-from-pending docs would fall back to ctid). key_type
     * == BM25_KEY_NONE for a keyless index (key[] unused). The pending list is transient
     * (drained + page-recycled every seal) so this is NOT a segment-format break, but it
     * IS WAL-logged, so the append path memsets the header before fill. */
    uint8           key_type;   /* BM25_KEY_* */
    uint8           key_pad0;
    uint16          key_size;   /* 0 when key_type == NONE */
    unsigned char   key[BM25_KEY_MAX_SIZE];
} BM25PendingDocHeader;

/* Every member must live inside the pre-existing MAXALIGN slack. What this pins, since
 * v8, is the FIXED HEADER's stride and nothing beyond it: v8 moved every term entry by
 * inserting the doclen array after the header, without disturbing this assert -- that
 * move is legitimate precisely because it is announced per record
 * (BM25_PENDING_DOC_FIELDLENS) and floored (BM25_MIN_READ_PENDING_FIELDLENS), which is
 * what an unannounced struct growth would not be. If THIS fires, the fixed header grew
 * past its stride, so even a reader that knows about the array lands mid-record: shrink
 * the struct back, or treat it as a real packed-record break (ADR 0009) and raise
 * BM25_OLDEST_READABLE. */
StaticAssertDecl(MAXALIGN(sizeof(BM25PendingDocHeader)) == 40,
                 "BM25PendingDocHeader outgrew its 40-byte pending-record stride");
/* And pin the EXACT size, which the MAXALIGN assert above cannot: it accepts anything
 * in 33..40, so v8's nfieldlens could have grown the struct to 44 (stride 48, every
 * term entry moved) and still passed. This is what states "nfieldlens consumed the
 * pre-existing tid padding" as a checked property rather than a comment. */
StaticAssertDecl(sizeof(BM25PendingDocHeader) == 40,
                 "BM25PendingDocHeader is no longer exactly 40 bytes; a member stopped "
                 "fitting the struct's existing padding");

/* Byte span of a pending record's header REGION: the fixed header plus, when the
 * record carries one, its per-field doclen array. This -- not
 * MAXALIGN(sizeof(BM25PendingDocHeader)) -- is the offset from a doc header to its
 * first BM25PendingTermEntry, on every walker.
 *
 * PRECONDITION: nfieldlens has been bounded. bm25_pending_iter_next is the one
 * canonical decode boundary that does it (<= BM25_MAX_FIELDS, and nonzero when the
 * flag is set) before the value becomes a stride; every other caller receives its
 * header from that iterator. The write side passes its own field_count, which
 * bm25_meta_validate has already bounded. */
static inline Size
bm25_pending_doc_header_size(uint32 nfieldlens)
{
    return MAXALIGN(sizeof(BM25PendingDocHeader) + sizeof(uint32) * (Size) nfieldlens);
}

static inline Size
bm25_pending_doc_entries_off(const BM25PendingDocHeader *dh)
{
    if ((dh->flags & BM25_PENDING_DOC_FIELDLENS) == 0)
        return MAXALIGN(sizeof(BM25PendingDocHeader));    /* v7 and earlier */
    return bm25_pending_doc_header_size(dh->nfieldlens);
}

/* The record's per-field doclen array, or NULL when the record predates v8 and the
 * reader must fall back to summing tf over the field's term entries. Aligned by
 * construction: the array starts at sizeof(BM25PendingDocHeader) == 40 bytes past a
 * MAXALIGN'd header. */
static inline const uint32 *
bm25_pending_doc_fieldlens(const BM25PendingDocHeader *dh)
{
    if ((dh->flags & BM25_PENDING_DOC_FIELDLENS) == 0)
        return NULL;
    return (const uint32 *) ((const char *) dh + sizeof(BM25PendingDocHeader));
}

typedef struct BM25PendingTermEntry
{
    uint16          termlen;
    uint16          tf;
    uint16          field_id;   /* M5: which indexed column this term came from.
                                 * The pending list is transient (drained + page-
                                 * recycled at every seal), so this layout change is
                                 * not a segment-format break; but it IS WAL-logged,
                                 * so the append path memsets the entry before fill. */
    uint16          pos_bytes;  /* M4: byte length of the delta-varbyte position blob
                                 * that follows the term bytes. The blob holds `tf`
                                 * ascending positions (first delta = pos[0]); the
                                 * drain decodes it into the TRUE per-occurrence
                                 * positions instead of dict-order ordinals. */
    /* char term[termlen] follows, then uint8 posblob[pos_bytes], then MAXALIGN
     * to the next entry. */
} BM25PendingTermEntry;

/* Issue #313 SEGREAD-08: the term bytes start at sizeof(BM25PendingTermEntry). */
StaticAssertDecl(sizeof(BM25PendingTermEntry) == 8,
                 "BM25PendingTermEntry changed size; the pending term records moved");

/* In-page pending iterator (BM25PendingIter) -- the ONE canonical walker over the
 * per-doc records on a single pending page. The drain (bm25_pending_drain), the
 * VACUUM dead-entry sweep (bm25_pending_mark_dead, Phase 3), the query-path pending
 * scans (bm25_scan.c, bm25_scan_rank.c, bm25_scan_match.c) and the wildcard expander
 * (bm25_dict_expand_wildcard, bm25_seg_dict.c) all go through it.
 *
 * The record-stride logic lives in exactly one place, and that place is
 * bm25_pending_term_entry_span (bm25_pending.c, declared in bm25.h) -- NOT this
 * iterator, which merely calls it. The distinction matters because a walker that
 * needs a doc's individual term entries re-walks them itself after iter_next has
 * returned the doc, so it strides the same records a second time; it must call that
 * function rather than re-derive MAXALIGN(sizeof(BM25PendingTermEntry) + termlen +
 * pos_bytes) inline. bm25_dict_expand_wildcard did exactly that until SEGREAD-13
 * (issue #154), which is what made this paragraph's claim false. `cur` points at the current
 * doc's header within PageGetContents(); the term entries follow it consecutively.
 * Initialize with bm25_pending_iter_begin(&it, page); advance with
 * bm25_pending_iter_next(&it) (returns false past the last doc); release with
 * bm25_pending_iter_end(&it) (currently a no-op -- the iterator holds no resources,
 * but callers MUST still call it so a future buffered variant stays source-compatible). */
typedef struct BM25PendingIter
{
    char                   *cur_ptr;    /* cursor into PageGetContents() */
    char                   *end_ptr;    /* page->pd_lower boundary */
    BM25PendingDocHeader   *cur;        /* current doc header, or NULL before first next() */
} BM25PendingIter;

#endif                          /* BM25_FORMAT_H */
