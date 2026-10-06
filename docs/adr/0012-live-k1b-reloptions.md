---
id: 0012
title: Resolve k1/b/boost live at scan start instead of baking them at build time
date: 2026-07-16
status: Accepted
summary: Make the global k1/b reloptions and the existing per-field k1_<col>/b_<col>/boost_<col> knobs resolve from the index's CURRENT reloptions at every scan start — ALTER INDEX SET/RESET takes effect on the next scan, no REINDEX — because nothing in the postings bakes these three parameters, the opposite of the analyzer, whose stemmed tokens ARE the postings and must stay build-stamped and fingerprint-gated.
---

# 0012. Resolve k1/b/boost live at scan start instead of baking them at build time

## Context

The ROADMAP's `pg_search`-parity table contradicted itself about this project's
oldest remaining configuration gap: one row read "Configurable k1/b — ⚠️ math
accepts them; not user-settable" while the very next section credited M5 with
"per-field boost/k1/b" already landed. Both halves were true of different
things — per-field `k1_<col>`/`b_<col>`/`boost_<col>` reloptions have been
settable at `CREATE INDEX` since M5 (parsed from the raw `pg_class.reloptions`
array by `match_field_reloption`, src/bm25_build.c:116-155: `k1 >= 0`, `b in
[0, 1]`, `boost >= 0`), stamped once into the write-once field-config page by
`bm25_resolve_fields` — but there was no GLOBAL `k1`/`b` reloption (a bare
`WITH (k1=0.9)` errored as "unrecognized parameter"), and every scorer read
its k1/b/boost from that build-time stamp, never from the index's current
catalog state.

That stamping hid two live footguns, both reproduced on a running server
before this change:

1. **`ALTER INDEX ... SET (k1_body='banana')` was accepted.** The dynamically
   named per-field knobs bypass `build_reloptions` entirely (stripped by
   `bm25_strip_field_knobs` before it runs, src/bm25_handler.c:348-396); the
   only value check ran inside `bm25_resolve_fields`, at build time. An ALTER
   updated `pg_class.reloptions` with the garbage string and errored only at
   the next `REINDEX`.
2. **`ALTER INDEX ... SET (k1_body=2.0)` — a perfectly valid value — was
   accepted and silently inert.** `pg_class.reloptions` changed; every scan
   kept using the field-config page's build-era stamp. Divergence with no
   gate: exactly the failure shape the analyzer's fingerprint gate
   (src/bm25_scan.c:1157-1191) exists to catch for the analyzer, except
   nothing analogous existed for k1/b/boost.

**Why live resolution is safe here and can never be safe for the analyzer.**
The v5 WAND impact table stores, per field per block, the raw ingredients
`{max_tf, min_doclen}` — the format comment is explicit that this is
"deliberately NOT a single baked float4 upper bound, because idf and avgdl
are index-wide statistics that drift as the index grows; baking them at seal
time would go stale" (src/bm25_format.h:377-387). The scan-time bound
(`bm25_block_ub`, src/bm25_wand.c:150-152) calls the exact same `termscore`
the real scorer calls, over the SAME per-scan `fcfg[]` array (`ctx->k1_f`/
`ctx->b_f` copied from `fcfg[f].k1`/`fcfg[f].b` in `bm25_wand_ctx_build`,
src/bm25_wand.c:176-177) — bound and scorer read one shared source and cannot
diverge, whatever that source resolves to at scan start. THEORY.md's own
safety argument for the bound (§4a, lines 181-190) rests on two properties:
`termscore` is monotone (rises with tf, falls with doclen) and `idf >= 0`, so
`(max_tf, min_doclen)` dominates every real posting in the block. Monotonicity
is exactly what `k1 >= 0` and `b in [0, 1]` guarantee — the validated ranges
are not cosmetic input hygiene, they are the WAND bound's safety
precondition, and any design that lets k1/b vary live must keep enforcing
them at every point of entry, not just at `CREATE INDEX`.

The analyzer's answer is the mirror image. Its stemmed, stopword-filtered
tokens ARE the postings on disk (`meta->analyzer_fingerprint`,
`BM25FieldConfigHeader.per_field_fingerprint`); changing the analyzer without
a REINDEX would silently desynchronize what the index scan finds from what a
fresh `CREATE INDEX` would find, so it stays build-stamped and gated by a
scan-time fingerprint comparison, ERROR by default. k1/b/boost bake nothing —
every posting is stored independent of them, and the bound/scorer both read
whatever the current knob value is — so gating them the analyzer's way would
force a pointless REINDEX for a parameter that changes zero bytes on disk.
Same divergence problem (build-time value vs. current catalog state), opposite
correct answer, because the two features differ in exactly one respect:
whether the parameter is baked into the postings.

## Decision

We resolve k1/b/boost from the index's **current reloptions**, once per scan
start, rather than from the build-era field-config stamp. `store_positions`
and the analyzer configuration are untouched by this — they remain
build-stamped, for the structural/baked reasons above.

**Resolution rule** (identical at `CREATE INDEX` and at scan start):
effective(field) = the field's own `k1_<col>`/`b_<col>`/`boost_<col>` knob if
present in current reloptions, else the index-wide `k1`/`b` reloption (boost
has no index-wide knob — a uniform boost is a no-op on ranking and would only
invite confusion), else the compiled default (1.2/0.75/1.0).

**Mechanics:**

- **Global `k1`/`b` become real reloptions** (`add_real_reloption`,
  src/bm25_handler.c:462-467): `k1 in [0, 1e30]` default 1.2, `b in [0, 1]`
  default 0.75, acting as the per-field default. A bare `WITH (k1=0.9)` no
  longer errors.
- **The garbage-at-ALTER footgun is closed at the trust boundary that used to
  let it through.** `bm25_options` now calls `bm25_check_field_knob_value`
  (src/bm25_handler.c, wired in immediately before `bm25_strip_field_knobs`
  discards the per-field knobs) whenever `validate == true` — which is true
  for both `CREATE INDEX` and `ALTER INDEX`, the two contexts that share this
  parsing path — reusing `match_field_reloption`'s range checks so there is
  exactly one place that decides what a valid k1/b/boost value is.
  `validate == false` (relcache re-parsing already-stored options on every
  ordinary backend startup) deliberately stays lenient: an index that
  predates this fix and already carries garbage through the old hole must not
  brick relcache loads for every session that opens it.
- **The silently-inert footgun is closed by `bm25_resolve_live_params`**
  (`Relation index, BM25FieldConfig *fcfg, uint32 field_count`,
  src/bm25_build.c:360-404) — the same raw-reloptions scan and
  `match_field_reloption` range validation `bm25_resolve_fields` uses at
  build time, now also run at scan start to OVERWRITE `fcfg[]`'s k1/b/boost
  (store_positions and analyzer fields are left as the fieldcfg loaded them).
  It is called from one shared helper, `bm25_scan_load_fieldcfg`
  (src/bm25_scan.c:727), which all five ranked-scan entry points route
  through — the exhaustive scorer, the WAND builder, and the three WAND debug
  probes — so the resolver lands in one place, not five, and WAND's bound/
  scorer coherence (above) is preserved by construction: both read the same
  post-overlay `fcfg[]` for the scan's lifetime.
- **Legacy garbage fails loud, never silently.** An index that stored an
  invalid k1/b/boost value through the pre-fix hole will hit
  `bm25_resolve_live_params`'s `match_field_reloption` parse/range check on
  its next scan; that call now carries a non-NULL `reset_hint` ("Use ALTER
  INDEX ... RESET (<option>) to clear the bad value."), so the failure is an
  `ereport(ERROR, ...)` naming the exact repair, never a crash and never a
  silently-substituted default.
- **`store_positions_<field>` is deliberately NOT made live.** Whether a
  field's postings carry a position stream is structural — the `BM25_PAGE_POS`
  chain either exists on disk for that field or it doesn't — so an ALTER of
  `store_positions_<col>` remains catalog-only and inert until `REINDEX`,
  exactly as before this change. This asymmetry (k1/b/boost live,
  store_positions still build-stamped) is intentional, not an oversight, and
  is the reason it is called out here rather than left implicit.
- **`bm25_debug_fieldcfg` reports the build-era stamp**, unchanged by this
  work — under live semantics the field-config page is no longer the
  authority on the EFFECTIVE per-field k1/b/boost; `pg_class.reloptions` is.
  The C doc comment above the function (src/bm25_analyzer.c:456-470) now
  carries that distinction explicitly, so a reader doesn't mistake the stamp
  for the live value.
- **`bm25_stats` becomes honest.** Its k1/b columns switch from the
  metapage's write-once copy to `index->rd_options`'s current parsed values
  (src/bm25_meta.c, `bm25_stats`) — the metapage's own `meta->k1`/`meta->b`
  fields (src/bm25_meta.c:84-85) stay as they were (a vestigial build-time
  default, never read for this purpose again), but the column values a user
  actually sees now track what a live ALTER changed. No SQL signature change:
  same 13 columns, same order.
- **No format change, no extension version bump.** Reloptions are entirely
  C-side (`amoptions`); nothing here touches a SQL catalog object, so there
  is no `--0.1--0.2.sql`, no `control`-file change, nothing for `ALTER
  EXTENSION` to apply. An existing index's metapage/field-config stamp is
  untouched; its absent reloptions parse to the same 1.2/0.75/1.0 defaults
  the stamp already carried, so a pre-existing default-configured index is
  unaffected byte-for-byte (confirmed by all 63 pre-existing suites passing
  unchanged, byte-green, across all three implementation tasks).

**Evidence.** `sql/58_k1b_reloptions.sql` stanza 5 is the pinned proof that
WAND parity survives non-default k1/b AND a live ALTER, not merely an
assumption: at `k1=2.0, b=0.3`, WAND (`wand_top_k=100`) vs. exhaustive
(`wand_top_k=0`) rank the same top-100 bit-for-bit both before and after a
live `ALTER INDEX ... SET (k1_body='0.5')` (`wand_parity_nondefault`,
`wand_parity_after_live_alter`, `alter_changed_ranking` — all `t`), and an
anti-neuter pruning probe (`bm25_wand_stats`, `deep_check_skips > 0 OR
blocks_skipped > 0`) confirms WAND actually pruned rather than degrading to
an exhaustive scan in disguise (`deep_check_skips = 2106` of 2592 docs
scored) — a parity check that never exercises pruning would be vacuous, the
exact lesson M2b's own acceptance suite already encodes.

## Alternatives considered

- **Stamp k1/b/boost like the analyzer and gate ALTER behind a fingerprint
  comparison.** Rejected: the fingerprint gate exists because the analyzer's
  tokens are physically on disk and a mismatch would return wrong answers.
  k1/b/boost bake nothing into any posting, so gating them the same way would
  force a `REINDEX` — a full rewrite of every segment — purely to change a
  floating-point parameter that the bound and scorer already read fresh at
  scan time. That cost buys no correctness the live design doesn't already
  have.
- **Reject ALTER of these knobs outright, requiring `REINDEX` for any k1/b/
  boost change.** Rejected as user-hostile (it turns a one-line `ALTER INDEX`
  into a full rebuild for a parameter with zero on-disk impact), and there is
  no clean way to implement it: `amoptions`/`bm25_options` sees the same
  `validate=true` call for `CREATE INDEX` and `ALTER INDEX` — there is no
  signal in that path distinguishing "this is the first time" from "this is a
  later change," so "allow at CREATE, refuse at ALTER" is not a rule this
  code can enforce without inventing a new, purpose-built catalog check.
- **A GUC-style session override (`SET bm25.k1 = ...`) instead of a
  reloption.** Rejected: the ROADMAP's ask, and BriefBank's actual need, is
  per-index tuning that persists in the catalog and is visible to every
  session and to `bm25_stats`, not an ephemeral per-session knob a query
  could silently forget to set.

## Consequences

- The ROADMAP's k1/b self-contradiction is resolved in favor of as-built
  truth: per-field settability has been true since M5, and this change adds
  the missing global reloptions plus makes all three (global k1/b, per-field
  k1/b/boost) live via `ALTER INDEX ... SET`/`RESET`, with no `REINDEX`. This
  was the last open, non-optional entry in the ROADMAP's cross-cutting
  production/ops list.
- Tuning an index's ranking now costs one `ALTER INDEX` and takes effect on
  the very next scan — no rebuild, no downtime, no coordination with
  concurrent readers beyond ordinary catalog visibility.
- The `store_positions`/analyzer vs. k1/b/boost split is now a documented,
  intentional asymmetry rather than an implicit inconsistency someone could
  later "fix" by accident in either direction — a future reader who wants to
  make `store_positions` live must reckon with the fact that positions are
  physically on disk or not, the same argument that keeps the analyzer
  build-stamped.
- `bm25_debug_fieldcfg` and `bm25_stats` now answer two different questions
  and must be read accordingly: the former is a build-era stamp (useful for
  auditing what a `CREATE INDEX`/last `REINDEX` resolved), the latter (for
  k1/b) and `pg_class.reloptions` (for per-field knobs) are the live,
  effective truth.
- An index that stored an invalid per-field value through the pre-fix
  validation hole (before this change shipped) will start ERRORing on its
  next scan with a named `RESET` hint, rather than continuing to silently
  carry the garbage until an eventual `REINDEX` — a deliberate fail-loud
  break for a state that was already broken, not a new failure mode.
- No format change and no extension version bump means no upgrade step is
  required for this ADR; it ships as an ordinary code release.

## Addendum (2026-10-04)

**The lock level of `ALTER INDEX ... SET` for these options.** This record resolves
k1, b and the per-field knobs from the live reloptions and says nothing about the lock
an ALTER takes. Core computes that lock level from the reloptions registered in the
backend running the ALTER: `AlterTableGetRelOptionsLockLevel` takes the strongest
lock mode among the registered options whose names the command mentions, and
`AlterTableGetLockLevel` starts from ShareUpdateExclusiveLock (PG 18.6,
`reloptions.c` and `tablecmds.c`). An option that is not registered contributes
nothing.

Two consequences, checked once on a PG 18.6 cluster during the 2026-10-04 grind (a
discovery pass, not a regression test) by watching the lock held on the index after
`BEGIN; ALTER INDEX ... SET (...)` for `k1_title`, `boost_body`, `k1` and `analyzer`;
the other options follow from the source:

- The per-field `k1_<col>`, `b_<col>` and `boost_<col>` options are not registered, so
  they always take only ShareUpdateExclusiveLock.
- bm25 registers its other options lazily, on the first `bm25_options` call in a
  backend (`bm25_handler.c`). In a backend that has not yet loaded a bm25 index's
  relcache entry, ALTER INDEX ... SET takes only ShareUpdateExclusiveLock for every
  bm25 reloption, including `k1`, `b`, `analyzer` and `key_field`. In a backend that
  has loaded one first, `k1` and `analyzer` take AccessExclusiveLock.

So nothing here is protected by a lock against a concurrent ALTER; the live resolution
this record describes has to tolerate a change at any scan start. A change to scoring
knobs landing mid-statement was not reproduced: a correlated subplan that rescanned
the index scan four times, with the ALTER committing part-way through, used the old k1
in every rescan, presumably because syscache invalidation is processed only at
incidental points.
Other statement shapes were not tried. The defences that matter error by default: the analyzer
fingerprint gates (ADR 0081) and the INSERT-time key check (ADR 0067). For the WAND
over-pull tail, [0108](0108-a-wand-rebuild-may-only-extend-the-emitted-prefix.md) pins
k1, b and boost, so a change between the first build and the rebuild cannot move
emitted rows' scores.

## Addendum (2026-10-05, PRs #331-#350)

Per-field knob bounds and surface (#304 SCORE-02, SURFACE-06, SURFACE-09; D14, D15; PR
#335).

- **Upper caps at DDL only.** `k1_<col>` is capped at `BM25_K1_MAX` (1e30, the index-wide
  `k1`'s cap, now shared) and `boost_<col>` at `BM25_FIELD_BOOST_MAX` (1e6), in
  `bm25_check_field_knob_value`, which runs at CREATE INDEX and ALTER INDEX SET. The value
  parser the scan and INSERT paths use is unchanged and lenient, so an index already storing
  an over-cap value keeps working instead of wedging every INSERT (the #292 pattern). Both
  caps are skipped under `IsBinaryUpgrade`: pg_upgrade recreates indexes through
  `pg_dump --binary-upgrade`'s CREATE INDEX, and DefineIndex validates reloptions with no
  binary-upgrade bypass, so a stored over-cap value would abort the upgrade.
- **Release note:** an index already storing an over-cap knob fails a later `ALTER INDEX
  SET` and a plain dump/restore until that knob is `RESET`; pg_upgrade is
  unaffected. The pg_upgrade bypass was verified by a manual `postgres -b` A/B only (no
  TAP).
- **Unmatched suffixes warn.** ambuild emits a WARNING with a RESET hint for each per-field
  knob whose suffix names no key column (a typo, or an INCLUDE column), at CREATE INDEX and
  REINDEX only, never per row on INSERT. Not an ERROR: that would make every existing index
  carrying such a knob fail REINDEX and dump/restore, for a setting that never had an effect.
- **Scan-key type gate.** `bm25_rescan_parse_key` checks every scan key's subtype before
  decoding its argument; anything but text, varchar, a domain over those, or jsonb is a clean
  ERROR instead of a backend crash on a superuser-built opclass. That gate is the boundary,
  because CREATE OPERATOR CLASS never runs amvalidate; `bm25_validate` only reports such an
  operator.
- **Monotonicity.** The "termscore is monotone" premise above holds in exact arithmetic;
  the floating-point inversions of a few `DBL_EPSILON` are absorbed by `wand_widen_ub`'s
  headroom (ADR 0004 and 0016 addenda of this date). Bounding k1 and b still matters for the
  same reason the record gives.
