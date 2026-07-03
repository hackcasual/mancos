#include "yafc/parser/factorio_data_source.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace fs = std::filesystem;

namespace yafc {

namespace {
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
}  // namespace

Version Version::Parse(const std::string& text) {
  Version v;
  int fields[3] = {0, 0, 0};
  int i = 0;
  size_t pos = 0;
  while (i < 3 && pos < text.size()) {
    size_t next = text.find('.', pos);
    try {
      fields[i] = std::stoi(text.substr(pos, next - pos));
    } catch (...) {
      break;
    }
    i++;
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  v.major = fields[0];
  v.minor = fields[1];
  v.patch = fields[2];
  return v;
}

std::string Version::ToString2() const {
  return std::to_string(major) + "." + std::to_string(minor);
}
std::string Version::ToString3() const {
  return ToString2() + "." + std::to_string(patch);
}

std::unique_ptr<ModInfo> ModInfo::FromFolder(const std::string& folder) {
  std::string json = ReadFile(folder + "/info.json");
  if (json.empty()) return nullptr;
  nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return nullptr;

  auto info = std::make_unique<ModInfo>();
  info->folder = folder;
  info->name = parsed.value("name", "");
  info->parsedVersion = Version::Parse(parsed.value("version", ""));
  info->parsedFactorioVersion =
      Version::Parse(parsed.value("factorio_version", "1.1"));
  if (parsed.contains("dependencies") && parsed["dependencies"].is_array()) {
    info->dependencies.clear();
    for (const auto& dep : parsed["dependencies"]) {
      if (dep.is_string()) info->dependencies.push_back(dep.get<std::string>());
    }
  } else {
    info->dependencies = {"base"};  // upstream defaultDependencies
  }
  return info;
}

void ModInfo::ParseDependencies() {
  // Upstream: ^\(?([?!~+]?)\)?\s*([\w- ]+?)(?:\s*[><=]+\s*[\d.]*)?\s*$
  static const std::regex kDependencyRegex(
      R"(^\(?([?!~+]?)\)?\s*([\w\- ]+?)(?:\s*[><=]+\s*[\d.]*)?\s*$)");
  parsedDependencies.clear();
  incompatibilities.clear();
  for (const std::string& dependency : dependencies) {
    std::smatch match;
    if (!std::regex_match(dependency, match, kDependencyRegex)) continue;
    std::string modifier = match[1].str();
    if (modifier == "!") {
      incompatibilities.push_back(match[2].str());
    } else if (modifier == "~") {
      // required, but does not affect load order
    } else {
      parsedDependencies.emplace_back(match[2].str(), modifier == "?");
    }
  }
}

bool ModInfo::ValidForFactorioVersion(const Version& factorioVersion) const {
  auto majorMinorEquals = [](const Version& a, const Version& b) {
    return a.major == b.major && a.minor == b.minor;
  };
  return majorMinorEquals(factorioVersion, parsedFactorioVersion) ||
         (majorMinorEquals(factorioVersion, {1, 0, 0}) &&
          majorMinorEquals(parsedFactorioVersion, {0, 18, 0})) ||
         name == "core";
}

bool ModInfo::CheckDependencies(const std::map<std::string, ModInfo*>& allMods,
                                const std::vector<std::string>& modsToDisable) const {
  for (const auto& [mod, optional] : parsedDependencies) {
    if (!optional && !allMods.count(mod)) return false;
  }
  for (const std::string& incompatibility : incompatibilities) {
    if (allMods.count(incompatibility) &&
        std::find(modsToDisable.begin(), modsToDisable.end(), incompatibility) ==
            modsToDisable.end()) {
      return false;
    }
  }
  return true;
}

bool ModInfo::CanLoad(const std::vector<std::string>& notLoadedMods) const {
  for (const auto& [mod, _] : parsedDependencies) {
    if (std::find(notLoadedMods.begin(), notLoadedMods.end(), mod) !=
        notLoadedMods.end()) {
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------------ ModSet ----

std::pair<std::string, std::string> ModSet::ResolveModPath(
    const std::string& currentMod, const std::string& fullPath,
    bool isLuaRequire) const {
  std::string mod = currentMod;
  bool useDotSplitters = isLuaRequire && fullPath.find('/') == std::string::npos;

  std::vector<std::string> parts;
  std::string current;
  for (char c : fullPath) {
    bool isSplitter = useDotSplitters ? (c == '.') : (c == '/' || c == '\\');
    if (isSplitter) {
      if (!current.empty()) parts.push_back(std::move(current));
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) parts.push_back(std::move(current));

  for (const std::string& part : parts) {
    if (part == "..") throw std::runtime_error("attempt to traverse to parent directory");
  }

  size_t start = 0;
  if (!parts.empty() && parts[0].size() > 4 && parts[0].rfind("__", 0) == 0 &&
      parts[0].compare(parts[0].size() - 2, 2, "__") == 0) {
    mod = parts[0].substr(2, parts[0].size() - 4);
    start = 1;
  }

  std::string resolved;
  for (size_t i = start; i < parts.size(); ++i) {
    if (!resolved.empty()) resolved += '/';
    resolved += parts[i];
  }
  if (isLuaRequire) resolved += ".lua";
  return {mod, resolved};
}

bool ModSet::ModPathExists(const std::string& mod, const std::string& path) const {
  auto it = mods.find(mod);
  if (it == mods.end()) return false;
  return fs::exists(fs::path(it->second->folder) / path);
}

std::string ModSet::ReadModFile(const std::string& mod, const std::string& path) const {
  auto it = mods.find(mod);
  if (it == mods.end()) return {};
  return ReadFile((fs::path(it->second->folder) / path).string());
}

std::vector<std::string> ModSet::GetAllModFiles(const std::string& mod,
                                                const std::string& prefix) const {
  std::vector<std::string> result;
  auto it = mods.find(mod);
  if (it == mods.end()) return result;
  fs::path base = fs::path(it->second->folder);
  fs::path dir = base / prefix;
  if (!fs::exists(dir)) return result;
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      result.push_back(fs::relative(entry.path(), base).generic_string());
    }
  }
  return result;
}

// ------------------------------------------------------------------- Parse ----

void FactorioDataSource::FindMods(const std::string& directory,
                                  std::vector<std::unique_ptr<ModInfo>>& out) {
  if (!fs::exists(directory)) return;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (!entry.is_directory()) continue;  // TODO(port): zipped mods (minizip)
    auto info = ModInfo::FromFolder(entry.path().string());
    if (info != nullptr) out.push_back(std::move(info));
  }
}

ParseResult FactorioDataSource::Parse(
    const std::function<void(const std::string&)>& progress) {
  auto report = [&](const std::string& s) {
    if (progress) progress(s);
  };

  // Mod list (or vanilla default).
  std::map<std::string, ModInfo*> enabled;  // name -> resolved info (null at first)
  bool hasModList = false;
  if (!modPath_.empty()) {
    std::string modListText = ReadFile((fs::path(modPath_) / "mod-list.json").string());
    if (!modListText.empty()) {
      nlohmann::json modList = nlohmann::json::parse(modListText, nullptr, false);
      if (!modList.is_discarded() && modList.contains("mods")) {
        hasModList = true;
        for (const auto& entry : modList["mods"]) {
          if (entry.value("enabled", false)) {
            enabled[entry.value("name", "")] = nullptr;
          }
        }
      }
    }
  }
  if (!hasModList) enabled["base"] = nullptr;
  enabled["core"] = nullptr;

  report("discovering mods");
  ownedMods_.clear();
  std::vector<std::unique_ptr<ModInfo>> found;
  FindMods(factorioPath_, found);
  if (!modPath_.empty() && modPath_ != factorioPath_) FindMods(modPath_, found);

  if (!hasModList) {
    // The game enables the Space Age DLC mods by default when present.
    bool foundSpaceAge =
        std::any_of(found.begin(), found.end(),
                    [](const auto& m) { return m->name == "space-age"; });
    if (foundSpaceAge) {
      enabled["space-age"] = nullptr;
      enabled["quality"] = nullptr;
      enabled["elevated-rails"] = nullptr;
    }
  }

  // Factorio version from the base mod.
  Version factorioVersion;
  for (const auto& mod : found) {
    if (mod->name == "base") {
      mod->parsedFactorioVersion = mod->parsedVersion;
      factorioVersion = std::max(factorioVersion, mod->parsedVersion);
    }
  }
  if (factorioVersion == Version{}) {
    throw std::runtime_error("could not read Factorio's base info.json");
  }
  if (factorioVersion < Version{1, 1, 0} || !(factorioVersion < Version{2, 2, 0})) {
    throw std::runtime_error("unsupported Factorio version " +
                             factorioVersion.ToString3());
  }

  // Pick the best candidate per enabled mod.
  for (auto& mod : found) {
    auto it = enabled.find(mod->name);
    if (it == enabled.end() || !mod->ValidForFactorioVersion(factorioVersion)) continue;
    ModInfo* existing = it->second;
    if (existing == nullptr || mod->parsedVersion > existing->parsedVersion) {
      it->second = mod.get();
    }
  }

  for (const auto& [name, info] : enabled) {
    if (info == nullptr) {
      throw std::runtime_error("mod not found: " + name);
    }
    info->ParseDependencies();
  }

  // Iteratively drop mods with unmet dependencies or incompatibilities.
  std::vector<std::string> modsToDisable;
  do {
    modsToDisable.clear();
    for (const auto& [name, info] : enabled) {
      if (!info->CheckDependencies(enabled, modsToDisable)) {
        modsToDisable.push_back(name);
      }
    }
    for (const std::string& name : modsToDisable) enabled.erase(name);
  } while (!modsToDisable.empty());

  // Load order: core first, then alphabetical batches of loadable mods.
  std::vector<std::string> modLoadOrder{"core"};
  std::vector<std::string> modsToLoad;
  for (const auto& [name, _] : enabled) {
    if (name != "core") modsToLoad.push_back(name);
  }
  std::sort(modsToLoad.begin(), modsToLoad.end(), [](const auto& a, const auto& b) {
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
          return std::tolower(static_cast<unsigned char>(x)) <
                 std::tolower(static_cast<unsigned char>(y));
        });
  });
  while (!modsToLoad.empty()) {
    std::vector<std::string> batch;
    for (const std::string& mod : modsToLoad) {
      if (enabled[mod]->CanLoad(modsToLoad)) batch.push_back(mod);
    }
    if (batch.empty()) {
      throw std::runtime_error("circular mod dependencies");
    }
    for (const std::string& mod : batch) {
      modLoadOrder.push_back(mod);
      modsToLoad.erase(std::find(modsToLoad.begin(), modsToLoad.end(), mod));
    }
  }

  // Prepare the shared mod set for the Lua context.
  modSet_.mods.clear();
  for (const auto& [name, info] : enabled) modSet_.mods[name] = info;
  // Keep ownership of every discovered mod alive for the session.
  for (auto& mod : found) ownedMods_.push_back(std::move(mod));

  report("creating lua context");
  ParseResult result;
  result.factorioVersion = factorioVersion;
  result.modLoadOrder = modLoadOrder;
  result.modSet = &modSet_;
  result.lua = std::make_unique<LuaContext>(modSet_, envPath_);
  LuaContext& lua = *result.lua;

  // Globals the sandbox expects: helpers.game_version, mods, feature_flags,
  // settings. (mod-settings.dat parsing is the next parser chunk; an empty
  // settings table matches a fresh vanilla install.)
  {
    lua_State* L = lua.raw();
    std::vector<std::pair<std::string, LuaValue>> modsTable;
    for (const auto& [name, info] : enabled) {
      LuaValue v;
      v.kind = LuaValue::Kind::String;
      v.string = info->parsedVersion.ToString3();
      modsTable.emplace_back(name, v);
    }
    lua.SetGlobalTable("mods", modsTable);

    // Feature flags follow the game: each Space Age feature is on when its
    // mod is enabled. (Upstream leaves these always-false with a TODO; that
    // contradicts Factorio's documented behavior, so we follow the game.)
    auto flag = [](bool on) {
      LuaValue v;
      v.kind = LuaValue::Kind::Boolean;
      v.boolean = on;
      return v;
    };
    bool spaceAge = enabled.count("space-age") != 0;
    lua.SetGlobalTable("feature_flags",
                       {{"quality", flag(enabled.count("quality") != 0)},
                        {"rail_bridges", flag(enabled.count("elevated-rails") != 0)},
                        {"space_travel", flag(spaceAge)},
                        {"spoiling", flag(spaceAge)},
                        {"freezing", flag(spaceAge)},
                        {"segmented_units", flag(spaceAge)},
                        {"expansion_shaders", flag(spaceAge)},
                        {"expansion", flag(spaceAge)}});

    // Empty settings table (mod-settings.dat parsing is the next chunk).
    lua_createtable(L, 0, 0);
    lua_setglobal(L, "settings");

    // helpers.game_version
    lua_getglobal(L, "helpers");
    lua_pushstring(L, "game_version");
    lua_pushstring(L, factorioVersion.ToString3().c_str());
    lua_rawset(L, -3);
    lua_pop(L, 1);
  }

  report("running data stage");
  int definesRef =
      lua.ExecEnvFile("Defines" + factorioVersion.ToString2() + ".lua", "defines");
  lua.ExecEnvFile("Sandbox.lua", "pre", definesRef);
  lua.DoModFiles(modLoadOrder, "data.lua", progress);
  lua.DoModFiles(modLoadOrder, "data-updates.lua", progress);
  lua.DoModFiles(modLoadOrder, "data-final-fixes.lua", progress);
  if (factorioVersion < Version{2, 0, 0}) {
    lua.ExecEnvFile("Postprocess1.1.lua", "post");
  }
  lua.ExecEnvFile("Postprocess.lua", "post");

  report("data stage complete");
  return result;
}

}  // namespace yafc
