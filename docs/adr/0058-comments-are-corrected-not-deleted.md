---
id: 0058
title: Stale comments are corrected in place, and cite symbols by name
date: 2026-08-11
status: Accepted
summary: Comments that contradict the code are rewritten rather than deleted, because several of them are the declared contract for a struct or a trust boundary; and cross-references name a function or a constant instead of a line number, because every line-number citation in the tree had already rotted.
---

# 0058. Stale comments are corrected in place, and cite symbols by name

## Context

Issues `#63` (4 findings) and `#64` (34 findings), plus the documentation halves
of `#58.4`, `#58.5`, `#59.6`, `#62.2` and `#62.4`, catalogued comments whose
claims the code does not support. They were not evenly distributed noise. They
clustered into a few shapes, and the shapes matter more than the sites:

- **Modules described as dead that answer every query.** `bm25_wand.c`'s file
  header stated "Nothing here is wired into `bm25_scan_build_ranking_once` yet"
  and that the file was reachable only from debug probes. It is the DEFAULT
  ranked-scan path (`bm25_wand_top_k` defaults to 100). The first thing a reader
  learned about the module producing every ranked result was that it was dead
  code.
- **Safety-critical bounds labelled test scaffolding.** Three separate comments
  said "nothing calls `bm25_block_ub` yet". It runs twice per cursor on every
  ranked scan, and it must dominate every real per-posting contribution -- a
  1-ULP-low change silently prunes documents out of every production top-k, with
  no error.
- **Asserted guarantees nothing implements.** A dictionary lookup documented as
  giving "the same O(log) page-granularity pruning the contract intends" is an
  unpruned linear scan; the same file describes the identical traversal correctly
  300 lines later. Comments claimed `GenericXLogStart` opens a critical section;
  `START_CRIT_SECTION` lives entirely inside `GenericXLogFinish`, and appears in
  exactly two places in this tree, neither of them on the swap path.
- **Version and milestone labels frozen at a past release.** The format header
  said `format_version /* = 6 */` and titled itself "on-disk format v6" while
  `BM25_FORMAT_VERSION` was 7.
- **References to symbols that do not exist.** A dedupe contract named its
  consumer `rank_seg_cb`, a symbol found nowhere in the tree.

Doing nothing was not an option because these are not decoration. `bm25.h` and
`bm25_format.h` are the declared contracts for the scan struct and for every
on-disk byte; `#63.1`'s comment promised a validation guarantee at the trust
boundary that does not exist. And nothing in the toolchain can object: a comment
is invisible to the compiler and to all 92 regression suites, so this rots
silently and only ever gets worse.

## Decision

Stale comments are **corrected in place, not deleted**, and every cross-reference
**names a function, struct or constant rather than a line number**.

Three rules follow from it:

1. **A version number is written down in exactly one place.** Prose says
   `BM25_FORMAT_VERSION` and does not restate its value. A version literal
   survives in a comment only where it is a historical claim about what that
   version introduced and a current reader still has to handle it -- which is
   real here, because `bm25_meta_validate` is a two-directional floor, not an
   equality test, so an index stamped 5 or 6 is read REINDEX-free by a build at
   7 and keeps its own older stamp on disk. "The format is vN" is therefore not
   a safe sentence about any particular index.
2. **A milestone tag is allowed only as history.** "M3 wrote exactly one
   `BM25FieldConfig`" may stay where a back-compat reader still handles that page
   shape; "field_count is 1 in M3" may not, because it reads as a claim about
   the current format.
3. **The finding's own suggested wording is untrusted input.** It is re-derived
   against the tree like any other claim.

## Alternatives considered

- **Delete the stale comments instead of fixing them.** Cheapest, and it cannot
  introduce a false claim -- the one failure mode that matters here. Rejected
  because it destroys the most valuable comments in the tree: the ones stating a
  struct's contract, a trust boundary's validation duty, or why a lock ordering
  is what it is. Deleting `#63.1` would have removed the qtree field's contract
  rather than correcting it. The stale half was wrong; the surrounding argument
  was not.
- **Restamp the version literals to 7.** Rejected on the evidence in front of
  us: a hardcoded literal is precisely what rotted, in six places, and would rot
  again at the next bump. This is why rule 1 forbids the number rather than
  updating it.
- **Take the code option where a finding offered one.** Several findings offered
  a fix that changes behavior -- implement `amvalidate`, add a per-page first-key
  header to make the dictionary lookup genuinely O(log), drop the unused `heap`
  parameter, derive a loop bound from `lengthof(roots)`. All declined here and
  left for their own PRs. A docs-only change can be verified to have changed
  nothing (ADR 0059); a mixed one cannot, and the churn would bury the code
  change in 26 files of prose. This is the split-by-risk rule that made PR-K's
  three-way split work.
- **Add a CI gate that resolves every `bm25_*` identifier named in a comment.**
  Attractive, and the natural continuation of `check_source_ascii.py` and
  `check_packaging_identity.py`. Deferred rather than rejected: it needs a real
  answer for historical references that are deliberately dead, and bundling a new
  gate into the sweep it was meant to police is the wrong order.

## Consequences

- The comments that describe the WAND engine, the impact codec's trust boundary,
  the dictionary lookup's real cost, and the pending/segment dedupe now match the
  code. A reader triaging a wrong-ranking report is no longer told the responsible
  module is dead.
- **`#64.19` and `#64.33`/`#64.34` turned out to be already fixed** -- `bm25_validate`
  is now a real opclass check, and the `blocks_skipped` counter already carries its
  `!cand_is_resident` guard in both code and docs. Writing the finding's suggested
  text ("the AM performs no opclass validation") would have introduced a brand-new
  false claim into a PR whose entire purpose is removing them. Re-verification is
  not optional here; it is the work.
- **Two findings were refuted, not fixed.** `#64.31`'s prescribed lock order
  (`LIVE -> segcat -> meta`) is the pre-fix order; the code locks meta first and
  the comment already said so. `#62.2`'s "any user with CREATE can shadow
  `english_stem`" overstates: `pg_catalog` is searched implicitly first, so the
  hijack needs a `search_path` naming `pg_catalog` after another schema. Both are
  recorded in the corrected comments at their narrower, true scope.
- Enumerating each defect by SHAPE rather than by the sites the issues named
  found survivors the issues missed -- in `bm25_keymap.c`, `bm25_phrase.c`,
  `bm25_pending.c`, `bm25_query.c`, `bm25_segment.c`, `bm25_upgrade.c`,
  `README.md` and `ARCHITECTURE.md`, none of which the issues listed. Fixing only
  the listed sites would have left the tree contradicting itself, which is worse
  than uniform staleness: a reader finding two disagreeing comments has no way to
  tell which was updated.
- The unpleasant part: nothing prevents this recurring. There is still no gate.
  The next reader to move a function and not its comment reintroduces exactly
  these defects, and CI stays green. The deferred identifier-resolution gate is
  the only real countermeasure and it is not built.
- Two follow-ups fell out and are deliberately NOT in this change, because both
  need executable edits: the `key_field` reloption's user-visible description
  string still says "heap column name" when resolution is against the index
  descriptor, and clangd reports unused includes across at least nine files.
