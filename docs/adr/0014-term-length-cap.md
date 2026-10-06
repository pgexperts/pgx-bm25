---
id: 0014
title: Cap indexed term length at 2047 bytes and drop over-long tokens with a NOTICE
date: 2026-07-29
status: Accepted
summary: Terms longer than BM25_MAX_TERM_BYTES (2047) are dropped at tokenization with a NOTICE, matching core FTS, and chain_ensure enforces the one-record-per-page bound as a runtime error rather than an Assert.
---

# 0014. Cap indexed term length at 2047 bytes and drop over-long tokens with a NOTICE

## Context

A dictionary entry stores its term **inline**: the DICT writer emits
`MAXALIGN(sizeof(BM25DictEntry) + termlen)` bytes and `chain_write` requires the
whole record to fit one segment page. That precondition was stated only as an
`Assert`, which is compiled out of every production build, and `chain_ensure`'s
fresh-page branch returned unconditionally without re-testing the request against
the new page's 8144-byte capacity. Nothing anywhere upstream bounded `termlen`:
the tokenizer splits on `isalnum` runs with no maximum, Snowball stemming does not
shorten a run that has no recognizable suffix, and the accumulator's
`ACCUM_KEY_MAX` truncates only the *hash key*, never the stored term.

So `CREATE INDEX` over a row containing a single ~20 KB alphanumeric run —
a base64 or hex blob, a DNA string, a machine-generated identifier — copied about
12 KB past the end of an 8 KB shared buffer and left `pd_lower` far beyond
`pd_upper` for `GenericXLogFinish` to WAL-log. Ordinary user SQL over
uncorrupted data; no special privilege needed (review ref C1, issue #32).

Doing nothing was not an option, and neither was fixing only the writer: an error
raised deep inside the segment writer names a page-capacity limit the user cannot
act on, and `e->termlen = (uint16) termlen` would silently truncate long before
that error fired.

## Decision

We will cap terms at **`BM25_MAX_TERM_BYTES` = 2047** and enforce it in two
independent places:

1. **Where tokens are produced** (`bm25_analyze`): a run longer than the cap is
   *dropped* — not truncated, not fatal — with
   `NOTICE: bm25: word is too long to be indexed` /
   `DETAIL: Words longer than 2047 bytes are ignored.` The raw run is checked
   before `ts_lexize` (so a 20 KB run costs no stemming), and each emitted lexeme
   is re-checked afterwards, because `stem_dict_oid` is user-chosen and a
   thesaurus/synonym dictionary can emit a lexeme *longer* than its input.

2. **In the writer** (`chain_ensure`): a `need` above `CHAIN_PAGE_CAPACITY` is an
   `ERRCODE_PROGRAM_LIMIT_EXCEEDED` error. This is a trust boundary, not an
   assertion — it holds in production builds, and it still catches a pre-cap
   index being merged forward.

2047 is not arbitrary: it is exactly core full-text search's own limit. A corpus
that survives `to_tsvector` therefore survives this index, with the same wording
and the same drop-not-fail behaviour.

## Alternatives considered

- **Truncate the term to the cap** — cheapest diff, but it silently merges
  distinct blobs into one dictionary entry and makes `df` and every BM25 score
  quietly wrong. Rejected: a wrong score is worse than a missing token.
- **`ereport(ERROR)` on an over-long token** — fail-loud is this project's usual
  stance, but here it makes `CREATE INDEX` refuse an entire legitimate corpus over
  one unsearchable blob in one row. Core FTS declines to do this, and so do we.
- **Cap only in `chain_write`/`chain_ensure`** — closes the memory-safety hole but
  surfaces as an unactionable page-capacity error from inside the segment writer,
  and leaves the `uint16` truncation in place on the pending path.
- **Cap in `accum_find_or_add_term`** (the accumulator choke point) — it is a
  genuine single choke point, but it also sits on the *merge* path, where terms
  come from existing segments rather than user text; dropping or erroring there
  would corrupt or block a merge of an index built before this cap. The tokenizer
  is the real trust boundary.

## Consequences

- A document with an over-long run still indexes; only that token is unsearchable.
  Users get a NOTICE per occurrence, which on a blob-heavy `CREATE INDEX` is
  noisy — the same noise core FTS produces, accepted for consistency.
- `BM25_MAX_TERM_BYTES` is now a compatibility surface: raising it is safe (older
  indexes stay readable), lowering it would strand terms in existing segments.
  It is deliberately below the point where the on-disk `uint16 termlen` or the
  page bound could be reached, so neither becomes a second implicit limit.
- `bm25_debug_terms` aggregates through a small fixed-width hash key and fails
  loud on terms near the cap. That limit is debug-only and pre-existing; the new
  `sql/59_term_length_cap` suite therefore proves round-tripping with `@@@`
  probes rather than dictionary introspection.
- The `Assert` in `chain_write` is gone. Any future chain record whose size is
  derived from user data now gets a real error instead of an out-of-bounds write.
