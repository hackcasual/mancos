#!/usr/bin/env bash
# Build and run the GLOP-on-wasm tracer demo under node.
# Links directly against the or-tools build tree (see solver-wasm/README.md for the
# or-tools wasm build recipe; no `cmake --install` needed).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OT="$ROOT/third_party/or-tools"
B="$OT/build_wasm"
OUT="$ROOT/solver-wasm/demo/out"

source "$ROOT/third_party/emsdk/emsdk_env.sh" >/dev/null 2>&1
mkdir -p "$OUT"

em++ -std=c++20 -O2 "$ROOT/solver-wasm/demo/yafc_lp_demo.cc" \
  -I"$OT" -I"$B" \
  -I"$B/_deps/absl-src" \
  -I"$B/_deps/protobuf-src/src" \
  -I"$B/_deps/protobuf-src/third_party/utf8_range" \
  "$B/lib/libglop.a" "$B/lib/libprotobuf.a" \
  "$B/lib/libutf8_validity.a" "$B/lib/libz.a" "$B/lib/libbz2_static.a" \
  "$B"/lib/libabsl_*.a \
  -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576 \
  -o "$OUT/yafc_lp_demo.js"

echo "== running under node =="
node "$OUT/yafc_lp_demo.js"
