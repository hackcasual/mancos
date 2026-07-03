// Port of Yafc.Parser/LuaContext.cs — the Lua 5.2.1 environment that runs
// Factorio's data stage: sandboxing, mod-aware require(), the defines table,
// helpers (compare_versions, evaluate_expression), yafc.parse_energy, and the
// chunk-name bookkeeping that turns Lua tracebacks into __mod__/path form.
//
// Not ported yet: mod-fix hooks, the Windows %e printf shim (registered as a
// pass-through; the check in Sandbox.lua never activates it on our runtimes).
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace yafc {

class ModSet;

// Discriminated view of a Lua value pulled out of the state.
class LuaTableRef;
struct LuaValue {
  enum class Kind { Nil, Boolean, Number, String, Table };
  Kind kind = Kind::Nil;
  bool boolean = false;
  double number = 0;
  std::string string;
  int tableRef = 0;  // registry ref when kind == Table

  bool IsNil() const { return kind == Kind::Nil; }
};

class LuaContext {
 public:
  // envPath: directory with Sandbox.lua, Defines*.lua, Postprocess*.lua,
  // Serpent.lua (yafc-ce's Yafc/Data). mods must outlive the context.
  LuaContext(ModSet& mods, std::string envPath);
  ~LuaContext();
  LuaContext(const LuaContext&) = delete;
  LuaContext& operator=(const LuaContext&) = delete;

  // Loads + runs a chunk; returns a registry ref to its return value.
  // Throws std::runtime_error with a mod-pathed traceback on failure.
  int Exec(const std::string& chunk, const std::string& mod, const std::string& name,
           int argumentRef = 0);
  int ExecEnvFile(const std::string& fileName, const std::string& name,
                  int argumentRef = 0);

  // Runs fileName (data.lua, ...) for every mod in order; missing files skip.
  // The require cache resets per mod, like Factorio.
  void DoModFiles(const std::vector<std::string>& modOrder, const std::string& fileName,
                  const std::function<void(const std::string&)>& progress = {});

  // Table access for the deserializer/tests.
  LuaValue GetGlobal(const std::string& name);
  LuaValue GetField(int tableRef, const std::string& key);
  LuaValue GetIndex(int tableRef, int index);
  std::vector<std::string> StringKeys(int tableRef);
  void Unref(int ref);
  int RawLen(int tableRef);
  void SetGlobalTable(const std::string& name,
                      const std::vector<std::pair<std::string, LuaValue>>& entries);
  int NewTableRef();

  lua_State* raw() { return L_; }

  // "600kW" -> 0.0006 (internally megawatts/megajoules; upstream ParseEnergyDouble).
  static double ParseEnergy(const std::string& energy);
  // Minimal math-expression evaluator (upstream uses Roslyn; grammar: + - * /
  // % ^ unary, parentheses, numbers, variables resolved via lookup).
  static double EvaluateExpression(
      const std::string& expression,
      const std::function<std::optional<double>(const std::string&)>& variable);

 private:
  friend struct LuaCallbacks;

  void RegisterApi(int (*fn)(lua_State*), const char* name);
  void RegisterApi(int (*fn)(lua_State*), const char* topLevel, const char* name);
  std::string GetString(int index);
  std::string ReplaceChunkIdsInTraceback(const std::string& raw);
  int RequireImpl();

  lua_State* L_ = nullptr;
  ModSet& mods_;
  std::string envPath_;
  int tracebackReg_ = 0;
  std::vector<std::pair<std::string, std::string>> fullChunkNames_;  // (mod, file)
  std::map<std::pair<std::string, std::string>, int> required_;     // require cache
};

}  // namespace yafc
