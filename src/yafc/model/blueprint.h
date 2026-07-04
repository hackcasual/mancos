// Factorio blueprint-string export (port of Yafc.Blueprints — Blueprint.cs
// model + an auto-layout inspired by BlueprintUtilities.ExportRecipiesAsBlueprint,
// extended to place SETS of buildings: each recipe row contributes
// ceil(buildingCount) copies with the recipe (and its quality), modules and
// optionally fuel configured, so the string stamps a working block of the
// solved factory rather than one sample building per recipe).
//
// String format (2.0 game format, upstream BlueprintString.ToBpString):
// "0" + base64(zlib(json)) — the zlib stream is a standard 0x78-header +
// deflate + adler32, which miniz's mz_compress2 emits whole.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "yafc/model/production_table.h"

namespace yafc {

struct BlueprintOptions {
  std::string label = "mancos";
  bool includeFuel = true;
  // Safety valve: a solve can legitimately ask for thousands of buildings;
  // a blueprint that size is unusable and the string huge. Rows are capped
  // to this many placed copies (the result reports what was truncated).
  int maxBuildingsPerRow = 200;
};

struct BlueprintResult {
  std::string blueprintString;  // "0..." game-format string; empty on failure
  std::string error;            // set when blueprintString is empty
  int buildings = 0;            // total entities placed
  int truncatedRows = 0;        // rows that hit maxBuildingsPerRow
  int width = 0, height = 0;    // layout bounding box, tiles
};

// Builds the blueprint JSON (upstream Blueprint.cs shape) for the given
// solved rows and encodes it. Rows without a chosen entity or with a zero
// building count are skipped; Mechanics pseudo-recipes get no recipe field
// (upstream rule — they're not real placeable recipes).
BlueprintResult ExportBlueprint(const std::vector<const RecipeRow*>& rows,
                                const BlueprintOptions& options);

// Exposed for tests: the raw blueprint JSON before encoding.
nlohmann::json BuildBlueprintJson(const std::vector<const RecipeRow*>& rows,
                                  const BlueprintOptions& options,
                                  BlueprintResult& stats);

// "0" + base64(zlib(text)) — the game string wrapper.
std::string EncodeBlueprintString(const std::string& json);

}  // namespace yafc
