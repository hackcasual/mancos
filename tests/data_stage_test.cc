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

// ---------------------------------------------------------------------------
// Deserializer: data.raw -> Database (Phase 3 final chunk).
#include "yafc/analysis/automation_analysis.h"
#include "yafc/analysis/milestones.h"
#include "yafc/parser/data_deserializer.h"

TEST_CASE("vanilla LoadData -> Database" * doctest::skip(kRoot.empty())) {
  FactorioDataSource source(kRoot + "/data/factorio/data", "",
                            kRoot + "/third_party/yafc-ce/Yafc/Data");
  ParseResult stage = source.Parse();
  LoadDataResult result =
      DataDeserializer::LoadData(*stage.lua, stage.factorioVersion);
  REQUIRE(result.db != nullptr);
  Database& db = *result.db;

  CHECK(db.recipes.count() > 200);
  CHECK(db.items.count() > 200);
  auto* ironPlate = dynamic_cast<Item*>(db.FindByTypeDotName("Item.iron-plate"));
  REQUIRE(ironPlate != nullptr);
  CHECK(!ironPlate->production.empty());
  REQUIRE(!ironPlate->iconSpec.empty());
  CHECK(ironPlate->iconSpec[0].path.find("__base__/") == 0);

  // Special objects and synthesized recipes exist under upstream names.
  CHECK(db.FindByTypeDotName("Power.electricity") != nullptr);
  CHECK(db.FindByTypeDotName("Power.void") != nullptr);
  CHECK(db.FindByTypeDotName("Mechanics.generator.electricity") != nullptr);
  CHECK(db.voidEnergy != nullptr);
  CHECK(db.science != nullptr);
  CHECK(db.character != nullptr);
  CHECK(db.qualityNormal != nullptr);

  // Accessibility over the real database.
  Dependencies deps;
  deps.Calculate(db);
  Milestones ms;
  MilestonesInput input;
  auto warnings = ms.Compute(db, deps, input);
  CHECK(ms.IsAccessible(ironPlate));
  CHECK(!warnings.mostObjectsInaccessible);
}

TEST_CASE("modded (py) LoadData -> Database" * doctest::skip(kModsRoot2.empty())) {
  FactorioDataSource source(kModsRoot2 + "/data/factorio/data",
                            kModsRoot2 + "/data/factorio/mods",
                            kModsRoot2 + "/third_party/yafc-ce/Yafc/Data");
  ParseResult stage = source.Parse();
  LoadDataResult result =
      DataDeserializer::LoadData(*stage.lua, stage.factorioVersion);
  REQUIRE(result.db != nullptr);
  Database& db = *result.db;
  std::printf("  [deserialize] %d objects, %d recipes, %d items, %d errors\n",
              db.objects.count(), db.recipes.count(), db.items.count(),
              static_cast<int>(result.errors.size()));
  for (size_t i = 0; i < result.errors.size() && i < 8; ++i) {
    std::printf("  [deserialize] error: %s\n", result.errors[i].c_str());
  }

  CHECK(db.recipes.count() > 1000);
  CHECK(db.items.count() > 1000);
  CHECK(db.FindByTypeDotName("Item.ore-lead") != nullptr);
  CHECK(db.FindByTypeDotName("Item.lead-plate") != nullptr);

  auto* grade1 = dynamic_cast<Recipe*>(db.FindByTypeDotName("Recipe.grade-1-lead"));
  REQUIRE(grade1 != nullptr);
  REQUIRE(!grade1->ingredients.empty());
  REQUIRE(!grade1->products.empty());
  std::printf("  [grade-1-lead] time=%.2f\n", grade1->time);
  for (const Ingredient& i : grade1->ingredients) {
    std::printf("  [grade-1-lead] in:  %.3f x %s\n", i.amount, i.goods->name.c_str());
  }
  for (const Product& p : grade1->products) {
    std::printf("  [grade-1-lead] out: %.3f x %s\n", p.amount, p.goods->name.c_str());
  }
  for (const FactorioIconPart& part : grade1->iconSpec) {
    std::printf("  [grade-1-lead] icon: %s (size %d scale %.2f)\n", part.path.c_str(),
                part.size, part.scale);
  }
  CHECK(!grade1->crafters.empty());

  Dependencies deps;
  deps.Calculate(db);
  Milestones ms;
  MilestonesInput input;
  ms.Compute(db, deps, input);
  auto* ironPlate = db.FindByTypeDotName("Item.iron-plate");
  REQUIRE(ironPlate != nullptr);
  CHECK(ms.IsAccessible(ironPlate));
}
