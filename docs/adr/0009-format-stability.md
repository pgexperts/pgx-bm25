---
id: 0009
title: Format-negotiation baseline (v6): floor gate, informational flags, and the online bm25_upgrade path
date: 2026-07-13
status: Accepted
summary: Replace the exact-equality format gate with a two-directional min_read_version floor, ship a v6 negotiation baseline that reads legacy v5 indexes REINDEX-free, and add a bm25_upgrade(regclass) online migration engine for the rare true break.
---

# 0009. Format-negotiation baseline (v6): floor gate, informational flags, and the online bm25_upgrade path

## Context

Through v5, `bm25_meta_read()` gated every read with an exact-equality check:
`if (format_version != BM25_FORMAT_VERSION) ERROR "REINDEX required"`. A v4
index and a hypothetical v6 index were rejected identically. Combined with
`docs/adr/0005-tradeoffs.md`'s note that v5 finished the "reserve-then-fill"
arc (no reserved-but-unused fields left after M2b), this meant *any* future
on-disk change — even a purely additive one, like a new optional trailing
region — would have forced another hard break and a full REINDEX. Roadmap #4
exists because BriefBank deploys via Ansible across a primary and physical
streaming replicas and cannot REINDEX a multi-GB corpus on every release.

Two forces made "just don't ever change the format again" untenable and
"read-only backward compatibility" insufficient (see Alternatives): the
codebase already discriminates several structures without a version bump
(segment header, dict entry, block header, field-config page all use
length/count prefixes and `pd_lower`-detected trailing regions — self-describing
by construction), so the metapage's fixed-`sizeof`, no-discriminator `memcpy`
was the odd one out; and version skew is a hard operational requirement, not
an edge case — a streaming standby still on the old binary replays the
primary's WAL byte-for-byte and will read new-format pages the instant the
primary upgrades, and rolling app-node deploys add more skew on top. New data
must be either old-binary-safe (ignored) or cleanly refused, in both temporal
directions, never silently mis-parsed.

## Decision

Replace the exact-equality gate with a floor-based, two-directional
negotiation contract, and ship the online upgrade engine needed to cross a
real future break, rather than deferring it until one lands.

**On-disk change.** `BM25MetaPageData` gains two `uint32` fields, appended
after `field_count` using the same append-then-`pd_lower`-advance,
zero-init mechanism used for the v3→v4 growth (`src/bm25_format.h:158-162`):
`min_read_version` (the floor — the oldest binary `BM25_FORMAT_VERSION` that
can correctly read this index) and `feature_flags` (an informational bitmap
of optional capabilities present — multifield, positions, WAND impacts).
`BM25_FORMAT_VERSION` moves 5 → **6**; `BM25_OLDEST_READABLE` is a new
per-binary constant, **5** — the oldest `format_version` this binary still
has a reader for. A legacy pre-#4 v5 index has no on-disk `min_read_version`/
`feature_flags`; a v6 binary reads them as `0` from the zeroed page tail
(`PageInit` zeroes the whole page and no v5 writer ever advanced `pd_lower`
past the v5-sized struct), which the floor gate treats as "legacy, always
readable" — so adopting v6 requires **no REINDEX** of existing v5 indexes.

**The gate** (`bm25_meta_validate`, extracted in `src/bm25_meta.c`) checks
both directions: forward, `min_read_version > BM25_FORMAT_VERSION` means this
binary is too old for a capability the index requires — ERROR "index requires
extension format >= N"; backward, `format_version < BM25_OLDEST_READABLE`
means this binary has dropped the reader for that generation — ERROR "this
build reads >= N", hint REINDEX. Between the two bounds, the index is
accepted: older self-describing regions are simply absent, newer optional
regions are skipped via the existing `pd_lower`/length-prefix detection.

**The gate is enforced on the query hot path, not only at the margins.**
`bm25_meta_validate` is called from `bm25_meta_read` (build/introspection/
maintenance callers) **and** from `bm25_scan_snapshot`
(`src/bm25_seg_read.c`), which every ranked/filter scan calls to build its
per-scan snapshot. The original task plan wired the gate only into
`bm25_meta_read`; that left the per-query snapshot path ungated, so a stale-
or too-new-format index could have been scanned silently through
`bm25_scan_snapshot` without ever calling the gating read. This was found and
closed during implementation — the ADR records the corrected scope: the floor
is checked on every scan, not just on build/maintenance entry points.

**`feature_flags` is informational only — it never gates.** The two-directional
check decides solely on `min_read_version`/`format_version`; `feature_flags` is
derived by a shared `bm25_derive_feature_flags(Relation, const BM25MetaPageData *)`
from segment/field-config content when it reads as `0` (a legacy index), stamped
explicitly by `bm25_upgrade`, and surfaced via `bm25_stats` (now 13 output
columns) for diagnostics only.

**The additive-vs-breaking rule** (the contract's core, binding on all future
format work):

- **Additive** — data an old binary can safely not read. Exactly three shapes
  qualify: (i) a length-prefixed trailing region on a single-header-per-page
  structure, detected via bytes-remaining below `pd_lower`; (ii) an entirely
  new page type old binaries never follow a link to; (iii) a previously
  invalid/zero sentinel field being filled. `min_read_version` stays
  **unchanged**; a `feature_flags` bit may be set.
- **Breaking** — an existing field's shape or semantics changes (the v4→v5
  `max_impact` → per-field impact table precedent), **or** a packed
  multi-record structure grows a field. The second half is a deliberate
  carve-out: `BM25DictEntry`, `BM25BlockHeader`, and the segcat entry pack many
  records per page and the reader strides by the compile-time `sizeof` with no
  per-record length prefix — appending even one field desyncs an old reader at
  the second record and mis-parses rather than skipping cleanly. Only the
  metapage (one struct per page, a fixed-`sizeof` `memcpy` that ignores its own
  tail) and genuine single-header trailing regions are safely growable in
  place; a packed multi-record struct is not, regardless of how small the
  addition looks.

**The two mechanisms that MAKE the additive rule true.** "Additive" is a claim
about *writes*, not only reads: it says an older binary keeps accepting,
reading, **and writing** an index containing newer data. Classifying a shape as
additive does not make that safe — two mechanisms do, and the contract is only
honest because both ship in v6. Neither can be added to a released v6 later: a
v6 that lacks them is exactly the binary a future v7 index meets in the field.

- **The orphan sweep must not free page kinds it does not know**
  (`BM25_PAGE_ALL_KNOWN`, `src/bm25_format.h`; enforced in `bm25_reclaim_orphans`,
  `src/bm25_fsm.c`). This is what makes additive shape (ii) — a new page type —
  real. The sweep is closed-world: it marks from a HARDCODED compile-time root
  set, then sweeps `[1, nblocks)` and frees everything unmarked. A v7 page type is
  unreachable to a v6 binary *out of ignorance*, not because it is dead, so the
  naive sweep frees the entire v7 chain — and an orphan is stamped
  `BM25_PAGE_DELETED` with `InvalidFullTransactionId`, which `bm25_page_alloc`
  reuses IMMEDIATELY (no horizon wait), handing those pages to the next seal.
  Silent corruption, by a binary the contract calls safe. The sweep therefore
  refuses to free any page whose opaque carries a bit outside the mask of kinds
  this build knows. It fails SAFE — it leaks a page (a binary that knows the kind
  reaches it from its own root set and reclaims it normally) rather than
  corrupting one. **A future `BM25_PAGE_*` addition MUST extend
  `BM25_PAGE_ALL_KNOWN`**; the mask lives directly beneath the flag defines so the
  two cannot drift.
- **Metapage writers must RAISE `pd_lower`, never assign it**
  (`bm25_meta_set_pd_lower`, `src/bm25_meta.c`; every metapage writer routes
  through it). This is what makes the metapage growability carve-out above real.
  The metapage grows by APPENDING struct fields (v3→v4 and v5→v6 both did; a v7
  would). Under the floor gate a v6 binary ACCEPTS a v7 index, and the moment it
  writes the metapage — any insert, seal, or merge bumps stats — an assigning
  writer sets `pd_lower` back to the end of the *v6* struct, dropping v7's
  appended fields into the page hole. `GenericXLogFinish`'s delta path ZEROES
  `[pd_lower, pd_upper)` on apply, to the live buffer and on WAL redo alike, so
  v7's fields (possibly chain roots) are destroyed. Raising to
  `Max(current, end-of-struct)` leaves the unknown tail below `pd_lower` and hence
  preserved verbatim; the only cost is a smaller page hole than this build needs.
  The reader side already ignores the tail by construction (`bm25_meta_read` copies
  a fixed `sizeof` and never consults `pd_lower`), so raising never makes unknown
  bytes visible to the older build.

Both guards are pinned by discriminating regression checks, not merely documented.
`sql/55_format_compat.sql` case (H) allocates a page carrying a flag outside the
mask and asserts VACUUM leaves it untouched (a neutered guard reports flags `8704`
instead of `8192`); cases (C)/(G) drive `bm25_debug_stamp_version`, a real
`aminsert`, and a real seal across a live synthetic trailing region and assert it
survives byte-for-byte (an assigning writer reports `f`). Both were verified by
neutering each guard and observing the suite go red.

**Invariant, forever:** `min_read_version >= 5` and `BM25_OLDEST_READABLE >= 5`.
Per-block impacts became mandatory at v5 and the block-header reader assumes
their presence; neither bound may drop below it.

**`bm25_upgrade(regclass)`** — a new SQL-callable maintenance function,
online and requiring no heap rescan. It seals pending, re-reads and
gate-validates the metapage, then dispatches on a static
`{from_gen, to_gen, rewrite_segment}` transform registry: an unmatched gap is
identity (no rewrite — today's v5→v6, and every gap this build accepts, since
the registry ships with no real transforms yet); a matched gap re-emits every
segment through the existing merge-accumulate machinery and publishes via the
merge's atomic catalog swap. The lookup is a single predicate
(`bm25_upgrade_transform_matches`) evaluated ABOVE the path branch, so a
registered transform and the test-only synthetic flag reach the identical rewrite
path — the flag is simply a way to force a match, not a second code path. **The version re-stamp is folded into the SAME
`GenericXLog` record as the catalog-root flip** when a rewrite happens — the
merge swap already mutates the metapage inside one record, so
`format_version`/`min_read_version`/`feature_flags` join it there; a crash
between a committed swap and a separate re-stamp would otherwise leave
new-format segments under an old-format metapage. In the identity-only case
there is no swap, so the re-stamp is a single standalone atomic metapage
write. `bm25_upgrade` is primary-only (`RecoveryInProgress()` guard) and
idempotent (already-current is a no-op `NOTICE`). The rewrite path caps its
drop count at `BM25_RETIRED_PER_PAGE` (the swap retires into one
pre-acquired retired-list page); exceeding it is a clean
`ERRCODE_PROGRAM_LIMIT_EXCEEDED` with a "run `bm25_merge()` first" hint — a
known bound, not a silent truncation. The registry ships empty of real
transforms; a test-only synthetic transform (`bm25_debug_enable_synthetic_transform`)
exercises the full rewrite-and-swap path so it is proven, not hypothetical.

**Packaging.** This is the project's first release that assumes existing
installs, not only fresh `CREATE EXTENSION`. `pg_bm25_index.control`'s
`default_version` moves `0.1` → `0.2`, and `pg_bm25_index--0.1--0.2.sql` is the
project's **first** extension upgrade script (`CREATE FUNCTION bm25_upgrade`,
the widened `bm25_stats`, and the debug levers). Rollout is three ordered
steps: (1) install the new binary on every node (primary and all standbys) —
a standby still on the old binary refuses cleanly via the backward gate until
this completes, never mis-reads; (2) `ALTER EXTENSION pg_bm25_index UPDATE` —
creates `bm25_upgrade` and the widened `bm25_stats` in the catalog (skipping
this step makes the next one fail with "function does not exist"); (3)
`SELECT bm25_upgrade('idx')` **on the primary only** — the standby receives
any rewritten segments purely through WAL replay of the swap record; it does
not and should not run `bm25_upgrade` itself.

**Tests.** `sql/55_format_compat.sql` is the negotiation matrix (legacy-v5
accept, too-old-v4 refuse, additive-v7 read-through a real synthetic optional
region, breaking-v7 refuse, the exact floor==6 boundary, the upgrade
round-trip + idempotency, the synthetic segment-rewrite path, and the two
guard checks (G)/(H) above).
`t/013_format_compat_replica.pl` proves the floor propagates through WAL
replay (a standby refuses a synthetic future floor exactly as the primary
would, and reads an additive region through unchanged).
`t/014_upgrade_crash.pl` proves the upgrade round-trip and crash-mid-upgrade
atomicity (a crash between the swap and re-stamp is impossible by
construction, not merely untested). All of the above run under the
cassert+UBSan hardening gate (`docs/adr/0008-cassert-ubsan-ci.md`) — a
metapage struct-grow and cross-version read is exactly its alignment/format
bug class.

## Alternatives considered

- **Absolute never-REINDEX.** Guaranteeing that no future format change will
  ever require a REINDEX over-constrains all future format work: it would
  rule out a legitimate breaking change (an existing field reshaping, as
  v4→v5's `max_impact` replacement genuinely required) or force every such
  change to carry a permanent shim reading both shapes forever. The chosen
  contract instead makes additive changes free and gives breaking changes a
  documented, bounded escape hatch (`bm25_upgrade`, and REINDEX as the honest
  fallback when no online transform exists) — a guarantee that can actually
  be kept indefinitely rather than one that would eventually be broken by a
  real need.
- **Read-only backward compatibility** (new binaries read old formats, but
  never write a way for old binaries to read new formats safely). This
  covers the common minor-version case but not the roadmap's actual
  requirement — physical replicas and rolling deploys mean a genuinely OLDER
  binary must survive reading NEWER data mid-rollout. A read-only-backward
  design would leave that direction unaddressed: new data would need to be
  either invisible to an old binary or it would mis-parse. It also does
  nothing for the "old data never migrates" problem — existing indexes stay
  frozen at their build-time format forever, so an install that upgrades
  degrades feature adoption on old data at every future format bump. Rejected
  as insufficient on the direction that actually motivated this ADR.
- **Capability-bitmap gating** (make the gate consult `feature_flags`
  per-capability, not just the scalar floor). This is YAGNI today: nothing in
  the current or planned feature set is genuinely orthogonal-optional in a
  way a single monotonic floor can't express — every real capability so far
  (positions, multifield, WAND impacts) is either always-on-if-present or
  gated by content-detection already. `feature_flags` is kept and populated
  (informational, consumed by `bm25_upgrade` and `bm25_stats`) so the data
  exists if a genuinely optional, independently-togglable capability shows up
  later, but the gate itself decides on the floor alone. Per-feature
  negotiation can be layered on top of that data later without another format
  break, if it's ever actually needed.
- **Deferring the upgrade engine** until a real breaking change needs it.
  Rejected: the user chose to build the skeleton (registry, atomic-swap
  re-stamp, packaging, rollout procedure) now, exercised end to end via a
  test-only synthetic transform, rather than leaving it as an untested,
  speculative shape that would first be proven correct under the pressure of
  an actual future break.

## Consequences

**What becomes easier.** Additive format changes — a new optional trailing
region, a new page type, filling a zero sentinel — are now free: no REINDEX,
no version bump beyond leaving `min_read_version` unchanged, no coordination
burden on existing installs. This is the majority of format evolution based
on the M4/M5 precedent (both filled reserved v4 surface with no break).

**What becomes harder / stays hard.** A genuinely breaking change (an
existing field reshaping, or growing a packed multi-record struct) still
raises the floor and still needs a REINDEX for any binary older than the new
`BM25_OLDEST_READABLE` — this contract does not, and cannot, make every
future change free. It does add a real online path for the common case: a
registered transform plus `bm25_upgrade`, so a breaking change against
sealed segments no longer forces a heap rescan the way REINDEX does. A
breaking change with no feasible online transform is still an honest ERROR
with a REINDEX hint, not a silently-attempted unsafe rewrite.

**What is now committed to permanently.** `min_read_version >= 5` and
`BM25_OLDEST_READABLE >= 5` forever (the v5 per-block-impact precondition).
The additive-vs-breaking classification rule, including the packed-record
carve-out, is now the binding standard for every future on-disk change — a
change that "only adds a field" to `BM25DictEntry`/`BM25BlockHeader`/the
segcat entry must still be treated and tested as breaking, not additive.

Also permanent, and cheaper to state than to rediscover: the two guards above are
now preconditions of the contract, not optimizations. **Every new `BM25_PAGE_*`
flag must be added to `BM25_PAGE_ALL_KNOWN`** (an omission does not fail any test
of the new page type — it fails an OLD binary vacuuming a NEW index, which no
single-version test can see), and **every new metapage writer must route through
`bm25_meta_set_pd_lower`** rather than assigning `pd_lower`. Both failure modes
are silent and only appear under version skew, which is precisely the situation
this ADR exists to make safe.

**Follow-up / known bounds.** The rewrite path's `BM25_RETIRED_PER_PAGE` drop
cap means a large-segment-count index cannot cross a real breaking change in
one atomic step today; `bm25_merge()` first is the documented workaround, and
a batched rewrite (folding the re-stamp into only the final batch's swap) is
the follow-up needed to lift the bound once a real breaking transform is
registered. The transform registry itself is empty of real entries — proven
via the synthetic test-only transform, not yet by a shipped feature — so the
first real breaking change will be the first genuine end-to-end exercise of
the dispatch-and-rewrite path in production.

## Addendum (2026-08-16)

One sentence in the unknown-page-kind bullet above is now narrower than it reads.
It says "an orphan is stamped `BM25_PAGE_DELETED` with `InvalidFullTransactionId`,
which `bm25_page_alloc` reuses IMMEDIATELY (no horizon wait)". That is still true
of the case this record reasons about — a future page kind is by definition not
`BM25_PAGE_PENDING`, so its stamp is still the immediate-reuse one, and the leak
guard is still the only thing standing between a v7 chain and a v6 seal. It is no
longer true of orphans *in general*: as of issue #135 `bm25_reclaim_orphans` gives
`BM25_PAGE_PENDING` orphans a real `retire_xid`, because a pending page carries
`seg_gen = 0` and so has no `bm25_seg_page_validate` backstop when a pre-seal scan
snapshot still holds its block number. See ADR 0019's addendum for the decision
and the asymmetry. Nothing about the additive contract or the leak guard changes.

## Addendum (2026-10-05)

Two additive regions were written on 2026-10-05 on this record's rule, both with zero as the
safe value and neither involving a `format_version`, `min_read_version`, feature bit or read
gate.

- **Key stamp (#292, ADR 0112).** A 12-byte `BM25KeyStamp` follows the `store_positions` flag
  array on the field-config page. Presence is decided by `pd_lower` and the magic. An older
  binary never reads past the flag array; an index without a stamp keeps the pre-stamp check.
- **Orphan-sweep evidence (#300, ADR 0116).** The metapage grows past its old 104 bytes:
  `reserved_tail_pad` at 100 (the old trailing padding, named and never interpreted),
  `orphan_ops_begun` at 104, `orphan_ops_done` at 108 and `swept_epoch` at 112, `sizeof` 120,
  offsets pinned by `StaticAssertDecl`. Older binaries `memcpy` exactly 104 bytes and
  `bm25_meta_set_pd_lower` only raises `pd_lower`, so an older writer carries the tail through.
  On every existing metapage the bytes are zero, and zero reads as "never swept", which makes
  the next VACUUM sweep: the safe value.

The unknown-page-kind argument above still holds: the sweep leaks any page whose flag bits it
does not know. Its stamp for orphans is no longer uniform (ADR 0019's addendum of 2026-08-16),
and the sweep now also skips unreachable pending pages by an epoch rule (ADR 0117).

## Addendum (2026-10-05, PRs #331-#350)

- **The rollout procedure above applies only once update scripts exist.** The project is
  pre-release: `default_version` is `1.0`, the only install script is
  `bm25_native--1.0.sql`, and it is edited in place. Picking up SQL-level changes today means
  `DROP EXTENSION bm25_native CASCADE` and recreating the indexes, the drop-and-reindex
  migration ADR 0013 already names. Step (2) (`ALTER EXTENSION ... UPDATE`) has no script to
  run until a `1.0--1.1` script ships. README and ARCHITECTURE.md now say so (#311 CI-05, PR
  #340). Residual: the release-time freeze gate (a sha256 of the shipped 1.0 script) is
  release work, not done.
- **More struct pins, on this record's additive rule.** `StaticAssertDecl` pins now cover
  `BM25PageOpaque` (24), `BM25KeymapHeader` (8), `BM25DictEntry` (20), `BM25BlockHeader`
  (16, with its 2-byte hole at offset 2) and `BM25PendingTermEntry` (8), beside the earlier
  metapage, catalog, retired-entry, segment-header and pending-document pins (#313
  SEGREAD-08, PR #345). `bm25_debug_layout()` reports the same offsets to SQL (ADR 0126).
  No layout changed.

## Addendum (2026-10-06, 1.0.0 public release)

- **The 1.0 install script is frozen.** The 2026-10-05 addendum's "edited in place"
  posture ends with the 1.0.0 public release. `bm25_native--1.0.sql` is now immutable:
  any later SQL change ships as a `bm25_native--1.0--X.sql` update script with a
  `default_version` bump, and the rollout procedure above (binaries on every node →
  `ALTER EXTENSION ... UPDATE` → `bm25_upgrade` on the primary) applies as written from
  the first such script onward. Editing a released install script would give fresh
  installs a different catalog from existing ones, and PostgreSQL never re-runs an install
  script on a database that already ran `CREATE EXTENSION`, so nothing would reconcile them.
  The drop-and-recreate migration for SQL changes is retired with the edit-in-place rule.
- **The release-time freeze gate is done** (the residual the 2026-10-05 addendum left open).
  `test/check_packaging_identity.py` pins `bm25_native--1.0.sql` by sha256 in
  `RELEASED_SCRIPTS`. CI runs that check before any build, but CI is dispatched manually
  (ADR 0106), so an edit to the shipped script fails the next CI run, not the commit that
  made it. The hash folds CRLF to LF so a Windows autocrlf checkout does not fail, and any
  other byte change, whitespace included, does fail. Every script a release ships, install
  or update, gets an entry; existing entries are never edited.
- **What a post-1.0 SQL change touches.** A new `bm25_native--1.0--X.sql`, listed in the
  Makefile's `DATA`; `default_version` and META.json's `version`/`provides.version`, bumped
  together (the packaging check ties them); and a `RELEASED_SCRIPTS` entry when it ships.
  "No `DROP EXTENSION`" holds for what an update script can express: a change that cannot
  be (reshaping the operator class, say) is a breaking release and must say so.
- ADR 0010's 2026-10-05 addendum (in-place edit, drop-and-recreate) and ADR 0082's
  "pre-1.0 posture" are historical as of this date. ADR 0013's drop-and-reindex path is
  the 0.x-to-1.0 rebrand migration and is unaffected.
