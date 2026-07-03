#!/usr/bin/env bash
# Builds the web module + assembles the static app into web/dist.
# Serve with e.g.:  python3 -m http.server -d web/dist 8080
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

source "$ROOT/third_party/emsdk/emsdk_env.sh" >/dev/null 2>&1

emcmake cmake -S "$ROOT" -B "$ROOT/build/web" -DCMAKE_BUILD_TYPE=Release \
  -DYAFC_WEB=ON -DBUILD_TESTING=OFF
cmake --build "$ROOT/build/web" --target yafc_web -j"$(nproc)"

mkdir -p "$ROOT/web/dist"
cp "$ROOT/build/web/yafc_web.js" "$ROOT/build/web/yafc_web.wasm" "$ROOT/web/dist/"
cp "$ROOT/web/index.html" "$ROOT/web/app.js" "$ROOT/web/worker.js" "$ROOT/web/dist/"
echo "web app assembled in web/dist ($(du -sh "$ROOT/web/dist" | cut -f1))"
