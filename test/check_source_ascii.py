#!/usr/bin/env python3
"""Static gate: files that must be 7-bit ASCII must stay 7-bit ASCII.

Role in the system. This is NOT a style rule -- it guards a real, reproducible
install failure.

`CREATE EXTENSION` reads bm25_native--1.0.sql and validates the WHOLE script
against the SERVER ENCODING before executing any of it. The script carried 11
comment lines containing em dashes (U+2014, bytes E2 80 94) and a Sigma
(U+03A3), which meant:

  UTF8     fine.
  LATIN1   fine by accident -- every byte 00..FF is legal LATIN1, so the bytes
           were accepted and simply rendered as mojibake in a comment nobody
           reads. This is why nothing ever noticed.
  EUC_JP   E2 is a valid lead byte but 80 is not a valid trail, so the script is
           not legal EUC_JP at all. `CREATE EXTENSION bm25_native` died with
           `invalid byte sequence for encoding "EUC_JP": 0xe2 0x80` before a
           single line of it ran. The extension could not be installed AT ALL in
           an EUC_JP database (M3 #66.11, which scoped the finding to the C
           sources -- where it genuinely is cosmetic -- and never looked at the
           SQL script, where it is load-bearing).

Scope note. The two groups below are NOT equally serious, and the docstring
above is about the first one. For the C sources a file's encoding never reaches
a server, so their non-ASCII was genuinely cosmetic -- that is what M3 #66.11
graded, correctly, for the half of the tree it looked at. They are swept and
gated anyway for two reasons: the transliteration is only worth doing once, and
"some files in src/ are ASCII" is not a property anyone can hold in their head
or check by eye. Nothing under sql/ is covered and nothing should be -- several
regression suites carry deliberate multibyte test DATA, which is the point of
them.

Why a gate rather than review: the broken script and the fixed script produce
byte-identical output under every encoding CI runs (`pg_virtualenv` and
`initdb --no-locale` are both C/UTF8), so no suite -- and no amount of green
CI -- can notice this rotting back in. Same unobservability that let META.json
and .pgx-build.yml rot (see test/check_packaging_identity.py, ADR 0052).

The shipped-file list is DERIVED from the Makefile's EXTENSION/DATA rather than
hardcoded, so renaming the script cannot silently drop it from the gate.

Deliberately dependency-free (stdlib only): it runs in CI before any build step.

Usage: python3 test/check_source_ascii.py [repo_root]
Exit 0 = clean, 1 = at least one non-ASCII byte (each offender printed).
"""

import os
import re
import sys
import unicodedata

def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def offenders(path):
    """Yield (line_no, col, char, name) for every non-ASCII character.

    Read as bytes and decoded permissively: the point is to report what is
    there, including in a file that is not valid UTF-8 at all (which would
    itself be a finding worth printing rather than crashing on).

    Split on "\\n" ONLY -- never str.splitlines(), which treats U+2028 LINE
    SEPARATOR, U+2029 PARAGRAPH SEPARATOR and U+0085 NEL as line boundaries and
    therefore CONSUMES them, so they would never be enumerated as characters.
    U+2028 encodes as E2 80 A8: the same E2 80 prefix that is illegal in EUC_JP
    and the whole reason this gate exists. A gate that reports "clean" on the
    exact byte shape it was written to catch is worse than no gate.
    """
    raw = read_bytes(path)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        yield (0, 0, None, f"not valid UTF-8: {e}")
        text = raw.decode("utf-8", "replace")
    for lineno, line in enumerate(text.split("\n"), 1):
        for col, ch in enumerate(line, 1):
            if ord(ch) > 0x7F:
                try:
                    name = unicodedata.name(ch)
                except ValueError:
                    name = "unnamed"
                yield (lineno, col, ch, name)


def makefile_var(makefile, name):
    """Every value assigned to `name`, across =, +=, := and := forms.

    Returns None if the variable is never assigned.

    Collecting ALL assignments rather than the first is the point. `DATA = a.sql`
    followed by `DATA += b.sql` ships both files, and a second `DATA =` line
    wins outright under make's last-assignment-wins -- in each case reading only
    the first match silently leaves a SHIPPED file unchecked, which is the one
    failure direction this gate must not have. Under-covering while printing
    "clean" is worse than crashing.
    """
    # Join backslash-continued lines first, so a continued DATA is read whole
    # rather than as a bogus "\" entry.
    joined = re.sub(r"\\\n\s*", " ", makefile)
    values = []
    seen = False
    for m in re.finditer(rf"^{name}\s*(?::=|\+=|=)\s*(.*)$", joined, re.M):
        seen = True
        # Strip a trailing `#` comment; make would not treat it as a filename.
        values.extend(m.group(1).split("#", 1)[0].split())
    return values if seen else None


def c_sources(root):
    """Every src/*.c and src/*.h, globbed rather than listed.

    Globbed on purpose: a listed set would silently stop covering a file added
    after this was written, and an uncovered file is exactly the state this gate
    exists to prevent.
    """
    out = []
    d = os.path.join(root, "src")
    for name in sorted(os.listdir(d)):
        if name.endswith((".c", ".h")):
            out.append(os.path.join("src", name))
    return out


def shipped_files(root):
    """The files PostgreSQL itself reads at CREATE EXTENSION time.

    Two sources, both derived rather than hardcoded so a rename cannot silently
    drop a file from the gate:

      - every *.control at the repo root. Not just `<EXTENSION>.control`:
        PostgreSQL also reads a per-version `<EXTENSION>--<VERSION>.control` if
        one exists, and that file is named in neither EXTENSION nor DATA, so
        deriving only from those two variables would never reach it.
      - everything in the Makefile's DATA, which is the script set installed
        into the extension directory.
    """
    with open(os.path.join(root, "Makefile"), encoding="utf-8") as f:
        makefile = f.read()

    ext = makefile_var(makefile, "EXTENSION")
    data = makefile_var(makefile, "DATA")
    if not ext or data is None:
        raise SystemExit(
            "check_source_ascii: cannot derive EXTENSION/DATA from the Makefile "
            "-- the derivation is broken, fix it rather than trusting a pass")

    controls = sorted(n for n in os.listdir(root) if n.endswith(".control"))
    # The main control file must be among them; if it is not, the layout has
    # changed underneath this script and the derivation can no longer be trusted.
    main_control = f"{ext[0]}.control"
    if main_control not in controls:
        raise SystemExit(
            f"check_source_ascii: {main_control} not found at the repo root "
            "-- the derivation is broken, fix it rather than trusting a pass")

    # dict.fromkeys: de-duplicate (a file can appear in DATA twice) while
    # keeping a stable, readable order.
    return list(dict.fromkeys(controls + data))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..")

    groups = [
        ("shipped to the server (CREATE EXTENSION parses these against the "
         "server encoding)", shipped_files(root)),
        ("C sources", c_sources(root)),
    ]

    failures = 0
    for label, rels in groups:
        print(f"{label}:")
        if not rels:
            print("  FAIL no files matched -- the derivation above is broken")
            failures += 1
            continue
        for rel in rels:
            path = os.path.join(root, rel)
            if not os.path.isfile(path):
                print(f"  FAIL {rel}: does not exist")
                failures += 1
                continue
            bad = list(offenders(path))
            if not bad:
                print(f"  ok   {rel}")
                continue
            failures += len(bad)
            print(f"  FAIL {rel}: {len(bad)} non-ASCII character(s)")
            for lineno, col, ch, name in bad[:10]:
                where = f"{rel}:{lineno}:{col}"
                if ch is None:
                    print(f"         {where}: {name}")
                else:
                    print(f"         {where}: U+{ord(ch):04X} {name} ({ch!r})")
            if len(bad) > 10:
                print(f"         ... and {len(bad) - 10} more")

    print()
    if failures:
        print(f"source ascii: {failures} non-ASCII occurrence(s)")
        print("Transliterate, do not re-encode. House mapping: EM/EN DASH -> "
              "'--', GREEK CAPITAL SIGMA -> 'sum', SECTION SIGN -> 'section', "
              "RIGHTWARDS DOUBLE ARROW -> '=>', RIGHTWARDS ARROW -> '->', "
              "MULTIPLICATION SIGN -> 'x', GREEK SMALL THETA -> 'theta', "
              "GREEK CAPITAL DELTA -> 'delta', HORIZONTAL ELLIPSIS -> '...', "
              "DOUBLE VERTICAL LINE -> '||', MIDDLE DOT -> '*'.")
        return 1
    print("source ascii: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
