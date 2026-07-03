// Port of Yafc.Model/Data/DataClasses.cs — the FactorioObject hierarchy.
// Field and type names intentionally match upstream C# (camelCase) so ports of
// analyses/parser code stay mechanical. Trimmed to the members the solver,
// analyses and parser need; UI-only conveniences and the dependency-graph
// methods (GetDependencies) arrive with the milestone analysis port.
//
// Ownership: all objects are owned by Database (unique_ptr arena); every
// cross-reference is a raw non-owning pointer, mirroring the C# object graph.
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yafc {

class Entity;
class EntityCrafter;
class Fluid;
class Goods;
class Item;
class Location;
class Mechanics;
class Module;
class Quality;
class Recipe;
class Technology;
class Tile;

using FactorioId = int32_t;
inline constexpr FactorioId kInvalidFactorioId = -1;

// Upstream: FactorioObjectSortOrder — defines the id-range layout in Database.
enum class FactorioObjectSortOrder {
  SpecialGoods, Items, Fluids, Recipes, Mechanics, Technologies,
  Entities, Tiles, Qualities, Locations, Triggers,
};

enum class FactorioObjectSpecialType {
  Normal, Voiding, Barreling, Stacking, Pressurization, Crating, Recycling,
};

// Upstream RecipeFlags ([Flags] enum).
struct RecipeFlags {
  static constexpr uint32_t kUsesMiningProductivity = 1u << 0;
  static constexpr uint32_t kUsesFluidTemperature = 1u << 2;
  static constexpr uint32_t kScaleProductionWithPower = 1u << 3;
  static constexpr uint32_t kHasResearchTriggerCraft = 1u << 4;
  static constexpr uint32_t kHasResearchTriggerCaptureEntity = 1u << 5;
  static constexpr uint32_t kHasResearchTriggerMineEntity = 1u << 6;
  static constexpr uint32_t kHasResearchTriggerBuildEntity = 1u << 7;
  static constexpr uint32_t kHasResearchTriggerCreateSpacePlatform = 1u << 8;
  static constexpr uint32_t kHasResearchTriggerSendToOrbit = 1u << 9;
  static constexpr uint32_t kHasResearchTriggerScripted = 1u << 10;
  static constexpr uint32_t kHasResearchTriggerMask =
      kHasResearchTriggerCraft | kHasResearchTriggerCaptureEntity |
      kHasResearchTriggerMineEntity | kHasResearchTriggerBuildEntity |
      kHasResearchTriggerCreateSpacePlatform | kHasResearchTriggerSendToOrbit |
      kHasResearchTriggerScripted;
};

enum class UnitOfMeasure {
  None, Percent, Second, PerSecond, ItemPerSecond, FluidPerSecond,
  Megawatt, Megajoule, Celsius,
};

struct TemperatureRange {
  int min = 0;
  int max = 0;

  TemperatureRange() = default;
  TemperatureRange(int min, int max) : min(min), max(max) {}
  explicit TemperatureRange(int single) : min(single), max(single) {}
  static TemperatureRange Any() {
    return {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
  }
  bool IsAny() const {
    return min == std::numeric_limits<int>::min() &&
           max == std::numeric_limits<int>::max();
  }
  bool IsSingle() const { return min == max; }
  bool Contains(int value) const { return min <= value && value <= max; }
  bool operator==(const TemperatureRange&) const = default;
};

// Upstream FactorioIconPart: one layer of an object's icon. Pure data here;
// PNG decode/compositing happens in the web layer.
struct FactorioIconPart {
  std::string path;
  int size = 32;
  float x = 0, y = 0, r = 1, g = 1, b = 1, a = 1;
  float scale = 1;

  bool IsSimple() const {
    return x == 0 && y == 0 && r == 1 && g == 1 && b == 1 && a == 1 && scale == 1;
  }
};

class FactorioObject {
 public:
  virtual ~FactorioObject() = default;

  std::string factorioType;
  std::string name;
  std::string locName;   // falls back to name at load end (upstream CalculateMaps)
  std::string locDescr;
  int iconId = 0;
  std::vector<FactorioIconPart> iconSpec;
  FactorioId id = kInvalidFactorioId;
  FactorioObjectSpecialType specialType = FactorioObjectSpecialType::Normal;
  bool showInExplorers = true;

  virtual std::string_view type() const = 0;
  virtual FactorioObjectSortOrder sortingOrder() const = 0;
  std::string typeDotName() const { return std::string(type()) + '.' + name; }
};

// ---------------------------------------------------------------- goods ----

class Goods : public FactorioObject {
 public:
  float fuelValue = 0;
  bool isLinkable = true;
  std::vector<Recipe*> production;
  std::vector<Recipe*> usages;
  std::vector<FactorioObject*> miscSources;
  std::vector<Entity*> fuelFor;

  virtual bool isPower() const { return false; }
  virtual UnitOfMeasure flowUnitOfMeasure() const = 0;
  Fluid* fluid();  // this as Fluid, or nullptr
  virtual Item* HasSpentFuel() { return nullptr; }  // returns spent item or null
};

class Item : public Goods {
 public:
  Item* fuelResult = nullptr;
  std::vector<Item*> fuelResultOf;
  int stackSize = 0;
  Entity* placeResult = nullptr;
  Entity* plantResult = nullptr;
  int rocketCapacity = 0;
  int weight = 0;
  float ingredientToWeightCoefficient = 0.5f;
  // Upstream lazily resolves these from the "spoil.<name>" Mechanics recipe;
  // the loader fills them directly here.
  FactorioObject* spoilResult = nullptr;
  float baseSpoilTime = 0;

  std::string_view type() const override { return "Item"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Items;
  }
  UnitOfMeasure flowUnitOfMeasure() const override {
    return UnitOfMeasure::ItemPerSecond;
  }
  Item* HasSpentFuel() override { return fuelResult; }
  float GetSpoilTime(const Quality& quality) const;
};

struct ModuleSpecification {
  std::string category;
  float baseConsumption = 0;
  float baseSpeed = 0;
  float baseProductivity = 0;
  float basePollution = 0;
  float baseQuality = 0;
};

class Module : public Item {
 public:
  ModuleSpecification moduleSpecification;
};

class Ammo : public Item {
 public:
  std::vector<std::string> projectileNames;
  std::optional<std::vector<std::string>> targetFilter;
};

class Fluid : public Goods {
 public:
  std::string originalName;  // name without temperature
  float heatCapacity = 1e-3f;
  TemperatureRange temperatureRange;
  int temperature = 0;
  float heatValue = 0;
  // Shared across temperature clones (upstream: one List referenced by all).
  std::shared_ptr<std::vector<Fluid*>> variants;

  std::string_view type() const override { return "Fluid"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Fluids;
  }
  UnitOfMeasure flowUnitOfMeasure() const override {
    return UnitOfMeasure::FluidPerSecond;
  }
  void SetTemperature(int temp) {
    temperature = temp;
    heatValue = (temp - temperatureRange.min) * heatCapacity;
  }
};

class Special : public Goods {
 public:
  std::string virtualSignal;
  bool power = false;
  bool isVoid = false;
  int temperature = 0;
  std::shared_ptr<std::vector<Special*>> variants;

  bool isPower() const override { return power; }
  std::string_view type() const override { return power ? "Power" : "Special"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::SpecialGoods;
  }
  UnitOfMeasure flowUnitOfMeasure() const override {
    return isVoid       ? UnitOfMeasure::None
           : power      ? UnitOfMeasure::Megawatt
                        : UnitOfMeasure::PerSecond;
  }
};

// ----------------------------------------------------------- ingredients ----

class Ingredient {
 public:
  float amount = 0;
  Goods* goods = nullptr;
  std::vector<Goods*> variants;  // empty = no variants
  TemperatureRange temperature = TemperatureRange::Any();

  Ingredient() = default;
  Ingredient(Goods* goods, float amount);

  bool ContainsVariant(const Goods* product) const {
    if (goods == product) return true;
    for (const Goods* v : variants) {
      if (v == product) return true;
    }
    return false;
  }
};

class Product {
 public:
  Goods* goods = nullptr;
  float amountMin = 0;
  float amountMax = 0;
  float probability = 1;
  float amount = 0;  // average amount including probability and range
  std::optional<float> percentSpoiled;

  Product() = default;
  Product(Goods* goods, float amount)
      : goods(goods), amountMin(amount), amountMax(amount), amount(amount),
        productivityAmount_(amount) {}
  Product(Goods* goods, float min, float max, float probability)
      : goods(goods), amountMin(min), amountMax(max), probability(probability),
        amount(probability * (min + max) / 2),
        productivityAmount_(probability * (min + max) / 2) {}

  // Upstream SetCatalyst: the catalyst part of the output does not scale with
  // productivity bonuses.
  void SetCatalyst(float catalyst);
  float productivityAmount() const { return productivityAmount_; }
  float GetAmountPerRecipe(float productivityBonus) const {
    return amount + productivityBonus * productivityAmount_;
  }
  bool IsSimple() const;

 private:
  friend struct DatabaseDumpAccess;  // bundle (de)serialization
  float productivityAmount_ = 0;
};

// -------------------------------------------------------------- recipes ----

class RecipeOrTechnology : public FactorioObject {
 public:
  std::vector<EntityCrafter*> crafters;
  std::vector<Ingredient> ingredients;
  std::vector<Product> products;
  Entity* sourceEntity = nullptr;
  std::vector<Tile*> sourceTiles;
  Goods* mainProduct = nullptr;
  float time = 0;
  bool enabled = false;
  uint32_t flags = 0;  // RecipeFlags

  std::string_view type() const override { return "Recipe"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Recipes;
  }
};

// Upstream AllowedEffects ([Flags]); Speed|Productivity|Consumption|Pollution|Quality.
struct AllowedEffects {
  static constexpr uint32_t kSpeed = 1u << 0;
  static constexpr uint32_t kProductivity = 1u << 1;
  static constexpr uint32_t kConsumption = 1u << 2;
  static constexpr uint32_t kPollution = 1u << 3;
  static constexpr uint32_t kQuality = 1u << 4;
  static constexpr uint32_t kAll =
      kSpeed | kProductivity | kConsumption | kPollution | kQuality;
  static constexpr uint32_t kNone = 0;
};

class Recipe : public RecipeOrTechnology {
 public:
  uint32_t allowedEffects = AllowedEffects::kNone;
  std::vector<std::string> allowedModuleCategories;  // empty = no restriction
  std::vector<Technology*> technologyUnlock;
  std::map<Technology*, float> technologyProductivity;
  bool preserveProducts = false;
  bool hidden = false;
  std::optional<float> maximumProductivity;

  bool HasIngredientVariants() const {
    for (const Ingredient& i : ingredients) {
      if (!i.variants.empty()) return true;
    }
    return false;
  }
};

class Mechanics : public Recipe {
 public:
  FactorioObject* source = nullptr;
  std::string localizationKey;

  std::string_view type() const override { return "Mechanics"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Mechanics;
  }
};

class Technology : public RecipeOrTechnology {
 public:
  float count = 0;
  std::vector<Technology*> prerequisites;
  std::vector<Recipe*> unlockRecipes;
  std::vector<Location*> unlockLocations;
  std::map<Recipe*, float> changeRecipeProductivity;
  bool unlocksFluidMining = false;
  FactorioObject* triggerObject = nullptr;
  std::vector<Entity*> triggerEntities;
  Quality* triggerMinimumQuality = nullptr;

  std::string_view type() const override { return "Technology"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Technologies;
  }
};

// ------------------------------------------------------------- entities ----

enum class EntityEnergyType {
  Void, Electric, Heat, SolidFuel, FluidFuel, FluidHeat, Labor,
};

class EntityEnergy {
 public:
  EntityEnergyType type = EntityEnergyType::Void;
  TemperatureRange workingTemperature;
  TemperatureRange acceptedTemperature = TemperatureRange::Any();
  std::vector<std::pair<std::string, float>> emissions;
  float drain = 0;
  float baseFuelConsumptionLimit = std::numeric_limits<float>::infinity();
  float effectivity = 1;
  std::vector<Goods*> fuels;
};

class Entity : public FactorioObject {
 public:
  std::vector<Product> loot;
  bool mapGenerated = false;
  float mapGenDensity = 0;
  float basePower = 0;
  // Upstream: energy is a nullable reference; entities without an energy
  // source (ore patches, etc.) have hasEnergy == false and require no fuel.
  bool hasEnergy = false;
  EntityEnergy energy;
  std::vector<Entity*> sourceEntities;  // asteroid death sources
  std::vector<Item*> itemsToPlace;
  std::vector<Location*> spawnLocations;
  float heatingPower = 0;
  bool hasBurntInventory = false;
  int width = 0;
  int height = 0;
  int size = 0;
  Entity* spoilResult = nullptr;
  float baseSpoilTime = 0;
  std::string autoplaceControl;

  std::string_view type() const override { return "Entity"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Entities;
  }
  float Power(const Quality& quality) const;
};

class EntityWithModules : public Entity {
 public:
  uint32_t allowedEffects = AllowedEffects::kNone;
  std::vector<std::string> allowedModuleCategories;
  int moduleSlots = 0;

  static bool CanAcceptModule(const ModuleSpecification& module,
                              uint32_t effects,
                              const std::vector<std::string>* categories);
  bool CanAcceptModule(const ModuleSpecification& module) const {
    return CanAcceptModule(module, allowedEffects,
                           allowedModuleCategories.empty()
                               ? nullptr
                               : &allowedModuleCategories);
  }
};

struct Effect {
  float consumption = 0;
  float speed = 0;
  float productivity = 0;
  float pollution = 0;
  float quality = 0;
};

struct EffectReceiver {
  Effect baseEffect;
  bool usesModuleEffects = true;
  bool usesBeaconEffects = true;
  bool usesSurfaceEffects = true;
};

class EntityCrafter : public EntityWithModules {
 public:
  int itemInputs = 0;
  int fluidInputs = 0;
  std::vector<Goods*> inputs;  // lab inputs (science packs); empty otherwise
  std::vector<RecipeOrTechnology*> recipes;
  float baseCraftingSpeed = 1;
  float effectReceiverBaseProductivity = 0;
  EffectReceiver effectReceiver;
  int rocketInventorySize = 0;
  bool hasVectorToPlaceResult = false;

  float CraftingSpeed(const Quality& quality) const;
};

class EntityBelt : public Entity {
 public:
  float beltItemsPerSecond = 0;
};

class EntityBeacon : public EntityWithModules {
 public:
  float beaconEfficiency = 0;
  float profile(int numberOfBeacons) const;
  std::vector<float> profileValues;
};

class EntityContainer : public Entity {
 public:
  int inventorySize = 0;
  std::string logisticMode;
  int logisticSlotsCount = 0;
};

class EntityInserter : public Entity {
 public:
  float inserterSwingTime = 0;
  bool isBulkInserter = false;
};

class EntityAccumulator : public Entity {
 public:
  float baseAccumulatorCapacity = 0;
};

class EntityReactor : public EntityCrafter {
 public:
  float reactorNeighborBonus = 1;
};

class EntityProjectile : public Entity {
 public:
  std::vector<std::string> placeEntities;
};

class EntitySpawner : public Entity {
 public:
  std::string capturedEntityName;  // empty = none
};

class EntityAttractor : public EntityCrafter {
 public:
  float range = 0;
  float attractorEfficiency = 0;
  float drain = 0;
};

class Tile : public FactorioObject {
 public:
  Fluid* fluidResult = nullptr;
  std::vector<Location*> locations;

  std::string_view type() const override { return "Tile"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Tiles;
  }
};

class Location : public FactorioObject {
 public:
  std::vector<Technology*> technologyUnlock;
  std::vector<std::string> entitySpawns;
  std::vector<std::string> placementControls;

  std::string_view type() const override { return "Location"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Locations;
  }
};

class ResearchTrigger : public FactorioObject {
 public:
  ResearchTrigger() { showInExplorers = false; }
  std::string_view type() const override { return "Research trigger"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Triggers;
  }
};

// -------------------------------------------------------------- quality ----

class Quality : public FactorioObject {
 public:
  Quality* nextQuality = nullptr;
  Quality* previousQuality = nullptr;
  int level = 0;
  std::vector<Technology*> technologyUnlock;
  float BeaconConsumptionFactor = 0;
  float UpgradeChance = 0;

  std::string_view type() const override { return "Quality"; }
  FactorioObjectSortOrder sortingOrder() const override {
    return FactorioObjectSortOrder::Qualities;
  }

  float StandardBonus() const { return 0.3f * level; }
  float AccumulatorCapacityBonus() const { return static_cast<float>(level); }
  float BeaconTransmissionBonus() const { return 0.2f * level; }
  float ApplyStandardBonus(float baseValue) const {
    return baseValue * (1 + 0.3f * level);
  }
  float ApplyAccumulatorCapacityBonus(float baseValue) const {
    return baseValue * (1 + level);
  }
  // Floored to the nearest hundredth: a 25% normal bonus is 32%, not 32.5%.
  float ApplyModuleBonus(float baseValue) const;
  float ApplyBeaconBonus(float baseValue) const { return baseValue + level * 0.2f; }
};

// Upstream IObjectWithQuality<T>: canonical (object, quality) pairs compared by
// reference. A value pair with member equality has the same semantics.
template <typename T>
struct ObjectWithQuality {
  T* target = nullptr;
  Quality* quality = nullptr;

  bool operator==(const ObjectWithQuality&) const = default;
  explicit operator bool() const { return target != nullptr; }
};

template <typename T>
ObjectWithQuality<T> With(T* target, Quality* quality) {
  return {target, quality};
}

}  // namespace yafc
