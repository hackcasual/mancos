# Changelog

## v0.1.5 — 2026-07-05

- Quality upgrades now stop at the highest tier your milestones allow, with
  that tier absorbing the remaining upgrade chance — no more phantom
  legendary output while legendary is still locked. The Research tab lists
  each quality tier with what unlocks it.
- Module configuration is one box per slot: tap a module to fill every box,
  tap a box first to fill from it rightward, long-press (or right-click) a
  box to set just that one.
- Layout: the Factory project selector and Settings live in the top bar,
  the solve status sits under the catalog sidebar, and Main products are
  listed at the top of the catalog.
- Bundles now carry pump speeds, inserter hand-size data
  (stack_size_bonus, max_belt_stack_size) and the inserter/bulk-inserter/
  belt stacking research effects — groundwork for belt and inserter
  throughput planning. Older bundles keep loading unchanged; rebuild with
  this bundler to add the new data.

## v0.1.4 — 2026-07-05

- Quality (Factorio 2.0/2.1) support end to end: demand goals, recipe rows
  and links all carry a quality tier; quality-module upgrade chances spread
  products across tiers in the solver; quality badges throughout the UI.
  Every quality affordance hides itself on packs that disable the feature.
- Fixed Factorio 2.1's quality-format change (chain_probability split):
  correct per-tier spreads for 2.0 and 2.1 data, including bundles built by
  older bundlers — old files are repaired at load, no rebuilds needed.
- Building quality: pick a machine tier per row (+30% crafting speed/level).
- Individual projects per pack with per-project settings: productivity
  research levels (critical for quality recycling loops), mining/research
  productivity %, and a hide-unreachable display filter.
- Blueprint export: one click copies a stampable Factorio blueprint of the
  page — every row's buildings placed with recipes, quality, modules and
  fuel configured.
- Milestone-aware defaults: new rows pick the highest-tier crafter your
  milestones allow (never the character); building/fuel/module pickers show
  milestone lock badges.
- Mobile: demand/pin entry uses a proper in-page dialog (native prompt()
  doesn't appear in installed PWAs); goals split into "Main products" and
  "Inputs to consume".
- Share links carry pack + milestones; search sorts reachable goods first;
  fluid temperature variants selectable per row.
- Node bundler: opt-in self-updater — interactive runs offer to install new
  releases (opt out with --no-update-check or MANCOS_BUNDLER_NO_UPDATE=1).
- Header shows the loaded pack's name; link previews (Open Graph metadata).

## v0.1.3 — 2026-07-03

- New project icon (the alpaca): favicon on both pages and the PWA/install
  icons.

- App mode (PWA): installable with offline support — the app shell (HTML/JS/
  wasm) precaches under a per-build service-worker cache, and pack bundles/
  manifests use stale-while-revalidate, so previously loaded packs plan fully
  offline. "Install app" button appears when the browser offers installation.
- Search hint now follows milestone progression (suggests the next pack you
  haven't reached) and search matches localized names alongside internal ones.

## v0.1.2 — 2026-07-03

- "Switch pack" closes the current project (persisted per pack, restored on
  re-pick) and returns to the pack chooser.
- Fixed cross-pack contamination: opening a pack with no saved project now
  starts clean instead of carrying the previous pack's tables.

## v0.1.1 — 2026-07-03

- Fixed Windows bundler crash (`weakly_canonical` on drive-letter paths):
  relative mod paths are computed by prefix stripping, no canonicalization.

## v0.1.0 — 2026-07-03

- First release: node-runnable bundler CLI (`mancos-bundler.mjs` +
  wasm, packaged Lua env), published from the release workflow.
- Planner + browser bundler live on GitHub Pages with a beta channel
  (`/beta/`, promoted via fast-forward); hosted packs served from the
  mancos-data repo.
- Feature state at release: yafc project import/export and share links,
  milestones dialog, research filter, modules/beacons with per-slot editor,
  building/fuel selection with favorites, link over-production control,
  undo/redo, multi-page projects, multi-language, mobile layout.
