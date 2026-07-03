cmake_minimum_required(VERSION 3.24)
project(yafc-web C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(EMSCRIPTEN)
  # Real C++ exceptions (native wasm EH, supported by all current engines)
  # for yafc's own code: the throw/catch boundary at the wasm-core API edge
  # (bad bundle files, bad game/mod folders picked by the user) must actually
  # unwind, or the whole module aborts instead of returning a JSON error.
  # Emscripten's default wasm codegen leaves try/catch as dead code otherwise.
  # Scoped to C++ only (miniz/lua are C) and safe alongside the prebuilt
  # or-tools/GLOP static libs: those are built without exceptions and never
  # throw, and yafc::LpSolver already talks to GLOP via status codes, not
  # exceptions, so no unwind ever needs to cross that boundary.
  # Also for C (Lua's setjmp/longjmp-based lua_pcall): -fwasm-exceptions
  # switches SUPPORT_LONGJMP to the matching wasm-EH-based mechanism too, so
  # every TU using setjmp/longjmp needs the same flag or the link fails with
  # undefined `emscripten_longjmp` (the old JS-based mechanism's symbol).
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions)
endif()

# Prebuilt or-tools GLOP (see scripts/bootstrap.sh + solver-wasm/README.md)
include(cmake/ortools_glop.cmake)

# JSON for .yafc project files (and later, data-stage interchange).
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Zip access for zipped mods (miniz: single-file, MIT, wasm-friendly).
FetchContent_Declare(
  miniz
  URL https://github.com/richgel999/miniz/archive/refs/tags/3.0.2.tar.gz
  URL_HASH SHA256=c4b4c25a4eb81883448ff8924e6dba95c800094a198dc9ce66a292ac2ef8e018
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniz)
# Patched Lua 5.2.1 (Factorio-compatible; fetched by scripts/bootstrap.sh)
include(cmake/lua.cmake)

add_subdirectory(src)

# The bundler CLI: filesystem-facing half of the split app (native only; the
# browser bundler page will reuse the same core through the wasm build).
if(NOT EMSCRIPTEN)
  add_executable(yafc_bundler src/tools/bundler_main.cc)
  target_link_libraries(yafc_bundler PRIVATE yafc_core)
endif()

# The web module: the wasm core of the main app (built by scripts/build-web.sh).
option(YAFC_WEB "Build the web (embind) module" OFF)
if(EMSCRIPTEN AND YAFC_WEB)
  add_executable(yafc_web src/web/web_api.cc)
  target_link_libraries(yafc_web PRIVATE yafc_core miniz)
  target_link_options(yafc_web PRIVATE
    --bind -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createYafcModule
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152
    # Resizable ArrayBuffers (default-on) break Firefox's TextDecoder on any
    # string crossing embind; use classic detach-on-grow memory instead.
    -sGROWABLE_ARRAYBUFFERS=0
    # NOTE: stay on legacy wasm EH ('try') for now — engines reject modules
    # mixing encodings, and flipping requires rebuilding the whole prebuilt
    # dep stack (or-tools/lua/miniz) with -sWASM_LEGACY_EXCEPTIONS=0 as a
    # COMPILE flag too. Firefox's deprecation notice is cosmetic. Tracked in
    # PLAN.md (packaging).
    -sEXPORTED_FUNCTIONS=_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=HEAPU8
    -sENVIRONMENT=web,worker,node)

  # The bundler's wasm core: same pipeline as yafc_bundler (CLI), exposed to
  # the browser bundler page (web/bundler.html) via WORKERFS-mounted
  # directories instead of real paths (split-app contract, PLAN Phase 4.5).
  # Runs worker-only: WORKERFS requires ENVIRONMENT_MAY_BE_WORKER, and the
  # page never runs the core on the main thread (threading directive).
  add_executable(yafc_bundler_web src/web/bundler_web_api.cc)
  target_link_libraries(yafc_bundler_web PRIVATE yafc_core)
  set(YAFC_ENV_DATA_DIR "${CMAKE_SOURCE_DIR}/third_party/yafc-ce/Yafc/Data")
  target_link_options(yafc_bundler_web PRIVATE
    --bind -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createYafcBundlerModule
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152
    -sGROWABLE_ARRAYBUFFERS=0
    # NOTE: stay on legacy wasm EH ('try') for now — engines reject modules
    # mixing encodings, and flipping requires rebuilding the whole prebuilt
    # dep stack (or-tools/lua/miniz) with -sWASM_LEGACY_EXCEPTIONS=0 as a
    # COMPILE flag too. Firefox's deprecation notice is cosmetic. Tracked in
    # PLAN.md (packaging).
    -sENVIRONMENT=worker
    -lworkerfs.js
    "SHELL:--pre-js ${CMAKE_SOURCE_DIR}/web/bundler_pre.js"
    # Only the Lua env files the data stage reads (envPath) — not yafc's own
    # UI assets (Icons/Digits/fonts) which the bundler never touches.
    # (CMake dedupes identical repeated flags in one option list, so each
    # --embed-file must be wrapped in SHELL: to stay a distinct atomic token.)
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/Sandbox.lua@/env/Sandbox.lua"
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/Serpent.lua@/env/Serpent.lua"
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/Postprocess.lua@/env/Postprocess.lua"
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/Postprocess1.1.lua@/env/Postprocess1.1.lua"
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/Defines1.1.lua@/env/Defines1.1.lua"
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/Defines2.0.lua@/env/Defines2.0.lua"
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/Defines2.1.lua@/env/Defines2.1.lua"
    "SHELL:--embed-file ${YAFC_ENV_DATA_DIR}/locale@/env/locale")
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
