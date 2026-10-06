/*
 * bm25_snippet.c -- bm25_snippet(field, start_tag, end_tag, max_num_chars): the
 * query-term highlighter (M4 C-SNIPPET, D8/D10/D11).
 *
 * ROLE IN THE SYSTEM: at projection time, given the column VALUE of a matched row,
 * return a short excerpt of that text with each query-term occurrence wrapped in
 * <mark>...</mark> (configurable tags). This is the ONLY consumer that maps stemmed
 * query terms back onto original-cased text.
 *
 * WHY RE-ANALYZE, NOT READ POSITIONS (D8): the stored POS chain holds post-stemming
 * ordinals with NO retained mapping to original character offsets, so it cannot drive
 * highlighting. Instead we re-run the index's analyzer over the passed text and use the
 * per-token src_off/src_len (D10 -- the original byte span of each surface run) to place
 * the tags. Highlight parity with the scorer: the query terms come from bm25_analyze under
 * the scan's own cached qcfg -- over so->qterm on the text path, the same call the scorer
 * makes, and over each non-negated leaf's text on the jsonb path, which is what the
 * exhaustive scorer analyzes per leaf -- and the field is analyzed with that SAME qcfg, so
 * a term stems identically on both sides. The query side is built once per scan, on the
 * first call, and cached on the scan (so->qtoks / so->qwild, TEXT-12 #153), because the
 * query is scan-constant while this function runs per projected row. Only the FIELD text,
 * which differs per row, is analyzed on every call.
 *
 * QUERY-TREE SCOPE (#308 TEXT-04, decision D17): any jsonb tree highlights, not only a
 * single MATCH/TERM leaf. See snippet_build_query_set for what each leaf contributes and
 * where the approximation to the scorer's matcher lies.
 *
 * SCAN-RESIDENT CONTRACT: sources the query terms + analyzer config from the active
 * scored scan registry via bm25_sole_scored_scan(), which ERRORs if another registered
 * scan could still be genuinely concurrent (unlike bm25_score(ctid)/bm25_score_key,
 * snippet carries no row identity to pick the right one). Outside a running bm25 index
 * scan there is no registered scan and this returns SQL NULL -- the same documented
 * "only works under the bm25 index scan" limitation as the other accessors.
 *
 * NULL semantics (all return SQL NULL, matching D11 / the drop-NULL-snippets convention):
 * NULL field; NULL max_num_chars; no active scan; empty query; ZERO hits in the field.
 * max_num_chars <= 0 is a trust-boundary ERROR (a caller-supplied budget must be
 * positive). A NULL start_tag/end_tag takes the '<mark>'/'</mark>' default and a NULL
 * escape takes true, so those never yield NULL.
 *
 * ESCAPING (ADR 0048): the field text is HTML/XML-escaped by DEFAULT -- the `escape`
 * argument defaults to true. The defaults frame the excerpt in markup ('<mark>'), so the
 * documented way to consume this function is to render it as HTML, and markup stored in
 * the indexed column would otherwise reach the browser verbatim. This DEPARTS from
 * ts_headline, which escapes nothing; the divergence is deliberate. The caller's TAGS are
 * never escaped -- they are markup by contract, and escaping them would turn '<mark>' into
 * visible text -- so tags remain trusted input and a caller passing user-controlled tags is
 * still injecting. `escape => false` restores byte-verbatim output for non-HTML consumers.
 *
 * BUDGET UNITS (ADR 0049): max_num_chars counts CHARACTERS of the ORIGINAL field text --
 * not bytes, and not output length. Two consequences worth stating: an escaped '&' spends
 * one unit of budget while emitting five bytes, so `escape` cannot change WHICH window is
 * selected (only how it renders); and the tags and ellipses have never counted.
 *
 * SERVER-ENCODING SAFETY (ADR 0050): every byte this function emits either came from the
 * user's own text (already valid in the server encoding) or from the caller's tags --
 * except the truncation ellipsis, the one non-ASCII literal the extension writes itself.
 * It is converted through the server's own encoding machinery rather than hardcoded; see
 * snippet_append_ellipsis.
 *
 * UTF-8 SAFETY: hit spans are analyzer word-run edges, and a run edge is always a
 * character boundary because bm25_analyze advances by whole characters (bm25_next_char).
 * That conclusion is unchanged from the ASCII-only tokenizer but the REASON inverted: a
 * multibyte character used to be a separator and so could never be inside a run, whereas
 * now it is a word character and routinely is -- the edges are safe because the split
 * measures characters, not because runs are ASCII (ADR 0046). Only the context-EXPANSION
 * step can land mid-character; snap_left/snap_right walk off any continuation byte
 * (the 10xxxxxx test, applied only in a multibyte encoding) so a snapped edge is always
 * a char boundary.
 */

#include "postgres.h"

#include "bm25.h"
#include "bm25_query.h"         /* BM25Query, bm25_glob_match: the TEXT-04 hit set */
#include "mb/pg_wchar.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"     /* get_rel_name: #301 refusal message */

/*
 * A highlighted span in the ORIGINAL field text: [start, end) byte offsets, plus the
 * same edges as CHARACTER offsets (cstart/cend, filled by snippet_hit_charpos).
 *
 * Both denominations are carried because the two jobs need different ones: max_num_chars
 * is a character budget, while every read of the text -- appendBinaryStringInfo, the
 * word-boundary snapping -- indexes bytes. Deriving one from the other on demand would be
 * a rescan of the field per comparison inside an O(nhits^2) window loop.
 *
 * Hits are produced in analyzer token order over non-overlapping source runs, so BOTH
 * edges are monotonically non-decreasing across the array -- the property snippet_hit_charpos
 * relies on to resolve every edge in a single forward pass.
 */
typedef struct SnippetHit
{
    int start;
    int end;
    int cstart;
    int cend;
} SnippetHit;

/*
 * snippet_hit_charpos -- fill in every hit's cstart/cend in ONE forward pass over the field,
 * and return the field's total character count.
 *
 * Walks bytes and counts characters with bm25_mblen_bounded, recording the running
 * character count when the byte cursor reaches each hit edge, then finishes the walk to the
 * end of the field: the window code needs the room on the RIGHT of the core in characters
 * too (TEXT-06), and continuing this walk is the cheapest place to get it. O(flen + nhits)
 * rather than the O(flen*nhits) a per-edge pg_mbstrlen_with_len would cost.
 *
 * Correct only because hit edges are non-decreasing (see SnippetHit). If that were ever
 * violated the affected edge would receive the previous edge's count -- a slightly wrong
 * budget, never an out-of-range read -- because the inner loops only ever advance.
 *
 * In a single-byte encoding the character length is identically 1, so cstart==start and
 * cend==end and every character computation below collapses to byte arithmetic. That is
 * also why the ASCII regression corpora are unaffected by the unit: for ASCII text under
 * any server encoding, characters and bytes coincide.
 */
static int
snippet_hit_charpos(const char *text, int textlen, SnippetHit *hits, int nhits)
{
    int b = 0;                  /* byte cursor */
    int c = 0;                  /* characters strictly before byte b */
    int h;

    for (h = 0; h < nhits; h++)
    {
        while (b < hits[h].start && b < textlen)
        {
            b += bm25_mblen_bounded(text + b, textlen - b);
            c++;
        }
        hits[h].cstart = c;
        while (b < hits[h].end && b < textlen)
        {
            b += bm25_mblen_bounded(text + b, textlen - b);
            c++;
        }
        hits[h].cend = c;
    }
    while (b < textlen)
    {
        b += bm25_mblen_bounded(text + b, textlen - b);
        c++;
    }
    return c;
}

/*
 * snippet_char_to_byte -- byte offset of character index `target`, walking FORWARD from the
 * (anchor_byte, anchor_char) pair, which the caller must have obtained together.
 *
 * Forward-only by necessity: bm25_mblen_bounded reads a LEAD byte and reports the character's length,
 * so it cannot step backward, and no general server encoding supports a reliable backward
 * step (the continuation-byte test in snap_left/snap_right is UTF-8-specific and applies
 * only to nudging an already-computed edge). A target at or left of the anchor therefore
 * restarts from byte 0 -- the one other boundary known for free.
 *
 * Returns a byte offset clamped to [0, textlen] that always lands on a character boundary.
 */
static int
snippet_char_to_byte(const char *text, int textlen,
                     int anchor_byte, int anchor_char, int target)
{
    int b = anchor_byte;
    int c = anchor_char;

    if (target < anchor_char)
    {
        b = 0;
        c = 0;
    }
    while (c < target && b < textlen)
    {
        b += bm25_mblen_bounded(text + b, textlen - b);
        c++;
    }
    return (b > textlen) ? textlen : b;
}

/*
 * snippet_append_text -- copy [p, p+len) of the ORIGINAL field into `out`, HTML/XML-escaping
 * the five metacharacters when `escape` is set. Runs between metacharacters are copied in
 * one block, so unescaped text costs a single memcpy as before.
 *
 * The five are the htmlspecialchars(ENT_QUOTES) set: escaping the two quote characters as
 * well as the angle brackets and ampersand is what makes the result safe inside a quoted
 * ATTRIBUTE, not merely as element content. &#39; rather than &apos; because the latter is
 * XML/HTML5 and not defined in HTML 4.
 *
 * A BYTE-wise scan is correct here even though ADR 0046 established that the tokenizer must
 * work in characters: PostgreSQL requires every SERVER encoding to be ASCII-transparent --
 * no byte of a multibyte character may fall in the ASCII range -- so none of these five can
 * appear inside one. (The encodings that break that rule, SJIS/BIG5/GBK, are client-only for
 * exactly this reason, and their trail bytes start at 0x40 in any case, above all five.)
 * The comparisons are against a possibly-signed char, which is fine for the same reason:
 * every value tested is below 0x80.
 */
static void
snippet_append_text(StringInfo out, const char *p, int len, bool escape)
{
    int i;
    int flush = 0;

    if (!escape)
    {
        appendBinaryStringInfo(out, p, len);
        return;
    }

    for (i = 0; i < len; i++)
    {
        const char *ent;

        switch (p[i])
        {
            case '&':
                ent = "&amp;";
                break;
            case '<':
                ent = "&lt;";
                break;
            case '>':
                ent = "&gt;";
                break;
            case '"':
                ent = "&quot;";
                break;
            case '\'':
                ent = "&#39;";
                break;
            default:
                continue;
        }
        if (i > flush)
            appendBinaryStringInfo(out, p + flush, i - flush);
        appendStringInfoString(out, ent);
        flush = i + 1;
    }
    if (len > flush)
        appendBinaryStringInfo(out, p + flush, len - flush);
}

/*
 * Query hit set under construction (snippet_build_query_set). toks grows by repalloc as
 * leaves are analyzed; wild never exceeds BM25_QUERY_MAX_LEAVES, the parser's cap on
 * leaves of every kind, so it is allocated at that size up front.
 */
typedef struct SnippetQuerySet
{
    BM25Token  *toks;
    int         ntoks;
    int         captoks;
    BM25Token  *wild;
    int         nwild;
} SnippetQuerySet;

static void
snippet_add_tokens(SnippetQuerySet *qs, const BM25Token *t, int n)
{
    if (n <= 0)
        return;
    if (qs->ntoks + n > qs->captoks)
    {
        int newcap = Max(qs->captoks * 2, qs->ntoks + n);

        qs->toks = (qs->toks == NULL)
            ? (BM25Token *) palloc(sizeof(BM25Token) * newcap)
            : (BM25Token *) repalloc(qs->toks, sizeof(BM25Token) * newcap);
        qs->captoks = newcap;
    }
    memcpy(qs->toks + qs->ntoks, t, sizeof(BM25Token) * n);
    qs->ntoks += n;
}

/*
 * snippet_add_leaves -- add every NON-NEGATED leaf under `node` to the hit set.
 *
 * A must_not subtree is skipped whole: bm25_query_flatten marks every leaf below a
 * must_not edge negated, at any depth, so skipping the edge is the same test as reading
 * leaf->negated, without depending on flatten having run. Highlighting a term the query
 * EXCLUDES would mark text the row was returned despite, not because of.
 *
 * MATCH, TERM and PHRASE text goes through bm25_analyze under the scan's qcfg, the call
 * the exhaustive scorer makes per leaf (bm25_scan_rank.c), so each stems identically.
 * A phrase contributes its terms wherever they occur in the field, not only the
 * occurrences that form the phrase -- the same rule the text-syntax "..." path has always
 * had, since it too reaches here as the analyzed terms of so->qterm.
 *
 * WILDCARD keeps its pattern, folded by bm25_fold_term exactly as
 * bm25_dict_expand_wildcard folds it, and snippet_is_hit globs it against each FIELD
 * token. That stands in for the scan's dictionary expansion without capturing it (D17:
 * bm25_scan_rank.c is unchanged): the field is analyzed with the index's own analyzer,
 * so its tokens are the dictionary bytes the row was indexed under, and a glob match on
 * one is membership in the expansion for any row the scan returned. (The expansion cap
 * cannot make them differ: exceeding it ERRORs the scan rather than truncating.)
 *
 * Field scope is ignored throughout, as for a text-syntax field:term query: this function
 * is handed a VALUE, not a column, so it cannot tell which field a leaf was scoped to.
 * Depth is bounded by the parser's BM25_QUERY_MAX_DEPTH.
 *
 * Residuals (#308, deliberately left): re-analysis approximates the scan's matcher rather
 * than replaying it. (1) Field scope is ignored. (2) A phrase marks its terms outside the
 * phrase too. (3) A text that was never indexed (a literal, another column) is matched
 * against the query terms and patterns as given, with no dictionary to consult, so a
 * wildcard can mark a word the expansion never contained.
 */
static void
snippet_add_leaves(BM25ScanOpaque so, const BM25Query *node, SnippetQuerySet *qs)
{
    int i;

    switch (node->kind)
    {
        case BM25Q_MATCH:
        case BM25Q_TERM:
        case BM25Q_PHRASE:
            if (node->textlen > 0)
            {
                BM25Token *t;
                int        n = bm25_analyze(&so->qcfg, node->text, node->textlen, &t);

                snippet_add_tokens(qs, t, n);
            }
            break;
        case BM25Q_WILDCARD:
            {
                int foldlen;

                Assert(qs->nwild < BM25_QUERY_MAX_LEAVES);  /* checked: parse_leaf caps total leaves */
                qs->wild[qs->nwild].ptr = bm25_fold_term(node->text, node->textlen,
                                                         &foldlen);
                qs->wild[qs->nwild].len = foldlen;
                qs->nwild++;
            }
            break;
        case BM25Q_BOOLEAN:
            for (i = 0; i < node->nmust; i++)
                snippet_add_leaves(so, node->must[i], qs);
            for (i = 0; i < node->nshould; i++)
                snippet_add_leaves(so, node->should[i], qs);
            /* must_not: skipped, see above */
            break;
        case BM25Q_BOOST:
            snippet_add_leaves(so, node->child, qs);
            break;
    }
}

/*
 * snippet_build_query_set -- fill so->qtoks/nqtoks/qwild/nqwild for this scan.
 *
 * The jsonb path walks so->qtree whatever its root (TEXT-04): until #308 only a single
 * MATCH/TERM root reached so->qterm, so a PHRASE, WILDCARD, BOOLEAN or BOOST root
 * returned NULL on every row -- the answer that also means "no hit". The text path, and
 * nothing else, still reads so->qterm. A single MATCH/TERM jsonb root is ALSO copied into
 * so->qterm, and walking the tree analyzes the same bytes, so the two agree.
 *
 * Sorted with bm25_token_cmp so the per-field-token probe is a bsearch (XCUT-06 #305):
 * a plain-text query has no term cap, and the linear probe this replaced made the hit loop
 * O(field tokens x query tokens), seconds per row at 40k query terms. Duplicates are left
 * in; bsearch is indifferent to them.
 *
 * Allocates in so->scanctx, so the set shares qterm/qtree's lifetime and is freed by the
 * MemoryContextReset in bm25_rescan.
 */
static void
snippet_build_query_set(BM25ScanOpaque so)
{
    MemoryContext   oldcxt = MemoryContextSwitchTo(so->scanctx);
    SnippetQuerySet qs;

    memset(&qs, 0, sizeof(qs));
    if (so->qtree != NULL)
    {
        qs.wild = (BM25Token *) palloc(sizeof(BM25Token) * BM25_QUERY_MAX_LEAVES);
        snippet_add_leaves(so, so->qtree, &qs);
    }
    else if (so->qterm != NULL && so->qtermlen > 0)
        qs.ntoks = bm25_analyze(&so->qcfg, so->qterm, so->qtermlen, &qs.toks);

    /* n == 0 guard: qsort/bsearch with a NULL base is undefined even at count 0. */
    if (qs.ntoks > 1)
        qsort(qs.toks, (size_t) qs.ntoks, sizeof(BM25Token), bm25_token_cmp);
    so->qtoks  = qs.toks;
    so->qwild  = qs.wild;
    so->nqwild = qs.nwild;
    so->nqtoks = qs.ntoks;      /* last: the "built" flag the other three hang on */
    MemoryContextSwitchTo(oldcxt);
}

/*
 * snippet_is_hit -- is the field token's stemmed form (tok->ptr, tok->len) in the query's
 * hit set? A bsearch over the sorted terms, then a glob against each wildcard pattern (at
 * most BM25_QUERY_MAX_LEAVES of them, so the per-token cost stays bounded).
 */
static bool
snippet_is_hit(const BM25Token *tok, const BM25ScanOpaque so)
{
    int i;

    if (so->nqtoks > 0 &&
        bsearch(tok, so->qtoks, (size_t) so->nqtoks, sizeof(BM25Token),
                bm25_token_cmp) != NULL)
        return true;
    for (i = 0; i < so->nqwild; i++)
        if (bm25_glob_match(so->qwild[i].ptr, so->qwild[i].len, tok->ptr, tok->len))
            return true;
    return false;
}

/*
 * The word-boundary rule for excerpt edges. It has no predicate function of its own --
 * the snap_left / snap_right loops immediately below inline it -- but it is the reason
 * those loops are shaped the way they are, so it is stated once here.
 *
 * A byte offset `off` may START an excerpt (or, symmetrically, END one) when it is not
 * in the MIDDLE of a word: off is a boundary iff off == 0 or off == textlen, or one of
 * text[off-1]/text[off] is a non-word byte. The snap loops are the negation of that --
 * they keep walking while BOTH neighbouring bytes are word bytes, i.e. while off is not
 * a boundary, and stop at the first boundary (or at the hit edge, whichever comes first).
 *
 * The test itself is bm25_is_word_byte -- the analyzer's own predicate, deliberately not
 * a second ASCII test written out here. This decides where an excerpt may be cut, so it
 * has to agree with the tokenizer about where words end or the excerpt shows a fragment.
 * When multibyte characters became word characters (ADR 0046), the previous local test
 * (`!IS_HIGHBIT_SET(c) && isalnum(c)`) began reporting every byte of an accented word as
 * a separator, which would have allowed an excerpt to start partway into one.
 *
 * A byte-level predicate remains correct for a walk that starts at an arbitrary offset
 * and moves backward: in a multibyte encoding every non-ASCII byte, lead or continuation
 * alike, answers "word byte", so the walk cannot mistake a character's interior for a
 * word boundary. In a single-byte encoding a byte IS a character, so the per-encoding
 * table bm25_is_word_byte consults there (#295) is a character test with no interior to
 * mistake. The continuation-byte nudges below still do the separate job of landing the
 * final edge on a character boundary.
 */

/*
 * snap_left / snap_right -- pull an excerpt edge outward to include surrounding CONTEXT
 * words up to a byte budget, then snap to a whole-word boundary so the excerpt never
 * begins/ends inside a word, and to a UTF-8 char boundary so it never splits a multibyte
 * sequence (D10 mb-guard).
 *
 * snap_left: clamp `lower_bound` (the budget floor) up to 0 and jump straight to it --
 * there is no leftward walk from the hit. If that point landed in the MIDDLE of a word,
 * advance right (never past hit_start) until text[p] is no longer a word byte: that
 * lands p on the SEPARATOR AFTER the partial word, not on the next word's start, which
 * is what "don't emit a partial leading word" actually buys. Worked example: in
 * "aa bb cc HIT" with lower_bound = 1, p ends at 2, so the excerpt begins with the
 * space. Finally nudge right to the next char boundary (a continuation byte is
 * 10xxxxxx) so we never start mid-sequence. Both adjustments only ever move right, so
 * the excerpt stays within budget.
 *
 * snap_right is the mirror in direction but NOT in landing point: jump straight to
 * `upper_bound` (clamped to textlen) -- no rightward walk from the hit -- then, if that
 * landed mid-word, retreat left (never past hit_end) to the FIRST byte of that partial
 * word -- so p names the word's start, not the separator before it, and the half-open
 * excerpt text[..p) keeps that separator and drops the word. Then retreat left off a
 * continuation byte to a char boundary. Worked example: in
 * "HIT aa bb cc" (the second 'c' at offset 11) with upper_bound = 11, p ends at 10 --
 * the FIRST 'c' -- so the excerpt is "HIT aa bb " and stops at the separator, with the
 * partial word excluded. Both adjustments only ever move left, so the excerpt stays
 * within budget.
 */
static int
snap_left(const char *text, int hit_start, int lower_bound)
{
    int p;

    if (lower_bound < 0)
        lower_bound = 0;
    p = lower_bound;
    /* If lower_bound landed inside a word (both neighbouring bytes are word bytes),
     * advance past the REST of that word so we don't emit a partial one. The exit
     * condition is !word(text[p]) (or p == hit_start), so p ends on the separator
     * that follows the word, not on the next word's first byte. */
    while (p < hit_start &&
           p > 0 &&
           bm25_is_word_byte((unsigned char) text[p - 1]) &&
           bm25_is_word_byte((unsigned char) text[p]))
        p++;
    /* Nudge off any UTF-8 continuation byte (10xxxxxx) to the next char boundary.
     *
     * Gated on UTF-8 SPECIFICALLY, not on "any multibyte encoding" (TEXT-04). The test
     * `(b & 0xC0) == 0x80` identifies a continuation byte in UTF-8 and in nothing else:
     * pg_database_encoding_max_length() > 1 is also true for EUC_JP, EUC_JIS_2004,
     * EUC_KR, EUC_CN, EUC_TW and MULE_INTERNAL (the multibyte SERVER encodings besides
     * UTF-8; GB18030, GBK and the like are client-only), where bytes in 0x80-0xBF
     * are perfectly ordinary lead or trailing bytes and this loop would walk off a
     * character boundary rather than onto one.
     *
     * The single-byte half of the original reasoning still stands and is why the gate
     * cannot simply be dropped: in a single-byte encoding 0x80-0xBF are ordinary letters
     * (the WIN1251 Cyrillic block among them), and skipping them would eat the first
     * letter of the very word the loop above just snapped to. Harmless while the word
     * test was ASCII-only -- it never landed p on such a byte -- but since #295
     * bm25_is_word_byte calls the letters of a single-byte encoding's high half word
     * bytes (its generated per-encoding table), so in a WIN1251 database the loop above
     * can now land p on one, and this gate is what keeps it.
     *
     * In a UTF-8 database the loop body itself is currently unreachable (the snap loop
     * above stops only on a non-word byte, the hit edge or the budget floor, and every
     * high byte is a word byte there, so none of those is a continuation byte). It is
     * kept as the mb-guard for the moment someone narrows or widens the word test, which
     * the bottom of this file explicitly contemplates. */
    while (p < hit_start && GetDatabaseEncoding() == PG_UTF8 &&
           (((unsigned char) text[p]) & 0xC0) == 0x80)
        p++;
    return p;
}

static int
snap_right(const char *text, int textlen, int hit_end, int upper_bound)
{
    int p;

    if (upper_bound > textlen)
        upper_bound = textlen;
    p = upper_bound;
    /* If upper_bound landed inside a word (both neighbouring bytes are word bytes),
     * retreat left past the rest of that word so we don't emit a partial one. The exit
     * condition is !word(text[p - 1]) (or p == hit_end), so p ends on the word's FIRST
     * byte -- not on the separator before it, and not on "the word end". The excerpt is
     * text[..p), so the whole partial word is excluded either way; the distinction
     * matters to anyone reasoning about what byte p names. This is where snap_left is
     * NOT symmetric: it ends on the separator AFTER its partial word.
     *
     * p == textlen is excluded (like the continuation-byte loop below): that is the
     * end-of-field boundary, where the final word is complete and text[p] must not be
     * read. */
    while (p > hit_end &&
           p < textlen &&
           bm25_is_word_byte((unsigned char) text[p - 1]) &&
           bm25_is_word_byte((unsigned char) text[p]))
        p--;
    /* Retreat off any UTF-8 continuation byte to the preceding char boundary, gated on
     * UTF-8 specifically for the reason given in snap_left.
     * The `p < textlen` bound (mirroring the word-retreat loop above) is required:
     * p == textlen is the end-of-field boundary (a valid, complete char edge), and
     * reading text[textlen] would be a 1-byte heap overread past the varlena payload. */
    while (p > hit_end && p < textlen && GetDatabaseEncoding() == PG_UTF8 &&
           (((unsigned char) text[p]) & 0xC0) == 0x80)
        p--;
    return p;
}

/*
 * snippet_append_ellipsis -- append the truncation marker in a form the SERVER ENCODING
 * can actually represent.
 *
 * U+2026 HORIZONTAL ELLIPSIS used to be emitted as three hardcoded UTF-8 bytes
 * ("\xE2\x80\xA6"), unconditionally. In a UTF-8 database that is right; in any other
 * server encoding it injects bytes that do not mean what they say and may not be legal
 * at all. Concretely, in EUC_JP the 0xE2 0x80 pair is not a valid sequence, so every
 * truncated snippet failed on conversion to the client with "invalid byte sequence for
 * encoding"; in LATIN1 it silently rendered as three mojibake characters. This is the
 * ONLY place the extension writes a non-ASCII literal of its own -- everywhere else the
 * bytes it emits came from the user's own text -- which is why it was the one site ADR
 * 0046's encoding sweep did not reach.
 *
 * pg_unicode_to_server_noerror does the conversion the server's own way and reports
 * whether the character is representable at all, so encodings that DO have an ellipsis
 * (EUC_JP maps it to JIS X 0208 0xA1 0xC4) get the real one rather than a fallback. It
 * NUL-terminates, and needs MAX_UNICODE_EQUIVALENT_STRING + 1 bytes. In a UTF-8 database
 * it returns exactly the three bytes written before, so this changes no existing output.
 *
 * The ASCII fallback is three periods. It is one character in the source and three in
 * the output, which does not matter: the ellipses have never counted toward
 * max_num_chars, only the original text does.
 *
 * (Available since PostgreSQL 16 -- verified against REL_16/17/18_STABLE, with REL_12 and
 * REL_15 as the negative control -- and this extension's floor is 17.)
 */
static void
snippet_append_ellipsis(StringInfo out)
{
    unsigned char   buf[MAX_UNICODE_EQUIVALENT_STRING + 1];

    if (pg_unicode_to_server_noerror(0x2026, buf))
        appendStringInfoString(out, (char *) buf);
    else
        appendStringInfoString(out, "...");
}

/*
 * bm25_snippet(field text, start_tag text, end_tag text, max_num_chars int, escape bool)
 *   RETURNS text -- VOLATILE, PARALLEL RESTRICTED, COST 5000 (scan-slot resident; SQL
 *   defaults supply '<mark>' / '</mark>' / 300 / true so the C body normally sees 5
 *   args).
 *
 * VOLATILE, not STABLE, since #148 SQL-05: this reads the same backend-local
 * scored-scan registry bm25_score does and must carry the same marking, and STABLE
 * additionally let evaluate_function fold an all-Const call during estimation, where
 * no scored scan is live. COST 5000 because this re-analyzes the query terms AND the
 * whole field value on every row -- see bm25_match(text,text)'s derivation in
 * bm25_native--1.0.sql, which it borrows. docs/adr/0082.
 */
PG_FUNCTION_INFO_V1(bm25_snippet);
Datum
bm25_snippet(PG_FUNCTION_ARGS)
{
    BM25ScanOpaque so;
    BM25ScanOpaque refused;     /* #301: an owned scan the caller may not read */
    text          *field;
    const char    *ftext;
    int            flen;
    text          *start_tag;
    text          *end_tag;
    int            max_chars;
    bool           escape;
    BM25Token     *ftoks;
    int            nf;
    SnippetHit    *hits;
    int            nhits = 0;
    int            total_chars;
    int            i;
    /* best window state */
    int            best_i = -1,
                   best_j = -1;
    int            best_hits = 0;
    int            best_span = 0;
    StringInfoData out;

    /* --- trust boundary + NULL/empty guards (D11) --- */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();                       /* NULL field -> NULL */
    if (PG_ARGISNULL(3))
        PG_RETURN_NULL();                       /* NULL budget -> NULL (defensive) */
    max_chars = PG_GETARG_INT32(3);
    if (max_chars <= 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: \"max_num_chars\" must be positive (got %d)",
                        max_chars)));

    /* Escaping is the DEFAULT and the fail-safe direction, so both ways of not saying
     * anything -- an explicit NULL, and a catalog entry predating the argument -- select
     * it. The PG_NARGS() test matters during the window where a rebuilt library is loaded
     * against a not-yet-updated catalog: reading arg 4 unconditionally would read past a
     * 4-argument fcinfo. */
    escape = (PG_NARGS() > 4 && !PG_ARGISNULL(4)) ? PG_GETARG_BOOL(4) : true;

    /* R3: resolve the sole live scored scan; ERROR if another could still be
     * genuinely concurrent (snippet carries no row identity to disambiguate).
     * Placed after the max_num_chars check so an invalid budget still errors
     * on its own terms. */
    so = bm25_sole_scored_scan("bm25_snippet", fcinfo, &refused);
    /* #301: the resolver sees only scans the caller owns and may read; another
     * role's scan is invisible and lands in the "no scan" NULL below. When the caller
     * has no readable scan but does own one it may not read (a definer-opened
     * refcursor is owned by the role that first fetched it), that is a refusal, and
     * it raises rather than returning NULL, like the ambiguity check: NULL is also
     * the "no hits" answer, and a highlighter that silently returns nothing on every
     * row is harder to diagnose than an error naming the cause. The error depends
     * only on such a scan existing, not on anything it ranked. */
    if (so == NULL && refused != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("bm25: bm25_snippet cannot read the table ranked by the active bm25 scan"),
                 errdetail("The function bm25_snippet requires SELECT on table \"%s\" or on every "
                           "column its index reads, and no row-level security in force for the "
                           "current user.",
                           get_rel_name(refused->indexrel->rd_index->indrelid))));
    /* No active bm25 scored scan (plain SELECT, or projected after endscan): the same
     * NULL degradation bm25_score uses. We cannot know the query terms/analyzer without it.
     * A scan with neither a query text nor a jsonb tree has no terms to highlight. */
    if (so == NULL || !so->qcfg_valid ||
        (so->qtree == NULL && (so->qterm == NULL || so->qtermlen <= 0)))
        PG_RETURN_NULL();

    start_tag = PG_ARGISNULL(1) ? cstring_to_text("<mark>") : PG_GETARG_TEXT_PP(1);
    end_tag   = PG_ARGISNULL(2) ? cstring_to_text("</mark>") : PG_GETARG_TEXT_PP(2);

    field = PG_GETARG_TEXT_PP(0);
    ftext = VARDATA_ANY(field);
    flen  = VARSIZE_ANY_EXHDR(field);

    /* The query hit set: stemmed terms plus wildcard patterns. Empty (an all-stopword
     * query, say) -> no hits -> NULL.
     *
     * TEXT-12 (issue #153): built LAZILY, on this scan's first snippet call, and cached on
     * the scan. This is a projection function -- it runs once per RETURNED ROW -- while its
     * input (so->qterm or so->qtree under so->qcfg) is fixed for the whole scan: qcfg_valid
     * is already true here (the guard above tested it), and the query is frozen for exactly
     * that window, so the cached set stays correct for every later row of this scan. */
    if (so->nqtoks < 0)
        snippet_build_query_set(so);
    if (so->nqtoks == 0 && so->nqwild == 0)
        PG_RETURN_NULL();

    /* Analyze the field VALUE with the same config; each token carries src_off/src_len
     * (D10) -- the original-text byte span we highlight. */
    nf = bm25_analyze(&so->qcfg, ftext, flen, &ftoks);
    if (nf == 0)
        PG_RETURN_NULL();

    /* Mark hits. Multiple field tokens can share one source run (compound splitter) and
     * therefore one (src_off,src_len); collapse consecutive identical spans so the run is
     * wrapped once, not once per lexeme. Tokens with src_off < 0 (synthetic) are skipped.
     * Hits are appended in token order, so hits[].start is non-decreasing.
     *
     * Cancellable per field token (XCUT-06 #305): nf is the token count of an unbounded
     * caller-supplied value, and each probe is a bsearch plus up to
     * BM25_QUERY_MAX_LEAVES globs. No lock or buffer is held here. */
    hits = (SnippetHit *) palloc(sizeof(SnippetHit) * nf);
    for (i = 0; i < nf; i++)
    {
        int hs,
            he;

        CHECK_FOR_INTERRUPTS();
        if (ftoks[i].src_off < 0)
            continue;
        if (!snippet_is_hit(&ftoks[i], so))
            continue;
        hs = ftoks[i].src_off;
        he = ftoks[i].src_off + ftoks[i].src_len;
        if (nhits > 0 && hits[nhits - 1].start == hs && hits[nhits - 1].end == he)
            continue;                           /* same run as previous lexeme */
        hits[nhits].start = hs;
        hits[nhits].end   = he;
        nhits++;
    }

    if (nhits == 0)
        PG_RETURN_NULL();                       /* D11: zero hits -> NULL */

    /* Resolve every hit edge's CHARACTER offset before any budget arithmetic: from here
     * down, max_chars is compared only against cstart/cend. */
    total_chars = snippet_hit_charpos(ftext, flen, hits, nhits);

    /*
     * Best window: two-pointer over the hit list. For each left index i, extend the
     * right index j as far as the covered original-text CHARACTER span
     * (hits[j].cend - hits[i].cstart) allows within max_chars. Pick the window covering
     * the MOST hits; tie-break TIGHTER span, then EARLIEST start. Tags and ellipses never
     * count toward the budget, and neither does escaping: the budget measures the ORIGINAL
     * text consumed, so '&' spends one unit whether it emits one byte or five. A single hit
     * longer than max_chars still yields a one-hit window (its span exceeds the budget but
     * we always emit at least the hit -- see the emit clamp below).
     */
    {
        int j = 0;

        for (i = 0; i < nhits; i++)
        {
            int cover;
            int span;

            if (j < i)
                j = i;
            /* grow j while the [i..j+1] span still fits */
            while (j + 1 < nhits &&
                   hits[j + 1].cend - hits[i].cstart <= max_chars)
                j++;
            cover = j - i + 1;
            span  = hits[j].cend - hits[i].cstart;
            if (cover > best_hits ||
                (cover == best_hits && span < best_span))
            {
                best_hits = cover;
                best_span = span;
                best_i    = i;
                best_j    = j;
            }
        }
    }

    /*
     * Excerpt bounds. Core span [hits[best_i].start, hits[best_j].end) always emits (even
     * if it alone exceeds max_chars -- a single over-long hit word is emitted whole, D11).
     * Distribute the REMAINING budget as surrounding context, snapped outward to word
     * boundaries on whole-character steps (mb-guard). If the core already fills/overflows
     * the budget, no context is added.
     *
     * Split leftover evenly, EXCEPT that a side with less text than its half gives the
     * shortfall to the other side (TEXT-06 #308; ADR 0049 counts the budget, this is how
     * it is spent). The even split alone handed a hit in the field's first or last words
     * about half the requested excerpt, and truncated a short field that would have fit
     * whole, with a spurious ellipsis. Done in CHARACTERS, before the byte conversion,
     * against the room on each side: cstart characters on the left, total_chars - cend
     * on the right. The three steps give the left its half (or all its room), the right
     * whatever the left left over (or all its room), then the left whatever the right
     * left over; left_room + right_room <= slack throughout, and each is within its room.
     */
    {
        int core_start = hits[best_i].start;
        int core_end   = hits[best_j].end;
        int core_span  = hits[best_j].cend - hits[best_i].cstart;   /* CHARACTERS */
        int slack      = max_chars - core_span;      /* may be negative for an over-long hit */
        int left_avail = hits[best_i].cstart;
        int right_avail = total_chars - hits[best_j].cend;
        int left_room  = 0;
        int right_room = 0;
        int lower;
        int upper;
        int estart;
        int eend;
        int h;

        if (slack > 0)
        {
            left_room  = Min(slack / 2, left_avail);
            right_room = Min(slack - left_room, right_avail);
            left_room  = Min(slack - right_room, left_avail);
        }

        /* Convert the per-side CHARACTER allowance into the byte bounds snap_* works in.
         * The left bound has to be found from the start of the text (a character count can
         * only be walked forward); the right bound walks on from the core's own end, which
         * snippet_hit_charpos already paired with its character offset. The left target is
         * never negative (left_room <= cstart), and snippet_char_to_byte would clamp it to 0
         * if it were.
         *
         * The right target is summed in int64 and clamped at flen. Neither is load-bearing:
         * right_room <= total_chars - cend, so the sum is at most total_chars, which is at
         * most flen. That bound now rests on the room clamp just above, one fact in this
         * function; before TEXT-06 it rested on a conjunction of three facts in other files
         * (the minimum span of a hit, the maximum size of a text datum, the halving of
         * slack), and the carry-over removed the last of them. Summing wide keeps the
         * safety local to this line whichever argument is current: a wrap would hand
         * snippet_char_to_byte a target LEFT of its anchor, i.e. no trailing context at all
         * rather than all of it.
         *
         * Clamping at flen is exact rather than approximate: a field can never hold more
         * characters than bytes, so character offset flen is always at or past the end. */
        {
            int64   right_target = (int64) hits[best_j].cend + (int64) right_room;

            if (right_target > (int64) flen)
                right_target = (int64) flen;

            lower = snippet_char_to_byte(ftext, flen, 0, 0,
                                         hits[best_i].cstart - left_room);
            upper = snippet_char_to_byte(ftext, flen, core_end, hits[best_j].cend,
                                         (int) right_target);
        }

        /* Expand left/right within the per-side budget; snap_* pulls in whole context
         * words and lands the edge on a word + UTF-8 char boundary (mb-guard, D10). Both
         * only ever move the edge INWARD from the bound handed to them, so the character
         * budget computed above survives the snapping. */
        estart = snap_left(ftext, core_start, lower);
        eend   = snap_right(ftext, flen, core_end, upper);

        initStringInfo(&out);

        /* Leading ellipsis when we truncated the original at the left. */
        if (estart > 0)
            snippet_append_ellipsis(&out);

        /* Emit [estart, eend) with tags around each in-window hit span. Every byte that
         * came from the FIELD goes through snippet_append_text, while the caller's TAGS are
         * appended verbatim, because they are markup by contract. Casing and punctuation
         * are still preserved; escaping only rewrites the five HTML metacharacters.
         *
         * Routing the HIT span through the escaper too is defence in depth and does nothing
         * today: a hit is a whole analyzer word run, and bm25_is_word_byte admits only
         * ASCII alnum bytes and high-bit bytes (all of them in a multibyte or SQL_ASCII
         * database, the encoding's letters and digits in a single-byte one), so none of
         * the five ASCII metacharacters can fall inside one. It is written this way so that widening
         * the word-character predicate -- admitting the apostrophe in "don't", say -- cannot
         * silently open an escaping hole in a path nobody re-examined. */
        {
            int cursor = estart;

            for (h = best_i; h <= best_j; h++)
            {
                int hstart = hits[h].start;
                int hend   = hits[h].end;

                if (hstart < cursor)            /* defensive: overlapping/adjacent spans */
                    hstart = cursor;
                if (hstart >= hend)             /* fully consumed by the previous span */
                    continue;
                if (hstart > cursor)
                    snippet_append_text(&out, ftext + cursor, hstart - cursor, escape);
                appendBinaryStringInfo(&out, VARDATA_ANY(start_tag),
                                       VARSIZE_ANY_EXHDR(start_tag));
                snippet_append_text(&out, ftext + hstart, hend - hstart, escape);
                appendBinaryStringInfo(&out, VARDATA_ANY(end_tag),
                                       VARSIZE_ANY_EXHDR(end_tag));
                cursor = hend;
            }
            if (eend > cursor)
                snippet_append_text(&out, ftext + cursor, eend - cursor, escape);
        }

        /* Trailing ellipsis when we truncated the original at the right. */
        if (eend < flen)
            snippet_append_ellipsis(&out);
    }

    PG_RETURN_TEXT_P(cstring_to_text_with_len(out.data, out.len));
}
