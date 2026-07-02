// Port of Yafc.Model/Analysis/DependencyNode.cs — dependency trees like
// "Recipe.engine-unit requires (iron-gear AND steel AND pipe) AND
// (assembler-1 OR assembler-2) AND Technology.engine".
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "yafc/model/bits.h"
#include "yafc/model/data_classes.h"

namespace yafc {

// Upstream AutomationStatus (sbyte enum; ordering matters).
enum class AutomationStatus : int8_t {
  NotAutomatable = -1,
  Unknown = 0,        // internal to the analysis
  UnknownInQueue = 1, // internal to the analysis
  AutomatableLater = 2,
  AutomatableNow = 3,
};

class DependencyNode {
 public:
  // Upstream Flags: low bits describe the dependency kind (for UI), the high
  // bits carry require-all vs require-any and one-time-investment semantics.
  enum class Flags : uint32_t {
    RequireEverything = 0x100,
    OneTimeInvestment = 0x200,

    Ingredient = 1 | RequireEverything,
    CraftingEntity = 2 | OneTimeInvestment,
    SourceEntity = 3 | OneTimeInvestment,
    TechnologyUnlock = 4 | OneTimeInvestment,
    Source = 5,
    Fuel = 6,
    ItemToPlace = 7,
    TechnologyPrerequisites = 8 | RequireEverything | OneTimeInvestment,
    IngredientVariant = 9,
    Disabled = 10,
    Location = 11 | OneTimeInvestment,
  };

  DependencyNode() : DependencyNode(std::vector<FactorioId>{}, Flags::Source) {}

  // ListNode: leaf list of object ids with require-any/-all semantics.
  static DependencyNode Create(const std::vector<const FactorioObject*>& elements,
                               Flags flags);
  static DependencyNode Create(std::vector<FactorioId> ids, Flags flags) {
    return DependencyNode(std::move(ids), flags);
  }
  // AndNode / OrNode; nested nodes of the same kind are flattened, exact
  // duplicates removed, single children returned directly (upstream behavior).
  static DependencyNode RequireAll(std::vector<DependencyNode> dependencies);
  static DependencyNode RequireAny(std::vector<DependencyNode> dependencies);

  std::vector<FactorioId> Flatten() const;

  bool IsAccessible(const std::function<bool(FactorioId)>& isAccessible) const;

  Bits AggregateBits(const std::function<Bits(FactorioId)>& getBits) const;

  // Context for the CraftingEntity special case (upstream reads
  // Database.character and Milestones.Instance globals).
  struct AutomationContext {
    FactorioId characterId = kInvalidFactorioId;
    std::function<bool(FactorioId)> accessibleWithCurrentMilestones;
  };
  AutomationStatus IsAutomatable(
      const std::function<AutomationStatus(FactorioId)>& getAutomation,
      AutomationStatus automationState, const AutomationContext& ctx) const;

 private:
  enum class Kind { And, Or, List };

  struct Impl {
    Kind kind;
    Flags flags = Flags::Source;            // List only
    std::vector<FactorioId> elements;       // List only
    std::vector<DependencyNode> children;   // And/Or only
  };

  DependencyNode(std::vector<FactorioId> ids, Flags flags);
  static DependencyNode Combine(Kind kind, std::vector<DependencyNode> deps);
  explicit DependencyNode(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

  bool HasFlag(Flags f) const;

  std::shared_ptr<const Impl> impl_;
};

}  // namespace yafc
