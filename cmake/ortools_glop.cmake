# Imports the standalone-GLOP build of or-tools from third_party/or-tools as
# target ortools::glop. The build tree is produced by scripts/bootstrap.sh
# (wasm: build_wasm via emcmake, native: build_native) — recipe documented in
# solver-wasm/README.md.

set(ORTOOLS_ROOT "${CMAKE_SOURCE_DIR}/third_party/or-tools")
if(EMSCRIPTEN)
  set(ORTOOLS_BUILD "${ORTOOLS_ROOT}/build_wasm")
else()
  set(ORTOOLS_BUILD "${ORTOOLS_ROOT}/build_native")
endif()

if(NOT EXISTS "${ORTOOLS_BUILD}/lib/libglop.a")
  message(FATAL_ERROR
    "Missing ${ORTOOLS_BUILD}/lib/libglop.a — run scripts/bootstrap.sh first "
    "(builds or-tools GLOP for both native and wasm; see solver-wasm/README.md).")
endif()

file(GLOB ORTOOLS_ABSL_LIBS "${ORTOOLS_BUILD}/lib/libabsl_*.a")
set(ORTOOLS_GLOP_LIBS
  "${ORTOOLS_BUILD}/lib/libglop.a"
  "${ORTOOLS_BUILD}/lib/libprotobuf.a"
  "${ORTOOLS_BUILD}/lib/libutf8_validity.a"
  "${ORTOOLS_BUILD}/lib/libz.a"
  "${ORTOOLS_BUILD}/lib/libbz2_static.a"
  ${ORTOOLS_ABSL_LIBS})

add_library(ortools_glop INTERFACE)
add_library(ortools::glop ALIAS ortools_glop)
target_include_directories(ortools_glop SYSTEM INTERFACE
  "${ORTOOLS_ROOT}"
  "${ORTOOLS_BUILD}"
  "${ORTOOLS_BUILD}/_deps/absl-src"
  "${ORTOOLS_BUILD}/_deps/protobuf-src/src"
  "${ORTOOLS_BUILD}/_deps/protobuf-src/third_party/utf8_range")

if(EMSCRIPTEN)
  # wasm-ld resolves lazily across archives; no grouping needed.
  target_link_libraries(ortools_glop INTERFACE ${ORTOOLS_GLOP_LIBS})
else()
  # GNU ld is single-pass; absl archives have circular deps -> rescan group.
  list(JOIN ORTOOLS_GLOP_LIBS "," _glop_libs_joined)
  target_link_libraries(ortools_glop INTERFACE
    "$<LINK_GROUP:RESCAN,${_glop_libs_joined}>")
  find_package(Threads REQUIRED)
  target_link_libraries(ortools_glop INTERFACE Threads::Threads)
endif()
