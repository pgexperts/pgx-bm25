---
id: 0112
title: The build stamps the key identity on the field-config page and every INSERT is checked against it
date: 2026-10-05
status: Accepted
summary: ambuild and ambuildempty write (key_type, key_size, key_attno) as an additive tail of the field-config page; bm25_insert compares each row with it, an unstamped index keeps the segment-0 check, and the pending drain refuses a chain whose live records disagree on key type and width.
---

# 0112. The build stamps the key identity on the field-config page and every INSERT is checked against it

## Context

`key_field` is structural: it fixes every KEYMAP's type and width and the meaning of every
stored key. It is also an ordinary reloption, so `ALTER INDEX ... SET (key_field = ...)`
is accepted, and `bm25_insert` re-resolves it for every row. ADR 0067 made INSERT the
refusal point, because `amoptions` cannot see the index and a seal-time refusal would
wedge an index whose divergent rows are already queued. It compared a row's
`(key_type, key_size)` with segment 0's KEYMAP header, and recorded two residuals: a
same-type different-column change passes, and an empty catalog checks nothing.

Issue #292 showed the empty catalog is the common case. An index created on an empty
table and then loaded spends its early life with no segment. Every `key_field` change
went into the pending chain there. A width change then wedged every seal and VACUUM, and
every INSERT past `seal_threshold`, with no recovery short of REINDEX. Keyed to keyless,
keyless to keyed, an int4 column A to column B, and uuid to text sealed silently mis-keyed
rows. With segments present, a same-type different-column change still passed.

The user decided on 2026-10-05 on the stamp below, with no `object_access_hook`.

## Decision

**The stamp.** `ambuild` and `ambuildempty` write the resolved `key_type`, `key_size` and
`key_attno` as a 12-byte `BM25KeyStamp` (magic `'KEYM'`) after the `store_positions` flag
array on the field-config page. It is written once and is immutable for the life of the
relfilenode, so reading it needs no lock.

**It is additive (ADR 0009).** An older binary never reads past the flag array. Presence
is decided by `pd_lower` and the magic, and no `format_version` or `min_read_version`
gate is involved, which would be unsound while `bm25_upgrade`'s registry is empty.

**Every INSERT is compared with all three fields**, segments or not, before the row
reaches the pending chain. The error is `ERRCODE_FEATURE_NOT_SUPPORTED` with a REINDEX
hint, naming the columns. Reverting `key_field` still recovers the index without a
REINDEX.

**An unstamped index keeps the segment-0 check.** An index built before this change has
no stamp. It keeps ADR 0067's comparison of type and width against segment 0, which
reads entry 0 under the metapage lock (ADR 0107), and nothing at all while there is no
segment.

**The drain refuses a mixed chain.** `bm25_pending_drain` compares the `(key_type,
key_size)` of every live record and raises `ERRCODE_FEATURE_NOT_SUPPORTED`, with a
REINDEX hint, when two disagree, keyless included. This is the backstop for an unstamped
index with no segment yet and for a chain an older binary already mixed. Restoring
`key_field` does not repair rows already queued.

**Debug probes.** `bm25_debug_keystamp` shows the stamp and `bm25_debug_clear_keystamp`
strips it so the suites can exercise the unstamped fallback; both are owner-only and
revoked from PUBLIC. Suites `sql/126_key_identity_stamp` and `sql/127_key_stamp_fallback`.

## Alternatives considered

- **Compare against the first valid pending record when there is no segment.** The
  minimum fix, but weaker: it would have to run under the append's metapage lock to be
  race-free, because `ALTER INDEX` takes only `ShareUpdateExclusiveLock`, which does not
  conflict with inserters (ADR 0012's addendum). It costs a pending-head read per row and
  still misses the same-type different-column case.
- **Refuse at DDL time from `bm25_options`.** `amoptions` receives only the merged Datum
  and `validate`, so it can neither see the old value nor tell CREATE from ALTER. A
  WARNING fails for the same reason.
- **An `object_access_hook` on `OAT_POST_ALTER` of `pg_class`, comparing with the stamp.**
  It could refuse the ALTER at DDL time, but it fires on every `pg_class` alter, the new
  tuple is not visible without `SnapshotSelf`, and hook chaining is fiddly. The user
  decided against it; the stamp would serve it later.
- **A `format_version` read gate.** Unsound while the upgrade registry is empty (ADR 0009).

## Consequences

- Fixes both of ADR 0067's residuals for every index built by this binary: the empty
  catalog and the same-type different-column change.
- `bm25_upgrade` cannot stamp, so an index built before this change keeps the weaker
  segment-0 check, with its ADR 0067 residual, until REINDEX. The drain's refusal turns
  the type-and-width half of that residual into a loud seal error rather than a silently
  mis-keyed segment.
- An index already mixed is not repaired by either mechanism; REINDEX is the cure.
- Untested: the corrupt-stamp branches, and the accumulator's width guard, which is now a
  backstop (#309).

## Addendum (2026-10-05, PRs #331-#350)

The stamp now also serves the ranked scan (#303.I, PR #343). Both ranking builders used to
learn `(key_type, key_size)` from the first keyed segment, else from the pending records, so
on a keyless index every ranked scan walked and decoded the whole pending chain a second time
to learn "keyless". `bm25_ranked_key_config` answers from the stamp, a page the scan has
already read. An unstamped index keeps the full discovery, including the walk to the end of a
keyless pending run: stopping at the first keyless record would be unsound there, because
nothing on INSERT stops keyed records from following keyless ones while there is no segment.
