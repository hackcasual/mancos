#include "yafc/serialization/database_dump.h"

#include "yafc/serialization/icon_scale.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include <miniz.h>

#include "yafc/parser/factorio_data_source.h"

namespace yafc {

// Access hook for Product's private productivityAmount_.
struct DatabaseDumpAccess {
  static float Get(const Product& p) { return p.productivityAmount_; }
  static void Set(Product& p, float v) { p.productivityAmount_ = v; }
};

namespace {

using nlohmann::json;

int32_t Ref(const FactorioObject* obj) { return obj != nullptr ? obj->id : -1; }

template <typename T>
json Refs(const std::vector<T*>& list) {
  json arr = json::array();
  for (const T* o : list) arr.push_back(Ref(o));
  return arr;
}

json DumpTemperatureRange(const TemperatureRange& r) { return json{r.min, r.max}; }
TemperatureRange LoadTemperatureRange(const json& j) {
  return {j.at(0).get<int>(), j.at(1).get<int>()};
}

json DumpIconSpec(const std::vector<FactorioIconPart>& spec) {
  json arr = json::array();
  for (const FactorioIconPart& p : spec) {
    arr.push_back(json{{"p", p.path}, {"sz", p.size}, {"x", p.x}, {"y", p.y},
                       {"r", p.r}, {"g", p.g}, {"b", p.b}, {"a", p.a},
                       {"s", p.scale}});
  }
  return arr;
}

json DumpProducts(const std::vector<Product>& products) {
  json arr = json::array();
  for (const Product& p : products) {
    json e{{"goods", Ref(p.goods)}, {"min", p.amountMin}, {"max", p.amountMax},
           {"prob", p.probability}, {"amount", p.amount},
           {"prodAmount", DatabaseDumpAccess::Get(p)}};
    if (p.percentSpoiled.has_value()) e["spoiled"] = *p.percentSpoiled;
    arr.push_back(std::move(e));
  }
  return arr;
}

json DumpIngredients(const std::vector<Ingredient>& ingredients) {
  json arr = json::array();
  for (const Ingredient& i : ingredients) {
    arr.push_back(json{{"goods", Ref(i.goods)}, {"amount", i.amount},
                       {"variants", Refs(i.variants)},
                       {"temp", DumpTemperatureRange(i.temperature)}});
  }
  return arr;
}

json DumpEnergy(const EntityEnergy& e) {
  json emissions = json::array();
  for (const auto& [name, value] : e.emissions) emissions.push_back(json{name, value});
  return json{{"type", static_cast<int>(e.type)},
              {"workTemp", DumpTemperatureRange(e.workingTemperature)},
              {"accTemp", DumpTemperatureRange(e.acceptedTemperature)},
              {"emissions", std::move(emissions)},
              {"drain", e.drain},
              {"fuelLimit", std::isfinite(e.baseFuelConsumptionLimit)
                                ? json(e.baseFuelConsumptionLimit)
                                : json()},
              {"effectivity", e.effectivity},
              {"fuels", Refs(e.fuels)}};
}

// The concrete class tag (type() alone cannot distinguish subclasses).
std::string KindOf(const FactorioObject* o) {
  if (dynamic_cast<const Module*>(o)) return "Module";
  if (dynamic_cast<const Ammo*>(o)) return "Ammo";
  if (dynamic_cast<const Item*>(o)) return "Item";
  if (dynamic_cast<const Fluid*>(o)) return "Fluid";
  if (dynamic_cast<const Special*>(o)) return "Special";
  if (dynamic_cast<const Mechanics*>(o)) return "Mechanics";
  if (dynamic_cast<const Technology*>(o)) return "Technology";
  if (dynamic_cast<const Recipe*>(o)) return "Recipe";
  if (dynamic_cast<const EntityAttractor*>(o)) return "EntityAttractor";
  if (dynamic_cast<const EntityReactor*>(o)) return "EntityReactor";
  if (dynamic_cast<const EntityCrafter*>(o)) return "EntityCrafter";
  if (dynamic_cast<const EntityBeacon*>(o)) return "EntityBeacon";
  if (dynamic_cast<const EntityBelt*>(o)) return "EntityBelt";
  if (dynamic_cast<const EntityPump*>(o)) return "EntityPump";
  if (dynamic_cast<const EntityContainer*>(o)) return "EntityContainer";
  if (dynamic_cast<const EntityInserter*>(o)) return "EntityInserter";
  if (dynamic_cast<const EntityAccumulator*>(o)) return "EntityAccumulator";
  if (dynamic_cast<const EntityProjectile*>(o)) return "EntityProjectile";
  if (dynamic_cast<const EntitySpawner*>(o)) return "EntitySpawner";
  if (dynamic_cast<const Entity*>(o)) return "Entity";
  if (dynamic_cast<const Tile*>(o)) return "Tile";
  if (dynamic_cast<const Location*>(o)) return "Location";
  if (dynamic_cast<const Quality*>(o)) return "Quality";
  if (dynamic_cast<const ResearchTrigger*>(o)) return "ResearchTrigger";
  throw std::runtime_error("dump: unknown object kind for " + o->name);
}

std::unique_ptr<FactorioObject> CreateByKind(const std::string& kind) {
  if (kind == "Module") return std::make_unique<Module>();
  if (kind == "Ammo") return std::make_unique<Ammo>();
  if (kind == "Item") return std::make_unique<Item>();
  if (kind == "Fluid") return std::make_unique<Fluid>();
  if (kind == "Special") return std::make_unique<Special>();
  if (kind == "Mechanics") return std::make_unique<Mechanics>();
  if (kind == "Technology") return std::make_unique<Technology>();
  if (kind == "Recipe") return std::make_unique<Recipe>();
  if (kind == "EntityAttractor") return std::make_unique<EntityAttractor>();
  if (kind == "EntityReactor") return std::make_unique<EntityReactor>();
  if (kind == "EntityCrafter") return std::make_unique<EntityCrafter>();
  if (kind == "EntityBeacon") return std::make_unique<EntityBeacon>();
  if (kind == "EntityBelt") return std::make_unique<EntityBelt>();
  if (kind == "EntityPump") return std::make_unique<EntityPump>();
  if (kind == "EntityContainer") return std::make_unique<EntityContainer>();
  if (kind == "EntityInserter") return std::make_unique<EntityInserter>();
  if (kind == "EntityAccumulator") return std::make_unique<EntityAccumulator>();
  if (kind == "EntityProjectile") return std::make_unique<EntityProjectile>();
  if (kind == "EntitySpawner") return std::make_unique<EntitySpawner>();
  if (kind == "Entity") return std::make_unique<Entity>();
  if (kind == "Tile") return std::make_unique<Tile>();
  if (kind == "Location") return std::make_unique<Location>();
  if (kind == "Quality") return std::make_unique<Quality>();
  if (kind == "ResearchTrigger") return std::make_unique<ResearchTrigger>();
  // Forward compatibility: a newer bundler may introduce entity subkinds this
  // build doesn't know (e.g. EntityPump before it existed here). Entities are
  // structurally safe to load as their base class — derived-only fields are
  // skipped because every reader block is dynamic_cast-gated. Non-entity
  // kinds stay fatal: recipes/goods with unknown semantics would corrupt the
  // solve rather than merely lose detail.
  if (kind.rfind("Entity", 0) == 0) return std::make_unique<Entity>();
  throw std::runtime_error("load: unknown object kind " + kind);
}

json DumpObject(const FactorioObject* o) {
  json e{{"kind", KindOf(o)},       {"id", o->id},
         {"factorioType", o->factorioType}, {"name", o->name},
         {"locName", o->locName},   {"locDescr", o->locDescr},
         {"special", static_cast<int>(o->specialType)},
         {"explorers", o->showInExplorers},
         {"icon", DumpIconSpec(o->iconSpec)}};

  if (auto* g = dynamic_cast<const Goods*>(o)) {
    e["fuelValue"] = g->fuelValue;
    e["isLinkable"] = g->isLinkable;
    e["production"] = Refs(g->production);
    e["usages"] = Refs(g->usages);
    e["miscSources"] = Refs(g->miscSources);
    e["fuelFor"] = Refs(g->fuelFor);
  }
  if (auto* i = dynamic_cast<const Item*>(o)) {
    e["fuelResult"] = Ref(i->fuelResult);
    e["fuelResultOf"] = Refs(i->fuelResultOf);
    e["stackSize"] = i->stackSize;
    e["placeResult"] = Ref(i->placeResult);
    e["plantResult"] = Ref(i->plantResult);
    e["rocketCapacity"] = i->rocketCapacity;
    e["weight"] = i->weight;
    e["i2w"] = i->ingredientToWeightCoefficient;
    e["spoilResult"] = Ref(i->spoilResult);
    e["baseSpoilTime"] = i->baseSpoilTime;
  }
  if (auto* m = dynamic_cast<const Module*>(o)) {
    const ModuleSpecification& s = m->moduleSpecification;
    e["module"] = json{{"cat", s.category}, {"cons", s.baseConsumption},
                       {"speed", s.baseSpeed}, {"prod", s.baseProductivity},
                       {"pol", s.basePollution}, {"qual", s.baseQuality}};
  }
  if (auto* am = dynamic_cast<const Ammo*>(o)) {
    e["projectiles"] = am->projectileNames;
    if (am->targetFilter.has_value()) e["targetFilter"] = *am->targetFilter;
  }
  if (auto* f = dynamic_cast<const Fluid*>(o)) {
    e["originalName"] = f->originalName;
    e["heatCapacity"] = f->heatCapacity;
    e["tempRange"] = DumpTemperatureRange(f->temperatureRange);
    e["temperature"] = f->temperature;
    e["heatValue"] = f->heatValue;
    if (f->variants != nullptr) e["variants"] = Refs(*f->variants);
  }
  if (auto* s = dynamic_cast<const Special*>(o)) {
    e["signal"] = s->virtualSignal;
    e["power"] = s->power;
    e["isVoid"] = s->isVoid;
    e["temperature"] = s->temperature;
    if (s->variants != nullptr) e["variants"] = Refs(*s->variants);
  }
  if (auto* r = dynamic_cast<const RecipeOrTechnology*>(o)) {
    e["crafters"] = Refs(r->crafters);
    e["ingredients"] = DumpIngredients(r->ingredients);
    e["products"] = DumpProducts(r->products);
    e["sourceEntity"] = Ref(r->sourceEntity);
    e["sourceTiles"] = Refs(r->sourceTiles);
    e["mainProduct"] = Ref(r->mainProduct);
    e["time"] = r->time;
    e["enabled"] = r->enabled;
    e["flags"] = r->flags;
  }
  if (auto* r = dynamic_cast<const Recipe*>(o)) {
    e["allowedEffects"] = r->allowedEffects;
    e["allowedModuleCategories"] = r->allowedModuleCategories;
    e["technologyUnlock"] = Refs(r->technologyUnlock);
    json techProd = json::array();
    for (const auto& [tech, value] : r->technologyProductivity) {
      techProd.push_back(json{Ref(tech), value});
    }
    e["technologyProductivity"] = std::move(techProd);
    e["preserveProducts"] = r->preserveProducts;
    e["hidden"] = r->hidden;
    if (r->maximumProductivity.has_value()) e["maxProd"] = *r->maximumProductivity;
  }
  if (auto* m = dynamic_cast<const Mechanics*>(o)) {
    e["source"] = Ref(m->source);
    e["locKey"] = m->localizationKey;
  }
  if (auto* t = dynamic_cast<const Technology*>(o)) {
    e["count"] = t->count;
    e["prerequisites"] = Refs(t->prerequisites);
    e["unlockRecipes"] = Refs(t->unlockRecipes);
    e["unlockLocations"] = Refs(t->unlockLocations);
    json changeProd = json::array();
    for (const auto& [recipe, value] : t->changeRecipeProductivity) {
      changeProd.push_back(json{Ref(recipe), value});
    }
    e["changeRecipeProductivity"] = std::move(changeProd);
    e["unlocksFluidMining"] = t->unlocksFluidMining;
    e["inserterStackBonus"] = t->inserterStackSizeBonus;
    e["bulkInserterBonus"] = t->bulkInserterCapacityBonus;
    e["beltStackBonus"] = t->beltStackSizeBonus;
    e["triggerObject"] = Ref(t->triggerObject);
    e["triggerEntities"] = Refs(t->triggerEntities);
    e["triggerMinQuality"] = Ref(t->triggerMinimumQuality);
  }
  if (auto* en = dynamic_cast<const Entity*>(o)) {
    e["loot"] = DumpProducts(en->loot);
    e["mapGenerated"] = en->mapGenerated;
    e["mapGenDensity"] = en->mapGenDensity;
    e["basePower"] = en->basePower;
    e["hasEnergy"] = en->hasEnergy;
    e["energy"] = DumpEnergy(en->energy);
    e["sourceEntities"] = Refs(en->sourceEntities);
    e["itemsToPlace"] = Refs(en->itemsToPlace);
    e["spawnLocations"] = Refs(en->spawnLocations);
    e["heatingPower"] = en->heatingPower;
    e["hasBurntInventory"] = en->hasBurntInventory;
    e["width"] = en->width;
    e["height"] = en->height;
    e["size"] = en->size;
    e["spoilResult"] = Ref(en->spoilResult);
    e["baseSpoilTime"] = en->baseSpoilTime;
    e["autoplace"] = en->autoplaceControl;
  }
  if (auto* em = dynamic_cast<const EntityWithModules*>(o)) {
    e["allowedEffects"] = em->allowedEffects;
    e["allowedModuleCategories"] = em->allowedModuleCategories;
    e["moduleSlots"] = em->moduleSlots;
  }
  if (auto* ec = dynamic_cast<const EntityCrafter*>(o)) {
    e["itemInputs"] = ec->itemInputs;
    e["fluidInputs"] = ec->fluidInputs;
    e["inputs"] = Refs(ec->inputs);
    e["recipes"] = Refs(ec->recipes);
    e["craftingSpeed"] = ec->baseCraftingSpeed;
    e["erBaseProd"] = ec->effectReceiverBaseProductivity;
    const EffectReceiver& er = ec->effectReceiver;
    e["effectReceiver"] =
        json{{"cons", er.baseEffect.consumption}, {"speed", er.baseEffect.speed},
             {"prod", er.baseEffect.productivity}, {"pol", er.baseEffect.pollution},
             {"qual", er.baseEffect.quality},      {"um", er.usesModuleEffects},
             {"ub", er.usesBeaconEffects},         {"us", er.usesSurfaceEffects}};
    e["rocketInventorySize"] = ec->rocketInventorySize;
    e["hasVector"] = ec->hasVectorToPlaceResult;
  }
  if (auto* er2 = dynamic_cast<const EntityReactor*>(o)) {
    e["neighborBonus"] = er2->reactorNeighborBonus;
  }
  if (auto* ea = dynamic_cast<const EntityAttractor*>(o)) {
    e["range"] = ea->range;
    e["attractorEfficiency"] = ea->attractorEfficiency;
    e["attractorDrain"] = ea->drain;
  }
  if (auto* eb = dynamic_cast<const EntityBelt*>(o)) {
    e["beltSpeed"] = eb->beltItemsPerSecond;
  }
  if (auto* epu = dynamic_cast<const EntityPump*>(o)) {
    e["pumpingSpeed"] = epu->pumpingSpeed;
  }
  if (auto* ebc = dynamic_cast<const EntityBeacon*>(o)) {
    e["beaconEfficiency"] = ebc->beaconEfficiency;
    e["profile"] = ebc->profileValues;
  }
  if (auto* ecn = dynamic_cast<const EntityContainer*>(o)) {
    e["inventorySize"] = ecn->inventorySize;
    e["logisticMode"] = ecn->logisticMode;
    e["logisticSlots"] = ecn->logisticSlotsCount;
  }
  if (auto* ei = dynamic_cast<const EntityInserter*>(o)) {
    e["swingTime"] = ei->inserterSwingTime;
    e["bulk"] = ei->isBulkInserter;
    e["stackBonus"] = ei->stackSizeBonus;
    e["maxBeltStack"] = ei->maxBeltStackSize;
  }
  if (auto* eac = dynamic_cast<const EntityAccumulator*>(o)) {
    e["capacity"] = eac->baseAccumulatorCapacity;
  }
  if (auto* ep = dynamic_cast<const EntityProjectile*>(o)) {
    e["placeEntities"] = ep->placeEntities;
  }
  if (auto* es = dynamic_cast<const EntitySpawner*>(o)) {
    e["captured"] = es->capturedEntityName;
  }
  if (auto* tl = dynamic_cast<const Tile*>(o)) {
    e["fluidResult"] = Ref(tl->fluidResult);
    e["locations"] = Refs(tl->locations);
  }
  if (auto* loc = dynamic_cast<const Location*>(o)) {
    e["technologyUnlock"] = Refs(loc->technologyUnlock);
    e["entitySpawns"] = loc->entitySpawns;
    e["placementControls"] = loc->placementControls;
  }
  if (auto* q = dynamic_cast<const Quality*>(o)) {
    e["next"] = Ref(q->nextQuality);
    e["prev"] = Ref(q->previousQuality);
    e["level"] = q->level;
    e["technologyUnlock"] = Refs(q->technologyUnlock);
    e["beaconConsumption"] = q->BeaconConsumptionFactor;
    e["upgradeChance"] = q->UpgradeChance;
    e["chainProbability"] = q->ChainProbability;
  }
  return e;
}

}  // namespace

nlohmann::json DumpDatabase(const Database& db) {
  json objects = json::array();
  for (const FactorioObject* o : db.objects) objects.push_back(DumpObject(o));

  json fluidVariants = json::object();
  for (const auto& [name, list] : db.fluidVariants) {
    fluidVariants[name] = Refs(list);
  }
  json aliases = json::object();
  for (const auto& [alias, target] : db.objectsByTypeName) {
    // Only store entries that are aliases (name differs from typeDotName).
    if (target->typeDotName() != alias) aliases[alias] = Ref(target);
  }

  return json{{"formatVersion", kBundleFormatVersion},
              {"objects", std::move(objects)},
              {"rootAccessible", Refs(db.rootAccessible)},
              {"fluidVariants", std::move(fluidVariants)},
              {"heatVariants", Refs(db.heatVariants)},
              {"aliases", std::move(aliases)},
              {"combinator", db.constantCombinatorCapacity}};
}

std::unique_ptr<Database> LoadDatabase(const nlohmann::json& dump) {
  if (dump.value("formatVersion", -1) != kBundleFormatVersion) {
    throw std::runtime_error("bundle: unsupported format version");
  }
  const json& objects = dump.at("objects");

  // Pass 1: instantiate everything so references can resolve.
  std::vector<std::unique_ptr<FactorioObject>> owned;
  std::vector<FactorioObject*> byId;
  owned.reserve(objects.size());
  byId.reserve(objects.size());
  for (const json& e : objects) {
    auto obj = CreateByKind(e.at("kind").get<std::string>());
    if (e.at("id").get<int>() != static_cast<int>(byId.size())) {
      throw std::runtime_error("bundle: objects not in id order");
    }
    byId.push_back(obj.get());
    owned.push_back(std::move(obj));
  }

  auto ref = [&](const json& j) -> FactorioObject* {
    int id = j.get<int>();
    if (id < 0) return nullptr;
    return byId.at(id);
  };
  auto refT = [&]<typename T>(const json& j, T*) -> T* {
    FactorioObject* o = ref(j);
    if (o == nullptr) return nullptr;
    T* typed = dynamic_cast<T*>(o);
    if (typed == nullptr) throw std::runtime_error("bundle: reference kind mismatch");
    return typed;
  };
#define REF(j, T) refT((j), static_cast<T*>(nullptr))
  auto refs = [&]<typename T>(const json& j, std::vector<T*>& out) {
    out.clear();
    for (const json& entry : j) out.push_back(REF(entry, T));
  };

  auto loadIconSpec = [](const json& j, std::vector<FactorioIconPart>& out) {
    out.clear();
    for (const json& p : j) {
      out.push_back({p.at("p").get<std::string>(), p.at("sz").get<int>(),
                     p.at("x").get<float>(), p.at("y").get<float>(),
                     p.at("r").get<float>(), p.at("g").get<float>(),
                     p.at("b").get<float>(), p.at("a").get<float>(),
                     p.at("s").get<float>()});
    }
  };
  auto loadProducts = [&](const json& j, std::vector<Product>& out) {
    out.clear();
    for (const json& p : j) {
      Product product(REF(p.at("goods"), Goods), p.at("min").get<float>(),
                      p.at("max").get<float>(), p.at("prob").get<float>());
      product.amount = p.at("amount").get<float>();
      DatabaseDumpAccess::Set(product, p.at("prodAmount").get<float>());
      if (p.contains("spoiled")) product.percentSpoiled = p["spoiled"].get<float>();
      out.push_back(product);
    }
  };
  auto loadIngredients = [&](const json& j, std::vector<Ingredient>& out) {
    out.clear();
    for (const json& i : j) {
      Ingredient ingredient;
      ingredient.goods = REF(i.at("goods"), Goods);
      ingredient.amount = i.at("amount").get<float>();
      refs(i.at("variants"), ingredient.variants);
      ingredient.temperature = LoadTemperatureRange(i.at("temp"));
      out.push_back(std::move(ingredient));
    }
  };
  auto loadEnergy = [&](const json& j, EntityEnergy& e) {
    e.type = static_cast<EntityEnergyType>(j.at("type").get<int>());
    e.workingTemperature = LoadTemperatureRange(j.at("workTemp"));
    e.acceptedTemperature = LoadTemperatureRange(j.at("accTemp"));
    e.emissions.clear();
    for (const json& em : j.at("emissions")) {
      e.emissions.emplace_back(em.at(0).get<std::string>(), em.at(1).get<float>());
    }
    e.drain = j.at("drain").get<float>();
    e.baseFuelConsumptionLimit = j.at("fuelLimit").is_null()
                                     ? std::numeric_limits<float>::infinity()
                                     : j.at("fuelLimit").get<float>();
    e.effectivity = j.at("effectivity").get<float>();
    refs(j.at("fuels"), e.fuels);
  };

  // Shared variant lists: identical id lists share one vector instance.
  std::map<std::vector<int>, std::shared_ptr<std::vector<Fluid*>>> fluidVariantShare;
  std::map<std::vector<int>, std::shared_ptr<std::vector<Special*>>> specialVariantShare;

  // Pass 2: fill fields.
  for (size_t idx = 0; idx < objects.size(); ++idx) {
    const json& e = objects[idx];
    FactorioObject* o = byId[idx];
    o->factorioType = e.at("factorioType").get<std::string>();
    o->name = e.at("name").get<std::string>();
    o->locName = e.at("locName").get<std::string>();
    o->locDescr = e.at("locDescr").get<std::string>();
    o->specialType = static_cast<FactorioObjectSpecialType>(e.at("special").get<int>());
    o->showInExplorers = e.at("explorers").get<bool>();
    loadIconSpec(e.at("icon"), o->iconSpec);

    if (auto* g = dynamic_cast<Goods*>(o)) {
      g->fuelValue = e.at("fuelValue").get<float>();
      g->isLinkable = e.at("isLinkable").get<bool>();
      refs(e.at("production"), g->production);
      refs(e.at("usages"), g->usages);
      refs(e.at("miscSources"), g->miscSources);
      refs(e.at("fuelFor"), g->fuelFor);
    }
    if (auto* i = dynamic_cast<Item*>(o)) {
      i->fuelResult = REF(e.at("fuelResult"), Item);
      refs(e.at("fuelResultOf"), i->fuelResultOf);
      i->stackSize = e.at("stackSize").get<int>();
      i->placeResult = REF(e.at("placeResult"), Entity);
      i->plantResult = REF(e.at("plantResult"), Entity);
      i->rocketCapacity = e.at("rocketCapacity").get<int>();
      i->weight = e.at("weight").get<int>();
      i->ingredientToWeightCoefficient = e.at("i2w").get<float>();
      i->spoilResult = ref(e.at("spoilResult"));
      i->baseSpoilTime = e.at("baseSpoilTime").get<float>();
    }
    if (auto* m = dynamic_cast<Module*>(o)) {
      const json& s = e.at("module");
      m->moduleSpecification = {s.at("cat").get<std::string>(),
                                s.at("cons").get<float>(), s.at("speed").get<float>(),
                                s.at("prod").get<float>(), s.at("pol").get<float>(),
                                s.at("qual").get<float>()};
    }
    if (auto* am = dynamic_cast<Ammo*>(o)) {
      am->projectileNames = e.at("projectiles").get<std::vector<std::string>>();
      if (e.contains("targetFilter")) {
        am->targetFilter = e["targetFilter"].get<std::vector<std::string>>();
      }
    }
    if (auto* f = dynamic_cast<Fluid*>(o)) {
      f->originalName = e.at("originalName").get<std::string>();
      f->heatCapacity = e.at("heatCapacity").get<float>();
      f->temperatureRange = LoadTemperatureRange(e.at("tempRange"));
      f->temperature = e.at("temperature").get<int>();
      f->heatValue = e.at("heatValue").get<float>();
      if (e.contains("variants")) {
        std::vector<int> ids = e["variants"].get<std::vector<int>>();
        auto& shared = fluidVariantShare[ids];
        if (shared == nullptr) {
          shared = std::make_shared<std::vector<Fluid*>>();
          for (int id : ids) shared->push_back(REF(json(id), Fluid));
        }
        f->variants = shared;
      }
    }
    if (auto* s = dynamic_cast<Special*>(o)) {
      s->virtualSignal = e.at("signal").get<std::string>();
      s->power = e.at("power").get<bool>();
      s->isVoid = e.at("isVoid").get<bool>();
      s->temperature = e.at("temperature").get<int>();
      if (e.contains("variants")) {
        std::vector<int> ids = e["variants"].get<std::vector<int>>();
        auto& shared = specialVariantShare[ids];
        if (shared == nullptr) {
          shared = std::make_shared<std::vector<Special*>>();
          for (int id : ids) shared->push_back(REF(json(id), Special));
        }
        s->variants = shared;
      }
    }
    if (auto* r = dynamic_cast<RecipeOrTechnology*>(o)) {
      refs(e.at("crafters"), r->crafters);
      loadIngredients(e.at("ingredients"), r->ingredients);
      loadProducts(e.at("products"), r->products);
      r->sourceEntity = REF(e.at("sourceEntity"), Entity);
      refs(e.at("sourceTiles"), r->sourceTiles);
      r->mainProduct = REF(e.at("mainProduct"), Goods);
      r->time = e.at("time").get<float>();
      r->enabled = e.at("enabled").get<bool>();
      r->flags = e.at("flags").get<uint32_t>();
    }
    if (auto* r = dynamic_cast<Recipe*>(o)) {
      r->allowedEffects = e.at("allowedEffects").get<uint32_t>();
      r->allowedModuleCategories =
          e.at("allowedModuleCategories").get<std::vector<std::string>>();
      refs(e.at("technologyUnlock"), r->technologyUnlock);
      r->technologyProductivity.clear();
      for (const json& tp : e.at("technologyProductivity")) {
        r->technologyProductivity[REF(tp.at(0), Technology)] = tp.at(1).get<float>();
      }
      r->preserveProducts = e.at("preserveProducts").get<bool>();
      r->hidden = e.at("hidden").get<bool>();
      if (e.contains("maxProd")) r->maximumProductivity = e["maxProd"].get<float>();
    }
    if (auto* m = dynamic_cast<Mechanics*>(o)) {
      m->source = ref(e.at("source"));
      m->localizationKey = e.at("locKey").get<std::string>();
    }
    if (auto* t = dynamic_cast<Technology*>(o)) {
      t->count = e.at("count").get<float>();
      refs(e.at("prerequisites"), t->prerequisites);
      refs(e.at("unlockRecipes"), t->unlockRecipes);
      refs(e.at("unlockLocations"), t->unlockLocations);
      t->changeRecipeProductivity.clear();
      for (const json& cp : e.at("changeRecipeProductivity")) {
        t->changeRecipeProductivity[REF(cp.at(0), Recipe)] = cp.at(1).get<float>();
      }
      t->unlocksFluidMining = e.at("unlocksFluidMining").get<bool>();
      t->inserterStackSizeBonus = e.value("inserterStackBonus", 0.0f);
      t->bulkInserterCapacityBonus = e.value("bulkInserterBonus", 0.0f);
      t->beltStackSizeBonus = e.value("beltStackBonus", 0.0f);
      t->triggerObject = ref(e.at("triggerObject"));
      refs(e.at("triggerEntities"), t->triggerEntities);
      t->triggerMinimumQuality = REF(e.at("triggerMinQuality"), Quality);
    }
    if (auto* en = dynamic_cast<Entity*>(o)) {
      loadProducts(e.at("loot"), en->loot);
      en->mapGenerated = e.at("mapGenerated").get<bool>();
      en->mapGenDensity = e.at("mapGenDensity").get<float>();
      en->basePower = e.at("basePower").get<float>();
      en->hasEnergy = e.at("hasEnergy").get<bool>();
      loadEnergy(e.at("energy"), en->energy);
      refs(e.at("sourceEntities"), en->sourceEntities);
      refs(e.at("itemsToPlace"), en->itemsToPlace);
      refs(e.at("spawnLocations"), en->spawnLocations);
      en->heatingPower = e.at("heatingPower").get<float>();
      en->hasBurntInventory = e.at("hasBurntInventory").get<bool>();
      en->width = e.at("width").get<int>();
      en->height = e.at("height").get<int>();
      en->size = e.at("size").get<int>();
      en->spoilResult = REF(e.at("spoilResult"), Entity);
      en->baseSpoilTime = e.at("baseSpoilTime").get<float>();
      en->autoplaceControl = e.at("autoplace").get<std::string>();
    }
    if (auto* em = dynamic_cast<EntityWithModules*>(o)) {
      em->allowedEffects = e.at("allowedEffects").get<uint32_t>();
      em->allowedModuleCategories =
          e.at("allowedModuleCategories").get<std::vector<std::string>>();
      em->moduleSlots = e.at("moduleSlots").get<int>();
    }
    if (auto* ec = dynamic_cast<EntityCrafter*>(o)) {
      ec->itemInputs = e.at("itemInputs").get<int>();
      ec->fluidInputs = e.at("fluidInputs").get<int>();
      refs(e.at("inputs"), ec->inputs);
      refs(e.at("recipes"), ec->recipes);
      ec->baseCraftingSpeed = e.at("craftingSpeed").get<float>();
      ec->effectReceiverBaseProductivity = e.at("erBaseProd").get<float>();
      const json& er = e.at("effectReceiver");
      ec->effectReceiver.baseEffect = {er.at("cons").get<float>(),
                                       er.at("speed").get<float>(),
                                       er.at("prod").get<float>(),
                                       er.at("pol").get<float>(),
                                       er.at("qual").get<float>()};
      ec->effectReceiver.usesModuleEffects = er.at("um").get<bool>();
      ec->effectReceiver.usesBeaconEffects = er.at("ub").get<bool>();
      ec->effectReceiver.usesSurfaceEffects = er.at("us").get<bool>();
      ec->rocketInventorySize = e.at("rocketInventorySize").get<int>();
      ec->hasVectorToPlaceResult = e.at("hasVector").get<bool>();
    }
    if (auto* er2 = dynamic_cast<EntityReactor*>(o)) {
      er2->reactorNeighborBonus = e.at("neighborBonus").get<float>();
    }
    if (auto* ea = dynamic_cast<EntityAttractor*>(o)) {
      ea->range = e.at("range").get<float>();
      ea->attractorEfficiency = e.at("attractorEfficiency").get<float>();
      ea->drain = e.at("attractorDrain").get<float>();
    }
    if (auto* eb = dynamic_cast<EntityBelt*>(o)) {
      eb->beltItemsPerSecond = e.at("beltSpeed").get<float>();
    }
    if (auto* epu = dynamic_cast<EntityPump*>(o)) {
      epu->pumpingSpeed = e.value("pumpingSpeed", 0.0f);
    }
    if (auto* ebc = dynamic_cast<EntityBeacon*>(o)) {
      ebc->beaconEfficiency = e.at("beaconEfficiency").get<float>();
      ebc->profileValues = e.at("profile").get<std::vector<float>>();
    }
    if (auto* ecn = dynamic_cast<EntityContainer*>(o)) {
      ecn->inventorySize = e.at("inventorySize").get<int>();
      ecn->logisticMode = e.at("logisticMode").get<std::string>();
      ecn->logisticSlotsCount = e.at("logisticSlots").get<int>();
    }
    if (auto* ei = dynamic_cast<EntityInserter*>(o)) {
      ei->inserterSwingTime = e.at("swingTime").get<float>();
      ei->isBulkInserter = e.at("bulk").get<bool>();
      ei->stackSizeBonus = e.value("stackBonus", 0);
      ei->maxBeltStackSize = e.value("maxBeltStack", 1);
    }
    if (auto* eac = dynamic_cast<EntityAccumulator*>(o)) {
      eac->baseAccumulatorCapacity = e.at("capacity").get<float>();
    }
    if (auto* ep = dynamic_cast<EntityProjectile*>(o)) {
      ep->placeEntities = e.at("placeEntities").get<std::vector<std::string>>();
    }
    if (auto* es = dynamic_cast<EntitySpawner*>(o)) {
      es->capturedEntityName = e.at("captured").get<std::string>();
    }
    if (auto* tl = dynamic_cast<Tile*>(o)) {
      tl->fluidResult = REF(e.at("fluidResult"), Fluid);
      refs(e.at("locations"), tl->locations);
    }
    if (auto* loc = dynamic_cast<Location*>(o)) {
      refs(e.at("technologyUnlock"), loc->technologyUnlock);
      loc->entitySpawns = e.at("entitySpawns").get<std::vector<std::string>>();
      loc->placementControls = e.at("placementControls").get<std::vector<std::string>>();
    }
    if (auto* q = dynamic_cast<Quality*>(o)) {
      q->nextQuality = REF(e.at("next"), Quality);
      q->previousQuality = REF(e.at("prev"), Quality);
      q->level = e.at("level").get<int>();
      refs(e.at("technologyUnlock"), q->technologyUnlock);
      q->BeaconConsumptionFactor = e.at("beaconConsumption").get<float>();
      q->UpgradeChance = e.at("upgradeChance").get<float>();
      // Bundles written before the Factorio 2.1 chain_probability split lack
      // this field, and which fallback is right depends on the era of the
      // GAME DATA they were built from — recoverable from upgradeChance:
      //  - 2.0 data: next_probability ~0.1 per tier; the tier's own
      //    next_probability IS the chaining rule -> chain = upgradeChance.
      //  - 2.1 data through the old bundler: next_probability = 1 was baked
      //    with no chain info; apply the engine's documented default,
      //    chain_probability = clamp(next_probability * 0.1, 0, 1).
      // The 0.5 threshold separates the two shapes (real data sits at ~0.1
      // vs exactly 1). New bundles carry the field explicitly.
      float up = q->UpgradeChance;
      q->ChainProbability = e.value(
          "chainProbability",
          up >= 0.5f ? std::min(up * 0.1f, 1.0f) : up);
    }
  }
#undef REF

  std::unordered_map<std::string, FactorioObject*> aliases;
  for (const auto& [alias, id] : dump.at("aliases").items()) {
    aliases[alias] = byId.at(id.get<int>());
  }

  auto db = std::make_unique<Database>();
  db->LoadBuiltData(std::move(owned), aliases);

  auto loadRefList = [&](const json& j, auto& out) {
    using T = std::remove_pointer_t<typename std::remove_reference_t<decltype(out)>::value_type>;
    out.clear();
    for (const json& entry : j) {
      out.push_back(dynamic_cast<T*>(byId.at(entry.get<int>())));
    }
  };
  loadRefList(dump.at("rootAccessible"), db->rootAccessible);
  loadRefList(dump.at("heatVariants"), db->heatVariants);
  db->fluidVariants.clear();
  for (const auto& [name, ids] : dump.at("fluidVariants").items()) {
    std::vector<Fluid*> list;
    loadRefList(ids, list);
    db->fluidVariants[name] = std::move(list);
  }
  db->constantCombinatorCapacity = dump.at("combinator").get<int>();
  return db;
}

// ------------------------------------------------------------------ bundle ----

FactorioObject* IconFallbackStep(FactorioObject* o) {
  if (o == nullptr) return nullptr;
  // Mechanics before Recipe: Mechanics derives from Recipe, and desktop shows
  // e.g. "mining lead rock" with the rock's icon, boiler steam with the
  // boiler's.
  if (auto* mechanics = dynamic_cast<Mechanics*>(o)) {
    if (mechanics->source != nullptr) return mechanics->source;
  }
  if (auto* recipe = dynamic_cast<RecipeOrTechnology*>(o)) {
    if (!recipe->products.empty()) return recipe->products[0].goods;
  }
  if (auto* entity = dynamic_cast<Entity*>(o)) {
    if (!entity->itemsToPlace.empty()) return entity->itemsToPlace[0];
  }
  return nullptr;
}

BundleWriteStats WriteBundle(const std::string& outPath, const Database& db,
                             const ModSet& mods, const std::string& factorioVersion,
                             const std::map<std::string, std::string>& modVersions,
                             const std::vector<float>* costs,
                             const std::map<std::string,
                                            std::map<std::string, std::string>>*
                                 locales) {
  BundleWriteStats stats;
  stats.objects = static_cast<size_t>(db.objects.count());

  std::vector<uint8_t> cbor = json::to_cbor(DumpDatabase(db));
  stats.databaseBytes = cbor.size();

  // Icons: dedupe by source path; resolve through the mods.
  std::unordered_map<std::string, std::string> fileForPath;  // icon path -> zip name
  json manifest = json::object();
  std::vector<std::pair<std::string, std::string>> iconFiles;  // zip name -> bytes
  for (const FactorioObject* o : db.objects) {
    if (o->iconSpec.empty()) continue;
    json layers = json::array();
    bool any = false;
    for (const FactorioIconPart& part : o->iconSpec) {
      auto it = fileForPath.find(part.path);
      if (it == fileForPath.end()) {
        std::string bytes;
        try {
          auto [mod, path] = mods.ResolveModPath("", part.path, false);
          bytes = mods.ReadModFile(mod, path);
        } catch (const std::exception&) {
          bytes.clear();
        }
        if (bytes.empty()) {
          stats.missingIcons++;
          fileForPath[part.path] = "";  // negative-cache
          it = fileForPath.find(part.path);
        } else {
          // Crop mip strips + downscale to the web display budget.
          bytes = DownscaleIconPng(bytes, 32);
          std::string zipName = "icons/" + std::to_string(iconFiles.size()) + ".png";
          stats.iconBytes += bytes.size();
          iconFiles.emplace_back(zipName, std::move(bytes));
          it = fileForPath.emplace(part.path, zipName).first;
        }
      }
      if (it->second.empty()) continue;
      layers.push_back(json{{"file", it->second}, {"sz", part.size}, {"x", part.x},
                            {"y", part.y}, {"r", part.r}, {"g", part.g},
                            {"b", part.b}, {"a", part.a}, {"s", part.scale}});
      any = true;
    }
    if (any) manifest[o->typeDotName()] = std::move(layers);
  }
  // Objects without their own icon inherit one along the desktop fallback
  // chain (recipe -> main product, mechanics -> source, entity -> placer);
  // bake the alias so any bundle consumer gets complete coverage.
  for (FactorioObject* o : db.objects) {
    if (manifest.contains(o->typeDotName())) continue;
    FactorioObject* source = o;
    for (int depth = 0; depth < 4; ++depth) {
      source = IconFallbackStep(source);
      if (source == nullptr) break;
      if (auto it = manifest.find(source->typeDotName()); it != manifest.end()) {
        manifest[o->typeDotName()] = *it;
        break;
      }
    }
  }
  stats.iconFiles = iconFiles.size();

  json meta{{"formatVersion", kBundleFormatVersion},
            {"factorioVersion", factorioVersion},
            {"mods", modVersions}};

  mz_zip_archive zip{};
  if (!mz_zip_writer_init_file(&zip, outPath.c_str(), 0)) {
    throw std::runtime_error("bundle: cannot create " + outPath);
  }
  auto add = [&](const char* name, const void* data, size_t size, mz_uint level) {
    if (!mz_zip_writer_add_mem(&zip, name, data, size, level)) {
      mz_zip_writer_end(&zip);
      throw std::runtime_error(std::string("bundle: failed writing ") + name);
    }
  };
  std::string metaText = meta.dump();
  std::string manifestText = manifest.dump();
  add("meta.json", metaText.data(), metaText.size(), MZ_DEFAULT_COMPRESSION);
  add("database.cbor", cbor.data(), cbor.size(), MZ_DEFAULT_COMPRESSION);
  if (costs != nullptr) {
    std::vector<uint8_t> costsCbor = json::to_cbor(json(*costs));
    add("costs.cbor", costsCbor.data(), costsCbor.size(), MZ_DEFAULT_COMPRESSION);
  }
  if (locales != nullptr) {
    for (const auto& [lang, catalog] : *locales) {
      std::string text = json(catalog).dump();
      add(("locale/" + lang + ".json").c_str(), text.data(), text.size(),
          MZ_DEFAULT_COMPRESSION);
    }
  }
  add("icons.json", manifestText.data(), manifestText.size(), MZ_DEFAULT_COMPRESSION);
  for (const auto& [name, bytes] : iconFiles) {
    add(name.c_str(), bytes.data(), bytes.size(), MZ_NO_COMPRESSION);  // PNGs
  }
  if (!mz_zip_writer_finalize_archive(&zip)) {
    mz_zip_writer_end(&zip);
    throw std::runtime_error("bundle: finalize failed");
  }
  mz_zip_writer_end(&zip);
  return stats;
}

namespace {
Bundle ReadBundleFromZip(mz_zip_archive& zip) {
  auto readEntry = [&](const std::string& name) -> std::string {
    int index = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0);
    if (index < 0) throw std::runtime_error("bundle: missing " + name);
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
    if (data == nullptr) throw std::runtime_error("bundle: cannot read " + name);
    std::string result(static_cast<char*>(data), size);
    mz_free(data);
    return result;
  };

  Bundle bundle;
  bundle.meta = json::parse(readEntry("meta.json"));
  std::string cbor = readEntry("database.cbor");
  bundle.db = LoadDatabase(
      json::from_cbor(std::vector<uint8_t>(cbor.begin(), cbor.end())));
  bundle.iconManifest = json::parse(readEntry("icons.json"));
  if (mz_zip_reader_locate_file(&zip, "costs.cbor", nullptr, 0) >= 0) {
    std::string costsBytes = readEntry("costs.cbor");
    bundle.costs = json::from_cbor(
        std::vector<uint8_t>(costsBytes.begin(), costsBytes.end()))
        .get<std::vector<float>>();
  }

  mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory) continue;
    std::string name = stat.m_filename;
    if (name.rfind("icons/", 0) == 0) bundle.iconFiles[name] = readEntry(name);
    if (name.rfind("locale/", 0) == 0 && name.size() > 12) {
      bundle.localeFiles[name.substr(7, name.size() - 12)] = readEntry(name);
    }
  }
  return bundle;
}
}  // namespace

Bundle ReadBundle(const std::string& path) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
    throw std::runtime_error("bundle: cannot open " + path);
  }
  try {
    Bundle bundle = ReadBundleFromZip(zip);
    mz_zip_reader_end(&zip);
    return bundle;
  } catch (...) {
    mz_zip_reader_end(&zip);
    throw;
  }
}

Bundle ReadBundleFromMemory(const std::string& bytes) {
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
    throw std::runtime_error("bundle: cannot open from memory");
  }
  try {
    Bundle bundle = ReadBundleFromZip(zip);
    mz_zip_reader_end(&zip);
    return bundle;
  } catch (...) {
    mz_zip_reader_end(&zip);
    throw;
  }
}

}  // namespace yafc
