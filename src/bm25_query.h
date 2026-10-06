/* bm25_query.h -- M6 jsonb query tree: parse, flatten, boolean-formula eval, glob.
 *
 * Role in the system: turns a builder-produced jsonb query object (the six
 * bm25_* builder functions in bm25_native--1.0.sql) into an in-memory AST
 * (BM25Query), consumed by bm25_rescan and the exhaustive scorer in
 * bm25_scan_rank.c. The parser IS the injection-safety boundary (D1 of the M6
 * design): a user's term/pattern arrives as a jsonb VALUE under a known key,
 * never as an operator, so there is no string-DSL round-trip to attack --
 * bm25_query_parse only ever asks "what jsonb value is under this key",
 * never "what does this string mean as a query".
 *
 * bm25_query_flatten assigns every leaf (match/term/phrase/wildcard) -- both
 * positive (must/should) AND must_not leaves -- a bit in ONE uint64 presence
 * mask, generalizing the and_presence/PhraseAndEnt.mask mechanism already in
 * bm25_scan_match.c (M4's phrase-fallback AND) to an arbitrary boolean tree.
 * bm25_query_eval reads that mask back through the boolean formula at
 * drain time (D5/D7 of the design).
 *
 * bm25_glob_match is the wildcard matcher wildcard expansion tests dict
 * entries against; it lives here rather than bm25_seg_dict.c because it is a
 * pure byte-matching function with no segment/PG dependency.
 */
#ifndef BM25_QUERY_H
#define BM25_QUERY_H

#include "bm25.h"               /* BM25Query forward-decl typedef, BM25FieldConfig,
                                 * BM25_FIELD_ALL (via bm25_format.h), bm25_field_by_name */
#include "utils/jsonb.h"

/* ALL leaf clauses -- positive (must/should) AND must_not -- share ONE uint64
 * presence mask (matching the existing PhraseAndEnt.mask width, bm25_scan.h).
 * So the cap is on TOTAL leaves, not just positive ones: every leaf_bit
 * assigned by bm25_query_flatten must stay < 64, AND the mask bit for it MUST
 * always be built with a 64-bit shift -- UINT64CONST(1) << leaf_bit, exactly
 * as bm25_query_eval's leaf case does below. NEVER `1u << leaf_bit`: that's a
 * 32-bit shift, undefined behavior for leaf_bit 32..63, and would silently
 * alias two leaves onto the same bit instead of erroring. Any future
 * presence-mask build must copy the 64-bit form: the scan-time mask is the
 * `and_presence` hash of PhraseAndEnt, assembled by phrase_and_mark and read by
 * the boolean/AND-fallback evaluation in bm25_scan_build_ranking_exhaustive
 * (bm25_scan_rank.c). */
#define BM25_QUERY_MAX_LEAVES 64

/* Cap on TOTAL nodes (leaves + boolean/boost interior nodes), enforced in
 * parse_node as each node is entered, beside the depth cap. The leaf cap above
 * does not bound interior nodes: an empty {"boolean":{}} is not a leaf, so a
 * should[] padded with 100000 of them parsed, and bm25_query_eval walked every
 * one for every candidate document (nodes x candidates, #305 QUERY-03). A
 * realistic generated tree is under ~200 nodes. A tree with no leafless subtree
 * exceeds 1024 only by hanging long unary boost/boolean chains (up to the depth
 * cap) over many of its 64 leaves, about 64 x 32 ~ 2000 nodes at most, so 1024
 * rejects padding and synthetic chains, not real queries. Same SQLSTATE
 * as the leaf and depth caps (22023), for the reason given above
 * bm25_query_count_leaf in bm25_query.c. */
#define BM25_QUERY_MAX_NODES 1024

/* Range a SCORING leaf's folded boost (the product of every enclosing
 * boost.weight) must lie in, checked in flatten_recurse (#304 QUERY-01). The
 * folded boost multiplies the leaf's idf (bm25_term_idf); a value near the
 * float8 edges made boost*idf underflow to 0 -- which the scorer reads as "did
 * not match" and silently drops the document -- or overflow to Inf/NaN scores.
 * Checking the product at parse time is deterministic: a check of boost*idf at
 * the use site would make the same query succeed or fail as df drifts. Leaves
 * under must_not are exempt: their boost is never read. */
#define BM25_QUERY_MIN_BOOST 1e-6
#define BM25_QUERY_MAX_BOOST 1e6

typedef enum BM25QKind
{
    BM25Q_MATCH, BM25Q_TERM, BM25Q_PHRASE, BM25Q_WILDCARD,   /* leaves */
    BM25Q_BOOLEAN, BM25Q_BOOST                                /* interior */
} BM25QKind;

struct BM25Query
{
    BM25QKind kind;
    int32     field_id;         /* leaf: BM25_FIELD_ALL or resolved id; unused
                                 * (BM25_FIELD_ALL) on interior nodes */

    /* Leaf payload -- analyzed lazily by the scorer, except the WILDCARD
     * pattern, which is raw (the scorer/expander lowercases it but does NOT
     * stem it, D8). NULL/0 on interior (BOOLEAN/BOOST) nodes. */
    char     *text;             /* MATCH terms / TERM value / WILDCARD pattern / PHRASE text */
    int       textlen;
    int       slop;              /* PHRASE only; 0 otherwise */
    bool      ordered;           /* PHRASE only, from the jsonb "ordered" key (default
                                  * true). parse_leaf defaults every OTHER leaf kind
                                  * (match/term/wildcard) to true too, but parse_boolean
                                  * and parse_boost's palloc0 leave it false on interior
                                  * nodes -- NOT true as on a leaf. Meaningless outside
                                  * PHRASE either way. */

    /* A BOOST node's OWN weight as parsed; flatten leaves it unchanged. For a
     * LEAF, once bm25_query_flatten has run, this holds the PRODUCT of every
     * enclosing BOOST weight on the root->leaf path, folded top-down (1.0 with
     * no enclosing BOOST); before flatten it is the parse-time default 1.0. A
     * BOOLEAN node keeps 1.0 throughout: flatten writes only leaves. */
    double    boost;

    int       leaf_bit;          /* leaves: assigned 0.. by bm25_query_flatten,
                                  * one counter over ALL leaves; interior
                                  * nodes: always -1. -1 on a leaf before
                                  * flatten runs (not yet assigned). */
    bool      negated;           /* leaf: true if any root->leaf path edge is a
                                  * must_not (presence-only, NO score -- D11).
                                  * Meaningless before flatten runs. */

    /* BOOLEAN children (must=AND, should=OR-unless-must-present,
     * must_not=AND-NOT; D5). NULL/0 when the array key was absent. */
    BM25Query **must;
    int         nmust;
    BM25Query **should;
    int         nshould;
    BM25Query **must_not;
    int         nmust_not;

    /* BOOST child (NULL on every other node kind). */
    BM25Query  *child;
};

/* Parse a jsonb query tree; resolve field names to ids against the index's
 * baked field config (via bm25_field_by_name); validate the tree, raising
 * ereport(ERROR) on:
 *   - a node that is not a single-key {"<kind>": {...}} object, whose key is
 *     not one of match/term/phrase/wildcard/boolean/boost, or whose value is
 *     not itself an object. parse_node raises this one bullet through SEVERAL
 *     distinct messages -- 'malformed query node' (three variants), 'query node
 *     "%s" must have exactly one key', 'query node "%s" value must be an object'
 *     and 'unknown query node "%s"' -- so a test should match the errcode
 *     (ERRCODE_INVALID_PARAMETER_VALUE), not the text;
 *   - a key in a node's args object outside that node kind's closed set
 *     (#304 QUERY-02): match {field, terms}, term {field, value}, phrase
 *     {field, phrase, slop, ordered}, wildcard {field, pattern}, boolean
 *     {must, should, must_not}, boost {weight, query}. A typo such as
 *     "mustnot" used to be ignored, silently dropping the exclusion; a "field"
 *     on a boolean or boost is rejected too, since it is not inherited;
 *   - a "field" that is present but neither a string nor null, or that names
 *     a column the index doesn't have ("unknown search field");
 *   - a leaf's required text field (match.terms / term.value / phrase.phrase /
 *     wildcard.pattern) missing or not a string (an explicit null is NOT a
 *     string, so it errors -- unlike the optional keys below);
 *   - a wildcard pattern with no '*', a literal prefix shorter than
 *     bm25_wildcard_min_prefix, or more '*'s than bm25_wildcard_max_stars --
 *     checked HERE (parse time) so the guard is path-independent (fires
 *     identically on @@@ and &@@). bm25_wildcard_max_pattern_length is applied
 *     here TOO, but only as a RAW-byte approximation: the matcher runs on the
 *     case-FOLDED pattern, and the authoritative re-check lives in
 *     bm25_dict_expand_wildcard, where the fold has happened (QRY-06). The
 *     expansion COUNT cap (bm25_wildcard_max_expansions) is not parse-time at
 *     all -- it depends on dict contents;
 *   - a phrase "slop" present and not a number; a number with a fractional
 *     part (1.7, -0.4: numeric_int4 would round them, #304 QUERY-05; 2.0 and
 *     1e0 are integral and accepted); integral but outside int32,
 *     which surfaces as PostgreSQL's own "integer out of range" from the
 *     numeric_int4 cast rather than a bm25 message; or in int32 range but
 *     outside [0, BM25_MAX_PHRASE_SLOP];
 *   - a phrase "ordered" present and neither a boolean nor null. An explicit
 *     JSON null is ACCEPTED and takes the default (true). That is the same hole
 *     "field" and "slop" have: parse_field_id, get_optional_int and
 *     get_optional_bool all read jbvNull as "absent";
 *   - a boolean's must/should/must_not present and neither a jsonb array nor
 *     null (an explicit null reads as absent, i.e. an empty clause list -- so
 *     {"must": null, "must_not": [...]} trips the must_not-only rule below
 *     rather than being accepted), or an array whose elements aren't themselves
 *     query node objects. A NULL element gets its own message naming its
 *     position, since it is what a STRICT builder handed a NULL argument
 *     produces (#304 QUERY-09);
 *   - a must_not-only boolean (no must/should) -- D6. An entirely empty boolean
 *     is NOT an error: it is the empty-match sentinel;
 *   - a boost "weight" missing or non-numeric; numeric but outside float8 range,
 *     which -- the same shape the "slop" bullet above has for numeric_int4 --
 *     surfaces as PostgreSQL's own "is out of range for type double precision"
 *     from the numeric_float8 cast rather than a bm25 message. That cast runs
 *     BEFORE the positivity test, so it wins for any out-of-range weight, and it
 *     rejects an UNDERFLOWING magnitude (1e-400) as well as an overflowing one
 *     (1e400); or in float8 range and not strictly positive (boost <= 0 would
 *     otherwise silently drop a matching doc downstream instead of erroring);
 *   - a boost "query" missing or not a query node object;
 *   - more than BM25_QUERY_MAX_LEAVES leaf clauses total (must + should +
 *     must_not), enforced as each leaf is parsed rather than after the whole
 *     tree is built (#68);
 *   - more than BM25_QUERY_MAX_NODES nodes total, counted as each node is
 *     entered (#305 QUERY-03);
 *   - recursion nested past a fixed depth cap (defends parse/eval cost).
 * The folded-boost range (BM25_QUERY_MIN_BOOST..MAX_BOOST) is NOT checked here:
 * it needs the root->leaf product and the must_not flag, which only
 * bm25_query_flatten computes; every query-path caller runs both.
 * fields == NULL selects STRUCTURAL-ONLY mode (bm25_query_validate): a
 * "field" is still type-checked but never resolved, so "unknown search field"
 * cannot fire and every leaf gets BM25_FIELD_ALL. Index callers always pass a
 * real array (possibly with field_count 0), never NULL.
 * Allocates the WHOLE tree (nodes, text, arrays) in cxt -- switches
 * CurrentMemoryContext there for the duration of the parse and switches back
 * before returning (or not at all, on the ERROR paths above; PostgreSQL's
 * normal error unwind resets CurrentMemoryContext for the caller). */
extern BM25Query *bm25_query_parse(Jsonb *jb, const BM25FieldConfig *fields,
                                   uint32 field_count, MemoryContext cxt);

/* Flatten to the leaf array in a single DFS, assigning each leaf->leaf_bit
 * from ONE 0.. counter over ALL leaves (positive AND must_not), folding
 * enclosing BOOST weights into leaf->boost (product, top-down), and setting
 * leaf->negated on any leaf whose root->leaf path crosses a must_not edge (the
 * flag propagates to every leaf below that edge, at any further depth -- a
 * leaf nested inside a must_not's own must[] is still presence-only). Returns
 * the TOTAL leaf count; ereport(ERROR) if it would exceed BM25_QUERY_MAX_LEAVES
 * (64) -- so `UINT64CONST(1) << leaf_bit` is always in range (no UB, no
 * positive/must_not bit aliasing) -- or if a non-negated leaf's folded boost
 * lies outside [BM25_QUERY_MIN_BOOST, BM25_QUERY_MAX_BOOST]. `leaves` must
 * have room for `max` entries; callers pass max == BM25_QUERY_MAX_LEAVES. */
extern int bm25_query_flatten(BM25Query *root, BM25Query **leaves, int max);

/* Parse + flatten jb with no index (fields == NULL above) and discard the
 * result: raises exactly the errors those two raise that do not depend on an
 * index's field config, and returns normally otherwise. Used by the &@@ jsonb
 * projection when no scored scan ranked its query (#245). */
extern void bm25_query_validate(Jsonb *jb);

/* Evaluate the boolean formula over a doc's leaf presence bitmask (both
 * positive and must_not leaves are represented by their bit in `present`,
 * D5/D11): must = AND, must_not = AND-NOT, should = OR (required only when no
 * must). The leaf case tests `present & (UINT64CONST(1) << leaf_bit)` (64-bit
 * shift; leaf_bit must be >= 0, i.e. the tree must be flattened first). See the
 * definition in bm25_query.c. */
extern bool bm25_query_eval(const BM25Query *node, uint64 present);

/* fnmatch-lite: does `term` match `pattern` (only '*' is special, may appear
 * more than once, matches zero or more bytes)? A pure byte comparator --
 * lowercasing is the caller's job (both sides are expected already
 * lowercased, matching the dict's stored case convention). */
extern bool bm25_glob_match(const char *pattern, int patlen,
                            const char *term, int termlen);

#endif                          /* BM25_QUERY_H */
