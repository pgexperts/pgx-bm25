/* bm25_tokenize.c -- the tokenizers: split text into word runs and case-fold them.
 * bm25_tokenize is the minimal M0/M1 form (now debug-only); bm25_analyze is the M3
 * analyzer that adds the stopword + stemmer stage.
 *
 * Both are encoding-aware (ADR 0046): a word run is measured in CHARACTERS via
 * bm25_next_char, not bytes, so a multibyte character is never split down the
 * middle, and case folding happens on the bytes we EMIT rather than on a working
 * copy. Folding up front is what the byte-wise version did, and it could not work:
 * a fold is not byte-length preserving, and a byte-wise tolower() corrupts a
 * multibyte lead byte outright on any platform whose single-byte ctype table
 * covers 0x80-0xFF. */

#include "postgres.h"

#include "bm25.h"
#include "miscadmin.h"      /* CHECK_FOR_INTERRUPTS */
#include "tsearch/ts_cache.h"
#include "tsearch/ts_public.h"
#include "utils/array.h"
#include "utils/builtins.h"

/* Ceiling on the UP-FRONT token-array guess, in entries.
 *
 * Both tokenizers used to allocate textlen/2+1 entries immediately. At
 * sizeof(BM25Token) == 24 that is TWELVE bytes of scratch per input byte, so an
 * ordinary large text column crossed MaxAllocSize (1 GB - 1) at about 89.5 MB of
 * input and died with "invalid memory allocation request size 1200000024" --
 * naming neither bm25 nor the document, and far below text's own ~1 GB limit
 * (#65.13). The array grows on demand anyway (a compound-splitting dictionary can
 * emit several tokens per run, so the run count was never a bound on the token
 * count), so the estimate only has to cover the common small document; anything
 * larger pays for the tokens it actually produces, not for its input size.
 * 1024 entries = 24 KB, enough that a typical document still never repallocs. */
#define BM25_TOKCAP_INITIAL_MAX 1024

/* Hard ceiling on the token array, in entries. repalloc refuses anything past
 * MaxAllocSize, and reaching that through the doubling below would resurface the
 * same anonymous allocator error this fix exists to remove. It also keeps the int
 * doubling from overflowing to a negative capacity. */
#define BM25_MAX_DOC_TOKENS ((int) (MaxAllocSize / sizeof(BM25Token)))

/*
 * tok_array_grow -- double the token array, or fail naming the document.
 *
 * Shared by every emit site in this file so the ceiling and its wording exist
 * once. Callers guarantee *tokcap >= 1, which is what makes the doubling advance.
 */
static void
tok_array_grow(BM25Token **toks, int *tokcap)
{
    int     newcap;

    if (*tokcap >= BM25_MAX_DOC_TOKENS)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bm25: document produces more than %d index tokens",
                        BM25_MAX_DOC_TOKENS),
                 errdetail("The per-document token array cannot exceed %zu bytes.",
                           (Size) MaxAllocSize)));

    /* Saturate at the ceiling rather than overshooting it, so the last growth
     * step is still usable instead of erroring one allocation early. */
    newcap = (*tokcap > BM25_MAX_DOC_TOKENS / 2)
        ? BM25_MAX_DOC_TOKENS
        : *tokcap * 2;

    *toks   = (BM25Token *) repalloc(*toks, sizeof(BM25Token) * (Size) newcap);
    *tokcap = newcap;
}

/*
 * bm25_tokenize -- split text into tokens (no stopword/stemmer stage).
 *
 * Superseded by bm25_analyze for every indexing path; it survives only as the
 * token source for three debug SRFs (bm25_debug_pending_append in bm25_pending.c,
 * and the accumulator probes bm25_debug_accum / bm25_debug_accum_multi in
 * bm25_accum.c), which is why it needs no analyzer config. It shares the analyzer's
 * character rules so a debug probe cannot disagree with the real tokenizer about
 * where a word ends or how it folds.
 *
 * Each token owns its folded bytes: the fold can change length, so a token can no
 * longer be a (offset, length) view into one shared buffer. Returns the number of
 * tokens found (0 for empty or separator-only input).
 *
 * All allocations are in the current memory context; callers that need a shorter
 * lifetime should switch context before calling.
 */
int
bm25_tokenize(const char *text, int textlen, BM25Token **out)
{
    BM25Token  *toks;
    int         ntok = 0;
    int         tokcap;
    int         i;
    int         pos = 0;

    /* Alternating word/non-word gives at most textlen/2+1 runs and this tokenizer
     * emits exactly one token per run, so that IS a true bound -- but paying it up
     * front costs 12 bytes of scratch per input byte (#65.13). Clamp the guess and
     * grow on demand; the bound still holds, it is just no longer prepaid. */
    tokcap = Min(textlen / 2 + 1, BM25_TOKCAP_INITIAL_MAX);
    toks   = palloc(sizeof(BM25Token) * (Size) tokcap);

    i = 0;
    while (i < textlen)
    {
        int     start;
        int     rawlen;
        int     foldedlen;
        char   *folded;
        bool    is_word;

        /* Cancellable, like bm25_analyze's loop (TEXT-09). This walks caller-supplied
         * text a character at a time and folds each run, so a large input is a long
         * uninterruptible stretch of work reachable from SQL -- and the header above
         * claims this function "shares the analyzer's character rules so a debug probe
         * cannot disagree with the real tokenizer", which had quietly stopped being
         * true of everything except the character classification itself. */
        CHECK_FOR_INTERRUPTS();

        /* Skip leading separators. */
        while (i < textlen)
        {
            int clen = bm25_next_char(text + i, textlen - i, &is_word);

            if (is_word)
                break;
            i += clen;
        }
        start = i;

        /* Consume word characters. */
        while (i < textlen)
        {
            int clen = bm25_next_char(text + i, textlen - i, &is_word);

            if (!is_word)
                break;
            i += clen;
        }

        rawlen = i - start;
        if (rawlen == 0)
            continue;           /* ran off the end skipping separators */

        /* Same term-length cap as bm25_analyze, and for the same reason: a term longer
         * than BM25_MAX_TERM_BYTES cannot be stored, so emitting one here would let a
         * debug probe produce a token stream the real indexing path would have refused.
         * NOTICE-and-skip rather than ERROR, matching bm25_analyze's sites -- an
         * over-long word is dropped, not a failure of the whole call.
         *
         * The raw length is only the cheap pre-filter. The bound that matters is on the
         * STORED bytes, and a case fold can LENGTHEN a term (ICU maps a single dotted
         * capital I to two characters), which is exactly why bm25_analyze re-checks
         * after folding rather than before. Checking rawlen alone would leave a
         * lengthening fold able to emit a token the real path refuses -- the same drift
         * this fix exists to remove, one step further along. */
        if (rawlen > BM25_MAX_TERM_BYTES)
        {
            ereport(NOTICE,
                    (errmsg("bm25: word is too long to be indexed"),
                     errdetail("Words longer than %d bytes are ignored.",
                               BM25_MAX_TERM_BYTES)));
            continue;
        }

        if (ntok == tokcap)
            tok_array_grow(&toks, &tokcap);

        folded = bm25_fold_term(text + start, rawlen, &foldedlen);
        if (foldedlen > BM25_MAX_TERM_BYTES)
        {
            /* Freed through the char* bm25_fold_term returned, not through
             * toks[ntok].ptr, which is const char * -- the token never took
             * ownership because it is not being emitted. */
            pfree(folded);
            ereport(NOTICE,
                    (errmsg("bm25: word is too long to be indexed"),
                     errdetail("Words longer than %d bytes are ignored.",
                               BM25_MAX_TERM_BYTES)));
            continue;
        }
        toks[ntok].ptr = folded;
        toks[ntok].len = foldedlen;
        toks[ntok].pos = pos++;
        /* No snippet path consumes these, but leave them at the documented
         * "no source run" value rather than uninitialized. */
        toks[ntok].src_off = -1;
        toks[ntok].src_len = 0;
        ntok++;
    }

    *out = toks;
    return ntok;
}

/*
 * bm25_analyze -- M3 analyzer token production.
 *
 * Filter order is fixed: split -> stopword -> stemmer -> fold. The stopword +
 * stemmer steps are a SINGLE ts_lexize() call over the Snowball "<language>_stem"
 * dictionary (which carries the language stoplist), which is what makes
 * tokenization a deterministic pure function of (dict OID, token) -- the property
 * WAL replay and physical replicas rely on. The config (dict OID, stopword policy)
 * comes from bm25_analyzer.c.
 *
 * The fold sits LAST, not first, and only on the stopwords=none path: the stemmed path
 * is folded by ts_lexize, and folding beforehand would be both redundant and
 * destructive (see the working-copy comment below). "The dictionary folds its own
 * input" holds for every core template that can plausibly sit behind a `<language>_stem`
 * name -- snowball, simple, ispell, thesaurus -- but `stem_dict_oid` is resolved by NAME
 * and a `synonym` dictionary declared CaseSensitive=true does not fold. Such a
 * dictionary now receives raw-cased text where it used to receive byte-lowercased text,
 * and its caller gets whatever it returns; nothing downstream is corrupted, but
 * cross-case matching through it is the configuration's own responsibility.
 *
 * One consequence worth naming: the fold's
 * mapping comes from the database default collation, so "deterministic" here means
 * deterministic for a given database -- the same guarantee ts_lexize itself carries,
 * since dict_snowball folds through the same locale machinery. It is NOT a promise
 * that two servers with different collation providers agree, which is why a
 * collation-library upgrade is a REINDEX event for this index type exactly as it is
 * for a collated btree (ADR 0046).
 *
 * We call the cached dictionary's lexize FmgrInfo directly: preload-free, and
 * replica-identical because a PHYSICAL replica has byte-identical catalogs, so the
 * same OID is the same dictionary there. That is the ONLY sense in which a dict OID
 * is stable -- pg_upgrade renumbers it, which is why the fingerprint records a
 * name-derived identity rather than this OID (ADR 0080).
 *
 * The token is a (char*, len)
 * pair, NOT NUL-terminated -- the lexize function reads the len arg. A stoplist
 * word yields res == NULL or res[0].lexeme == NULL; a stemmed word yields one
 * (or, for compound splitters, several) lexemes.
 *
 * POSITION ADVANCES ONCE PER SOURCE RUN, not once per emitted lexeme (issue #184,
 * ADR 0087, analyzer revision 5): all the lexemes of one word share that word's
 * position, matching core FTS's ts_parse.c, and a run's repeated lexemes are
 * deduplicated. Positions advance only for runs that KEPT at least one token, so
 * they are post-stopword (matching index time) unless stopwords=none -- a dropped
 * stopword leaves no gap, which still differs from core and is out of scope here
 * (ADR 0087 names it).
 */
int
bm25_analyze(const BM25AnalyzerConfig *cfg, const char *text, int textlen,
             BM25Token **out)
{
    char       *buf;
    BM25Token  *toks;
    int         ntok = 0;
    int         tokcap;
    int         i;
    int         pos = 0;
    TSDictionaryCacheEntry *dict = lookup_ts_dictionary_cache(cfg->stem_dict_oid);

    /* VERBATIM NUL-terminated working copy -- deliberately NOT folded. The fold used
     * to happen here, which is what broke multibyte text: a byte-wise fold rewrites a
     * multibyte lead byte (0xC3 -> 0xE3 under a UTF-8 LC_CTYPE on BSD/macOS), and no
     * fold is byte-length preserving, so folding before the split destroys both the
     * characters and the src_off/src_len spans measured against them. Runs are now
     * split on these original bytes and folded at emit (ADR 0046).
     *
     * The copy itself stays, for the NUL: stem_dict_oid is a user-chosen dictionary,
     * and while the core dictionaries honour the length argument, handing one a
     * pointer into a bare varlena payload would leave a NUL-scanning dictionary
     * reading off the end of the datum.
     *
     * The textlen guard is required, not defensive. Callers may legitimately pass
     * (NULL, 0) -- a query-tree leaf carrying no text reaches this through
     * bm25_scan_rank.c's per-leaf analyze -- and the byte loop this replaced tolerated that
     * implicitly by never executing. memcpy does not: C requires both pointers to be
     * valid even when n is 0, glibc annotates it __nonnull, and UBSan's
     * nonnull-attribute check duly halted the backend on it. macOS's memcpy carries no
     * such annotation, so the local UBSan build cannot see this one -- it took the
     * hardening gate. */
    buf = palloc(textlen + 1);
    if (textlen > 0)
        memcpy(buf, text, textlen);
    buf[textlen] = '\0';

    /* Initial guess, not a bound: alternating word/non-word gives at most
     * (textlen+1)/2 word RUNS, and textlen/2+1 covers that. It does NOT bound the
     * TOKEN count, because ts_lexize returns a lexeme ARRAY and the emit loop below
     * appends one token per lexeme -- a compound splitter or a thesaurus dictionary
     * turns one run into several. stem_dict_oid is a user-chosen dictionary (the
     * `language` reloption, resolved by name through search_path), so nothing
     * upstream constrains it to one lexeme per run and this array must grow. #148
     * HDL-07 gave that reloption a validator, and it does NOT change this: it checks
     * that "<language>_stem" RESOLVES, never what the dictionary DOES, so an ispell
     * compound splitter or a thesaurus passes it and still returns several lexemes
     * per run. See ADR 0023's addendum.
     *
     * Clamped, because the guess is not a bound in the other direction either: it
     * scaled with the INPUT (12 bytes of scratch per input byte) rather than with
     * the tokens produced, which put a hard ~89.5 MB ceiling on an indexable value
     * (#65.13). Small documents still fit the first allocation and never repalloc;
     * large ones now grow to what they actually need. */
    tokcap = Min(textlen / 2 + 1, BM25_TOKCAP_INITIAL_MAX);
    toks   = palloc(sizeof(BM25Token) * (Size) tokcap);  /* tokcap >= 1: growth advances */

    i = 0;
    while (i < textlen)
    {
        int         start;
        int         rawlen;
        TSLexeme   *res;
        bool        is_word;

        /* Per word run, so every caller is cancellable in proportion to its input. The
         * scan paths gained their checks in ADR 0024 but this loop had none, which left
         * the tokenizer itself an uninterruptible stretch on a large value -- reachable
         * from ordinary SQL through the @@@ standalone evaluator, whose operands are
         * unbounded user text (H15), as well as from build and insert. */
        CHECK_FOR_INTERRUPTS();

        /* Character-wise, not byte-wise: bm25_next_char always returns >= 1 and never
         * more than the bytes remaining, so each loop advances and neither can stop on
         * a continuation byte. That is what keeps a run's edges on character
         * boundaries, which is in turn what lets src_off/src_len be exact. */
        while (i < textlen)
        {
            int clen = bm25_next_char(buf + i, textlen - i, &is_word);

            if (is_word)
                break;
            i += clen;
        }
        start = i;
        while (i < textlen)
        {
            int clen = bm25_next_char(buf + i, textlen - i, &is_word);

            if (!is_word)
                break;
            i += clen;
        }
        rawlen = i - start;
        if (rawlen == 0)
            continue;           /* ran off the end skipping separators */

        /* An alphanumeric run has no natural length limit (base64/hex blobs, DNA
         * strings, machine-generated identifiers), but a term must fit one segment
         * page inline in its dictionary record. Drop the run here rather than let it
         * reach the writer: skipping one unsearchable monster token keeps the rest of
         * the document indexable, and mirrors what core FTS does with the same limit.
         * Checked BEFORE ts_lexize so a 20 KB run costs no stemming work either. */
        if (rawlen > BM25_MAX_TERM_BYTES)
        {
            ereport(NOTICE,
                    (errmsg("bm25: word is too long to be indexed"),
                     errdetail("Words longer than %d bytes are ignored.",
                               BM25_MAX_TERM_BYTES)));
            continue;
        }

        res = (TSLexeme *) DatumGetPointer(
            FunctionCall4(&dict->lexize,
                          PointerGetDatum(dict->dictData),
                          PointerGetDatum(buf + start),
                          Int32GetDatum(rawlen),
                          PointerGetDatum(NULL)));

        if (res == NULL || res[0].lexeme == NULL)
        {
            /* Stoplist word (or fully filtered). Drop it for the default stopword
             * policy; keep its lowercased surface form for stopwords=none so the
             * token stream still contains it. An empty result array is still an
             * allocation (dict_snowball returns a zeroed two-entry array), freed
             * here as ts_parse.c's parsetext frees every result it consumes. */
            if (res != NULL)
                pfree(res);
            if (cfg->stopwords == BM25_STOPWORDS_NONE)
            {
                int     keptlen;
                char   *kept;

                /* This is the ONE path that writes a dictionary key without going
                 * through ts_lexize, so it must fold exactly the way the dictionary
                 * folds its own input -- otherwise a stopwords=none index and a
                 * wildcard query disagree about the same word's bytes. */
                kept = bm25_fold_term(buf + start, rawlen, &keptlen);

                /* The rawlen cap above bounded the run, not the fold: a case fold can
                 * LENGTHEN a term (ICU maps a single 'I-dot' to two characters), so the
                 * bytes we are about to store need their own check, exactly as the
                 * stemmed branch re-checks its lexemes. */
                if (keptlen > BM25_MAX_TERM_BYTES)
                {
                    ereport(NOTICE,
                            (errmsg("bm25: word is too long to be indexed"),
                             errdetail("Words longer than %d bytes are ignored.",
                                       BM25_MAX_TERM_BYTES)));
                    pfree(kept);
                    continue;
                }
                if (ntok == tokcap)
                    tok_array_grow(&toks, &tokcap);
                toks[ntok].ptr = kept;
                toks[ntok].len = keptlen;
                toks[ntok].pos = pos++;
                toks[ntok].src_off = start;     /* D10: original-text byte span of this run */
                toks[ntok].src_len = rawlen;
                ntok++;
            }
        }
        else
        {
            /* One or more stemmed lexemes (compound splitters emit several;
             * Snowball stem emits one). Emit each, ALL AT THE RUN'S POSITION.
             *
             * ONE POSITION PER SOURCE RUN, matching core FTS (issue #184, ADR 0087,
             * analyzer revision 5). src/backend/tsearch/ts_parse.c bumps `prs->pos`
             * once per LexizeExec result set and then again only for a lexeme carrying
             * TSL_ADDPOS, so a compound split's lexemes SHARE the run's position. This
             * loop now does the same: `pos` is stamped, not incremented, inside the
             * per-lexeme body, and bumped once after the run.
             *
             * The TSL_ADDPOS half is deliberately NOT reproduced, and today it cannot
             * be reached: the only core dictionaries that set that flag are multi-word
             * substituters, and the lexize call below passes a NULL DictSubState, which
             * makes a thesaurus fail loud ("forbidden call of thesaurus or nested
             * call") rather than emit anything. So co-positioning every lexeme of a run
             * is exact parity for every dictionary that can actually be configured
             * here. If lookahead-capable dictionaries are ever supported, this loop
             * must give an ADDPOS lexeme its own position or parity breaks silently.
             *
             * WHY THIS COULD NOT BE DONE ON ITS OWN -- the history is load-bearing,
             * because the obvious one-line version of this change was written once,
             * passed all 105 suites of the day, and silently broke phrase search into
             * zero-row results (ADR 0077). The document side is only half the pipeline.
             * The query side used to expand a phrase into one matcher term per query
             * LEXEME, and bm25_phrase.c's ordered_match requires STRICTLY INCREASING
             * document positions -- N co-positioned document lexemes can never fill N
             * strictly increasing slots. Core survives the identical document layout
             * only because phraseto_tsquery compensates on the QUERY side, emitting
             * alternatives ('footballklubber' | 'foot' & 'ball' & 'klubber' | ...).
             *
             * That counterpart now exists and shipped first, deliberately inert: #193
             * gave the matcher a SLOT model (bm25_phrase_slot_map plus the recheck's
             * per-slot list merge, and an exact bipartite path in unordered_match for
             * the overlapping slot sets co-positioning makes reachable), and #194 made
             * doclen the run count on both ingest paths (format v8). Both derive their
             * behaviour from the very positions this loop emits, so while position
             * advanced per lexeme every slot was one token and doclen equalled ntok.
             * This loop is what activates them; neither needed a further change.
             *
             * WITHIN-RUN DEDUP BY EXACT TERM BYTES, and it belongs in this same change
             * rather than after it. A compound repeats lexemes -- ispell_sample turns
             * 'footballklubber' into {footballklubber, foot, ball, klubber, football,
             * klubber}, with 'klubber' twice. Once those share a position, keeping both
             * would give a term the text contains ONCE a tf of 2, and store that term
             * twice at the same position. That is the tf third of "one source word
             * counted N times", and it is also what core's to_tsvector does not do
             * (five distinct lexemes, each at :1).
             *
             * NOTHING DOWNSTREAM WOULD HAVE CAUGHT IT, which is the actual argument for
             * doing it here. It is tempting to believe bm25_seg_build.c's
             * Assert(npos == tfs[j]) is a tripwire for the un-deduped case; it is not.
             * bm25_accum_add_field_tokens increments tf and appends a position in the
             * SAME iteration, so npos == tf holds by construction whatever the values,
             * and both position codecs use unbiased deltas, so the resulting zero delta
             * encodes and decodes silently. An un-deduped build would therefore be
             * quietly wrong in tf and in core parity with every assertion green -- the
             * same shape of failure as ADR 0077's.
             *
             * It also cannot land separately AFTERWARDS without spending a second
             * BM25_ANALYZER_REVISION, i.e. a second REINDEX for every user.
             *
             * Scope is the RUN, not the document: the same term in two different source
             * words is two occurrences and must stay two postings. Comparison is over
             * [run_first, ntok), the tokens this run has emitted so far -- O(k^2) in one
             * run's lexeme count, which a dictionary bounds, against an O(1) allocation.
             *
             * WHAT DID NOT CHANGE. The stopwords=none surface branch above already
             * emitted at most one token per run, so its `pos++` was per-run already.
             * bm25_tokenize (the raw, dictionary-free tokenizer at the top of this file)
             * likewise emits one token per run and is deliberately untouched. */
            int     j;
            /* First token ordinal this run emits; the dedup window's lower bound, and
             * the test for "did this run emit anything" once the loop is done. Both
             * uses need it to be read AFTER the over-long-lexeme `continue` below has
             * had its chance to skip every lexeme the run produced. */
            int     run_first = ntok;

            /* MEMORY (TEXT-03(a), issue #305). Each lexeme is its own palloc chunk
             * in the caller's context -- the dictionary API's contract, which
             * ts_parse.c's parsetext relies on when it keeps every lexeme as its word
             * and pfrees only the result array. This loop does the same: a kept
             * lexeme becomes the token's bytes with no second copy, a skipped one
             * (over-long or a within-run duplicate) is freed, and the array goes once
             * the run is done. Per analyzed word that drops two dead allocations, the
             * result array and the copy, which were about half of what a long
             * document's analysis held live until its caller's context was reset.
             *
             * RESIDUAL (TEXT-03(b), decision D21): every caller still analyzes a
             * whole field before it can learn the document is over the 65,535-token
             * ingest ceiling, so a document certain to be rejected is analyzed in
             * full first. Stopping early needs a caller-supplied token limit and an
             * overflow report here, with each caller raising its own existing error
             * (sql/68, 77 and 100 pin them) and the insert path carrying the count
             * across fields. Not done: it costs CPU and statement-scoped memory, no
             * data, and statement_timeout bounds it. */
            for (j = 0; res[j].lexeme != NULL; j++)
            {
                int     llen = (int) strlen(res[j].lexeme);
                int     d;
                bool    dup = false;

                /* stem_dict_oid is a user-chosen TS dictionary, and not every
                 * dictionary shortens: a thesaurus/synonym dictionary can emit a
                 * lexeme LONGER than the run it replaced, so the rawlen check above
                 * does not bound this. Re-check what we are about to store. */
                if (llen > BM25_MAX_TERM_BYTES)
                {
                    ereport(NOTICE,
                            (errmsg("bm25: word is too long to be indexed"),
                             errdetail("Words longer than %d bytes are ignored.",
                                       BM25_MAX_TERM_BYTES)));
                    pfree(res[j].lexeme);
                    continue;
                }
                /* Within-run dedup (see the block comment above). Scans only this
                 * run's own emissions, so a repeat of the SAME word later in the
                 * document is untouched and still counts toward tf. */
                for (d = run_first; d < ntok; d++)
                {
                    if (toks[d].len == llen &&
                        memcmp(toks[d].ptr, res[j].lexeme, (Size) llen) == 0)
                    {
                        dup = true;
                        break;
                    }
                }
                if (dup)
                {
                    pfree(res[j].lexeme);
                    continue;
                }

                /* One token per LEXEME, so this is the site that can outrun the
                 * run-count estimate. Grow before the write, never after. */
                if (ntok == tokcap)
                    tok_array_grow(&toks, &tokcap);

                /* Adopted, not copied (see MEMORY above). The token is (ptr, len), so
                 * the lexeme's trailing NUL is simply not part of it. */
                toks[ntok].ptr = res[j].lexeme;
                toks[ntok].len = llen;
                /* Stamped, not incremented: every lexeme of this run shares it. */
                toks[ntok].pos = pos;
                /* D10: all lexemes of a compound-split run share the run's source span
                 * (start .. start+rawlen). The snippet highlighter marks the whole run. */
                toks[ntok].src_off = start;
                toks[ntok].src_len = rawlen;
                ntok++;
            }

            /* Consume the position only if the run actually emitted. A run whose every
             * lexeme was over-long (or deduped to nothing) contributes no token, and
             * must not leave a GAP in the position sequence: doclen is max(pos)+1
             * (bm25_accum_add_field_tokens), so a gap would inflate the BM25 length
             * denominator for a document whose text is unchanged, and bm25_phrase's
             * slop arithmetic counts position distance. This is why the run's emission
             * is detected by comparing ntok rather than by inspecting res[]. */
            if (ntok > run_first)
                pos++;
            pfree(res);
        }
    }

    *out = toks;
    return ntok;
}

/*
 * bm25_tokenize_to_array -- shared helper: run bm25_analyze with cfg and return
 * the resulting token strings as a palloc'd text[].  Called by both debug
 * overloads so the array-building logic lives in one place.
 */
static ArrayType *
bm25_tokenize_to_array(const BM25AnalyzerConfig *cfg, text *in)
{
    BM25Token  *toks;
    int         n,
                i;
    Datum      *elems;

    n = bm25_analyze(cfg, VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in), &toks);
    elems = palloc(sizeof(Datum) * n);
    for (i = 0; i < n; i++)
        elems[i] = PointerGetDatum(
            cstring_to_text_with_len(toks[i].ptr, toks[i].len));
    return construct_array_builtin(elems, n, TEXTOID);
}

/*
 * bm25_debug_tokenize(text) -- index-default english/default/standard analyzer.
 *
 * Builds the documented default config (language="english",
 * stopwords=BM25_STOPWORDS_DEFAULT, tokenizer=STANDARD), derives the dict OID
 * via bm25_analyzer_default_config, then calls bm25_analyze.  Replaces the old
 * ASCII-only bm25_tokenize wrapper that predated the M3 analyzer.  STABLE
 * because it performs a catalog lookup to resolve the Snowball dict OID.
 */
PG_FUNCTION_INFO_V1(bm25_debug_tokenize);
Datum
bm25_debug_tokenize(PG_FUNCTION_ARGS)
{
    text               *in = PG_GETARG_TEXT_PP(0);
    BM25AnalyzerConfig  cfg;

    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.language, "english", BM25_STEMMER_NAME_LEN);
    cfg.stopwords      = BM25_STOPWORDS_DEFAULT;
    cfg.tokenizer_type = BM25_TOKENIZER_STANDARD;
    bm25_analyzer_default_config(&cfg);   /* fills stem_dict_oid, stemmer_id, stopword_set_hash */

    PG_RETURN_ARRAYTYPE_P(bm25_tokenize_to_array(&cfg, in));
}

/*
 * debug_cfg_from_args -- the analyzer config the two explicit-config debug probes
 * (bm25_debug_tokenize_cfg and bm25_debug_analyze_positions) build from their
 * (tokenizer, stopwords, language) arguments. One helper since issue #313 TEXT-10:
 * the block was duplicated verbatim in both.
 *
 * The language length check is bm25_validate_language's, with its message. Without
 * it the strlcpy into the BM25_STEMMER_NAME_LEN buffer TRUNCATED an over-long name
 * silently, and the truncated name went to the dictionary lookup -- which errors in
 * practice, but resolves to the wrong dictionary if one shares the first 31 bytes.
 * The full bm25_validate_language is not called: the lookup itself stays
 * bm25_analyzer_default_config's, as before.
 */
static void
debug_cfg_from_args(BM25AnalyzerConfig *cfg, const char *tokenizer,
                    const char *stopwords, const char *language)
{
    /* Second argument is the TOKENIZER; see bm25_debug_tokenize_cfg's header. Shared
     * with the `tokenizer` reloption. */
    bm25_validate_tokenizer(tokenizer);

    if (strlen(language) >= BM25_STEMMER_NAME_LEN)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: language name \"%s\" is too long", language),
                 errdetail("The maximum length is %d bytes.",
                           BM25_STEMMER_NAME_LEN - 1)));

    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->language, language, BM25_STEMMER_NAME_LEN);
    cfg->tokenizer_type = BM25_TOKENIZER_STANDARD;

    if (strcmp(stopwords, "none") == 0)
        cfg->stopwords = BM25_STOPWORDS_NONE;
    else if (strcmp(stopwords, "default") == 0)
        cfg->stopwords = BM25_STOPWORDS_DEFAULT;
    else
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bm25: unknown stopwords value \"%s\" (expected \"default\" or \"none\")",
                        stopwords)));

    bm25_analyzer_default_config(cfg);   /* fills stem_dict_oid, stemmer_id, stopword_set_hash */
}

/*
 * bm25_debug_tokenize_cfg(text, tokenizer text, stopwords text, language text)
 * -- explicit-config overload.
 *
 * Builds a BM25AnalyzerConfig from the caller-supplied args. `language` drives the
 * Snowball stemmer; `stopwords` is "default" or "none"; `tokenizer` selects the
 * tokenizer, of which "standard" is the only one. STABLE for the same reason as the
 * 1-arg form: catalog lookup for the dict OID.
 *
 * THE SECOND ARGUMENT IS THE TOKENIZER, NOT THE ANALYZER (#148 HDL-07), and it used
 * to be named `analyzer` here while being discarded under a comment that read
 * "standard is the only tokenizer in M3" -- the two knobs conflated in one
 * declaration. Every one of the eleven call sites in sql/ passes "standard", which is
 * a tokenizer value, so the NAME was the thing that was wrong, not the callers.
 *
 * It is now validated, through bm25_handler.c's bm25_validate_tokenizer -- the same
 * function the `tokenizer` reloption uses, deliberately shared rather than copied.
 * Before this it accepted any string whatsoever and discarded it, so
 * bm25_debug_tokenize(t,'TOTAL_NONSENSE','default','german') returned German stems
 * and reported nothing: the same accepted-and-ignored defect HDL-07 fixed on the
 * reloption, on the surface a test would have used to notice.
 *
 * There is deliberately NO `analyzer` argument here to validate against
 * bm25_validate_analyzer, so the asymmetry that leaves is nominal, not behavioural:
 * the reloption's reserved `analyzer` knob has no counterpart in this signature.
 */
PG_FUNCTION_INFO_V1(bm25_debug_tokenize_cfg);
Datum
bm25_debug_tokenize_cfg(PG_FUNCTION_ARGS)
{
    text               *in        = PG_GETARG_TEXT_PP(0);
    char               *tokenizer = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char               *stopwords = text_to_cstring(PG_GETARG_TEXT_PP(2));
    char               *language  = text_to_cstring(PG_GETARG_TEXT_PP(3));
    BM25AnalyzerConfig  cfg;

    debug_cfg_from_args(&cfg, tokenizer, stopwords, language);

    PG_RETURN_ARRAYTYPE_P(bm25_tokenize_to_array(&cfg, in));
}

/* bm25_debug_analyze_positions(text, tokenizer, stopwords, language) -- the POSITION of
 * each token bm25_analyze emits, as an int[] parallel to bm25_debug_tokenize's text[].
 *
 * Exists because TEXT-05 was invisible without it. 65_multilexeme_tokens has exercised
 * the compound-splitting path since it was written, but asserts only token COUNTS -- so
 * the one dimension that was wrong (position advancing per lexeme instead of per run)
 * had no assertion anywhere. A probe that returns the tokens but not their positions
 * cannot distinguish the two behaviours. Development and regression testing only. */
PG_FUNCTION_INFO_V1(bm25_debug_analyze_positions);
Datum
bm25_debug_analyze_positions(PG_FUNCTION_ARGS)
{
    text               *in        = PG_GETARG_TEXT_PP(0);
    char               *tokenizer = text_to_cstring(PG_GETARG_TEXT_PP(1));
    char               *stopwords = text_to_cstring(PG_GETARG_TEXT_PP(2));
    char               *language  = text_to_cstring(PG_GETARG_TEXT_PP(3));
    BM25AnalyzerConfig  cfg;
    BM25Token          *toks;
    int                 ntok, i;
    Datum              *elems;
    ArrayType          *arr;

    debug_cfg_from_args(&cfg, tokenizer, stopwords, language);

    ntok  = bm25_analyze(&cfg, VARDATA_ANY(in), (int) VARSIZE_ANY_EXHDR(in), &toks);
    elems = (Datum *) palloc(sizeof(Datum) * Max(ntok, 1));
    for (i = 0; i < ntok; i++)
        elems[i] = Int32GetDatum((int32) toks[i].pos);

    arr = construct_array(elems, ntok, INT4OID, sizeof(int32), true, TYPALIGN_INT);
    PG_RETURN_ARRAYTYPE_P(arr);
}
