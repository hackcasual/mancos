#include "yafc/solver/lp.h"

#include <string>

#include "google/protobuf/text_format.h"
#include "ortools/glop/lp_solver.h"
#include "ortools/glop/parameters.pb.h"
#include "ortools/lp_data/lp_data.h"
#include "ortools/lp_data/lp_types.h"

namespace yafc {

namespace glop = operations_research::glop;

namespace {

LpResult ToResult(glop::ProblemStatus s) {
  switch (s) {
    case glop::ProblemStatus::OPTIMAL: return LpResult::Optimal;
    case glop::ProblemStatus::PRIMAL_FEASIBLE: return LpResult::Feasible;
    case glop::ProblemStatus::PRIMAL_INFEASIBLE:
    case glop::ProblemStatus::DUAL_UNBOUNDED: return LpResult::Infeasible;
    case glop::ProblemStatus::ABNORMAL:
    case glop::ProblemStatus::IMPRECISE: return LpResult::Abnormal;
    default: return LpResult::Other;
  }
}

LpBasisStatus ToBasis(glop::VariableStatus s) {
  switch (s) {
    case glop::VariableStatus::FREE: return LpBasisStatus::Free;
    case glop::VariableStatus::AT_LOWER_BOUND: return LpBasisStatus::AtLowerBound;
    case glop::VariableStatus::AT_UPPER_BOUND: return LpBasisStatus::AtUpperBound;
    case glop::VariableStatus::FIXED_VALUE: return LpBasisStatus::FixedValue;
    case glop::VariableStatus::BASIC: return LpBasisStatus::Basic;
  }
  return LpBasisStatus::Free;
}

LpBasisStatus ToBasis(glop::ConstraintStatus s) {
  switch (s) {
    case glop::ConstraintStatus::FREE: return LpBasisStatus::Free;
    case glop::ConstraintStatus::AT_LOWER_BOUND: return LpBasisStatus::AtLowerBound;
    case glop::ConstraintStatus::AT_UPPER_BOUND: return LpBasisStatus::AtUpperBound;
    case glop::ConstraintStatus::FIXED_VALUE: return LpBasisStatus::FixedValue;
    case glop::ConstraintStatus::BASIC: return LpBasisStatus::Basic;
  }
  return LpBasisStatus::Free;
}

}  // namespace

LpSolver::LpSolver()
    : lp_(std::make_unique<glop::LinearProgram>()),
      solver_(std::make_unique<glop::LPSolver>()),
      // yafc default (DataUtils.CreateSolver)
      parameters_text_("solution_feasibility_tolerance:1e-1") {}

LpSolver::~LpSolver() = default;

LpSolver::VarId LpSolver::MakeNumVar(double lb, double ub, const std::string& name) {
  glop::ColIndex col = lp_->CreateNewVariable();
  lp_->SetVariableName(col, name);
  lp_->SetVariableBounds(col, lb, ub);
  return col.value();
}

void LpSolver::SetVariableBounds(VarId var, double lb, double ub) {
  lp_->SetVariableBounds(glop::ColIndex(var), lb, ub);
}

LpSolver::ConstraintId LpSolver::MakeConstraint(double lb, double ub,
                                                const std::string& name) {
  glop::RowIndex row = lp_->CreateNewConstraint();
  lp_->SetConstraintName(row, name);
  lp_->SetConstraintBounds(row, lb, ub);
  return row.value();
}

void LpSolver::SetConstraintBounds(ConstraintId c, double lb, double ub) {
  lp_->SetConstraintBounds(glop::RowIndex(c), lb, ub);
}

void LpSolver::SetCoefficient(ConstraintId c, VarId var, double coefficient) {
  lp_->SetCoefficient(glop::RowIndex(c), glop::ColIndex(var), coefficient);
}

void LpSolver::SetObjectiveCoefficient(VarId var, double coefficient) {
  lp_->SetObjectiveCoefficient(glop::ColIndex(var), coefficient);
}

void LpSolver::SetMinimization() { lp_->SetMaximizationProblem(false); }
void LpSolver::SetMaximization() { lp_->SetMaximizationProblem(true); }

void LpSolver::ClearObjective() {
  for (glop::ColIndex col(0); col < lp_->num_variables(); ++col) {
    lp_->SetObjectiveCoefficient(col, 0.0);
  }
}

bool LpSolver::SetSolverParameters(const std::string& text_proto) {
  glop::GlopParameters params;
  if (!google::protobuf::TextFormat::ParseFromString(text_proto, &params)) {
    return false;
  }
  parameters_text_ = text_proto;
  return true;
}

LpResult LpSolver::Solve() {
  glop::GlopParameters params;
  google::protobuf::TextFormat::ParseFromString(parameters_text_, &params);
  solver_->SetParameters(params);
  // glop requires columns ordered by row with no zero coefficients; the C#
  // MPSolver wrapper does this internally before every solve.
  lp_->CleanUp();
  return ToResult(solver_->Solve(*lp_));
}

LpResult LpSolver::SolveWithDifferentSeeds() {
  // DataUtils.TrySolveWithDifferentSeeds: retry ABNORMAL with new random seeds.
  static constexpr int kSeeds[] = {17, 42, 1337};
  LpResult result = Solve();
  for (int seed : kSeeds) {
    if (result != LpResult::Abnormal) return result;
    SetSolverParameters(parameters_text_ + " random_seed:" + std::to_string(seed));
    result = Solve();
  }
  return result;
}

double LpSolver::SolutionValue(VarId var) const {
  return solver_->variable_values()[glop::ColIndex(var)];
}

double LpSolver::DualValue(ConstraintId c) const {
  return solver_->dual_values()[glop::RowIndex(c)];
}

double LpSolver::ObjectiveValue() const { return solver_->GetObjectiveValue(); }

LpBasisStatus LpSolver::VariableBasisStatus(VarId var) const {
  return ToBasis(solver_->variable_statuses()[glop::ColIndex(var)]);
}

LpBasisStatus LpSolver::ConstraintBasisStatus(ConstraintId c) const {
  return ToBasis(solver_->constraint_statuses()[glop::RowIndex(c)]);
}

}  // namespace yafc
