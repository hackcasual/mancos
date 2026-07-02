#include "yafc/analysis/dependencies.h"

#include <algorithm>

namespace yafc {

namespace {

using Flags = DependencyNode::Flags;

template <typename T>
std::vector<const FactorioObject*> AsObjects(const std::vector<T*>& in) {
  return {in.begin(), in.end()};
}

// RecipeOrTechnology.GetDependenciesHelper()
std::vector<DependencyNode> RecipeDependenciesHelper(const RecipeOrTechnology& recipe) {
  std::vector<DependencyNode> collector;
  if (!recipe.ingredients.empty()) {
    std::vector<const FactorioObject*> plain;
    for (const Ingredient& ingredient : recipe.ingredients) {
      if (!ingredient.variants.empty()) {
        collector.push_back(
            DependencyNode::Create(AsObjects(ingredient.variants), Flags::IngredientVariant));
      } else {
        plain.push_back(ingredient.goods);
      }
    }
    if (!plain.empty()) {
      collector.push_back(DependencyNode::Create(plain, Flags::Ingredient));
    }
  }
  if (!(recipe.flags & RecipeFlags::kHasResearchTriggerMask)) {
    // Trigger researches don't require a crafter.
    collector.push_back(
        DependencyNode::Create(AsObjects(recipe.crafters), Flags::CraftingEntity));
  }
  if (recipe.sourceEntity != nullptr) {
    collector.push_back(DependencyNode::Create({recipe.sourceEntity}, Flags::SourceEntity));
  }
  if (!recipe.sourceTiles.empty()) {
    std::vector<const FactorioObject*> locations;
    for (const Tile* tile : recipe.sourceTiles) {
      for (Location* loc : tile->locations) {
        if (std::find(locations.begin(), locations.end(), loc) == locations.end()) {
          locations.push_back(loc);
        }
      }
    }
    collector.push_back(DependencyNode::Create(locations, Flags::Location));
  }
  return collector;
}

DependencyNode GoodsDependencies(const Goods& goods) {
  std::vector<const FactorioObject*> sources(goods.production.begin(),
                                             goods.production.end());
  sources.insert(sources.end(), goods.miscSources.begin(), goods.miscSources.end());
  return DependencyNode::Create(sources, Flags::Source);
}

DependencyNode EntityDependencies(const Entity& entity) {
  std::vector<DependencyNode> sources;
  if (entity.mapGenerated) {
    sources.push_back(
        DependencyNode::Create(AsObjects(entity.spawnLocations), Flags::Location));
  }
  if (!entity.itemsToPlace.empty()) {
    sources.push_back(
        DependencyNode::Create(AsObjects(entity.itemsToPlace), Flags::Source));
  }
  if (!entity.sourceEntities.empty()) {  // asteroid death
    sources.push_back(
        DependencyNode::Create(AsObjects(entity.sourceEntities), Flags::Source));
  }
  // TODO(port): captured-spawner sources (captureAmmo/EntitySpawner) — the
  // Ammo/EntitySpawner classes are not ported yet.
  if (sources.empty()) {
    sources.push_back(DependencyNode::Create(std::vector<FactorioId>{}, Flags::ItemToPlace));
  }
  if (entity.hasEnergy) {
    return DependencyNode::RequireAll(
        {DependencyNode::Create(AsObjects(entity.energy.fuels), Flags::Fuel),
         DependencyNode::RequireAny(std::move(sources))});
  }
  return DependencyNode::RequireAny(std::move(sources));
}

DependencyNode TechnologyDependencies(const Technology& tech, const Database& db) {
  std::vector<DependencyNode> nodes = RecipeDependenciesHelper(tech);
  if (!tech.prerequisites.empty()) {
    nodes.push_back(DependencyNode::Create(AsObjects(tech.prerequisites),
                                           Flags::TechnologyPrerequisites));
  }
  if (tech.flags & RecipeFlags::kHasResearchTriggerMineEntity) {
    // Prefer the mining mechanic for the entity; fall back to the entity.
    std::vector<const FactorioObject*> sources;
    for (Entity* e : tech.triggerEntities) {
      const FactorioObject* source = e;
      for (Mechanics* m : db.mechanics) {
        if (m->source == e) {
          source = m;
          break;
        }
      }
      sources.push_back(source);
    }
    nodes.push_back(DependencyNode::Create(sources, Flags::Source));
  }
  if (tech.flags & RecipeFlags::kHasResearchTriggerBuildEntity) {
    nodes.push_back(
        DependencyNode::Create(AsObjects(tech.triggerEntities), Flags::Source));
  }
  if (tech.flags & (RecipeFlags::kHasResearchTriggerCreateSpacePlatform |
                    RecipeFlags::kHasResearchTriggerSendToOrbit |
                    RecipeFlags::kHasResearchTriggerScripted)) {
    std::vector<const FactorioObject*> sources;
    if (tech.flags & RecipeFlags::kHasResearchTriggerCreateSpacePlatform) {
      for (Item* i : db.items) {
        if (i->factorioType == "space-platform-starter-pack") {
          if (FactorioObject* m = db.FindByTypeDotName("Mechanics.launch." + i->name)) {
            sources.push_back(m);
          }
        }
      }
    }
    if ((tech.flags & RecipeFlags::kHasResearchTriggerSendToOrbit) &&
        tech.triggerObject != nullptr) {
      if (FactorioObject* m =
              db.FindByTypeDotName("Mechanics.launch." + tech.triggerObject->name)) {
        sources.push_back(m);
      }
    }
    if ((tech.flags & RecipeFlags::kHasResearchTriggerScripted) &&
        tech.triggerObject != nullptr) {
      sources.push_back(tech.triggerObject);
    }
    nodes.push_back(DependencyNode::Create(sources, Flags::Source));
  }
  if (!tech.enabled) {
    // Disabled technologies are inaccessible (empty require-any list).
    nodes.push_back(DependencyNode::Create(std::vector<FactorioId>{}, Flags::Disabled));
  }
  return DependencyNode::RequireAll(std::move(nodes));
}

DependencyNode QualityDependencies(const Quality& quality, const Database& db) {
  std::vector<DependencyNode> collector;
  collector.push_back(DependencyNode::Create(AsObjects(quality.technologyUnlock),
                                             Flags::TechnologyUnlock));
  if (quality.previousQuality != nullptr) {
    collector.push_back(
        DependencyNode::Create({quality.previousQuality}, Flags::Source));
  }
  if (quality.level != 0) {
    std::vector<const FactorioObject*> qualityModules;
    for (Module* m : db.allModules) {
      if (m->moduleSpecification.baseQuality > 0) qualityModules.push_back(m);
    }
    collector.push_back(DependencyNode::Create(qualityModules, Flags::Source));
  }
  return DependencyNode::RequireAll(std::move(collector));
}

}  // namespace

DependencyNode GetDependencies(const FactorioObject& obj, const Database& db) {
  // Dispatch replaces the C# virtual (most-derived checks first).
  if (auto* tech = dynamic_cast<const Technology*>(&obj)) {
    return TechnologyDependencies(*tech, db);
  }
  if (auto* recipe = dynamic_cast<const Recipe*>(&obj)) {
    std::vector<DependencyNode> nodes = RecipeDependenciesHelper(*recipe);
    if (!recipe->enabled) {
      nodes.push_back(DependencyNode::Create(AsObjects(recipe->technologyUnlock),
                                             Flags::TechnologyUnlock));
    }
    return DependencyNode::RequireAll(std::move(nodes));
  }
  if (auto* item = dynamic_cast<const Item*>(&obj)) {
    if (item == db.science) {
      return DependencyNode::Create(AsObjects(db.technologies.all()), Flags::Source);
    }
    return GoodsDependencies(*item);
  }
  if (auto* goods = dynamic_cast<const Goods*>(&obj)) return GoodsDependencies(*goods);
  if (auto* entity = dynamic_cast<const Entity*>(&obj)) return EntityDependencies(*entity);
  if (auto* tile = dynamic_cast<const Tile*>(&obj)) {
    return DependencyNode::Create(AsObjects(tile->locations), Flags::Location);
  }
  if (auto* location = dynamic_cast<const Location*>(&obj)) {
    return DependencyNode::Create(AsObjects(location->technologyUnlock),
                                  Flags::TechnologyUnlock);
  }
  if (auto* quality = dynamic_cast<const Quality*>(&obj)) {
    return QualityDependencies(*quality, db);
  }
  // Unknown object kind: no sources (inaccessible unless root-accessible).
  return DependencyNode::Create(std::vector<FactorioId>{}, Flags::Source);
}

void Dependencies::Calculate(const Database& db) {
  dependencyList = db.objects.CreateMapping<DependencyNode>();
  reverseDependencies = db.objects.CreateMapping<std::vector<FactorioId>>();

  for (FactorioObject* obj : db.objects) {
    DependencyNode packed = GetDependencies(*obj, db);
    dependencyList[obj] = packed;
    for (FactorioId req : packed.Flatten()) {
      auto& reverse = reverseDependencies.ById(req);
      if (std::find(reverse.begin(), reverse.end(), obj->id) == reverse.end()) {
        reverse.push_back(obj->id);
      }
    }
  }
}

}  // namespace yafc
