/* bm25.h -- shared function declarations and cross-module prototypes.
 * This is the single contract included by every bm25_*.c translation unit.
 * On-disk structs, constants, and accessor inlines live in bm25_format.h
 * (included below); this file keeps SQL-callable decls and module prototypes.
 * Several subsystems have their own headers: the WAND engine (bm25_wand.h), the
 * jsonb query tree (bm25_query.h), and the statistics layer the two ranking
 * builders share (bm25_stats.h: per-term idf, the pending scoring arm, the score
 * accumulator, and the pending key backfill, ADR 0093). bm25_scan.h is the
 * scanner's narrow seam for bm25_debug.c.
 *
 * Like every PostgreSQL header, this assumes the including .c file has already
 * included "postgres.h" as its first include; test/check_source_style.py
 * enforces that ordering. The version-floor check below relies on it, since
 * PG_VERSION_NUM comes from there. */
#ifndef BM25_H
#define BM25_H

/* Build-time floor: PG16's planner will not build an incremental-sort path
 * over an amcanorderbyop index, so `ORDER BY x &@@ q, <tiebreak>` collapses
 * onto the tiebreak column on 16 (fixed on 17+). Rather than ship
 * version-inconsistent ranking behavior, refuse to build below 17. The message
 * stays short and points here; this comment is the explanation. */
#if PG_VERSION_NUM < 170000
#error "bm25_native requires PostgreSQL 17 or later (see the build-time floor comment in bm25.h)"
#endif

#include "bm25_format.h"        /* on-disk structs, constants, accessors; it also owns
                                * BM25_FORMAT_VERSION -- no version label is duplicated
                                * into this header, where it would rot silently */
#include <math.h>               /* isinf, for bm25_score_is_unmatched */

#include "fmgr.h"
#include "access/amapi.h"
#include "access/generic_xlog.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "commands/vacuum.h"    /* vacuum_delay_point, BM25_VACUUM_DELAY_POINT below */
#include "storage/bufmgr.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* vacuum_delay_point gained an is_analyze argument in PG 18 (it is
 * vacuum_delay_point(void) on the PG 17 floor). Shared here (moved from
 * bm25_handler.c during the maintenance-interrupts pass) because
 * bm25_reclaim_orphans (bm25_fsm.c) is exclusively VACUUM-path too --
 * amvacuumcleanup is its only caller -- and needs the same throttling form
 * bm25_bulkdelete already uses, not a bare CHECK_FOR_INTERRUPTS: without it,
 * autovacuum's cost-delay budget is silently bypassed for whatever loop
 * omits it (ADR 0025). false = "not an ANALYZE", which every index
 * maintenance call here is. */
#if PG_VERSION_NUM >= 180000
#define BM25_VACUUM_DELAY_POINT()   vacuum_delay_point(false)
#else
#define BM25_VACUUM_DELAY_POINT()   vacuum_delay_point()
#endif

/* -------------------------------------------------------------------------
 * Cancellable-work counter (issue #156)
 * -------------------------------------------------------------------------
 * A backend-local, NON-TRANSACTIONAL tally of work steps performed on the
 * interrupt-checked loops. It exists so a regression suite can assert that a
 * cancelled scan or seal stopped EARLY -- by how much work it got through,
 * not by how many milliseconds elapsed.
 *
 * WHY NOT WALL CLOCK. sql/66_scan_interrupts and sql/80_maintenance_interrupts
 * used to bound `clock_timestamp() - t0` against a fixed interval. That bounds a
 * property of the RUNNER, not of the code: a scheduling stall or a noisy-neighbour
 * VM blows the boundary on an unchanged tree (observed on macos-latest:
 * `not ok 72 - 66_scan_interrupts  1909 ms` while the identical commit passed on a
 * concurrent run), which is why ADR 0070 excluded both suites from the macOS leg.
 * A work count is immune to exactly that: a stalled backend burns wall clock
 * without advancing a single loop iteration.
 *
 * ONLY IF THE CANCEL'S ARRIVAL IS TOO. A cancel delivered by statement_timeout lands
 * when the runner services the timer, and work keeps accruing until then: macos-latest
 * serviced 10 ms timers at 13-56 ms, and the counts crossed their bounds (ADR 0070's
 * 2026-10-06 addendum). So the suites inject the cancel at a named step with
 * bm25_native.debug_cancel_at, and the count measures only what follows its arrival.
 *
 * NON-TRANSACTIONAL IS THE POINT. The reader runs in a PL/pgSQL
 * `WHEN query_canceled` handler, i.e. AFTER the subtransaction the cancelled
 * statement ran in has already aborted. Anything recorded in a table inside that
 * block is gone by then; a C global is not. Nothing here is ever WAL-logged,
 * reset by abort, or shared between backends.
 *
 * RESET IS EXPLICIT AND IS THE ONLY RESET. bm25_debug_work_reset() zeroes it;
 * otherwise it is monotone for the life of the backend (it starts at 0 at process
 * start). There is deliberately no statement-start hook: a per-statement reset
 * would fire for the handler's OWN statements and destroy the value before it
 * could be read. So a caller that wants a delta resets immediately before the work
 * it means to measure.
 *
 * THE UNIT is "one instrumented loop step", and which loops those are is a
 * deliberate, documented set rather than every CHECK_FOR_INTERRUPTS in the tree:
 * one POST page decoded (bm25_seg_scan_postings), one POS page crossed
 * (pos_cursor_next_page), one dictionary entry replayed (bm25_debug_postings), one
 * pending page drained (bm25_pending_drain), one segment page rotation during a
 * build (chain_ensure), one KEYMAP page rotation (bm25_keymap_write, issue #305).
 * The first five are the loops the two interrupt suites' assertions run in, and the
 * last is sql/148's; adding a site is cheap, but it changes what a stored ratio
 * means, so do it deliberately. (sql/80's seal builds a keyless index, so the KEYMAP
 * site leaves its ratio unchanged.)
 *
 * DELIBERATELY NOT FUSED WITH CHECK_FOR_INTERRUPTS. BM25_WORK_UNIT() is its own
 * statement sitting beside the check, not a macro wrapping it, because the two
 * must be able to come apart: the regressions these suites guard against are "the
 * check was deleted" and "the check went dead under a held buffer content lock"
 * (ADR 0041), and in BOTH the work still happens. Counting the work independently
 * of the check is what makes the counter climb when the check goes dead. A single
 * DELETED check usually does not fail the suites, because a neighbouring check
 * services the cancel: within a page for a scan (a per-posting check, the next POS
 * page), and for the seal's drain at the next chunk boundary (bm25_page_alloc and the
 * accumulator), 786 pages later in sql/80 but still under its bound. ci.yml's
 * CHECK_FOR_INTERRUPTS grep floors are what catch a deletion, and this design leaves
 * them counting exactly what they counted before.
 *
 * COST: one increment of a backend-local uint64. Not atomic (nothing else reads it
 * concurrently -- it is process-local), not a function call (a macro over a plain
 * global), no lock, no memory barrier. It compiles to a load/add/store on a line
 * that is already hot in L1 because the same loop touched it last iteration.
 *
 * The rate is what actually settles this, and it is stated as arithmetic rather
 * than as a benchmark claim because the effect is far below what a benchmark on
 * this hardware can resolve. EVERY site above is per PAGE or per dictionary ENTRY
 * -- none is per posting -- so the increments are thousands of instructions apart.
 * Measured on the development machine: the 500k-document seal in
 * sql/80_maintenance_interrupts performs 15,822 increments across 966 ms, one per
 * 61 microseconds; the phrase scan in sql/66_scan_interrupts performs 526 across
 * 817 ms, one per 1.5 ms. Do not add a site to a per-posting loop without
 * revisiting this paragraph.
 */
extern uint64 bm25_work_units;

#define BM25_WORK_UNIT()    ((void) bm25_work_units++)

extern Datum bm25_handler(PG_FUNCTION_ARGS);

/* -------------------------------------------------------------------------
 * In-memory scan-side posting / ranked element: a heap TID plus two vestigial
 * fields. NOT an on-disk record -- nothing writes this struct to a page; it is
 * only ever an element of a scan's so->postings (the @@@ TID set) or so->ranked
 * (scored order) array.
 *
 * tf and doclen date from M1, when doclen was denormalized into each posting to
 * avoid a separate norms stream. M2 moved doclen to the per-field NORMS chain
 * (bm25_seg_doclen_field, declared below), and the two fields have been dead
 * weight since: every producer stores 0 in both and every consumer reads only
 * .tid. The producers are tid_collector_add, bm25_scan_build_ranking_exhaustive
 * and bm25_load_if_needed (bm25_scan_rank.c, bm25_scan.c) plus bm25_wand_build_ranking
 * (bm25_wand.c). Note before shrinking the struct: sizeof(BM25Posting) is the
 * unit the @@@ collector's materialization budget is denominated in
 * (BM25_MATCH_BYTES_PER_TID, bm25_scan.c).
 * -------------------------------------------------------------------------*/
typedef struct BM25Posting
{
    ItemPointerData tid;
    uint32          tf;         /* occurrences of this term in the document */
    uint32          doclen;     /* the field's length: its count of emitting source
                                 * word runs (bm25_accum_add_field_tokens). Dead
                                 * weight, per the note above -- every producer
                                 * stores 0 -- but the definition is stated here so
                                 * it does not read as a token count if it is ever
                                 * revived. */
} BM25Posting;

/* -------------------------------------------------------------------------
 * bm25_tokenize.c -- the word-run tokenizers: bm25_tokenize (M0/M1, whitespace/
 * alnum runs, folded) and bm25_analyze (M3, below)
 * -------------------------------------------------------------------------
 * Each token OWNS its bytes: ptr is a separately palloc'd folded (bm25_tokenize) or
 * stemmed/folded (bm25_analyze) term, not a pointer into the input or a shared
 * lowercased copy. The terms and the token array are allocated in the caller's
 * current memory context.
 * M3's analyzer (bm25_analyze, same file) HAS superseded bm25_tokenize on every
 * ingest and scan path; bm25_tokenize survives as a debug-only entry point.
 * BM25Token itself is shared by both and is not deprecated. */
typedef struct BM25Token
{
    const char *ptr;    /* the token's own palloc'd folded/stemmed bytes */
    int         len;    /* byte length of the token */
    int         pos;    /* ordinal position (0-based) */
    /* M4/D10 -- byte span of the ORIGINAL surface run this token came from, into the
     * text passed to bm25_analyze. Exact by construction: bm25_analyze splits word runs
     * on the original text and folds only the bytes it EMITS (ADR 0046), so a span never
     * has to survive a case fold. It could not: an encoding-aware fold is not
     * byte-length preserving, which is why the fold does not happen before the split.
     * Used ONLY by bm25_snippet to map a stemmed hit back to its original-cased span.
     * -1 for synthetic tokens with no source run. src_len is the run's byte length
     * (== rawlen, and NOT necessarily == len, which is the folded/stemmed term's
     * length); a compound splitter (one run -> several lexemes) shares one
     * src_off/src_len across all its lexemes. Inert for every other caller
     * (build/insert/scan read only ptr/len/pos); bm25_tokenize sets them to the
     * no-source-run sentinel (-1, 0) rather than a real span, because it feeds no
     * snippet path. */
    int         src_off;
    int         src_len;
} BM25Token;

/* Longest term the index will store, in bytes. A term ends up inline in a
 * BM25DictEntry record, and that record must fit one segment page whole
 * (chain_write's contract) -- so an uncapped term is an out-of-bounds page write,
 * not merely a large one. The cap is enforced where tokens are PRODUCED
 * (bm25_analyze), so an over-long run is dropped with a NOTICE instead of failing
 * the INSERT/CREATE INDEX deep inside the segment writer; chain_ensure keeps an
 * independent hard check as the trust boundary.
 *
 * 2047 deliberately matches core full-text search's own limit (to_tsvector emits
 * "word is too long to be indexed" and skips the word above the same threshold),
 * so a corpus that indexes cleanly under tsvector indexes cleanly here. */
#define BM25_MAX_TERM_BYTES 2047

extern int bm25_tokenize(const char *text, int textlen, BM25Token **out);

/* -------------------------------------------------------------------------
 * bm25_analyzer.c -- M3 analyzer config: reloption -> usable config, the
 * Snowball ts_lexize dict OID lookup, the analyzer fingerprint, and the
 * per-index field-config page (BM25_PAGE_FIELDCFG) reader/writer.
 * -------------------------------------------------------------------------
 * The config is a function of the reloptions PLUS the catalog lookups they imply:
 * tokenization runs ts_lexize over the dict OID that "<language>_stem" resolves to,
 * and the fingerprint's stemmer identity reads that dictionary's row, its template's,
 * and both namespaces. Same catalog, same answers, so WAL replay and a physical
 * replica tokenize byte-identically. Baked at CREATE INDEX; changing it requires REINDEX
 * (the fingerprint gate enforces this, at scan start AND per row on ingest).
 *
 * That lookup resolves an UNQUALIFIED name through the CALLER's search_path
 * (get_ts_dict_oid on a one-element name list, bm25_snowball_dict_oid), and the
 * fingerprint is recomputed from the index's own reloptions on EVERY scan
 * (bm25_scan_corpus_stats -> bm25_analyzer_config -> bm25_fingerprint_gate,
 * bm25_scan_rank.c). Two consequences, which used to be conflated:
 *
 *  - WHICH dictionary is reached is not pinned. pg_catalog is searched implicitly
 *    first, so the core snowball dict normally wins; a search_path that lists
 *    pg_catalog explicitly AFTER another schema resolves "<language>_stem" to that
 *    schema's dictionary instead. Builds cannot follow: since PG17, CREATE INDEX and
 *    REINDEX run under a restricted "pg_catalog, pg_temp" path, so the build side
 *    always binds pg_catalog's. The fingerprint gate is what keeps the disagreement
 *    loud rather than silent -- ERROR under require_analyzer_match = true (the
 *    default), WARNING-and-proceed (through the shadowing dictionary) under false.
 *    Since #188 it runs on the WRITE path too, where proceeding is the more expensive
 *    choice: bm25_insert commits terms the index's own analyzer does not produce, so
 *    the row stays unfindable until REINDEX (docs/adr/0081).
 *  - WHAT the fingerprint records about it IS pinned. stemmer_id is NOT the dict OID;
 *    it is an FNV-1a hash of the dictionary's schema-qualified name plus its
 *    template's (bm25_stemmer_identity, bm25_analyzer.c). The OID is an initdb/OID-
 *    counter allocation, so recording it made PG_UPGRADE look like an analyzer change
 *    and demand a REINDEX of indexes that were fine. It never caused a MISSED
 *    mismatch -- pg_ts_dict.oid is unique, so shadow detection was always exact; the
 *    OID simply could not tell a different dictionary from a renumbered one. A
 *    logical dump/restore was never affected either way, because pg_dump emits
 *    CREATE INDEX and the restore re-stamps. Dictionary OPTIONS remain outside the
 *    identity, and so does a stemmer that changed under the same name across a
 *    pg_upgrade (PostgreSQL 17 -> 18 English); since #296 both are seen instead by
 *    the fingerprint's seventh component, a hash of the dictionary's output over a
 *    fixed probe word list (bm25_analyzer_probe_hash, bm25_analyzer.c). */
typedef struct BM25AnalyzerConfig
{
    char    language[BM25_STEMMER_NAME_LEN]; /* Snowball language, e.g. "english" */
    Oid     stem_dict_oid;                   /* ts_lexize Snowball dict OID for `language` */
    int     stopwords;                       /* BM25_STOPWORDS_NONE | BM25_STOPWORDS_DEFAULT */
    int     tokenizer_type;                  /* BM25_TOKENIZER_* */
    uint32  stemmer_id;                      /* stable id of the stemmer used in the fingerprint:
                                              * FNV-1a over the dict's schema-qualified name +
                                              * its template's, NOT stem_dict_oid */
    uint32  stopword_set_hash;               /* FNV-1a over the resolved stopword set tag */
} BM25AnalyzerConfig;

/* Resolve reloptions on an open index into a usable analyzer config (looks up the
 * Snowball ts dict OID, computes stemmer_id + stopword_set_hash). Falls back to
 * the documented defaults (english/default/standard) when rd_options is NULL. */
extern void   bm25_analyzer_config(Relation index, BM25AnalyzerConfig *out);
/* Resolve directly from a parsed BM25Options bytea, for a caller holding the
 * reloption struct rather than an open relation. bm25_analyzer_config is its only
 * caller today -- it was once described as serving reloption validation, which never
 * called it and still does not; do not infer a validator-safe contract from that.
 * BM25Options is defined later in this header (bm25_handler.c's reloption set). */
struct BM25Options;
extern void   bm25_analyzer_config_from_opts(struct BM25Options *opts,
                                             BM25AnalyzerConfig *out);
/* Build a caller-supplied config (language/stopwords/tokenizer already set) into a
 * fully-derived one: dict OID + stemmer_id + stopword_set_hash. Public so callers
 * without an open index relation (debug tokenizer, explicit-config overloads) can
 * derive the same fields the resolver produces. The name is a misnomer: it fills no
 * defaults (every caller sets english/default/standard, or the reloption values,
 * before calling); it ASCII-lowercases `language`, resolves the dict OID and derives
 * the stemmer id and stopword hash from what the caller set. Kept, not renamed,
 * because ADR 0080 names it. */
extern void   bm25_analyzer_default_config(BM25AnalyzerConfig *out);
/* validate_string callback for the `language` reloption, registered in
 * bm25_handler.c. Raises if no "<language>_stem" dictionary is visible, so a bad
 * language fails at CREATE/ALTER INDEX rather than at the next scan. Lives in
 * bm25_analyzer.c because it must fold and resolve the name exactly as the resolver
 * does; see its own comment for what DDL-time validation does NOT pin. */
extern void   bm25_validate_language(const char *value);
/* validate_string callback for the `tokenizer` reloption (bm25_handler.c), exported
 * so the explicit-config debug tokenizer overloads in bm25_tokenize.c enforce the
 * SAME set rather than a second copy of it. Accepts only "standard". */
extern void   bm25_validate_tokenizer(const char *value);

/* FNV-1a over (stemmer_id || language || stopword_set_hash || tokenizer_type ||
 * analyzer_revision || database_encoding || probe_hash), little-endian for the six
 * uint32 operands; language is the lowercased cfg->language up to AND INCLUDING its
 * NUL. probe_hash (#296) hashes the resolved dictionary's fold and lexize output over a
 * fixed word list, cached per backend by dictionary OID.
 * Concatenation order is fixed and part of the on-disk contract -- a new component is
 * APPENDED, never inserted; bm25_analyzer_fingerprint's body is authoritative if this
 * list ever drifts from it again. The revision covers what the other components
 * cannot: a change to the analyzer's CODE that alters its output for unchanged
 * reloptions (ADR 0046), or a change to how an existing component's value is ENCODED,
 * which is indistinguishable from the first once it is on disk (ADR 0080,
 * stemmer_id). */
extern uint32 bm25_analyzer_fingerprint(const BM25AnalyzerConfig *cfg);

/* Read the require_analyzer_match bool reloption off an open index (default true
 * when rd_options is NULL). Both fingerprint gates use it to pick ERROR (true) vs
 * WARNING (false) on an analyzer fingerprint mismatch. */
extern bool   bm25_require_analyzer_match(Relation index);

/* Which side of the index tripped the analyzer fingerprint gate. It selects the
 * MESSAGE only -- the comparison and the ERROR-vs-WARNING decision are single-sourced
 * in bm25_fingerprint_gate, deliberately (issue #157: the same predicate written twice
 * drifts). The wording has to differ because the cost of proceeding does: SCAN yields
 * wrong results for one session, INGEST commits a row that no correctly analyzed query
 * will find until REINDEX. */
typedef enum BM25GateSite
{
    BM25_GATE_SCAN,             /* read path: BOTH scan-start sites -- the ranked
                                 * prologue bm25_scan_corpus_stats and the boolean
                                 * @@@ path bm25_load_if_needed -- plus the debug
                                 * probe. Two, not one: the @@@ filter path has its
                                 * own snapshot and would mis-filter silently. */
    BM25_GATE_INGEST            /* write path: bm25_insert, before it tokenizes */
} BM25GateSite;

/* Compare a freshly resolved analyzer fingerprint against the metapage's stamped one;
 * ERROR (or WARN, per require_analyzer_match) on mismatch. Exported rather than
 * file-static so bm25_debug_fingerprint_gate can drive the REAL gate with a
 * caller-supplied fingerprint -- a re-implementation in the SRF would test the copy. */
extern void   bm25_fingerprint_gate(Relation index, uint32 resolved_fp,
                                    uint32 index_fp, BM25GateSite site);
/* M4: read the phrase_fallback reloption (default 'error'). true == 'and'
 * (WARN + AND-of-terms on a position-less segment), false == 'error' (raise).
 * TEXT phrase path only -- this has exactly one call site, the (text,text) phrase
 * degradation gate in bm25_scan_rank.c. The jsonb query-tree phrase gate never consults
 * it and always ERRORs; see phrase_fallback_offset in BM25Options below. */
extern bool   bm25_phrase_fallback_is_and(Relation index);

/* Tokenize text into analyzer tokens (split -> stopword -> stemmer via ts_lexize
 * -> fold). Tokens point into palloc'd storage in CurrentMemoryContext. Returns
 * token count; drops stopwords (so positions are post-stopword, matching index
 * time) UNLESS cfg->stopwords == BM25_STOPWORDS_NONE, in which case a stoplist word
 * is kept as its lowercased surface form. Deterministic for a given database: a
 * pure function of (cfg->stem_dict_oid, database default collation, token). The
 * collation operand is not new -- ts_lexize has always folded through it -- but it is
 * why a collation-provider upgrade is a REINDEX event here (ADR 0046). */
extern int    bm25_analyze(const BM25AnalyzerConfig *cfg, const char *text,
                           int textlen, BM25Token **out);
/* qsort/bsearch comparator over BM25Token: length first, then bytes, so equality is
 * exactly (len, bytes) equality. Defined in bm25_handler.c (bm25_match); shared with
 * bm25_snippet's hit set (#305 XCUT-06), the same sort + bsearch shape. */
extern int    bm25_token_cmp(const void *a, const void *b);

/* Character classification + case folding, the single definition of "word
 * character" and "lowercase" for the whole extension (ADR 0046). The index and the
 * query must fold identically or a term silently fails to match, so every path that
 * splits text or builds a dictionary key routes through these: bm25_analyze, the
 * wildcard expander (bm25_seg_dict.c) and the snippet edge snapper.
 *
 * bm25_mblen_bounded returns the byte length of the character at `p` and ERRORs
 * ("invalid byte sequence") if it would extend past `remaining` (> 0) bytes; it is
 * the only character-length primitive, in place of the deprecated pg_mblen (TEXT-08).
 * bm25_next_char returns the same length (so a caller always advances and never
 * overreads) and sets *is_word.
 * bm25_fold_term returns a NUL-terminated palloc'd fold with its length in
 * *foldedlen; it is NOT byte-length preserving, so callers must use *foldedlen and
 * must not carry offsets across the fold. */
extern bool   bm25_is_word_byte(unsigned char c);
extern int    bm25_mblen_bounded(const char *p, int remaining);
extern int    bm25_next_char(const char *p, int remaining, bool *is_word);
extern char  *bm25_fold_term(const char *s, int len, int *foldedlen);
/* The generated high-half (0x80-0xFF) word-class bitmap of a single-byte server
 * encoding -- 16 bytes, one bit per byte -- or NULL when the encoding has none
 * (bm25_sb_wordclass.c, written by ci/gen_sb_wordclass.py; issue #295). Only
 * bm25_is_word_byte consults it. */
extern const uint8 *bm25_sb_wordclass(int encoding);

/* Field-config page helpers (BM25_PAGE_FIELDCFG; defined in bm25_analyzer.c).
 * bm25_fieldcfg_write lays out [BM25FieldConfigHeader][BM25FieldConfig x count] on
 * one page (N <= BM25_MAX_FIELDS fits BLCKSZ; the reader follows nextblk continuation
 * pages if a future layout overflows) and returns the chain root; read fills
 * hdr_out + a caller array sized >= BM25_MAX_FIELDS. fields[] and per_field_fingerprint[]
 * are sized >= field_count. bm25_build is the sole live caller. */
/* M4: store_positions[] is the per-field position-storage bit array (length
 * field_count), appended AFTER the [BM25FieldConfig x field_count] records on the
 * page (D12 back-compat -- the config records keep the M5 layout). The reader fills
 * out_store_positions[] (>= field_count) when the trailing array is present (M4-
 * written pages) and leaves it untouched otherwise (pre-M4 pages carry no positions,
 * so the bit is never consulted). Either array may be NULL to opt out. */
/* #292: both writers also stamp the resolved key identity (key_type, key_size,
 * key_attno -- bm25_resolve_fields' outputs) as the page's BM25KeyStamp tail. */
extern BlockNumber bm25_fieldcfg_write(Relation index, Relation heaprel,
                                       const BM25FieldConfig *fields, uint32 field_count,
                                       const uint32 *per_field_fingerprint,
                                       const uint8 *store_positions /* >= field_count, or NULL */,
                                       uint8 key_type, uint16 key_size, int key_attno);
/* INIT_FORKNUM sibling of bm25_fieldcfg_write, for bm25_buildempty only -- see its
 * header comment (bm25_analyzer.c) for why it cannot share bm25_page_alloc. */
extern BlockNumber bm25_fieldcfg_write_init(Relation index,
                                            const BM25FieldConfig *fields, uint32 field_count,
                                            const uint32 *per_field_fingerprint,
                                            const uint8 *store_positions /* >= field_count, or NULL */,
                                            uint8 key_type, uint16 key_size, int key_attno);
extern void        bm25_fieldcfg_read(Relation index, BlockNumber root,
                                      BM25FieldConfigHeader *hdr_out,
                                      BM25FieldConfig *cfg_out /* >= BM25_MAX_FIELDS */,
                                      uint8 *out_store_positions /* >= BM25_MAX_FIELDS, or NULL */);
/* #292: read the field-config page's key-identity stamp. Returns false for a page
 * written before #292 (no stamp); ERRCODE_INDEX_CORRUPTED for a tail that is present
 * but is not a valid stamp. MAIN_FORKNUM only, like bm25_fieldcfg_read. */
extern bool        bm25_fieldcfg_read_keystamp(Relation index, BlockNumber root,
                                               BM25KeyStamp *out);

/* M5: resolve N per-field BM25FieldConfig from the index reloptions + the indexed
 * columns' attnames. field_name = the column's attname; k1/b/boost from the
 * per-field reloption (<param>_<attname>) or the index default (the k1/b
 * reloptions, boost 1.0). tokenizer_type/stemmer_name are cloned from the single
 * index analyzer (per-field analyzer divergence is DEFERRED, spec section 9). Also
 * resolves key_field -> key_type/key_size/key_attno by name against the INDEX tuple
 * descriptor -- key AND INCLUDE columns -- not the heap: out_key_attno is the
 * 0-based index attno the build/insert callback's values[] is subscripted by, and a
 * key_field naming a column the index does not carry is an ERROR, not a silent
 * miss. BM25_KEY_NONE / attno -1 when no key_field. out array is sized >=
 * BM25_MAX_FIELDS by the caller. */
/* HDL-02: refuse an indexed KEY column whose base type is not text/varchar, before
 * anything dereferences its Datum as a varlena. Called at the top of bm25_build and
 * at the top of bm25_insert -- specifically NOT from bm25_resolve_fields, which on
 * the insert path runs only after the tokenize loop has already dereferenced. */
extern void        bm25_check_indexed_column_types(Relation index);
extern void        bm25_resolve_fields(Relation index, Relation heap,
                                       const BM25AnalyzerConfig *idxcfg,
                                       BM25FieldConfig *out /* >= natts */,
                                       uint32 *out_field_count,
                                       uint8 *out_key_type, uint16 *out_key_size,
                                       int *out_key_attno,
                                       uint8 *out_store_positions /* >= natts, or NULL */);

/* Live re-resolution (spec 2026-07-16 addendum, Task 2) of the tunable per-field
 * k1/b/boost from the index's CURRENT reloptions, called at scan start so an
 * ALTER INDEX ... SET/RESET of these knobs takes effect on the next scan with no
 * REINDEX. Overwrites fcfg[0..field_count-1].k1/.b/.boost in place; leaves every
 * other BM25FieldConfig member (field_name, tokenizer_type, stemmer_name) as the
 * caller loaded it -- those stay build-era (structural / analyzer identity).
 * ereports (with a RESET hint) on a stored-garbage value that predates ALTER-time
 * validation, exactly like match_field_reloption's other callers. */
extern void        bm25_resolve_live_params(Relation index, BM25FieldConfig *fcfg,
                                            uint32 field_count);

/* Upper bounds on k1 and on a per-field boost, enforced at CREATE/ALTER only
 * (SCORE-02; the per-field caps are skipped under pg_upgrade). BM25_K1_MAX is
 * shared by the index-wide k1 reloption and k1_<col>. BM25_FIELD_BOOST_MAX keeps
 * boost * idf * (k1 + 1) finite for any idf a real corpus produces; the specific
 * value is a documented choice, not a derivation. */
#define BM25_K1_MAX             1e30
#define BM25_FIELD_BOOST_MAX    1e6

/* Value-only validation for a dynamically-named per-field knob (k1_<attname> /
 * b_<attname> / boost_<attname> / store_positions_<attname>), called from
 * bm25_options at CREATE/ALTER-time (validate=true) so garbage per-field values are
 * rejected at DDL time rather than surviving until the next REINDEX. Also applies
 * the BM25_K1_MAX / BM25_FIELD_BOOST_MAX caps, which no other caller of the value
 * parser enforces -- except under pg_upgrade (IsBinaryUpgrade), whose CREATE INDEX
 * must accept whatever the old cluster stored. amoptions has no column list in scope, so the suffix is not
 * checked here; bm25_warn_unmatched_field_knobs does that at build. */
extern void        bm25_check_field_knob_value(const char *defname, const char *value);

/* The column-name suffix of a per-field knob ("title" for "k1_title"), or NULL
 * when `name` is not a per-field knob. The one definition of the knob shape:
 * bm25_options strips exactly these names before build_reloptions. */
extern const char *bm25_field_knob_suffix(const char *name);

/* SURFACE-06: WARNING for each per-field knob whose suffix names no key column
 * of the index (a typo, or an INCLUDE column). Called from ambuild only, so it
 * fires at CREATE INDEX and REINDEX and never on the INSERT or scan paths. */
extern void        bm25_warn_unmatched_field_knobs(Relation index);

/* -------------------------------------------------------------------------
 * bm25_meta.c -- metapage (LSM directory) lifecycle and Generic WAL page helpers
 * -------------------------------------------------------------------------
 * Deliberately carries no format-version label: BM25_FORMAT_VERSION
 * (bm25_format.h) is the single source of truth and bm25_meta_validate below is
 * the one gate that enforces it.
 * -------------------------------------------------------------------------*/
extern void   bm25_meta_init(Relation index, ForkNumber forknum);
extern void   bm25_meta_read(Relation index, BM25MetaPageData *out);
extern void   bm25_meta_write(Relation index, const BM25MetaPageData *in); /* raises pd_lower */
/* RAISE (never lower) a metapage's pd_lower to at least the end of this build's
 * BM25MetaPageData. EVERY metapage writer must call this instead of assigning
 * pd_lower, or it silently destroys a newer binary's appended tail -- see the
 * contract note in bm25_meta.c. */
extern void   bm25_meta_set_pd_lower(Page page);
/* The v6 two-directional version gate (magic tag + min_read_version/format_version),
 * factored out of bm25_meta_read so bm25_scan_snapshot -- which reads metapage
 * fields directly under its own SHARE lock instead of calling bm25_meta_read --
 * enforces the identical gate on the scan's one-and-only metapage touch. */
extern void   bm25_meta_validate(const BM25MetaPageData *out);
/* Open a user-supplied regclass for a WRITE entry point (bm25_seal/merge/upgrade and
 * every mutating bm25_debug_* function): ownership, not-in-recovery (25006) and AM
 * identity checked before any page is touched. index_open() checks none of them.
 * Every such function must route through this -- see its definition. */
extern Relation bm25_index_open_owned(Oid relid, LOCKMODE lockmode);
/* Read-only sibling: SELECT on the indexed TABLE, no row-level security applying to
 * the caller there, + AM identity; no ownership. */
extern Relation bm25_index_open_readable(Oid relid, LOCKMODE lockmode);
/* Derive the informational feature_flags bitmap (BM25_FEAT_*) from an index's
 * resolved content (field count + field-config store_positions). Never gates;
 * shared by bm25_build (build-time stamp) and bm25_upgrade (re-derive for an
 * existing index). */
extern uint32 bm25_derive_feature_flags(Relation index, const BM25MetaPageData *meta);
/* Copy the metadata from an already-EXCLUSIVE-locked metabuffer without re-locking
 * (used by the pending append/truncate paths that hold the metabuffer themselves). */
extern void   bm25_meta_read_locked(Buffer metabuf, BM25MetaPageData *out);
/* Drained-head accessor (seal/merge): read the metapage and report pending_head. */
extern void   bm25_meta_read_pending_head(Relation index, BlockNumber *out);
/* Read-and-bump meta->next_gen; returns >= 1 (seg_gen 0 stays the reserved sentinel). */
extern uint32 bm25_next_gen(Relation index);
extern void   bm25_next_gen_check(Relation index, uint32 next_gen);
/* Orphan-op brackets around a maintenance op that can leave orphan pages (issue #300;
 * see bm25_meta.c). Caller holds the seal/merge singleton ExclusiveLock across both. */
extern void   bm25_orphan_op_begin(Relation index);
extern void   bm25_orphan_op_end(Relation index);
/* Returns the page EXCL-locked; see bm25_meta.c for the Phase-1/Phase-4 contract. */
extern Buffer bm25_page_alloc(Relation index, Relation heaprel);
extern void   bm25_page_init(Page page, uint16 flags);
extern void   bm25_buildempty(Relation index);

/* -------------------------------------------------------------------------
 * bm25_segment.c -- read-path helpers and SQL-callable debug SRFs
 * -------------------------------------------------------------------------
 * The M0/M1 in-place builder (BM25Builder, BM25Term, bm25_builder_begin/
 * add_doc/flush, bm25_segment_add_doc/new_term/append_posting) was removed;
 * ambuild uses BM25Accum + bm25_segment_build_and_commit (bm25_seg_build.c)
 * and aminsert uses the pending list (bm25_pending_append). */

/* -------------------------------------------------------------------------
 * bm25_accum.c -- in-memory drain accumulator (dense docids, per-term postings)
 * -------------------------------------------------------------------------
 * BM25Accum is the build input for seal and merge.  It ingests documents (TID +
 * token list), assigns dense local doc-ids in add order (0,1,2,...), and
 * accumulates per-term sorted postings {local_docid, tf} plus a per-doc
 * {tid, doclen} array.  The segment builder (Task 5) walks terms in
 * lexicographic order after bm25_accum_sort() is called once. */
typedef struct BM25Accum BM25Accum;
/* M3 single-field begin (== bm25_accum_begin_multi(1)); M5 multi-field begin
 * pins the field dimension up front (docs carry a doclen_by_field[field_count]). */
extern BM25Accum *bm25_accum_begin(void);
extern BM25Accum *bm25_accum_begin_multi(uint32 field_count);
/* M5 split add: register the doc once (returns its dense local docid), then add
 * each indexed column's tokens tagged with its dense field_id. The posting key is
 * (term, field_id); doclen is tracked per (doc, field). */
extern uint32     bm25_accum_add_doc_multi(BM25Accum *a, ItemPointer tid);
extern void       bm25_accum_add_field_tokens(BM25Accum *a, uint32 local_docid,
                                              uint32 field_id, BM25Token *toks,
                                              int ntok);
/* M4 positions: expose the position list of the posting at sorted-term index `i`,
 * posting index `j` (parallel to bm25_accum_term_postings' arrays). Returns the
 * ascending per-(doc,field) position array (0-based analyzer ordinals) and its
 * length (== the posting's tf, since every token occurrence records a position).
 * The pointer is into the accumulator context; valid until bm25_accum_free. */
extern const uint32 *bm25_accum_posting_positions(BM25Accum *a, uint32 i, uint32 j,
                                                  uint32 *npos);
/* Single-field convenience: add_doc_multi + add_field_tokens(...,0,...). Keeps
 * every single-field caller (bm25_debug_accum, 1-field drain, tests) byte-identical. */
extern uint32     bm25_accum_add_doc(BM25Accum *a, ItemPointer tid,
                                     BM25Token *toks, int ntok); /* returns local docid */
extern void       bm25_accum_free(BM25Accum *a);

/* Read-side accessors used by the segment builder (Task 5). Defined in
 * bm25_accum.c; iterate terms in sorted order and their postings in (docid,field)
 * order. */
extern uint32     bm25_accum_ndocs(BM25Accum *a);
extern uint64     bm25_accum_total_len(BM25Accum *a);      /* corpus-wide sum over fields */
extern uint64     bm25_accum_total_tokens(BM25Accum *a);   /* sum of tf; >= total_len (ADR 0088) */
extern uint32     bm25_accum_nterms(BM25Accum *a);
extern uint32     bm25_accum_field_count(BM25Accum *a); /* builder loop bound */
extern void       bm25_accum_sort(BM25Accum *a);   /* sort terms lexicographically; call
                                                    * ONCE after all add_doc. Does not rebuild
                                                    * the hash -- add_doc after sort is O(nterms). */
/* term i (after sort): name/len + df (total posting count over ALL fields -- the
 * per-segment dict df; per-field df is derived at query time from the block RLE). */
extern const char *bm25_accum_term(BM25Accum *a, uint32 i, int *termlen, uint32 *df);
/* LIFETIME: the three arrays are REUSED accumulator scratch, not fresh
 * allocations -- they are invalidated by the next bm25_accum_term_postings call on
 * the same accumulator. Consume them before requesting the next term. Holding one
 * across a call does not fault; it silently yields the NEXT term's postings, which
 * is why this is stated here and not only at the definition. Full rationale (and
 * why the arrays are not caller-freed) in bm25_accum.c. */
extern void        bm25_accum_term_postings(BM25Accum *a, uint32 i,
                                            const uint32 **docids, const uint32 **tfs,
                                            const uint32 **field_ids, uint32 *n);
extern ItemPointer bm25_accum_doc_tid(BM25Accum *a, uint32 local_docid);
extern uint32      bm25_accum_doc_len(BM25Accum *a, uint32 local_docid); /* sum over fields */
/* M5 key_field: a per-docid fixed-width key store parallel to the TID array. The
 * builder/merge sets key meta once (set_keymeta) so the accumulator sizes its flat
 * keys[] buffer, then stashes each doc's key bytes (set_doc_key). At seal,
 * bm25_segment_build_orphans passes bm25_accum_doc_key(a,0) (the dense base) to
 * bm25_keymap_write. key_type==NONE => keys stays NULL and doc_key returns NULL
 * (the ctid-fallback path, byte-identical to a keyless index). */
extern void        bm25_accum_set_keymeta(BM25Accum *a, uint8 key_type, uint16 key_size);
extern uint8       bm25_accum_key_type(BM25Accum *a);
extern uint16      bm25_accum_key_size(BM25Accum *a);
/* M4: the per-field store_positions gate (default all-true). The builder sets it
 * once (from the resolved field config) before adding docs; bm25_segment_build_orphans
 * consults it to decide, per posting, whether to emit a POS frame. When ANY field is
 * on, the segment's pos_root is written; when EVERY field is off, no POS chain is
 * built (pos_root Invalid -- the pre-M4 shape). bits[] is field_count wide (1 = on). */
extern void        bm25_accum_set_store_positions(BM25Accum *a, const uint8 *bits);
extern const uint8 *bm25_accum_store_positions(BM25Accum *a); /* NULL => all fields on */
extern void        bm25_accum_set_doc_key(BM25Accum *a, uint32 local_docid,
                                          const unsigned char *key, uint16 key_size);
extern const unsigned char *bm25_accum_doc_key(BM25Accum *a, uint32 local_docid);
/* Per-field build stats (the segment builder fills the header/NORMS from these). */
extern void        bm25_accum_total_len_by_field(BM25Accum *a, uint64 *out /* >= field_count */);
extern uint32      bm25_accum_doc_len_field(BM25Accum *a, uint32 docid, uint32 field_id);
extern uint64      bm25_accum_ndocs_field(BM25Accum *a, uint32 field_id); /* docs that HAVE the field */
extern void        bm25_accum_ndocs_by_field(BM25Accum *a, uint64 *out /* >= field_count */);

/* Merge-path accumulator extensions (Phase 4, Task 23; M5 field-aware). The seal
 * path ingests a doc as per-field token lists (add_doc_multi + add_field_tokens,
 * deriving per-field doclen + postings together); the merge path instead replays a
 * source segment's STORED postings, so it registers a doc by its known TID +
 * per-field doclens first and then attaches (term, field_id, tf) postings one at a
 * time. add_doc_blank assigns the next dense local id (same scheme as add_doc);
 * add_posting appends a single posting for an already-registered doc, which keeps
 * the per-term df (== posting count) correct as long as the caller feeds at most
 * one posting per (term, doc, field). */
extern uint32     bm25_accum_add_doc_blank(BM25Accum *a, ItemPointer tid,
                                           const uint32 *doclen_by_field,
                                           uint32 field_count);
extern void       bm25_accum_add_posting(BM25Accum *a, const char *term,
                                         int termlen, uint32 field_id,
                                         uint32 local_docid, uint32 tf);
/* M4 merge position replay: attach a posting's positions to the posting the
 * immediately-preceding bm25_accum_add_posting created for (term, field_id).
 * npos MUST equal that posting's tf (D2); positions are copied into a->cxt. */
extern void       bm25_accum_add_positions_to_last(BM25Accum *a, const char *term,
                                                   int termlen, uint32 field_id,
                                                   const uint32 *positions,
                                                   uint32 npos);

/* ---- BUILD-04/PEND-12: the maintenance memory budget ----
 *
 * The accumulator holds an ENTIRE build, merge input set, or pending drain in RAM
 * with no spill-to-disk and no tuplesort, so its residency was bounded only by the
 * corpus. Since the merge and the pre-merge seal both run from amvacuumcleanup,
 * that was an OOM in an autovacuum worker -- a process with no user-visible failure
 * path, so the index simply stopped being maintained.
 *
 * All three feeders now chunk: accumulate until bm25_accum_over_budget, seal what
 * is held as one segment, start a fresh accumulator, continue. Budget exhaustion
 * SEALS, it never errors -- an error on the autovacuum or opportunistic-insert path
 * would wedge the index, which is the disease rather than the cure.
 *
 * bm25_maintenance_budget_bytes is the ceiling (autovacuum_work_mem in an AV worker
 * when set, else maintenance_work_mem, else the debug override);
 * bm25_accum_over_budget measures one accumulator against it. Read the budget ONCE
 * per operation and pass it down, so every chunk of one build/merge/drain is judged
 * against the same number. */
extern Size       bm25_maintenance_budget_bytes(void);
extern bool       bm25_accum_over_budget(BM25Accum *a, Size budget);
/* Predicted accumulator residency of replaying one segment's live docs, from its
 * catalog entry's own fields. Drives the merge's selection trim, which needs to
 * know -- before committing to a set of inputs -- whether merging them can actually
 * reduce the segment count under the budget. Lives in bm25_accum.c because it is a
 * sum of that file's sizeof()s and must not be restated anywhere else. Charges
 * data, not allocator slack, so actuals run up to ~2x; absorbing that is what the
 * chunk boundary is for. */
extern uint64     bm25_accum_estimate_bytes(uint64 ndocs, uint64 live_ndocs,
                                            uint64 total_len, uint64 stored_tokens,
                                            uint32 nterms, uint32 field_count,
                                            bool has_positions, uint16 key_size);

/* -------------------------------------------------------------------------
 * bm25_seg_build.c -- sealed-segment builder + two-phase seal commit (Task 5)
 * -------------------------------------------------------------------------
 * Take a sorted BM25Accum, write the DOCMAP/NORMS/LIVE/POST/DICT chains + the
 * segment header as ORPHAN pages, then publish with ONE final Generic WAL record
 * that appends the BM25SegCatEntry, updates global stats, and resets the pending
 * anchor (the linearization point, D-SEAL). drained_head records which pending
 * head was drained (InvalidBlockNumber for the ambuild path). */
extern void  bm25_segment_build_and_commit(Relation index, Relation heaprel,
                                           BM25Accum *a, BlockNumber drained_head);

/* ---- BUILD-04: the N-entry publish primitives ----
 *
 * A budgeted seal, drain or merge produces SEVERAL segments from one operation, and
 * they must all become visible in ONE record: a scan captures pending_head and the
 * whole live catalog under a single metapage SHARE lock and then SUMS per-TID
 * contributions with no cross-source dedup, so any committed state in which some
 * chunks are published while their source is still live double-scores (and, on the
 * membership path, double-returns) every doc in the published part -- durably, if a
 * crash lands there. Publishing all N at once costs nothing: the append record is
 * metapage + one catalog page, and the swap's flip record is metapage + one
 * retired-list tail, both regardless of N.
 *
 * Build the chunks with bm25_segment_build_orphans, turn each header into an entry
 * with bm25_segcat_entry_from_hdr, then publish the array through the APPEND form
 * (seal/build: adds entries, resets the pending anchor) or the SWAP form (merge:
 * replaces the catalog chain, retires the dropped gens, optional format re-stamp).
 * The two accumulator-taking wrappers above and below are the N == 1 cases. */
extern void  bm25_segcat_entry_from_hdr(BM25SegCatEntry *e,
                                        BlockNumber header_blkno,
                                        const BM25SegmentHeader *hdr);
extern void  bm25_segcat_publish_append(Relation index, Relation heaprel,
                                        const BM25SegCatEntry *entries, int n,
                                        BlockNumber drained_head);
extern void  bm25_segcat_publish_swap(Relation index, Relation heaprel,
                                      const BM25SegCatEntry *new_entries, int nnew,
                                      uint32 *drop_gens, int ndrop,
                                      const BM25FormatRestamp *restamp);

/* Phase-1 orphan builder, factored out of bm25_segment_build_and_commit so BOTH
 * the seal publish and the merge swap publish share one segment-page writer
 * (Task 23). Writes the DOCMAP/NORMS/LIVE/POST/DICT chains + the segment header
 * as ORPHAN pages (metapage untouched), stamps each with a fresh monotonic gen,
 * fills *hdr (including hdr->gen), and returns the header block. The caller's own
 * record links the segment into a live structure (the linearization point). */
extern BlockNumber bm25_segment_build_orphans(Relation index, Relation heaprel,
                                              BM25Accum *a, BM25SegmentHeader *hdr);

/* Lay `n` BM25SegCatEntry onto a FRESH orphan BM25_PAGE_SEGCAT chain (metapage
 * untouched) and return its first block, for the swap's root flip. */
extern BlockNumber bm25_segcat_build_orphan_chain(Relation index, Relation heaprel,
                                                  BM25SegCatEntry *entries, int n,
                                                  BlockNumber *out_tail /* or NULL */);

/* Internal entry used by both bm25_merge() and bm25_merge_maybe(). Reads the
 * chosen segments, re-accumulates their LIVE docs into one OR MORE new segments
 * (BUILD-04: the accumulator is cut at bm25_maintenance_budget_bytes, at input-
 * segment boundaries), and publishes them all in ONE atomic swap alongside the
 * removal of every input.
 *
 * Returns PROGRESS -- the segment count fell, or dead docs were dropped -- and NOT
 * "a merge ran". The distinction is what makes bm25_merge_maybe's force loop
 * terminate once a merge can emit as many segments as it consumed; see the
 * function's own header. */
extern bool  bm25_merge_execute(Relation index, Relation heaprel);

/* bm25_upgrade segment-rewrite path (roadmap #4 Task 6): re-emit EVERY live
 * segment through the same merge accumulate + atomic-swap machinery, folding
 * `restamp` into the swap's single catalog-flip WAL record so the version re-stamp
 * is crash-atomic with the publish. Takes the per-index seal/merge singleton and
 * opens the heap itself (mirrors bm25_merge_maybe). Returns the number of segments
 * re-emitted. */
extern int   bm25_merge_rewrite_all(Relation index, const BM25FormatRestamp *restamp);

/* Dictionary iterator over one segment's sorted DICT chain (Task 23 merge feed).
 * Yields (term, termlen, post_root, post_off, df) per entry in stored order.
 * begin returns an opaque cursor (palloc'd); end frees it. The term pointer is
 * valid only until the next _next() call (it points into the currently SHARE-
 * locked DICT page's buffer copy). */
extern void *bm25_seg_dict_iter_begin(Relation index, BM25SegmentHeader *h);
extern bool  bm25_seg_dict_iter_next(void *iter, char **term, int *termlen,
                                     BlockNumber *post_root, uint16 *post_off,
                                     uint32 *df,
                                     BlockNumber *pos_post_root /* nullable */,
                                     uint16 *pos_post_off /* nullable */);
extern void  bm25_seg_dict_iter_end(void *iter);
/* MINIMAL nested-lock fix for the merge feed (bm25_merge.c) -- see the header
 * comment on bm25_seg_dict_iter_unlock (bm25_seg_dict.c) for the full contract,
 * including the requirement to copy term/termlen out BEFORE calling _unlock. */
extern void  bm25_seg_dict_iter_unlock(void *iter);
extern void  bm25_seg_dict_iter_relock(void *iter);

/* -------------------------------------------------------------------------
 * bm25_pending.c -- GIN-fastupdate-style WAL-logged pending list (Task 6)
 * -------------------------------------------------------------------------
 * aminsert appends each document's postings to the pending list under a short
 * metabuffer+tail-page exclusive lock; a manual seal (bm25_seal) drains the
 * chain into a BM25Accum and publishes a segment via the Task-5 two-phase commit,
 * then recycles the drained pages. The on-page record structs (BM25PendingDocHeader,
 * BM25PendingTermEntry) and the in-page iterator (BM25PendingIter) live in
 * bm25_format.h. heaprel is pre-opened by the caller (D-ALLOC/M6: never
 * table_open under a buffer lock). */
extern void  bm25_pending_append(Relation index, Relation heaprel, ItemPointer tid,
                                 BM25Token *toks, int ntok);
/* M5 multi-field append: each indexed column's tokens tagged with its dense
 * field_id. toks_by_field[f]/ntok_by_field[f] is field f's token list (a NULL
 * column is ntok 0). The drain reconstructs per-field postings from the stored
 * field_id on each term entry. bm25_pending_append is the field-0 wrapper.
 * key_type/key_size/key carry the row's fixed-width key_field value (key_type ==
 * BM25_KEY_NONE / key == NULL for a keyless index) so a sealed-from-pending doc
 * keeps its user key in the KEYMAP the next seal builds. */
extern void  bm25_pending_append_multi(Relation index, Relation heaprel,
                                       ItemPointer tid,
                                       BM25Token **toks_by_field,
                                       const int *ntok_by_field,
                                       uint32 field_count,
                                       uint8 key_type, uint16 key_size,
                                       const unsigned char *key);
/* Key config off the PENDING list, for a keyed index with no sealed keyed segment
 * yet (CREATE INDEX on an empty table, then INSERT). Returns false if the pending
 * list is empty or keyless. See bm25_ranked_keys_fill_from_pending. */
extern bool  bm25_pending_keymeta(Relation index, BlockNumber pending_head,
                                  uint32 epoch_bound,
                                  uint8 *out_type, uint16 *out_size);
extern bool  bm25_pending_should_seal(Relation index);
/* Drain the WHOLE pending chain into ORPHAN segments and hand back one
 * BM25SegCatEntry per output; the caller publishes them all in ONE record
 * (bm25_segcat_publish_append), which is also what detaches the chain.
 *
 * BUILD-04: one drain is not one segment. The accumulator is sealed and restarted
 * whenever it crosses `budget`, at a PAGE boundary and never under a content lock;
 * a document still being assembled from same-TID continuation records has
 * contributed nothing to the accumulator yet, so it lands whole in the next chunk.
 * Returns 0 when nothing survived to publish -- an empty chain, a chain whose every
 * doc had been tombstoned by VACUUM's pending sweep, or one holding only orphaned
 * continuation fragments -- in which case the publish record does only the anchor
 * detach. *out_entries is palloc'd in the caller's context. */
extern int   bm25_pending_drain(Relation index, Relation heaprel, Size budget,
                                BM25SegCatEntry **out_entries);
extern void  bm25_pending_truncate(Relation index, BlockNumber drained_head);
/* Detach the whole pending chain from the metapage in one metapage-only record,
 * writing the same five anchor fields the segment-publish record writes. Caller
 * MUST hold the seal singleton (that is what makes a FULL reset correct). Used
 * when a drain consumed a chain but published nothing -- an all-tombstoned chain
 * -- so the recycle that follows is never freeing pages the anchor still names
 * (issue #131). */
extern void  bm25_pending_reset_anchor(Relation index);
/* BUILD-05 / #292: reject an inserted row whose resolved key_field configuration
 * (type, width, key column) disagrees with the key-identity stamp the build left on
 * the field-config page rooted at field_config_blkno. An index without a stamp (built
 * by a binary predating #292) falls back to comparing type and width against the
 * KEYMAP of its first segment, and checks nothing while it has none. Called from
 * bm25_insert before the row reaches the pending chain -- see the rationale there
 * for why this is neither a DDL-time nor a seal-time check. */
extern void  bm25_validate_key_config_for_insert(Relation index,
                                                 BlockNumber field_config_blkno,
                                                 uint8 key_type, uint16 key_size,
                                                 int key_attno);
/* The drain+publish+recycle core for a caller that ALREADY holds the seal
 * singleton and an open heap Relation. One copy, shared by bm25_seal_index and
 * bm25_insert's opportunistic seal (PEND-16 -- they were byte-identical copies,
 * which is how issue #131 came to exist in two places). */
extern void  bm25_seal_pending_locked(Relation index, Relation heaprel);
/* The seal core, factored out of bm25_seal_sql so amvacuumcleanup can reuse the
 * proven singleton-protected drain+build+commit+truncate without duplicating it.
 * Takes an OPEN index Relation (caller holds the lock); opens the heap internally
 * via table_open(IndexGetRelation(...)) so callers (e.g. vacuumcleanup) need not
 * supply a heap Relation. No-ops cleanly when the CHAIN is empty, which is NOT
 * the same state as "no live docs recovered" -- a chain whose docs are all
 * tombstoned still yields zero entries but a valid drained_head, so the
 * anchor-detach record is written and the pages are recycled (BUILD-12; the
 * definition's header carries the full distinction). */
extern void  bm25_seal_index(Relation index);
extern int   bm25_seal_threshold_kb;     /* GUC backing var (KB) */
/* M2b: bm25_native.wand_top_k GUC backing var. Top-N heap size for the block-max WAND
 * accelerator on a bag-of-words scored (&@@) scan (bm25_scan_rank.c's seam
 * dispatcher); 0 disables WAND, forcing the exhaustive scorer unconditionally.
 * PGC_USERSET, so a session may dial it per-query (e.g. down to prove exhaustive
 * parity, or up past the default 100 for a large LIMIT that would otherwise pay
 * the over-pull tail-fallback rebuild). */
extern int   bm25_wand_top_k;

/* -------------------------------------------------------------------------
 * bm25_query.c -- M6 jsonb query tree (parse/flatten/eval/glob)
 * -------------------------------------------------------------------------
 * Forward declaration only -- bm25_query.h (included by the scanner's files and
 * bm25_query.c) carries the full struct definition and the parse/flatten/
 * eval/glob prototypes. This typedef alone is what lets BM25ScanOpaqueData
 * hold a `BM25Query *qtree` member (Task 3) without bm25.h itself depending
 * on the jsonb C API. */
typedef struct BM25Query BM25Query;

/* GUC-tunable wildcard expansion guardrails (D9 of the M6 design), registered
 * in bm25_handler.c's _PG_init alongside bm25_native.wand_top_k/bm25_native.seal_threshold.
 * bm25_wildcard_min_prefix is enforced at PARSE time (bm25_query_parse), so it
 * fires identically on the @@@ filter and &@@ ranked paths; the expansion cap is
 * enforced later and incrementally, in the expander's accumulator (bm25_wild_add,
 * bm25_seg_dict.c), which ERRORs -- never truncates -- as soon as the deduped term
 * set would exceed it, so the union is never materialized past the cap. */
extern int   bm25_wildcard_min_prefix;
extern int   bm25_wildcard_max_expansions;
/* Maintenance-interrupts pass: bm25_glob_match is O(pattern * term) worst case
 * (a run of alternating literal/'*' segments), and validate_wildcard_pattern had
 * no cap on either dimension of that product -- min_prefix/max_expansions bound
 * what a MATCHING pattern can cost to EXPAND, not what an arbitrary pattern costs
 * to EVALUATE. Both enforced at PARSE time, same PGC_SUSET reasoning as the pair
 * above (ADR 0025): a limit that exists to constrain the caller cannot be in the
 * caller's gift. */
extern int   bm25_wildcard_max_pattern_length;
extern int   bm25_wildcard_max_stars;

/* #62.5: KB one scan may spend materializing its match set; 0 means "follow
 * work_mem". Consumed by bm25_scan.c's bm25_match_doc_limit (ADR 0047). */
extern int   bm25_max_match_memory_kb;

/* bm25_native.debug_budget (KB, PGC_SUSET, 0 = off): overrides the maintenance
 * memory budget the build/merge/drain accumulator chunks against. TEST LEVER, not
 * a tuning knob -- maintenance_work_mem (or autovacuum_work_mem in an AV worker)
 * is the contract. See its _PG_init registration and ADR 0084. */
extern int   bm25_debug_budget_kb;

/* bm25_native.debug_count_slicing (PGC_SUSET, off): the segment writer cuts posting
 * blocks every BM25_POSTINGS_PER_BLOCK postings, as before issue #289, instead of
 * at document boundaries. TEST LEVER: it exists to build the straddling layout old
 * segments still carry. See its _PG_init registration. */
extern bool  bm25_debug_count_slicing;

/* bm25_native.debug_pause (PGC_SUSET, '' = off): names ONE pause point at which this
 * backend waits until the advisory lock (BM25_DEBUG_PAUSE_LOCKKEY, N) is free, N
 * being the point's 1-based position in bm25_handler.c's pause-point table. A TAP
 * test holds that lock to park a VACUUM, merge, INSERT or scan at an exact
 * interleaving (t/020, t/022, t/025, t/027, t/033).
 * TEST LEVER: inert unless set, and a superuser-only way to stall maintenance. */
#define BM25_DEBUG_PAUSE_LOCKKEY    0x626D3235      /* 'bm25'; pg_advisory_lock(int4, int4) key1 */
extern char *bm25_debug_pause;
extern void  bm25_debug_pause_point(const char *point);

/* bm25_native.debug_cancel_at (PGC_SUSET, '' = off): names pause points at which this
 * backend sets its own query-cancel flags, as a SIGINT would, before any park that
 * debug_pause asks for at the same point. A single-session suite uses it to show a
 * cancel arriving mid-loop is serviced at the step it arrives on or the next, not at
 * the loop's end (sql/148, keymap_rotation; sql/66, scan_post_page; sql/80,
 * drain_pending_page and debug_dict_entry -- the last three replaced a
 * statement_timeout, whose service time is a property of the runner, ADR 0070).
 * bm25_native.debug_cancel_after (PGC_SUSET, 0 = first hit) holds the cancel back
 * until bm25_work_units reaches it, which is how those three land mid-loop rather
 * than on the first iteration. TEST LEVERS: inert unless set. */
extern char *bm25_debug_cancel_at;
extern int   bm25_debug_cancel_after;

/* The decode boundary for one BM25PendingTermEntry, and THE definition of that
 * record's on-page stride: bounds the fixed header and then the MAXALIGN'd
 * (header + termlen + pos_bytes) span to [cur, end), and returns that span so the
 * caller advances without recomputing it. Exactly bm25_dictentry_validate's shape
 * for the segment-side record, and declared here beside it (and beside the pending
 * iterator below) rather than in bm25_format.h, which carries the record STRUCTS and
 * the static-inline offset helpers over them, not the out-of-line walkers.
 * Callers: bm25_pending_iter_next and the drain (bm25_pending.c), and the wildcard
 * expander (bm25_seg_dict.c). */
extern Size  bm25_pending_term_entry_span(const char *cur, const char *end);
/* The validated walk of a CAPTURED pending chain (#291): every scan-side walker that
 * follows a BM25ScanSnapshot's pending_head reads its pages through this. Per page it
 * checks the extent, a visited-count cycle cap, the chain epoch against the
 * snapshot's next_gen (ERRCODE_T_R_SERIALIZATION_FAILURE: the page was re-initialized
 * after the capture, which a hot standby without feedback can see), then the page
 * kind. The writer-side walkers (drain and truncate under the seal singleton,
 * VACUUM's mark-dead sweep under its ShareLock side) hold a lock that excludes the
 * seal, which keeps the chain from being recycled under them, and carry their own
 * extent and cycle checks, so they do not use it.
 *
 * Usage: bm25_pending_walk_init once per walk; then for each block,
 * buf = bm25_pending_walk_read(&w, blk) returns it pinned and SHARE-locked, and the
 * caller reads the records and nextblk off that page (or a copy) and releases it. */
typedef struct BM25PendingWalk
{
    Relation        index;
    BlockNumber     nblocks;        /* extent sample; 0 until the first read */
    uint64          visited;        /* pages read so far, for the cycle cap */
    uint32          epoch_bound;    /* BM25ScanSnapshot.next_gen */
} BM25PendingWalk;
extern void  bm25_pending_walk_init(BM25PendingWalk *w, Relation index,
                                    uint32 epoch_bound);
extern Buffer bm25_pending_walk_read(BM25PendingWalk *w, BlockNumber blk);
/* The canonical in-page pending iterator (shared by drain + VACUUM sweep). */
extern void  bm25_pending_iter_begin(BM25PendingIter *it, Page page);
extern bool  bm25_pending_iter_next(BM25PendingIter *it);
extern void  bm25_pending_iter_end(BM25PendingIter *it);
extern bool  bm25_pending_page_has_dead(Page page, IndexBulkDeleteCallback cb,
                                        void *cb_state);
/* VACUUM sweep (Task 17): invalidate pending entries whose TID is dead per cb and
 * decrement the metapage doc counts; returns the count cleared. */
extern uint64 bm25_pending_mark_dead(Relation index, IndexBulkDeleteCallback cb,
                                     void *cb_state);

/* -------------------------------------------------------------------------
 * bm25_seg_read.c -- sealed-segment reader (Phase 1 minimal, Task 5)
 * (split by #228, ADR 0101, across bm25_seg_read.c, bm25_seg_dict.c,
 * bm25_seg_chain.c and bm25_seg_debug.c; each file's header says what it holds)
 * -------------------------------------------------------------------------
 * Snapshot the catalog, read a segment header, linear-scan the chained DICT for a
 * term, decode a term's block-encoded postings, test the live-docs bit, and map a
 * local doc-id to its heap TID. Option (d) reuse-safety: every followed segment
 * page's stamped seg_gen is validated against the expected (catalog/header) gen;
 * a mismatch aborts the scan cleanly (gens never repeat => no ABA). */
typedef struct BM25SegReader BM25SegReader;   /* opaque per-(segment) reader */

/* D-SNAP / C3: scan-start snapshot captured atomically under ONE metapage SHARE
 * lock. Holds the pending_head, segcat_root, and a palloc'd copy of ALL live
 * BM25SegCatEntry (Model A) plus the global stats (ndocs/total_len/k1/b).
 * Callers MUST use bm25_scan_snapshot rather than pairing bm25_meta_read with
 * bm25_segcat_read: two separate metapage locks allow a seal to interleave and
 * either double-score or silently drop a document. */
typedef struct BM25ScanSnapshot
{
    BlockNumber     pending_head;   /* pending-list head at snapshot time */
    BlockNumber     segcat_root;    /* live catalog chain root at snapshot time */
    BM25SegCatEntry *segs;          /* Model A: palloc'd COPY of ALL live catalog
                                     * entries, taken while still holding the metapage
                                     * SHARE lock (the catalog is bounded -- a handful
                                     * of pages even at scale -- so copying it under the
                                     * lock is cheap and means the scan never re-reads a
                                     * catalog page after the snapshot). Allocated in the
                                     * caller's CurrentMemoryContext at bm25_scan_snapshot
                                     * time; the caller must keep that context alive for
                                     * the scan (or pfree segs before resetting it). */
    uint32          nsegs;          /* live segment count (length of segs[]) */
    uint64          ndocs;          /* global live-doc count (for idf/avgdl) */
    uint64          total_len;      /* global Sigma doclen (for avgdl) */
    float8          k1;             /* BM25 k1 from the metapage */
    float8          b;              /* BM25 b from the metapage */
    uint32          analyzer_fingerprint;   /* v4: index analyzer fp; what both fingerprint
                                             * gates (scan start, per-row ingest) compare against */
    uint32          field_count;            /* v4: cached field count (1 in M3) */
    BlockNumber     field_config_blkno;     /* M5: field-config chain root (k1/b/boost per
                                             * field); captured under the same metapage lock
                                             * so the BM25F scorer reads the config once per
                                             * scan (m6) -- Invalid => single default field. */
    uint32          next_gen;       /* #291: meta.next_gen at snapshot time, the epoch
                                     * BOUND every walk of pending_head passes to
                                     * bm25_pending_walk_init. A pending chain's epoch is
                                     * drawn from next_gen when the chain starts, in the
                                     * metapage record that publishes the new head, so
                                     * every page of the captured chain carries an epoch
                                     * below this value (or 0, a chain an older binary
                                     * started), and a page re-initialized after the
                                     * capture carries one at or above it. In memory
                                     * only; nothing on disk. */
} BM25ScanSnapshot;

/* ONE metapage SHARE lock; copies the whole live catalog (Model A). */
extern void  bm25_scan_snapshot(Relation index, BM25ScanSnapshot *out);
/* A palloc'd copy of the whole live catalog. Unlike bm25_scan_snapshot, the metapage
 * lock covers only the segcat_root read; the walk that follows holds no metapage lock.
 * So the caller MUST hold the seal/merge singleton (LockPage on the metapage,
 * ShareLock or stronger): a catalog page orphaned by a swap gets no retire_xid, the next
 * bm25_reclaim_orphans frees it for immediate reuse, and only the singleton keeps that
 * swap out of the walk (issue #270), so a walker holding it reads the root of a chain
 * no sweep frees -- even the share-mode sweep that may run beside it (issue #300),
 * which frees only chains orphaned before the walker took the lock. Production code calls the _locked
 * form, which asserts the lock under cassert. The plain form is for the debug SRFs,
 * which read unlocked and can fail with XX002 when a merge and a VACUUM land inside the
 * walk. A caller holding no singleton that needs only entry 0 uses
 * bm25_segcat_first_entry, which reads the root under the metapage SHARE and returns
 * false when the catalog is empty. */
extern void  bm25_segcat_read_locked(Relation index, BM25SegCatEntry **out, uint32 *nsegs);
extern void  bm25_segcat_read(Relation index, BM25SegCatEntry **out, uint32 *nsegs);
extern bool  bm25_segcat_first_entry(Relation index, BM25SegCatEntry *out);
/* M6 Task 7: expand a WILDCARD leaf's glob pattern into the DISTINCT set of dict
 * terms it matches across all live segments + pending (deduped into ONE set so a
 * term in N sources scores once under the leaf bit). Returns the count; *out_terms
 * is a cxt-palloc'd BM25Token array (ptr/len valid, rest inert). Matches the RAW
 * lowercased pattern against the STEMMED dict bytes (D8: no re-stem). Field scope
 * is applied by the caller at scoring time. ERRORs on the max_expansions cap. */
extern int   bm25_dict_expand_wildcard(Relation index, const BM25ScanSnapshot *snap,
                                       const char *pattern, int patlen,
                                       MemoryContext cxt, BM25Token **out_terms);
/* Segment-page decode boundary: seg_gen (option (d)) THEN page kind (H6), in
 * that order -- see the definition for why the order is load-bearing. want_kind
 * is the BM25_PAGE_* bit (or OR of bits) this chain may reach, tested as
 * `flags & want_kind`, never equality (BM25_PAGE_DELETED can be ORed onto a live
 * kind). Either argument may be 0 to skip that half. */
extern void  bm25_seg_page_validate_kind(Page page, uint32 expected_gen,
                                         uint16 want_kind);
extern void  bm25_seg_page_validate(Page page, uint32 expected_gen); /* want_kind == 0 wrapper */
/* A BM25_PAGE_SEGCAT page plays one of two roles, told apart only by seg_gen (issues
 * #276, #302, #303): a catalog page carries 0, a segment header page its segment's
 * gen (>= 1). bm25_segcat_page_validate checks the content bounds, the SEGCAT kind
 * and the role, in that order, and returns the content byte count. */
typedef enum BM25SegcatRole
{
    BM25_SEGCAT_ROLE_CATALOG,
    BM25_SEGCAT_ROLE_HEADER
} BM25SegcatRole;
extern Size  bm25_segcat_page_validate(Page pg, BM25SegcatRole role, BlockNumber blk);
/* The gen arm (issue #303): a page of a sealed chain that carries another segment's
 * gen. The caller has released the page; this decides the class and raises. A segment
 * still in the live catalog cannot have had its pages reclaimed, so a mismatch there is
 * corruption (ERRCODE_INDEX_CORRUPTED); one that has left it is the reclaim race
 * (ERRCODE_T_R_SERIALIZATION_FAILURE, as before). The catalog is read under the
 * metapage SHARE (bm25_scan_snapshot), never by an unlocked walk (ADR 0107); a caller
 * holding the seal/merge singleton needs no read at all. Hold no content lock. Never
 * returns; not marked so, because the noreturn macro is spelled differently across the
 * supported majors. */
extern void  bm25_seg_gen_mismatch(Relation index, BlockNumber blk, uint32 page_gen,
                                   uint32 want_gen, const char *what);
/* The decode boundary for one BM25DictEntry: bounds the fixed header AND the
 * MAXALIGN'd term span to [cur, end), then returns the entry's total on-page
 * length so the caller can advance a cursor without recomputing it. Shared by
 * every DICT-chain walker (bm25_seg_dict.c's lookup/iterator, bm25_segment.c's
 * debug SRFs) so a corrupt termlen is caught once, in one place, for all of them. */
extern Size  bm25_dictentry_validate(const char *cur, const char *end);
/* The dictionary's byte order: memcmp, then shorter first. Locale-independent, and the
 * order the builder sorts the DICT chain in (bm25_seg_dict.c). */
extern int   bm25_term_cmp(const char *a, int alen, const char *b, int blen);
/* DICT chain order across pages (issue #303), for every DICT walker: a DICT page holds
 * at least one entry, and its first term sorts strictly after the previous page's last.
 * That is what ends a cycle through DICT pages and what catches an unsorted page
 * boundary, which made a present term read as absent. Per walk: bm25_dict_order_init;
 * per page, bm25_dict_page_first on the page's entry span before decoding it and
 * bm25_dict_page_last with its last term after; walks that check order within a page
 * too (the merge iterator, the dumps) call bm25_dict_entry_order_validate per entry.
 * Whole-chain walks count entries in nentries and check bm25_dict_count_validate
 * against the segment header's nterms at the chain's end. `last` is BLCKSZ because
 * bm25_dictentry_validate bounds a term by its page, not by BM25_MAX_TERM_BYTES. */
typedef struct BM25DictOrder
{
    int             lastlen;    /* -1 before the first page */
    uint64          nentries;   /* entries counted by the caller */
    char            last[BLCKSZ];
} BM25DictOrder;
extern void  bm25_dict_order_init(BM25DictOrder *o);
extern void  bm25_dict_page_first(const BM25DictOrder *o, const char *cur,
                                  const char *end, BlockNumber blk);
extern void  bm25_dict_page_last(BM25DictOrder *o, const char *term, int termlen);
extern void  bm25_dict_entry_order_validate(const char *prev, int prevlen,
                                            const char *term, int termlen,
                                            BlockNumber blk);
extern void  bm25_dict_count_validate(uint64 nentries, uint32 nterms);
/* The decode boundary for a page's content byte count (pd_lower -
 * SizeOfPageHeaderData). PageIsVerifiedExtended does not bound pd_lower from
 * below, so every reader that derives a length from it (a chain page's content
 * span, a KEYMAP page's key[] span, a DICT/SEGCAT/retired-list entry count,
 * ...) must bound it itself before subtracting -- this is the one place that
 * does. Used at every read-side pd_lower-derived-length site in the tree except
 * the write-side chain/page builders (reading back only pages they themselves just
 * initialized). Also checks pd_special (issue #302.D). See its definition in
 * bm25_seg_read.c for the caller list and the reasoning for the exception. */
extern Size  bm25_page_content_bytes(Page pg);
/* Issue #302.D: every bm25 page puts its BM25PageOpaque at BM25_PAGE_SPECIAL_OFF, and
 * BM25PageGetOpaque trusts pd_special to say so. bm25_page_special_ok is the bare
 * compare (for bm25_page_alloc, which drops rather than raises);
 * bm25_page_special_validate raises ERRCODE_INDEX_CORRUPTED, for opaque readers that
 * do not go through bm25_page_content_bytes, which runs the same compare. */
#define BM25_PAGE_SPECIAL_OFF   (BLCKSZ - MAXALIGN(sizeof(BM25PageOpaque)))
static inline bool
bm25_page_special_ok(Page pg)
{
    return ((PageHeader) pg)->pd_special == BM25_PAGE_SPECIAL_OFF;
}
extern void  bm25_page_special_validate(Page pg, BlockNumber blk);
/* Gate an on-disk BlockNumber before ReadBuffer. InvalidBlockNumber is P_NEW, so an
 * unchecked corrupt pointer EXTENDS the relation on a read-only scan rather than
 * failing; see the definition in bm25_seg_read.c for why the extent bound that looks
 * like the obvious third clause is deliberately absent. */
extern void  bm25_seg_blkno_validate(BlockNumber blkno, const char *what);
/* The per-walk extent bound (SEGREAD-11): ERRCODE_INDEX_CORRUPTED when blk >= nblocks,
 * naming the chain in `what`. The caller captures nblocks once per walk or per cursor,
 * never per pointer. Exported for the KEYMAP walker in bm25_keymap.c (issue #225), so
 * every segment chain reports an out-of-extent link through the same function. */
extern void  bm25_seg_chain_extent_validate(BlockNumber blk, BlockNumber nblocks,
                                            const char *what);
/* The re-sampling extent test: true when blk < *nblocks; otherwise re-reads the
 * relation's extent into *nblocks once and tests again. For a walker whose sample can
 * legitimately be stale-low: the SEGCAT walkers (bm25_seg_read.c, #225), whose sample
 * precedes their root read and can miss a page published in between, and the
 * pending-chain walkers (bm25_pending.c, #243), whose sample can go stale mid-walk as
 * appends link new pages. Each file's note gives the argument. Steady-state cost is
 * no lseek beyond the caller's own sample. */
extern bool  bm25_blk_in_extent(Relation index, BlockNumber blk, BlockNumber *nblocks);
/* Bound a DICT-supplied uint16 byte offset before `PageGetContents(pg) + off`. Not
 * covered by bm25_dictentry_validate, which bounds the entry header and term span
 * only. See the definition for why the test is `>` rather than `>=`. */
extern void  bm25_seg_page_off_validate(uint32 off, Size pagebytes, const char *what);
extern void  bm25_seg_header_read(Relation index, BlockNumber header_blkno,
                                  uint32 expected_gen, BM25SegmentHeader *out);
/* Header + trailing per-field sumdoclen (out_lens) AND N_field (out_ndocs) under one
 * SHARE lock; both buffers >= BM25_MAX_FIELDS. The BM25F scorer's corpus-stat pass. */
extern void  bm25_seg_header_read_lens(Relation index, BlockNumber header_blkno,
                                       uint32 expected_gen, BM25SegmentHeader *out,
                                       uint64 *out_lens /* >= BM25_MAX_FIELDS */,
                                       uint64 *out_ndocs /* >= BM25_MAX_FIELDS */);
/* Look up a term; out-params its POST location + df AND (M4) its POS-chain entry
 * (pos_post_root/pos_post_off). pos_post_root == InvalidBlockNumber for a segment
 * sealed before M4 (pos_root Invalid) -- the caller then never opens the POS chain.
 * pos_post_root/pos_post_off may be NULL for a caller that only wants POST. */
extern bool  bm25_seg_dict_lookup(Relation index, BM25SegmentHeader *h,
                                  const char *term, int termlen,
                                  BlockNumber *post_root, uint16 *post_off, uint32 *df,
                                  BlockNumber *pos_post_root, uint16 *pos_post_off);
typedef void (*bm25_post_cb)(uint32 local_docid, uint32 tf, uint32 field_id, void *state);
/* M4 position callback: fires per posting immediately AFTER bm25_post_cb, ONLY when
 * a non-NULL pos_cb is passed to bm25_seg_scan_postings AND the posting's field bears
 * positions. positions[] is the ascending decoded per-(doc,field) ordinal list of
 * length npos (== the posting's tf). state is the SAME pointer passed as pos_state. */
typedef void (*bm25_pos_cb)(uint32 local_docid, uint32 field_id,
                            const uint32 *positions, uint32 npos, void *state);
/* Decode exactly `df` postings of one term, invoking cb(local_docid, tf, field_id,
 * state) per posting. field_id is decoded from the per-block field-id RLE stream
 * (section 3.5); single-field segments (field_rle_bytes == 0) emit field_id = 0 for every
 * posting. The df bound is REQUIRED, not optional: a segment lays ALL terms' blocks
 * into ONE shared chain (D-POST) with no inter-term delimiter, so a term's run is
 * bounded solely by its posting count. `state` is forwarded to cb directly (no
 * wrapper struct -- callers pass their own callback state). */
/* Decode `df` postings, invoking cb per posting. M4: when pos_cb != NULL a SECOND
 * lockstep cursor is opened at (pos_post_root, pos_post_off) and, per posting whose
 * field bears positions, one frame is decoded and pos_cb fired right after cb. The
 * per-field gate is field_store_positions[field_id] (length field_count); NULL means
 * "every field bears positions" (the default all-on index). A posting on a
 * positions-off field consumes NO frame (the builder wrote none). Passing pos_cb ==
 * NULL is the pre-M4 bag-of-words path: the POS chain is never touched.
 *
 * Self-check (D2): the frame's decoded tf-count MUST equal the posting's tf; a
 * mismatch is a hard elog(ERROR) (stream desync), never a silent misparse. */
extern void  bm25_seg_scan_postings(Relation index, BlockNumber post_root,
                                    uint16 post_off, uint32 df,
                                    uint32 expected_gen,
                                    bm25_post_cb cb, void *state,
                                    BlockNumber pos_post_root, uint16 pos_post_off,
                                    bm25_pos_cb pos_cb, void *pos_state,
                                    const bool *field_store_positions,
                                    uint32 field_count);
/* M2b Task 3 (WAND fast path): read ONE block's header + trailing impact table --
 * no docid/tf/RLE decode -- and hand back the (blk, off) of the next block
 * (*next_blk == InvalidBlockNumber at chain end). The caller supplies the df
 * bound and tracks cumulative ndocs itself (see bm25_seg_scan_postings' df-bound
 * comment: a term's blocks carry no end marker, D-POST shares one chain across
 * every term in the segment). */
extern void  bm25_seg_block_header_read(Relation index, BlockNumber blk, uint16 off,
                                        uint32 expected_gen, BM25BlockHeader *hdr,
                                        BM25BlockImpact *imp,
                                        BlockNumber *next_blk, uint16 *next_off);
/* Issue #289: the same header-only read, plus the block's LEAD (bm25_format.h:
 * BM25BlockLead) -- its first document's docid and postings. That is what tells a
 * reader whether, and with which postings, the previous block's last document
 * continues into this one (a "straddle"), without decoding the rest of the run.
 * lead may be NULL. */
extern void  bm25_seg_block_header_read_lead(Relation index, BlockNumber blk,
                                             uint16 off, uint32 expected_gen,
                                             BM25BlockHeader *hdr,
                                             BM25BlockImpact *imp,
                                             BlockNumber *next_blk, uint16 *next_off,
                                             uint32 match_docid, BM25BlockLead *lead);
extern bool  bm25_seg_doc_is_live(Relation index, BM25SegmentHeader *h, uint32 local_docid);
extern ItemPointerData bm25_seg_docid_to_tid(Relation index, BM25SegmentHeader *h,
                                             uint32 local_docid);
extern uint32 bm25_seg_doclen(Relation index, BM25SegmentHeader *h, uint32 local_docid); /* sum over fields */
/* Per-(doc,field) doclen from the packed per-field NORMS (cell docid*field_count+
 * field_id). C3's BM25F scorer reads doclen_f from here; the merge reads every
 * field's cell to re-register a doc with its per-field doclens. */
extern uint32 bm25_seg_doclen_field(Relation index, BM25SegmentHeader *h,
                                    uint32 local_docid, uint32 field_id);

/* H16 -- forward-only cursors over a segment's dense per-docid chains.
 *
 * The four one-shot functions above each re-walk their chain FROM THE ROOT, and all
 * of them are called per scored posting (and per doc per field in the merge replay).
 * On a 1M-doc single-field segment the NORMS chain is ~490 pages, so a term with
 * df = 500k cost ~1.2e8 ReadBuffer + LWLock pairs for that term alone, growing
 * quadratically with segment size.
 *
 * A BM25SegReader holds one cursor per chain and resumes each lookup from the page
 * the previous one landed on, which every hot caller can exploit because they all
 * walk docids forward (ascending-(docid,field_id) posting order, the merge's
 * `for d in 0..ndocs`, WAND's forward-only skipping) and one docid's field_count
 * NORMS cells are contiguous. Ascending access becomes O(1) amortized.
 *
 * A cursor is a pure optimization and CANNOT change an answer, but that is a property
 * of the RESUME TEST, not something the shape of the struct gives for free. The test
 * resumes only when the cursor names the SAME CHAIN ROOT the caller is asking about
 * and the target is at or after the cursor's page; every page actually read is still
 * seg_gen- and page-kind-validated. So a reader needs no invalidation and no reset
 * between segments to stay correct, and, without page images (below), no cleanup --
 * it is plain scalars, normally stack-declared. Using the one-shot functions instead
 * is always correct, just slower.
 *
 * Issue #267 adds an optional PAGE IMAGE per cursor: a BLCKSZ copy of the page the
 * last lookup landed on, so the next lookup on that page reads the copy with no
 * ReadBuffer and no content lock (before it, a frequent term paid one NORMS and one
 * DOCMAP buffer access per scored pair even with the cursor already on the page). Only
 * cursors over chains that are immutable for their gen may keep one -- NORMS, DOCMAP
 * and KEYMAP, written once before the segment is published -- and only after an
 * explicit bm25_seg_reader_cache_pages; see there. The image lives in caller-chosen
 * memory, not a pinned buffer, so it needs no resource-owner cleanup and is freed with
 * its context on ERROR; bm25_seg_reader_release_pages frees it early.
 *
 * SEGREAD-14 (issue #154) is why `root` is recorded at all. Before it the test was
 * blk/seen only, so a cursor carried over to a DIFFERENT chain -- another segment's
 * NORMS, or this segment's DOCMAP -- resumed at a page belonging to the old chain with
 * the old chain's `seen` as the byte origin. No production caller did that (each one
 * re-inits per segment and keeps one cursor per chain), so this closes a latent trap
 * rather than a live bug; what it buys is that the "cannot change an answer" sentence
 * above is now enforced instead of merely being true of today's callers.
 *
 * What every cursor caches is the byte-offset -> page MAPPING, which is stable for any
 * chain a reader can see; only an opted-in NORMS, DOCMAP or KEYMAP cursor also caches
 * page CONTENT (the image above). That is worth stating because LIVEDOCS content is NOT
 * immutable: bm25_livedocs_clear tombstones a doc after the segment is sealed. It only
 * flips a bit inside the existing bitmap -- it never rewrites pd_lower and never extends
 * or splices the chain -- and the mapping depends on nothing else, so a cursor into a
 * LIVEDOCS chain stays valid across tombstoning. The cell it then reads is the current
 * one, since the read itself is what fetches the page.
 *
 * `h` is captured BY POINTER: the caller must keep the header alive for the reader's
 * lifetime (every current caller declares both in the same scope). */
typedef struct BM25ChainCursor
{
    BlockNumber blk;        /* page a previous lookup landed on; Invalid = unpositioned */
    /* Chain this cursor's blk/seen describe -- the `root` chain_read_at was called
     * with when it landed there. The resume test compares it against the root of the
     * chain now being read, so a cursor pointed at a different chain falls back to a
     * fresh root walk instead of resuming at a byte offset that means nothing there.
     * InvalidBlockNumber when unpositioned; a zero-filled cursor leaves this 0, which
     * is BM25_METAPAGE_BLKNO and therefore never a chain root either. */
    BlockNumber root;
    /* Content bytes on the pages BEFORE blk. Size, not uint32: a chain whose total
     * content exceeds 4GB (a large NORMS/LIVEDOCS/DOCMAP chain on a big table) would
     * otherwise wrap this accumulator itself before chain_read_at's per-iteration
     * (Size) cast could do anything about it -- the cast only helps the COMPARISON,
     * not an already-wrapped stored value. */
    Size        seen;
    /* Index extent captured when the cursor was opened: the chain walk's hard bound
     * (SEGREAD-11). Captured here rather than per chain_read_at call because
     * chain_read_at runs per scored posting and RelationGetNumberOfBlocks lseeks on
     * every call outside recovery -- ADR 0071 declines that cost in the innermost
     * loop and names this exact shape (pos_cursor_open's) as the one to copy when a
     * bound IS wanted: one lseek per cursor, not one per block. Nothing in this
     * extension truncates the relation, so a value captured at open stays a LOWER
     * bound on the extent for the cursor's whole life and cannot reject a page that
     * legitimately exists. 0 means "not captured" -- a zero-filled cursor whose owner
     * skipped bm25_seg_reader_init -- and makes chain_read_at capture per call rather
     * than reject every block. */
    BlockNumber nblocks;
    /* Page image (issue #267). img_cxt NULL means this cursor never keeps one; only
     * bm25_seg_reader_cache_pages sets it, and never on a LIVEDOCS cursor. img is
     * allocated in img_cxt on first use and holds a copy of page img_blk taken under
     * its share lock after the same kind/gen/extent checks as a fresh read;
     * img_blk is InvalidBlockNumber while img holds nothing. A zero-filled cursor has
     * img_cxt NULL, so it never reads an image either. */
    MemoryContext img_cxt;
    char       *img;
    BlockNumber img_blk;
} BM25ChainCursor;

/* Every field, so no cursor is left with an uninitialized image pointer: the reader
 * init and the one-shot paths' stack cursors all start here. */
static inline void
bm25_chain_cursor_init(BM25ChainCursor *c, BlockNumber root, BlockNumber nblocks)
{
    c->blk     = InvalidBlockNumber;
    c->root    = root;
    c->seen    = 0;
    c->nblocks = nblocks;
    c->img_cxt = NULL;
    c->img     = NULL;
    c->img_blk = InvalidBlockNumber;
}

/* BM25SegWalk: the validated page reader for sealed segment chains (issue #303), the
 * segment-side twin of BM25PendingWalk. Every walk along a sealed chain -- DICT, POST,
 * POS, LIVE, DOCMAP, NORMS, KEYMAP -- reads its pages through it, so the per-page checks
 * live in one place (bm25_seg_chain.c holds the implementation and the reasoning):
 * block 0, extent, a revisit test, a visit cap, content bytes, gen, kind. Every failure
 * is ERRCODE_INDEX_CORRUPTED except a gen mismatch on a segment that has left the live
 * catalog, which is the reclaim race and stays retryable (40001).
 *
 * Usage: bm25_seg_walk_init once per walk (stores only), then bm25_seg_walk_read per
 * page, which returns the buffer pinned and SHARE-locked with *pagebytes validated; the
 * caller releases it. nblocks is the caller's extent sample, taken after the chain was
 * published (ADR 0095); 0 makes the first read take it. A quiet walk (the debug dumps)
 * re-samples at a link past the extent and gets InvalidBuffer back if it is still past
 * it, where an ordinary walk ERRORs. A walk that can start past its chain's root (a
 * cursor resume) sets `root` after the init, so a link back to the root is a revisit
 * too. bm25_seg_walk_check runs the pre-read checks alone, for a walker that hands a
 * block on without reading it (bm25_livedocs_locate). */
typedef struct BM25SegWalk
{
    Relation        index;
    BlockNumber     nblocks;    /* extent sample; 0 = take it at the first read */
    BlockNumber     start;      /* first block this walk read (Invalid before) */
    BlockNumber     prev;       /* block read last (Invalid before) */
    BlockNumber     root;       /* the chain's root when the walk may start past it
                                 * (a cursor resume); Invalid when not known */
    uint32          visited;    /* pages read so far, for the cap */
    uint32          gen;        /* the segment's gen; 0 = no gen check */
    uint16          kind;       /* BM25_PAGE_* */
    bool            quiet;      /* debug dumps: a link past the extent ends the walk */
    const char     *what;       /* chain name for messages */
} BM25SegWalk;

static inline void
bm25_seg_walk_init(BM25SegWalk *w, Relation index, BlockNumber nblocks, uint32 gen,
                   uint16 kind, bool quiet, const char *what)
{
    w->index   = index;
    w->nblocks = nblocks;
    w->start   = InvalidBlockNumber;
    w->prev    = InvalidBlockNumber;
    w->root    = InvalidBlockNumber;
    w->visited = 0;
    w->gen     = gen;
    w->kind    = kind;
    w->quiet   = quiet;
    w->what    = what;
}
extern bool  bm25_seg_walk_check(BM25SegWalk *w, BlockNumber blk);
extern Buffer bm25_seg_walk_read(BM25SegWalk *w, BlockNumber blk, Size *pagebytes);
/* The image-aware read chain_read_at and the KEYMAP walk share (issue #267): the
 * cursor's page image when it holds blk at the walk's gen (*buf = InvalidBuffer), else
 * bm25_seg_walk_read's share-locked buffer (*buf set). An image hit runs the revisit
 * test and the cap like a read, and the kind check on the image. Release with
 * bm25_chain_page_done, after calling bm25_chain_page_keep on the page a lookup landed
 * on. cur may be NULL. */
extern Page  bm25_seg_walk_get(BM25SegWalk *w, BM25ChainCursor *cur, BlockNumber blk,
                               Buffer *buf, Size *pagebytes);
extern void  bm25_chain_page_keep(BM25ChainCursor *cur, BlockNumber blk, Page pg,
                                  Buffer buf, uint16 want_kind);
extern void  bm25_chain_page_done(Buffer buf);

/* The content bytes the segment builder packs onto every page of a dense per-docid
 * chain (BM25_PAGE_LIVE, _DOCMAP or _NORMS) except the chain's last (issues #293/#294).
 * The readers address these chains by byte offset (and bm25_livedocs_locate by a
 * fixed bits-per-page divide), so a non-final page holding anything else misaddresses
 * every later docid; the walkers reject one rather than read through it. */
extern Size  bm25_chain_full_span(uint16 kind);
/* The KEYMAP twin (issue #303): key[] bytes bm25_keymap_write leaves on every KEYMAP
 * page but the last, for key width key_size. The root carries the BM25KeymapHeader
 * first, so its span is measured after the header and is smaller. */
extern Size  bm25_keymap_full_span(uint16 key_size, bool root);

typedef struct BM25SegReader
{
    Relation            index;
    BM25SegmentHeader  *h;
    BM25ChainCursor     norms;
    BM25ChainCursor     livedocs;
    BM25ChainCursor     docmap;
    /* KEYMAP (issue #225), read by bm25_seg_reader_key in bm25_keymap.c. Its `seen`
     * counts key[] bytes only -- the root page's BM25KeymapHeader is excluded -- so it
     * is a different origin from the other three cursors' and is walked by its own
     * loop rather than chain_read_at. */
    BM25ChainCursor     keymap;
    /* key_size as decoded (and validated) off the KEYMAP root header by the first
     * lookup that visited the root; 0 until then. A resume that starts past the root
     * never re-reads the header, and needs key_size to turn a docid into a byte offset
     * before it can decide whether it may resume at all -- so it is cached here, and
     * trusted only while the keymap cursor's root still names this reader's chain. */
    uint16              key_size;
    /* Set by bm25_seg_reader_init_checked when the segment's LIVEDOCS bitmap was all
     * set at init (or by bm25_seg_reader_init_known, handed that result for the same
     * segment): bm25_seg_reader_doc_is_live then answers true without a per-lookup
     * read. See bm25_seg_reader_init_checked. */
    bool                assume_live;
} BM25SegReader;

extern void  bm25_seg_reader_init(BM25SegReader *r, Relation index,
                                  BM25SegmentHeader *h);
/* bm25_seg_reader_init for a query reader (issue #229, ADR 0100). When the segment's
 * LIVEDOCS bitmap is all set -- checked once here, one page per ~65k documents --
 * liveness is answered from that instead of one LIVEDOCS buffer access per lookup,
 * which measured 36-92% of a frequent-term WAND build's buffer traffic. The check
 * runs only when expected_lookups (an upper bound on the reader's liveness lookups)
 * is at least twice the bitmap's page count. Returns the flag it set. Only query
 * paths opt in: VACUUM's bulkdelete and the merge must see the bits per document. */
extern bool  bm25_seg_reader_init_checked(BM25SegReader *r, Relation index,
                                          BM25SegmentHeader *h,
                                          uint64 expected_lookups);
/* Same segment, same call: reuse another reader's init_checked result. */
extern void  bm25_seg_reader_init_known(BM25SegReader *r, Relation index,
                                        BM25SegmentHeader *h, bool all_live);
/* Issue #267: let this reader's NORMS, DOCMAP and KEYMAP cursors keep a page image in
 * cxt (8 KB each, allocated on first use), so same-page lookups cost no buffer
 * access. Call after the init; a re-init turns it off. cxt must outlive every use of
 * the reader, and the caller bounds how many readers hold images at once. */
extern void  bm25_seg_reader_cache_pages(BM25SegReader *r, MemoryContext cxt);
/* Free the reader's page images now rather than with their context. The reader stays
 * usable and allocates again if it is used again. */
extern void  bm25_seg_reader_release_pages(BM25SegReader *r);
extern bool  bm25_seg_reader_doc_is_live(BM25SegReader *r, uint32 local_docid);
extern ItemPointerData bm25_seg_reader_docid_to_tid(BM25SegReader *r,
                                                    uint32 local_docid);
extern uint32 bm25_seg_reader_doclen_field(BM25SegReader *r, uint32 local_docid,
                                           uint32 field_id);
/* Whole-doc length (sum over fields) through the reader's NORMS cursor: the reader
 * form of bm25_seg_doclen, with the identical summation. */
extern uint32 bm25_seg_reader_doclen(BM25SegReader *r, uint32 local_docid);
/* Read-only catalog lookup by generation (Task-0 contract): walk the catalog chain
 * for the entry whose gen matches, copy it into *out, return true (false if gone).
 * TEST/DEBUG-ONLY: no production caller (the Phase-4 merge never adopted it); only
 * bm25_debug_segcat_walk calls it -- see its definition. Distinct from the mutation
 * locator bm25_segcat_locate_entry below (which keys on header_blkno and returns the
 * physical block+slot for an in-place WAL mutation). */
extern bool  bm25_segcat_find_entry(Relation index, uint32 gen, BM25SegCatEntry *out);

/* -------------------------------------------------------------------------
 * bm25_keymap.c -- per-segment docid->key flat array (BM25_PAGE_KEYMAP, M5).
 * Writer invoked from bm25_segment_build_orphans (one more orphan chain like
 * DOCMAP/NORMS); reader from the scorer / debug SRF. Fixed-width keys only
 * (int4/int8/uuid/text<=16B); no key->docid reverse map (spec section 9).
 * -------------------------------------------------------------------------*/
/* Turn a heap Datum for the key column into key_size bytes (right-NUL-padded).
 * int4/int8 stored native-endian (same server reads them back); uuid its 16 raw
 * bytes; text copied and TRUNCATED to key_size (16 B) if longer -- the row is still
 * indexed and keyed, never dropped or errored (a text key_field must be <=16 B to
 * have a distinct identity; two rows sharing a 16-byte prefix collide, same as any
 * non-unique key). */
extern void bm25_key_extract(uint8 key_type, uint16 key_size, Datum value,
                             unsigned char *out /* >= key_size */);
/* Map a PostgreSQL type OID to the fixed-width bm25 key encoding (int4/int8/uuid/
 * text|varchar). Returns false for any other type -- the caller decides whether
 * that is an ERROR (index build, bm25_build.c) or a NULL result (a score-time
 * arg-type mismatch, bm25_score_key). Lets bm25_score_key decode its argument by
 * the ARGUMENT's own type rather than the active scan's key type. */
extern bool bm25_key_type_from_oid(Oid typid, uint8 *out_key_type, uint16 *out_key_size);
/* Write a per-segment docid->key flat array; returns the KEYMAP chain root (or
 * InvalidBlockNumber when key_type==NONE or ndocs==0). Orphan-built like the
 * DOCMAP/NORMS chains: each page its own <=1-buffer Generic WAL record stamped
 * with gen; nothing is reachable until the caller's publish record. keys[] is the
 * dense fixed-width key[docid] array (base pointer; the writer indexes it as
 * keys + docid*key_size). */
extern BlockNumber bm25_keymap_write(Relation index, Relation heaprel, uint32 gen,
                                     uint8 key_type, uint16 key_size,
                                     const unsigned char *keys, uint32 ndocs);
/* Read key[local_docid] into out (>= key_size bytes), setting *out_size. Validates
 * seg_gen on every followed page (option (d)) like bm25_seg_docid_to_tid, and bounds
 * every link against the extent. Returns false when keymap_root is Invalid (no
 * key_field -> caller falls back to ctid). One-shot: walks from the root and captures
 * the extent on every call, so no per-row caller uses it -- see the reader form. */
extern bool bm25_seg_key(Relation index, BM25SegmentHeader *h, uint32 local_docid,
                         unsigned char *out /* >= key_size */, uint16 *out_size);
/* Reader form of bm25_seg_key (issue #225): resumes from the reader's KEYMAP cursor and
 * uses the extent captured at bm25_seg_reader_init. Same answers, same errors. */
extern bool bm25_seg_reader_key(BM25SegReader *r, uint32 local_docid,
                                unsigned char *out /* >= key_size */, uint16 *out_size);

/* Per-segment reader cache for loops that resolve keys for rows whose winning segment
 * varies row to row -- the ranked-row finalizers in bm25_scan_rank.c and bm25_wand.c. One
 * entry per distinct (header block, gen) the loop meets, each holding the segment's
 * header and a BM25SegReader over it, so a row pays neither a header read nor a KEYMAP
 * walk from the root when its segment was seen before. Entries are dynahash elements
 * allocated in `cxt`, which never move, so a reader's `h` pointer into its own entry
 * stays valid for the cache's life. A reader holds no buffer pin between calls, so
 * there is nothing to release: the cache is plain memory and goes with `cxt`, on the
 * success path and on ERROR alike. */
typedef struct BM25SegKeyCache
{
    Relation        index;
    struct HTAB    *segs;           /* (header_blkno, gen) -> BM25SegKeyCacheEntry */
    struct BM25SegKeyCacheEntry *last;  /* most recent hit: consecutive rows often share */
    /* The cache's context, where its readers keep their KEYMAP page images (#267).
     * Passed explicitly: the finalizers resolve keys with scanctx current, and an
     * image there would outlive the cache. */
    MemoryContext   cxt;
    int             nimages;        /* readers given an image so far; capped */
} BM25SegKeyCache;
extern void bm25_seg_key_cache_init(BM25SegKeyCache *c, Relation index,
                                    MemoryContext cxt);
/* One ranked row's key source for bm25_seg_key_cache_fill: the winning segment
 * (header block + gen), the row's docid in it, and the row's index in the ranking. */
typedef struct BM25KeyReq
{
    BlockNumber header_blkno;
    uint32      gen;
    uint32      local_docid;
    uint32      slot;
} BM25KeyReq;
/* Resolve every request's key into keys[slot * km_size] and set present[slot] for
 * each one resolved (issue #246). Rows of a segment whose KEYMAP spans several pages
 * are resolved in docid order (reqs is used as scratch and overwritten), so that KEYMAP is read in
 * one forward pass instead of from its root on every backward docid jump; each key
 * lands in the same slot, with the same bytes, as resolving the rows in rank order
 * would put it. */
extern void bm25_seg_key_cache_fill(BM25SegKeyCache *c, BM25KeyReq *reqs, uint32 n,
                                    unsigned char *keys, bool *present, uint16 km_size);
/* The (key_type, key_size) pair check for a KEYMAP header: key_type must be a known
 * BM25_KEY_* and key_size exactly the width that type implies. Called on the read side
 * by bm25_seg_keymeta and on the WRITE side by the pending drain, which would otherwise
 * copy an unvalidated on-page tag into the segment it is sealing. */
extern void bm25_seg_keymeta_validate(uint8 key_type, uint16 key_size);
/* Read a segment's KEYMAP {key_type, key_size} from the chain root header. Returns
 * false (leaving *out_type=NONE) when keymap_root is Invalid. Used by the merge to
 * discover the key config of the segments it re-seals so the merged segment rebuilds
 * an identical keymap (R10). */
extern bool bm25_seg_keymeta(Relation index, BM25SegmentHeader *h,
                             uint8 *out_type, uint16 *out_size);

/* -------------------------------------------------------------------------
 * bm25_seg_read.c -- tombstone (delete) path (Task 16)
 * -------------------------------------------------------------------------
 * The write half of the live-docs bitmap: clear one doc's LIVE bit and decrement
 * the matching live counters (segment header's catalog entry + metapage global
 * stats) in ONE Generic WAL record. The two locators below find the physical
 * block+offset the WAL mutation targets. */

/* Map a local doc-id to its LIVE page + byte/bit. Walks the LIVE chain from
 * livedocs_root. Returns the EXCL-lockable block and the in-contents offsets.
 * Internal to the tombstone path. */
extern BlockNumber bm25_livedocs_locate(Relation index, BlockNumber livedocs_root,
                                        uint32 gen, uint32 local_docid, Size *byte_off,
                                        int *bit);
/* Internal catalog block+slot locator by header_blkno (for WAL mutation). Distinct
 * from the read-only, test/debug-only bm25_segcat_find_entry(gen). It walks the
 * catalog with no metapage lock, so the bm25_segcat_read_locked contract applies: a
 * production caller holds the singleton, ShareLock or stronger (issue #270). The one
 * production caller is bm25_livedocs_clear, which asserts it. */
extern void  bm25_segcat_locate_entry(Relation index, BlockNumber header_blkno,
                                      BlockNumber *out_blk, int *out_idx);
/* Tombstone one document: clear its LIVE bit and decrement live counters in the
 * segment header's catalog entry and the metapage global stats, in ONE Generic WAL
 * record. Takes the segment's header_blkno (BM25SegmentHeader has no self-block
 * field), which every caller has from BM25SegCatEntry.header_blkno. */
extern void  bm25_livedocs_clear(Relation index, BlockNumber header_blkno,
                                 uint32 local_docid);

/* -------------------------------------------------------------------------
 * bm25_build.c -- ambuild: heap scan -> builder -> segment flush
 * -------------------------------------------------------------------------*/
extern IndexBuildResult *bm25_build(Relation heap, Relation index,
                                    struct IndexInfo *info);
extern bool bm25_insert(Relation index, Datum *values, bool *isnull,
                        ItemPointer ht_ctid, Relation heap,
                        IndexUniqueCheck checkUnique, bool indexUnchanged,
                        struct IndexInfo *indexInfo);


/* -------------------------------------------------------------------------
 * bm25_scan.c -- index scan state and callbacks
 * -------------------------------------------------------------------------
 * BM25ScanOpaqueData is the WHOLE per-scan state, not just a query term plus a
 * postings list: the scan's own memory context, the query term / parsed jsonb
 * tree and its field scope, the flat posting union the unordered @@@ path walks,
 * the descending-score ranked set and the iterator over it, the block-max WAND
 * latches, the &@@ distance stash plus the pristine ORDER BY RHS this scan is
 * IDENTIFIED by (#138), the per-scan key projection and the lazily-built score
 * lookup indexes, and the M4 phrase/proximity state. The "Task 11" and similar
 * milestone tags on the fields below are provenance, not future work -- the
 * state they label is present, not pending. Each field carries its own note.
 */
/* R3: BM25ScanOpaqueData.cur_ranked_idx sentinel -- the scan is not positioned
 * on any emitted row (before the first gettuple / after rescan). */
#define BM25_NO_CUR ((uint32) 0xFFFFFFFFu)

/*
 * #290: is this ranked[] entry a WHERE-set row that the ORDER BY query does not
 * match? A scan whose WHERE query differs from its ORDER BY query returns the
 * whole WHERE set, as SQL requires, and emits those rows after every ranked row at
 * distance +Infinity, the value &@@ gives every row off the index. They sit at the
 * END of ranked[] with score -Infinity, so the emit computes their distance
 * (-score) like any other row's and the scan stays positioned on them, which the
 * Merge Append distance resolver (#252) relies on.
 *
 * -Infinity is an unambiguous in-band marker: every real BM25 score is finite and
 * non-negative. The score accessors test it and answer NULL for such a row,
 * because the ORDER BY query did not score it.
 */
static inline bool
bm25_score_is_unmatched(double score)
{
    return isinf(score) && score < 0;
}

/*
 * BM25PinnedStats -- the scoring inputs of a capped WAND build, pinned for the
 * over-pull tail rebuild (#268).
 *
 * The tail rebuild (bm25_gettuple) re-reads the index under a fresh snapshot, and
 * the corpus statistics that snapshot yields -- live N, avgdl, per-field N and
 * every query term's df -- count each valid pending TID and every tombstone and
 * merge with no visibility check. Any INSERT (committed or not, the cursor's own
 * included), VACUUM or merge between the two builds therefore moved them, and the
 * tail was scored on a different scale from the rows already emitted: distances
 * stopped being monotonic and the relative order shifted, so rows were emitted
 * twice or skipped.
 *
 * So the tail is scored under THESE values instead. A document's score is then a
 * function of its own tf and doclen only, and a document present in both builds
 * scores bit-identically in both (the WAND and exhaustive scorers already agree
 * bit-for-bit on equal inputs, D8). Pinned: per-field avgdl, the live-tunable
 * per-field k1/b/boost (the per-field k1_<col>/b_<col>/boost_<col> reloptions are
 * not registered options, so ALTER INDEX ... SET takes only ShareUpdateExclusiveLock
 * for them and they CAN change under an open scan), and each query token's per-field
 * idf. live N and per-field N are not kept: they reach the score only through idf.
 *
 * A capped scan is always a plain OR text query (the WAND gate excludes phrases and
 * multi-leaf trees), so the exhaustive rebuild's work list is the same query tokens
 * in the same order and idf[qi] lines up with its qi. idf uses the WAND driver's
 * layout, [nq][BM25_MAX_FIELDS].
 */
typedef struct BM25PinnedStats
{
    uint32  field_count;                    /* snap->field_count of the pinned build */
    int     nq;                             /* query tokens the pinned build scored */
    double  avgdl_f[BM25_MAX_FIELDS];
    double  k1[BM25_MAX_FIELDS];
    double  b[BM25_MAX_FIELDS];
    double  boost[BM25_MAX_FIELDS];
    double  idf[FLEXIBLE_ARRAY_MEMBER];     /* [nq * BM25_MAX_FIELDS] */
} BM25PinnedStats;

typedef struct BM25ScanOpaqueData
{
    /* Per-scan memory context: owns qterm, postings, ranked, scores.
     * Reset on each rescan so prior allocations are freed atomically;
     * deleted in bm25_endscan.  Prevents use-after-free of ranked/scores
     * across gettuple calls when the executor's current context differs. */
    MemoryContext   scanctx;
    /* current query term (M0: single term from the first scan key) */
    char           *qterm;
    int             qtermlen;
    /* posting cursor over the matched term (unordered @@@ path) */
    BM25Posting    *postings;   /* deduped TID union across segments + pending */
    uint32          npost;      /* number of postings loaded */
    uint32          cur;        /* next index to return */
    bool            loaded;     /* postings have been loaded for this rescan */
    /* scoring iterator (Task 11): descending-score ranked result set.
     * ranked[] and scores[] are parallel arrays; only ranked[i].tid is used
     * here (tf/doclen are not needed after ranking).  Both arrays are allocated
     * in scanctx and must outlive the scan's duration.
     * bm25_score() reads these via bm25_scored_scan_head(). */
    BM25Posting    *ranked;     /* TIDs in descending-score order */
    double         *scores;     /* parallel score per entry */
    uint32          nranked;    /* valid entries in ranked/scores */
    uint32          rcur;       /* next index for the ordered iterator */
    /* R3: index into ranked[]/scores[]/ranked_keys[] of the row bm25_gettuple
     * most recently emitted for THIS scan (BM25_NO_CUR when not positioned).
     * The score accessors use it to resolve which of several concurrently-
     * active scored scans owns the row being projected (identity resolution). */
    uint32          cur_ranked_idx;
    bool            scoring;    /* true when an ORDER BY key is present */
    /* Rank-collapse fix: the distance (-score) of the tuple bm25_gettuple most
     * recently returned, so the &@@ operator's per-row target-list projection
     * (bm25_distance/_jsonb) returns the real distance instead of +inf. */
    float8          cur_orderby_dist;
    /* #138: a PRISTINE copy of this scan's ORDER BY key RHS -- the query text
     * (or jsonb) the caller wrote after `&@@`. This is the scan's identity as
     * far as the distance projection is concerned, and it exists because
     * bm25_distance cannot resolve by ROW identity the way bm25_score does: its
     * arguments are (document value, query), with no ctid, and the fmgr contract
     * gives a scalar function no handle on the slot being projected. The query,
     * though, it does receive -- so the distance resolver matches the projected
     * expression's own RHS against these bytes to find the OWNING scan, instead
     * of assuming the registry head owns the row (which is false under nesting:
     * a correlated inner scan registers on load, inside the outer's projection).
     *
     * PRISTINE is load-bearing: the field-scope and phrase micro-parsers rewrite
     * so->qterm IN PLACE (memmove), so qterm is NOT the bytes the caller wrote
     * and cannot be used for this. Copied at the scan-key capture site, before
     * any parsing, into scanctx.
     *
     * orderby_rhs_jsonb tags which operator family produced it, so the
     * (text,text) and (text,jsonb) &@@ forms can never cross-match on a
     * coincidental byte sequence. NULL when this scan is not scoring. */
    char           *orderby_rhs;
    int             orderby_rhs_len;
    bool            orderby_rhs_jsonb;
    /* M2b block-max WAND: set by bm25_wand_build_ranking when the ranked set was
     * produced by the WAND driver and filled to capacity (nranked == wand_top_k).
     * The seam's gettuple tail-fallback (Task 9) consults it to decide whether an
     * over-pull past k needs the exhaustive tail; false whenever WAND was not used
     * or fewer than k live docs matched (the set is already complete). */
    bool            wand_capped;
    /* M2b Task 10: latches once the gettuple over-pull tail fallback has rebuilt
     * the FULL exhaustive ranking for THIS scan (an executor pulling past
     * wand_top_k rows only ever needs that rebuild once -- the rebuilt set is
     * already the complete exact order). Prevents re-running the (expensive)
     * exhaustive scorer on every subsequent gettuple call once the tail is
     * exhausted too. Reset alongside wand_capped in bm25_rescan/beginscan. */
    bool            wand_tail_done;
    /* #268: the corpus statistics a capped WAND build scored under, kept so the
     * over-pull tail rebuild can score under the SAME ones. NULL unless the latest
     * build was a capped WAND build; see BM25PinnedStats. In scanctx, so the rescan
     * reset frees it and bm25_rescan NULLs it with the other scanctx pointers. */
    struct BM25PinnedStats *stats_pin;
    /* Field scope of the current query (C4): BM25_FIELD_ALL (bare RHS -> BM25F
     * over every field with its boost), or a resolved dense field_id from a
     * "field:term" RHS. Set in bm25_rescan by the first-colon micro-parse and
     * read by both the BM25F scorer (idf-gating) and the @@@ boolean path
     * (seg_tid_cb). Signed so the -1 sentinel is distinct from field_id 0. */
    int32           qfield;
    /* M5 key_field: when the scanned index has a valid keymap_root, the scorer
     * resolves each ranked doc's user key and stores it here parallel to ranked[]/
     * scores[], so bm25_score_key / the debug SRF can return by key. ranked_key_type
     * == BM25_KEY_NONE (and ranked_keys NULL) for a keyless index -> pure ctid path,
     * allocating nothing new.
     *
     * Not every ranked row of a KEYED index resolves a key. ranked_key_present marks
     * the slots that did; a false slot means the key is UNKNOWN to this scan, which
     * is a different thing from a key of zero.
     *
     * Why the distinction cannot be carried by the slot's own bytes: an unresolved
     * slot is left at its palloc0 fill, and int4/int8 keys are stored raw, so a
     * genuine `id = 0` encodes to exactly that pattern. With no out-of-band flag the
     * two are the same bytes, and a probe for key 0 -- what `WHERE id = 0` encodes
     * to, not an exotic input -- resolved to whichever unresolved row happened to be
     * in the ranking. An absent value can never be signalled by a value drawn from
     * the same domain as the real data.
     *
     * What actually leaves a slot unresolved, now that pending rows are backfilled
     * (bm25_ranked_keys_fill_from_pending): a row from a segment with no KEYMAP (an
     * index predating M5 key_field), a pending record that disappeared between the
     * scan's snapshot and the backfill, and a record whose on-page key metadata
     * disagrees with what this scan is projecting. NOT a row whose key_field is SQL
     * NULL -- that is stored as the zero sentinel with key_type SET (bm25_build.c,
     * bm25_pending.c) and deliberately projects AS key 0; the format carries no null
     * flag, so a NULL key and a genuine 0 are one identity by design.
     *
     * A key slot is meaningful ONLY when its present[] flag is true. Both consumers
     * -- the current-row memcmp fast path and the score_by_key hash build, both in
     * bm25_score.c -- must consult it, or an unresolved row re-enters the lookup
     * under a key it does not have. */
    unsigned char  *ranked_keys;    /* [nranked * ranked_key_size], or NULL */
    bool           *ranked_key_present; /* [nranked], or NULL; parallel to ranked_keys */
    uint8           ranked_key_type;/* BM25_KEY_* (NONE => ctid-only) */
    uint16          ranked_key_size;
    /* R3 Task 4: lazily-built per-scan score indexes for the decoupled/arbitrary-id
     * lookup path (bm25_score/bm25_score_key when the projected row is not the
     * scan's current row). NULL until first probe; built in scanctx so they are
     * freed by the rescan reset / endscan delete. The current-row hot path does
     * not use them. */
    struct HTAB    *score_by_tid;   /* ItemPointerData -> double */
    struct HTAB    *score_by_key;   /* ranked_key_size bytes -> double (keyed idx only) */
    /* M4 phrase / proximity (C-MATCH). Set by bm25_rescan_parse_field when the RHS
     * (after the optional field: split) is a quoted "phrase" with an optional slop
     * suffix. The phrase's ANALYZED term sequence is not stored here -- it is exactly
     * the query tokens the scorer already produces (bm25_analyze over so->qterm,
     * which the parse rewrote to the quotes-stripped phrase text), consumed IN ORDER
     * (qi == query token ordinal). When qphrase is true the scorer stashes each term's
     * per-(TID,field) position list and runs a post-accumulation recheck (D4): a doc
     * survives iff the ordered/unordered match (D5) holds within ANY single field it
     * satisfies every SLOT in (cross-field OR, D6). Filter-only (D9): survivors keep
     * their already-accumulated BM25F score. A single-slot phrase reduces to plain
     * term-present (matcher trivially true).
     *
     * The stash stays keyed by TOKEN ordinal; grouping tokens into per-source-word
     * slots (issue #184, BM25PhraseSlotMap) happens only inside the recheck, so
     * nothing about how positions are gathered depends on it.
     *
     * qphrase is read by BOTH scan paths, and must be (#132): the scored &@@ path runs
     * the recheck inline, and the boolean-only @@@ path (bm25_load_if_needed) DELEGATES
     * to that same exhaustive scorer instead of its flat OR-union, which requests no
     * positions at all and so could never apply the recheck itself. It is also the WAND
     * gate's exclusion (bm25_scan_build_ranking_once): WAND is OR-of-terms and cannot
     * carry positions. */
    bool            qphrase;        /* RHS was a quoted phrase */
    bool            qphrase_ordered;/* ~>n (PRE/n, ordered) vs ~n (W/n, unordered) */
    int             qslop;          /* max slop n (0 for an exact "..." phrase) */
    /* M4 snippet (C-SNIPPET, D11). The resolved analyzer config, cached when scoring
     * publishes this scan as the active slot (bm25_gettuple). bm25_snippet re-analyzes
     * the passed field VALUE with this exact config so its stemmed tokens match the
     * query terms (bm25_analyze over so->qterm) that the scorer already produced --
     * highlight parity is automatic. qcfg is a pure function of the index reloptions,
     * so caching it here just avoids re-resolving it at projection time; it is only
     * meaningful once qcfg_valid is set. */
    BM25AnalyzerConfig qcfg;
    bool            qcfg_valid;     /* qcfg has been resolved for this scan */
    /* TEXT-12 (issue #153): bm25_snippet's query hit set, built once per scan rather
     * than for EVERY projected row, because the query is scan-constant. qtoks is the
     * stemmed query tokens, SORTED with bm25_token_cmp for bsearch (#305 XCUT-06):
     * bm25_analyze(&qcfg, qterm, qtermlen) on the text path, or the union over every
     * non-negated MATCH/TERM/PHRASE leaf of so->qtree on the jsonb path (#308
     * TEXT-04). qwild is the jsonb path's non-negated WILDCARD patterns, case-folded
     * (bm25_fold_term) exactly as the dictionary expander folds them. Allocated in
     * scanctx (the context qterm, qtree and the ranking itself live in), so they die
     * with the MemoryContextReset in bm25_rescan, and populated LAZILY on the first
     * bm25_snippet call -- not where qcfg_valid is set -- so a scored scan that never
     * projects a snippet does no query analysis at all.
     *
     * THE WINDOW IS EXACTLY qcfg_valid's. qterm/qtermlen/qtree are rewritten during
     * rescan's RHS parse (the text, field-scoped, phrase and jsonb paths all assign
     * them), but every one of those writes happens inside
     * bm25_rescan, which clears qcfg_valid before the parse and never sets it; the
     * only site that sets it is the first scoring gettuple, after the parse is
     * done. So from the moment qcfg_valid is true until the next rescan clears it,
     * qterm is frozen -- the same fact the over-pull tail rebuild relies on when it
     * re-runs the ranking build over qterm mid-scan. qtoks/nqtoks are cleared
     * alongside qcfg_valid for that reason: they are only ever read under it. qwild/
     * nqwild are written together with nqtoks and read only when nqtoks >= 0, so they
     * need no clearing of their own. */
    BM25Token      *qtoks;          /* [nqtoks], scanctx, sorted; valid iff qcfg_valid */
    int             nqtoks;         /* -1 = not built yet; >= 0 = built, and 0
                                     * legitimately means an all-stopword query. Filled
                                     * on the FIRST bm25_snippet call rather than where
                                     * qcfg_valid is set, so a scored scan that never
                                     * projects a snippet -- the common case -- does no
                                     * query analysis at all, and the NOTICE an over-long
                                     * query term raises keeps firing once per scan
                                     * instead of once per projected row. */
    BM25Token      *qwild;          /* [nqwild] folded wildcard patterns (ptr/len only) */
    int             nqwild;         /* valid iff nqtoks >= 0; at most
                                     * BM25_QUERY_MAX_LEAVES */
    /* M6 (C-AST, Task 3): the parsed+flattened jsonb query tree, when the @@@/&@@
     * RHS this rescan read was jsonb rather than text (NULL on the text path).
     * Allocated in scanctx; reset to NULL at the top of every bm25_rescan (the
     * prior tree, if any, died with the MemoryContextReset just above). A single
     * MATCH/TERM leaf tree is text-equivalent -- its leaf is ALSO copied into
     * qterm/qtermlen/qfield below so every existing (text,text) consumer runs
     * unmodified. Anything richer -- a BOOLEAN, BOOST, PHRASE or WILDCARD root --
     * leaves qterm NULL and is scored through the multi-leaf presence-mask path
     * instead; bm25_qtree_is_multileaf (bm25_scan.h) is the dispatch, read by the
     * scored &@@ ranking build, by the WAND gate it forces off, and by the @@@
     * membership load, so the filter set and the ranked set stay identical.
     * The ONE shape rejected outright is a must_not (negated) PHRASE leaf, which
     * bm25_rescan_parse_jsonb (bm25_scan.c) ERRORs on eagerly, over the flattened
     * leaves, before either path builds anything. */
    BM25Query      *qtree;
    /* #290: the WHERE (@@@) keys this scan still has to apply, each parsed into its
     * own opaque (only its query fields -- qterm, qtermlen, qfield, the phrase
     * fields, qtree -- and scanctx are meaningful; it is never registered, positioned or
     * emitted from). The scan's own query is the ORDER BY key when there is one,
     * else the first WHERE key, and a WHERE key whose RHS is byte-identical to that
     * query is not listed: its set is the query's own. nwhere > 0 routes the
     * ranking build to the filtered builder (bm25_scan_rank.c), which computes the
     * scan's own set and every listed key's set under ONE snapshot and applies them
     * as SQL does: the result is the intersection of all WHERE sets, in ORDER BY
     * order, with the WHERE rows the ORDER BY query does not match last
     * (bm25_score_is_unmatched). In scanctx; rebuilt by every bm25_rescan. */
    struct BM25ScanOpaqueData **where;
    int             nwhere;
    /* #290: a WHERE key of a SCORED scan was skipped as byte-identical to the ORDER
     * BY key. Skipping it does not drop its condition: the ORDER BY query's own set
     * then belongs to the WHERE intersection, so the filtered build intersects it in
     * and no row can fall into the unmatched tail. False on an unscored scan, whose
     * own query is the first WHERE key and is always intersected. */
    bool            where_has_orderby;
    /* R3: intrusive link for the active-scored-scan registry (replaces the
     * single global slot). A scan is on this list from its first scoring
     * gettuple until bm25_endscan. See bm25_score.c for the registry + the
     * identity-based score resolution it enables. */
    /* The heap this scan is reading, stashed by bm25_gettuple so the ctid score
     * accessor can map a projected HOT-chain descendant back to the ROOT line
     * pointer the index actually holds (bm25_score.c). The AM exposes no
     * amgetbitmap, so every scored scan is a plain index scan and
     * IndexScanDesc.heapRelation is always non-NULL there. NULL before the first
     * gettuple, which is exactly when no row can be projected anyway. */
    Relation        heaprel;
    struct BM25ScanOpaqueData *next_active;
    /* #242: backend-unique, never-reused identity for this scan, assigned once in
     * bm25_beginscan (never 0). bm25_score/bm25_score_key bind their call site to
     * a scan by THIS value, not by pointer: an opaque is palloc'd, so a freed
     * scan's address can be handed to a later scan and a cached pointer would
     * silently alias it. Survives rescan (a rescanned inner stays the same scan
     * for its call sites); the registry is searched by it, never dereferenced
     * through a cached pointer. */
    uint64          scan_serial;
    /* #242: bm25_emit_seq stamped on this scan's most recent emitted row
     * (bm25_scored_scan_emitted, called from bm25_gettuple). The score accessors
     * compare it with the sequence a call site recorded at its previous call to
     * tell a scan that has moved since then from one whose current row is a
     * leftover. Not reset by rescan: the stamp only ever needs to be older than
     * any later call, and a rescanned scan has no current row until it emits. */
    uint64          last_emit_seq;
    /* #301: GetUserId() when the scan registered, and the index it scans. Both are
     * set by bm25_register_scored_scan, so they are meaningful exactly while the
     * scan is on the registry. Registration rather than bm25_beginscan is the
     * capture point, and in practice the two coincide: the executor calls
     * index_beginscan lazily, on the first fetch, and the first scoring gettuple
     * registers. Every registry walker ignores a scan whose owner_userid is not the
     * caller's current user id, so a SECURITY DEFINER function's still-open scan (a
     * LANGUAGE sql SRF in the caller's target list) is invisible to the caller's
     * accessors. The row-addressed accessors also check the caller's SELECT
     * privilege and row-level security on indexrel's heap (bm25_score.c). indexrel
     * is open for as long as the scan is registered: bm25_endscan deregisters
     * before the executor closes the index. */
    Oid             owner_userid;
    Relation        indexrel;
} BM25ScanOpaqueData;
typedef BM25ScanOpaqueData *BM25ScanOpaque;

/* SURFACE-09: can a scan decode a query argument of this type? jsonb, text,
 * varchar, a domain over text/varchar, or 0 (untyped, read as text). Enforced on
 * every scan key in bm25_rescan; also reported by amvalidate. */
extern bool  bm25_query_rhs_type_ok(Oid typid);
extern IndexScanDesc bm25_beginscan(Relation index, int nkeys, int norderbys);
extern void  bm25_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                         ScanKey orderbys, int norderbys);
extern bool  bm25_gettuple(IndexScanDesc scan, ScanDirection dir);
extern void  bm25_endscan(IndexScanDesc scan);

/* bm25_field_by_name -- resolve a field name to its dense field_id against a
 * BAKED field-config array (see the definition in bm25_scan.c for the exact
 * lookup contract). Extern (was static) so bm25_query.c -- a separate
 * translation unit added in M6 -- can resolve a jsonb node's "field" the same
 * way the (text,text) "field:term" micro-parser does. */
extern int32 bm25_field_by_name(const BM25FieldConfig *fields, uint32 field_count,
                                const char *name, int len, bool *found);

/* -------------------------------------------------------------------------
 * bm25_phrase.c -- the pure phrase / proximity matcher (M4 C-MATCH, D5).
 * -------------------------------------------------------------------------
 * bm25_phrase_match decides whether the phrase's N SLOTS co-occur within ONE
 * field of ONE document given each slot's ascending position list. It is a PURE
 * function of the position lists + (ordered, slop) -- no PG state -- so it is
 * unit-testable and shared by the scorer's recheck. Semantics (n = max slop =
 * extra distance beyond perfect adjacency):
 *   - EXACT (ordered, slop 0): consecutive slots at consecutive positions.
 *   - ORDERED PRE/n: query order, positions strictly increasing, chosen
 *     (p_N - p_1) - (N-1) <= n.
 *   - UNORDERED W/n: some choice of one distinct position per slot has window
 *     span (maxpos - minpos) <= (N-1) + n.
 * pos[i] is slot i's ascending, DEDUPLICATED position array of length npos[i]; an
 * empty list for ANY slot => no match. N == 1 => match iff that single slot's list
 * is non-empty (a single-slot phrase == plain term-present). A repeated slot is a
 * distinct entry with its own (identical) list; the strict-increase advance forces
 * two DISTINCT positions ("the the" needs two adjacent occurrences).
 *
 * A SLOT is one source word of the query (issue #184). Slot lists MAY OVERLAP
 * arbitrarily across slots -- that is the whole point of the alternative-group
 * design, and it is what the matcher's unordered path had to be taught (see
 * unordered_match's KEY FACT block). Analyzer revision 5 emits per-run positions, so
 * a compound split gives a slot several tokens; without one, a slot is exactly one
 * query token.
 */
extern bool  bm25_phrase_match(const uint32 *const *pos, const uint32 *npos,
                               int nterms, bool ordered, int slop);

/*
 * Query-side ALTERNATIVE GROUPS (issue #184). One SLOT is one source word of the
 * query: the set of query lexemes bm25_analyze emitted for it, which a document
 * position satisfies if it carries ANY of them (OR-per-slot). The phrase's span
 * arithmetic is then measured in slots, i.e. in source words -- core FTS's notion
 * of word distance -- rather than in lexemes.
 *
 * Members of a slot are CONTIGUOUS token ordinals, because bm25_analyze emits in
 * source order from one monotone position counter: slot k owns the ordinals
 * [slot_start[k], slot_start[k+1]). That contiguity is why the recheck's per-field
 * gather is O(ntokens) and not O(ntokens * nslots).
 *
 * WHEN EVERY RUN YIELDS ONE LEXEME THIS IS THE IDENTITY MAP: toks[i].pos == i and
 * every token gets its own slot (nslots == ntok, slot_start[k] == k), so every
 * behaviour of this file reduces to what it did before slots existed -- which was the
 * correctness argument for landing the query side before the analyzer change.
 * bm25_analyze now increments its position counter once per source RUN, not once per
 * emitted token (revision 5, ADR 0087), so a run that yields several lexemes (a
 * compound splitter) gives them one position and they share a slot.
 */
typedef struct BM25PhraseSlotMap
{
    int     nslots;
    /* [0..nslots]; slot_start[0] == 0 and slot_start[nslots] == ntok. uint8 is
     * enough because ntok is capped at BM25_PHRASE_MAX_TERMS (64) by both phrase
     * call sites before the map is built. */
    uint8   slot_start[BM25_PHRASE_MAX_TERMS + 1];
} BM25PhraseSlotMap;

extern void   bm25_phrase_slot_map(const BM25Token *toks, int ntok,
                                   BM25PhraseSlotMap *out);

/* Merge nlists ascending position lists into ONE ascending DEDUPLICATED list,
 * written to out[] (capacity must be the sum of lens[]); returns its length.
 * Used by the phrase recheck to fold a slot's member terms into the single list
 * bm25_phrase_match consumes. O(P log nlists) via a binary heap over the list
 * cursors, cancellable per emitted position. Pure, so the selftest covers it. */
extern uint32 bm25_phrase_merge_lists(const uint32 *const *lists, const uint32 *lens,
                                      int nlists, uint32 *out);

/*
 * bm25_scan_build_ranking -- public entry point for building a scan's ranking.
 * Wraps the WAND-vs-exhaustive dispatcher in a bounded subtransaction retry
 * (see bm25_scan_rank.c) so the option-(d) seg_gen reuse abort never surfaces to
 * the user.  Fills so->ranked / so->scores / so->nranked (+ WAND also
 * so->wand_capped/ranked_keys) in descending score order; ranked[]/scores[]
 * are palloc'd in so->scanctx so they outlive the retry subtransaction.
 *
 * force_exhaustive: when true, skips the WAND gate and runs the full
 * exhaustive OR-sum scorer even if WAND is otherwise enabled -- used by
 * bm25_gettuple's over-pull tail fallback to rebuild the FULL ranking
 * inside the retry wrapper (rather than calling the exhaustive scorer
 * directly, which would bypass the seg_gen retry and let that abort leak
 * to the user). Pass false for the normal top-level build.
 *
 * A forced build on a scan whose previous build pinned its statistics
 * (so->stats_pin, #268) scores under those pinned values rather than the fresh
 * snapshot's; postings and membership still come from the fresh snapshot, and so
 * does every retry of it. A non-forced build clears the pin first.
 */
extern void  bm25_scan_build_ranking(Relation index, BM25ScanOpaque so,
                                     const char *query, int querylen,
                                     bool force_exhaustive);
/* (bm25_global_stats used to be declared here. Its only caller is
 * bm25_debug_global_stats, so it moved to bm25_debug.c as a file-static when the
 * debug SRFs split out -- docs/adr/0053. A shared header declaring a symbol only
 * one translation unit ever uses is the same smell #67.13 named in bm25_wand.h.) */

/* -------------------------------------------------------------------------
 * bm25_score.c -- BM25 math: IDF and term score (Lucene "+1" variant),
 *               plus the active scored-scan registry for bm25_score(ctid).
 * -------------------------------------------------------------------------
 * bm25_idf and bm25_termscore are pure (no PG state) so they are reusable
 * from any translation unit. df is uint64 like ndocs: it is summed across segments
 * and the pending list (issue #313 SCORE-06).
 *
 * idf  = ln(1 + (N - df + 0.5) / (df + 0.5))
 * score = idf * tf*(k1+1) / (tf + k1*(1 - b + b*doclen/avgdl))
 */
extern double bm25_idf(uint64 ndocs, uint64 df);
extern double bm25_termscore(double idf, uint32 tf, uint32 doclen,
                             double avgdl, double k1, double b);

/*
 * Active scored-scan registry (defined in bm25_score.c). A scored scan
 * registers on its first scoring gettuple and deregisters in bm25_endscan;
 * the score/snippet/distance accessors resolve against it. Replaces the old
 * single bm25_active_scored_scan pointer so concurrently-active scored scans
 * (correlated subqueries / self-joins) no longer clobber one another.
 */
extern void           bm25_register_scored_scan(BM25ScanOpaque so, Relation index);
extern void           bm25_deregister_scored_scan(BM25ScanOpaque so);
/* #242: stamp so->last_emit_seq; bm25_gettuple calls it for every scored row. */
extern void           bm25_scored_scan_emitted(BM25ScanOpaque so);
/* #132: the ONE phrase-detection predicate, shared by the scan's micro-parser and
 * bm25_match, so the index path and the off-index path cannot disagree about what
 * a phrase is. out_quote may be NULL when only the yes/no answer is wanted. */
extern bool bm25_query_phrase_offset(const char *s, int len,
                                     bool allow_field_prefix, int *out_quote);
/* #298/#306: the ONE `field:` scope-prefix predicate (no whitespace or quote before the
 * colon), shared by the index-path split, bm25_query_phrase_offset and bm25_match.
 * out_colon may be NULL. */
extern bool bm25_query_field_prefix(const char *s, int len, int *out_colon);

/* #301: the head is the newest scan registered under the caller's current user id. */
extern BM25ScanOpaque bm25_scored_scan_head(void);
/* #301: considers only scans the caller owns AND may read (SELECT on the heap, a
 * partition ancestor, or every column the index reads; no row-level security in
 * force). With none, *refused names an owned scan the caller may not read, or is
 * NULL. Caches the privilege check in fcinfo->flinfo->fn_extra, which the caller
 * must therefore not use for anything else. */
extern BM25ScanOpaque bm25_sole_scored_scan(const char *fn, FunctionCallInfo fcinfo,
                                            BM25ScanOpaque *refused);
/* #138: the ONE resolver behind both &@@ distance projections
 * (bm25_distance in bm25_score.c and bm25_distance_jsonb in bm25_handler.c).
 * Shared deliberately -- two copies of this logic would drift, and the jsonb
 * copy is exactly the one an earlier fix forgot. Returns +infinity when no
 * live scan ranked this query, which is also the off-index (seqscan) answer.
 * *owned (may be NULL) reports which: true when a scan owned the row, false on
 * that +infinity fall-through -- the only case whose query no scan parsed (#245). */
extern float8 bm25_distance_for_query(bool is_jsonb, const char *rhs, int rhslen,
                                      bool *owned);

/* -------------------------------------------------------------------------
 * bm25_fsm.c -- page reclamation (Phase 3: orphan mark-and-sweep -> FSM)
 * -------------------------------------------------------------------------
 * Mark every page reachable from the metapage / pending / segcat / retired
 * chains and from each live segment's header + chains, then RecordFreeIndexPage
 * each unmarked block and IndexFreeSpaceMapVacuum. Frees crashed-seal leftovers
 * and just-truncated pending pages; live segment pages are marked, never freed.
 * The same file holds the XID-horizon retired-segment reclamation
 * (bm25_reclaim_retired, below). */
extern void  bm25_reclaim_orphans(Relation index);
/* This server lifetime's crash epoch for the orphan-sweep gate, 0 if unknown (issue
 * #300, bm25_fsm.c). Call with no buffer lock held and no WAL window open. */
extern uint64 bm25_crash_epoch(void);

/* Stamp a page BM25_PAGE_DELETED + retire_xid in its opaque, under Generic WAL
 * (the stamp-and-gate reuse marker -- nbtree safexid / bloom BLOOM_DELETED idiom).
 * Every free path stamps this BEFORE RecordFreeIndexPage so bm25_page_alloc can
 * later PROVE a returned FSM page is actually free (the FSM fork is not crash-safe,
 * so an unmarked page may be live). retire_xid = InvalidFullTransactionId marks a
 * page reusable immediately (orphan / pending recycle); a valid retire_xid gates
 * reuse on the cluster horizon (retired-segment reclaim). Caller holds buf
 * EXCLUSIVE-locked and guarantees !PageIsNew. The opaque lives in the special area
 * (outside the [pd_lower,pd_upper) hole), so a plain delta record suffices. */
extern void  bm25_page_mark_deleted(Relation index, Buffer buf,
                                    FullTransactionId retire_xid);

/* In-swap retire helper (Task 24): records a merged-away segment as ONE RANGE
 * descriptor on the persistent retired-free list, crash-atomically with the swap.
 * Called once per dropped segment from inside bm25_segcat_publish_swap
 * while its single Generic WAL record is open -- `rpage` is the already-registered
 * retired-list tail page, `*pn` the next free slot, `roots` the segment's chain
 * roots the caller pre-read BEFORE opening the window, `retire_xid` captured inside
 * the swap's metapage exclusive-lock window (that lock, not any critical section of
 * ours, is what orders it). PURE MEMORY: no buffer access, so the swap's WAL
 * window stays throw-free (m2a.md:6692). Writes no record of its own; does NO
 * per-page work (D-RETIRE/M7). BM25RetiredEntry / BM25_RETIRED_PER_PAGE live in
 * bm25_format.h. */
extern void  bm25_retire_segment(Page rpage, int *pn, BM25SegCatEntry *seg,
                                 const BM25SegmentHeader *roots,
                                 FullTransactionId retire_xid);

/* XID-horizon reclamation of retired segments into the FSM (Task 25). Refreshes the
 * local horizon, then frees (stamp-and-gate) every retired segment whose retire_xid
 * is removable cluster-wide and drops its RANGE entry; keeps not-yet-removable ones.
 * heaprel is the pre-opened heap supplying the cluster-wide visibility horizon; NULL
 * defers all reclaim (always safe). See the definition for the WAL-window ordering. */
extern void  bm25_reclaim_retired(Relation index, Relation heaprel);

/* -------------------------------------------------------------------------
 * bm25_handler.c -- reloption set (the parsed BM25Options varlena)
 * -------------------------------------------------------------------------
 * Offsets are byte offsets from the start of the BM25Options varlena to the
 * NUL-terminated string value, per the reloptions string-option convention
 * (offset 0 => option absent => caller substitutes the default). */
typedef struct BM25Options
{
    int32   vl_len_;                /* varlena header (reloptions convention) */
    int     analyzer_offset;        /* text reloption: RESERVED. Validated to "english"
                                     * only (bm25_handler.c) and never consumed by the
                                     * analyzer (bm25_analyzer_config_from_opts). */
    int     language_offset;        /* text reloption: Snowball language, default "english" */
    int     stopwords_offset;       /* text reloption: "default" | "none", default "default" */
    int     tokenizer_offset;       /* text reloption: "standard", default "standard" */
    bool    require_analyzer_match; /* bool reloption, default true */
    /* ---- M5 additions ---- */
    int     key_field_offset;       /* text: INDEX column (key or INCLUDE'd) naming the
                                     * docid->key map's source; 0 = none. Resolved against
                                     * RelationGetDescr(index), NOT the heap descriptor --
                                     * see bm25_resolve_fields and the add_string_reloption
                                     * note in bm25_handler.c, where the same "heap column
                                     * name" phrase was corrected in 2026-08. */
    /* ---- M4 additions ---- */
    bool    store_positions;        /* index-wide default (bool, default true); each field's
                                     * bit is store_positions_<attname> || this default.
                                     * Per-field knobs are dynamically named (like k1_<col>)
                                     * and read from the raw reloption array in
                                     * bm25_resolve_fields, not from this parsed bytea. */
    int     phrase_fallback_offset; /* text: 'error' (default) | 'and'. On a phrase query
                                     * touching a position-less segment / off field: ERROR
                                     * ('error') or WARN + plain AND-of-terms ('and'). Read
                                     * at scan time via bm25_phrase_fallback_is_and.
                                     * SCOPE: the (text,text) phrase path ONLY. A phrase
                                     * leaf in a jsonb query tree hits its own gate, which
                                     * hardcodes ERROR and never reads this option -- a
                                     * capability limit, not a policy choice: the
                                     * AND-of-terms downgrade rides PhraseAndEnt's single
                                     * uint64 mask, whose two uses are mutually exclusive
                                     * (phrase-term bits OR leaf bits, never both), so
                                     * degrading one leaf of a boolean tree needs a second
                                     * presence structure. See the gate's own comment in
                                     * bm25_scan_rank.c. */
    /* ---- live-params additions (spec 2026-07-16) ---- */
    double  k1;                     /* index-wide default k1 (real, default 1.2) */
    double  b;                      /* index-wide default b  (real, default 0.75) */
} BM25Options;

extern bytea *bm25_options(Datum reloptions, bool validate);

/* -------------------------------------------------------------------------
 * bm25_merge.c -- tiered merge engine (Phase 4)
 * -------------------------------------------------------------------------
 * Merge tuning. The four constants below are compile-time #defines and nothing
 * else: no GUC and no reloption exposes them, and bm25_merge.c / bm25_seg_build.c
 * read the macros directly at their use sites. That absence is specific to these
 * four -- the neighbouring seal knob IS a GUC (bm25_native.seal_threshold), and the
 * extension's reloption surface is substantial (analyzer, language, stopwords,
 * tokenizer, require_analyzer_match, key_field, store_positions, phrase_fallback,
 * k1, b, plus the dynamically-named per-field <knob>_<attname> family), so do not
 * read it as "reloptions are out of scope here". */
#define BM25_TARGET_SEGMENT_COUNT   8       /* merge down toward this many live segments */
#define BM25_MERGE_LAYER_FANOUT     4       /* >= this many segments in one size layer => merge them */
#define BM25_MERGE_TOMBSTONE_FRAC   0.15    /* a segment >=15% tombstoned is a merge candidate */
#define BM25_MERGE_MAX_INPUTS       32      /* cap inputs per merge pass (bounds one record's work) */

/* Size-layer bucket of a segment: floor(log_FANOUT(ndocs)). Same bucket == same layer. */
extern int  bm25_merge_layer_of(uint64 ndocs);

/* Fill chosen[] (caller-allocated, length nsegs) with true for each catalog entry
 * selected for the next merge; returns the number chosen (0 == nothing to do). */
extern int  bm25_merge_select(const BM25SegCatEntry *segs, uint32 nsegs, bool *chosen);

/* The merge entry points, both defined in bm25_merge.c: bm25_merge_maybe is the
 * tiered policy (opportunistic, or forced by the SQL function) and bm25_merge_sql is
 * the bm25_merge(regclass) SQL function. */
extern void  bm25_merge_maybe(Relation index, bool force);  /* tiered policy; force = manual */
extern Datum bm25_merge_sql(PG_FUNCTION_ARGS);   /* bm25_merge(regclass) */
/* bm25_seal_sql / bm25_seal(regclass) are owned by Phase 1 (Task 6); Phase 4 only
 * USES bm25_seal -- it does not re-declare it. */

#endif                          /* BM25_H */
