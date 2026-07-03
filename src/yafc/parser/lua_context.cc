#include "yafc/parser/lua_context.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "yafc/parser/factorio_data_source.h"

namespace yafc {

namespace {

std::string ReadFileOrThrow(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Strip a UTF-8 BOM (upstream CleanupBom).
std::string_view CleanupBom(std::string_view chunk) {
  if (chunk.size() >= 3 && static_cast<unsigned char>(chunk[0]) == 0xEF &&
      static_cast<unsigned char>(chunk[1]) == 0xBB &&
      static_cast<unsigned char>(chunk[2]) == 0xBF) {
    return chunk.substr(3);
  }
  return chunk;
}

LuaContext* Self(lua_State* L) {
  return static_cast<LuaContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// "[string "12 core/lualib/util.lua"]:34:..." -> 12 (chunk id), or -1.
int ParseTracebackEntry(const std::string& s, size_t* endOfName) {
  constexpr std::string_view kPrefix = "[string \"";
  if (s.rfind(kPrefix, 0) == 0) {
    size_t endOfNum = s.find(' ', kPrefix.size());
    size_t nameEnd = s.find("\"]:", kPrefix.size());
    if (endOfNum != std::string::npos && nameEnd != std::string::npos &&
        endOfNum < nameEnd) {
      if (endOfName != nullptr) *endOfName = nameEnd + 2;
      try {
        return std::stoi(s.substr(kPrefix.size(), endOfNum - kPrefix.size()));
      } catch (...) {
        return -1;
      }
    }
  }
  return -1;
}

std::string GetDirectoryName(const std::string& s) {
  size_t lastSlash = s.rfind('/');
  return lastSlash == std::string::npos ? "" : s.substr(0, lastSlash + 1);
}

}  // namespace

// Static trampolines; `this` rides in upvalue 1.
struct LuaCallbacks {
  static int RawLog(lua_State* L) {
    size_t len = 0;
    const char* s = lua_tolstring(L, 1, &len);
    if (s != nullptr) fprintf(stderr, "[lua] %.*s\n", static_cast<int>(len), s);
    return 0;
  }

  static int Require(lua_State* L) {
    // C++ exceptions must not unwind through Lua's C frames; convert to a
    // Lua error (longjmp), which the surrounding pcall handles.
    try {
      return Self(L)->RequireImpl();
    } catch (const std::exception& e) {
      lua_pushstring(L, e.what());
    }
    return lua_error(L);
  }

  static int Traceback(lua_State* L) {
    LuaContext* self = Self(L);
    const char* message = lua_tostring(L, 1);
    luaL_traceback(L, L, message, 0);
    std::string fixed = self->ReplaceChunkIdsInTraceback(self->GetString(-1));
    lua_pushlstring(L, fixed.data(), fixed.size());
    return 1;
  }

  static int DebugTraceback(lua_State* L) {
    LuaContext* self = Self(L);
    luaL_traceback(L, L, nullptr, 0);
    std::string fixed = self->ReplaceChunkIdsInTraceback(self->GetString(-1));
    lua_pushlstring(L, fixed.data(), fixed.size());
    return 1;
  }

  static int SourceFixups(lua_State* L) {
    LuaContext* self = Self(L);
    const char* name = lua_tostring(L, 2);
    if (name != nullptr) {
      size_t end = 0;
      // Upstream regex "^[0-9]+ ": leading chunk id.
      std::string s(name);
      while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) end++;
      if (end > 0 && end < s.size() && s[end] == ' ') {
        int chunkId = std::stoi(s.substr(0, end));
        if (chunkId >= 0 && chunkId < static_cast<int>(self->fullChunkNames_.size())) {
          const auto& [mod, file] = self->fullChunkNames_[chunkId];
          std::string fixed = "__" + mod + "__/" + file;
          lua_pushlstring(L, fixed.data(), fixed.size());
          std::string at = "@" + fixed;
          lua_pushlstring(L, at.data(), at.size());
          return 2;
        }
      }
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    return 2;
  }

  // The %e exponent shim is Windows-only behavior; Sandbox.lua never installs
  // it on runtimes with two-digit exponents. Pass arguments through unchanged.
  static int StringFormat(lua_State* L) { return lua_gettop(L) - 1; }

  static int CompareVersions(lua_State* L) {
    const char* a = lua_tostring(L, 1);
    const char* b = lua_tostring(L, 2);
    Version va = Version::Parse(a != nullptr ? a : "");
    Version vb = Version::Parse(b != nullptr ? b : "");
    lua_pushnumber(L, va < vb ? -1 : va == vb ? 0 : 1);
    return 1;
  }

  static int EvaluateExpression(lua_State* L) {
    const char* expression = lua_tostring(L, 1);
    if (expression == nullptr) {
      lua_pushnumber(L, 0);
      return 1;
    }
    bool hasVariables = lua_istable(L, 2);
    double value = LuaContext::EvaluateExpression(
        expression, [&](const std::string& name) -> std::optional<double> {
          if (!hasVariables) return std::nullopt;
          lua_getfield(L, 2, name.c_str());
          std::optional<double> result;
          if (lua_isnumber(L, -1)) result = lua_tonumber(L, -1);
          lua_pop(L, 1);
          return result;
        });
    lua_pushnumber(L, value);
    return 1;
  }

  static int ParseEnergy(lua_State* L) {
    const char* value = lua_tostring(L, 1);
    lua_pushnumber(L, LuaContext::ParseEnergy(value != nullptr ? value : ""));
    return 1;
  }
};

LuaContext::LuaContext(ModSet& mods, std::string envPath)
    : mods_(mods), envPath_(std::move(envPath)) {
  L_ = luaL_newstate();
  luaL_openlibs(L_);

  // helpers.game_version + empty yafc table.
  lua_createtable(L_, 0, 1);
  lua_setglobal(L_, "helpers");
  lua_createtable(L_, 0, 0);
  lua_setglobal(L_, "yafc");

  RegisterApi(&LuaCallbacks::RawLog, "raw_log");
  RegisterApi(&LuaCallbacks::Require, "require");
  RegisterApi(&LuaCallbacks::SourceFixups, "yafc_sourcefixups");
  RegisterApi(&LuaCallbacks::StringFormat, "yafc_format");
  RegisterApi(&LuaCallbacks::DebugTraceback, "debug", "traceback");
  RegisterApi(&LuaCallbacks::CompareVersions, "helpers", "compare_versions");
  RegisterApi(&LuaCallbacks::EvaluateExpression, "helpers", "evaluate_expression");
  RegisterApi(&LuaCallbacks::ParseEnergy, "yafc", "parse_energy");
  RegisterApi(&LuaCallbacks::ParseEnergy, "yafc", "parse_power");

  // Sandbox.lua stamps data.data_crawler with this (upstream: yafc version).
  lua_pushstring(L_, "0.1.0-web");
  lua_setglobal(L_, "yafc_version");

  // Traceback message handler for pcall.
  lua_pushlightuserdata(L_, this);
  lua_pushcclosure(L_, &LuaCallbacks::Traceback, 1);
  tracebackReg_ = luaL_ref(L_, LUA_REGISTRYINDEX);
}

LuaContext::~LuaContext() {
  if (L_ != nullptr) lua_close(L_);
}

void LuaContext::RegisterApi(int (*fn)(lua_State*), const char* name) {
  lua_pushlightuserdata(L_, this);
  lua_pushcclosure(L_, fn, 1);
  lua_setglobal(L_, name);
}

void LuaContext::RegisterApi(int (*fn)(lua_State*), const char* topLevel,
                             const char* name) {
  lua_getglobal(L_, topLevel);
  lua_pushstring(L_, name);
  lua_pushlightuserdata(L_, this);
  lua_pushcclosure(L_, fn, 1);
  lua_rawset(L_, -3);
  lua_pop(L_, 1);
}

std::string LuaContext::GetString(int index) {
  size_t len = 0;
  const char* s = lua_tolstring(L_, index, &len);
  return s != nullptr ? std::string(s, len) : std::string();
}

std::string LuaContext::ReplaceChunkIdsInTraceback(const std::string& raw) {
  std::string result;
  size_t pos = 0;
  while (pos <= raw.size()) {
    size_t next = raw.find("\n\t", pos);
    std::string line = raw.substr(pos, next == std::string::npos ? next : next - pos);
    size_t endOfName = 0;
    int chunkId = ParseTracebackEntry(line, &endOfName);
    if (chunkId >= 0 && chunkId < static_cast<int>(fullChunkNames_.size())) {
      const auto& [mod, name] = fullChunkNames_[chunkId];
      line = "__" + mod + "__/" + name + line.substr(endOfName);
    }
    if (!result.empty()) result += "\n\t";
    result += line;
    if (next == std::string::npos) break;
    pos = next + 2;
  }
  return result;
}

int LuaContext::Exec(const std::string& chunk, const std::string& mod,
                     const std::string& name, int argumentRef) {
  fullChunkNames_.emplace_back(mod, name);
  std::string chunkName = std::to_string(fullChunkNames_.size() - 1) + " " + name;

  lua_rawgeti(L_, LUA_REGISTRYINDEX, tracebackReg_);  // message handler
  std::string_view body = CleanupBom(chunk);

  if (luaL_loadbufferx(L_, body.data(), body.size(), chunkName.c_str(), nullptr) !=
      LUA_OK) {
    std::string error = GetString(-1);
    lua_pop(L_, 2);
    throw std::runtime_error("lua load error in " + mod + "/" + name + ": " + error);
  }

  int argcount = 0;
  if (argumentRef > 0) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, argumentRef);
    argcount = 1;
  }

  if (lua_pcall(L_, argcount, 1, -2 - argcount) != LUA_OK) {
    std::string error = GetString(-1);
    lua_pop(L_, 2);
    throw std::runtime_error("lua error in " + mod + "/" + name + ":\n" + error);
  }
  int result = luaL_ref(L_, LUA_REGISTRYINDEX);
  lua_pop(L_, 1);  // message handler
  return result;
}

int LuaContext::ExecEnvFile(const std::string& fileName, const std::string& name,
                            int argumentRef) {
  return Exec(ReadFileOrThrow(envPath_ + "/" + fileName), "*", name, argumentRef);
}

int LuaContext::RequireImpl() {
  lua_State* L = L_;
  const char* rawFile = lua_tostring(L, 1);
  if (rawFile == nullptr) throw std::runtime_error("cannot require(nil)");
  std::string argument = rawFile;
  std::string file = argument;

  if (file.find("..") != std::string::npos) {
    throw std::runtime_error("attempt to traverse to parent directory");
  }
  if (file.size() >= 4 && file.compare(file.size() - 4, 4, ".lua") == 0) {
    file = file.substr(0, file.size() - 4);
  }
  for (char& c : file) {
    if (c == '\\') c = '/';
  }
  std::string originalFile = file;
  for (char& c : file) {
    if (c == '.') c = '/';
  }
  std::string fileExt = file + ".lua";
  lua_settop(L, 0);

  // Find the calling chunk to determine the current mod + directory.
  luaL_traceback(L, L, nullptr, 1);
  std::string traceback = GetString(-1);
  lua_pop(L, 1);
  int traceId = -1;
  size_t pos = 0;
  while (pos <= traceback.size()) {
    size_t next = traceback.find("\n\t", pos);
    std::string line =
        traceback.substr(pos, next == std::string::npos ? next : next - pos);
    traceId = ParseTracebackEntry(line, nullptr);
    if (traceId >= 0) break;
    if (next == std::string::npos) break;
    pos = next + 2;
  }
  if (traceId < 0 || traceId >= static_cast<int>(fullChunkNames_.size())) {
    throw std::runtime_error("require: cannot determine calling chunk");
  }
  const auto& [mod, source] = fullChunkNames_[traceId];
  std::pair<std::string, std::string> requiredFile{mod, fileExt};

  if (file.rfind("__", 0) == 0) {
    requiredFile = mods_.ResolveModPath(mod, originalFile, true);
  } else if (mod == "*") {
    int result = Exec(ReadFileOrThrow(envPath_ + "/" + fileExt), "*", file);
    lua_rawgeti(L, LUA_REGISTRYINDEX, result);
    return 1;
  } else if (mods_.ModPathExists(requiredFile.first,
                                 GetDirectoryName(source) + fileExt)) {
    requiredFile.second = GetDirectoryName(source) + fileExt;
  } else if (mods_.ModPathExists(requiredFile.first, fileExt)) {
    // keep as-is
  } else if (mods_.ModPathExists("core", "lualib/" + fileExt)) {
    requiredFile.first = "core";
    requiredFile.second = "lualib/" + fileExt;
  } else {
    // Just find anything (upstream fallback).
    for (const std::string& path :
         mods_.GetAllModFiles(requiredFile.first, GetDirectoryName(source))) {
      if (path.size() >= fileExt.size() &&
          path.compare(path.size() - fileExt.size(), fileExt.size(), fileExt) == 0) {
        requiredFile.second = path;
        break;
      }
    }
  }

  if (auto it = required_.find(requiredFile); it != required_.end()) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
    return 1;
  }

  std::string bytes = mods_.ReadModFile(requiredFile.first, requiredFile.second);
  if (!bytes.empty()) {
    lua_pushlstring(L, argument.data(), argument.size());
    int argumentReg = luaL_ref(L, LUA_REGISTRYINDEX);
    int result = Exec(bytes, requiredFile.first, requiredFile.second, argumentReg);
    required_[requiredFile] = result;
    lua_rawgeti(L, LUA_REGISTRYINDEX, result);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

void LuaContext::DoModFiles(const std::vector<std::string>& modOrder,
                            const std::string& fileName,
                            const std::function<void(const std::string&)>& progress) {
  for (const std::string& mod : modOrder) {
    required_.clear();
    mods_.currentLoadingMod = mod;
    if (progress) progress(mod + "/" + fileName);
    std::string bytes = mods_.ReadModFile(mod, fileName);
    if (bytes.empty()) continue;
    Exec(bytes, mod, fileName);
  }
  mods_.currentLoadingMod.clear();
}

// ------------------------------------------------------------ table access ----

namespace {
LuaValue PopValue(lua_State* L) {
  LuaValue v;
  switch (lua_type(L, -1)) {
    case LUA_TBOOLEAN:
      v.kind = LuaValue::Kind::Boolean;
      v.boolean = lua_toboolean(L, -1) != 0;
      break;
    case LUA_TNUMBER:
      v.kind = LuaValue::Kind::Number;
      v.number = lua_tonumber(L, -1);
      break;
    case LUA_TSTRING: {
      v.kind = LuaValue::Kind::String;
      size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      v.string.assign(s, len);
      break;
    }
    case LUA_TTABLE:
      v.kind = LuaValue::Kind::Table;
      v.tableRef = luaL_ref(L, LUA_REGISTRYINDEX);  // pops
      return v;
  }
  lua_pop(L, 1);
  return v;
}
}  // namespace

LuaValue LuaContext::GetGlobal(const std::string& name) {
  lua_getglobal(L_, name.c_str());
  return PopValue(L_);
}

LuaValue LuaContext::GetField(int tableRef, const std::string& key) {
  lua_rawgeti(L_, LUA_REGISTRYINDEX, tableRef);
  lua_pushlstring(L_, key.data(), key.size());
  lua_rawget(L_, -2);
  LuaValue v = PopValue(L_);
  lua_pop(L_, 1);
  return v;
}

LuaValue LuaContext::GetIndex(int tableRef, int index) {
  lua_rawgeti(L_, LUA_REGISTRYINDEX, tableRef);
  lua_rawgeti(L_, -1, index);
  LuaValue v = PopValue(L_);
  lua_pop(L_, 1);
  return v;
}

std::vector<std::string> LuaContext::StringKeys(int tableRef) {
  std::vector<std::string> keys;
  lua_rawgeti(L_, LUA_REGISTRYINDEX, tableRef);
  lua_pushnil(L_);
  while (lua_next(L_, -2) != 0) {
    lua_pop(L_, 1);  // value
    if (lua_type(L_, -1) == LUA_TSTRING) keys.push_back(GetString(-1));
  }
  lua_pop(L_, 1);
  return keys;
}

void LuaContext::Unref(int ref) { luaL_unref(L_, LUA_REGISTRYINDEX, ref); }

int LuaContext::RawLen(int tableRef) {
  lua_rawgeti(L_, LUA_REGISTRYINDEX, tableRef);
  int len = static_cast<int>(lua_rawlen(L_, -1));
  lua_pop(L_, 1);
  return len;
}

void LuaContext::SetGlobalTable(
    const std::string& name,
    const std::vector<std::pair<std::string, LuaValue>>& entries) {
  lua_createtable(L_, 0, static_cast<int>(entries.size()));
  for (const auto& [key, value] : entries) {
    lua_pushlstring(L_, key.data(), key.size());
    switch (value.kind) {
      case LuaValue::Kind::Boolean: lua_pushboolean(L_, value.boolean); break;
      case LuaValue::Kind::Number: lua_pushnumber(L_, value.number); break;
      case LuaValue::Kind::String:
        lua_pushlstring(L_, value.string.data(), value.string.size());
        break;
      case LuaValue::Kind::Table:
        lua_rawgeti(L_, LUA_REGISTRYINDEX, value.tableRef);
        break;
      default: lua_pushnil(L_); break;
    }
    lua_rawset(L_, -3);
  }
  lua_setglobal(L_, name.c_str());
}

int LuaContext::NewTableRef() {
  lua_createtable(L_, 0, 0);
  return luaL_ref(L_, LUA_REGISTRYINDEX);
}

// ------------------------------------------------------------------ helpers ----

double LuaContext::ParseEnergy(const std::string& energy) {
  // Upstream ParseEnergyDouble: internally store energy in mega-units.
  if (energy.size() < 2) return 0;
  char mul = energy[energy.size() - 2];
  if (std::isalpha(static_cast<unsigned char>(mul))) {
    double base = std::atof(energy.substr(0, energy.size() - 2).c_str());
    switch (mul) {
      case 'k': case 'K': return base * 1e-3;
      case 'M': return base;
      case 'G': return base * 1e3;
      case 'T': return base * 1e6;
      case 'P': return base * 1e9;
      case 'E': return base * 1e12;
      case 'Z': return base * 1e15;
      case 'Y': return base * 1e18;
      case 'R': return base * 1e21;
      case 'Q': return base * 1e24;
    }
  }
  return std::atof(energy.substr(0, energy.size() - 1).c_str()) * 1e-6;
}

namespace {
// Recursive-descent parser: expr := term (± term)*; term := pow (*/% pow)*;
// pow := unary (^ pow)?; unary := ±unary | primary; primary := num|var|(expr).
struct ExprParser {
  const std::string& s;
  size_t pos = 0;
  const std::function<std::optional<double>(const std::string&)>& variable;
  bool failed = false;

  void SkipSpace() {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
  }
  bool Eat(char c) {
    SkipSpace();
    if (pos < s.size() && s[pos] == c) {
      pos++;
      return true;
    }
    return false;
  }
  double Expr() {
    double v = Term();
    while (true) {
      if (Eat('+')) v += Term();
      else if (Eat('-')) v -= Term();
      else return v;
    }
  }
  double Term() {
    double v = Pow();
    while (true) {
      if (Eat('*')) v *= Pow();
      else if (Eat('/')) v /= Pow();
      else if (Eat('%')) v = std::fmod(v, Pow());
      else return v;
    }
  }
  double Pow() {
    double v = Unary();
    if (Eat('^')) return std::pow(v, Pow());
    return v;
  }
  double Unary() {
    if (Eat('-')) return -Unary();
    if (Eat('+')) return Unary();
    return Primary();
  }
  double Primary() {
    SkipSpace();
    if (Eat('(')) {
      double v = Expr();
      if (!Eat(')')) failed = true;
      return v;
    }
    if (pos < s.size() &&
        (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.')) {
      size_t end = pos;
      char* out = nullptr;
      double v = std::strtod(s.c_str() + pos, &out);
      end = out - s.c_str();
      pos = end;
      return v;
    }
    if (pos < s.size() && (std::isalpha(static_cast<unsigned char>(s[pos])) ||
                           s[pos] == '_')) {
      size_t start = pos;
      while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) ||
                                s[pos] == '_')) {
        pos++;
      }
      std::string name = s.substr(start, pos - start);
      if (auto v = variable(name)) return *v;
      failed = true;
      return 0;
    }
    failed = true;
    return 0;
  }
};
}  // namespace

double LuaContext::EvaluateExpression(
    const std::string& expression,
    const std::function<std::optional<double>(const std::string&)>& variable) {
  ExprParser parser{.s = expression, .variable = variable};
  double result = parser.Expr();
  parser.SkipSpace();
  // Upstream returns 1 when it cannot parse; keep that quirk.
  if (parser.failed || parser.pos != expression.size()) return 1;
  return result;
}

}  // namespace yafc
