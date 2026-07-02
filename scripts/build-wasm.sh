#!/usr/bin/env bash
# wasm build + tests under node. Prereq: scripts/bootstrap.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

source "$ROOT/third_party/emsdk/emsdk_env.sh" >/dev/null 2>&1

emcmake cmake -S "$ROOT" -B "$ROOT/build/wasm" -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-Release}
cmake --build "$ROOT/build/wasm" -j"$(nproc)"
ctest --test-dir "$ROOT/build/wasm" --output-on-failure
