// Factorio locale .cfg handling (bundler-side): parses per-mod
// locale/<lang>/*.cfg files (INI-like: [section] key=value) in mod load order
// (later mods override), plus yafc's own env locale, and applies localized
// names/descriptions onto the Database before it is dumped into a bundle —
// so the main app renders human names with zero client-side i18n machinery.
// (Multi-language switching later ships raw catalogs in the bundle instead.)
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "yafc/model/database.h"
#include "yafc/parser/factorio_data_source.h"

namespace yafc {

// "section.key" -> text, e.g. "item-name.iron-plate" -> "Iron plate".
using LocaleCatalog = std::unordered_map<std::string, std::string>;

void ParseLocaleCfg(const std::string& text, LocaleCatalog& into);

LocaleCatalog LoadLocale(const ModSet& mods, const std::vector<std::string>& modOrder,
                         const std::string& language, const std::string& envPath);

// Sets locName/locDescr using Factorio's implicit-key rules:
// <kind>-name.<name> with fallbacks (items fall back to their placed entity,
// recipes to their main product, split fluids use their original name);
// Mechanics recipes compose "<verb> <source>" from their localization key.
void ApplyLocale(Database& db, const LocaleCatalog& catalog);

}  // namespace yafc
