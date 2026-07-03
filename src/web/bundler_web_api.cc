// The wasm-core API for the browser bundler page (PLAN Phase 4.5, split-app
// architecture). Runs inside a dedicated Web Worker (web/bundler-worker.js);
// the page grants it read access to the user's Factorio data/mods folders
// via the File System Access API, mounted into this module's virtual
// filesystem as WORKERFS (web/bundler_pre.js: Module.mountFS). Same pipeline
// as src/tools/bundler_main.cc (the native/CI bundler), packaged as embind
// bindings instead of a CLI main().
//
// The yafc env files (Sandbox.lua, Defines*.lua, Postprocess*.lua, locale/)
// are baked into this binary at link time (CMakeLists.txt --embed-file) at
// /env — they are yafc's own data, not the user's, so the page never asks
// for them.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <nlohmann/json.hpp>

#include "yafc/analysis/automation_analysis.h"
#include "yafc/analysis/cost_analysis.h"
#include "yafc/parser/data_deserializer.h"
#include "yafc/parser/locale.h"
#include "yafc/serialization/database_dump.h"

using namespace yafc;
using nlohmann::json;

namespace {

// Holds the most recently produced bundle so bundleBytes() can hand it to
// JS as a memory view (mirrors web_api.cc's iconFile convention).
std::string g_lastBundle;

std::string Err(const std::string& message) {
  return json{{"error", message}}.dump();
}

}  // namespace

// dataPath/modsPath: WORKERFS mount points for the user's selected folders
// (modsPath == "" means vanilla, matching bundler_main.cc's CLI contract).
// progress: JS function(step: string) called as each stage starts.
static std::string runBundler(std::string dataPath, std::string modsPath,
                              emscripten::val progress) {
  const std::string envPath = "/env";
  auto report = [&](const std::string& step) { progress(step); };
  try {
    FactorioDataSource source(dataPath, modsPath, envPath);
    ParseResult parsed = source.Parse(report);

    LoadDataResult loaded =
        DataDeserializer::LoadData(*parsed.lua, parsed.factorioVersion, false, report);

    std::vector<std::string> languages =
        FindLocaleLanguages(*parsed.modSet, parsed.modLoadOrder);
    report("locale: " + std::to_string(languages.size()) + " languages");
    std::map<std::string, std::map<std::string, std::string>> locales;
    for (const std::string& lang : languages) {
      LocaleCatalog catalog =
          LoadLocale(*parsed.modSet, parsed.modLoadOrder, lang, envPath);
      locales[lang] = {catalog.begin(), catalog.end()};
    }
    // Bake English as the dump default; the main app re-applies per language.
    if (auto it = locales.find("en"); it != locales.end()) {
      LocaleCatalog en(it->second.begin(), it->second.end());
      ApplyLocale(*loaded.db, en);
    }

    report("analysis: dependencies/milestones/automation/cost");
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

    report("writing bundle");
    const std::string outPath = "/tmp/bundle.yafcbundle";
    BundleWriteStats stats =
        WriteBundle(outPath, *loaded.db, *parsed.modSet,
                    parsed.factorioVersion.ToString3(), modVersions, &costs,
                    &locales);

    std::ifstream in(outPath, std::ios::binary);
    g_lastBundle.assign(std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>());
    std::remove(outPath.c_str());

    json warnings = json::array();
    for (const std::string& e : loaded.errors) warnings.push_back(e);

    return json{{"objects", stats.objects},
                {"databaseBytes", stats.databaseBytes},
                {"iconFiles", stats.iconFiles},
                {"iconBytes", stats.iconBytes},
                {"missingIcons", stats.missingIcons},
                {"languages", locales.size()},
                {"mods", modVersions},
                {"factorioVersion", parsed.factorioVersion.ToString3()},
                {"bundleBytes", g_lastBundle.size()},
                {"warnings", std::move(warnings)}}
        .dump();
  } catch (const std::exception& e) {
    return Err(e.what());
  }
}

static emscripten::val bundleBytes() {
  return emscripten::val(emscripten::typed_memory_view(
      g_lastBundle.size(), reinterpret_cast<const uint8_t*>(g_lastBundle.data())));
}

EMSCRIPTEN_BINDINGS(yafc_bundler_web) {
  emscripten::function("runBundler", &runBundler);
  emscripten::function("bundleBytes", &bundleBytes);
}
