#include "yafc/analysis/milestones.h"

#include <algorithm>
#include <deque>

namespace yafc {

namespace {

// Upstream WalkAccessibilityGraph: flood-fill over reverse dependencies from
// the root-accessible objects, refusing to traverse pruneAt nodes. Optionally
// records the order in which milestones are first reached.
std::vector<bool> WalkAccessibilityGraph(
    const Database& db, const Dependencies& deps, const MilestonesInput& input,
    const std::unordered_set<const FactorioObject*>& pruneAt,
    const std::vector<FactorioObject*>& milestones,
    std::vector<FactorioObject*>* sortedMilestones) {
  std::deque<FactorioId> queue;
  for (FactorioObject* obj : db.rootAccessible) {
    if (!pruneAt.count(obj)) queue.push_back(obj->id);
  }
  for (const FactorioObject* obj : input.markedAccessible) {
    queue.push_back(obj->id);
  }

  std::vector<bool> accessible(db.objects.count(), false);
  for (FactorioId id : queue) accessible[id] = true;
  std::vector<bool> prune(db.objects.count(), false);
  for (const FactorioObject* obj : pruneAt) prune[obj->id] = true;

  while (!queue.empty()) {
    FactorioId node = queue.front();
    queue.pop_front();

    if (sortedMilestones != nullptr) {
      FactorioObject* obj = db.objects.ById(node);
      if (std::find(milestones.begin(), milestones.end(), obj) != milestones.end() &&
          std::find(sortedMilestones->begin(), sortedMilestones->end(), obj) ==
              sortedMilestones->end()) {
        sortedMilestones->push_back(obj);
      }
    }

    for (FactorioId child : deps.reverseDependencies.ById(node)) {
      if (!accessible[child] && !prune[child] &&
          deps.dependencyList.ById(child).IsAccessible(
              [&](FactorioId x) { return accessible[x]; })) {
        accessible[child] = true;
        queue.push_back(child);
      }
    }
  }
  return accessible;
}

}  // namespace

MilestonesWarnings Milestones::Compute(const Database& db, const Dependencies& deps,
                                       const MilestonesInput& input) {
  MilestonesWarnings warnings;

  std::vector<FactorioObject*> milestones = input.milestones;
  bool autoSort = input.autoSort;
  if (milestones.empty()) {
    // Upstream default: science packs and locations (minus the start states).
    for (Item* pack : db.allSciencePacks) milestones.push_back(pack);
    for (Location* location : db.locations) {
      if (location->name != "nauvis" && location->name != "space-location-unknown") {
        milestones.push_back(location);
      }
    }
    autoSort = true;
  }

  // First walk: basic accessibility + milestone discovery order.
  std::vector<FactorioObject*> sortedMilestones;
  std::vector<bool> baseAccessibility = WalkAccessibilityGraph(
      db, deps, input, input.markedInaccessible, milestones, &sortedMilestones);

  // Per-milestone walks with that milestone pruned. (Upstream runs these in
  // parallel; sequential here until wasm pthreads are wired up.)
  std::vector<std::vector<bool>> milestoneAccessibility;
  milestoneAccessibility.reserve(milestones.size());
  for (FactorioObject* milestone : milestones) {
    auto pruneAt = input.markedInaccessible;
    pruneAt.insert(milestone);
    milestoneAccessibility.push_back(
        WalkAccessibilityGraph(db, deps, input, pruneAt, {}, nullptr));
  }

  if (autoSort) {
    // Inaccessible milestones keep their original order at the end.
    currentMilestones = sortedMilestones;
    for (FactorioObject* m : milestones) {
      if (std::find(currentMilestones.begin(), currentMilestones.end(), m) ==
          currentMilestones.end()) {
        currentMilestones.push_back(m);
      }
    }
  } else {
    currentMilestones = milestones;
  }

  // Milestone index lookup in the (possibly re-sorted) order.
  auto milestoneIndex = [&](size_t sortedPos) {
    return std::find(milestones.begin(), milestones.end(),
                     currentMilestones[sortedPos]) -
           milestones.begin();
  };

  // Assemble masks for accessible objects.
  milestoneResult = db.objects.CreateMapping<Bits>();
  for (FactorioObject* obj : db.objects) {
    if (!baseAccessibility[obj->id]) continue;
    Bits bits(true);
    for (size_t i = 0; i < currentMilestones.size(); ++i) {
      if (!milestoneAccessibility[milestoneIndex(i)][obj->id]) {
        bits.Set(static_cast<int>(i) + 1, true);
      }
    }
    milestoneResult[obj] = bits;
  }

  // Predict masks for inaccessible objects by OR-ing their parents' masks.
  std::deque<FactorioObject*> inaccessibleQueue;
  std::vector<bool> queued(db.objects.count(), false);
  for (FactorioObject* obj : db.objects) {
    if (!baseAccessibility[obj->id]) {
      inaccessibleQueue.push_back(obj);
      queued[obj->id] = true;
    }
  }
  while (!inaccessibleQueue.empty()) {
    FactorioObject* inaccessible = inaccessibleQueue.front();
    inaccessibleQueue.pop_front();
    queued[inaccessible->id] = false;

    Bits milestoneBits = deps.dependencyList[inaccessible].AggregateBits(
        [&](FactorioId id) { return milestoneResult.ById(id); });
    milestoneBits.Set(0, false);  // inaccessible

    if (!(milestoneBits == milestoneResult[inaccessible])) {
      // OR to avoid flipping between two sets forever.
      milestoneResult[inaccessible] |= milestoneBits;
      for (FactorioId childId : deps.reverseDependencies[inaccessible]) {
        FactorioObject* child = db.objects.ById(childId);
        if (!milestoneResult[child][0] && !queued[childId]) {
          inaccessibleQueue.push_back(child);
          queued[childId] = true;
        }
      }
    }
  }

  // Locked mask from researched milestones (bit 0 always set).
  Bits locked(true);
  for (size_t i = 0; i < currentMilestones.size(); ++i) {
    locked.Set(static_cast<int>(i) + 1,
               input.unlockedMilestones.count(currentMilestones[i]) == 0);
  }
  lockedMask = locked;

  int accessibleObjects =
      static_cast<int>(std::count(baseAccessibility.begin(), baseAccessibility.end(), true));
  warnings.mostObjectsInaccessible = accessibleObjects < db.objects.count() / 2;
  for (FactorioObject* m : milestones) {
    if (std::find(sortedMilestones.begin(), sortedMilestones.end(), m) ==
        sortedMilestones.end()) {
      warnings.unreachableMilestones.push_back(m);
    }
  }
  return warnings;
}

FactorioObject* Milestones::GetHighest(const FactorioObject* target, bool all) const {
  if (target == nullptr) return nullptr;
  Bits ms = milestoneResult[target];
  if (!all) ms &= lockedMask;
  if (ms == 0) return nullptr;
  int msb = ms.HighestBitSet() - 1;
  return msb < 0 || msb >= static_cast<int>(currentMilestones.size())
             ? nullptr
             : currentMilestones[msb];
}

}  // namespace yafc
