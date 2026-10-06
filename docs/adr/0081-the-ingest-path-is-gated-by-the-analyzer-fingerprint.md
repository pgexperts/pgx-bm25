---
id: 0081
title: The ingest path is gated by the analyzer fingerprint, not by a restricted search_path
date: 2026-08-23
status: Accepted
summary: bm25_insert resolved the analyzer under the caller's search_path and tokenized with it while nothing compared the result against meta.analyzer_fingerprint, so an INSERT under a shadowing dictionary — or after a language reloption edit — durably stored terms the index's own analyzer does not produce; the fix reads the metapage before tokenizing and calls the existing bm25_fingerprint_gate with an INGEST site, rather than pinning name resolution the way CREATE INDEX does, because the reloption vector is invisible to name pinning.
---

# 0081. The ingest path is gated by the analyzer fingerprint, not by a restricted search_path

## Context

`bm25_fingerprint_gate` existed since M3 and had scan-side callers only: the ranked
prologue `bm25_scan_corpus_stats` (`src/bm25_scan.c:1762`), the boolean `@@@` filter path
`bm25_load_if_needed` (`src/bm25_scan.c:4012`), and the `bm25_debug_fingerprint_gate`
probe. Two real read sites, not one — `docs/adr/0053`'s table has this right and is worth
trusting over any prose that says otherwise.

`bm25_insert` resolved the index's analyzer with `bm25_analyzer_config` and tokenized
every indexed column through it, and nothing compared the fingerprint of that resolved
config against the one `ambuild` stamped on the metapage. The code said so in its own
comment (`src/bm25_build.c`, "NOT gated … the invariant above is asserted, not
enforced"), and `docs/adr/0080`'s Consequences named it as out of scope for that record.

The asymmetry that makes this reachable is PG17's `RestrictSearchPath`. `CREATE INDEX`
and `REINDEX` run under a restricted `pg_catalog, pg_temp` path, so a build always binds
`pg_catalog`'s `<lang>_stem` and stamps the fingerprint it actually used. `INSERT` is
not one of the commands that restriction covers, so `bm25_insert` resolved
`"<language>_stem"` through the **caller's** `search_path`.

Two independent vectors reach the same corruption, and the second one is why the shape
of the fix matters:

1. **Dictionary shadowing.** `SET search_path = sh, public, pg_catalog` — with
   `pg_catalog` listed explicitly *last*, since it is searched implicitly first
   otherwise — makes `sh.english_stem` win. The INSERT then stores that dictionary's
   stems.
2. **A reloption edit, with no shadowing at all.** `language` is registered as a plain
   string reloption with no validator (#148, HDL-07), and `AccessExclusiveLock` on
   `ALTER INDEX … SET` sets the lock level rather than prohibiting the change. So
   `ALTER INDEX t_bm SET (language = 'french')` is accepted, and the next INSERT writes
   French stems into an English index.

Both are *durable*. This is the whole reason the defect was rated above the scan-side
case it resembles: a wrongly-analyzed scan returns bad results for one session and the
next session is fine, while a wrongly-analyzed insert commits terms the index's own
analyzer does not produce, the next seal folds them into a published segment, and the
row stays invisible to every correct query until `REINDEX`. Fixing the `search_path`
afterwards recovers nothing.

`ambuild` was verified to need nothing, empirically rather than by assumption: under a
`search_path` whose canary confirms `english_stem` resolves to a shadow schema,
`CREATE INDEX` still stamps a fingerprint that a recompute from a clean path matches
exactly, and the rows it indexed are findable from a clean path. The build is
self-consistent by construction. The write path was the only ungated one that a
non-privileged user reaches.

## Decision

**Gate `bm25_insert` against `meta.analyzer_fingerprint` using the existing
`bm25_fingerprint_gate`, reading the metapage before tokenizing so the error precedes
the work.**

Four sub-decisions come with it.

**The gate stays one function.** It gained a `BM25GateSite` parameter
(`BM25_GATE_SCAN` / `BM25_GATE_INGEST`) and moved from `bm25_scan.c` to
`bm25_analyzer.c`, where both of its operands (`bm25_analyzer_fingerprint`,
`bm25_require_analyzer_match`) already live — leaving it in `bm25_scan.c` would have
required `bm25_build.c` to include a scan-private header to reach a predicate about the
analyzer. The comparison and the `require_analyzer_match` → severity decision are
written once and shared; `site` selects nothing but the message. #157 is this project's
own record of a predicate written twice and then drifting, and the point of a single
function is that a future change to the comparison cannot fix one path and miss the
other.

**The messages differ anyway, because the consequence of proceeding does.** The scan
site keeps its existing wording verbatim (suites 26 and 101 pin it). The ingest site
says plainly that rows are being stored under a mismatched analyzer and that what is
written persists until `REINDEX`. A message that told the two apart only by the word
"query" would understate the second by a lot.

It hedges on one thing, deliberately: **"may not be findable", not "will not"**. The
gate compares fingerprints; it cannot compare tokens. A fingerprint moves both for a
genuinely different stemming dictionary (tokens diverge, the rows really are lost) and
for a re-encoding of an existing component — ADR 0080's own `stemmer_id` change is
exactly the latter, where tokenization is byte-identical and the rows are perfectly
findable. The first draft of this record shipped the strong claim, which was false for
precisely the transition the warning exists to serve. The `errdetail` explains which
case is which; the primary line refuses to guess. The same correction applies to the
all-NULL row, which contributes no postings at all and so cannot have "terms this
index's analyzer does not produce".

**`require_analyzer_match = false` is still honored on the write path** — WARNING, then
proceed, not a hard error. `docs/adr/0080` recommends exactly that setting as the safe
deferral for the #62 transition, on the ground that tokenization is byte-identical
across it; hard-erroring on ingest regardless would turn every table mid-transition
read-only, which is a worse failure than the one being prevented. The escape hatch
stays, it just stops being quiet.

**And on that path the WARNING is memoized per `(index OID, local transaction id)`.**
The scan gate fires once per scan; this one is on a per-row path, so unmemoized the
recommended deferral turns `INSERT INTO t SELECT … FROM huge` into N client warnings
*and* N server log lines (`log_min_messages` defaults to `warning`). A routine bulk load
could fill a log volume — plausibly worse than the outage the setting exists to defer.
A single-entry static memo keyed on `MyProc->vxid.lxid` is enough, because this is
warning suppression and not correctness: the key resets on every transaction start with
no hook to register, a missed suppression costs one extra line, an evicted entry costs
one skipped line, and the ERROR path — the one that actually protects the index — is
never memoized. The message says it covers the whole transaction, so a single line is
not misread as a single row.

## Alternatives considered

- **A `RestrictSearchPath` equivalent for ingest** — pinning `"<language>_stem"` to
  `pg_catalog` the way a maintenance command does. Rejected as a half fix: it
  constrains *name resolution* only, so vector 2 above (the reloption edit) is entirely
  invisible to it, and vector 2 is the one that needs no privilege beyond owning the
  index. One fingerprint comparison covers both vectors with one mechanism, and covers
  any future analyzer component the same way for free.
- **Caching the validated fingerprint in `rd_amcache`** to avoid a per-row metapage
  read. Rejected *for now*, and on grounds of proportion rather than principle: there
  is currently zero `rd_amcache` use anywhere in this tree, so this would introduce a
  new invalidation-correctness surface to save one hot-buffer read on a path that
  already pays a full `bm25_analyzer_config` plus a `ts_lexize` per token per row. If
  the metapage read ever shows up in a profile, that is the time to revisit it — with a
  measurement.
- **A hard ERROR on ingest regardless of `require_analyzer_match`.** Rejected: see the
  third sub-decision above. Correct in isolation, wrong in the presence of ADR 0080's
  recommended deferral.
- **Gating at seal time instead of insert time.** Rejected for the same reason the
  BUILD-05 `key_field` check (`src/bm25_build.c`) is at insert time and not seal time:
  by seal time the divergent rows are already in the pending chain, so the only options
  are publish (corruption) or error — and erroring wedges the index, since every later
  seal, autovacuum's included, hits the same rows and fails the same way, with REINDEX
  the only exit. Failing the INSERT keeps the chain homogeneous.

## Consequences

- **An INSERT under a shadowing `search_path`, or after a `language`/analyzer reloption
  edit, now fails with `ERRCODE_FEATURE_NOT_SUPPORTED` instead of silently corrupting
  the index.** Under `require_analyzer_match = false` it warns once per transaction and
  proceeds, and the warning states the durability explicitly.
- **An un-migrated index is now effectively read-only, not merely unreadable.** Any
  fingerprint-moving change refuses every `INSERT` and every non-HOT `UPDATE` as well as
  every scan, and only `REINDEX` restamps `analyzer_fingerprint` — `bm25_upgrade()`
  never touches it. Both upgrade notes in `README.md` promised refusal "on its first
  *scan*", which would have let a reader size an upgrade window for degraded queries and
  get a write outage; both now say so, and point at the `require_analyzer_match = false`
  deferral and its one-warning-per-transaction cost.
- **For `UPDATE`, whether the refusal fires is nondeterministic from the application's
  point of view.** During a mismatch, an `UPDATE` touching only non-indexed columns
  succeeds when the row update is HOT (no `aminsert` call at all) and fails when page
  space forces a non-HOT update. That is defensible — the gate cannot fire on a code
  path that never runs — but "the same statement works sometimes" is a support-ticket
  generator, and it is better written down here than rediscovered.
- **`ERRCODE_FEATURE_NOT_SUPPORTED` on a write refusal makes applications string-match.**
  `0A000` cannot distinguish "this index needs a REINDEX" from any other unsupported
  feature, so a caller that wants to retry-after-reindex has to match on the message
  text — which is exactly what `sql/102` itself has to do. Kept anyway, for consistency
  with the scan gate that has used this code since M3; a dedicated code would be a
  separate, cross-cutting decision. Recording the cost rather than pretending it is free.
- **The format-version floor now binds earlier on the insert path.** `bm25_meta_read`
  runs `bm25_meta_validate`, so `aminsert` now meets the `BM25_OLDEST_READABLE` gate
  before tokenizing and before the all-NULL early-out, where it previously first met it
  inside `bm25_pending_append_multi`. Consequence: an all-NULL row into an index below
  the floor used to return `false` without validating anything and now errors. The right
  answer either way; it just arrives for a row that used to slip past. The comment in
  `src/bm25_pending.c` that claimed "aminsert reads no validated metapage before this
  point" was true when written and is now updated.
- **No insert benchmark exists to record a delta against.** `bench/index_build.sh` times
  `CREATE INDEX` (the `ambuild` path) and populates its heap *before* the index exists,
  so it never calls `aminsert`; `wand_vs_exhaustive.sh` and `decoupled_score_hash.sh`
  are read-path. The numbers below therefore come from ad-hoc `INSERT … SELECT` timings
  rather than from `bench/`, and "dominated by `ts_lexize`" rests on the isolated
  upper-bound measurement rather than on a checked-in harness. Adding an ingest
  benchmark to `bench/` would be the honest follow-up.
- **`bm25_insert` pays one additional metapage read per row** — `ReadBuffer` on block 0,
  a `BUFFER_LOCK_SHARE`, a `memcpy` of the struct, and `bm25_meta_validate`. Block 0 of
  an index being inserted into is as hot as a buffer gets, and the same insert already
  reads it (under `EXCLUSIVE`) inside `bm25_pending_append_multi` and again in
  `bm25_pending_should_seal`, so this is a third such read rather than a first. Measured
  two ways on a `--enable-cassert` / `-Og` PG 18.3 build, which inflates buffer-manager
  and lock costs relative to a production build:
  - *Isolated upper bound.* `bm25_stats`, a strict superset of the added
    `bm25_meta_read` (it opens the relation and reads the segment catalogue too), costs
    ≈1.9 µs per call (200 000 calls, 406 ms, minus a 21 ms `generate_series` baseline).
    Against a per-row insert cost of ≈13 µs for 12-token documents and ≈60 µs for
    120-token ones, that bounds the addition at ≈15 % in the unrealistic short-document
    case and ≈3 % at a realistic document size.
  - *End-to-end A/B* against a binary built from a reverted `src/`, bulk
    `INSERT … SELECT`: **inside run-to-run noise in both workloads.** Worth recording
    how that conclusion was reached, because the first attempt got it wrong: measuring
    the two binaries in separate blocks minutes apart showed a clean-looking +22 %, and
    re-measuring them adjacent in time showed the gated build *faster* on the short-
    document workload. The apparent regression was thermal/ordering drift on the host,
    which is precisely why this project does not gate CI on benchmark numbers from
    shared hardware.

  This is still a real per-row cost and not a per-scan one, so it belongs in the record
  even though it did not rise above noise. `rd_amcache` is the mitigation if a profile
  ever shows it.
- **An all-NULL row is now refused too**, where it previously contributed no postings
  and was harmless. Deliberate: the error has to precede the tokenize loop, which is
  where "did any column produce tokens" is decided; and a gate that fires on some rows
  and not others is harder to diagnose than one that fires on all of them. A mismatched
  analyzer means the index is misconfigured for this session, and every non-NULL row
  behind the all-NULL one would fail anyway.
- **The gate moved translation units.** `bm25_fingerprint_gate` now lives in
  `bm25_analyzer.c` and is declared in `bm25.h`; `bm25_scan.h` no longer declares it,
  and `bm25_scan.c`'s file header now names three shared scan-start helpers rather than
  four. The `bm25_debug_fingerprint_gate` probe still drives the real function, on
  `BM25_GATE_SCAN`, and suite 26's expected output is unchanged because the scan-side
  message text is byte-identical.
- **The ingest WARNING's primary line deliberately carries no fingerprint integers**;
  they live in `DETAIL`. A WARNING cannot be caught by a plpgsql handler, so a suite can
  only observe it in raw output, and the two integers fold the database encoding — which
  would make the expected file environment-dependent. This is also just the PG message
  style (short primary line, diagnostics in `DETAIL`), and `\set VERBOSITY terse` drops
  the `DETAIL`. The two ERROR variants keep their integers, because a plpgsql handler
  hides them — and their `DETAIL`/`HINT`, being integer-free, are themselves pinned by
  `sql/102` via `GET STACKED DIAGNOSTICS`, so a wording regression or a `DETAIL`/`HINT`
  swap fails the suite rather than shipping.
- **What is still not covered is unchanged and is now the only entry on that list**: a
  dictionary's OPTIONS changing under a stable qualified name and template
  (`ALTER TEXT SEARCH DICTIONARY … (StopWords = …)`) moves no fingerprint component and
  is invisible to both sides. See the "STILL NOT COVERED" comment in
  `src/bm25_analyzer.c`.
- **`bm25_debug_pending_append` remains ungated and is deliberately left that way.** It
  is the tree's only other write path that tokenizes, but it calls `bm25_tokenize` (the
  raw ASCII splitter) and never resolves an analyzer config at all, so there is no
  resolved fingerprint for a gate to compare — it is *unconditionally* analyzer-divergent
  by construction, which is what its suites depend on. It is `REVOKE`d from `PUBLIC` and
  ownership-gated. Making it analyzer-faithful is a separate change with its own blast
  radius across existing expected output, and is not this record's decision.
- **`sql/102_ingest_fingerprint_gate` is the regression guard**, covering both vectors,
  the `require_analyzer_match = false` warning, the "stored but unfindable" outcome that
  warning describes *in the different-stemmer case*, and the `REINDEX` recovery its hint
  promises. It also pins the `ambuild`-is-self-consistent property empirically, with a
  canary proving the shadow is reachable. Three of its assertions fail against a build
  from a reverted `src/`.
- **The memo's semantics are pinned behaviorally, in three cases rather than one.** A
  six-row `INSERT … SELECT` must emit exactly one warning; a following separate statement
  must emit another (without which a memo that never *reset* — suppressing every warning
  after the first, forever — would pass the first case and look correct); and three
  statements inside one explicit transaction must emit one between them. Warning counts
  are plain lines in the expected output, so any of these regressing shows up as a diff.

## Addendum (2026-08-23)

Vector 2 above describes `language` as "a plain string reloption with no validator
(#148, HDL-07)". That is no longer literally true: ADR 0082 gave it a
`validate_string` callback which resolves `<language>_stem` at `CREATE INDEX` /
`ALTER INDEX` time.

**The vector is unchanged, and this record's reasoning still holds.** The validator
only rejects a language whose dictionary is not visible -- `'french'` resolves
perfectly well, so `ALTER INDEX t_bm SET (language = 'french')` is still accepted
and still moves the analyzer under a built index. Validation answers "does this
name exist"; the fingerprint gate answers "does this index's on-disk content agree
with the analyzer this INSERT is about to use", which is a different question and
the only one that catches a legal edit. What the validator changed is a narrower
case not discussed here: `SET (language = 'klingon')` used to commit and leave the
index unreadable at the next scan, and now fails at the statement.

## Addendum (2026-10-04)

The Context says `AccessExclusiveLock` on `ALTER INDEX ... SET` sets the lock level
for the `language` reloption. That holds only in a backend that has already registered
the option: bm25 registers its reloptions lazily, on the first `bm25_options` call in a
backend, and core takes an ALTER's lock level from the options registered in the backend
running it, so a backend that has not loaded a bm25 index's relcache entry takes only
ShareUpdateExclusiveLock for every bm25 reloption, `language` and `analyzer` included
(ADR 0012's 2026-10-04 addendum has the detail). The decision here does not depend on
the lock level. The fingerprint gates, at scan start and at ingest, error by default and are the
defence against the analyzer moving under an index.
