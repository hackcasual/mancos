#include "yafc/parser/data_deserializer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <stdexcept>
#include <typeinfo>

namespace yafc {

// Upstream SpecialNames (Database.cs).
namespace special_names {
constexpr const char* kBurnableFluid = "burnable-fluid.";
constexpr const char* kHeat = "heat";
constexpr const char* kVoid = "void";
constexpr const char* kElectricity = "electricity";
constexpr const char* kHotFluid = "hot-fluid";
constexpr const char* kSpecificFluid = "fluid.";
constexpr const char* kMiningRecipe = "mining.";
constexpr const char* kBoilerRecipe = "boiler.";
constexpr const char* kFakeRecipe = "fake-recipe";
constexpr const char* kFixedRecipe = "fixed-recipe.";
constexpr const char* kGeneratorRecipe = "generator";
constexpr const char* kPumpingRecipe = "pump.";
constexpr const char* kLabs = "labs.";
constexpr const char* kTechnologyTrigger = "technology-trigger";
constexpr const char* kRocketLaunch = "launch";
constexpr const char* kRocketCraft = "rocket.";
constexpr const char* kReactorRecipe = "reactor";
constexpr const char* kSpoilRecipe = "spoil";
constexpr const char* kPlantRecipe = "plant";
constexpr const char* kAsteroidCapture = "asteroid-capture";
}  // namespace special_names

namespace sn = special_names;

// --------------------------------------------------------- lua helpers ----

DataDeserializer::Tbl DataDeserializer::Wrap(LuaValue&& value) const {
  if (value.kind != LuaValue::Kind::Table) return {};
  LuaContext* L = &lua_;
  return Tbl{L, std::shared_ptr<int>(new int(value.tableRef), [L](int* ref) {
               L->Unref(*ref);
               delete ref;
             })};
}

DataDeserializer::Tbl DataDeserializer::GetTbl(const Tbl& t, const char* key) const {
  if (!t) return {};
  return Wrap(lua_.GetField(*t.ref, key));
}

DataDeserializer::Tbl DataDeserializer::GetTblIdx(const Tbl& t, int index) const {
  if (!t) return {};
  return Wrap(lua_.GetIndex(*t.ref, index));
}

bool DataDeserializer::Has(const Tbl& t, const char* key) const {
  if (!t) return false;
  LuaValue v = lua_.GetField(*t.ref, key);
  bool has = v.kind != LuaValue::Kind::Nil;
  if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
  return has;
}

namespace {
double NumberOf(const LuaValue& v, double def) {
  // Upstream converters accept numbers and numeric strings.
  if (v.kind == LuaValue::Kind::Number) return v.number;
  if (v.kind == LuaValue::Kind::String) {
    char* end = nullptr;
    double parsed = std::strtod(v.string.c_str(), &end);
    if (end != v.string.c_str()) return parsed;
  }
  return def;
}
}  // namespace

std::string DataDeserializer::GetStr(const Tbl& t, const char* key,
                                     const std::string& def) const {
  if (!t) return def;
  LuaValue v = lua_.GetField(*t.ref, key);
  if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
  return v.kind == LuaValue::Kind::String ? v.string : def;
}

bool DataDeserializer::GetStrOpt(const Tbl& t, const char* key, std::string& out) const {
  if (!t) return false;
  LuaValue v = lua_.GetField(*t.ref, key);
  if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
  if (v.kind != LuaValue::Kind::String) return false;
  out = v.string;
  return true;
}

double DataDeserializer::GetNum(const Tbl& t, const char* key, double def) const {
  if (!t) return def;
  LuaValue v = lua_.GetField(*t.ref, key);
  if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
  return NumberOf(v, def);
}

float DataDeserializer::GetFloat(const Tbl& t, const char* key, float def) const {
  return static_cast<float>(GetNum(t, key, def));
}

int DataDeserializer::GetInt(const Tbl& t, const char* key, int def) const {
  return static_cast<int>(GetNum(t, key, def));
}

long long DataDeserializer::GetLong(const Tbl& t, const char* key, long long def) const {
  return static_cast<long long>(GetNum(t, key, static_cast<double>(def)));
}

bool DataDeserializer::GetBool(const Tbl& t, const char* key, bool def) const {
  if (!t) return def;
  LuaValue v = lua_.GetField(*t.ref, key);
  if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
  if (v.kind == LuaValue::Kind::Boolean) return v.boolean;
  if (v.kind == LuaValue::Kind::String) {
    if (v.string == "true") return true;
    if (v.string == "false") return false;
  }
  return def;
}

double DataDeserializer::GetNumIdx(const Tbl& t, int index, double def) const {
  if (!t) return def;
  LuaValue v = lua_.GetIndex(*t.ref, index);
  if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
  return NumberOf(v, def);
}

std::string DataDeserializer::GetStrIdx(const Tbl& t, int index,
                                        const std::string& def) const {
  if (!t) return def;
  LuaValue v = lua_.GetIndex(*t.ref, index);
  if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
  return v.kind == LuaValue::Kind::String ? v.string : def;
}

int DataDeserializer::ArrayLen(const Tbl& t) const {
  return t ? lua_.RawLen(*t.ref) : 0;
}

std::vector<DataDeserializer::Tbl> DataDeserializer::ArrayTbls(const Tbl& t) const {
  std::vector<Tbl> result;
  int n = ArrayLen(t);
  for (int i = 1; i <= n; ++i) {
    Tbl e = GetTblIdx(t, i);
    if (e) result.push_back(std::move(e));
  }
  return result;
}

std::vector<std::string> DataDeserializer::ArrayStrs(const Tbl& t) const {
  std::vector<std::string> result;
  int n = ArrayLen(t);
  for (int i = 1; i <= n; ++i) {
    LuaValue v = lua_.GetIndex(*t.ref, i);
    if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
    if (v.kind == LuaValue::Kind::String) result.push_back(std::move(v.string));
  }
  return result;
}

std::vector<double> DataDeserializer::ArrayNums(const Tbl& t) const {
  std::vector<double> result;
  int n = ArrayLen(t);
  for (int i = 1; i <= n; ++i) {
    LuaValue v = lua_.GetIndex(*t.ref, i);
    if (v.kind == LuaValue::Kind::Table) lua_.Unref(v.tableRef);
    if (v.kind == LuaValue::Kind::Number) result.push_back(v.number);
  }
  return result;
}

std::vector<std::string> DataDeserializer::Keys(const Tbl& t) const {
  return t ? lua_.StringKeys(*t.ref) : std::vector<std::string>{};
}

void DataDeserializer::ReadObjectOrArray(
    const Tbl& t, const std::function<void(const Tbl&)>& fn) const {
  if (!t) return;
  std::vector<Tbl> elements = ArrayTbls(t);
  if (!elements.empty()) {
    for (const Tbl& e : elements) fn(e);
  } else {
    fn(t);
  }
}

// ------------------------------------------------------------- registry ----

FactorioObject* DataDeserializer::CreateConcrete(Kind kind, const std::string& typeInRaw,
                                                 const std::string& name) {
  std::unique_ptr<FactorioObject> obj;
  if (kind == Kind::Item) {
    if (typeInRaw == "ammo") obj = std::make_unique<Ammo>();
    else if (typeInRaw == "module") obj = std::make_unique<Module>();
    else obj = std::make_unique<Item>();
  } else if (kind == Kind::Entity) {
    // Upstream type -> C# class switch.
    if (typeInRaw == "accumulator") obj = std::make_unique<EntityAccumulator>();
    else if (typeInRaw == "beacon") obj = std::make_unique<EntityBeacon>();
    else if (typeInRaw == "container" || typeInRaw == "logistic-container")
      obj = std::make_unique<EntityContainer>();
    else if (typeInRaw == "inserter") obj = std::make_unique<EntityInserter>();
    else if (typeInRaw == "lightning-attractor") obj = std::make_unique<EntityAttractor>();
    else if (typeInRaw == "projectile") obj = std::make_unique<EntityProjectile>();
    else if (typeInRaw == "pump") obj = std::make_unique<EntityPump>();
    else if (typeInRaw == "reactor") obj = std::make_unique<EntityReactor>();
    else if (typeInRaw == "transport-belt") obj = std::make_unique<EntityBelt>();
    else if (typeInRaw == "unit-spawner") obj = std::make_unique<EntitySpawner>();
    else if (typeInRaw == "agricultural-tower" || typeInRaw == "assembling-machine" ||
             typeInRaw == "asteroid-collector" || typeInRaw == "boiler" ||
             typeInRaw == "burner-generator" || typeInRaw == "character" ||
             typeInRaw == "electric-energy-interface" || typeInRaw == "furnace" ||
             typeInRaw == "generator" || typeInRaw == "lab" ||
             typeInRaw == "mining-drill" || typeInRaw == "offshore-pump" ||
             typeInRaw == "rocket-silo" || typeInRaw == "solar-panel")
      obj = std::make_unique<EntityCrafter>();
    else obj = std::make_unique<Entity>();
  } else {
    switch (kind) {
      case Kind::Fluid: obj = std::make_unique<Fluid>(); break;
      case Kind::Recipe: obj = std::make_unique<Recipe>(); break;
      case Kind::Mechanics: obj = std::make_unique<Mechanics>(); break;
      case Kind::Technology: obj = std::make_unique<Technology>(); break;
      case Kind::Quality: obj = std::make_unique<Quality>(); break;
      case Kind::Location: obj = std::make_unique<Location>(); break;
      case Kind::Tile: obj = std::make_unique<Tile>(); break;
      case Kind::Special: obj = std::make_unique<Special>(); break;
      case Kind::Trigger: obj = std::make_unique<ResearchTrigger>(); break;
      default: throw std::runtime_error("bad kind");
    }
  }
  obj->name = name;
  FactorioObject* raw = obj.get();
  owned_.push_back(std::move(obj));
  allObjects_.push_back(raw);
  return raw;
}

FactorioObject* DataDeserializer::GetObjectRaw(Kind kind, const std::string& name,
                                               const char* requestedType) {
  auto key = std::make_pair(kind, name);
  auto it = registeredObjects_.find(key);
  if (it != registeredObjects_.end()) return it->second;

  std::string typeInRaw;
  if (kind == Kind::Item || kind == Kind::Entity) {
    const char* prototype = kind == Kind::Item ? "item" : "entity";
    auto pit = prototypes_.find({prototype, name});
    if (pit == prototypes_.end()) pit = prototypes_.find({"asteroid-chunk", name});
    if (pit == prototypes_.end()) {
      throw std::runtime_error("data.raw does not contain an object named '" + name +
                               "' that can be loaded as a(n) " + requestedType);
    }
    typeInRaw = pit->second;
  }

  FactorioObject* obj = CreateConcrete(kind, typeInRaw, name);
  registeredObjects_[key] = obj;
  return obj;
}

// ---------------------------------------------------- fluid/heat variants ----

Fluid* DataDeserializer::GetFluidFixedTemp(const std::string& key, int temperature) {
  Fluid* basic = GetObject<Fluid>(key);
  if (basic->temperature == temperature) return basic;
  if (temperature < basic->temperatureRange.min) {
    temperature = basic->temperatureRange.min;
  }
  std::string idWithTemp = key + "@" + std::to_string(temperature);

  if (basic->temperature == 0) {
    basic->SetTemperature(temperature);
    registeredObjects_[{Kind::Fluid, idWithTemp}] = basic;
    return basic;
  }

  auto it = registeredObjects_.find({Kind::Fluid, idWithTemp});
  if (it != registeredObjects_.end()) return static_cast<Fluid*>(it->second);

  Fluid* split = SplitFluid(basic, temperature);
  registeredObjects_[{Kind::Fluid, idWithTemp}] = split;
  return split;
}

Fluid* DataDeserializer::SplitFluid(Fluid* basic, int temperature) {
  if (basic->variants == nullptr) {
    basic->variants = std::make_shared<std::vector<Fluid*>>();
    basic->variants->push_back(basic);
  }
  auto copy = std::make_unique<Fluid>(*basic);
  copy->SetTemperature(temperature);
  Fluid* raw = copy.get();
  raw->variants->push_back(raw);
  if (raw->fuelValue > 0) fuels_.Add(sn::kBurnableFluid, raw);
  fuels_.Add(std::string(sn::kSpecificFluid) + basic->name, raw);
  owned_.push_back(std::move(copy));
  allObjects_.push_back(raw);
  return raw;
}

namespace {
// Upstream AddTemperatureToIcon: overlay digit layers describing the
// temperature; pure icon-spec data (the web layer renders the digits).
void AddTemperatureToIcon(FactorioObject* obj, int temperature) {
  std::string iconStr = std::to_string(temperature) + "d";
  int size = obj->iconSpec.empty() ? 64 : obj->iconSpec[0].size;
  float shift = 7.0f * size / 32;
  float offset = -11.5f * size / 32 + 0.25f;
  for (size_t n = 0; n < iconStr.size() && n < 4; ++n) {
    FactorioIconPart part;
    part.path = std::string("__.__/") + iconStr[n];
    part.size = size;
    part.y = offset;
    part.x = n * shift + offset;
    part.scale = 0.28f;
    obj->iconSpec.push_back(std::move(part));
  }
}
}  // namespace

void DataDeserializer::UpdateSplitFluids() {
  std::set<const std::vector<Fluid*>*> processed;

  for (FactorioObject* o : allObjects_) {
    auto* fluid = dynamic_cast<Fluid*>(o);
    if (fluid == nullptr) continue;
    if (fluid->temperature == 0) fluid->temperature = fluid->temperatureRange.min;
    if (fluid->variants == nullptr || !processed.insert(fluid->variants.get()).second) {
      continue;
    }
    std::sort(fluid->variants->begin(), fluid->variants->end(),
              [](const Fluid* a, const Fluid* b) { return a->temperature < b->temperature; });
    fluidVariants_[fluid->typeDotName()] = *fluid->variants;
    for (Fluid* variant : *fluid->variants) {
      AddTemperatureToIcon(variant, variant->temperature);
      variant->name += "@" + std::to_string(variant->temperature);
    }
  }
}

Special* DataDeserializer::GetHeatFixedTemp(int temperature) {
  if (heat_->temperature == temperature) return heat_;
  std::string idWithTemp = std::string(sn::kHeat) + "@" + std::to_string(temperature);
  if (heat_->temperature == 0) {
    heat_->temperature = temperature;
    registeredObjects_[{Kind::Special, idWithTemp}] = heat_;
    return heat_;
  }
  auto it = registeredObjects_.find({Kind::Special, idWithTemp});
  if (it != registeredObjects_.end()) return static_cast<Special*>(it->second);
  Special* split = SplitHeat(heat_, temperature);
  registeredObjects_[{Kind::Special, idWithTemp}] = split;
  return split;
}

Special* DataDeserializer::SplitHeat(Special* basic, int temperature) {
  if (basic->variants == nullptr) {
    basic->variants = std::make_shared<std::vector<Special*>>();
    basic->variants->push_back(basic);
  }
  auto copy = std::make_unique<Special>(*basic);
  copy->temperature = temperature;
  Special* raw = copy.get();
  raw->variants->push_back(raw);
  fuels_.Add(sn::kHeat, raw);
  owned_.push_back(std::move(copy));
  allObjects_.push_back(raw);
  return raw;
}

void DataDeserializer::UpdateSplitHeats() {
  if (heat_->temperature == 0 || heat_->variants == nullptr) return;
  std::sort(heat_->variants->begin(), heat_->variants->end(),
            [](const Special* a, const Special* b) { return a->temperature < b->temperature; });
  for (Special* variant : *heat_->variants) {
    AddTemperatureToIcon(variant, variant->temperature);
    variant->name += "@" + std::to_string(variant->temperature);
  }
}

// -------------------------------------------------------- special objects ----

Special* DataDeserializer::CreateSpecialObject(bool isPower, const std::string& name,
                                               const std::string& icon) {
  Special* obj = GetObject<Special>(name);
  obj->factorioType = "special";
  obj->iconSpec = {FactorioIconPart{.path = icon}};
  obj->power = isPower;
  if (isPower) {
    obj->fuelValue = 1.0f;
    fuels_.Add(name, obj);
  }
  return obj;
}

Item* DataDeserializer::CreateSpecialItem(const std::string& name) {
  Item* obj = GetObject<Item>(name);
  obj->factorioType = "special";
  obj->isLinkable = false;
  obj->showInExplorers = false;
  rootAccessible_.push_back(obj);
  return obj;
}

DataDeserializer::DataDeserializer(LuaContext& lua, const Version& factorioVersion)
    : lua_(lua), factorioVersion_(factorioVersion) {
  // Special items exist before prototype scanning (upstream `prototypes` seed).
  prototypes_[{"item", "science"}] = "item";
  prototypes_[{"item", "item-total-input"}] = "item";
  prototypes_[{"item", "item-total-output"}] = "item";

  electricity_ = CreateSpecialObject(
      true, sn::kElectricity, "__core__/graphics/icons/alerts/electricity-icon-unplugged.png");
  heat_ = CreateSpecialObject(true, sn::kHeat,
                              "__core__/graphics/arrows/heat-exchange-indication.png");
  voidEnergy_ = CreateSpecialObject(true, sn::kVoid, "__core__/graphics/icons/mip/infinity.png");
  voidEnergy_->isVoid = true;
  voidEnergy_->isLinkable = false;
  voidEnergy_->showInExplorers = false;
  rootAccessible_.push_back(voidEnergy_);

  if (!(factorioVersion_ < Version{2, 0, 0})) {
    rocketLaunch_ = CreateSpecialObject(
        false, sn::kRocketLaunch, "__base__/graphics/entity/rocket-silo/rocket-static-pod.png");
    science_ = GetObject<Item>("science");
    science_->showInExplorers = false;
  } else {
    rocketLaunch_ = CreateSpecialObject(
        false, sn::kRocketLaunch, "__base__/graphics/entity/rocket-silo/02-rocket.png");
    science_ = CreateSpecialItem("science");
    rootAccessible_.erase(
        std::find(rootAccessible_.begin(), rootAccessible_.end(), science_));
    science_->isLinkable = true;
  }
  formerAliases_["Special.research-unit"] = science_;

  Mechanics* generatorProduction =
      CreateSpecialRecipe(electricity_, sn::kGeneratorRecipe, "generating");
  generatorProduction->products = {Product(electricity_, 1.0f)};
  generatorProduction->flags |= RecipeFlags::kScaleProductionWithPower;
  generatorProduction->ingredients = {};
  specialWithProducts_.insert(generatorProduction);

  Item* totalItemInput = CreateSpecialItem("item-total-input");
  Item* totalItemOutput = CreateSpecialItem("item-total-output");
  formerAliases_["Special.total-item-input"] = totalItemInput;
  formerAliases_["Special.total-item-output"] = totalItemOutput;
}

Mechanics* DataDeserializer::CreateSpecialRecipe(FactorioObject* production,
                                                 const std::string& category,
                                                 const std::string& localizationKey) {
  std::string fullName =
      category + (category.ends_with('.') ? "" : ".") + production->name;
  auto it = registeredObjects_.find({Kind::Mechanics, fullName});
  if (it != registeredObjects_.end()) return static_cast<Mechanics*>(it->second);

  Mechanics* recipe = GetObject<Mechanics>(fullName);
  recipe->time = 1.0f;
  recipe->factorioType = sn::kFakeRecipe;
  recipe->name = fullName;
  recipe->source = production;
  recipe->localizationKey = localizationKey;
  recipe->enabled = true;
  recipe->hidden = true;
  recipe->technologyUnlock = {};
  recipeCategories_.Add(category, recipe);
  return recipe;
}

void DataDeserializer::EnsureSpoilageEntityExists() {
  if (spoilageEntity_ != nullptr) return;
  auto entity = std::make_unique<EntityCrafter>();
  entity->name = "spoilage";
  entity->factorioType = "spoilage";
  entity->iconSpec = {FactorioIconPart{.path = "__core__/graphics/clock-icon.png"}};
  entity->hasEnergy = true;
  entity->energy.type = EntityEnergyType::Void;
  entity->energy.effectivity = std::numeric_limits<float>::infinity();
  entity->mapGenerated = true;
  entity->itemInputs = 1;
  entity->effectReceiver.usesModuleEffects = false;
  entity->effectReceiver.usesBeaconEffects = false;
  entity->effectReceiver.usesSurfaceEffects = false;
  spoilageEntity_ = entity.get();
  allObjects_.push_back(entity.get());
  registeredObjects_[{Kind::Entity, "spoilage"}] = entity.get();
  rootAccessible_.push_back(entity.get());
  recipeCrafters_.Add(spoilageEntity_, sn::kSpoilRecipe);
  owned_.push_back(std::move(entity));
}

void DataDeserializer::EnsureLaunchRecipe(Item* item,
                                          const std::vector<Product>* launchProducts) {
  Mechanics* recipe = CreateSpecialRecipe(item, sn::kRocketLaunch, "launched");
  int ingredientCount = factorioVersion_ < Version{2, 0, 0} ? item->stackSize : 1;
  recipe->ingredients = {Ingredient(item, static_cast<float>(ingredientCount)),
                         Ingredient(rocketLaunch_, 1)};
  if (launchProducts != nullptr) {
    recipe->products = *launchProducts;
    specialWithProducts_.insert(recipe);
  } else if (!specialWithProducts_.count(recipe)) {
    recipe->products = {};
  }
  recipe->time = 0.0f;  // TODO(upstream): what to put here?
}

// ------------------------------------------------------------- main flow ----

void DataDeserializer::LoadPrototypes(const Tbl& raw, const Tbl& prototypes) {
  for (const std::string& prototype : Keys(prototypes)) {
    Tbl types = GetTbl(prototypes, prototype.c_str());
    if (!types) continue;
    for (const std::string& type : Keys(types)) {
      Tbl rawTable = GetTbl(raw, type.c_str());
      if (!rawTable) continue;
      for (const std::string& name : Keys(rawTable)) {
        prototypes_[{prototype, name}] = type;
      }
    }
  }
}

void DataDeserializer::DeserializePrototypes(const Tbl& raw, const std::string& type,
                                             void (DataDeserializer::*fn)(const Tbl&)) {
  Tbl table = GetTbl(raw, type.c_str());
  if (!table) return;
  for (const std::string& key : Keys(table)) {
    Tbl entry = GetTbl(table, key.c_str());
    if (!entry) continue;
    try {
      (this->*fn)(entry);
    } catch (const std::exception& e) {
      Error("failed to load " + type + "." + key + ": " + e.what());
    }
  }
}

void DataDeserializer::DeserializeItem(const Tbl& table) {
  if (GetStr(table, "type") == "module") {
    if (Tbl moduleEffect = GetTbl(table, "effect")) {
      Module* module = GetObject<Module>(table);
      auto load = [&](const char* effect) {
        if (Tbl t = GetTbl(moduleEffect, effect)) return GetFloat(t, "bonus", 0);
        return GetFloat(moduleEffect, effect, 0);
      };
      module->moduleSpecification = ModuleSpecification{
          .category = GetStr(table, "category"),
          .baseConsumption = load("consumption"),
          .baseSpeed = load("speed"),
          .baseProductivity = load("productivity"),
          .basePollution = load("pollution"),
          .baseQuality = load("quality"),
      };
    }
  } else if (GetStr(table, "type") == "ammo") {
    if (Tbl ammoType = GetTbl(table, "ammo_type")) {
      Ammo* ammo = GetObject<Ammo>(table);
      ReadObjectOrArray(ammoType, [&](const Tbl& t) {
        if (Tbl action = GetTbl(t, "action")) {
          ReadObjectOrArray(action, [&](const Tbl& trigger) {
            if (GetStr(trigger, "type") == "direct") {
              if (Tbl delivery = GetTbl(trigger, "action_delivery")) {
                if (GetStr(delivery, "type") == "projectile") {
                  ammo->projectileNames.push_back(GetStr(delivery, "projectile"));
                }
              }
            }
          });
        }
      });
      if (Tbl targets = GetTbl(ammoType, "target_filter")) {
        ammo->targetFilter = ArrayStrs(targets);
      }
    }
  }

  Item* item = DeserializeCommon<Item>(table, "item");

  std::string placeResult;
  if (GetStrOpt(table, "place_result", placeResult) && !placeResult.empty()) {
    placeResults_[item] = {placeResult};
  }
  item->stackSize = GetInt(table, "stack_size", 1);

  std::string fuelValue;
  if (GetStrOpt(table, "fuel_value", fuelValue)) {
    item->fuelValue = static_cast<float>(LuaContext::ParseEnergy(fuelValue));
    GetRef<Item>(table, "burnt_result", item->fuelResult);
    std::string category;
    if (GetStrOpt(table, "fuel_category", category)) fuels_.Add(category, item);
  }

  std::optional<std::vector<Product>> launchProducts;
  if (GetStr(table, "send_to_orbit_mode", "not-sendable") != "not-sendable" ||
      item->factorioType == "space-platform-starter-pack" ||
      factorioVersion_ < Version{2, 0, 0}) {
    if (Tbl product = GetTbl(table, "rocket_launch_product")) {
      launchProducts = {
          {LoadProduct("rocket_launch_product", item->stackSize, {}, true)(product)}};
    } else if (Tbl products = GetTbl(table, "rocket_launch_products")) {
      launchProducts.emplace();
      auto loader = LoadProduct(item->typeDotName(), item->stackSize, {}, true);
      for (const Tbl& p : ArrayTbls(products)) launchProducts->push_back(loader(p));
    } else if (!(factorioVersion_ < Version{2, 0, 0})) {
      launchProducts.emplace();
    }
  }
  if (launchProducts.has_value()) EnsureLaunchRecipe(item, &*launchProducts);

  item->weight = GetInt(table, "weight", item->weight);
  item->ingredientToWeightCoefficient =
      GetFloat(table, "ingredient_to_weight_coefficient", item->ingredientToWeightCoefficient);

  Item* spoiled = nullptr;
  if (GetRef<Item>(table, "spoil_result", spoiled)) {
    EnsureSpoilageEntityExists();
    Mechanics* recipe = CreateSpecialRecipe(item, sn::kSpoilRecipe, "spoiling");
    recipe->ingredients = {Ingredient(item, 1)};
    recipe->products = {Product(spoiled, 1)};
    specialWithProducts_.insert(recipe);
    recipe->time = GetLong(table, "spoil_ticks", 0) / 60.0f;
    // Our model stores spoilage directly (upstream reads the recipe lazily).
    item->spoilResult = spoiled;
    item->baseSpoilTime = recipe->time;
  } else if (Tbl spoil = GetTbl(table, "spoil_to_trigger_result")) {
    if (Tbl triggers = GetTbl(spoil, "trigger")) {
      ReadObjectOrArray(triggers, [&](const Tbl& trigger) {
        if (GetStr(trigger, "type") != "direct") return;
        if (Tbl delivery = GetTbl(trigger, "action_delivery")) {
          ReadObjectOrArray(delivery, [&](const Tbl& d) {
            if (GetStr(d, "type") != "instant") return;
            if (Tbl effects = GetTbl(d, "source_effects")) {
              ReadObjectOrArray(effects, [&](const Tbl& effect) {
                if (GetStr(effect, "type") == "create-entity") {
                  item->baseSpoilTime = GetLong(table, "spoil_ticks", 0) / 60.0f;
                  pendingItemSpoilEntities_.emplace_back(
                      item, GetStr(effect, "entity_name"));
                }
              });
            }
          });
        }
      });
    }
  }

  std::string plantResult;
  if (GetStrOpt(table, "plant_result", plantResult) && !plantResult.empty()) {
    plantResults_[item] = plantResult;
  }
}

void DataDeserializer::DeserializeFluid(const Tbl& table) {
  Fluid* fluid = DeserializeCommon<Fluid>(table, "fluid");
  fluid->originalName = fluid->name;

  std::string fuelValue;
  if (GetStrOpt(table, "fuel_value", fuelValue)) {
    fluid->fuelValue = static_cast<float>(LuaContext::ParseEnergy(fuelValue));
    fuels_.Add(sn::kBurnableFluid, fluid);
  }
  fuels_.Add(std::string(sn::kSpecificFluid) + fluid->name, fluid);

  std::string heatCap;
  if (GetStrOpt(table, "heat_capacity", heatCap)) {
    fluid->heatCapacity = static_cast<float>(LuaContext::ParseEnergy(heatCap));
  }
  fluid->temperatureRange = TemperatureRange(GetInt(table, "default_temperature", 0),
                                             GetInt(table, "max_temperature", 0));
}

void DataDeserializer::DeserializeTile(const Tbl& table) {
  Tile* tile = DeserializeCommon<Tile>(table, "tile");
  std::string fluidName;
  if (GetStrOpt(table, "fluid", fluidName)) {
    Fluid* baseFluid = GetObject<Fluid>(fluidName);
    Fluid* pumpingFluid = GetFluidFixedTemp(fluidName, baseFluid->temperatureRange.min);
    tile->fluidResult = pumpingFluid;

    std::string recipeCategory = std::string(sn::kPumpingRecipe) + "tile";
    Mechanics* recipe = CreateSpecialRecipe(pumpingFluid, recipeCategory, "pumping");
    formerAliases_["Mechanics.pump." + pumpingFluid->name + "." + pumpingFluid->name] =
        recipe;
    if (!specialWithProducts_.count(recipe)) {
      recipe->products = {Product(pumpingFluid, 1200.0f)};  // Factorio default pump rate
      recipe->ingredients = {};
      recipe->time = 1.0f;
      specialWithProducts_.insert(recipe);
    }
    recipe->sourceTiles.push_back(tile);
  }
}

void DataDeserializer::DeserializeLocation(const Tbl& table) {
  Location* location = DeserializeCommon<Location>(table, "space-location");
  if (Tbl mapGen = GetTbl(table, "map_gen_settings")) {
    if (Tbl controls = GetTbl(mapGen, "autoplace_controls")) {
      location->placementControls = Keys(controls);
    }
    if (Tbl autoplace = GetTbl(mapGen, "autoplace_settings")) {
      if (Tbl entity = GetTbl(autoplace, "entity")) {
        if (Tbl settings = GetTbl(entity, "settings")) {
          location->entitySpawns = Keys(settings);
        }
      }
      if (Tbl tileTbl = GetTbl(autoplace, "tile")) {
        if (Tbl settings = GetTbl(tileTbl, "settings")) {
          for (const std::string& tileName : Keys(settings)) {
            for (FactorioObject* o : allObjects_) {
              auto* tile = dynamic_cast<Tile*>(o);
              if (tile != nullptr && tile->name == tileName) {
                tile->locations.push_back(location);
              }
            }
          }
        }
      }
    }
  }
  if (Tbl spawns = GetTbl(table, "asteroid_spawn_definitions")) {
    for (const Tbl& spawn : ArrayTbls(spawns)) {
      std::string asteroid;
      if (GetStrOpt(spawn, "asteroid", asteroid)) {
        location->entitySpawns.push_back(asteroid);
      }
    }
  }
}

void DataDeserializer::DeserializeQuality(const Tbl& table) {
  Quality* quality = DeserializeCommon<Quality>(table, "quality");
  std::string nextQuality;
  if (GetStrOpt(table, "next", nextQuality)) {
    quality->nextQuality = GetObject<Quality>(nextQuality);
    quality->nextQuality->previousQuality = quality;
  }
  quality->BeaconConsumptionFactor = GetFloat(table, "beacon_power_usage_multiplier", 1);
  quality->level = GetInt(table, "level", 0);
  quality->UpgradeChance = GetFloat(table, "next_probability", 0);
  // Factorio 2.1 split chaining out of next_probability: an upgrade that
  // reached this tier steps again with chain_probability. 2.0 data has no
  // such field — there, the reached tier's own next_probability chains.
  quality->ChainProbability =
      GetFloat(table, "chain_probability", quality->UpgradeChance);
}

void DataDeserializer::DeserializeAsteroidChunk(const Tbl& table) {
  Entity* chunk = DeserializeCommon<Entity>(table, "asteroid-chunk");
  Item* asteroid = GetObject<Item>(chunk->name);
  if (asteroid->showInExplorers) {  // no mining recipes for parameter chunks
    Mechanics* recipe = CreateSpecialRecipe(asteroid, sn::kAsteroidCapture, "mining");
    recipe->time = 1;
    recipe->ingredients = {};
    recipe->products = {Product(asteroid, 1)};
    specialWithProducts_.insert(recipe);
    recipe->sourceEntity = chunk;
  }
}

// ------------------------------------------------------------ main entry ----

LoadDataResult DataDeserializer::LoadData(
    LuaContext& lua, const Version& factorioVersion, bool netProduction,
    const std::function<void(const std::string&)>& progress) {
  auto report = [&](const std::string& s) {
    if (progress) progress(s);
  };

  DataDeserializer d(lua, factorioVersion);

  Tbl data = d.Wrap(lua.GetGlobal("data"));
  if (!data) throw std::runtime_error("no data global after the data stage");
  d.raw_ = d.GetTbl(data, "raw");
  if (!d.raw_) throw std::runtime_error("no data.raw after the data stage");
  Tbl defines = d.Wrap(lua.GetGlobal("defines"));
  Tbl prototypes = d.GetTbl(defines, "prototypes");
  if (!prototypes) throw std::runtime_error("no defines.prototypes");

  report("scanning prototypes");
  d.LoadPrototypes(d.raw_, prototypes);

  report("loading items");
  Tbl itemPrototypes = d.GetTbl(prototypes, "item");
  for (const std::string& prototypeName : d.Keys(itemPrototypes)) {
    d.DeserializePrototypes(d.raw_, prototypeName, &DataDeserializer::DeserializeItem);
  }
  report("loading fluids");
  d.DeserializePrototypes(d.raw_, "fluid", &DataDeserializer::DeserializeFluid);
  report("loading tiles");
  d.DeserializePrototypes(d.raw_, "tile", &DataDeserializer::DeserializeTile);
  report("loading recipes");
  d.DeserializePrototypes(d.raw_, "recipe", &DataDeserializer::DeserializeRecipe);
  report("loading locations");
  d.DeserializePrototypes(d.raw_, "planet", &DataDeserializer::DeserializeLocation);
  d.DeserializePrototypes(d.raw_, "space-location", &DataDeserializer::DeserializeLocation);
  try {
    d.rootAccessible_.push_back(d.GetObject<Location>("nauvis"));
  } catch (const std::exception&) {
    d.Error("no nauvis location (total conversion?); starting location unknown");
  }
  report("loading qualities");
  d.DeserializePrototypes(d.raw_, "quality", &DataDeserializer::DeserializeQuality);
  Quality* normalQuality = nullptr;
  try {
    normalQuality = d.GetObject<Quality>("normal");
  } catch (const std::exception&) {
    d.Error("no normal quality");
  }
  report("loading technologies");
  d.DeserializePrototypes(d.raw_, "technology", &DataDeserializer::DeserializeTechnology);
  if (normalQuality != nullptr) d.rootAccessible_.push_back(normalQuality);
  d.DeserializePrototypes(d.raw_, "asteroid-chunk",
                          &DataDeserializer::DeserializeAsteroidChunk);

  report("loading entities");
  Tbl entityPrototypes = d.GetTbl(prototypes, "entity");
  for (const std::string& prototypeName : d.Keys(entityPrototypes)) {
    d.DeserializePrototypes(d.raw_, prototypeName, &DataDeserializer::DeserializeEntity);
  }

  d.ParseCaptureEffects();
  d.ParseModYafcHandles(d.GetTbl(data, "script_enabled"));

  report("computing maps");
  // Deterministic order (upstream sorts allObjects by sortingOrder, typeDotName).
  std::stable_sort(d.allObjects_.begin(), d.allObjects_.end(),
                   [](const FactorioObject* a, const FactorioObject* b) {
                     if (a->sortingOrder() != b->sortingOrder()) {
                       return a->sortingOrder() < b->sortingOrder();
                     }
                     return a->typeDotName() < b->typeDotName();
                   });

  Tbl utilityConstants = d.GetTbl(d.raw_, "utility-constants");
  Tbl utilityDefault = d.GetTbl(utilityConstants, "default");
  d.rocketCapacity_ = d.GetInt(utilityDefault, "rocket_lift_weight", 1000000);
  d.defaultItemWeight_ = d.GetInt(utilityDefault, "default_item_weight", 100);

  d.UpdateSplitFluids();
  d.UpdateSplitHeats();
  d.UpdateRecipeIngredientFluids();
  d.CalculateMaps(netProduction);
  d.UpdateRecipeCatalysts();
  d.CalculateItemWeights();
  d.ResolvePendingReferences();

  report("building database");
  // Reorder owned_ to match allObjects_ (ownership follows the sorted order).
  std::unordered_map<const FactorioObject*, std::unique_ptr<FactorioObject>> byPtr;
  for (auto& o : d.owned_) byPtr[o.get()] = std::move(o);
  std::vector<std::unique_ptr<FactorioObject>> sortedOwned;
  sortedOwned.reserve(d.allObjects_.size());
  for (FactorioObject* o : d.allObjects_) sortedOwned.push_back(std::move(byPtr[o]));

  LoadDataResult result;
  result.db = std::make_unique<Database>();
  result.db->LoadBuiltData(std::move(sortedOwned), d.formerAliases_);
  result.db->rootAccessible = d.rootAccessible_;
  result.db->fluidVariants.clear();
  for (auto& [key, variants] : d.fluidVariants_) {
    result.db->fluidVariants[key] = variants;
  }
  if (d.heat_->variants != nullptr) result.db->heatVariants = *d.heat_->variants;
  result.db->constantCombinatorCapacity = d.constantCombinatorCapacity_;
  result.errors = std::move(d.errors_);
  result.costAnalysisExclusions = {d.science_};
  return result;
}

// -------------------------------------------------------- cross references ----

void DataDeserializer::ParseCaptureEffects() {
  std::unordered_set<std::string> captureRobots;
  for (FactorioObject* o : allObjects_) {
    if (o->factorioType == "capture-robot") captureRobots.insert(o->name);
  }
  std::unordered_set<std::string> captureProjectiles;
  for (FactorioObject* o : allObjects_) {
    if (auto* p = dynamic_cast<EntityProjectile*>(o)) {
      for (const std::string& placed : p->placeEntities) {
        if (captureRobots.count(placed)) {
          captureProjectiles.insert(p->name);
          break;
        }
      }
    }
  }
  std::vector<Ammo*> captureAmmo;
  for (FactorioObject* o : allObjects_) {
    if (auto* a = dynamic_cast<Ammo*>(o)) {
      for (const std::string& projectile : a->projectileNames) {
        if (captureProjectiles.count(projectile)) {
          captureAmmo.push_back(a);
          break;
        }
      }
    }
  }
  if (captureAmmo.empty()) return;
  std::unordered_map<std::string, Entity*> entities;
  for (FactorioObject* o : allObjects_) {
    if (auto* e = dynamic_cast<Entity*>(o)) entities[e->name] = e;
  }
  for (Ammo* ammo : captureAmmo) {
    for (FactorioObject* o : allObjects_) {
      auto* spawner = dynamic_cast<EntitySpawner*>(o);
      if (spawner == nullptr || spawner->capturedEntityName.empty()) continue;
      bool matches = !ammo->targetFilter.has_value() ||
                     std::find(ammo->targetFilter->begin(), ammo->targetFilter->end(),
                               spawner->name) != ammo->targetFilter->end();
      if (matches) {
        Entity* captured = entities[spawner->capturedEntityName];
        if (captured != nullptr) {
          // TODO(port): Entity.captureAmmo (dependency analysis for captured
          // spawners) once DependencyNode consumes it.
          (void)captured;
        }
      }
    }
  }
}

void DataDeserializer::ParseModYafcHandles(const Tbl& scriptEnabled) {
  if (!scriptEnabled) return;
  for (const Tbl& element : ArrayTbls(scriptEnabled)) {
    std::string type = GetStr(element, "type");
    std::string name = GetStr(element, "name");
    Kind kind;
    if (type == "item") kind = Kind::Item;
    else if (type == "fluid") kind = Kind::Fluid;
    else if (type == "technology") kind = Kind::Technology;
    else if (type == "recipe") kind = Kind::Recipe;
    else if (type == "entity") kind = Kind::Entity;
    else continue;
    auto it = registeredObjects_.find({kind, name});
    if (it != registeredObjects_.end()) rootAccessible_.push_back(it->second);
  }
}

bool DataDeserializer::CanFit(const RecipeOrTechnology* recipe, int itemInputs,
                              int fluidInputs, const std::vector<Goods*>& slots) {
  for (const Ingredient& ingredient : recipe->ingredients) {
    if (dynamic_cast<const Item*>(ingredient.goods) != nullptr && --itemInputs < 0) {
      return false;
    }
    if (dynamic_cast<const Fluid*>(ingredient.goods) != nullptr && --fluidInputs < 0) {
      return false;
    }
    if (!slots.empty() &&
        std::find(slots.begin(), slots.end(), ingredient.goods) == slots.end()) {
      return false;
    }
  }
  return true;
}

void DataDeserializer::CalculateMaps(bool netProduction) {
  DataBucket<Goods*, Recipe*> itemUsages;
  DataBucket<Goods*, Recipe*> itemProduction;
  DataBucket<Goods*, FactorioObject*> miscSources;
  DataBucket<Entity*, Item*> entityPlacers;
  DataBucket<Recipe*, Technology*> recipeUnlockers;
  DataBucket<Location*, Technology*> locationUnlockers;
  DataBucket<Entity*, Location*> entityLocations;
  DataBucket<std::string, Location*> autoplaceControlLocations;
  DataBucket<RecipeOrTechnology*, EntityCrafter*> actualRecipeCrafters;
  DataBucket<Goods*, Entity*> usageAsFuel;
  DataBucket<Item*, Item*> fuelResults;
  std::vector<Recipe*> allRecipes;
  std::vector<Mechanics*> allMechanics;

  // step 1 - collect maps (index loop: GetObject can append to allObjects_)
  for (size_t oi = 0; oi < allObjects_.size(); ++oi) {
    FactorioObject* o = allObjects_[oi];
    if (auto* technology = dynamic_cast<Technology*>(o)) {
      for (Recipe* recipe : technology->unlockRecipes) {
        recipeUnlockers.Add(recipe, technology);
      }
      for (Location* location : technology->unlockLocations) {
        locationUnlockers.Add(location, technology);
      }
    } else if (auto* recipe = dynamic_cast<Recipe*>(o)) {
      allRecipes.push_back(recipe);

      // products grouped by goods
      std::map<Goods*, float> productAmounts;
      for (const Product& product : recipe->products) {
        productAmounts[product.goods] += product.amount;
      }
      for (const auto& [goods, outputAmount] : productAmounts) {
        float inputAmount = 0;
        if (netProduction) {
          for (const Ingredient& i : recipe->ingredients) {
            if (i.goods == goods && i.variants.empty()) {
              inputAmount = i.amount;
              break;
            }
          }
        }
        if (outputAmount > inputAmount) itemProduction.Add(goods, recipe);
      }

      for (Ingredient& ingredient : recipe->ingredients) {
        float inputAmount = ingredient.amount;
        float outputAmount = 0;
        if (netProduction && ingredient.variants.empty()) {
          for (const Product& p : recipe->products) {
            if (p.goods == ingredient.goods) outputAmount += p.amount;
          }
        }
        if (ingredient.variants.empty() && inputAmount > outputAmount) {
          itemUsages.Add(ingredient.goods, recipe);
        } else if (!ingredient.variants.empty()) {
          ingredient.goods = ingredient.variants[0];
          for (Goods* variant : ingredient.variants) {
            itemUsages.Add(variant, recipe);
          }
        }
      }
      if (auto* mechanics = dynamic_cast<Mechanics*>(recipe)) {
        allMechanics.push_back(mechanics);
      }
    } else if (auto* item = dynamic_cast<Item*>(o)) {
      if (auto it = placeResults_.find(item); it != placeResults_.end()) {
        try {
          item->placeResult = GetObject<Entity>(it->second[0]);
          for (const std::string& entityName : it->second) {
            entityPlacers.Add(GetObject<Entity>(entityName), item);
          }
        } catch (const std::exception& e) {
          Error("place_result of " + item->name + ": " + e.what());
        }
      }
      if (auto it = plantResults_.find(item); it != plantResults_.end()) {
        try {
          item->plantResult = GetObject<Entity>(it->second);
          entityPlacers.Add(GetObject<Entity>(it->second), item, true);
        } catch (const std::exception& e) {
          Error("plant_result of " + item->name + ": " + e.what());
        }
      }
      if (item->fuelResult != nullptr) {
        fuelResults.Add(item->fuelResult, item);
        miscSources.Add(item->fuelResult, item);
      }
    } else if (auto* location = dynamic_cast<Location*>(o)) {
      for (const std::string& entityName : location->entitySpawns) {
        try {
          entityLocations.Add(GetObject<Entity>(entityName), location);
        } catch (const std::exception&) {
          // spawn definitions may reference non-entity prototypes
        }
      }
      for (const std::string& control : location->placementControls) {
        autoplaceControlLocations.Add(control, location);
      }
    }
  }
  // Entities in a separate pass: GetObject calls above may create entities.
  for (size_t oi = 0; oi < allObjects_.size(); ++oi) {
    auto* entity = dynamic_cast<Entity*>(allObjects_[oi]);
    if (entity == nullptr) continue;
    for (const Product& product : entity->loot) {
      miscSources.Add(product.goods, entity);
    }
    if (auto* crafter = dynamic_cast<EntityCrafter*>(entity)) {
      crafter->recipes.clear();
      for (const std::string& category : recipeCrafters_.GetRaw(crafter)) {
        for (RecipeOrTechnology* recipe : recipeCategories_.GetRaw(category)) {
          if (CanFit(recipe, crafter->itemInputs, crafter->fluidInputs, crafter->inputs)) {
            crafter->recipes.push_back(recipe);
          }
        }
      }
      for (RecipeOrTechnology* recipe : crafter->recipes) {
        actualRecipeCrafters.Add(recipe, crafter, true);
      }
    }
    if (entity->hasEnergy && entity->energy.type != EntityEnergyType::Void &&
        entity->energy.type != EntityEnergyType::Labor) {
      std::vector<Goods*> fuelList;
      for (const std::string& category : fuelUsers_.GetRaw(entity)) {
        for (Goods* fuel : fuels_.GetRaw(category)) fuelList.push_back(fuel);
      }
      if (entity->energy.type == EntityEnergyType::FluidHeat) {
        std::erase_if(fuelList, [&](Goods* g) {
          auto* f = dynamic_cast<Fluid*>(g);
          return f == nullptr ||
                 !entity->energy.acceptedTemperature.Contains(f->temperature) ||
                 f->temperature <= entity->energy.workingTemperature.min;
        });
      }
      if (entity->energy.type == EntityEnergyType::Heat && heat_->variants != nullptr) {
        std::erase_if(fuelList, [&](Goods* g) {
          auto* s = dynamic_cast<Special*>(g);
          return s == nullptr || s->temperature < entity->energy.workingTemperature.min;
        });
      }
      entity->energy.fuels = fuelList;
      for (Goods* fuel : fuelList) usageAsFuel.Add(fuel, entity);
    } else if (entity->hasEnergy) {
      // void/labor energy burns the void pseudo-fuel
      entity->energy.fuels = {voidEnergy_};
      usageAsFuel.Add(voidEnergy_, entity);
    }
  }

  // step 2 - fill maps
  for (FactorioObject* o : allObjects_) {
    if (auto* recipeOrTech = dynamic_cast<RecipeOrTechnology*>(o)) {
      if (auto* recipe = dynamic_cast<Recipe*>(recipeOrTech)) {
        recipe->technologyUnlock = recipeUnlockers.GetArray(recipe);
      }
      recipeOrTech->crafters.clear();
      for (EntityCrafter* crafter : actualRecipeCrafters.GetRaw(recipeOrTech)) {
        recipeOrTech->crafters.push_back(crafter);
      }
    } else if (auto* goods = dynamic_cast<Goods*>(o)) {
      goods->usages = itemUsages.GetArray(goods);
      goods->production.clear();
      for (Recipe* r : itemProduction.GetRaw(goods)) {
        if (std::find(goods->production.begin(), goods->production.end(), r) ==
            goods->production.end()) {
          goods->production.push_back(r);
        }
      }
      goods->miscSources = miscSources.GetArray(goods);
      if (auto* item = dynamic_cast<Item*>(goods)) {
        item->fuelResultOf = fuelResults.GetArray(item);
      }
      goods->fuelFor = usageAsFuel.GetArray(goods);
    } else if (auto* entity = dynamic_cast<Entity*>(o)) {
      entity->itemsToPlace = entityPlacers.GetArray(entity);
      entity->spawnLocations = entityLocations.GetArray(entity);
      if (!entity->autoplaceControl.empty()) {
        for (Location* loc : autoplaceControlLocations.GetRaw(entity->autoplaceControl)) {
          if (std::find(entity->spawnLocations.begin(), entity->spawnLocations.end(),
                        loc) == entity->spawnLocations.end()) {
            entity->spawnLocations.push_back(loc);
          }
        }
      }
      if (!entity->spawnLocations.empty()) entity->mapGenerated = true;
      entity->sourceEntities = asteroids_.GetArray(entity->name);
    }
    if (auto* location = dynamic_cast<Location*>(o)) {
      location->technologyUnlock = locationUnlockers.GetArray(location);
    }
  }

  // Fixed-recipe accessibility propagation (upstream crafters queue).
  std::deque<EntityCrafter*> crafterQueue;
  for (FactorioObject* o : allObjects_) {
    if (auto* c = dynamic_cast<EntityCrafter*>(o)) crafterQueue.push_back(c);
  }
  while (!crafterQueue.empty()) {
    EntityCrafter* crafter = crafterQueue.front();
    crafterQueue.pop_front();

    bool hasFixedCategory = false;
    for (const std::string& category : recipeCrafters_.GetRaw(crafter)) {
      if (category.rfind(sn::kFixedRecipe, 0) == 0) {
        hasFixedCategory = true;
        break;
      }
    }
    if (!hasFixedCategory) continue;
    Recipe* fixedRecipe = nullptr;
    int plainRecipes = 0;
    for (RecipeOrTechnology* r : crafter->recipes) {
      if (dynamic_cast<Mechanics*>(r) == nullptr &&
          dynamic_cast<Recipe*>(r) != nullptr && typeid(*r) == typeid(Recipe)) {
        plainRecipes++;
        fixedRecipe = static_cast<Recipe*>(r);
      }
    }
    if (plainRecipes != 1 || fixedRecipe == nullptr || fixedRecipe->enabled) continue;

    bool addedUnlocks = false;
    for (Item* placeItem : crafter->itemsToPlace) {
      for (Recipe* itemRecipe : placeItem->production) {
        if (itemRecipe->enabled) {
          fixedRecipe->enabled = true;
          addedUnlocks = true;
          break;
        }
        bool hasNew = false;
        for (Technology* t : itemRecipe->technologyUnlock) {
          if (std::find(fixedRecipe->technologyUnlock.begin(),
                        fixedRecipe->technologyUnlock.end(),
                        t) == fixedRecipe->technologyUnlock.end()) {
            fixedRecipe->technologyUnlock.push_back(t);
            hasNew = true;
          }
        }
        addedUnlocks |= hasNew;
      }
      if (fixedRecipe->enabled) break;
    }

    if (addedUnlocks) {
      std::vector<Item*> products;
      for (const Product& p : fixedRecipe->products) {
        if (auto* item = dynamic_cast<Item*>(p.goods)) products.push_back(item);
      }
      for (FactorioObject* o : allObjects_) {
        auto* newCrafter = dynamic_cast<EntityCrafter*>(o);
        if (newCrafter == nullptr) continue;
        for (Item* placed : newCrafter->itemsToPlace) {
          if (std::find(products.begin(), products.end(), placed) != products.end()) {
            crafterQueue.push_back(newCrafter);
            break;
          }
        }
      }
    }
  }

  // step 3a - voiding recipes
  for (Recipe* recipe : allRecipes) {
    if (recipe->products.empty()) {
      recipe->specialType = FactorioObjectSpecialType::Voiding;
    }
  }

  // step 3b - packing/unpacking detection
  auto countNonDsrRecipes = [](const std::vector<Recipe*>& recipes) {
    int count = 0;
    for (const Recipe* r : recipes) {
      if (r->name.find("StackedRecipe-") == std::string::npos &&
          r->name.find("DSR_HighPressure-") == std::string::npos &&
          r->specialType != FactorioObjectSpecialType::Recycling &&
          r->specialType != FactorioObjectSpecialType::Voiding) {
        count++;
      }
    }
    return count;
  };

  for (Recipe* recipe : allRecipes) {
    if (recipe->specialType != FactorioObjectSpecialType::Normal) continue;
    if (recipe->products.size() != 1 || recipe->ingredients.empty()) continue;

    Goods* packed = recipe->products[0].goods;
    if (countNonDsrRecipes(packed->usages) != 1 &&
        countNonDsrRecipes(packed->production) != 1) {
      continue;
    }
    float inSum = 0, outSum = 0;
    for (const Ingredient& i : recipe->ingredients) inSum += i.amount;
    for (const Product& p : recipe->products) outSum += p.amount;
    if (inSum <= outSum) continue;

    for (Recipe* unpacking : packed->usages) {
      if (!AreInverseRecipes(recipe, unpacking)) continue;

      bool allFluid = true, allItem = true, anyItem = false, anyFluid = false;
      for (const Product& p : unpacking->products) {
        bool isFluid = dynamic_cast<Fluid*>(p.goods) != nullptr;
        bool isItem = dynamic_cast<Item*>(p.goods) != nullptr;
        allFluid &= isFluid;
        allItem &= isItem;
        anyItem |= isItem;
        anyFluid |= isFluid;
      }
      FactorioObjectSpecialType type;
      if (dynamic_cast<Fluid*>(packed) != nullptr && allFluid) {
        type = FactorioObjectSpecialType::Pressurization;
      } else if (dynamic_cast<Item*>(packed) != nullptr && allItem) {
        type = unpacking->products.size() == 1 ? FactorioObjectSpecialType::Stacking
                                               : FactorioObjectSpecialType::Crating;
      } else if (dynamic_cast<Item*>(packed) != nullptr && anyItem && anyFluid) {
        type = FactorioObjectSpecialType::Barreling;
      } else {
        continue;
      }
      recipe->specialType = type;
      unpacking->specialType = type;
      packed->specialType = type;

      auto* packedItem = dynamic_cast<Item*>(packed);
      if (countNonDsrRecipes(packed->usages) != 1 ||
          (packedItem != nullptr &&
           (packedItem->fuelValue != 0 || packedItem->placeResult != nullptr ||
            dynamic_cast<Module*>(packedItem) != nullptr))) {
        recipe->specialType = FactorioObjectSpecialType::Normal;
        packed->specialType = FactorioObjectSpecialType::Normal;
      }
      bool minedSource = false;
      for (FactorioObject* src : packed->miscSources) {
        if (dynamic_cast<Entity*>(src) != nullptr) minedSource = true;
      }
      if (minedSource || countNonDsrRecipes(packed->production) > 1) {
        unpacking->specialType = FactorioObjectSpecialType::Normal;
        packed->specialType = FactorioObjectSpecialType::Normal;
      }
    }
  }
}

bool DataDeserializer::AreInverseRecipes(const Recipe* packing, const Recipe* unpacking) {
  const Product& packedProduct = packing->products[0];
  if (packedProduct.probability != 1) return false;
  for (const Product& p : unpacking->products) {
    if (p.probability != 1) return false;
  }
  if (packedProduct.amountMin != packedProduct.amountMax) return false;
  for (const Product& p : unpacking->products) {
    if (p.amountMin != p.amountMax) return false;
  }
  if (unpacking->ingredients.size() != 1 ||
      packing->ingredients.size() != unpacking->products.size()) {
    return false;
  }

  float ratio = 0;
  const Recipe* larger = nullptr;

  auto checkProportions = [&](const Recipe* currentLarger, float largerCount,
                              float smallerCount) {
    if (largerCount / smallerCount != std::floor(largerCount / smallerCount)) return false;
    if (ratio != 0 && ratio != largerCount / smallerCount) return false;
    if (larger != nullptr && larger != currentLarger) return false;
    ratio = largerCount / smallerCount;
    larger = currentLarger;
    return true;
  };

  auto checkRatios = [&](const Recipe* first, const Recipe* second) {
    std::map<Goods*, float> ingredients;
    for (const Ingredient& item : first->ingredients) {
      if (ingredients.count(item.goods)) return false;  // duplicate ingredients
      ingredients[item.goods] = item.amount;
    }
    for (const Product& item : second->products) {
      auto it = ingredients.find(item.goods);
      if (it == ingredients.end()) return false;
      float count = it->second;
      if (count > item.amount) {
        if (!checkProportions(first, count, item.amount)) return false;
      } else if (count == item.amount) {
        if (ratio != 0 && ratio != 1) return false;
        ratio = 1;
      } else {
        if (!checkProportions(second, item.amount, count)) return false;
      }
    }
    return true;
  };

  return checkRatios(packing, unpacking) && checkRatios(unpacking, packing);
}

void DataDeserializer::CalculateItemWeights() {
  if (factorioVersion_ < Version{2, 0, 0}) return;

  std::map<Item*, std::vector<Item*>> dependencies;
  std::map<std::string, Recipe*> recipesByName;
  for (FactorioObject* o : allObjects_) {
    auto* recipe = dynamic_cast<Recipe*>(o);
    if (recipe == nullptr) continue;
    if (typeid(*recipe) == typeid(Recipe)) recipesByName[recipe->name] = recipe;
    for (const Ingredient& i : recipe->ingredients) {
      auto* ingredient = dynamic_cast<Item*>(i.goods);
      if (ingredient == nullptr) continue;
      for (const Product& p : recipe->products) {
        if (auto* product = dynamic_cast<Item*>(p.goods)) {
          dependencies[ingredient].push_back(product);
        }
      }
    }
  }

  std::deque<Item*> queue;
  for (FactorioObject* o : allObjects_) {
    if (auto* item = dynamic_cast<Item*>(o)) queue.push_back(item);
  }
  while (!queue.empty()) {
    Item* item = queue.front();
    queue.pop_front();
    if (item->weight != 0 || item->stackSize == 0) continue;

    auto rit = recipesByName.find(item->name);
    Recipe* recipe = rit == recipesByName.end() ? nullptr : rit->second;
    if (recipe == nullptr || recipe->products.empty() || recipe->ingredients.empty() ||
        recipe->hidden) {
      continue;
    }

    float weight = 0;
    bool ready = true;
    for (const Ingredient& ingredient : recipe->ingredients) {
      if (auto* i = dynamic_cast<Item*>(ingredient.goods)) {
        if (i->weight == 0) {
          ready = false;  // wait for ingredient weights
          break;
        }
        weight += i->weight * ingredient.amount;
      } else {
        weight += defaultItemWeight_ * ingredient.amount;
      }
    }
    if (!ready) continue;

    item->weight = static_cast<int>(weight / recipe->products[0].amount *
                                    item->ingredientToWeightCoefficient);
    if (item->weight == 0) continue;

    if (!(recipe->allowedEffects & AllowedEffects::kProductivity)) {
      item->weight = std::max(item->weight, rocketCapacity_ / item->stackSize);
    } else if (item->weight * item->stackSize < rocketCapacity_) {
      item->weight =
          rocketCapacity_ / (rocketCapacity_ / item->weight / item->stackSize) /
          item->stackSize;
    }

    if (auto dit = dependencies.find(item); dit != dependencies.end()) {
      for (Item* product : dit->second) {
        if (product->weight == 0) queue.push_back(product);
      }
    }
  }

  int maxStacks = 0;
  bool haveSilo = false;
  for (FactorioObject* o : allObjects_) {
    auto* crafter = dynamic_cast<EntityCrafter*>(o);
    if (crafter != nullptr && crafter->factorioType == "rocket-silo") {
      haveSilo = true;
      maxStacks = std::max(maxStacks, crafter->rocketInventorySize);
    }
  }
  if (!haveSilo) maxStacks = 1;

  for (FactorioObject* o : allObjects_) {
    auto* item = dynamic_cast<Item*>(o);
    if (item == nullptr) continue;
    if (item->weight == 0) {
      if (defaultItemWeight_ == 0) {
        item->rocketCapacity = 0;
        continue;
      }
      item->weight = defaultItemWeight_;
    }
    int capacity = rocketCapacity_ / item->weight;
    if (maxStacks != std::numeric_limits<int>::max() ||
        factorioVersion_ < Version{2, 1, 0}) {
      capacity = std::min(capacity, maxStacks * item->stackSize);
    }
    item->rocketCapacity = capacity;

    auto it = registeredObjects_.find(
        {Kind::Mechanics, std::string(sn::kRocketLaunch) + "." + item->name});
    if (it != registeredObjects_.end()) {
      auto* recipe = static_cast<Mechanics*>(it->second);
      if (!recipe->ingredients.empty()) {
        recipe->ingredients[0] = Ingredient(item, static_cast<float>(capacity));
      }
      for (Product& p : recipe->products) {
        p = Product(p.goods, p.amountMin * capacity, p.amountMax * capacity,
                    p.probability);
      }
    }
  }
}

void DataDeserializer::ResolvePendingReferences() {
  for (auto& [item, entityName] : pendingItemSpoilEntities_) {
    auto it = registeredObjects_.find({Kind::Entity, entityName});
    if (it != registeredObjects_.end()) item->spoilResult = it->second;
  }
  for (auto& [entity, entityName] : pendingEntitySpoil_) {
    auto it = registeredObjects_.find({Kind::Entity, entityName});
    if (it != registeredObjects_.end()) {
      entity->spoilResult = dynamic_cast<Entity*>(it->second);
    }
  }
  for (PendingTriggerEntities& pending : pendingTriggerEntities_) {
    if (pending.allSpawners) {
      for (FactorioObject* o : allObjects_) {
        auto* spawner = dynamic_cast<EntitySpawner*>(o);
        if (spawner != nullptr && !spawner->capturedEntityName.empty()) {
          pending.tech->triggerEntities.push_back(spawner);
        }
      }
    } else {
      for (const std::string& name : pending.names) {
        auto it = registeredObjects_.find({Kind::Entity, name});
        if (it != registeredObjects_.end()) {
          if (auto* e = dynamic_cast<Entity*>(it->second)) {
            pending.tech->triggerEntities.push_back(e);
          }
        }
      }
    }
  }
}

}  // namespace yafc
