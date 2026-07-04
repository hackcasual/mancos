// Port of Yafc.Model/Model/RecipeParameters.cs — turns a row's recipe +
// crafter + fuel choice into concrete parameters: effective craft time
// (crafting speed, quality, module/beacon speed), fuel use per building,
// productivity (entity base + mining/research/tech-level bonuses, capped by
// maximumProductivity), and warning flags. Generator rows (ScaleProduction-
// WithPower) invert to energy terms like upstream. Includes the
// ModuleTemplate.GetModulesInfo port (module slot fill + beacon effects) and
// the fixed parts of ModuleFillerParameters (filler module, beacon fill;
// autoFillPayback economics not ported).
//
// Not ported yet: UselessQuality (needs milestone access at parameter time).
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

#include "yafc/model/data_classes.h"

namespace yafc {

class RecipeRow;

// Upstream RecipeRowCustomModule: one module choice in a ModuleTemplate.
// In `list`, fixedCount == 0 means "fill the remaining building slots"; in
// `beaconList` fixedCount is the TOTAL module count across all beacons.
struct RecipeRowCustomModule {
  Module* module = nullptr;
  int fixedCount = 0;
};

// Upstream ModuleTemplate: a row's explicit module/beacon configuration.
struct ModuleTemplate {
  std::vector<RecipeRowCustomModule> list;
  EntityBeacon* beacon = nullptr;
  std::vector<RecipeRowCustomModule> beaconList;

  bool empty() const { return list.empty() && beacon == nullptr; }
};

// Upstream ModuleFillerParameters (page defaults applied to rows without an
// explicit template). autoFillPayback is parsed for .yafc fidelity but the
// payback economics are not applied.
struct ModuleFillerParameters {
  bool fillMiners = false;
  float autoFillPayback = 0;
  Module* fillerModule = nullptr;
  EntityBeacon* beacon = nullptr;
  Module* beaconModule = nullptr;
  int beaconsPerBuilding = 8;
};

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

  // Upstream AddModules (quality flattened to normal): count may be
  // fractional for beacon-transmitted effects (efficiency x profile).
  void AddModules(const ModuleSpecification& module, float count,
                  uint32_t allowedEffects = AllowedEffects::kAll) {
    if (allowedEffects & AllowedEffects::kSpeed) speed += module.baseSpeed * count;
    if ((allowedEffects & AllowedEffects::kProductivity) && module.baseProductivity > 0) {
      productivity += module.baseProductivity * count;
    }
    if (allowedEffects & AllowedEffects::kConsumption) {
      consumption += module.baseConsumption * count;
    }
    if (allowedEffects & AllowedEffects::kQuality) quality += module.baseQuality * count;
  }
};

// Upstream Project.current.settings subset that parameters read.
struct ProductionSettings {
  float miningProductivity = 0;
  float researchProductivity = 0;
  std::map<Technology*, int> productivityTechnologyLevels;
  // Upstream derives this from reactor-layout prefs; 1 = lone reactor.
  float reactorBonusMultiplier = 1;
  // Page-level module defaults (upstream: ProductionTable.modules).
  ModuleFillerParameters filler;
  // Database::qualityNormal, injected by the caller (nullptr in tests/tools
  // that never touch quality — see RecipeParameters::quality/qualityNormal).
  Quality* qualityNormal = nullptr;
};

struct RecipeParameters {
  float recipeTime = 1;
  float fuelUsagePerSecondPerBuilding = 0;
  ModuleEffects activeEffects;
  uint32_t warningFlags = 0;
  // What actually got slotted (after compatibility filtering) — upstream
  // UsedModule; drives the UI display.
  std::vector<RecipeRowCustomModule> usedModules;
  EntityBeacon* usedBeacon = nullptr;
  int usedBeaconCount = 0;
  // Resolved quality dimension (upstream RecipeRow.recipe.quality): `quality`
  // is this row's target/floor tier (row.quality, defaulting to
  // qualityNormal); `qualityNormal` is always the Normal tier, forced for
  // goods that don't accept quality modifiers (fluids, technologies).
  // Both stay nullptr if the caller never wired ProductionSettings::
  // qualityNormal — callers that don't care about quality see no behavior
  // change (single untagged-quality resolution, as before this was ported).
  Quality* quality = nullptr;
  Quality* qualityNormal = nullptr;

  float productivity() const { return activeEffects.productivity; }
  float fuelUsagePerSecondPerRecipe() const {
    return recipeTime * fuelUsagePerSecondPerBuilding;
  }

  static RecipeParameters Calculate(const RecipeRow& row,
                                    const ProductionSettings& settings);
};

}  // namespace yafc
