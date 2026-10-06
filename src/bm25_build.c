/* bm25_build.c -- ambuild and aminsert entry points.
 *
 * ambuild (bm25_build) initializes the metapage -- stamped with whatever
 * BM25_FORMAT_VERSION says, which is the single source of truth; this comment
 * deliberately does not repeat the number -- scans the heap into an
 * in-memory BM25Accum via bm25_build_callback, and seals it through the
 * two-phase bm25_segment_build_and_commit. The heap-scan callback +
 * BM25BuildState live here; the builder/commit machinery lives in
 * bm25_seg_build.c.
 *
 * ONE build is not one segment (BUILD-04). The callback seals the accumulator and
 * starts a fresh one whenever the chunk crosses bm25_maintenance_budget_bytes, so a
 * build publishes ceil(corpus / budget) segments through that same two-phase
 * commit; the merge ladder consolidates them afterwards. A build is still ONE
 * segment for every corpus that fits the budget, which is the ordinary case and
 * every case the regression suite reaches without bm25_native.debug_budget.
 *
 * bm25_insert appends each post-build tuple to the WAL-logged pending list
 * (bm25_pending_append), then opportunistically seals under a conditional page
 * lock so an inserter never blocks (the GIN-fastupdate pattern). */
#include "postgres.h"

#include "bm25.h"
#include "access/tableam.h"
#include "access/reloptions.h"   /* untransformRelOptions (raw per-field knob scan) */
#include "catalog/index.h"
#include "catalog/pg_class.h"    /* Anum_pg_class_reloptions */
#include "commands/defrem.h"     /* defGetString */
#include "miscadmin.h"           /* IsBinaryUpgrade (pg_upgrade skips the knob caps) */
#include "nodes/pg_list.h"       /* List/foreach */
#include "storage/lmgr.h"   /* ConditionalLockPage/UnlockPage seal singleton (D-SEAL/M5) */
#include "utils/builtins.h"      /* format_type_be (HDL-02 type-mismatch message) */
#include "utils/lsyscache.h"     /* getBaseType -- domains over text are indexable */
#include "utils/syscache.h"      /* SearchSysCache1(RELOID)/SysCacheGetAttr */
#include <math.h>                /* isfinite() -- reject nan/inf knob values */

/* ambuild heap-scan accumulator: one BM25Accum collects the per-field tokens of every
 * tuple the heap scan hands the callback (tupleIsAlive is not consulted, so
 * recently-dead tuples are indexed too), and the build seals it, as one segment
 * until the budget splits it (see the file header). cfg is the index's
 * resolved analyzer config, threaded into the callback so every field is tokenized
 * through the same stemmer/stopword set the metapage fingerprint advertises (per-
 * field analyzer divergence is DEFERRED, section 9 -- all fields share cfg). field_count is
 * the index's attribute count (== meta.field_count). */
/* key_* fields (M5 key_field): key_type == BM25_KEY_NONE means no key_field (the
 * callback stashes nothing and the segment keeps keymap_root Invalid -> ctid). */
/* tupcxt is the per-tuple scratch arena for tokenizer output -- see the reset note in
 * bm25_build_callback. */
typedef struct
{
    BM25Accum          *accum;
    BM25AnalyzerConfig  cfg;
    uint32              field_count;
    uint8               key_type;
    uint16              key_size;
    int                 key_attno;      /* index attribute holding the key column */
    MemoryContext       tupcxt;         /* reset after every callback invocation */
    /* ---- BUILD-04 chunking ---- */
    Relation            heap;           /* D-ALLOC/M6: the publish needs the heap and
                                         * the callback signature does not carry it */
    Size                budget;         /* read ONCE in bm25_build, so every chunk of
                                         * one build is judged against one number */
    uint64              total_docs;     /* docs published by SEALED chunks; the live
                                         * accumulator's own ndocs is added at the end.
                                         * uint64 because per-chunk ndocs is uint32 and
                                         * a build now has many chunks. */
    uint8               store_pos[BM25_MAX_FIELDS];  /* re-applied to each fresh chunk */
} BM25BuildState;

/*
 * A fresh accumulator for the next chunk, configured exactly as the first one.
 *
 * Factored out so the two creation sites -- bm25_build's initial one and the
 * callback's seal-and-restart -- cannot drift. They MUST agree: a chunk built
 * without the keymeta writes no KEYMAP and silently reverts that chunk's rows to
 * ctid identity, and a chunk built without the store_positions gate makes the
 * builder emit POS frames for a position-less field and desync the D2
 * tf-count == tf lockstep at read time. Neither shows up as a build failure.
 */
static BM25Accum *
bm25_build_accum_new(BM25BuildState *bs)
{
    BM25Accum *a = bm25_accum_begin_multi(bs->field_count);

    bm25_accum_set_keymeta(a, bs->key_type, bs->key_size);
    bm25_accum_set_store_positions(a, bs->store_pos);
    return a;
}

static void
bm25_build_callback(Relation index, ItemPointer tid, Datum *values,
                    bool *isnull, bool tupleIsAlive, void *state)
{
    BM25BuildState *bs = (BM25BuildState *) state;
    MemoryContext   old;
    uint32          docid;
    uint64          doc_tok;    /* running per-document token count (#158 ceiling) */
    uint32          f;
    bool            any = false;

    /* Skip rows whose indexed TEXT fields are all NULL, matching the insert path
     * behavior in bm25_insert. Build and insert must agree on what a document is,
     * or REINDEX produces different ndocs/avgdl and reorders search results. The
     * accumulator stores the TID with its docid, so skipping a row creates no
     * docid<->TID drift. */
    for (f = 0; f < bs->field_count; f++)
    {
        if (!isnull[f])
        {
            any = true;
            break;
        }
    }
    if (!any)
        return;                 /* all-NULL document contributes no postings */

    /* Tokenize into a context we own and reset per row (H12). CurrentMemoryContext
     * here is the CREATE INDEX statement's context, and it is never reset between
     * callbacks: heapam_index_build_range_scan resets econtext's per-tuple context but
     * does NOT switch into it before calling us, which is exactly why ginBuildCallback
     * keeps its own funcCtx. Without this, bm25_analyze's lowercased copy (one byte per
     * input byte), its token array (24 bytes per BM25Token, one per ~2 input bytes) and
     * every per-lexeme copy stay live for the whole heap scan -- an unbounded leak on top
     * of the accumulator's own footprint, roughly 13x the scanned text.
     *
     * Resetting under the accumulator is safe because it keeps nothing that points in
     * here: accum_find_or_add_term palloc+memcpy's the term into a->cxt, positions are
     * copied by value, and both add_doc_multi and add_field_tokens switch to a->cxt
     * themselves. */
    old = MemoryContextSwitchTo(bs->tupcxt);
    docid = bm25_accum_add_doc_multi(bs->accum, tid);

    /* Tokenize each indexed column into its dense field. A NULL column contributes
     * no tokens (doclen 0 for that field). Tokenizing via the index's baked analyzer
     * keeps the source side of the consistency invariant: scan/insert/recheck agree
     * on stems. */
    doc_tok = 0;
    for (f = 0; f < bs->field_count; f++)
    {
        text      *t;
        BM25Token *toks;
        int        ntok;

        if (isnull[f])
            continue;
        t = DatumGetTextPP(values[f]);
        ntok = bm25_analyze(&bs->cfg, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t), &toks);

        /* The SAME per-document token ceiling the INSERT path applies (#158).
         *
         * bm25_pending_append_multi rejects a document above PG_UINT16_MAX tokens
         * because BM25PendingTermEntry.tf is uint16 on disk. ambuild bypasses the
         * pending list entirely and feeds the accumulator, whose AccumPosting.tf is
         * uint32, so it had no ceiling at all -- and the two entry points therefore
         * disagreed about what is indexable. The same row was accepted or rejected
         * depending on index-creation order: CREATE INDEX over a table already holding
         * an oversized document succeeded, while INSERTing that byte-identical row
         * afterwards raised ERRCODE_PROGRAM_LIMIT_EXCEEDED. Durable, too -- REINDEX
         * takes this path, so an index in that state rebuilt into the same state rather
         * than surfacing the limit.
         *
         * Capping here rather than widening tf is the deliberate choice: widening is an
         * on-disk record-layout change (ADR 0009 treatment) for a case no corpus has --
         * ~65k tokens is far past any natural prose document, a 2000-word article being
         * ~2000 tokens. See ADR 0079.
         *
         * Checked incrementally, before the tokens are handed to the accumulator, so an
         * oversized document costs one field's analysis rather than the whole row's. */
        doc_tok += (uint64) ntok;
        if (doc_tok > PG_UINT16_MAX)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bm25: document has too many tokens to index"),
                     errdetail("The document has at least " UINT64_FORMAT " tokens; the "
                               "limit is %d, because a pending term entry stores its term "
                               "frequency in 16 bits and both ingest paths must agree on "
                               "what is indexable.",
                               doc_tok, PG_UINT16_MAX),
                     errhint("Split the value across rows, or index a summary column.")));

        bm25_accum_add_field_tokens(bs->accum, docid, f, toks, ntok);
    }

    /* M5 key_field: stash this doc's fixed-width key beside its TID. A NULL key is
     * left as the accumulator's zero sentinel (set at doc registration): the row is
     * still indexed and its key projects as the zero value (there is no null flag in
     * the keymap, so a NULL key and a genuine key of 0 are indistinguishable --
     * "unspecified identity", like a non-unique key). ctid fallback applies to a
     * keyless index (keymap_root Invalid), not to a sealed NULL-key row.
     *
     * NEITHER conjunct below is redundant, though the second reads that way.
     * key_type != NONE guards bm25_accum_set_doc_key's Assert(a->keys != NULL)
     * (and, in a non-cassert build, a memcpy to NULL + docid*key_size).
     * !isnull guards bm25_key_extract, which DEREFERENCES the Datum for two of
     * the four key types -- DatumGetUUIDP(value)->data and DatumGetTextPP(value)
     * -- and core leaves values[] zero for a NULL attribute, so dropping it is a
     * NULL-pointer dereference on every uuid- or text-keyed index. It is only
     * genuinely redundant for int4/int8, where bm25_key_extract's leading
     * memset(out, 0, key_size) would reproduce the sentinel byte-for-byte. Do
     * not "simplify" it away on the strength of those two types. */
    if (bs->key_type != BM25_KEY_NONE && !isnull[bs->key_attno])
    {
        unsigned char kbuf[BM25_KEY_MAX_SIZE];
        bm25_key_extract(bs->key_type, bs->key_size, values[bs->key_attno], kbuf);
        bm25_accum_set_doc_key(bs->accum, docid, kbuf, bs->key_size);
    }

    MemoryContextSwitchTo(old);
    MemoryContextReset(bs->tupcxt);

    /*
     * BUILD-04: seal-and-restart when this chunk has filled its memory budget.
     *
     * ambuild used to hold the ENTIRE heap's postings in one accumulator with no
     * spill and no bound -- the corpus was the only limit. This is the GIN
     * ginBuildCallback shape: publish what is held, start a fresh accumulator,
     * carry on. Page I/O and WAL inside a build callback are sanctioned for exactly
     * that reason; heapam_index_build_range_scan releases the heap page's content
     * lock before invoking us.
     *
     * NO DOCUMENT IS DUPLICATED OR LOST. Each callback invocation registers its
     * document in whichever accumulator is live at that instant and the check runs
     * AFTER it, so a document belongs to exactly one chunk; a sealed accumulator is
     * freed and never fed again. Everything not yet published is still held by
     * bs->accum, which bm25_build publishes unconditionally once the scan ends -- so
     * the union of the chunks is the scan, exactly once each.
     *
     * NOTHING CAN OBSERVE THE INTERMEDIATE STATES. Unlike the merge, this needs no
     * atomicity argument across chunks: for CREATE INDEX and REINDEX the relation's
     * pg_class row is uncommitted or exclusively locked, and under CONCURRENTLY
     * ambuild runs with indisready = false, so the planner ignores the index and no
     * backend runs aminsert into it -- indisready flips only after index_build
     * returns. An abort or crash mid-build drops the whole relfilenode, so the
     * per-chunk records need no cleanup story of their own.
     *
     * Placed AFTER the tupcxt switch-back and reset, deliberately: the publish and
     * the fresh accumulator must be parented on the statement context, never on the
     * per-tuple arena that is about to be reset under them.
     */
    if (bm25_accum_over_budget(bs->accum, bs->budget))
    {
        CHECK_FOR_INTERRUPTS();     /* a chunk publish is real work; ginBuildCallback
                                     * checks at the same point, and nothing is
                                     * locked here */
        bs->total_docs += (uint64) bm25_accum_ndocs(bs->accum);
        /* drained_head Invalid: ambuild has no pending list to detach (the corpus is
         * the heap scan), exactly as the final publish in bm25_build. */
        bm25_segment_build_and_commit(index, bs->heap, bs->accum,
                                      InvalidBlockNumber);
        bm25_accum_free(bs->accum);
        bs->accum = bm25_build_accum_new(bs);
    }
}

/* Read the RAW (user-typed) reloption text array for a relation from
 * pg_class.reloptions. rd_options is the *parsed* bytea (the static relopt table
 * only), so the dynamically-named per-field knobs (k1_<attname> etc.) are visible
 * only in this raw array. Returns NIL when the relation has no reloptions. */
static List *
raw_reloptions(Oid relid)
{
    HeapTuple   tup;
    Datum       datum;
    bool        isnull;
    List       *result;

    tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tup))
        elog(ERROR, "cache lookup failed for relation %u", relid);
    datum = SysCacheGetAttr(RELOID, tup, Anum_pg_class_reloptions, &isnull);
    result = isnull ? NIL : untransformRelOptions(datum);
    ReleaseSysCache(tup);
    return result;
}

/* Match one raw reloption "<param>_<attname>" against a field name; on hit, parse
 * and RANGE-VALIDATE the value into *out. Returns true if the key matched this
 * param+field (a matched-but-invalid value ERRORs; it never returns false).
 *
 * These per-field knobs bypass the static reloption validator entirely --
 * bm25_strip_field_knobs removes them before build_reloptions runs -- so this is
 * ONE of two trust boundaries where a bad value must be rejected: the other is
 * bm25_check_field_knob_value, called from bm25_options at CREATE/ALTER time so
 * the error fires at DDL time rather than the next REINDEX. reset_hint is non-NULL
 * only from that DDL-time caller (an ALTER-context hint on how to recover); the
 * build-time callers below pass NULL. strtod never sets an error for non-numeric
 * input (it returns 0.0 and stops at the first bad char), so we check the end
 * pointer + errno ourselves and enforce the BM25 ranges (k1 >= 0, b in [0,1],
 * boost >= 0). strtod also happily parses "nan"/"inf"/"-inf", which would
 * otherwise sail through: NaN compares false against every range check below,
 * and +inf passes the >= 0 checks outright -- either one poisons every score
 * computed with the knob. isfinite() folds that rejection into the same
 * parse-failure branch so it gets the existing error message and hint.
 * Mirrors bm25_validate_tokenizer/bm25_validate_stopwords, which reject bad
 * values at CREATE INDEX before any page is written. */
static bool
match_field_reloption(const char *key, const char *val,
                      const char *param, const char *attname, double *out,
                      const char *reset_hint)
{
    size_t  plen = strlen(param);
    char   *end;
    double  dv;

    /* key must be exactly "<param>_<attname>" */
    if (strncmp(key, param, plen) != 0 || key[plen] != '_')
        return false;
    if (strcmp(key + plen + 1, attname) != 0)
        return false;

    errno = 0;
    dv = strtod(val, &end);
    if (end == val || *end != '\0' || errno == ERANGE || !isfinite(dv))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: %s_%s value \"%s\" is not a valid number",
                        param, attname, val),
                 reset_hint ? errhint("%s", reset_hint) : 0));

    if (strcmp(param, "b") == 0)
    {
        if (dv < 0.0 || dv > 1.0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25: b_%s must be in [0, 1] (got %g)", attname, dv),
                     reset_hint ? errhint("%s", reset_hint) : 0));
    }
    else if (dv < 0.0)          /* k1 and boost: non-negative */
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: %s_%s must be >= 0 (got %g)", param, attname, dv),
                 reset_hint ? errhint("%s", reset_hint) : 0));

    *out = dv;
    return true;
}

/* M4: match one raw reloption "store_positions_<attname>" against a field name; on
 * hit, parse the boolean value into *out. Returns true if the key matched (a
 * matched-but-invalid value ERRORs -- the trust boundary, since these per-field knobs
 * bypass the reloption validator like k1_/b_/boost_). Accepts the reloptions boolean
 * spellings (on/off/true/false/1/0/yes/no) case-insensitively. Both callers
 * (bm25_resolve_fields and bm25_check_field_knob_value) live in this file, so this
 * stays static. */
static bool
match_field_bool_reloption(const char *key, const char *val,
                           const char *attname, bool *out)
{
    static const char *const prefix = "store_positions";
    size_t  plen = strlen(prefix);

    if (strncmp(key, prefix, plen) != 0 || key[plen] != '_')
        return false;
    if (strcmp(key + plen + 1, attname) != 0)
        return false;

    if (pg_strcasecmp(val, "true") == 0 || pg_strcasecmp(val, "on") == 0 ||
        pg_strcasecmp(val, "yes") == 0 || strcmp(val, "1") == 0)
        *out = true;
    else if (pg_strcasecmp(val, "false") == 0 || pg_strcasecmp(val, "off") == 0 ||
             pg_strcasecmp(val, "no") == 0 || strcmp(val, "0") == 0)
        *out = false;
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: store_positions_%s value \"%s\" is not a valid boolean",
                        attname, val)));
    return true;
}

/* Value-only validation for a dynamically-named per-field knob, used by
 * bm25_options at CREATE/ALTER time. amoptions cannot see the index's
 * columns, so this checks only the VALUE against the prefix's range; an
 * unknown-column suffix is reported later, at build, by
 * bm25_warn_unmatched_field_knobs. The range logic is
 * match_field_reloption's, reached by matching the key against its own
 * embedded attname (everything after the prefix): the finite / >= 0 checks and
 * b's <= 1 are in match_field_reloption; the k1_<col>/boost_<col> caps are in
 * bm25_check_field_knob_value.
 *
 * SCORE-02: the UPPER caps (k1_<col> <= BM25_K1_MAX, boost_<col> <=
 * BM25_FIELD_BOOST_MAX) live here and not in match_field_reloption, on purpose.
 * match_field_reloption also runs at every ranked-scan start
 * (bm25_resolve_live_params) and on every INSERT (bm25_resolve_fields), so a cap
 * there would make an index that already stores, say, k1_body='1e308' ERROR on
 * every scan and every INSERT after a binary upgrade -- the #292 wedge. This is
 * the same split the index-wide k1 reloption has: bounded when validate=true,
 * parsed leniently from the catalog. An already-stored out-of-range value keeps
 * its old behavior until ALTER INDEX ... RESET or SET replaces it.
 *
 * Residual: any later ALTER INDEX ... SET re-validates the MERGED option list,
 * so an index carrying such a stored value fails an unrelated SET with this
 * error until the knob is RESET; a plain dump of it fails at restore for the
 * same reason. Both name the knob, which is the repair.
 *
 * pg_upgrade is exempt: it recreates every index through pg_dump
 * --binary-upgrade's CREATE INDEX, and DefineIndex validates reloptions
 * (validate=true) with no binary-upgrade bypass, so the caps would abort the
 * whole upgrade of a cluster holding one such index -- with no way to RESET it
 * mid-upgrade. The caps are therefore skipped when IsBinaryUpgrade; every
 * pre-existing check still runs there. */
void
bm25_check_field_knob_value(const char *defname, const char *value)
{
    static const char *const numeric_params[] = {"k1", "b", "boost"};
    int     i;
    double  dv;
    bool    bv;

    for (i = 0; i < (int) lengthof(numeric_params); i++)
    {
        size_t      plen = strlen(numeric_params[i]);

        if (strncmp(defname, numeric_params[i], plen) == 0 && defname[plen] == '_')
        {
            const char *attname = defname + plen + 1;

            (void) match_field_reloption(defname, value, numeric_params[i],
                                         attname, &dv, NULL);
            if (IsBinaryUpgrade)    /* see the pg_upgrade note above */
                return;
            /* The value is echoed as written: %g would print 1000001 as 1e+06,
             * which reads as the cap itself. */
            if (strcmp(numeric_params[i], "k1") == 0 && dv > BM25_K1_MAX)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: k1_%s must be in [0, %g] (got \"%s\")",
                                attname, BM25_K1_MAX, value)));
            if (strcmp(numeric_params[i], "boost") == 0 && dv > BM25_FIELD_BOOST_MAX)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: boost_%s must be in [0, %g] (got \"%s\")",
                                attname, BM25_FIELD_BOOST_MAX, value)));
            return;
        }
    }
    if (strncmp(defname, "store_positions_", 16) == 0)
        (void) match_field_bool_reloption(defname, value, defname + 16, &bv);
}

/* The suffix of a per-field knob name, or NULL. bm25_options strips every name
 * this accepts before build_reloptions, and bm25_warn_unmatched_field_knobs warns
 * about every one whose suffix is not a key column, so the two share this one
 * definition: a name recognized by one and not the other would either be refused
 * as an unrecognized parameter or stored without ever being checked. The suffix
 * must be non-empty, so none of the fixed keys ("key_field", "b", ...) match. */
const char *
bm25_field_knob_suffix(const char *name)
{
    static const char *const params[] = {"k1", "b", "boost", "store_positions"};
    int     i;

    for (i = 0; i < (int) lengthof(params); i++)
    {
        size_t  plen = strlen(params[i]);

        if (strncmp(name, params[i], plen) == 0 &&
            name[plen] == '_' && name[plen + 1] != '\0')
            return name + plen + 1;
    }
    return NULL;
}

/* SURFACE-06: a per-field knob whose suffix matches no KEY column is stored in
 * pg_class.reloptions and then ignored by bm25_resolve_fields, which matches
 * suffixes against key-column attnames only -- so a typo (k1_titel) or an
 * INCLUDE column (k1_id) silently did nothing. amoptions cannot see the columns,
 * so ambuild is the earliest point that can tell.
 *
 * A WARNING, not an ERROR (#304): an ERROR here would make every
 * existing index that carries such a knob fail REINDEX and fail CREATE INDEX at
 * dump/restore, for a setting that has never had any effect. It is called from
 * bm25_build only -- never from bm25_resolve_fields, which also runs per row on
 * INSERT -- so it fires once per CREATE INDEX / REINDEX (once per partition for
 * a partitioned index) and an ALTER'd typo never reaches the INSERT path.
 *
 * Residual: ALTER INDEX ... SET (k1_typo = ...) is not reported until the next
 * REINDEX; amoptions, which runs at ALTER, has no Relation. */
void
bm25_warn_unmatched_field_knobs(Relation index)
{
    int         natts = IndexRelationGetNumberOfKeyAttributes(index);
    TupleDesc   idesc = RelationGetDescr(index);
    List       *rawopts = raw_reloptions(RelationGetRelid(index));
    ListCell   *lc;

    foreach(lc, rawopts)
    {
        DefElem    *de = (DefElem *) lfirst(lc);
        const char *suffix = bm25_field_knob_suffix(de->defname);
        bool        matched = false;
        int         f;

        if (suffix == NULL)
            continue;
        for (f = 0; f < natts && !matched; f++)
            matched = strcmp(NameStr(TupleDescAttr(idesc, f)->attname), suffix) == 0;
        if (!matched)
            ereport(WARNING,
                    (errcode(ERRCODE_UNDEFINED_COLUMN),
                     errmsg("bm25: reloption \"%s\" names no indexed column, "
                            "so it has no effect", de->defname),
                     errdetail("Per-field options apply only to the index's key "
                               "columns, not to INCLUDE columns."),
                     errhint("Correct the column name, or remove the option with "
                             "ALTER INDEX ... RESET (%s).", de->defname)));
    }
}

/* HDL-02, the load-bearing half: refuse an indexed KEY column this AM cannot read
 * as text, BEFORE anything dereferences its Datum.
 *
 * bm25_build_callback and bm25_insert both do DatumGetTextPP(values[f]) on every
 * indexed key column with no type test, so a non-text column reaches
 * PG_DETOAST_DATUM_PACKED on a Datum that is not a varlena: a by-value type like
 * int4 dereferences the integer itself as a pointer (42 -> SIGSEGV -> postmaster
 * crash-restart of every backend), and a by-reference type like uuid reads its
 * first bytes as a varlena header and yields a garbage VARSIZE.
 *
 * amvalidate rejecting a non-text opclass is NOT a substitute: DDL never invokes
 * amvalidate -- only the amvalidate() SQL function does, which opr_sanity drives --
 * so a malformed opclass reaches CREATE INDEX unexamined. Core's own default-opclass
 * resolution refuses the ORDINARY `CREATE INDEX ... (int_col)` before the AM sees
 * it, so the reachable case is specifically an explicitly-named opclass declared
 * FOR TYPE something-else. This check is what stops that one.
 *
 * WHY IT IS A SEPARATE FUNCTION rather than a test inside bm25_resolve_fields'
 * per-column loop, where it first lived: on the INSERT path bm25_resolve_fields is
 * called AFTER the tokenize loop has already run DatumGetTextPP (it is only needed
 * for key_field, which is resolved later), so a check living there ran strictly too
 * late and left the crash reachable for an index built by a binary predating it --
 * demonstrated, signal 11 on the first INSERT. It must be callable at the top of
 * each path, so it is.
 *
 * BASE TYPE, not the declared type: a DOMAIN over text passes core's opclass
 * resolution (binary-coercible to text) and its Datum genuinely is a text varlena,
 * so indexing one is legitimate and used to work. Comparing the raw atttypid broke
 * every such index -- not just at CREATE INDEX, but on every INSERT into an
 * already-built one after a pure binary upgrade. getBaseType resolves the domain
 * chain. It is only consulted when the fast path misses, so the ordinary text and
 * varchar columns cost two integer compares and no syscache lookup. */
void
bm25_check_indexed_column_types(Relation index)
{
    TupleDesc   idesc = RelationGetDescr(index);
    int         natts = IndexRelationGetNumberOfKeyAttributes(index);
    int         f;

    for (f = 0; f < natts; f++)
    {
        Form_pg_attribute att = TupleDescAttr(idesc, f);
        Oid               typid = att->atttypid;

        if (typid == TEXTOID || typid == VARCHAROID)
            continue;
        if (getBaseType(typid) == TEXTOID || getBaseType(typid) == VARCHAROID)
            continue;

        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bm25: column \"%s\" has type %s, which bm25_native "
                        "cannot index",
                        NameStr(att->attname), format_type_be(typid)),
                 errdetail("The access method reads every indexed column as text."),
                 errhint("Index a text or varchar column, or cast this one.")));
    }
}

/*
 * bm25_resolve_fields -- resolve the N per-field BM25FieldConfig for a CREATE INDEX.
 *
 * Each indexed column becomes a dense field (field_id = index attribute position,
 * field_name = the column attname). k1/b default to the index-wide k1/b reloptions
 * (BM25_DEFAULT_K1/B when the index carries no reloptions at all); boost defaults
 * to 1.0. Per-field overrides come from the dynamically-named reloptions
 * k1_<attname>/b_<attname>/boost_<attname> scanned out of the RAW reloption array
 * (they cannot use the static relopt_parse_elt table -- the field names are only
 * known at CREATE INDEX). tokenizer_type/stemmer_name are cloned from the single
 * index analyzer (per-field analyzer divergence is DEFERRED, spec section 9).
 *
 * key_field (a single fixed-name reloption) is resolved against the INDEX tuple
 * descriptor -- key + INCLUDE attributes, see the key_field block at the bottom of
 * this function -- into key_type/key_size/key_attno; an unknown column or an
 * unsupported key type ERRORs here (bm25_options cannot check: amoptions is handed
 * a bare reloptions Datum, no Relation of either kind).
 *
 * The `heap` parameter is consequently never dereferenced here. It is kept on
 * purpose, for signature symmetry with the sibling writer bm25_fieldcfg_write(index,
 * heaprel, ...) and with bm25_build's own call shape -- bm25_buildempty opens a heap
 * it does not strictly need for this call for the same reason (see its header comment
 * in bm25_meta.c). Dropping it would make the two call sites diverge over a detail
 * that is true today rather than guaranteed.
 *
 * A per-field knob whose suffix matches no key column is ignored here; ambuild
 * reports it with a WARNING (bm25_warn_unmatched_field_knobs), never this
 * function, which also runs per row on INSERT.
 */
void
bm25_resolve_fields(Relation index, Relation heap,
                    const BM25AnalyzerConfig *idxcfg,
                    BM25FieldConfig *out, uint32 *out_field_count,
                    uint8 *out_key_type, uint16 *out_key_size, int *out_key_attno,
                    uint8 *out_store_positions /* >= natts, or NULL */)
{
    int              natts = IndexRelationGetNumberOfKeyAttributes(index);
    TupleDesc        idesc = RelationGetDescr(index);
    List            *rawopts;
    const char      *key_field = NULL;   /* from the static key_field reloption */
    bool             sp_default = true;  /* M4: index-wide store_positions default */
    int              f;

    /* key_field is a single fixed-name reloption -> read it via the parsed struct.
     * The index-wide store_positions default (bool, default true) rides along. */
    if (index->rd_options != NULL)
    {
        BM25Options *o = (BM25Options *) index->rd_options;
        if (o->key_field_offset != 0)
            key_field = (const char *) o + o->key_field_offset;
        sp_default = o->store_positions;
    }

    /* Per-field knobs are dynamically named (k1_<attname> etc.); the static relopt
     * table can't hold them, so scan the RAW reloption text array. */
    rawopts = raw_reloptions(RelationGetRelid(index));

    for (f = 0; f < natts; f++)
    {
        Form_pg_attribute att = TupleDescAttr(idesc, f);
        const char       *attname = NameStr(att->attname);
        ListCell         *lc;

        memset(&out[f], 0, sizeof(out[f]));   /* NUL-pad fixed-width names; WAL-clean */
        out[f].field_id        = (uint32) f;   /* dense, immutable */
        strlcpy(out[f].field_name, attname, BM25_FIELD_NAME_LEN);
        out[f].k1              = index->rd_options != NULL
                                 ? ((BM25Options *) index->rd_options)->k1
                                 : BM25_DEFAULT_K1;
        out[f].b               = index->rd_options != NULL
                                 ? ((BM25Options *) index->rd_options)->b
                                 : BM25_DEFAULT_B;
        out[f].boost           = 1.0;
        out[f].tokenizer_type  = idxcfg->tokenizer_type;   /* cloned (section 9 deferred) */
        strlcpy(out[f].stemmer_name, idxcfg->language, BM25_STEMMER_NAME_LEN);

        if (out_store_positions != NULL)
            out_store_positions[f] = sp_default ? 1 : 0;   /* index default; per-field overrides below */

        foreach(lc, rawopts)
        {
            DefElem *de  = (DefElem *) lfirst(lc);
            char    *val = defGetString(de);
            double   dv;
            bool     bv;
            if (match_field_reloption(de->defname, val, "k1",    attname, &dv, NULL))
                out[f].k1 = dv;
            else if (match_field_reloption(de->defname, val, "b", attname, &dv, NULL))
                out[f].b = dv;
            else if (match_field_reloption(de->defname, val, "boost", attname, &dv, NULL))
                out[f].boost = dv;
            else if (match_field_bool_reloption(de->defname, val, attname, &bv))
            {
                if (out_store_positions != NULL)
                    out_store_positions[f] = bv ? 1 : 0;
            }
        }
    }
    *out_field_count = (uint32) natts;

    /* key_field -> key_type/key_size/key_attno resolved against the INDEX attributes
     * (key + INCLUDE), NOT the heap: the callback receives values[] indexed by index
     * attribute position, and the key column is carried as an INCLUDE column (whose
     * type needs no bm25 opclass -- see amcaninclude in bm25_handler.c). out_key_attno
     * is the 0-based dense INDEX attribute position so bm25_build_callback reads
     * values[key_attno]. Searching the full index TupleDesc (RelationGetDescr covers
     * included columns too) also lets a text key column double as an indexed field. */
    *out_key_type  = BM25_KEY_NONE;
    *out_key_size  = 0;
    *out_key_attno = -1;
    if (key_field != NULL && key_field[0] != '\0')
    {
        int       nall = idesc->natts;   /* key + INCLUDE attributes */
        int       a;
        bool      found = false;
        for (a = 0; a < nall; a++)
        {
            Form_pg_attribute att = TupleDescAttr(idesc, a);
            if (!att->attisdropped &&
                strcmp(NameStr(att->attname), key_field) == 0)
            {
                if (!bm25_key_type_from_oid(att->atttypid, out_key_type, out_key_size))
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("bm25: key_field \"%s\" type not supported "
                                    "(int4/int8/uuid/text only)", key_field)));
                *out_key_attno = a;   /* 0-based dense INDEX attno for values[] */
                found = true;
                break;
            }
        }
        if (!found)
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_COLUMN),
                     errmsg("bm25: key_field \"%s\" is not an indexed or INCLUDE'd "
                            "column of the index", key_field)));
    }
}

/* Live re-resolution of the tunable per-field parameters (k1/b/boost) from the
 * index's CURRENT reloptions, called at scan start. This is what makes
 * ALTER INDEX ... SET/RESET of these knobs take effect on the next scan with
 * no REINDEX: nothing in the postings bakes k1/b/boost (the WAND impact
 * tables store raw max_tf/min_doclen precisely so bounds could be computed
 * with scan-time parameters), so current-reloptions resolution is sound in a
 * way it can never be for the analyzer, whose tokens ARE the postings.
 * store_positions and the analyzer fields are deliberately NOT touched here --
 * they are structural and stay with the build-era fieldcfg stamp.
 * Stored garbage (possible from pre-validation ALTERs) fails loud with the
 * repair named, never silently defaulted. */
void
bm25_resolve_live_params(Relation index, BM25FieldConfig *fcfg, uint32 field_count)
{
    double      gk1 = BM25_DEFAULT_K1;
    double      gb = BM25_DEFAULT_B;
    List       *rawopts;
    ListCell   *lc;
    uint32      f;

    if (index->rd_options != NULL)
    {
        BM25Options *o = (BM25Options *) index->rd_options;

        gk1 = o->k1;
        gb = o->b;
    }
    for (f = 0; f < field_count; f++)
    {
        fcfg[f].k1 = gk1;
        fcfg[f].b = gb;
        fcfg[f].boost = 1.0;
    }

    rawopts = raw_reloptions(RelationGetRelid(index));
    foreach(lc, rawopts)
    {
        DefElem    *de = (DefElem *) lfirst(lc);
        char       *val = defGetString(de);
        double      dv;

        for (f = 0; f < field_count; f++)
        {
            const char *hint = "Use ALTER INDEX ... RESET (<option>) to clear the bad value.";

            if (match_field_reloption(de->defname, val, "k1",
                                      fcfg[f].field_name, &dv, hint))
                fcfg[f].k1 = dv;
            else if (match_field_reloption(de->defname, val, "b",
                                           fcfg[f].field_name, &dv, hint))
                fcfg[f].b = dv;
            else if (match_field_reloption(de->defname, val, "boost",
                                           fcfg[f].field_name, &dv, hint))
                fcfg[f].boost = dv;
        }
    }
}

IndexBuildResult *
bm25_build(Relation heap, Relation index, struct IndexInfo *info)
{
    IndexBuildResult  *result = palloc0(sizeof(IndexBuildResult));
    BM25BuildState     bs;
    double             reltuples;
    BM25AnalyzerConfig cfg;
    BM25MetaPageData   meta;
    BM25FieldConfig    fields[BM25_MAX_FIELDS];
    uint32             fpvec[BM25_MAX_FIELDS];
    uint8              store_pos[BM25_MAX_FIELDS];   /* M4: per-field position-storage gate */
    uint32             field_count;
    uint8              key_type;
    uint16             key_size;
    int                key_attno;
    uint32             i;
    uint32             idx_fp;

    /* M5: each indexed column becomes a dense field. Enforce the format cap here
     * (bm25_options has no column count in scope). Reject BEFORE any page is
     * written so a too-wide CREATE INDEX fails cleanly. */
    if (info->ii_NumIndexAttrs > BM25_MAX_FIELDS)
        ereport(ERROR,
                (errcode(ERRCODE_TOO_MANY_COLUMNS),
                 errmsg("bm25: index has %d columns, exceeds BM25_MAX_FIELDS %d",
                        info->ii_NumIndexAttrs, BM25_MAX_FIELDS)));

    bm25_meta_init(index, MAIN_FORKNUM);        /* block 0 first */

    /* Resolve reloptions -> analyzer config (the single source of the stemmer/
     * language/stopword/tokenizer choice), compute the fingerprint, resolve every
     * indexed column into a dense field, and write the N-field config page BEFORE
     * the heap scan so the metapage already advertises the v4 analyzer identity
     * when the segment publishes. Each call is its own preload-free Generic WAL
     * record; no window spans the heap scan. The same cfg is threaded into the
     * heap-scan callback (bs.cfg) so the segment stores tokens stemmed/stopword-
     * filtered through the analyzer the metapage fingerprint advertises. */
    bm25_analyzer_config(index, &cfg);
    bm25_meta_read(index, &meta);
    idx_fp = bm25_analyzer_fingerprint(&cfg);
    meta.analyzer_fingerprint = idx_fp;

    /* HDL-02: before the heap scan, so a non-text column is refused at DDL time
     * rather than by the first bm25_build_callback's DatumGetTextPP. */
    bm25_check_indexed_column_types(index);

    /* M5: resolve every indexed column into a dense field; per-field analyzer
     * divergence is DEFERRED (section 9), so every per_field_fingerprint entry is the
     * one index fingerprint. */
    bm25_resolve_fields(index, heap, &cfg, fields, &field_count,
                        &key_type, &key_size, &key_attno, store_pos);
    /* SURFACE-06: after the resolve, so a knob with an invalid VALUE errors
     * first instead of being warned about and then refused. */
    bm25_warn_unmatched_field_knobs(index);
    for (i = 0; i < field_count; i++)
        fpvec[i] = idx_fp;                     /* all fields share the index analyzer */
    /* M4: persist the per-field store_positions flags on the field-config page (the
     * trailing flag array, D12) so the scan-side reader can gate frame-presence.
     * #292: and the resolved key identity, which bm25_insert checks every row
     * against (bm25_validate_key_config_for_insert). CREATE INDEX CONCURRENTLY and
     * both REINDEX forms reach this same ambuild, so every build path stamps. */
    meta.field_config_blkno = bm25_fieldcfg_write(index, heap, fields, field_count,
                                                  fpvec, store_pos,
                                                  key_type, key_size, key_attno);
    meta.field_count        = field_count;     /* == natts (M5); was hard 1 in M3 */
    /* v6: derive the informational feature_flags bitmap now that field_config_blkno
     * and field_count are resolved (the field-config page is already on disk, so
     * the derivation's fieldcfg_read sees the just-written store_positions bits). */
    meta.feature_flags      = bm25_derive_feature_flags(index, &meta);
    /* BM25_FEAT_SEGCAT_TOKENS is stamped HERE and never derived (ADR 0088). It is not
     * a property of index content but a promise about who wrote the bytes: every
     * catalog entry this index will ever hold is written by this binary or a later
     * one, so its total_tokens field is meaningful rather than pre-ADR-0074 stack
     * residue. bm25_derive_feature_flags cannot conclude that from an existing index,
     * which is exactly why bm25_upgrade must not confer the bit -- an existing index
     * gains it by REINDEX, the same event analyzer revision 5 already forces on the
     * compound dictionaries this estimate matters for. */
    meta.feature_flags     |= BM25_FEAT_SEGCAT_TOKENS;
    bm25_meta_write(index, &meta);

    bs.cfg         = cfg;       /* callback tokenizes through the index's analyzer */
    bs.field_count = field_count;   /* == meta.field_count; each column a dense field */
    /* M5 key_field: thread the resolved key type/size/attno into the callback and
     * size the accumulator's per-doc key store (no-op when key_type==NONE). */
    bs.key_type    = key_type;
    bs.key_size    = key_size;
    bs.key_attno   = key_attno;
    /* M4: thread the per-field position-storage gate into the accumulator so the
     * seg builder emits POS frames only for on-fields (and skips the POS chain
     * entirely when every field is off). Copied onto the state because every CHUNK's
     * accumulator needs it, not just the first (BUILD-04). */
    memcpy(bs.store_pos, store_pos, sizeof(bs.store_pos));
    /* BUILD-04: the heap the chunk publishes allocate from (D-ALLOC/M6), the budget
     * read ONCE so every chunk of this build is judged against the same number, and
     * the running total of docs already published. */
    bs.heap        = heap;
    bs.budget      = bm25_maintenance_budget_bytes();
    bs.total_docs  = 0;
    bs.accum       = bm25_build_accum_new(&bs);   /* M5: N-field accumulator */
    /* Per-tuple arena for the callback's tokenizer scratch (H12). A child of the
     * statement context, so an error during the scan still discards it. */
    bs.tupcxt      = AllocSetContextCreate(CurrentMemoryContext,
                                           "bm25 build tokenizer scratch",
                                           ALLOCSET_DEFAULT_SIZES);
    Assert(bs.field_count == meta.field_count);   /* invariant */
    reltuples = table_index_build_scan(heap, index, info, true, true,
                                       bm25_build_callback, &bs, NULL);
    MemoryContextDelete(bs.tupcxt);
    /* ambuild has no pending list to drain (the corpus is the heap scan), so the
     * drained_head is InvalidBlockNumber; heap is the pre-opened heap (D-ALLOC).
     *
     * BUILD-04: this publishes the FINAL chunk -- which is the only chunk unless the
     * budget fired during the scan. Unconditional, so nothing the scan accumulated
     * can be dropped; an empty final chunk (the budget fired on the very last
     * document) writes no pages and publishes no catalog entry. */
    bm25_segment_build_and_commit(index, heap, bs.accum, InvalidBlockNumber);
    /* Summed across chunks: bm25_accum_ndocs describes only the accumulator still in
     * hand, and reading it alone under-reported index_tuples by every earlier
     * chunk's docs -- which is what pg_class.reltuples for the index is set from. */
    result->index_tuples = (double) (bs.total_docs +
                                     (uint64) bm25_accum_ndocs(bs.accum));
    bm25_accum_free(bs.accum);
    result->heap_tuples = reltuples;
    return result;
}

/*
 * bm25_insert -- index one heap tuple after build time.
 *
 * Tokenizes the indexed text value and appends the document to the WAL-logged
 * pending list via bm25_pending_append; the heap-scan ambuild path is unaffected
 * (it builds a segment directly). NULL values contribute no postings and are
 * skipped (ndocs/total_len count only non-NULL docs).
 *
 * After appending, an opportunistic seal fires when the pending list crosses
 * bm25_native.seal_threshold -- but only via a CONDITIONAL acquire of the seal singleton
 * (the heavyweight LockPage(BM25_METAPAGE_BLKNO, ExclusiveLock)), so an inserter
 * never blocks: if another backend already holds it (a seal is in progress) we
 * bail and let that backend / a later manual seal / VACUUM drain. The singleton
 * is held across the WHOLE drain+build+commit so two inserters cannot both drain
 * the same docs and publish duplicate segments (D-SEAL/M5). PG_TRY/PG_FINALLY
 * re-throws on error (NOT PG_CATCH), so the builder's abort-on-error precondition
 * still holds. heap is the pre-opened heap Relation aminsert received; thread it
 * down so the allocator never table_opens under a buffer lock (D-ALLOC/M6).
 *
 * MVCC contract: the scan sets xs_recheck=false (the index match is authoritative;
 * see bm25_gettuple). The executor's heap-visibility/MVCC check runs independently
 * of xs_recheck, so uncommitted or rolled-back rows are still dropped. Orphaned
 * postings from aborted inserts persist until
 * VACUUM tombstones / merge reclaim. indexUnchanged is intentionally NOT acted on:
 * a non-HOT update with an unchanged indexed column still produces a new heap TID
 * that must be indexed so it is reachable by scans; short-circuiting would make
 * new row versions unfindable (it is only a hint for bottom-up deletion).
 *
 * Returns false: bm25 stores everything itself; no index tuple is returned.
 */
bool
bm25_insert(Relation index, Datum *values, bool *isnull,
            ItemPointer ht_ctid, Relation heap,
            IndexUniqueCheck checkUnique, bool indexUnchanged,
            struct IndexInfo *indexInfo)
{
    BM25AnalyzerConfig  cfg;
    /* The fingerprint gate's metapage read; the key-config gate below reuses its
     * field_config_blkno, which is write-once at build, so this read cannot go stale
     * for it within the statement. */
    BM25MetaPageData    meta;
    /* KEY attributes only: those are the tokenized fields. An INCLUDE'd key_field
     * column (M5) is NOT a field -- it must not be fed to the tokenizer. Matches
     * bm25_build's field_count (IndexRelationGetNumberOfKeyAttributes). */
    uint32              field_count = IndexRelationGetNumberOfKeyAttributes(index);
    BM25Token         **toks_by_field;
    int                *ntok_by_field;
    uint32              f;
    bool                any = false;
    MemoryContext       scratch,
                        old;

    /* Everything below up to the pending append is per-row scratch, so run it in a
     * context we delete before returning (H12). aminsert runs with
     * CurrentMemoryContext == estate->es_query_cxt and ExecInsertIndexTuples does not
     * switch to a per-tuple context around index_insert, so bm25_analyze's lowercased
     * copy, its token array, every per-lexeme copy and the fresh untransformRelOptions
     * List that bm25_resolve_fields builds all survived until the whole statement ended
     * -- an INSERT ... SELECT of a million rows accumulated a million rows' worth. Same
     * shape as gininsert's insertCtx, and no PG_TRY for the same reason gininsert needs
     * none: the context is a child of the query context, so an error discards it with
     * its parent.
     *
     * Nothing here outlives the delete: bm25_pending_append_multi writes the tokens onto
     * the page before returning, cfg is a flat POD struct copied by value, and the
     * opportunistic seal below runs after the switch back and reads only pages. */
    scratch = AllocSetContextCreate(CurrentMemoryContext,
                                    "bm25 insert tokenizer scratch",
                                    ALLOCSET_DEFAULT_SIZES);
    old = MemoryContextSwitchTo(scratch);

    /* Resolve the index's baked analyzer config once and tokenize EACH indexed
     * column into its dense field via bm25_analyze so inserted docs stem/stopword-
     * filter EXACTLY like build-time docs and like the query path (the fingerprint
     * gate is only sound under this invariant). All fields share cfg (per-field
     * analyzer divergence is DEFERRED, section 9). A row with every field NULL contributes
     * no postings and is skipped entirely. Every catalog read inside
     * bm25_analyzer_config is syscache-backed -- the dict-OID lookup plus, since
     * ADR 0080, the pg_ts_dict/pg_ts_template/namespace reads behind stemmer_id -- so
     * per-insert cost is still dominated by the ts_lexize calls, but this IS a
     * per-row call site and not a per-scan one.
     *
     * GATED since #188 -- see the block below the resolution. */
    /* HDL-02: BEFORE the tokenize loop below, which is where the DatumGetTextPP
     * dereference happens. bm25_resolve_fields runs much later (it is only needed
     * for key_field), so a check living inside it ran strictly too late on this
     * path -- an index over a non-text column built by a binary predating that
     * check still segfaulted on its first INSERT. */
    bm25_check_indexed_column_types(index);

    bm25_analyzer_config(index, &cfg);

    /*
     * #188: enforce the invariant the paragraph above only asserted. INSERT is not one
     * of the maintenance commands PG17's RestrictSearchPath covers, so the resolution
     * just above followed the CALLER's search_path -- and ALTER INDEX ... SET
     * (language = 'french') moves the analyzer with no shadowing at all. (#148 HDL-07
     * has since given `language` a validate_string callback; it does NOT close this
     * vector, because it only rejects a language whose dictionary cannot be resolved,
     * and 'french' resolves. The edit is legal; the gate is what stops it silently
     * changing what an INSERT writes.) Either way the row was written with terms
     * this index's own analyzer does not produce, the next seal folded them into a real
     * segment, and the row stayed unfindable until REINDEX. That is durable corruption,
     * not the read path's one-session wrong answer, which is why the gate's INGEST
     * wording differs (bm25_analyzer.c).
     *
     * WHY A FINGERPRINT COMPARE AND NOT A RESTRICTED search_path. Pinning resolution
     * the way CREATE INDEX does constrains name lookup only, so it cannot see the
     * reloption-edit vector -- half the defect, and the half that needs no privileges
     * beyond owning the index. One comparison covers both.
     *
     * WHY THE METAPAGE AND NOT rd_amcache. There is no rd_amcache use anywhere in this
     * tree; introducing one here would buy a single hot-buffer read on a path that
     * already pays a full bm25_analyzer_config plus a ts_lexize per token per row, at
     * the price of new invalidation-correctness surface. Measure before caching.
     *
     * WHY HERE AND NOT AFTER THE TOKENIZE LOOP. The error must precede the work, and
     * the metapage read is the cheap half. The cost is that an all-NULL row -- which
     * contributes no postings and would be harmless -- is refused too. Deliberate: the
     * mismatch means the index is misconfigured for this session, every non-NULL row
     * behind it will fail anyway, and a gate that fires on some rows and not others is
     * harder to diagnose than one that fires on all of them.
     *
     * bm25_pending_append_multi re-reads the metapage later under an EXCLUSIVE buffer
     * lock, so this is not the read that decides where the row lands; this one takes
     * only a SHARE buffer lock (no heavyweight lock at all) and its sole output is the
     * stamped fingerprint.
     *
     * WHAT ACTUALLY SERIALIZES THE DANGEROUS CASE, stated carefully because the obvious
     * answer is wrong: it is NOT "a concurrent REINDEX holds AccessExclusiveLock".
     * REINDEX CONCURRENTLY runs under ShareUpdateExclusiveLock, which does NOT conflict
     * with this inserter's RowExclusiveLock -- so a concurrent reindex genuinely can run
     * alongside this read. It is harmless anyway: REINDEX CONCURRENTLY builds into a NEW
     * relfilenode and swaps at the end under a stronger lock, so the metapage this reads
     * is the old index's and stays coherent for the life of the statement. The case that
     * WOULD be dangerous -- the analyzer identity changing under a live inserter -- is
     * serialized by ALTER INDEX ... SET's AccessExclusiveLock only when that lock is
     * taken, and it is taken only once the backend running the ALTER has registered the
     * reloption: bm25 registers its options lazily (first bm25_options call in a
     * backend) and core derives the ALTER's lock level from the options registered in
     * THAT backend, so a cold backend takes just ShareUpdateExclusiveLock, which does
     * not conflict with this inserter's RowExclusiveLock. The real defence is the
     * fingerprint gate below, which errors by default (ADR 0081).
     * (#156 BLD-04 is about to add REINDEX CONCURRENTLY coverage; do not let that change
     * lean on a lock level this path never takes.) */
    bm25_meta_read(index, &meta);
    bm25_fingerprint_gate(index, bm25_analyzer_fingerprint(&cfg),
                          meta.analyzer_fingerprint, BM25_GATE_INGEST);

    toks_by_field = palloc(sizeof(BM25Token *) * field_count);
    ntok_by_field = palloc0(sizeof(int) * field_count);
    for (f = 0; f < field_count; f++)
    {
        text *t;
        if (isnull[f])
        {
            toks_by_field[f] = NULL;
            ntok_by_field[f] = 0;
            continue;
        }
        t = DatumGetTextPP(values[f]);
        ntok_by_field[f] = bm25_analyze(&cfg, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t),
                                        &toks_by_field[f]);
        any = true;
    }
    if (!any)
    {
        MemoryContextSwitchTo(old);
        MemoryContextDelete(scratch);
        return false;       /* all-NULL document contributes no postings */
    }

    /* M5 key_field: extract the row's key so the pending record carries it and the
     * next seal writes it into a KEYMAP (else a sealed-from-pending doc reverts to
     * ctid). Resolve the key column once per insert (reloption + tupdesc scan; cheap
     * next to the ts_lexize tokenization above). A NULL key leaves key_type set but
     * stores the zero sentinel (projected as key 0 once sealed; NOT ctid -- a NULL key
     * and a genuine 0 are indistinguishable). */
    {
        BM25FieldConfig  kfields[BM25_MAX_FIELDS];
        uint32           kfield_count;
        uint8            key_type;
        uint16           key_size;
        int              key_attno;
        unsigned char    kbuf[BM25_KEY_MAX_SIZE];
        const unsigned char *keyp = NULL;

        bm25_resolve_fields(index, heap, &cfg, kfields, &kfield_count,
                            &key_type, &key_size, &key_attno, NULL);

        /* BUILD-05: refuse the row rather than let a changed key_field poison the
         * pending chain. key_field is registered as a plain string reloption, and
         * AccessExclusiveLock there sets the LOCK LEVEL for `ALTER INDEX ... SET`
         * (and only in a backend that has already registered it; a cold backend
         * takes just ShareUpdateExclusiveLock), not a prohibition -- so the ALTER is
         * accepted, and this function
         * re-resolves key_field from LIVE reloptions on every row. Without a gate,
         * rows written after the ALTER carry a different key type into their pending
         * doc headers, and the next seal publishes a segment whose KEYMAP disagrees
         * with every existing one; a later merge then re-keys everything to
         * whichever segment it happens to read first.
         *
         * WHY HERE AND NOT AT DDL TIME: amoptions' signature is (Datum reloptions,
         * bool validate). It never receives the index, so it can neither tell a
         * CREATE from an ALTER nor compare against what the index was built with,
         * and core offers no AM hook on ALTER ... SET that does.
         *
         * WHY HERE AND NOT AT SEAL TIME: a seal-time check is a strictly worse
         * place even though it is cheaper to reach. By then the divergent rows are
         * already in the chain, so the only options are publish (corruption) or
         * error -- and erroring wedges the index, since every subsequent seal,
         * including autovacuum's, hits the same rows and fails the same way, with
         * REINDEX the only exit. Failing the INSERT keeps the chain homogeneous, so
         * reverting the reloption is enough to recover and no seal ever sees a
         * mixed chain.
         *
         * #292: that last property holds for an index whose build stamped its key
         * identity, which every build by this binary does: the row is compared with
         * the stamp, including the key COLUMN, whether or not the index has segments
         * yet. An index built before the stamp existed is still compared with its
         * first segment only, so its empty-catalog window stays open (the drain's
         * mixed-chain refusal in bm25_pending_drain is the backstop there). */
        bm25_validate_key_config_for_insert(index, meta.field_config_blkno,
                                            key_type, key_size, key_attno);

        if (key_type != BM25_KEY_NONE && !isnull[key_attno])
        {
            bm25_key_extract(key_type, key_size, values[key_attno], kbuf);
            keyp = kbuf;
        }
        /* heap is the pre-opened heap Relation aminsert received; thread it down so
         * the allocator never table_opens under a buffer lock (D-ALLOC/M6). */
        bm25_pending_append_multi(index, heap, ht_ctid, toks_by_field, ntok_by_field,
                                  field_count, key_type, key_size, keyp);
    }
    MemoryContextSwitchTo(old);
    MemoryContextDelete(scratch);

    /* Opportunistic seal: never block the inserter (conditional acquire of the
     * seal singleton, held across the entire drain+build+commit -- D-SEAL/M5).
     * Deliberately AFTER the scratch delete: bm25_pending_drain's accumulator must not
     * be parented on a context we are about to free. */
    if (bm25_pending_should_seal(index) &&
        ConditionalLockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock))
    {
        PG_TRY();
        {
            /* Re-check under the singleton: a sealer that just finished leaves
             * should_seal false (pending was advanced in its publish record). */
            if (bm25_pending_should_seal(index))
                /* PEND-16 / issue #131: this used to be a verbatim copy of
                 * bm25_seal_index's body, so the C1 anchor-reset defect existed in
                 * two places and had to be fixed in both. Both sites now call the
                 * one core; the singleton is already held here (conditionally --
                 * an inserter must never block on it). */
                bm25_seal_pending_locked(index, heap);
        }
        PG_FINALLY();
        {
            UnlockPage(index, BM25_METAPAGE_BLKNO, ExclusiveLock);
        }
        PG_END_TRY();
    }
    return false;       /* bm25 stores everything itself; no index tuple returned */
}
