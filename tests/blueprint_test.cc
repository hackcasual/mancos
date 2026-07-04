// Blueprint export: game-format string encoding (decode round-trip through
// miniz) and the sets-of-buildings auto-layout content.
#include "yafc/model/blueprint.h"

#include <memory>
#include <vector>

#include <miniz.h>
#include "doctest/doctest.h"
#include "yafc/model/database.h"

using namespace yafc;

namespace {

template <typename T>
T* Add(std::vector<std::unique_ptr<FactorioObject>>& objects, const char* name) {
  auto obj = std::make_unique<T>();
  obj->name = name;
  T* raw = obj.get();
  objects.push_back(std::move(obj));
  return raw;
}

// Inverse of EncodeBlueprintString: strip "0", base64-decode, zlib-inflate.
nlohmann::json DecodeBlueprintString(const std::string& text) {
  REQUIRE(!text.empty());
  REQUIRE(text[0] == '0');
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<unsigned char> bytes;
  int buffer = 0, bits = 0;
  for (size_t i = 1; i < text.size(); ++i) {
    int v = value(text[i]);
    if (v < 0) continue;  // '='
    buffer = (buffer << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
    }
  }
  std::vector<unsigned char> inflated(1 << 20);
  mz_ulong outLength = inflated.size();
  REQUIRE(mz_uncompress(inflated.data(), &outLength, bytes.data(),
                        static_cast<mz_ulong>(bytes.size())) == MZ_OK);
  return nlohmann::json::parse(inflated.begin(), inflated.begin() + outLength);
}

}  // namespace

TEST_CASE("blueprint export: sets of buildings with recipes, modules and quality") {
  std::vector<std::unique_ptr<FactorioObject>> objects;
  Item* ore = Add<Item>(objects, "ore");
  Item* plate = Add<Item>(objects, "plate");
  Item* coal = Add<Item>(objects, "coal");
  coal->fuelValue = 4;
  auto* speedModule = Add<Module>(objects, "speed-module");
  speedModule->moduleSpecification.baseSpeed = 0.5f;
  Recipe* smelt = Add<Recipe>(objects, "smelt");
  smelt->time = 2;
  smelt->allowedEffects = AllowedEffects::kAll;
  smelt->ingredients.emplace_back(ore, 1.0f);
  smelt->products.emplace_back(plate, 1.0f);
  auto* furnace = Add<EntityCrafter>(objects, "steel-furnace");
  furnace->factorioType = "furnace";
  furnace->baseCraftingSpeed = 1;
  furnace->allowedEffects = AllowedEffects::kAll;
  furnace->moduleSlots = 2;
  furnace->width = 2;
  furnace->height = 2;
  auto* normal = Add<Quality>(objects, "normal");
  normal->level = 0;
  auto* rare = Add<Quality>(objects, "rare");
  rare->level = 1;
  normal->nextQuality = rare;
  rare->previousQuality = normal;
  Database db;
  db.LoadBuiltData(std::move(objects));

  ProductionTable root;
  root.settings.qualityNormal = normal;
  root.AddLink({ore, normal});
  root.AddLink({plate, rare}, 5);
  RecipeRow* row = root.AddRecipe(smelt);
  row->entity = {furnace, normal};
  row->fuel = {coal, normal};
  row->quality = rare;
  row->modules.list = {{speedModule, 0}};
  REQUIRE(root.Solve() == TableSolveResult::Ok);
  // 5/s at 1 plate/craft, 2s craft at 2x speed (two +50% modules fill the
  // slots) -> buildings = 5 * (2/2) = 5.
  CHECK(row->buildingCount() == doctest::Approx(5.0f));

  BlueprintOptions options;
  options.label = "test";
  BlueprintResult result = ExportBlueprint({row}, options);
  REQUIRE(result.error.empty());
  CHECK(result.buildings == 5);
  CHECK(result.truncatedRows == 0);

  nlohmann::json decoded = DecodeBlueprintString(result.blueprintString);
  const nlohmann::json& bp = decoded.at("blueprint");
  CHECK(bp.at("item") == "blueprint");
  CHECK(bp.at("label") == "test");
  const nlohmann::json& entities = bp.at("entities");
  REQUIRE(entities.size() == 5);

  const nlohmann::json& first = entities[0];
  CHECK(first.at("entity_number") == 1);
  CHECK(first.at("name") == "steel-furnace");
  CHECK(first.at("recipe") == "smelt");
  CHECK(first.at("recipe_quality") == "rare");
  // 2x2 building at top-left (0,0): center (1,1).
  CHECK(first.at("position").at("x") == doctest::Approx(1.0));
  CHECK(first.at("position").at("y") == doctest::Approx(1.0));
  // Fuel request for the burner inventory.
  CHECK(first.at("burner_fuel_inventory").at("filters")[0].at("name") == "coal");
  // 2 speed modules (fill-remaining) as item requests in crafter inventory 4.
  const nlohmann::json& item = first.at("items")[0];
  CHECK(item.at("id").at("name") == "speed-module");
  REQUIRE(item.at("items").at("in_inventory").size() == 2);
  CHECK(item.at("items").at("in_inventory")[0].at("inventory") == 4);

  // No two entities overlap: 2x2 bodies with 1-tile gaps.
  for (size_t i = 0; i < entities.size(); ++i) {
    for (size_t j = i + 1; j < entities.size(); ++j) {
      double dx = std::abs(entities[i].at("position").at("x").get<double>() -
                           entities[j].at("position").at("x").get<double>());
      double dy = std::abs(entities[i].at("position").at("y").get<double>() -
                           entities[j].at("position").at("y").get<double>());
      CHECK(std::max(dx, dy) >= 2.0);  // >= width -> disjoint footprints
    }
  }
}

TEST_CASE("blueprint export: per-row cap, mechanics rows get no recipe field") {
  std::vector<std::unique_ptr<FactorioObject>> objects;
  Item* ore = Add<Item>(objects, "ore");
  auto* mine = Add<Mechanics>(objects, "mine-ore");
  mine->time = 1;
  mine->products.emplace_back(ore, 1.0f);
  auto* drill = Add<EntityCrafter>(objects, "burner-drill");
  drill->factorioType = "mining-drill";
  drill->baseCraftingSpeed = 1;
  drill->width = 2;
  drill->height = 2;
  Database db;
  db.LoadBuiltData(std::move(objects));

  ProductionTable root;
  root.AddLink({ore, nullptr}, 100);
  RecipeRow* row = root.AddRecipe(mine);
  row->entity = {drill, nullptr};
  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(row->buildingCount() == doctest::Approx(100.0f));

  BlueprintOptions options;
  options.maxBuildingsPerRow = 10;
  BlueprintResult result = ExportBlueprint({row}, options);
  REQUIRE(result.error.empty());
  CHECK(result.buildings == 10);
  CHECK(result.truncatedRows == 1);

  nlohmann::json decoded = DecodeBlueprintString(result.blueprintString);
  const nlohmann::json& entities = decoded.at("blueprint").at("entities");
  REQUIRE(entities.size() == 10);
  // Mechanics pseudo-recipe: entity carries no recipe field.
  CHECK(!entities[0].contains("recipe"));
}
