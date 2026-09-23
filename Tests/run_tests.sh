#!/usr/bin/env bash
# Compiles and runs the NoteCap core unit tests. The core DSP has no JUCE
# dependency, so this needs only Xcode's clang (no cmake, no brew).
#
#   Tests/run_tests.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../Source"
BUILD="$HERE/build"
mkdir -p "$BUILD"

# On GitHub Actions, turn compiler errors into ::error annotations (readable
# through the public API, unlike the job log).
annotate_errors () {
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
        grep -E "error:" "$1" | head -20 | sed 's/%/%25/g; s/^/::error title=Compile error::/'
    fi
}

compile_and_run () {
    local name="$1"
    echo "== building $name =="
    if ! clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
        -I "$SRC" -I "$HERE" "$HERE/$name.cpp" \
        -framework Accelerate -o "$BUILD/$name" 2> "$BUILD/$name.err"; then
        cat "$BUILD/$name.err" >&2; annotate_errors "$BUILD/$name.err"; exit 1
    fi
    cat "$BUILD/$name.err" >&2
    "$BUILD/$name"
}

compile_and_run test_core
compile_and_run test_engine

echo ""
echo "All test binaries passed."
