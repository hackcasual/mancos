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

namespace {

void PrintHelp(const char* program) {
  std::printf(
      "yafc-bundler — builds a Mancos .yafcbundle from Factorio game files\n"
      "\n"
      "usage: %s <factorio-data-path> <mods-path|vanilla> <env-lua-path> "
      "<out.yafcbundle>\n"
      "       %s -h | --help\n"
      "\n"
      "arguments:\n"
      "  factorio-data-path  Factorio's data/ directory (contains core/ and base/)\n"
      "  mods-path           your mods/ directory (contains mod-list.json), or the\n"
      "                      literal word 'vanilla' to bundle the base game only\n"
      "  env-lua-path        the bundler's Lua environment directory (Sandbox.lua,\n"
      "                      Defines*.lua — ships with releases as env/)\n"
      "  out.yafcbundle      output file the Mancos planner loads\n"
      "\n"
      "The bundle contains the parsed prototype database, analyses, per-object\n"
      "costs, extracted icons (32px max), and locale catalogs. Everything runs\n"
      "locally; nothing leaves your machine unless you share the bundle.\n"
      "\n"
      "example:\n"
      "  %s ~/factorio/data ~/factorio/mods env pyanodon.yafcbundle\n",
      program, program, program);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintHelp(argv[0]);
      return 0;
    }
  }
  if (argc != 5) {
    std::fprintf(stderr,
                 "usage: %s <factorio-data-path> <mods-path|vanilla> <env-lua-path> "
                 "<out.yafcbundle>\nrun with --help for details\n",
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

    std::vector<std::string> languages =
        FindLocaleLanguages(*parsed.modSet, parsed.modLoadOrder);
    std::fprintf(stderr, "[locale] %zu languages\n", languages.size());
    std::map<std::string, std::map<std::string, std::string>> locales;
    for (const std::string& lang : languages) {
      LocaleCatalog catalog =
          LoadLocale(*parsed.modSet, parsed.modLoadOrder, lang, envPath);
      locales[lang] = {catalog.begin(), catalog.end()};
    }
    // Bake English as the dump default; the app re-applies per language.
    if (auto it = locales.find("en"); it != locales.end()) {
      LocaleCatalog en(it->second.begin(), it->second.end());
      ApplyLocale(*loaded.db, en);
    }

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
                    parsed.factorioVersion.ToString3(), modVersions, &costs,
                    &locales);
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0);

    std::printf("bundle written: %s\n", outPath.c_str());
    std::printf("  objects:   %zu (database %.1f MB cbor)\n", stats.objects,
                stats.databaseBytes / 1e6);
    std::printf("  icons:     %zu files, %.1f MB (%zu unresolvable)\n",
                stats.iconFiles, stats.iconBytes / 1e6, stats.missingIcons);
    std::printf("  languages: %zu\n", locales.size());
    std::printf("  mods:      %zu, factorio %s\n", modVersions.size(),
                parsed.factorioVersion.ToString3().c_str());
    std::printf("  elapsed:   %.1fs\n", seconds.count());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bundler failed: %s\n", e.what());
    return 1;
  }
}
