---
id: 0082
title: Declaration defaults that misrepresent their function are made explicit
date: 2026-08-23
status: Accepted
summary: Every declaration in bm25_native--1.0.sql was audited against what its function actually does, and the ones whose omitted volatility, parallel safety, COST or ROWS misrepresented it now state the property; declarations where the conservative default is already correct are deliberately left implicit. bm25_boost keeps IMMUTABLE by rendering its weight through numeric; the analyzer, language and debug-tokenizer knobs gain DDL-time validators; module_pathname takes the $libdir prefix.
---

# 0082. Declaration defaults that misrepresent their function are made explicit

## Context

A fresh-eyes review (#148) traced every `CREATE FUNCTION` in
`bm25_native--1.0.sql` against the C symbol it names. The bindings were clean --
77 symbols, 77 distinct SQL bindings, zero mismatches -- and the privilege
architecture was sound. The defects were all in declaration **properties**, and
they had one shared cause: a property that is not written is a property nobody
decided. Omitting a volatility keyword means `VOLATILE`; omitting `PARALLEL`
means `PARALLEL UNSAFE`; omitting `COST` means 1; omitting `ROWS` on a
set-returning function means 1000. The file had accumulated a mix of
carefully-argued markings (`bm25_distance`'s `STABLE PARALLEL RESTRICTED`, with a
measured justification) and silent defaults, and nothing distinguished them.

Two had measured consequences.

`bm25_boost` was declared `IMMUTABLE` while its body passed a `float8` to
`jsonb_build_object`, which renders `FLOAT8OID` through `float8out` -- and
`float8out` honours `extra_float_digits`, a `PGC_USERSET` GUC. On PG 18.3 the
same call yields `{"weight": 0.3333333333333333}` at the default and
`{"weight": 0.333333333333}` at `-3`. `IMMUTABLE` is what admits a function to an
index expression, and PostgreSQL checks the *declared* volatility without
inlining the body, so an expression index over `bm25_boost(w, q)` stored the
building session's rendering and a later session with a different setting probed
it with a different string and got zero rows. The same declaration also licensed
plan-time folding, baking the planning session's setting into a cached plan.

The six jsonb query builders all defaulted to `PARALLEL UNSAFE`. `standard_planner`
derives `glob->parallelModeOK` from `max_parallel_hazard()` over the **raw parse
tree**, before `subquery_planner` runs `eval_const_expressions` -- so a single
`PARALLEL UNSAFE` `FuncExpr` anywhere in a query disables parallelism for the
whole plan even though the builder is `IMMUTABLE`, folds to a `Const`, and never
executes. Because the bm25 index scan is itself `amcanparallel = false`, the loss
landed entirely on everything else in the query: joins to large tables, parallel
aggregates, CTEs. This fired on the documented, intended use of the builder API.

The rest were quieter but the same shape. `bm25_snippet` was `STABLE` under a
comment claiming it was marked "exactly like `bm25_score`", which is `VOLATILE` --
three functions reading the identical backend-local scored-scan registry carried two
different markings between them (`bm25_distance` STABLE, `bm25_snippet` STABLE,
`bm25_score`/`bm25_score_key` VOLATILE), and only `bm25_distance`'s was argued. It also carried
`procost = 1` while calling `bm25_analyze` over the query terms *and* the whole
field value per row. All 23 set-returning probes carried `prorows = 1000`, across
probes whose real cardinality spans six orders of magnitude. Four probes that meet
the file's own stated "pure function of its arguments" test were left `VOLATILE`.
Not one `COMMENT ON` existed in a heavily-commented install script, so
several hundred lines of explanatory prose -- the snippet escaping contract, the
`bm25_score` NULL guarantee -- were invisible from `\df+` and `obj_description()`.

Two reloptions had the same defect one layer down. `analyzer` and `language` were
registered with a `NULL` `validate_string` callback while `stopwords`, `tokenizer`
and `phrase_fallback` had one. `analyzer` selects nothing (there is one analyzer
pipeline; `analyzer_offset` is consumed nowhere), so `WITH (analyzer = 'german')`
succeeded, stemmed in english, did not perturb the analyzer fingerprint -- so
`require_analyzer_match` never fired -- and told the user nothing at all.
`language` deferred rather than ignored: `ALTER INDEX ... SET (language =
'klingon')` committed successfully and left an index that errored on the next
scan, when `bm25_snowball_dict_oid` resolved the name for real.

## Decision

Every declaration in `bm25_native--1.0.sql` was audited against what its function
actually does, and **wherever the omitted default misrepresented the function, the
property is now stated and carries its reason in the file.**

The converse is deliberate and is the reason this record is not titled "every
declaration is explicit": of the 103 `CREATE FUNCTION`s in the file, 54 still carry
no volatility keyword and 87 carry no `PARALLEL` keyword, and that is correct. For a
`bm25_debug_*` probe that opens an index relation, `VOLATILE` and `PARALLEL UNSAFE`
are the accurate markings, and the defaults already supply them — annotating all 103
would be churn that buys nothing and would make the file's genuinely load-bearing
markings harder to find, not easier. `bm25_score` and the four `bm25_score_key`
overloads are exactly this case: `VOLATILE` by omission is what they should be, which
is why SQL-05 moved `bm25_snippet` to match THEM rather than the other way round.
An explicit property in this file therefore means "the default was wrong here", and
that signal is worth preserving.

Concretely:

- **`bm25_boost` keeps `IMMUTABLE`** and is made honest by rendering the weight as
  `weight::pg_catalog.numeric`. `float8_numeric` formats with a hard-coded
  `DBL_DIG` and `numeric_out` ignores `extra_float_digits`, so the rendering is
  setting-independent. The cast target is written `pg_catalog`-qualified, but NOT
  for the CVE-2018-1058 reason that governs the function calls in these bodies: an
  unquoted `numeric` is a grammar keyword pinned to `pg_catalog` by
  `SystemTypeName()`, so the prefix is redundant there. See the addendum to
  ADR 0051 for the measurement and for why it is written anyway.
- **All six builders are `PARALLEL SAFE`.**
- **`bm25_snippet` is `VOLATILE PARALLEL RESTRICTED COST 5000`**, matching
  `bm25_score` over the same state and borrowing `bm25_match(text,text)`'s cost
  derivation.
- **Every set-returning probe carries a `ROWS`** from one of four
  order-of-magnitude buckets (10 / 100 / 10000 / 100000), and the four that drive a
  whole ranking build carry `COST 5000`.
- **The PUBLIC-executable surface carries `COMMENT ON`** -- the fourteen C names
  the REVOKE allowlist names as deliberately public, the six builders, the four
  operators, the opclass and the access method. Debug functions deliberately get
  none: a comment is user-facing documentation.
- **The four pure probes become `IMMUTABLE`**, `bm25_debug_glob_match`'s keyword
  order is normalised, and the two selectivity estimators become `STABLE` to match
  how core declares `eqsel`/`scalarltsel`.
- **The explicit-config debug tokenizer's second argument is validated too, as the
  `tokenizer` it actually is.** `bm25_debug_tokenize(text,text,text,text)` and
  `bm25_debug_analyze_positions` took a second argument named `analyzer` in the C
  code, discarded it under a comment reading *"standard is the only tokenizer in
  M3"* -- the two knobs conflated in one declaration -- and accepted any string
  whatsoever: `bm25_debug_tokenize(t,'TOTAL_NONSENSE','default','german')` returned
  German stems and reported nothing. All eleven call sites in `sql/` pass
  `'standard'`, a tokenizer value, so the NAME was wrong rather than the callers. It
  is renamed `tokenizer` and validated through `bm25_validate_tokenizer` -- the same
  function the reloption uses, which is exported for this and deliberately shared
  rather than copied.
- **`analyzer` and `language` get `validate_string` callbacks.**
  `bm25_validate_analyzer` accepts only the reserved default `english`;
  `bm25_validate_language` (in `bm25_analyzer.c`, because it must ASCII-fold and
  resolve `<language>_stem` exactly as the resolver does) requires the dictionary to
  resolve **under a restricted `search_path`**. That last part is not incidental:
  `DefineIndex` calls `RestrictSearchPath()` before `index_reloptions(...,
  validate=true)`, so on the `CREATE INDEX` path the callback already ran against
  `pg_catalog` alone, while `ATExecSetRelOptions` restricts nothing — an `ALTER`
  would otherwise accept a dictionary the next `REINDEX` could never find, which is
  the very "accepted now, broken later" shape the validator exists to remove. So the
  validator restricts for itself, under its own GUC nest level, and both DDL paths
  answer one question: *will the build find this dictionary?*
- **`module_pathname` becomes `'$libdir/bm25_native'`**, the PGXS convention.

`sql/103_declaration_properties` pins the catalog properties and the two plan-shape
consequences; `sql/23_reloptions` pins the two new validators; and
`sql/85_builder_search_path` gained a third canary because the boost body's
argument types moved (ADR 0051 addendum).

## Alternatives considered

- **Drop `IMMUTABLE` from `bm25_boost` and mark it `STABLE`** (the review's first
  option) -- correct, and strictly less useful. `STABLE` would keep the function
  out of index expressions but would also forbid a legitimate expression index on
  a boost tree, and it would leave the rendering session-dependent rather than
  fixing it. Fixing the rendering makes the stronger marking true instead of
  weakening the marking to match a defect.
- **Keep rendering the weight as `float8` and document the hazard** -- rejected on
  the same ground the review rejected it: `IMMUTABLE` is a promise the planner
  acts on, not a hint, and the observable failure is silently wrong query results.
- **Treat the 15-significant-digit precision loss as a blocker** -- considered and
  dismissed. `float8 -> numeric` loses the 16th and 17th digits of the
  shortest-exact form, so a weight round-trips with up to ~1e-16 relative error,
  which a BM25 score multiplier cannot distinguish. The one place that is *not*
  merely imprecise is the top of the range, and it took a reproduction to find: the
  rounding is round-to-nearest, so a weight near `DBL_MAX` rounds **outward past
  it** and `numeric_float8` raises `numeric_value_out_of_range` reading it back.
  Measured exactly on PG 18.3: the top **four** doubles overflow — `DBL_MAX`
  (`1.7976931348623157e308`) down to `1.7976931348623151e308` — and the fifth,
  `1.797693134862315e308`, round-trips. `1e308` is some 4e15 ULPs below the band and
  is unaffected, which is why it is no use as a boundary pin;
  `sql/103_declaration_properties` brackets the edge with that adjacent pair instead.
  Accepted rather than worked around — these are absurd score multipliers and the
  failure is a loud ERROR, not a wrong number — but it is a real new error on
  previously-working input, so it is pinned rather than left to be rediscovered.
- **Mark `bm25_score`/`bm25_score_key` `STABLE` to match `bm25_snippet`**, rather
  than the reverse -- rejected. `STABLE` is the unsafe direction here:
  `evaluate_function` folds a `STABLE` call whose arguments are all `Const` when
  `context->estimate` is set, and `bm25_snippet`'s four SQL defaults *are*
  `Const`s. `bm25_distance` is `STABLE` only because measurement showed `VOLATILE`
  makes the planner insert a `Sort` and break ranking (ADR 0028); nothing forces
  `bm25_score` or `bm25_snippet` off `VOLATILE`, because neither is on the
  `amcanorderbyop` path.
- **Derive each SRF's `ROWS` from something real** -- not possible. An SRF's
  cardinality is a function of the index handed to it, which `prorows` cannot see.
  The buckets are shape markers and are labelled as such in the file.
- **Keep the bare `module_pathname = 'bm25_native'`** -- this was the expected
  outcome, on the grounds that the no-sudo dev harness resolves the module through
  `dynamic_library_path` and a `$libdir` prefix would pin it to `pkglibdir`, where
  a stale copy sits. Measurement changed the answer: **PG 18's
  `load_external_function()` STRIPS a leading `$libdir/` from a simple name and
  then searches `dynamic_library_path`** (added with `extension_control_path`,
  `4f7f7b03758`; moved out of `expand_dynamic_library_name` so `LOAD` keeps the old
  meaning, `f777d773878`; narrowed to non-nested names, `d07e2d4237c` -- all before
  18.0). Verified directly: with `dynamic_library_path` forced to `'$libdir'`,
  `'$libdir/bm25_native'` resolved to `pkglibdir` and failed on a missing symbol;
  with the repo build dir prepended, the same string resolved to the repo build.
  So on PG18 the two forms are indistinguishable and the harness is unaffected;
  on PG17 the prefix pins `pkglibdir`, which CI satisfies with `sudo make install`.
- **Leave the debug tokenizer alone and document the asymmetry** -- the obvious
  reading was that this is the #157 shape (one knob, two surfaces, opposite answers)
  and that the choice was between validating `analyzer` there too or writing the
  divergence down. Measurement dissolved the dilemma: there IS no `analyzer` argument
  in that signature. The second argument is the tokenizer slot -- the C code's own
  discard comment said so, and every call site passes `'standard'` -- so the correct
  fix was neither option but a rename plus the tokenizer predicate. Documenting
  would have preserved a misleading parameter name; validating it against
  `bm25_validate_analyzer` would have broken all eleven call sites and cemented the
  confusion.
- **Copy the three-line tokenizer check into `bm25_tokenize.c`** instead of exporting
  `bm25_validate_tokenizer` -- rejected. Two copies of a predicate is precisely how
  two surfaces drift into accepting different sets, which is the defect being fixed.
- **Comment only the six builders** (the narrowest reading of the finding) --
  rejected in favour of keying the comment set to the PUBLIC grant state, which is
  a predicate the suite can check independently of the list in the script. A test
  that restates the code's own predicate can only confirm the predicate is
  self-consistent; `sql/63_debug_privileges` learned that the hard way.
- **Make `bm25_validate_language` pin the dictionary** -- out of reach and not
  claimed. Resolution is by unqualified name through the caller's `search_path`,
  and validating at DDL time does not change that. What it buys is narrower and
  worth stating exactly: it converts a broken index into a rejected statement.

## Consequences

- `WITH (analyzer = '<anything but english>')` is now an error. This is a
  **behaviour change for existing DDL**: `sql/23_reloptions` itself had been
  passing `analyzer = 'german'` as an incidental demonstration of how invisible
  the knob was. Any script doing the same must drop the clause.
- **An index CREATED before this change with a non-`english` `analyzer` is a
  migration hazard**, and an earlier draft of this record wrongly said it could not
  be ("the value was never consumed" is true of READS only). Reads keep working —
  the relcache path parses with `validate=false`. But the value is still in
  `pg_class.reloptions`, so `pg_get_indexdef` emits it, which means `pg_dump` output
  fails to restore and `pg_upgrade` fails on it; and because `ALTER INDEX … SET`
  re-validates *every* reloption, an unrelated edit such as
  `SET (require_analyzer_match = true)` now fails too. The remedy is
  `ALTER INDEX … RESET (analyzer)`, which changes no behaviour because the value was
  never consumed, and the ERROR's `errhint` names it. A dump taken before the reset
  still has to be edited.
- `ALTER INDEX ... SET (language = ...)` now fails at the statement for an
  unresolvable dictionary instead of at the next scan. A restore or migration that
  sets `language` before creating the dictionary will now fail earlier and more
  visibly.
- `bm25_boost`'s output text changed for long fractions, which are now cut to 15
  significant digits (`0.3333333333333333` becomes `0.333333333333333`). Round values
  are unaffected — `float8out(2.0)` already emitted `2`, so `{"weight": 2}` is what
  both renderings produce, which is why no existing expected output moved. jsonb
  compares numbers with `numeric_cmp`, so equality against a literal is unaffected
  either way, but a file pinning the *text* of a fractional weight would move.
- An expression index over `bm25_boost(...)` built before this change holds values
  in the old rendering and will not match new probes. Such an index is a curiosity
  rather than a documented use, but a `REINDEX` is the fix if one exists.
- On PG17 a no-sudo, build-in-repo harness no longer works: there the `$libdir`
  prefix really does pin `pkglibdir`. The local harness is PG18 and CI installs, so
  nothing in the project is affected today, and the control file states the
  constraint.
- The `$libdir` prefix does **not** remove the `dynamic_library_path` redirection
  exposure the finding names, on PG18. That is now recorded in the control file
  rather than implied by the convention.
- **`bm25_snippet` VOLATILE changes plan shape, not answers.**
  `make_sort_input_target` postpones unconditionally for any target-list column
  containing a volatile function, so a query with an `ORDER BY` now projects the
  snippet *above* the sort (an extra `Result` node) where the old `STABLE` +
  `procost = 1` marking kept it below; and `contain_volatile_functions` on a
  subquery target list blocks subquery pull-up, so `SELECT … FROM (SELECT
  bm25_snippet(body) …) t` no longer flattens. Both were checked and the snippet is
  still correct above the sort — the scored-scan registry is keyed on the scan, and
  the scan node is not shut down until the plan ends — and
  `sql/103_declaration_properties` pins that as a functional assertion rather than
  leaving it to the `provolatile` check. Projecting later is the price of not being
  foldable, which is the trade this record chose.
- **The `language` validator runs on its own default at registration, and that
  widened one blast radius.** `init_string_reloption` calls `validator(default_val)`
  before allocating, so `bm25_validate_language("english")` — a syscache lookup under
  a GUC nest push/pop — executes once per backend at the first `bm25_options` call,
  including a `validate = false` relcache load. Measured: with
  `pg_catalog.english_stem` dropped, `SELECT count(*) FROM t` on a table that merely
  *has* a bm25 index now ERRORs, where before only index use did. The precondition is
  a superuser `DROP` that also cascades away the `english` text search configuration,
  so the database is already comprehensively broken and every bm25 scan in it was
  failing beforehand — but the radius genuinely grew from "the index" to "any query
  touching the table". The remedy, if it is ever judged to matter, is to register
  `language` with a `NULL` `default_val` (the validator returns immediately on NULL)
  and let `bm25_opt_str` supply `"english"` through its documented fallback. **Not
  taken here**, deliberately: that moves a default the analyzer *fingerprint* is
  computed from, and getting that wrong is a cluster-wide `REINDEX` — too much risk
  for a failure mode that requires an already-broken database.
- **This ships only to NEW installs.** `default_version` stays `1.0` and there is no
  `bm25_native--1.0--1.1.sql`, so a database that already ran `CREATE EXTENSION`
  keeps the old catalog entries — the old `bm25_boost` body, `bm25_snippet` still
  `STABLE`, no `ROWS`, no comments — until `DROP EXTENSION` + `CREATE EXTENSION`.
  That is this project's pre-1.0 posture (`docs/adr/0013`), not an oversight, but
  every guarantee above is a guarantee about a fresh install, and no suite can
  detect the divergence because `installcheck` always starts from one.
- Every future PUBLIC-executable function must carry a `COMMENT`, enforced by
  `sql/103_declaration_properties`, which derives the requirement from the GRANT
  state rather than from a hand-maintained list.
