#!/usr/bin/env bash
# Builds the web module + assembles the static app into web/dist.
# Serve with e.g.:  python3 -m http.server -d web/dist 8080
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

source "$ROOT/third_party/emsdk/emsdk_env.sh" >/dev/null 2>&1

emcmake cmake -S "$ROOT" -B "$ROOT/build/web" -DCMAKE_BUILD_TYPE=Release \
  -DYAFC_WEB=ON -DBUILD_TESTING=OFF
cmake --build "$ROOT/build/web" --target mancos_web mancos_bundler_web -j"$(nproc)"

mkdir -p "$ROOT/web/dist"
cp "$ROOT/build/web/mancos_web.js" "$ROOT/build/web/mancos_web.wasm" "$ROOT/web/dist/"
cp "$ROOT/build/web/mancos_bundler_web.js" "$ROOT/build/web/mancos_bundler_web.wasm" "$ROOT/web/dist/"
cp "$ROOT/web/index.html" "$ROOT/web/app.js" "$ROOT/web/worker.js" "$ROOT/web/dist/"
cp "$ROOT/web/bundler.html" "$ROOT/web/bundler.js" "$ROOT/web/bundler-worker.js" "$ROOT/web/dist/"

# App mode: manifest, icons, and the service worker stamped with a build
# version so each deploy gets its own atomic shell cache.
cp "$ROOT/web/manifest.webmanifest" "$ROOT/web/dist/"
mkdir -p "$ROOT/web/dist/icons"
cp "$ROOT"/web/icons/*.png "$ROOT/web/dist/icons/"
SW_VERSION="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)-$(date +%s)"
sed "s/__BUILD_VERSION__/$SW_VERSION/" "$ROOT/web/sw.js" > "$ROOT/web/dist/sw.js"

# Publish any locally built bundles + a manifest (human priority 3). Hosted
# deployments curate this list by license; local dev just ships what's there.
if ls "$ROOT"/data/*.yafcbundle >/dev/null 2>&1; then
  mkdir -p "$ROOT/web/dist/bundles"
  cp "$ROOT"/data/*.yafcbundle "$ROOT/web/dist/bundles/"
  python3 - "$ROOT/web/dist/bundles" <<'PYEOF'
import json, os, sys
bundle_dir = sys.argv[1]
packs = []
for name in sorted(os.listdir(bundle_dir)):
    if not name.endswith('.yafcbundle'):
        continue
    stem = name[:-len('.yafcbundle')]
    packs.append({
        'id': stem,
        'name': stem.replace('-', ' ').replace('_', ' ').title(),
        'file': 'bundles/' + name,
        'bytes': os.path.getsize(os.path.join(bundle_dir, name)),
    })
with open(os.path.join(bundle_dir, 'manifest.json'), 'w') as f:
    json.dump({'packs': packs}, f, indent=1)
print(f"manifest: {len(packs)} pack(s)")
PYEOF
fi
echo "web app assembled in web/dist ($(du -sh "$ROOT/web/dist" | cut -f1))" 
