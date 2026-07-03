// Bundle format: the split-app contract (PLAN Phase 4.5). The BUNDLER (native
// CLI or a future browser page with filesystem access) is the only component
// that reads raw game files; it parses + deserializes and emits a single
// bundle. The MAIN APP only ever loads bundles.
//
// A bundle is a zip:
//   meta.json      {formatVersion, factorioVersion, mods{name:version}}
//   database.cbor  full Database dump (all objects, every field, refs by id)
//   icons.json     typeDotName -> [{file,size,x,y,r,g,b,a,scale}] layer lists
//   icons/<n>.png  deduped icon layer images pulled from the mods
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "yafc/model/database.h"

namespace yafc {

class ModSet;
struct Version;

inline constexpr int kBundleFormatVersion = 1;

// ---- database dump (usable standalone, e.g. for tests) ----
nlohmann::json DumpDatabase(const Database& db);
// Throws std::runtime_error on malformed/unsupported dumps.
std::unique_ptr<Database> LoadDatabase(const nlohmann::json& dump);

// ---- bundles ----
struct BundleWriteStats {
  size_t objects = 0;
  size_t iconFiles = 0;
  size_t iconBytes = 0;
  size_t missingIcons = 0;  // icon paths that could not be read from the mods
  size_t databaseBytes = 0;
};

// Extracts icons through mods and writes the complete bundle zip.
// costs: optional per-object yafc cost (by id, from CostAnalysis) -> costs.cbor.
// locales: optional raw catalogs per language -> locale/<lang>.json entries
// (the web app applies them client-side via ApplyLocale).
BundleWriteStats WriteBundle(const std::string& outPath, const Database& db,
                             const ModSet& mods, const std::string& factorioVersion,
                             const std::map<std::string, std::string>& modVersions,
                             const std::vector<float>* costs = nullptr,
                             const std::map<std::string,
                                            std::map<std::string, std::string>>*
                                 locales = nullptr);

struct Bundle {
  std::unique_ptr<Database> db;
  nlohmann::json meta;
  nlohmann::json iconManifest;                     // typeDotName -> layer list
  std::map<std::string, std::string> iconFiles;    // "icons/<n>.png" -> bytes
  std::vector<float> costs;                        // per-object yafc cost (may be empty)
  std::map<std::string, std::string> localeFiles;  // lang -> raw catalog json text
};

// Loads a bundle from a file or from memory (the web app's path).
Bundle ReadBundle(const std::string& path);
Bundle ReadBundleFromMemory(const std::string& bytes);

}  // namespace yafc
