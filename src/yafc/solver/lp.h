// Thin wrapper over or-tools glop, shaped after the Google.OrTools.LinearSolver
// C# surface that yafc-ce uses (Solver/Variable/Constraint/Objective), so the
// Phase 2 port of ProductionTable.cs / CostAnalysis.cs / DataUtils.cs maps
// almost 1:1. See .claude/PLAN.md for the API inventory.
#pragma once

#include <limits>
#include <memory>
#include <string>

namespace operations_research::glop {
class LinearProgram;
class LPSolver;
}  // namespace operations_research::glop

namespace yafc {

inline constexpr double kInfinity = std::numeric_limits<double>::infinity();

// Mirrors Google.OrTools' Solver.ResultStatus subset yafc branches on.
enum class LpResult { Optimal, Feasible, Infeasible, Abnormal, Other };

// Mirrors Solver.BasisStatus (yafc checks AT_LOWER_BOUND / BASIC / FREE).
enum class LpBasisStatus { Free, AtLowerBound, AtUpperBound, FixedValue, Basic };

// One LP instance == one C# `Solver` (yafc creates a fresh one per solve).
class LpSolver {
 public:
  using VarId = int;
  using ConstraintId = int;

  LpSolver();
  ~LpSolver();
  LpSolver(const LpSolver&) = delete;
  LpSolver& operator=(const LpSolver&) = delete;

  // Model building (C#: MakeNumVar / MakeConstraint / SetCoefficient / SetBounds).
  VarId MakeNumVar(double lb, double ub, const std::string& name);
  void SetVariableBounds(VarId var, double lb, double ub);
  ConstraintId MakeConstraint(double lb, double ub, const std::string& name);
  void SetConstraintBounds(ConstraintId c, double lb, double ub);
  void SetCoefficient(ConstraintId c, VarId var, double coefficient);

  // Objective (C#: solver.Objective() SetCoefficient/SetMinimization/Clear/Value).
  void SetObjectiveCoefficient(VarId var, double coefficient);
  void SetMinimization();
  void SetMaximization();
  void ClearObjective();

  // C#: SetSolverSpecificParametersAsString — text-format GlopParameters.
  bool SetSolverParameters(const std::string& text_proto);

  // C#: Solve(); DataUtils.TrySolveWithDifferentSeeds retries ABNORMAL results
  // with fresh random_seed values — same behavior here (3 attempts).
  LpResult Solve();
  LpResult SolveWithDifferentSeeds();

  // Results, valid after a successful Solve (C# names in comments).
  double SolutionValue(VarId var) const;             // Variable.SolutionValue()
  double DualValue(ConstraintId c) const;            // Constraint.DualValue()
  double ObjectiveValue() const;                     // Objective.Value()
  LpBasisStatus VariableBasisStatus(VarId var) const;      // Variable.BasisStatus()
  LpBasisStatus ConstraintBasisStatus(ConstraintId c) const;  // Constraint.BasisStatus()

 private:
  std::unique_ptr<operations_research::glop::LinearProgram> lp_;
  std::unique_ptr<operations_research::glop::LPSolver> solver_;
  std::string parameters_text_;
};

}  // namespace yafc
