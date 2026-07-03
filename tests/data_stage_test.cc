// Phase 3 tracer bullet: run Factorio's REAL data stage (vanilla + Space Age
// DLC mods from the user's install) through the ported LuaContext, and verify
// data.raw contains well-known prototypes. Skipped when the local game data
// (data/factorio/data) is not present (e.g. CI).
#include <cstdio>
#include <filesystem>
#include <string>

#include "doctest/doctest.h"
#include "yafc/parser/factorio_data_source.h"

using namespace yafc;
namespace fs = std::filesystem;

namespace {

// Locate the repo root by walking up from the CWD looking for the data dir.
std::string FindRepoRoot() {
  fs::path dir = fs::current_path();
  for (int i = 0; i < 6; ++i) {
    if (fs::exists(dir / "data/factorio/data/base/info.json") &&
        fs::exists(dir / "third_party/yafc-ce/Yafc/Data/Sandbox.lua")) {
      return dir.string();
    }
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  return {};
}

const std::string kRoot = FindRepoRoot();

}  // namespace

TEST_CASE("vanilla+SA data stage produces data.raw" * doctest::skip(kRoot.empty())) {
  FactorioDataSource source(kRoot + "/data/factorio/data", "",
                            kRoot + "/third_party/yafc-ce/Yafc/Data");
  ParseResult result = source.Parse([](const std::string& step) {
    std::printf("  [data-stage] %s\n", step.c_str());
  });

  CHECK(result.factorioVersion.major == 2);
  // core first, then the alphabetical batches.
  REQUIRE(result.modLoadOrder.size() == 5);
  CHECK(result.modLoadOrder[0] == "core");
  CHECK(result.modLoadOrder[1] == "base");

  LuaContext& lua = *result.lua;
  LuaValue data = lua.GetGlobal("data");
  REQUIRE(data.kind == LuaValue::Kind::Table);
  LuaValue raw = lua.GetField(data.tableRef, "raw");
  REQUIRE(raw.kind == LuaValue::Kind::Table);

  auto prototype = [&](const char* type, const char* name) {
    LuaValue category = lua.GetField(raw.tableRef, type);
    if (category.kind != LuaValue::Kind::Table) return false;
    return lua.GetField(category.tableRef, name).kind == LuaValue::Kind::Table;
  };

  // Base game prototypes.
  CHECK(prototype("item", "iron-plate"));
  CHECK(prototype("recipe", "iron-gear-wheel"));
  CHECK(prototype("technology", "automation"));
  CHECK(prototype("fluid", "water"));
  CHECK(prototype("furnace", "stone-furnace"));
  // Space Age DLC prototypes (present because the DLC mods auto-enable).
  CHECK(prototype("item", "carbon-fiber"));
  CHECK(prototype("recipe", "casting-iron"));
  // Quality mod prototype.
  CHECK(prototype("quality", "legendary"));

  // A recipe's contents are reachable (ingredients of iron-gear-wheel).
  LuaValue recipes = lua.GetField(raw.tableRef, "recipe");
  LuaValue gearWheel = lua.GetField(recipes.tableRef, "iron-gear-wheel");
  LuaValue ingredients = lua.GetField(gearWheel.tableRef, "ingredients");
  REQUIRE(ingredients.kind == LuaValue::Kind::Table);
  LuaValue first = lua.GetIndex(ingredients.tableRef, 1);
  REQUIRE(first.kind == LuaValue::Kind::Table);
  LuaValue ingredientName = lua.GetField(first.tableRef, "name");
  CHECK(ingredientName.string == "iron-plate");
}

TEST_CASE("energy and expression helpers match upstream") {
  CHECK(LuaContext::ParseEnergy("600kW") == doctest::Approx(0.6));  // megawatts
  CHECK(LuaContext::ParseEnergy("1.21GW") == doctest::Approx(1210));
  CHECK(LuaContext::ParseEnergy("100J") == doctest::Approx(1e-4));

  auto vars = [](const std::string& name) -> std::optional<double> {
    if (name == "L" || name == "l") return 6;
    return std::nullopt;
  };
  // Classic tech count formula at L=6: 2^(L-6)*1000 = 1000.
  CHECK(LuaContext::EvaluateExpression("2^(L-6)*1000", vars) == doctest::Approx(1000));
  CHECK(LuaContext::EvaluateExpression("(l-3)*100", vars) == doctest::Approx(300));
  // Unparsable expressions return 1 (upstream quirk).
  CHECK(LuaContext::EvaluateExpression("2 + unknown_fn(3)", vars) == doctest::Approx(1));
}

namespace {
std::string FindModsRoot() {
  fs::path dir = fs::current_path();
  for (int i = 0; i < 6; ++i) {
    if (fs::exists(dir / "data/factorio/mods/mod-list.json") &&
        fs::exists(dir / "data/factorio/data/base/info.json")) {
      return dir.string();
    }
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  return {};
}
const std::string kModsRoot2 = FindModsRoot();
}  // namespace

TEST_CASE("FULL modded data stage (93 mods)" * doctest::skip(kModsRoot2.empty())) {
  FactorioDataSource source(kModsRoot2 + "/data/factorio/data",
                            kModsRoot2 + "/data/factorio/mods",
                            kModsRoot2 + "/third_party/yafc-ce/Yafc/Data");
  ParseResult result = source.Parse([](const std::string& step) {
    if (step.find("data-final-fixes") != std::string::npos ||
        step.find(".lua") == std::string::npos) {
      std::printf("  [modded] %s\n", step.c_str());
    }
  });

  CHECK(result.modLoadOrder.size() > 30);  // 40 enabled of 91 zips

  LuaContext& lua = *result.lua;
  LuaValue data = lua.GetGlobal("data");
  REQUIRE(data.kind == LuaValue::Kind::Table);
  LuaValue raw = lua.GetField(data.tableRef, "raw");
  REQUIRE(raw.kind == LuaValue::Kind::Table);

  auto prototype = [&](const char* type, const char* name) {
    LuaValue category = lua.GetField(raw.tableRef, type);
    if (category.kind != LuaValue::Kind::Table) return false;
    return lua.GetField(category.tableRef, name).kind == LuaValue::Kind::Table;
  };
  // Pyanodon content from the corpus alongside vanilla (the lead chain from
  // the reference screenshot lives in pyrawores).
  CHECK(prototype("item", "iron-plate"));
  CHECK(prototype("item", "lead-plate"));
  CHECK(prototype("item", "ore-lead"));
  CHECK(prototype("item", "grade-1-lead"));
  CHECK(prototype("recipe", "grade-1-lead"));
}
