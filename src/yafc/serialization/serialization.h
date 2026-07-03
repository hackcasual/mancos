// Port of the serialization layer (Yafc.Model/Serialization/*): upstream uses
// reflection over public properties; here every serializable class declares
// its fields once in a Visit* function (serialization.cc), and the JSON
// writer, JSON reader and the undo snapshotter all walk that declaration.
//
// Compatibility contract (PLAN.md): property names and shapes match the
// upstream .yafc JSON for the fields that exist in the C++ model; unknown
// properties are skipped on read (upstream ErrorCollector semantics) so files
// from desktop yafc load even where our model is still a subset.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "yafc/model/database.h"
#include "yafc/model/production_table.h"

namespace yafc {

class ProjectPage {
 public:
  std::string guid;
  std::string name;
  std::string icon;         // typeDotName of the icon object, if any
  ProductionTable content;  // contentType "ProductionTable"
};

class ProjectSettings {
 public:
  std::vector<FactorioObject*> milestones;
  std::vector<FactorioObject*> milestonesUnlocked;  // researched subset
};

class Project {
 public:
  ProjectSettings settings;
  std::vector<std::unique_ptr<ProjectPage>> pages;
};

// Serializes the project to .yafc-shaped JSON.
nlohmann::json SaveProject(const Project& project);
std::string SaveProjectToString(const Project& project, int indent = 2);

// Loads a project; FactorioObject references resolve by typeDotName against
// db. Unresolvable refs and malformed values are collected, not fatal.
struct LoadResult {
  std::unique_ptr<Project> project;
  std::vector<std::string> errors;  // "unresolved:<name>", "badvalue:<prop>", "badjson"
};
LoadResult LoadProject(const nlohmann::json& json, const Database& db);
LoadResult LoadProjectFromString(const std::string& text, const Database& db);

// Snapshot-based undo over the whole project. Upstream snapshots individual
// model objects through the property serializers; whole-tree snapshots have
// the same observable behavior at this scale (revisit if profiling says so).
class UndoSystem {
 public:
  explicit UndoSystem(const Database* db, size_t maxDepth = 100)
      : db_(db), maxDepth_(maxDepth) {}

  void RecordSnapshot(const Project& project);  // call BEFORE a mutation
  bool CanUndo() const { return !undo_.empty(); }
  bool CanRedo() const { return !redo_.empty(); }
  // Both return the replacement project (the argument is the current state).
  std::unique_ptr<Project> Undo(const Project& current);
  std::unique_ptr<Project> Redo(const Project& current);

 private:
  const Database* db_;
  size_t maxDepth_;
  std::vector<nlohmann::json> undo_;
  std::vector<nlohmann::json> redo_;
};

}  // namespace yafc
