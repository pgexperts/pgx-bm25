---
id: 0046
title: Tokenize and fold by character, not by byte
date: 2026-08-10
status: Accepted
summary: The analyzer splits word runs with a multibyte-aware character scanner and folds with str_tolower at emit time, replacing four independent byte-wise approximations that disagreed with each other and produced different index content on macOS than on Linux.
---

# 0046. Tokenize and fold by character, not by byte

## Context

The analyzer decided two things about every byte of every document: is this a word
character, and what is its lowercase. Both answers came from single-byte,
`LC_CTYPE`-sensitive libc calls — `isalnum()` and `tolower()` — applied one byte at
a time to a string that is usually not one byte per character.

Four places made those decisions independently, and each had drifted:

| Site | Test |
|---|---|
| `bm25_tokenize.c` `is_word_char` | `isalnum(byte)` |
| `bm25_tokenize.c` (both tokenizers) | `tolower(byte)` over a working copy |
| `bm25_seg_read.c` wildcard expander | `tolower(byte)`, under a comment asserting "tolower per byte == the analyzer" |
| `bm25_snippet.c` `is_word_byte` | `!IS_HIGHBIT_SET(c) && isalnum(c)` |

Issue #60.7 filed this as a folding defect, and #65.2 as a locale-sensitivity defect
in the language-name fold. Measuring it against a live server showed the folding half
was the smaller half, and that the failure mode depends on the platform's C library:

- **BSD/macOS, UTF-8 `LC_CTYPE`.** The single-byte ctype table covers `0x80-0xFF`, so
  `isalnum(0xC3)` is true and `tolower(0xC3)` is `0xE3`: the lead byte of every
  Latin-1-range character is folded as though it were the Latin-1 character `Ã`.
  Continuation bytes are not alnum, so the run was also cut mid-character. The
  surviving term was a lone `0xE3`, which is not valid UTF-8. `CREATE INDEX` over
  ordinary accented text failed outright:

  ```
  CREATE INDEX t_bm ON t USING bm25_native (body);
  ERROR:  invalid byte sequence for encoding "UTF8": 0xe3
  ```

  The error is raised inside `ts_lexize`, which validates its input before stemming.
  The extension could not index non-ASCII text at all in such a database.

- **glibc, any `LC_CTYPE`.** The single-byte tables for multibyte locales are
  ASCII-only, so `0x80-0xFF` are simply not alnum. No corruption and no error —
  instead every accented word was silently shredded into its ASCII fragments.
  `'café Ärger'` indexed as `{caf, rger}`, and, worse, two genuinely different words
  collapsed onto one term: `Ärger` and `Örger` both became `rger`, merging their
  postings and returning each other's documents.

So the same source produced two different indexes depending on the build host, and
neither matched what the extension claims. CI runs Linux, which is precisely why the
hard failure had never been seen: the platform that errors is the one nobody tests on.

Two further facts made "leave it ASCII-only" untenable. The extension advertises the
whole Snowball language set through the `language` reloption, and German, French,
Russian and Spanish corpora are non-ASCII by construction — a `language='german'`
index that drops every umlaut is not a German index. And PostgreSQL's own text search,
running in the same databases, already handles this correctly: `to_tsvector` returned
whole words in both a C-`LC_CTYPE` and a UTF-8-`LC_CTYPE` database, differing only in
how far it case-folded them.

## Decision

**Word runs are measured in characters and folded at emit.** Three helpers in
`bm25_analyzer.c` are now the only place the extension answers either question, and
every path that splits text or builds a dictionary key routes through them:

- `bm25_next_char(p, remaining, *is_word)` returns the byte length of the character at
  `p`, clamped to `remaining`, and classifies it. A multibyte character is a word
  character; a single byte defers to `bm25_is_word_byte`.
- `bm25_is_word_byte(c)` — in a multibyte server encoding every non-ASCII byte is part
  of a word; in a single-byte encoding the locale's `isalnum` decides, because there a
  high byte *is* a whole character.
- `bm25_fold_term(s, len, *foldedlen)` — `str_tolower` under the database default
  collation, which is the same fold `dict_snowball` applies to its own input.

The multibyte rule is stated unconditionally rather than delegating to `iswalnum`,
which makes run-splitting independent of `LC_CTYPE`. It matches the rule core FTS's
default parser documents for the C locale ("any non-ascii symbol with multibyte
encoding ... is an alpha character").

**The fold moved from before the split to after it.** `bm25_analyze` keeps a verbatim
NUL-terminated working copy, splits on those original bytes, hands raw runs to
`ts_lexize` (the dictionary folds its own input), and folds only on the
`stopwords='none'` surface path — the one path that writes a dictionary key without
passing through `ts_lexize`. This is not a stylistic preference: a fold is not
byte-length preserving, so folding first destroys the `src_off`/`src_len` spans that
`bm25_snippet` measures against the original text. Folding at emit makes those spans
exact by construction rather than by an invariant that has to be maintained.

**`bm25_lower_ascii` is a true `'A'..'Z'` fold** (#65.2). Its result feeds the
dictionary-name lookup *and* the on-disk fingerprint, so it must be a mapping every
server reproduces; `tolower()` is not one.

**The analyzer fingerprint gains a fifth component, `BM25_ANALYZER_REVISION`, now 2.**
The gate refuses a scan whose analyzer would tokenize differently from the one that
built the index, but its four existing components identify the analyzer's
*configuration* — and this change altered the analyzer's *output* with every reloption
identical. A pre-0046 index would therefore have hashed to the same fingerprint,
passed the gate, and silently returned nothing for its accented terms: precisely the
failure the gate exists to prevent. With the component appended (never inserted;
components 1-4 keep their positions), such an index fails loud with the existing
`analyzer fingerprint mismatch ... REINDEX` error instead. The constant is bumped only
when `bm25_analyze`'s output changes for fixed reloptions, never for a refactor, since
a needless bump costs every user a REINDEX.

## Alternatives considered

- **Fix only the folds, as filed.** The plan named three fold sites. Swapping them
  leaves `isalnum` intact, so on macOS the run is still cut mid-character and
  `CREATE INDEX` still fails. The filed fix could not reach the defect.
- **Keep the tokenizer honestly ASCII-only** — make `is_word_char` and the fold true
  ASCII everywhere, so behaviour is identical on every platform and locale. This was
  seriously considered and is defensible: it changes nothing on Linux or in a
  C-locale database, and it fixes the macOS failure. Rejected because it makes the
  `language` reloption decorative for every language it exists to serve, and because
  it regresses single-byte-encoding databases, where the locale's `isalnum`/`tolower`
  already handle accented characters correctly.
- **Classify with `iswalnum` on the wide character**, matching core FTS under a real
  locale. Rejected for run-splitting: it would make token boundaries — and therefore
  document lengths and every score derived from them — depend on `LC_CTYPE`. Folding
  already carries a collation dependency that cannot be removed; adding a second one
  to splitting buys only the ability to treat non-ASCII punctuation as a separator.
- **Use `ts_locale.h`'s `t_isalnum` / `lowerstr_with_len`.** Rejected on portability:
  that header's API changed between the two versions this extension supports —
  PG18 replaced the plain classifiers with `_with_len`/`_cstr`/`_unbounded` variants
  and `lowerstr_with_len` is gone. `pg_mblen`, `pg_database_encoding_max_length` and
  `str_tolower` are stable across both.

## Consequences

**Every existing index needs a REINDEX, and says so.** Terms change for non-ASCII text
— what was stored as `rger` is now stored as the whole word, which under a C collation
is `Ärger`, under a folding collation `ärger`, and under a folding collation with the
German stemmer `arg` — and the revision bump means the
fingerprint gate refuses a pre-0046 index on the first scan rather than under-returning
from it. That refusal reaches indexes whose content is pure ASCII and therefore
byte-identical under both analyzers, which is a real cost paid deliberately: the gate
cannot tell from a fingerprint whether a given index happens to contain a non-ASCII
term, and a loud REINDEX for everyone beats a silent wrong answer for some. It is also
cheap here specifically, because `v1.0.0` is not yet tagged and there are no deployed
indexes to disturb. Had this landed after a release, the trade would have deserved its
own decision.

Pure-ASCII *tokenization* is unaffected — all 87 pre-existing suites pass unchanged,
which is the scope statement for the tokenizer change itself. On macOS with a UTF-8
`LC_CTYPE` there is nothing to reindex at all, because such an index could not be
built in the first place.

**A collation-provider change is now a REINDEX event**, the same way it already is for
a collated btree. This is not new exposure — `dict_snowball` has always folded through
the same machinery, so any index using a stemmer already carried it — but it is now
worth stating, because the fold reaches terms the dictionary never saw.

**Cross-case matching of non-ASCII depends on the database's collation.** Under a C
collation `str_tolower` folds ASCII only, so `Ärger` is stored with its capital and a
query for `ärger` will not find it. Under a UTF-8 collation both fold and it matches.
This is exactly `to_tsvector`'s behaviour in the same databases, and it is a property
of the database rather than of this code — which is why `sql/82_encoding_aware_tokens`
asserts only properties that hold under both, and never pins a folded term.

**A language reloption containing non-ASCII bytes is passed through unfolded.** The
`'A'..'Z'` fold deliberately does not touch them, so such a value must be spelled
identically at `CREATE INDEX` and at scan time or the fingerprint gate will report a
mismatch. Snowball language tags are ASCII, so this costs nothing in practice. The
Turkish-`LC_CTYPE` failure #65.2 described — `language='ENGLISH'` resolving to
`englısh_stem` — is closed by the same fold.

**On PG17 the surface fold and the dictionary's own fold are not the same call, and
the tests cannot prove they agree.** `dict_snowball` does not expose its fold. PG18
uses `str_tolower` under the default collation — identical to `bm25_fold_term`, so
parity there is by construction. PG17 uses `lowerstr_with_len`, which is `towlower()`
over the database `LC_CTYPE` and ignores the default collation's provider. The two
agree for a database created the ordinary way, where collation and ctype come from one
locale under libc, and can diverge on PG17 under an ICU or builtin default collation.

An earlier draft of this record claimed `sql/82` would catch such a divergence in CI.
It would not, and the claim is withdrawn: CI's clusters are C-locale, where both folds
are the identity on non-ASCII and the divergence is invisible. No ICU configuration is
tested. The honest position is that this is an untested narrow configuration —
PG17 + a non-libc default collation + non-ASCII terms — in which the `stopwords='none'`
surface path and the wildcard expander can write or look up keys the stemmed path did
not produce, giving a silent no-match.

A related hole is deliberately left open: `pg_upgrade` from 17 to 18 changes the
dictionary's effective fold without changing any fingerprint input, so the gate passes
over an index whose terms the new server may no longer reproduce. Encoding the server
major in `BM25_ANALYZER_REVISION` would close it at the cost of forcing a REINDEX on
every major upgrade for everyone, which is the wrong trade for a configuration this
narrow. It is the same exposure a collated btree carries across a collation-library
change, and the same remedy applies: reindex collation-dependent indexes after one.

**A transliterating stemmer still defeats an accented wildcard prefix, and folding
makes that more visible rather than less.** Wildcards deliberately bypass the stemmer
(the M6 trade-off: stemming `judg*` produces nonsense), so a pattern is matched against
STEMMED dictionary bytes. Where the fold reaches non-ASCII, it also hands the stemmer a
word its rules recognise — and the German stemmer then transliterates, storing `arg`
for `Ärger`. No accented prefix matches that. So on a folding database with
`language='german'`, `bm25_wildcard('body','ärg*')` returns nothing while the exact term
query returns the row. This is not new and not a folding defect — the same thing happens
in ASCII, where `runnin*` misses a document stored as `run` — but it is the reason
`sql/82` uses `english`, whose stemmer leaves these words intact, and the reason to be
careful about promising "accented wildcards now work". They work exactly as far as the
chosen stemmer leaves the prefix's characters alone.

**Non-ASCII punctuation now joins adjacent words.** Treating every multibyte
character as a word character means a curly apostrophe or a guillemet is not a
separator, so `don’t` is one token. Core FTS splits there under a real locale and
does not under C. Accepting it is the price of locale-independent splitting; if it
ever needs to change, it changes in `bm25_is_word_byte` alone.

## Addendum (2026-10-05)

Single-byte encodings no longer treat every high byte as a separator (#295, ADR 0114,
analyzer revision 6). A high byte is a word byte when a generated, per-encoding table says the
encoding maps it to a letter or digit; unmapped bytes are separators, and SQL_ASCII high bytes
are word bytes. Run splitting stays a pure function of the database encoding. Case folding is
unchanged, at core parity.
