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

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
