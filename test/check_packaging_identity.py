#!/usr/bin/env python3
"""Static gate: the packaging manifests must agree with the extension itself.

Role in the system. META.json (PGXN) and .pgx-build.yml (PGX packaging) are
the only two files in the tree that describe this extension to the OUTSIDE
world, and NOTHING in the build or test path reads either of them -- `git grep
pgx-build` has no in-repo hits, and PGXS never opens META.json. That is exactly
why both had rotted: META.json still named `pg_bm25_index` 0.1.0 and pointed at
a script that has not existed since the rebrand, and .pgx-build.yml advertised
`pgversions: "16+"` against a HARD PG17 `#error` floor. A wrong manifest costs
nothing until the moment someone publishes, and then it is simply wrong (M3
#60.1, ADR 0052).

This script closes that loop: it derives the truth from the files the build
ACTUALLY uses -- Makefile, bm25_native.control, src/bm25.h -- and fails if a
manifest disagrees. Adding it as a gate is the point; a manifest nothing reads
cannot be kept honest by review alone.

Deliberately dependency-free (stdlib only, no PyYAML): it has to run in CI
before any build step and on a developer laptop with nothing installed. The
.pgx-build.yml reader is a flat `key: value` scanner, not a YAML parser, which
is sufficient for that file's shape and is asserted below -- a nested rewrite
of the file would fail the key lookups loudly rather than parse wrongly.

Usage: python3 test/check_packaging_identity.py [repo_root]
Exit 0 = consistent, 1 = at least one disagreement (each printed).
"""

import hashlib
import json
import os
import re
import sys

failures = []
notes = []

# Install scripts that have shipped in a public release, pinned by content hash.
# PostgreSQL never re-runs an install script on a database that already ran
# CREATE EXTENSION, so editing a released script would give fresh installs a
# different catalog from upgraded ones with nothing to reconcile them. Once a
# version ships, its script is frozen and SQL changes go into a new
# bm25_native--X--Y.sql update script (ADR 0009, addendum of 2026-10-06).
# Every script a release ships -- install or update (--X--Y) -- gets an entry
# here at release time, since a shipped update script is just as immutable;
# never edit an existing entry.
RELEASED_SCRIPTS = {
    "bm25_native--1.0.sql":
        "a99df54d3406a9b9e856f12619d94adbcbb04701e10fac61982c3bc5a14d65bf",
}


def check(label, got, want):
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {label}: {got!r}"
          + ("" if ok else f" (expected {want!r})"))
    if not ok:
        failures.append(f"{label}: got {got!r}, expected {want!r}")
    return ok


def read(root, name):
    with open(os.path.join(root, name), encoding="utf-8") as f:
        return f.read()


def flat_yaml(text):
    """Top-level `key: value` pairs only; quotes stripped. See module docstring."""
    out = {}
    for line in text.splitlines():
        if not line or line[0].isspace() or line.lstrip().startswith("#"):
            continue
        m = re.match(r"^([A-Za-z0-9_]+):\s*(.*?)\s*$", line)
        if m:
            out[m.group(1)] = m.group(2).strip().strip('"').strip("'")
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..")

    # ---- authoritative sources: what the build actually uses -------------
    makefile = read(root, "Makefile")
    ext = re.search(r"^EXTENSION\s*=\s*(\S+)", makefile, re.M).group(1)
    data = re.search(r"^DATA\s*=\s*(\S+)", makefile, re.M).group(1)

    control = read(root, f"{ext}.control")
    default_version = re.search(
        r"^default_version\s*=\s*'([^']+)'", control, re.M).group(1)

    # The PG floor is the #error guard, not a comment or the CI matrix: that
    # guard is what a too-old build actually hits.
    #
    # Anchored to the "#error" that FOLLOWS the guard, and required to be
    # UNIQUE. A bare "first #if PG_VERSION_NUM <" match would silently derive
    # the wrong floor the moment a second or earlier version guard appears in
    # the header -- deriving a wrong answer confidently is worse than not
    # deriving one, since the whole point of this script is to be the thing
    # that notices.
    header = read(root, "src/bm25.h")
    guards = re.findall(
        r"#if\s+PG_VERSION_NUM\s*<\s*(\d+)\s*\n\s*#error", header)
    if len(guards) != 1:
        print(f"  FAIL src/bm25.h: expected exactly one '#if PG_VERSION_NUM < N'"
              f" guard followed by #error, found {len(guards)}: {guards}")
        failures.append(f"src/bm25.h version-floor guard is not unique: {guards}")
        return 1
    floor_major = int(guards[0]) // 10000

    # D26 / #311 CI-16: the extension is deliberately not relocatable (see the
    # control file's comment); pin it so a flip back to true is a visible edit.
    relocatable = re.search(r"^relocatable\s*=\s*(\S+)", control, re.M)
    check("control relocatable",
          relocatable.group(1) if relocatable else None, "false")
    # #311 CI-09: PGXS has no header deps on a non-autodepend PG, so the Makefile
    # must carry its own object->header rule or a struct edit links stale objects.
    check("Makefile has an $(OBJS) -> src/*.h dependency rule",
          bool(re.search(r"^\$\(OBJS\)[^:\n]*:.*src/\*\.h", makefile, re.M)), True)

    print(f"authoritative: extension={ext} script={data} "
          f"default_version={default_version} pg_floor={floor_major}")

    # PGXN requires a three-part semver; the control file carries the
    # extension's two-part version. Pad rather than compare loosely, so a
    # 1.0 -> 1.1 bump that forgets META.json is caught.
    parts = default_version.split(".")
    while len(parts) < 3:
        parts.append("0")
    semver = ".".join(parts)

    # ---- META.json (PGXN distribution metadata) --------------------------
    print("META.json:")
    meta = json.loads(read(root, "META.json"))
    check("name", meta.get("name"), ext)
    check("version", meta.get("version"), semver)
    provides = meta.get("provides", {})
    check("provides keys", sorted(provides), [ext])
    entry = provides.get(ext, {})
    check("provides.file", entry.get("file"), data)
    check("provides.version", entry.get("version"), semver)
    # The original defect was a provides->file naming a script not in the
    # tarball, so existence is checked, not just the string.
    script_exists = os.path.isfile(os.path.join(root, entry.get("file") or ""))
    check("provides.file exists on disk", script_exists, True)

    # A PGXN consumer reads only META.json, so the PostgreSQL floor has to be
    # stated there and equal the #error guard's (#311 CI-16). `.get` chains,
    # not indexing, so a missing section is a printed FAIL, not a traceback.
    requires = (meta.get("prereqs", {}).get("runtime", {}).get("requires", {}))
    check("prereqs.runtime.requires.PostgreSQL",
          requires.get("PostgreSQL"), f"{floor_major}.0.0")
    # PGXN's meta-spec 1.0.0 vocabulary. Membership, not a pinned value: the
    # status is a per-release judgment (1.0.0 is "stable"), not
    # something derivable from the build files.
    release_status = meta.get("release_status")
    check("release_status is a PGXN status",
          release_status in ("stable", "testing", "unstable"), True)
    repo = meta.get("resources", {}).get("repository", {})
    check("resources.repository.type", repo.get("type"), "git")
    check("resources.repository.url is set",
          bool(repo.get("url")) and bool(repo.get("web")), True)

    # ---- .pgx-build.yml (PGX packaging) ----------------------------------
    print(".pgx-build.yml:")
    pgx = flat_yaml(read(root, ".pgx-build.yml"))
    check("name", pgx.get("name"), ext)
    check("version", pgx.get("version"), default_version)
    check("pgversions", pgx.get("pgversions"), f"{floor_major}+")

    # ---- no pre-rebrand identity left in the live packaging surface ------
    # Prose, ADR bodies and the plan archives under docs/ deliberately keep the
    # old name as a historical record (same discipline as the ADR bodies during
    # the rebrand), so this is scoped to the packaging files only.
    print("rebrand residue:")
    stale = "pg_" + "bm25_index"        # split so this line is not itself a hit
    for name in ("META.json", ".pgx-build.yml", f"{ext}.control",
                 "Makefile", "test/oracle/bm25_oracle.py"):
        check(f"{name} contains {stale}", stale in read(root, name), False)

    # ---- released install scripts are frozen ------------------------------
    # CRLF is folded to LF before hashing so a Windows checkout with autocrlf
    # does not fail a byte-identical script; any other edit, whitespace
    # included, is a change to shipped SQL and fails here.
    print("released scripts:")
    for name, want in RELEASED_SCRIPTS.items():
        path = os.path.join(root, name)
        if not os.path.isfile(path):
            check(f"{name} exists", False, True)
            continue
        with open(path, "rb") as f:
            got = hashlib.sha256(f.read().replace(b"\r\n", b"\n")).hexdigest()
        check(f"{name} sha256 (frozen at release)", got, want)

    print()
    if failures:
        print(f"packaging identity: {len(failures)} disagreement(s)")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("packaging identity: consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
