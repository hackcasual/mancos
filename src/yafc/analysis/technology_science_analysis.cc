#include "yafc/analysis/technology_science_analysis.h"

#include <deque>

namespace yafc {

void TechnologyScienceAnalysis::Compute(
    const Database& db, const Dependencies& deps,
    const std::unordered_set<const FactorioObject*>& excluded) {
  const std::vector<Item*>& sciencePacks = db.allSciencePacks;
  auto sciencePackIndex = db.goods.CreateMapping<int>();
  for (size_t i = 0; i < sciencePacks.size(); ++i) {
    sciencePackIndex[sciencePacks[i]] = static_cast<int>(i);
  }

  std::vector<Mapping<Technology, float>> sciencePackCount(sciencePacks.size());
  for (auto& mapping : sciencePackCount) {
    mapping = db.technologies.CreateMapping<float>();
  }

  auto processing = db.technologies.CreateMapping<bool>();
  auto requirementMap = db.technologies.CreateMapping<Technology, bool>(db.technologies);
  std::deque<Technology*> queue;

  for (Technology* tech : db.technologies) {
    if (excluded.count(tech)) continue;
    if (tech->prerequisites.empty()) {
      processing[tech] = true;
      queue.push_back(tech);
    }
  }

  std::deque<Technology*> prerequisiteQueue;
  while (!queue.empty()) {
    Technology* current = queue.front();
    queue.pop_front();

    // First prerequisite: bulk-copy its totals, then walk the remainder.
    if (!current->prerequisites.empty()) {
      Technology* firstRequirement = current->prerequisites[0];
      for (auto& pack : sciencePackCount) {
        pack[current] += pack[firstRequirement];
      }
      requirementMap.CopyRow(firstRequirement, current);
    }

    requirementMap(current, current) = true;
    prerequisiteQueue.push_back(current);

    while (!prerequisiteQueue.empty()) {
      Technology* prerequisite = prerequisiteQueue.front();
      prerequisiteQueue.pop_front();

      if (!(prerequisite->flags & RecipeFlags::kHasResearchTriggerMask)) {
        for (const Ingredient& ingredient : prerequisite->ingredients) {
          int science = sciencePackIndex[ingredient.goods];
          sciencePackCount[science][current] += ingredient.amount * prerequisite->count;
        }
      }

      for (Technology* prereqPrereq : prerequisite->prerequisites) {
        if (!requirementMap(current, prereqPrereq)) {
          prerequisiteQueue.push_back(prereqPrereq);
          requirementMap(current, prereqPrereq) = true;
        }
      }
    }

    // Unlock technologies whose prerequisites are now all processed.
    for (FactorioId unlockId : deps.reverseDependencies[current]) {
      auto* tech = dynamic_cast<Technology*>(db.objects.ById(unlockId));
      if (tech == nullptr || processing[tech]) continue;
      bool ready = true;
      for (Technology* prereq : tech->prerequisites) {
        if (!processing[prereq]) {
          ready = false;
          break;
        }
      }
      if (ready) {
        processing[tech] = true;
        queue.push_back(tech);
      }
    }
  }

  allSciencePacks = db.technologies.CreateMapping<std::vector<Ingredient>>();
  for (Technology* tech : db.technologies) {
    std::vector<Ingredient> packs;
    for (size_t i = 0; i < sciencePacks.size(); ++i) {
      if (sciencePackCount[i][tech] != 0) {
        packs.emplace_back(sciencePacks[i], sciencePackCount[i][tech]);
      }
    }
    allSciencePacks[tech] = std::move(packs);
  }
}

const Ingredient* TechnologyScienceAnalysis::GetMaxTechnologyIngredient(
    const Milestones& milestones, const Technology* tech) const {
  const std::vector<Ingredient>& list = allSciencePacks[tech];
  const Ingredient* ingredient = nullptr;
  Bits order;
  for (const Ingredient& entry : list) {
    Bits entryOrder = milestones.GetMilestoneResult(entry.goods->id);
    if (!(entryOrder == 0)) entryOrder -= 1;
    if (ingredient == nullptr || entryOrder > order) {
      order = entryOrder;
      ingredient = &entry;
    }
  }
  return ingredient;
}

}  // namespace yafc
