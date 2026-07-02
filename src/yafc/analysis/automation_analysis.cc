#include "yafc/analysis/automation_analysis.h"

#include <deque>

namespace yafc {

void AutomationAnalysis::Compute(
    const Database& db, const Dependencies& deps, const Milestones& milestones,
    const std::unordered_set<const FactorioObject*>& excluded) {
  auto state = db.objects.CreateMapping<AutomationStatus>();
  if (db.voidEnergy != nullptr) {
    state[db.voidEnergy] = AutomationStatus::AutomatableNow;
  }

  std::deque<FactorioId> processingQueue;
  int unknowns = 0;

  for (Recipe* recipe : db.recipes) {
    if (excluded.count(recipe)) continue;
    bool hasAutomatableCrafter = false;
    for (EntityCrafter* crafter : recipe->crafters) {
      if (crafter != db.character && milestones.IsAccessible(crafter)) {
        hasAutomatableCrafter = true;
      }
    }
    if (!hasAutomatableCrafter) state[recipe] = AutomationStatus::NotAutomatable;
  }

  for (FactorioObject* obj : db.objects) {
    if (excluded.count(obj)) continue;
    if (!milestones.IsAccessible(obj)) {
      state[obj] = AutomationStatus::NotAutomatable;
    } else if (state[obj] == AutomationStatus::Unknown) {
      unknowns++;
      state[obj] = AutomationStatus::UnknownInQueue;
      processingQueue.push_back(obj->id);
    }
  }

  DependencyNode::AutomationContext ctx{
      .characterId = db.character != nullptr ? db.character->id : kInvalidFactorioId,
      .accessibleWithCurrentMilestones =
          [&](FactorioId id) { return milestones.IsAccessibleWithCurrentMilestones(id); },
  };

  while (!processingQueue.empty()) {
    FactorioId index = processingQueue.front();
    processingQueue.pop_front();

    AutomationStatus automationState =
        milestones.IsAccessibleWithCurrentMilestones(index)
            ? AutomationStatus::AutomatableNow
            : AutomationStatus::AutomatableLater;
    automationState = deps.dependencyList.ById(index).IsAutomatable(
        [&](FactorioId id) { return state.ById(id); }, automationState, ctx);
    if (automationState == AutomationStatus::UnknownInQueue) {
      automationState = AutomationStatus::Unknown;
    }

    state.ById(index) = automationState;
    if (automationState != AutomationStatus::Unknown) {
      unknowns--;
      for (FactorioId revDep : deps.reverseDependencies.ById(index)) {
        AutomationStatus oldState = state.ById(revDep);
        if (oldState == AutomationStatus::Unknown ||
            (oldState == AutomationStatus::AutomatableLater &&
             automationState == AutomationStatus::AutomatableNow)) {
          if (oldState == AutomationStatus::AutomatableLater) unknowns++;
          processingQueue.push_back(revDep);
          state.ById(revDep) = AutomationStatus::UnknownInQueue;
        }
      }
    }
  }
  if (db.voidEnergy != nullptr) {
    state[db.voidEnergy] = AutomationStatus::NotAutomatable;
  }

  // Upstream TODO carried over: remaining unknowns (cycles) are assumed
  // not automatable.
  if (unknowns > 0) {
    for (AutomationStatus& s : state.values()) {
      if (s == AutomationStatus::Unknown) s = AutomationStatus::NotAutomatable;
    }
  }
  automatable = state;
}

}  // namespace yafc
