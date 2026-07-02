// Port of the flat solver core of Yafc.Model/Model/ProductionTable.cs Solve().
// Operates on the flattened recipe/link lists that upstream's hierarchical
// Setup() produces; nested-table flattening, science-pack decomposition and
// model mutations (auto link removal, undo) stay in the model layer (later
// Phase 2 work). Per the web-native UI decision, results are typed codes and
// flags — presentation/i18n happen in the web layer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yafc {

enum class LinkAlgorithm { Match, AllowOverProduction, AllowOverConsumption };

// Upstream ProductionLink.Flags (bit-compatible).
struct LinkFlags {
  static constexpr uint32_t kLinkNotMatched = 1u << 0;
  static constexpr uint32_t kLinkRecursiveNotMatched = 1u << 1;
  static constexpr uint32_t kHasConsumption = 1u << 2;
  static constexpr uint32_t kHasProduction = 1u << 3;
  static constexpr uint32_t kHasProductionAndConsumption =
      kHasConsumption | kHasProduction;
};

// Upstream WarningFlags (bit-compatible subset used by the solver).
struct RecipeWarningFlags {
  static constexpr uint32_t kDeadlockCandidate = 1u << 16;
  static constexpr uint32_t kOverproductionRequired = 1u << 17;
};

// Upstream: Solve() returns null or a localized message; we return a code.
enum class TableSolveResult {
  Ok,
  // INFEASIBLE even after slack analysis (upstream: "no solution + no deadlocks").
  NoSolutionNoDeadlocks,
  // ABNORMAL from GLOP (upstream: "numerical errors").
  NumericalErrors,
  UnexpectedError,
};

struct SolverGoodsEntry {
  int link = -1;        // index into links, or -1 if unlinked (ignored by solver)
  double amount = 0.0;  // per craft; positive for both products and ingredients
};

struct SolverRecipe {
  std::string name;                  // ISolverRow.SolverName
  double base_cost = 1.0;            // ISolverRow.BaseCost
  double fixed_rps = -1.0;           // >=0 pins the variable (fixedBuildings/recipeTime)
  std::vector<SolverGoodsEntry> products;     // upstream ProductsForSolver (amount > 0)
  std::vector<SolverGoodsEntry> ingredients;  // upstream Ingredients + fuel

  // Results:
  double recipes_per_second = 0.0;
  uint32_t warning_flags = 0;
};

struct SolverLink {
  std::string name;      // goods name (diagnostics only)
  double amount = 0.0;   // demand: >0 desired production, <0 desired consumption
  LinkAlgorithm algorithm = LinkAlgorithm::Match;
  double goods_cost = 1.0;  // |goods.Cost()| — slack weighting in the fallback

  // Results:
  uint32_t flags = 0;
  // Upstream semantics: slack variable value (cost-scaled units), signed:
  // positive = overproduction absorbed, negative = deadlock injection.
  double not_matched_flow = 0.0;
  // Goods-unit totals over the solution (feeds "Extra products"-style UI).
  double production = 0.0;
  double consumption = 0.0;
};

// Mirrors ProductionTable.Solve() on pre-flattened inputs. Mutates recipes and
// links in place with results.
TableSolveResult SolveProductionTable(std::vector<SolverRecipe>& recipes,
                                      std::vector<SolverLink>& links);

}  // namespace yafc
