#!/bin/sh
# Build and run the lockdep smoke test at every feature level.
#
# lockdep is header-only and has no GN test target yet, so this drives a host
# compiler directly.  It needs ustl's headers, which live in the ours checkout;
# point USTL_INCLUDE at them if this script cannot work them out.
#
#   ./tests/run-smoke.sh [compiler]
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cxx=${1:-${CXX:-c++}}
out=${TMPDIR:-/tmp}/lockdep-smoke.$$

ustl=${USTL_INCLUDE:-}
if [ -z "$ustl" ]; then
    for candidate in \
        "$here/../../../system/lib/ustl/include" \
        "$here/../../system/lib/ustl/include" \
        "$here/../ustl/include"
    do
        if [ -d "$candidate" ]; then
            ustl=$candidate
            break
        fi
    done
fi
if [ -z "$ustl" ] || [ ! -d "$ustl" ]; then
    echo "cannot find ustl headers; set USTL_INCLUDE" >&2
    exit 2
fi

rc=0
run_level() {
    name=$1
    shift
    printf '=== %s ===\n' "$name"
    if ! "$cxx" -std=c++23 -Wall -Wextra -Werror \
        -I"$here/include" -I"$ustl" \
        "$@" "$here/tests/smoke.cpp" -o "$out"; then
        printf 'COMPILE FAILED: %s\n' "$name"
        rc=1
        return
    fi
    if "$out"; then
        printf 'PASS: %s\n' "$name"
    else
        printf 'RUNTIME FAILED: %s\n' "$name"
        rc=1
    fi
    rm -f "$out"
}

# No macros at all: the build never asked for lockdep.
run_level disabled
run_level metadata-only -DCONFIG_LOCK_DEP=1 -DLOCK_DEP_ENABLED_FEATURE_LEVEL=1
run_level validation -DCONFIG_LOCK_DEP=1 -DLOCK_DEP_ENABLED_FEATURE_LEVEL=2

exit $rc
