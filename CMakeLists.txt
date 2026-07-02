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
# Patched Lua 5.2.1 (Factorio-compatible; fetched by scripts/bootstrap.sh)
include(cmake/lua.cmake)

add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
