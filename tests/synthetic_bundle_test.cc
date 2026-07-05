// Synthetic-bundle test: builds a small hand-crafted database covering the
// feature surface (crafters with power/fuel, spent-fuel loops, modules,
// beacons, allowed-effects gating, costs), writes a REAL .yafcbundle zip,
// reloads it the way the web app does, and solves recipe tables on the
// reloaded database. This is the CI stand-in for the golden pyanodon test,
// which skips when no game data is present: every field the solver depends
// on must survive the dump/bundle round-trip.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "doctest/doctest.h"
#include "yafc/model/database.h"
#include "yafc/model/production_table.h"
#include "yafc/parser/factorio_data_source.h"
#include "yafc/serialization/database_dump.h"

using namespace yafc;
namespace fs = std::filesystem;

namespace {

template <typename T>
T* Add(std::vector<std::unique_ptr<FactorioObject>>& objects, const char* name) {
  auto obj = std::make_unique<T>();
  obj->name = name;
  obj->locName = name;
  T* raw = obj.get();
  objects.push_back(std::move(obj));
  return raw;
}

// A pocket modpack: electric smelting with modules + beacons, and a
// nuclear-style burn/reprocess loop driven by spent fuel.
std::unique_ptr<Database> BuildSyntheticDatabase() {
  std::vector<std::unique_ptr<FactorioObject>> objects;
  Item* ore = Add<Item>(objects, "ore");
  Item* plate = Add<Item>(objects, "plate");
  Item* cell = Add<Item>(objects, "fuel-cell");
  Item* spent = Add<Item>(objects, "spent-cell");
  Item* heat = Add<Item>(objects, "heat");
  auto* electricity = Add<Special>(objects, "electricity");
  electricity->power = true;
  electricity->fuelValue = 1;
  cell->fuelValue = 8;
  cell->fuelResult = spent;

  auto* speedModule = Add<Module>(objects, "speed-module");
  speedModule->moduleSpecification.baseSpeed = 0.5f;
  speedModule->moduleSpecification.baseConsumption = 0.7f;
  auto* prodModule = Add<Module>(objects, "prod-module");
  prodModule->moduleSpecification.baseProductivity = 0.1f;
  prodModule->moduleSpecification.baseSpeed = -0.15f;

  Recipe* mine = Add<Recipe>(objects, "mine");
  mine->products.emplace_back(ore, 1.0f);
  Recipe* smelt = Add<Recipe>(objects, "smelt");
  smelt->time = 4;
  smelt->allowedEffects = AllowedEffects::kAll;
  smelt->ingredients.emplace_back(ore, 1.0f);
  smelt->products.emplace_back(plate, 1.0f);
  Recipe* makeCell = Add<Recipe>(objects, "make-cell");
  makeCell->ingredients.emplace_back(plate, 1.0f);
  makeCell->products.emplace_back(cell, 1.0f);
  Recipe* burn = Add<Recipe>(objects, "burn");
  burn->time = 1;
  burn->products.emplace_back(heat, 1.0f);
  Recipe* reprocess = Add<Recipe>(objects, "reprocess");
  reprocess->ingredients.emplace_back(spent, 1.0f);
  reprocess->products.emplace_back(ore, 0.5f);

  auto* furnace = Add<EntityCrafter>(objects, "furnace");
  furnace->baseCraftingSpeed = 2;
  furnace->basePower = 0.18f;
  furnace->hasEnergy = true;
  furnace->energy.fuels.push_back(electricity);
  furnace->allowedEffects = AllowedEffects::kAll;
  furnace->moduleSlots = 2;
  smelt->crafters.push_back(furnace);

  auto* reactor = Add<EntityCrafter>(objects, "reactor");
  reactor->baseCraftingSpeed = 1;
  reactor->basePower = 8;  // MW; cell fuelValue 8 -> 1 cell/s per building
  reactor->hasEnergy = true;
  reactor->energy.fuels.push_back(cell);
  burn->crafters.push_back(reactor);

  auto* beacon = Add<EntityBeacon>(objects, "beacon");
  beacon->allowedEffects = AllowedEffects::kSpeed | AllowedEffects::kConsumption;
  beacon->moduleSlots = 2;
  beacon->beaconEfficiency = 0.5f;
  beacon->profileValues = {1.0f, 0.8f};

  auto db = std::make_unique<Database>();
  db->LoadBuiltData(std::move(objects));
  return db;
}

Goods* FindGoods(Database& db, const std::string& tdn) {
  INFO("goods lookup: ", tdn);
  auto* g = dynamic_cast<Goods*>(db.FindByTypeDotName(tdn));
  REQUIRE(g != nullptr);
  return g;
}

RecipeOrTechnology* FindRecipe(Database& db, const std::string& tdn) {
  INFO("recipe lookup: ", tdn);
  auto* r = dynamic_cast<RecipeOrTechnology*>(db.FindByTypeDotName(tdn));
  REQUIRE(r != nullptr);
  return r;
}

}  // namespace

TEST_CASE("synthetic bundle: write, reload, and solve recipes (CI path)") {
  std::unique_ptr<Database> source = BuildSyntheticDatabase();

  // Fabricated per-object costs prove the costs.cbor side channel.
  std::vector<float> costs(static_cast<size_t>(source->objects.count()), 0.0f);
  for (FactorioObject* o : source->objects) costs[o->id] = 1.0f + o->id;

  std::string path =
      (fs::temp_directory_path() / "mancos_synthetic.yafcbundle").string();
  ModSet emptyMods;  // no icons to resolve; specs in this db are empty
  BundleWriteStats stats =
      WriteBundle(path, *source, emptyMods, "9.9.9", {{"synthetic", "1.0.0"}}, &costs);
  CHECK(stats.objects == static_cast<size_t>(source->objects.count()));
  CHECK(stats.missingIcons == 0);

  Bundle bundle = ReadBundle(path);
  REQUIRE(bundle.db != nullptr);
  Database& db = *bundle.db;
  CHECK(db.objects.count() == source->objects.count());
  CHECK(bundle.meta["mods"]["synthetic"] == "1.0.0");
  REQUIRE(bundle.costs.size() == costs.size());
  CHECK(bundle.costs[FindGoods(db, "Item.plate")->id] ==
        doctest::Approx(1.0f + FindGoods(db, "Item.plate")->id));

  // --- electric smelting with modules + beacons on the RELOADED database ---
  auto* smelt = FindRecipe(db, "Recipe.smelt");
  auto* furnace = dynamic_cast<EntityCrafter*>(db.FindByTypeDotName("Entity.furnace"));
  auto* beacon = dynamic_cast<EntityBeacon*>(db.FindByTypeDotName("Entity.beacon"));
  auto* speedModule = dynamic_cast<Module*>(db.FindByTypeDotName("Item.speed-module"));
  auto* prodModule = dynamic_cast<Module*>(db.FindByTypeDotName("Item.prod-module"));
  REQUIRE(furnace != nullptr);
  REQUIRE(beacon != nullptr);
  REQUIRE(speedModule != nullptr);
  REQUIRE(prodModule != nullptr);
  CHECK(beacon->profile(2) == doctest::Approx(0.8f));

  {
    ProductionTable root;
    root.AddLink({FindGoods(db, "Item.ore"), nullptr});
    root.AddLink({FindGoods(db, "Item.plate"), nullptr}, 11);
    root.AddLink({FindGoods(db, "Power.electricity"), nullptr});
    root.AddRecipe(FindRecipe(db, "Recipe.mine"));
    RecipeRow* row = root.AddRecipe(smelt);
    row->entity = {furnace, nullptr};
    row->fuel = {FindGoods(db, "Power.electricity"), nullptr};
    row->modules.list = {{prodModule, 1}, {speedModule, 0}};
    row->modules.beacon = beacon;
    row->modules.beaconList = {{speedModule, 4}};

    REQUIRE(root.Solve() == TableSolveResult::Ok);
    // speed: crafter 2x; modules 0.5 - 0.15; beacons 0.5 eff x 0.8 profile
    // x 4 modules x 0.5 speed = +0.8 -> recipeTime = 4/2/(1+0.35+0.8).
    CHECK(row->parameters.recipeTime == doctest::Approx(2.0f / 2.15f));
    CHECK(row->parameters.activeEffects.productivity == doctest::Approx(0.1f));
    CHECK(row->recipesPerSecond == doctest::Approx(10));  // 11 plates / 1.1
    CHECK(row->parameters.usedBeacon == beacon);
    CHECK(row->parameters.usedBeaconCount == 2);
    // Electric fuel: power flows through the fuel path with consumption mod.
    CHECK(row->parameters.fuelUsagePerSecondPerBuilding > 0);
  }

  // --- spent-fuel loop: burn cells, reprocess spent into ore ---
  {
    ProductionTable root;
    root.AddLink({FindGoods(db, "Item.heat"), nullptr}, 10);
    ProductionLink* spentLink = root.AddLink({FindGoods(db, "Item.spent-cell"), nullptr});
    root.AddLink({FindGoods(db, "Item.fuel-cell"), nullptr});
    root.AddLink({FindGoods(db, "Item.plate"), nullptr});
    root.AddLink({FindGoods(db, "Item.ore"), nullptr});
    root.AddRecipe(FindRecipe(db, "Recipe.mine"));
    RecipeRow* smeltRow = root.AddRecipe(smelt);
    smeltRow->entity = {furnace, nullptr};
    root.AddRecipe(FindRecipe(db, "Recipe.make-cell"));
    RecipeRow* burnRow = root.AddRecipe(FindRecipe(db, "Recipe.burn"));
    auto* reactor = dynamic_cast<EntityCrafter*>(db.FindByTypeDotName("Entity.reactor"));
    REQUIRE(reactor != nullptr);
    burnRow->entity = {reactor, nullptr};
    burnRow->fuel = {FindGoods(db, "Item.fuel-cell"), nullptr};
    root.AddRecipe(FindRecipe(db, "Recipe.reprocess"));

    REQUIRE(root.Solve() == TableSolveResult::Ok);
    CHECK(burnRow->recipesPerSecond == doctest::Approx(10));
    // fuelResult survived the bundle: spent cells match burn -> reprocess.
    CHECK((spentLink->flags & LinkFlags::kLinkNotMatched) == 0);
    CHECK(spentLink->linkFlow == doctest::Approx(10));
  }

  std::remove(path.c_str());
}

TEST_CASE("unknown Entity subkinds from newer bundlers load as plain entities") {
  std::unique_ptr<Database> source = BuildSyntheticDatabase();
  nlohmann::json dump = DumpDatabase(*source);

  // Simulate a future bundler: the beacon (referenced by nothing typed)
  // carries an entity kind this build has never heard of, plus a field
  // only that kind would understand.
  bool patched = false;
  for (auto& e : dump["objects"]) {
    if (e["name"] == "beacon") {
      e["kind"] = "EntityWarpGate";
      e["warpFactor"] = 9.75;
      patched = true;
    }
  }
  REQUIRE(patched);

  std::unique_ptr<Database> reloaded = LoadDatabase(dump);
  REQUIRE(reloaded != nullptr);
  FactorioObject* gate = reloaded->FindByTypeDotName("Entity.beacon");
  REQUIRE(gate != nullptr);
  CHECK(dynamic_cast<Entity*>(gate) != nullptr);       // base data survives
  CHECK(dynamic_cast<EntityBeacon*>(gate) == nullptr);  // subkind detail lost

  // Truly unknown non-entity kinds must still refuse to load.
  nlohmann::json bad = DumpDatabase(*source);
  bad["objects"][0]["kind"] = "QuantumRecipe";
  CHECK_THROWS(LoadDatabase(bad));
}
