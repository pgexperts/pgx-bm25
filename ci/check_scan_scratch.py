"""H17 / ADR 0036 ordering floor: the hot scan paths must create their scratch
memory context BEFORE anything takes the snapshot, because bm25_scan_snapshot
palloc's the whole live catalog copy (snap.segs) into CurrentMemoryContext. Create
the context afterwards and that copy lands in the caller's context instead, where
it lives to end of transaction -- the exact leak H17 fixed.

pg_regress cannot see this: it is an allocation-lifetime property, not an output
one. Hence a source-ordering check.

ADR 0045 note -- why this script knows about two call forms. The snapshot used to
be taken directly in each of the three target functions. The scan-prologue dedup
moved it inside bm25_scan_corpus_stats, which the two scorer paths now call
instead. The PROPERTY is unchanged (both still create their scratch context first);
only the call that carries the snapshot moved. So the check looks for whichever
snapshot-taking call comes first, and separately asserts that
bm25_scan_corpus_stats really does still take the snapshot -- otherwise this file
could silently pass by matching a helper that no longer allocates anything.

#309 CI-14: the scan reads each file through ci/count_calls.py's code_of (comments
and string literals blanked, offsets and newlines kept), so a comment that names
AllocSetContextCreate, or one with a column-0 identifier followed by "(", can no
longer stand in for code or start a phantom function.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from count_calls import code_of  # noqa: E402  (path set just above)

TARGETS = {'bm25_scan_build_ranking_exhaustive',
           'bm25_scan_build_ranking_once',
           'bm25_load_if_needed'}

# Either form allocates snap.segs into CurrentMemoryContext: the direct call, or
# the shared prologue helper that wraps it.
SNAPSHOT_CALLS = ('bm25_scan_snapshot(', 'bm25_scan_corpus_stats(')

# #228 (ADR 0101) split the scanner: the two ranking builders and
# bm25_scan_corpus_stats are in bm25_scan_rank.c, and bm25_load_if_needed stayed in
# bm25_scan.c. A target found in neither file records no create/snapshot line and
# fails below, so a function that moves again cannot silently drop out of the check.
SRCS = ('src/bm25_scan.c', 'src/bm25_scan_rank.c')
first = {}
helper_takes_snapshot = False

for src in SRCS:
    cur, helper_body = None, False
    for i, line in enumerate(code_of(src).split('\n')):
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\(', line)
        if m:
            cur = m.group(1)
            # Track the helper's own body so we can prove it still snapshots.
            helper_body = (cur == 'bm25_scan_corpus_stats')
            if cur in TARGETS:
                first.setdefault(cur, {'file': src})
        if helper_body and 'bm25_scan_snapshot(' in line:
            helper_takes_snapshot = True
        if cur in TARGETS:
            d = first[cur]
            if 'create' not in d and 'AllocSetContextCreate' in line:
                d['create'] = i + 1
            if 'snap' not in d and any(c in line for c in SNAPSHOT_CALLS):
                d['snap'] = i + 1

bad = []
for fn in sorted(TARGETS):
    d = first.get(fn, {})
    print("%-38s %-22s create=%s snapshot=%s"
          % (fn, d.get('file', '(not found)'), d.get('create'), d.get('snap')))
    if 'create' not in d or 'snap' not in d or d['create'] > d['snap']:
        bad.append(fn)

if bad:
    print("FAIL: scratch context not created before the snapshot in: %s"
          % ', '.join(bad))
    sys.exit(1)

# Without this, removing the snapshot from bm25_scan_corpus_stats would leave the
# two scorer paths matching a call that allocates nothing, and this check would
# report OK while guarding nothing.
if not helper_takes_snapshot:
    print("FAIL: bm25_scan_corpus_stats no longer calls bm25_scan_snapshot -- "
          "this check's premise is broken; re-point it at whatever takes the "
          "snapshot now (ADR 0036, ADR 0045)")
    sys.exit(1)

print("OK: all three hot scan paths snapshot inside a scratch context")
