// The Phase 0 tracer bullet as a unit test, via the yafc::LpSolver wrapper:
// 900 lead plates/min from lead ore, gangue byproduct + grade-3 recycle loop,
// yafc's two-pass solve (equality links -> INFEASIBLE -> candidate slack ->
// OPTIMAL). Reference values from the desktop yafc run of the same chain.
#include "yafc/solver/lp.h"

#include <vector>

#include "doctest/doctest.h"

using yafc::kInfinity;
using yafc::LpBasisStatus;
using yafc::LpResult;
using yafc::LpSolver;

TEST_CASE("lead plate production table solves like desktop yafc") {
  LpSolver s;

  // Recipes (variables, crafts/min).
  int mine = s.MakeNumVar(0, kInfinity, "lead-ore-mining");
  int g1 = s.MakeNumVar(0, kInfinity, "lead-grade-1");
  int g2 = s.MakeNumVar(0, kInfinity, "lead-grade-2");
  int g3 = s.MakeNumVar(0, kInfinity, "lead-grade-3");
  int dust = s.MakeNumVar(0, kInfinity, "silver-lead-dust");
  int molten = s.MakeNumVar(0, kInfinity, "molten-lead");
  int cast = s.MakeNumVar(0, kInfinity, "cast-lead-plate");

  // Links (constraints): production - consumption = demand.
  int lOre = s.MakeConstraint(0, 0, "lead-ore");
  int lG1 = s.MakeConstraint(0, 0, "grade1-lead");
  int lG2 = s.MakeConstraint(0, 0, "grade2-lead");
  int lFeed = s.MakeConstraint(0, 0, "grade3-dust-feed");
  int lDust = s.MakeConstraint(0, 0, "silver-dust");
  int lMolten = s.MakeConstraint(0, 0, "molten-lead");
  int lPlate = s.MakeConstraint(900, 900, "lead-plate");
  int lGangue = s.MakeConstraint(0, 0, "gangue");

  s.SetCoefficient(lOre, mine, 1);
  s.SetCoefficient(lOre, g1, -5);
  s.SetCoefficient(lG1, g1, 1);
  s.SetCoefficient(lG1, g2, -2);
  s.SetCoefficient(lG1, g3, 1);  // recycle
  s.SetCoefficient(lG2, g2, 1);
  s.SetCoefficient(lGangue, g2, 1);  // byproduct
  s.SetCoefficient(lG2, g3, -4);
  s.SetCoefficient(lFeed, g3, 2);
  s.SetCoefficient(lFeed, dust, -1);
  s.SetCoefficient(lDust, dust, 1);
  s.SetCoefficient(lDust, molten, -1);
  s.SetCoefficient(lMolten, molten, 22.5);
  s.SetCoefficient(lMolten, cast, -1.5875);
  s.SetCoefficient(lPlate, cast, 1);

  s.SetMinimization();
  for (int v : {mine, g1, g2, g3, dust, molten, cast}) {
    s.SetObjectiveCoefficient(v, 1);
  }

  // Pass 1: strict links must be infeasible (nothing consumes gangue).
  REQUIRE(s.SolveWithDifferentSeeds() == LpResult::Infeasible);

  // Pass 2: yafc slack fallback on infeasibility candidates.
  s.ClearObjective();
  // splits (multi-output products): absorb surplus, coefficient -cost.
  struct Slack { int link; double cost; int var; };
  std::vector<Slack> splits = {
      {lGangue, 10, 0}, {lG2, 1, 0}, {lG1, 1, 0}, {lFeed, 1, 0}};
  for (Slack& sl : splits) {
    sl.var = s.MakeNumVar(0, kInfinity, "positive-slack");
    s.SetCoefficient(sl.link, sl.var, -sl.cost);
    s.SetObjectiveCoefficient(sl.var, 1);
  }
  // deadlocks (grade1<->grade2 loop): inject goods, coefficient +cost.
  for (int link : {lG1, lG2}) {
    int v = s.MakeNumVar(0, kInfinity, "negative-slack");
    s.SetCoefficient(link, v, 1);
    s.SetObjectiveCoefficient(v, 1);
  }

  LpResult r = s.SolveWithDifferentSeeds();
  REQUIRE((r == LpResult::Optimal || r == LpResult::Feasible));

  CHECK(s.SolutionValue(mine) == doctest::Approx(1111.25));
  CHECK(s.SolutionValue(g1) == doctest::Approx(222.25));
  CHECK(s.SolutionValue(g2) == doctest::Approx(127.0));
  CHECK(s.SolutionValue(g3) == doctest::Approx(31.75));
  CHECK(s.SolutionValue(dust) == doctest::Approx(63.5));
  CHECK(s.SolutionValue(molten) == doctest::Approx(63.5));
  CHECK(s.SolutionValue(cast) == doctest::Approx(900.0));

  // Gangue slack absorbed 127/min (yafc "Extra products"), gated on basis
  // status the way ProductionTable.cs reads slack.
  const Slack& gangue = splits[0];
  REQUIRE(s.VariableBasisStatus(gangue.var) != LpBasisStatus::AtLowerBound);
  CHECK(s.SolutionValue(gangue.var) * gangue.cost == doctest::Approx(127.0));
}
