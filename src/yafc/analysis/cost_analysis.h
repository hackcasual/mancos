// Port of Yafc.Model/Analysis/CostAnalysis.cs — estimates a "cost" for every
// object by solving an LP over the whole recipe graph (maximize goods costs
// subject to: products of a recipe cannot cost more than its ingredients plus
// the recipe's logistics cost), then derives flows (constraint duals), recipe
// waste percentages and the important-items ranking.
//
// Not yet ported (depends on analyses/model pieces that come later):
//  - Milestones / AutomationAnalysis: accessibility comes in through the
//    AccessibilityHooks seam (defaults: everything accessible/automatable).
//  - TechnologyScienceAnalysis targetTechnology mode (upstream: science pack
//    usage for one target tech instead of all accessible techs).
//  - GetDisplayCost/GetItemAmount (UI strings — the web layer owns i18n).
#pragma once

#include <functional>
#include <unordered_set>
#include <vector>

#include "yafc/model/database.h"

namespace yafc {

class Milestones;
class AutomationAnalysis;

// Seam for accessibility; defaults to all-accessible, or built from the
// Milestones + Automation analyses via HooksFromAnalyses.
struct AccessibilityHooks {
  std::function<bool(const FactorioObject*)> isAccessible;
  std::function<bool(const FactorioObject*)> isAutomatable;
  std::function<bool(const FactorioObject*)> isAutomatableWithCurrentMilestones;
  std::function<bool(const FactorioObject*)> isAccessibleAtNextMilestone;

  bool Accessible(const FactorioObject* o) const {
    return !isAccessible || isAccessible(o);
  }
  bool Automatable(const FactorioObject* o) const {
    return !isAutomatable || isAutomatable(o);
  }
  bool AutomatableNow(const FactorioObject* o) const {
    return !isAutomatableWithCurrentMilestones || isAutomatableWithCurrentMilestones(o);
  }
  bool AccessibleAtNextMilestone(const FactorioObject* o) const {
    return !isAccessibleAtNextMilestone || isAccessibleAtNextMilestone(o);
  }
};

AccessibilityHooks HooksFromAnalyses(const Milestones& milestones,
                                     const AutomationAnalysis& automation);

struct CostAnalysisInput {
  float pollutionCostModifier = 1.0f;  // project.settings.PollutionCostModifier
  std::unordered_set<const FactorioObject*> excludedObjects;  // Analysis exclusions
  AccessibilityHooks access;
};

class CostAnalysis {
 public:
  explicit CostAnalysis(bool onlyCurrentMilestones)
      : onlyCurrentMilestones_(onlyCurrentMilestones) {}

  // Returns false when the LP failed (upstream: analysis warning); cost
  // mappings are still populated (infinity for non-automatable objects).
  bool Compute(Database& db, const CostAnalysisInput& input);

  // Results (upstream mapping names).
  Mapping<FactorioObject, float> cost;
  Mapping<Recipe, float> recipeCost;
  Mapping<RecipeOrTechnology, float> recipeProductCost;
  Mapping<FactorioObject, float> flow;
  Mapping<Recipe, float> recipeWastePercentage;
  std::vector<Goods*> importantItems;

  static float GetBuildingHours(const Recipe* recipe, float flow) {
    return recipe->time * flow * (1000.0f / 3600.0f);
  }

 private:
  bool ShouldInclude(const FactorioObject* obj, const AccessibilityHooks& a) const {
    return onlyCurrentMilestones_ ? a.AutomatableNow(obj) : a.Automatable(obj);
  }

  bool onlyCurrentMilestones_;
};

}  // namespace yafc
