/* bm25_accum.c -- the in-memory drain accumulator (BM25Accum).
 *
 * Build input for seal and merge. Ingests documents (TID + per-field tokens),
 * assigns dense local doc-ids 0..N-1 in add order, and accumulates, per term, a
 * sorted postings list {local_docid, field_id, tf}, plus a per-doc {tid,
 * doclen_by_field[]}. The segment builder walks terms in lexicographic order and
 * emits block-compressed postings; bm25_accum_sort() establishes that order once.
 *
 * M5 field dimension (the SINGLE owner of the accumulator's field extension):
 * the posting KEY is (term, field_id) -- the same term in the same doc but a
 * different field is a DISTINCT posting (it contributes to df in that field).
 * doclen is tracked per (doc, field) in AccumDoc.doclen_by_field[]; sumdoclen is
 * tracked per field in total_len_by_field[]; ndocs_by_field[] counts docs that
 * HAVE each field (>=1 token) for the scorer's per-field N_field/avgdl_field.
 * WHAT doclen COUNTS is source WORD RUNS, not tokens -- see the contract on
 * bm25_accum_add_field_tokens, which is the single place it is computed.
 * A single-field accumulator (field_count == 1) is byte-for-byte the M3 layout:
 * one doclen_by_field[0], total_len_by_field[0] == the old scalar total_len.
 *
 * Term lookup is a dynahash keyed on the FULL term, (pointer, length) into the
 * AccumTerm's own copy of the bytes, hashed and compared over every byte -- exact for
 * every length, so there is no collision fallback. See AccumHashKey for why the two
 * fixed-width keys before it (#58, then #305) were each a way to choose collisions from
 * document text. Fast enough for seal-sized
 * batches; this is the scale fix for M0/M1's O(terms) linear builder. Within a
 * doc+field, repeated tokens increment tf on the term's current (doc,field)
 * posting in O(1) via a per-term "last touched (docid,field)" guard. Everything
 * is palloc'd in a dedicated context so bm25_accum_free is a single
 * MemoryContextDelete.
 *
 * BOUNDED SINCE #146. One accumulator no longer holds a whole build, merge input
 * set or pending drain: all three feeders measure it against
 * bm25_maintenance_budget_bytes and, when it is over, seal what it holds as a
 * segment and start a fresh one. That the whole thing lives in ONE context is what
 * makes the measurement cheap and complete -- see bm25_accum_over_budget. */
#include "postgres.h"

#include "bm25.h"
#include "common/hashfn.h"   /* hash_bytes -- the term map's hash */
#include "funcapi.h"
#include "miscadmin.h"        /* AmAutoVacuumWorkerProcess */
#include "postmaster/autovacuum.h"   /* autovacuum_work_mem (the AV-worker budget) */
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h" /* tuplestore_begin_heap/putvalues -- funcapi.h stopped
                                 * pulling this in transitively as of PG19 */

typedef struct AccumPosting
{
    uint32  local_docid;
    uint32  field_id;
    uint32  tf;
    /* M4 positions: the ascending per-(doc,field) 0-based token ordinals of every
     * occurrence of this term in this posting. npos == tf by construction (one
     * position recorded per token occurrence). Grown in a->cxt; NULL until the first
     * occurrence. The seg builder emits one [tf-count][deltapos x tf] POS frame from this.
     * Only the seal (add_field_tokens) path fills it; the merge path (add_posting)
     * leaves it NULL/0 (C-POS-MERGE replays positions separately). */
    uint32 *pos;
    uint32  npos;
    uint32  cappos;
} AccumPosting;

typedef struct AccumTerm
{
    char           *term;       /* palloc'd, termlen bytes (not NUL-terminated) */
    int             termlen;
    AccumPosting   *post;       /* ascending by (local_docid, field_id) */
    uint32          npost;
    uint32          cappost;
    uint32          last_docid; /* docid of the posting at post[npost-1], or UINT32_MAX */
    uint32          last_field; /* field_id of the posting at post[npost-1] (paired guard) */
} AccumTerm;

/*
 * dynahash key: the term itself, as (pointer, length), hashed with hash_bytes and
 * matched with a length check plus memcmp over the whole term (accum_key_hash /
 * accum_key_match). Two distinct terms never share an entry, at any length, so a
 * lookup is one probe and there is no fallback path.
 *
 * WHY NOT A FIXED-WIDTH KEY. The key used to be a 256-byte buffer, and both ways of
 * filling it let whoever supplies document text choose collisions. Until #58 a long
 * term was truncated to its first 255 bytes, so terms sharing a prefix collided by
 * construction. ADR 0076 then keyed long terms on a 32-bit hash_bytes of the full
 * term, on the premise that nobody can aim a hash collision -- but a birthday search
 * over ~300k candidate words finds colliding pairs in about a second of SQL (#305
 * PEND-02), so the linear fallback that resolved collisions was reachable on demand:
 * each later occurrence of the displaced term scanned terms[], O(occurrences x
 * nterms), inside a build, a seal (which holds the seal singleton, so INSERTs queue
 * behind it) or a merge. The full-term key is the "right destination" ADR 0076
 * itself named and deferred. It also shrinks every entry from 260 bytes to 24 on a
 * 64-bit build (PEND-06), which was most of a short term's footprint -- and dynahash
 * used to hash all 256 key bytes per lookup, where it now hashes termlen.
 *
 * POINTER LIFETIME. The pointer stored in an entry is the AccumTerm's own palloc'd
 * copy (t->term, in a->cxt, alive as long as the map), never the caller's token
 * buffer, which does not outlive the call; accum_find_or_add_term re-points it right
 * after HASH_ENTER. That is the difference from BM25PendKey in bm25_pending.c, whose
 * bytes live in caller arrays that outlive its HTAB. bm25_accum_sort's qsort moves
 * AccumTerm structs but never the bytes t->term points at, so the pointers survive
 * the sort; it is termidx that goes stale (see a->frozen).
 *
 * Residual: plain bucket-chain flooding. hash_bytes is unkeyed, so a caller who
 * supplies many terms with equal low hash bits lengthens one bucket chain. Each probe
 * there is a hash compare first and a memcmp only on equal hashes, the same exposure
 * every dynahash keyed on user text has in core, and nothing in this file can make it
 * worse than that.
 */
typedef struct AccumHashKey
{
    const char     *term;       /* NOT owned: points at an AccumTerm's term bytes */
    int             termlen;
} AccumHashKey;

typedef struct AccumHashEntry
{
    AccumHashKey    key;
    uint32          termidx;    /* index into BM25Accum.terms */
} AccumHashEntry;

typedef struct AccumDoc
{
    ItemPointerData tid;
    uint32         *doclen_by_field;    /* palloc0'd field_count wide, in a->cxt */
} AccumDoc;

struct BM25Accum
{
    MemoryContext   cxt;
    HTAB           *map;
    AccumTerm      *terms;
    uint32          nterms;
    uint32          capterms;
    AccumDoc       *docs;
    uint32          ndocs;
    uint32          capdocs;
    uint32          field_count;        /* 1 for a single-field (M3) accumulator */
    uint64         *total_len_by_field; /* sumdoclen per field; [field_count] wide */
    /* Sum of tf over every posting this accumulator holds -- the TOKEN count, which
     * since analyzer revision 5 (ADR 0087) is a strictly larger quantity than the sum
     * of total_len_by_field (the RUN count) for any dictionary that emits several
     * lexemes per word. Fed on both ingest shapes: bm25_accum_add_field_tokens adds
     * ntok (build/seal/drain), bm25_accum_add_posting adds tf (merge replay).
     * bm25_accum_add_positions_to_last deliberately does NOT add -- it decorates a
     * posting the merge path already counted. Sealed into the segment header so the
     * merge budget estimator can charge per token instead of per run (ADR 0088). */
    uint64          total_tokens;
    uint64         *ndocs_by_field;     /* live docs that HAVE each field (>=1 token) */
    /* M5 key_field: a dense fixed-width key[docid] byte array, capdocs*key_size wide,
     * grown in lockstep with docs[]. NULL (and key_type==NONE) for a keyless index
     * so the ctid-fallback path allocates nothing. */
    uint8           key_type;           /* BM25_KEY_* (NONE until set_keymeta) */
    uint16          key_size;           /* fixed key width in bytes (0 when NONE) */
    unsigned char  *keys;               /* [capdocs * key_size], or NULL when NONE */
    /* M4: per-field store_positions gate; [field_count] wide (1 = store frames for
     * that field). NULL until set_store_positions (treated as all-on). */
    uint8          *store_positions;
    /* ONE reusable set of parallel scratch arrays handed out by
     * bm25_accum_term_postings -- see the lifetime contract on that function. Grown
     * (never shrunk) to the largest term's posting count, so the scratch a build
     * retains is bounded by the BIGGEST TERM rather than by the total posting count.
     * They live in a->cxt, so bm25_accum_free's MemoryContextDelete reclaims them
     * with everything else; there is deliberately no explicit pfree. */
    uint32         *post_docids;
    uint32         *post_tfs;
    uint32         *post_fields;
    uint32          post_cap;           /* elements allocated in EACH of the three */
    bool            sorted;
    /* Set by bm25_accum_sort and never cleared: the qsort reorders terms[] without
     * rebuilding the map, so every entry's termidx is stale from then on. The
     * collision fallback used to paper over that (a stale hit failed its memcmp and
     * fell into the linear scan); with exact keys a stale hit would SUCCEED and hand
     * back another term's index, so accum_find_or_add_term refuses outright instead.
     * Every caller sorts exactly once, as its terminal step before the segment
     * build, so this never fires on a real path. */
    bool            frozen;
    /* PEND-12/BUILD-04 memory budget: bytes this context already held before the
     * first document was added -- the dynahash's bucket directory and segments
     * (hash_create is told 4096 entries, but for a table that size dynahash
     * allocates the buckets only, no element entries up front) plus the 1024-slot
     * terms/docs arrays. Measured at about 130 KB on a 64-bit build with the full-term
     * key (this comment used to say ~1.2 MB). NOT the keys[] array: set_keymeta runs
     * after this is captured, so on a keyed index its initial 1024 slots (at most
     * 16 KB) are charged as document data. That errs toward sealing early, which is
     * the harmless direction, and moving the capture would mean ordering the two
     * calls -- a constraint on every caller for 16 KB. bm25_accum_over_budget
     * charges only
     * what accrues ON TOP of it, so the budget reads as "document data" and a
     * maintenance_work_mem at its 1 MB legal floor does not fire the seal on
     * every single document. Real peak is therefore budget + this. */
    Size            baseline;
};

/* palloc on first growth, repalloc after. Caller has already switched to a->cxt.
 * For the SMALL, bounded arrays only -- see repalloc_or_alloc_huge for the ones
 * whose size scales with the corpus. */
static void *
repalloc_or_alloc(void *ptr, Size sz)
{
    return (ptr == NULL) ? palloc(sz) : repalloc(ptr, sz);
}

/*
 * The corpus-scaling growth sites, PEND-12.
 *
 * Plain palloc/repalloc cap at MaxAllocSize (1 GB) and fail with the anonymous
 * "invalid memory alloc request size N" -- a message naming no relation, no
 * operation and no tuning knob -- from inside a seal, which on the opportunistic
 * path is a user backend's INSERT. Four arrays here can legitimately cross that
 * ceiling: a->terms (40 B/term, ~26 M distinct terms), a hot term's t->post
 * (32 B/posting, ~33 M postings, reachable with stopwords disabled on a large
 * corpus), a->docs (~44 M docs), a->keys, and the postings scratch trio, which
 * inherits the largest term's posting count.
 *
 * THE MEMORY BUDGET DOES NOT MAKE THESE UNREACHABLE, which is the whole reason
 * both fixes are needed. Below a 1 GB budget the seal fires long before any single
 * array reaches the cliff (growth between checks is bounded: at most one document's
 * tokens on the build path -- itself capped at 65 535 by ADR 0079 -- and at most
 * one input segment on the merge path). But maintenance_work_mem legally and
 * commonly exceeds 1 GB, and there the budget permits exactly the residency these
 * calls have to express. So: the budget makes the cliff unreachable below 1 GB
 * budgets, the HUGE flag makes it unreachable above them.
 *
 * The per-posting pos[] list is deliberately NOT converted: npos == tf, and tf is
 * capped at 65 535 on both ingest paths (ADR 0079), so it tops out at 256 KB.
 *
 * This is the shape tuplesort uses for memtuples -- an array first allocated with
 * plain palloc and later grown with repalloc_huge -- so the mixed pairing is the
 * established idiom, not a novelty.
 */
static void *
repalloc_or_alloc_huge(void *ptr, Size sz)
{
    return (ptr == NULL) ? palloc_extended(sz, MCXT_ALLOC_HUGE)
                         : repalloc_huge(ptr, sz);
}

/*
 * Double a uint32 capacity, refusing the wrap rather than computing it.
 *
 * Every cap here doubles from a small power of two, so it reaches 2^31 and then
 * wraps to 0 -- after which the array is repalloc'd to zero bytes and every
 * subsequent write is out of bounds. Unreachable in practice (2^31 terms at 40 B
 * is ~86 GB of terms[] alone, i.e. a budget nobody sets), but the guard is two
 * instructions and turns a silent wrap into a named error.
 */
static uint32
accum_double_cap(uint32 cap, const char *what)
{
    if (cap > PG_UINT32_MAX / 2)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: accumulator %s array cannot grow beyond %u entries",
                        what, cap)));
    return cap * 2;
}

/*
 * bm25_maintenance_budget_bytes -- the memory ceiling a build, merge or drain
 * accumulates against before it seals a chunk and starts a fresh accumulator.
 *
 * autovacuum_work_mem when set and we ARE an autovacuum worker, else
 * maintenance_work_mem. That precedence is GIN's (ginInsertCleanup), and it is the
 * right one here for the same reason: the autovacuum worker is the process
 * BUILD-04 is actually about -- bm25_vacuumcleanup seals and then merges, both
 * through this accumulator -- and an installation that has configured a separate
 * AV budget means it.
 *
 * bm25_native.debug_budget overrides both. It exists because maintenance_work_mem's
 * own GUC floor is 1 MB, which sits above this accumulator's fixed baseline but
 * still forces test corpora into the tens of megabytes before a chunk boundary is
 * crossed; a 64 KB debug budget reaches every chunk path on a hundred-row corpus in
 * milliseconds. See its registration in _PG_init for why it is PGC_SUSET and why it
 * is not a tuning knob.
 */
Size
bm25_maintenance_budget_bytes(void)
{
    int kb;

    if (bm25_debug_budget_kb > 0)
        kb = bm25_debug_budget_kb;
    else if (AmAutoVacuumWorkerProcess() && autovacuum_work_mem != -1)
        kb = autovacuum_work_mem;
    else
        kb = maintenance_work_mem;

    return (Size) kb * 1024;
}

/*
 * bm25_accum_over_budget -- has this accumulator's DOCUMENT data passed `budget`?
 *
 * MemoryContextMemAllocated(recurse=true) over a->cxt is the complete measure:
 * every allocation this accumulator makes lands there -- the terms/docs/keys
 * arrays, each term's post[], each posting's pos[], the per-doc doclen vectors, the
 * term strings, the postings scratch, and the dynahash, which was created with
 * HASH_CONTEXT on a->cxt so recurse=true reaches its child context too. It counts
 * malloc'd blocks including allocator slack, which is the honest number for "will
 * this OOM" and the same accounting tuplesort budgets against.
 *
 * Minus the baseline, for the reason recorded on that field. A budget of 0 (the
 * caller could not derive one) disables the check rather than sealing every
 * document.
 *
 * Cheap enough to call per document: MemoryContextMemAllocated sums mem_allocated
 * over the context and its one child, a handful of pointer reads.
 */
bool
bm25_accum_over_budget(BM25Accum *a, Size budget)
{
    Size used;

    if (budget == 0)
        return false;
    used = MemoryContextMemAllocated(a->cxt, true);
    return used > a->baseline && (used - a->baseline) > budget;
}

/*
 * bm25_accum_estimate_bytes -- predict the accumulator residency of re-accumulating
 * one segment's live documents, from the fields its catalog entry already carries.
 *
 * The merge needs this BEFORE it commits to a set of inputs: chunking makes an
 * over-large set safe, but not useful, because a rung of budget-sized segments
 * merges into the same number of budget-sized segments, gets re-selected next pass,
 * and the forced merge loop never terminates. The selection trim uses this to keep
 * only sets whose merge can actually reduce the segment count.
 *
 * It lives here, not in bm25_merge.c, because it is a sum of THIS file's sizeof()s
 * and would drift the moment one of these structs gained a field. Nothing in it is
 * a baked byte count -- the lesson sql/83's header states about platform padding
 * applies to an estimator as much as to an assertion.
 *
 * The charges, in the order the accumulator incurs them:
 *
 *   postings: total_len is the segment's summed doclen, i.e. its count of source word
 *     runs, and a posting is created per distinct (term, doc, field). Positions add
 *     4 bytes per token (npos == tf summed over postings == the token count), and only
 *     when the index stores them -- multiplied onto the SAME total_len, so both halves
 *     scale with runs while the quantities they charge for scale with TOKENS.
 *
 *     THAT GAP IS NOW REACHABLE, and this note is the record of it rather than a
 *     prediction. Until analyzer revision 5 (issue #184, ADR 0087) the analyzer emitted
 *     one token per run, so total_len WAS the token count and both charges were exact.
 *     Revision 5 made position advance once per run, so for a dictionary that emits
 *     several lexemes per word -- in practice an ispell/hunspell compound splitter,
 *     the only such class this analyzer can currently be configured with --
 *     total_len is now smaller than the token count by that dictionary's lexemes-per-run
 *     ratio (about 5x for ispell_sample's 'footballklubber'), and both charges
 *     UNDER-estimate by it. Snowball English still emits one lexeme per run, so the
 *     default configuration is unaffected and no English index sees any change.
 *
 *     FIXED, by `stored_tokens` (ADR 0088). BM25SegCatEntry.total_tokens carries the
 *     segment's token count, named into the 4-byte hole the struct already had, so
 *     sizeof and the catalog's fixed stride are unchanged. The caller passes it only
 *     when BM25_FEAT_SEGCAT_TOKENS certifies the writer; otherwise it passes 0.
 *
 *     THE CLAMP IS Max(stored_tokens, total_len), NOT AN UNCONDITIONAL SWITCH, and the
 *     error direction below is the whole reason. Tokens >= runs by construction, so the
 *     Max degrades to exactly the pre-ADR-0088 arithmetic for every case where the count
 *     is missing or untrustworthy -- an unflagged index, a legacy entry reading 0, a
 *     uint32 saturated below the true value, or a tombstone-decayed entry whose integer
 *     floors rounded it under total_len. It can never charge LESS than this function
 *     charged before, which means no configuration gets a worse estimate than it had.
 *
 *     Charging TOKENS rather than POSTINGS follows the same logic one level down. A
 *     posting is per (term, doc, field), so a term repeated within one document folds
 *     into a single posting at a higher tf: postings <= tokens. Charging tokens
 *     therefore OVER-charges the sizeof(AccumPosting) term and lands EXACTLY on the
 *     4-bytes-per-position term. Charging postings would be exact on the larger term
 *     and under-count the smaller -- trading the cheap error for the expensive one. It
 *     also keeps the calibration continuous: the 1.6x actual/estimate ratio measured
 *     for BM25_ACCUM_SLACK_FACTOR was taken back when total_len WAS the token count.
 *
 *     Two routes stay rejected. Summing df over the segment's dict would get a posting
 *     count without new bytes, but this runs during SELECTION, over every candidate,
 *     before the merge has committed to reading anything -- an O(dict) read per
 *     candidate is exactly the cost the estimator exists to avoid paying. A blanket
 *     multiplier is worse than either: it would make every English index refuse merges
 *     that fit, to insure against a dictionary it is not using.
 *
 *     The consequence is bounded by the layering below, which is why this is a
 *     follow-up rather than a blocker: BM25_ACCUM_SLACK_FACTOR absorbs the first 2x,
 *     bm25_accum_over_budget still measures what is actually held, and the progress
 *     check still stops the force loop. What is left for a compound-dictionary index
 *     sitting at the budget floor is one no-op rewrite per autovacuum -- the same
 *     bounded cost the many-tiny-documents shape already carries below, not a return of
 *     the unbounded spin this estimator was written to prevent.
 *   terms: one dynahash entry, one AccumTerm, and the term string plus its initial
 *     8-posting array. AVG_TERM_ALLOC is the one number here that is a judgement
 *     rather than a sizeof, and it is deliberately generous.
 *   docs: an AccumDoc, its per-field doclen vector, and its key slot.
 *
 * Tombstoned documents are not replayed, and the charges above account for that
 * WITHOUT a live-fraction multiplier: the postings/positions input arrives already
 * decayed (the tombstone path shrinks the catalog entry in place), and the per-document
 * charge scales with live_ndocs directly. Only the per-term charge ignores tombstones,
 * which over-estimates -- the safe direction. Applying a live fraction on top of the
 * first of those discounted it twice; see the note at the charge itself.
 *
 * IT MUST PREDICT THE SAME QUANTITY THE CUT MEASURES, and the sum above does not:
 * bm25_accum_over_budget reads MemoryContextMemAllocated, i.e. malloc'd BLOCKS
 * including allocator slack, while the charges above are a sum of sizeof()s. Arrays
 * double and AllocSet blocks are rounded up, so actual residency runs above the raw
 * sum -- MEASURED, not assumed: 600 six-token documents sum to ~828 kB and actually
 * cut into 11 chunks at a 128 kB budget, i.e. ~1.3-1.4 MB, about 1.6x.
 *
 * Leaving the two on different scales was a real defect and not a rounding
 * inconvenience. The selection trim compares predicted chunks against input count,
 * so an estimate half the true size predicts half the chunks and declares an
 * INFEASIBLE set feasible -- and bm25_merge_execute computes progress only AFTER the
 * swap, so the whole index gets read, rewritten and retired before anything notices.
 * On a corpus at the budget floor that repeats on every autovacuum, forever, which is
 * exactly the regime the trim exists to prevent.
 *
 * BM25_ACCUM_SLACK_FACTOR closes the scale gap. It is deliberately ABOVE the measured
 * 1.6x, because the two directions cost differently: over-estimating makes the trim
 * refuse a merge that would have fit (cost: the segment count stays higher than
 * necessary until the operator raises maintenance_work_mem), while under-estimating
 * buys a full no-op rewrite of the index per vacuum. Err high.
 *
 * Residual (#305, PEND-06), not recalibrated: the 1.6x above predates the full-term
 * map key, which cut AccumHashEntry from 260 bytes to 24. The measured footprint fell
 * by MORE than that sizeof difference, because dynahash allocates elements in
 * batches sized from the entry (58 x 280 B before, 51 x 40 B now), so a merge chunk
 * no longer pays a 16 kB batch for its first few terms. On sql/105's term-heavy
 * fifty-document segments the raw sum is ~55 kB against ~43-51 kB measured, so the
 * factor of 2 now over-estimates by roughly 2x there. That is the safe direction
 * (the trim refuses some merges that would have fit); a term-light corpus still sits
 * near the old ratio.
 *
 * 2x is NOT a proof, and one corpus shape can still beat it: many tiny documents.
 * The per-doc charges below (AccumDoc, the field_count uint32 array, the key) are
 * summed as raw bytes, but each is a separate chunked allocation, so a one-field
 * one-token document pays ~16 bytes of real residency against ~4 bytes charged --
 * a per-doc ratio well over 2 that the per-posting and per-term terms, which
 * dominate on ordinary corpora, no longer dilute. The consequence is bounded, not
 * a return of the old defect: the progress check still stops the force loop, and
 * nothing carries a refusal across vacuums, so the cost is one no-op rewrite per
 * autovacuum rather than a spin. A measured actual/estimate ratio fed back from the
 * previous chunk run would remove the guess entirely; it is not worth a persistent
 * counter yet.
 *
 * It stays an estimate either way, which is why the safety net is layered: the
 * estimate decides what is worth ATTEMPTING, the budget check decides what is
 * actually held, and the progress check decides whether to try again.
 */
uint64
bm25_accum_estimate_bytes(uint64 ndocs, uint64 live_ndocs, uint64 total_len,
                          uint64 stored_tokens, uint32 nterms, uint32 field_count,
                          bool has_positions, uint16 key_size)
{
    /* Term string + its initial 8-slot postings array, averaged. Terms are short
     * (a stemmed English token is ~6 bytes) but every term pays for the postings
     * array whether it fills it or not. */
    const uint64 AVG_TERM_ALLOC = 16 + 8 * sizeof(AccumPosting);
    /* See the scale note in this function's header. Integer, applied last, so the
     * charges above stay readable as the byte counts they are. */
#define BM25_ACCUM_SLACK_FACTOR 2
    uint64  est;
    /* See the header: never below total_len, so a missing or untrustworthy count
     * reproduces the pre-ADR-0088 charge rather than under-charging. */
    uint64  tokens = Max(stored_tokens, total_len);

    if (ndocs == 0 || live_ndocs == 0)
        return 0;

    /* NO live-fraction scaling here, and that is a correction rather than an
     * omission. `tokens` is already scaled to live documents: the tombstone path
     * (bm25_livedocs_clear) decays the catalog entry's total_len -- and, since ADR
     * 0088, its total_tokens -- in place on every tombstone, in the same WAL record
     * that decrements live_ndocs. It is the ONLY tombstone path, so the value read
     * here has already had the dead documents taken out of it. Multiplying by
     * live_ndocs/ndocs on top of that discounted twice: measured on a 10-document
     * segment with one document tombstoned, the charge was for 540 x 0.9 = 486
     * tokens where 540 are actually replayed, and the error grew as live_ndocs fell.
     *
     * Under-charging is the expensive direction (see the slack-factor note below), so
     * this was the wrong way to be wrong. The double discount predated ADR 0088 --
     * that record mirrored the decay for total_tokens precisely so the two stayed
     * comparable, which kept the error proportional rather than fixing it.
     *
     * The decay itself is NOT removable, which is why the fix is on this side. The
     * entry's total_len is folded into the metapage's corpus-wide total_len
     * (bm25_segcat_publish_append/_swap) and that is avgdl's numerator, so a segment
     * that stopped decaying would leave dead documents' lengths in the scoring
     * statistic -- a wrong answer, in exchange for a bounded memory estimate.
     *
     * Residual, stated so it is not mistaken for exactness: the decay subtracts the
     * segment's AVERAGE doclen per tombstone, so if the deleted documents were longer
     * or shorter than average the value drifts, and it clamps at 0. Both are
     * pre-existing and neither is what live_frac was correcting -- it corrected
     * nothing, it only shrank an already-shrunk number.
     *
     * The other two charges are unaffected and were already right: nterms is the
     * segment's term count at seal and is never decayed (an over-estimate for a
     * tombstoned segment -- the safe direction), and the per-document charge scales
     * with live_ndocs directly. */
    est = (uint64) ((double) tokens *
                    (double) (sizeof(AccumPosting) + (has_positions ? 4 : 0)));
    est += (uint64) nterms * (sizeof(AccumHashEntry) + sizeof(AccumTerm) +
                              AVG_TERM_ALLOC);
    est += live_ndocs * (sizeof(AccumDoc) +
                         (uint64) field_count * sizeof(uint32) +
                         (uint64) key_size);
    return est * BM25_ACCUM_SLACK_FACTOR;
}

/* dynahash callbacks for AccumHashKey: hash and compare the whole term. keysize is
 * ignored (it is always sizeof(AccumHashKey)); the bytes are behind the pointer. */
static uint32
accum_key_hash(const void *key, Size keysize)
{
    const AccumHashKey *k = (const AccumHashKey *) key;

    return hash_bytes((const unsigned char *) k->term, k->termlen);
}

static int
accum_key_match(const void *key1, const void *key2, Size keysize)
{
    const AccumHashKey *k1 = (const AccumHashKey *) key1;
    const AccumHashKey *k2 = (const AccumHashKey *) key2;

    if (k1->termlen != k2->termlen)
        return 1;               /* unequal => nonzero, as dynahash expects */
    return memcmp(k1->term, k2->term, k1->termlen);
}

BM25Accum *
bm25_accum_begin_multi(uint32 field_count)
{
    MemoryContext   cxt = AllocSetContextCreate(CurrentMemoryContext,
                                                "bm25 accumulator",
                                                ALLOCSET_DEFAULT_SIZES);
    MemoryContext   old = MemoryContextSwitchTo(cxt);
    BM25Accum      *a = palloc0(sizeof(BM25Accum));
    HASHCTL         ctl;

    Assert(field_count >= 1);   /* checked: bm25_meta_validate bounds meta.field_count */
    a->cxt = cxt;
    a->field_count = field_count;
    a->total_len_by_field = palloc0(sizeof(uint64) * field_count);
    a->ndocs_by_field     = palloc0(sizeof(uint64) * field_count);
    a->capterms = 1024;
    a->terms = palloc(sizeof(AccumTerm) * a->capterms);
    a->capdocs = 1024;
    a->docs = palloc(sizeof(AccumDoc) * a->capdocs);

    /* No HASH_KEYCOPY: dynahash's default for a custom hash is memcpy of keysize
     * bytes, which copies the (pointer, length) pair -- exactly the shallow copy
     * wanted, since accum_find_or_add_term re-points it at the term's own bytes. */
    MemSet(&ctl, 0, sizeof(ctl));
    ctl.keysize = sizeof(AccumHashKey);
    ctl.entrysize = sizeof(AccumHashEntry);
    ctl.hash = accum_key_hash;
    ctl.match = accum_key_match;
    ctl.hcxt = cxt;
    a->map = hash_create("bm25 accum terms", 4096, &ctl,
                         HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

    /* Capture the fixed overhead AFTER everything above is allocated and BEFORE
     * any document arrives -- that is exactly the quantity the budget must not
     * charge for (see BM25Accum.baseline). */
    a->baseline = MemoryContextMemAllocated(cxt, true);

    MemoryContextSwitchTo(old);
    return a;
}

/* Single-field convenience: identical to the M3 accumulator. Its callers today are
 * the debug SRFs in this file (bm25_debug_accum, bm25_debug_accum_set_keymeta); the
 * build, drain and merge paths call bm25_accum_begin_multi directly and land on
 * this same layout whenever field_count == 1. */
BM25Accum *
bm25_accum_begin(void)
{
    return bm25_accum_begin_multi(1);
}

/* Caller MUST already be switched to a->cxt: this allocates the term string and
 * the initial postings array without switching context itself. All three callers
 * do -- bm25_accum_add_field_tokens (the seal path, which holds a->cxt for the
 * whole token loop), plus bm25_accum_add_posting and
 * bm25_accum_add_positions_to_last (the merge path), each of which switches on
 * entry and holds a->cxt until it returns. */
static uint32
accum_find_or_add_term(BM25Accum *a, const char *term, int termlen)
{
    AccumHashKey    key;
    AccumHashEntry *e;
    bool            found;

    if (unlikely(a->frozen))
        elog(ERROR, "bm25: accumulator term added after bm25_accum_sort");

    key.term = term;            /* the caller's bytes, for this probe only */
    key.termlen = termlen;
    e = (AccumHashEntry *) hash_search(a->map, &key, HASH_ENTER, &found);
    if (found)
        return e->termidx;      /* exact: accum_key_match compared every byte */

    /* New term. Between HASH_ENTER above and the two assignments at the end, the
     * entry points at the CALLER's bytes and its termidx is unset. An ERROR in that
     * window (the growth or the pallocs below) abandons the whole accumulator -- it
     * lives in the statement's memory and no caller catches and continues -- so the
     * half-made entry is never probed. */
    if (a->nterms == a->capterms)
    {
        a->capterms = accum_double_cap(a->capterms, "terms");
        a->terms = repalloc_huge(a->terms, sizeof(AccumTerm) * a->capterms);
    }
    {
        AccumTerm *t = &a->terms[a->nterms];
        t->term = palloc(termlen);
        memcpy(t->term, term, termlen);
        t->termlen = termlen;
        t->cappost = 8;
        t->post = palloc(sizeof(AccumPosting) * t->cappost);
        t->npost = 0;
        t->last_docid = PG_UINT32_MAX;
        t->last_field = PG_UINT32_MAX;
    }
    /* Re-point the stored key at the copy just made: same bytes, so the hash and
     * the bucket are unchanged, but now with the map's lifetime. */
    e->key.term = a->terms[a->nterms].term;
    e->termidx = a->nterms;
    return a->nterms++;
}

/* Register one doc by TID, assigning the next dense local id, and allocate its
 * per-field doclen vector (all zero until add_field_tokens fills a field). Does
 * NOT touch tokens; the caller adds each field's tokens via add_field_tokens. */
uint32
bm25_accum_add_doc_multi(BM25Accum *a, ItemPointer tid)
{
    MemoryContext   old = MemoryContextSwitchTo(a->cxt);
    uint32          docid = a->ndocs;

    if (a->ndocs == a->capdocs)
    {
        a->capdocs = accum_double_cap(a->capdocs, "docs");
        a->docs = repalloc_huge(a->docs, sizeof(AccumDoc) * a->capdocs);
        if (a->keys != NULL)
            a->keys = repalloc_huge(a->keys, (Size) a->capdocs * a->key_size);
    }
    a->docs[docid].tid = *tid;
    a->docs[docid].doclen_by_field = palloc0(sizeof(uint32) * a->field_count);
    if (a->keys != NULL)
        memset(a->keys + (Size) docid * a->key_size, 0, a->key_size);  /* NULL-key sentinel */
    a->ndocs++;
    a->sorted = false;

    MemoryContextSwitchTo(old);
    return docid;
}

/* Add one field's tokens to an already-registered doc, stamping field_id on every
 * new posting. The posting KEY is (local_docid, field_id): the (docid,field) guard
 * makes the same term in the same doc but a different field a distinct posting
 * (df counts it in that field). The field's sumlen and (when ntok>0) its ndocs are
 * accumulated alongside its doclen.
 *
 * DOCLEN IS THE SOURCE-RUN COUNT, i.e. max(token position) + 1, and 0 for a field with
 * no tokens. It is NOT ntok. Positions are per-(doc,field) ordinals stamped by
 * bm25_analyze, one per source word run, so this is the number of runs that emitted at
 * least one token -- the quantity BM25's length normalization is supposed to measure,
 * and the quantity core FTS's word distances count. It equals ntok exactly while the
 * analyzer emits one token per run, which is true of every analyzer revision to date;
 * an analyzer that co-positions a compound word's lexemes makes the two differ, and
 * this expression is the one that stays right.
 *
 * A MAX-SCAN, deliberately, not toks[ntok-1].pos + 1: this is the single doclen
 * producer for BOTH ingest paths, and the drain path (bm25_pending.c) rebuilds its
 * token array ENTRY BY ENTRY, so its tokens arrive in dictionary order, not position
 * order. "Last token" would silently read one term's last occurrence there. The merge
 * path does not come through here at all -- bm25_accum_add_doc_blank copies the
 * doclens already stored in the input segments' NORMS. */
void
bm25_accum_add_field_tokens(BM25Accum *a, uint32 local_docid,
                            uint32 field_id, BM25Token *toks, int ntok)
{
    MemoryContext   old = MemoryContextSwitchTo(a->cxt);
    int             i;
    uint32          doclen = 0;

    Assert(field_id < a->field_count);   /* checked: the index's own column map */
    for (i = 0; i < ntok; i++)
        if ((uint32) toks[i].pos + 1 > doclen)
            doclen = (uint32) toks[i].pos + 1;

    a->docs[local_docid].doclen_by_field[field_id] = doclen;
    a->total_len_by_field[field_id] += (uint64) doclen;
    /* Tokens, not runs: doclen above is max(position)+1 and this is the emitted
     * token count, and the two diverge exactly when a dictionary splits compounds. */
    a->total_tokens += (uint64) ntok;
    if (ntok > 0)
        a->ndocs_by_field[field_id]++;
    a->sorted = false;

    for (i = 0; i < ntok; i++)
    {
        uint32       ti = accum_find_or_add_term(a, toks[i].ptr, toks[i].len);
        AccumTerm   *t = &a->terms[ti];
        AccumPosting *pp;

        if (t->last_docid == local_docid && t->last_field == field_id)
        {
            pp = &t->post[t->npost - 1];
            pp->tf++;                       /* repeated token in same doc+field */
        }
        else
        {
            if (t->npost == t->cappost)
            {
                t->cappost = accum_double_cap(t->cappost, "postings");
                t->post = repalloc_huge(t->post, sizeof(AccumPosting) * t->cappost);
            }
            pp = &t->post[t->npost];
            pp->local_docid = local_docid;
            pp->field_id = field_id;
            pp->tf = 1;
            pp->pos = NULL;                 /* lazily grown below */
            pp->npos = 0;
            pp->cappos = 0;
            t->npost++;
            t->last_docid = local_docid;
            t->last_field = field_id;
        }

        /* M4: record this occurrence's position on the current posting. Positions
         * arrive ascending (toks[i].pos is a per-field 0-based ordinal), so the
         * list stays sorted; the builder delta-encodes it as-is. */
        if (pp->npos == pp->cappos)
        {
            pp->cappos = pp->cappos ? pp->cappos * 2 : 4;
            pp->pos = repalloc_or_alloc(pp->pos, sizeof(uint32) * pp->cappos);
        }
        pp->pos[pp->npos++] = (uint32) toks[i].pos;
    }

    MemoryContextSwitchTo(old);
}

/* Single-field convenience: register the doc and add its tokens under field 0.
 * Preserves the exact M3 semantics for every single-field caller. */
uint32
bm25_accum_add_doc(BM25Accum *a, ItemPointer tid, BM25Token *toks, int ntok)
{
    uint32 docid = bm25_accum_add_doc_multi(a, tid);
    bm25_accum_add_field_tokens(a, docid, 0, toks, ntok);
    return docid;
}

/* Merge-path doc registration: assign the next dense local id for a doc whose
 * per-field doclens are already known (read from the source segment's per-field
 * NORMS), WITHOUT a token list. Postings for the doc are attached afterward via
 * bm25_accum_add_posting (which carries field_id). Copies field_count doclens into
 * the doc's doclen_by_field[] and accumulates each into total_len_by_field[] /
 * ndocs_by_field[]. The caller's field_count MUST equal the accumulator's. */
uint32
bm25_accum_add_doc_blank(BM25Accum *a, ItemPointer tid,
                         const uint32 *doclen_by_field, uint32 field_count)
{
    MemoryContext   old = MemoryContextSwitchTo(a->cxt);
    uint32          docid = a->ndocs;
    uint32          f;

    Assert(field_count == a->field_count);   /* checked: one meta.field_count feeds both */
    if (a->ndocs == a->capdocs)
    {
        a->capdocs = accum_double_cap(a->capdocs, "docs");
        a->docs = repalloc_huge(a->docs, sizeof(AccumDoc) * a->capdocs);
        if (a->keys != NULL)
            a->keys = repalloc_huge(a->keys, (Size) a->capdocs * a->key_size);
    }
    a->docs[docid].tid = *tid;
    a->docs[docid].doclen_by_field = palloc0(sizeof(uint32) * a->field_count);
    if (a->keys != NULL)
        memset(a->keys + (Size) docid * a->key_size, 0, a->key_size);  /* NULL-key sentinel */
    for (f = 0; f < field_count; f++)
    {
        a->docs[docid].doclen_by_field[f] = doclen_by_field[f];
        a->total_len_by_field[f] += (uint64) doclen_by_field[f];
        if (doclen_by_field[f] > 0)
            a->ndocs_by_field[f]++;
    }
    a->ndocs++;
    a->sorted = false;

    MemoryContextSwitchTo(old);
    return docid;
}

/* Merge-path posting append: attach one (term, field_id, tf) posting to an
 * already-registered doc (local_docid from a prior add_doc_blank). The merge feeds
 * source postings one per (term, doc, field), so each call is a NEW posting for the
 * term in that field; df (== npost) therefore stays correct without per-doc dedup.
 * The (last_docid, last_field) guard is still honored (a defensive O(1) merge of a
 * same-(doc,field) repeat into the current posting's tf) so a hypothetical
 * duplicate feed cannot inflate df. */
void
bm25_accum_add_posting(BM25Accum *a, const char *term, int termlen,
                       uint32 field_id, uint32 local_docid, uint32 tf)
{
    MemoryContext   old = MemoryContextSwitchTo(a->cxt);
    uint32          ti = accum_find_or_add_term(a, term, termlen);
    AccumTerm      *t = &a->terms[ti];

    /* Real check, not an Assert: merge_post_cb (bm25_merge.c) reaches here with a
     * field_id decoded off a source segment, and the field-RLE decode bounds it
     * only by BM25_MAX_FIELDS while the per-doc arrays this feeds are field_count wide.
     * See the fuller note in bm25_accum_add_positions_to_last, which the same
     * corrupt segment reaches one callback later. */
    if (field_id >= a->field_count)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: posting field %u exceeds field count %u: "
                        "corrupt segment field ids",
                        field_id, a->field_count)));
    a->sorted = false;
    /* Counted BEFORE the repeat-merge branch below, which returns early: that branch
     * folds tf into an existing posting, so its tokens are just as real as a new
     * posting's and must not be lost. */
    a->total_tokens += (uint64) tf;
    if (t->last_docid == local_docid && t->last_field == field_id)
    {
        t->post[t->npost - 1].tf += tf;     /* defensive: same-(doc,field) repeat */
        MemoryContextSwitchTo(old);
        return;
    }
    if (t->npost == t->cappost)
    {
        t->cappost = accum_double_cap(t->cappost, "postings");
        t->post = repalloc_huge(t->post, sizeof(AccumPosting) * t->cappost);
    }
    t->post[t->npost].local_docid = local_docid;
    t->post[t->npost].field_id = field_id;
    t->post[t->npost].tf = tf;
    /* Merge path does not carry positions here (C-POS-MERGE replays them). Init the
     * list empty so the builder's per-posting frame emit is well-defined. */
    t->post[t->npost].pos = NULL;
    t->post[t->npost].npos = 0;
    t->post[t->npost].cappos = 0;
    t->npost++;
    t->last_docid = local_docid;
    t->last_field = field_id;

    MemoryContextSwitchTo(old);
}

/* Merge-path position replay: attach `npos` ascending 0-based positions to the
 * posting most recently created by bm25_accum_add_posting for (term, field_id).
 * The merge feeds one posting then (immediately after) its positions -- mirroring
 * the reader's cb-then-pos_cb lockstep -- so "last posting" is always the correct
 * target. Enforces that lockstep -- the term HAS a last posting, that posting is for
 * this field, and npos == its tf (the D2 invariant) -- and treats any violation as
 * source-segment corruption, not as an Assert. The seal path grows
 * pos incrementally in add_field_tokens; here the full list arrives at once, so we
 * size cappos exactly. Positions are copied into a->cxt (the caller's buffer is
 * transient reader scratch). Idempotent-safe against the add_posting same-(doc,field)
 * merge: because that path folds a repeat into the current posting's tf WITHOUT a
 * matching second pos_cb (the reader emits one frame per posting), replay only ever
 * lands one position list per posting. */
void
bm25_accum_add_positions_to_last(BM25Accum *a, const char *term, int termlen,
                                 uint32 field_id, const uint32 *positions,
                                 uint32 npos)
{
    MemoryContext   old = MemoryContextSwitchTo(a->cxt);
    uint32          ti = accum_find_or_add_term(a, term, termlen);
    AccumTerm      *t = &a->terms[ti];
    AccumPosting   *pp;

    /* Same reasoning as the three checks below, and reached the same way: the
     * merge replay (bm25_merge.c merge_pos_cb) hands us a field_id decoded off a
     * source segment's field RLE, whose decode bound is BM25_MAX_FIELDS while
     * AccumDoc.doclen_by_field is only field_count wide. An Assert here compiles
     * out in production, and the out-of-range id then indexes that narrower array
     * downstream in bm25_seg_build.c -- an OOB read that bakes garbage doclens into
     * the merged segment's norms. Silent corruption propagation is strictly worse
     * than the clean error the checks below aim for, so this is a real check too.
     *
     * Enumerated per ADR 0040 rather than fixing only the site the issue named.
     * The other three Assert(field_id < a->field_count) sites in this file are
     * deliberately left as Asserts: bm25_accum_add_field_tokens takes a field_id
     * resolved from the index's own column mapping, and bm25_accum_ndocs_field /
     * bm25_accum_doc_len_field take the builder's own loop variable -- all three
     * are caller-controlled and already bounded by field_count, never on-disk. */
    if (field_id >= a->field_count)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: position replay field %u exceeds field count %u: "
                        "corrupt segment field ids",
                        field_id, a->field_count)));

    /* All three checks below guard the SAME corruption class -- a source segment
     * whose POS stream does not line up with its POST stream -- so all three are
     * real ereports on ERRCODE_INDEX_CORRUPTED (the unified SQLSTATE, matching the
     * mirror-image lockstep check on the read side in bm25_seg_chain.c), not
     * Asserts and not a bare elog. An Assert is compiled out without
     * --enable-cassert, i.e. in every production build, which is precisely where a
     * corrupt segment shows up.
     *
     * The npost == 0 case in particular MUST be caught before the subscript:
     * accum_find_or_add_term CREATES the term with npost == 0 when the POS stream
     * names a term the POST stream never posted, and npost is uint32, so
     * `t->npost - 1` is 4294967295 -- &t->post[4294967295] is ~137 GB past the
     * array, and the tf comparison below would read through it. */
    if (t->npost == 0)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: position replay for a term with no posting "
                        "(field %u, npos %u): position stream desync",
                        field_id, npos)));

    pp = &t->post[t->npost - 1];

    /* The last posting must be the one just added for this (doc,field): the reader
     * emits post_cb then pos_cb in lockstep for the same posting. */
    if (pp->field_id != field_id)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: position replay field %u != last posting field %u "
                        "(local_docid %u): position stream desync",
                        field_id, pp->field_id, pp->local_docid)));

    /* D2: one position per token occurrence, so npos must equal the posting's tf. */
    if (pp->tf != npos)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: position replay npos %u != posting tf %u "
                        "(local_docid %u field %u): position stream desync",
                        npos, pp->tf, pp->local_docid, field_id)));

    if (npos > 0)
    {
        pp->cappos = npos;
        pp->pos = palloc(sizeof(uint32) * npos);
        memcpy(pp->pos, positions, sizeof(uint32) * npos);
        pp->npos = npos;
    }
    MemoryContextSwitchTo(old);
}

void
bm25_accum_free(BM25Accum *a)
{
    MemoryContextDelete(a->cxt);    /* frees terms, postings, docs, hashtable */
}

/* ---- read-side accessors (segment builder, Task 5) ---- */

static int
accum_term_cmp(const void *pa, const void *pb)
{
    const AccumTerm *ta = (const AccumTerm *) pa;
    const AccumTerm *tb = (const AccumTerm *) pb;
    int n = Min(ta->termlen, tb->termlen);
    int c = memcmp(ta->term, tb->term, n);
    if (c != 0)
        return c;
    return ta->termlen - tb->termlen;
}

/* TERMINAL operation: qsort reorders terms[] but does NOT rebuild the hash, so
 * every AccumHashEntry.termidx is left stale, and a->frozen makes any later term
 * insert an ERROR rather than a silent misfile (see that field). The seal/merge flow
 * only ever sorts once, after all docs are ingested, and then frees the
 * accumulator. */
void
bm25_accum_sort(BM25Accum *a)
{
    a->frozen = true;
    if (a->sorted)
        return;
    qsort(a->terms, a->nterms, sizeof(AccumTerm), accum_term_cmp);
    a->sorted = true;       /* postings are already ascending by construction */
}

uint32
bm25_accum_ndocs(BM25Accum *a)
{
    return a->ndocs;
}

uint32
bm25_accum_nterms(BM25Accum *a)
{
    return a->nterms;
}

uint32
bm25_accum_field_count(BM25Accum *a)
{
    return a->field_count;
}

/* Corpus-wide sumdoclen: the sum over all fields. Kept as a derived sum so the
 * legacy single-field callers (metapage global total_len) read the same value
 * they did in M3 (where total_len_by_field[0] == total_len). */
uint64
bm25_accum_total_len(BM25Accum *a)
{
    uint64 sum = 0;
    uint32 f;
    for (f = 0; f < a->field_count; f++)
        sum += a->total_len_by_field[f];
    return sum;
}

/* Corpus-wide token count (sum of tf over all postings). Sibling of
 * bm25_accum_total_len, and deliberately a DIFFERENT number since analyzer
 * revision 5: that one counts source runs, this counts the tokens they emitted.
 * They coincide for any one-lexeme-per-run dictionary, which is every Snowball
 * language -- so an English-only build cannot tell these two getters apart, and a
 * test that needs to must use a compound dictionary (ADR 0087, sql/107). */
uint64
bm25_accum_total_tokens(BM25Accum *a)
{
    return a->total_tokens;
}

/* Per-field sumdoclen (>= field_count entries in out). */
void
bm25_accum_total_len_by_field(BM25Accum *a, uint64 *out)
{
    uint32 f;
    for (f = 0; f < a->field_count; f++)
        out[f] = a->total_len_by_field[f];
}

/* Live docs that HAVE field_id (>=1 token in that field) -- the scorer's N_field. */
uint64
bm25_accum_ndocs_field(BM25Accum *a, uint32 field_id)
{
    Assert(field_id < a->field_count);   /* checked: the builder's own f loop bound */
    return a->ndocs_by_field[field_id];
}

/* Per-field N_field as an array (>= field_count entries in out). Sibling of
 * bm25_accum_total_len_by_field; the segment builder serializes both into the
 * header so the BM25F scorer can compute avgdl_field = sumlen_field / N_field. */
void
bm25_accum_ndocs_by_field(BM25Accum *a, uint64 *out)
{
    uint32 f;
    for (f = 0; f < a->field_count; f++)
        out[f] = a->ndocs_by_field[f];
}

/* Per-(doc,field) doclen; the builder writes the per-field NORMS from this. */
uint32
bm25_accum_doc_len_field(BM25Accum *a, uint32 docid, uint32 field_id)
{
    Assert(field_id < a->field_count);   /* checked: the builder's own f loop bound */
    return a->docs[docid].doclen_by_field[field_id];
}

const char *
bm25_accum_term(BM25Accum *a, uint32 i, int *termlen, uint32 *df)
{
    *termlen = a->terms[i].termlen;
    /* df = the number of (doc, field) postings for the term (see the field
     * dimension note in the file header): the same term in two fields of one doc
     * is two postings, so this equals the document count only when
     * field_count == 1. */
    *df = a->terms[i].npost;
    return a->terms[i].term;
}

/* Split sorted-term i's postings into the three parallel uint32 streams the segment
 * builder encodes (AccumPosting interleaves them). The parallel field_ids[] is what
 * lets the builder emit the per-block field-id RLE alongside the docid/tf streams.
 *
 * ==========================================================================
 * LIFETIME CONTRACT -- READ THIS BEFORE ADDING A CALLER
 * ==========================================================================
 * The three returned arrays are SCRATCH OWNED BY THE ACCUMULATOR, not fresh
 * allocations. THEY ARE INVALIDATED BY THE NEXT bm25_accum_term_postings CALL on
 * the same accumulator: that call overwrites them in place, and a longer term also
 * repallocs them to a different address. Consume them -- or copy out what you need --
 * before asking for another term. Holding term i's docids[] across the call for
 * term i+1 does not fault; it silently reads term i+1's postings, which is exactly
 * the kind of bug that survives single-field tests. Every caller today consumes the
 * arrays entirely within one iteration of its own per-term loop, and the builder's
 * two passes (POST, then POS) are sequential, so nothing aliases.
 *
 * Why scratch rather than a per-call allocation: these arrays used to be palloc'd
 * into a->cxt on every call, and a->cxt is not freed until bm25_accum_free at the
 * very END of the build. That retained 12 bytes of pure scratch per posting per
 * pass -- and the segment builder makes TWO full passes over all nterms
 * (bm25_seg_build.c: the POST pass, then the POS pass whenever any field stores
 * positions, which is the default), so 24 bytes/posting held for the whole build,
 * piled on an accumulator that was, at the time, unbounded by maintenance_work_mem
 * altogether. One reused buffer bounds the scratch at the single largest term
 * instead -- and it still matters now that the accumulator IS budget-bounded: the
 * budget is measured over this same context, so per-call scratch would have been
 * charged against it and would have made the chunks smaller for no reason.
 *
 * The arrays are never NULL, even for a zero-posting term (capacity floors at 1
 * element): bm25_seg_build.c's POST loop documents and relies on that. */
void
bm25_accum_term_postings(BM25Accum *a, uint32 i,
                         const uint32 **docids, const uint32 **tfs,
                         const uint32 **field_ids, uint32 *n)
{
    AccumTerm  *t = &a->terms[i];
    uint32      need = Max(t->npost, 1u);
    uint32      j;

    if (need > a->post_cap)
    {
        MemoryContext old = MemoryContextSwitchTo(a->cxt);

        /* Grow-only: capacity converges on the largest term after a few terms, so
         * the steady state is zero allocation per call. post_cap is advanced only
         * after all three succeed -- a repalloc that ereports leaves the smaller
         * (still accurate) capacity behind, and the next call simply re-grows. */
        a->post_docids = repalloc_or_alloc_huge(a->post_docids, sizeof(uint32) * need);
        a->post_tfs    = repalloc_or_alloc_huge(a->post_tfs, sizeof(uint32) * need);
        a->post_fields = repalloc_or_alloc_huge(a->post_fields, sizeof(uint32) * need);
        a->post_cap = need;

        MemoryContextSwitchTo(old);
    }

    for (j = 0; j < t->npost; j++)
    {
        a->post_docids[j] = t->post[j].local_docid;
        a->post_tfs[j]    = t->post[j].tf;
        a->post_fields[j] = t->post[j].field_id;
    }
    *docids = a->post_docids;
    *tfs = a->post_tfs;
    *field_ids = a->post_fields;
    *n = t->npost;
}

/* M4: the position list of the j-th posting of sorted term i (parallel to the
 * arrays bm25_accum_term_postings returns). Points into the accumulator context;
 * *npos == the posting's tf (or 0 for a merge-replayed posting that carries none). */
const uint32 *
bm25_accum_posting_positions(BM25Accum *a, uint32 i, uint32 j, uint32 *npos)
{
    AccumPosting *pp = &a->terms[i].post[j];
    *npos = pp->npos;
    return pp->pos;
}

ItemPointer
bm25_accum_doc_tid(BM25Accum *a, uint32 d)
{
    return &a->docs[d].tid;
}

/* M5 key_field: fix the key type/width and allocate the dense keys[] buffer sized to
 * the current capdocs. Called ONCE by the builder/merge before any doc is added, so
 * every subsequent add_doc / add_doc_blank has a keys[] slot to fill and to grow in
 * lockstep. key_type==NONE is a no-op (keys stays NULL -> ctid fallback). */
void
bm25_accum_set_keymeta(BM25Accum *a, uint8 key_type, uint16 key_size)
{
    /* key_size arrives here from an on-disk value in the two callers that carry a
     * trust boundary -- the pending page's BM25PendingDocHeader
     * (bm25_pending_drain, dh->key_size) and a source segment's KEYMAP header (the
     * merge path) -- neither of which has bounded it against this accumulator.
     * (CREATE INDEX calls this too, but with a width derived from the key column's
     * type OID in bm25_key_type_from_oid, bounded by construction.)
     * bm25_accum_set_doc_key's equality check only
     * proves LATER docs agree with the FIRST key_size this function was ever
     * called with; a uniformly-corrupt key_size sails through that check and
     * still sizes the palloc0 below and every memcpy stride off it (a->keys +
     * docid*key_size). Bound it here, the one call both callers share, before it
     * becomes an allocation size. */
    if (key_type != BM25_KEY_NONE && (key_size == 0 || key_size > BM25_KEY_MAX_SIZE))
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: key_size %u is invalid (maximum is %d)",
                        key_size, BM25_KEY_MAX_SIZE)));

    a->key_type = key_type;
    a->key_size = key_size;
    if (key_type != BM25_KEY_NONE && a->keys == NULL)
    {
        MemoryContext old = MemoryContextSwitchTo(a->cxt);
        /* palloc0 so any docid registered but never keyed reads as the all-zero
         * sentinel. That sentinel is SEALED as key 0, not as a per-doc ctid
         * fallback: this accumulator is keyed, so bm25_keymap_write emits a real
         * KEYMAP and every docid in it -- sentinel or not -- resolves through
         * bm25_seg_key. A NULL key and a genuine 0 are indistinguishable on disk
         * (there is no null flag in the keymap); see bm25_build.c's key_field
         * block. Ctid fallback belongs to a KEYLESS accumulator, where key_type is
         * NONE, keys stays NULL and keymap_root is left Invalid. */
        a->keys = palloc0((Size) a->capdocs * key_size);
        MemoryContextSwitchTo(old);
    }
}

uint8
bm25_accum_key_type(BM25Accum *a)
{
    return a->key_type;
}

uint16
bm25_accum_key_size(BM25Accum *a)
{
    return a->key_size;
}

/* Decode-boundary probe (trust-boundary review, 2026-08): bm25_accum_set_keymeta's
 * key_size bound only fires on an already-corrupt on-disk key_size (a pending
 * doc header or a source segment's KEYMAP header), which a regression suite
 * cannot produce -- the real key types this accumulator ever sees from a valid
 * writer (int4/int8/uuid/text) never exceed BM25_KEY_MAX_SIZE. This drives the
 * real function directly through a throwaway one-field accumulator, so the
 * check under test is the one production code runs, not a reimplementation.
 * TEST-ONLY: no relation touched, no persistent state (the accumulator's
 * context is torn down here regardless of outcome via bm25_accum_free; on the
 * ERROR path it is reclaimed when the aborted statement's memory context
 * resets, same as every other bm25_debug_* probe that allocates before
 * erroring). */
PG_FUNCTION_INFO_V1(bm25_debug_accum_set_keymeta);
Datum
bm25_debug_accum_set_keymeta(PG_FUNCTION_ARGS)
{
    int32      key_type = PG_GETARG_INT32(0);
    int32      key_size = PG_GETARG_INT32(1);
    BM25Accum *a;

    if (key_type < 0 || key_type > PG_UINT8_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_accum_set_keymeta: key_type out of uint8 range")));
    if (key_size < 0 || key_size > PG_UINT16_MAX)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_accum_set_keymeta: key_size out of uint16 range")));

    a = bm25_accum_begin();
    bm25_accum_set_keymeta(a, (uint8) key_type, (uint16) key_size);
    bm25_accum_free(a);
    PG_RETURN_INT32(key_size);
}

/* Trust-boundary probe for the merge-replay field_id bounds (ADR 0040
 * enumeration, 2026-08). bm25_field_rle_decode bounds a decoded field id to
 * BM25_MAX_FIELDS -- correct for the scorer's BM25_MAX_FIELDS-wide arrays -- but this
 * accumulator's per-doc arrays are only field_count wide, so an id in
 * [field_count, BM25_MAX_FIELDS) is precisely the gap the two checks under test
 * close. Only a corrupt source segment produces one, which a regression suite
 * cannot manufacture, so this drives the REAL functions over caller-supplied
 * values exactly as bm25_debug_accum_set_keymeta does for the key-width bound.
 *
 * which: 0 = bm25_accum_add_posting, 1 = bm25_accum_add_positions_to_last.
 *
 * For which=1 a valid posting is seeded first, at field_id itself when that is
 * in range and at field 0 otherwise. That keeps the two directions clean: an
 * in-range call passes the bound AND the downstream (field, tf) lockstep checks,
 * while an out-of-range call cannot be seeded at its own field (add_posting
 * would reject it first) and so reaches the bound under test with npost > 0 --
 * proving the bound fires on its own merit rather than being masked by the
 * npost == 0 desync check that sits below it.
 *
 * TEST-ONLY: no relation is touched and no persistent state is created. The
 * accumulator's context is torn down here on success; on the ERROR path it is
 * reclaimed when the aborted statement's context resets, as with every other
 * bm25_debug_ probe that allocates before erroring. */
PG_FUNCTION_INFO_V1(bm25_debug_accum_field_bound);
Datum
bm25_debug_accum_field_bound(PG_FUNCTION_ARGS)
{
    int32           field_count = PG_GETARG_INT32(0);
    int32           field_id    = PG_GETARG_INT32(1);
    int32           which       = PG_GETARG_INT32(2);
    BM25Accum      *a;
    ItemPointerData tid;
    uint32          doclens[BM25_MAX_FIELDS];
    uint32          positions[1];
    uint32          seed_field;
    int32           f;

    if (field_count < 1 || field_count > BM25_MAX_FIELDS)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_accum_field_bound: field_count must be in [1, %d]",
                        BM25_MAX_FIELDS)));
    if (field_id < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_accum_field_bound: field_id must be non-negative")));
    if (which != 0 && which != 1)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_accum_field_bound: which must be 0 or 1")));

    a = bm25_accum_begin_multi((uint32) field_count);
    for (f = 0; f < field_count; f++)
        doclens[f] = 1;
    ItemPointerSet(&tid, 1, 1);
    (void) bm25_accum_add_doc_blank(a, &tid, doclens, (uint32) field_count);

    if (which == 0)
        bm25_accum_add_posting(a, "t", 1, (uint32) field_id, 0, 1);
    else
    {
        seed_field = (field_id < field_count) ? (uint32) field_id : 0;
        bm25_accum_add_posting(a, "t", 1, seed_field, 0, 1);
        positions[0] = 0;
        bm25_accum_add_positions_to_last(a, "t", 1, (uint32) field_id,
                                         positions, 1);
    }

    bm25_accum_free(a);
    PG_RETURN_INT32(field_id);
}

/* M4: copy the per-field store_positions bits into the accumulator context (so they
 * outlive the caller's stack array). NULL bits leaves the accumulator all-on. */
void
bm25_accum_set_store_positions(BM25Accum *a, const uint8 *bits)
{
    MemoryContext old;

    if (bits == NULL)
        return;
    old = MemoryContextSwitchTo(a->cxt);
    a->store_positions = palloc(a->field_count);
    memcpy(a->store_positions, bits, a->field_count);
    MemoryContextSwitchTo(old);
}

const uint8 *
bm25_accum_store_positions(BM25Accum *a)
{
    return a->store_positions;
}

/* Stash one doc's key bytes at keys[docid*key_size]. Overwrites the zero sentinel
 * add_doc* wrote.
 *
 * A NULL key VALUE does not mean this is skipped, and skipping it does not produce
 * a ctid fallback. The three callers differ:
 *   - bm25_pending.c's drain_doc_flush calls this for EVERY row of a keyed index,
 *     NULL-valued or not: the pending record already carries the zero sentinel
 *     bm25_insert wrote, and sealing it here bakes in key 0.
 *   - bm25_merge.c's per-doc copy calls it for every source doc, given a keyed
 *     acc and a source segment that has a keymap -- again including a source doc
 *     whose stored key is the sentinel.
 *   - bm25_build.c's callback is the only one that skips, on `!isnull[key_attno]`.
 *     The sentinel then survives -- and is still sealed as key 0, because this
 *     accumulator is keyed and bm25_keymap_write emits a real KEYMAP for it.
 * Ctid fallback is a SEGMENT-level property (key_type NONE -> keys NULL ->
 * keymap_root Invalid -> bm25_seg_key returns false), never a per-doc one: with a
 * valid keymap_root, bm25_seg_key resolves every docid or raises
 * ERRCODE_INDEX_CORRUPTED. */
void
bm25_accum_set_doc_key(BM25Accum *a, uint32 local_docid,
                       const unsigned char *key, uint16 key_size)
{
    Assert(a->keys != NULL);   /* checked: callers gate on key_type != BM25_KEY_NONE */
    /* key_size is fixed for the WHOLE accumulator by the FIRST keyed header
     * bm25_pending_drain sees (bm25_accum_set_keymeta, called once per drain).
     * This was an Assert-only guard, so in a production (non-cassert) build a
     * later doc whose key_size disagrees would silently memcpy key_size bytes
     * into a slot sized a->key_size -- a heap overflow whenever key_size >
     * a->key_size. REACHABLE, not just theoretical: bm25_resolve_fields
     * re-resolves the key_field reloption on every aminsert, so an
     * ALTER INDEX ... SET (key_field=...) between inserts feeds mixed-width
     * keys into the same drain pass. This is a DDL-reachable schema change, not
     * on-disk corruption -- ordinary reloption ALTERs are accepted by the
     * catalog with no cross-check against pending writes already queued under
     * the old key_field, so FEATURE_NOT_SUPPORTED (not INDEX_CORRUPTED) is the
     * honest errcode, and the hint points at the actual fix (REINDEX) rather
     * than sending the operator hunting for storage corruption.
     *
     * #292: for the pending drain this is now a backstop, not the first line.
     * The insert gate refuses a changed key_field on every stamped index, and
     * bm25_pending_drain refuses a chain whose records disagree on (type, width)
     * before any of them reaches this call -- a superset of this width check. It
     * stays because it is what keeps the memcpy below in bounds for ANY caller. */
    if (key_size != a->key_size)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: key_field width changed (%u vs %u bytes) since this "
                        "index's pending writes began",
                        key_size, a->key_size),
                 errhint("REINDEX the index; changing key_field after build is not supported.")));
    memcpy(a->keys + (Size) local_docid * a->key_size, key, key_size);
}

/* Base of the dense keys[] array (index it as base + docid*key_size), or NULL for a
 * keyless accumulator. bm25_segment_build_orphans passes doc_key(a,0) to the writer. */
const unsigned char *
bm25_accum_doc_key(BM25Accum *a, uint32 local_docid)
{
    if (a->keys == NULL)
        return NULL;
    return a->keys + (Size) local_docid * a->key_size;
}

/* Corpus-wide per-doc length: the sum over the doc's fields. Preserves the old
 * single-field semantics for any surviving whole-doc caller. */
uint32
bm25_accum_doc_len(BM25Accum *a, uint32 d)
{
    uint32 sum = 0;
    uint32 f;
    for (f = 0; f < a->field_count; f++)
        sum += a->docs[d].doclen_by_field[f];
    return sum;
}

/* ---- debug SRF (drives the accumulator from a text[] of documents) ---- */
PG_FUNCTION_INFO_V1(bm25_debug_accum);
Datum
bm25_debug_accum(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    ArrayType      *arr = PG_GETARG_ARRAYTYPE_P(0);
    Datum          *elems;
    bool           *nullp;
    int             nelems;
    BM25Accum      *a;
    TupleDesc       tupdesc;
    Tuplestorestate *ts;
    int             i;
    uint32          ti;

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

    deconstruct_array_builtin(arr, TEXTOID, &elems, &nullp, &nelems);

    a = bm25_accum_begin();
    for (i = 0; i < nelems; i++)
    {
        text       *doc;
        BM25Token  *toks;
        int         ntok;
        ItemPointerData tid;

        if (nullp[i])
            continue;
        doc = DatumGetTextPP(elems[i]);
        ntok = bm25_tokenize(VARDATA_ANY(doc), VARSIZE_ANY_EXHDR(doc), &toks);
        /* synthetic TID just so the docmap has a value; doc order = local docid */
        ItemPointerSet(&tid, 1, (OffsetNumber) (i + 1));
        bm25_accum_add_doc(a, &tid, toks, ntok);
    }
    bm25_accum_sort(a);

    for (ti = 0; ti < bm25_accum_nterms(a); ti++)
    {
        int             termlen;
        uint32          df;
        const char     *term = bm25_accum_term(a, ti, &termlen, &df);
        const uint32   *docids;
        const uint32   *tfs;
        const uint32   *field_ids;
        uint32          n, j;

        bm25_accum_term_postings(a, ti, &docids, &tfs, &field_ids, &n);
        for (j = 0; j < n; j++)
        {
            Datum   vals[5];
            bool    nulls[5] = {0};

            vals[0] = PointerGetDatum(cstring_to_text_with_len(term, termlen));
            vals[1] = Int32GetDatum((int32) df);
            vals[2] = Int32GetDatum((int32) docids[j]);
            vals[3] = Int32GetDatum((int32) tfs[j]);
            vals[4] = Int32GetDatum((int32) bm25_accum_doc_len(a, docids[j]));
            tuplestore_putvalues(ts, tupdesc, vals, nulls);
        }
    }

    bm25_accum_free(a);
    return (Datum) 0;
}

/* ---- multi-field accumulator probe (C2) ----
 * Feed field0[i] as field_id 0 and field1[i] as field_id 1 for doc i, then emit
 * (term, field_id, df, local_docid, tf, doclen) per posting so the suite can assert
 * (term, field_id) is the posting key and doclen is tracked per (doc, field). Uses
 * the raw tokenizer (like bm25_debug_accum) -- this is a codec unit, not an analyzer
 * test. Both arrays must have equal length (one doc per index). */
PG_FUNCTION_INFO_V1(bm25_debug_accum_multi);
Datum
bm25_debug_accum_multi(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    ArrayType      *arr0 = PG_GETARG_ARRAYTYPE_P(0);
    ArrayType      *arr1 = PG_GETARG_ARRAYTYPE_P(1);
    Datum          *e0, *e1;
    bool           *n0, *n1;
    int             ne0, ne1;
    BM25Accum      *a;
    TupleDesc       tupdesc;
    Tuplestorestate *ts;
    int             i;
    uint32          ti;

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

    deconstruct_array_builtin(arr0, TEXTOID, &e0, &n0, &ne0);
    deconstruct_array_builtin(arr1, TEXTOID, &e1, &n1, &ne1);
    if (ne0 != ne1)
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bm25_debug_accum_multi: field0 and field1 must have equal length")));

    a = bm25_accum_begin_multi(2);
    for (i = 0; i < ne0; i++)
    {
        ItemPointerData tid;
        uint32          docid;

        ItemPointerSet(&tid, 1, (OffsetNumber) (i + 1));
        docid = bm25_accum_add_doc_multi(a, &tid);
        if (!n0[i])
        {
            text      *d0 = DatumGetTextPP(e0[i]);
            BM25Token *t0;
            int        c0 = bm25_tokenize(VARDATA_ANY(d0), VARSIZE_ANY_EXHDR(d0), &t0);
            bm25_accum_add_field_tokens(a, docid, 0, t0, c0);
        }
        if (!n1[i])
        {
            text      *d1 = DatumGetTextPP(e1[i]);
            BM25Token *t1;
            int        c1 = bm25_tokenize(VARDATA_ANY(d1), VARSIZE_ANY_EXHDR(d1), &t1);
            bm25_accum_add_field_tokens(a, docid, 1, t1, c1);
        }
    }
    bm25_accum_sort(a);

    for (ti = 0; ti < bm25_accum_nterms(a); ti++)
    {
        int             termlen;
        uint32          df;
        const char     *term = bm25_accum_term(a, ti, &termlen, &df);
        const uint32   *docids;
        const uint32   *tfs;
        const uint32   *field_ids;
        uint32          n, j;

        bm25_accum_term_postings(a, ti, &docids, &tfs, &field_ids, &n);
        for (j = 0; j < n; j++)
        {
            Datum   vals[6];
            bool    nulls[6] = {0};

            vals[0] = PointerGetDatum(cstring_to_text_with_len(term, termlen));
            vals[1] = Int32GetDatum((int32) field_ids[j]);
            vals[2] = Int32GetDatum((int32) df);
            vals[3] = Int32GetDatum((int32) docids[j]);
            vals[4] = Int32GetDatum((int32) tfs[j]);
            vals[5] = Int32GetDatum((int32) bm25_accum_doc_len_field(a, docids[j],
                                                                     field_ids[j]));
            tuplestore_putvalues(ts, tupdesc, vals, nulls);
        }
    }

    bm25_accum_free(a);
    return (Datum) 0;
}
