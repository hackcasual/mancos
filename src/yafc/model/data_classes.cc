#include "yafc/model/data_classes.h"

#include <algorithm>
#include <cmath>

namespace yafc {

Fluid* Goods::fluid() { return dynamic_cast<Fluid*>(this); }

float Item::GetSpoilTime(const Quality& quality) const {
  return quality.ApplyStandardBonus(baseSpoilTime);
}

Ingredient::Ingredient(Goods* goods, float amount) : amount(amount), goods(goods) {
  if (Fluid* f = goods ? goods->fluid() : nullptr) {
    temperature = f->temperatureRange;
  }
}

void Product::SetCatalyst(float catalyst) {
  float catalyticMin = amountMin - catalyst;
  float catalyticMax = amountMax - catalyst;

  if (catalyticMax <= 0) {
    productivityAmount_ = 0.0f;
  } else if (catalyticMin >= 0.0f) {
    productivityAmount_ = (catalyticMin + catalyticMax) * 0.5f * probability;
  } else {
    // Upstream: "super duper rare case, might not be precise"
    productivityAmount_ =
        probability * catalyticMax * catalyticMax * 0.5f / (catalyticMax - catalyticMin);
  }
}

bool Product::IsSimple() const {
  return amountMin == amountMax && amount == 1 && probability == 1 &&
         !percentSpoiled.has_value() && dynamic_cast<const Item*>(goods) != nullptr;
}

float Entity::Power(const Quality& quality) const {
  if (factorioType == "boiler" || factorioType == "reactor" ||
      factorioType == "generator" || factorioType == "burner-generator" ||
      factorioType == "accumulator") {
    return quality.ApplyStandardBonus(basePower);
  }
  if (factorioType == "beacon") return basePower * quality.BeaconConsumptionFactor;
  return basePower;
}

bool EntityWithModules::CanAcceptModule(const ModuleSpecification& module,
                                        uint32_t effects,
                                        const std::vector<std::string>* categories) {
  if (module.baseProductivity > 0.0f && !(effects & AllowedEffects::kProductivity)) return false;
  if (module.baseConsumption < 0.0f && !(effects & AllowedEffects::kConsumption)) return false;
  if (module.basePollution < 0.0f && !(effects & AllowedEffects::kPollution)) return false;
  if (module.baseSpeed > 0.0f && !(effects & AllowedEffects::kSpeed)) return false;
  if (module.baseQuality > 0.0f && !(effects & AllowedEffects::kQuality)) return false;
  if (categories != nullptr &&
      std::find(categories->begin(), categories->end(), module.category) ==
          categories->end()) {
    return false;
  }
  return true;
}

float EntityCrafter::CraftingSpeed(const Quality& quality) const {
  if (factorioType == "agricultural-tower" ||
      factorioType == "electric-energy-interface" || factorioType == "mining-drill") {
    return baseCraftingSpeed;
  }
  return quality.ApplyStandardBonus(baseCraftingSpeed);
}

float EntityBeacon::profile(int numberOfBeacons) const {
  if (profileValues.empty()) return 1.0f;
  if (numberOfBeacons <= 0) return profileValues[0];
  size_t index = std::min(static_cast<size_t>(numberOfBeacons) - 1,
                          profileValues.size() - 1);
  return profileValues[index];
}

float Quality::ApplyModuleBonus(float baseValue) const {
  return std::floor(ApplyStandardBonus(baseValue) * 100) / 100;
}

}  // namespace yafc
