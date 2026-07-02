#include "yafc/lua/lua_vm.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace yafc {

LuaVm::LuaVm() : state_(luaL_newstate()) { luaL_openlibs(state_); }

LuaVm::~LuaVm() { lua_close(state_); }

std::optional<std::string> LuaVm::Run(const std::string& script) {
  if (luaL_dostring(state_, script.c_str()) != LUA_OK) {
    last_error_ = lua_tostring(state_, -1);
    lua_pop(state_, 1);
    return last_error_;
  }
  return std::nullopt;
}

std::optional<std::string> LuaVm::EvalToString(const std::string& script) {
  const std::string chunk = "return (" + script + ")";
  if (luaL_dostring(state_, chunk.c_str()) != LUA_OK) {
    last_error_ = lua_tostring(state_, -1);
    lua_pop(state_, 1);
    return std::nullopt;
  }
  const char* s = lua_tostring(state_, -1);
  std::optional<std::string> result =
      s ? std::optional<std::string>(s) : std::nullopt;
  if (!s) last_error_ = "result not convertible to string";
  lua_pop(state_, 1);
  return result;
}

}  // namespace yafc
