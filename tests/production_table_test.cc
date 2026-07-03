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
