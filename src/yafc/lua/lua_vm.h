// RAII wrapper over the Factorio-patched Lua 5.2.1 state. Grows into the
// LuaContext port (Phase 3): require() resolution across mods, defines table,
// settings stage. For the skeleton it proves the patched interpreter links and
// behaves (deterministic pairs()) on both native and wasm targets.
#pragma once

#include <optional>
#include <string>

struct lua_State;

namespace yafc {

class LuaVm {
 public:
  LuaVm();   // opens state + standard libraries
  ~LuaVm();
  LuaVm(const LuaVm&) = delete;
  LuaVm& operator=(const LuaVm&) = delete;

  // Runs `script`; returns the error message on failure.
  std::optional<std::string> Run(const std::string& script);

  // Runs `script` expecting a single return value, converted to string.
  // Returns std::nullopt on error (error message via last_error()).
  std::optional<std::string> EvalToString(const std::string& script);

  const std::string& last_error() const { return last_error_; }
  lua_State* raw() { return state_; }

 private:
  lua_State* state_;
  std::string last_error_;
};

}  // namespace yafc
