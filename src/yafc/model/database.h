// Port of Yafc.Model/Data/Database.cs. Objects of one type occupy a
// continuous id range (sorted by FactorioObjectSortOrder), so per-type lookups
// are dense arrays (Mapping) instead of hash maps — the analyses depend on
// this for speed.
//
// Upstream Database is a C# static class; here it is a regular object so tests
// and future worker threads can own independent instances.
#pragma once

#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "yafc/model/data_classes.h"

namespace yafc {

// Typed view over the id-sorted master object list: [start, start+count).
template <typename T>
class FactorioIdRange {
 public:
  FactorioIdRange() = default;
  FactorioIdRange(int start, int end, const std::vector<FactorioObject*>& source)
      : start_(start) {
    all_.reserve(end - start);
    for (int i = start; i < end; ++i) {
      T* typed = dynamic_cast<T*>(source[i]);
      assert(typed != nullptr && "object outside its declared sort-order range");
      all_.push_back(typed);
    }
  }

  int start() const { return start_; }
  int count() const { return static_cast<int>(all_.size()); }
  const std::vector<T*>& all() const { return all_; }
  T* operator[](int i) const { return all_[i]; }
  T* ById(FactorioId id) const { return all_[id - start_]; }

  auto begin() const { return all_.begin(); }
  auto end() const { return all_.end(); }

  template <typename TValue>
  class Mapping;
  template <typename TOther, typename TValue>
  class Mapping2;

  template <typename TValue>
  Mapping<TValue> CreateMapping() const {
    return Mapping<TValue>(*this);
  }
  template <typename TOther, typename TValue>
  Mapping2<TOther, TValue> CreateMapping(const FactorioIdRange<TOther>& other) const {
    return Mapping2<TOther, TValue>(*this, other);
  }

  // Dense per-object storage keyed by id offset (upstream Mapping<TKey,TValue>).
  template <typename TValue>
  class Mapping {
   public:
    Mapping() = default;
    explicit Mapping(const FactorioIdRange& source)
        : offset_(source.start()), values_(source.count()) {}

    TValue& operator[](const T* key) { return values_[key->id - offset_]; }
    const TValue& operator[](const T* key) const { return values_[key->id - offset_]; }
    TValue& ById(FactorioId id) { return values_[id - offset_]; }
    size_t size() const { return values_.size(); }
    void Clear() { std::fill(values_.begin(), values_.end(), TValue{}); }
    std::vector<TValue>& values() { return values_; }

   private:
    int offset_ = 0;
    std::vector<TValue> values_;
  };

  // 2D dense storage over two ranges (upstream Mapping<TKey1,TKey2,TValue>).
  template <typename TOther, typename TValue>
  class Mapping2 {
   public:
    Mapping2() = default;
    Mapping2(const FactorioIdRange& key1, const FactorioIdRange<TOther>& key2)
        : offset1_(key1.start()), offset2_(key2.start()), count2_(key2.count()),
          values_(static_cast<size_t>(key1.count()) * key2.count()) {}

    TValue& operator()(const T* x, const TOther* y) {
      return values_[(x->id - offset1_) * static_cast<size_t>(count2_) +
                     (y->id - offset2_)];
    }

   private:
    int offset1_ = 0, offset2_ = 0, count2_ = 0;
    std::vector<TValue> values_;
  };

 private:
  int start_ = 0;
  std::vector<T*> all_;
};

// Convenience alias: yafc::Mapping<Goods, float> == dense per-Goods floats.
template <typename TKey, typename TValue>
using Mapping = typename FactorioIdRange<TKey>::template Mapping<TValue>;

class Database {
 public:
  // Takes ownership of all objects, stable-sorts them by sortingOrder (so each
  // type is one contiguous range), assigns ids, and builds ranges + name maps.
  // formerAliases: old typeDotNames from upstream renames, added as lookup
  // aliases only.
  void LoadBuiltData(std::vector<std::unique_ptr<FactorioObject>> allObjects,
                     const std::unordered_map<std::string, FactorioObject*>&
                         formerAliases = {});

  FactorioObject* FindByTypeDotName(const std::string& typeDotName) const {
    auto it = objectsByTypeName.find(typeDotName);
    return it == objectsByTypeName.end() ? nullptr : it->second;
  }

  // Id ranges (upstream property names).
  FactorioIdRange<FactorioObject> objects;
  FactorioIdRange<Goods> goods;
  FactorioIdRange<Special> specials;
  FactorioIdRange<Item> items;
  FactorioIdRange<Fluid> fluids;
  FactorioIdRange<Recipe> recipes;
  FactorioIdRange<Mechanics> mechanics;
  FactorioIdRange<RecipeOrTechnology> recipesAndTechnologies;
  FactorioIdRange<Technology> technologies;
  FactorioIdRange<Entity> entities;
  FactorioIdRange<Quality> qualities;
  FactorioIdRange<Location> locations;

  std::unordered_map<std::string, FactorioObject*> objectsByTypeName;
  // name -> temperature-sorted fluid variants; filled by the parser (upstream
  // LoadBuiltData parameters).
  std::unordered_map<std::string, std::vector<Fluid*>> fluidVariants;
  std::vector<Special*> heatVariants;
  std::vector<Item*> allSciencePacks;
  std::vector<Module*> allModules;
  std::vector<EntityCrafter*> allCrafters;
  std::vector<EntityBeacon*> allBeacons;
  std::vector<EntityBelt*> allBelts;
  std::vector<EntityContainer*> allContainers;
  Quality* qualityNormal = nullptr;  // upstream Quality.Normal
  Entity* character = nullptr;
  int constantCombinatorCapacity = 18;

 private:
  std::vector<std::unique_ptr<FactorioObject>> storage_;
};

}  // namespace yafc
