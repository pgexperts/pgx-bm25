---
id: 0051
title: The SQL-bodied query builders schema-qualify every pg_catalog call
date: 2026-08-10
status: Accepted
summary: The six M6 jsonb builders call pg_catalog.jsonb_build_object/to_jsonb explicitly rather than relying on the caller's search_path, chosen over SET search_path (which would block inlining) and BEGIN ATOMIC (which would change the bodies' representation for a narrower guarantee).
---

# 0051. The SQL-bodied query builders schema-qualify every pg_catalog call

## Context

The six M6 query builders — `bm25_match_terms`, `bm25_term`, `bm25_phrase`, `bm25_wildcard`,
`bm25_boolean`, `bm25_boost` — are `LANGUAGE sql IMMUTABLE` with **string** bodies. A string
body (unlike `BEGIN ATOMIC`) is re-parsed at call time under the *caller's* `search_path`, so
their unqualified `jsonb_build_object` / `to_jsonb` references were resolved against whatever
the caller could see: the CVE-2018-1058 pattern.

The reason this is not merely a schema-ordering race is the resolution rule. `pg_catalog` is
searched implicitly first only as a **tiebreak among candidates with identical argument-type
lists** (`FuncnameGetCandidates`). Once the type lists differ, `func_select_candidate` ranks by
count of exact input-type matches. A user-created, non-variadic
`jsonb_build_object(text,text,text,text)` therefore scores 2 against
`pg_catalog.jsonb_build_object(VARIADIC "any")`'s 0 and wins **outright, whatever the order of
the path**. Putting the hostile schema last does not help.

What that costs is specific to these functions. The builders' entire stated purpose — the
comment above them in the extension script says so — is that the query tree's shape comes from
*which builder was called*, never from parsing a caller-supplied string. A hijacked body
returns an attacker-chosen tree from the one API surface that was supposed to make that
impossible, and it runs with the caller's privileges. If the victim had built
`CREATE INDEX ... ((bm25_term('body', title)))` under a different path, the index expression and
the runtime expression disagree and the index returns wrong rows.

Severity is bounded by the precondition: the attacker needs `CREATE` on a schema in the
victim's `search_path`, which PG15's revocation of `PUBLIC CREATE` on `public` makes non-default.
These functions are not `SECURITY DEFINER`, so there is no privilege escalation beyond the
caller's own rights. This is a hardening gap, not an out-of-the-box exploit — but it is standard
extension practice to close it, and the fix is mechanical.

Nothing in the tree could have caught it. All four existing builder suites
(`48_m6_builders`, `46_m6_boolean`, `47_m6_wildcard`, `49_m6_acceptance`) call the builders under
pg_regress's default `search_path`, where hijacked and clean code are indistinguishable.

## Decision

Every call inside the six builder bodies is written `pg_catalog.jsonb_build_object(...)` /
`pg_catalog.to_jsonb(...)`, and `sql/85_builder_search_path` pins it.

`COALESCE` is deliberately left alone: it is SQL *grammar*, parsed into a `CoalesceExpr`, and is
never resolved as a function name through the path. (The original finding listed it as
hijackable; its own verifier note corrected that, and the correction is right.)

The suite is built around **canaries** — unqualified twins of two builder bodies, created inside
the suite — which must come back hijacked. Without them, "the builders still return the right
tree" would also be satisfied if the shadow functions were never eligible in the first place:
the suite would be green for the wrong reason and would stay green after someone stripped every
`pg_catalog.` prefix. Part 4 repeats the check with the hostile schema *last* on the path,
because a green Part 3 alone would not distinguish the fix from ordering luck.

Part 2 adds a static pin over `pg_proc.prosrc` asserting that each builder's count of bare
`jsonb_build_object`/`to_jsonb` equals its count of the qualified spelling. The behavioral test
can only see argument shapes it happens to shadow, so it would silently miss a *seventh* builder
added later; the static count would not.

## Alternatives considered

- **`SET search_path = pg_catalog` on each function** — rejected, but *not* for the reason that
  first suggested itself. The obvious argument is that a `SET` clause makes a function ineligible
  for inlining and these one-expression builders are the shape the planner wants to inline. That
  argument does not hold here, and the measurement is worth recording because it is
  counter-intuitive: **the builders do not inline today either way.** `jsonb_build_object` and
  `to_jsonb` are declared **STABLE** (`provolatile = 's'`, because they call type output
  functions that are themselves STABLE — `to_jsonb('…'::timestamptz)` moves with `TimeZone`.
  Not `DateStyle`: jsonb renders datetimes as hardcoded ISO 8601 via `JsonEncodeDateTime`,
  so `DateStyle` is inert here), and `inline_function` refuses to inline an IMMUTABLE-declared SQL
  function whose body contains mutable functions. Verified on PG18: an IMMUTABLE
  `plusone(x) → x + 1` inlines to `(x + 1)` in `EXPLAIN (VERBOSE)`, while `bm25_term(f, v)` over
  a table stays an opaque function call.

  What is left is still decisive. Qualification is the remediation PostgreSQL gives extension
  authors for CVE-2018-1058, so it is the spelling a reviewer expects to find; it costs nothing
  at runtime; and it leaves inlining *available* if these bodies ever stop calling STABLE
  builtins, where a `SET` clause forecloses it by construction and adds a GUC save/restore to
  every call. A `SET` clause is strictly more machinery for the same guarantee.
- **`BEGIN ATOMIC` bodies (PG14+, available given the hard PG17 floor)** — parses the body at
  `CREATE` time and stores the parsed tree, so it is immune to caller `search_path` *and* records
  dependencies on the referenced functions. Genuinely attractive, and rejected on scope rather
  than on merit: it changes how all six bodies are represented in the catalog and how a future
  upgrade script must rewrite them, to close the same hole that six `pg_catalog.` prefixes close.
  Qualification is also the guidance PostgreSQL gives extension authors for CVE-2018-1058, so it
  is the spelling a reviewer expects to find. Worth revisiting if these bodies ever grow beyond
  one expression.
- **Leave it; the precondition is non-default** — true, and it is why the severity is medium
  rather than high. But the cost of the fix is six prefixes, and the functions' advertised
  contract is precisely the one being broken.

## Consequences

- Builder output is now independent of the caller's `search_path`, at no runtime cost.
- **The builders do not inline, and that is a property of their bodies, not of this change** (see
  the `SET search_path` alternative above). Constant-argument calls — the ordinary
  `WHERE col @@@ bm25_term('body','tort')` — still fold to a single `Const` at plan time, because
  folding keys off the *declared* IMMUTABLE marking rather than on inlining; that is unaffected
  here. A future reader tempted to "restore inlining" by relaxing the markings should know the
  blocker is the STABLE volatility of the jsonb builtins.
- **The qualification is load-bearing and looks like noise.** A future edit that "tidies" the
  prefixes away reintroduces the defect silently under any ordinary test path. Two things guard
  it: the comment block above the builders states the mechanism, and suite 85 fails both
  behaviorally and statically. Verified by reverting the fix: 8 assertions flip.
- Any builder added later must be qualified *and* added to suite 85's Part 2 name list. Part 2
  only checks the six names it knows; a seventh builder is invisible to it until listed.
- Suite 85 depends on `pg_proc.prosrc` holding the literal body text. That is true for
  string-bodied SQL functions and would stop being true under `BEGIN ATOMIC` — so adopting the
  rejected alternative above later means rewriting Part 2, not just the bodies.

## Addendum (2026-08-23)

`bm25_boost`'s body gained a TYPE name -- `weight::pg_catalog.numeric` -- when #148
SQL-01 made its rendering independent of `extra_float_digits` (ADR 0082). Two
things follow that this record did not cover.

**The CVE-2018-1058 rule does not extend to grammar type keywords.** An unquoted
`numeric` is not an identifier looked up through the `search_path`: gram.y's
`Numeric` production emits `SystemTypeName("numeric")`, an explicitly
`pg_catalog`-qualified `TypeName`. Measured on PG 18.3 -- with
`CREATE DOMAIN shadow.numeric AS text` first on the path, `x::numeric` still
resolves to `pg_catalog.numeric`, while the QUOTED `x::"numeric"` resolves to the
shadow. So the prefix on that cast is redundant. It is written anyway so the body
reads under one rule rather than two, and because the exemption is narrow: a type
name that is NOT a grammar keyword -- any extension type, any domain -- IS
path-resolved and would need the qualification for real.

**A shadow's signature is part of the assertion.** Suite 85's boost shadow was
declared `shadow.jsonb_build_object(text, float8, text, jsonb)` to match the old
body. When the body's second argument became `numeric` the shadow stopped matching
anything, and the suite stayed green while covering one less thing -- the precise
failure mode this record's Consequences warn about, arriving from the other
direction. Part 3 now carries a third canary, `canary_boost`, whose outer call is
qualified and whose inner call is not, so the boost shadow's ELIGIBILITY is itself
witnessed rather than assumed. Any future change to a builder body's argument types
must move the corresponding shadow with it.
