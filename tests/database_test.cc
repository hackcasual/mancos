#include "yafc/model/database.h"

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

// A tiny synthetic game: ore -> plate smelting, a lab, a quality tier chain.
// Objects are added deliberately out of sort order to prove LoadBuiltData
// partitions ids correctly.
struct MiniGame {
  Database db;
  Item* ore;
  Item* plate;
  Item* pack;
  Module* speedModule;
  Fluid* water;
  Recipe* smelt;
  Technology* tech;
  EntityCrafter* furnace;
  EntityCrafter* lab;
  Quality* normal;
  Quality* uncommon;

  MiniGame() {
    std::vector<std::unique_ptr<FactorioObject>> objects;
    furnace = Add<EntityCrafter>(objects, "stone-furnace", "furnace");
    smelt = Add<Recipe>(objects, "iron-plate");
    normal = Add<Quality>(objects, "normal");
    ore = Add<Item>(objects, "iron-ore");
    water = Add<Fluid>(objects, "water");
    tech = Add<Technology>(objects, "automation");
    plate = Add<Item>(objects, "iron-plate");
    uncommon = Add<Quality>(objects, "uncommon");
    lab = Add<EntityCrafter>(objects, "lab", "lab");
    pack = Add<Item>(objects, "automation-science-pack");
    speedModule = Add<Module>(objects, "speed-module");

    normal->level = 0;
    uncommon->level = 1;
    normal->nextQuality = uncommon;
    uncommon->previousQuality = normal;

    smelt->ingredients.emplace_back(ore, 1.0f);
    smelt->products.emplace_back(plate, 1.0f);
    smelt->crafters.push_back(furnace);
    smelt->time = 3.2f;
    ore->production = {};
    plate->production = {smelt};
    ore->usages = {smelt};
    lab->inputs = {pack};

    db.LoadBuiltData(std::move(objects));
  }
};

}  // namespace

TEST_CASE("LoadBuiltData partitions ids into contiguous per-type ranges") {
  MiniGame g;

  // Sort order: SpecialGoods, Items, Fluids, Recipes, ..., Technologies,
  // Entities, ..., Qualities. Items got the first ids despite insertion order.
  CHECK(g.db.objects.count() == 11);
  CHECK(g.db.items.count() == 4);
  CHECK(g.db.fluids.count() == 1);
  CHECK(g.db.goods.count() == 5);
  CHECK(g.db.recipes.count() == 1);
  CHECK(g.db.technologies.count() == 1);
  CHECK(g.db.entities.count() == 2);
  CHECK(g.db.qualities.count() == 2);

  // Insertion order preserved within a type (stable sort): ore, plate, pack, module.
  CHECK(g.db.items[0] == g.ore);
  CHECK(g.db.items[1] == g.plate);
  CHECK(g.db.items[3] == g.speedModule);

  // goods spans specials+items+fluids contiguously; ById round-trips.
  for (Goods* good : g.db.goods) {
    CHECK(g.db.goods.ById(good->id) == good);
    CHECK(g.db.objects.ById(good->id) == good);
  }

  // Ranges see through to the same objects with the right static type.
  CHECK(g.db.recipes[0] == g.smelt);
  CHECK(g.db.recipes[0]->products[0].goods == g.plate);
}

TEST_CASE("typeDotName lookup and derived collections") {
  MiniGame g;

  CHECK(g.db.FindByTypeDotName("Item.iron-ore") == g.ore);
  CHECK(g.db.FindByTypeDotName("Recipe.iron-plate") == g.smelt);
  CHECK(g.db.FindByTypeDotName("Item.iron-plate") == g.plate);
  CHECK(g.db.FindByTypeDotName("Quality.normal") == g.normal);
  CHECK(g.db.FindByTypeDotName("Item.nope") == nullptr);

  CHECK(g.db.qualityNormal == g.normal);
  CHECK(g.db.allModules == std::vector<Module*>{g.speedModule});
  CHECK(g.db.allCrafters.size() == 2);
  CHECK(g.db.allSciencePacks == std::vector<Item*>{g.pack});
}

TEST_CASE("dense mappings index by object and by id") {
  MiniGame g;

  auto costs = g.db.goods.CreateMapping<float>();
  costs[g.ore] = 1.5f;
  costs[g.plate] = 4.0f;
  CHECK(costs[g.ore] == 1.5f);
  CHECK(costs.ById(g.plate->id) == 4.0f);
  CHECK(costs[g.water] == 0.0f);  // value-initialized

  auto matrix = g.db.recipes.CreateMapping<Goods, int>(g.db.goods);
  matrix(g.smelt, g.plate) = 7;
  CHECK(matrix(g.smelt, g.plate) == 7);
  CHECK(matrix(g.smelt, g.ore) == 0);

  costs.Clear();
  CHECK(costs[g.plate] == 0.0f);
}

TEST_CASE("quality bonus math matches upstream") {
  MiniGame g;
  CHECK(g.uncommon->ApplyStandardBonus(100) == doctest::Approx(130));
  CHECK(g.uncommon->ApplyAccumulatorCapacityBonus(100) == doctest::Approx(200));
  // Module bonus floors to hundredths: 25% -> 32%, not 32.5%.
  CHECK(g.uncommon->ApplyModuleBonus(0.25f) == doctest::Approx(0.32f));
  CHECK(g.normal->ApplyModuleBonus(0.25f) == doctest::Approx(0.25f));
  // Crafting speed: furnaces scale with quality, mining drills do not.
  g.furnace->baseCraftingSpeed = 2.0f;
  CHECK(g.furnace->CraftingSpeed(*g.uncommon) == doctest::Approx(2.6f));
  g.furnace->factorioType = "mining-drill";
  CHECK(g.furnace->CraftingSpeed(*g.uncommon) == doctest::Approx(2.0f));
}

TEST_CASE("product probability and catalyst math matches upstream") {
  Item goods;
  // 50% chance of 1-3 outputs: average 1.0
  Product p(&goods, 1, 3, 0.5f);
  CHECK(p.amount == doctest::Approx(1.0f));
  CHECK(p.GetAmountPerRecipe(0.0f) == doctest::Approx(1.0f));
  // With +100% productivity and no catalyst, doubles.
  CHECK(p.GetAmountPerRecipe(1.0f) == doctest::Approx(2.0f));
  // One unit is a catalyst: only the non-catalytic part scales.
  p.SetCatalyst(1.0f);
  CHECK(p.productivityAmount() == doctest::Approx(0.5f));  // (0+2)/2 * 0.5
  CHECK(p.GetAmountPerRecipe(1.0f) == doctest::Approx(1.5f));

  // ObjectWithQuality value semantics stand in for upstream reference equality.
  Quality q1, q2;
  CHECK(With(&goods, &q1) == With(&goods, &q1));
  CHECK(With(&goods, &q1) != With(&goods, &q2));
}
