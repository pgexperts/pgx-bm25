---
id: 0050
title: The snippet ellipsis is converted to the server encoding, not hardcoded
date: 2026-08-10
status: Accepted
summary: bm25_snippet's truncation marker goes through pg_unicode_to_server_noerror with an ASCII fallback, replacing three hardcoded UTF-8 bytes that were mojibake in LATIN1 and an outright encoding error in EUC_JP.
---

# 0050. The snippet ellipsis is converted to the server encoding, not hardcoded

## Context

`bm25_snippet` marks a truncated excerpt with U+2026 HORIZONTAL ELLIPSIS, emitted as the
literal bytes `"\xE2\x80\xA6"` at both the leading and trailing sites — unconditionally,
with no reference to the database's encoding. That is correct in exactly one server
encoding.

This is the **only** non-ASCII literal the extension writes on its own. Every other byte
`bm25_snippet` emits either came from the user's own column (already valid in the server
encoding by construction) or from the caller's tag arguments. That is why ADR 0046's
encoding sweep, which fixed how the analyzer *reads* text, did not reach it: the defect is
in what the extension *writes*.

Both failure modes were reproduced against live databases on PostgreSQL 18:

| Encoding | Pre-fix result |
|---|---|
| `LATIN1` | No error. Every byte is legal LATIN1, so the value silently carries `e2 80 a6` and renders as three mojibake characters. Byte-level output: `e280a6203c6d3e6563686f3c2f6d3e20e280a6`. |
| `EUC_JP` | `ERROR: invalid byte sequence for encoding "EUC_JP": 0xe2 0x80`. `0xE2` is a valid lead byte but `0x80` is not a valid trail, so the value is not legal EUC_JP and the query dies on conversion for the client. |

The LATIN1 case is the more dangerous of the two for a regression suite: it raises no
error, so only an assertion about the actual bytes can tell fixed from broken.

## Decision

The marker is produced by `pg_unicode_to_server_noerror(0x2026, buf)`, falling back to the
ASCII `"..."` when the server encoding cannot represent U+2026.

Using the server's own conversion rather than a UTF-8 test means encodings that *do* have
an ellipsis get the real character: verified live, EUC_JP now emits `a1 c4` (JIS X 0208),
not the fallback. In a UTF-8 database the call returns exactly the three bytes written
before, so no existing output changes — the full 88-suite run is unaffected.

`pg_unicode_to_server_noerror` was added in PostgreSQL 16. Verified by fetching
`src/include/mb/pg_wchar.h` at `REL_16/17/18_STABLE` (present) with `REL_12_STABLE` and
`REL_15_STABLE` as the negative control (absent), so the ref parameter was demonstrably
honored rather than returning a cached local copy. This extension's floor is 17.

## Alternatives considered

- **`GetDatabaseEncoding() == PG_UTF8 ? "…" : "..."`** — rejected. It is correct and needs
  no version floor, but it hands the ASCII fallback to every encoding that has a perfectly
  good ellipsis of its own, EUC_JP among them.
- **Always emit ASCII `"..."`** — rejected. It would change output in the one encoding
  everybody actually uses, churning expected output across three suites to fix a bug none
  of those suites is in.
- **Make the marker a parameter or a GUC** — rejected as a real but separate feature. The
  defect is that the current marker is wrong in some encodings, not that it is fixed.

## Consequences

- **Output changes only in databases where it was already wrong.** UTF-8 is byte-identical.
- **Regression coverage is `t/018_snippet_encoding.pl`, and it is LATIN1-only.** The EUC_JP
  case — the louder failure, and the one that exercises the conversion path rather than the
  fallback — cannot be covered yet, because **`bm25_native` cannot be installed in an
  EUC_JP database at all**: `bm25_native--1.0.sql` carries 11 comment lines with non-ASCII
  characters, and `CREATE EXTENSION` validates the whole script against the server encoding
  before any of this code is reachable. Proving the EUC_JP behaviour above required running
  against a 7-bit-clean copy of the script via `extension_control_path`.
- **That script-encoding defect is larger than this one and is filed separately.** Worth
  noting that review finding #66's "non-ASCII throughout" item is scoped to the C sources,
  where it is cosmetic (a `.c` file's encoding never reaches a server), and does not mention
  the SQL script, where it decides whether the extension installs at all.
- Every CI gate runs `initdb -E UTF8`, so no existing gate exercised this and none would
  have caught it. `sql/82`, `sql/39` Part 4 and (on the PR-H branch) `sql/84` Part 4 all
  presume a UTF-8 regression database.

## Addendum (2026-08-11)

The EUC_JP coverage this record listed as impossible is now in place. The blocker
named above — `bm25_native--1.0.sql`'s 11 non-ASCII comment lines making the
extension uninstallable in an EUC_JP database — was fixed under ADR 0054, which
transliterates the shipped script to 7-bit ASCII and gates it in CI.

`t/018_snippet_encoding.pl` therefore now runs both nodes: LATIN1 for the ASCII
fallback branch, EUC_JP for the conversion branch. The EUC_JP node confirms this
record's central claim against a real cluster rather than a hand-patched copy of
the script — the marker comes back as `a1c4`, the JIS X 0208 ellipsis, not the
`...` fallback.

One correction to the reasoning above, which does not change the decision. This
record's Context frames EUC_JP's problem as a *validation* failure, which it is
for the script. It does not follow that declaring an encoding would have solved
the class: `encoding = 'UTF8'` in the control file converts rather than validates,
and U+2014 EM DASH has no EUC_JP equivalent in PostgreSQL's conversion table
(which maps JIS 0x213D to U+2015 HORIZONTAL BAR instead), so that route fails too.
See ADR 0054's Alternatives for the measurements.
