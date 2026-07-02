#include "yafc/analysis/cost_analysis.h"

#include <cmath>
#include <memory>
#include <vector>

#include "doctest/doctest.h"

using namespace yafc;

namespace {

template <typename T>
T* Add(std::vector<std::unique_ptr<FactorioObject>>& objects, const char* name,
       const char* factorioType = "") {
  auto obj = std::make_unique<T>();
  obj->name = name;
  obj->factorioType = factorioType;
  T* raw = obj.get();
  objects.push_back(std::move(obj));
  return raw;
}

// Synthetic economy: ore patch -> mine -> smelt -> science pack -> technology,
// plus a wasteful alternative smelt, a fluid temperature chain, and a
// placeable furnace.
struct Economy {
  Database db;
  Entity* orePatch;
  EntityCrafter* miner;
  EntityCrafter* furnace;
  Item* ore;
  Item* plate;
  Item* pack;
  Item* furnaceItem;
  Fluid* water15;
  Fluid* water50;
  Recipe* mine;
  Recipe* smelt;
  Recipe* wasteful;
  Recipe* makePack;
  Recipe* buildFurnace;
  Recipe* pump;
  Recipe* heatWater;
  Technology* tech;

  Economy() {
    std::vector<std::unique_ptr<FactorioObject>> objects;
    orePatch = Add<Entity>(objects, "ore-patch");
    miner = Add<EntityCrafter>(objects, "miner", "mining-drill");
    furnace = Add<EntityCrafter>(objects, "furnace", "furnace");
    ore = Add<Item>(objects, "ore");
    plate = Add<Item>(objects, "plate");
    pack = Add<Item>(objects, "pack");
    furnaceItem = Add<Item>(objects, "furnace-item");
    water15 = Add<Fluid>(objects, "water@15");
    water50 = Add<Fluid>(objects, "water@50");
    mine = Add<Recipe>(objects, "mine-ore");
    smelt = Add<Recipe>(objects, "smelt");
    wasteful = Add<Recipe>(objects, "wasteful-smelt");
    makePack = Add<Recipe>(objects, "make-pack");
    buildFurnace = Add<Recipe>(objects, "build-furnace");
    pump = Add<Recipe>(objects, "pump-water");
    heatWater = Add<Recipe>(objects, "heat-water");
    tech = Add<Technology>(objects, "automation");

    orePatch->mapGenerated = true;
    orePatch->mapGenDensity = 4000;
    orePatch->loot.emplace_back(ore, 1.0f);
    ore->miscSources = {orePatch};

    miner->size = 3;
    furnace->size = 3;
    miner->energy.type = EntityEnergyType::Void;
    furnace->energy.type = EntityEnergyType::Void;
    furnace->itemsToPlace = {furnaceItem};
    water15->temperature = 15;
    water50->temperature = 50;

    auto recipe = [&](Recipe* r, std::vector<Ingredient> in, std::vector<Product> out,
                      EntityCrafter* crafter, float time) {
      r->ingredients = std::move(in);
      r->products = std::move(out);
      r->crafters = {crafter};
      r->time = time;
    };
    recipe(mine, {}, {{ore, 1.0f}}, miner, 1);
    mine->sourceEntity = orePatch;
    recipe(smelt, {{ore, 1.0f}}, {{plate, 1.0f}}, furnace, 2);
    recipe(wasteful, {{ore, 10.0f}}, {{plate, 1.0f}}, furnace, 2);
    recipe(makePack, {{plate, 1.0f}}, {{pack, 1.0f}}, furnace, 1);
    recipe(buildFurnace, {{plate, 1.0f}}, {{furnaceItem, 1.0f}}, furnace, 1);
    recipe(pump, {}, {{water15, 1.0f}}, miner, 1);
    recipe(heatWater, {{water15, 1.0f}}, {{water50, 1.0f}}, furnace, 1);

    tech->ingredients.emplace_back(pack, 1.0f);
    tech->count = 100;

    plate->usages = {makePack, buildFurnace};
    ore->usages = {smelt, wasteful};

    db.LoadBuiltData(std::move(objects));
    db.fluidVariants["water"] = {water15, water50};
  }
};

}  // namespace

TEST_CASE("cost analysis solves and orders costs along the chain") {
  Economy e;
  CostAnalysis analysis(false);
  REQUIRE(analysis.Compute(e.db, {}));

  // Costs are positive, finite, and grow along the production chain.
  CHECK(analysis.cost[e.ore] > 0);
  CHECK(std::isfinite(analysis.cost[e.ore]));
  CHECK(analysis.cost[e.plate] > analysis.cost[e.ore]);
  CHECK(analysis.cost[e.pack] > analysis.cost[e.plate]);

  // The efficient smelt is on the cost frontier (zero waste); the 10-ore
  // variant wastes most of its input cost.
  CHECK(analysis.recipeWastePercentage[e.smelt] == doctest::Approx(0.0f).epsilon(1e-3));
  CHECK(analysis.recipeWastePercentage[e.wasteful] > 0.5f);

  // Science demand pulls flow through the pack chain (constraint duals).
  CHECK(analysis.flow[e.makePack] > 0);
  CHECK(analysis.flow[e.pack] > 0);

  // Technology cost = sum of its ingredient costs.
  CHECK(analysis.cost[e.tech] == doctest::Approx(analysis.cost[e.pack]));
  // Recipe product cost mapping.
  CHECK(analysis.recipeProductCost[e.smelt] == doctest::Approx(analysis.cost[e.plate]));
  // A placeable entity costs as much as its cheapest placing item.
  CHECK(analysis.cost[e.furnace] == doctest::Approx(analysis.cost[e.furnaceItem]));

  // Fluid temperature chain: lower temp never costs more than higher.
  CHECK(analysis.cost[e.water15] <= analysis.cost[e.water50] + 1e-6f);

  // Important items: plate and ore have multiple usages; ranking exists.
  REQUIRE(analysis.importantItems.size() == 2);
}

TEST_CASE("non-automatable objects get infinite cost") {
  Economy e;
  CostAnalysisInput input;
  input.access.isAutomatable = [&](const FactorioObject* o) {
    return o != e.water50;  // pretend hot water cannot be automated
  };
  CostAnalysis analysis(false);
  REQUIRE(analysis.Compute(e.db, input));
  CHECK(analysis.cost[e.water50] == std::numeric_limits<float>::infinity());
  CHECK(std::isfinite(analysis.cost[e.plate]));
}

TEST_CASE("excluded objects are ignored by the analysis") {
  Economy e;
  CostAnalysisInput input;
  input.excludedObjects.insert(e.wasteful);
  CostAnalysis analysis(false);
  REQUIRE(analysis.Compute(e.db, input));
  // Excluded recipe got no constraint and no cost accumulation.
  CHECK(analysis.recipeCost[e.wasteful] == 0.0f);
  CHECK(analysis.recipeWastePercentage[e.wasteful] == 0.0f);
  // The rest of the economy is unaffected.
  CHECK(std::isfinite(analysis.cost[e.plate]));
  CHECK(analysis.cost[e.plate] > analysis.cost[e.ore]);
}

TEST_CASE("mining rarity penalty raises ore cost") {
  Economy scarce;
  scarce.orePatch->mapGenDensity = 20;  // far below the 2000 penalty threshold
  CostAnalysis a1(false);
  REQUIRE(a1.Compute(scarce.db, {}));

  Economy rich;
  CostAnalysis a2(false);
  REQUIRE(a2.Compute(rich.db, {}));

  CHECK(a1.cost[scarce.ore] > a2.cost[rich.ore]);
}
