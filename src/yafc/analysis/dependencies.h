// Port of Yafc.Model/Analysis/Dependencies.cs plus the per-class
// GetDependencies() implementations from DataClasses.cs (kept out of the data
// model as a free-function dispatch so data_classes.h stays decoupled from the
// analysis layer).
#pragma once

#include <vector>

#include "yafc/analysis/dependency_node.h"
#include "yafc/model/database.h"

namespace yafc {

// Upstream FactorioObject.GetDependencies() (virtual); db is needed for the
// global lookups upstream does through Database statics.
DependencyNode GetDependencies(const FactorioObject& obj, const Database& db);

class Dependencies {
 public:
  void Calculate(const Database& db);

  // dependencyList[obj]: what obj requires (categorized require-any/-all tree).
  Mapping<FactorioObject, DependencyNode> dependencyList;
  // reverseDependencies[obj]: the ids of objects that require obj.
  Mapping<FactorioObject, std::vector<FactorioId>> reverseDependencies;
};

}  // namespace yafc
