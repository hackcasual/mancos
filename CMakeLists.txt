cmake_minimum_required(VERSION 3.24)
project(yafc-web C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
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
  target_link_libraries(yafc_web PRIVATE yafc_core)
  target_link_options(yafc_web PRIVATE
    --bind -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createYafcModule
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=2097152
    -sEXPORTED_FUNCTIONS=_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=HEAPU8
    -sENVIRONMENT=web,worker,node)
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
