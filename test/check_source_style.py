#!/usr/bin/env python3
"""Static gate: the C sources follow the enforceable half of the house style.

Role in the system. ARCHITECTURE.md ("C source house style") documents how
src/*.c and src/*.h are written, and ADR 0092 records why the tree keeps its
own style rather than running pgindent. A written rule that nothing checks
decays at the rate of the least careful commit, so every rule in that section
that a script can decide without judgement is decided here instead of in
review. The rest (comment wrap near 88 columns, one statement per line,
declarations at the top of a block) stay review rules on purpose: they need a
C parser or a judgement call, and a gate that guesses is worse than none.

Each rule, and why it exists:

  no tab characters   The tree is 4-space indented. One file used tabs until
                      2026-08 and rendered at a different width from its
                      siblings in any editor not set to tab-4; this keeps it
                      from coming back.
  no carriage returns A CRLF file diffs as a whole-file rewrite and hides the
                      real change. Nothing in the build needs one.
  no line over 120    The hard ceiling, measured with tabs expanded to 4 (tabs
                      are banned anyway; the expansion just keeps the reported
                      column honest). Comment text wraps near 88 by habit; 120
                      is where a line stops being reviewable in a split diff.
                      The 79-column pgindent width is deliberately NOT the
                      rule -- see ADR 0092.
  no trailing space   Invisible in review, noisy in every later diff of the
                      line.
  postgres.h first    PostgreSQL's own convention: every .c includes
                      "postgres.h" as its literal first #include, and no
                      header includes it. The headers' version-floor #error
                      (bm25.h) reads PG_VERSION_NUM, which only postgres.h
                      defines, so a .c that forgot it would fail with a
                      misleading "requires PostgreSQL 17" message instead of
                      the real cause.
  ereport(ERROR, ...) carries errcode()
                      Without one PostgreSQL reports SQLSTATE XX000
                      internal_error, so a client, pooler or PL/pgSQL handler
                      that classifies by SQLSTATE sees a user-input or
                      corruption error as a backend bug (#66.10, #66.14 were
                      exactly that). elog() is exempt: it is the internal-error
                      path by definition.
  hash_create() names HASH_CONTEXT
                      Without the flag dynahash builds the table in whatever
                      MemoryContext happens to be current at the call. On the
                      insert path that was CurTransactionContext, so a table
                      meant to live for one row survived for the life of the
                      backend (#146). The flag and ctl.hcxt together are what
                      make the table die with the scratch it belongs to.
  Assert() is classified
                      Every Assert carries a trailing /* invariant */ or
                      /* checked: <where> */. The two say different things --
                      "this code's own construction guarantees it" versus
                      "something else already validated it and this is a
                      cassert-only tripwire" -- and the difference decides
                      whether an Assert is the right mechanism at all. An
                      Assert compiles out of every production build, so one
                      guarding attacker-influenced on-disk data is the WRONG
                      mechanism (ADR 0071: that is an ereport on
                      ERRCODE_INDEX_CORRUPTED). Writing the classification down
                      at the call site is what keeps that judgement from being
                      re-litigated, or silently lost, on the next edit (#157.6).

The errcode() rule is checked on the ereport call's OWN balanced parentheses
after comments and string/character literals are blanked out. A substring test
is not acceptable: an earlier attempt passed a call whose only "errcode" was in
its message text, and a test that is fooled by the text it is checking reports
"clean" on exactly the case it exists to catch. Character literals get their
own lexer state because '"' otherwise desynchronises string tracking (the
snippet escaper has one; see ADR 0054's addendum).

A level that is not a literal non-error level (a variable, a macro, anything
unrecognised) is treated as possibly ERROR and must carry errcode() too: the
gate cannot see its runtime value, and guessing "not an error" is the unsafe
direction.

The hash_create() and Assert() rules use the SAME balanced-paren scan
(call_args) for the same reason, and in the hash_create case the reason is not
theoretical: every one of the tree's call sites wraps, and HASH_CONTEXT sits on
a CONTINUATION line at all of them. A line-oriented grep -- the form issue #157
proposed -- therefore reports all fourteen clean call sites as violations, and
an implementation that was tuned until that noise went away would have been
tuned into uselessness. The flags are read from the call's FOURTH top-level
argument, not from the whole call text, so a table named "... HASH_CONTEXT ..."
cannot satisfy the rule (the same failure the errcode rule already learned).

The Assert tag is required on the line the call's CLOSING parenthesis lands on,
read back from the ORIGINAL text at offsets code_only preserved -- the tag is a
comment, so it does not exist in the lexed copy the call was found in.

The file set is globbed, not listed, so a new file is covered the moment it
exists -- the same reasoning as test/check_source_ascii.py, whose structure
and messages this mirrors.

Deliberately dependency-free (stdlib only): it runs in CI before any build step.

Usage: python3 test/check_source_style.py [repo_root]
Exit 0 = clean, 1 = at least one violation (each offender printed).
"""

import os
import re
import sys

MAX_COLUMNS = 120
TAB_WIDTH = 4

# Levels that can never raise an error; anything else must carry errcode().
NON_ERROR_LEVELS = {
    "DEBUG5", "DEBUG4", "DEBUG3", "DEBUG2", "DEBUG1",
    "LOG", "LOG_SERVER_ONLY", "COMMERROR", "INFO", "NOTICE", "WARNING",
    "WARNING_CLIENT_ONLY",
}


def c_sources(root):
    """Every src/*.c and src/*.h, globbed rather than listed (see module doc)."""
    out = []
    d = os.path.join(root, "src")
    for name in sorted(os.listdir(d)):
        if name.endswith((".c", ".h")):
            out.append(os.path.join("src", name))
    return out


def code_only(text):
    """Return text with comments blanked and string/char literal CONTENTS blanked.

    Same length as the input and newlines are preserved, so every offset and
    line number computed on the result is valid for the original. Quote
    characters themselves are kept so the literal's extent stays visible.
    """
    out = []
    i = 0
    n = len(text)
    mode = "code"
    while i < n:
        ch = text[i]
        if mode == "code":
            if text.startswith("/*", i):
                mode = "block"
                out.append("  ")
                i += 2
            elif text.startswith("//", i):
                mode = "line"
                out.append("  ")
                i += 2
            elif ch == '"':
                mode = "str"
                out.append(ch)
                i += 1
            elif ch == "'":
                mode = "chr"
                out.append(ch)
                i += 1
            else:
                out.append(ch)
                i += 1
        elif mode == "block":
            if text.startswith("*/", i):
                mode = "code"
                out.append("  ")
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
        elif mode == "line":
            if ch == "\\" and text.startswith("\n", i + 1):
                # Line splicing happens before comments are recognised, so a
                # // comment ending in a backslash continues onto the next line.
                out.append(" \n")
                i += 2
            elif ch == "\n":
                mode = "code"
                out.append("\n")
                i += 1
            else:
                out.append(" ")
                i += 1
        else:
            quote = '"' if mode == "str" else "'"
            if ch == "\\" and i + 1 < n:
                # An escape consumes the next character, whatever it is -- this
                # is what keeps '\'' and "\"" from ending the literal early.
                # A backslash-newline continuation keeps its newline.
                out.append("_")
                out.append("\n" if text[i + 1] == "\n" else "_")
                i += 2
            elif ch == quote:
                mode = "code"
                out.append(ch)
                i += 1
            elif ch == "\n":
                # An unterminated literal cannot span a raw newline in valid C;
                # resynchronise rather than blank the rest of the file.
                mode = "code"
                out.append("\n")
                i += 1
            else:
                out.append("_")
                i += 1
    return "".join(out)


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def call_args(code, open_pos):
    """Split the call whose '(' is at open_pos into its top-level argument spans.

    Returns (close_pos, [(start, end), ...]); close_pos is -1 and the list empty
    when the parentheses never balance. Nested calls, casts and parenthesised
    expressions are skipped by depth, so a comma inside one does not split an
    argument -- and because `code` is the comment- and literal-blanked copy, a
    comma or parenthesis inside a string or comment cannot split one either.

    Every rule below that has to look INSIDE a call uses this. Three of them do,
    and all three are calls whose argument list routinely wraps across lines, so
    a per-line regex would be reading fragments rather than arguments.
    """
    depth = 0
    args = []
    start = open_pos + 1
    for k in range(open_pos, len(code)):
        c = code[k]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append((start, k))
                return k, args
        elif c == "," and depth == 1:
            args.append((start, k))
            start = k + 1
    return -1, []


def whitespace_violations(rel, text):
    """Tabs, CRs, width and trailing whitespace, on every physical line.

    Split on "\\n" ONLY, never str.splitlines(): splitlines() also breaks on
    \\r, \\v, \\f and U+2028 and would CONSUME a carriage return -- the exact
    byte the CR rule exists to report.
    """
    out = []
    for lineno, line in enumerate(text.split("\n"), 1):
        if "\t" in line:
            out.append(f"{rel}:{lineno}:{line.index(chr(9)) + 1}: tab character "
                       "(indent with 4 spaces)")
        if "\r" in line:
            out.append(f"{rel}:{lineno}:{line.index(chr(13)) + 1}: carriage return "
                       "(use LF line endings)")
        width = len(line.replace("\r", "").expandtabs(TAB_WIDTH))
        if width > MAX_COLUMNS:
            out.append(f"{rel}:{lineno}: {width} columns (limit {MAX_COLUMNS}; split the "
                       "string literal or move the trailing comment above the line)")
        body = line[:-1] if line.endswith("\r") else line
        if body != body.rstrip(" \t\f\v"):
            out.append(f"{rel}:{lineno}: trailing whitespace")
    return out


INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"][^>"\n]*[>"])', re.M)


def include_violations(rel, text, code):
    """postgres.h is the first #include of a .c and appears in no header.

    Directives are found in the comment-blanked text, so a commented-out
    include neither satisfies nor violates the rule. String contents are
    blanked there too, so each include's "..." operand is read back from the
    ORIGINAL text at the same offsets (code_only preserves length).
    """
    out = []
    operands = []
    for m in INCLUDE_RE.finditer(code):
        s, e = m.span(1)
        operands.append((line_of(code, m.start()), text[s:e]))
    if rel.endswith(".c"):
        if not operands:
            out.append(f"{rel}: no #include at all; \"postgres.h\" must be the first")
        elif operands[0][1] != '"postgres.h"':
            out.append(f"{rel}:{operands[0][0]}: first #include is {operands[0][1]}, "
                       "must be \"postgres.h\"")
    else:
        for ln, op in operands:
            if op in ('"postgres.h"', "<postgres.h>"):
                out.append(f"{rel}:{ln}: header includes {op}; headers must assume it "
                           "(the including .c file includes it first)")
    return out


EREPORT_RE = re.compile(r"\bereport\s*\(")
# errcode() and core's two errno-mapping helpers, errcode_for_file_access() and
# errcode_for_socket_access(), which set the SQLSTATE from errno and are the idiomatic
# way to report a failed file or socket call. An errcode wrapped in a project macro is
# deliberately NOT recognised: the gate would have to expand macros to see it, and
# writing errcode(...) at the call site keeps the SQLSTATE visible to a reviewer.
ERRCODE_RE = re.compile(r"\berrcode(?:_for_(?:file|socket)_access)?\s*\(")


def ereport_violations(rel, code, seen):
    """Every ereport() that can raise an error carries errcode() in its own parens.

    `seen` counts the calls examined. main() refuses to report clean on zero:
    a lexer bug that blanked every call would otherwise pass this rule vacuously.
    """
    out = []
    for m in EREPORT_RE.finditer(code):
        seen["ereport"] += 1
        open_pos = m.end() - 1
        close_pos, args = call_args(code, open_pos)
        ln = line_of(code, m.start())
        if close_pos < 0:
            out.append(f"{rel}:{ln}: ereport( with unbalanced parentheses -- "
                       "cannot check it, fix the call")
            continue
        level = code[args[0][0]:args[0][1]].strip()
        if level in NON_ERROR_LEVELS:
            continue
        span = code[open_pos:close_pos + 1]
        if not ERRCODE_RE.search(span):
            out.append(f"{rel}:{ln}: ereport({level}, ...) has no errcode() -- it would "
                       "report SQLSTATE XX000 internal_error")
    return out


HASH_CREATE_RE = re.compile(r"\bhash_create\s*\(")
HASH_CONTEXT_RE = re.compile(r"\bHASH_CONTEXT\b")
# hash_create(name, nelem, ctl, flags) -- the flags this rule reads are argument 4.
HASH_CREATE_NARGS = 4


def hash_create_violations(rel, code, seen):
    """Every hash_create() passes HASH_CONTEXT in its flags argument.

    Checked on the FOURTH top-level argument specifically. Searching the whole
    call would let a table NAME carrying the token satisfy the rule, and the
    errcode rule above already paid for that lesson once.

    The rule cannot see ctl.hcxt, which HASH_CONTEXT is useless without: that is
    an assignment to a caller-chosen local somewhere above the call, and every
    regex shape for it either misses a differently-named HASHCTL or accepts a
    stale assignment from an unrelated table. HASH_CONTEXT is the half a gate can
    decide, and it is the half that is easy to forget -- ctl.hcxt without the flag
    is silently ignored by dynahash, but so is neither, and both bugs are caught
    by requiring the flag and reviewing the field.
    """
    out = []
    for m in HASH_CREATE_RE.finditer(code):
        seen["hash_create"] += 1
        close_pos, args = call_args(code, m.end() - 1)
        ln = line_of(code, m.start())
        if close_pos < 0:
            out.append(f"{rel}:{ln}: hash_create( with unbalanced parentheses -- "
                       "cannot check it, fix the call")
            continue
        if len(args) != HASH_CREATE_NARGS:
            out.append(f"{rel}:{ln}: hash_create( takes {HASH_CREATE_NARGS} arguments, "
                       f"found {len(args)} -- cannot locate the flags, fix the call")
            continue
        flags = code[args[3][0]:args[3][1]]
        if not HASH_CONTEXT_RE.search(flags):
            out.append(f"{rel}:{ln}: hash_create() flags omit HASH_CONTEXT -- the table "
                       "is built in whatever context is current (set ctl.hcxt and pass "
                       "the flag)")
    return out


ASSERT_RE = re.compile(r"\bAssert\s*\(")
# The trailing tag, on the closing parenthesis's own line: an optional statement
# semicolon, then a block comment opening with the bare word "invariant" or with
# "checked:". "checked:" deliberately requires text after it at review time, not
# here -- a gate that tried to judge whether the named `where` is real would be
# guessing, and the tag's value is that a human wrote down which of the two it is.
ASSERT_TAG_RE = re.compile(r"[ \t]*;?[ \t]*/\*[ \t]*(?:invariant\b|checked:)")


def assert_violations(rel, text, code, seen):
    """Every Assert() carries a trailing /* invariant */ or /* checked: ... */.

    Found in `code` (so an Assert written inside a comment or a string literal --
    this tree has nine, all of them prose ABOUT an assertion -- is not a call site
    and is not required to carry a tag), then the tag is read back from `text` at
    the same offsets, because code_only blanked the comment the tag lives in.

    StaticAssertDecl and lowercase assert() are out of scope: the first is a
    compile-time check that cannot be compiled out, and the second appears only in
    bm25_phrase.c's standalone harness, which does not ship.
    """
    out = []
    for m in ASSERT_RE.finditer(code):
        seen["Assert"] += 1
        close_pos, _ = call_args(code, m.end() - 1)
        ln = line_of(code, m.start())
        if close_pos < 0:
            out.append(f"{rel}:{ln}: Assert( with unbalanced parentheses -- "
                       "cannot check it, fix the call")
            continue
        eol = text.find("\n", close_pos + 1)
        rest = text[close_pos + 1:eol if eol >= 0 else len(text)]
        if not ASSERT_TAG_RE.match(rest):
            out.append(f"{rel}:{line_of(code, close_pos)}: Assert() has no "
                       "classification -- append /* invariant */ (this code's own "
                       "construction guarantees it) or /* checked: <where> */ "
                       "(validated elsewhere; this is a cassert-only tripwire)")
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..")

    rels = c_sources(root)
    print("C source style (src/*.c, src/*.h):")
    failures = 0
    # One counter per rule that looks inside a call. Reported below and required
    # to be non-zero: a lexer or balanced-paren bug that found NO call sites would
    # otherwise let all three rules pass vacuously, which is the one failure mode
    # a static gate cannot afford (see ADR 0059 on verifying a no-op change).
    seen = {"ereport": 0, "hash_create": 0, "Assert": 0}
    if not rels:
        print("  FAIL no files matched -- the derivation above is broken")
        failures += 1
    for rel in rels:
        with open(os.path.join(root, rel), "rb") as f:
            raw = f.read()
        # Decoded permissively: an undecodable byte is test/check_source_ascii.py's
        # finding to report, not a reason for this gate to crash and say nothing.
        text = raw.decode("utf-8", "replace")
        code = code_only(text)
        bad = (whitespace_violations(rel, text)
               + include_violations(rel, text, code)
               + ereport_violations(rel, code, seen)
               + hash_create_violations(rel, code, seen)
               + assert_violations(rel, text, code, seen))
        if not bad:
            print(f"  ok   {rel}")
            continue
        failures += len(bad)
        print(f"  FAIL {rel}: {len(bad)} violation(s)")
        for b in bad[:10]:
            print(f"         {b}")
        if len(bad) > 10:
            print(f"         ... and {len(bad) - 10} more")

    print()
    print("calls examined: " + ", ".join(f"{k} {seen[k]}" for k in sorted(seen)))
    for name in sorted(seen):
        if rels and seen[name] == 0:
            print(f"  FAIL no {name}( call found anywhere -- the lexer is broken, "
                  "fix it rather than trusting a pass")
            failures += 1
    if failures:
        print(f"source style: {failures} violation(s)")
        print("See ARCHITECTURE.md \"C source house style\" and ADR 0092. Indent with 4 "
              "spaces, LF endings, no line over 120 columns, no trailing whitespace; "
              "#include \"postgres.h\" first in every .c and in no header; every "
              "ereport that can raise an error names its errcode(); every hash_create "
              "passes HASH_CONTEXT; every Assert is tagged /* invariant */ or "
              "/* checked: <where> */.")
        return 1
    print("source style: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
