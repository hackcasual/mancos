# YAFC-Web: Port yafc-ce to the browser as C++/WebAssembly

# Operating Guidelines

Do not make changes to README.md, all text there is human authored

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
6. [x] (2026-07-04) Mobile-safe demand/pin entry — native prompt() doesn't reliably appear in
   installed/standalone-mode PWAs; replaced with a shared in-page amount dialog for both
   "Set demand" and "Pin rate".
7. [x] (2026-07-04) Main-products separator — Goals strip splits into "Main products"
   (output demand) and "Inputs to consume" (negative/consume goals) sections.
8. [x] (2026-07-04) Producing at quality (Factorio 2.0) — full solver threading: see
   Increment 12 below.
9. [x] (2026-07-04) Individual projects + project settings — multiple named projects per
   bundle, each with its own productivity research state (mining %, research %,
   per-technology productivity levels — critical for quality recycling loops): see
   Increment 13 below. Reference scenario: LDS recycling for higher-quality plastic.
10. [x] (2026-07-04) Blueprint auto-generation (sets of buildings with recipes placed) —
   building footprints were already in the bundle (Entity width/height from
   selection_box, upstream-parity tile rounding, round-tripped by database_dump.cc);
   now exposed in rowOptions crafter briefs and consumed by the exporter: see
   Increment 14 below.

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
   CheckBuiltCountExceeded. [x] RecipeParameters ported 2026-07-03
   (`recipe_parameters.*`, upstream CalculateParameters): crafting speed + quality,
   fuel energy incl. FluidHeat temperature math and generator inversion
   (ScaleProductionWithPower), productivity sources (mining/research/per-tech levels/
   reactor neighbor bonus), maximumProductivity cap, speed/energy-usage mods, drain,
   fuel-consumption-limit throttling; Setup() recomputes parameters per row from
   ProductionTable::settings. Rows get a default crafter (first) + fuel (first) at
   web add; tableSolve emits entity tdn/locName, building count and per-building MW;
   nameplates render "crafter ×N · kW". Not yet: GetModulesInfo (modules/beacons),
   UselessQuality warning.
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
   Next: TS types, multi-language catalogs (done), .yafc project import/export (done),
   RecipeParameters for building counts (done 2026-07-03); modules/beacons UI next.
   Increment 4 (2026-07-03): Milestones filter (desktop Milestones dialog): lazy
   Dependencies+Milestones compute in the worker (~100ms pyanodon), milestonesList/
   setMilestones APIs (Milestones::SetUnlocked = mask-only update, ~0.4ms/toggle),
   per-bundle persisted unlocked set, auto-opening <dialog> on first pack load,
   milestone-icon lock badges + rank sort (reachable < research-locked <
   milestone-locked < inaccessible) in candidate lists and goods search; auto-pull
   skips gated recipes. Spent-fuel fix (user bug report, nuclear sample page):
   burning a fuel with fuelResult now emits the spent item 1:1 in the solver flatten,
   CalculateFlow and per-row web flows (desktop parity); per-row entity/fuel choices
   round-trip through .yafc load/save (VisitRow "entity" prop added) and
   tableAddRecipe(tdn, fixed, entityTdn, fuelTdn) — a reactor burning mox vs uranium
   cells is a different chain; user's 53-page project: every page solves.
   Not ported yet: custom milestone sets ("Edit milestones" editor), tech progression
   settings dialog. Temperature-variant fluid links (steam@250) fixed in Increment 11.
   Increment 5 (2026-07-03): modules & beacons + row config (directive): ModuleTemplate/
   GetModulesInfo port in recipe_parameters (slot fill with fixedCount-0 = fill-remaining,
   entity+recipe CanAcceptModule filtering, beaconList totals -> ceil(total/slots) beacons,
   beaconEfficiency x profile(N) — validated against Factorio 2.0 1/sqrt(N) profiles),
   ModuleFillerParameters (page defaults: fillerModule + beacon fill; autoFillPayback
   economics NOT ported), usedModules/usedBeacon echo for UI. Serialization: row
   "modules" + table "modules" (filler) round-trip .yafc (overrideCrafterBeacons not
   ported). Web API: tableAddRecipe(tdn, configJson), rowOptions (crafters/fuels/
   compatible modules/beacons+their modules), setDefaults (favorites = default
   building/fuel pick), tableSetFiller. UI: row-config <dialog> on every plate (building/
   fuel/module-fill/beacon sections, ☆ favorites persisted per bundle), module+beacon
   icons on nameplates. User project: 55 module rows apply, all 53 pages solve.
   Module UI is single-type fill; multi-entry templates from desktop are preserved and
   shown as chips. Next: per-slot module editor, module cost in candidate ranking.
   Increment 6 (2026-07-03): bundle diet. Icons cropped to base mip + area-downscaled
   to <=32px at bundle time (icon_scale.cc: stb_image decode, alpha-weighted box
   filter, stb_image_write with miniz -9 deflate; third_party/stb vendored). Pyanodon
   bundle regenerated with baked icon aliases: 41.9 -> 16.6 MB (icons 33.7 -> 8.3 MB),
   web/dist 46 -> 22 MB; 0 missing icons, all icons square <=32px, browser-verified.
   Format study (sharp/libvips over all 4899 icons): pngcrush-class recompress only
   -6..9% (stb+miniz already competitive); WebP lossless -23%; WebP lossy q90 -48%
   (~4 MB); AVIF q70 ~-41% (worse than WebP at 32px, plus Safari<16 concerns).
   Verdict: skip libwebp integration for now. Locale files are 6.33 MB compressed
   (26.3 MB raw, 55 languages) = ~38% of the bundle; splitting/lazy-loading them was
   considered and DECLINED 2026-07-03 (user: "keep the compressed locale data to
   simplify") — the bundle stays a single self-contained file at ~16.6 MB.
   Increment 7 (2026-07-03): PRODUCT RENAMED TO "MANCOS" (user directive, ship
   cleanup): page titles/headers, localStorage keys (mancos:* with one-time yafc:*
   migration), provenance line on the landing panel. "yafc" stays wherever it means
   the upstream project/data models (comments, Yafc.Model paths, namespace yafc,
   cmake targets) and in file formats (.yafc, .yafcbundle). Undo/redo shipped
   (100-deep pages-state snapshots, Ctrl+Z/Ctrl+Shift+Z/Ctrl+Y + header buttons,
   per-bundle history, headless-Chrome verified).
   Increment 8 (2026-07-03): solver stress (user directive: nuclear sample, P-238 from
   spent uranium cells, vary over-production). Link algorithms exposed end-to-end:
   tableAddLink(tdn, perSec, algo 0/1/2), =/≥/≤ toggle on link pills (dashed border
   when loose), linkAlgos per page persisted + .yafc round-trip. Findings
   (scratchpad/stress_nuclear.mjs vs pyanodon): isotope separation ratios exact
   (1/min pu-238 -> 3.333/min seperation = 1/0.3); zero-sum recursive cycles (barrel
   pairs, purex waste-washing) get kDeadlockCandidate warnings while status stays Ok
   (desktop-like slack behavior); no numerical failures in any config; spent-fuel
   demand correctly drives reactor rate via the fuelResult product. Fixed from
   findings: barreling/voiding pseudo-recipes now rank below real production and are
   excluded from auto-pick (desktop DataUtils parity) — auto-pull was "producing"
   fluids by emptying their own barrels. Semantics note: an over-production link with
   NO consumer still reports unmatched (desktop parity) — pure surpluses should stay
   unlinked. Full PUREX auto-closure needs consumer-pull (washing loops recycle
   upward), queued with the dependency-explorer work.
   Increment 9 (2026-07-03): shared links now carry pack + milestones (user request), not
   just the raw .yafc project text. Share envelope is `{pack, milestones, project}`
   (pack = manifest id of the loaded bundle, null for a locally-loaded file) deflated+
   base64url as before; `initPacks()` decodes the link's pack id before the chooser
   renders and auto-fetches that pack instead of falling back to `mancos:lastPack`/manual
   click, and `loadProjectFromUrl()` applies the link's milestone set via `setMilestones`
   in place of the normal localStorage-restore-or-open-the-dialog first-load path. Legacy
   links (raw text, no envelope) still decode fine — `parseSharedPayload` falls back to
   treating non-JSON/no-`.project` payloads as the old format. Verified end-to-end with a
   CDP-driven headless Chromium smoke test (no chromium-cli/puppeteer installed in this
   sandbox — drove `~/.cache/puppeteer/chrome` directly over the DevTools Protocol via
   node's built-in `fetch`+`WebSocket`, clicking through the real UI since app.js is an ES
   module and its top-level bindings aren't reachable from `Runtime.evaluate`): fresh
   profile -> load pack -> set a demand + row + milestone -> Share (clipboard) -> clear
   storage -> open the captured link -> pack auto-loads, goal/row/milestones reappear,
   milestones dialog does not re-prompt.
   Increment 10 (2026-07-03): search results now sort milestone-reachable goods before
   locked ones (user request) — `GoodsRank` (mirrors the existing `RecipeRank` tiering)
   added to `searchGoods`; also fixed a latent truncate-then-never-sort bug in the same
   function (it used to stop scanning the database the instant the prefix-match bucket
   hit `limit`, so anything past that point in raw id order never got a chance regardless
   of relevance — now it collects every match, sorts, then truncates).
   Increment 11 (2026-07-03): fixed temperature-variant fluid links (user bug report:
   "silver plate from soot" only matched coke-oven-gas at exactly 100°, not the 250°/500°
   producers that should also satisfy a ">=100°" ingredient). Root cause was a porting
   gap, not a data/parser bug: the parser's temperature bucketing (`UpdateRecipeIngredientFluids`,
   already correct, matches upstream) always defaults `Ingredient::goods` to the coldest
   qualifying variant and lists the rest in `Ingredient::variants`, but nothing in the
   C++ port let a row bind to any variant OTHER than that default — upstream's
   `RecipeRow.variants`/`GetVariant` selection mechanism was never carried over (an
   already-noted PLAN gap). Ported: `RecipeRow::variants` (vector<Goods*>, matches
   `Ingredient::variants`'s type so it reuses the existing ref-list JSON Prop() overload
   for .yafc round-trip unchanged) + `RecipeRow::ResolveIngredient()` (upstream
   GetVariant: first pinned option present in `ingredient.variants`, else the parser's
   default), threaded into both `ProductionTable::Solve()`'s solver-ingredient resolution
   and `AddFlow`'s consumption summation — the two places that previously read
   `ingredient.goods` unconditionally. Web API: `tableAddRecipe` config gains a
   `"variants": [tdn,...]` field; `RecipeBrief`'s ingredient entries gain a `"variants"`
   list when more than one qualifying option exists (what a picker UI would enumerate);
   `tableSolve` row flows now report the resolved variant's tdn. Verified against the
   real Pyanodon coke-oven-gas chain (3 temperature variants, `Recipe.quench-ovengas`
   consumer): default still resolves to @100 (unchanged behavior), pinning `variants:
   ["Fluid.coke-oven-gas@500"]` makes the row consume @500 and a link to a @500 producer
   now actually solves and matches (previously impossible at any pin, since there was no
   pin). Native test suite green. Not yet done: no UI variant-picker in app.js (the
   mechanism is API-complete but nothing in the row-config dialog exposes it yet) and no
   equivalent variant selection for fuel temperature (row.fuel is chosen directly from
   entity->energy.fuels, a different mechanism, out of scope for this fix).
   Increment 12 (2026-07-04): mobile UX fixes (user directive) + full quality (Factorio 2.0)
   threading (user directive: "producing at quality"). Mobile: native `prompt()` doesn't
   reliably appear in installed/standalone-mode PWAs (the likely cause of the reported
   "can't set a demand on mobile" bug) — replaced with a shared `#amountDialog` (number
   input + optional quality `<select>`) used by both "Set demand" and "Pin rate", with a
   dedicated Unpin action; Goals strip splits into "Main products" / "Inputs to consume"
   sections. Quality: previously only entity/building quality existed (`ObjectWithQuality<
   EntityCrafter>`, always Normal from the web layer); `QualityGoods`/links were wired for
   it but products/ingredients always resolved at `nullptr` ("no quality dimension (yet)").
   Ported upstream's `BuildProducts` upgrade-chance math (`ProductionTableContent.cs`):
   `RecipeRow` gains a target/floor `quality` (nullptr = unset, resolved to
   `ProductionSettings::qualityNormal` — a new caller-injected field, since the model core
   has no Database back-reference — by `RecipeParameters::Calculate`, so untouched callers
   see zero behavior change). Ingredients consume at that resolved quality (forced to
   Normal for goods that don't accept it — new `Goods::AcceptsQuality()`, Item-only, out-
   of-line for the forward-declaration); products spread across quality tiers via
   `Quality::UpgradeChance` compounding + the module `Quality` effect
   (`ModuleEffects::qualityMod()`, already computed, just never consumed before), shared
   by `Solve()` (LP inputs), `AddFlow` (flow reporting) and a new `RecipeRow::DisplayFlows()`
   (also fixes a latent bug: the web layer's per-row nameplate flows were recomputed
   straight from `recipe->products` and ignored productivity entirely). Milestone-gating
   the top of the quality chain (upstream `Quality.MaxAccessible`) is NOT ported — the
   walk uses every quality tier the loaded data defines, not just currently-reachable ones;
   a real gap but low-impact (over-inclusive, not incorrect) and flagged for a follow-up.
   Serialization: quality-wrapped refs (desktop `"!target!quality"`, quality by bare
   `name` not typeDotName) for `QualityGoods`/`ObjectWithQuality<T>` in both directions;
   `RecipeRow.recipe`'s "recipe" property is bridged through a temporary `ObjectWithQuality
   <RecipeOrTechnology>` so upstream's actual shape (recipe+quality as one wrapped ref) is
   preserved without changing this port's separate recipe/quality fields; an unresolvable
   quality name degrades leniently (nullptr, i.e. Normal) rather than a load error. Web API:
   `qualityList()` (level-ordered, milestone-gated like recipes/goods) alongside
   `tableAddLink`'s new required qualityTdn arg, `tableAddRecipe`'s config `"quality"`,
   `tableSolve`'s row/flow/link quality briefs, `projectSaveAll`/`projectLoad` quality
   round-trip for goals+rows; goods that can't accept quality are clamped to Normal
   server-side (not just client-gated) so a mismatched/dead link can't be created. UI:
   quality `<select>` in the amount dialog (goal-only, hidden for Fluid/Special goods and
   whenever the loaded pack has no real non-Normal tier — see below), a quality button
   row in the row-config dialog (no favorites, unlike building/fuel), small icon badges
   (reusing `iconImg` — Quality objects get bundler-extracted icons like any other
   FactorioObject) on goal pills/plate names/flow chips/link pills, omitted entirely at
   Normal to keep the common case unchanged. Off-table "produce/consume/link only" actions
   stay Normal-only for now (the `linked` list has no quality dimension) — a non-Normal
   surplus/import still displays (with its badge) but isn't actionable from that panel;
   flagged as a follow-up alongside MaxAccessible gating. Discovered mid-session: Pyanodon's
   Alien Life (the only previously-available local bundle) disables the quality feature
   entirely at the game-data level (common for overhaul packs) — the Database then only
   carries Normal + Factorio's internal always-inaccessible "quality-unknown" sentinel;
   added `hasSelectableQualities()` gating so the picker/section don't appear at all in
   that case instead of offering a dead-end option. Verified: 2 new production-table tests
   (module-upgrade-chance spread across 3 synthetic tiers incl. `DisplayFlows` parity; a
   fixed non-Normal floor with no modules produces 100% at that exact tier) + 1
   serialization round-trip test, native + wasm/node suites green throughout. Built a local
   vanilla bundle (`mancos_bundler <data> vanilla <env> out.yafcbundle`, real Uncommon/
   Rare/Epic/Legendary tiers with correct milestone gating) since the checked-in bundle
   has quality disabled; CDP-driven headless-Chromium pass against it: quality picker
   renders in both dialogs, a non-Normal goal and non-Normal row-quality selection each
   solve and show their badge, no console errors. Not yet done: MaxAccessible milestone-
   gating of the upgrade-chance walk; quality dimension on the `linked` (off-table link-
   only) list; fuel-quality picker (fuel defaults to Normal, unchanged from before).
   Increment 13 (2026-07-04): individual projects + per-project settings (user directive:
   quality recycling loops need per-project productivity research levels; "mining
   productivity as well"; reference output: recycling LDS for higher-quality plastic).
   FOUND AND FIXED a real quality-math bug while verifying against the hosted
   factorio_2.1_SpaceAge_Quality bundle: **Factorio 2.1 changed the quality prototype
   format** — module quality effects are now real fractions (0.025, was 10x-display 0.25
   in 2.0), next_probability is 1.0 (was 0.1), and tier-to-tier chaining moved to a NEW
   chain_probability field (0.1) on the reached tier. The 2.0-era math (multiply
   next_probability all the way up — upstream yafc-ce's current model too) makes every
   upgraded item chain straight to Legendary at 100% on 2.1 data. Fix: Quality gains
   ChainProbability (parser: chain_probability, falling back to next_probability = the
   2.0 chaining rule; dump format writes it, reader tolerates old bundles via
   e.value() fallback), QualityDistribution multiplies UpgradeChance for the FIRST step
   and the reached tier's ChainProbability for later steps — correct for both eras.
   ***The hosted factorio_2.1_SpaceAge_Quality.yafcbundle (mancos-data) must be REBUILT
   with the fixed bundler*** — it baked next_probability=1 with no chain info, so quality
   spreads stay wrong on the old file (loads fine otherwise). Upstream yafc-ce likely has
   the same 2.1 bug (worth an upstream report). Per-project settings: ProjectSettings
   gains miningProductivity/researchProductivity/productivityTechnologyLevels (upstream
   names, .yafc round-trip incl. dictionary-keyed-by-typeDotName serialization + test);
   web API tableSetSettings (re-sent per rebuild like the filler) + productivityOptions
   (techs with changeRecipeProductivity, milestone-gated briefs); projectSaveAll/Load
   carry {pages, settings} (old bare-array shape still accepted). UI: app.js state is now
   projects[{name, pages, activePage, settings}] with activeProject (localStorage
   migration from both older shapes; history/share/export operate on the active project;
   import replaces the active project only), project selector + rename/add/remove in the
   Factory bar, "⚙ Settings" dialog (mining/research % + per-tech level inputs, icons +
   lock badges, +N%/lv meta). Verified: LDS reference scenario on a locally-built vanilla
   bundle — 60/min uncommon plastic goal with 4x quality-module-3 recyclers = 533.33
   recycler crafts/min (exact analytic match: 1.25 plastic x 9% exactly-uncommon), LDS
   productivity 3 drops the LDS crafting row 533->410 crafts/min and copper imports
   8267->5805/min; per-project settings isolation confirmed in a CDP browser pass
   (project 1 keeps 0/0 while project 2 holds mining 50% + LDS level 3); native+wasm
   suites green. Also fixed en route: tableSolve row flows briefly emitted true
   per-minute values under the "perMin" key (all other endpoints emit per-second under
   that historical name; app.js scales) — reverted to per-second for consistency.
   Increment 14 (2026-07-04): blueprint export (user directive: auto-generate blueprints
   with SETS of the buildings with recipes placed). New core module
   src/yafc/model/blueprint.{h,cc}: game-format string encoding ("0" + base64(zlib(json)),
   upstream BlueprintString.ToBpString — miniz mz_compress2 emits the whole zlib stream
   incl. the 0x78 header and adler32 the C# code assembles by hand), blueprint JSON in
   the upstream Blueprint.cs shape (2.0 VERSION marker, entity_number/name/position/
   direction, recipe + recipe_quality, entity quality when non-Normal, burner fuel as a
   burner_fuel_inventory request filter, modules as item requests with in_inventory
   stacks into the right module inventory — mining-drill 2 / lab 3 / crafter 4, upstream
   BlueprintModuleInventory). Layout deliberately deviates from upstream's
   ExportRecipiesAsBlueprint (which places ONE sample building per recipe row, globally
   shelf-packed): each row places ceil(buildingCount) copies — a stampable block of the
   solved factory — in its own contiguous shelf sequence wrapping at a square-ish target
   width (sqrt of total area), 1-tile gaps, positions as proper entity centers (half-tile
   for odd sizes so top-lefts stay on the integer grid). Per-row cap (default 200,
   truncation reported) guards against thousand-building solves. Mechanics pseudo-recipes
   get no recipe field (upstream rule; note Mechanics DERIVES from Recipe, so the check
   must exclude it explicitly, not just dynamic_cast<Recipe>). Web API:
   tableExportBlueprint({label, includeFuel, maxPerRow}) on the solved table -> {blueprint,
   buildings, truncatedRows, width, height}. UI: "Blueprint" button by Clear page; copies
   to clipboard (prompt fallback), status line reports size + truncation; label =
   "<project> — <page>". Tests: 2 native/wasm cases (decode round-trip through miniz
   inflate, recipe/quality/fuel/module content, no-overlap geometry, cap + mechanics
   rules); e2e-verified against the real Py bundle under node (150-foundry lead-plate
   block, decoded with node's OWN zlib to prove format validity independent of miniz)
   and in headless Chromium (button -> clipboard holds "0eNq..." string). Py bundle
   rebuilt with the current bundler (data/ + web/dist + mancos-data all updated;
   14.9->16.6 MB, chainProbability baked). NOT rebuilt: the two factorio_2.1_* hosted
   bundles (need 2.1 game files, not on this machine) — still carry the broken quality
   chain (Increment 13); other 2.0-era hosted bundles are fine without rebuild via the
   tolerant dump reader. Not done: beacons are not placed in the blueprint (row beacon
   configs are ignored by the exporter); no belts/inserters/power poles (pure building
   blocks); no per-row selection UI (whole page only).
   Increment 15 (2026-07-05): building-quality picker ("Building quality" section in the
   row dialog, +30%/level speed via the pre-existing CraftingSpeed(quality) math the web
   layer never fed; .yafc quality-wrapped entity refs; badge on the entity chip); quality
   affordances fully hidden on quality-disabled packs (hasSelectableQualities gating
   audited in-browser on Py); OG/twitter meta on index.html; opt-in self-updater in the
   node bundler launcher (release workflow stamps the tag; prompt-only on TTY, tar/unzip
   extraction, --no-update-check + MANCOS_BUNDLER_NO_UPDATE=1 opt-outs — verified against
   the live v0.1.3 release, needs a new tag to go live); pack name shown next to Switch
   pack; per-project hideUnreachable display setting (filters 🚫 from search/recipe
   lists/quality pickers/prod-tech list; 🔒 stays). USER BUG (screenshots, hosted 2.1
   pack): quality spread showed only Normal+Legendary and a tiered goal never matched —
   the predicted pre-fix-bundle failure from Increment 13. Fixed WITHOUT bundle rebuilds:
   the dump reader reconstructs ChainProbability for old bundles from upgradeChance's
   shape (~0.1 → 2.0 rule: chain = upgradeChance; 1.0 → old-bundler 2.1 data: chain =
   upgradeChance x 0.1, the engine's documented default; 0.5 threshold separates the
   eras). Plus quality-aware flow linking: `linked` entries (and linkAlgos keys) carry
   "Item.x!Quality.y" for non-Normal tiers, produce/consume/link-only now work on tiered
   surpluses, and auto-pulled rows inherit the flow's tier as recipe quality; projectSave/
   Load round-trips the suffixed entries. Verified the user's exact scenario end-to-end
   in-browser on the unmodified hosted-format bundle: 300/min uncommon PSP + 4x q3
   modules -> 1111 crafts/min, five-tier spread, goal matched, tiered surpluses
   actionable. Caveat: the era heuristic can misjudge exotic modded quality chains in OLD
   bundles only (new bundles carry chainProbability explicitly).
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

## Wasm EH migration (note 2026-07-03)

Browsers deprecate the legacy 'try' EH encoding in favor of try_table. Migrating requires
the NEW encoding consistently across every object: rebuild or-tools/lua/miniz archives and
all targets with -sWASM_LEGACY_EXCEPTIONS=0 at compile AND link (engines hard-reject mixed
modules; node 22 additionally needs --experimental-wasm-exnref). Until then the web module
stays fully legacy — one cosmetic deprecation notice in Firefox.

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
