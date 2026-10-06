---
id: 0080
title: The analyzer fingerprint identifies the stemmer by schema-qualified name and template, not by dictionary OID
date: 2026-08-23
status: Accepted
summary: stemmer_id becomes an FNV-1a hash of the resolved ts dictionary's schema-qualified name plus its template's, replacing the raw pg_ts_dict OID, so a pg_upgrade that renumbers english_stem no longer makes every index fail the analyzer fingerprint gate and demand a REINDEX that would rebuild byte-identical segments; this is a false-positive fix only — shadow detection was already exact, up to 32-bit hash collision and one newly-invisible superuser DROP+CREATE case — and the re-encoding rides BM25_ANALYZER_REVISION 4.
---

# 0080. The analyzer fingerprint identifies the stemmer by schema-qualified name and template, not by dictionary OID

## Context

The analyzer fingerprint's first component, `stemmer_id`, was the raw `pg_ts_dict`
OID that `"<language>_stem"` resolved to (`bm25_analyzer.c`,
`out->stemmer_id = (uint32) out->stem_dict_oid`). The fingerprint is recomputed from
the index's own reloptions at the start of *every* scan and compared against the value
the build stamped on the metapage (`bm25_fingerprint_gate`), so whatever `stemmer_id`
is made of, that is what the gate is actually comparing.

An OID is the wrong thing to compare, and it fails in exactly one direction — which is
worth stating carefully, because the broader version of the complaint is false and was
written into the first draft of this record.

**It is not stable, so the gate raised false alarms after `pg_upgrade`.** The core
Snowball dictionaries are created by initdb's `snowball_create.sql` with ordinary DDL,
so their OIDs come off the OID counter and are not pinned catalog constants — the tree
already said so, and `expected/12_meta_v4.out` already noted the fingerprint "folds the
english_stem dict OID (which differs across PG majors)". `pg_upgrade` carries index
files into the new cluster *without rebuilding them*, while the new cluster's initdb
numbers `english_stem` on its own counter. Every index's stored fingerprint therefore
met a recomputed one derived from a different number. Under the default
`require_analyzer_match = true` that is an `ERRCODE_FEATURE_NOT_SUPPORTED` on the first
query against every bm25 index in the database, demanding a REINDEX that would have
produced byte-identical segments — the analyzer had not changed at all.

**Only `pg_upgrade`.** A logical dump/restore was never affected, and saying otherwise
overstates the defect by a factor of two: `pg_dump` emits the index as
`CREATE INDEX … USING bm25_native (…)`, so the restore re-runs `ambuild` and re-stamps
`meta.analyzer_fingerprint` from the restoring cluster's own catalog
(`src/bm25_build.c`). Stored and recomputed move together.

**It is NOT a detection weakness.** Resolution is by an *unqualified* name through the
caller's `search_path`, which the build side cannot follow: since PG17, `CREATE INDEX`
and `REINDEX` run under a restricted `pg_catalog, pg_temp` path (PG17 release notes,
"Change functions to use a safe `search_path` during maintenance operations", which
names both commands; confirmed empirically on 18.3), so a build always binds
pg_catalog's dictionary while a scan binds whatever the session reaches. A session that
puts its own `english_stem` ahead of pg_catalog tokenizes through *that* dictionary —
and the OID scheme **always caught that**, because `pg_ts_dict.oid` is unique and two
different dictionaries can never share one. An OID's weakness is one-directional: it
cannot distinguish "a different dictionary" from "the same dictionary, renumbered", so
it over-fires and never under-fires. The qualified name distinguishes the two. That is
the entire delta, and it is a false-positive fix, not a coverage fix.

The module header documented the search_path exposure honestly and ended "Documented,
not defended against." `src/bm25.h` meanwhile asserted the config was a function of the
reloptions plus a catalog lookup whose OIDs were stable. Both statements were about the
same fact, and only one of them was true.

## Decision

`stemmer_id` becomes

```
FNV1a32( dict_nspname ‖ NUL ‖ dictname ‖ NUL ‖ tmpl_nspname ‖ NUL ‖ tmplname ‖ NUL )
```

computed by `bm25_stemmer_identity` from the `pg_ts_dict` / `pg_ts_template` syscache
tuples, reusing the same FNV-1a helper and offset basis as `stopword_set_hash`. Every
part is NUL-terminated so `("ab","c")` and `("a","bc")` cannot collide. Component order
inside `bm25_analyzer_fingerprint` is untouched: this is the same slot with the same
width, only derived differently.

The dictionary OID is still resolved and still carried in `cfg->stem_dict_oid`, because
`ts_lexize` needs a real dictionary. The OID is the *handle*; the hash is the *identity*.

The template is hashed alongside the name because the name alone does not fix behavior:
`DROP` + `CREATE` at the same qualified name with `TEMPLATE = simple` instead of
`snowball` is a different analyzer, and identity must move when it does.

`BM25_ANALYZER_REVISION` goes 3 → 4, and its contract is widened from "bump when
`bm25_analyze`'s output changes" to "bump when the output changes **or** when an
existing component's value is re-encoded". Once a fingerprint is on disk the two cases
are indistinguishable — both are a stored number that no longer matches — so they need
the same signal.

## Alternatives considered

- **Pin resolution instead: qualify the name, or store the resolved OID in the
  metapage.** This is the fix for a different defect — it would stop a shadowing
  dictionary from being *used*, where this record only makes sure the disagreement is
  *recorded* honestly. Not taken, on scope: it changes what the `language` reloption
  means, and the shadowing exposure is already refused by the gate on the scan side.
  Note the tempting justification "qualifying would remove the ability to use a
  non-core stemmer" is **not** available: PG17's restricted build-time `search_path`
  already means a dictionary outside `pg_catalog` cannot be bound at `CREATE INDEX` at
  all (verified: `'klingon_stem'::regdictionary` resolves in a session, yet
  `WITH (language='klingon')` fails with "text search dictionary does not exist").
  Storing the resolved OID instead is separately wrong: it makes the index unreadable
  after the `pg_upgrade` this record is about.
- **Include `dictinitoption` in the identity.** It would catch
  `ALTER TEXT SEARCH DICTIONARY english_stem (StopWords = ...)`, which changes behavior
  invisibly today. Rejected: `dictinitoption` is a *reconstructed* option string, and
  hashing text that a dump/restore may re-spell reintroduces precisely the spurious
  REINDEX this record removes. The blind spot is not new — the OID had it too for
  `ALTER` — and trading a known-stable identity for a wider but re-spellable one is the
  wrong direction.
- **Append a seventh fingerprint component instead of re-encoding the first.** The
  "append, never insert" rule protects component *order*; nothing here reorders. An
  appended component would leave the OID still in the hash, so the `pg_upgrade` false
  alarm would survive the fix.
- **Do not bump `BM25_ANALYZER_REVISION`, since tokenization is unchanged.** Tempting,
  and the constant's old wording ("bump on any change to `bm25_analyze`'s output")
  literally says not to. Rejected because the effect is identical either way — every
  stored fingerprint moves — so the only thing skipping the bump would buy is a constant
  whose documented meaning no longer describes what forces a REINDEX. Better to widen
  the contract than to leave the next reader to discover the exception.
- **Teach `bm25_upgrade` to re-stamp `analyzer_fingerprint` in place, avoiding the
  REINDEX.** It is technically possible — the contents really are still valid — but
  `bm25_upgrade` has never touched the analyzer fingerprint (it re-stamps
  `format_version` / `min_read_version` / `feature_flags`), and it cannot tell an index
  stamped by a pre-#62 binary from one whose analyzer genuinely differs. A re-stamp
  would paper over real mismatches. Inventing that machinery for a one-time transition
  is not worth a permanent hole in the gate.

## Consequences

- **Every existing index fails the scan-start gate once and must be reindexed.** This
  follows the precedent set by ADR 0077 (revision 3): the gate raises
  `ERRCODE_FEATURE_NOT_SUPPORTED` with "REINDEX or set require_analyzer_match = false"
  rather than degrading. Verified end to end — an index built by the pre-fix binary
  errors on the first query under the new one, and `REINDEX INDEX` clears it.
- **Unlike ADR 0077, the REINDEX is conservative rather than corrective.** Tokenization
  is byte-identical across this change, so the existing segments are correct and the
  only stale thing is the stamped number. For this specific transition
  `require_analyzer_match = false` is therefore a safe escape hatch (it is not, in
  general, for a revision bump that changed output). Say so in the release note; do not
  weaken the gate to say it.
- **`pg_upgrade` stops being a REINDEX event for this reason.** It may still be one for
  others — a collation-provider change remains one (ADR 0046).
- **A dictionary RENAME or `SET SCHEMA` — or a `SET SCHEMA` on its TEMPLATE — becomes a
  REINDEX event, where it was not before.** This is the cost side of the trade and the
  case where the new identity over-fires and the OID did not: a rename changes no
  behavior at all, yet moves the hash. Reaching it takes a superuser, since a build can
  only bind a `pg_catalog` dictionary — but *not* more than that: initdb's Snowball
  dictionaries are dependency-protected, which is a `CASCADE` hint rather than a
  `pg_depend` pin, so `DROP TEXT SEARCH DICTIONARY pg_catalog.english_stem CASCADE`
  succeeds (verified in a rolled-back transaction on 18.3). Exotic, not impossible. It
  is pinned as intended behavior by `sql/101_stemmer_identity` Parts 3 and 4b, and
  should not be rediscovered as a surprise.
- The gate's *detection* behavior is essentially unchanged, with two qualifiers worth
  stating rather than glossing: the DROP+CREATE-with-different-options case above is
  newly invisible, and a 32-bit FNV-1a can collide where a raw OID was injective.
  Neither is new in kind — the composite fingerprint has always been a 32-bit FNV, so
  gate-level exactness was never absolute, and the gate guards against misconfiguration
  rather than acting as a security boundary. What genuinely changes is that the gate now
  also *stops* firing when the shadow leaves the path, because the identity is a pure
  function of the names.
- **The `INSERT` path remains ungated, and this record does not change that.**
  `bm25_fingerprint_gate` has scan-side callers only, and `INSERT` is not one of the
  maintenance commands PG17 restricts, so `bm25_insert` resolves the analyzer under the
  caller's `search_path`. An insert under a shadowing `<lang>_stem` stores
  wrong-dictionary terms silently and the row is then invisible to a correct query.
  Named here so it is not mistaken for something this change covers.
- Two syscache lookups (`TSDICTOID`, `TSTEMPLATEOID`) plus two `get_namespace_name`
  calls are added to `bm25_analyzer_default_config`. That is **not** only a per-scan
  path: `bm25_insert` (`src/bm25_build.c`) calls it once per inserted row, and
  `bm25_match` (`src/bm25_handler.c`) once per row on a seqscan-evaluated `@@@`. Every
  added read is syscache-backed and the path already did a catalog lookup
  (`get_ts_dict_oid`), so the cost stays far below the `ts_lexize` calls beside it —
  but the call sites are per-row, not per-scan.
- `sql/101_stemmer_identity` is the regression guard. It is the first suite in the tree
  that can tell an OID-derived identity from a name-derived one: two of its assertions
  (`identity_tracks_schema`, `identity_survives_recreate`) return `f` against the
  pre-fix build. Every prior fingerprint suite runs with one candidate dictionary under
  the default `search_path`, where the two schemes are indistinguishable.

## Addendum (2026-08-23)

The Consequences above record that "the `INSERT` path remains ungated, and this record
does not change that." That is no longer the state of the tree: #188 gated it, and
`docs/adr/0081` is the record. Nothing about *this* decision changed — the identity is
still the dictionary's schema-qualified name plus its template's, and this record's
reasoning about `pg_upgrade`, renames and detection stands unaltered. What changed is
that the gate the identity feeds now runs on the write path as well as the read path,
so the durable-corruption consequence named above is closed rather than merely
documented.

One point in the Consequences is load-bearing for 0081 and worth flagging here rather
than leaving it to be rediscovered: the recommendation that `require_analyzer_match =
false` is a safe escape hatch **for this specific transition** is why 0081 keeps that
setting honored on ingest (WARNING, then proceed) instead of hard-erroring. An
unconditional ingest error would have turned every table mid-#62-transition read-only.

## Addendum (2026-08-23)

The verification quoted in "Alternatives considered" — `'klingon_stem'::regdictionary`
resolves in a session, yet `WITH (language='klingon')` fails with "text search
dictionary does not exist" — no longer produces that message. ADR 0082 (#148 HDL-07)
gave `language` a `validate_string` callback, which now rejects the value first, with
`bm25: invalid language "klingon"` (pinned at `expected/23_reloptions.out`).

**The conclusion is unaffected, and is in fact now enforced one step earlier.** The
point being made was that a dictionary outside `pg_catalog` cannot be bound at
`CREATE INDEX` at all, so "qualifying the name would remove the ability to use a
non-core stemmer" was never an available objection. That is still true: the new
validator deliberately resolves under a restricted `search_path` precisely so it
answers the same question the build does. Only the error text moved.

## Addendum (2026-10-04)

The fresh-eyes review of this date found the premise that a post-pg_upgrade REINDEX would rebuild byte-identical segments (the analyzer 'had not changed at all') to be false for PostgreSQL 17 to 18: the English Snowball stemmer changed between PostgreSQL 17 and 18 (a new Step-1b undouble guard; 'added' stems to 'ad' on 17 and 'add' on 18, plus five other words), and no fingerprint component moves across pg_upgrade. #296 proposes a behavioural probe-hash component.

## Addendum (2026-10-05)

#296 (ADR 0114) adds fingerprint component 7, a hash of the dictionary's raw output over a
fixed probe word list, which sees what this record's identity cannot: a stemmer that changed
under an unchanged name (PostgreSQL 18's Snowball update across `pg_upgrade`) and an in-place
`ALTER TEXT SEARCH DICTIONARY`. The identity stays component 1 and keeps its purpose, which is
not firing when nothing changed; the probe is additive, so a renumbered but identical
dictionary still passes. `dictinitoption` is still not hashed: the probe observes an option's
effect wherever it touches a probe word, and a `TSDICTOID` syscache callback makes an `ALTER`
visible to the next statement in the same session. A behaviour change confined to words outside
the probe list stays invisible. The claim in the `bm25_analyzer.c` header that `pg_upgrade`
cannot change tokenization is corrected.
