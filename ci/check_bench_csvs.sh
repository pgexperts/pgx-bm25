#!/usr/bin/env bash
# Report-only: warn when a benchmark step produced no data (#309 CI-15).
#
# Role in the system. Each bench step is continue-on-error with a plain `>`
# redirect (see the bench job in .github/workflows/ci.yml), so a script that fails
# leaves a green job and a CSV holding at most its comment and header lines. This
# prints each CSV's row count and raises a ::warning:: (never an error: the bench
# job gates nothing, ADR 0098) for a CSV that is missing or has no data row. It
# says nothing about the numbers themselves; shared runners are too noisy for that.
#
# Usage: check_bench_csvs.sh DIR NAME.csv [NAME.csv ...]
set -uo pipefail

dir=${1:?usage: check_bench_csvs.sh DIR NAME.csv [NAME.csv ...]}
shift
empty=0
for name in "$@"; do
    f="$dir/$name"
    if [ ! -f "$f" ]; then
        echo "::warning::bench output $f is missing -- its step did not run or did not write"
        empty=$((empty + 1))
        continue
    fi
    # Data rows: non-blank lines that are not '#' comments, less the column header.
    rows=$(grep -cv -e '^#' -e '^[[:space:]]*$' "$f" || true)
    rows=$((rows > 0 ? rows - 1 : 0))
    echo "$f: $(wc -l < "$f" | tr -d ' ') lines, $rows data rows"
    if [ "$rows" -eq 0 ]; then
        echo "::warning::bench output $f has no data rows -- its script failed; see that step's log"
        empty=$((empty + 1))
    fi
done
echo "$empty of $# benchmark outputs empty or missing (report-only)"
exit 0
