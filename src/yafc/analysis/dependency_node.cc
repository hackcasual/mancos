#include "yafc/analysis/dependency_node.h"

#include <algorithm>

namespace yafc {

namespace {
constexpr uint32_t Raw(DependencyNode::Flags f) { return static_cast<uint32_t>(f); }
}  // namespace

DependencyNode::DependencyNode(std::vector<FactorioId> ids, Flags flags) {
  // Dedupe while preserving order (upstream Distinct()).
  std::vector<FactorioId> unique;
  for (FactorioId id : ids) {
    if (std::find(unique.begin(), unique.end(), id) == unique.end()) {
      unique.push_back(id);
    }
  }
  auto impl = std::make_shared<Impl>();
  impl->kind = Kind::List;
  impl->flags = flags;
  impl->elements = std::move(unique);
  impl_ = std::move(impl);
}

DependencyNode DependencyNode::Create(
    const std::vector<const FactorioObject*>& elements, Flags flags) {
  std::vector<FactorioId> ids;
  ids.reserve(elements.size());
  for (const FactorioObject* e : elements) ids.push_back(e->id);
  return DependencyNode(std::move(ids), flags);
}

DependencyNode DependencyNode::Combine(Kind kind, std::vector<DependencyNode> deps) {
  std::vector<DependencyNode> real;
  for (DependencyNode& d : deps) {
    if (d.impl_->kind == kind) {
      for (const DependencyNode& child : d.impl_->children) real.push_back(child);
    } else {
      real.push_back(std::move(d));
    }
  }
  // Distinct() by node identity (shared impl), like upstream reference equality.
  std::vector<DependencyNode> unique;
  for (DependencyNode& d : real) {
    bool seen = false;
    for (const DependencyNode& u : unique) seen |= u.impl_ == d.impl_;
    if (!seen) unique.push_back(std::move(d));
  }
  if (unique.empty()) {
    // Upstream throws; a solver-facing port prefers an explicit empty
    // require-all (always satisfied) over an exception path. Callers only hit
    // this via RequireAll({}) which upstream never does.
    return DependencyNode({}, kind == Kind::And ? Flags::Ingredient : Flags::Source);
  }
  if (unique.size() == 1) return unique[0];

  auto impl = std::make_shared<Impl>();
  impl->kind = kind;
  impl->children = std::move(unique);
  return DependencyNode(std::move(impl));
}

DependencyNode DependencyNode::RequireAll(std::vector<DependencyNode> dependencies) {
  return Combine(Kind::And, std::move(dependencies));
}

DependencyNode DependencyNode::RequireAny(std::vector<DependencyNode> dependencies) {
  return Combine(Kind::Or, std::move(dependencies));
}

bool DependencyNode::HasFlag(Flags f) const {
  return (Raw(impl_->flags) & Raw(f)) == Raw(f);
}

std::vector<FactorioId> DependencyNode::Flatten() const {
  std::vector<FactorioId> result;
  if (impl_->kind == Kind::List) {
    result = impl_->elements;
  } else {
    for (const DependencyNode& child : impl_->children) {
      for (FactorioId id : child.Flatten()) result.push_back(id);
    }
  }
  return result;
}

bool DependencyNode::IsAccessible(
    const std::function<bool(FactorioId)>& isAccessible) const {
  switch (impl_->kind) {
    case Kind::And:
      for (const DependencyNode& child : impl_->children) {
        if (!child.IsAccessible(isAccessible)) return false;
      }
      return true;
    case Kind::Or:
      for (const DependencyNode& child : impl_->children) {
        if (child.IsAccessible(isAccessible)) return true;
      }
      return false;
    case Kind::List:
      if (HasFlag(Flags::RequireEverything)) {
        for (FactorioId id : impl_->elements) {
          if (!isAccessible(id)) return false;
        }
        return true;
      }
      for (FactorioId id : impl_->elements) {
        if (isAccessible(id)) return true;
      }
      return false;
  }
  return false;
}

Bits DependencyNode::AggregateBits(
    const std::function<Bits(FactorioId)>& getBits) const {
  switch (impl_->kind) {
    case Kind::And: {
      Bits result;
      for (const DependencyNode& child : impl_->children) {
        result |= child.AggregateBits(getBits);
      }
      return result;
    }
    case Kind::Or: {
      // Min over children (cheapest alternative).
      bool first = true;
      Bits best;
      for (const DependencyNode& child : impl_->children) {
        Bits bits = child.AggregateBits(getBits);
        if (first || bits < best) {
          best = std::move(bits);
          first = false;
        }
      }
      return best;
    }
    case Kind::List: {
      Bits bits;
      if (HasFlag(Flags::RequireEverything)) {
        for (FactorioId id : impl_->elements) bits |= getBits(id);
        return bits;
      }
      if (!impl_->elements.empty()) {
        bool first = true;
        Bits best;
        for (FactorioId id : impl_->elements) {
          Bits b = getBits(id);
          if (first || b < best) {
            best = std::move(b);
            first = false;
          }
        }
        return bits | best;
      }
      return bits;
    }
  }
  return {};
}

AutomationStatus DependencyNode::IsAutomatable(
    const std::function<AutomationStatus(FactorioId)>& getAutomation,
    AutomationStatus automationState, const AutomationContext& ctx) const {
  switch (impl_->kind) {
    case Kind::And: {
      AutomationStatus result = automationState;
      for (const DependencyNode& child : impl_->children) {
        result = std::min(result, child.IsAutomatable(getAutomation, automationState, ctx));
      }
      return result;
    }
    case Kind::Or: {
      AutomationStatus result = AutomationStatus::NotAutomatable;
      for (const DependencyNode& child : impl_->children) {
        result = std::max(result, child.IsAutomatable(getAutomation, automationState, ctx));
      }
      return result;
    }
    case Kind::List:
      if (!HasFlag(Flags::OneTimeInvestment)) {
        if (HasFlag(Flags::RequireEverything)) {
          for (FactorioId id : impl_->elements) {
            automationState = std::min(automationState, getAutomation(id));
          }
        } else {
          AutomationStatus localHighest = AutomationStatus::NotAutomatable;
          for (FactorioId id : impl_->elements) {
            localHighest = std::max(localHighest, getAutomation(id));
          }
          automationState = std::min(automationState, localHighest);
        }
      } else if (automationState == AutomationStatus::AutomatableNow &&
                 impl_->flags == Flags::CraftingEntity) {
        // If only the character is currently accessible as a crafter, the
        // object is not automatable *now*.
        bool hasMachine = false;
        for (FactorioId id : impl_->elements) {
          if (id != ctx.characterId && ctx.accessibleWithCurrentMilestones &&
              ctx.accessibleWithCurrentMilestones(id)) {
            hasMachine = true;
            break;
          }
        }
        if (!hasMachine) automationState = AutomationStatus::AutomatableLater;
      }
      return automationState;
  }
  return automationState;
}

}  // namespace yafc
