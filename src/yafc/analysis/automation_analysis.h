// Port of Yafc.Model/Analysis/AutomationAnalysis.cs — classifies every object
// as NotAutomatable / AutomatableLater / AutomatableNow by propagating over
// the dependency graph (Milestones must be computed first).
#pragma once

#include <unordered_set>

#include "yafc/analysis/dependencies.h"
#include "yafc/analysis/milestones.h"
#include "yafc/model/database.h"

namespace yafc {

class AutomationAnalysis {
 public:
  void Compute(const Database& db, const Dependencies& deps,
               const Milestones& milestones,
               const std::unordered_set<const FactorioObject*>& excluded = {});

  Mapping<FactorioObject, AutomationStatus> automatable;

  bool IsAutomatable(const FactorioObject* obj) const {
    return automatable[obj] != AutomationStatus::NotAutomatable;
  }
  bool IsAutomatableNow(const FactorioObject* obj) const {
    return automatable[obj] == AutomationStatus::AutomatableNow;
  }
};

}  // namespace yafc
