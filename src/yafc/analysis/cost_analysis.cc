#include "yafc/analysis/cost_analysis.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

#include "yafc/analysis/automation_analysis.h"
#include "yafc/analysis/milestones.h"
#include "yafc/solver/lp.h"

namespace yafc {

AccessibilityHooks HooksFromAnalyses(const Milestones& milestones,
                                     const AutomationAnalysis& automation) {
  AccessibilityHooks hooks;
  hooks.isAccessible = [&milestones](const FactorioObject* o) {
    return milestones.IsAccessible(o);
  };
  hooks.isAutomatable = [&automation](const FactorioObject* o) {
    return automation.IsAutomatable(o);
  };
  hooks.isAutomatableWithCurrentMilestones = [&automation](const FactorioObject* o) {
    return automation.IsAutomatableNow(o);
  };
  hooks.isAccessibleAtNextMilestone = [&milestones](const FactorioObject* o) {
    return milestones.IsAccessibleAtNextMilestone(o);
  };
  return hooks;
}

namespace {
// Upstream tuning constants (CostAnalysis.cs).
constexpr float kCostPerSecond = 0.1f;
constexpr float kCostPerMj = 0.1f;
constexpr float kCostPerIngredientPerSize = 0.1f;
constexpr float kCostPerProductPerSize = 0.2f;
constexpr float kCostPerItem = 0.02f;
constexpr float kCostPerFluid = 0.0005f;
constexpr float kCostPerPollution = 0.01f;
constexpr float kCostLowerLimit = -10.0f;
constexpr float kCostLimitWhenGeneratesOnMap = 1e4f;
constexpr float kMiningPenalty = 1.0f;
constexpr float kMiningMaxDensityForPenalty = 2000.0f;
constexpr float kMiningMaxExtraPenaltyForRarity = 10.0f;

constexpr float kInf = std::numeric_limits<float>::infinity();
}  // namespace

bool CostAnalysis::Compute(Database& db, const CostAnalysisInput& input) {
  const AccessibilityHooks& access = input.access;
  auto excluded = [&](const FactorioObject* o) {
    return input.excludedObjects.count(o) != 0;
  };

  LpSolver solver;  // DataUtils.CreateSolver
  solver.SetMaximization();

  // Best accessible container density, for spoilage recipe costs.
  float bestContainerSlotsPerTile = 1.0f;
  for (const EntityContainer* container : db.allContainers) {
    float area = static_cast<float>(container->width) * container->height;
    if (ShouldInclude(container, access) && area > 0) {
      bestContainerSlotsPerTile =
          std::max(bestContainerSlotsPerTile, container->inventorySize / area);
    }
  }

  auto variables = db.goods.CreateMapping<int>();
  auto constraints = db.recipes.CreateMapping<int>();
  std::fill(variables.values().begin(), variables.values().end(), -1);
  std::fill(constraints.values().begin(), constraints.values().end(), -1);

  // Science pack usage over all accessible technologies. (Upstream also has a
  // targetTechnology mode via TechnologyScienceAnalysis — not ported yet.)
  std::unordered_map<Goods*, float> sciencePackUsage;
  for (Technology* technology : db.technologies) {
    if (excluded(technology) || !access.Accessible(technology)) continue;
    for (const Ingredient& ingredient : technology->ingredients) {
      if (!access.Automatable(ingredient.goods)) continue;
      if (onlyCurrentMilestones_ &&
          !access.AccessibleAtNextMilestone(ingredient.goods)) {
        continue;
      }
      sciencePackUsage[ingredient.goods] += ingredient.amount * technology->count;
    }
  }

  for (Goods* goods : db.goods) {
    if (excluded(goods) || !ShouldInclude(goods, access)) continue;

    float mapGeneratedAmount = 0.0f;
    for (FactorioObject* src : goods->miscSources) {
      if (auto* ent = dynamic_cast<Entity*>(src); ent && ent->mapGenerated) {
        for (const Product& product : ent->loot) {
          if (product.goods == goods) mapGeneratedAmount += product.amount;
        }
      }
    }
    double upperBound = mapGeneratedAmount > 0
                            ? kCostLimitWhenGeneratesOnMap / mapGeneratedAmount
                            : kInfinity;
    int variable = solver.MakeNumVar(kCostLowerLimit, upperBound, goods->name);
    // Small objective weight so goods not needed for science still get costs.
    solver.SetObjectiveCoefficient(variable, 1e-3);
    variables[goods] = variable;
  }

  for (const auto& [item, count] : sciencePackUsage) {
    if (variables[item] >= 0) {
      solver.SetObjectiveCoefficient(variables[item], count / 1000.0f);
    }
  }

  auto exportCost = db.objects.CreateMapping<float>();
  auto recipeProductionCost = db.recipesAndTechnologies.CreateMapping<float>();
  recipeCost = db.recipes.CreateMapping<float>();
  flow = db.objects.CreateMapping<float>();

  // Coefficient accumulation replaces upstream's SetCoefficientCheck (same
  // goods appearing among both products and ingredients must sum).
  std::map<std::pair<int, int>, double> coefficients;  // (constraint, var)

  for (Recipe* recipe : db.recipes) {
    if (excluded(recipe) || !ShouldInclude(recipe, access)) continue;
    if (onlyCurrentMilestones_ && !access.AutomatableNow(recipe)) continue;

    // Fuel selection: only when every candidate crafter agrees on one fuel.
    Goods* singleUsedFuel = nullptr;
    float singleUsedFuelAmount = 0.0f;
    float minEmissions = 100.0f;
    int minSize = 15;
    float minPower = 1000.0f;

    for (const EntityCrafter* crafter : recipe->crafters) {
      for (const auto& [_, e] : crafter->energy.emissions) {
        minEmissions = std::min(e, minEmissions);
      }
      if (crafter->energy.type == EntityEnergyType::Heat) break;  // upstream quirk
      if (crafter->size < minSize) minSize = crafter->size;

      float power = crafter->energy.type == EntityEnergyType::Void
                        ? 0.0f
                        : recipe->time * crafter->basePower /
                              (crafter->baseCraftingSpeed * crafter->energy.effectivity);
      if (power < minPower) minPower = power;

      for (Goods* fuel : crafter->energy.fuels) {
        if (!ShouldInclude(fuel, access)) continue;
        if (fuel->fuelValue <= 0.0f) {
          singleUsedFuel = nullptr;
          break;
        }
        float amount = power / fuel->fuelValue;
        if (singleUsedFuel == nullptr) {
          singleUsedFuel = fuel;
          singleUsedFuelAmount = amount;
        } else if (singleUsedFuel == fuel) {
          singleUsedFuelAmount = std::min(singleUsedFuelAmount, amount);
        } else {
          singleUsedFuel = nullptr;
          break;
        }
      }
      if (singleUsedFuel == nullptr) break;
    }

    if (minPower < 0.0f) minPower = 0.0f;
    int size = std::max(
        minSize, static_cast<int>(recipe->ingredients.size() + recipe->products.size()) / 2);
    float sizeUsage = kCostPerSecond * recipe->time * size;
    float logisticsCost =
        sizeUsage * (1.0f + kCostPerIngredientPerSize * recipe->ingredients.size() +
                     kCostPerProductPerSize * recipe->products.size()) +
        kCostPerMj * minPower;

    // Spoilage recipes: cost reflects parallel spoiling in storage.
    if (dynamic_cast<Mechanics*>(recipe) != nullptr &&
        recipe->name.starts_with("spoil.") && recipe->ingredients.size() == 1) {
      if (auto* spoilingItem = dynamic_cast<Item*>(recipe->ingredients[0].goods)) {
        logisticsCost = kCostPerSecond * recipe->time /
                        (spoilingItem->stackSize * bestContainerSlotsPerTile);
      }
    }

    if (singleUsedFuel != nullptr && singleUsedFuel->isPower()) singleUsedFuel = nullptr;

    int constraint = solver.MakeConstraint(-kInfinity, 0, recipe->name);
    constraints[recipe] = constraint;

    for (const Product& product : recipe->products) {
      if (variables[product.goods] < 0) continue;
      coefficients[{constraint, variables[product.goods]}] += product.amount;
      if (dynamic_cast<Item*>(product.goods)) {
        logisticsCost += product.amount * kCostPerItem;
      } else if (dynamic_cast<Fluid*>(product.goods)) {
        logisticsCost += product.amount * kCostPerFluid;
      }
    }

    if (singleUsedFuel != nullptr && variables[singleUsedFuel] >= 0) {
      coefficients[{constraint, variables[singleUsedFuel]}] -= singleUsedFuelAmount;
    }

    for (const Ingredient& ingredient : recipe->ingredients) {
      if (variables[ingredient.goods] >= 0) {
        coefficients[{constraint, variables[ingredient.goods]}] -= ingredient.amount;
      }
      if (dynamic_cast<Item*>(ingredient.goods)) {
        logisticsCost += ingredient.amount * kCostPerItem;
      } else if (dynamic_cast<Fluid*>(ingredient.goods)) {
        logisticsCost += ingredient.amount * kCostPerFluid;
      }
    }

    if (recipe->sourceEntity != nullptr && recipe->sourceEntity->mapGenerated) {
      float totalMining = 0.0f;
      for (const Product& product : recipe->products) totalMining += product.amount;
      float miningPenalty = kMiningPenalty;
      float totalDensity = recipe->sourceEntity->mapGenDensity / totalMining;
      if (totalDensity < kMiningMaxDensityForPenalty) {
        float extraPenalty = std::log(kMiningMaxDensityForPenalty / totalDensity);
        miningPenalty += std::min(extraPenalty, kMiningMaxExtraPenaltyForRarity);
      }
      logisticsCost *= miningPenalty;
    }

    if (minEmissions >= 0.0f) {
      logisticsCost +=
          minEmissions * kCostPerPollution * recipe->time * input.pollutionCostModifier;
    }

    solver.SetConstraintBounds(constraint, -kInfinity, logisticsCost);  // SetUb
    exportCost[recipe] = logisticsCost;
    recipeCost[recipe] = logisticsCost;
  }

  // "Strange item sources": an item cannot cost more than its goods source.
  for (Item* item : db.items) {
    if (excluded(item) || !ShouldInclude(item, access)) continue;
    for (FactorioObject* source : item->miscSources) {
      auto* g = dynamic_cast<Goods*>(source);
      if (g != nullptr && ShouldInclude(g, access) && variables[g] >= 0 &&
          variables[item] >= 0) {
        int constraint = solver.MakeConstraint(-kInfinity, 0, "source-" + item->name);
        solver.SetCoefficient(constraint, variables[g], -1);
        solver.SetCoefficient(constraint, variables[item], 1);
      }
    }
  }

  // Fluid temperature chains: lower temperature cannot cost more than higher.
  auto chainVariants = [&](const auto& fluids) {
    for (size_t i = 1; i < fluids.size(); ++i) {
      Goods* prev = fluids[i - 1];
      Goods* cur = fluids[i];
      if (variables[prev] < 0 || variables[cur] < 0) continue;
      int constraint = solver.MakeConstraint(-kInfinity, 0, "variant-" + prev->name);
      solver.SetCoefficient(constraint, variables[prev], 1);
      solver.SetCoefficient(constraint, variables[cur], -1);
    }
  };
  for (const auto& [name, fluids] : db.fluidVariants) chainVariants(fluids);
  if (!db.heatVariants.empty()) chainVariants(db.heatVariants);

  for (const auto& [key, amount] : coefficients) {
    solver.SetCoefficient(key.first, key.second, amount);
  }

  LpResult result = solver.SolveWithDifferentSeeds();
  bool solved = result == LpResult::Optimal || result == LpResult::Feasible;

  if (solved) {
    for (Goods* g : db.goods) {
      if (excluded(g) || variables[g] < 0) continue;
      exportCost[g] = static_cast<float>(solver.SolutionValue(variables[g]));
    }
    for (Recipe* recipe : db.recipes) {
      if (excluded(recipe) || constraints[recipe] < 0) continue;
      float recipeFlow = static_cast<float>(solver.DualValue(constraints[recipe]));
      if (recipeFlow > 0.0f) {
        flow[recipe] = recipeFlow;
        for (const Product& product : recipe->products) {
          flow[product.goods] += recipeFlow * product.amount;
        }
      }
    }
  }

  for (FactorioObject* o : db.objects) {
    if (excluded(o)) continue;
    if (!ShouldInclude(o, access)) {
      exportCost[o] = kInf;
      continue;
    }
    if (auto* recipe = dynamic_cast<RecipeOrTechnology*>(o)) {
      for (const Ingredient& ingredient : recipe->ingredients) {
        exportCost[o] += exportCost[ingredient.goods] * ingredient.amount;
      }
      for (const Product& product : recipe->products) {
        recipeProductionCost[recipe] += product.amount * exportCost[product.goods];
      }
    } else if (auto* entity = dynamic_cast<Entity*>(o)) {
      float minimal = kInf;
      for (Item* item : entity->itemsToPlace) {
        minimal = std::min(minimal, exportCost[item]);
      }
      exportCost[o] = minimal;
    }
  }
  cost = exportCost;
  recipeProductCost = recipeProductionCost;

  recipeWastePercentage = db.recipes.CreateMapping<float>();
  if (solved) {
    for (Recipe* recipe : db.recipes) {
      if (excluded(recipe) || constraints[recipe] < 0) continue;
      float productCost = 0.0f;
      for (const Product& product : recipe->products) {
        productCost += product.amount * exportCost[product.goods];
      }
      recipeWastePercentage[recipe] = 1.0f - productCost / exportCost[recipe];
    }
  }

  importantItems.clear();
  for (Goods* g : db.goods) {
    if (!excluded(g) && g->usages.size() > 1) importantItems.push_back(g);
  }
  auto importance = [&](Goods* x) {
    int usefulUsages = 0;
    for (Recipe* y : x->usages) {
      if (ShouldInclude(y, access) && recipeWastePercentage[y] == 0.0f) ++usefulUsages;
    }
    return flow[x] * cost[x] * usefulUsages;
  };
  std::stable_sort(importantItems.begin(), importantItems.end(),
                   [&](Goods* a, Goods* b) { return importance(a) > importance(b); });

  return solved;
}

}  // namespace yafc
