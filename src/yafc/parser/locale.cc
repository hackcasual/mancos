#include "yafc/parser/locale.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace yafc {

void ParseLocaleCfg(const std::string& text, LocaleCatalog& into) {
  std::string section;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string_view line(text.data() + pos, end - pos);
    pos = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = std::string(line.substr(1, line.size() - 2));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = section.empty()
                          ? std::string(line.substr(0, eq))
                          : section + "." + std::string(line.substr(0, eq));
    into[key] = std::string(line.substr(eq + 1));
  }
}

LocaleCatalog LoadLocale(const ModSet& mods, const std::vector<std::string>& modOrder,
                         const std::string& language, const std::string& envPath) {
  LocaleCatalog catalog;
  for (const std::string& mod : modOrder) {
    for (const std::string& file : mods.GetAllModFiles(mod, "locale/" + language + "/")) {
      if (file.size() < 4 || file.compare(file.size() - 4, 4, ".cfg") != 0) continue;
      ParseLocaleCfg(mods.ReadModFile(mod, file), catalog);
    }
  }
  // yafc's own strings (special objects etc.).
  fs::path envLocale = fs::path(envPath) / "locale" / language;
  if (fs::exists(envLocale)) {
    for (const auto& entry : fs::directory_iterator(envLocale)) {
      if (entry.path().extension() != ".cfg") continue;
      std::ifstream in(entry.path(), std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      ParseLocaleCfg(ss.str(), catalog);
    }
  }
  return catalog;
}

std::vector<std::string> FindLocaleLanguages(
    const ModSet& mods, const std::vector<std::string>& modOrder) {
  std::vector<std::string> languages;
  for (const std::string& mod : modOrder) {
    for (const std::string& file : mods.GetAllModFiles(mod, "locale/")) {
      // "locale/<lang>/<file>.cfg"
      size_t start = std::string("locale/").size();
      size_t slash = file.find('/', start);
      if (slash == std::string::npos) continue;
      std::string lang = file.substr(start, slash - start);
      if (std::find(languages.begin(), languages.end(), lang) == languages.end()) {
        languages.push_back(lang);
      }
    }
  }
  std::sort(languages.begin(), languages.end());
  return languages;
}

void ResetLocale(Database& db) {
  for (FactorioObject* o : db.objects) {
    o->locName = o->name;
    o->locDescr.clear();
  }
}

namespace {

const std::string* Find(const LocaleCatalog& catalog, const std::string& key) {
  auto it = catalog.find(key);
  return it == catalog.end() ? nullptr : &it->second;
}

// Try "<section>-name.<name>" (+ description sibling).
bool Apply(const LocaleCatalog& catalog, FactorioObject* o,
           const std::string& section, const std::string& name) {
  const std::string* text = Find(catalog, section + "-name." + name);
  if (text == nullptr) return false;
  o->locName = *text;
  if (const std::string* descr = Find(catalog, section + "-description." + name)) {
    o->locDescr = *descr;
  }
  return true;
}

}  // namespace

void ApplyLocale(Database& db, const LocaleCatalog& catalog) {
  // First pass: direct keys per kind.
  for (FactorioObject* o : db.objects) {
    if (auto* f = dynamic_cast<Fluid*>(o)) {
      std::string base = f->originalName.empty() ? f->name : f->originalName;
      if (Apply(catalog, o, "fluid", base) && f->name != base) {
        // Preserve the temperature suffix on split fluids: "Molten lead (850°)".
        size_t at = f->name.rfind('@');
        if (at != std::string::npos) {
          o->locName += " (" + f->name.substr(at + 1) + "°)";
        }
      }
      continue;
    }
    if (dynamic_cast<Item*>(o) != nullptr) {
      if (!Apply(catalog, o, "item", o->name) &&
          !Apply(catalog, o, "entity", o->name) &&
          !Apply(catalog, o, "equipment", o->name)) {
        // Placed-entity fallback below (needs placeResult, second pass).
      }
      continue;
    }
    if (dynamic_cast<Mechanics*>(o) != nullptr) continue;  // composed later
    if (dynamic_cast<Technology*>(o) != nullptr) {
      if (!Apply(catalog, o, "technology", o->name)) {
        // "tech-name-3" -> "tech-name" (leveled technologies).
        size_t dash = o->name.rfind('-');
        if (dash != std::string::npos &&
            o->name.find_first_not_of("0123456789", dash + 1) == std::string::npos) {
          if (Apply(catalog, o, "technology", o->name.substr(0, dash))) {
            o->locName += " " + o->name.substr(dash + 1);
          }
        }
      }
      continue;
    }
    if (dynamic_cast<Recipe*>(o) != nullptr) {
      Apply(catalog, o, "recipe", o->name);  // fallback to product in pass 2
      continue;
    }
    if (dynamic_cast<Entity*>(o) != nullptr) {
      Apply(catalog, o, "entity", o->name);
      continue;
    }
    if (dynamic_cast<Quality*>(o) != nullptr) {
      Apply(catalog, o, "quality", o->name);
      continue;
    }
    if (dynamic_cast<Location*>(o) != nullptr) {
      if (!Apply(catalog, o, "space-location", o->name)) {
        Apply(catalog, o, "planet", o->name);
      }
      continue;
    }
    if (dynamic_cast<Tile*>(o) != nullptr) {
      Apply(catalog, o, "tile", o->name);
      continue;
    }
  }

  // Second pass: fallbacks that depend on other objects' resolved names.
  for (Item* item : db.items) {
    if (item->locName == item->name && item->placeResult != nullptr &&
        item->placeResult->locName != item->placeResult->name) {
      item->locName = item->placeResult->locName;
    }
  }
  for (Recipe* recipe : db.recipes) {
    if (recipe->locName != recipe->name) continue;
    Goods* product = recipe->mainProduct != nullptr
                         ? recipe->mainProduct
                         : (recipe->products.size() == 1 ? recipe->products[0].goods
                                                         : nullptr);
    if (product != nullptr && product->locName != product->name) {
      recipe->locName = product->locName;
    }
  }
  // Mechanics: "<verb> <source>" from the localization tag.
  for (Mechanics* m : db.mechanics) {
    static const std::unordered_map<std::string, std::string> kVerbs = {
        {"mining", "Mining"},     {"boiling", "Boiling"},
        {"pump", "Pumping"},      {"launched", "Launch of"},
        {"launch", "Launch of"},  {"spoil", "Spoiling of"},
        {"generator", "Power from"}, {"decompose", "Decomposition of"},
    };
    auto verb = kVerbs.find(m->localizationKey);
    std::string sourceName;
    if (m->source != nullptr && m->source->locName != m->source->name) {
      sourceName = m->source->locName;
    } else if (m->mainProduct != nullptr) {
      sourceName = m->mainProduct->locName;
    }
    if (!sourceName.empty()) {
      m->locName = (verb != kVerbs.end() ? verb->second : "Processing of") + " " +
                   sourceName;
    }
  }
}

}  // namespace yafc
