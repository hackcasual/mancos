#include "yafc/model/recipe_parameters.h"

#include "yafc/model/production_table.h"

namespace yafc {

namespace {

float CraftingSpeedOf(const ObjectWithQuality<EntityCrafter>& entity) {
  if (entity.quality != nullptr) return entity.target->CraftingSpeed(*entity.quality);
  return entity.target->baseCraftingSpeed;
}

float PowerOf(const ObjectWithQuality<EntityCrafter>& entity) {
  if (entity.quality != nullptr) return entity.target->Power(*entity.quality);
  return entity.target->basePower;
}

float FuelConsumptionLimit(const EntityEnergy& energy, const Quality* quality) {
  if (quality != nullptr) return quality->ApplyStandardBonus(energy.baseFuelConsumptionLimit);
  return energy.baseFuelConsumptionLimit;
}

// Port of ModuleTemplate.GetModulesInfo + ModuleFillerParameters' fixed fill:
// slot-fill the row's module list (fixedCount 0 = fill remaining slots),
// falling back to the page's filler module; then beacon effects (template
// beaconList counts are totals across beacons; beaconCount = ceil(total /
// beacon slots)), falling back to the filler's beacon settings. Compatibility
// respects both the entity's and the recipe's allowed effects/categories.
void ApplyModules(const RecipeRow& row, const EntityCrafter& entity,
                  const ModuleFillerParameters& filler, RecipeParameters& result) {
  ModuleEffects& effects = result.activeEffects;
  const auto* recipe = dynamic_cast<const Recipe*>(row.recipe);
  auto accepts = [&](const Module* module) {
    if (module == nullptr) return false;
    if (!entity.CanAcceptModule(module->moduleSpecification)) return false;
    if (recipe != nullptr &&
        !EntityWithModules::CanAcceptModule(
            module->moduleSpecification, recipe->allowedEffects,
            recipe->allowedModuleCategories.empty() ? nullptr
                                                    : &recipe->allowedModuleCategories)) {
      return false;
    }
    return true;
  };

  if (entity.effectReceiver.usesModuleEffects) {
    if (!row.modules.list.empty()) {
      int remaining = entity.moduleSlots;
      for (const RecipeRowCustomModule& cm : row.modules.list) {
        if (remaining <= 0) break;
        if (!accepts(cm.module)) continue;
        int count = cm.fixedCount == 0 ? remaining : std::min(cm.fixedCount, remaining);
        remaining -= count;
        effects.AddModules(cm.module->moduleSpecification, static_cast<float>(count));
        result.usedModules.push_back({cm.module, count});
      }
    } else if (filler.fillerModule != nullptr && entity.moduleSlots > 0 &&
               accepts(filler.fillerModule) &&
               (filler.fillMiners ||
                !(row.recipe->flags & RecipeFlags::kUsesMiningProductivity))) {
      effects.AddModules(filler.fillerModule->moduleSpecification,
                         static_cast<float>(entity.moduleSlots));
      result.usedModules.push_back({filler.fillerModule, entity.moduleSlots});
    }
  }

  if (!entity.effectReceiver.usesBeaconEffects) return;
  if (row.modules.beacon != nullptr) {
    EntityBeacon* beacon = row.modules.beacon;
    int totalModules = 0;
    for (const RecipeRowCustomModule& bm : row.modules.beaconList) {
      if (bm.module != nullptr && beacon->CanAcceptModule(bm.module->moduleSpecification)) {
        totalModules += bm.fixedCount;
      }
    }
    if (totalModules > 0 && beacon->moduleSlots > 0) {
      int beaconCount = (totalModules - 1) / beacon->moduleSlots + 1;
      float efficiency = beacon->beaconEfficiency * beacon->profile(beaconCount);
      for (const RecipeRowCustomModule& bm : row.modules.beaconList) {
        if (bm.module == nullptr ||
            !beacon->CanAcceptModule(bm.module->moduleSpecification)) {
          continue;
        }
        effects.AddModules(bm.module->moduleSpecification,
                           efficiency * static_cast<float>(bm.fixedCount));
      }
      result.usedBeacon = beacon;
      result.usedBeaconCount = beaconCount;
    }
  } else if (row.modules.empty() && filler.beacon != nullptr &&
             filler.beaconModule != nullptr && filler.beaconsPerBuilding > 0 &&
             filler.beacon->moduleSlots > 0 &&
             filler.beacon->CanAcceptModule(filler.beaconModule->moduleSpecification)) {
    int beaconCount = filler.beaconsPerBuilding;
    float efficiency = filler.beacon->beaconEfficiency * filler.beacon->profile(beaconCount);
    effects.AddModules(
        filler.beaconModule->moduleSpecification,
        efficiency * static_cast<float>(beaconCount * filler.beacon->moduleSlots));
    result.usedBeacon = filler.beacon;
    result.usedBeaconCount = beaconCount;
  }
}

}  // namespace

RecipeParameters RecipeParameters::Calculate(const RecipeRow& row,
                                             const ProductionSettings& settings) {
  RecipeParameters result;
  uint32_t& warningFlags = result.warningFlags;
  const RecipeOrTechnology* recipe = row.recipe;
  const ObjectWithQuality<EntityCrafter>& entity = row.entity;
  const QualityGoods& fuel = row.fuel;

  result.qualityNormal = settings.qualityNormal;
  result.quality = row.quality != nullptr ? row.quality : settings.qualityNormal;

  if (recipe == nullptr) {
    result.recipeTime = 1;
    return result;
  }
  if (entity.target == nullptr) {
    warningFlags |= WarningFlags::kEntityNotSpecified;
    result.recipeTime = recipe->time;
    return result;
  }

  float recipeTime = recipe->time / CraftingSpeedOf(entity);
  float productivity = entity.target->effectReceiver.baseEffect.productivity;
  float speed = entity.target->effectReceiver.baseEffect.speed;
  float consumption = entity.target->effectReceiver.baseEffect.consumption;
  const EntityEnergy& energy = entity.target->energy;
  bool hasEnergy = entity.target->hasEnergy;
  float energyUsage = PowerOf(entity);
  float energyPerUnitOfFuel = 0;
  float fuelUsagePerSecondPerBuilding = 0;

  if (hasEnergy && fuel.target != nullptr) {
    const Fluid* fluid = const_cast<Goods*>(fuel.target)->fluid();
    energyPerUnitOfFuel = fuel.target->fuelValue;

    if (energy.type == EntityEnergyType::FluidHeat) {
      if (fluid == nullptr) {
        warningFlags |= WarningFlags::kFuelWithTemperatureNotLinked;
      } else {
        int temperature = fluid->temperature;
        if (temperature > energy.workingTemperature.max) {
          temperature = energy.workingTemperature.max;
          warningFlags |= WarningFlags::kFuelTemperatureExceedsMaximum;
        }
        energyPerUnitOfFuel =
            (temperature - energy.workingTemperature.min) * fluid->heatCapacity;
      }
    }
    if (fluid != nullptr && !energy.acceptedTemperature.Contains(fluid->temperature)) {
      warningFlags |= WarningFlags::kFuelDoesNotProvideEnergy;
    }
    if (energyPerUnitOfFuel > 0) {
      fuelUsagePerSecondPerBuilding =
          energyUsage <= 0 ? 0
                           : energyUsage / (energyPerUnitOfFuel * energy.effectivity);
    } else {
      fuelUsagePerSecondPerBuilding = 0;
      warningFlags |= WarningFlags::kFuelDoesNotProvideEnergy;
    }
  } else {
    fuelUsagePerSecondPerBuilding = energyUsage;
    warningFlags |= WarningFlags::kFuelNotSpecified;
  }

  // Generators: production scales with power output rather than craft time.
  if ((recipe->flags & RecipeFlags::kScaleProductionWithPower) &&
      energyPerUnitOfFuel > 0 && energy.type != EntityEnergyType::Void) {
    if (energyUsage == 0) {
      float limit = FuelConsumptionLimit(energy, entity.quality);
      fuelUsagePerSecondPerBuilding = limit;
      recipeTime = 1.0f / (limit * energyPerUnitOfFuel * energy.effectivity);
    } else {
      recipeTime = 1.0f / energyUsage;
    }
  }

  bool isMining = (recipe->flags & RecipeFlags::kUsesMiningProductivity) != 0;
  if (isMining) {
    productivity += settings.miningProductivity;
  } else if (dynamic_cast<const Technology*>(recipe) != nullptr) {
    productivity += settings.researchProductivity;
  } else if (auto* actualRecipe = dynamic_cast<const Recipe*>(recipe)) {
    for (const auto& [tech, changePerLevel] : actualRecipe->technologyProductivity) {
      auto it = settings.productivityTechnologyLevels.find(tech);
      if (it != settings.productivityTechnologyLevels.end()) {
        productivity += changePerLevel * it->second;
      }
    }
  }

  if (auto* reactor = dynamic_cast<const EntityReactor*>(entity.target)) {
    if (reactor->reactorNeighborBonus > 0) {
      productivity += reactor->reactorNeighborBonus * settings.reactorBonusMultiplier;
      warningFlags |= WarningFlags::kReactorsNeighborsFromPrefs;
    }
  }
  if (entity.target->factorioType == "solar-panel") {
    warningFlags |= WarningFlags::kAssumesNauvisSolarRatio;
  } else if (entity.target->factorioType == "lightning-attractor") {
    warningFlags |= WarningFlags::kAssumesFulgoraAndModel;
  } else if (entity.target->factorioType == "asteroid-collector") {
    warningFlags |= WarningFlags::kAsteroidCollectionNotModelled;
  }

  ModuleEffects& effects = result.activeEffects;
  effects.productivity += productivity;
  effects.speed += speed;
  effects.consumption += consumption;
  // Upstream gates GetModulesInfo on the entity accepting any effect at all.
  if (entity.target->allowedEffects != AllowedEffects::kNone) {
    ApplyModules(row, *entity.target, settings.filler, result);
  }

  if (auto* r = dynamic_cast<const Recipe*>(recipe);
      r != nullptr && r->maximumProductivity.has_value() &&
      effects.productivity > *r->maximumProductivity) {
    warningFlags |= WarningFlags::kExcessProductivity;
    effects.productivity = *r->maximumProductivity;
  }

  recipeTime /= effects.speedMod();
  fuelUsagePerSecondPerBuilding *= effects.energyUsageMod();

  if (hasEnergy && energy.drain > 0 && energyPerUnitOfFuel > 0) {
    fuelUsagePerSecondPerBuilding += energy.drain / energyPerUnitOfFuel;
  }
  if (hasEnergy) {
    float limit = FuelConsumptionLimit(energy, entity.quality);
    if (fuelUsagePerSecondPerBuilding > limit) {
      recipeTime *= fuelUsagePerSecondPerBuilding / limit;
      fuelUsagePerSecondPerBuilding = limit;
      warningFlags |= WarningFlags::kFuelUsageInputLimited;
    }
  }

  result.recipeTime = recipeTime;
  result.fuelUsagePerSecondPerBuilding = fuelUsagePerSecondPerBuilding;
  return result;
}

}  // namespace yafc
