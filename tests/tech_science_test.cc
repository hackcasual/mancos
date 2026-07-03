#include "yafc/analysis/technology_science_analysis.h"

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

// Tech tree: A (20x pack1) <- B (5x pack1 + 15x pack2) <- C (2x pack2),
// where C also lists A directly as a prerequisite.
struct TechWorld {
  Database db;
  Dependencies deps;
  EntityCrafter* lab;
  EntityCrafter* assembler;
  Item* pack1;
  Item* pack2;
  Recipe* makePack1;
  Recipe* makePack2;
  Technology* a;
  Technology* b;
  Technology* c;

  TechWorld() {
    std::vector<std::unique_ptr<FactorioObject>> objects;
    lab = Add<EntityCrafter>(objects, "lab", "lab");
    assembler = Add<EntityCrafter>(objects, "assembler", "assembling-machine");
    pack1 = Add<Item>(objects, "pack1");
    pack2 = Add<Item>(objects, "pack2");
    makePack1 = Add<Recipe>(objects, "make-pack1");
    makePack2 = Add<Recipe>(objects, "make-pack2");
    a = Add<Technology>(objects, "tech-a");
    b = Add<Technology>(objects, "tech-b");
    c = Add<Technology>(objects, "tech-c");

    lab->inputs = {pack1, pack2};

    makePack1->products.emplace_back(pack1, 1.0f);
    makePack1->crafters = {assembler};
    makePack1->enabled = true;
    makePack2->products.emplace_back(pack2, 1.0f);
    makePack2->crafters = {assembler};
    makePack2->enabled = true;
    pack1->production = {makePack1};
    pack2->production = {makePack2};

    auto tech = [this](Technology* t, std::vector<Ingredient> in, float count,
                       std::vector<Technology*> prereq) {
      t->ingredients = std::move(in);
      t->count = count;
      t->prerequisites = std::move(prereq);
      t->enabled = true;
      t->crafters = {lab};
    };
    tech(a, {{pack1, 2.0f}}, 10, {});
    tech(b, {{pack1, 1.0f}, {pack2, 3.0f}}, 5, {a});
    tech(c, {{pack2, 1.0f}}, 2, {b, a});

    db.LoadBuiltData(std::move(objects));
    db.rootAccessible = {assembler, lab};
    deps.Calculate(db);
  }
};

}  // namespace

TEST_CASE("science pack totals accumulate over the prerequisite tree once") {
  TechWorld w;
  TechnologyScienceAnalysis analysis;
  analysis.Compute(w.db, w.deps);

  // A: 2*10 pack1.
  REQUIRE(analysis.allSciencePacks[w.a].size() == 1);
  CHECK(analysis.allSciencePacks[w.a][0].goods == w.pack1);
  CHECK(analysis.allSciencePacks[w.a][0].amount == doctest::Approx(20));

  // B: pack1 = 20 (A) + 5 (B itself); pack2 = 15.
  REQUIRE(analysis.allSciencePacks[w.b].size() == 2);
  CHECK(analysis.allSciencePacks[w.b][0].amount == doctest::Approx(25));
  CHECK(analysis.allSciencePacks[w.b][1].amount == doctest::Approx(15));

  // C: A must be counted once even though it is reachable via both edges:
  // pack1 = 25, pack2 = 15 + 2.
  REQUIRE(analysis.allSciencePacks[w.c].size() == 2);
  CHECK(analysis.allSciencePacks[w.c][0].amount == doctest::Approx(25));
  CHECK(analysis.allSciencePacks[w.c][1].amount == doctest::Approx(17));
}

TEST_CASE("GetMaxTechnologyIngredient prefers the later-milestone pack") {
  TechWorld w;
  TechnologyScienceAnalysis analysis;
  analysis.Compute(w.db, w.deps);

  Milestones ms;
  MilestonesInput input;
  input.milestones = {w.pack2};  // pack2 is the "late game" milestone
  input.autoSort = false;
  ms.Compute(w.db, w.deps, input);

  const Ingredient* max = analysis.GetMaxTechnologyIngredient(ms, w.b);
  REQUIRE(max != nullptr);
  CHECK(max->goods == w.pack2);
}
