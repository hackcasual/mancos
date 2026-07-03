// Port of Yafc.Model/Analysis/TechnologyScienceAnalysis.cs — for every
// technology, the total science-pack amounts needed to research it and its
// entire prerequisite tree.
#pragma once

#include <unordered_set>
#include <vector>

#include "yafc/analysis/milestones.h"
#include "yafc/model/database.h"

namespace yafc {

class TechnologyScienceAnalysis {
 public:
  void Compute(const Database& db, const Dependencies& deps,
               const std::unordered_set<const FactorioObject*>& excluded = {});

  // Science packs (goods+amount) required for each tech incl. prerequisites.
  Mapping<Technology, std::vector<Ingredient>> allSciencePacks;

  // The pack whose milestone mask ranks highest (i.e. the latest-game pack).
  const Ingredient* GetMaxTechnologyIngredient(const Milestones& milestones,
                                               const Technology* tech) const;
};

}  // namespace yafc
