# Static Lua 5.2.1 with Factorio patches (deterministic pairs() ordering),
# fetched + patched into third_party/lua-5.2.1 by scripts/bootstrap.sh.

set(LUA_ROOT "${CMAKE_SOURCE_DIR}/third_party/lua-5.2.1")
if(NOT EXISTS "${LUA_ROOT}/src/lua.h")
  message(FATAL_ERROR
    "Missing ${LUA_ROOT} — run scripts/bootstrap.sh first (downloads lua 5.2.1 "
    "from lua.org and applies third_party/yafc-ce/lua/lua-5.2.1.patch).")
endif()

file(GLOB LUA_SOURCES "${LUA_ROOT}/src/*.c")
list(REMOVE_ITEM LUA_SOURCES "${LUA_ROOT}/src/lua.c" "${LUA_ROOT}/src/luac.c")

add_library(lua52 STATIC ${LUA_SOURCES})
add_library(lua::lua ALIAS lua52)
target_include_directories(lua52 SYSTEM PUBLIC "${LUA_ROOT}/src")
if(NOT EMSCRIPTEN)
  # POSIX APIs for os/io libs on desktop; wasm uses plain ANSI C paths.
  target_compile_definitions(lua52 PRIVATE LUA_USE_POSIX)
endif()
