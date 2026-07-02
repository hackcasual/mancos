# YAFC-Web: Port yafc-ce to the browser as C++/WebAssembly

Goal: port [Yafc-CE](https://github.com/Yafc-CE/yafc-ce) (Factorio production calculator,
C#/.NET 10 + SDL2 + Google OR-Tools + Lua 5.2.1) to run entirely client-side in a browser.

**Approach decision (2026-07-02): port the C# codebase to C++**, compiled to wasm with
Emscripten. Chosen over .NET-WASM/Blazor hosting of the existing C# because it gives a better
user experience and a simpler system:
- payload: a C++ build ships a few MB of wasm vs ~30+ MB of .NET runtime + assemblies;
  startup is near-instant, no JIT/interpreter warm-up, no GC pauses.
- the two hard native deps become *direct* dependencies — GLOP via its C++ API (no
  SWIG/P-Invoke shim), Lua via its C API — one toolchain, one linker.
- **UI decision (2026-07-02): web-native front-end** (HTML/DOM/canvas + TS) over a wasm-core
  API — SDL2/SDL2_ttf/SDL2_image are NOT ported; the browser already provides events, text,
  fonts, PNG decode, clipboard. I18n lives in the web layer (message catalogs + Intl);
  the C++ core returns typed codes, never localized strings.
- no dotnet↔emscripten version-lock problem (dotnet pins its own emsdk for NativeFileReference).
- native desktop builds fall out for free (same C++ + real SDL2), useful for dev and testing.

Reference clone lives in `third_party/yafc-ce` (upstream analysis only, not part of the build).

## Upstream inventory (analyzed 2026-07-02, ~33k lines C#)

| Project | Size | What it is | Porting notes |
|---|---|---|---|
| `Yafc.Model` | 10.2k loc, 42 files | FactorioObject data model, production-table solver, analyses (cost, milestones, automation), reflection-based JSON serialization, undo system | Solver → `glop::LPSolver` direct (Phase 0 proves it). Serialization/undo need a C++ answer to C# reflection (see below) |
| `Yafc.Parser` | 6.0k loc, 11 files | Runs Factorio's Lua data stage (patched Lua 5.2.1), mod discovery/dependency sort, mod-settings.dat parser, icon atlas building | Lua C API is *easier* from C++; SharpCompress → libzip/minizip; SDL_image stays |
| `Yafc.UI` | 5.9k loc, 30 files | Custom immediate-mode GUI over SDL2/SDL_ttf (layout, widgets, scroll, drag, text input) | NOT ported — replaced by a web-native front-end over the wasm-core API; reuse its interaction/layout design as spec |
| `Yafc` (app) | 10.8k loc, 47 files | Screens/pages (production tables, milestones, preferences, wizards), main loop, project lifecycle | Screens rebuilt web-native; non-UI logic (project lifecycle) moves into the core |
| `Yafc.I18n` | 0.4k | localization tables | web-layer i18n (catalogs + Intl); core emits typed codes; upstream translations convertible as data |
| `Yafc.Core` | 27 loc | misc | trivial |

### Dependency inventory → C++ replacements

Native:
| Dependency | Used by | C++ port |
|---|---|---|
| Google.OrTools 9.15 (GLOP) | Yafc.Model (3 files) | or-tools standalone glop lib, direct `glop::LPSolver` (Phase 0) |
| Lua 5.2.1 + Factorio patches (vendored, P/Invoke) | Yafc.Parser LuaContext | same source + patches, direct C API, emcc-compiled |
| SDL2, SDL2_ttf, SDL2_image (SDL2-CS bindings) | Yafc.UI | dropped — web-native UI; browser does events/fonts/PNG/clipboard |
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

## Phase 1 — C++ project skeleton — DONE 2026-07-02 (deferred bits noted)

1. [x] Root CMake project, C++20, native (`scripts/build-native.sh`) + wasm
       (`scripts/build-wasm.sh`) targets; `scripts/bootstrap.sh` fetches/builds all of
       third_party (emsdk, or-tools+patch, lua, yafc-ce ref); tests via ctest run natively
       and under node (emscripten's CROSSCOMPILING_EMULATOR). Native test loop: ~0.01s.
2. [x] glop imported from or-tools build trees (`cmake/ortools_glop.cmake`; native needs
       LINK_GROUP:RESCAN for absl's circular archives). Lua 5.2.1 + Factorio patch built
       both ways (`cmake/lua.cmake`); patch behavior locked by tests (insertion-ordered
       pairs(), tolerant next()). `yafc::LpSolver` wrapper mirrors the C# Solver API names;
       Phase 0 tracer now lives on as `tests/solver_test.cc` on both targets.
       Deferred to the phase that needs them: SDL2 (Phase 4), libzip + JSON lib (Phase 2/3).
3. [x] CI workflow `.github/workflows/ci.yml` (bootstrap cached on patch/script hash;
       both targets) — unverified until a GitHub remote exists.
4. [~] Base infra: UTF-8 std::string throughout; error handling = status returns near the
       solver, exceptions off on wasm. Object-graph ownership decided at Phase 2 start
       (leaning: arena owned by Database/Project, raw non-owning pointers between objects).

## Phase 2 — Port Yafc.Model (headless core)

1. Data model: [x] ported 2026-07-02 (`src/yafc/model/data_classes.{h,cc}` +
   `database.{h,cc}`): FactorioObject hierarchy (Goods/Item/Module/Fluid/Special,
   RecipeOrTechnology/Recipe/Mechanics/Technology, Entity family incl. crafters/labs,
   Quality with upstream bonus math, Ingredient/Product with catalyst-productivity math,
   ObjectWithQuality), Database with LoadBuiltData id assignment (stable sort by
   FactorioObjectSortOrder → contiguous per-type ranges), FactorioIdRange views, dense 1D/2D
   Mappings, typeDotName lookup + alias support, derived collections (allModules/allCrafters/
   allSciencePacks/...). C# camelCase field names kept for mechanical porting. Trimmed for
   now: GetDependencies (arrives with milestone analysis), spoilage Lazy resolution (loader
   fills fields), attractor/spawner/projectile entity subclasses.
2. Solver: [x] flat solve core ported 2026-07-02 (`src/yafc/model/production_table_solver.*`
   + `graph.h` Tarjan SCC): LinkAlgorithm bounds, fixed buildings, accumulating link
   coefficients, one-sided-link disabling, BaseCost objective, slack fallback with real
   GetInfeasibilityCandidates (splits + SCC deadlocks + chords), notMatchedFlow +
   bit-compatible link/warning flags, per-link production/consumption totals. 7 test
   scenarios both targets. [x] CostAnalysis ported 2026-07-02
   (`src/yafc/analysis/cost_analysis.*`): full LP (logistics/power/pollution costs, single
   fuel selection, spoilage-container special case, mining rarity penalty, misc-source and
   fluid/heat temperature-chain constraints), flows from constraint duals, recipe waste %,
   entity placement costs, important-items ranking; accessibility via AccessibilityHooks
   seam until Milestones/Automation are ported; TechnologyScienceAnalysis targetTechnology
   mode TODO. [x] Dependency graph + Milestones + AutomationAnalysis ported 2026-07-02
   (`bits.h`, `dependency_node.*`, `dependencies.*`, `milestones.*`,
   `automation_analysis.*`): require-all/any dependency trees with per-class
   GetDependencies dispatch (capture-ammo spawner sources TODO), reverse index, milestone
   flood-fill with per-milestone pruning walks + inaccessible-mask prediction + locked
   mask, automation queue propagation (character-only-crafter and late-machine cases),
   HooksFromAnalyses wiring into CostAnalysis. Per-milestone walks sequential until wasm
   pthreads land. Remaining: hierarchical Setup/flatten, CalculateFlow rollup,
   CheckBuiltCountExceeded, TechnologyScienceAnalysis.
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

## Phase 4 — Web-native UI (decision 2026-07-02: no SDL port)

1. Define the wasm-core API boundary (embind or C ABI + small TS glue): project
   open/save, table & link CRUD, solve, database queries (goods/recipes/icons), undo.
   Core emits typed warning/error codes + structured results; all presentation and i18n
   live in the web layer.
2. Front-end stack: TypeScript; framework + rendering strategy decided by a spike on the
   production-table grid (DOM vs canvas for the big table; yafc's ImGui layout behavior as
   the spec). Icons: decode mod PNGs with browser APIs, composite layered icons on canvas.
3. I18n web-native: message catalogs (ICU-style) + Intl for numbers/units; convert
   yafc-ce's existing translation files as seed data.
4. Screens incrementally: production table view first (usable milestone!), then
   milestones/settings/wizards/blueprints. Preferences in localStorage/OPFS.

## Phase 5 — Product & packaging

- Static hosting; wasm size budget & brotli; lazy-load data-stage worker.
- Project persistence: OPFS autosave + file import/export (compatible `.yafc` JSON).
- Upstream tracking: pin the ported yafc-ce commit; document porting status per file;
  periodically diff upstream for calculator-logic fixes to forward-port.

## Threading & performance (directive 2026-07-02: multithreaded, off-main-thread solving)

- Architecture: the wasm core runs in a **dedicated Web Worker** from day one — the main
  thread only does UI; solve/data-stage requests are async messages. This also sidesteps
  most COOP/COEP pain until pthreads are enabled.
- Within the core: wasm pthreads (SharedArrayBuffer, COOP/COEP headers — coi-serviceworker
  for static hosts) for parallel work: concurrent table solves, analyses, data-stage
  subtasks. Desktop native uses std::thread identically.
- Polish pass: build with `-msimd128` (+ relaxed SIMD feature-detect), profile GLOP and the
  Lua data stage; Emscripten BigInt/bulk-memory/threads flags tuned; measure before/after.

## Key risks

1. **Port fidelity** (33k loc of subtle calculator logic) — mitigated by golden tests vs the
   C# implementation (Phase 2.5) and by porting file-by-file, not redesigning.
2. **Serialization without reflection** — visitor/member-list pattern, `.yafc` compat locked
   by round-trip tests against real project files.
3. **UI rewrite volume + API boundary design** — the web-native front-end is new code
   (~specced by Yafc.UI/Yafc behavior); keep the wasm-core API thin and typed, spike the
   table grid early to fix the rendering approach.
4. **Data-stage size/perf in browser** — worker + progress; prepackaged vanilla dump option.
5. **Game-asset licensing for a hosted app** — icons/sounds are Wube's; require user-supplied
   game files (as desktop yafc does) unless cleared otherwise.
