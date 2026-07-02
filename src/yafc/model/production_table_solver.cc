#include "yafc/model/production_table_solver.h"

#include <cmath>
#include <map>
#include <utility>

#include "yafc/model/graph.h"
#include "yafc/solver/lp.h"

namespace yafc {

namespace {

// Upstream GetInfeasibilityCandidates(allRecipes): a graph over links where
// each recipe connects its ingredient links (sources) to its product links
// (targets); splits collect the targets of multi-output recipes, deadlocks come
// from strongly-connected components (the last member of each SCC plus any
// member with a forward "chord" edge inside the component).
struct InfeasibilityCandidates {
  std::vector<int> deadlocks;
  std::vector<int> splits;
};

InfeasibilityCandidates GetInfeasibilityCandidates(
    const std::vector<SolverRecipe>& recipes) {
  Graph<int> graph;
  InfeasibilityCandidates result;

  for (const SolverRecipe& recipe : recipes) {
    // FindAllRecipeLinks: sources = ingredient (+fuel) links, targets = product links.
    std::vector<int> sources, targets;
    for (const SolverGoodsEntry& e : recipe.ingredients) {
      if (e.link >= 0) sources.push_back(e.link);
    }
    for (const SolverGoodsEntry& e : recipe.products) {
      if (e.link >= 0) targets.push_back(e.link);
    }
    for (int src : sources) {
      for (int tgt : targets) graph.Connect(src, tgt);
    }
    if (targets.size() > 1) {
      result.splits.insert(result.splits.end(), targets.begin(), targets.end());
    }
  }

  for (const std::vector<int>& loop : graph.NontrivialComponents()) {
    result.deadlocks.push_back(loop.back());  // upstream: sources.Add(list[^1])
    for (size_t i = 0; i + 1 < loop.size(); ++i) {
      for (size_t j = i + 2; j < loop.size(); ++j) {
        if (graph.HasConnection(loop[i], loop[j])) {
          result.deadlocks.push_back(loop[i]);
          break;
        }
      }
    }
  }
  return result;
}

}  // namespace

TableSolveResult SolveProductionTable(std::vector<SolverRecipe>& recipes,
                                      std::vector<SolverLink>& links) {
  LpSolver solver;  // DataUtils.CreateSolver: GLOP + solution_feasibility_tolerance:1e-1
  solver.SetMinimization();

  // Variables, one per recipe; fixed buildings pin the rate.
  std::vector<int> vars(recipes.size());
  for (size_t i = 0; i < recipes.size(); ++i) {
    vars[i] = solver.MakeNumVar(0.0, kInfinity, recipes[i].name);
    if (recipes[i].fixed_rps >= 0.0) {
      solver.SetVariableBounds(vars[i], recipes[i].fixed_rps, recipes[i].fixed_rps);
    }
    recipes[i].recipes_per_second = 0.0;
    recipes[i].warning_flags = 0;
  }

  // Constraints, one per link; bounds by algorithm, flags seeded by demand sign.
  std::vector<int> constraints(links.size());
  for (size_t i = 0; i < links.size(); ++i) {
    SolverLink& link = links[i];
    double min = link.algorithm == LinkAlgorithm::AllowOverConsumption ? -kInfinity
                                                                       : link.amount;
    double max = link.algorithm == LinkAlgorithm::AllowOverProduction ? kInfinity
                                                                      : link.amount;
    constraints[i] = solver.MakeConstraint(min, max, link.name);
    link.flags = link.amount > 0   ? LinkFlags::kHasConsumption
                 : link.amount < 0 ? LinkFlags::kHasProduction
                                   : 0;
    link.not_matched_flow = 0.0;
    link.production = link.consumption = 0.0;
  }

  // Coefficients. Upstream AddLinkCoefficient accumulates via GetCoefficient so
  // the same goods appearing on both sides (catalysts) sums correctly.
  std::map<std::pair<int, int>, double> coefficients;  // (link, recipe) -> amount
  for (size_t i = 0; i < recipes.size(); ++i) {
    for (const SolverGoodsEntry& product : recipes[i].products) {
      if (product.amount <= 0.0 || product.link < 0) continue;
      links[product.link].flags |= LinkFlags::kHasProduction;
      coefficients[{product.link, static_cast<int>(i)}] += product.amount;
    }
    for (const SolverGoodsEntry& ingredient : recipes[i].ingredients) {
      if (ingredient.link < 0) continue;
      links[ingredient.link].flags |= LinkFlags::kHasConsumption;
      coefficients[{ingredient.link, static_cast<int>(i)}] -= ingredient.amount;
    }
  }
  for (const auto& [key, amount] : coefficients) {
    solver.SetCoefficient(constraints[key.first], vars[key.second], amount);
  }

  // Links with production or consumption missing entirely get their constraint
  // disabled. (Upstream additionally deletes fully-unused links from the model;
  // that mutation belongs to the model layer, the flag is enough here.)
  for (size_t i = 0; i < links.size(); ++i) {
    if ((links[i].flags & LinkFlags::kHasProductionAndConsumption) !=
        LinkFlags::kHasProductionAndConsumption) {
      links[i].flags |= LinkFlags::kLinkNotMatched;
      solver.SetConstraintBounds(constraints[i], -kInfinity, kInfinity);
    }
  }

  for (size_t i = 0; i < recipes.size(); ++i) {
    solver.SetObjectiveCoefficient(vars[i], recipes[i].base_cost);
  }

  LpResult result = solver.Solve();

  if (result != LpResult::Feasible && result != LpResult::Optimal) {
    // Slack fallback: find out *why* it is infeasible.
    solver.ClearObjective();
    InfeasibilityCandidates candidates = GetInfeasibilityCandidates(recipes);
    std::vector<int> positive_slack(links.size(), -1);
    std::vector<int> negative_slack(links.size(), -1);

    for (int link : candidates.deadlocks) {
      if (negative_slack[link] >= 0) continue;
      double cost = std::abs(links[link].goods_cost);
      int v = solver.MakeNumVar(0.0, kInfinity, "negative-slack." + links[link].name);
      solver.SetCoefficient(constraints[link], v, cost);
      solver.SetObjectiveCoefficient(v, 1.0);
      negative_slack[link] = v;
    }
    for (int link : candidates.splits) {
      if (positive_slack[link] >= 0) continue;
      double cost = std::abs(links[link].goods_cost);
      int v = solver.MakeNumVar(0.0, kInfinity, "positive-slack." + links[link].name);
      solver.SetCoefficient(constraints[link], v, -cost);
      solver.SetObjectiveCoefficient(v, 1.0);
      positive_slack[link] = v;
    }

    result = solver.Solve();

    if (result == LpResult::Optimal || result == LpResult::Feasible) {
      // Upstream stores the raw slack solution values into notMatchedFlow,
      // gated on basis status, then flags the affected links.
      for (size_t i = 0; i < links.size(); ++i) {
        if (positive_slack[i] >= 0 &&
            solver.VariableBasisStatus(positive_slack[i]) !=
                LpBasisStatus::AtLowerBound) {
          links[i].not_matched_flow += solver.SolutionValue(positive_slack[i]);
        }
        if (negative_slack[i] >= 0 &&
            solver.VariableBasisStatus(negative_slack[i]) !=
                LpBasisStatus::AtLowerBound) {
          links[i].not_matched_flow -= solver.SolutionValue(negative_slack[i]);
        }
        if (links[i].not_matched_flow != 0.0) {
          links[i].flags |=
              LinkFlags::kLinkNotMatched | LinkFlags::kLinkRecursiveNotMatched;
        }
      }

      // Upstream FindAllRecipeLinks loop: recipes touching a recursively
      // unmatched link inherit a warning. (The owner-chain propagation is
      // hierarchical model work, not part of the flat core.)
      for (SolverRecipe& recipe : recipes) {
        auto check = [&](const SolverGoodsEntry& e) {
          if (e.link < 0) return;
          const SolverLink& link = links[e.link];
          if (link.flags & LinkFlags::kLinkRecursiveNotMatched) {
            recipe.warning_flags |= link.not_matched_flow > 0
                                        ? RecipeWarningFlags::kOverproductionRequired
                                        : RecipeWarningFlags::kDeadlockCandidate;
          }
        };
        for (const SolverGoodsEntry& e : recipe.ingredients) check(e);
        for (const SolverGoodsEntry& e : recipe.products) check(e);
      }
    } else if (result == LpResult::Infeasible) {
      return TableSolveResult::NoSolutionNoDeadlocks;
    } else if (result == LpResult::Abnormal) {
      return TableSolveResult::NumericalErrors;
    } else {
      return TableSolveResult::UnexpectedError;
    }
  }

  // Constraint basis pass: links that are not tight are "not matched" if they
  // had slack flow or a non-Match algorithm.
  for (size_t i = 0; i < links.size(); ++i) {
    LpBasisStatus basis = solver.ConstraintBasisStatus(constraints[i]);
    if ((basis == LpBasisStatus::Basic || basis == LpBasisStatus::Free) &&
        (links[i].not_matched_flow != 0.0 ||
         links[i].algorithm != LinkAlgorithm::Match)) {
      links[i].flags |= LinkFlags::kLinkNotMatched;
    }
  }

  for (size_t i = 0; i < recipes.size(); ++i) {
    recipes[i].recipes_per_second = solver.SolutionValue(vars[i]);
  }

  // Goods-unit production/consumption totals per link (upstream: CalculateFlow).
  for (const auto& [key, amount] : coefficients) {
    double flow = amount * recipes[key.second].recipes_per_second;
    if (flow > 0) {
      links[key.first].production += flow;
    } else {
      links[key.first].consumption -= flow;
    }
  }

  return TableSolveResult::Ok;
}

}  // namespace yafc
