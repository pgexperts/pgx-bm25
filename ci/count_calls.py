#!/usr/bin/env python3
"""Count call sites in C source with comments and string literals blanked.

Role in the system. The CI source floors in .github/workflows/ci.yml (the
interrupt-check floor and the tokenizer scratch-context floor) used to count
raw `grep` matches, and ci/check_scan_scratch.py matched raw lines. A raw match
cannot tell code from a comment that writes the same text, so a floor set to the
exact count let one comment mention pay for one deleted call (#309 CI-14; ADR
0093 records the #67.13 instance, a comment that wrote the delay macro in its
call form). This helper is the one lexer those checks share: it reuses
code_only() from test/check_source_style.py, which already blanks comments and
literal contents while keeping every offset and newline, so there is a single
definition of "this text is code" in the tree.

A name only counts at an identifier boundary, so a longer identifier that ends
in the same text (MY_CHECK_FOR_INTERRUPTS()) is not a call to the shorter one.

Usage:
    count_calls.py PATTERN [PATTERN ...] -- FILE [FILE ...]
prints one integer: the total number of matches of any PATTERN (literal text,
for example 'CHECK_FOR_INTERRUPTS()') over the code of every FILE. A missing or
unreadable FILE, or an empty FILE list, is an error (exit 2), never a zero: a
floor fed a glob that matched nothing must fail, not pass with a count of 0.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test"))
from check_source_style import code_only  # noqa: E402  (path set just above)


def code_of(path):
    """The comment- and literal-blanked text of one C file."""
    with open(path, "rb") as f:
        # Decoded permissively, as check_source_style.py does: an undecodable byte
        # is test/check_source_ascii.py's finding, not a reason to miscount.
        return code_only(f.read().decode("utf-8", "replace"))


def count(patterns, paths):
    rx = re.compile("|".join(r"(?<![A-Za-z0-9_])" + re.escape(p) for p in patterns))
    return sum(len(rx.findall(code_of(p))) for p in paths)


def main(argv):
    if "--" not in argv:
        sys.stderr.write(__doc__)
        return 2
    sep = argv.index("--")
    patterns, paths = argv[:sep], argv[sep + 1:]
    if not patterns or not paths:
        sys.stderr.write("count_calls.py: need at least one PATTERN and one FILE\n")
        return 2
    try:
        print(count(patterns, paths))
    except OSError as e:
        sys.stderr.write("count_calls.py: %s\n" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
