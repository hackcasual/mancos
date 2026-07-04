#include "yafc/model/production_table.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace yafc {

namespace {

// Upstream ProductionTableContent.BuildProducts's upgradeProbabilities: with
// quality modules active, a craft at `base` quality has a shrinking chance to
// land one tier higher, compounding up the quality chain. Returns "exactly
// this tier" fractions of the recipe's base (Normal-quality) output amount,
// paired with the quality each fraction lands at. Without quality modules
// (qualityMod <= 0) the whole amount lands at `base` (a row can still target
// a non-Normal floor with no modules — 100% at that exact tier, no spread).
//
// Chance model (covers both Factorio data eras — see Quality field docs):
//   P(>= tier 1)   = qualityMod x next_probability(base)
//   P(>= tier k+1) = P(>= tier k) x ChainProbability(tier k)
// where ChainProbability is 2.1's chain_probability, or (for 2.0 data) the
// reached tier's own next_probability — the 2.0 chaining rule.
//
// Milestone-gating the top of this walk (upstream Quality.MaxAccessible) is
// not ported yet: this walks the full quality chain the loaded data defines.
std::vector<std::pair<Quality*, double>> QualityDistribution(Quality* base, double qualityMod) {
  std::vector<double> probabilityAtLeast{1.0};
  if (qualityMod > 0 && base != nullptr) {
    double running = 1.0;
    for (Quality* q = base; q->nextQuality != nullptr; q = q->nextQuality) {
      running *= q == base ? q->UpgradeChance : q->ChainProbability;
      probabilityAtLeast.push_back(std::min(1.0, running * qualityMod));
    }
    for (size_t j = 0; j + 1 < probabilityAtLeast.size(); ++j) {
      probabilityAtLeast[j] -= probabilityAtLeast[j + 1];
    }
  }
  std::vector<std::pair<Quality*, double>> out;
  Quality* q = base;
  for (double fraction : probabilityAtLeast) {
    out.push_back({q, fraction});
    if (q != nullptr) q = q->nextQuality;
  }
  return out;
}

// Per-product quality spread for one row: fluids/specials are Normal-only;
// otherwise the upgrade-chance distribution starting at the row's target
// quality. `qualityNormal`/`quality` are both nullptr for callers that never
// wired ProductionSettings::qualityNormal — preserves the pre-quality-
// threading behavior (single untagged-quality resolution) exactly.
std::vector<std::pair<Quality*, double>> ProductQualitySpread(const Goods* goods,
                                                              const RecipeParameters& params) {
  if (!goods->AcceptsQuality() || params.quality == nullptr) {
    return {{params.qualityNormal, 1.0}};
  }
  return QualityDistribution(params.quality, params.activeEffects.qualityMod());
}

// Ingredients don't spread across qualities — a row consumes one concrete
// tier per ingredient (its target quality, or Normal if the goods can't
// carry quality at all).
Quality* IngredientQuality(const Goods* goods, const RecipeParameters& params) {
  if (!goods->AcceptsQuality()) return params.qualityNormal;
  return params.quality != nullptr ? params.quality : params.qualityNormal;
}

}  // namespace

Goods* RecipeRow::ResolveIngredient(const Ingredient& ingredient) const {
  for (Goods* option : ingredient.variants) {
    if (std::find(variants.begin(), variants.end(), option) != variants.end()) {
      return option;
    }
  }
  return ingredient.goods;
}

bool RecipeRow::FindLink(const QualityGoods& goods, ProductionLink** link) const {
  // Upstream linkRoot = subgroup ?? owner: a nested table's header row
  // resolves its links against the nested table first.
  const ProductionTable* root = subgroup != nullptr ? subgroup.get() : owner;
  return root != nullptr && root->FindLink(goods, link);
}

std::vector<RecipeRow::DisplayFlow> RecipeRow::DisplayFlows() const {
  std::vector<DisplayFlow> out;
  if (recipe == nullptr) return out;
  for (const Product& product : recipe->products) {
    double baseAmount = product.GetAmountPerRecipe(parameters.productivity()) * recipesPerSecond;
    if (baseAmount <= 0) continue;
    for (const auto& [quality, fraction] : ProductQualitySpread(product.goods, parameters)) {
      double amount = baseAmount * fraction;
      if (amount > 0) out.push_back({{product.goods, quality}, amount});
    }
  }
  for (const Ingredient& ingredient : recipe->ingredients) {
    Goods* goods = ResolveIngredient(ingredient);
    Quality* quality = IngredientQuality(goods, parameters);
    out.push_back({{goods, quality}, -ingredient.amount * recipesPerSecond});
  }
  if (fuel && !fuel.target->isPower()) {
    double fuelFlow = parameters.fuelUsagePerSecondPerRecipe() * recipesPerSecond;
    if (fuelFlow > 0) {
      out.push_back({{fuel.target, fuel.quality}, -fuelFlow});
      if (Item* spent = fuel.target->HasSpentFuel()) {
        out.push_back({{spent, fuel.quality}, fuelFlow});
      }
    }
  }
  return out;
}

RecipeRow* ProductionTable::AddRecipe(RecipeOrTechnology* recipe) {
  auto row = std::make_unique<RecipeRow>();
  row->owner = this;
  row->recipe = recipe;
  recipes.push_back(std::move(row));
  return recipes.back().get();
}

RecipeRow* ProductionTable::AddNestedTable(RecipeOrTechnology* headerRecipe) {
  RecipeRow* row = AddRecipe(headerRecipe);
  row->subgroup = std::make_unique<ProductionTable>(row);
  return row;
}

ProductionLink* ProductionTable::AddLink(QualityGoods goods, double amount) {
  auto link = std::make_unique<ProductionLink>();
  link->owner = this;
  link->goods = goods;
  link->amount = amount;
  links.push_back(std::move(link));
  return links.back().get();
}

void ProductionTable::RebuildLinkMap() {
  linkMap_.clear();
  for (const auto& link : links) linkMap_[link->goods] = link.get();
}

bool ProductionTable::FindLink(const QualityGoods& goods, ProductionLink** link) const {
  // Upstream: search this table's links, then walk up the owner chain.
  const ProductionTable* searchFrom = this;
  while (true) {
    auto it = searchFrom->linkMap_.find(goods);
    if (it != searchFrom->linkMap_.end()) {
      *link = it->second;
      return true;
    }
    if (searchFrom->owner == nullptr) {
      *link = nullptr;
      return false;
    }
    searchFrom = searchFrom->owner->owner;
  }
}

void ProductionTable::GetAllRecipes(std::vector<RecipeRow*>& out) const {
  for (const auto& row : recipes) {
    out.push_back(row.get());
    if (row->subgroup) row->subgroup->GetAllRecipes(out);
  }
}

void ProductionTable::Setup(std::vector<RecipeRow*>& allRecipes,
                            std::vector<ProductionLink*>& allLinks,
                            const ProductionSettings& settings) {
  containsDesiredProducts = false;
  RebuildLinkMap();
  for (const auto& link : links) {
    if (link->amount != 0) containsDesiredProducts = true;
    allLinks.push_back(link.get());
  }

  // The header row of a nested table is analyzed with its children, so its
  // ingredients/products resolve against the nested table's links first.
  std::vector<RecipeRow*> rows;
  if (owner != nullptr) rows.push_back(owner);
  for (const auto& row : recipes) rows.push_back(row.get());

  for (RecipeRow* row : rows) {
    if (!row->enabled) {
      row->recipesPerSecond = 0;
      row->hierarchyEnabled = false;
      continue;
    }
    row->parameters = RecipeParameters::Calculate(*row, settings);
    row->hierarchyEnabled = true;
    if (row->subgroup != nullptr && row != owner) {
      row->subgroup->Setup(allRecipes, allLinks, settings);
    } else if (row->recipe != nullptr) {
      allRecipes.push_back(row);
    }
    // TODO(port): science-pack quality decomposition (upstream Setup).
  }
}

TableSolveResult ProductionTable::Solve() {
  std::vector<RecipeRow*> allRows;
  std::vector<ProductionLink*> allLinks;
  Setup(allRows, allLinks, settings);

  std::unordered_map<const ProductionLink*, int> linkIndex;
  for (size_t i = 0; i < allLinks.size(); ++i) {
    linkIndex[allLinks[i]] = static_cast<int>(i);
  }

  std::vector<SolverLink> solverLinks(allLinks.size());
  for (size_t i = 0; i < allLinks.size(); ++i) {
    const ProductionLink& link = *allLinks[i];
    solverLinks[i] = {.name = link.goods.target->name,
                      .amount = link.amount,
                      .algorithm = link.algorithm,
                      .goods_cost = link.goodsCost};
  }

  auto resolve = [&](const RecipeRow* row, Goods* goods, Quality* quality) {
    ProductionLink* link = nullptr;
    if (goods != nullptr) row->FindLink({goods, quality}, &link);
    return link != nullptr ? linkIndex[link] : -1;
  };

  std::vector<SolverRecipe> solverRecipes(allRows.size());
  for (size_t i = 0; i < allRows.size(); ++i) {
    RecipeRow* row = allRows[i];
    SolverRecipe& sr = solverRecipes[i];
    sr.name = row->recipe->name;
    sr.base_cost = row->baseCost;
    if (row->fixedRate > 0) {
      sr.fixed_rps = row->fixedRate;  // direct crafts/second pin (web)
    } else if (row->fixedBuildings > 0 && row->parameters.recipeTime > 0) {
      sr.fixed_rps = row->fixedBuildings / row->parameters.recipeTime;
    }
    for (const Product& product : row->recipe->products) {
      double baseAmount = product.GetAmountPerRecipe(row->parameters.productivity());
      if (baseAmount <= 0) continue;
      for (const auto& [quality, fraction] : ProductQualitySpread(product.goods, row->parameters)) {
        double amount = baseAmount * fraction;
        if (amount <= 0) continue;
        sr.products.push_back({resolve(row, product.goods, quality), amount});
      }
    }
    for (const Ingredient& ingredient : row->recipe->ingredients) {
      Goods* goods = row->ResolveIngredient(ingredient);
      Quality* quality = IngredientQuality(goods, row->parameters);
      sr.ingredients.push_back({resolve(row, goods, quality), ingredient.amount});
    }
    if (row->fuel) {
      ProductionLink* link = nullptr;
      row->FindLink(row->fuel, &link);
      double fuelPerRecipe = row->parameters.fuelUsagePerSecondPerRecipe();
      sr.ingredients.push_back({link != nullptr ? linkIndex[link] : -1,
                                fuelPerRecipe});
      // Burning fuel yields its spent form 1:1 (upstream HasSpentFuel), e.g.
      // fuel cells -> used-up cells; closes nuclear reprocessing loops. The
      // spent item keeps the fuel's own quality tier (upstream FuelResult).
      if (Item* spent = row->fuel.target->HasSpentFuel(); spent && fuelPerRecipe > 0) {
        sr.products.push_back({resolve(row, spent, row->fuel.quality), fuelPerRecipe});
      }
    }
  }

  TableSolveResult result = SolveProductionTable(solverRecipes, solverLinks);
  if (result != TableSolveResult::Ok) return result;

  for (size_t i = 0; i < allRows.size(); ++i) {
    allRows[i]->recipesPerSecond = solverRecipes[i].recipes_per_second;
    allRows[i]->parameters.warningFlags |= solverRecipes[i].warning_flags;
  }
  for (size_t i = 0; i < allLinks.size(); ++i) {
    allLinks[i]->flags = solverLinks[i].flags;
    allLinks[i]->notMatchedFlow = solverLinks[i].not_matched_flow;
  }

  builtCountExceeded = CheckBuiltCountExceeded();
  CalculateFlow(nullptr);
  return result;
}

namespace {

// Upstream AddFlow: per-(goods,quality) production/consumption sums for one
// row, at its solved rate — same productivity + quality-spread math as
// Solve()'s LP inputs (see QualityDistribution/ProductQualitySpread above).
void AddFlow(const RecipeRow& row,
             std::map<std::pair<Goods*, Quality*>, std::pair<double, double>>& summer) {
  for (const Product& product : row.recipe->products) {
    double baseAmount =
        product.GetAmountPerRecipe(row.parameters.productivity()) * row.recipesPerSecond;
    if (baseAmount <= 0) continue;
    for (const auto& [quality, fraction] : ProductQualitySpread(product.goods, row.parameters)) {
      double amount = baseAmount * fraction;
      if (amount <= 0) continue;
      summer[{product.goods, quality}].first += amount;
    }
  }
  for (const Ingredient& ingredient : row.recipe->ingredients) {
    Goods* goods = row.ResolveIngredient(ingredient);
    Quality* quality = IngredientQuality(goods, row.parameters);
    summer[{goods, quality}].second += ingredient.amount * row.recipesPerSecond;
  }
  if (row.fuel) {
    double fuelFlow = row.parameters.fuelUsagePerSecondPerRecipe() * row.recipesPerSecond;
    auto& [prod, cons] = summer[{row.fuel.target, row.fuel.quality}];
    cons += fuelFlow;
    if (Item* spent = row.fuel.target->HasSpentFuel()) {
      summer[{spent, row.fuel.quality}].first += fuelFlow;
    }
  }
}

}  // namespace

void ProductionTable::CalculateFlow(RecipeRow* include) {
  std::map<std::pair<Goods*, Quality*>, std::pair<double, double>> flowDict;
  if (include != nullptr && include->recipe != nullptr) AddFlow(*include, flowDict);

  for (const auto& row : recipes) {
    if (!row->enabled) continue;
    if (row->subgroup != nullptr) {
      row->subgroup->CalculateFlow(row.get());
      for (const ProductionTableFlow& elem : row->subgroup->flow) {
        auto& [prod, cons] = flowDict[{elem.goods.target, elem.goods.quality}];
        if (elem.amount > 0) {
          prod += elem.amount;
        } else {
          cons -= elem.amount;
        }
      }
    } else if (row->recipe != nullptr) {
      AddFlow(*row, flowDict);
    }
  }

  for (const auto& link : links) {
    std::pair<double, double> flowParams{0, 0};
    auto it = flowDict.find({link->goods.target, link->goods.quality});
    if (!(link->flags & LinkFlags::kLinkNotMatched)) {
      if (it != flowDict.end()) {
        flowParams = it->second;
        flowDict.erase(it);
      }
    } else {
      if (it != flowDict.end()) flowParams = it->second;
      // A broken link inside a nested table taints the matching parent link.
      if (std::abs(flowParams.first - flowParams.second) > 1e-8 && owner != nullptr) {
        ProductionLink* parent = nullptr;
        if (owner->owner != nullptr && owner->owner->FindLink(link->goods, &parent) &&
            parent != link.get()) {
          parent->flags |= LinkFlags::kChildNotMatched | LinkFlags::kLinkNotMatched;
        }
      }
    }
    link->linkFlow = flowParams.first;
  }

  flow.clear();
  for (const auto& [goods, prodCons] : flowDict) {
    ProductionLink* link = nullptr;
    FindLink({goods.first, goods.second}, &link);
    flow.push_back({{goods.first, goods.second}, prodCons.first - prodCons.second, link});
  }
  // Deterministic order (upstream sorts with a goods comparer).
  std::sort(flow.begin(), flow.end(), [](const auto& a, const auto& b) {
    return a.goods.target->id < b.goods.target->id;
  });
  // TODO(port): shouldBeReported low-throughput filter.
}

bool ProductionTable::CheckBuiltCountExceeded() {
  bool exceeded = false;
  for (const auto& row : recipes) {
    if (row->builtBuildings.has_value() && row->buildingCount() > *row->builtBuildings) {
      row->parameters.warningFlags |= RecipeWarningFlags::kExceedsBuiltCount;
      exceeded = true;
    } else if (row->subgroup != nullptr && row->subgroup->CheckBuiltCountExceeded()) {
      row->parameters.warningFlags |= RecipeWarningFlags::kExceedsBuiltCount;
      exceeded = true;
    }
  }
  return exceeded;
}

}  // namespace yafc
