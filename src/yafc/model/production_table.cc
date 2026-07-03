#include "yafc/model/production_table.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace yafc {

bool RecipeRow::FindLink(const QualityGoods& goods, ProductionLink** link) const {
  // Upstream linkRoot = subgroup ?? owner: a nested table's header row
  // resolves its links against the nested table first.
  const ProductionTable* root = subgroup != nullptr ? subgroup.get() : owner;
  return root != nullptr && root->FindLink(goods, link);
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
                            std::vector<ProductionLink*>& allLinks) {
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
    // Upstream: recipe.parameters = RecipeParameters.CalculateParameters(recipe)
    // — not ported yet; parameters is caller-supplied data (see header).
    row->hierarchyEnabled = true;
    if (row->subgroup != nullptr && row != owner) {
      row->subgroup->Setup(allRecipes, allLinks);
    } else if (row->recipe != nullptr) {
      allRecipes.push_back(row);
    }
    // TODO(port): science-pack quality decomposition (upstream Setup).
  }
}

TableSolveResult ProductionTable::Solve() {
  std::vector<RecipeRow*> allRows;
  std::vector<ProductionLink*> allLinks;
  Setup(allRows, allLinks);

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

  // Quality is not threaded through recipes yet: links resolve on plain goods.
  auto resolve = [&](const RecipeRow* row, Goods* goods) {
    ProductionLink* link = nullptr;
    if (goods != nullptr) row->FindLink({goods, nullptr}, &link);
    return link != nullptr ? linkIndex[link] : -1;
  };

  std::vector<SolverRecipe> solverRecipes(allRows.size());
  for (size_t i = 0; i < allRows.size(); ++i) {
    RecipeRow* row = allRows[i];
    SolverRecipe& sr = solverRecipes[i];
    sr.name = row->recipe->name;
    sr.base_cost = row->baseCost;
    if (row->fixedBuildings > 0) {
      sr.fixed_rps = row->fixedBuildings / row->parameters.recipeTime;
    }
    for (const Product& product : row->recipe->products) {
      double amount = product.GetAmountPerRecipe(row->parameters.productivity);
      if (amount <= 0) continue;
      sr.products.push_back({resolve(row, product.goods), amount});
    }
    for (const Ingredient& ingredient : row->recipe->ingredients) {
      sr.ingredients.push_back({resolve(row, ingredient.goods), ingredient.amount});
    }
    if (row->fuel) {
      ProductionLink* link = nullptr;
      row->FindLink(row->fuel, &link);
      sr.ingredients.push_back({link != nullptr ? linkIndex[link] : -1,
                                row->parameters.fuelUsagePerSecondPerRecipe});
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

// Upstream AddFlow: per-goods production/consumption sums for one row.
void AddFlow(const RecipeRow& row,
             std::map<std::pair<Goods*, Quality*>, std::pair<double, double>>& summer) {
  for (const Product& product : row.recipe->products) {
    auto& [prod, cons] = summer[{product.goods, nullptr}];
    prod += product.GetAmountPerRecipe(row.parameters.productivity) * row.recipesPerSecond;
  }
  for (const Ingredient& ingredient : row.recipe->ingredients) {
    auto& [prod, cons] = summer[{ingredient.goods, nullptr}];
    cons += ingredient.amount * row.recipesPerSecond;
  }
  if (row.fuel) {
    auto& [prod, cons] = summer[{row.fuel.target, row.fuel.quality}];
    cons += row.parameters.fuelUsagePerSecondPerRecipe * row.recipesPerSecond;
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
