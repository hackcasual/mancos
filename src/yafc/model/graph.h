// Port of Yafc.Model/Math/Graph.cs — directed graph with Tarjan SCC, used by
// ProductionTable's GetInfeasibilityCandidates. Instead of upstream's
// MergeStrongConnectedComponents (which returns a remapped graph), this exposes
// the non-trivial components directly — that is the only part the solver reads.
// Component element order matches upstream (Tarjan stack segment order), which
// the candidate selection depends on (it takes the *last* element).
#pragma once

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace yafc {

template <typename T>
class Graph {
 public:
  void Connect(const T& from, const T& to) {
    int f = GetNode(from), t = GetNode(to);
    auto& arcs = arcs_[f];
    if (std::find(arcs.begin(), arcs.end(), t) == arcs.end()) arcs.push_back(t);
  }

  bool HasConnection(const T& from, const T& to) const {
    auto f = ids_.find(from);
    auto t = ids_.find(to);
    if (f == ids_.end() || t == ids_.end()) return false;
    const auto& arcs = arcs_[f->second];
    return std::find(arcs.begin(), arcs.end(), t->second) != arcs.end();
  }

  // Non-trivial strongly connected components: size > 1, or a single node with
  // a self-arc. Upstream: MergeStrongConnectedComponents nodes with list != null.
  std::vector<std::vector<T>> NontrivialComponents() const {
    TarjanState st{.state = std::vector<int>(values_.size(), kUndefined),
                   .low = std::vector<int>(values_.size(), 0)};
    for (int v = 0; v < static_cast<int>(values_.size()); ++v) {
      if (st.state[v] == kUndefined) StrongConnect(v, st);
    }
    return std::move(st.components);
  }

 private:
  static constexpr int kUndefined = -1;
  static constexpr int kOffStack = -2;

  struct TarjanState {
    std::vector<int> state;  // "index" in Tarjan terms; kUndefined/kOffStack sentinels
    std::vector<int> low;
    std::vector<int> stack;
    std::vector<std::vector<T>> components;
    int index = 0;
  };

  int GetNode(const T& v) {
    auto [it, inserted] = ids_.try_emplace(v, static_cast<int>(values_.size()));
    if (inserted) {
      values_.push_back(v);
      arcs_.emplace_back();
    }
    return it->second;
  }

  void StrongConnect(int root, TarjanState& st) const {
    st.low[root] = st.state[root] = st.index++;
    st.stack.push_back(root);

    for (int n : arcs_[root]) {
      if (st.state[n] == kUndefined) {
        StrongConnect(n, st);
        st.low[root] = std::min(st.low[root], st.low[n]);
      } else if (st.state[n] >= 0) {
        st.low[root] = std::min(st.low[root], st.state[n]);
      }
    }

    if (st.low[root] == st.state[root]) {
      auto rootIt = std::find(st.stack.rbegin(), st.stack.rend(), root).base() - 1;
      size_t rootIndex = rootIt - st.stack.begin();
      size_t count = st.stack.size() - rootIndex;
      bool selfLoop = std::find(arcs_[root].begin(), arcs_[root].end(), root) !=
                      arcs_[root].end();
      if (count > 1 || selfLoop) {
        std::vector<T> component;
        component.reserve(count);
        for (size_t i = rootIndex; i < st.stack.size(); ++i) {
          component.push_back(values_[st.stack[i]]);
        }
        st.components.push_back(std::move(component));
      }
      for (size_t i = rootIndex; i < st.stack.size(); ++i) {
        st.state[st.stack[i]] = kOffStack;
      }
      st.stack.resize(rootIndex);
    }
  }

  std::unordered_map<T, int> ids_;
  std::vector<T> values_;
  std::vector<std::vector<int>> arcs_;
};

}  // namespace yafc
