// Port of Yafc.Model/Model/RecipeParameters.cs — turns a row's recipe +
// crafter + fuel choice into concrete parameters: effective craft time
// (crafting speed, quality, module/beacon speed), fuel use per building,
// productivity (entity base + mining/research/tech-level bonuses, capped by
// maximumProductivity), and warning flags. Generator rows (ScaleProduction-
// WithPower) invert to energy terms like upstream.
//
// Not ported yet: GetModulesInfo (module/beacon fill — rows have no module
// UI), UselessQuality (needs milestone access at parameter time).
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>

#include "yafc/model/data_classes.h"

namespace yafc {

class RecipeRow;

// Upstream WarningFlags (bit-compatible; extends the solver subset).
struct WarningFlags {
  static constexpr uint32_t kAssumesNauvisSolarRatio = 1u << 0;
  static constexpr uint32_t kReactorsNeighborsFromPrefs = 1u << 1;
  static constexpr uint32_t kFuelUsageInputLimited = 1u << 2;
  static constexpr uint32_t kAsteroidCollectionNotModelled = 1u << 3;
  static constexpr uint32_t kAssumesFulgoraAndModel = 1u << 4;
  static constexpr uint32_t kUselessQuality = 1u << 5;
  static constexpr uint32_t kExcessProductivity = 1u << 6;
  static constexpr uint32_t kEntityNotSpecified = 1u << 8;
  static constexpr uint32_t kFuelNotSpecified = 1u << 9;
  static constexpr uint32_t kFuelTemperatureExceedsMaximum = 1u << 10;
  static constexpr uint32_t kFuelDoesNotProvideEnergy = 1u << 11;
  static constexpr uint32_t kFuelWithTemperatureNotLinked = 1u << 12;
  static constexpr uint32_t kDeadlockCandidate = 1u << 16;
  static constexpr uint32_t kOverproductionRequired = 1u << 17;
  static constexpr uint32_t kExceedsBuiltCount = 1u << 18;
  static constexpr uint32_t kTemperatureForIngredientNotMatch = 1u << 24;
};

// Upstream ModuleEffects.
struct ModuleEffects {
  float speed = 0;
  float productivity = 0;
  float consumption = 0;
  float quality = 0;

  float speedMod() const { return std::max(1.0f + speed, 0.2f); }
  float energyUsageMod() const { return std::max(1.0f + consumption, 0.2f); }
  float qualityMod() const { return std::max(quality, 0.0f); }
};

// Upstream Project.current.settings subset that parameters read.
struct ProductionSettings {
  float miningProductivity = 0;
  float researchProductivity = 0;
  std::map<Technology*, int> productivityTechnologyLevels;
  // Upstream derives this from reactor-layout prefs; 1 = lone reactor.
  float reactorBonusMultiplier = 1;
};

struct RecipeParameters {
  float recipeTime = 1;
  float fuelUsagePerSecondPerBuilding = 0;
  ModuleEffects activeEffects;
  uint32_t warningFlags = 0;

  float productivity() const { return activeEffects.productivity; }
  float fuelUsagePerSecondPerRecipe() const {
    return recipeTime * fuelUsagePerSecondPerBuilding;
  }

  static RecipeParameters Calculate(const RecipeRow& row,
                                    const ProductionSettings& settings);
};

}  // namespace yafc
