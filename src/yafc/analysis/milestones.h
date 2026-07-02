// Port of Yafc.Model/Analysis/Milestones.cs — computes, per object, which
// milestones must be researched before the object becomes accessible.
// milestoneResult bit 0 = "accessible at all"; bit i+1 = "requires milestone i".
//
// Project-side state (marked accessible/inaccessible objects, unlocked
// milestones) arrives through MilestonesInput instead of upstream's Project
// settings + undo hooks; warnings are returned as a typed struct (web i18n).
#pragma once

#include <unordered_set>
#include <vector>

#include "yafc/analysis/dependencies.h"
#include "yafc/model/bits.h"
#include "yafc/model/database.h"

namespace yafc {

struct MilestonesInput {
  // Empty = upstream default: all science packs + locations except nauvis and
  // space-location-unknown, auto-sorted by discovery order.
  std::vector<FactorioObject*> milestones;
  bool autoSort = true;
  std::unordered_set<const FactorioObject*> markedAccessible;    // user overrides
  std::unordered_set<const FactorioObject*> markedInaccessible;
  std::unordered_set<const FactorioObject*> unlockedMilestones;  // researched
};

struct MilestonesWarnings {
  bool mostObjectsInaccessible = false;   // fewer than half of objects reachable
  std::vector<FactorioObject*> unreachableMilestones;
};

class Milestones {
 public:
  MilestonesWarnings Compute(const Database& db, const Dependencies& deps,
                             const MilestonesInput& input);

  bool IsAccessible(FactorioId id) const { return milestoneResult.ById(id)[0]; }
  bool IsAccessible(const FactorioObject* obj) const { return IsAccessible(obj->id); }
  bool IsAccessibleWithCurrentMilestones(FactorioId id) const {
    return (milestoneResult.ById(id) & lockedMask) == 1;
  }
  bool IsAccessibleWithCurrentMilestones(const FactorioObject* obj) const {
    return IsAccessibleWithCurrentMilestones(obj->id);
  }
  bool IsAccessibleAtNextMilestone(const FactorioObject* obj) const {
    Bits mask = milestoneResult[obj] & lockedMask;
    if (mask == 1) return true;
    if (mask[0]) return false;
    return false;  // upstream: same (see the commented power-of-two check there)
  }
  Bits GetMilestoneResult(FactorioId id) const { return milestoneResult.ById(id); }
  Bits GetMilestoneResult(const FactorioObject* obj) const {
    return milestoneResult[obj];
  }
  // Highest milestone required for target (locked ones only unless all=true).
  FactorioObject* GetHighest(const FactorioObject* target, bool all) const;

  std::vector<FactorioObject*> currentMilestones;
  Bits lockedMask;
  Mapping<FactorioObject, Bits> milestoneResult;
};

}  // namespace yafc
