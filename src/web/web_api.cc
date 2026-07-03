// The wasm-core API for the main (planning) app — milestone 1 of Phase 4.
// Runs inside a dedicated Web Worker; the page talks to it via postMessage
// (web/worker.js). The main app NEVER reads game files: it only loads bundles
// produced by the yafc-bundler (split-app contract, PLAN Phase 4.5).
//
// Interop style: JSON strings in/out (typed API can come with the TS layer);
// icon PNG bytes cross as typed arrays.
#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <unordered_set>

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include "yafc/analysis/dependencies.h"
#include "yafc/analysis/milestones.h"
#include "yafc/model/production_table.h"
#include "yafc/parser/locale.h"
#include "yafc/serialization/serialization.h"
#include "yafc/serialization/database_dump.h"

using namespace yafc;
using nlohmann::json;

namespace {

std::unique_ptr<Bundle> g_bundle;
std::unique_ptr<ProductionTable> g_table;
std::unordered_set<const Technology*> g_researched;
bool g_researchFilter = false;
// Favorite buildings/fuels/modules (user "defaults"): first favorite among a
// candidate list wins over data order when a row doesn't pin a choice.
std::unordered_set<const FactorioObject*> g_favorites;

// Milestone gating (desktop Milestones dialog). Computed lazily: the walks
// cost a few flood fills over the whole database, so tests/smokes that never
// touch milestones don't pay for them.
std::unique_ptr<Dependencies> g_deps;
std::unique_ptr<Milestones> g_milestones;
std::unordered_set<const FactorioObject*> g_unlockedMilestones;

Database& Db() { return *g_bundle->db; }

void EnsureMilestones() {
  if (g_milestones != nullptr) return;
  g_deps = std::make_unique<Dependencies>();
  g_deps->Calculate(Db());
  g_milestones = std::make_unique<Milestones>();
  MilestonesInput input;  // default milestones: science packs + locations
  input.unlockedMilestones = g_unlockedMilestones;
  g_milestones->Compute(Db(), *g_deps, input);
}

// Adds "milestone" (highest locked milestone) or "inaccessible" to a brief
// when the object is gated; no-ops until the milestone analysis has run.
void AddMilestoneInfo(json& brief, const FactorioObject* o) {
  if (g_milestones == nullptr) return;
  if (!g_milestones->IsAccessible(o)) {
    brief["inaccessible"] = true;
    return;
  }
  if (g_milestones->IsAccessibleWithCurrentMilestones(o)) return;
  if (FactorioObject* m = g_milestones->GetHighest(o, /*all=*/false)) {
    brief["milestone"] = json{{"tdn", m->typeDotName()}, {"locName", m->locName}};
  }
}

float CostOf(const FactorioObject* o) {
  if (o == nullptr || g_bundle->costs.empty()) return 0;
  if (o->id < 0 || o->id >= static_cast<int>(g_bundle->costs.size())) return 0;
  return g_bundle->costs[o->id];
}

// A recipe is available when enabled from the start or unlocked by any
// researched technology; with the research filter off, everything counts.
bool IsAvailable(const RecipeOrTechnology* r) {
  if (!g_researchFilter || r->enabled) return true;
  if (auto* recipe = dynamic_cast<const Recipe*>(r)) {
    for (const Technology* tech : recipe->technologyUnlock) {
      if (g_researched.count(tech)) return true;
    }
    return recipe->technologyUnlock.empty();
  }
  return true;
}

json GoodsBrief(const Goods* g) {
  json brief{{"tdn", g->typeDotName()}, {"name", g->name},
             {"locName", g->locName},   {"kind", std::string(g->type())}};
  AddMilestoneInfo(brief, g);
  return brief;
}

json RecipeBrief(const RecipeOrTechnology* r) {
  json in = json::array(), out = json::array();
  for (const Ingredient& i : r->ingredients) {
    in.push_back(json{{"tdn", i.goods->typeDotName()}, {"name", i.goods->locName},
                      {"amount", i.amount}});
  }
  for (const Product& p : r->products) {
    out.push_back(json{{"tdn", p.goods->typeDotName()}, {"name", p.goods->locName},
                       {"amount", p.amount}});
  }
  json unlockedBy = json::array();
  if (auto* recipe = dynamic_cast<const Recipe*>(r)) {
    for (const Technology* tech : recipe->technologyUnlock) {
      unlockedBy.push_back(tech->locName);
    }
  }
  json brief{{"tdn", r->typeDotName()}, {"name", r->name},
             {"locName", r->locName},   {"time", r->time},
             {"cost", CostOf(r)},       {"available", IsAvailable(r)},
             {"unlockedBy", std::move(unlockedBy)},
             {"in", std::move(in)},     {"out", std::move(out)}};
  AddMilestoneInfo(brief, r);
  return brief;
}

// Candidate ordering (user directive): available first, then yafc cost.
// Milestone gating stacks on top: reachable-now < research-locked <
// beyond-current-milestones < statically inaccessible.
int RecipeRank(const RecipeOrTechnology* r) {
  if (g_milestones != nullptr) {
    if (!g_milestones->IsAccessible(r)) return 3;
    if (!g_milestones->IsAccessibleWithCurrentMilestones(r)) return 2;
  }
  return IsAvailable(r) ? 0 : 1;
}

void SortRecipes(std::vector<const RecipeOrTechnology*>& list) {
  std::stable_sort(list.begin(), list.end(),
                   [](const RecipeOrTechnology* a, const RecipeOrTechnology* b) {
                     int rankA = RecipeRank(a), rankB = RecipeRank(b);
                     if (rankA != rankB) return rankA < rankB;
                     return CostOf(a) < CostOf(b);
                   });
}

std::string Err(const std::string& message) {
  return json{{"error", message}}.dump();
}

}  // namespace

// Bundle bytes arrive via wasm memory (worker mallocs + copies, then calls
// this with the pointer) — binary data must not cross embind as a JS string.
static std::string loadBundlePtr(unsigned ptr, unsigned length) {
  try {
    std::string bytes(reinterpret_cast<const char*>(static_cast<uintptr_t>(ptr)),
                      length);
    auto bundle = std::make_unique<Bundle>(ReadBundleFromMemory(bytes));
    g_bundle = std::move(bundle);
    g_table = std::make_unique<ProductionTable>();
    g_deps.reset();
    g_milestones.reset();
    g_unlockedMilestones.clear();
    g_favorites.clear();
    return json{{"objects", Db().objects.count()},
                {"recipes", Db().recipes.count()},
                {"items", Db().items.count()},
                {"meta", g_bundle->meta}}.dump();
  } catch (const std::exception& e) {
    return Err(e.what());
  }
}

static std::string searchGoods(std::string query, int limit) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  std::transform(query.begin(), query.end(), query.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  json prefix = json::array(), contains = json::array();
  for (const Goods* g : Db().goods) {
    if (!g->isLinkable || !g->showInExplorers) continue;
    std::string name = g->name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    size_t pos = name.find(query);
    if (pos == std::string::npos) continue;
    (pos == 0 ? prefix : contains).push_back(GoodsBrief(g));
    if (prefix.size() >= static_cast<size_t>(limit)) break;
  }
  for (json& e : contains) {
    if (prefix.size() >= static_cast<size_t>(limit)) break;
    prefix.push_back(std::move(e));
  }
  return prefix.dump();
}

static std::string goodsInfo(std::string tdn) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  auto* g = dynamic_cast<Goods*>(Db().FindByTypeDotName(tdn));
  if (g == nullptr) return Err("unknown goods " + tdn);
  std::vector<const RecipeOrTechnology*> production(g->production.begin(),
                                                    g->production.end());
  std::vector<const RecipeOrTechnology*> usages(g->usages.begin(), g->usages.end());
  SortRecipes(production);
  SortRecipes(usages);
  json producers = json::array(), consumers = json::array();
  for (const RecipeOrTechnology* r : production) producers.push_back(RecipeBrief(r));
  for (const RecipeOrTechnology* r : usages) consumers.push_back(RecipeBrief(r));
  return json{{"goods", GoodsBrief(g)},
              {"producers", std::move(producers)},
              {"consumers", std::move(consumers)}}.dump();
}

static void tableClear() { g_table = std::make_unique<ProductionTable>(); }

static std::string tableAddLink(std::string tdn, double amountPerMinute) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  auto* g = dynamic_cast<Goods*>(Db().FindByTypeDotName(tdn));
  if (g == nullptr) return Err("unknown goods " + tdn);
  g_table->AddLink({g, nullptr}, amountPerMinute);
  return json{{"ok", true}}.dump();
}

// First favorite in a candidate list, else the first entry (desktop default:
// data order until the user stars a preference).
template <typename T>
T* PickDefault(const std::vector<T*>& candidates) {
  for (T* c : candidates) {
    if (g_favorites.count(c)) return c;
  }
  return candidates.empty() ? nullptr : candidates[0];
}

std::vector<RecipeRowCustomModule> ParseModuleList(const json& arr) {
  std::vector<RecipeRowCustomModule> list;
  if (!arr.is_array()) return list;
  for (const json& e : arr) {
    auto* module = dynamic_cast<Module*>(Db().FindByTypeDotName(e.value("tdn", "")));
    if (module != nullptr) list.push_back({module, e.value("count", 0)});
  }
  return list;
}

json ModuleListJson(const std::vector<RecipeRowCustomModule>& list) {
  json arr = json::array();
  for (const RecipeRowCustomModule& m : list) {
    arr.push_back(json{{"tdn", m.module->typeDotName()}, {"count", m.fixedCount}});
  }
  return arr;
}

// Row configuration: {"fixed": crafts/s, "entity": tdn, "fuel": tdn,
// "modules": {"list": [{tdn,count}], "beacon": tdn, "beaconList": [...]}}.
// Empty/missing fields pick defaults: favorite-or-first crafter, its
// favorite-or-first fuel. A project's per-row choices matter: a reactor
// burning mox vs uranium cells produces different spent fuel.
static std::string tableAddRecipe(std::string tdn, std::string configJson) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  auto* r = dynamic_cast<RecipeOrTechnology*>(Db().FindByTypeDotName(tdn));
  if (r == nullptr) return Err("unknown recipe " + tdn);
  json config = json::parse(configJson, nullptr, false);
  if (config.is_discarded() || !config.is_object()) config = json::object();

  RecipeRow* row = g_table->AddRecipe(r);
  double fixed = config.value("fixed", 0.0);
  if (fixed > 0) row->fixedRate = static_cast<float>(fixed);

  auto* entity = dynamic_cast<EntityCrafter*>(
      Db().FindByTypeDotName(config.value("entity", "")));
  if (entity == nullptr) entity = PickDefault(r->crafters);
  if (entity != nullptr) row->entity = {entity, Db().qualityNormal};

  auto* fuel = dynamic_cast<Goods*>(Db().FindByTypeDotName(config.value("fuel", "")));
  if (fuel == nullptr && entity != nullptr && entity->hasEnergy) {
    fuel = PickDefault(entity->energy.fuels);
  }
  if (fuel != nullptr) row->fuel = {fuel, nullptr};

  if (const json& modules = config["modules"]; modules.is_object()) {
    row->modules.list = ParseModuleList(modules.value("list", json::array()));
    row->modules.beacon = dynamic_cast<EntityBeacon*>(
        Db().FindByTypeDotName(modules.value("beacon", "")));
    row->modules.beaconList = ParseModuleList(modules.value("beaconList", json::array()));
  }
  return json{{"ok", true}, {"recipe", RecipeBrief(r)}}.dump();
}

// Page-level module defaults, applied to rows without an explicit template.
// Input: {"fillerModule": tdn, "beacon": tdn, "beaconModule": tdn,
// "beaconsPerBuilding": n, "fillMiners": bool}.
static std::string tableSetFiller(std::string request) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  json parsed = json::parse(request, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return Err("bad request");
  ModuleFillerParameters& filler = g_table->settings.filler;
  filler = {};
  filler.fillMiners = parsed.value("fillMiners", false);
  filler.fillerModule = dynamic_cast<Module*>(
      Db().FindByTypeDotName(parsed.value("fillerModule", "")));
  filler.beacon = dynamic_cast<EntityBeacon*>(
      Db().FindByTypeDotName(parsed.value("beacon", "")));
  filler.beaconModule = dynamic_cast<Module*>(
      Db().FindByTypeDotName(parsed.value("beaconModule", "")));
  filler.beaconsPerBuilding = parsed.value("beaconsPerBuilding", 8);
  return json{{"ok", true}}.dump();
}

// Favorites double as defaults (desktop: starred objects float to the top
// and win the default pick). Input: {"favorites": [tdn...]}.
static std::string setDefaults(std::string request) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  json parsed = json::parse(request, nullptr, false);
  if (parsed.is_discarded()) return Err("bad request");
  g_favorites.clear();
  for (const auto& tdn : parsed.value("favorites", std::vector<std::string>())) {
    if (FactorioObject* obj = Db().FindByTypeDotName(tdn)) g_favorites.insert(obj);
  }
  return json{{"ok", true}, {"count", g_favorites.size()}}.dump();
}

// Everything the row-config dialog needs: crafter candidates for the recipe,
// fuel candidates for the (chosen or default) crafter, modules compatible
// with recipe+crafter, and beacons with their compatible modules.
static std::string rowOptions(std::string recipeTdn, std::string entityTdn) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  auto* r = dynamic_cast<RecipeOrTechnology*>(Db().FindByTypeDotName(recipeTdn));
  if (r == nullptr) return Err("unknown recipe " + recipeTdn);
  auto* entity = dynamic_cast<EntityCrafter*>(Db().FindByTypeDotName(entityTdn));
  if (entity == nullptr) entity = PickDefault(r->crafters);

  auto brief = [](const FactorioObject* o, json extra = json::object()) {
    extra["tdn"] = o->typeDotName();
    extra["locName"] = o->locName;
    extra["cost"] = CostOf(o);
    extra["favorite"] = g_favorites.count(o) != 0;
    return extra;
  };

  json crafters = json::array();
  for (EntityCrafter* c : r->crafters) {
    crafters.push_back(brief(c, json{{"speed", c->baseCraftingSpeed},
                                     {"moduleSlots", c->moduleSlots},
                                     {"powerMw", c->basePower},
                                     {"selected", c == entity}}));
  }

  json fuels = json::array();
  if (entity != nullptr && entity->hasEnergy) {
    for (Goods* fuel : entity->energy.fuels) {
      fuels.push_back(brief(fuel, json{{"fuelValue", fuel->fuelValue}}));
    }
  }

  const auto* recipe = dynamic_cast<const Recipe*>(r);
  auto recipeAccepts = [&](const Module* m) {
    return recipe == nullptr ||
           EntityWithModules::CanAcceptModule(
               m->moduleSpecification, recipe->allowedEffects,
               recipe->allowedModuleCategories.empty()
                   ? nullptr
                   : &recipe->allowedModuleCategories);
  };
  json modules = json::array();
  if (entity != nullptr && entity->moduleSlots > 0) {
    for (Module* m : Db().allModules) {
      if (!entity->CanAcceptModule(m->moduleSpecification) || !recipeAccepts(m)) continue;
      const ModuleSpecification& spec = m->moduleSpecification;
      modules.push_back(brief(m, json{{"speed", spec.baseSpeed},
                                      {"productivity", spec.baseProductivity},
                                      {"consumption", spec.baseConsumption}}));
    }
  }

  json beacons = json::array();
  for (EntityBeacon* b : Db().allBeacons) {
    json beaconModules = json::array();
    for (Module* m : Db().allModules) {
      if (b->CanAcceptModule(m->moduleSpecification)) {
        beaconModules.push_back(brief(m));
      }
    }
    beacons.push_back(brief(b, json{{"moduleSlots", b->moduleSlots},
                                    {"efficiency", b->beaconEfficiency},
                                    {"modules", std::move(beaconModules)}}));
  }

  return json{{"entity", entity != nullptr ? json(entity->typeDotName()) : json()},
              {"hasEnergy", entity != nullptr && entity->hasEnergy},
              {"moduleSlots", entity != nullptr ? entity->moduleSlots : 0},
              {"crafters", std::move(crafters)},
              {"fuels", std::move(fuels)},
              {"modules", std::move(modules)},
              {"beacons", std::move(beacons)}}.dump();
}

static std::string tableSolve() {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  TableSolveResult result = g_table->Solve();
  json rows = json::array();
  for (const auto& row : g_table->recipes) {
    if (row->recipe == nullptr) continue;
    json flows = json::array();
    for (const Product& p : row->recipe->products) {
      flows.push_back(json{{"tdn", p.goods->typeDotName()},
                           {"perMin", p.amount * row->recipesPerSecond}});
    }
    for (const Ingredient& i : row->recipe->ingredients) {
      flows.push_back(json{{"tdn", i.goods->typeDotName()},
                           {"perMin", -i.amount * row->recipesPerSecond}});
    }
    // Item fuels show like desktop: fuel as an ingredient, its spent form as
    // a product. Power fuels stay off the chips — the nameplate shows kW.
    if (row->fuel && !row->fuel.target->isPower()) {
      double fuelFlow =
          row->parameters.fuelUsagePerSecondPerRecipe() * row->recipesPerSecond;
      if (fuelFlow > 0) {
        flows.push_back(json{{"tdn", row->fuel.target->typeDotName()},
                             {"perMin", -fuelFlow}});
        if (Item* spent = row->fuel.target->HasSpentFuel()) {
          flows.push_back(json{{"tdn", spent->typeDotName()},
                               {"perMin", fuelFlow}});
        }
      }
    }
    json entity;
    if (row->entity.target != nullptr) {
      float perBuildingMW = row->entity.quality != nullptr
                                ? row->entity.target->Power(*row->entity.quality)
                                : row->entity.target->basePower;
      perBuildingMW *= row->parameters.activeEffects.energyUsageMod();
      entity = json{{"tdn", row->entity.target->typeDotName()},
                    {"locName", row->entity.target->locName},
                    {"powerMw", perBuildingMW}};
    }
    json usedModules = json::array();
    for (const RecipeRowCustomModule& m : row->parameters.usedModules) {
      usedModules.push_back(json{{"tdn", m.module->typeDotName()},
                                 {"locName", m.module->locName},
                                 {"count", m.fixedCount}});
    }
    json usedBeacon;
    if (row->parameters.usedBeacon != nullptr) {
      usedBeacon = json{{"tdn", row->parameters.usedBeacon->typeDotName()},
                        {"locName", row->parameters.usedBeacon->locName},
                        {"count", row->parameters.usedBeaconCount}};
    }
    rows.push_back(json{{"recipe", RecipeBrief(row->recipe)},
                        {"craftsPerMin", row->recipesPerSecond},
                        {"buildings", row->buildingCount()},
                        {"entity", std::move(entity)},
                        {"modules", std::move(usedModules)},
                        {"beacon", std::move(usedBeacon)},
                        {"warnings", row->parameters.warningFlags},
                        {"flows", std::move(flows)}});
  }
  json links = json::array();
  for (const auto& link : g_table->links) {
    links.push_back(json{{"tdn", link->goods.target->typeDotName()},
                         {"name", link->goods.target->locName},
                         {"amount", link->amount},
                         {"flow", link->linkFlow},
                         {"flags", link->flags}});
  }
  json flows = json::array();
  for (const ProductionTableFlow& flow : g_table->flow) {
    flows.push_back(json{{"tdn", flow.goods.target->typeDotName()},
                         {"name", flow.goods.target->locName},
                         {"perMin", flow.amount}});
  }
  return json{{"status", static_cast<int>(result)},
              {"rows", std::move(rows)},
              {"links", std::move(links)},
              {"flows", std::move(flows)}}.dump();
}

// Research state. Input: {"filter":bool,"techs":[tdn...]}; prerequisites are
// implied transitively. Returns the expanded set for persistence/rendering.
static std::string listLanguages() {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  json langs = json::array();
  for (const auto& [lang, _] : g_bundle->localeFiles) langs.push_back(lang);
  return langs.dump();
}

// Applies a language: reset to internal names, then English as the base
// (covers holes in partial translations), then the chosen catalog on top.
static std::string setLanguage(std::string lang) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  LocaleCatalog merged;
  auto addCatalog = [&](const std::string& code) {
    auto it = g_bundle->localeFiles.find(code);
    if (it == g_bundle->localeFiles.end()) return false;
    json parsed = json::parse(it->second, nullptr, false);
    if (parsed.is_discarded()) return false;
    for (const auto& [key, value] : parsed.items()) {
      merged[key] = value.get<std::string>();
    }
    return true;
  };
  addCatalog("en");
  bool found = lang == "en" || addCatalog(lang);
  ResetLocale(Db());
  ApplyLocale(Db(), merged);
  return json{{"ok", true}, {"applied", found ? lang : "en"}}.dump();
}

static std::string setResearch(std::string request) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  json parsed = json::parse(request, nullptr, false);
  if (parsed.is_discarded()) return Err("bad request");
  g_researchFilter = parsed.value("filter", false);
  g_researched.clear();
  std::deque<const Technology*> queue;
  for (const auto& tdn : parsed.value("techs", std::vector<std::string>())) {
    if (auto* tech = dynamic_cast<Technology*>(Db().FindByTypeDotName(tdn))) {
      queue.push_back(tech);
    }
  }
  while (!queue.empty()) {
    const Technology* tech = queue.front();
    queue.pop_front();
    if (!g_researched.insert(tech).second) continue;
    for (const Technology* prereq : tech->prerequisites) queue.push_back(prereq);
  }
  json techs = json::array();
  for (const Technology* tech : g_researched) techs.push_back(tech->typeDotName());
  return json{{"filter", g_researchFilter}, {"techs", std::move(techs)}}.dump();
}

// Milestones (desktop Milestones dialog): the current milestone set in
// discovery order, with each one's unlocked ("I have access to this") state.
static std::string milestonesJson() {
  json list = json::array();
  for (FactorioObject* m : g_milestones->currentMilestones) {
    list.push_back(json{{"tdn", m->typeDotName()},
                        {"locName", m->locName},
                        {"unlocked", g_unlockedMilestones.count(m) != 0}});
  }
  return list.dump();
}

static std::string milestonesList() {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  EnsureMilestones();
  return milestonesJson();
}

// Input: {"unlocked":[tdn...]}. Only the locked mask changes, so this is
// cheap; the accessibility walks from EnsureMilestones are reused.
static std::string setMilestones(std::string request) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  json parsed = json::parse(request, nullptr, false);
  if (parsed.is_discarded()) return Err("bad request");
  EnsureMilestones();
  g_unlockedMilestones.clear();
  for (const auto& tdn : parsed.value("unlocked", std::vector<std::string>())) {
    if (FactorioObject* obj = Db().FindByTypeDotName(tdn)) {
      g_unlockedMilestones.insert(obj);
    }
  }
  g_milestones->SetUnlocked(g_unlockedMilestones);
  return milestonesJson();
}

// Leveled families (human priority 4): "steel-mk03" / "physical-damage-4"
// group under a base name with a numeric level; level 0 = no level suffix.
static std::pair<std::string, int> TechFamily(const std::string& name) {
  size_t dash = name.rfind('-');
  if (dash == std::string::npos || dash + 1 >= name.size()) return {name, 0};
  std::string tail = name.substr(dash + 1);
  size_t digits = 0;
  if (tail.rfind("mk", 0) == 0) digits = 2;
  if (tail.find_first_not_of("0123456789", digits) != std::string::npos ||
      tail.size() == digits) {
    return {name, 0};
  }
  return {name.substr(0, dash), std::atoi(tail.c_str() + digits)};
}

static std::string searchTechs(std::string query, int limit) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  std::transform(query.begin(), query.end(), query.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  json results = json::array();
  for (const Technology* tech : Db().technologies) {
    std::string haystack = tech->name + "\x01" + tech->locName;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (haystack.find(query) == std::string::npos) continue;
    auto [family, level] = TechFamily(tech->name);
    results.push_back(json{{"tdn", tech->typeDotName()},
                           {"locName", tech->locName},
                           {"family", family},
                           {"level", level},
                           {"researched", g_researched.count(tech) != 0},
                           {"unlocks", tech->unlockRecipes.size()}});
    if (results.size() >= static_cast<size_t>(limit)) break;
  }
  return results.dump();
}

// ---- projects (human priority 1 + multi-page) ----
// Pages live in JS state; the C++ side converts between the .yafc Project
// format and per-page UI mirrors. The solve workspace holds the active page.

// pagesJson: [{name, goals:[{tdn,perMin}], linked:[tdn], rows:[{tdn}]}]
static std::string projectSaveAll(std::string pagesJson) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  json pages = json::parse(pagesJson, nullptr, false);
  if (pages.is_discarded() || !pages.is_array()) return Err("bad pages state");
  Project project;
  for (const json& p : pages) {
    auto page = std::make_unique<ProjectPage>();
    page->name = p.value("name", "Page");
    for (const json& goal : p.value("goals", json::array())) {
      auto* goods = dynamic_cast<Goods*>(
          Db().FindByTypeDotName(goal.value("tdn", "")));
      if (goods == nullptr) continue;
      // UI state is per-minute; .yafc stores per-second (desktop units).
      page->content.AddLink({goods, nullptr}, goal.value("perMin", 0.0) / 60.0);
    }
    for (const json& tdn : p.value("linked", json::array())) {
      auto* goods = dynamic_cast<Goods*>(
          Db().FindByTypeDotName(tdn.get<std::string>()));
      if (goods != nullptr) page->content.AddLink({goods, nullptr}, 0);
    }
    for (const json& row : p.value("rows", json::array())) {
      auto* recipe = dynamic_cast<RecipeOrTechnology*>(
          Db().FindByTypeDotName(row.value("tdn", "")));
      if (recipe == nullptr) continue;
      RecipeRow* added = page->content.AddRecipe(recipe);
      if (auto* fuel = dynamic_cast<Goods*>(
              Db().FindByTypeDotName(row.value("fuel", "")))) {
        added->fuel = {fuel, nullptr};
      }
      if (auto* entity = dynamic_cast<EntityCrafter*>(
              Db().FindByTypeDotName(row.value("entity", "")))) {
        added->entity = {entity, Db().qualityNormal};
      }
      if (row.contains("modules") && row["modules"].is_object()) {
        const json& modules = row["modules"];
        added->modules.list = ParseModuleList(modules.value("list", json::array()));
        added->modules.beacon = dynamic_cast<EntityBeacon*>(
            Db().FindByTypeDotName(modules.value("beacon", "")));
        added->modules.beaconList =
            ParseModuleList(modules.value("beaconList", json::array()));
      }
    }
    if (p.contains("filler") && p["filler"].is_object()) {
      const json& filler = p["filler"];
      ModuleFillerParameters& f = page->content.settings.filler;
      f.fillMiners = filler.value("fillMiners", false);
      f.fillerModule = dynamic_cast<Module*>(
          Db().FindByTypeDotName(filler.value("fillerModule", "")));
      f.beacon = dynamic_cast<EntityBeacon*>(
          Db().FindByTypeDotName(filler.value("beacon", "")));
      f.beaconModule = dynamic_cast<Module*>(
          Db().FindByTypeDotName(filler.value("beaconModule", "")));
      f.beaconsPerBuilding = filler.value("beaconsPerBuilding", 8);
    }
    project.pages.push_back(std::move(page));
  }
  return SaveProjectToString(project, /*indent=*/0);
}

// Returns every page as a UI mirror; nested subgroups are flattened.
static std::string projectLoad(std::string text) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  LoadResult loaded = LoadProjectFromString(text, Db());
  if (loaded.project == nullptr || loaded.project->pages.empty()) {
    return Err("not a yafc project (no pages)");
  }
  json pages = json::array();
  for (const auto& pagePtr : loaded.project->pages) {
    ProductionTable& source = pagePtr->content;
    json goals = json::array(), linked = json::array(), rows = json::array();
    for (const auto& link : source.links) {
      if (link->goods.target == nullptr) continue;
      if (link->amount != 0) {
        goals.push_back(json{{"tdn", link->goods.target->typeDotName()},
                             {"name", link->goods.target->locName},
                             {"perMin", link->amount * 60}});
      } else {
        linked.push_back(link->goods.target->typeDotName());
      }
    }
    std::vector<RecipeRow*> allRows;
    source.GetAllRecipes(allRows);
    for (const RecipeRow* row : allRows) {
      if (row->recipe == nullptr) continue;
      json rowJson{{"tdn", row->recipe->typeDotName()}};
      if (row->fuel) rowJson["fuel"] = row->fuel.target->typeDotName();
      if (row->entity.target != nullptr) {
        rowJson["entity"] = row->entity.target->typeDotName();
      }
      if (!row->modules.empty()) {
        json modules{{"list", ModuleListJson(row->modules.list)},
                     {"beaconList", ModuleListJson(row->modules.beaconList)}};
        if (row->modules.beacon != nullptr) {
          modules["beacon"] = row->modules.beacon->typeDotName();
        }
        rowJson["modules"] = std::move(modules);
      }
      rows.push_back(std::move(rowJson));
    }
    json pageJson{{"name", pagePtr->name},
                  {"goals", std::move(goals)},
                  {"linked", std::move(linked)},
                  {"rows", std::move(rows)}};
    const ModuleFillerParameters& f = source.settings.filler;
    if (f.fillerModule != nullptr || f.beacon != nullptr || f.fillMiners) {
      json filler{{"fillMiners", f.fillMiners},
                  {"beaconsPerBuilding", f.beaconsPerBuilding}};
      if (f.fillerModule != nullptr) filler["fillerModule"] = f.fillerModule->typeDotName();
      if (f.beacon != nullptr) filler["beacon"] = f.beacon->typeDotName();
      if (f.beaconModule != nullptr) filler["beaconModule"] = f.beaconModule->typeDotName();
      pageJson["filler"] = std::move(filler);
    }
    pages.push_back(std::move(pageJson));
  }
  json errors = json::array();
  for (const std::string& error : loaded.errors) errors.push_back(error);
  return json{{"pages", std::move(pages)}, {"errors", std::move(errors)}}.dump();
}

static std::string iconLayers(std::string tdn) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  if (!g_bundle->iconManifest.contains(tdn)) return "[]";
  return g_bundle->iconManifest[tdn].dump();
}

static emscripten::val iconFile(std::string file) {
  if (g_bundle == nullptr) return emscripten::val::null();
  auto it = g_bundle->iconFiles.find(file);
  if (it == g_bundle->iconFiles.end()) return emscripten::val::null();
  return emscripten::val(emscripten::typed_memory_view(it->second.size(),
      reinterpret_cast<const uint8_t*>(it->second.data())));
}

// Share-link compression fallback (zlib format, wire-compatible with the
// page's CompressionStream('deflate') path) for browsers without the
// Compression Streams API. Static buffers back the returned views.
static std::string g_zlibBuffer;

static emscripten::val zlibDeflate(unsigned ptr, unsigned length) {
  const auto* src = reinterpret_cast<const unsigned char*>(
      static_cast<uintptr_t>(ptr));
  mz_ulong bound = mz_compressBound(length);
  g_zlibBuffer.resize(bound);
  mz_ulong outLength = bound;
  if (mz_compress2(reinterpret_cast<unsigned char*>(g_zlibBuffer.data()),
                   &outLength, src, length, MZ_BEST_COMPRESSION) != MZ_OK) {
    return emscripten::val::null();
  }
  return emscripten::val(emscripten::typed_memory_view(
      outLength, reinterpret_cast<const uint8_t*>(g_zlibBuffer.data())));
}

static emscripten::val zlibInflate(unsigned ptr, unsigned length) {
  const auto* src = reinterpret_cast<const unsigned char*>(
      static_cast<uintptr_t>(ptr));
  for (mz_ulong capacity = std::max(length * 8u, 64u * 1024u);
       capacity <= 256u * 1024u * 1024u; capacity *= 4) {
    g_zlibBuffer.resize(capacity);
    mz_ulong outLength = capacity;
    int rc = mz_uncompress(reinterpret_cast<unsigned char*>(g_zlibBuffer.data()),
                           &outLength, src, length);
    if (rc == MZ_OK) {
      return emscripten::val(emscripten::typed_memory_view(
          outLength, reinterpret_cast<const uint8_t*>(g_zlibBuffer.data())));
    }
    if (rc != MZ_BUF_ERROR) break;  // grow only on insufficient buffer
  }
  return emscripten::val::null();
}

EMSCRIPTEN_BINDINGS(yafc_web) {
  emscripten::function("zlibDeflate", &zlibDeflate);
  emscripten::function("zlibInflate", &zlibInflate);
  emscripten::function("loadBundlePtr", &loadBundlePtr);
  emscripten::function("searchGoods", &searchGoods);
  emscripten::function("goodsInfo", &goodsInfo);
  emscripten::function("tableClear", &tableClear);
  emscripten::function("tableAddLink", &tableAddLink);
  emscripten::function("tableAddRecipe", &tableAddRecipe);
  emscripten::function("tableSetFiller", &tableSetFiller);
  emscripten::function("setDefaults", &setDefaults);
  emscripten::function("rowOptions", &rowOptions);
  emscripten::function("tableSolve", &tableSolve);
  emscripten::function("projectSaveAll", &projectSaveAll);
  emscripten::function("projectLoad", &projectLoad);
  emscripten::function("listLanguages", &listLanguages);
  emscripten::function("setLanguage", &setLanguage);
  emscripten::function("setResearch", &setResearch);
  emscripten::function("searchTechs", &searchTechs);
  emscripten::function("milestonesList", &milestonesList);
  emscripten::function("setMilestones", &setMilestones);
  emscripten::function("iconLayers", &iconLayers);
  emscripten::function("iconFile", &iconFile);
}
