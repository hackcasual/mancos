#include "yafc/model/blueprint.h"

#include <algorithm>
#include <cmath>

#include <miniz.h>

namespace yafc {

namespace {

// Upstream Blueprint.VERSION: the 2.0 game format marker.
constexpr int64_t kBlueprintVersion = 562949956632577;

std::string Base64(const unsigned char* data, size_t length) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((length + 2) / 3 * 4);
  for (size_t i = 0; i < length; i += 3) {
    uint32_t chunk = data[i] << 16;
    if (i + 1 < length) chunk |= data[i + 1] << 8;
    if (i + 2 < length) chunk |= data[i + 2];
    out.push_back(kAlphabet[(chunk >> 18) & 63]);
    out.push_back(kAlphabet[(chunk >> 12) & 63]);
    out.push_back(i + 1 < length ? kAlphabet[(chunk >> 6) & 63] : '=');
    out.push_back(i + 2 < length ? kAlphabet[chunk & 63] : '=');
  }
  return out;
}

// Upstream EntityCrafter.BlueprintModuleInventory (defines.inventory.*).
int ModuleInventoryOf(const Entity& entity) {
  if (entity.factorioType == "mining-drill") return 2;
  if (entity.factorioType == "lab") return 3;
  return 4;  // crafter_modules
}

int BuildingCopies(const RecipeRow& row, const BlueprintOptions& options,
                   BlueprintResult& stats) {
  // A hair under the ceiling is treated as exact (18.0000001 -> 18).
  int copies = static_cast<int>(std::ceil(row.buildingCount() - 1e-6f));
  if (copies < 1 && row.recipesPerSecond > 0) copies = 1;
  if (copies > options.maxBuildingsPerRow) {
    copies = options.maxBuildingsPerRow;
    stats.truncatedRows += 1;
  }
  return copies;
}

}  // namespace

nlohmann::json BuildBlueprintJson(const std::vector<const RecipeRow*>& rows,
                                  const BlueprintOptions& options,
                                  BlueprintResult& stats) {
  using nlohmann::json;

  struct Lane {
    const RecipeRow* row;
    int copies;
  };
  std::vector<Lane> lanes;
  double totalArea = 0;
  int widest = 1;
  for (const RecipeRow* row : rows) {
    if (row == nullptr || row->recipe == nullptr) continue;
    if (row->entity.target == nullptr) continue;
    int copies = BuildingCopies(*row, options, stats);
    if (copies <= 0) continue;
    const Entity& e = *row->entity.target;
    totalArea += static_cast<double>((e.width + 1) * (e.height + 1)) * copies;
    widest = std::max(widest, e.width + 1);
    lanes.push_back({row, copies});
  }

  // Square-ish target width (upstream's estimate), but never narrower than
  // the widest single building.
  int targetWidth =
      std::max(widest, static_cast<int>(std::ceil(std::sqrt(totalArea))));

  json entities = json::array();
  int index = 1;  // entity_number is 1-based
  int cursorY = 0, totalWidth = 0;

  // One shelf sequence per recipe row: copies run left-to-right and wrap at
  // targetWidth; the next recipe row always starts a fresh shelf. Keeping a
  // row's buildings contiguous beats globally tighter packing — the point
  // of the blueprint is stampable per-recipe building sets.
  for (const Lane& lane : lanes) {
    const RecipeRow& row = *lane.row;
    const Entity& proto = *row.entity.target;
    int x = 0, shelfHeight = 0;
    for (int i = 0; i < lane.copies; ++i) {
      if (x + proto.width > targetWidth && x > 0) {
        cursorY += shelfHeight + 1;
        x = 0;
        shelfHeight = 0;
      }
      json entity{{"entity_number", index++},
                  {"name", proto.name},
                  // Factorio positions are entity centers; half-tile centers
                  // for odd sizes keep every top-left on the integer grid.
                  {"position", {{"x", x + proto.width / 2.0},
                                {"y", cursorY + proto.height / 2.0}}},
                  {"direction", 0}};
      if (row.entity.quality != nullptr && row.entity.quality->level != 0) {
        entity["quality"] = row.entity.quality->name;
      }
      // Mechanics (mining/boiling/... pseudo-recipes) and technologies have
      // no in-game recipe to set (upstream rule; Mechanics derives from
      // Recipe, so it must be excluded explicitly).
      if (dynamic_cast<const Recipe*>(row.recipe) != nullptr &&
          dynamic_cast<const Mechanics*>(row.recipe) == nullptr) {
        entity["recipe"] = row.recipe->name;
        if (row.quality != nullptr) entity["recipe_quality"] = row.quality->name;
      }
      if (options.includeFuel && row.fuel && !row.fuel.target->isPower() &&
          dynamic_cast<const Item*>(row.fuel.target) != nullptr) {
        json filter{{"index", 1}, {"comparator", "="},
                    {"name", row.fuel.target->name}, {"count", 1}};
        if (row.fuel.quality != nullptr) filter["quality"] = row.fuel.quality->name;
        entity["burner_fuel_inventory"] = json{{"filters", json::array({filter})}};
      }
      if (!row.parameters.usedModules.empty()) {
        json items = json::array();
        int stack = 0;
        int inventory = ModuleInventoryOf(proto);
        for (const RecipeRowCustomModule& m : row.parameters.usedModules) {
          if (m.module == nullptr || m.fixedCount <= 0) continue;
          json inInventory = json::array();
          for (int s = 0; s < m.fixedCount; ++s) {
            inInventory.push_back(json{{"inventory", inventory}, {"stack", stack++}});
          }
          items.push_back(json{{"id", {{"name", m.module->name}}},
                               {"items", {{"in_inventory", std::move(inInventory)}}}});
        }
        if (!items.empty()) entity["items"] = std::move(items);
      }
      entities.push_back(std::move(entity));
      x += proto.width + 1;
      totalWidth = std::max(totalWidth, x);
      shelfHeight = std::max(shelfHeight, proto.height);
    }
    cursorY += shelfHeight + 1;
  }

  stats.buildings = index - 1;
  stats.width = totalWidth > 0 ? totalWidth - 1 : 0;  // trailing gap trimmed
  stats.height = cursorY > 0 ? cursorY - 1 : 0;

  return json{{"blueprint", {{"item", "blueprint"},
                             {"label", options.label},
                             {"entities", std::move(entities)},
                             {"icons", json::array()},
                             {"version", kBlueprintVersion}}}};
}

std::string EncodeBlueprintString(const std::string& text) {
  mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(text.size()));
  std::vector<unsigned char> compressed(bound);
  mz_ulong outLength = bound;
  if (mz_compress2(compressed.data(), &outLength,
                   reinterpret_cast<const unsigned char*>(text.data()),
                   static_cast<mz_ulong>(text.size()),
                   MZ_BEST_COMPRESSION) != MZ_OK) {
    return {};
  }
  return "0" + Base64(compressed.data(), outLength);
}

BlueprintResult ExportBlueprint(const std::vector<const RecipeRow*>& rows,
                                const BlueprintOptions& options) {
  BlueprintResult result;
  nlohmann::json blueprint = BuildBlueprintJson(rows, options, result);
  if (result.buildings == 0) {
    result.error = "no placeable buildings — rows need a solved rate and a chosen crafter";
    return result;
  }
  result.blueprintString = EncodeBlueprintString(blueprint.dump());
  if (result.blueprintString.empty()) result.error = "compression failed";
  return result;
}

}  // namespace yafc
