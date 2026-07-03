// THE golden test: parse the real Pyanodon corpus, deserialize to a Database,
// build the lead-plate production table from the reference desktop-yafc
// screenshot (900 lead plates/min via hot-air casting), solve it with the
// ported pipeline, and check every flow against the screenshot.
//
// Chain (all recipes discovered from the parsed data):
//   mining.lead-rock.lead-rock:  -> 1 ore-lead
//   grade-1-lead:        5 ore-lead -> 1 grade-1-lead
//   grade-2-lead-crusher: 2 grade-1 -> 1 stone (byproduct) + 1 grade-2
//   grade-2-lead:         2 grade-2 -> 0.5 grade-1 (recycle!) + 1 grade-3
//   grade-2-crush-lead:   1 grade-3 -> 1 lead-dust
//   molten-lead-01:       4 dust + 2 borax + 2 graphite -> 90 molten-lead
//   hotair-lead-plate-3:  100 molten + 1 sand-casting + 50 hot-air -> 63 plates
// Links: the lead goods chain; unlinked imports: borax, graphite, sand-casting,
// hot-air; unlinked byproduct: stone (the screenshot's "Extra products").
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "doctest/doctest.h"
#include "yafc/model/production_table.h"
#include "yafc/parser/data_deserializer.h"

using namespace yafc;
namespace fs = std::filesystem;

namespace {
std::string FindRoot() {
  fs::path dir = fs::current_path();
  for (int i = 0; i < 6; ++i) {
    if (fs::exists(dir / "data/factorio/mods/mod-list.json") &&
        fs::exists(dir / "data/factorio/data/base/info.json")) {
      return dir.string();
    }
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  return {};
}
const std::string kGoldenRoot = FindRoot();
}  // namespace

TEST_CASE("GOLDEN: lead plate table end-to-end on real Pyanodon data" *
          doctest::skip(kGoldenRoot.empty())) {
  FactorioDataSource source(kGoldenRoot + "/data/factorio/data",
                            kGoldenRoot + "/data/factorio/mods",
                            kGoldenRoot + "/third_party/yafc-ce/Yafc/Data");
  ParseResult parsed = source.Parse();
  LoadDataResult loaded = DataDeserializer::LoadData(*parsed.lua, parsed.factorioVersion);
  Database& db = *loaded.db;
  REQUIRE(loaded.errors.empty());

  auto goods = [&](const char* typeDotName) {
    auto* g = dynamic_cast<Goods*>(db.FindByTypeDotName(typeDotName));
    REQUIRE(g != nullptr);
    return g;
  };
  auto recipe = [&](const char* typeDotName) {
    auto* r = dynamic_cast<RecipeOrTechnology*>(db.FindByTypeDotName(typeDotName));
    REQUIRE(r != nullptr);
    return r;
  };

  ProductionTable table;
  // Demand: 900 lead plates per minute (solver units are per-minute here).
  table.AddLink({goods("Item.lead-plate"), nullptr}, 900);
  for (const char* link : {"Fluid.molten-lead", "Item.lead-dust", "Item.grade-3-lead",
                           "Item.grade-2-lead", "Item.grade-1-lead", "Item.ore-lead"}) {
    table.AddLink({goods(link), nullptr});
  }

  RecipeRow* cast = table.AddRecipe(recipe("Recipe.hotair-lead-plate-3"));
  RecipeRow* molten = table.AddRecipe(recipe("Recipe.molten-lead-01"));
  RecipeRow* dust = table.AddRecipe(recipe("Recipe.grade-2-crush-lead"));
  RecipeRow* grade3 = table.AddRecipe(recipe("Recipe.grade-2-lead"));
  RecipeRow* grade2 = table.AddRecipe(recipe("Recipe.grade-2-lead-crusher"));
  RecipeRow* grade1 = table.AddRecipe(recipe("Recipe.grade-1-lead"));
  RecipeRow* mining = table.AddRecipe(recipe("Mechanics.mining.lead-rock.lead-rock"));

  REQUIRE(table.Solve() == TableSolveResult::Ok);

  // Screenshot reference values (per minute).
  CHECK(cast->recipesPerSecond == doctest::Approx(900.0 / 63));         // 14.29 crafts
  CHECK(molten->recipesPerSecond == doctest::Approx(1428.57 / 90).epsilon(0.001));
  CHECK(dust->recipesPerSecond == doctest::Approx(63.49).epsilon(0.001));
  CHECK(grade3->recipesPerSecond == doctest::Approx(63.49).epsilon(0.001));
  CHECK(grade2->recipesPerSecond == doctest::Approx(126.98).epsilon(0.001));
  CHECK(grade1->recipesPerSecond == doctest::Approx(222.22).epsilon(0.001));
  CHECK(mining->recipesPerSecond == doctest::Approx(1111.11).epsilon(0.001));

  // Per-link flows match the screenshot column values.
  auto linkFlow = [&](const char* name) {
    ProductionLink* link = nullptr;
    REQUIRE(table.FindLink({goods(name), nullptr}, &link));
    CHECK((link->flags & LinkFlags::kLinkNotMatched) == 0);
    return link->linkFlow;
  };
  CHECK(linkFlow("Fluid.molten-lead") == doctest::Approx(1428.57).epsilon(0.001));
  CHECK(linkFlow("Item.lead-dust") == doctest::Approx(63.49).epsilon(0.001));
  CHECK(linkFlow("Item.grade-2-lead") == doctest::Approx(126.98).epsilon(0.001));
  CHECK(linkFlow("Item.ore-lead") == doctest::Approx(1111.11).epsilon(0.001));
  // grade-1 flow includes the recycle: 222.22 produced + 31.75 recycled = 254.
  CHECK(linkFlow("Item.grade-1-lead") == doctest::Approx(253.97).epsilon(0.001));

  // Unlinked flows: imports negative, the stone byproduct positive ("Extra
  // products: 127/m" in the screenshot).
  double stone = 0, sandCasting = 0, hotAir = 0, borax = 0;
  for (const ProductionTableFlow& flow : table.flow) {
    if (flow.goods.target->name == "stone") stone = flow.amount;
    if (flow.goods.target->name == "sand-casting") sandCasting = flow.amount;
    if (flow.goods.target->name == "hot-air") hotAir = flow.amount;
    if (flow.goods.target->name == "borax") borax = flow.amount;
  }
  CHECK(stone == doctest::Approx(126.98).epsilon(0.001));
  CHECK(sandCasting == doctest::Approx(-900.0 / 63).epsilon(0.001));
  CHECK(hotAir == doctest::Approx(-714.29).epsilon(0.001));
  CHECK(borax == doctest::Approx(-31.75).epsilon(0.001));

  std::printf("[golden] 900 plates/min: mine=%.2f g1=%.2f g2=%.2f g3=%.2f dust=%.2f "
              "molten=%.2f cast=%.2f stone=+%.2f\n",
              mining->recipesPerSecond, grade1->recipesPerSecond,
              grade2->recipesPerSecond, grade3->recipesPerSecond,
              dust->recipesPerSecond, molten->recipesPerSecond,
              cast->recipesPerSecond, stone);
}
