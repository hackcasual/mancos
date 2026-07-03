// Port of Yafc.Parser/FactorioPropertyTree.cs — the binary property-tree
// format used by mod-settings.dat. Values are materialized directly as Lua
// values in the given context (dictionaries/lists become tables).
#pragma once

#include <optional>
#include <string>

#include "yafc/parser/lua_context.h"

namespace yafc {

// Parses mod-settings.dat content; returns a registry ref to the settings
// table, or nullopt when the version/format is unsupported.
std::optional<int> ReadModSettings(const std::string& bytes, LuaContext& lua);

}  // namespace yafc
