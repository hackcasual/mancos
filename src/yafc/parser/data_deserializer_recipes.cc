// Recipes, technologies, products/ingredients (with fluid temperatures) —
// port of FactorioDataDeserializer_RecipeAndTechnology.cs.
#include <cmath>
#include <stdexcept>

#include "yafc/parser/data_deserializer.h"

namespace yafc {

namespace {
constexpr const char* kLabs = "labs.";
constexpr const char* kTechnologyTrigger = "technology-trigger";
}  // namespace

void DataDeserializer::DeserializeRecipe(const Tbl& table) {
  Recipe* recipe = DeserializeCommon<Recipe>(table, "recipe");
  LoadRecipeData(recipe, table);

  auto readCategories = [&](const Tbl& categories) {
    for (const std::string& category : ArrayStrs(categories)) {
      recipeCategories_.Add(category, recipe);
      if (category == "recycling" || category == "recycling-or-hand-crafting") {
        recipe->specialType = FactorioObjectSpecialType::Recycling;
      }
    }
  };

  if (Tbl categoriesTable = GetTbl(table, "categories")) {  // 2.1
    readCategories(categoriesTable);
  } else {  // 2.0 and 1.1
    std::string recipeCategory = GetStr(table, "category", "crafting");
    if (recipeCategory == "recycling" || recipeCategory == "recycling-or-hand-crafting") {
      recipe->specialType = FactorioObjectSpecialType::Recycling;
    }
    recipeCategories_.Add(recipeCategory, recipe);
    if (Tbl additional = GetTbl(table, "additional_categories")) {
      readCategories(additional);
    }
  }

  uint32_t allowedEffects = AllowedEffects::kNone;
  if (GetBool(table, "allow_consumption", true)) allowedEffects |= AllowedEffects::kConsumption;
  if (GetBool(table, "allow_speed", true)) allowedEffects |= AllowedEffects::kSpeed;
  if (GetBool(table, "allow_productivity", false)) allowedEffects |= AllowedEffects::kProductivity;
  if (GetBool(table, "allow_pollution", true)) allowedEffects |= AllowedEffects::kPollution;
  if (GetBool(table, "allow_quality", true)) allowedEffects |= AllowedEffects::kQuality;
  recipe->allowedEffects = allowedEffects;

  if (Tbl categories = GetTbl(table, "allowed_module_categories")) {
    recipe->allowedModuleCategories = ArrayStrs(categories);
  }
}

void DataDeserializer::DeserializeTechnology(const Tbl& table) {
  Technology* technology = DeserializeCommon<Technology>(table, "technology");
  LoadTechnologyData(technology, table);
  technology->products = {Product(science_, 1)};
}

void DataDeserializer::LoadRecipeData(Recipe* recipe, const Tbl& table) {
  recipe->ingredients = LoadIngredientList(table, recipe->typeDotName());
  recipe->products = LoadProductList(table, recipe->typeDotName(),
                                     /*allowSimpleSyntax=*/factorioVersion_ < Version{2, 0, 0});

  recipe->time = GetFloat(table, "energy_required", 0.5f);
  recipe->preserveProducts = GetBool(table, "preserve_products_in_machine_output", false);

  std::string mainProductName;
  if (GetStrOpt(table, "main_product", mainProductName) && !mainProductName.empty()) {
    for (const Product& p : recipe->products) {
      if (p.goods != nullptr && p.goods->name == mainProductName) {
        recipe->mainProduct = p.goods;
        break;
      }
    }
  } else if (recipe->products.size() == 1) {
    recipe->mainProduct = recipe->products[0].goods;
  }

  recipe->hidden = GetBool(table, "hidden", false);
  recipe->enabled = GetBool(table, "enabled", true);
  recipe->maximumProductivity = GetFloat(table, "maximum_productivity", 3.0f);
}

void DataDeserializer::LoadTechnologyData(Technology* technology, const Tbl& table) {
  Tbl unit = GetTbl(table, "unit");
  if (unit) {
    technology->ingredients = LoadResearchIngredientList(unit);
    recipeCategories_.Add(kLabs, technology);
  } else if (Tbl researchTrigger = GetTbl(table, "research_trigger")) {
    LoadResearchTrigger(researchTrigger, technology);
    recipeCategories_.Add(kTechnologyTrigger, technology);
  } else {
    Error("could not get requirements to unlock " + technology->name);
  }

  technology->enabled = GetBool(table, "enabled", true);
  technology->time = GetFloat(unit, "time", 1.0f);
  technology->count = GetFloat(unit, "count", 1000.0f);
  // TODO(port): count_formula (helpers.evaluate_expression exists; upstream
  // has the same TODO).

  if (Tbl prerequisites = GetTbl(table, "prerequisites")) {
    technology->prerequisites.clear();
    for (const std::string& name : ArrayStrs(prerequisites)) {
      technology->prerequisites.push_back(GetObject<Technology>(name));
    }
  }

  if (Tbl modifiers = GetTbl(table, "effects")) {
    for (const Tbl& modifier : ArrayTbls(modifiers)) {
      std::string type = GetStr(modifier, "type");
      if (type == "unlock-recipe") {
        Recipe* recipe = nullptr;
        if (GetRef<Recipe>(modifier, "recipe", recipe)) {
          technology->unlockRecipes.push_back(recipe);
        }
      } else if (type == "change-recipe-productivity") {
        Recipe* recipe = nullptr;
        if (GetRef<Recipe>(modifier, "recipe", recipe)) {
          float change = technology->changeRecipeProductivity[recipe] +
                         GetFloat(modifier, "change", 0);
          technology->changeRecipeProductivity[recipe] = change;
          recipe->technologyProductivity[technology] = change;
        }
      } else if (type == "unlock-quality") {
        Quality* quality = nullptr;
        if (GetRef<Quality>(modifier, "quality", quality)) {
          quality->technologyUnlock.push_back(technology);
        }
      } else if (type == "mining-with-fluid") {
        technology->unlocksFluidMining = true;
      } else if (type == "inserter-stack-size-bonus") {
        technology->inserterStackSizeBonus += GetFloat(modifier, "modifier", 0);
      } else if (type == "bulk-inserter-capacity-bonus" ||
                 type == "stack-inserter-capacity-bonus") {
        technology->bulkInserterCapacityBonus += GetFloat(modifier, "modifier", 0);
      } else if (type == "belt-stack-size-bonus") {
        technology->beltStackSizeBonus += GetFloat(modifier, "modifier", 0);
      } else if (type == "unlock-space-location") {
        Location* location = nullptr;
        if (GetRef<Location>(modifier, "space_location", location)) {
          technology->unlockLocations.push_back(location);
        }
      }
    }
  }
}

Goods* DataDeserializer::LoadItemOrFluid(const Tbl& table, bool useTemperature,
                                         bool isAlwaysItem) {
  std::string type;
  bool hasType = GetStrOpt(table, "type", type);
  bool is20 = !(factorioVersion_ < Version{2, 0, 0});

  if ((is20 && isAlwaysItem) || hasType) {
    std::string name;
    if (GetStrOpt(table, "name", name)) {
      if (isAlwaysItem || type == "item") return GetObject<Item>(name);
      if (type == "fluid") {
        if (useTemperature) {
          if (Has(table, "temperature")) {
            return GetFluidFixedTemp(name, GetInt(table, "temperature", 0));
          }
          return GetFluidFixedTemp(name, GetObject<Fluid>(name)->temperatureRange.min);
        }
        return GetObject<Fluid>(name);
      }
    } else if (type == "research-progress") {
      std::string researchItem;
      if (GetStrOpt(table, "research_item", researchItem)) {
        return GetObject<Item>(researchItem);
      }
    }
  } else if (!is20) {
    // 1.1 simple syntax: { [1] = "name", [2] = amount } or named item.
    std::string name;
    if (GetStrOpt(table, "name", name)) return GetObject<Item>(name);
    name = GetStrIdx(table, 1);
    if (!name.empty()) return GetObject<Item>(name);
  }
  return nullptr;
}

std::function<Product(const DataDeserializer::Tbl&)> DataDeserializer::LoadProduct(
    const std::string& recipeName, int multiplier, std::optional<float> percentSpoiled,
    bool isAlwaysItem) {
  return [this, recipeName, multiplier, percentSpoiled, isAlwaysItem](const Tbl& table) {
    Goods* goods = LoadItemOrFluid(table, /*useTemperature=*/true, isAlwaysItem);
    float min = 0, max = 0, catalyst = 0;
    std::optional<float> spoiled = percentSpoiled;

    if (dynamic_cast<Item*>(goods) != nullptr) {
      if (Has(table, "amount")) {
        min = max = static_cast<float>(GetInt(table, "amount", 0));
      } else if (Has(table, "amount_min") && Has(table, "amount_max")) {
        min = static_cast<float>(GetInt(table, "amount_min", 0));
        max = static_cast<float>(GetInt(table, "amount_max", 0));
      } else {
        throw std::runtime_error("could not load product amount for " + recipeName);
      }
      if (!spoiled.has_value() && Has(table, "percent_spoiled")) {
        spoiled = GetFloat(table, "percent_spoiled", 0);
      }
      float extraCountFraction = GetFloat(table, "extra_count_fraction", 0);
      min += extraCountFraction;
      max += extraCountFraction;
      catalyst = GetFloat(table, "ignored_by_productivity",
                          GetFloat(table, "ignored_by_stats",
                                   GetFloat(table, "catalyst_amount", 0)));
    } else if (dynamic_cast<Fluid*>(goods) != nullptr) {
      if (Has(table, "amount")) {
        min = max = GetFloat(table, "amount", 0);
      } else if (Has(table, "amount_min") && Has(table, "amount_max")) {
        min = GetFloat(table, "amount_min", 0);
        max = GetFloat(table, "amount_max", 0);
      } else {
        throw std::runtime_error("could not load product amount for " + recipeName);
      }
      catalyst = GetFloat(table, "ignored_by_productivity",
                          GetFloat(table, "ignored_by_stats",
                                   GetFloat(table, "catalyst_amount", 0)));
    } else {
      throw std::runtime_error("could not load a product for " + recipeName);
    }

    float probability;
    if (factorioVersion_ < Version{2, 1, 0}) {
      probability = GetFloat(table, "probability", 1);
    } else {
      probability = GetFloat(table, "independent_probability", 1);
      if (Tbl shared = GetTbl(table, "shared_probability")) {
        probability = static_cast<float>(
            probability * (GetNum(shared, "max", 1) - GetNum(shared, "min", 0)));
      }
    }

    Product product(goods, min * multiplier, max * multiplier, probability);
    product.percentSpoiled = spoiled;
    if (catalyst > 0) product.SetCatalyst(catalyst);
    return product;
  };
}

std::vector<Product> DataDeserializer::LoadProductList(const Tbl& table,
                                                       const std::string& typeDotName,
                                                       bool allowSimpleSyntax) {
  std::optional<float> percentSpoiled;
  if (GetBool(table, "result_is_always_fresh", false)) percentSpoiled = 0;

  if (Tbl resultList = GetTbl(table, "results")) {
    std::vector<Product> products;
    auto loader = LoadProduct(typeDotName, 1, percentSpoiled);
    for (const Tbl& t : ArrayTbls(resultList)) {
      Product product = loader(t);
      if (product.amount != 0) products.push_back(std::move(product));
    }
    return products;
  }

  std::string name;
  if (allowSimpleSyntax && GetStrOpt(table, "result", name)) {
    Product product(GetObject<Item>(name),
                    static_cast<float>(GetInt(table, "count", GetInt(table, "result_count", 1))));
    product.percentSpoiled = percentSpoiled;
    return {product};
  }
  return {};
}

std::vector<Ingredient> DataDeserializer::LoadIngredientList(
    const Tbl& table, const std::string& typeDotName) {
  std::vector<Ingredient> result;
  Tbl list = GetTbl(table, "ingredients");
  if (!list) return result;
  for (const Tbl& t : ArrayTbls(list)) {
    Goods* goods = LoadItemOrFluid(t, /*useTemperature=*/false, /*isAlwaysItem=*/false);
    if (goods == nullptr) {
      Error("failed to load at least one ingredient for " + typeDotName);
      continue;
    }
    float amount = dynamic_cast<Item*>(goods) != nullptr
                       ? static_cast<float>(GetInt(t, "amount", 0))
                       : GetFloat(t, "amount", 0);
    Ingredient ingredient(goods, amount);
    if (auto* f = dynamic_cast<Fluid*>(goods)) {
      if (Has(t, "temperature")) {
        ingredient.temperature = TemperatureRange(GetInt(t, "temperature", 0));
      } else {
        ingredient.temperature =
            TemperatureRange(GetInt(t, "minimum_temperature", f->temperatureRange.min),
                             GetInt(t, "maximum_temperature", f->temperatureRange.max));
      }
    }
    result.push_back(std::move(ingredient));
  }
  return result;
}

std::vector<Ingredient> DataDeserializer::LoadResearchIngredientList(const Tbl& unit) {
  std::vector<Ingredient> result;
  Tbl list = GetTbl(unit, "ingredients");
  if (!list) return result;
  for (const Tbl& t : ArrayTbls(list)) {
    std::string name = GetStrIdx(t, 1);
    int amount = static_cast<int>(GetNumIdx(t, 2, 0));
    if (!name.empty() && amount != 0) {
      result.emplace_back(GetObject<Item>(name), static_cast<float>(amount));
    }
  }
  return result;
}

void DataDeserializer::LoadResearchTrigger(const Tbl& trigger, Technology* technology) {
  std::string type;
  if (!GetStrOpt(trigger, "type", type)) {
    Error("research trigger of " + technology->typeDotName() + " has no type");
    return;
  }

  // Reads quality-filtered item/entity references ({name=..., quality=..., comparator=...}).
  auto loadQualityFromFilter = [&](const char* key, std::string& objectName) {
    if (GetStrOpt(trigger, key, objectName)) return true;
    if (Tbl filter = GetTbl(trigger, key)) {
      if (GetStrOpt(filter, "name", objectName)) {
        std::string qualityName = GetStr(filter, "quality");
        auto it = registeredObjects_.find({Kind::Quality, qualityName});
        if (it == registeredObjects_.end()) return true;
        auto* quality = static_cast<Quality*>(it->second);
        std::string comparator = GetStr(filter, "comparator");
        if (((comparator == "!=" || comparator == "≠") && quality->level == 0) ||
            comparator == ">") {
          technology->triggerMinimumQuality = quality->nextQuality;
        } else if (comparator == ">=" || comparator == "≥" || comparator == "=") {
          technology->triggerMinimumQuality = quality;
        }
        return true;
      }
    }
    return false;
  };

  if (type == "craft-fluid") {
    std::string fluidName;
    if (!GetStrOpt(trigger, "fluid", fluidName)) {
      Error("research trigger craft-fluid of " + technology->typeDotName() +
            " has no fluid");
      return;
    }
    float count = GetFloat(trigger, "count", 1);
    technology->ingredients = {Ingredient(GetObject<Fluid>(fluidName), count)};
    technology->flags = RecipeFlags::kHasResearchTriggerCraft;
  } else if (type == "craft-item") {
    std::string itemName;
    if (!loadQualityFromFilter("item", itemName)) {
      Error("research trigger craft-item of " + technology->typeDotName() +
            " has no recognized item");
      return;
    }
    float count = GetFloat(trigger, "count", 1);
    technology->ingredients = {Ingredient(GetObject<Item>(itemName), count)};
    technology->flags = RecipeFlags::kHasResearchTriggerCraft;
  } else if (type == "capture-spawner") {
    technology->flags = RecipeFlags::kHasResearchTriggerCaptureEntity;
    std::string entity;
    if (GetStrOpt(trigger, "entity", entity)) {
      pendingTriggerEntities_.push_back({technology, {entity}, false});
    } else {
      pendingTriggerEntities_.push_back({technology, {}, true});
    }
  } else if (type == "mine-entity") {
    technology->flags = RecipeFlags::kHasResearchTriggerMineEntity;
    std::string entity;
    if (GetStrOpt(trigger, "entity", entity)) {  // 2.0
      pendingTriggerEntities_.push_back({technology, {entity}, false});
    } else if (Tbl entities = GetTbl(trigger, "entities")) {  // 2.1
      pendingTriggerEntities_.push_back({technology, ArrayStrs(entities), false});
    } else {
      Error("research trigger mine-entity of " + technology->typeDotName() +
            " has no entity");
    }
  } else if (type == "build-entity") {
    technology->flags = RecipeFlags::kHasResearchTriggerBuildEntity;
    std::string entity;
    if (loadQualityFromFilter("entity", entity)) {
      pendingTriggerEntities_.push_back({technology, {entity}, false});
    } else {
      Error("research trigger build-entity of " + technology->typeDotName() +
            " has no recognized entity");
    }
  } else if (type == "create-space-platform") {
    technology->flags = RecipeFlags::kHasResearchTriggerCreateSpacePlatform;
  } else if (type == "send-item-to-orbit") {
    std::string itemName;
    if (!GetStrOpt(trigger, "item", itemName)) {
      Error("research trigger send-item-to-orbit of " + technology->typeDotName() +
            " has no item");
      return;
    }
    Item* item = GetObject<Item>(itemName);
    technology->triggerObject = item;
    technology->flags |= RecipeFlags::kHasResearchTriggerSendToOrbit;
    EnsureLaunchRecipe(item, nullptr);
  } else if (type == "scripted") {
    auto obj = std::make_unique<ResearchTrigger>();
    obj->name = technology->name;
    obj->factorioType = GetStr(trigger, "type");
    ResearchTrigger* raw = obj.get();
    allObjects_.push_back(raw);
    registeredObjects_[{Kind::Trigger, raw->name}] = raw;
    owned_.push_back(std::move(obj));
    technology->triggerObject = raw;
    technology->flags |= RecipeFlags::kHasResearchTriggerScripted;
    rootAccessible_.push_back(raw);
  } else {
    Error("research trigger of " + technology->typeDotName() +
          " has unsupported type " + type);
  }
}

void DataDeserializer::UpdateRecipeIngredientFluids() {
  for (FactorioObject* o : allObjects_) {
    auto* recipe = dynamic_cast<Recipe*>(o);
    if (recipe == nullptr) continue;
    for (Ingredient& ingredient : recipe->ingredients) {
      auto* fluid = dynamic_cast<Fluid*>(ingredient.goods);
      if (fluid == nullptr || fluid->variants == nullptr) continue;

      const std::vector<Fluid*>& variants = *fluid->variants;
      int min = -1, max = static_cast<int>(variants.size()) - 1;
      for (int i = 0; i < static_cast<int>(variants.size()); ++i) {
        Fluid* variant = variants[i];
        if (variant->temperature < ingredient.temperature.min) continue;
        if (min == -1) min = i;
        if (variant->temperature > ingredient.temperature.max) {
          max = i - 1;
          break;
        }
      }

      if (min >= 0 && max >= 0) {
        ingredient.goods = variants[min];
        if (max > min) {
          ingredient.variants.assign(variants.begin() + min, variants.begin() + max + 1);
        }
      }
    }
  }
}

void DataDeserializer::UpdateRecipeCatalysts() {
  // 1.1-only behavior (catalyst_amount inference); no-op for 2.0.
}

}  // namespace yafc
