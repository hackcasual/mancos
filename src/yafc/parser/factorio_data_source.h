// Port of Yafc.Parser/FactorioDataSource.cs — mod discovery, version and
// dependency resolution, load ordering, mod file access, and the data-stage
// orchestration (Sandbox + Defines -> data.lua x3 -> Postprocess).
//
// This chunk reads mods from FOLDERS only (vanilla + unpacked mods); zipped
// mods (minizip) and mod-settings.dat (FactorioPropertyTree) are the next
// parser chunks. Locale files are not read (i18n lives in the web layer).
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "yafc/parser/lua_context.h"

namespace yafc {

struct Version {
  int major = 0, minor = 0, patch = 0;

  static Version Parse(const std::string& text);  // tolerant; missing parts = 0
  auto operator<=>(const Version&) const = default;
  std::string ToString2() const;  // "2.0" — selects the Defines file
  std::string ToString3() const;  // "2.0.28"
};

class ModInfo {
 public:
  static std::unique_ptr<ModInfo> FromFolder(const std::string& folder);

  std::string name;
  std::string folder;
  Version parsedVersion;
  Version parsedFactorioVersion{1, 1, 0};
  std::vector<std::string> dependencies;  // raw strings from info.json

  // Parsed dependency info (upstream ParseDependencies).
  std::vector<std::pair<std::string, bool>> parsedDependencies;  // (mod, optional)
  std::vector<std::string> incompatibilities;

  void ParseDependencies();
  bool ValidForFactorioVersion(const Version& factorioVersion) const;
  bool CheckDependencies(const std::map<std::string, ModInfo*>& allMods,
                         const std::vector<std::string>& modsToDisable) const;
  bool CanLoad(const std::vector<std::string>& notLoadedMods) const;
};

// The active mod set + file access, shared with LuaContext (upstream statics).
class ModSet {
 public:
  std::map<std::string, ModInfo*> mods;  // name -> info (owned elsewhere)
  std::string currentLoadingMod;

  // "__mod__/a/b" or "a.b" (lua require form) -> (mod, "a/b.lua")
  std::pair<std::string, std::string> ResolveModPath(const std::string& currentMod,
                                                     const std::string& fullPath,
                                                     bool isLuaRequire) const;
  bool ModPathExists(const std::string& mod, const std::string& path) const;
  // Missing mods/files read as empty (upstream tolerance for pcall'd requires).
  std::string ReadModFile(const std::string& mod, const std::string& path) const;
  std::vector<std::string> GetAllModFiles(const std::string& mod,
                                          const std::string& prefix) const;
};

struct ParseResult {
  std::unique_ptr<LuaContext> lua;  // data stage completed; data/defines ready
  Version factorioVersion;
  std::vector<std::string> modLoadOrder;
  ModSet* modSet = nullptr;  // owned by the DataSource
};

class FactorioDataSource {
 public:
  // factorioPath: the game's data/ folder (contains core and base).
  // modPath: mods/ folder with mod-list.json, or "" for vanilla-with-DLC.
  // envPath: yafc's Data/ directory (Sandbox.lua & friends).
  FactorioDataSource(std::string factorioPath, std::string modPath,
                     std::string envPath)
      : factorioPath_(std::move(factorioPath)), modPath_(std::move(modPath)),
        envPath_(std::move(envPath)) {}

  // Runs discovery + the full Lua data stage. Throws std::runtime_error on
  // unrecoverable problems (missing mods, circular dependencies, Lua errors).
  ParseResult Parse(const std::function<void(const std::string&)>& progress = {});

 private:
  void FindMods(const std::string& directory, std::vector<std::unique_ptr<ModInfo>>& out);

  std::string factorioPath_;
  std::string modPath_;
  std::string envPath_;
  std::vector<std::unique_ptr<ModInfo>> ownedMods_;
  ModSet modSet_;
};

}  // namespace yafc
