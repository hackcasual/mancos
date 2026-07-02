// Tracer bullet: headless yafc-style production-table solve on GLOP/wasm under node.
//
// Scenario (mirrors the desktop yafc screenshot for a modded game): produce
// 900 lead plates/min from lead ore mining. The chain has a genuine byproduct
// (gangue, nothing consumes it) and a recycle loop (lead grade 3 returns
// grade-1 lead into grade 2's input), so simple ratio arithmetic fails and the
// linear solver is required — same reason yafc uses GLOP.
//
//   lead ore -> grade 1 -> grade 2 -> grade 3 -> silver lead dust
//                  ^          |          |            |
//                  |          +-> gangue +-> grade 1  v
//                  +---------------------- (recycle)  molten lead -> cast plate
//
// This exercises the exact solver functionality Yafc-CE uses
// (Yafc.Model/Model/ProductionTable.cs, Analysis/CostAnalysis.cs, Data/DataUtils.cs):
//
//   yafc (MPSolver C# API)                         -> here (glop C++ API)
//   ---------------------------------------------------------------------
//   Solver.CreateSolver("GLOP_LINEAR_PROGRAMMING")    LPSolver
//   SetSolverSpecificParametersAsString(...)          TextFormat -> GlopParameters
//   MakeNumVar / SetBounds                            CreateNewVariable + SetVariableBounds
//   MakeConstraint / SetBounds                        CreateNewConstraint + SetConstraintBounds
//   Constraint.SetCoefficient                         SetCoefficient
//   Objective SetCoefficient/SetMinimization/Clear    SetObjectiveCoefficient / SetMaximizationProblem
//   Solve() -> ResultStatus                           Solve -> ProblemStatus
//   Variable.SolutionValue()                          variable_values()
//   Constraint.DualValue()                            dual_values()
//   Variable/Constraint.BasisStatus()                 GetVariableStatus / GetConstraintStatus
//
// Pass 1 solves with equality link constraints (yafc semantics); the gangue
// byproduct makes that INFEASIBLE. Pass 2 reproduces yafc's fallback
// (ProductionTable.cs after an INFEASIBLE result): objective.Clear(), then
// GetInfeasibilityCandidates picks
//   - "splits":    links produced by multi-output recipes -> positive slack
//                  (phantom consumption, coefficient -cost, objective 1)
//   - "deadlocks": links inside strongly-connected recipe loops -> negative
//                  slack (phantom production, coefficient +cost, objective 1)
// and re-solves; unmatched flow is read off slack values gated by BasisStatus.
// The candidate sets below are the (hand-derived) result of that analysis for
// this fixed topology — the full Graph/SCC computation is a Phase 2 port item.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "google/protobuf/text_format.h"
#include "ortools/glop/lp_solver.h"
#include "ortools/glop/parameters.pb.h"
#include "ortools/lp_data/lp_data.h"
#include "ortools/lp_data/lp_types.h"

using operations_research::glop::ColIndex;
using operations_research::glop::GlopParameters;
using operations_research::glop::kInfinity;
using operations_research::glop::LinearProgram;
using operations_research::glop::LPSolver;
using operations_research::glop::ProblemStatus;
using operations_research::glop::RowIndex;
using operations_research::glop::VariableStatus;

static const char* StatusName(ProblemStatus s) {
  switch (s) {
    case ProblemStatus::OPTIMAL: return "OPTIMAL";
    case ProblemStatus::PRIMAL_FEASIBLE: return "FEASIBLE";
    case ProblemStatus::PRIMAL_INFEASIBLE: return "INFEASIBLE";
    case ProblemStatus::DUAL_INFEASIBLE: return "DUAL_INFEASIBLE";
    case ProblemStatus::ABNORMAL: return "ABNORMAL";
    default: return "OTHER";
  }
}

struct Recipe { const char* name; ColIndex var; double expectedCraftsPerMin; };
struct Link { const char* name; RowIndex row; double demandPerMin; };

int main() {
  std::printf("== yafc-web tracer: 900 lead plates/min from lead ore, GLOP on wasm ==\n");

  LinearProgram lp;
  lp.SetName("lead-plates-900-per-min");

  // --- Variables: crafts/min per recipe (yafc: MakeNumVar(0, +inf, name)).
  auto makeRecipe = [&lp](const char* name, double expected) {
    ColIndex c = lp.CreateNewVariable();
    lp.SetVariableName(c, name);
    lp.SetVariableBounds(c, 0.0, kInfinity);
    return Recipe{name, c, expected};
  };
  // Expected crafts/min match the desktop yafc screenshot flows.
  Recipe mine   = makeRecipe("lead-ore-mining",  1111.25);
  Recipe grade1 = makeRecipe("lead-grade-1",      222.25);
  Recipe grade2 = makeRecipe("lead-grade-2",      127.0);
  Recipe grade3 = makeRecipe("lead-grade-3",       31.75);
  Recipe dust   = makeRecipe("silver-lead-dust",   63.5);
  Recipe molten = makeRecipe("molten-lead",        63.5);
  Recipe cast   = makeRecipe("cast-lead-plate",   900.0);

  // --- Links: one constraint per traded good; production - consumption = demand.
  // (Crystals, lead rods, wood and heat are imported: unlinked in yafc => no constraint.)
  auto makeLink = [&lp](const char* name, double demand) {
    RowIndex r = lp.CreateNewConstraint();
    lp.SetConstraintName(r, name);
    lp.SetConstraintBounds(r, demand, demand);
    return Link{name, r, demand};
  };
  Link lOre    = makeLink("lead-ore",         0);
  Link lG1     = makeLink("grade1-lead",      0);
  Link lG2     = makeLink("grade2-lead",      0);
  Link lFeed   = makeLink("grade3-dust-feed", 0);
  Link lDust   = makeLink("silver-dust",      0);
  Link lMolten = makeLink("molten-lead",      0);
  Link lPlate  = makeLink("lead-plate",     900);  // the production goal
  Link lGangue = makeLink("gangue",           0);  // byproduct: nothing consumes it

  // --- Recipe coefficients (per craft).
  lp.SetCoefficient(lOre.row, mine.var, 1.0);
  lp.SetCoefficient(lOre.row, grade1.var, -5.0);      // 5 ore -> 1 grade1
  lp.SetCoefficient(lG1.row, grade1.var, 1.0);
  lp.SetCoefficient(lG1.row, grade2.var, -2.0);       // 2 grade1 -> 1 grade2 + 1 gangue
  lp.SetCoefficient(lG1.row, grade3.var, 1.0);        // recycle: grade3 returns 1 grade1
  lp.SetCoefficient(lG2.row, grade2.var, 1.0);
  lp.SetCoefficient(lGangue.row, grade2.var, 1.0);
  lp.SetCoefficient(lG2.row, grade3.var, -4.0);       // 4 grade2 -> 2 dust feed + 1 grade1
  lp.SetCoefficient(lFeed.row, grade3.var, 2.0);
  lp.SetCoefficient(lFeed.row, dust.var, -1.0);       // 1 feed -> 1 silver dust
  lp.SetCoefficient(lDust.row, dust.var, 1.0);
  lp.SetCoefficient(lDust.row, molten.var, -1.0);     // 1 dust (+crystal+rod) -> 22.5 molten
  lp.SetCoefficient(lMolten.row, molten.var, 22.5);
  lp.SetCoefficient(lMolten.row, cast.var, -1.5875);  // 1.5875 molten (+wood+heat) -> 1 plate
  lp.SetCoefficient(lPlate.row, cast.var, 1.0);

  // --- Objective: minimize recipe cost (yafc: SetCoefficient(var, BaseCost); SetMinimization).
  lp.SetMaximizationProblem(false);
  std::vector<Recipe*> recipes = {&mine, &grade1, &grade2, &grade3, &dust, &molten, &cast};
  for (Recipe* r : recipes) lp.SetObjectiveCoefficient(r->var, 1.0);

  // yafc: SetSolverSpecificParametersAsString("solution_feasibility_tolerance:1e-1"),
  // plus "random_seed:N" retries on ABNORMAL (DataUtils.TrySolveWithDifferentSeeds).
  GlopParameters params;
  if (!google::protobuf::TextFormat::ParseFromString(
          "solution_feasibility_tolerance:1e-1 random_seed:17", &params)) {
    std::printf("FAILED to parse GlopParameters text proto\n");
    return 1;
  }

  // --- Pass 1: equality links. The unconsumed byproduct must make this infeasible.
  LPSolver solver;
  solver.SetParameters(params);
  ProblemStatus status = solver.Solve(lp);
  std::printf("pass 1 (strict links): %s\n", StatusName(status));
  if (status == ProblemStatus::OPTIMAL) {
    std::printf("RESULT CHECK FAILED: expected infeasible (gangue byproduct)\n");
    return 1;
  }

  // --- Pass 2: yafc's slack fallback (ProductionTable.cs after INFEASIBLE result).
  for (Recipe* r : recipes) lp.SetObjectiveCoefficient(r->var, 0.0);  // objective.Clear()

  // GetInfeasibilityCandidates for this topology:
  // splits = products of multi-output recipes: grade2 makes {grade2-lead, gangue},
  //          grade3 makes {grade1-lead, dust-feed}.
  // deadlocks = the grade1<->grade2 strongly-connected loop (grade3 recycles grade1).
  // Goods costs come from CostAnalysis upstream; here: gangue is a waste product
  // (high |cost| => cheap to void per unit), everything else 1.
  struct SlackVar { Link* link; double cost; ColIndex var; };
  std::vector<SlackVar> splits = {
      {&lGangue, 10.0, ColIndex(0)}, {&lG2, 1.0, ColIndex(0)},
      {&lG1, 1.0, ColIndex(0)}, {&lFeed, 1.0, ColIndex(0)}};
  std::vector<SlackVar> deadlocks = {{&lG1, 1.0, ColIndex(0)}, {&lG2, 1.0, ColIndex(0)}};
  for (SlackVar& s : splits) {  // positive slack: absorb surplus (phantom consumption)
    s.var = lp.CreateNewVariable();
    lp.SetVariableName(s.var, std::string("positive-slack.") + s.link->name);
    lp.SetVariableBounds(s.var, 0.0, kInfinity);
    lp.SetCoefficient(s.link->row, s.var, -s.cost);
    lp.SetObjectiveCoefficient(s.var, 1.0);
  }
  for (SlackVar& s : deadlocks) {  // negative slack: inject goods to break loops
    s.var = lp.CreateNewVariable();
    lp.SetVariableName(s.var, std::string("negative-slack.") + s.link->name);
    lp.SetVariableBounds(s.var, 0.0, kInfinity);
    lp.SetCoefficient(s.link->row, s.var, s.cost);
    lp.SetObjectiveCoefficient(s.var, 1.0);
  }

  LPSolver solver2;
  solver2.SetParameters(params);
  status = solver2.Solve(lp);
  std::printf("pass 2 (slack fallback): %s\n", StatusName(status));
  if (status != ProblemStatus::OPTIMAL && status != ProblemStatus::PRIMAL_FEASIBLE) return 1;

  // --- Read results (yafc: SolutionValue, BasisStatus, DualValue).
  std::printf("\n%-18s %12s %12s\n", "recipe", "crafts/min", "expected");
  bool ok = true;
  for (Recipe* r : recipes) {
    double v = solver2.variable_values()[r->var];
    ok &= std::abs(v - r->expectedCraftsPerMin) < 0.05;
    std::printf("%-18s %12.2f %12.2f%s\n", r->name, v, r->expectedCraftsPerMin,
                std::abs(v - r->expectedCraftsPerMin) < 0.05 ? "" : "  <-- MISMATCH");
  }

  std::printf("\nunmatched flows (yafc 'Extra products' / deficits):\n");
  double gangueExtra = 0;
  for (SlackVar& s : splits) {
    // yafc checks BasisStatus != AT_LOWER_BOUND before reading slack SolutionValue.
    double extra = solver2.variable_statuses()[s.var] != VariableStatus::AT_LOWER_BOUND
                       ? solver2.variable_values()[s.var] * s.cost : 0.0;
    if (extra > 1e-9) std::printf("  extra   %-16s %10.2f/min\n", s.link->name, extra);
    if (s.link == &lGangue) gangueExtra = extra;
  }
  for (SlackVar& s : deadlocks) {
    double missing = solver2.variable_statuses()[s.var] != VariableStatus::AT_LOWER_BOUND
                         ? solver2.variable_values()[s.var] * s.cost : 0.0;
    if (missing > 1e-9) std::printf("  missing %-16s %10.2f/min\n", s.link->name, missing);
  }
  // exercise dual values + constraint basis status, as CostAnalysis/ProductionTable do
  std::vector<Link*> links = {&lOre, &lG1, &lG2, &lFeed, &lDust, &lMolten, &lPlate, &lGangue};
  for (Link* l : links) {
    (void)solver2.dual_values()[l->row];
    (void)solver2.constraint_statuses()[l->row];
  }

  ok &= std::abs(gangueExtra - 127.0) < 0.05;  // screenshot: Extra products 127/m
  std::printf("\n%s\n", ok ? "ALL CHECKS PASSED" : "RESULT CHECK FAILED");
  return ok ? 0 : 1;
}
