<h1 align="center">Mancos</h1>
<p align="center">A factory planner for heavily modded Factorio, in your browser.</p>

Mancos is a C++/WebAssembly port of [YAFC: Community Edition](https://github.com/Yafc-CE/yafc-ce)
(itself a continuation of [YAFC](https://github.com/ShadowTheAge/yafc) by ShadowTheAge) —
the planner built to handle deeply recursive modpacks like Pyanodons. The data
model, analyses (cost, milestones, automation) and the OR-Tools/GLOP-based
production-table solver are direct ports; the UI is web-native. It reads and
writes desktop YAFC `.yafc` project files.

## How it works

Mancos is split in two so the app never touches game files:

- **Bundler** (`bundler.html`, or the `mancos_bundler` CLI): runs the full Lua
  data stage against *your* copy of Factorio + mods, entirely client-side, and
  emits a single `.yafcbundle` (database, analyses, icons, locales). Game
  assets stay on your machine.
- **Planner** (`index.html`): loads a bundle and plans — demand goals, linked
  goods with per-link over-production control, modules/beacons, milestones,
  research filtering, multi-page projects, undo/redo, serverless share links.

Everything runs in the browser; the solver runs in a Web Worker.

## Building

```sh
./scripts/bootstrap.sh      # fetches emsdk, or-tools (GLOP), patched Lua
./scripts/build-native.sh   # native build + test suite
./scripts/build-wasm.sh     # wasm build, tests run under node
./scripts/build-web.sh      # assembles the app into web/dist
python3 -m http.server -d web/dist 8080
```

## Release channels

`main` deploys to the site root; the `beta` branch deploys to `/beta/` on
the same Pages site (a push to either branch rebuilds both). Land work on
`beta`, try it at `…/mancos/beta/`, then promote with
`scripts/promote-beta.sh` — a fast-forward of `main` to the beta tip that
refuses if the branches have diverged.

## License

- [GNU GPL 3.0](/LICENSE), same as the YAFC it is ported from.
- Original YAFC copyright ShadowTheAge; community continuation by the
  [yafc-ce contributors](https://github.com/Yafc-CE/yafc-ce).
- Powered by free software: Google OR-Tools, Lua, Emscripten, miniz,
  nlohmann/json, stb and others (see [full list](/licenses.txt)).
- Factorio is the property of Wube Software. Mancos ships no game data or
  assets — users build bundles from their own installation.
