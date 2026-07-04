// Port of the hierarchical model side of Yafc.Model/Model/ProductionTable.cs
// and ProductionTableContent.cs: ProductionTable (nested tables), RecipeRow,
// ProductionLink, link resolution up the hierarchy, Setup/flatten into the
// flat solve core, result write-back, CalculateFlow rollup and built-count
// checks.
//
// Simplified relative to upstream (arrives with later phases):
//  - RecipeParameters is a plain data seam; CalculateParameters (crafting
//    speed, modules, beacons, fuel selection) is not ported yet — callers set
//    recipeTime/productivity/fuelUsagePerSecondPerRecipe directly.
//  - Science-pack quality decomposition in Setup (quality science packs).
//  - The low-throughput flow display filter (shouldBeReported).
#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "yafc/model/data_classes.h"
#include "yafc/model/production_table_solver.h"
#include "yafc/model/recipe_parameters.h"

namespace yafc {

class ProductionTable;
class RecipeRow;

struct QualityGoods {
  Goods* target = nullptr;
  Quality* quality = nullptr;  // nullptr = no quality dimension (yet)

  bool operator==(const QualityGoods&) const = default;
  explicit operator bool() const { return target != nullptr; }
};

struct QualityGoodsHash {
  size_t operator()(const QualityGoods& g) const {
    return std::hash<const void*>()(g.target) * 31 ^ std::hash<const void*>()(g.quality);
  }
};

class ProductionLink {
 public:
  ProductionTable* owner = nullptr;
  QualityGoods goods;
  double amount = 0;  // demand (desired production)
  LinkAlgorithm algorithm = LinkAlgorithm::Match;
  // |goods.Cost()| for the slack fallback; fill from CostAnalysis when available.
  double goodsCost = 1.0;

  // Solve results:
  uint32_t flags = 0;         // LinkFlags
  double notMatchedFlow = 0;  // upstream semantics (slack solution values)
  double linkFlow = 0;        // total production through this link
};

struct ProductionTableFlow {
  QualityGoods goods;
  double amount = 0;  // production - consumption
  ProductionLink* link = nullptr;
};

class RecipeRow {
 public:
  ProductionTable* owner = nullptr;
  RecipeOrTechnology* recipe = nullptr;
  bool enabled = true;
  bool hierarchyEnabled = false;  // set by Setup
  float fixedBuildings = 0;       // >0 pins the rate at fixedBuildings/recipeTime
  float fixedRate = 0;            // >0 pins crafts/second directly (web pinning)
  std::optional<float> builtBuildings;
  ObjectWithQuality<EntityCrafter> entity;  // chosen crafter (null = unspecified)
  double baseCost = 1.0;  // upstream: recipe.RecipeBaseCost() from CostAnalysis
  QualityGoods fuel;      // optional; consumes fuelUsagePerSecondPerRecipe per craft
  // This row's target/floor quality tier (upstream RecipeRow.recipe.quality):
  // nullptr = unset, resolved to ProductionSettings::qualityNormal by
  // RecipeParameters::Calculate. Ingredients are consumed at this tier;
  // products are produced starting at this tier, spread upward by quality-
  // module upgrade chance (see RecipeParameters::quality/qualityNormal).
  Quality* quality = nullptr;
  ModuleTemplate modules;  // explicit module/beacon config (empty = filler defaults)
  RecipeParameters parameters;
  std::unique_ptr<ProductionTable> subgroup;  // nested table (this row is its header)

  // User-pinned fluid/temperature variants (upstream RecipeRow.variants):
  // when an ingredient has multiple qualifying variants (e.g. an ingredient
  // requiring coke-oven-gas at >=100 degrees can be satisfied by any of the
  // @100/@250/@500 goods), a row can pin which concrete one it actually
  // consumes. Untouched ingredients keep the parser's default (the coldest
  // qualifying variant, Ingredient::goods). Plain vector (matches
  // Ingredient::variants' type) so it reuses the ref-list Prop() overload
  // for .yafc (de)serialization as-is.
  std::vector<Goods*> variants;

  // Which concrete Goods this row actually consumes for `ingredient`: its
  // pinned variant if one of the qualifying options was chosen, otherwise
  // the parser's default (upstream RecipeRow.GetVariant).
  Goods* ResolveIngredient(const Ingredient& ingredient) const;

  // Solve results:
  double recipesPerSecond = 0;
  float buildingCount() const {
    return static_cast<float>(recipesPerSecond * parameters.recipeTime);
  }

  bool FindLink(const QualityGoods& goods, ProductionLink** link) const;

  // Per-(goods,quality) product/ingredient/fuel flows at this row's solved
  // rate, with productivity and (when quality modules are active) the
  // quality-upgrade distribution already applied — the same math Solve()
  // feeds to the LP and CalculateFlow uses for reporting, exposed so callers
  // (the web API's per-row nameplate display) don't recompute it themselves.
  // Products positive, ingredients/fuel negative; the spent form of a fuel
  // with HasSpentFuel is included as a positive entry.
  struct DisplayFlow {
    QualityGoods goods;
    double perSecond = 0;
  };
  std::vector<DisplayFlow> DisplayFlows() const;
};

class ProductionTable {
 public:
  explicit ProductionTable(RecipeRow* owner = nullptr) : owner(owner) {}

  RecipeRow* owner;  // header row; nullptr at the root table
  std::vector<std::unique_ptr<RecipeRow>> recipes;
  std::vector<std::unique_ptr<ProductionLink>> links;

  // Results:
  std::vector<ProductionTableFlow> flow;
  bool containsDesiredProducts = false;
  bool builtCountExceeded = false;  // root only; upstream returns a message

  RecipeRow* AddRecipe(RecipeOrTechnology* recipe);
  // Adds a nested table under a new (recipe-less unless given) header row.
  RecipeRow* AddNestedTable(RecipeOrTechnology* headerRecipe = nullptr);
  ProductionLink* AddLink(QualityGoods goods, double amount = 0);

  void RebuildLinkMap();
  bool FindLink(const QualityGoods& goods, ProductionLink** link) const;

  // Depth-first recipe listing (upstream GetAllRecipes).
  void GetAllRecipes(std::vector<RecipeRow*>& out) const;

  // Parameter context (mining/research productivity, tech levels); read from
  // the ROOT table during Setup for every row in the tree.
  ProductionSettings settings;

  // Flatten + solve + write back + CalculateFlow + built-count check.
  // Call on the root table.
  TableSolveResult Solve();

 private:
  void Setup(std::vector<RecipeRow*>& allRecipes, std::vector<ProductionLink*>& allLinks,
             const ProductionSettings& settings);
  void CalculateFlow(RecipeRow* include);
  bool CheckBuiltCountExceeded();

  std::unordered_map<QualityGoods, ProductionLink*, QualityGoodsHash> linkMap_;
};

}  // namespace yafc
