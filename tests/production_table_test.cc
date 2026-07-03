// Tests for the hierarchical ProductionTable model: nested subgroups, link
// resolution up the hierarchy, flatten/solve/write-back, CalculateFlow rollup,
// ChildNotMatched propagation and built-count warnings.
#include "yafc/model/production_table.h"

#include <memory>
#include <vector>

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

struct Goods2 {
  Database db;
  Item* ore;
  Item* plate;
  Item* useful;
  Item* gangue;
  Recipe* mine;
  Recipe* smelt;
  Recipe* byp;

  Goods2() {
    std::vector<std::unique_ptr<FactorioObject>> objects;
    ore = Add<Item>(objects, "ore");
    plate = Add<Item>(objects, "plate");
    useful = Add<Item>(objects, "useful");
    gangue = Add<Item>(objects, "gangue");
    mine = Add<Recipe>(objects, "mine");
    smelt = Add<Recipe>(objects, "smelt");
    byp = Add<Recipe>(objects, "byproduct-maker");

    mine->products.emplace_back(ore, 1.0f);
    smelt->ingredients.emplace_back(ore, 1.0f);
    smelt->products.emplace_back(plate, 1.0f);
    byp->products.emplace_back(useful, 1.0f);
    byp->products.emplace_back(gangue, 1.0f);
    db.LoadBuiltData(std::move(objects));
  }
};

}  // namespace

TEST_CASE("nested table: header recipe resolves against inner links") {
  Goods2 g;
  ProductionTable root;
  ProductionLink* plateLink = root.AddLink({g.plate, nullptr}, 30);

  // Nested table headed by the smelt recipe; ore is linked *inside* it.
  RecipeRow* header = root.AddNestedTable(g.smelt);
  ProductionLink* oreLink = header->subgroup->AddLink({g.ore, nullptr});
  RecipeRow* mineRow = header->subgroup->AddRecipe(g.mine);

  REQUIRE(root.Solve() == TableSolveResult::Ok);

  CHECK(header->recipesPerSecond == doctest::Approx(30));
  CHECK(mineRow->recipesPerSecond == doctest::Approx(30));
  CHECK(plateLink->linkFlow == doctest::Approx(30));
  CHECK(oreLink->linkFlow == doctest::Approx(30));
  CHECK((oreLink->flags & LinkFlags::kLinkNotMatched) == 0);
  CHECK(root.containsDesiredProducts);

  // The subgroup exports the header's product upward; at the root the plate
  // link swallows it, so no unmatched flow remains anywhere.
  REQUIRE(header->subgroup->flow.size() == 1);
  CHECK(header->subgroup->flow[0].goods.target == g.plate);
  CHECK(header->subgroup->flow[0].amount == doctest::Approx(30));
  CHECK(root.flow.empty());
}

TEST_CASE("broken child link taints the parent link (ChildNotMatched)") {
  Goods2 g;
  ProductionTable root;
  root.AddLink({g.useful, nullptr}, 10);
  ProductionLink* rootGangue = root.AddLink({g.gangue, nullptr});

  RecipeRow* header = root.AddNestedTable(nullptr);
  // The subgroup shadows the root's gangue link with its own.
  ProductionLink* childGangue = header->subgroup->AddLink({g.gangue, nullptr});
  RecipeRow* bypRow = header->subgroup->AddRecipe(g.byp);

  REQUIRE(root.Solve() == TableSolveResult::Ok);

  CHECK(bypRow->recipesPerSecond == doctest::Approx(10));
  // Child gangue link: production, no consumption -> one-sided, not matched.
  CHECK((childGangue->flags & LinkFlags::kLinkNotMatched) != 0);
  // The parent's gangue link is tainted by the child's imbalance.
  CHECK((rootGangue->flags & LinkFlags::kChildNotMatched) != 0);
  CHECK((rootGangue->flags & LinkFlags::kLinkNotMatched) != 0);
  // The surplus surfaces in the root flow as an extra product.
  bool foundGangue = false;
  for (const ProductionTableFlow& f : root.flow) {
    if (f.goods.target == g.gangue) {
      foundGangue = true;
      CHECK(f.amount == doctest::Approx(10));
    }
  }
  CHECK(foundGangue);
}

TEST_CASE("fixed buildings pin rate via recipeTime; built-count warnings") {
  Goods2 g;
  ProductionTable root;
  root.AddLink({g.ore, nullptr});
  root.AddLink({g.plate, nullptr}, 0)->algorithm = LinkAlgorithm::AllowOverProduction;

  g.smelt->time = 4;  // parameters are computed from the recipe now
  RecipeRow* mineRow = root.AddRecipe(g.mine);
  RecipeRow* smeltRow = root.AddRecipe(g.smelt);
  smeltRow->fixedBuildings = 2;

  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(smeltRow->recipesPerSecond == doctest::Approx(0.5));
  CHECK(mineRow->recipesPerSecond == doctest::Approx(0.5));
  CHECK(smeltRow->buildingCount() == doctest::Approx(2.0f));
  CHECK(!root.builtCountExceeded);

  // Now claim only 1 built building: 2 needed -> warning.
  smeltRow->builtBuildings = 1;
  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(root.builtCountExceeded);
  CHECK((smeltRow->parameters.warningFlags & RecipeWarningFlags::kExceedsBuiltCount) != 0);

  smeltRow->builtBuildings = 3;
  smeltRow->parameters.warningFlags = 0;
  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(!root.builtCountExceeded);
}

TEST_CASE("crafter speed, power and fuel drive recipe parameters") {
  std::vector<std::unique_ptr<FactorioObject>> objects;
  Item* ore = Add<Item>(objects, "ore");
  Item* plate = Add<Item>(objects, "plate");
  Special* power = Add<Special>(objects, "electricity");
  power->power = true;
  power->fuelValue = 1;
  Recipe* smelt = Add<Recipe>(objects, "smelt");
  smelt->time = 4;
  smelt->ingredients.emplace_back(ore, 1.0f);
  smelt->products.emplace_back(plate, 1.0f);
  auto* furnace = Add<EntityCrafter>(objects, "furnace");
  furnace->baseCraftingSpeed = 2;
  furnace->basePower = 0.18f;  // MW
  furnace->hasEnergy = true;
  furnace->energy.fuels.push_back(power);
  Database db;
  db.LoadBuiltData(std::move(objects));

  ProductionTable root;
  root.AddLink({ore, nullptr});
  root.AddLink({plate, nullptr}, 10);
  root.AddLink({power, nullptr});
  RecipeRow* row = root.AddRecipe(smelt);
  row->entity = {furnace, nullptr};
  row->fuel = {power, nullptr};

  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(row->parameters.recipeTime == doctest::Approx(2.0f));  // 4s / speed 2
  CHECK(row->recipesPerSecond == doctest::Approx(10));
  CHECK(row->buildingCount() == doctest::Approx(20.0f));
  // Electric "fuel": usage per building == power draw (fuelValue 1, eff 1).
  CHECK(row->parameters.fuelUsagePerSecondPerBuilding == doctest::Approx(0.18f));
  CHECK(row->parameters.fuelUsagePerSecondPerRecipe() == doctest::Approx(0.36f));
  CHECK((row->parameters.warningFlags & WarningFlags::kFuelNotSpecified) == 0);
  CHECK((row->parameters.warningFlags & WarningFlags::kEntityNotSpecified) == 0);

  // Power flow: 20 buildings x 0.18 MW = 3.6 MW consumed.
  bool foundPower = false;
  for (const ProductionTableFlow& f : root.flow) {
    if (f.goods.target == power) {
      foundPower = true;
      CHECK(f.amount == doctest::Approx(-3.6));
    }
  }
  CHECK(foundPower);
}

TEST_CASE("modules and beacons: slot fill, compatibility, beacon profile math") {
  std::vector<std::unique_ptr<FactorioObject>> objects;
  Item* ore = Add<Item>(objects, "ore");
  Item* plate = Add<Item>(objects, "plate");
  auto* speedModule = Add<Module>(objects, "speed-module");
  speedModule->moduleSpecification.baseSpeed = 0.5f;
  speedModule->moduleSpecification.baseConsumption = 0.7f;
  auto* prodModule = Add<Module>(objects, "prod-module");
  prodModule->moduleSpecification.baseProductivity = 0.1f;
  prodModule->moduleSpecification.baseSpeed = -0.15f;
  Recipe* smelt = Add<Recipe>(objects, "smelt");
  smelt->time = 4;
  smelt->allowedEffects = AllowedEffects::kAll;
  smelt->ingredients.emplace_back(ore, 1.0f);
  smelt->products.emplace_back(plate, 1.0f);
  Recipe* noProd = Add<Recipe>(objects, "no-prod");
  noProd->time = 4;
  noProd->allowedEffects = AllowedEffects::kSpeed | AllowedEffects::kConsumption;
  noProd->ingredients.emplace_back(ore, 1.0f);
  noProd->products.emplace_back(plate, 1.0f);
  auto* furnace = Add<EntityCrafter>(objects, "furnace");
  furnace->allowedEffects = AllowedEffects::kAll;
  furnace->moduleSlots = 2;
  auto* beacon = Add<EntityBeacon>(objects, "beacon");
  beacon->allowedEffects = AllowedEffects::kSpeed | AllowedEffects::kConsumption;
  beacon->moduleSlots = 2;
  beacon->beaconEfficiency = 0.5f;
  beacon->profileValues = {1.0f, 0.8f};
  Database db;
  db.LoadBuiltData(std::move(objects));

  ProductionTable root;
  root.AddLink({ore, nullptr});
  root.AddLink({plate, nullptr}, 11);
  RecipeRow* row = root.AddRecipe(smelt);
  row->entity = {furnace, nullptr};
  // 1 fixed prod module + speed module filling the remaining slot.
  row->modules.list = {{prodModule, 1}, {speedModule, 0}};

  REQUIRE(root.Solve() == TableSolveResult::Ok);
  // speed = 0.5 - 0.15 = 0.35 -> recipeTime = 4 / 1.35; productivity 0.1.
  CHECK(row->parameters.activeEffects.speed == doctest::Approx(0.35f));
  CHECK(row->parameters.activeEffects.productivity == doctest::Approx(0.1f));
  CHECK(row->parameters.recipeTime == doctest::Approx(4.0f / 1.35f));
  // 11 plates/s at 1.1 plates per craft -> 10 crafts/s.
  CHECK(row->recipesPerSecond == doctest::Approx(10));
  REQUIRE(row->parameters.usedModules.size() == 2);
  CHECK(row->parameters.usedModules[0].module == prodModule);
  CHECK(row->parameters.usedModules[0].fixedCount == 1);
  CHECK(row->parameters.usedModules[1].fixedCount == 1);

  // Productivity module is rejected by a recipe that disallows productivity;
  // the speed module then fills both slots.
  row->recipe = noProd;
  row->modules.list = {{prodModule, 1}, {speedModule, 0}};
  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(row->parameters.activeEffects.productivity == doctest::Approx(0.0f));
  CHECK(row->parameters.activeEffects.speed == doctest::Approx(1.0f));
  REQUIRE(row->parameters.usedModules.size() == 1);
  CHECK(row->parameters.usedModules[0].module == speedModule);
  CHECK(row->parameters.usedModules[0].fixedCount == 2);

  // Beacons: 4 speed modules across 2-slot beacons -> 2 beacons, profile 0.8,
  // efficiency 0.5 -> speed += 0.5 * 0.8 * 4 * 0.5 = 0.8.
  row->recipe = smelt;
  row->modules.list.clear();
  row->modules.beacon = beacon;
  row->modules.beaconList = {{speedModule, 4}};
  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(row->parameters.activeEffects.speed == doctest::Approx(0.8f));
  CHECK(row->parameters.usedBeacon == beacon);
  CHECK(row->parameters.usedBeaconCount == 2);

  // Filler defaults apply when the row has no explicit template.
  row->modules = {};
  root.settings.filler.fillerModule = speedModule;
  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(row->parameters.activeEffects.speed == doctest::Approx(1.0f));
  REQUIRE(row->parameters.usedModules.size() == 1);
  CHECK(row->parameters.usedModules[0].fixedCount == 2);
}

// Nuclear-style loop: a reactor burns fuel cells (fuel, not ingredient) into
// spent cells, which reprocessing recovers into part of the ore that new
// cells are made from. Without the spent-fuel product the loop deadlocks.
TEST_CASE("burning fuel emits its spent form and closes reprocessing loops") {
  std::vector<std::unique_ptr<FactorioObject>> objects;
  Item* ore = Add<Item>(objects, "ore");
  Item* cell = Add<Item>(objects, "fuel-cell");
  Item* spent = Add<Item>(objects, "spent-cell");
  Item* heat = Add<Item>(objects, "heat");
  cell->fuelValue = 1;
  cell->fuelResult = spent;
  Recipe* mine = Add<Recipe>(objects, "mine");
  mine->products.emplace_back(ore, 1.0f);
  Recipe* makeCell = Add<Recipe>(objects, "make-cell");
  makeCell->ingredients.emplace_back(ore, 1.0f);
  makeCell->products.emplace_back(cell, 1.0f);
  Recipe* burn = Add<Recipe>(objects, "burn");
  burn->time = 1;
  burn->products.emplace_back(heat, 1.0f);
  Recipe* reprocess = Add<Recipe>(objects, "reprocess");
  reprocess->ingredients.emplace_back(spent, 1.0f);
  reprocess->products.emplace_back(ore, 0.5f);
  auto* reactor = Add<EntityCrafter>(objects, "reactor");
  reactor->baseCraftingSpeed = 1;
  reactor->basePower = 1;  // MW; fuelValue 1 -> 1 cell/s per building
  reactor->hasEnergy = true;
  reactor->energy.fuels.push_back(cell);
  Database db;
  db.LoadBuiltData(std::move(objects));

  ProductionTable root;
  root.AddLink({heat, nullptr}, 10);
  ProductionLink* cellLink = root.AddLink({cell, nullptr});
  ProductionLink* spentLink = root.AddLink({spent, nullptr});
  root.AddLink({ore, nullptr});
  root.AddRecipe(mine);
  RecipeRow* makeRow = root.AddRecipe(makeCell);
  RecipeRow* burnRow = root.AddRecipe(burn);
  burnRow->entity = {reactor, nullptr};
  burnRow->fuel = {cell, nullptr};
  RecipeRow* reprocessRow = root.AddRecipe(reprocess);

  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(burnRow->recipesPerSecond == doctest::Approx(10));
  CHECK(makeRow->recipesPerSecond == doctest::Approx(10));
  CHECK(reprocessRow->recipesPerSecond == doctest::Approx(10));
  // Spent cells are produced by burning, consumed by reprocessing: matched.
  CHECK((spentLink->flags & LinkFlags::kLinkNotMatched) == 0);
  CHECK(spentLink->linkFlow == doctest::Approx(10));
  CHECK((cellLink->flags & LinkFlags::kLinkNotMatched) == 0);
  CHECK(cellLink->linkFlow == doctest::Approx(10));
  // No deadlock warnings anywhere.
  CHECK((burnRow->parameters.warningFlags & WarningFlags::kDeadlockCandidate) == 0);
}

TEST_CASE("disabled rows are excluded from the solve") {
  Goods2 g;
  ProductionTable root;
  root.AddLink({g.ore, nullptr});
  root.AddLink({g.plate, nullptr}, 10);

  root.AddRecipe(g.mine);
  RecipeRow* cheat = root.AddRecipe(g.mine);  // second miner, disabled
  cheat->enabled = false;
  root.AddRecipe(g.smelt);

  REQUIRE(root.Solve() == TableSolveResult::Ok);
  CHECK(cheat->recipesPerSecond == 0.0);
  CHECK(!cheat->hierarchyEnabled);
  CHECK(root.recipes[0]->recipesPerSecond == doctest::Approx(10));
}
