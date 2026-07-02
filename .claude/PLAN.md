# YAFC-Web: Port yafc-ce to the browser as C++/WebAssembly

Goal: port [Yafc-CE](https://github.com/Yafc-CE/yafc-ce) (Factorio production calculator,
C#/.NET 10 + SDL2 + Google OR-Tools + Lua 5.2.1) to run entirely client-side in a browser.

**Approach decision (2026-07-02): port the C# codebase to C++**, compiled to wasm with
Emscripten. Chosen over .NET-WASM/Blazor hosting of the existing C# because it gives a better
user experience and a simpler system:
- payload: a C++ build ships a few MB of wasm vs ~30+ MB of .NET runtime + assemblies;
  startup is near-instant, no JIT/interpreter warm-up, no GC pauses.
- all three native deps become *direct* dependencies — GLOP via its C++ API (no SWIG/P-Invoke
  shim), Lua via its C API, SDL2 via Emscripten's built-in port — one toolchain, one linker.
- no dotnet↔emscripten version-lock problem (dotnet pins its own emsdk for NativeFileReference).
- native desktop builds fall out for free (same C++ + real SDL2), useful for dev and testing.

Reference clone lives in `third_party/yafc-ce` (upstream analysis only, not part of the build).

## Upstream inventory (analyzed 2026-07-02, ~33k lines C#)

| Project | Size | What it is | Porting notes |
|---|---|---|---|
| `Yafc.Model` | 10.2k loc, 42 files | FactorioObject data model, production-table solver, analyses (cost, milestones, automation), reflection-based JSON serialization, undo system | Solver → `glop::LPSolver` direct (Phase 0 proves it). Serialization/undo need a C++ answer to C# reflection (see below) |
| `Yafc.Parser` | 6.0k loc, 11 files | Runs Factorio's Lua data stage (patched Lua 5.2.1), mod discovery/dependency sort, mod-settings.dat parser, icon atlas building | Lua C API is *easier* from C++; SharpCompress → libzip/minizip; SDL_image stays |
| `Yafc.UI` | 5.9k loc, 30 files | Custom immediate-mode GUI over SDL2/SDL_ttf (layout, widgets, scroll, drag, text input) | Port faithfully onto SDL2; Emscripten SDL2/ttf ports render to canvas. Multi-window (dropdowns/tooltips) must become overlays in-canvas |
| `Yafc` (app) | 10.8k loc, 47 files | Screens/pages (production tables, milestones, preferences, wizards), main loop, project lifecycle | Straight port once UI layer exists |
| `Yafc.I18n` | 0.4k | localization tables | trivial |
| `Yafc.Core` | 27 loc | misc | trivial |

### Dependency inventory → C++ replacements

Native:
| Dependency | Used by | C++ port |
|---|---|---|
| Google.OrTools 9.15 (GLOP) | Yafc.Model (3 files) | or-tools standalone glop lib, direct `glop::LPSolver` (Phase 0) |
| Lua 5.2.1 + Factorio patches (vendored, P/Invoke) | Yafc.Parser LuaContext | same source + patches, direct C API, emcc-compiled |
| SDL2, SDL2_ttf, SDL2_image (SDL2-CS bindings) | Yafc.UI | Emscripten `-sUSE_SDL=2` ports / system SDL2 natively |
| SHCore.dll (Windows DPI) | Yafc.UI | drop |

Managed NuGet:
| Dependency | Used by | C++ port |
|---|---|---|
| Newtonsoft.Json | Parser (mod-list.json), Yafc (welcome screen) | one JSON lib for everything (e.g. nlohmann or rapidjson) |
| System.Text.Json (BCL) | Yafc.Model serialization (.yafc project files) | same JSON lib; schema kept byte-compatible |
| SharpCompress | Parser (zipped mods) | libzip or minizip-ng |
| System.IO.Hashing | Parser data-stage cache keys | xxHash |
| Serilog (+console/file sinks) | everywhere | small logger over fmt; console sink in browser |
| Microsoft.CodeAnalysis.CSharp (Roslyn!) | Parser MathExpression — parses Factorio 2.0 math-expression strings (tech count formulas) via the C# parser | ~150-line recursive-descent expression parser |
| xunit (tests) | test projects | GoogleTest or Catch2 |

OR-Tools API surface actually used (all GLOP, no CP-SAT/MIP): create solver, GlopParameters
via text proto (`solution_feasibility_tolerance`, `random_seed` retry loop), variables/
constraints with bounds + coefficients, min/max objective, solve → status, solution values,
dual values, basis status per variable/constraint. Files: `Model/ProductionTable.cs`,
`Analysis/CostAnalysis.cs`, `Data/DataUtils.cs`. All of it maps 1:1 onto
`glop::LPSolver`/`LinearProgram` (see `solver-wasm/demo/yafc_lp_demo.cc` for the mapping table).

## Phase 0 — Solver feasibility (GLOP on wasm) — DONE 2026-07-02

The load-bearing native dep, proven first:
- [x] emsdk at `third_party/emsdk` (emcc 6.0.2, node 22).
- [x] or-tools v9.15 at `third_party/or-tools` (same version yafc pins on NuGet).
- [x] v9.15 cross-compiles with emscripten out of the box (upstream host-protoc support;
      the old patches from or-tools discussion #2997 are already upstream). One 2-line local
      patch: force static deps in `cmake/dependencies/CMakeLists.txt`.
- [x] Configure: `emcmake cmake -DBUILD_CXX=OFF -DBUILD_GLOP=ON -DBUILD_DEPS=ON` —
      standalone GLOP target (~10x smaller than full or-tools; glop + lp_data + absl +
      protobuf + zlib).
- [x] `libglop.a` built for wasm32-emscripten (~2.0 MB linked demo wasm at -O2).
- [x] Tracer bullet `solver-wasm/demo/yafc_lp_demo.cc` runs headless under node: the modded
      lead-plate chain (900 plates/min; gangue byproduct + grade-3 recycle loop force a real
      LP solve) using yafc's two-pass algorithm — equality links → INFEASIBLE → cost-weighted
      slack on infeasibility candidates → OPTIMAL — GlopParameters text proto, duals, basis
      status; crafts/min match the desktop yafc reference run exactly (incl. 127/min gangue
      as "Extra products").

Recipe + status: `solver-wasm/README.md`.

## Phase 1 — C++ project skeleton

1. `src/` CMake project, C++20; two first-class targets from day one:
   native desktop (Linux) for fast iteration/tests, and wasm via emcmake.
2. Vendor/build deps both ways: glop (from Phase 0), Lua 5.2.1 + Factorio patches
   (reuse `third_party/yafc-ce/lua/*.patch`), SDL2/SDL2_ttf/SDL2_image
   (system libs natively; `-sUSE_SDL=2` ports on wasm), libzip, a JSON lib
   (project files + data), {fmt} or std::format.
3. CI: build both targets + run unit tests natively and under node.
4. Decide base infrastructure: error handling policy, string type (UTF-8 std::string),
   ownership model for the object graph (model objects live in arenas owned by the
   database/project; raw pointers between them, like the C# object graph but explicit).

## Phase 2 — Port Yafc.Model (headless core)

1. Data model: FactorioObject hierarchy (goods/items/fluids/recipes/entities/technologies,
   quality variants), Database container, mappings (`CreateMapping<T>` → typed arrays keyed
   by object id — ports naturally to `std::vector` indexed by dense ids).
2. Solver: ProductionTable solve loop incl. slack-variable infeasibility fallback and
   `TrySolveWithDifferentSeeds`; CostAnalysis. Direct `glop::LPSolver` per Phase 0 demo.
3. Serialization (sized 2026-07-02: upstream = 2,009 loc infra + ~20 serialized classes;
   C++ ≈ 1.5-2.5k loc + ~1 line per property in per-class field lists; ~3-5 focused days on
   top of the model classes; risk is compat quirks — obsolete-prop migration, unknown-prop
   tolerance — locked down by round-trip tests on real .yafc files):
   C# uses reflection over properties → C++ needs explicit schemas.
   Approach: hand-written per-class serialize/deserialize with a tiny visitor helper so each
   class declares fields once (macro or member-list function used by JSON writer, JSON reader,
   AND the undo snapshotter — same three consumers as upstream).
   **Hard requirement: read/write upstream `.yafc` project JSON unchanged** so desktop users
   can move projects between yafc-ce and yafc-web.
4. Undo system: upstream snapshots objects via the property serializers; same visitor
   mechanism gives us binary snapshot/restore.
5. Tests: port the interesting parts of `Yafc.Model.Tests`; add golden tests — run desktop
   C# yafc-ce on a fixed dataset, dump solved tables/analyses, compare C++ output within
   tolerance. This is the main defense against silent port regressions.

## Phase 3 — Port Yafc.Parser (data pipeline in the browser)

1. Lua data stage: LuaContext (require resolution across mods, `defines` table, feature
   flags, settings stage) against patched Lua 5.2.1 built with emcc. Direct C API — simpler
   than upstream's P/Invoke.
2. Mod handling: mod-list.json, dependency sort, versioned zips (libzip over OPFS/MEMFS),
   mod-settings.dat (custom binary property-tree parser — straight port).
3. Prototype → model deserialization (`Data/` subdir) and icon atlas (SDL_image decode,
   layered icon compositing).
4. Browser data input UX: user picks Factorio `data/` + `mods/` via File System Access API /
   drag-drop / zip upload; mirror into OPFS; run data stage in a worker with progress UI.
   Vanilla-only quick start: prepackaged prototype dump downloaded from the site (check
   Wube redistribution rules — prototypes yes, icons are game assets: likely require the
   user to supply game files, same as yafc today).
5. Perf checkpoint: full vanilla data stage under wasm; budget ~seconds, streamed progress.

## Phase 4 — Port Yafc.UI + app screens

1. Port the immediate-mode GUI core (ImGui.cs, layout, batching, text cache, input) onto
   SDL2; keep yafc's look/behavior — it's a calculator UI, fidelity matters to users.
   Spike first: SDL2 renderer + SDL_ttf under emscripten in-canvas (fonts, DPI, IME/text
   input, clipboard, cursors, mouse wheel).
2. Multi-window usage (dropdown panels, dialogs, tooltips as OS windows) → in-canvas
   overlay windows managed by our compositor; desktop target may keep real windows.
3. Port app screens incrementally: main production table view first (usable milestone!),
   then milestones/settings/wizards/blueprints etc.
4. Browser niceties: canvas resize/DPI, touch (stretch), persistent preferences
   (localStorage/OPFS).

## Phase 5 — Product & packaging

- Static hosting; wasm size budget & brotli; lazy-load data-stage worker.
- Project persistence: OPFS autosave + file import/export (compatible `.yafc` JSON).
- Threading: solver/data-stage in workers (pthreads need COOP/COEP — coi-serviceworker on
  GitHub Pages, or single-threaded + async slicing as fallback).
- Upstream tracking: pin the ported yafc-ce commit; document porting status per file;
  periodically diff upstream for calculator-logic fixes to forward-port.

## Key risks

1. **Port fidelity** (33k loc of subtle calculator logic) — mitigated by golden tests vs the
   C# implementation (Phase 2.5) and by porting file-by-file, not redesigning.
2. **Serialization without reflection** — visitor/member-list pattern, `.yafc` compat locked
   by round-trip tests against real project files.
3. **SDL2 UI in canvas** — Phase 4 spike early (text input/IME is the classic pain);
   fallback is a rewrite of the front-end in web tech over the C++ core (JS interop via
   embind), decided at the spike.
4. **Data-stage size/perf in browser** — worker + progress; prepackaged vanilla dump option.
5. **Game-asset licensing for a hosted app** — icons/sounds are Wube's; require user-supplied
   game files (as desktop yafc does) unless cleared otherwise.
