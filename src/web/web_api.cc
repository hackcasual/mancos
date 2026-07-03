// The wasm-core API for the main (planning) app — milestone 1 of Phase 4.
// Runs inside a dedicated Web Worker; the page talks to it via postMessage
// (web/worker.js). The main app NEVER reads game files: it only loads bundles
// produced by the yafc-bundler (split-app contract, PLAN Phase 4.5).
//
// Interop style: JSON strings in/out (typed API can come with the TS layer);
// icon PNG bytes cross as typed arrays.
#include <algorithm>
#include <memory>
#include <string>

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <nlohmann/json.hpp>

#include "yafc/model/production_table.h"
#include "yafc/serialization/database_dump.h"

using namespace yafc;
using nlohmann::json;

namespace {

std::unique_ptr<Bundle> g_bundle;
std::unique_ptr<ProductionTable> g_table;

Database& Db() { return *g_bundle->db; }

json GoodsBrief(const Goods* g) {
  return json{{"tdn", g->typeDotName()}, {"name", g->name},
              {"locName", g->locName},   {"kind", std::string(g->type())}};
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
  return json{{"tdn", r->typeDotName()}, {"name", r->name},
              {"locName", r->locName},   {"time", r->time},
              {"in", std::move(in)},     {"out", std::move(out)}};
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
  json producers = json::array(), consumers = json::array();
  for (const Recipe* r : g->production) producers.push_back(RecipeBrief(r));
  for (const Recipe* r : g->usages) consumers.push_back(RecipeBrief(r));
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

static std::string tableAddRecipe(std::string tdn) {
  if (g_bundle == nullptr) return Err("no bundle loaded");
  auto* r = dynamic_cast<RecipeOrTechnology*>(Db().FindByTypeDotName(tdn));
  if (r == nullptr) return Err("unknown recipe " + tdn);
  g_table->AddRecipe(r);
  return json{{"ok", true}, {"recipe", RecipeBrief(r)}}.dump();
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
    rows.push_back(json{{"recipe", RecipeBrief(row->recipe)},
                        {"craftsPerMin", row->recipesPerSecond},
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

EMSCRIPTEN_BINDINGS(yafc_web) {
  emscripten::function("loadBundlePtr", &loadBundlePtr);
  emscripten::function("searchGoods", &searchGoods);
  emscripten::function("goodsInfo", &goodsInfo);
  emscripten::function("tableClear", &tableClear);
  emscripten::function("tableAddLink", &tableAddLink);
  emscripten::function("tableAddRecipe", &tableAddRecipe);
  emscripten::function("tableSolve", &tableSolve);
  emscripten::function("iconLayers", &iconLayers);
  emscripten::function("iconFile", &iconFile);
}
