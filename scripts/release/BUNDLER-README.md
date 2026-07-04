# Mancos bundler (node)

Builds a `.yafcbundle` for the [Mancos planner](https://hackcasual.github.io/mancos/)
from your own copy of Factorio — no browser, no C++ toolchain, just
[Node.js](https://nodejs.org) 18+.

```sh
node mancos-bundler.mjs <factorio-data> <mods-dir|vanilla> <out.yafcbundle>

# examples
node mancos-bundler.mjs ~/factorio/data vanilla vanilla.yafcbundle
node mancos-bundler.mjs "C:\Games\Factorio\data" "%APPDATA%\Factorio\mods" py.yafcbundle
```

- `factorio-data` is Factorio's `data/` directory (contains `core/` and `base/`).
- The mods directory must contain `mod-list.json`; pass `vanilla` to bundle
  the base game only.
- Load the result in the planner via "Load bundle…", or host it yourself.

Everything runs locally. The bundle contains the parsed prototype database,
analyses, per-object costs, extracted icons (32px), and locale catalogs —
data derived from your game files, so share it only where that's appropriate.

`env/` holds the bundler's Lua environment (from YAFC-CE, GPL-3.0); an
explicit env-dir can be passed as a fourth argument before the output path.
Licensed GPL-3.0 — see LICENSE and licenses.txt.

## Updates

Interactive runs check GitHub for a newer release and ask before
downloading it over this install (never without asking; a declined or
failed update just continues with the current version). Opt out with
`--no-update-check` or `MANCOS_BUNDLER_NO_UPDATE=1`; non-interactive
runs (CI, pipes) never check.
