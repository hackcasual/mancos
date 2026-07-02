#include "yafc/model/database.h"

#include <algorithm>
#include <stdexcept>

namespace yafc {

void Database::LoadBuiltData(
    std::vector<std::unique_ptr<FactorioObject>> allObjects,
    const std::unordered_map<std::string, FactorioObject*>& formerAliases) {
  storage_ = std::move(allObjects);

  // Upstream CalculateMaps sorts by sortingOrder before LoadBuiltData; the
  // sort must be stable so ids stay deterministic for identical inputs.
  std::stable_sort(storage_.begin(), storage_.end(),
                   [](const auto& a, const auto& b) {
                     return a->sortingOrder() < b->sortingOrder();
                   });

  std::vector<FactorioObject*> sorted;
  sorted.reserve(storage_.size());
  for (size_t i = 0; i < storage_.size(); ++i) {
    storage_[i]->id = static_cast<FactorioId>(i);
    if (storage_[i]->locName.empty()) storage_[i]->locName = storage_[i]->name;
    sorted.push_back(storage_[i].get());
  }

  objectsByTypeName.clear();
  for (FactorioObject* o : sorted) {
    if (!objectsByTypeName.emplace(o->typeDotName(), o).second) {
      throw std::runtime_error("duplicate typeDotName: " + o->typeDotName());
    }
  }
  for (const auto& [alias, target] : formerAliases) {
    objectsByTypeName.emplace(alias, target);  // TryAdd: keep existing entries
  }

  auto skip = [&](int from, FactorioObjectSortOrder order) {
    while (from < static_cast<int>(sorted.size()) &&
           sorted[from]->sortingOrder() == order) {
      ++from;
    }
    return from;
  };

  int firstSpecial = 0;
  int firstItem = skip(firstSpecial, FactorioObjectSortOrder::SpecialGoods);
  int firstFluid = skip(firstItem, FactorioObjectSortOrder::Items);
  int firstRecipe = skip(firstFluid, FactorioObjectSortOrder::Fluids);
  int firstMechanics = skip(firstRecipe, FactorioObjectSortOrder::Recipes);
  int firstTechnology = skip(firstMechanics, FactorioObjectSortOrder::Mechanics);
  int firstEntity = skip(firstTechnology, FactorioObjectSortOrder::Technologies);
  int firstTile = skip(firstEntity, FactorioObjectSortOrder::Entities);
  int firstQuality = skip(firstTile, FactorioObjectSortOrder::Tiles);
  int firstLocation = skip(firstQuality, FactorioObjectSortOrder::Qualities);
  int firstTrigger = skip(firstLocation, FactorioObjectSortOrder::Locations);
  int last = skip(firstTrigger, FactorioObjectSortOrder::Triggers);
  if (last != static_cast<int>(sorted.size())) {
    throw std::runtime_error("objects with out-of-order sortingOrder");
  }

  objects = {0, last, sorted};
  specials = {firstSpecial, firstItem, sorted};
  items = {firstItem, firstFluid, sorted};
  fluids = {firstFluid, firstRecipe, sorted};
  goods = {firstSpecial, firstRecipe, sorted};
  recipes = {firstRecipe, firstTechnology, sorted};
  mechanics = {firstMechanics, firstTechnology, sorted};
  recipesAndTechnologies = {firstRecipe, firstEntity, sorted};
  technologies = {firstTechnology, firstEntity, sorted};
  entities = {firstEntity, firstTile, sorted};
  qualities = {firstQuality, firstLocation, sorted};
  locations = {firstLocation, firstTrigger, sorted};

  // Derived collections (upstream: OfType<> scans).
  allSciencePacks.clear();
  allModules.clear();
  allCrafters.clear();
  allBeacons.clear();
  allBelts.clear();
  allContainers.clear();
  for (Item* item : items) {
    if (auto* m = dynamic_cast<Module*>(item)) allModules.push_back(m);
  }
  for (Entity* entity : entities) {
    if (auto* c = dynamic_cast<EntityCrafter*>(entity)) {
      allCrafters.push_back(c);
      if (c->factorioType == "lab") {
        for (Goods* input : c->inputs) {
          if (auto* pack = dynamic_cast<Item*>(input)) {
            if (std::find(allSciencePacks.begin(), allSciencePacks.end(), pack) ==
                allSciencePacks.end()) {
              allSciencePacks.push_back(pack);
            }
          }
        }
      }
    }
    if (auto* b = dynamic_cast<EntityBeacon*>(entity)) allBeacons.push_back(b);
    if (auto* b = dynamic_cast<EntityBelt*>(entity)) allBelts.push_back(b);
    if (auto* c = dynamic_cast<EntityContainer*>(entity)) allContainers.push_back(c);
  }

  qualityNormal = nullptr;
  for (Quality* q : qualities) {
    if (q->level == 0) {
      qualityNormal = q;
      break;
    }
  }

  auto ch = objectsByTypeName.find("Entity.character");
  character = ch == objectsByTypeName.end() ? nullptr
                                            : dynamic_cast<Entity*>(ch->second);
}

}  // namespace yafc
