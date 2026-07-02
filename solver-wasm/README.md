# solver-wasm — Google OR-Tools GLOP compiled to WebAssembly

Yafc-CE's solver dependency is Google OR-Tools' GLOP linear programming solver (native code —
no wasm build published by Google). This directory proves the wasm build of exactly the
functionality yafc uses, and hosts the tracer-bullet demo.

## Status: PROVEN (2026-07-02)

`demo/yafc_lp_demo.cc` solves a real modded production chain — 900 lead plates/min from lead
ore, with a gangue byproduct and a grade-3→grade-2 recycle loop — headless under node, using
yafc's actual two-pass algorithm (equality links → INFEASIBLE → cost-weighted slack on
infeasibility candidates → OPTIMAL). Output matches the desktop yafc reference run
(crafts/min: 1111.25 / 222.25 / 127 / 31.75 / 63.5 / 63.5 / 900; extra gangue 127/min).
Artifact size: ~2.0 MB wasm + 66 KB js at -O2 (before -Os/closure/brotli).

## What yafc needs from OR-Tools

Only GLOP LP solving (no CP-SAT/MIP; full API table in `.claude/PLAN.md`). The or-tools
**standalone GLOP** target (`BUILD_CXX=OFF, BUILD_GLOP=ON`) provides it all through
`glop::LPSolver` with a ~10x smaller dependency surface (glop + lp_data + abseil + protobuf +
zlib/bzip2). The C++ port uses this API directly — no SWIG/P-Invoke layer.

## Build recipe (reproduced 2026-07-02)

```bash
# 1. Toolchain: emsdk in third_party/emsdk (tested: emcc 6.0.2, node 22.16)
git clone --depth 1 https://github.com/emscripten-core/emsdk third_party/emsdk
third_party/emsdk/emsdk install latest && third_party/emsdk/emsdk activate latest
source third_party/emsdk/emsdk_env.sh

# 2. or-tools v9.15 (same version as yafc-ce's Google.OrTools NuGet)
git clone --depth 1 --branch v9.15 https://github.com/google/or-tools third_party/or-tools

# 3. Patch third_party/or-tools/cmake/dependencies/CMakeLists.txt — wasm wants static libs:
#      set(BUILD_SHARED_LIBS ON)        -> OFF   (top of file, applies to fetched deps)
#      set(protobuf_BUILD_SHARED_LIBS ON) -> OFF
#      bzip2 block: add set(ENABLE_SHARED_LIB OFF) + set(ENABLE_STATIC_LIB ON)
#      (bzip2 has its own switches and ignores BUILD_SHARED_LIBS)

# 4. Configure + build (host protoc builds automatically via cmake/host.cmake)
cd third_party/or-tools
emcmake cmake -S . -B build_wasm -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CXX=OFF -DBUILD_GLOP=ON -DBUILD_DEPS=ON \
  -DBUILD_SAMPLES=OFF -DBUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build build_wasm --target glop -j$(nproc)
cmake --build build_wasm --target bz2_static -j$(nproc)

# 5. Tracer demo: build + run under node (links against the build tree directly;
#    `cmake --install` is avoided — it requires building 'all', which trips -Werror
#    in the bundled benchmark dep)
./solver-wasm/demo/build-and-run.sh
```

## Notes

- or-tools v9.15 cross-compiles cleanly with emscripten; the patches from
  [or-tools discussion #2997](https://github.com/google/or-tools/discussions/2997)
  (glop fenv guards, raw_logging) are upstream by now — only the static-libs patch above
  remains.
- `glop::LPSolver` API deltas vs the C# `MPSolver` wrapper worth remembering for the port:
  statuses come from `variable_statuses()` / `constraint_statuses()` (no per-object
  `BasisStatus()`), values from `variable_values()` / `dual_values()`, parameters via a
  `GlopParameters` proto (text-format parse replicates
  `SetSolverSpecificParametersAsString`).
