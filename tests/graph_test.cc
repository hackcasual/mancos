#include "yafc/model/graph.h"

#include <algorithm>
#include <vector>

#include "doctest/doctest.h"

TEST_CASE("tarjan finds loops, ignores trees, honors self-loops") {
  yafc::Graph<int> g;
  // chain: 0 -> 1 -> 2; loop: 2 -> 3 -> 4 -> 2; self-loop: 5; isolated arc 1 -> 5
  g.Connect(0, 1);
  g.Connect(1, 2);
  g.Connect(2, 3);
  g.Connect(3, 4);
  g.Connect(4, 2);
  g.Connect(5, 5);
  g.Connect(1, 5);

  auto components = g.NontrivialComponents();
  REQUIRE(components.size() == 2);

  std::vector<std::vector<int>> sorted = components;
  for (auto& c : sorted) std::sort(c.begin(), c.end());
  std::sort(sorted.begin(), sorted.end());
  CHECK(sorted[0] == std::vector<int>{2, 3, 4});
  CHECK(sorted[1] == std::vector<int>{5});

  CHECK(g.HasConnection(2, 3));
  CHECK_FALSE(g.HasConnection(3, 2));
}
