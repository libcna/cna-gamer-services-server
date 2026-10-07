#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# One entry point for the checks a human maintainer runs most often. Every build uses ccache and no
# command in this script intentionally exceeds six parallel workers.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tier=${1:-quick}

if ! command -v ccache >/dev/null 2>&1; then
    echo "ccache is required but was not found" >&2
    exit 2
fi

# Managed or read-only homes sometimes make the configured cache unwritable. Keep compilation
# cached without changing the user's global ccache configuration.
if [[ -z ${CCACHE_DIR:-} ]]; then
    configured_cache=$(ccache --show-config | sed -n 's/.*cache_dir = //p' | head -n 1)
    cache_probe=
    if [[ -n $configured_cache ]]; then
        cache_probe=$(mktemp "$configured_cache/.cna-write-probe.XXXXXX" 2>/dev/null || true)
    fi
    if [[ -z $cache_probe ]]; then
        export CCACHE_DIR=${TMPDIR:-/tmp}/cna-gamer-services-ccache
        mkdir -p "$CCACHE_DIR"
    else
        rm -f "$cache_probe"
    fi
fi

configure_and_build() {
    local preset=$1
    cmake --preset "$preset"
    cmake --build --preset "$preset"
}

run_tests() {
    local build=$1
    shift
    CTEST_PARALLEL_LEVEL=6 ctest --test-dir "$root/$build" --output-on-failure "$@"
}

run_tests_serial() {
    local build=$1
    shift
    CTEST_PARALLEL_LEVEL=1 ctest --test-dir "$root/$build" --output-on-failure "$@"
}

case "$tier" in
    quick)
        configure_and_build dev
        run_tests build-agent -L unit
        ;;
    normal)
        configure_and_build dev
        run_tests build-agent
        ;;
    security)
        configure_and_build asan
        # Instrumented translation units and test processes have a much larger resident set.
        # Keep both phases serial so this tier remains safe on memory-constrained builders.
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 run_tests_serial build-asan -L unit
        ;;
    performance)
        configure_and_build release
        python3 "$root/tests/service_benchmark.py" "$root/build-release" --smoke
        ;;
    full)
        configure_and_build dev
        run_tests build-agent
        python3 "$root/tools/cna-gamer-services-docs/check.py" "$root" --strict
        ;;
    *)
        echo "usage: tools/qualify.sh {quick|normal|full|security|performance}" >&2
        exit 2
        ;;
esac

echo
echo "ccache statistics for this qualification run:"
ccache --show-stats
