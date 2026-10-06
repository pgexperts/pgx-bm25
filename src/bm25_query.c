/* bm25_query.c -- M6 jsonb query tree: parse, flatten, boolean-formula eval, glob.
 *
 * Role in the system: the ONLY place a user-supplied jsonb query object
 * (produced by the bm25_* builder functions) is interpreted. bm25_query_parse
 * walks the jsonb with the low-level JsonbIterator/getKeyJsonValueFromContainer
 * API (never jsonb_get_element/SQL-level accessors -- this runs inside an
 * index scan, before any SPI/executor context exists) and builds an in-memory
 * BM25Query tree, resolving each leaf's "field" against the index's baked
 * field config via the shared bm25_field_by_name (bm25_scan.c).
 *
 * Every ereport(ERROR) below is a validation boundary, not a bug report: a
 * malformed or out-of-policy query object (unknown node key, must_not-only,
 * a wildcard with no '*' or too short a prefix, runaway nesting) is a user
 * mistake, caught here before it reaches the scorer.
 *
 * bm25_query_flatten and bm25_query_eval are the read side of the same
 * contract the M4 phrase-fallback AND path already uses (and_presence /
 * PhraseAndEnt.mask, bm25_scan_match.c): every leaf clause gets a bit in one
 * uint64, and boolean structure becomes a formula over that bitmask,
 * evaluated once per candidate document at drain time.
 */
#include "postgres.h"

#include "bm25_query.h"

#include "lib/stringinfo.h"     /* check_node_keys' message assembly */
#include "nodes/pg_list.h"      /* List/lappend/foreach -- must/should/must_not arrays */
#include "utils/fmgrprotos.h"   /* numeric_int4/numeric_float8/numeric_trunc/numeric_eq --
                                 * jsonb numeric -> C scalar, slop integrality */

/* Defends parse/eval stack depth against a pathologically nested boolean/boost
 * tree; far deeper than any realistic query (a handful of levels). */
#define BM25_QUERY_MAX_DEPTH 32

static BM25Query *parse_node(JsonbContainer *obj, const BM25FieldConfig *fields,
                             uint32 field_count, int depth, int *nleaves,
                             int *nnodes);
static BM25Query *parse_leaf(JsonbContainer *args, BM25QKind kind,
                             const BM25FieldConfig *fields, uint32 field_count,
                             int *nleaves);
static BM25Query *parse_boolean(JsonbContainer *args, const BM25FieldConfig *fields,
                                uint32 field_count, int depth, int *nleaves,
                                int *nnodes);
static BM25Query *parse_boost(JsonbContainer *args, const BM25FieldConfig *fields,
                              uint32 field_count, int depth, int *nleaves,
                              int *nnodes);
static BM25Query **parse_node_array(JsonbContainer *args, const char *key,
                                    const BM25FieldConfig *fields, uint32 field_count,
                                    int depth, int *out_n, int *nleaves, int *nnodes);
static void check_node_keys(JsonbContainer *args, const char *nodename,
                            const char *const *allowed);
static int32 parse_field_id(JsonbContainer *args, const BM25FieldConfig *fields,
                            uint32 field_count);
static void get_required_string(JsonbContainer *args, const char *key,
                                const char *nodename, char **out_text, int *out_len);
static int  get_optional_int(JsonbContainer *args, const char *key, int dflt);
static bool get_optional_bool(JsonbContainer *args, const char *key, bool dflt);
static void validate_wildcard_pattern(const char *pattern, int patlen);
static void flatten_recurse(BM25Query *node, double boost_acc, bool negated_acc,
                            BM25Query **leaves, int *n, int max);

/* Count a leaf as it is PARSED, not after the whole tree is materialized (#68).
 *
 * BM25_QUERY_MAX_LEAVES has always been enforced -- but only in flatten_recurse, which
 * runs after parse has already allocated the entire tree. Depth was bounded
 * (BM25_QUERY_MAX_DEPTH); BREADTH was not, so a wide-but-shallow jsonb value
 * -- {"boolean": {"should": [ {match: ...} x 100000 ]}} -- allocated every one of those
 * nodes before anything objected. Bounded by the jsonb value's own size, so not
 * unbounded in the strict sense, and cancellable since the interrupt check landed; but
 * "the cap exists" and "the cap runs before the work" are different claims and only the
 * first was true.
 *
 * The same constant, applied at the earliest point it can be: a tree is rejected at the
 * 65th leaf rather than at the 100000th allocation. flatten_recurse keeps its own check
 * too, on both leaf_bit's range (shifted into a uint64 presence mask) and the leaves[]
 * array write it guards -- not merely a second copy of this one. With parse_leaf as the
 * only leaf constructor, every tree that reaches flatten has already passed THIS count
 * against the same constant, so flatten's check cannot currently be the one that fires;
 * it is kept anyway because nothing in bm25_query_flatten's signature guarantees that
 * about a future caller (ADR 0099).
 *
 * The message AND the SQLSTATE are flatten's, deliberately: matching flatten's existing
 * 22023 exactly is what keeps a client that branches on SQLSTATE from seeing any change
 * for a tree that is invalid on leaf count alone. (It also agrees with this file's other
 * shape limits -- the depth cap and the phrase.slop bound are 22023 as well; the only
 * 54000s here are the two wildcard checks against the superuser-tunable
 * bm25_native.wildcard_max_* GUCs. The leaf cap itself is a compile-time constant tied
 * to the uint64 presence-mask width, which also sizes the callers' leaves[] arrays; it
 * is not a GUC.) The first draft of this guard used ERRCODE_PROGRAM_LIMIT_EXCEEDED;
 * adversarial review caught it, since no suite pinned the errcode and it would have been
 * the worst of both -- the same message under a different code.
 *
 * For a caller that parses and then flattens, and a tree invalid ONLY on leaf count,
 * enforcing earlier changes nothing but the allocation before the error. Two things do
 * change. bm25_debug_query_parse, which never flattens, had no cap at all and now
 * rejects a 65-leaf tree. And a tree invalid for more than one reason can now report
 * this error where it used to report a later-detected one, sometimes under a different
 * SQLSTATE (42703, 54000, 22003 measured), because this count fires as soon as the
 * 65th leaf is parsed. Both are recorded, with measured examples, in ADR 0099 rather
 * than pinned by tests that would only pin wording. */
static inline void
bm25_query_count_leaf(int *nleaves)
{
    if (++(*nleaves) > BM25_QUERY_MAX_LEAVES)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: query has more than %d leaf clauses (must + should + must_not)",
                        BM25_QUERY_MAX_LEAVES)));
}

/* The closed key set of each node kind's args object (#304 QUERY-02). The
 * parsers read their keys by name with getKeyJsonValueFromContainer, which never
 * sees any other key, so before this check a typo was dropped without a word:
 * {"boolean":{"must":[...],"mustnot":[...]}} returned the documents the
 * exclusion was written to remove, and {"term":{"fild":"body",...}} searched
 * every field. Rejecting is the only safe reading of a key the parser does not
 * act on. The builders in bm25_native--1.0.sql emit exactly these keys, and
 * sql/143_query_args_strict parses every builder's output to keep it so. A key
 * added here must also be read by its parser, or it becomes the same silent drop
 * again. */
static const char *const match_keys[]    = {"field", "terms", NULL};
static const char *const term_keys[]     = {"field", "value", NULL};
static const char *const phrase_keys[]   = {"field", "phrase", "slop", "ordered", NULL};
static const char *const wildcard_keys[] = {"field", "pattern", NULL};
static const char *const boolean_keys[]  = {"must", "should", "must_not", NULL};
static const char *const boost_keys[]    = {"weight", "query", NULL};

/* Fold a key for the "did you mean" hint only: ASCII-lowercase it and drop
 * '_', '-' and ' ', so "mustNot", "must-not" and "MUST_NOT" all meet
 * "must_not". Never used to ACCEPT a key -- matching stays exact. */
static void
fold_key_for_hint(const char *key, int len, StringInfo out)
{
    int i;

    resetStringInfo(out);
    for (i = 0; i < len; i++)
    {
        char c = key[i];

        if (c == '_' || c == '-' || c == ' ')
            continue;
        appendStringInfoChar(out, (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c);
    }
}

/* check_node_keys -- ERROR on the first key of `args` outside `allowed`
 * (NULL-terminated). Walks the object once with skipNested, so a nested value
 * costs one iterator step however large it is. Bounded without an interrupt
 * check: jsonb object keys are unique, so at most one key past the allowed set
 * (four at most) is ever examined before the ERROR. */
static void
check_node_keys(JsonbContainer *args, const char *nodename, const char *const *allowed)
{
    JsonbIterator      *it = JsonbIteratorInit(args);
    JsonbIteratorToken  tok;
    JsonbValue          v;

    while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
    {
        const char *const *a;
        const char        *suggest = NULL;
        StringInfoData     list;
        StringInfoData     fkey;
        StringInfoData     fallowed;

        if (tok != WJB_KEY)
            continue;
        for (a = allowed; *a != NULL; a++)
            if ((int) strlen(*a) == v.val.string.len &&
                memcmp(*a, v.val.string.val, v.val.string.len) == 0)
                break;
        if (*a != NULL)
            continue;

        /* Unknown key: build the message parts, then raise. */
        initStringInfo(&list);
        initStringInfo(&fkey);
        initStringInfo(&fallowed);
        fold_key_for_hint(v.val.string.val, v.val.string.len, &fkey);
        for (a = allowed; *a != NULL; a++)
        {
            appendStringInfo(&list, "%s\"%s\"", a == allowed ? "" : ", ", *a);
            fold_key_for_hint(*a, (int) strlen(*a), &fallowed);
            if (suggest == NULL && strcmp(fkey.data, fallowed.data) == 0)
                suggest = *a;
        }
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: unknown key \"%.*s\" in a \"%s\" query node",
                        v.val.string.len, v.val.string.val, nodename),
                 errdetail("A \"%s\" node accepts only the keys %s.", nodename, list.data),
                 suggest ? errhint("Did you mean \"%s\"?", suggest) : 0));
    }
}

/*
 * parse_node -- the one recursive descent point. Every query-tree node,
 * including the outermost jsonb value, is a single-key object {"<kind>": {...}}
 * (D1's builder-function shape). Reads that ONE key via the JsonbIterator
 * (not getKeyJsonValueFromContainer, which needs to know the key name in
 * advance -- here we don't, that's the whole point of dispatch), dispatches
 * on it, and recurses into the value (which every known kind requires to be
 * itself an object -- the leaf/boolean/boost "args").
 */
static BM25Query *
parse_node(JsonbContainer *obj, const BM25FieldConfig *fields,
          uint32 field_count, int depth, int *nleaves, int *nnodes)
{
    JsonbIterator      *it;
    JsonbIteratorToken   tok;
    JsonbValue           key;
    JsonbValue           val;
    char                *keystr;
    JsonbContainer       *args;
    BM25Query            *node;

    if (depth > BM25_QUERY_MAX_DEPTH)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: query tree nested too deep (max %d)",
                        BM25_QUERY_MAX_DEPTH)));

    /* Every node passes through here, leaf or interior, so this one counter
     * bounds width as the depth check above bounds shape (#305 QUERY-03; see
     * BM25_QUERY_MAX_NODES). Counted before anything is allocated for the
     * node, as the leaf cap is (ADR 0099). */
    if (++(*nnodes) > BM25_QUERY_MAX_NODES)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: query has more than %d nodes (leaf, boolean and boost nodes combined)",
                        BM25_QUERY_MAX_NODES)));

    if (!JsonContainerIsObject(obj))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: malformed query node (expected a jsonb object)")));

    it  = JsonbIteratorInit(obj);
    tok = JsonbIteratorNext(&it, &key, false);
    Assert(tok == WJB_BEGIN_OBJECT);   /* checked: the JsonContainerIsObject gate above */

    tok = JsonbIteratorNext(&it, &key, false);
    if (tok != WJB_KEY)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: malformed query node (expected exactly one key)")));
    keystr = pnstrdup(key.val.string.val, key.val.string.len);

    /* skipNested=true: a nested object/array value comes back as ONE
     * WJB_VALUE of type jbvBinary (a shim over the raw container bytes)
     * instead of the iterator recursing into it itself -- exactly the shape
     * every known node's "args" is expected to be. */
    tok = JsonbIteratorNext(&it, &val, true);
    if (tok != WJB_VALUE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: malformed query node \"%s\"", keystr)));

    tok = JsonbIteratorNext(&it, &key, false);
    if (tok != WJB_END_OBJECT)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: query node \"%s\" must have exactly one key", keystr)));

    if (val.type != jbvBinary || !JsonContainerIsObject(val.val.binary.data))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: query node \"%s\" value must be an object", keystr)));
    args = val.val.binary.data;

    if (strcmp(keystr, "match") == 0)
        node = parse_leaf(args, BM25Q_MATCH, fields, field_count, nleaves);
    else if (strcmp(keystr, "term") == 0)
        node = parse_leaf(args, BM25Q_TERM, fields, field_count, nleaves);
    else if (strcmp(keystr, "phrase") == 0)
        node = parse_leaf(args, BM25Q_PHRASE, fields, field_count, nleaves);
    else if (strcmp(keystr, "wildcard") == 0)
        node = parse_leaf(args, BM25Q_WILDCARD, fields, field_count, nleaves);
    else if (strcmp(keystr, "boolean") == 0)
        node = parse_boolean(args, fields, field_count, depth, nleaves, nnodes);
    else if (strcmp(keystr, "boost") == 0)
        node = parse_boost(args, fields, field_count, depth, nleaves, nnodes);
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: unknown query node \"%s\"", keystr)));

    return node;
}

/* parse_field_id -- resolve the optional "field" key. Absent/null -> the same
 * BM25_FIELD_ALL sentinel a bare (colon-less) "field:term" RHS uses; a string
 * resolves through the SAME bm25_field_by_name the text-RHS path uses, so an
 * unknown field name errors identically on both surfaces. */
static int32
parse_field_id(JsonbContainer *args, const BM25FieldConfig *fields, uint32 field_count)
{
    JsonbValue  jv;
    JsonbValue *found = getKeyJsonValueFromContainer(args, "field", 5, &jv);
    int32       field_id;
    bool        ok;

    if (found == NULL || jv.type == jbvNull)
        return BM25_FIELD_ALL;

    if (jv.type != jbvString)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"field\" must be a string or null")));

    /* Structural-only mode (bm25_query_validate, #245): there is no index, so
     * no field config to resolve against. The name's TYPE was checked above;
     * whether it names a real column is a property of an index, not of the
     * tree, so it is left unjudged rather than guessed at. */
    if (fields == NULL)
        return BM25_FIELD_ALL;

    field_id = bm25_field_by_name(fields, field_count,
                                  jv.val.string.val, jv.val.string.len, &ok);
    if (!ok)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("bm25: unknown search field \"%.*s\"",
                        jv.val.string.len, jv.val.string.val)));
    return field_id;
}

static void
get_required_string(JsonbContainer *args, const char *key, const char *nodename,
                    char **out_text, int *out_len)
{
    JsonbValue  jv;
    JsonbValue *found = getKeyJsonValueFromContainer(args, key, (int) strlen(key), &jv);

    if (found == NULL || jv.type != jbvString)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"%s\" node requires a string \"%s\"", nodename, key)));

    *out_text = pnstrdup(jv.val.string.val, jv.val.string.len);
    *out_len  = jv.val.string.len;
}

static int
get_optional_int(JsonbContainer *args, const char *key, int dflt)
{
    JsonbValue  jv;
    JsonbValue *found = getKeyJsonValueFromContainer(args, key, (int) strlen(key), &jv);

    if (found == NULL || jv.type == jbvNull)
        return dflt;
    if (jv.type != jbvNumeric)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"%s\" must be a number", key)));

    /* numeric_int4 ROUNDS, so 1.7 used to run as 2 and -0.4 as 0 (slipping
     * past the caller's "< 0" test): a widened proximity window nobody wrote
     * (#304 QUERY-05). Compare the VALUE with its truncation rather than test
     * the display scale, so 2.0 and 1e0 -- integral, with nonzero scale or an
     * exponent -- stay legal. The builder's int argument never produces a
     * fraction; only raw jsonb can. */
    if (!DatumGetBool(DirectFunctionCall2(numeric_eq,
                                          NumericGetDatum(jv.val.numeric),
                                          DirectFunctionCall2(numeric_trunc,
                                                              NumericGetDatum(jv.val.numeric),
                                                              Int32GetDatum(0)))))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"%s\" must be an integer", key)));
    return DatumGetInt32(DirectFunctionCall1(numeric_int4, NumericGetDatum(jv.val.numeric)));
}

static bool
get_optional_bool(JsonbContainer *args, const char *key, bool dflt)
{
    JsonbValue  jv;
    JsonbValue *found = getKeyJsonValueFromContainer(args, key, (int) strlen(key), &jv);

    if (found == NULL || jv.type == jbvNull)
        return dflt;
    if (jv.type != jbvBool)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"%s\" must be a boolean", key)));
    return jv.val.boolean;
}

/*
 * validate_wildcard_pattern -- the D9 guardrail, checked at PARSE time so it
 * fires identically whether the tree reaches the scan via @@@ (filter) or &@@
 * (ranked) -- neither path can skip it, unlike a check living in the scorer's
 * wildcard-expansion step (which only the ranked path would necessarily have run
 * before Task 8 wired the filter path through the same tree; both now funnel
 * through bm25_rescan_parse_jsonb). The expansion
 * COUNT cap (bm25_wildcard_max_expansions) is a separate, later check: it
 * depends on the dict contents, not just the pattern text, so it belongs in
 * the expander, not here.
 *
 * Maintenance-interrupts pass: length and star-count caps added. bm25_glob_match
 * (below) is O(pattern * term) worst case, and neither dimension of that product
 * was bounded here -- min_prefix only bounds how much of the pattern must be a
 * literal prefix, not the pattern's total length or how many '*' it contains, so
 * "aaa" + 500000 more "a*" pairs would have sailed through unbounded. Both new
 * checks are PGC_SUSET (bm25_wildcard_max_pattern_length /
 * bm25_wildcard_max_stars, bm25_handler.c), the same class as min_prefix and
 * max_expansions per ADR 0025.
 */
static void
validate_wildcard_pattern(const char *pattern, int patlen)
{
    const char *star;
    const char *p;
    int         prefixlen;
    int         nstars;

    /* Length first, and before any scan of the buffer: bm25_glob_match's cost
     * scales with pattern length regardless of content, so this must run before
     * the memchr/star-count scans below touch the whole pattern. */
    /* This is the RAW-byte check, and it is an approximation of the bound that matters
     * (QRY-06): the matcher runs on the case-FOLDED pattern, whose length can differ in
     * either direction. Kept here because it is cheap and rejects the obvious cases
     * before any per-byte scan; the authoritative re-check against the folded pattern
     * lives in bm25_dict_expand_wildcard, where the fold has actually happened. */
    if (patlen > bm25_wildcard_max_pattern_length)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: wildcard pattern length %d exceeds bm25_native.wildcard_max_pattern_length (%d)",
                        patlen, bm25_wildcard_max_pattern_length)));

    star = memchr(pattern, '*', (size_t) patlen);

    if (star == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: wildcard pattern \"%.*s\" has no '*'",
                        patlen, pattern)));

    prefixlen = (int) (star - pattern);
    if (prefixlen < bm25_wildcard_min_prefix)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: wildcard prefix too short (min %d)",
                        bm25_wildcard_min_prefix)));

    /* Star count over the WHOLE pattern, not just after the prefix: each '*' is
     * an independent backtrack point for bm25_glob_match, and a run like
     * "a*a*a*...a" is exactly the worst-case shape its header comment describes.
     * Bounded by patlen (already capped above, max 8192), so this loop needs no
     * interrupt check of its own -- same reasoning ADR 0024 applies to any loop
     * bounded by a small compile-time-like constant. */
    nstars = 0;
    for (p = pattern; p < pattern + patlen; p++)
        if (*p == '*')
            nstars++;
    if (nstars > bm25_wildcard_max_stars)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: wildcard pattern has %d '*' wildcards, maximum is %d (bm25_native.wildcard_max_stars)",
                        nstars, bm25_wildcard_max_stars)));
}

static BM25Query *
parse_leaf(JsonbContainer *args, BM25QKind kind,
          const BM25FieldConfig *fields, uint32 field_count, int *nleaves)
{
    BM25Query *n;

    /* Every leaf in the tree is constructed here, so this is the single point at
     * which the leaf cap can be applied DURING parse rather than after it. */
    bm25_query_count_leaf(nleaves);

    /* Closed key set before any key is read, so an unknown key is reported
     * ahead of, say, a field-resolution error on the same node. */
    switch (kind)
    {
        case BM25Q_MATCH:
            check_node_keys(args, "match", match_keys);
            break;
        case BM25Q_TERM:
            check_node_keys(args, "term", term_keys);
            break;
        case BM25Q_PHRASE:
            check_node_keys(args, "phrase", phrase_keys);
            break;
        case BM25Q_WILDCARD:
            check_node_keys(args, "wildcard", wildcard_keys);
            break;
        default:
            elog(ERROR, "bm25: parse_leaf called with non-leaf kind %d", (int) kind);
    }

    n = palloc0(sizeof(BM25Query));

    n->kind     = kind;
    n->field_id = parse_field_id(args, fields, field_count);
    n->boost    = 1.0;
    n->leaf_bit = -1;
    n->ordered  = true;

    switch (kind)
    {
        case BM25Q_MATCH:
            get_required_string(args, "terms", "match", &n->text, &n->textlen);
            break;
        case BM25Q_TERM:
            get_required_string(args, "value", "term", &n->text, &n->textlen);
            break;
        case BM25Q_PHRASE:
            get_required_string(args, "phrase", "phrase", &n->text, &n->textlen);
            n->slop    = get_optional_int(args, "slop", 0);
            /* A negative slop threads an impossible max-gap span into the
             * positional recheck (M4), so it silently matches NOTHING rather
             * than erroring or matching everything -- reject at parse, same
             * as the boost<=0 guard above, so every path (debug SRF, @@@,
             * &@@) errors identically instead of one of them going quiet. */
            if (n->slop < 0)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: \"phrase.slop\" must be non-negative (got %d)", n->slop)));
            /* Upper bound, for the same reason the TEXT surface has one
             * (bm25_scan.c's "~N" suffix parser): slop is added to (nterms - 1)
             * and cast to uint32 in both phrase matchers, so a near-INT_MAX value
             * wraps and the span test degenerates to "any co-occurrence". Only the
             * text surface enforced it, which meant the same query was rejected as
             * `"red car"~2147483647` and accepted as bm25_phrase(slop => ...) --
             * one bound, both surfaces (#65.7). */
            if (n->slop > BM25_MAX_PHRASE_SLOP)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: \"phrase.slop\" must not exceed %d (got %d)",
                                BM25_MAX_PHRASE_SLOP, n->slop)));
            n->ordered = get_optional_bool(args, "ordered", true);
            break;
        case BM25Q_WILDCARD:
            get_required_string(args, "pattern", "wildcard", &n->text, &n->textlen);
            validate_wildcard_pattern(n->text, n->textlen);
            break;
        default:
            elog(ERROR, "bm25: parse_leaf called with non-leaf kind %d", (int) kind);
    }
    return n;
}

/*
 * parse_node_array -- collect a "must"/"should"/"must_not" jsonb array of
 * query-node objects into a palloc'd BM25Query* array. Absent key or explicit
 * null -> NULL/0 (an empty array, per the builder's own default). A List is
 * used as scratch (rather than a two-pass count-then-fill over the jsonb
 * iterator, or realloc-growth) purely because it is the least code -- the
 * array is materialized once, after the jsonb walk, in the exact size needed.
 */
static BM25Query **
parse_node_array(JsonbContainer *args, const char *key,
                 const BM25FieldConfig *fields, uint32 field_count,
                 int depth, int *out_n, int *nleaves, int *nnodes)
{
    JsonbValue          jv;
    JsonbValue         *found = getKeyJsonValueFromContainer(args, key, (int) strlen(key), &jv);
    JsonbContainer      *arrc;
    JsonbIterator       *it;
    JsonbIteratorToken   tok;
    JsonbValue           elem;
    List                *list = NIL;
    ListCell            *lc;
    BM25Query          **arr;
    int                  n,
                         i;
    int                  elemno = 0;    /* 1-based, as the caller's SQL array is */

    *out_n = 0;
    if (found == NULL || jv.type == jbvNull)
        return NULL;

    if (jv.type != jbvBinary || !JsonContainerIsArray(jv.val.binary.data))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"%s\" must be a jsonb array", key)));

    arrc = jv.val.binary.data;
    it   = JsonbIteratorInit(arrc);
    tok  = JsonbIteratorNext(&it, &elem, false);
    Assert(tok == WJB_BEGIN_ARRAY);   /* checked: the JsonContainerIsArray gate above */

    for (;;)
    {
        CHECK_FOR_INTERRUPTS();

        tok = JsonbIteratorNext(&it, &elem, true);
        if (tok == WJB_END_ARRAY)
            break;
        elemno++;

        /* A NULL element is what a STRICT builder returns for a NULL argument
         * -- ARRAY[bm25_term(NULL, 'red')] -- so name that cause rather than
         * the generic shape complaint below, which describes a symptom the
         * caller never wrote (#304 QUERY-09; the builders stay STRICT). */
        if (tok == WJB_ELEM && elem.type == jbvNull)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25: \"%s\" array element %d is NULL", key, elemno),
                     errhint("A query builder such as bm25_term returns NULL when any argument is NULL. "
                             "For a leaf over all fields, omit \"field\" in a raw jsonb node, "
                             "e.g. '{\"term\": {\"value\": \"red\"}}'::jsonb.")));
        if (tok != WJB_ELEM || elem.type != jbvBinary ||
            !JsonContainerIsObject(elem.val.binary.data))
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bm25: \"%s\" array element must be a query node object", key)));

        list = lappend(list, parse_node(elem.val.binary.data, fields, field_count, depth + 1,
                                        nleaves, nnodes));
    }

    n = list_length(list);
    *out_n = n;
    if (n == 0)
        return NULL;

    arr = palloc(n * sizeof(BM25Query *));
    i = 0;
    foreach(lc, list)
        arr[i++] = (BM25Query *) lfirst(lc);
    return arr;
}

/* parse_boolean -- D5/D6. An entirely empty bm25_boolean() (no must/should/
 * must_not at all) is NOT an error here -- it is the "empty-match sentinel"
 * the design calls out (matches nothing; bm25_query_eval's BOOLEAN case, below,
 * returns false for it). Only must_not-only (must_not non-empty, must AND
 * should both empty) is rejected -- a query with no positive clause at all
 * would otherwise materialize match-all-minus-excluded, which D6 explicitly
 * disallows as expensive and surprising. */
static BM25Query *
parse_boolean(JsonbContainer *args, const BM25FieldConfig *fields,
             uint32 field_count, int depth, int *nleaves, int *nnodes)
{
    BM25Query *n;

    check_node_keys(args, "boolean", boolean_keys);
    n = palloc0(sizeof(BM25Query));

    n->kind     = BM25Q_BOOLEAN;
    n->field_id = BM25_FIELD_ALL;
    n->boost    = 1.0;
    n->leaf_bit = -1;

    n->must     = parse_node_array(args, "must", fields, field_count, depth, &n->nmust,
                                   nleaves, nnodes);
    n->should   = parse_node_array(args, "should", fields, field_count, depth, &n->nshould,
                                   nleaves, nnodes);
    n->must_not = parse_node_array(args, "must_not", fields, field_count, depth, &n->nmust_not,
                                   nleaves, nnodes);

    if (n->nmust == 0 && n->nshould == 0 && n->nmust_not > 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: query must have at least one positive (must/should) clause")));

    return n;
}

/* parse_boost -- the weight itself is NOT folded into the child here (parsing
 * is a plain top-down walk with no "accumulated boost so far" threaded
 * through it); bm25_query_flatten does the folding in one DFS instead, which
 * is simpler because it is the same walk that already needs to visit every
 * node to assign leaf_bit/negated. */
static BM25Query *
parse_boost(JsonbContainer *args, const BM25FieldConfig *fields,
           uint32 field_count, int depth, int *nleaves, int *nnodes)
{
    BM25Query  *n;
    JsonbValue  jv;
    JsonbValue *found;

    check_node_keys(args, "boost", boost_keys);
    n = palloc0(sizeof(BM25Query));

    n->kind     = BM25Q_BOOST;
    n->field_id = BM25_FIELD_ALL;
    n->leaf_bit = -1;

    found = getKeyJsonValueFromContainer(args, "weight", 6, &jv);
    if (found == NULL || jv.type != jbvNumeric)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"boost.weight\" must be a number")));
    n->boost = DatumGetFloat8(DirectFunctionCall1(numeric_float8, NumericGetDatum(jv.val.numeric)));
    /* boost is a positive score multiplier (1.0 = neutral); boost<=0 makes
     * idf_f fold to 0 (or flip sign), which the scorer/drain treat as "did
     * not match" (bm25_scan_rank.c's acc-drain iterates only accumulator entries,
     * so a doc whose sole positive leaf has idf_f==0 never enters acc at all
     * -- silently dropped even though its presence bit is set). Reject at
     * parse rather than downstream so every path (debug SRF, &@@, @@@) is
     * covered by one check. */
    if (n->boost <= 0.0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"boost.weight\" must be positive (got %g)", n->boost)));

    found = getKeyJsonValueFromContainer(args, "query", 5, &jv);
    if (found == NULL || jv.type != jbvBinary || !JsonContainerIsObject(jv.val.binary.data))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"boost.query\" must be a query node object")));

    n->child = parse_node(jv.val.binary.data, fields, field_count, depth + 1, nleaves, nnodes);
    return n;
}

BM25Query *
bm25_query_parse(Jsonb *jb, const BM25FieldConfig *fields,
                uint32 field_count, MemoryContext cxt)
{
    MemoryContext oldcxt = MemoryContextSwitchTo(cxt);
    BM25Query    *root;
    int           nleaves;
    int           nnodes;

    /* &jb->root is the top JsonbContainer regardless of whether the overall
     * jsonb datum is an object, array, or raw scalar (JB_ROOT_IS_SCALAR wraps
     * a scalar in a 1-element JB_FSCALAR|JB_FARRAY container) -- parse_node's
     * own JsonContainerIsObject check rejects the non-object shapes with the
     * same "malformed query node" message a nested non-object value would
     * get, so there is no separate top-level shape check to keep in sync. */
    nleaves = 0;
    nnodes = 0;
    root = parse_node(&jb->root, fields, field_count, 0, &nleaves, &nnodes);

    MemoryContextSwitchTo(oldcxt);
    return root;
}

/*
 * bm25_query_validate -- the index-free half of bm25_rescan_parse_jsonb's
 * checks (#245), for the &@@ jsonb projection when no scan ranked the query.
 *
 * Runs the same parse + flatten the index path runs at rescan, with a NULL field
 * config (parse_field_id's structural-only mode), so every error parse or flatten
 * raises that does not depend on the index -- malformed nodes, unknown keys, the
 * leaf and node caps, wildcard and slop policy, the folded-boost range -- is
 * raised identically here.
 * Checks the rescan applies after flatten (e.g. the must_not phrase rejection in
 * bm25_rescan_parse_jsonb) are not run. Field names are NOT resolved: an unknown
 * field is only knowable against an index.
 * The tree is discarded; a private context keeps it out of the caller's
 * (typically per-tuple) context and frees it at once on the success path.
 */
void
bm25_query_validate(Jsonb *jb)
{
    MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
                                              "bm25 query validate",
                                              ALLOCSET_SMALL_SIZES);
    BM25Query    *leaves[BM25_QUERY_MAX_LEAVES];
    BM25Query    *root;

    root = bm25_query_parse(jb, NULL, 0, cxt);
    (void) bm25_query_flatten(root, leaves, BM25_QUERY_MAX_LEAVES);
    MemoryContextDelete(cxt);
}

/*
 * flatten_recurse -- the single DFS bm25_query_flatten wraps. boost_acc is the
 * product of every enclosing BOOST weight seen so far on this root->node
 * path; negated_acc is whether any enclosing edge was a must_not child link.
 * Both are threaded DOWN (never back up), so a leaf's final boost/negated is
 * exactly the fold of its own root->leaf path -- independent of sibling
 * subtrees, which is what makes a single top-down pass sufficient (no second
 * pass to "undo" a wrong assumption is ever needed).
 */
static void
flatten_recurse(BM25Query *node, double boost_acc, bool negated_acc,
               BM25Query **leaves, int *n, int max)
{
    int i;

    switch (node->kind)
    {
        case BM25Q_MATCH:
        case BM25Q_TERM:
        case BM25Q_PHRASE:
        case BM25Q_WILDCARD:
            if (*n >= max)
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: query has more than %d leaf clauses (must + should + must_not)",
                                max)));
            /* parse_boost's "weight must be positive" guard is per-WEIGHT; the
             * PRODUCT of several individually-legal weights is not covered by it.
             * 1e-300 * 1e-300 folds to exactly 0.0 -- precisely the state that
             * guard exists to reject, since bm25_scan_rank.c's seg_posting_cb returns
             * early on idf_f == 0.0 BEFORE bm25_scores_add, so the doc never
             * enters the accumulator at all: absent from the result set, not
             * scored zero. The symmetric 1e300 nesting folds to +Inf and threads
             * Inf through every score and WAND bound instead (#65.8).
             *
             * "Finite and > 0" was not enough either: the folded boost is then
             * multiplied by idf (bm25_term_idf), so a single denormal weight still
             * underflowed boost*idf to 0, and one near DBL_MAX overflowed it (#304
             * QUERY-01). The test is therefore a closed range on the folded value,
             * [BM25_QUERY_MIN_BOOST, BM25_QUERY_MAX_BOOST], wide enough for any
             * real weighting and far enough inside float8 that boost*idf and the
             * BM25 sums stay finite and nonzero. Written as a negated range test so
             * that a NaN, which compares false to both bounds, is rejected too.
             *
             * Tested at the LEAF rather than at each multiply: the leaf's product is
             * the only value the scorer reads. An intermediate product that reached
             * 0 or +Inf can never come back into range (0*x == 0, Inf*x == Inf for
             * finite x > 0), so one check covers arbitrarily deep nesting; one that
             * left the range by a finite factor and came back is a legal weight.
             *
             * NOT tested for a negated leaf, whose boost is never read: a must_not
             * leaf marks presence and contributes no score, so bm25_scan_rank.c guards
             * both bm25_pending_score_term and seg_posting_cb with `if (!w->negated)`
             * and reaches presence marking through seg_and_cb, which ignores idf
             * entirely (bm25_term_idf's return value keys off df, not off the
             * boosted idf, so a zero boost does not even make the leaf drop out).
             * Erroring there would reject a query whose behaviour is correct and
             * unchanged. Caught in review of #65.8; sql/83 pins both must_not
             * cases. */
            if (!negated_acc &&
                !(boost_acc >= BM25_QUERY_MIN_BOOST && boost_acc <= BM25_QUERY_MAX_BOOST))
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("bm25: \"boost.weight\" folds to %.15g on a scoring leaf, "
                                "outside the supported range [%g, %g]",
                                boost_acc, BM25_QUERY_MIN_BOOST, BM25_QUERY_MAX_BOOST),
                         errdetail("A leaf's weight is the product of every enclosing \"boost.weight\". "
                                   "Leaves under must_not are exempt, because they contribute no score.")));
            node->boost    = boost_acc;
            node->negated  = negated_acc;
            node->leaf_bit = *n;
            leaves[*n]     = node;
            (*n)++;
            break;

        case BM25Q_BOOST:
            flatten_recurse(node->child, boost_acc * node->boost, negated_acc, leaves, n, max);
            break;

        case BM25Q_BOOLEAN:
            for (i = 0; i < node->nmust; i++)
                flatten_recurse(node->must[i], boost_acc, negated_acc, leaves, n, max);
            for (i = 0; i < node->nshould; i++)
                flatten_recurse(node->should[i], boost_acc, negated_acc, leaves, n, max);
            /* must_not children are ALWAYS negated from here down, regardless
             * of whether this BOOLEAN node itself was already inside a
             * must_not (D11: the flag only ever turns on, propagating to
             * every descendant, positive or not). */
            for (i = 0; i < node->nmust_not; i++)
                flatten_recurse(node->must_not[i], boost_acc, true, leaves, n, max);
            break;
    }
}

int
bm25_query_flatten(BM25Query *root, BM25Query **leaves, int max)
{
    int n = 0;

    flatten_recurse(root, 1.0, false, leaves, &n, max);
    return n;
}

/*
 * bm25_query_eval -- the boolean formula (D5/D11) over a doc's leaf presence
 * bitmask, evaluated once per candidate at drain time. `present` has bit
 * leaf->leaf_bit set iff that leaf matched the doc (positive AND must_not leaves
 * are both represented -- the scorer marks a bit for either kind; only must_not
 * leaves are exempt from scoring, not from presence).
 *
 *   leaf   -- present iff its bit is set. The shift is 64-bit (UINT64CONST(1) <<),
 *            NEVER 1u<< : leaf_bit ranges 0..63 and a 32-bit shift is UB / bit
 *            aliasing above 31. leaf_bit is -1 only on an unflattened leaf; the
 *            scan path always flattens first, so assert the invariant.
 *   BOOST  -- transparent: the weight is a score multiplier (folded into the leaf
 *            by flatten), never a match predicate, so evaluation recurses to the
 *            child unchanged.
 *   BOOLEAN -- must = AND (every must clause must match), must_not = AND-NOT (any
 *            match rejects), should = OR but REQUIRED only when there is no must
 *            (D5: with a must present, shoulds are score-only). An all-empty
 *            boolean (no must/should) matches nothing.
 */
bool
bm25_query_eval(const BM25Query *node, uint64 present)
{
    switch (node->kind)
    {
        case BM25Q_MATCH:
        case BM25Q_TERM:
        case BM25Q_PHRASE:
        case BM25Q_WILDCARD:
            Assert(node->leaf_bit >= 0);   /* checked: bm25_query_flatten assigns the bit */
            return (present & (UINT64CONST(1) << node->leaf_bit)) != 0;

        case BM25Q_BOOST:
            return bm25_query_eval(node->child, present);

        case BM25Q_BOOLEAN:
        {
            int i;

            for (i = 0; i < node->nmust; i++)
                if (!bm25_query_eval(node->must[i], present))
                    return false;
            for (i = 0; i < node->nmust_not; i++)
                if (bm25_query_eval(node->must_not[i], present))
                    return false;
            if (node->nmust == 0)
            {
                /* No must: at least one should must match (D5). An empty boolean
                 * (no should either) matches nothing. */
                if (node->nshould == 0)
                    return false;
                for (i = 0; i < node->nshould; i++)
                    if (bm25_query_eval(node->should[i], present))
                        return true;
                return false;
            }
            return true;        /* every must satisfied; shoulds are score-only */
        }
    }
    return false;                /* unreachable; silences -Wreturn-type */
}

/*
 * bm25_glob_match -- iterative (non-backtracking-recursive) '*'-glob match,
 * the textbook two-pointer algorithm (Wildcard Matching): advance both
 * cursors on a literal-byte match; on a '*', remember the position and try
 * consuming zero bytes first, backtracking (via star_ti) to consume one more
 * term byte under the star each time a later literal mismatches. O(pattern +
 * term) amortized, O(pattern * term) worst case (e.g. "a*a*a*...a" against a
 * run of 'a's) -- fine at the pattern/term lengths a search dictionary entry
 * has. No '?' or character-class support: only '*' is special (D8 scope).
 */
bool
bm25_glob_match(const char *pattern, int patlen, const char *term, int termlen)
{
    int pi = 0,
        ti = 0;
    int star_pi = -1,
        star_ti = -1;

    /* O(pattern * term) worst case (see the header comment above), and reachable
     * with arbitrary-length pattern/term text via bm25_debug_glob_match, which
     * calls this directly with no length cap of its own -- the wildcard query
     * path at least bounds pattern length via validate_wildcard_pattern above,
     * but term length there is only as short as a dictionary entry. Check at the
     * top of the loop whose trip count actually scales (ti, bounded by termlen
     * and the star-driven backtracking), not the small trailing-star skip below. */
    while (ti < termlen)
    {
        CHECK_FOR_INTERRUPTS();

        if (pi < patlen && pattern[pi] == '*')
        {
            star_pi = pi++;
            star_ti = ti;
        }
        else if (pi < patlen && pattern[pi] == term[ti])
        {
            pi++;
            ti++;
        }
        else if (star_pi >= 0)
        {
            pi = star_pi + 1;
            ti = ++star_ti;
        }
        else
            return false;
    }

    while (pi < patlen && pattern[pi] == '*')
        pi++;

    return pi == patlen;
}
