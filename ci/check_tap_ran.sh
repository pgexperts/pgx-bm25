#!/usr/bin/env bash
# Fail unless an installcheck log shows prove ran every t/*.pl suite (#309 CI-07).
#
# Role in the system. PGXS runs the TAP tier only when the server was configured
# with --enable-tap-tests. Otherwise Makefile.global's prove_installcheck is an
# `echo "TAP tests not enabled..."` and `make installcheck` still exits 0, so a
# build-and-test leg on a PGDG package that stopped shipping TAP support (or a
# pre-GA component that never did) would go green having skipped every crash and
# replica suite. The hardening and asan legs build PostgreSQL themselves with
# --enable-tap-tests; the PGDG legs have only this check.
#
# The evidence is prove's own summary line, "Files=N, Tests=M, ...", which it
# prints once per run whatever the outcome. N must equal the number of t/*.pl
# files: fewer means a suite was skipped (PROVE_TESTS shadowing auto-discovery,
# Makefile CI-08), and no line at all means prove never ran.
#
# Usage: check_tap_ran.sh INSTALLCHECK_LOG [TAP_DIR]   (TAP_DIR defaults to t)
set -euo pipefail

log=${1:?usage: check_tap_ran.sh INSTALLCHECK_LOG [TAP_DIR]}
dir=${2:-t}

want=$(find "$dir" -maxdepth 1 -name '*.pl' | wc -l | tr -d ' ')
if [ "$want" -eq 0 ]; then
    echo "::error::no $dir/*.pl files found -- this check is pointed at the wrong directory"
    exit 1
fi
line=$(grep -E '^Files=[0-9]+, Tests=[0-9]+' "$log" | tail -1 || true)
if [ -z "$line" ]; then
    echo "::error::the TAP tier did not run: no prove summary (Files=N, Tests=M) in $log." \
         "Is the server built with --enable-tap-tests? $(grep -m1 'TAP tests not enabled' "$log" || true)"
    exit 1
fi
got=${line#Files=}
got=${got%%,*}
echo "prove: $line"
if [ "$got" -ne "$want" ]; then
    echo "::error::prove ran $got TAP files but $dir/ holds $want -- a suite was skipped"
    exit 1
fi
echo "TAP tier ran all $want suites"
