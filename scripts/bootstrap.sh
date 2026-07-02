#!/usr/bin/env bash
# One-time setup: fetches and builds everything under third_party/ that the
# CMake build imports. Idempotent — safe to re-run; finished steps are skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"
JOBS="$(nproc)"
mkdir -p "$TP"

# ---- emsdk (provides emcc + node) -------------------------------------------
if [ ! -d "$TP/emsdk" ]; then
  git clone --depth 1 https://github.com/emscripten-core/emsdk "$TP/emsdk"
  "$TP/emsdk/emsdk" install latest
  "$TP/emsdk/emsdk" activate latest
fi
source "$TP/emsdk/emsdk_env.sh" >/dev/null 2>&1

# ---- yafc-ce reference clone (lua patches + porting reference) ---------------
if [ ! -d "$TP/yafc-ce" ]; then
  git clone --depth 1 https://github.com/Yafc-CE/yafc-ce "$TP/yafc-ce"
fi

# ---- Lua 5.2.1 + Factorio patches --------------------------------------------
if [ ! -f "$TP/lua-5.2.1/src/lua.h" ]; then
  curl -sSo "$TP/lua-5.2.1.tar.gz" https://www.lua.org/ftp/lua-5.2.1.tar.gz
  echo "64304da87976133196f9e4c15250b70f444467b6ed80d7cfd7b3b982b5177be5  $TP/lua-5.2.1.tar.gz" | sha256sum -c -
  tar xzf "$TP/lua-5.2.1.tar.gz" -C "$TP"
  rm "$TP/lua-5.2.1.tar.gz"
  patch -d "$TP/lua-5.2.1/src" -p1 --no-backup-if-mismatch \
    < "$TP/yafc-ce/lua/lua-5.2.1.patch"
fi

# ---- or-tools v9.15, standalone GLOP, native + wasm --------------------------
if [ ! -d "$TP/or-tools" ]; then
  git clone --depth 1 --branch v9.15 https://github.com/google/or-tools "$TP/or-tools"
  git -C "$TP/or-tools" apply "$ROOT/patches/or-tools-v9.15-static-deps.patch"
fi

GLOP_FLAGS=(-DCMAKE_BUILD_TYPE=Release -DBUILD_CXX=OFF -DBUILD_GLOP=ON
            -DBUILD_DEPS=ON -DBUILD_SAMPLES=OFF -DBUILD_EXAMPLES=OFF
            -DBUILD_SHARED_LIBS=OFF)

if [ ! -f "$TP/or-tools/build_native/lib/libglop.a" ]; then
  cmake -S "$TP/or-tools" -B "$TP/or-tools/build_native" "${GLOP_FLAGS[@]}"
  cmake --build "$TP/or-tools/build_native" --target glop bz2_static -j"$JOBS"
fi

if [ ! -f "$TP/or-tools/build_wasm/lib/libglop.a" ]; then
  emcmake cmake -S "$TP/or-tools" -B "$TP/or-tools/build_wasm" "${GLOP_FLAGS[@]}"
  cmake --build "$TP/or-tools/build_wasm" --target glop bz2_static -j"$JOBS"
fi

echo "bootstrap complete."
