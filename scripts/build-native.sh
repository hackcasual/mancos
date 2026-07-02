#!/usr/bin/env bash
# Native (desktop) build + tests. Prereq: scripts/bootstrap.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cmake -S "$ROOT" -B "$ROOT/build/native" -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-Release}
cmake --build "$ROOT/build/native" -j"$(nproc)"
ctest --test-dir "$ROOT/build/native" --output-on-failure
