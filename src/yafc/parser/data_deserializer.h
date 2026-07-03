// Port of Yafc.Parser/Data/FactorioDataDeserializer*.cs — turns the Lua data
// stage's data.raw into a populated Database: items/fluids (with temperature
// variants), recipes/technologies, entities (crafters, labs, drills, boilers,
// generators, reactors, ...), special objects (electricity/heat/void/launch
// slots), synthesized Mechanics recipes (mining, pumping, boiling, launching,
// spoiling, generators), cross-reference maps and rocket weight math.
//
// Icon *specs* (paths + layer transforms) are fully captured for the web icon
// pipeline; icon rendering, sounds, locale lookups (locName falls back to
// name) and the C# cache are skipped. Deviations are marked TODO(port).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <typeinfo>
#include <vector>

#include "yafc/model/database.h"
#include "yafc/parser/factorio_data_source.h"
#include "yafc/parser/lua_context.h"

namespace yafc {

struct LoadDataResult {
  std::unique_ptr<Database> db;
  std::vector<std::string> errors;  // recoverable per-object problems
  // Upstream: Analysis.ExcludeFromAnalysis<CostAnalysis>(science).
  std::vector<FactorioObject*> costAnalysisExclusions;
};

class DataDeserializer {
 public:
  // lua must have completed the data stage (data.raw available).
  static LoadDataResult LoadData(LuaContext& lua, const Version& factorioVersion,
                                 bool netProduction = false,
                                 const std::function<void(const std::string&)>& progress = {});

 private:
  // RAII registry ref for a Lua table (shared; unrefs when the last copy dies).
  struct Tbl {
    LuaContext* L = nullptr;
    std::shared_ptr<int> ref;
    explicit operator bool() const { return ref != nullptr; }
  };

  enum class Kind { Item, Entity, Fluid, Recipe, Mechanics, Technology, Quality,
                    Location, Tile, Special, Trigger };

  DataDeserializer(LuaContext& lua, const Version& factorioVersion);

  // ---- lua table helpers (DataParserUtils semantics) ----
  Tbl Wrap(LuaValue&& value) const;  // takes ownership of a table ref
  Tbl GetTbl(const Tbl& t, const char* key) const;
  Tbl GetTblIdx(const Tbl& t, int index) const;
  bool Has(const Tbl& t, const char* key) const;
  std::string GetStr(const Tbl& t, const char* key, const std::string& def = "") const;
  bool GetStrOpt(const Tbl& t, const char* key, std::string& out) const;
  double GetNum(const Tbl& t, const char* key, double def = 0) const;
  float GetFloat(const Tbl& t, const char* key, float def = 0) const;
  int GetInt(const Tbl& t, const char* key, int def = 0) const;
  long long GetLong(const Tbl& t, const char* key, long long def = 0) const;
  bool GetBool(const Tbl& t, const char* key, bool def = false) const;
  double GetNumIdx(const Tbl& t, int index, double def = 0) const;
  std::string GetStrIdx(const Tbl& t, int index, const std::string& def = "") const;
  std::vector<Tbl> ArrayTbls(const Tbl& t) const;
  std::vector<std::string> ArrayStrs(const Tbl& t) const;
  std::vector<double> ArrayNums(const Tbl& t) const;
  std::vector<std::string> Keys(const Tbl& t) const;
  int ArrayLen(const Tbl& t) const;
  void ReadObjectOrArray(const Tbl& t, const std::function<void(const Tbl&)>& fn) const;

  // ---- object registry (upstream GetObject/GetRef) ----
  template <typename T>
  static Kind KindOf();
  FactorioObject* CreateConcrete(Kind kind, const std::string& typeInRaw,
                                 const std::string& name);
  FactorioObject* GetObjectRaw(Kind kind, const std::string& name,
                               const char* requestedType);
  template <typename T>
  T* GetObject(const std::string& name);
  template <typename T>
  T* GetObject(const Tbl& table);
  template <typename T>
  bool GetRef(const Tbl& table, const char* key, T*& result);

  Fluid* GetFluidFixedTemp(const std::string& key, int temperature);
  Fluid* SplitFluid(Fluid* basic, int temperature);
  void UpdateSplitFluids();
  Special* GetHeatFixedTemp(int temperature);
  Special* SplitHeat(Special* basic, int temperature);
  void UpdateSplitHeats();

  Special* CreateSpecialObject(bool isPower, const std::string& name,
                               const std::string& signal);
  Item* CreateSpecialItem(const std::string& name);
  Mechanics* CreateSpecialRecipe(FactorioObject* production, const std::string& category,
                                 const std::string& localizationKey);
  void EnsureSpoilageEntityExists();
  void EnsureLaunchRecipe(Item* item, const std::vector<Product>* launchProducts);
  Recipe* CreateLaunchRecipe(EntityCrafter* entity, Recipe* recipe, int partsRequired);

  // ---- main flow ----
  void LoadPrototypes(const Tbl& raw, const Tbl& prototypes);
  void DeserializePrototypes(const Tbl& raw, const std::string& type,
                             void (DataDeserializer::*fn)(const Tbl&));
  void DeserializeItem(const Tbl& table);
  void DeserializeFluid(const Tbl& table);
  void DeserializeTile(const Tbl& table);
  void DeserializeRecipe(const Tbl& table);
  void DeserializeLocation(const Tbl& table);
  void DeserializeQuality(const Tbl& table);
  void DeserializeTechnology(const Tbl& table);
  void DeserializeAsteroidChunk(const Tbl& table);
  void DeserializeEntity(const Tbl& table);
  template <typename T>
  T* DeserializeCommon(const Tbl& table, const char* prototypeType);

  // recipes/technologies
  std::function<Product(const Tbl&)> LoadProduct(const std::string& recipeName,
                                                 int multiplier = 1,
                                                 std::optional<float> percentSpoiled = {},
                                                 bool isAlwaysItem = false);
  std::vector<Product> LoadProductList(const Tbl& table, const std::string& typeDotName,
                                       bool allowSimpleSyntax);
  std::vector<Ingredient> LoadIngredientList(const Tbl& table,
                                             const std::string& typeDotName);
  std::vector<Ingredient> LoadResearchIngredientList(const Tbl& unit);
  void LoadRecipeData(Recipe* recipe, const Tbl& table);
  void LoadTechnologyData(Technology* technology, const Tbl& table);
  void LoadResearchTrigger(const Tbl& trigger, Technology* technology);
  Goods* LoadItemOrFluid(const Tbl& table, bool useTemperature, bool isAlwaysItem);
  void UpdateRecipeIngredientFluids();
  void UpdateRecipeCatalysts();

  // entities
  bool GetFluidBoxFilter(const Tbl& table, const char* fluidBoxName, int temperature,
                         Fluid** fluid, TemperatureRange* range);
  void ReadFluidEnergySource(const Tbl& energySource, Entity* entity);
  void ReadEnergySource(const Tbl& energySource, Entity* entity, float defaultDrain = 0);
  void ParseModules(const Tbl& table, EntityWithModules* entity, uint32_t def);
  float EstimateArgument(const Tbl& args, const char* name, float def = 0);
  float EstimateArgumentIdx(const Tbl& args, int index, float def = 0);
  float EstimateNoiseExpression(const Tbl& expression);

  // cross-references + export
  void ParseCaptureEffects();
  void ParseModYafcHandles(const Tbl& scriptEnabled);
  void CalculateMaps(bool netProduction);
  void CalculateItemWeights();
  void ResolvePendingReferences();
  static bool AreInverseRecipes(const Recipe* packing, const Recipe* unpacking);
  static bool CanFit(const RecipeOrTechnology* recipe, int itemInputs, int fluidInputs,
                     const std::vector<Goods*>& slots);

  void Error(std::string message) { errors_.push_back(std::move(message)); }

  // upstream DataBucket
  template <typename K, typename V>
  class DataBucket {
   public:
    void Add(const K& key, const V& value, bool checkUnique = false) {
      auto& list = storage_[key];
      if (!checkUnique ||
          std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
      }
    }
    const std::vector<V>& GetRaw(const K& key) const {
      static const std::vector<V> kEmpty;
      auto it = storage_.find(key);
      return it == storage_.end() ? kEmpty : it->second;
    }
    std::vector<V> GetArray(const K& key) const { return GetRaw(key); }

   private:
    std::map<K, std::vector<V>> storage_;
  };

  LuaContext& lua_;
  Version factorioVersion_;
  Tbl raw_;
  std::vector<std::string> errors_;

  std::vector<std::unique_ptr<FactorioObject>> owned_;
  std::vector<FactorioObject*> allObjects_;
  std::vector<FactorioObject*> rootAccessible_;
  std::map<std::pair<Kind, std::string>, FactorioObject*> registeredObjects_;
  std::map<std::pair<std::string, std::string>, std::string> prototypes_;
  std::unordered_map<std::string, FactorioObject*> formerAliases_;

  DataBucket<std::string, Goods*> fuels_;
  DataBucket<Entity*, std::string> fuelUsers_;
  DataBucket<std::string, RecipeOrTechnology*> recipeCategories_;
  DataBucket<EntityCrafter*, std::string> recipeCrafters_;
  DataBucket<std::string, Entity*> asteroids_;
  std::map<Item*, std::vector<std::string>> placeResults_;
  std::map<Item*, std::string> plantResults_;
  std::unordered_map<std::string, std::vector<Fluid*>> fluidVariants_;  // typeDotName ->

  Special* voidEnergy_ = nullptr;
  Special* heat_ = nullptr;
  Special* rocketLaunch_ = nullptr;
  Special* electricity_ = nullptr;
  Item* science_ = nullptr;
  EntityCrafter* spoilageEntity_ = nullptr;
  int rocketCapacity_ = 1000000;
  int defaultItemWeight_ = 100;
  int constantCombinatorCapacity_ = 18;

  // Special recipes whose products have been assigned (upstream: products != null).
  std::unordered_set<const Recipe*> specialWithProducts_;

  // Deferred lookups (upstream uses Lazy<> evaluated after the load).
  std::vector<std::pair<Item*, std::string>> pendingItemSpoilEntities_;
  std::vector<std::pair<Entity*, std::string>> pendingEntitySpoil_;
  struct PendingTriggerEntities {
    Technology* tech;
    std::vector<std::string> names;  // empty + allSpawners: every capturing spawner
    bool allSpawners = false;
  };
  std::vector<PendingTriggerEntities> pendingTriggerEntities_;
};


// ------------------------- inline template definitions -------------------------

template <typename T>
DataDeserializer::Kind DataDeserializer::KindOf() {
  if constexpr (std::is_base_of_v<Item, T>) return Kind::Item;
  else if constexpr (std::is_base_of_v<Entity, T>) return Kind::Entity;
  else if constexpr (std::is_same_v<T, Mechanics>) return Kind::Mechanics;
  else if constexpr (std::is_base_of_v<Recipe, T>) return Kind::Recipe;
  else if constexpr (std::is_same_v<T, Technology>) return Kind::Technology;
  else if constexpr (std::is_same_v<T, Fluid>) return Kind::Fluid;
  else if constexpr (std::is_same_v<T, Quality>) return Kind::Quality;
  else if constexpr (std::is_same_v<T, Location>) return Kind::Location;
  else if constexpr (std::is_same_v<T, Tile>) return Kind::Tile;
  else if constexpr (std::is_same_v<T, Special>) return Kind::Special;
  else if constexpr (std::is_same_v<T, ResearchTrigger>) return Kind::Trigger;
  else static_assert(!sizeof(T), "unsupported registry type");
}

template <typename T>
T* DataDeserializer::GetObject(const std::string& name) {
  FactorioObject* obj = GetObjectRaw(KindOf<T>(), name, typeid(T).name());
  T* typed = dynamic_cast<T*>(obj);
  if (typed == nullptr) {
    throw std::runtime_error("'" + name + "' cannot be loaded as the requested type");
  }
  return typed;
}

template <typename T>
T* DataDeserializer::GetObject(const Tbl& table) {
  std::string name;
  if (!GetStrOpt(table, "name", name)) {
    throw std::runtime_error("prototype table without a 'name' key");
  }
  T* result = GetObject<T>(name);
  if (GetBool(table, "parameter", false)) {
    result->showInExplorers = false;
    if (auto* goods = dynamic_cast<Goods*>(result)) goods->isLinkable = false;
  }
  return result;
}

template <typename T>
bool DataDeserializer::GetRef(const Tbl& table, const char* key, T*& result) {
  result = nullptr;
  std::string name;
  if (!GetStrOpt(table, key, name) || name.empty()) return false;
  result = GetObject<T>(name);
  return true;
}

template <typename T>
T* DataDeserializer::DeserializeCommon(const Tbl& table, const char* prototypeType) {
  std::string name;
  if (!GetStrOpt(table, "name", name)) {
    throw std::runtime_error(std::string("definition of a ") + prototypeType +
                             " without a name");
  }
  T* target = GetObject<T>(table);
  target->factorioType = GetStr(table, "type");
  // Locale lookups are skipped (web i18n); locName falls back to name later.

  // Icon specs (paths + layout), for the web layer to extract and render.
  std::string icon;
  if (GetStrOpt(table, "icon", icon)) {
    FactorioIconPart part{.path = icon};
    part.size = GetInt(table, "icon_size", 64);
    target->iconSpec = {std::move(part)};
  } else if (Tbl iconList = GetTbl(table, "icons")) {
    std::optional<float> scale;
    std::vector<FactorioIconPart> parts;
    for (const Tbl& x : ArrayTbls(iconList)) {
      std::string path;
      if (!GetStrOpt(x, "icon", path)) {
        throw std::runtime_error("an icon layer for " + name + " has no path");
      }
      int expectedSize = std::is_same_v<T, Technology> ? 256 : 64;
      FactorioIconPart part{.path = path};
      part.size = GetInt(x, "icon_size", 64);
      part.scale = GetFloat(x, "scale", expectedSize / 2.0f / part.size);
      if (factorioVersion_ < Version{2, 0, 0}) part.scale *= part.size / 64.0f;

      if (!scale.has_value()) {  // first layer
        if (part.scale < 1) {
          scale = part.scale;
          part.scale = 1;
        } else {
          scale = 1;
        }
      } else {
        part.scale /= *scale;
      }

      if (Tbl shift = GetTbl(x, "shift")) {
        part.x = static_cast<float>(GetNumIdx(shift, 1)) / *scale;
        part.y = static_cast<float>(GetNumIdx(shift, 2)) / *scale;
      }
      if (Tbl tint = GetTbl(x, "tint")) {
        int arrayCount = ArrayLen(tint);
        if (arrayCount == 3 || arrayCount == 4) {
          part.r = static_cast<float>(GetNumIdx(tint, 1, 0));
          part.g = static_cast<float>(GetNumIdx(tint, 2, 0));
          part.b = static_cast<float>(GetNumIdx(tint, 3, 0));
          part.a = static_cast<float>(GetNumIdx(tint, 4, 1));
        } else {
          part.r = GetFloat(tint, "r", 0);
          part.g = GetFloat(tint, "g", 0);
          part.b = GetFloat(tint, "b", 0);
          part.a = GetFloat(tint, "a", 1);
        }
        if (part.r > 1 || part.g > 1 || part.b > 1 || part.a > 1) {
          part.r /= 255;
          part.g /= 255;
          part.b /= 255;
          part.a /= 255;
        }
      }
      parts.push_back(std::move(part));
    }
    target->iconSpec = std::move(parts);
  }
  return target;
}


}  // namespace yafc
