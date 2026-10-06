#!/usr/bin/env bash
# Report-only: say how much the coverage job actually measured (#309 CI-03).
#
# Role in the system. The coverage job is report-only (ADR 0098) and every step in
# it tolerates failure, so a runner or toolchain change that leaves the counters
# somewhere lcov does not look would produce a green job and an empty report. The
# asan job guards the same class with an __asan_ symbol canary; this is the coverage
# equivalent. It compares the .gcda files (written by the instrumented backends at
# exit) with the .gcno files (written by the compiler, one per object, every one of
# which defines at least one function and so gets counters), warns when they differ
# or when there are none, and puts lcov's line-coverage figure on the run's summary
# page so it can be read without downloading the artifact. It never fails the step.
#
# Usage: coverage_summary.sh SRC_DIR TRACEFILE
set -uo pipefail

src=${1:?usage: coverage_summary.sh SRC_DIR TRACEFILE}
trace=${2:?usage: coverage_summary.sh SRC_DIR TRACEFILE}
summary_file=${GITHUB_STEP_SUMMARY:-/dev/null}

warn() {
    echo "::warning::$*"
    echo "- warning: $*" >> "$summary_file"
}

gcno=$(find "$src" -name '*.gcno' | wc -l | tr -d ' ')
gcda=$(find "$src" -name '*.gcda' | wc -l | tr -d ' ')
echo "### Coverage (report-only)" >> "$summary_file"
echo "objects compiled with counters: $gcno; objects with counters written: $gcda"
if [ "$gcno" -eq 0 ]; then
    warn "no .gcno files under $src: the extension was not built with --coverage, so there is nothing to report"
elif [ "$gcda" -lt "$gcno" ]; then
    warn "only $gcda of $gcno objects wrote .gcda counters under $src: the run did not reach them, or the counters went elsewhere"
fi

line=
if [ -s "$trace" ]; then
    line=$(lcov --summary "$trace" 2>&1 | grep -E '^[[:space:]]*lines\.*:' | head -1 | sed 's/^[[:space:]]*//')
fi
if [ -z "$line" ]; then
    warn "no line-coverage figure: $trace is missing or empty, or lcov --summary printed none"
else
    echo "$line"
    echo "- $line" >> "$summary_file"
fi
echo "- objects with counters: $gcda of $gcno" >> "$summary_file"
exit 0
