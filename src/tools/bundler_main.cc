// yafc-bundler: the filesystem-facing half of the split app (PLAN Phase 4.5).
// Reads the user's game data + mods, runs the full parse/deserialize pipeline,
// and emits a self-contained bundle the main app can load without ever
// touching raw game files.
//
//   yafc-bundler <factorio-data-path> <mods-path-or-"vanilla"> <env-lua-path> <out.yafcbundle>
#include <chrono>
#include <cstdio>
#include <string>

#include "yafc/analysis/automation_analysis.h"
#include "yafc/analysis/cost_analysis.h"
#include "yafc/parser/data_deserializer.h"
#include "yafc/parser/locale.h"
#include "yafc/serialization/database_dump.h"

using namespace yafc;

int main(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr,
                 "usage: %s <factorio-data-path> <mods-path|vanilla> <env-lua-path> "
                 "<out.yafcbundle>\n",
                 argv[0]);
    return 2;
  }
  std::string factorioPath = argv[1];
  std::string modPath = argv[2] == std::string("vanilla") ? "" : argv[2];
  std::string envPath = argv[3];
  std::string outPath = argv[4];

  try {
    auto t0 = std::chrono::steady_clock::now();
    FactorioDataSource source(factorioPath, modPath, envPath);
    ParseResult parsed = source.Parse([](const std::string& step) {
      std::fprintf(stderr, "[parse] %s\n", step.c_str());
    });

    LoadDataResult loaded = DataDeserializer::LoadData(
        *parsed.lua, parsed.factorioVersion, false, [](const std::string& step) {
          std::fprintf(stderr, "[load] %s\n", step.c_str());
        });
    for (const std::string& error : loaded.errors) {
      std::fprintf(stderr, "[warn] %s\n", error.c_str());
    }

    std::fprintf(stderr, "[locale] applying en names\n");
    LocaleCatalog locale =
        LoadLocale(*parsed.modSet, parsed.modLoadOrder, "en", envPath);
    ApplyLocale(*loaded.db, locale);

    std::fprintf(stderr, "[analysis] dependencies/milestones/automation/cost\n");
    Dependencies deps;
    deps.Calculate(*loaded.db);
    Milestones milestones;
    milestones.Compute(*loaded.db, deps, {});
    AutomationAnalysis automation;
    automation.Compute(*loaded.db, deps, milestones);
    CostAnalysisInput costInput;
    costInput.access = HooksFromAnalyses(milestones, automation);
    CostAnalysis costAnalysis(false);
    costAnalysis.Compute(*loaded.db, costInput);
    std::vector<float> costs(costAnalysis.cost.values().begin(),
                             costAnalysis.cost.values().end());

    std::map<std::string, std::string> modVersions;
    for (const auto& [name, info] : parsed.modSet->mods) {
      modVersions[name] = info->parsedVersion.ToString3();
    }

    BundleWriteStats stats =
        WriteBundle(outPath, *loaded.db, *parsed.modSet,
                    parsed.factorioVersion.ToString3(), modVersions, &costs);
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0);

    std::printf("bundle written: %s\n", outPath.c_str());
    std::printf("  objects:   %zu (database %.1f MB cbor)\n", stats.objects,
                stats.databaseBytes / 1e6);
    std::printf("  icons:     %zu files, %.1f MB (%zu unresolvable)\n",
                stats.iconFiles, stats.iconBytes / 1e6, stats.missingIcons);
    std::printf("  mods:      %zu, factorio %s\n", modVersions.size(),
                parsed.factorioVersion.ToString3().c_str());
    std::printf("  elapsed:   %.1fs\n", seconds.count());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bundler failed: %s\n", e.what());
    return 1;
  }
}
