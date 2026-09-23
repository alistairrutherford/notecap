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

compile_and_run () {
    local name="$1"
    echo "== building $name =="
    clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
        -I "$SRC" -I "$HERE" "$HERE/$name.cpp" \
        -framework Accelerate -o "$BUILD/$name"
    "$BUILD/$name"
}

compile_and_run test_core
compile_and_run test_engine

echo ""
echo "All test binaries passed."
