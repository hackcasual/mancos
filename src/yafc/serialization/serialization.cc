#include "yafc/serialization/serialization.h"

#include <utility>

namespace yafc {

namespace {

// Each serializable type has one Visit<Type>(obj, visitor) declaring its
// properties; JsonWriter and JsonReader provide the same surface:
//   Prop(name, field)                       — scalars, refs, ref lists
//   PropObject(name, obj, visitFn)          — nested object
//   PropList(name, items, factory, visitFn) — owned collections
//   kReading / Has(name)                    — asymmetric cases (subgroup)

class JsonWriter {
 public:
  static constexpr bool kReading = false;

  explicit JsonWriter(nlohmann::json& out) : out_(out) {}

  bool Has(const char*) const { return false; }
  void Prop(const char* name, const bool& v) { out_[name] = v; }
  void Prop(const char* name, const float& v) { out_[name] = v; }
  void Prop(const char* name, const double& v) { out_[name] = v; }
  void Prop(const char* name, const std::string& v) {
    if (!v.empty()) out_[name] = v;
  }
  void Prop(const char* name, const LinkAlgorithm& v) {
    out_[name] = static_cast<int>(v);  // upstream: enums as integers
  }
  void Prop(const char* name, const std::optional<float>& v) {
    if (v.has_value()) out_[name] = *v;
  }
  template <typename T>
    requires std::derived_from<T, FactorioObject>
  void Prop(const char* name, T* const& ref) {  // refs: typeDotName strings
    if (ref != nullptr) out_[name] = ref->typeDotName();
  }
  void Prop(const char* name, const QualityGoods& v) {
    // TODO(port): quality suffix once quality is threaded through tables.
    if (v.target != nullptr) out_[name] = v.target->typeDotName();
  }
  template <typename T>
    requires std::derived_from<T, FactorioObject>
  void Prop(const char* name, const std::vector<T*>& refs) {
    nlohmann::json arr = nlohmann::json::array();
    for (T* ref : refs) arr.push_back(ref->typeDotName());
    out_[name] = std::move(arr);
  }

  template <typename T, typename VisitFn>
  void PropObject(const char* name, T& obj, VisitFn&& visit) {
    nlohmann::json child = nlohmann::json::object();
    JsonWriter writer(child);
    visit(obj, writer);
    out_[name] = std::move(child);
  }

  template <typename T, typename Factory, typename VisitFn>
  void PropList(const char* name, const std::vector<std::unique_ptr<T>>& items,
                Factory&&, VisitFn&& visit) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : items) {
      nlohmann::json child = nlohmann::json::object();
      JsonWriter writer(child);
      visit(*item, writer);
      arr.push_back(std::move(child));
    }
    out_[name] = std::move(arr);
  }

 private:
  nlohmann::json& out_;
};

class JsonReader {
 public:
  static constexpr bool kReading = true;

  JsonReader(const nlohmann::json& in, const Database& db,
             std::vector<std::string>& errors)
      : in_(in), db_(db), errors_(errors) {}

  bool Has(const char* name) const { return in_.contains(name); }
  void Prop(const char* name, bool& v) { Read(name, v); }
  void Prop(const char* name, float& v) { Read(name, v); }
  void Prop(const char* name, double& v) { Read(name, v); }
  void Prop(const char* name, std::string& v) { Read(name, v); }
  void Prop(const char* name, LinkAlgorithm& v) {
    int raw = static_cast<int>(v);
    if (Read(name, raw)) v = static_cast<LinkAlgorithm>(raw);
  }
  void Prop(const char* name, std::optional<float>& v) {
    auto it = in_.find(name);
    if (it != in_.end() && it->is_number()) v = it->get<float>();
  }
  template <typename T>
    requires std::derived_from<T, FactorioObject>
  void Prop(const char* name, T*& ref) {
    std::string typeDotName;
    if (Read(name, typeDotName)) ref = Resolve<T>(typeDotName);
  }
  void Prop(const char* name, QualityGoods& v) {
    std::string typeDotName;
    if (Read(name, typeDotName)) v.target = Resolve<Goods>(typeDotName);
  }
  template <typename T>
    requires std::derived_from<T, FactorioObject>
  void Prop(const char* name, std::vector<T*>& refs) {
    auto it = in_.find(name);
    if (it == in_.end() || !it->is_array()) return;
    refs.clear();
    for (const auto& entry : *it) {
      if (!entry.is_string()) continue;
      if (T* ref = Resolve<T>(entry.get<std::string>())) refs.push_back(ref);
    }
  }

  template <typename T, typename VisitFn>
  void PropObject(const char* name, T& obj, VisitFn&& visit) {
    auto it = in_.find(name);
    if (it == in_.end() || !it->is_object()) return;
    JsonReader reader(*it, db_, errors_);
    visit(obj, reader);
  }

  template <typename T, typename Factory, typename VisitFn>
  void PropList(const char* name, std::vector<std::unique_ptr<T>>& items,
                Factory&& factory, VisitFn&& visit) {
    auto it = in_.find(name);
    if (it == in_.end() || !it->is_array()) return;
    items.clear();
    for (const auto& entry : *it) {
      if (!entry.is_object()) continue;
      std::unique_ptr<T> item = factory();
      JsonReader reader(entry, db_, errors_);
      visit(*item, reader);
      items.push_back(std::move(item));
    }
  }

 private:
  template <typename T>
  bool Read(const char* name, T& v) {
    auto it = in_.find(name);
    if (it == in_.end()) return false;
    try {
      v = it->get<T>();
      return true;
    } catch (...) {
      errors_.push_back(std::string("badvalue:") + name);
      return false;
    }
  }

  template <typename T>
  T* Resolve(const std::string& typeDotName) {
    T* typed = dynamic_cast<T*>(db_.FindByTypeDotName(typeDotName));
    if (typed == nullptr) errors_.push_back("unresolved:" + typeDotName);
    return typed;
  }

  const nlohmann::json& in_;
  const Database& db_;
  std::vector<std::string>& errors_;
};

// -------------------------------------------------------- property lists ----

template <typename V>
void VisitTable(ProductionTable& table, V& v);

template <typename V>
void VisitLink(ProductionLink& link, V& v) {
  v.Prop("goods", link.goods);
  v.Prop("amount", link.amount);
  v.Prop("algorithm", link.algorithm);
}

template <typename V>
void VisitRow(RecipeRow& row, V& v) {
  v.Prop("recipe", row.recipe);
  v.Prop("fuel", row.fuel);
  v.Prop("fixedBuildings", row.fixedBuildings);
  v.Prop("builtBuildings", row.builtBuildings);
  v.Prop("enabled", row.enabled);
  auto visitTable = [](ProductionTable& t, auto& vv) { VisitTable(t, vv); };
  if constexpr (V::kReading) {
    if (v.Has("subgroup")) {
      row.subgroup = std::make_unique<ProductionTable>(&row);
      v.PropObject("subgroup", *row.subgroup, visitTable);
    }
  } else {
    if (row.subgroup != nullptr) v.PropObject("subgroup", *row.subgroup, visitTable);
  }
}

template <typename V>
void VisitTable(ProductionTable& table, V& v) {
  v.PropList("links", table.links,
             [&table] {
               auto link = std::make_unique<ProductionLink>();
               link->owner = &table;
               return link;
             },
             [](ProductionLink& l, auto& vv) { VisitLink(l, vv); });
  v.PropList("recipes", table.recipes,
             [&table] {
               auto row = std::make_unique<RecipeRow>();
               row->owner = &table;
               return row;
             },
             [](RecipeRow& r, auto& vv) { VisitRow(r, vv); });
}

template <typename V>
void VisitPage(ProjectPage& page, V& v) {
  v.Prop("guid", page.guid);
  v.Prop("name", page.name);
  v.Prop("icon", page.icon);
  v.PropObject("content", page.content,
               [](ProductionTable& t, auto& vv) { VisitTable(t, vv); });
}

template <typename V>
void VisitSettings(ProjectSettings& settings, V& v) {
  v.Prop("milestones", settings.milestones);
  v.Prop("milestonesUnlocked", settings.milestonesUnlocked);
}

template <typename V>
void VisitProject(Project& project, V& v) {
  v.PropObject("settings", project.settings,
               [](ProjectSettings& s, auto& vv) { VisitSettings(s, vv); });
  v.PropList("pages", project.pages,
             [] { return std::make_unique<ProjectPage>(); },
             [](ProjectPage& p, auto& vv) { VisitPage(p, vv); });
}

}  // namespace

nlohmann::json SaveProject(const Project& project) {
  nlohmann::json out = nlohmann::json::object();
  JsonWriter writer(out);
  VisitProject(const_cast<Project&>(project), writer);
  return out;
}

std::string SaveProjectToString(const Project& project, int indent) {
  return SaveProject(project).dump(indent);
}

LoadResult LoadProject(const nlohmann::json& json, const Database& db) {
  LoadResult result;
  result.project = std::make_unique<Project>();
  JsonReader reader(json, db, result.errors);
  VisitProject(*result.project, reader);
  return result;
}

LoadResult LoadProjectFromString(const std::string& text, const Database& db) {
  nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
  if (parsed.is_discarded()) {
    LoadResult result;
    result.errors.push_back("badjson");
    return result;
  }
  return LoadProject(parsed, db);
}

void UndoSystem::RecordSnapshot(const Project& project) {
  undo_.push_back(SaveProject(project));
  if (undo_.size() > maxDepth_) undo_.erase(undo_.begin());
  redo_.clear();
}

std::unique_ptr<Project> UndoSystem::Undo(const Project& current) {
  if (undo_.empty()) return nullptr;
  redo_.push_back(SaveProject(current));
  nlohmann::json snapshot = std::move(undo_.back());
  undo_.pop_back();
  return LoadProject(snapshot, *db_).project;
}

std::unique_ptr<Project> UndoSystem::Redo(const Project& current) {
  if (redo_.empty()) return nullptr;
  undo_.push_back(SaveProject(current));
  nlohmann::json snapshot = std::move(redo_.back());
  redo_.pop_back();
  return LoadProject(snapshot, *db_).project;
}

}  // namespace yafc
