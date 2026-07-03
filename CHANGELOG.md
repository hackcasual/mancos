# Changelog

## Unreleased

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
