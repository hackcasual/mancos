// Entities (crafters, labs, drills, boilers, generators, reactors, belts,
// beacons, containers, ...) — port of FactorioDataDeserializer_Entity.cs.
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "yafc/parser/data_deserializer.h"

namespace yafc {

namespace {
constexpr float kEstimationDistanceFromCenter = 3000.0f;
constexpr const char* kBurnableFluid = "burnable-fluid.";
constexpr const char* kHeat = "heat";
constexpr const char* kVoid = "void";
constexpr const char* kElectricity = "electricity";
constexpr const char* kHotFluid = "hot-fluid";
constexpr const char* kSpecificFluid = "fluid.";
constexpr const char* kMiningRecipe = "mining.";
constexpr const char* kBoilerRecipe = "boiler.";
constexpr const char* kFixedRecipe = "fixed-recipe.";
constexpr const char* kGeneratorRecipe = "generator";
constexpr const char* kPumpingRecipe = "pump.";
constexpr const char* kLabs = "labs.";
constexpr const char* kTechnologyTrigger = "technology-trigger";
constexpr const char* kRocketLaunch = "launch";
constexpr const char* kRocketCraft = "rocket.";
constexpr const char* kReactorRecipe = "reactor";
constexpr const char* kPlantRecipe = "plant";
constexpr const char* kAsteroidCapture = "asteroid-capture";

int RoundF(float v) { return static_cast<int>(std::lround(v)); }
}  // namespace

bool DataDeserializer::GetFluidBoxFilter(const Tbl& table, const char* fluidBoxName,
                                         int temperature, Fluid** fluid,
                                         TemperatureRange* range) {
  *fluid = nullptr;
  *range = {};
  Tbl fluidBoxData = GetTbl(table, fluidBoxName);
  if (!fluidBoxData) return false;
  std::string fluidName;
  if (!GetStrOpt(fluidBoxData, "filter", fluidName)) return false;

  *fluid = temperature == 0 ? GetObject<Fluid>(fluidName)
                            : GetFluidFixedTemp(fluidName, temperature);
  range->min = GetInt(fluidBoxData, "minimum_temperature", (*fluid)->temperatureRange.min);
  range->max = GetInt(fluidBoxData, "maximum_temperature", (*fluid)->temperatureRange.max);
  return true;
}

void DataDeserializer::ReadFluidEnergySource(const Tbl& energySource, Entity* entity) {
  EntityEnergy& energy = entity->energy;
  bool burns = GetBool(energySource, "burns_fluid", false);
  energy.type = burns ? EntityEnergyType::FluidFuel : EntityEnergyType::FluidHeat;
  energy.workingTemperature = TemperatureRange::Any();

  if (Has(energySource, "fluid_usage_per_tick")) {
    energy.baseFuelConsumptionLimit =
        GetFloat(energySource, "fluid_usage_per_tick", 0) * 60.0f;
  }

  Fluid* fluid = nullptr;
  TemperatureRange filterTemperature;
  if (GetFluidBoxFilter(energySource, "fluid_box", 0, &fluid, &filterTemperature)) {
    std::string fuelCategory = std::string(kSpecificFluid) + fluid->name;
    fuelUsers_.Add(entity, fuelCategory);
    if (!burns) {
      TemperatureRange temperature = fluid->temperatureRange;
      int maxT = GetInt(energySource, "maximum_temperature",
                        std::numeric_limits<int>::max());
      temperature.max = std::min(temperature.max, maxT);
      energy.workingTemperature = temperature;
      energy.acceptedTemperature = filterTemperature;
    }
  } else if (burns) {
    fuelUsers_.Add(entity, kBurnableFluid);
  } else {
    fuelUsers_.Add(entity, kHotFluid);
  }
}

void DataDeserializer::ReadEnergySource(const Tbl& energySource, Entity* entity,
                                        float defaultDrain) {
  std::string type = GetStr(energySource, "type", "burner");
  entity->hasEnergy = true;

  if (type == "void") {
    entity->energy = EntityEnergy{};
    entity->energy.type = EntityEnergyType::Void;
    entity->energy.effectivity = std::numeric_limits<float>::infinity();
    return;
  }

  EntityEnergy energy;
  std::vector<std::pair<std::string, float>> emissions;
  if (!(factorioVersion_ < Version{2, 0, 0})) {
    if (Tbl table = GetTbl(energySource, "emissions_per_minute")) {
      for (const std::string& key : Keys(table)) {
        emissions.emplace_back(key, GetFloat(table, key.c_str(), 0));
      }
    }
  } else if (Has(energySource, "emissions_per_minute")) {
    emissions.emplace_back("pollution", GetFloat(energySource, "emissions_per_minute", 0));
  }
  energy.emissions = std::move(emissions);
  energy.effectivity = GetFloat(energySource, "effectivity", 1);

  if (type == "electric") {
    fuelUsers_.Add(entity, kElectricity);
    energy.type = EntityEnergyType::Electric;
    std::string drain;
    energy.drain = GetStrOpt(energySource, "drain", drain)
                       ? static_cast<float>(LuaContext::ParseEnergy(drain))
                       : defaultDrain;
    entity->energy = std::move(energy);
  } else if (type == "burner") {
    energy.type = EntityEnergyType::SolidFuel;
    entity->energy = std::move(energy);
    if (Tbl categories = GetTbl(energySource, "fuel_categories")) {
      for (const std::string& cat : ArrayStrs(categories)) {
        fuelUsers_.Add(entity, cat);
      }
    } else {
      fuelUsers_.Add(entity, GetStr(energySource, "fuel_category", "chemical"));
    }
    entity->hasBurntInventory = GetInt(energySource, "burnt_inventory_size", 0) != 0;
  } else if (type == "heat") {
    energy.type = EntityEnergyType::Heat;
    energy.workingTemperature =
        TemperatureRange(GetInt(energySource, "min_working_temperature", 15),
                         GetInt(energySource, "max_temperature", 15));
    entity->energy = std::move(energy);
    fuelUsers_.Add(entity, kHeat);
  } else if (type == "fluid") {
    entity->energy = std::move(energy);
    ReadFluidEnergySource(energySource, entity);
  } else {
    entity->energy = std::move(energy);
  }
}

void DataDeserializer::ParseModules(const Tbl& table, EntityWithModules* entity,
                                    uint32_t def) {
  auto parseOne = [this](const std::string& s) -> uint32_t {
    std::string lower;
    for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "speed") return AllowedEffects::kSpeed;
    if (lower == "productivity") return AllowedEffects::kProductivity;
    if (lower == "consumption") return AllowedEffects::kConsumption;
    if (lower == "pollution") return AllowedEffects::kPollution;
    if (lower == "quality") return AllowedEffects::kQuality;
    Error("unknown allowed effect: " + s);
    return 0;
  };

  std::string effects;
  if (GetStrOpt(table, "allowed_effects", effects)) {
    entity->allowedEffects = parseOne(effects);
  } else if (Tbl list = GetTbl(table, "allowed_effects")) {
    entity->allowedEffects = AllowedEffects::kNone;
    for (const std::string& s : ArrayStrs(list)) {
      entity->allowedEffects |= parseOne(s);
    }
  } else {
    entity->allowedEffects = def;
  }

  if (Tbl categories = GetTbl(table, "allowed_module_categories")) {
    entity->allowedModuleCategories = ArrayStrs(categories);
  }

  // module_specification.module_slots in 1.1; module_slots in 2.0.
  if (Tbl spec = GetTbl(table, "module_specification")) {
    entity->moduleSlots = GetInt(spec, "module_slots", 0);
  } else {
    entity->moduleSlots = GetInt(table, "module_slots", 0);
  }
}

Recipe* DataDeserializer::CreateLaunchRecipe(EntityCrafter* entity, Recipe* recipe,
                                             int partsRequired) {
  std::string launchCategory = std::string(kRocketCraft) + entity->name;
  Mechanics* launchRecipe = CreateSpecialRecipe(recipe, launchCategory, "launch");
  recipeCrafters_.Add(entity, launchCategory);
  launchRecipe->ingredients.clear();
  for (const Product& p : recipe->products) {
    launchRecipe->ingredients.emplace_back(p.goods, p.amount * partsRequired);
  }
  launchRecipe->products = {Product(rocketLaunch_, 1)};
  specialWithProducts_.insert(launchRecipe);
  launchRecipe->time = 40.33f;
  recipeCrafters_.Add(entity, kRocketLaunch);
  return launchRecipe;
}

void DataDeserializer::DeserializeEntity(const Tbl& table) {
  static const std::unordered_set<std::string> kNoDefaultEnergyParsing = {
      "generator", "burner-generator", "offshore-pump", "solar-panel",
      "accumulator", "electric-energy-interface", "lightning-attractor",
  };

  std::string factorioType = GetStr(table, "type");
  std::string name = GetStr(table, "name");
  float defaultDrain = 0;

  if (Tbl placeableBy = GetTbl(table, "placeable_by")) {
    std::string itemName;
    if (GetStrOpt(placeableBy, "item", itemName)) {
      Item* item = GetObject<Item>(itemName);
      placeResults_[item].push_back(name);
    }
  }

  bool isCrafterLike = factorioType == "assembling-machine" ||
                       factorioType == "furnace" || factorioType == "rocket-silo";
  bool isGeneratorLike =
      factorioType == "generator" || factorioType == "burner-generator";

  if (factorioType == "accumulator") {
    auto* accumulator = GetObject<EntityAccumulator>(table);
    if (Tbl energy = GetTbl(table, "energy_source")) {
      std::string capacity, inputPower;
      if (GetStrOpt(energy, "buffer_capacity", capacity)) {
        accumulator->baseAccumulatorCapacity =
            static_cast<float>(LuaContext::ParseEnergy(capacity));
      }
      if (GetStrOpt(energy, "input_flow_limit", inputPower)) {
        accumulator->basePower = static_cast<float>(LuaContext::ParseEnergy(inputPower));
      }
    }
  } else if (factorioType == "agricultural-tower") {
    auto* tower = GetObject<EntityCrafter>(table);
    tower->basePower = static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "energy_usage")));
    float radius = GetFloat(table, "radius", 1);
    tower->baseCraftingSpeed =
        static_cast<float>(std::pow(2 * radius + 1, 2) - 1);
    tower->itemInputs = 1;
    recipeCrafters_.Add(tower, kPlantRecipe);
  } else if (factorioType == "asteroid") {
    Entity* asteroid = GetObject<Entity>(table);
    if (Tbl death = GetTbl(table, "dying_trigger_effect")) {
      ReadObjectOrArray(death, [&](const Tbl& trigger) {
        std::string type = GetStr(trigger, "type");
        std::string result;
        if ((type == "create-entity" && GetStrOpt(trigger, "entity_name", result)) ||
            (type == "create-asteroid-chunk" && GetStrOpt(trigger, "asteroid_name", result))) {
          asteroids_.Add(result, asteroid);
        }
      });
    }
  } else if (factorioType == "asteroid-collector") {
    auto* collector = GetObject<EntityCrafter>(table);
    collector->basePower =
        static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "arm_energy_usage"))) * 60;
    defaultDrain =
        static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "passive_energy_usage"))) * 60;
    collector->baseCraftingSpeed = 1;
    recipeCrafters_.Add(collector, kAsteroidCapture);
  } else if (factorioType == "beacon") {
    auto* beacon = GetObject<EntityBeacon>(table);
    beacon->beaconEfficiency = GetFloat(table, "distribution_effectivity", 0);
    if (Tbl profile = GetTbl(table, "profile")) {
      beacon->profileValues.clear();
      for (double v : ArrayNums(profile)) {
        beacon->profileValues.push_back(static_cast<float>(v));
      }
    } else {
      beacon->profileValues = {1.0f};
    }
    ParseModules(table, beacon, AllowedEffects::kNone);
    beacon->basePower = static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "energy_usage")));
  } else if (factorioType == "boiler") {
    auto* boiler = GetObject<EntityCrafter>(table);
    boiler->basePower =
        static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "energy_consumption")));
    boiler->fluidInputs = 1;
    bool hasOutput = GetStr(table, "mode") == "output-to-separate-pipe";
    Fluid* input = nullptr;
    TemperatureRange acceptTemperature;
    GetFluidBoxFilter(table, "fluid_box", 0, &input, &acceptTemperature);
    int targetTemp = GetInt(table, "target_temperature", 0);
    Fluid* output = input;
    if (hasOutput) {
      Fluid* fluid = nullptr;
      TemperatureRange ignored;
      output = GetFluidBoxFilter(table, "output_fluid_box", targetTemp, &fluid, &ignored)
                   ? fluid
                   : nullptr;
    }
    if (input != nullptr && output != nullptr) {
      std::string category = std::string(kBoilerRecipe) + boiler->name;
      Mechanics* recipe = CreateSpecialRecipe(output, category, "boiling");
      recipeCrafters_.Add(boiler, category);
      recipe->flags |= RecipeFlags::kUsesFluidTemperature;
      float inputEnergyPerOneFluid =
          (targetTemp - acceptTemperature.min) * input->heatCapacity;
      recipe->ingredients = {Ingredient(input, boiler->basePower / inputEnergyPerOneFluid)};
      recipe->ingredients[0].temperature = acceptTemperature;
      float outputEnergyPerOneFluid =
          (targetTemp - output->temperatureRange.min) * output->heatCapacity;
      recipe->products = {Product(output, boiler->basePower / outputEnergyPerOneFluid)};
      specialWithProducts_.insert(recipe);
      recipe->time = 1.0f;
      boiler->baseCraftingSpeed = 1.0f;
    }
  } else if (isGeneratorLike) {
    auto* generator = GetObject<EntityCrafter>(table);
    std::string maxPowerOutput;
    if (GetStrOpt(table, "max_power_output", maxPowerOutput)) {
      generator->basePower = static_cast<float>(LuaContext::ParseEnergy(maxPowerOutput));
    }
    Tbl burnerSource = GetTbl(table, "burner");
    if ((factorioVersion_ < Version{0, 18, 0} || factorioType == "burner-generator") &&
        burnerSource) {
      ReadEnergySource(burnerSource, generator);
    } else {
      generator->hasEnergy = true;
      generator->energy = EntityEnergy{};
      generator->energy.effectivity = GetFloat(table, "effectivity", 1);
      ReadFluidEnergySource(table, generator);
    }
    recipeCrafters_.Add(generator, kGeneratorRecipe);
  } else if (factorioType == "character") {
    auto* character = GetObject<EntityCrafter>(table);
    character->itemInputs = 255;
    if (Tbl mining = GetTbl(table, "mining_categories")) {
      for (const std::string& category : ArrayStrs(mining)) {
        recipeCrafters_.Add(character, std::string(kMiningRecipe) + category);
      }
    }
    if (Tbl crafting = GetTbl(table, "crafting_categories")) {
      for (const std::string& category : ArrayStrs(crafting)) {
        recipeCrafters_.Add(character, category);
      }
    }
    recipeCrafters_.Add(character, kTechnologyTrigger);
    character->hasEnergy = true;
    character->energy = EntityEnergy{};
    character->energy.type = EntityEnergyType::Labor;
    character->energy.effectivity = std::numeric_limits<float>::infinity();
    if (character->name == "character") {
      character->mapGenerated = true;
      rootAccessible_.insert(rootAccessible_.begin(), character);
    }
  } else if (factorioType == "constant-combinator") {
    if (name == "constant-combinator") {
      constantCombinatorCapacity_ = GetInt(table, "item_slot_count", 18);
    }
  } else if (factorioType == "container" || factorioType == "logistic-container") {
    auto* container = GetObject<EntityContainer>(table);
    container->inventorySize = GetInt(table, "inventory_size", 0);
    if (factorioType == "logistic-container") {
      container->logisticMode = GetStr(table, "logistic_mode");
      container->logisticSlotsCount = GetInt(table, "logistic_slots_count", 0);
      if (container->logisticSlotsCount == 0) {
        container->logisticSlotsCount = GetInt(table, "max_logistic_slots", 1000);
      }
    }
  } else if (factorioType == "electric-energy-interface") {
    auto* eei = GetObject<EntityCrafter>(table);
    eei->hasEnergy = true;
    eei->energy = EntityEnergy{};
    eei->energy.type = EntityEnergyType::Void;
    eei->energy.effectivity = std::numeric_limits<float>::infinity();
    std::string interfaceProduction;
    if (GetStrOpt(table, "energy_production", interfaceProduction)) {
      eei->baseCraftingSpeed = static_cast<float>(LuaContext::ParseEnergy(interfaceProduction));
      if (eei->baseCraftingSpeed > 0) recipeCrafters_.Add(eei, kGeneratorRecipe);
    }
  } else if (isCrafterLike) {
    auto* crafter = GetObject<EntityCrafter>(table);
    ParseModules(table, crafter, AllowedEffects::kNone);
    crafter->basePower = static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "energy_usage")));
    defaultDrain = crafter->basePower / 30.0f;
    crafter->baseCraftingSpeed = GetFloat(table, "crafting_speed", 1);
    crafter->itemInputs = factorioType == "furnace"
                              ? GetInt(table, "source_inventory_size", 1)
                              : GetInt(table, "ingredient_count", 255);
    if (Tbl fluidBoxes = GetTbl(table, "fluid_boxes")) {
      int count = 0;
      for (const Tbl& fluidBox : ArrayTbls(fluidBoxes)) {
        std::string prodType = GetStr(fluidBox, "production_type");
        if (prodType == "input-output" || prodType == "input") ++count;
      }
      crafter->fluidInputs = count;
    }
    if (Has(table, "vector_to_place_result")) crafter->hasVectorToPlaceResult = true;

    Recipe* fixedRecipe = nullptr;
    std::string fixedRecipeName;
    if (GetStrOpt(table, "fixed_recipe", fixedRecipeName)) {
      std::string fixedCategoryName = std::string(kFixedRecipe) + fixedRecipeName;
      fixedRecipe = GetObject<Recipe>(fixedRecipeName);
      recipeCrafters_.Add(crafter, fixedCategoryName);
      recipeCategories_.Add(fixedCategoryName, fixedRecipe);
    } else if (Tbl craftingCategories = GetTbl(table, "crafting_categories")) {
      for (const std::string& category : ArrayStrs(craftingCategories)) {
        recipeCrafters_.Add(crafter, category);
      }
    }

    if (factorioType == "rocket-silo") {
      bool launchToSpacePlatforms = GetBool(table, "launch_to_space_platforms", false);
      int rocketInventorySize;
      if (factorioVersion_ < Version{2, 1, 0} || !launchToSpacePlatforms) {
        rocketInventorySize =
            GetInt(table, "to_be_inserted_to_rocket_inventory_size",
                   factorioVersion_ < Version{2, 0, 0} ? 1 : 0);
      } else {
        rocketInventorySize = std::numeric_limits<int>::max();
      }
      if (rocketInventorySize > 0) {
        int partsRequired = GetInt(table, "rocket_parts_required", 100);
        if (fixedRecipe != nullptr) {
          Recipe* launchRecipe = CreateLaunchRecipe(crafter, fixedRecipe, partsRequired);
          formerAliases_["Mechanics.launch" + crafter->name + "." + crafter->name] =
              launchRecipe;
        } else {
          for (const std::string& categoryName : recipeCrafters_.GetRaw(crafter)) {
            for (RecipeOrTechnology* possible : recipeCategories_.GetRaw(categoryName)) {
              auto* rec = dynamic_cast<Recipe*>(possible);
              if (rec != nullptr) CreateLaunchRecipe(crafter, rec, partsRequired);
            }
          }
        }
        crafter->rocketInventorySize = rocketInventorySize;
      }
    }
  } else if (factorioType == "inserter") {
    auto* inserter = GetObject<EntityInserter>(table);
    inserter->inserterSwingTime = 1.0f / (GetFloat(table, "rotation_speed", 1) * 60);
    inserter->isBulkInserter = GetBool(table, "bulk", false) || GetBool(table, "stack", false);
  } else if (factorioType == "lab") {
    auto* lab = GetObject<EntityCrafter>(table);
    ParseModules(table, lab, AllowedEffects::kAll & ~AllowedEffects::kQuality);
    lab->basePower = static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "energy_usage")));
    lab->baseCraftingSpeed = GetFloat(table, "researching_speed", 1);
    recipeCrafters_.Add(lab, kLabs);
    if (Tbl inputs = GetTbl(table, "inputs")) {
      lab->inputs.clear();
      for (const std::string& input : ArrayStrs(inputs)) {
        lab->inputs.push_back(GetObject<Item>(input));
      }
    }
    lab->itemInputs = static_cast<int>(lab->inputs.size());
  } else if (factorioType == "lightning-attractor") {
    int range = GetInt(table, "range_elongation", 0);
    float efficiency = GetFloat(table, "efficiency", 0);
    if (range != 0 && efficiency > 0) {
      auto* attractor = GetObject<EntityAttractor>(table);
      attractor->hasEnergy = true;
      attractor->energy = EntityEnergy{};
      attractor->energy.type = EntityEnergyType::Void;
      attractor->energy.effectivity = std::numeric_limits<float>::infinity();
      attractor->range = static_cast<float>(range);
      attractor->attractorEfficiency = efficiency;
      if (Tbl energy = GetTbl(table, "energy_source")) {
        std::string drain;
        if (GetStrOpt(energy, "drain", drain)) {
          attractor->drain = static_cast<float>(LuaContext::ParseEnergy(drain)) * 60;
        }
      }
      recipeCrafters_.Add(attractor, kGeneratorRecipe);
    }
  } else if (factorioType == "mining-drill") {
    auto* drill = GetObject<EntityCrafter>(table);
    drill->basePower = static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "energy_usage")));
    ParseModules(table, drill, AllowedEffects::kAll);
    drill->baseCraftingSpeed = GetFloat(table, "mining_speed", 1);
    if (Has(table, "input_fluid_box")) drill->fluidInputs = 1;
    drill->hasVectorToPlaceResult = true;
    if (Tbl resourceCategories = GetTbl(table, "resource_categories")) {
      for (const std::string& resource : ArrayStrs(resourceCategories)) {
        recipeCrafters_.Add(drill, std::string(kMiningRecipe) + resource);
      }
    }
  } else if (factorioType == "offshore-pump") {
    auto* pump = GetObject<EntityCrafter>(table);
    pump->basePower = static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "energy_usage")));
    pump->baseCraftingSpeed = GetFloat(table, "pumping_speed", 20) / 20.0f;
    std::string fluidName;
    Tbl fluidBox = GetTbl(table, "fluid_box");
    bool hasFilter =
        (fluidBox && GetStrOpt(fluidBox, "filter", fluidName)) ||
        GetStrOpt(table, "fluid", fluidName);
    pump->hasEnergy = true;
    pump->energy = EntityEnergy{};
    pump->energy.type = EntityEnergyType::Void;
    pump->energy.effectivity = std::numeric_limits<float>::infinity();
    if (hasFilter) {
      Fluid* pumpingFluid = GetFluidFixedTemp(fluidName, 0);
      std::string recipeCategory = std::string(kPumpingRecipe) + pumpingFluid->name;
      Mechanics* recipe = CreateSpecialRecipe(pumpingFluid, recipeCategory, "pumping");
      recipeCrafters_.Add(pump, recipeCategory);
      if (!specialWithProducts_.count(recipe)) {
        recipe->products = {Product(pumpingFluid, 1200.0f)};
        recipe->ingredients = {};
        recipe->time = 1.0f;
        specialWithProducts_.insert(recipe);
      }
    } else {
      recipeCrafters_.Add(pump, std::string(kPumpingRecipe) + "tile");
    }
  } else if (factorioType == "projectile") {
    auto* projectile = GetObject<EntityProjectile>(table);
    if (Tbl actions = GetTbl(table, "action")) {
      ReadObjectOrArray(actions, [&](const Tbl& action) {
        if (GetStr(action, "type") != "direct") return;
        if (Tbl delivery = GetTbl(action, "action_delivery")) {
          ReadObjectOrArray(delivery, [&](const Tbl& d) {
            if (GetStr(d, "type") != "instant") return;
            if (Tbl effects = GetTbl(d, "target_effects")) {
              ReadObjectOrArray(effects, [&](const Tbl& effect) {
                std::string created;
                if (GetStr(effect, "type") == "create-entity" &&
                    GetStrOpt(effect, "entity_name", created)) {
                  projectile->placeEntities.push_back(created);
                }
              });
            }
          });
        }
      });
    }
  } else if (factorioType == "reactor") {
    auto* reactor = GetObject<EntityReactor>(table);
    reactor->reactorNeighborBonus = GetFloat(table, "neighbour_bonus", 1);
    reactor->basePower = static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "consumption")));
    reactor->baseCraftingSpeed = reactor->basePower;
    Tbl heatBuffer = GetTbl(table, "heat_buffer");
    int maxTemp = GetInt(heatBuffer, "max_temperature", 1000);
    Special* heatVariant = GetHeatFixedTemp(maxTemp);
    std::string reactorCategory =
        std::string(kReactorRecipe) + "@" + std::to_string(maxTemp);
    Mechanics* reactorRecipe = CreateSpecialRecipe(heatVariant, reactorCategory, "generating");
    if (!specialWithProducts_.count(reactorRecipe)) {
      reactorRecipe->products = {Product(heatVariant, 1)};
      reactorRecipe->flags |= RecipeFlags::kScaleProductionWithPower;
      reactorRecipe->ingredients = {};
      specialWithProducts_.insert(reactorRecipe);
    }
    recipeCrafters_.Add(reactor, reactorCategory);
    formerAliases_.emplace(std::string("Mechanics.") + kReactorRecipe + "." + kHeat,
                           reactorRecipe);
  } else if (factorioType == "solar-panel") {
    auto* solarPanel = GetObject<EntityCrafter>(table);
    solarPanel->hasEnergy = true;
    solarPanel->energy = EntityEnergy{};
    solarPanel->energy.type = EntityEnergyType::Void;
    solarPanel->energy.effectivity = std::numeric_limits<float>::infinity();
    recipeCrafters_.Add(solarPanel, kGeneratorRecipe);
    solarPanel->baseCraftingSpeed =
        static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "production"))) * 0.7f;
  } else if (factorioType == "transport-belt") {
    GetObject<EntityBelt>(table)->beltItemsPerSecond = GetFloat(table, "speed", 0) * 480.0f;
  } else if (factorioType == "unit-spawner") {
    auto* spawner = GetObject<EntitySpawner>(table);
    spawner->capturedEntityName = GetStr(table, "captured_spawner_entity");
  }

  Entity* entity = DeserializeCommon<Entity>(table, "entity");

  if (Tbl lootList = GetTbl(table, "loot")) {
    entity->loot.clear();
    if (factorioVersion_ < Version{2, 1, 0}) {
      for (const Tbl& x : ArrayTbls(lootList)) {
        entity->loot.emplace_back(GetObject<Item>(GetStr(x, "item")),
                                  GetFloat(x, "count_min", 1), GetFloat(x, "count_max", 1),
                                  GetFloat(x, "probability", 1));
      }
    } else {
      auto loader = LoadProduct(entity->name + ".loot");
      for (const Tbl& x : ArrayTbls(lootList)) entity->loot.push_back(loader(x));
    }
  }

  if (Tbl minable = GetTbl(table, "minable")) {
    std::vector<Product> products = LoadProductList(minable, "minable",
                                                    /*allowSimpleSyntax=*/true);
    if (factorioType == "resource") {
      std::string category = GetStr(table, "category", "basic-solid");
      Mechanics* recipe =
          CreateSpecialRecipe(entity, std::string(kMiningRecipe) + category, "mining");
      recipe->flags = RecipeFlags::kUsesMiningProductivity;
      recipe->time = GetFloat(minable, "mining_time", 1);
      recipe->products = products;
      specialWithProducts_.insert(recipe);
      recipe->allowedEffects = AllowedEffects::kAll;
      recipe->sourceEntity = entity;

      std::string requiredFluid;
      if (GetStrOpt(minable, "required_fluid", requiredFluid)) {
        float amount = GetFloat(minable, "fluid_amount", 0);
        recipe->ingredients = {
            Ingredient(GetObject<Fluid>(requiredFluid), amount / 10.0f)};  // 10x: upstream
        for (FactorioObject* o : allObjects_) {
          auto* tech = dynamic_cast<Technology*>(o);
          if (tech != nullptr && tech->unlocksFluidMining) {
            recipe->enabled = false;
            tech->unlockRecipes.push_back(recipe);
          }
        }
      } else {
        recipe->ingredients = {};
      }
    } else if (factorioType == "plant") {
      for (const auto& [seed, plantName] : plantResults_) {
        if (plantName != name) continue;
        Mechanics* recipe = CreateSpecialRecipe(seed, kPlantRecipe, "planting");
        recipe->time = GetInt(table, "growth_ticks", 0) / 60.0f;
        recipe->ingredients = {Ingredient(seed, 1)};
        recipe->products = products;
        specialWithProducts_.insert(recipe);
      }
      entity->loot = products;
    } else {
      entity->loot = products;
    }
  }

  if (Tbl box = GetTbl(table, "selection_box")) {
    Tbl topLeft = GetTblIdx(box, 1);
    Tbl bottomRight = GetTblIdx(box, 2);
    float x0 = static_cast<float>(GetNumIdx(topLeft, 1));
    float y0 = static_cast<float>(GetNumIdx(topLeft, 2));
    float x1 = static_cast<float>(GetNumIdx(bottomRight, 1));
    float y1 = static_cast<float>(GetNumIdx(bottomRight, 2));
    entity->width = RoundF(x1 - x0);
    entity->height = RoundF(y1 - y0);
  } else {
    entity->width = 3;
    entity->height = 3;
  }
  entity->size = std::max(entity->width, entity->height);

  Tbl energySource = GetTbl(table, "energy_source");
  if (energySource && !kNoDefaultEnergyParsing.count(factorioType)) {
    ReadEnergySource(energySource, entity, defaultDrain);
  }

  if (auto* entityCrafter = dynamic_cast<EntityCrafter*>(entity)) {
    Tbl effectReceiver = GetTbl(table, "effect_receiver");
    EffectReceiver receiver;
    if (effectReceiver) {
      if (Tbl baseEffect = GetTbl(effectReceiver, "base_effect")) {
        auto load = [&](const char* effect) {
          if (Tbl t = GetTbl(baseEffect, effect)) return GetFloat(t, "bonus", 0);
          return GetFloat(baseEffect, effect, 0);
        };
        receiver.baseEffect = Effect{.consumption = load("consumption"),
                                     .speed = load("speed"),
                                     .productivity = load("productivity"),
                                     .pollution = load("pollution"),
                                     .quality = load("quality")};
      }
      receiver.usesModuleEffects = GetBool(effectReceiver, "uses_module_effects", true);
      receiver.usesBeaconEffects = GetBool(effectReceiver, "uses_beacon_effects ", true);
      receiver.usesSurfaceEffects = GetBool(effectReceiver, "uses_surface_effects ", true);
    }
    entityCrafter->effectReceiver = receiver;
    entityCrafter->effectReceiverBaseProductivity = receiver.baseEffect.productivity;
  }

  if (Tbl generation = GetTbl(table, "autoplace")) {
    if (Tbl prob = GetTbl(generation, "probability_expression")) {
      float probability = EstimateNoiseExpression(prob);
      float richness = probability;
      if (Tbl rich = GetTbl(generation, "richness_expression")) {
        richness = EstimateNoiseExpression(rich);
      }
      entity->mapGenDensity = richness * probability;
    } else if (Has(generation, "coverage")) {
      float coverage = GetFloat(generation, "coverage", 0);
      float richBase = GetFloat(generation, "richness_base", 0);
      float richMultiplier = GetFloat(generation, "richness_multiplier", 0);
      float richMultiplierDistance =
          GetFloat(generation, "richness_multiplier_distance_bonus", 0);
      entity->mapGenDensity =
          coverage *
          (richBase + richMultiplier + richMultiplierDistance * kEstimationDistanceFromCenter);
    }
    std::string control;
    if (GetStrOpt(generation, "control", control)) {
      entity->mapGenerated = true;
      entity->autoplaceControl = control;
    }
  }

  if (entity->hasEnergy && (entity->energy.type == EntityEnergyType::Void ||
                            entity->energy.type == EntityEnergyType::Labor)) {
    fuelUsers_.Add(entity, kVoid);
  }

  entity->heatingPower =
      static_cast<float>(LuaContext::ParseEnergy(GetStr(table, "heating_energy")));

  if (Tbl healthEffect = GetTbl(table, "production_health_effect")) {
    if (Has(healthEffect, "not_producing")) {
      float lossPerTick = GetFloat(healthEffect, "not_producing", 0);
      entity->baseSpoilTime = GetFloat(table, "max_health", 0) * -60 * lossPerTick;
      if (Tbl dying = GetTbl(table, "dying_trigger_effect")) {
        ReadObjectOrArray(dying, [&](const Tbl& effect) {
          std::string spoilEntity;
          if (GetStr(effect, "type") == "create-entity" &&
              GetStrOpt(effect, "entity_name", spoilEntity)) {
            pendingEntitySpoil_.emplace_back(entity, spoilEntity);
          }
        });
      }
    }
  }
}

float DataDeserializer::EstimateArgument(const Tbl& args, const char* name, float def) {
  Tbl res = GetTbl(args, name);
  return res ? EstimateNoiseExpression(res) : def;
}

float DataDeserializer::EstimateArgumentIdx(const Tbl& args, int index, float def) {
  Tbl res = GetTblIdx(args, index);
  return res ? EstimateNoiseExpression(res) : def;
}

float DataDeserializer::EstimateNoiseExpression(const Tbl& expression) {
  std::string type = GetStr(expression, "type", "typed");

  if (type == "variable") {
    std::string varName = GetStr(expression, "variable_name");
    if (varName == "x" || varName == "y" || varName == "distance") {
      return kEstimationDistanceFromCenter;
    }
    if (Tbl noiseExpressions = GetTbl(raw_, "noise-expression")) {
      if (Tbl noiseExpr = GetTbl(noiseExpressions, varName.c_str())) {
        return EstimateArgument(noiseExpr, "expression");
      }
    }
    return 1.0f;
  }
  if (type == "function-application") {
    std::string funName = GetStr(expression, "function_name");
    Tbl args = GetTbl(expression, "arguments");
    if (!args) return 0;

    if (funName == "add") {
      float res = 0;
      for (const Tbl& el : ArrayTbls(args)) res += EstimateNoiseExpression(el);
      return res;
    }
    if (funName == "multiply") {
      float res = 1;
      for (const Tbl& el : ArrayTbls(args)) res *= EstimateNoiseExpression(el);
      return res;
    }
    if (funName == "subtract") return EstimateArgumentIdx(args, 1) - EstimateArgumentIdx(args, 2);
    if (funName == "divide") return EstimateArgumentIdx(args, 1) / EstimateArgumentIdx(args, 2);
    if (funName == "exponentiate") {
      return std::pow(EstimateArgumentIdx(args, 1), EstimateArgumentIdx(args, 2));
    }
    if (funName == "absolute-value") return std::abs(EstimateArgumentIdx(args, 1));
    if (funName == "clamp") {
      return std::clamp(EstimateArgumentIdx(args, 1), EstimateArgumentIdx(args, 2),
                        EstimateArgumentIdx(args, 3));
    }
    if (funName == "log2") {
      return std::log2(EstimateArgumentIdx(args, 1));
    }
    if (funName == "distance-from-nearest-point") {
      return EstimateArgument(args, "maximum_distance");
    }
    if (funName == "ridge") {
      return (EstimateArgumentIdx(args, 2) + EstimateArgumentIdx(args, 3)) * 0.5f;
    }
    if (funName == "terrace") return EstimateArgument(args, "value");
    if (funName == "random-penalty") {
      float source = EstimateArgument(args, "source");
      float penalty = EstimateArgument(args, "amplitude");
      if (penalty > source) return source / penalty;
      return (source + source - penalty) / 2;
    }
    if (funName == "spot-noise") {
      float quantity = EstimateArgument(args, "spot_quantity_expression");
      float spotCount;
      if (Tbl spots = GetTbl(args, "candidate_spot_count")) {
        spotCount = EstimateNoiseExpression(spots);
      } else {
        spotCount = EstimateArgument(args, "candidate_point_count", 256) /
                    EstimateArgument(args, "skip_span", 1);
      }
      float regionSize = EstimateArgument(args, "region_size", 512);
      regionSize *= regionSize;
      return spotCount * quantity / regionSize;
    }
    if (funName == "factorio-basis-noise" || funName == "factorio-quick-multioctave-noise" ||
        funName == "factorio-multioctave-noise") {
      return 0.1f * EstimateArgument(args, "output_scale", 1);
    }
    return 0;
  }
  if (type == "procedure-delimiter") return EstimateArgument(expression, "expression");
  if (type == "literal-number") return GetFloat(expression, "literal_value", 0);
  if (type == "literal-expression") return EstimateArgument(expression, "literal_value");
  return 0;
}

}  // namespace yafc
