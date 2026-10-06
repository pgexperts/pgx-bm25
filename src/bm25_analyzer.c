/* bm25_analyzer.c -- M3 analyzer config: resolve reloptions into a usable
 * BM25AnalyzerConfig, look up the Snowball ts_lexize dictionary OID, and derive
 * the stemmer_id + stopword_set_hash + analyzer fingerprint that gate the format.
 *
 * Role in the system: the single source of truth for "which dictionary do we
 * tokenize through, and what is its on-disk fingerprint". Tokenization
 * (bm25_tokenize.c's bm25_analyze) and the scan-time fingerprint gate both
 * depend on the config this file produces. The dict OID is resolved from the
 * SYSTEM CATALOG (pg_ts_dict) by NAME rather than stored as a raw id, because
 * ts_lexize over a fixed dict OID is a pure function of (dict OID, token) -- the
 * determinism the format contract requires.
 *
 * Resolution is by an UNQUALIFIED name: bm25_snowball_dict_oid builds
 * "<language>_stem" and hands get_ts_dict_oid a single-element name list, so WHICH
 * dictionary the name reaches is the caller's search_path's decision. What the
 * fingerprint RECORDS about that dictionary is deliberately not its OID -- see
 * bm25_stemmer_identity below. The OID is an allocation accident (the core Snowball
 * dicts are created by initdb's snowball_create.sql off the OID counter, not pinned
 * catalog constants), so recording it made the fingerprint move on PG_UPGRADE for a
 * reason unrelated to tokenization. pg_upgrade carries index files over without
 * rebuilding them, while the new cluster's initdb hands english_stem whatever number
 * its own OID counter reached -- so every index in the database failed the scan-start
 * gate whether or not anything about the analyzer had changed. The identity is now a
 * hash of the resolved dictionary's schema-qualified name plus its template's, which
 * pg_upgrade does not disturb.
 *
 * A pg_upgrade CAN change tokenization, though, and this comment once said otherwise.
 * The new major's dictionary is the same NAME over a possibly different stemmer:
 * PostgreSQL 18 imported new Snowball sources, and six English stems changed between
 * 17 and 18 (added/adding: ad -> add; egging, erring, offing likewise). The identity
 * cannot see that, by design, so since #296 the fingerprint carries a seventh
 * component that can: a hash of the dictionary's actual OUTPUT over a fixed probe word
 * list (bm25_analyzer_probe_hash). An upgrade that changes a probe word's stem or fold
 * trips the gate; one that changes nothing the probe covers still passes, which is the
 * benefit the name-derived identity was introduced for.
 *
 * Only pg_upgrade. A logical dump/restore was never affected: pg_dump emits the index
 * as CREATE INDEX, so the restore re-runs ambuild and re-stamps the fingerprint from
 * the restoring cluster's own catalog -- stored and recomputed move together.
 *
 * WHAT THIS DOES AND DOES NOT CHANGE ABOUT THE GATE. It does not improve shadow
 * detection, and the earlier version of this comment claiming otherwise was wrong:
 * pg_ts_dict.oid is unique, so two DIFFERENT dictionaries always had different OIDs
 * and the gate always caught a session that resolved "<lang>_stem" somewhere else.
 * What an OID cannot do is tell "a different dictionary" apart from "the same
 * dictionary, renumbered" -- it only ever produced FALSE POSITIVES, never false
 * negatives. Detection is essentially unchanged; what the hash adds is that it stops
 * firing when nothing changed. Two honest qualifiers on "unchanged": the DROP+CREATE
 * case below is newly invisible, and a 32-bit FNV-1a can collide where a raw OID was
 * injective. Neither is new in kind -- the COMPOSITE fingerprint has always been a
 * 32-bit FNV, so gate-level exactness was never absolute, and the gate is a
 * correctness guard against misconfiguration, not a security boundary.
 *
 * Tokenization still runs ts_lexize over the RESOLVED OID (bm25_analyze takes
 * cfg->stem_dict_oid), so a session that shadows "<lang>_stem" really would tokenize
 * through the shadowing dictionary. Nothing here pins resolution; the gate refusing
 * first is the whole defence -- ERROR under require_analyzer_match = true (the
 * default), WARN-and-proceed under false. Since #188 that defence covers BOTH
 * directions of traffic: bm25_fingerprint_gate (below) runs at scan start AND per row
 * in bm25_insert, before it tokenizes.
 *
 * The build/scan asymmetry that makes the gate load-bearing: since PG17, CREATE INDEX
 * and REINDEX run under a RESTRICTED search_path -- literally "pg_catalog, pg_temp"
 * (PG17 release notes, "use a safe search_path during maintenance operations", which
 * names both commands). A build therefore binds the pg_catalog dictionary and nothing
 * a user put on their own path, while a SCAN binds whatever the session reaches. The
 * two sides genuinely can disagree, and only the scan side is under user control --
 * which is also why `language` naming a dictionary outside pg_catalog cannot be built
 * against at all on a supported server.
 *
 * The dictionary's OPTIONS used to be the one thing NOT covered: ALTER TEXT SEARCH
 * DICTIONARY english_stem (StopWords = ...) changes what ts_lexize returns without
 * changing the dictionary's name, its template, or its OID. dictinitoption is still
 * deliberately NOT hashed -- it is a reconstructed option string, and hashing text that
 * a dump/restore may re-spell would reintroduce exactly the spurious-REINDEX failure
 * this identity exists to remove -- but the probe (#296) observes the option's EFFECT
 * instead, wherever it touches a probe word, and a TSDICTOID syscache callback makes an
 * ALTER visible to the very next statement of the same session. The same holds for
 * DROP + CREATE at the same qualified name and template with different OPTIONS. What
 * stays invisible is a behaviour change confined to words outside the probe list.
 *
 * The INGEST path used to be on that not-covered list too, and it was the worse entry:
 * bm25_insert resolved the analyzer and tokenized under the CALLER's search_path --
 * INSERT is not a maintenance command, so PG17's restriction does not apply to it --
 * with nothing comparing the result against the metapage, so the row was committed
 * with wrong-dictionary terms and stayed invisible to a correct query until REINDEX.
 * Closed by #188 / docs/adr/0081: bm25_insert now reads the metapage and calls
 * bm25_fingerprint_gate with BM25_GATE_INGEST before it tokenizes. Note the fix is a
 * fingerprint comparison rather than a restricted search_path for ingest, because a
 * post-build `ALTER INDEX ... SET (language = ...)` reaches the same corruption with no
 * shadowing anywhere, and name-resolution pinning cannot see it.
 *
 * The per-index field-config page (BM25_PAGE_FIELDCFG) reader/writer
 * (bm25_fieldcfg_write / bm25_fieldcfg_read) live here. write is called by
 * bm25_build with the N resolved fields (which stores its root into
 * meta->field_config_blkno); read backs the bm25_debug_fieldcfg SRF. The page is
 * a singleton at seg_gen = 0 (per-index, not segment-scoped), so seg_gen is never
 * validated on read. */

#include "postgres.h"

#include "bm25.h"
#include "access/xloginsert.h" /* log_newpage_buffer -- bm25_fieldcfg_write_init's INIT_FORKNUM write */
#include "catalog/namespace.h"
#include "catalog/pg_collation.h" /* DEFAULT_COLLATION_OID -- the fold's collation */
#include "catalog/pg_ts_dict.h"     /* Form_pg_ts_dict -- the stemmer identity's operands */
#include "catalog/pg_ts_template.h" /* Form_pg_ts_template -- ditto */
#include "funcapi.h"
#include "mb/pg_wchar.h"        /* pg_encoding_mblen, report_invalid_encoding,
                                 * pg_database_encoding_max_length */
#include "nodes/value.h"
#include "tsearch/ts_cache.h"   /* lookup_ts_dictionary_cache -- the probe's lexize handle */
#include "tsearch/ts_public.h"  /* TSLexeme */
#include "storage/lock.h"       /* LocalTransactionId -- the ingest WARNING's memo key */
#include "storage/proc.h"       /* MyProc->vxid.lxid -- ditto */
#include "utils/builtins.h"
#include "utils/formatting.h"   /* str_tolower -- the encoding-aware case fold */
#include "utils/guc.h"          /* NewGUCNestLevel/RestrictSearchPath/AtEOXact_GUC --
                                 * bm25_validate_language resolves the dictionary the
                                 * way CREATE INDEX will, not the way the caller would */
#include "utils/hsearch.h"      /* the per-backend probe cache */
#include "utils/inval.h"        /* CacheRegisterSyscacheCallback -- its TSDICTOID hook */
#include "utils/lsyscache.h"    /* get_namespace_name */
#include "utils/memutils.h"     /* CacheMemoryContext, AllocSetContextCreate */
#include "utils/syscache.h"     /* SearchSysCache1 -- TSDICTOID / TSTEMPLATEOID */
#include "utils/tuplestore.h"   /* tuplestore_begin_heap/putvalues -- no longer pulled
                                 * in transitively via funcapi.h as of PG19 */
#include <ctype.h>

/* The M3 field-config page holds exactly [header][one BM25FieldConfig]; assert the
 * full BM25_MAX_FIELDS form fits one page so M3 never needs a page chain (the reader
 * still follows nextblk to stay page-count-agnostic for M5). */
/* Header + all configs + the M4 trailing uint8 store_positions[BM25_MAX_FIELDS] flag
 * array + the #292 key-identity stamp must fit one page, so neither tail is ever split
 * across a continuation page (the readers recover both from the first page only). */
StaticAssertDecl(sizeof(BM25FieldConfigHeader) + BM25_MAX_FIELDS * sizeof(BM25FieldConfig)
                 + BM25_MAX_FIELDS + sizeof(BM25KeyStamp)
                 < BLCKSZ - SizeOfPageHeaderData - MAXALIGN(sizeof(BM25PageOpaque)),
                 "field-config records + store_positions flags + key stamp must fit one page");
/* The stamp's members must pack with no compiler padding: the writer memsets the
 * staged struct, but an unexpected hole would still be a layout nobody reviewed. */
StaticAssertDecl(sizeof(BM25KeyStamp) == 12, "BM25KeyStamp is a 12-byte packed record");

/* FNV-1a 32-bit. Offset basis / prime are the on-disk contract (fingerprint and
 * stopword_set_hash are compared across machines for replica equality) -- do not
 * change. */
#define BM25_FNV1A_OFFSET 0x811c9dc5u
#define BM25_FNV1A_PRIME  0x01000193u

/* Analyzer OUTPUT revision -- the fifth fingerprint component (there are seven; the
 * sixth, the database encoding, was appended by #151, and the seventh, the dictionary
 * probe, by #296).
 *
 * The gate's job is to refuse a scan whose analyzer would tokenize differently from
 * the one that built the index. The other components identify the analyzer's
 * CONFIGURATION (dict, language, stopword policy, tokenizer) and its ENVIRONMENT
 * (database encoding), which is not sufficient on its own: the analyzer's own CODE
 * can change what it emits for a configuration that did not change at all. ADR 0046
 * is exactly that case -- byte-wise splitting and folding became character-wise, so
 * an index built before it stores 'rger' where one built after it stores the whole
 * word, with every reloption identical. Without this component such an index keeps
 * its old fingerprint, passes the gate, and silently returns nothing for its
 * non-ASCII terms: the precise failure the gate exists to prevent, and the reason a
 * REINDEX note in a release document is not a substitute.
 *
 * Bump on ANY change to bm25_analyze's output for fixed reloptions, AND on any change
 * to how an existing component's VALUE is encoded -- the gate compares stored numbers,
 * so a re-encoding is indistinguishable on disk from a semantic change and needs the
 * same REINDEX signal. Do NOT bump for a refactor that provably preserves both -- a
 * needless bump costs every user a REINDEX.
 *
 * 1 = the original byte-wise analyzer; 2 = ADR 0046; 3 = #151's TEXT-06 half: single-
 * byte run splitting no longer consults LC_CTYPE, so a high byte in LATIN1 is a
 * separator rather than a locale-dependent letter.
 *
 * REVISION 3 DID NOT CHANGE POSITIONS, whatever this comment once said. #151's other
 * half (TEXT-05 -- advancing position per RUN rather than per LEXEME, matching core
 * FTS's ts_parse.c) was written, measured to break phrase search into silent zero-row
 * results, and REVERTED before revision 3 shipped; docs/adr/0077 records that. The
 * stale claim mattered because the work that finally landed per-run positions would
 * have leant on it to conclude the bump was already spent. It was not -- that change
 * is revision 5 below, and it paid its own.
 *
 * 4 = #62, the re-encoding case:
 * stemmer_id became a hash of the dictionary's schema-qualified name and template
 * instead of its raw OID (bm25_stemmer_identity). Tokenization is byte-identical
 * across that change -- an existing index's segments are still CORRECT, and REINDEX
 * is the conservative cure rather than a repair -- but every stored fingerprint moves,
 * so the gate must speak up rather than silently compare an old encoding against a
 * new one. See ADR 0080.
 *
 * 5 = #184, the change revision 3 did NOT make, landed with the query-side counterpart
 * whose absence forced the earlier revert. bm25_analyze now advances position once per
 * source RUN instead of once per emitted LEXEME, and deduplicates a run's repeated
 * lexemes. Three outputs move together for a dictionary that emits several lexemes per
 * word (a compound splitter or thesaurus; Snowball English emits one, so an
 * English-only index tokenizes byte-identically across this bump): the position
 * sequence, the token multiset, and doclen -- which is max(position)+1 since #194, so
 * a six-lexeme compound stops contributing six to the BM25 length denominator and
 * contributes one.
 *
 * This is a genuine TOKENIZATION change, not a re-encoding like revision 4, and the
 * distinction decides what an operator may safely do. `require_analyzer_match = false`
 * is a sound deferral for revision 4 precisely because tokenization was byte-identical
 * there (ADR 0080); across THIS bump it means querying an index whose stored terms and
 * positions disagree with the analyzer answering the query, which returns wrong
 * results rather than stale ones. REINDEX is the only correct response. See ADR 0087.
 *
 * 6 = #295 + #296, one bump for both so users pay one REINDEX:
 *  - #295, a tokenization change in SINGLE-BYTE databases only: a high byte that the
 *    encoding maps to a letter or digit is now a word byte (bm25_is_word_byte's
 *    generated table), and in SQL_ASCII every high byte is. Revision 3 made all of them
 *    separators, which indexed nothing for Cyrillic in WIN1251/KOI8-R and shredded
 *    accented LATIN1 words into cross-matching fragments. UTF-8 and other multibyte
 *    databases tokenize byte-identically across this bump.
 *  - #296, a new fingerprint component (7, the dictionary probe), which moves every
 *    stored fingerprint whatever the encoding.
 * For a single-byte database this is a genuine tokenization change, so as with 5,
 * require_analyzer_match = false there means querying stale terms; REINDEX. For
 * every other database the stored segments are still correct and the REINDEX only
 * restamps -- unless the index was carried across a pg_upgrade whose stemmer changed
 * (PostgreSQL 17 -> 18 English), which is exactly what the probe now detects and what
 * the REINDEX then repairs. */
#define BM25_ANALYZER_REVISION 6u

static uint32
bm25_fnv1a(const unsigned char *bytes, Size n, uint32 h)
{
    Size    i;

    for (i = 0; i < n; i++)
    {
        h ^= (uint32) bytes[i];
        h *= BM25_FNV1A_PRIME;
    }
    return h;
}

/* Resolve the Snowball stemming dictionary OID for a language. The core snowball
 * dicts are named "<language>_stem" in pg_ts_dict (search_path resolution via
 * get_ts_dict_oid). "english" -> english_stem. We resolve the DICT (not the
 * configuration) because ts_lexize takes a dict OID and the snowball dict applies
 * BOTH the language stoplist and the stemmer in one call. Resolving by NAME (not a
 * volatile id) is what survives a restart, but the name list below is UNQUALIFIED,
 * so WHICH dictionary the name reaches is the caller's search_path's decision -- see
 * the file header for what a shadowing "<lang>_stem" does to the fingerprint and to
 * the scan-time gate. stopwords = none is handled at tokenize time by keeping a
 * dropped token's surface form; the dict choice is the same and the fingerprint
 * distinguishes the two via stopword_set_hash. */
static Oid
bm25_snowball_dict_oid(const char *language)
{
    char    dictname[BM25_STEMMER_NAME_LEN + 6]; /* "<lang>_stem" + NUL */
    List   *names;
    Oid     oid;

    snprintf(dictname, sizeof(dictname), "%s_stem", language);
    names = list_make1(makeString(dictname));
    /* missing_ok = false: an unknown language is a user error at CREATE INDEX. */
    oid = get_ts_dict_oid(names, false);
    list_free_deep(names);
    return oid;
}

/* The stemmer IDENTITY recorded in the analyzer fingerprint: FNV-1a over the resolved
 * dictionary's SCHEMA-QUALIFIED NAME and its template's, each part NUL-terminated so
 * ("ab","c") and ("a","bc") cannot collide. Every operand is a stable catalog string,
 * never a number the OID counter chose.
 *
 * Why not the OID, which is what this used to be. Exactly one failure, stated
 * precisely because the obvious broader version of it is false:
 *
 *  - FALSE MISMATCH after PG_UPGRADE. english_stem is created by initdb, not pinned,
 *    so its OID differs between clusters and across major versions. pg_upgrade carries
 *    index files over without rebuilding them, so a stored fingerprint computed under
 *    the old cluster's OID met a recomputed one under the new cluster's, and the gate
 *    refused indexes whose contents were perfectly valid.
 *  - NOT a detection failure. pg_ts_dict.oid is unique, so two different dictionaries
 *    never shared one: the gate always caught a shadowing "<lang>_stem". An OID's
 *    weakness is one-directional -- it cannot tell "different dictionary" from "same
 *    dictionary, renumbered", so it over-fires and never under-fires. The qualified
 *    name distinguishes the two; that is the whole delta.
 *
 * The template is hashed alongside the name because the name alone does not fix
 * behavior: DROP + CREATE at the same qualified name with TEMPLATE = simple instead of
 * snowball is a different analyzer, and identity must move when it does.
 *
 * NOT hashed: dictinitoption. See the file header -- an ALTER that changes the
 * stoplist is invisible to THIS component, exactly as it was invisible to the OID; the
 * behavioural probe (fingerprint component 7, #296) is what sees it.
 *
 * The dictionary OID itself is still resolved and still carried in
 * cfg->stem_dict_oid: ts_lexize needs a real dictionary. This is the identity that
 * gets STORED, not the handle that gets USED.
 */
static uint32
bm25_stemmer_identity(Oid dictoid)
{
    HeapTuple            dicttup;
    HeapTuple            tmpltup;
    Form_pg_ts_dict      dict;
    Form_pg_ts_template  tmpl;
    char                *dictnsp;
    char                *tmplnsp;
    uint32               h = BM25_FNV1A_OFFSET;

    dicttup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictoid));
    if (!HeapTupleIsValid(dicttup))
        elog(ERROR, "cache lookup failed for text search dictionary %u", dictoid);
    dict = (Form_pg_ts_dict) GETSTRUCT(dicttup);

    tmpltup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(dict->dicttemplate));
    if (!HeapTupleIsValid(tmpltup))
        elog(ERROR, "cache lookup failed for text search template %u",
             dict->dicttemplate);
    tmpl = (Form_pg_ts_template) GETSTRUCT(tmpltup);

    /* get_namespace_name palloc's, and returns NULL for a namespace that is gone.
     * Holding these tuples does not lock the namespaces, so the NULL is not
     * impossible -- and the alternative to checking is strlen(NULL) below. Loud, not
     * defensive-quiet: a fingerprint computed from a placeholder would be a wrong
     * number written to disk. */
    dictnsp = get_namespace_name(dict->dictnamespace);
    tmplnsp = get_namespace_name(tmpl->tmplnamespace);
    if (dictnsp == NULL)
        elog(ERROR, "cache lookup failed for namespace %u", dict->dictnamespace);
    if (tmplnsp == NULL)
        elog(ERROR, "cache lookup failed for namespace %u", tmpl->tmplnamespace);

    /* Order is part of the on-disk contract: dict schema, dict name, template schema,
     * template name, each including its NUL. */
    h = bm25_fnv1a((const unsigned char *) dictnsp, strlen(dictnsp) + 1, h);
    h = bm25_fnv1a((const unsigned char *) NameStr(dict->dictname),
                   strlen(NameStr(dict->dictname)) + 1, h);
    h = bm25_fnv1a((const unsigned char *) tmplnsp, strlen(tmplnsp) + 1, h);
    h = bm25_fnv1a((const unsigned char *) NameStr(tmpl->tmplname),
                   strlen(NameStr(tmpl->tmplname)) + 1, h);

    pfree(dictnsp);
    pfree(tmplnsp);
    ReleaseSysCache(tmpltup);
    ReleaseSysCache(dicttup);
    return h;
}

/* Lowercase a language name in place (Snowball language tags are ASCII). The
 * fingerprint hashes the LOWERCASED language bytes, so we normalize once here.
 *
 * Deliberately a TRUE ASCII fold, not tolower(): tolower() is LC_CTYPE-sensitive,
 * and this result feeds both the dictionary-name lookup (bm25_snowball_dict_oid)
 * and the on-disk fingerprint. Under a Turkish-class LC_CTYPE, tolower('I') is the
 * DOTLESS i (U+0131) rather than 'i', so `WITH (language='ENGLISH')` would resolve
 * the name "englIsh_stem" to one whose fifth character is that dotless i -- not
 * "english_stem" -- and fail a lookup that succeeds everywhere
 * else -- and, worse, hash to a fingerprint no other server reproduces. An 'A'..'Z'
 * fold is the only mapping that is identical on every server, which is what a value
 * baked into the index format has to be. The remaining caveat is not ours to fix:
 * a language reloption containing non-ASCII bytes is passed through unfolded, so it
 * must be spelled identically at CREATE INDEX and at scan time. Snowball language
 * tags are ASCII, so this costs nothing in practice. See ADR 0046. */
static void
bm25_lower_ascii(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s += 'a' - 'A';
}

/* DDL-time validator for the `language` reloption (#148 HDL-07), registered as the
 * validate_string callback in bm25_handler.c. It lives here rather than beside the
 * other validators because it has to resolve the Snowball dictionary the same way
 * bm25_analyzer_default_config does, and both the ASCII fold and the "<lang>_stem"
 * name construction are this file's business.
 *
 * WHAT THIS DOES AND DOES NOT BUY. It converts a BROKEN INDEX into a REJECTED DDL
 * STATEMENT: `ALTER INDEX ... SET (language = 'klingon')` used to be accepted silently
 * and only explode at the next scan or insert, when the resolver ran for real, leaving
 * an index that could not be read and a user with no idea which statement did it.
 *
 * WHICH search_path IT RESOLVES UNDER, because this is subtle and an earlier draft of
 * this comment got it backwards. The two DDL entry points do NOT agree on their own:
 *
 *   CREATE INDEX -- DefineIndex calls RestrictSearchPath() (indexcmds.c) BEFORE
 *   index_reloptions(..., validate = true), so this callback already runs under
 *   `pg_catalog, pg_temp`, and get_ts_dict_oid skips the temp namespace for a
 *   non-relation object. Effectively pg_catalog only. That is also why a `language`
 *   naming a dictionary in a user schema cannot be BUILT against at all on PG17+ --
 *   see bm25_snowball_dict_oid's header and sql/65_multilexeme_tokens.
 *
 *   ALTER INDEX -- ATExecSetRelOptions does not restrict anything, so this callback
 *   would otherwise see the CALLER's search_path and accept a dictionary the
 *   subsequent REINDEX could never resolve. That is the same "accepted now, broken
 *   later" shape the validator exists to remove, just one step further out.
 *
 * So we restrict here too, under our own GUC nest level, and both paths then answer
 * the question that actually matters: will the BUILD find this dictionary? On the
 * CREATE INDEX path the call is a no-op; on ALTER INDEX it is the whole point.
 *
 * What is STILL not pinned, and cannot be from here: the INSERT and scan paths run
 * UNRESTRICTED, so the same reloptions can reach a different dictionary at run time
 * than at build time. That divergence is not a validation problem and is not fixed by
 * one -- it is what the analyzer-fingerprint gate covers (ADR 0081).
 *
 * RUNS ON ITS OWN DEFAULT AT REGISTRATION. init_string_reloption calls
 * `validator(default_val)` before allocating, so this executes once per backend at the
 * first bm25_options call -- including a validate = false relcache load. See the note
 * in bm25_handler.c's registration block for the measured consequence (with
 * pg_catalog.english_stem dropped, a plain seqscan of a table carrying a bm25 index now
 * errors) and for why the NULL-default remedy was not taken.
 *
 * missing_ok = true plus our own ereport, rather than letting get_ts_dict_oid raise:
 * core's message names "klingon_stem", a dictionary the user never typed. The length
 * check is not decoration either -- bm25_analyzer_config_from_opts strlcpy's the value
 * into a BM25_STEMMER_NAME_LEN buffer, so an over-long language would otherwise be
 * silently TRUNCATED to something that might resolve to a different dictionary.
 */
void
bm25_validate_language(const char *value)
{
    char    folded[BM25_STEMMER_NAME_LEN];
    char    dictname[BM25_STEMMER_NAME_LEN + 6];    /* "<lang>_stem" + NUL */
    List   *names;
    Oid     oid;
    int     save_nestlevel;

    if (value == NULL)
        return;

    if (strlen(value) >= BM25_STEMMER_NAME_LEN)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: language name \"%s\" is too long", value),
                 errdetail("The maximum length is %d bytes.",
                           BM25_STEMMER_NAME_LEN - 1)));

    /* Fold exactly as the resolver will, so validation and use agree on the name. */
    strlcpy(folded, value, sizeof(folded));
    bm25_lower_ascii(folded);

    snprintf(dictname, sizeof(dictname), "%s_stem", folded);
    names = list_make1(makeString(dictname));

    /* The nest level is what makes this safe to do inside a reloptions callback: the
     * restriction lasts exactly as long as the lookup. Same three-call pattern
     * DefineIndex uses. Restored BEFORE the ereport below so the failing statement's
     * error is raised under the caller's own settings. */
    save_nestlevel = NewGUCNestLevel();
    RestrictSearchPath();
    oid = get_ts_dict_oid(names, true);
    AtEOXact_GUC(false, save_nestlevel);

    list_free_deep(names);

    if (!OidIsValid(oid))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: invalid language \"%s\"", value),
                 errdetail("No text search dictionary named \"%s\" is reachable from a CREATE INDEX, "
                           "which resolves under a restricted search_path (pg_catalog).",
                           dictname),
                 errhint("Snowball languages are named <language>_stem, e.g. english_stem. "
                         "A dictionary in a user schema cannot be used: "
                         "PG17+ builds indexes under a restricted search_path.")));
}

/* ---------------------------------------------------------------------------
 * Character classification and case folding for the analyzer (ADR 0046).
 * ---------------------------------------------------------------------------
 * These three are the ONLY places the extension decides "is this a word
 * character" and "how do I lowercase this". Every tokenizing or term-matching
 * path routes through them, because the index and the query must agree
 * byte-for-byte: a term folded one way at build time and another way at scan
 * time is a silent no-match. The callers are bm25_analyze (bm25_tokenize.c),
 * bm25_dict_expand_wildcard (bm25_seg_dict.c) and bm25_snippet.c's edge snapper.
 */

/*
 * bm25_is_word_byte -- byte-level word-character test.
 *
 * In a MULTIBYTE server encoding every non-ASCII byte belongs to a multibyte
 * character and is therefore part of a word. Core full-text search's default parser
 * states the same rule but scopes it to one case -- "any non-ascii symbol with
 * multibyte encoding with C-locale is an alpha character" (wparser_def.c) -- and
 * defers to iswalnum() otherwise. Applying it unconditionally is the deliberate
 * difference: it makes run-splitting independent of LC_CTYPE, so the same document
 * tokenizes identically on every server. That matters more here than it does for
 * tsvector, because a run boundary decides document length and therefore every score
 * derived from it, and the format contract claims exactly that determinism. The cost
 * is that non-ASCII punctuation joins its neighbours instead of separating them.
 *
 * In a SINGLE-BYTE encoding (LATIN1, WIN1251, KOI8-R and friends) a high byte IS a
 * whole character, so whether it is a word byte depends on WHICH character the
 * encoding says it is. Since #295 (analyzer revision 6) that is decided by a fixed
 * per-encoding table, bm25_sb_wordclass: generated offline from PostgreSQL's own
 * conversion maps and its pg_u_isalnum classification at a pinned Unicode version
 * (ci/gen_sb_wordclass.py). It is what makes Cyrillic a letter in WIN1251 and
 * 'A-umlaut' a letter in LATIN1 while leaving the no-break space, a currency sign or
 * a byte the encoding does not map at all a separator. The answer is a pure function
 * of the database encoding -- already fingerprint component 6 -- and of this
 * extension's source; it consults neither LC_CTYPE nor the server's Unicode tables,
 * which move between majors, nor pg_conversion, which a CREATE DEFAULT CONVERSION can
 * redirect. Case folding is NOT widened to match: under a C collation a non-ASCII
 * letter keeps its case, exactly as it does in to_tsvector (core parity).
 *
 * SQL_ASCII declares no character set, so it has no table. Its high bytes are word
 * bytes, as in a multibyte encoding: what a SQL_ASCII database holds above 0x7F is in
 * practice UTF-8 (or another ASCII-superset encoding) that the server never checked,
 * and joining those bytes into runs indexes such text whole instead of shredding
 * every non-ASCII character into a separator.
 *
 * Exposed as a byte predicate because bm25_snippet.c walks an excerpt edge
 * BACKWARD from an arbitrary offset, where there is no character to measure yet.
 * That walk is correct for the table branch too: in a single-byte encoding a byte IS
 * a character, so there is no interior to mistake for a boundary.
 */
bool
bm25_is_word_byte(unsigned char c)
{
    if (IS_HIGHBIT_SET(c))
    {
        int             enc = GetDatabaseEncoding();
        const uint8    *tbl;

        if (pg_database_encoding_max_length() > 1 || enc == PG_SQL_ASCII)
            return true;
        /* A single-byte encoding with no table cannot arise for a server encoding
         * (the generator covers every one PostgreSQL defines), but if a future major
         * adds one, its high bytes stay separators -- the pre-#295 behaviour -- rather
         * than being guessed at. */
        tbl = bm25_sb_wordclass(enc);
        if (tbl == NULL)
            return false;
        return (tbl[(c - 0x80) >> 3] >> ((c - 0x80) & 7)) & 1;
    }
    /* ASCII-only below, deliberately (TEXT-06): the high half is decided above.
     *
     * This used to be a bare isalnum(), which consults LC_CTYPE -- and LC_CTYPE is not
     * the database default collation, is not part of the analyzer fingerprint, and is
     * not named in the determinism contract in bm25.h ("a pure function of
     * (cfg->stem_dict_oid, database default collation, token)"). In a single-byte server
     * encoding that fallback decides run boundaries for EVERY byte including 0x80-0xFF,
     * so the same LATIN1 text tokenized differently under a different LC_CTYPE, changing
     * doclen and therefore every score, with nothing to detect it.
     *
     * This function's header used to argue the locale's isalnum() was "correct and
     * necessary" for a single-byte encoding, and as linguistics it was.
     * But the contract this file publishes is DETERMINISM, and the two were never
     * reconciled -- that unreconciled pair is the defect, not the isalnum() call. Between
     * changing the contract and changing the code, the code loses: a scoring input that
     * varies with an environment setting nothing records is not a property this format
     * can carry.
     *
     * Revision 3 paid for that determinism by making EVERY high byte in a single-byte
     * encoding a separator: LATIN1 'A-umlaut' split its word into cross-matching
     * fragments, and a script that lives entirely in 0x80-0xFF (Cyrillic in WIN1251 or
     * KOI8-R) indexed nothing at all. The table branch above (#295, revision 6) restores
     * those letters without giving the determinism back. The ASCII half below is the
     * test revision 3 already used; c < 0x80 is guaranteed by the branch above. */
    return isalnum(c) ? true : false;
}

/*
 * bm25_mblen_bounded -- byte length of the character whose lead byte is at `p`, which
 * must not extend past the `remaining` (> 0) bytes the caller owns.
 *
 * The extension's one character-length primitive (TEXT-08, #308). It replaces pg_mblen,
 * which core marked deprecated in 17.8 / 18.2 (an unbounded historical alias), and it
 * deliberately does NOT use the bounded pg_mblen_range that release added: that symbol
 * is absent before those minors and the floor is 17.0, so using it means a minor-version
 * #if, and a binary built on 17.8+ headers then fails to load on 17.0-17.7. This is
 * built on pg_encoding_mblen instead, the long-standing frontend API, which reads only
 * the lead byte in every SERVER encoding (the ones whose length depends on a trail byte,
 * GB18030 among them, are client-only), so calling it on the last owned byte is safe.
 *
 * A length that overruns `remaining` is a truncated trailing sequence. Text reaching
 * here is encoding-validated, so only corrupt input can produce one; it ERRORs with
 * core's own "invalid byte sequence" report, as pg_mblen_range does, instead of being
 * clamped silently into a term.
 */
int
bm25_mblen_bounded(const char *p, int remaining)
{
    int clen = pg_encoding_mblen(GetDatabaseEncoding(), p);

    if (clen > remaining)
        report_invalid_encoding(GetDatabaseEncoding(), p, remaining);
    return clen;
}

/*
 * bm25_next_char -- measure the character at `p` and classify it.
 *
 * Returns its byte length, always >= 1 and never more than `remaining`, so a
 * caller stepping by the return value always advances (no infinite loop) and never
 * reads past the end of the value. A trailing character that `remaining` truncates
 * ERRORs in bm25_mblen_bounded rather than being clamped (TEXT-08).
 */
int
bm25_next_char(const char *p, int remaining, bool *is_word)
{
    int clen = bm25_mblen_bounded(p, remaining);

    /* A multibyte character is a word character by the rule above; a single byte
     * defers to the byte predicate, which is the same test. */
    *is_word = (clen > 1) ? true : bm25_is_word_byte((unsigned char) *p);
    return clen;
}

/*
 * bm25_fold_term -- encoding-aware lowercase of a term, returned NUL-terminated in
 * CurrentMemoryContext with its byte length in *foldedlen.
 *
 * The parity this exists for: the stopwords=none surface path and the wildcard
 * expander both produce dictionary keys WITHOUT going through ts_lexize, so they must
 * land on the same bytes as the keys the stemmed path wrote -- and those were folded by
 * the dictionary itself.
 *
 * On PG18 that parity is exact by construction: dict_snowball's lexize calls
 * str_tolower under DEFAULT_COLLATION_OID, the same call this makes. On PG17 it is
 * NOT -- dict_snowball there uses lowerstr_with_len, which is towlower() over the
 * database LC_CTYPE and ignores the default collation's provider. The two agree for a
 * database created the ordinary way, where collation and ctype come from one locale
 * and the provider is libc; they can diverge on PG17 under an ICU or builtin default
 * collation (ICU maps a dotted capital I to two characters where towlower gives one).
 * Choosing str_tolower anyway is deliberate: it is the current, provider-aware API and
 * the one PG18 already agrees with, and lowerstr_with_len does not exist on 18 to
 * match. See ADR 0046 for the residual exposure and why it is not encoded in the
 * fingerprint.
 *
 * NOT byte-length preserving -- a case fold can change length (ICU maps 'I-dot' to two
 * characters), so callers must use *foldedlen and must not assume offsets into the
 * folded bytes carry over to the original. bm25_analyze keeps its src_off/src_len
 * spans exact by splitting runs on the ORIGINAL text and folding only at emit.
 */
char *
bm25_fold_term(const char *s, int len, int *foldedlen)
{
    char   *folded;

    if (len <= 0)
    {
        *foldedlen = 0;
        return pstrdup("");
    }
    folded = str_tolower(s, (Size) len, DEFAULT_COLLATION_OID);
    *foldedlen = (int) strlen(folded);
    return folded;
}

/* Fill the derived (non-reloption) fields of a config: dict OID, stemmer_id,
 * stopword_set_hash (and ASCII-lowercase `language` in place). Despite its name it
 * supplies no defaults: see the declaration in bm25.h. The reloption-sourced fields
 * (language/stopwords/tokenizer_type) must already be set by the caller.
 *
 * Public (declared in bm25.h) so callers without an open index relation -- the
 * debug tokenizer and explicit-config overloads -- derive the same fields the
 * resolver produces, from a single code path. */
void
bm25_analyzer_default_config(BM25AnalyzerConfig *out)
{
    static const char *const sw_default = "snowball-default";
    static const char *const sw_none    = "none";
    const char *swtag;

    bm25_lower_ascii(out->language);
    out->stem_dict_oid = bm25_snowball_dict_oid(out->language);
    /* The OID is the HANDLE (ts_lexize needs it); the fingerprint gets the IDENTITY
     * derived from the dictionary's catalog names instead, so the stored value does
     * not move when a pg_upgrade renumbers the same dictionary.
     * The lookup above is still search_path-relative -- "the same reloptions" does not
     * pin the same dictionary -- which is precisely what the identity makes the gate
     * able to see reliably (file header). */
    out->stemmer_id = bm25_stemmer_identity(out->stem_dict_oid);

    swtag = (out->stopwords == BM25_STOPWORDS_NONE) ? sw_none : sw_default;
    out->stopword_set_hash =
        bm25_fnv1a((const unsigned char *) swtag, strlen(swtag),
                   BM25_FNV1A_OFFSET);
}

/* Read a NUL-terminated string reloption by its byte offset into the BM25Options
 * varlena. The PG string-reloption convention: offset 0 == absent, so the caller
 * substitutes the documented default.
 *
 * That fallback is defensive, not the live path, and the comment here used to say
 * otherwise: bm25_handler.c registers NON-NULL default_vals ("english"/"default"/
 * "standard"), and core's fillRelOptions copies a non-null default_val in even for
 * an option the user did not set -- so the offset is never 0 for these. The only way
 * to reach the deflt argument is an index with no reloptions AT ALL, where
 * index_reloptions returns NULL without calling amoptions and this function is never
 * reached either. Keep the two default sets identical anyway; nothing checks it. */
static const char *
bm25_opt_str(const BM25Options *opts, int offset, const char *deflt)
{
    return (offset == 0) ? deflt : (const char *) opts + offset;
}

/* Resolve from a parsed BM25Options bytea. We read the text reloptions through the
 * standard "offset == 0 means default" convention (the resolved string defaults
 * mirror bm25_handler.c's documented english/default/standard). */
void
bm25_analyzer_config_from_opts(struct BM25Options *opts, BM25AnalyzerConfig *out)
{
    const char *language = bm25_opt_str(opts, opts->language_offset, "english");
    const char *stopwords = bm25_opt_str(opts, opts->stopwords_offset, "default");

    /* opts->analyzer_offset is ACCEPTED BUT NEVER CONSUMED -- not by this function
     * and not anywhere else in the tree. There is exactly one analyzer pipeline
     * (standard tokenizer -> Snowball stemmer), so `analyzer` selects nothing:
     * setting it changes no behavior and, because it never reaches the analyzer
     * fingerprint, does not force a REINDEX either. It is reserved surface for a
     * future named-analyzer registry, which has NOT been built -- this was once
     * described as reserved "for M5", but M5 shipped without it, so do not read
     * the reloption's existence as evidence that some analyzer is selectable.
     * The behavior-bearing knobs are language/stopwords/tokenizer.
     *
     * Since #148 HDL-07 the reserved-ness is ENFORCED rather than merely documented:
     * bm25_validate_analyzer (bm25_handler.c) rejects any value but "english" at
     * DDL time, exactly as bm25_validate_tokenizer rejects any tokenizer but
     * "standard". `WITH (analyzer = 'german')` used to be accepted, stem in english,
     * and leave the fingerprint untouched so require_analyzer_match never fired --
     * a silently ignored knob with no observable trace anywhere. */
    memset(out, 0, sizeof(*out));
    strlcpy(out->language, language, BM25_STEMMER_NAME_LEN);
    out->stopwords = (strcmp(stopwords, "none") == 0)
        ? BM25_STOPWORDS_NONE : BM25_STOPWORDS_DEFAULT;
    /* "standard" is the only tokenizer the reloption validator accepts (M3). */
    out->tokenizer_type = BM25_TOKENIZER_STANDARD;
    bm25_analyzer_default_config(out);
}

/* Resolve from an open index relation's reloptions. When the relation has no bm25
 * reloptions (rd_options == NULL), fall back to the documented defaults (language
 * "english", stopwords default, standard tokenizer). */
void
bm25_analyzer_config(Relation index, BM25AnalyzerConfig *out)
{
    BM25Options *opts = (BM25Options *) index->rd_options;

    if (opts == NULL)
    {
        memset(out, 0, sizeof(*out));
        strlcpy(out->language, "english", BM25_STEMMER_NAME_LEN);
        out->stopwords      = BM25_STOPWORDS_DEFAULT;
        out->tokenizer_type = BM25_TOKENIZER_STANDARD;
        bm25_analyzer_default_config(out);
        return;
    }
    bm25_analyzer_config_from_opts(opts, out);
}

/* Read the require_analyzer_match bool reloption off an open index. Defaults to
 * true when the index has no bm25 reloptions (rd_options == NULL): a missing
 * setting means the scan-start gate ERRORs on mismatch (the safe default). Lives
 * here because BM25Options is the reloption struct this file already resolves. */
bool
bm25_require_analyzer_match(Relation index)
{
    BM25Options *opts = (BM25Options *) index->rd_options;

    return opts ? opts->require_analyzer_match : true;
}

/*
 * fpgate_warned_this_xact -- one-entry memo behind the INGEST gate's WARNING, keyed on
 * (index OID, local transaction id). Returns true when this transaction has already been
 * warned about this index, and records the pair otherwise.
 *
 * The key is MyProc->vxid.lxid rather than a real XID because aminsert can run in a
 * transaction that never assigns one, and because the lxid is already there to read.
 * It is a per-backend counter, so a hit needs the SAME backend, and it changes on every
 * transaction start -- which is exactly the reset semantics wanted, with no hook to
 * register and nothing to clean up at abort.
 *
 * SINGLE entry, deliberately. Two mismatched bm25 indexes written in the same
 * statement evict each other every row, so the memo degenerates to no suppression at
 * all for that case -- not "one extra line". That is accepted rather than overlooked:
 * a hash table here would cost lifetime management on a per-row path, and the cost of
 * being wrong in either direction is log volume, never correctness. That is the whole
 * reason a static is defensible here and would not be if this gated anything.
 *
 * Not applied to the SCAN site: that one already fires once per scan, and memoizing it
 * would suppress the second of two genuinely different scans in one transaction.
 */
static bool
fpgate_warned_this_xact(Relation index)
{
    static Oid                  warned_relid = InvalidOid;
    static LocalTransactionId   warned_lxid  = InvalidLocalTransactionId;

    Oid                 relid = RelationGetRelid(index);
    LocalTransactionId  lxid  = MyProc->vxid.lxid;

    if (warned_relid == relid && warned_lxid == lxid &&
        LocalTransactionIdIsValid(lxid))
        return true;

    warned_relid = relid;
    warned_lxid  = lxid;
    return false;
}

/*
 * bm25_fingerprint_gate -- compare a freshly resolved analyzer fingerprint against
 * the one the build stamped on the metapage. On mismatch: ERROR
 * (ERRCODE_FEATURE_NOT_SUPPORTED) by default, WARNING when the index reloption
 * require_analyzer_match = false, then proceed.
 *
 * WHY IT LIVES HERE. It used to be file-static-turned-extern in bm25_scan.c, back
 * when the scan was its only caller. Both of its operands are this file's
 * (bm25_analyzer_fingerprint, bm25_require_analyzer_match), and since #188 the WRITE
 * path calls it too -- so keeping it in bm25_scan.c would have meant bm25_build.c
 * including a scan-private header to reach a predicate about the analyzer. The scanner
 * (bm25_scan_rank.c) still owns the genuinely scan-start helpers bm25_debug.c reuses.
 *
 * WHY ONE FUNCTION AND NOT TWO. The comparison and the require_analyzer_match ->
 * severity decision are written ONCE and shared by both sites; `site` and `elevel`
 * then select nothing but the wording. Issue #157 is this project's own record of what
 * happens when one predicate gets written twice: the copies drift and only one of them
 * gets fixed. (The ingest side spends two ereports rather than one because "the row
 * WOULD be stored" and "the row IS stored" are different statements, and a message
 * that hedged between THOSE would be the useless kind of accurate. That is a different
 * axis from the token-divergence hedge below, which is genuine uncertainty.)
 *
 * WHY THE WORDING DIFFERS ANYWAY. The severity is symmetric but the CONSEQUENCE of
 * proceeding is not. On the read path a mismatch costs one session's results and the
 * next session, off the shadowing path, is fine. On the write path whatever is stored
 * persists: fixing the search_path afterwards recovers nothing, only REINDEX does. A
 * message that told the two apart only by the word "query" would understate the second,
 * so the ingest text names the durability.
 *
 * WHAT THE INGEST TEXT MUST NOT CLAIM, though, is that the row is definitely lost. The
 * gate compares fingerprints; it cannot compare TOKENS. A fingerprint moves for two
 * quite different reasons: a genuinely different stemming dictionary (tokens diverge,
 * the row really is unfindable) and a re-encoding of an existing component -- ADR 0080's
 * stemmer_id change is exactly that -- where tokenization is byte-identical and the row
 * is perfectly findable. So the primary line hedges ("may not be findable") and the
 * detail explains which case is which. The strong version was wrong for the one
 * transition this warning exists to serve, which is the worst possible audience for it.
 *
 * WHY require_analyzer_match = false STILL PROCEEDS ON WRITE. ADR 0080 recommends that
 * setting as the safe deferral for the #62 transition, on the ground that tokenization
 * is byte-identical across it. Hard-erroring on ingest regardless would turn every
 * table mid-transition read-only, which is a worse failure than the one being
 * prevented. The escape hatch stays; it just stops being quiet.
 *
 * WHY THE INGEST WARNING IS MEMOIZED PER TRANSACTION. Unlike the scan gate, which fires
 * once per scan, this one is on a PER-ROW path. Unmemoized, the very configuration ADR
 * 0080 recommends turns `INSERT INTO t SELECT ... FROM huge` into N client warnings AND
 * N server log lines (log_min_messages defaults to warning) -- a routine bulk load can
 * fill a log volume, which is plausibly worse than the outage the setting defers. One
 * warning per (index, local transaction) is enough to tell the operator what is
 * happening; the message says so, so nobody reads a single warning as a single row.
 * A one-entry static memo suffices because this is warning SUPPRESSION, not correctness:
 * a missed suppression costs an extra warning and a stale hit costs a skipped one, and
 * the ERROR path -- the one that protects the index -- is never memoized at all.
 *
 * resolved_fp is the fingerprint recomputed from the index's OWN reloptions -- at scan
 * start for BM25_GATE_SCAN, per row before tokenizing for BM25_GATE_INGEST. index_fp is
 * the stamped metapage value. Neither ERROR is a retryable serialization failure, so
 * both propagate cleanly through the scan's D-HORIZON/C4 retry wrapper (which catches
 * only ERRCODE_T_R_SERIALIZATION_FAILURE).
 */
void
bm25_fingerprint_gate(Relation index, uint32 resolved_fp, uint32 index_fp,
                      BM25GateSite site)
{
    int elevel;

    if (resolved_fp == index_fp)
        return;

    /* THE single severity decision, made once for both sites. Everything below is
     * message dispatch: four whole message literals rather than one assembled from
     * fragments, because a fragment is not a sentence a reader (or a translator) can
     * check, and because keeping the two read-path strings intact makes "the scan
     * wording did not change" verifiable by diff rather than by argument. */
    elevel = bm25_require_analyzer_match(index) ? ERROR : WARNING;

    if (site == BM25_GATE_INGEST)
    {
        if (elevel == ERROR)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bm25: analyzer fingerprint mismatch on insert "
                            "(row %u, index %u)", resolved_fp, index_fp),
                     errdetail("The row would be stored under an analyzer this index "
                               "does not advertise, and any terms it contributed might "
                               "not be findable until the index was rebuilt."),
                     errhint("Restore the search_path or the language reloption the "
                             "index was built with, or REINDEX the index.")));
        else if (!fpgate_warned_this_xact(index))
            /* The one message in this gate whose primary line carries no fingerprint
             * integers, on purpose: it is the only one a regression suite can observe
             * WITHOUT a plpgsql handler to hide them behind (a WARNING cannot be
             * caught), and the two integers fold the database encoding, so printing
             * them on the primary line would make the expected output
             * environment-dependent. In DETAIL they are still there for a user, and
             * \set VERBOSITY terse drops them for the suite. That split -- short
             * primary line, diagnostics in DETAIL -- is the house PG message style
             * anyway; the read-path messages predate it. */
            ereport(WARNING,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bm25: storing rows under a MISMATCHED analyzer; they may "
                            "not be findable until REINDEX"),
                     errdetail("Row fingerprint %u, index fingerprint %u. "
                               "require_analyzer_match = false, so rows are stored "
                               "anyway. Whether their terms actually differ depends on "
                               "what moved the fingerprint: a re-encoding of an "
                               "existing component leaves tokenization byte-identical "
                               "and the rows are findable, while a different stemming "
                               "dictionary does not and they are not. Either way, "
                               "unlike the same mismatch on the read path, what is "
                               "written persists until REINDEX. Reported once per "
                               "index per transaction, however many rows follow.",
                               resolved_fp, index_fp),
                     errhint("Restore the search_path or the language reloption the "
                             "index was built with, then REINDEX to make any affected "
                             "rows findable again.")));
    }
    else
    {
        /* Both read-path strings are verbatim what this gate has emitted since M3;
         * suites 26 and 101 pin them, and #188 changed nothing here. */
        if (elevel == ERROR)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bm25: analyzer fingerprint mismatch "
                            "(query %u, index %u); REINDEX or set "
                            "require_analyzer_match = false",
                            resolved_fp, index_fp)));
        else
            ereport(WARNING,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bm25: analyzer fingerprint mismatch "
                            "(query %u, index %u); proceeding because "
                            "require_analyzer_match = false",
                            resolved_fp, index_fp)));
    }
}

/* M4: read phrase_fallback off an open index (D7). Returns true when it is 'and'
 * (WARN + AND-of-terms fallback), false for the default 'error' (raise) -- the safe
 * default: a phrase query against a position-less index must never silently degrade
 * to a false-positive AND-match.
 *
 * Only ONE of the two defaults below is live. `opts == NULL` -- the index carries no
 * bm25 reloptions at all, so index_reloptions never called bm25_options -- is the
 * reachable case. The bm25_opt_str fallback is not: phrase_fallback is registered
 * with the non-NULL default_val "error", so core's fillRelOptions copies it into the
 * struct even when the user never set the option and the offset is never 0 (see the
 * add_string_reloption block in bm25_handler.c). It is kept as the belt-and-braces
 * pairing every other bm25_opt_str call has, and because the two default sets are
 * the same strings kept in step by hand. Lives here like bm25_require_analyzer_match
 * because BM25Options is the reloption struct this file resolves. */
bool
bm25_phrase_fallback_is_and(Relation index)
{
    BM25Options *opts = (BM25Options *) index->rd_options;
    const char  *v;

    if (opts == NULL)
        return false;               /* no reloptions => 'error' */
    v = bm25_opt_str(opts, opts->phrase_fallback_offset, "error");
    return strcmp(v, "and") == 0;
}

/* Little-endian serialize a uint32 into a 4-byte buffer. The fingerprint is an
 * on-disk value compared across machines (replica equality), so byte order is
 * fixed LE regardless of host endianness. */
static inline void
bm25_put_le32(unsigned char buf[4], uint32 v)
{
    buf[0] = (unsigned char) (v & 0xff);
    buf[1] = (unsigned char) ((v >> 8) & 0xff);
    buf[2] = (unsigned char) ((v >> 16) & 0xff);
    buf[3] = (unsigned char) ((v >> 24) & 0xff);
}

/* ---------------------------------------------------------------------------
 * The behavioural probe -- fingerprint component 7 (#296, analyzer revision 6).
 * ---------------------------------------------------------------------------
 * Components 1-6 say which dictionary the analyzer reaches and under what
 * configuration; none of them says what that dictionary DOES. So a dictionary whose
 * behaviour changed under an unchanged name passed the gate: PostgreSQL 18's Snowball
 * update changed six English stems (added: ad -> add, and egging, erring, offing...),
 * pg_upgrade carries the index files across without rebuilding them, and the
 * name-derived stemmer identity (ADR 0080) is identical on both sides -- so an index
 * holding 'ad' answered a query analyzed to 'add' with zero rows, silently. The same
 * blindness covered ALTER TEXT SEARCH DICTIONARY ... (StopWords = ...) in place.
 *
 * The probe runs the analyzer's own two transforms over a FIXED word list and hashes
 * what comes back: for each word, bm25_fold_term's fold (the fold bm25 applies itself
 * on the stopwords=none and wildcard paths) and the resolved dictionary's lexize output
 * for the word AS INGEST PASSES IT -- raw-cased, the dictionary folding its own input,
 * exactly as bm25_analyze calls it. A stoplist drop is recorded as zero lexemes. The
 * stopwords reloption is deliberately NOT applied: it is already component 3, and
 * keeping it out is what makes the dictionary OID a sufficient cache key.
 *
 * WHAT IT CATCHES, AND WHAT IT CANNOT. A change to any probe word's fold or lexemes.
 * Not a change confined to words outside the list -- the list IS the coverage, which is
 * why it carries the known 17 -> 18 English words verbatim, a spread over every
 * Snowball English step, common stopwords, and ASCII words for the other Snowball
 * languages. That reach is deliberate in both directions: a collation-library upgrade
 * (ICU, glibc) that changes how a probe word folds trips the gate too, which is
 * correct -- the stored terms were written under the old fold -- and is now a REINDEX
 * event the gate can SEE rather than one documented and hoped for.
 *
 * NON-ASCII PROBES ONLY IN A UTF-8 DATABASE. The literals below are UTF-8; converting
 * them into another server encoding would go through pg_conversion, which a CREATE
 * DEFAULT CONVERSION can redirect -- an environment input the fingerprint does not
 * record. The encoding is already component 6, so selecting the list by it keeps the
 * probe a pure function of recorded inputs. Every PostgreSQL server encoding is an
 * ASCII superset, so the ASCII list is valid everywhere.
 *
 * THE LISTS ARE PART OF THE ON-DISK CONTRACT. Adding, removing, reordering or
 * re-spelling a word changes every fingerprint: do it only with a
 * BM25_ANALYZER_REVISION bump.
 */
static const char *const bm25_probe_ascii[] = {
    /* PostgreSQL 17 -> 18 Snowball English Step 1b (undoubling after a word-initial
     * a/e/o): every one of these stems differently on the two majors. */
    "added", "adding", "addedly", "egging", "erring", "erringly", "offing",
    /* Snowball English, one or more per step: 1a, 1b, 1c, 2, 3, 4, 5, and the
     * exception lists. */
    "caresses", "ponies", "ties", "cats", "hopping", "filing", "agreed", "feed",
    "happy", "sky", "relational", "conditional", "digitizer", "operator", "feudalism",
    "decisiveness", "hopefulness", "callousness", "formality", "sensitivity",
    "sensibility", "triplicate", "formative", "formalize", "electricity", "hopeful",
    "goodness", "revival", "allowance", "inference", "airliner", "gyroscopic",
    "adjustable", "defensible", "irritant", "replacement", "adjustment", "dependent",
    "adoption", "communism", "activate", "homologous", "effective", "bowdlerize",
    "probate", "rate", "cease", "controlling", "rolling", "skis", "skies", "dying",
    "lying", "tying", "idly", "gently", "ugly", "early", "only", "singly", "news",
    "atlas", "cosmos", "bias", "andes", "inning", "outing", "canning", "herring",
    "earring", "proceed", "exceed", "succeed", "generalization", "communication",
    "arsenal", "running", "connected", "searches",
    /* Stopwords (a stoplist change is an output change) and case (the dictionary's
     * own fold). */
    "the", "and", "of", "is", "a", "was", "being", "have", "ourselves", "The", "RUNNING",
    "CONNECTED", "Searches",
    /* Digits and mixed runs: the tokenizer emits them, so the dictionary sees them. */
    "2024", "abc123", "x86",
    /* Other Snowball languages, ASCII spellings only. */
    "laufen", "katzen", "haeuser", "aufeinanderfolgenden", "und", "der", "nicht",
    "continuation", "maisons", "nationalement", "les", "corriendo", "casas",
    "nacionalidad", "los", "lopen", "kopen", "het", "gatti", "correndo", "della",
    "springande", "och", "kirjoittaminen", "ja",
};

/* UTF-8 only (see above). Uppercase forms exercise the fold, including the cases
 * where providers disagree: the dotted capital I, the Turkish dotless i, the Greek
 * final sigma, and a ligature whose fold changes length. */
static const char *const bm25_probe_utf8[] = {
    "H\xc3\xa4user",                            /* Haeuser */
    "\xc3\x84RGER",                             /* AERGER */
    "Stra\xc3\x9f" "e",                         /* Strasse (sharp s) */
    "\xc3\x9c" "bung",                          /* Uebung */
    "M\xc3\xa4" "dchen",                        /* Maedchen */
    "caf\xc3\xa9",                              /* cafe */
    "\xc3\x89L\xc3\x88VE",                      /* ELEVE */
    "g\xc3\xa9n\xc3\xa9ralement",               /* generalement */
    "na\xc3\xafve",                             /* naive */
    "canci\xc3\xb3n",                           /* cancion */
    "NI\xc3\x91OS",                             /* NINOS */
    "acci\xc3\xb3n",                            /* accion */
    "\xc4\xb0STANBUL",                          /* ISTANBUL, dotted capital I */
    "\xc4\xb1l\xc4\xb1k",                       /* ilik, dotless i */
    "KI\xc5\x9e",                               /* KIS, S-cedilla */
    "\xce\xa3\xce\x8a\xce\xa3\xce\xa5\xce\xa6\xce\x9f\xce\xa3", /* SISYFOS, final sigma */
    "\xce\xbf\xce\xb4\xcf\x8c\xcf\x82",         /* odos */
    "\xd0\x9c\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0", /* Moskva */
    "\xd0\xa1\xd0\xa2\xd0\x9e\xd0\x9b\xd0\x98\xd0\xa6\xd0\x90", /* STOLITSA */
    "\xd0\xa0\xd0\xbe\xd1\x81\xd1\x81\xd0\xb8\xd0\xb8", /* Rossii */
    "\xd0\xb1\xd0\xb5\xd0\xb3\xd1\x83\xd1\x89\xd0\xb8\xd0\xb9", /* begushchii */
    "ge\xc3\xafnteresseerd",                    /* geinteresseerd */
    "\xc3\x85ngstr\xc3\xb6m",                   /* Angstrom */
    "\xc3\xb6ppna",                             /* oppna */
    "\xef\xac\x81le",                           /* file, fi ligature */
};

/* Per-backend cache: dictionary OID -> probe hash. The gate runs per INSERTed row
 * (ADR 0081), so the probe -- a few hundred lexize calls -- must run once per
 * dictionary per backend, not per row; a hit is one dynahash lookup. */
typedef struct BM25ProbeCacheEnt
{
    Oid     dictoid;            /* hash key */
    bool    valid;              /* false once a TSDICTOID invalidation has arrived */
    uint32  hash;
} BM25ProbeCacheEnt;

static HTAB    *probe_cache = NULL;

/* Bumped by every TSDICTOID invalidation. A computation that straddles one -- lexize
 * and the dictionary-cache lookup both touch the catalog, which can process pending
 * invalidations -- must not be stored as valid, or an ALTER that landed mid-probe
 * would be cached away; see bm25_analyzer_probe_hash. */
static uint64   probe_inval_count = 0;

/* TSDICTOID syscache callback. Drops EVERY entry rather than matching hashvalue: an
 * ALTER/DROP TEXT SEARCH DICTIONARY is rare, a recompute is cheap, and matching would
 * need each entry's syscache hash stored alongside it for no measurable gain. This is
 * what lets an ALTER TEXT SEARCH DICTIONARY in the SAME session move the fingerprint
 * at the next statement (the dictionary cache that lexize goes through invalidates on
 * the same syscache, so the recompute sees the new options). */
#if PG_VERSION_NUM >= 190000
typedef SysCacheIdentifier BM25SyscacheId;     /* PG19 typed the callback's cache id */
#else
typedef int BM25SyscacheId;
#endif

static void
bm25_probe_cache_inval(Datum arg, BM25SyscacheId cacheid, uint32 hashvalue)
{
    HASH_SEQ_STATUS     st;
    BM25ProbeCacheEnt  *e;

    probe_inval_count++;
    if (probe_cache == NULL)
        return;
    hash_seq_init(&st, probe_cache);
    while ((e = (BM25ProbeCacheEnt *) hash_seq_search(&st)) != NULL)
        e->valid = false;
}

/* Fold (bm25_fold_term) and lexize each word of one probe list into h. Each word's
 * contribution is self-delimiting -- the fold up to and including a NUL, the lexeme
 * count, then each lexeme up to and including its NUL -- so no two different outputs
 * concatenate to the same bytes. */
static uint32
bm25_probe_list(TSDictionaryCacheEntry *dict, const char *const *words, int nwords,
                uint32 h)
{
    int     w;

    for (w = 0; w < nwords; w++)
    {
        int             len = (int) strlen(words[w]);
        int             foldedlen;
        char           *folded;
        char           *word;
        TSLexeme       *res;
        uint32          nlex = 0;
        uint32          i;
        unsigned char   le[4];

        folded = bm25_fold_term(words[w], len, &foldedlen);
        h = bm25_fnv1a((const unsigned char *) folded, (Size) foldedlen + 1, h);

        /* A writable NUL-terminated copy, as bm25_analyze hands the dictionary: lexize
         * takes a non-const pointer, and a string literal is not the place to find out
         * whether some dictionary scribbles on its input. */
        word = pnstrdup(words[w], len);
        res = (TSLexeme *) DatumGetPointer(
            FunctionCall4(&dict->lexize,
                          PointerGetDatum(dict->dictData),
                          PointerGetDatum(word),
                          Int32GetDatum(len),
                          PointerGetDatum(NULL)));
        if (res != NULL)
            while (res[nlex].lexeme != NULL)
                nlex++;
        bm25_put_le32(le, nlex);
        h = bm25_fnv1a(le, 4, h);
        for (i = 0; i < nlex; i++)
            h = bm25_fnv1a((const unsigned char *) res[i].lexeme,
                           strlen(res[i].lexeme) + 1, h);
    }
    return h;
}

/*
 * bm25_analyzer_probe_hash -- component 7's value for one dictionary: FNV-1a over
 * bm25_probe_list's output for the ASCII list, then (UTF-8 databases only) the UTF-8
 * list. Cached per backend by dictionary OID; see the block comment above.
 *
 * The work runs in a private context deleted before return, so the few hundred
 * lexeme arrays the dictionary pallocs do not land in the caller's context -- which
 * on the INSERT path is the per-row one.
 */
static uint32
bm25_analyzer_probe_hash(Oid dictoid)
{
    BM25ProbeCacheEnt      *e;
    bool                    found;
    uint64                  inval_before;
    TSDictionaryCacheEntry *dict;
    MemoryContext           probecxt;
    MemoryContext           oldcxt;
    uint32                  h = BM25_FNV1A_OFFSET;

    if (probe_cache == NULL)
    {
        HASHCTL ctl;

        memset(&ctl, 0, sizeof(ctl));
        ctl.keysize   = sizeof(Oid);
        ctl.entrysize = sizeof(BM25ProbeCacheEnt);
        ctl.hcxt      = CacheMemoryContext;
        probe_cache = hash_create("bm25 analyzer probe cache", 8, &ctl,
                                  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
        CacheRegisterSyscacheCallback(TSDICTOID, bm25_probe_cache_inval, (Datum) 0);
    }

    e = (BM25ProbeCacheEnt *) hash_search(probe_cache, &dictoid, HASH_FIND, NULL);
    if (e != NULL && e->valid)
        return e->hash;

    inval_before = probe_inval_count;
    probecxt = AllocSetContextCreate(CurrentMemoryContext, "bm25 analyzer probe",
                                     ALLOCSET_DEFAULT_SIZES);
    oldcxt = MemoryContextSwitchTo(probecxt);

    dict = lookup_ts_dictionary_cache(dictoid);
    h = bm25_probe_list(dict, bm25_probe_ascii, lengthof(bm25_probe_ascii), h);
    if (GetDatabaseEncoding() == PG_UTF8)
        h = bm25_probe_list(dict, bm25_probe_utf8, lengthof(bm25_probe_utf8), h);

    MemoryContextSwitchTo(oldcxt);
    MemoryContextDelete(probecxt);

    /* Entered only after the probe succeeded, so an ERROR inside lexize leaves no
     * half-built entry; and stored as valid only if no invalidation arrived while it
     * ran (the value is still correct to RETURN -- it reflects the catalog this
     * statement saw -- it just must not outlive the next one). */
    e = (BM25ProbeCacheEnt *) hash_search(probe_cache, &dictoid, HASH_ENTER, &found);
    e->hash  = h;
    e->valid = (probe_inval_count == inval_before);
    return h;
}

uint32
bm25_analyzer_fingerprint(const BM25AnalyzerConfig *cfg)
{
    uint32          h = BM25_FNV1A_OFFSET;
    unsigned char   le[4];

    /* 1. LE32(stemmer_id) -- bm25_stemmer_identity's hash of the resolved dictionary's
     * schema-qualified name and template, NOT its OID. Same slot, same width; only the
     * value's derivation changed, which is why this rode a BM25_ANALYZER_REVISION bump
     * rather than a new appended component. */
    bm25_put_le32(le, cfg->stemmer_id);
    h = bm25_fnv1a(le, 4, h);

    /* 2. language bytes, NUL-terminated, no padding (the lowercased language up to
     * and including its terminator). */
    h = bm25_fnv1a((const unsigned char *) cfg->language,
                   strlen(cfg->language) + 1, h);

    /* 3. LE32(stopword_set_hash) */
    bm25_put_le32(le, cfg->stopword_set_hash);
    h = bm25_fnv1a(le, 4, h);

    /* 4. LE32(tokenizer_type) */
    bm25_put_le32(le, (uint32) cfg->tokenizer_type);
    h = bm25_fnv1a(le, 4, h);

    /* 5. LE32(BM25_ANALYZER_REVISION) -- the analyzer's OUTPUT revision, appended
     * (never reordered: components 1-4 keep their positions, which is what the
     * on-disk order contract protects). See the constant for when to bump it. */
    bm25_put_le32(le, BM25_ANALYZER_REVISION);
    h = bm25_fnv1a(le, 4, h);

    /* 6. LE32(database encoding) -- appended for #151 (TEXT-06).
     *
     * The five components above describe the analyzer's configuration and code, and
     * nothing described its ENVIRONMENT. Yet run splitting branches on
     * pg_database_encoding_max_length(): the same configuration tokenizes differently in
     * a single-byte database than in a UTF-8 one, because a high byte is a whole
     * character in one and a continuation byte in the other. An index dumped and restored
     * into a differently-encoded database therefore kept a fingerprint that said its
     * terms were still valid.
     *
     * Encoding is the right operand and LC_CTYPE is not, which is the whole point of
     * TEXT-06's other half: the code no longer consults LC_CTYPE, so there is nothing
     * about it left to fingerprint. Fingerprinting a dependency is the alternative to
     * removing it, and removing it is better where the dependency was never wanted.
     * What remains -- the encoding branch -- IS wanted, and is now recorded.
     *
     * Appended, never reordered: components 1-5 keep their positions, which is the
     * on-disk order contract. Adding a component changes every fingerprint, so it can
     * only ride with a revision bump, and it does. */
    bm25_put_le32(le, (uint32) GetDatabaseEncoding());
    h = bm25_fnv1a(le, 4, h);

    /* 7. LE32(probe hash) -- appended for #296: what the resolved dictionary DOES to a
     * fixed word list, where components 1-6 only say which dictionary it is and in
     * what environment. See bm25_analyzer_probe_hash. Appended; rode revision 6. */
    bm25_put_le32(le, bm25_analyzer_probe_hash(cfg->stem_dict_oid));
    h = bm25_fnv1a(le, 4, h);

    return h;
}

/*
 * bm25_debug_analyzer_fingerprint(index regclass) RETURNS bigint
 *
 * Recompute the analyzer fingerprint from the index's CURRENT reloptions.
 * This is exactly the value the scan-time fingerprint gate computes query-side,
 * so a regression test can assert that the build-time fingerprint (stored in
 * the metapage, surfaced by bm25_stats) equals the recomputed value.  Returns
 * bigint (uint32 widened) to avoid int4 sign confusion in SQL.
 *
 * STABLE: opens the index relation, which is a catalog lookup.
 */
PG_FUNCTION_INFO_V1(bm25_debug_analyzer_fingerprint);
Datum
bm25_debug_analyzer_fingerprint(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index = bm25_index_open_readable(relid, AccessShareLock);
    BM25AnalyzerConfig  cfg;
    uint32              fp;

    bm25_analyzer_config(index, &cfg);
    fp = bm25_analyzer_fingerprint(&cfg);
    index_close(index, AccessShareLock);

    PG_RETURN_INT64((int64) fp);
}

/*
 * bm25_fieldcfg_fill_page -- the field-config page CONTENT, shared by both writers
 * below. Layout: [BM25FieldConfigHeader][BM25FieldConfig x field_count][store_
 * positions[field_count]][BM25KeyStamp]. fields[] is already fully resolved by bm25_resolve_fields
 * (field_id, field_name = column attname, per-field k1/b/boost, cloned
 * tokenizer_type/stemmer_name). per_field_fingerprint[0..field_count-1] all carry
 * the single index fingerprint -- per-field analyzer divergence is DEFERRED (spec
 * section 9) -- and [field_count..BM25_MAX_FIELDS-1] stay 0.
 *
 * C1 writes all fields on one page (N <= BM25_MAX_FIELDS x sizeof(BM25FieldConfig) fits
 * BLCKSZ); the reader already follows nextblk continuation pages if a future layout
 * overflows one page. Caller has already bm25_page_init'd pg as BM25_PAGE_FIELDCFG
 * inside its own GenericXLog window; this only fills content and raises pd_lower.
 */
static void
bm25_fieldcfg_fill_page(Page pg, const BM25FieldConfig *fields, uint32 field_count,
                        const uint32 *per_field_fingerprint /* >= field_count */,
                        const uint8 *store_positions /* >= field_count, or NULL */,
                        uint8 key_type, uint16 key_size, int key_attno)
{
    BM25KeyStamp            ks;
    BM25FieldConfigHeader  *hdr;
    BM25FieldConfig        *fc;
    char                   *contents;
    char                   *after;
    uint32                  i;

    contents = PageGetContents(pg);
    hdr = (BM25FieldConfigHeader *) contents;
    memset(hdr, 0, sizeof(*hdr));            /* zero per_field_fingerprint[count..] + pad */
    hdr->field_count = field_count;
    for (i = 0; i < field_count; i++)
        hdr->per_field_fingerprint[i] = per_field_fingerprint[i];

    /* sizeof(BM25FieldConfigHeader) (4 + 4*BM25_MAX_FIELDS = 132 B) is not a MAXALIGN(8)
     * multiple, so fc lands 4 bytes off the 8-byte boundary PageGetContents()
     * guarantees. A typed struct assignment through it stores BM25FieldConfig's
     * float8 members at a misaligned address -- UB that UBSan traps, and that can
     * genuinely fault on strict-alignment hardware. memcpy is alignment-agnostic
     * and leaves the on-disk byte layout untouched. */
    fc = (BM25FieldConfig *) (contents + sizeof(BM25FieldConfigHeader));
    for (i = 0; i < field_count; i++)
        memcpy(&fc[i], &fields[i], sizeof(BM25FieldConfig));
    after = (char *) &fc[field_count];

    /* M4 (D12): append a uint8 store_positions[field_count] flag array AFTER the
     * config records -- NOT inside BM25FieldConfig, so an M5 page (which has no flag
     * array) still parses byte-identically. The reader detects the array by bytes
     * remaining past the last config record (below pd_lower). When the caller passes
     * NULL (a positions-disabled build path) the array is omitted, matching an M5
     * page exactly (absent => never consulted since pos_root is Invalid). */
    /* #292: the key stamp's offset is fixed at "just past field_count flag bytes", so
     * a stamped page must carry the flag array. With no flags from the caller, write
     * zeros: an all-zero array is what every reader already assumes when the array is
     * absent, so this changes no reading of the page. */
    if (store_positions != NULL)
        memcpy(after, store_positions, field_count);
    else
        memset(after, 0, field_count);
    after += field_count;

    /* #292: the key-identity stamp (BM25KeyStamp, bm25_format.h). Staged and memcpy'd:
     * `after` is not aligned for the struct, and the memset zeroes the pad bytes for
     * a deterministic WAL image. */
    memset(&ks, 0, sizeof(ks));
    ks.magic     = BM25_KEYSTAMP_MAGIC;
    ks.key_type  = key_type;
    ks.key_size  = key_size;
    ks.key_attno = (int16) key_attno;
    memcpy(after, &ks, sizeof(ks));
    after += sizeof(ks);

    /* MANDATORY: advance pd_lower past the header + all entries + the flag array + the
     * key stamp so Generic WAL page-hole compression keeps them (the metapage/segment-
     * header discipline). */
    ((PageHeader) pg)->pd_lower = after - (char *) pg;
}

/*
 * bm25_fieldcfg_write -- write the N-field config page (MAIN_FORKNUM) and return
 * its root block. The page is BM25_PAGE_FIELDCFG with seg_gen = 0 (per-index,
 * non-segment, never gen-validated on read).
 *
 * WAL discipline (same as the segment-header / metapage writers): bm25_page_alloc
 * returns an EXCL-locked buffer; we register it FULL_IMAGE because the allocator
 * may hand back a reused page whose stale bytes must be replicated. One buffer,
 * one record, throw-free window (the 4-buffer cap holds trivially). The header
 * and each fields[i] are memset before fill (the resolver memsets each fields[i])
 * so the fixed-width name padding is deterministic across replicas.
 *
 * Two live callers: bm25_build (the MAIN_FORKNUM build) and, via the INIT_FORKNUM
 * sibling bm25_fieldcfg_write_init below, bm25_buildempty.
 */
BlockNumber
bm25_fieldcfg_write(Relation index, Relation heaprel,
                    const BM25FieldConfig *fields, uint32 field_count,
                    const uint32 *per_field_fingerprint /* >= field_count */,
                    const uint8 *store_positions /* >= field_count, or NULL */,
                    uint8 key_type, uint16 key_size, int key_attno)
{
    Buffer                  buf;
    GenericXLogState       *st;
    Page                    pg;
    BlockNumber             root;

    /* No metapage read needed: k1/b/boost are already resolved into fields[] by
     * bm25_resolve_fields (write_default read the metapage for defaults; the
     * resolver owns that now). */

    buf = bm25_page_alloc(index, heaprel);   /* EXCL-locked; we PageInit + FPI it */
    st  = GenericXLogStart(index);
    pg  = GenericXLogRegisterBuffer(st, buf, GENERIC_XLOG_FULL_IMAGE);

    bm25_page_init(pg, BM25_PAGE_FIELDCFG);  /* nextblk=Invalid, seg_gen=0 */
    bm25_fieldcfg_fill_page(pg, fields, field_count, per_field_fingerprint, store_positions,
                            key_type, key_size, key_attno);

    root = BufferGetBlockNumber(buf);
    GenericXLogFinish(st);
    UnlockReleaseBuffer(buf);
    return root;
}

/*
 * bm25_fieldcfg_write_init -- the INIT_FORKNUM sibling of bm25_fieldcfg_write, for
 * bm25_buildempty (fix #1) ONLY. bm25_buildempty stamps the INIT_FORKNUM with the
 * SAME analyzer identity a normal build stamps on MAIN_FORKNUM, so a query against
 * the post-crash-reset (empty) unlogged index reads as an empty index of the
 * correct config instead of tripping bm25_fingerprint_gate's mismatch ERROR.
 *
 * INIT_FORKNUM is fresh at buildempty time (metapage block 0 is reserved but not
 * yet written -- see bm25_meta_extend/bm25_meta_finish in bm25_meta.c), so there
 * is no FSM/reuse history to consult the way bm25_page_alloc consults for
 * MAIN_FORKNUM -- a plain P_NEW extend is both correct and exactly what
 * bm25_meta_extend already does for block 0 of this same fork.
 *
 * Fix (adversarial review, 2026-08): this used to write via GenericXLog and rely
 * on the caller's smgrimmedsync for durability. Both halves were wrong the same
 * way bm25_meta_init's INIT_FORKNUM path was: GenericXLog gates WAL on
 * RelationNeedsWAL, which is false for an unlogged relation's init fork, so it
 * silently emitted no WAL record; smgrimmedsync only flushes what's already on
 * the smgr md layer, and this page was never smgrwritten (only modified in
 * shared buffers), so it flushed the zero page smgrzeroextend wrote at P_NEW
 * time, not the real content. Matches bm25_meta_finish's fix exactly: PageInit +
 * fill directly on the buffer inside a critical section, MarkBufferDirty, then
 * log_newpage_buffer -- an unconditional XLOG_FPI record independent of
 * RelationNeedsWAL, which is what makes this page crash-recoverable before the
 * next checkpoint and replicated to a standby. No smgrimmedsync needed: FPI
 * replay provides the durability a synchronous flush at creation time cannot
 * (a flush only helps if no crash intervenes before it runs).
 */
BlockNumber
bm25_fieldcfg_write_init(Relation index,
                         const BM25FieldConfig *fields, uint32 field_count,
                         const uint32 *per_field_fingerprint /* >= field_count */,
                         const uint8 *store_positions /* >= field_count, or NULL */,
                         uint8 key_type, uint16 key_size, int key_attno)
{
    Buffer                  buf;
    Page                    pg;
    BlockNumber             root;

    buf = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    pg  = BufferGetPage(buf);

    bm25_page_init(pg, BM25_PAGE_FIELDCFG);
    bm25_fieldcfg_fill_page(pg, fields, field_count, per_field_fingerprint, store_positions,
                            key_type, key_size, key_attno);

    root = BufferGetBlockNumber(buf);

    START_CRIT_SECTION();
    MarkBufferDirty(buf);
    log_newpage_buffer(buf, true);   /* true: standard pd_lower/pd_upper layout */
    END_CRIT_SECTION();

    UnlockReleaseBuffer(buf);
    return root;
}

/*
 * bm25_fieldcfg_read -- read the per-index field-config page chain rooted at
 * `root` into hdr_out + a caller array sized >= BM25_MAX_FIELDS.
 *
 * Flat-page convention: header at PageGetContents(), entries packed after it.
 * M3 writes exactly one page holding all entries, but the reader walks nextblk
 * and copies field_count entries total so it stays page-count-agnostic for M5
 * (continuation pages carry NO header -- entries begin at PageGetContents()).
 * seg_gen is NOT validated (the page is per-index, seg_gen = 0). AccessShare on
 * each page; no WAL (read-only).
 */
void
bm25_fieldcfg_read(Relation index, BlockNumber root,
                   BM25FieldConfigHeader *hdr_out,
                   BM25FieldConfig *cfg_out /* >= BM25_MAX_FIELDS */,
                   uint8 *out_store_positions /* >= BM25_MAX_FIELDS, or NULL */)
{
    Buffer                  buf;
    Page                    pg;
    char                   *contents;
    BM25FieldConfigHeader  *hdr;
    uint32                  got = 0;
    uint32                  i;
    uint32                  visited = 0;    /* continuation pages walked (#65 cycle cap) */
    BlockNumber             blk = root;
    BlockNumber             nblocks = RelationGetNumberOfBlocks(index);

    /* Corruption, not a state to wait out (issue #313 XCUT-12 inventory): every
     * readable format carries a field-config page, the build writes it before any
     * other backend can see the index, and bm25_buildempty writes the INIT fork's.
     * The one caller that legitimately sees an unbuilt metapage,
     * bm25_index_stores_positions, tests for Invalid before calling here. */
    if (root == InvalidBlockNumber)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field-config page not written (field_config_blkno is invalid)"),
                 errhint("REINDEX the index.")));

    buf = ReadBuffer(index, blk);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg  = BufferGetPage(buf);
    /* #65: this walk had no page-kind check at all, on either the root or the
     * continuation pages -- the file header conceded seg_gen is never validated
     * here either. So a field_config_blkno left pointing at a recycled page, or a
     * nextblk aimed at a DICT/POST page, was decoded as BM25FieldConfig records:
     * silently wrong analyzer configuration rather than the loud
     * ERRCODE_INDEX_CORRUPTED the rest of the trust boundary promises, and the
     * fingerprint gate then reports an "analyzer mismatch" on a healthy index.
     *
     * expected_gen = 0 because field-config pages are per-index, not per-segment:
     * bm25_page_init stamps them seg_gen = 0, so gen validation is a documented
     * no-op here and only the kind half does work. */
    bm25_seg_page_validate_kind(pg, 0, BM25_PAGE_FIELDCFG);
    contents = PageGetContents(pg);
    hdr = (BM25FieldConfigHeader *) contents;
    *hdr_out = *hdr;                         /* field_count + per_field_fingerprint[] */

    if (hdr->field_count > BM25_MAX_FIELDS)
    {
        UnlockReleaseBuffer(buf);
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field-config field_count %u exceeds BM25_MAX_FIELDS %d",
                        hdr->field_count, BM25_MAX_FIELDS)));
    }

    /* First page: entries begin right after the header. */
    {
        /* src is misaligned by the same 4 bytes as the writer's fc (see
         * bm25_fieldcfg_write above) -- memcpy, not a typed dereference, for the
         * same reason. */
        BM25FieldConfig *src = (BM25FieldConfig *) (contents + sizeof(BM25FieldConfigHeader));
        /* bm25_page_content_bytes bounds pd_lower before it becomes `end` (see its
         * header comment); the `got != field_count` check below this block's
         * continuation-page loop is a backstop, not a substitute -- validating at
         * the point of read is the same discipline every chain reader in the
         * segment reader (bm25_seg_*.c) now follows. */
        char            *end = (char *) PageGetContents(pg) + bm25_page_content_bytes(pg);
        while (got < hdr_out->field_count && (char *) (src + 1) <= end)
        {
            memcpy(&cfg_out[got], src, sizeof(BM25FieldConfig));
            got++;
            src++;
        }

        /* M4 (D12): the per-field store_positions[field_count] uint8 flag array follows
         * the config records on THIS page, present ONLY on M4-written pages. Detect it
         * by bytes remaining past the last config, below pd_lower. Absent (pre-M4 page)
         * leaves out_store_positions untouched -- such a page has pos_root Invalid, so
         * the flags are never consulted. All configs + flags fit one page (BM25_MAX_FIELDS
         * StaticAssert), so the array is never split across a continuation page. */
        if (out_store_positions != NULL)
        {
            char *flags = (char *) src;      /* just past the configs we consumed */
            if (got == hdr_out->field_count &&
                flags + hdr_out->field_count <= end)
                memcpy(out_store_positions, flags, hdr_out->field_count);
        }
        blk = BM25PageGetOpaque(pg)->nextblk;
    }
    UnlockReleaseBuffer(buf);

    /* M5 continuation pages (none in M3): subsequent pages have NO header --
     * entries begin at PageGetContents(). `got < field_count` alone does not
     * bound the number of PAGES visited -- field_count is capped at BM25_MAX_FIELDS
     * above, but a corrupt nextblk chain whose pages each contribute zero
     * entries would never advance `got`. blk < nblocks is the same
     * out-of-extent backstop bm25_fsm.c:63/233 use -- it stops a stray link
     * that points past the relation's own extent, NOT an in-extent cycle
     * (nblocks bounds the value of blk, not how many times the loop visits a
     * block already in range).
     *
     * #65: an in-extent cycle used to loop forever, survivable only because
     * CHECK_FOR_INTERRUPTS made it killable. It is now BOUNDED as well, by the
     * same counter discipline bm25_pending.c's three chain walkers use. The bound
     * is deliberately tight: the StaticAssert above guarantees all BM25_MAX_FIELDS
     * records plus the flag array fit ONE page, so a correctly written index never
     * enters this loop at all, and even a legitimately split chain cannot need more
     * pages than there are fields. */
    while (got < hdr_out->field_count && blk != InvalidBlockNumber && blk < nblocks)
    {
        BM25FieldConfig *src, *end;

        CHECK_FOR_INTERRUPTS();

        if (++visited > (uint32) BM25_MAX_FIELDS)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: field-config chain revisits blocks; stopped at "
                            "block %u after %u pages", blk, visited - 1)));

        buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        pg  = BufferGetPage(buf);
        /* Same kind gate as the root page: a continuation link is exactly as
         * untrusted as the root pointer. */
        bm25_seg_page_validate_kind(pg, 0, BM25_PAGE_FIELDCFG);
        src = (BM25FieldConfig *) PageGetContents(pg);
        end = (BM25FieldConfig *) ((char *) PageGetContents(pg) + bm25_page_content_bytes(pg));
        while (got < hdr_out->field_count && src + 1 <= end)
            cfg_out[got++] = *src++;
        blk = BM25PageGetOpaque(pg)->nextblk;
        UnlockReleaseBuffer(buf);
    }

    if (got != hdr_out->field_count)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field-config read %u of %u entries",
                        got, hdr_out->field_count)));

    /* Issue #303.D: field ids are dense and positional -- the build writes entry f
     * with field_id f (bm25_build.c) and nothing rewrites it -- so entry i carrying
     * any other id is corruption. It has to be caught HERE, at the decode boundary:
     * the scan resolves a "field:" scope to fields[f].field_id raw, and the phrase
     * recheck then indexes its per-(term, field) stash by that id with no
     * field_count bound of its own, so an id past field_count read past the heap
     * stash, 0xFFFFFFFF widened a scoped query to every field (it is
     * BM25_FIELD_ALL once cast to int32), and two swapped in-range ids answered
     * one column's query from the other. Checking the identity covers all three,
     * at one compare per field per read. Scope: the D1 on-disk validation contract
 * (a corrupt page yields ERRCODE_INDEX_CORRUPTED, not a wrong answer or OOB read). */
    for (i = 0; i < hdr_out->field_count; i++)
        if (cfg_out[i].field_id != i)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: field-config entry %u of index \"%s\" carries field id %u",
                            i, RelationGetRelationName(index), cfg_out[i].field_id),
                     errdetail("Field ids are dense: entry i of the field-config page "
                               "carries field id i."),
                     errhint("REINDEX the index.")));
}

/*
 * bm25_fieldcfg_keystamp_offset -- byte offset, from PageGetContents(), of the #292
 * key stamp on a field-config root page holding field_count configs. One definition
 * for the reader and the debug clear probe, so they cannot disagree about where the
 * tail starts. field_count must already be bounded by BM25_MAX_FIELDS.
 */
static Size
bm25_fieldcfg_keystamp_offset(uint32 field_count)
{
    return sizeof(BM25FieldConfigHeader)
        + (Size) field_count * sizeof(BM25FieldConfig)
        + field_count;                   /* the store_positions flag array */
}

/*
 * bm25_fieldcfg_read_keystamp -- read the #292 key-identity stamp (BM25KeyStamp,
 * bm25_format.h) off the field-config root page into *out.
 *
 * Returns false when the page carries no stamp -- an index built by a binary that
 * predates #292, whose page ends at the flag array (or, for an M5-era page, at the
 * configs). Presence is decided by pd_lower alone, never by format_version: see the
 * additivity note at BM25KeyStamp. Bytes past the flag array that are not a whole,
 * correctly tagged, self-consistent stamp are ERRCODE_INDEX_CORRUPTED, not "no
 * stamp": no writer ever put anything else there, and quietly degrading to the
 * weaker segment-0 check on a damaged page would hide the damage.
 *
 * Root page only: the StaticAssert at the top of this file guarantees configs, flags
 * and stamp all fit the first page. Per-INSERT cost is this one SHARE-locked read
 * of a page that never changes after the build, so it stays hot.
 */
bool
bm25_fieldcfg_read_keystamp(Relation index, BlockNumber root, BM25KeyStamp *out)
{
    Buffer          buf;
    Page            pg;
    char           *contents;
    Size            content;
    Size            off;
    uint32          field_count;
    BM25KeyStamp    ks;
    bool            present = false;
    const char     *bad = NULL;

    if (root == InvalidBlockNumber)     /* corruption: see bm25_fieldcfg_read */
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field-config page not written (field_config_blkno is invalid)"),
                 errhint("REINDEX the index.")));

    buf = ReadBuffer(index, root);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    pg = BufferGetPage(buf);
    bm25_seg_page_validate_kind(pg, 0, BM25_PAGE_FIELDCFG);
    contents = PageGetContents(pg);
    content  = bm25_page_content_bytes(pg);

    if (content < sizeof(BM25FieldConfigHeader))
        bad = "the page is shorter than its header";
    else
    {
        memcpy(&field_count, contents + offsetof(BM25FieldConfigHeader, field_count),
               sizeof(field_count));
        if (field_count > BM25_MAX_FIELDS)
            bad = "its field_count exceeds BM25_MAX_FIELDS";
        else
        {
            off = bm25_fieldcfg_keystamp_offset(field_count);
            if (content > off)
            {
                if (content - off < sizeof(BM25KeyStamp))
                    bad = "the bytes past the store_positions array are a truncated key stamp";
                else
                {
                    memcpy(&ks, contents + off, sizeof(ks));
                    if (ks.magic != BM25_KEYSTAMP_MAGIC)
                        bad = "the bytes past the store_positions array do not carry the key-stamp magic";
                    else
                        present = true;
                }
            }
        }
    }
    UnlockReleaseBuffer(buf);

    if (bad != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INDEX_CORRUPTED),
                 errmsg("bm25: field-config page of index \"%s\" is corrupt: %s",
                        RelationGetRelationName(index), bad),
                 errhint("REINDEX the index.")));
    if (!present)
        return false;

    /* Self-consistency, so a caller can trust the triple: keyless means (NONE, 0, -1);
     * keyed means a known tag at exactly its width (the KEYMAP header's own check)
     * and an attno inside the index's tuple descriptor. */
    if (ks.key_type == BM25_KEY_NONE)
    {
        if (ks.key_size != 0 || ks.key_attno != -1)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: key stamp of index \"%s\" is keyless but carries "
                            "key_size %u, key column %d",
                            RelationGetRelationName(index), ks.key_size, ks.key_attno),
                     errhint("REINDEX the index.")));
    }
    else
    {
        bm25_seg_keymeta_validate(ks.key_type, ks.key_size);
        if (ks.key_attno < 0 || ks.key_attno >= RelationGetDescr(index)->natts)
            ereport(ERROR,
                    (errcode(ERRCODE_INDEX_CORRUPTED),
                     errmsg("bm25: key stamp of index \"%s\" names index column %d, "
                            "outside its %d columns",
                            RelationGetRelationName(index), ks.key_attno,
                            RelationGetDescr(index)->natts),
                     errhint("REINDEX the index.")));
    }
    *out = ks;
    return true;
}

/*
 * bm25_debug_keystamp(index regclass) -- the #292 key-identity stamp, as one row:
 * (stamped, key_type, key_size, key_attno, key_column). stamped = false and the rest
 * NULL for an index whose field-config page has no stamp. key_attno is the stamp's
 * 0-based INDEX attno; key_column is that index column's name, NULL when keyless.
 * Development/regression only: it is how the suites prove each build path (CREATE
 * INDEX, CONCURRENTLY, REINDEX, ambuildempty) stamped what it resolved.
 */
PG_FUNCTION_INFO_V1(bm25_debug_keystamp);
Datum
bm25_debug_keystamp(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index;
    BM25MetaPageData    meta;
    BM25KeyStamp        ks;
    bool                stamped;
    TupleDesc           tupdesc;
    Datum               v[5];
    bool                n[5] = {false, true, true, true, true};
    HeapTuple           tup;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        elog(ERROR, "return type must be a row type");
    tupdesc = BlessTupleDesc(tupdesc);

    index = bm25_index_open_readable(relid, AccessShareLock);
    bm25_meta_read(index, &meta);
    stamped = bm25_fieldcfg_read_keystamp(index, meta.field_config_blkno, &ks);

    v[0] = BoolGetDatum(stamped);
    if (stamped)
    {
        v[1] = Int32GetDatum((int32) ks.key_type);
        v[2] = Int32GetDatum((int32) ks.key_size);
        v[3] = Int32GetDatum((int32) ks.key_attno);
        n[1] = n[2] = n[3] = false;
        if (ks.key_attno >= 0)
        {
            v[4] = CStringGetTextDatum(NameStr(TupleDescAttr(RelationGetDescr(index),
                                                             ks.key_attno)->attname));
            n[4] = false;
        }
    }
    index_close(index, AccessShareLock);

    tup = heap_form_tuple(tupdesc, v, n);
    PG_RETURN_DATUM(HeapTupleGetDatum(tup));
}

/*
 * bm25_debug_clear_keystamp(index regclass) -> bool -- TEST-ONLY, MUTATING.
 * Removes the #292 key stamp from the field-config page, leaving exactly the page a
 * pre-#292 binary writes, so the suites can exercise the unstamped fallback (the
 * segment-0 check) and the drain's mixed-chain defence on an index this binary
 * built. Returns whether a stamp was present.
 *
 * pd_lower is LOWERED back to the end of the flag array and the stamp bytes zeroed;
 * the GenericXLog delta then treats them as page hole, so a standby converges on the
 * same unstamped page. Ownership + AM identity through bm25_index_open_owned, like
 * every other mutating probe; the install script's REVOKE loop covers it as well.
 */
PG_FUNCTION_INFO_V1(bm25_debug_clear_keystamp);
Datum
bm25_debug_clear_keystamp(PG_FUNCTION_ARGS)
{
    Oid                 relid = PG_GETARG_OID(0);
    Relation            index = bm25_index_open_owned(relid, RowExclusiveLock);
    BM25MetaPageData    meta;
    BM25KeyStamp        ks;
    bool                had;

    bm25_meta_read(index, &meta);
    /* Validates the page and the stamp, so this only ever clears a real stamp. */
    had = bm25_fieldcfg_read_keystamp(index, meta.field_config_blkno, &ks);
    if (had)
    {
        Buffer              buf;
        Page                pg;
        GenericXLogState   *st;
        uint32              field_count;
        char               *contents;
        Size                off;

        buf = ReadBuffer(index, meta.field_config_blkno);
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        st = GenericXLogStart(index);
        pg = GenericXLogRegisterBuffer(st, buf, 0);
        contents = PageGetContents(pg);
        memcpy(&field_count, contents + offsetof(BM25FieldConfigHeader, field_count),
               sizeof(field_count));
        off = bm25_fieldcfg_keystamp_offset(field_count);
        memset(contents + off, 0, sizeof(BM25KeyStamp));
        ((PageHeader) pg)->pd_lower = (contents + off) - (char *) pg;
        GenericXLogFinish(st);
        UnlockReleaseBuffer(buf);
    }
    index_close(index, RowExclusiveLock);
    PG_RETURN_BOOL(had);
}

/*
 * bm25_debug_fieldcfg(index regclass) -- one row per configured field from the
 * per-index field-config page (M3: exactly one, the default field). Reads the
 * metapage root, then bm25_fieldcfg_read. Development/regression only.
 *
 * This is the BUILD-ERA stamp, not the live truth. Since k1/b/boost were made
 * live (ADR 0012), the EFFECTIVE per-field k1/b/boost are resolved from the
 * index's CURRENT reloptions (pg_class.reloptions) at every scan start
 * (bm25_resolve_live_params), overlaying whatever this page shows -- an ALTER
 * INDEX changes the former without touching the latter. The stamp keeps sole
 * authority only for store_positions and the analyzer configuration, which
 * stay build-time and REINDEX-gated; for k1/b/boost it is a historical
 * record of what CREATE INDEX / the last REINDEX resolved, useful for
 * auditing, never for reading the current effective value.
 */
PG_FUNCTION_INFO_V1(bm25_debug_fieldcfg);
Datum
bm25_debug_fieldcfg(PG_FUNCTION_ARGS)
{
    ReturnSetInfo          *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
    Oid                     relid = PG_GETARG_OID(0);
    Relation                index;
    BM25MetaPageData        meta;
    BM25FieldConfigHeader   hdr;
    BM25FieldConfig         fields[BM25_MAX_FIELDS];
    Tuplestorestate        *ts;
    TupleDesc               tupdesc;
    MemoryContext           oldctx;
    uint32                  i;

    /* Standard Materialize-mode SRF guard. */
    if (rsi == NULL || !IsA(rsi, ReturnSetInfo) ||
        !(rsi->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bm25: set-valued function called in a context"
                        " that cannot accept a set")));

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
    bm25_meta_read(index, &meta);
    bm25_fieldcfg_read(index, meta.field_config_blkno, &hdr, fields, NULL);

    for (i = 0; i < hdr.field_count; i++)
    {
        Datum   v[6];
        bool    n[6] = {false, false, false, false, false, false};

        v[0] = Int32GetDatum((int32) fields[i].field_id);
        /* field_name is a fixed BM25_FIELD_NAME_LEN on-disk char array documented
         * "NUL-padded", but that is a WRITER convention, not something a reader can
         * trust: CStringGetTextDatum on a corrupt/torn page with no NUL anywhere in
         * the array would strlen() straight past it. strnlen bounds the scan to the
         * array's own declared size, so the worst a corrupt page can do is report
         * the field's full BM25_FIELD_NAME_LEN bytes verbatim, never overrun it. */
        v[1] = PointerGetDatum(cstring_to_text_with_len(fields[i].field_name,
                                   strnlen(fields[i].field_name, BM25_FIELD_NAME_LEN)));
        v[2] = Float8GetDatum(fields[i].k1);
        v[3] = Float8GetDatum(fields[i].b);
        v[4] = Float8GetDatum(fields[i].boost);
        /* Same trust boundary as field_name above: a fixed-width NUL-padded
         * on-disk array is only NUL-terminated by writer convention. */
        v[5] = PointerGetDatum(cstring_to_text_with_len(fields[i].stemmer_name,
                                   strnlen(fields[i].stemmer_name, BM25_STEMMER_NAME_LEN)));
        tuplestore_putvalues(ts, tupdesc, v, n);
    }

    index_close(index, AccessShareLock);
    return (Datum) 0;
}

/* Decode-boundary probe (trust-boundary review, 2026-08): field_name and
 * stemmer_name are fixed-width NUL-padded arrays that are only ever
 * NUL-terminated by WRITER convention -- a real field-config page can never
 * carry a name that fills the WHOLE array with no NUL (identifiers are capped
 * well under BM25_FIELD_NAME_LEN), so a regression suite has no legitimate way
 * to produce that page. This exercises the exact defensive technique used at
 * the fix site (strnlen bound + cstring_to_text_with_len) against a
 * caller-supplied buffer with NO NUL byte anywhere in it, proving the read
 * returns exactly maxlen bytes rather than scanning past them. TEST-ONLY: a
 * pure function of its arguments, no relation touched. */
PG_FUNCTION_INFO_V1(bm25_debug_bounded_name_bytes);
Datum
bm25_debug_bounded_name_bytes(PG_FUNCTION_ARGS)
{
    bytea      *raw    = PG_GETARG_BYTEA_PP(0);
    int32       maxlen = PG_GETARG_INT32(1);
    const char *buf    = VARDATA_ANY(raw);
    int         len    = (int) VARSIZE_ANY_EXHDR(raw);

    if (maxlen < 0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_bounded_name_bytes: maxlen must be non-negative")));
    if (len < maxlen)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("bm25_debug_bounded_name_bytes: input shorter than maxlen")));

    PG_RETURN_TEXT_P(cstring_to_text_with_len(buf, strnlen(buf, (size_t) maxlen)));
}
