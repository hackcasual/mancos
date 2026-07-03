// Zipped-mod support: synthetic archive round-trip plus discovery over the
// user's real mod corpus when present (data/factorio/mods).
#include <filesystem>
#include <string>

#include <miniz.h>

#include "doctest/doctest.h"
#include "yafc/parser/factorio_data_source.h"

using namespace yafc;
namespace fs = std::filesystem;

namespace {

std::string WriteTestZip() {
  std::string path = (fs::temp_directory_path() / "yafc_testmod_1.2.3.zip").string();
  fs::remove(path);

  mz_zip_archive zip{};
  REQUIRE(mz_zip_writer_init_file(&zip, path.c_str(), 0));
  const char* info = R"({"name":"testmod","version":"1.2.3",
                         "factorio_version":"2.0","dependencies":["base","? optionalmod"]})";
  const char* data = "data:extend{{type='item', name='test-item'}}";
  const char* util = "return 42";
  REQUIRE(mz_zip_writer_add_mem(&zip, "testmod_1.2.3/info.json", info,
                                strlen(info), MZ_DEFAULT_COMPRESSION));
  REQUIRE(mz_zip_writer_add_mem(&zip, "testmod_1.2.3/data.lua", data,
                                strlen(data), MZ_DEFAULT_COMPRESSION));
  REQUIRE(mz_zip_writer_add_mem(&zip, "testmod_1.2.3/lib/util.lua", util,
                                strlen(util), MZ_DEFAULT_COMPRESSION));
  REQUIRE(mz_zip_writer_finalize_archive(&zip));
  mz_zip_writer_end(&zip);
  return path;
}

std::string FindRepoRootWithMods() {
  fs::path dir = fs::current_path();
  for (int i = 0; i < 6; ++i) {
    if (fs::exists(dir / "data/factorio/mods/mod-list.json")) return dir.string();
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  return {};
}

const std::string kModsRoot = FindRepoRootWithMods();

}  // namespace

TEST_CASE("zipped mod: info.json discovery and file access") {
  std::string zipPath = WriteTestZip();
  auto info = ModInfo::FromZip(zipPath);
  REQUIRE(info != nullptr);
  CHECK(info->name == "testmod");
  CHECK(info->parsedVersion == Version{1, 2, 3});
  CHECK(info->folder == "testmod_1.2.3/");

  info->ParseDependencies();
  REQUIRE(info->parsedDependencies.size() == 2);
  CHECK(info->parsedDependencies[0] == std::pair<std::string, bool>{"base", false});
  CHECK(info->parsedDependencies[1] == std::pair<std::string, bool>{"optionalmod", true});

  ModSet mods;
  mods.mods["testmod"] = info.get();
  CHECK(mods.ModPathExists("testmod", "data.lua"));
  CHECK(!mods.ModPathExists("testmod", "control.lua"));
  CHECK(mods.ReadModFile("testmod", "lib/util.lua") == "return 42");
  CHECK(mods.ReadModFile("testmod", "missing.lua").empty());

  auto files = mods.GetAllModFiles("testmod", "lib/");
  REQUIRE(files.size() == 1);
  CHECK(files[0] == "lib/util.lua");

  fs::remove(zipPath);
}

TEST_CASE("real mod corpus discovers zipped mods" * doctest::skip(kModsRoot.empty())) {
  // Discovery only; the full modded data stage waits for mod-settings.dat.
  std::vector<std::unique_ptr<ModInfo>> found;
  for (const auto& entry :
       fs::directory_iterator(kModsRoot + "/data/factorio/mods")) {
    if (entry.is_regular_file() && entry.path().extension() == ".zip") {
      auto info = ModInfo::FromZip(entry.path().string());
      if (info != nullptr) found.push_back(std::move(info));
    }
  }
  CHECK(found.size() > 50);

  // Every discovered mod parsed a name, a version, and has a readable root.
  ModSet mods;
  int withData = 0;
  for (const auto& info : found) {
    CHECK(!info->name.empty());
    CHECK(info->folder.back() == '/');
    mods.mods[info->name] = info.get();
    if (mods.ModPathExists(info->name, "data.lua")) withData++;
  }
  CHECK(withData > 30);  // most content mods ship a data.lua
}
