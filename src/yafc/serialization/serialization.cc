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
  bool HasString(const char*) const { return false; }
  void Prop(const char* name, const bool& v) { out_[name] = v; }
  void Prop(const char* name, const int& v) { out_[name] = v; }
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
  // Desktop format for a quality-wrapped ref: "!target.typeDotName!quality
  // .name" (quality's bare internal name, not its typeDotName). Plain
  // (unwrapped) when quality is unset, matching the pre-quality-threading
  // format exactly for the common Normal-by-default case.
  static std::string QualityRef(const FactorioObject* target, const Quality* quality) {
    return quality != nullptr ? "!" + target->typeDotName() + "!" + quality->name
                              : target->typeDotName();
  }
  void Prop(const char* name, const QualityGoods& v) {
    if (v.target != nullptr) out_[name] = QualityRef(v.target, v.quality);
  }
  template <typename T>
  void Prop(const char* name, const ObjectWithQuality<T>& v) {
    if (v.target != nullptr) out_[name] = QualityRef(v.target, v.quality);
  }
  template <typename T>
    requires std::derived_from<T, FactorioObject>
  void Prop(const char* name, const std::vector<T*>& refs) {
    nlohmann::json arr = nlohmann::json::array();
    for (T* ref : refs) arr.push_back(ref->typeDotName());
    out_[name] = std::move(arr);
  }
  // Upstream Dictionary<FactorioObject, V>: a JSON object keyed by
  // typeDotName (e.g. productivityTechnologyLevels).
  template <typename T, typename V>
    requires std::derived_from<T, FactorioObject>
  void Prop(const char* name, const std::map<T*, V>& map) {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [ref, value] : map) obj[ref->typeDotName()] = value;
    out_[name] = std::move(obj);
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

  // Value-struct list (e.g. ModuleTemplate entries) — no factory needed.
  template <typename T, typename VisitFn>
  void PropStructList(const char* name, const std::vector<T>& items, VisitFn&& visit) {
    nlohmann::json arr = nlohmann::json::array();
    for (const T& item : items) {
      nlohmann::json child = nlohmann::json::object();
      JsonWriter writer(child);
      visit(const_cast<T&>(item), writer);
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
  bool HasString(const char* name) const {
    auto it = in_.find(name);
    return it != in_.end() && it->is_string();
  }
  void Prop(const char* name, bool& v) { Read(name, v); }
  void Prop(const char* name, int& v) { Read(name, v); }
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
    std::string ref;
    if (Read(name, ref)) {
      auto [target, quality] = ResolveWithQuality<Goods>(ref);
      v.target = target;
      v.quality = quality;
    }
  }
  template <typename T>
  void Prop(const char* name, ObjectWithQuality<T>& v) {
    std::string ref;
    if (Read(name, ref)) {
      auto [target, quality] = ResolveWithQuality<T>(ref);
      v.target = target;
      v.quality = quality;
    }
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
  template <typename T, typename V>
    requires std::derived_from<T, FactorioObject>
  void Prop(const char* name, std::map<T*, V>& map) {
    auto it = in_.find(name);
    if (it == in_.end() || !it->is_object()) return;
    map.clear();
    for (const auto& [key, value] : it->items()) {
      T* ref = Resolve<T>(key);
      if (ref != nullptr && value.is_number()) map[ref] = value.get<V>();
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

  template <typename T, typename VisitFn>
  void PropStructList(const char* name, std::vector<T>& items, VisitFn&& visit) {
    auto it = in_.find(name);
    if (it == in_.end() || !it->is_array()) return;
    items.clear();
    for (const auto& entry : *it) {
      if (!entry.is_object()) continue;
      T item{};
      JsonReader reader(entry, db_, errors_);
      visit(item, reader);
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
  T* Resolve(const std::string& reference) {
    // Desktop yafc writes quality-wrapped refs as "!TypeDotName!quality";
    // the quality dimension is not threaded through the web table yet, so
    // resolve the target and drop the quality (normal assumed).
    std::string typeDotName = reference;
    if (!typeDotName.empty() && typeDotName[0] == '!') {
      size_t bang = typeDotName.find('!', 1);
      typeDotName = bang == std::string::npos ? typeDotName.substr(1)
                                              : typeDotName.substr(1, bang - 1);
    }
    T* typed = dynamic_cast<T*>(db_.FindByTypeDotName(typeDotName));
    if (typed == nullptr) errors_.push_back("unresolved:" + reference);
    return typed;
  }

  // Desktop format: "!target.typeDotName!quality.name" (quality by its bare
  // internal name, not typeDotName); plain "target.typeDotName" when there's
  // no quality wrapper. An unresolvable quality name is tolerated (falls back
  // to nullptr, i.e. RecipeParameters later treats it as Normal) rather than
  // reported as a load error — the target reference is still valid on its
  // own, and quality is a value-add overlay, not required for a usable table.
  template <typename T>
  std::pair<T*, Quality*> ResolveWithQuality(const std::string& reference) {
    if (reference.empty() || reference[0] != '!') {
      return {Resolve<T>(reference), nullptr};
    }
    size_t bang = reference.find('!', 1);
    if (bang == std::string::npos) return {Resolve<T>(reference), nullptr};
    T* target = Resolve<T>(reference.substr(1, bang - 1));
    Quality* quality =
        dynamic_cast<Quality*>(db_.FindByTypeDotName("Quality." + reference.substr(bang + 1)));
    return {target, quality};
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
void VisitCustomModule(RecipeRowCustomModule& m, V& v) {
  v.Prop("module", m.module);
  v.Prop("fixedCount", m.fixedCount);
}

template <typename V>
void VisitModuleTemplate(ModuleTemplate& t, V& v) {
  auto visitModule = [](RecipeRowCustomModule& m, auto& vv) { VisitCustomModule(m, vv); };
  v.PropStructList("list", t.list, visitModule);
  v.Prop("beacon", t.beacon);
  v.PropStructList("beaconList", t.beaconList, visitModule);
}

template <typename V>
void VisitModuleFiller(ModuleFillerParameters& f, V& v) {
  v.Prop("fillMiners", f.fillMiners);
  v.Prop("autoFillPayback", f.autoFillPayback);
  v.Prop("fillerModule", f.fillerModule);
  v.Prop("beacon", f.beacon);
  v.Prop("beaconModule", f.beaconModule);
  v.Prop("beaconsPerBuilding", f.beaconsPerBuilding);
  // Not ported: overrideCrafterBeacons (per-crafter beacon overrides).
}

template <typename V>
void VisitRow(RecipeRow& row, V& v) {
  // Upstream RecipeRow.recipe is IObjectWithQuality<RecipeOrTechnology>: the
  // "recipe" property is itself a quality-wrapped ref, with the quality being
  // this row's target/floor tier (RecipeRow::quality here, since this port
  // keeps `recipe` and `quality` as separate fields rather than one wrapper
  // type). Bridge through a temporary so both directions share one Prop() —
  // harmless no-op assignment back on the write path.
  ObjectWithQuality<RecipeOrTechnology> recipeWithQuality{row.recipe, row.quality};
  v.Prop("recipe", recipeWithQuality);
  row.recipe = recipeWithQuality.target;
  row.quality = recipeWithQuality.quality;
  v.Prop("fuel", row.fuel);
  v.Prop("entity", row.entity);
  v.Prop("fixedBuildings", row.fixedBuildings);
  v.Prop("builtBuildings", row.builtBuildings);
  v.Prop("enabled", row.enabled);
  v.Prop("variants", row.variants);
  auto visitTemplate = [](ModuleTemplate& t, auto& vv) { VisitModuleTemplate(t, vv); };
  if constexpr (V::kReading) {
    if (v.Has("modules")) v.PropObject("modules", row.modules, visitTemplate);
  } else {
    if (!row.modules.empty()) v.PropObject("modules", row.modules, visitTemplate);
  }
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
  // Page-level module defaults (upstream ProductionTable.modules); only the
  // root table's settings are consulted during Setup.
  auto visitFiller = [](ModuleFillerParameters& f, auto& vv) { VisitModuleFiller(f, vv); };
  v.PropObject("modules", table.settings.filler, visitFiller);
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
  // Desktop yafc may store a non-string icon; skip silently when so.
  if constexpr (V::kReading) {
    if (v.HasString("icon")) v.Prop("icon", page.icon);
  } else {
    v.Prop("icon", page.icon);
  }
  v.PropObject("content", page.content,
               [](ProductionTable& t, auto& vv) { VisitTable(t, vv); });
}

template <typename V>
void VisitSettings(ProjectSettings& settings, V& v) {
  v.Prop("milestones", settings.milestones);
  v.Prop("milestonesUnlocked", settings.milestonesUnlocked);
  v.Prop("miningProductivity", settings.miningProductivity);
  v.Prop("researchProductivity", settings.researchProductivity);
  v.Prop("productivityTechnologyLevels", settings.productivityTechnologyLevels);
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
