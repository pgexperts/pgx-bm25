---
id: 0054
title: The files PostgreSQL parses are 7-bit ASCII, enforced in CI
date: 2026-08-11
status: Accepted
summary: bm25_native--1.0.sql and the control files are transliterated to ASCII and gated by test/check_source_ascii.py, because CREATE EXTENSION validates the whole script against the server encoding and the extension was uninstallable in EUC_JP.
---

# 0054. The files PostgreSQL parses are 7-bit ASCII, enforced in CI

## Context

`CREATE EXTENSION` reads the extension script and validates **the entire file**
against the server encoding before executing any of it. `bm25_native--1.0.sql`
carried 11 comment lines containing em dashes (U+2014) and a Sigma (U+03A3).
The consequence was not cosmetic:

| Encoding | Pre-fix `CREATE EXTENSION bm25_native` |
|---|---|
| `UTF8` | Succeeds. |
| `LATIN1` | Succeeds — by accident. Every byte 00..FF is legal LATIN1, so the UTF-8 bytes are accepted as-is and the comment simply renders as mojibake. |
| `EUC_JP` | `ERROR: invalid byte sequence for encoding "EUC_JP": 0xe2 0x80`. `0xE2` is a valid lead byte, `0x80` is not a valid trail. **The extension could not be installed at all.** |

Verified live on PostgreSQL 18 in both directions: the pre-fix script fails as
above and the transliterated script installs cleanly.

Two properties kept this invisible for the project's whole life. Every CI gate
runs a C/UTF8 cluster (`build-and-test` via `pg_virtualenv`, `hardening` via
`initdb --no-locale`), so no gate ever parsed the script under a stricter
encoding; and the broken and fixed scripts produce **byte-identical** output
under every encoding CI does run, so no regression suite could distinguish them.
This is the same unobservability that let `META.json` and `.pgx-build.yml` rot
(ADR 0052) — the code and the correction are indistinguishable to every gate,
which is precisely why a gate had to be added rather than a fix merely applied.

The clean-slate review's finding on non-ASCII sources (#66.11) counts only the C
sources and grades the whole item cosmetic. That grade is right for `.c` and `.h`
files — a source file's encoding never reaches a server — and wrong for the SQL
script, which the finding never examined. ADR 0050 hit this from the other side:
it could not write EUC_JP regression coverage for the snippet ellipsis because
the extension would not install there, and had to reach a 7-bit-clean copy of the
script through `extension_control_path` to prove its own fix.

## Decision

Every file PostgreSQL itself parses — the extension script named by the
Makefile's `DATA`, and every `*.control` at the repo root — is **7-bit ASCII**,
with the non-ASCII characters transliterated (em dash to `--`, Sigma-doclen to
`sum(doclen)`) rather than re-encoded or declared.

`test/check_source_ascii.py` enforces it as a static CI gate, running before the
build alongside the packaging-identity check. It derives its file set from
`EXTENSION`/`DATA` and from `*.control` rather than hardcoding names, so a rename
cannot silently drop a file from the gate.

The cosmetic C-source sweep is a separate change. This decision is about the
files that decide whether the extension installs.

## Alternatives considered

- **Declare `encoding = 'UTF8'` in the control file** — rejected, and the reason
  is not the obvious one. This makes PostgreSQL *convert* the script from UTF-8
  to the server encoding rather than validate it, so it appears to fix EUC_JP,
  whose repertoire does contain a Sigma and a dash. It does not: verified live,
  the same script under `encoding = 'UTF8'` fails in **both** LATIN1 and EUC_JP
  with `character with byte sequence 0xe2 0x80 0x94 in encoding "UTF8" has no
  equivalent in encoding "<target>"`. Per-character conversion tests show why —
  in EUC_JP, U+03A3 SIGMA converts (`a6b2`) and U+2015 HORIZONTAL BAR converts
  (`a1bd`), but **U+2014 EM DASH does not**, because PostgreSQL's UTF8→EUC_JP
  table maps the JIS X 0208 dash at 0x213D to U+2015, not U+2014. LATIN1
  converts none of the three. Declaring an encoding trades an unconditional
  failure for a failure that depends on the intersection of every character used
  with every target repertoire — strictly harder to reason about, and still
  broken. ASCII is the only repertoire every server encoding contains.
- **Document a UTF-8 requirement and leave the script alone** — rejected. This is
  an install failure, not a documentation gap, and the requirement would be
  "bm25_native does not support EUC_JP databases" for the sake of eleven comment
  lines.
- **Fix only the C sources, as #66.11 scopes it** — rejected. That is the half
  with no functional consequence. Doing it alone would close the finding while
  leaving the real defect in place.
- **A `.gitattributes` / editor-level encoding rule** — rejected. It constrains
  how files are written, not what they contain, and nothing would fail when it
  was bypassed.

## Consequences

- **bm25_native installs in every server encoding.** EUC_JP verified live; the
  UTF-8 and LATIN1 paths are byte-unchanged apart from the comment text.
- **`t/018_snippet_encoding.pl` gains its EUC_JP node**, which ADR 0050 recorded
  as blocked. It is discriminating rather than duplicative: EUC_JP exercises the
  ellipsis *conversion* branch (JIS X 0208 `a1c4`) where LATIN1 exercises the
  ASCII fallback, so an implementation that always emitted `...` passes every
  LATIN1 assertion and fails the two EUC_JP ones. Confirmed by injecting exactly
  that hazard.
- **The gate covers only what PostgreSQL parses.** The C sources are out of scope
  here and still carry ~1,000 non-ASCII characters; the gate grows a second group
  when that sweep lands. `sql/` regression files are deliberately never covered —
  several hold intentional multibyte test data.
- **Two gate hazards were found by adversarial review and fixed before merge**,
  both of the silent-under-coverage kind that makes a gate worse than none:
  `str.splitlines()` treats U+2028/U+2029/U+0085 as line *boundaries* and
  therefore consumes them, so the gate reported "clean" on a file containing
  U+2028 — bytes `E2 80 A8`, the exact prefix it exists to catch, reproduced
  end-to-end as a live EUC_JP install failure. And reading only the first `DATA`
  assignment missed `DATA +=` and a second `DATA =` line, both of which ship a
  file. The gate now scans on `\n` only, collects every assignment form including
  backslash-continued, and refuses to report a pass when its own derivation
  breaks.
- **Comments in the shipped script are now less typographically pleasant.** That
  is the price, and it is small; the ADR bodies, `ARCHITECTURE.md` and the plan
  archives are unaffected and keep their em dashes.

## Addendum (2026-08-11)

The second group this record anticipated has landed: `src/*.{c,h}` is now
transliterated and covered by the same gate. 1,012 non-ASCII characters across
959 lines in 26 of 27 files -- 859 of them em dashes -- became ASCII by the
mapping in `test/check_source_ascii.py`'s failure message.

This half remains cosmetic, exactly as #66.11 graded it, and the reason for
doing it is not the one the finding gave. It is that "some files in `src/` are
ASCII" is not a property anyone can hold in their head or verify by eye, so the
choice was between sweeping all of it and gating, or leaving a rule nobody can
apply. `sql/` stays uncovered permanently: several suites carry deliberate
multibyte test DATA, which is the point of them.

The sweep was verified by COMPILING every translation unit before and after at
`-g0` and comparing the object files: all 22 byte-identical. That is a stronger
statement than "every occurrence was in a comment" -- a hand-written
comment/string-literal scanner gets desynchronised by character literals like
`'"'` in the snippet escaper, and did so here before being discarded in favour
of the compiler.
