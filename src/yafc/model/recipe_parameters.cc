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

}  // namespace

RecipeParameters RecipeParameters::Calculate(const RecipeRow& row,
                                             const ProductionSettings& settings) {
  RecipeParameters result;
  uint32_t& warningFlags = result.warningFlags;
  const RecipeOrTechnology* recipe = row.recipe;
  const ObjectWithQuality<EntityCrafter>& entity = row.entity;
  const QualityGoods& fuel = row.fuel;

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

  // TODO(port): GetModulesInfo (module/beacon effects) once rows carry
  // modules; activeEffects then starts from the filled module set.
  ModuleEffects& effects = result.activeEffects;
  effects.productivity += productivity;
  effects.speed += speed;
  effects.consumption += consumption;

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
