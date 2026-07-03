// Round-trip and undo tests for the .yafc-shaped project serialization.
#include "yafc/serialization/serialization.h"

#include <memory>
#include <vector>

#include "doctest/doctest.h"

using namespace yafc;

namespace {

template <typename T>
T* Add(std::vector<std::unique_ptr<FactorioObject>>& objects, const char* name) {
  auto obj = std::make_unique<T>();
  obj->name = name;
  T* raw = obj.get();
  objects.push_back(std::move(obj));
  return raw;
}

struct SerWorld {
  Database db;
  Item* ore;
  Item* plate;
  Recipe* mine;
  Recipe* smelt;

  SerWorld() {
    std::vector<std::unique_ptr<FactorioObject>> objects;
    ore = Add<Item>(objects, "ore");
    plate = Add<Item>(objects, "plate");
    mine = Add<Recipe>(objects, "mine");
    smelt = Add<Recipe>(objects, "smelt");
    mine->products.emplace_back(ore, 1.0f);
    smelt->ingredients.emplace_back(ore, 1.0f);
    smelt->products.emplace_back(plate, 1.0f);
    db.LoadBuiltData(std::move(objects));
  }

  std::unique_ptr<Project> MakeProject() const {
    auto project = std::make_unique<Project>();
    project->settings.milestones = {plate};

    auto page = std::make_unique<ProjectPage>();
    page->guid = "0123456789abcdef";
    page->name = "Plates";
    ProductionTable& table = page->content;
    table.AddLink({plate, nullptr}, 30);
    ProductionLink* overProduce = table.AddLink({ore, nullptr});
    overProduce->algorithm = LinkAlgorithm::AllowOverProduction;

    RecipeRow* header = table.AddNestedTable(smelt);
    header->fixedBuildings = 2.5f;
    header->builtBuildings = 4;
    header->subgroup->AddLink({ore, nullptr});
    RecipeRow* mineRow = header->subgroup->AddRecipe(mine);
    mineRow->enabled = false;

    project->pages.push_back(std::move(page));
    return project;
  }
};

}  // namespace

TEST_CASE("project round-trips through .yafc-shaped JSON") {
  SerWorld w;
  auto project = w.MakeProject();

  std::string text = SaveProjectToString(*project);
  LoadResult loaded = LoadProjectFromString(text, w.db);
  REQUIRE(loaded.project != nullptr);
  CHECK(loaded.errors.empty());

  Project& p = *loaded.project;
  REQUIRE(p.settings.milestones.size() == 1);
  CHECK(p.settings.milestones[0] == w.plate);
  REQUIRE(p.pages.size() == 1);
  ProjectPage& page = *p.pages[0];
  CHECK(page.guid == "0123456789abcdef");
  CHECK(page.name == "Plates");

  ProductionTable& table = page.content;
  REQUIRE(table.links.size() == 2);
  CHECK(table.links[0]->goods.target == w.plate);
  CHECK(table.links[0]->amount == doctest::Approx(30));
  CHECK(table.links[1]->algorithm == LinkAlgorithm::AllowOverProduction);

  REQUIRE(table.recipes.size() == 1);
  RecipeRow& header = *table.recipes[0];
  CHECK(header.recipe == w.smelt);
  CHECK(header.fixedBuildings == doctest::Approx(2.5f));
  CHECK(header.builtBuildings.has_value());
  CHECK(*header.builtBuildings == doctest::Approx(4));
  REQUIRE(header.subgroup != nullptr);
  CHECK(header.subgroup->owner == &header);
  REQUIRE(header.subgroup->recipes.size() == 1);
  CHECK(header.subgroup->recipes[0]->recipe == w.mine);
  CHECK(!header.subgroup->recipes[0]->enabled);
  CHECK(header.subgroup->recipes[0]->owner == header.subgroup.get());

  // The reloaded project must be solvable straight away.
  header.subgroup->recipes[0]->enabled = true;
  header.fixedBuildings = 0;
  REQUIRE(table.Solve() == TableSolveResult::Ok);
  CHECK(header.recipesPerSecond == doctest::Approx(30));
}

TEST_CASE("unknown properties and unresolvable references degrade gracefully") {
  SerWorld w;
  const char* text = R"({
    "settings": {"milestones": ["Item.plate", "Item.does-not-exist"],
                 "someFutureSetting": 42},
    "pages": [{
      "name": "P",
      "unknownThing": {"nested": true},
      "content": {
        "links": [{"goods": "Item.plate", "amount": 10, "spoilerField": 1}],
        "recipes": [{"recipe": "Recipe.smelt", "modules": {"list": []}}]
      }
    }]
  })";
  LoadResult loaded = LoadProjectFromString(text, w.db);
  REQUIRE(loaded.project != nullptr);

  // The unresolvable milestone is reported and skipped; the rest loads.
  REQUIRE(loaded.errors.size() == 1);
  CHECK(loaded.errors[0] == "unresolved:Item.does-not-exist");
  CHECK(loaded.project->settings.milestones == std::vector<FactorioObject*>{w.plate});
  REQUIRE(loaded.project->pages.size() == 1);
  CHECK(loaded.project->pages[0]->content.links[0]->amount == doctest::Approx(10));
  CHECK(loaded.project->pages[0]->content.recipes[0]->recipe == w.smelt);
}

TEST_CASE("desktop quality-wrapped references resolve") {
  SerWorld w;
  const char* text = R"({
    "pages": [{
      "name": "P",
      "icon": {"some": "object"},
      "content": {
        "links": [{"goods": "!Item.plate!normal", "amount": 0.5}],
        "recipes": [{"recipe": "!Recipe.smelt!rare"}]
      }
    }]
  })";
  LoadResult loaded = LoadProjectFromString(text, w.db);
  REQUIRE(loaded.project != nullptr);
  CHECK(loaded.errors.empty());  // icon object tolerated silently
  ProductionTable& table = loaded.project->pages[0]->content;
  REQUIRE(table.links.size() == 1);
  CHECK(table.links[0]->goods.target == w.plate);
  CHECK(table.links[0]->amount == doctest::Approx(0.5));
  REQUIRE(table.recipes.size() == 1);
  CHECK(table.recipes[0]->recipe == w.smelt);
}

TEST_CASE("undo and redo restore project snapshots") {
  SerWorld w;
  auto project = w.MakeProject();
  UndoSystem undo(&w.db);

  undo.RecordSnapshot(*project);
  project->pages[0]->name = "Renamed";
  project->pages[0]->content.links[0]->amount = 60;

  auto restored = undo.Undo(*project);
  REQUIRE(restored != nullptr);
  CHECK(restored->pages[0]->name == "Plates");
  CHECK(restored->pages[0]->content.links[0]->amount == doctest::Approx(30));
  CHECK(undo.CanRedo());

  auto redone = undo.Redo(*restored);
  REQUIRE(redone != nullptr);
  CHECK(redone->pages[0]->name == "Renamed");
  CHECK(redone->pages[0]->content.links[0]->amount == doctest::Approx(60));
}
