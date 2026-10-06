#!/usr/bin/env python3
"""Static gate: every caller of the owned gate is exercised by sql/63.

Role in the system. bm25_index_open_owned (src/bm25_meta.c) is the one way a
WRITE entry point -- bm25_seal, bm25_merge, bm25_upgrade and every mutating
bm25_debug_* lever -- opens a user-supplied regclass: ownership, not in
recovery, AM identity, all before a page is touched. The regression suite
sql/63_debug_privileges is where that gate is proved per function: each caller
is aimed at a btree it owns (dbgp_pkey) and must refuse rather than write the
btree's block 0. That list drifted. Eight writers added after the suite was
written were never put in it (#309, REGR sql/63), so for them nothing showed the
gate was actually on their path.

This script closes the loop from the source side, which is the only side that
knows who calls the gate: a pg_proc query can enumerate functions but cannot
tell which ones open through the OWNED gate rather than the readable one or
neither. The suite has to tell them apart too. Both gates run the same AM
identity check, so a btree-aimed call refuses identically through either, and
a writer moved onto bm25_index_open_readable would pass that block unchanged.
What separates them is a non-owner WITHOUT table SELECT: the owned gate says
"must be owner of index", the readable gate "permission denied for table".
It

  1. finds every bm25_index_open_owned( call in src/*.c (comments and string
     literals blanked, so a call merely mentioned in a comment does not count);
  2. attributes each call to its enclosing function, which must be a SQL entry
     point (`name(PG_FUNCTION_ARGS)`) -- a call from a static helper would make
     the attribution guesswork, so it fails instead and asks for a human;
  3. maps that C symbol to its SQL name(s) through the install script's
     CREATE FUNCTION ... AS 'MODULE_PATHNAME'[, 'symbol'] (the symbol defaults
     to the SQL name, as PostgreSQL's own loader does); and
  4. requires sql/63 (SQL comments stripped, so a commented-out line does not
     count) to call each SQL name with 'dbgp_pkey' as its first argument, the
     AM identity refusal; and
  5. requires expected/63 to show each SQL name called on 'dbgp_bm25' and
     answered on the very next line by the ownership error -- the line only
     the owned gate produces for that role; and
  6. runs the other direction too: every function sql/63's ownership block
     calls on 'dbgp_bm25' must still be an owned-gate caller in src/. Without
     this, moving a writer onto the readable gate would just drop it from the
     list derived in step 1 and pass here.

Adding a new owned writer without adding it to sql/63, or moving a listed one
off the owned gate, therefore fails here, before any build. Step 5 reads the
expected file rather than the .sql so that regenerating the expected output
after a gate swap does not hide it: the swapped function's next line becomes
"permission denied for table dbgp". Stdlib only, like the other test/check_*.py gates.

Usage: python3 test/check_owned_gate_coverage.py [repo_root]
Exit 0 = every caller covered, 1 = at least one gap (each printed).
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_source_style import code_only, line_of  # noqa: E402

GATE = "bm25_index_open_owned"
SUITE = os.path.join("sql", "63_debug_privileges.sql")
EXPECTED = os.path.join("expected", "63_debug_privileges.out")
OWNER_ERROR = "ERROR:  must be owner of index dbgp_bm25"
# The suite's ownership section runs from this banner to the next "-- ---" banner.
OWNERSHIP_BANNER = re.compile(r"^-- -+ ownership$", re.M)
INSTALL = "bm25_native--1.0.sql"

# A column-0 line that begins a function definition: optional return type, then
# the name and its opening parenthesis. Statements inside a body are indented, so
# column 0 is a reliable boundary in this tree (ARCHITECTURE.md house style).
DEF_HEAD = re.compile(r"^(?:[A-Za-z_][\w \t\*]*?[ \t\*])?([A-Za-z_]\w*)\s*\(([^)]*)\)?",
                      re.M)


def sql_names_by_symbol(root):
    """C symbol -> SQL function names, from the install script."""
    with open(os.path.join(root, INSTALL), encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"--[^\n]*", "", text)
    out = {}
    for m in re.finditer(r"CREATE\s+(?:OR\s+REPLACE\s+)?FUNCTION\s+(\w+)\s*\(",
                         text, re.I):
        stmt = text[m.end():text.find(";", m.end())]
        lib = re.search(r"AS\s+'MODULE_PATHNAME'(?:\s*,\s*'(\w+)')?", stmt, re.I)
        if lib:
            out.setdefault(lib.group(1) or m.group(1), set()).add(m.group(1))
    return out


def enclosing_function(code, pos):
    """(name, is_sql_entry) of the definition head nearest above pos."""
    best = None
    for m in DEF_HEAD.finditer(code, 0, pos):
        line_end = code.find("\n", m.start())
        line = code[m.start():line_end if line_end >= 0 else len(code)]
        if line.rstrip().endswith(";"):
            continue  # a prototype or a column-0 macro call (PG_FUNCTION_INFO_V1)
        best = (m.group(1), "PG_FUNCTION_ARGS" in line)
    return best


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..")
    failures = []

    symbols = {}          # C symbol -> first "file:line" of its gate call
    calls = 0
    srcdir = os.path.join(root, "src")
    for name in sorted(os.listdir(srcdir)):
        if not name.endswith(".c"):
            continue
        rel = os.path.join("src", name)
        with open(os.path.join(root, rel), "rb") as f:
            code = code_only(f.read().decode("utf-8", "replace"))
        for m in re.finditer(r"\b" + GATE + r"\s*\(", code):
            line_start = code.rfind("\n", 0, m.start()) + 1
            if m.start() == line_start:
                continue  # the definition itself (column 0), not a call
            calls += 1
            where = f"{rel}:{line_of(code, m.start())}"
            fn = enclosing_function(code, m.start())
            if fn is None or not fn[1]:
                failures.append(f"{where}: {GATE} called from "
                                f"{fn[0] if fn else 'no function'}, not a SQL entry "
                                "point -- this check cannot attribute it to a SQL "
                                "name; extend the script rather than skip it")
                continue
            symbols.setdefault(fn[0], where)

    with open(os.path.join(root, SUITE), encoding="utf-8") as f:
        raw_suite = f.read()
    suite = re.sub(r"--[^\n]*", "", raw_suite)
    banner = OWNERSHIP_BANNER.search(raw_suite)
    declared = set()
    if banner is None:
        failures.append(f"{SUITE}: no '-- --- ownership' section banner found")
    else:
        end = raw_suite.find("\n-- ---", banner.end())
        section = re.sub(r"--[^\n]*", "",
                         raw_suite[banner.end():end if end >= 0 else len(raw_suite)])
        declared = set(re.findall(r"SELECT\s+(\w+)\(\s*'dbgp_bm25'", section))
    with open(os.path.join(root, EXPECTED), encoding="utf-8") as f:
        expected = f.read().split("\n")
    by_symbol = sql_names_by_symbol(root)

    print(f"owned-gate callers ({GATE} in src/*.c) vs {SUITE}:")
    for sym in sorted(symbols):
        sqlnames = sorted(by_symbol.get(sym, ()))
        if not sqlnames:
            failures.append(f"{symbols[sym]}: {sym} calls {GATE} but no CREATE "
                            f"FUNCTION in {INSTALL} binds that symbol")
            continue
        for sqlname in sqlnames:
            ok = True
            if not re.search(r"\b" + sqlname + r"\(\s*'dbgp_pkey'", suite):
                ok = False
                failures.append(f"{symbols[sym]}: {sqlname} opens through {GATE} but "
                                f"{SUITE} never calls {sqlname}('dbgp_pkey'::regclass, "
                                "...) -- add it to the AM identity block")
            call = re.compile(r"SELECT " + sqlname + r"\('dbgp_bm25'")
            if not any(call.match(line) and i + 1 < len(expected)
                       and expected[i + 1] == OWNER_ERROR
                       for i, line in enumerate(expected)):
                ok = False
                failures.append(f"{symbols[sym]}: {EXPECTED} has no non-owner call "
                                f"{sqlname}('dbgp_bm25'::regclass, ...) answered by "
                                f"\"{OWNER_ERROR}\" -- add it to the ownership block "
                                "(or the function no longer opens through the owned gate)")
            if ok:
                print(f"  ok   {sqlname}")

    owned_sqlnames = {n for sym in symbols for n in by_symbol.get(sym, ())}
    for name in sorted(declared - owned_sqlnames):
        failures.append(f"{SUITE}: the ownership block calls {name} as an owned writer, "
                        f"but no SQL entry point named {name} calls {GATE} in src/ -- it "
                        "moved to another gate or lost its gate")

    # Non-vacuity: a lexer or regex bug that found nothing would pass silently.
    if calls == 0 or not symbols:
        failures.append(f"no {GATE}( call attributed anywhere -- the scan is broken")
    print(f"calls examined: {calls}, SQL entry points: {len(symbols)}, "
          f"declared in the ownership block: {len(declared)}")
    for f in failures:
        print(f"  FAIL {f}")
    if failures:
        print(f"owned-gate coverage: {len(failures)} gap(s)")
        return 1
    print("owned-gate coverage: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
