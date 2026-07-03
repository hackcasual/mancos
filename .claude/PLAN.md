# YAFC-Web: Port yafc-ce to the browser as C++/WebAssembly

# Human Priorities (2026-07-03) — tracked

1. [x] (2026-07-03) Loading yafc projects — wire the Phase 2 serialization layer into the web app:
   projectSave/projectLoad web APIs mapping the session table <-> Project pages[0]
   (links with amount -> goals, amount 0 -> plain links, rows -> rows); .yafc file
   import/export buttons. Full desktop .yafc compat still gated on the remaining
   RecipeRow fields (entity/modules/quality) per Phase 2.3.
2. [x] (2026-07-03) Base-64 project share links (server-less) — project JSON -> deflate (miniz) ->
   base64url in a ?p= query parameter; loading a URL with ?p= restores the table.
   ~1-2KB tables deflate+encode to a few hundred chars.
3. [x] (2026-07-03) Modpack manifest — web/dist/bundles/manifest.json [{id,name,file,bytes,notes}];
   pack selector on start; last-loaded pack id in localStorage, auto-opened by default;
   local file loading stays for user-generated bundles (split-app licensing model).
4. [x] (2026-07-03) Technology levels — group leveled technology families (name-N and mkNN patterns)
   in the picker as one entry with a researched-level selector; prerequisite closure
   already implies lower levels; recipe gating by family level falls out of the
   existing technologyUnlock ∩ researched check.
5. [x] (2026-07-03) Mobile phone mode — single-column layout optimized for building a table:
   catalog as a bottom sheet, large touch targets, collapsible flows/links sections.

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
   pthreads land. [x] Hierarchical model ported 2026-07-03 (`production_table.*`):
   ProductionTable/RecipeRow/ProductionLink with nested subgroups, linkRoot resolution,
   Setup/flatten into the solve core, CalculateFlow rollup with ChildNotMatched,
   CheckBuiltCountExceeded; RecipeParameters is a data seam (CalculateParameters —
   crafting speed/modules/beacons/fuel — is Phase 3/4 work, needs parsed entities).
   [x] TechnologyScienceAnalysis ported 2026-07-03.
3. Serialization: [x] foundation ported 2026-07-03 (`serialization/serialization.*`,
   nlohmann/json): single Visit* property declaration per class walked by JSON writer,
   JSON reader and undo; FactorioObject refs as typeDotName; unknown properties and
   unresolvable refs collected, not fatal; Project/ProjectPage/ProjectSettings +
   table/row/link subset with upstream field names. Remaining for full .yafc compat
   (Phase 3+, as model fields land): entity/modules/beacons on RecipeRow, quality
   suffixes on goods refs, obsolete-prop migrations, validation against real desktop
   .yafc files — that validation is the acceptance bar.
   Approach: hand-written per-class serialize/deserialize with a tiny visitor helper so each
   class declares fields once (macro or member-list function used by JSON writer, JSON reader,
   AND the undo snapshotter — same three consumers as upstream).
   **Hard requirement: read/write upstream `.yafc` project JSON unchanged** so desktop users
   can move projects between yafc-ce and yafc-web.
4. Undo system: [x] snapshot-based UndoSystem 2026-07-03 (whole-project JSON snapshots,
   bounded depth, undo/redo stacks). Upstream snapshots per-object; revisit granularity
   if profiling demands it.
5. Tests: port the interesting parts of `Yafc.Model.Tests`; add golden tests — run desktop
   C# yafc-ce on a fixed dataset, dump solved tables/analyses, compare C++ output within
   tolerance. This is the main defense against silent port regressions.

## Phase 3 — Port Yafc.Parser (data pipeline in the browser)

1. Lua data stage: [x] ported 2026-07-03 (`src/yafc/parser/lua_context.*`,
   `factorio_data_source.*`): LuaContext on the direct C API (sandbox + version-selected
   defines, mod-aware require with the traceback-based caller detection, chunk-name
   rewriting, helpers.compare_versions/evaluate_expression (tiny recursive-descent parser
   replaces Roslyn), yafc.parse_energy), mod discovery from folders, version selection,
   dependency parse/check, core-first alphabetical-batch load order, data.lua ×3 +
   Postprocess. TRACER GREEN: the full vanilla+Space Age data stage runs from
   data/factorio/data on native AND as wasm under node (-sNODERAWFS in tests), data.raw
   verified. Environment lua files load from third_party/yafc-ce/Yafc/Data (vendoring +
   GPL-3.0 licensing decision pending). Deviations: feature_flags follow the game (mod
   presence) instead of upstream's always-false TODO; mod-fix hooks and locale files not
   ported (web i18n). Next: zipped mods (minizip), mod-settings.dat property tree,
   FactorioDataDeserializer (data.raw → Database).
2. Mod handling: [x] complete 2026-07-03 — mod-list.json + dependency sort (in 3.1),
   zipped mods via miniz (`zip_archive.*`, upstream depth-1 info.json rule; all 91 corpus
   zips discovered), mod-settings.dat property tree (`property_tree.*`) into the settings
   global. FULL modded data stage (40-mod Pyanodon suite) green on native (~8s suite) and
   wasm/node (~10s), lead-chain prototypes verified in data.raw.
3. Prototype → model deserialization (`Data/` subdir) — in flight (background port).
   Icon pipeline (decision 2026-07-03: icons ARE extracted for the web UI, no SDL):
   the deserializer captures per-object iconSpec (mod-relative paths + layer
   size/shift/tint/scale, upstream FactorioIconPart); a core extraction utility
   resolves those paths through ModSet (zips/folders), dedupes PNGs by content hash,
   and emits blobs + an icons.json manifest (typeDotName → layer list). The web layer
   stores blobs in OPFS and composites layers on canvas at draw time (browser PNG
   decode — no SDL_image). Hosted vanilla quickstart still gated on the game-asset
   licensing question (PLAN "Key risks" #5).
4. Browser data input UX: user picks Factorio `data/` + `mods/` via File System Access API /
   drag-drop / zip upload; mirror into OPFS; run data stage in a worker with progress UI.
   Vanilla-only quick start: prepackaged prototype dump downloaded from the site (check
   Wube redistribution rules — prototypes yes, icons are game assets: likely require the
   user to supply game files, same as yafc today).
5. Perf checkpoint: full vanilla data stage under wasm; budget ~seconds, streamed progress.

## Phase 4 — Web-native UI (decision 2026-07-02: no SDL port)

1. [x] Milestone 1 shipped 2026-07-03: wasm-core API boundary (`src/web/web_api.cc`,
   embind, JSON results; bundle bytes via wasm memory) running in a dedicated Web Worker
   (`web/worker.js`, per the threading directive), with a vanilla-JS planner page
   (`web/index.html`/`app.js`): load bundle, search goods, producer browsing, demand
   goals, link/row management, solve, flows incl. imports/surplus, first-layer icons via
   blob URLs. `scripts/build-web.sh` -> web/dist (2.3 MB incl. wasm). Headless node smoke
   drives the golden lead table through the public API.
   Increment 2 (2026-07-03): candidate auto-pull on flows (directive: "produce ▸"/
   "consume ▸" link the goods and auto-add a sole candidate or present a ranked picker —
   in-table-ingredient overlap first), full icon layer compositing (canvas: scale/shift/
   tint per FactorioIconPart), nameplate-row visual identity (blueprint grid workspace,
   circuit red/green flow semantics, amber goals, tabular-mono numerics, system fonts
   only — offline-first), row removal, goal edit/remove, unlink, per-bundle localStorage
   persistence. Increment 3 (2026-07-03): locale .cfg parser in the bundler (`parser/locale.*`,
   Factorio implicit-key rules with entity/product fallbacks, split-fluid temperature
   suffixes, Mechanics verb composition) — names baked into the dump ("Lead (grade 1)");
   analyses run at bundle time and per-object yafc costs ship as costs.cbor; candidates
   sort available-first-then-cost (directive) with cost badges; Research tab
   (searchTechs/setResearch APIs, transitive prerequisite closure, per-bundle persisted,
   optional researched-only filter gating candidate availability with lock badges).
   Next: TS types, multi-language catalogs, .yafc project import/export, RecipeParameters
   for building counts.
2. Front-end stack: TypeScript; framework + rendering strategy decided by a spike on the
   production-table grid (DOM vs canvas for the big table; yafc's ImGui layout behavior as
   the spec). Icons: decode mod PNGs with browser APIs, composite layered icons on canvas.
3. I18n web-native: message catalogs (ICU-style) + Intl for numbers/units; convert
   yafc-ce's existing translation files as seed data.
4. Screens incrementally: production table view first (usable milestone!), then
   milestones/settings/wizards/blueprints. Preferences in localStorage/OPFS.
5. Pre-parsed popular mod bundles. Instead of requiring the user to point open local files
   have a bundler that runs the parsing logic, serializes it and includes translation
   strings/art assets. This could be pre-setup for a few popular mod packs and allow
   a user to select their modpack combination of choice.
   Implementation notes (2026-07-03):
   - Needs **Database serialization** (dump/load the parsed Database itself, bypassing the
     Lua data stage on load) — the C++ analogue of upstream's Cache.ReadCSharp/WriteCSharp,
     which we skipped. Format: versioned binary or JSON via the existing serialization
     visitor pattern; load path must rebuild id ranges + derived collections.
   - Needs the **locale .cfg parser** (per-mod locale/<lang>/*.cfg INI-ish files) that we
     skipped — bundles carry translation catalogs for the web i18n layer.
   - Bundle = {database dump, icons.json manifest + deduped PNG blobs (Phase 3.3 extractor),
     locale catalogs, modpack metadata incl. mod versions for cache keys}.
   - [x] Bundler CLI + bundle format shipped 2026-07-03: `yafc_bundler` (native)
     parses+deserializes+extracts and writes the bundle zip (meta.json, database.cbor
     14.6 MB for the py corpus, icons.json + 4,899 PNGs 33.7 MB); `ReadBundle[FromMemory]`
     loads it with zero game-file access; the golden lead-plate test passes identically
     on a bundle-roundtripped Database. Locale catalogs still TODO (needs the .cfg parser).
   - **Split-app architecture (directive 2026-07-03): the product is TWO apps.**
     (a) The *bundler*: has filesystem access (native CLI for CI, and/or a browser page
     using the File System Access API running the same wasm core) — points at the user's
     game+mods, runs parse+deserialize+icon extraction+locale collection, emits a single
     bundle file. (b) The *main app*: NEVER touches raw game files — it only loads bundles,
     either pre-set (hosted, where licenses permit) or user-local (generated by the
     bundler from their own purchased assets, kept in OPFS or as a downloaded file).
   - Licensing consequence: the hosted site ships only bundles whose mod licenses permit
     redistribution; anything encumbered (base game assets, restrictive mods) stays in
     user-generated local bundles — the user bundles their own bought assets. This
     resolves Key risk #5 by construction.
   - [x] Browser bundler page shipped 2026-07-03: `src/web/bundler_web_api.cc`
     (embind: `runBundler(dataPath, modsPath, progress)` + `bundleBytes()`) is the same
     pipeline as the CLI (`yafc_bundler`), built as a second wasm module
     (`yafc_bundler_web`, worker-only — `-sENVIRONMENT=worker`, `-lworkerfs.js`) with
     yafc's own env Lua files baked in via `--embed-file` at `/env` (the page never asks
     for those, only the user's data/mods folders). `web/bundler.html`/`bundler.js` grant
     read access via `showDirectoryPicker()` (File System Access API — Chromium-only,
     feature-detected with a fallback message); `web/bundler-worker.js` receives the
     picked `FileSystemDirectoryHandle`s (structured-cloneable, so the possibly-huge
     directory walk happens off the main thread), builds a WORKERFS file list (relative
     paths faked via `new File([handle], path)`, no eager reads — WORKERFS reads lazily
     per chunk via `FileReaderSync`), and mounts it (`web/bundler_pre.js`'s
     `Module.mountFS`, injected via `--pre-js` so it can see the bare `FS`/`WORKERFS`
     runtime symbols without exporting them). Output crosses back as a transferred
     ArrayBuffer, saved via `showSaveFilePicker()` (download-link fallback). A fresh
     worker spins up per run since an aborted wasm instance can't be reused.
     Fixed along the way: the wasm build had exceptions silently disabled (Emscripten's
     default leaves `try`/`catch` dead code), so C++ error paths — bad bundle files, bad
     game/mod folders — aborted the whole module instead of returning a JSON error; now
     built with `-fwasm-exceptions` project-wide (CMakeLists.txt), matched with
     `SUPPORT_LONGJMP` staying wasm-EH-based too (needed for Lua's `lua_pcall`). Doesn't
     touch or-tools/GLOP's own (exception-free) codegen — `yafc::LpSolver` already talks
     to it via status codes, so no unwind ever crosses that boundary; full wasm/native
     test suites pass unchanged (golden numbers verified) after the flag change.
     Not yet exercised in a real browser (no headless-browser tooling in this sandbox) —
     validated via a Node smoke test with `FileReaderSync`-independent checks (module
     loads, embind bindings callable, error path returns JSON) and the existing
     wasm/node data-stage tests; still needs a real Chromium pass against actual game
     files before calling it done-done.

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
