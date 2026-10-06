---
id: 0067
title: A changed key_field is refused at INSERT time, not at DDL time or seal time
date: 2026-08-20
status: Accepted
summary: bm25_insert rejects a row whose resolved key_field configuration disagrees with the KEYMAP of the index's existing segments, because amoptions cannot see the index to refuse the ALTER and a seal-time refusal would wedge the index once divergent rows are already queued.
---

# 0067. A changed `key_field` is refused at INSERT time, not at DDL time or seal time

## Context

`key_field` is structural: it fixes the on-disk `BM25KeymapHeader.key_type` and
`key_size` of every segment. It is registered as a plain string reloption, and
`AccessExclusiveLock` there sets the LOCK LEVEL for `ALTER INDEX ... SET`, not a
prohibition — so the ALTER is accepted, and `bm25_insert` re-resolved `key_field`
from LIVE reloptions on every row.

Rows written after such an ALTER carried a different key type into their pending
doc headers, and the next seal published a segment whose KEYMAP disagreed with
every existing one. Nothing gated that: unlike the analyzer, `key_field` has no
fingerprint on the metapage. The merge then discovers key config from whichever
segment it reads first and silently re-keys everything to that width. Issue #147
(BUILD-05).

## Decision

`bm25_insert` calls `bm25_validate_key_config_for_insert` before the row reaches
the pending chain: it reads the first segment's `bm25_seg_keymeta` and errors when
`key_type`/`key_size` disagree with what this row resolved to.

Both directions across the key/keyless boundary are checked. `bm25_seg_keymeta`
sets `(BM25_KEY_NONE, 0)` before returning false, so a keyless segment yields a
directly comparable pair and needs no special case.

## Alternatives considered

- **Reject the ALTER in `bm25_options(validate = true)`** — the obvious fix, and what
  the issue proposed. **Not implementable**: `amoptions` is
  `(Datum reloptions, bool validate)`. It never receives the index, so it can
  neither distinguish a CREATE from an ALTER nor compare against what the index was
  built with, and core exposes no AM hook on `ALTER ... SET` that does.
- **Refuse at seal time** — cheaper to reach, and it was built and tested that way
  first. **Strictly worse**: by then the divergent rows are already queued, so the
  only options are publish (corruption) or error — and erroring WEDGES the index,
  because every later seal, autovacuum's included, hits the same rows and fails
  identically, with REINDEX the only exit. That trades silent corruption for an
  operational outage. Refusing the INSERT keeps the chain homogeneous, so VACUUM
  keeps working and restoring the reloption recovers with no REINDEX. The regression
  suite pins exactly that recovery.
- **Bake `key_type`/`key_size`/`key_attno` into the field-config page at build time** —
  the fuller fix, and it would additionally remove a per-row `SearchSysCache1` +
  `untransformRelOptions` + tupdesc scan from the insert path. Deferred as a larger
  change; it is the only thing that would close the residual below.

## Consequences

Coverage is honest and bounded. This catches any `key_type` or `key_size` change,
including to and from keyless — the divergences that corrupt the on-disk KEYMAP. It
does NOT catch re-pointing `key_field` at a DIFFERENT column of the SAME type and
width (two `int4` INCLUDE columns): the stored keys change meaning while the
configuration stays byte-identical. Detecting that needs the resolved attno
persisted per index, i.e. the deferred alternative above.

Cost is one segcat read plus one segment-header read per row, and only when the
index already has segments. Measured worst case on bulk INSERT of deliberately tiny
documents: ~0.5 us/row, ~6-8%; on realistic documents tokenization dominates and it
disappears. The allocation lands in `bm25_insert`'s per-row scratch context.

Stopping at the FIRST segment is sound by induction now that both directions are
checked: every row admitted since has been validated against the same set, so the
set cannot have become heterogeneous through this path. An index already mixed by a
binary predating this check is not repaired by it — REINDEX is the cure, as the
errhint says.

## Addendum (2026-10-04)

**The per-row check no longer walks the catalog.** The Consequences' cost note, one
segcat read plus one segment-header read per row, describes the old read. Issue #270
([0107](0107-catalog-walkers-require-the-metapage-singleton.md)) found that the check ran
holding no singleton and that its walk could read a reused catalog page, which fails a
healthy INSERT with XX002. `bm25_validate_key_config_for_insert` now takes entry 0 from
`bm25_segcat_first_entry`, which reads the metapage and the catalog's root page under the
metapage SHARE, and then does the same segment-header and key-metadata reads as before.
The check still stops at the first segment, and the induction argument above is
unchanged, since every segment shares one key configuration. The measured cost of
roughly 0.5 us per row was taken against the old whole-catalog read and was not
re-measured.

**The lock-level premise.** The Context says `AccessExclusiveLock` sets the lock level
for `ALTER INDEX ... SET` on this reloption. That holds only in a backend that has
already registered it. bm25 registers its reloptions lazily, on the first `bm25_options`
call in a backend, and core takes an ALTER's lock level from the options registered in
the backend running it, so from a backend that has not loaded a bm25 index's relcache
entry the ALTER takes only ShareUpdateExclusiveLock (see ADR 0012's 2026-10-04
addendum). That reinforces the Decision rather than weakening it: this INSERT-time
check, which fails loud, is the defence, and no lock level was ever what kept a
changed `key_field` out.

## Addendum (2026-10-04)

The fresh-eyes review of this date found a hole in the segment-0 comparison: when the catalog has no segments yet (an index created on an empty table, until its first seal), the check compares nothing and a key_field change is accepted. A width change (int4 -> int8) then fails every seal and VACUUM, and every INSERT once pending passes seal_threshold; reverting the reloption does not recover, contrary to this record's revert-recovers property in that window (REINDEX, or VACUUM with INDEX_CLEANUP off, gets past it). Same-width changes are silent and need REINDEX: a different column, uuid -> text, and keyed -> RESET -> keyed or keyless -> keyed (later rows sealed with key 0). #292 proposes stamping the build-time key identity.

## Addendum (2026-10-05)

The key stamp of ADR 0112 (#292, decided 2026-10-05) closes both residuals this record
accepted for an index built by the current binary: an empty segment catalog now checks the
row against the stamp, and a same-type different-column change is caught because the stamp
carries `key_attno`. The mechanism here, refusal at INSERT, stands. An index with no stamp
keeps exactly the segment-0 check described above, with its residuals, until REINDEX, and the
pending drain refuses a chain whose live records disagree on key type and width.
