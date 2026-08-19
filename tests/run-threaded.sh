#!/bin/sh
# Build and run the threaded lockdep test, plain and under the sanitizers.
#
# The plain run says the logic holds with real threads and per-thread contexts.
# The ThreadSanitizer run is the one that says the lock-free registry and the
# dependency set are actually safe to record into from several CPUs.
#
#   ./tests/run-threaded.sh [compiler]
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cxx=${1:-${CXX:-c++}}
out=${TMPDIR:-/tmp}/lockdep-threaded.$$

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
run_variant() {
    name=$1
    shift
    printf '=== %s ===\n' "$name"
    if ! "$cxx" -std=c++23 -Wall -Wextra -Werror \
        -I"$here/include" -I"$ustl" \
        -DCONFIG_LOCK_DEP=1 -DLOCK_DEP_ENABLED_FEATURE_LEVEL=2 \
        "$@" "$here/tests/threaded.cpp" -o "$out"; then
        printf 'COMPILE FAILED: %s\n' "$name"
        rc=1
        return
    fi
    if "$out"; then
        printf 'PASS: %s\n' "$name"
    else
        printf 'FAILED: %s\n' "$name"
        rc=1
    fi
    rm -f "$out"
}

run_variant plain -O2
run_variant thread-sanitizer -O1 -g -fsanitize=thread
run_variant address-undefined -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

exit $rc
