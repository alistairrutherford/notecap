#!/usr/bin/env bash
# Builds the offline chord evaluator (no JUCE needed).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$HERE/build"
clang++ -std=c++17 -O2 -I "$HERE/../../Source" "$HERE/notecap_eval.cpp" -framework Accelerate -o "$HERE/build/notecap_eval"
echo "built $HERE/build/notecap_eval"
