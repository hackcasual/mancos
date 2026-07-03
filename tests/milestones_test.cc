// Tests for the dependency graph, Milestones and AutomationAnalysis ports,
// plus their integration with CostAnalysis through HooksFromAnalyses.
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "doctest/doctest.h"
#include "yafc/analysis/automation_analysis.h"
#include "yafc/analysis/cost_analysis.h"
#include "yafc/analysis/dependencies.h"
#include "yafc/analysis/milestones.h"
#include "yafc/model/bits.h"

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

// ore -> plate -> science pack -> tech; tech unlocks adv-smelt; a hand-only
// recipe (character crafter); a "late machine" placeable only after the tech.
struct World {
  Database db;
  Dependencies deps;
  Entity* orePatch;
  EntityCrafter* character;
  EntityCrafter* miner;
  EntityCrafter* furnace;
  EntityCrafter* lateMachine;
  EntityCrafter* lab;
  Item* ore;
  Item* plate;
  Item* pack;
  Item* handItem;
  Item* lateItem;
  Recipe* mine;
  Recipe* smelt;
  Recipe* makePack;
  Recipe* advSmelt;
  Recipe* handOnly;
  Recipe* makeLate;
  Recipe* lateRecipe;
  Technology* tech;

  World() {
    std::vector<std::unique_ptr<FactorioObject>> objects;
    orePatch = Add<Entity>(objects, "ore-patch");
    character = Add<EntityCrafter>(objects, "character", "character");
    miner = Add<EntityCrafter>(objects, "miner", "mining-drill");
    furnace = Add<EntityCrafter>(objects, "furnace", "furnace");
    lateMachine = Add<EntityCrafter>(objects, "late-machine", "assembling-machine");
    lab = Add<EntityCrafter>(objects, "lab", "lab");
    ore = Add<Item>(objects, "ore");
    plate = Add<Item>(objects, "plate");
    pack = Add<Item>(objects, "pack");
    handItem = Add<Item>(objects, "hand-item");
    lateItem = Add<Item>(objects, "late-item");
    mine = Add<Recipe>(objects, "mine-ore");
    smelt = Add<Recipe>(objects, "smelt");
    makePack = Add<Recipe>(objects, "make-pack");
    advSmelt = Add<Recipe>(objects, "adv-smelt");
    handOnly = Add<Recipe>(objects, "hand-only");
    makeLate = Add<Recipe>(objects, "make-late-machine");
    lateRecipe = Add<Recipe>(objects, "late-recipe");
    tech = Add<Technology>(objects, "automation");

    auto recipe = [](Recipe* r, std::vector<Ingredient> in, std::vector<Product> out,
                     std::vector<EntityCrafter*> crafters, bool enabled = true) {
      r->ingredients = std::move(in);
      r->products = std::move(out);
      r->crafters = std::move(crafters);
      r->time = 1;
      r->enabled = enabled;
    };
    recipe(mine, {}, {{ore, 1.0f}}, {miner});
    mine->sourceEntity = orePatch;
    recipe(smelt, {{ore, 1.0f}}, {{plate, 1.0f}}, {furnace});
    recipe(makePack, {{plate, 1.0f}}, {{pack, 1.0f}}, {furnace});
    recipe(advSmelt, {{ore, 1.0f}}, {{plate, 2.0f}}, {furnace}, /*enabled=*/false);
    advSmelt->technologyUnlock = {tech};
    // Hand-crafting: the character is the only crafter.
    recipe(handOnly, {{plate, 1.0f}}, {{handItem, 1.0f}}, {character});
    recipe(makeLate, {{pack, 1.0f}}, {{lateItem, 1.0f}}, {furnace});
    recipe(lateRecipe, {{plate, 1.0f}}, {{handItem, 1.0f}}, {lateMachine});

    lab->inputs = {pack};
    tech->crafters = {lab};
    tech->ingredients.emplace_back(pack, 1.0f);
    tech->count = 10;
    tech->enabled = true;

    lateMachine->itemsToPlace = {lateItem};

    ore->production = {mine};
    plate->production = {smelt, advSmelt};
    pack->production = {makePack};
    handItem->production = {handOnly, lateRecipe};
    lateItem->production = {makeLate};
    ore->usages = {smelt, advSmelt};
    plate->usages = {makePack, handOnly, lateRecipe};
    pack->usages = {makeLate};

    db.LoadBuiltData(std::move(objects));
    db.rootAccessible = {orePatch, character, miner, furnace, lab};
    deps.Calculate(db);
  }
};

}  // namespace

TEST_CASE("dependency graph: forward trees and reverse index") {
  World w;
  // smelt requires ore (ingredient) and furnace (crafter).
  auto flat = w.deps.dependencyList[w.smelt].Flatten();
  CHECK(std::find(flat.begin(), flat.end(), w.ore->id) != flat.end());
  CHECK(std::find(flat.begin(), flat.end(), w.furnace->id) != flat.end());
  // ore is required by smelt and advSmelt (reverse deps include both).
  auto& rev = w.deps.reverseDependencies[w.ore];
  CHECK(std::find(rev.begin(), rev.end(), w.smelt->id) != rev.end());
  CHECK(std::find(rev.begin(), rev.end(), w.advSmelt->id) != rev.end());
}

TEST_CASE("milestones: accessibility, masks, locking, GetHighest") {
  World w;
  Milestones ms;
  MilestonesInput input;
  input.milestones = {w.pack};
  input.autoSort = false;
  auto warnings = ms.Compute(w.db, w.deps, input);

  CHECK(!warnings.mostObjectsInaccessible);
  CHECK(warnings.unreachableMilestones.empty());

  // Everything on the basic chain is accessible.
  for (FactorioObject* o : {(FactorioObject*)w.ore, (FactorioObject*)w.plate,
                            (FactorioObject*)w.pack, (FactorioObject*)w.tech,
                            (FactorioObject*)w.advSmelt, (FactorioObject*)w.lateMachine}) {
    CHECK(ms.IsAccessible(o));
  }

  // tech consumes the pack -> requires the pack milestone; plate does not.
  CHECK(ms.GetMilestoneResult(w.tech)[1]);
  CHECK(!ms.GetMilestoneResult(w.plate)[1]);
  // advSmelt is unlocked by tech -> inherits the pack milestone bit.
  CHECK(ms.GetMilestoneResult(w.advSmelt)[1]);

  // Pack not researched: tech unavailable now, plate available.
  CHECK(!ms.IsAccessibleWithCurrentMilestones(w.tech));
  CHECK(ms.IsAccessibleWithCurrentMilestones(w.plate));
  CHECK(ms.GetHighest(w.tech, /*all=*/true) == w.pack);

  // Research the pack milestone: tech becomes available now.
  input.unlockedMilestones = {w.pack};
  ms.Compute(w.db, w.deps, input);
  CHECK(ms.IsAccessibleWithCurrentMilestones(w.tech));

  // SetUnlocked flips the locked mask without re-running the walks.
  ms.SetUnlocked({});
  CHECK(!ms.IsAccessibleWithCurrentMilestones(w.tech));
  CHECK(ms.GetHighest(w.tech, /*all=*/false) == w.pack);
  ms.SetUnlocked({w.pack});
  CHECK(ms.IsAccessibleWithCurrentMilestones(w.tech));
}

TEST_CASE("milestones: marked-inaccessible objects stay out and get predictions") {
  World w;
  Milestones ms;
  MilestonesInput input;
  input.milestones = {w.pack};
  input.autoSort = false;
  input.markedInaccessible = {w.pack};
  ms.Compute(w.db, w.deps, input);

  CHECK(!ms.IsAccessible(w.pack));
  // tech requires pack -> also inaccessible; its predicted mask has bit0 clear.
  CHECK(!ms.IsAccessible(w.tech));
  CHECK(!ms.GetMilestoneResult(w.tech)[0]);
  // The ore/plate side is unaffected.
  CHECK(ms.IsAccessible(w.plate));
}

TEST_CASE("automation: hand-only recipes and late machines classify correctly") {
  World w;
  Milestones ms;
  MilestonesInput input;
  input.milestones = {w.pack};
  input.autoSort = false;
  ms.Compute(w.db, w.deps, input);

  AutomationAnalysis automation;
  automation.Compute(w.db, w.deps, ms);

  // The plate chain runs on machines available now.
  CHECK(automation.automatable[w.plate] == AutomationStatus::AutomatableNow);
  CHECK(automation.automatable[w.smelt] == AutomationStatus::AutomatableNow);

  // hand-only recipe has no machine crafters at all -> not automatable...
  CHECK(automation.automatable[w.handOnly] == AutomationStatus::NotAutomatable);
  // ...but handItem is also produced by lateRecipe on the late machine, which
  // needs the pack milestone -> automatable later, not now.
  CHECK(automation.automatable[w.lateRecipe] == AutomationStatus::AutomatableLater);
  CHECK(automation.automatable[w.handItem] == AutomationStatus::AutomatableLater);

  // After researching the milestone everything upgrades to AutomatableNow.
  input.unlockedMilestones = {w.pack};
  ms.Compute(w.db, w.deps, input);
  AutomationAnalysis automation2;
  automation2.Compute(w.db, w.deps, ms);
  CHECK(automation2.automatable[w.lateRecipe] == AutomationStatus::AutomatableNow);
  CHECK(automation2.automatable[w.handItem] == AutomationStatus::AutomatableNow);
}

TEST_CASE("cost analysis consumes the real analyses via hooks") {
  World w;
  Milestones ms;
  MilestonesInput input;
  input.milestones = {w.pack};
  input.autoSort = false;
  ms.Compute(w.db, w.deps, input);
  AutomationAnalysis automation;
  automation.Compute(w.db, w.deps, ms);

  CostAnalysisInput costInput;
  costInput.access = HooksFromAnalyses(ms, automation);
  CostAnalysis analysis(false);
  REQUIRE(analysis.Compute(w.db, costInput));

  CHECK(std::isfinite(analysis.cost[w.plate]));
  CHECK(analysis.cost[w.plate] > 0);
  // handItem is only automatable later; with the "all milestones" analysis it
  // still gets a finite cost (lateRecipe is automatable), but a hand-crafted
  // exclusive would be infinite. Make one: exclude lateRecipe.
  CostAnalysisInput exclusiveInput = costInput;
  exclusiveInput.access.isAutomatable = [&](const FactorioObject* o) {
    return automation.IsAutomatable(o) && o != w.lateRecipe && o != w.handItem;
  };
  CostAnalysis analysis2(false);
  REQUIRE(analysis2.Compute(w.db, exclusiveInput));
  CHECK(analysis2.cost[w.handItem] == std::numeric_limits<float>::infinity());
}
