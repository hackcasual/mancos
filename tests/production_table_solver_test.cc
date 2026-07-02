// Tests for the ported ProductionTable solve core, matched against upstream
// semantics:
//  - a byproduct nobody consumes is a ONE-SIDED link: its constraint is
//    dropped in pass 1 (no slack involved) and it surfaces as surplus
//    production ("Extra products") with LinkNotMatched — no warnings.
//  - the slack fallback runs only when pass 1 is INFEASIBLE: two-sided links
//    that cannot balance (multi-output splits, deadlocked loops).
#include "yafc/model/production_table_solver.h"

#include "doctest/doctest.h"

using namespace yafc;

namespace {

// Link indices for the lead chain.
enum : int { kOre, kG1, kG2, kFeed, kDust, kMolten, kPlate, kGangue, kLinkCount };

std::vector<SolverLink> LeadLinks() {
  std::vector<SolverLink> links(kLinkCount);
  const char* names[] = {"lead-ore",    "grade1-lead", "grade2-lead",
                         "grade3-feed", "silver-dust", "molten-lead",
                         "lead-plate",  "gangue"};
  for (int i = 0; i < kLinkCount; ++i) links[i].name = names[i];
  links[kPlate].amount = 900;  // desired products: 900 plates/min
  return links;
}

std::vector<SolverRecipe> LeadRecipes() {
  std::vector<SolverRecipe> r(7);
  r[0] = {.name = "lead-ore-mining", .products = {{kOre, 1}}};
  r[1] = {.name = "lead-grade-1", .products = {{kG1, 1}}, .ingredients = {{kOre, 5}}};
  r[2] = {.name = "lead-grade-2",
          .products = {{kG2, 1}, {kGangue, 1}},
          .ingredients = {{kG1, 2}}};
  r[3] = {.name = "lead-grade-3",
          .products = {{kFeed, 2}, {kG1, 1}},  // recycle loop back into grade1
          .ingredients = {{kG2, 4}}};
  r[4] = {.name = "silver-lead-dust", .products = {{kDust, 1}}, .ingredients = {{kFeed, 1}}};
  r[5] = {.name = "molten-lead", .products = {{kMolten, 22.5}}, .ingredients = {{kDust, 1}}};
  r[6] = {.name = "cast-lead-plate",
          .products = {{kPlate, 1}},
          .ingredients = {{kMolten, 1.5875}}};
  return r;
}

}  // namespace

TEST_CASE("lead chain: unconsumed byproduct = dropped link + surplus, no warnings") {
  auto recipes = LeadRecipes();
  auto links = LeadLinks();

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);

  CHECK(recipes[0].recipes_per_second == doctest::Approx(1111.25));
  CHECK(recipes[1].recipes_per_second == doctest::Approx(222.25));
  CHECK(recipes[2].recipes_per_second == doctest::Approx(127.0));
  CHECK(recipes[3].recipes_per_second == doctest::Approx(31.75));
  CHECK(recipes[4].recipes_per_second == doctest::Approx(63.5));
  CHECK(recipes[5].recipes_per_second == doctest::Approx(63.5));
  CHECK(recipes[6].recipes_per_second == doctest::Approx(900.0));

  // Gangue: production-only link -> constraint dropped, flagged, surplus
  // reported in goods units ("Extra products" = 127/min), no slack, no warning.
  CHECK((links[kGangue].flags & LinkFlags::kLinkNotMatched) != 0);
  CHECK((links[kGangue].flags & LinkFlags::kHasConsumption) == 0);
  CHECK(links[kGangue].production - links[kGangue].consumption ==
        doctest::Approx(127.0));
  CHECK(links[kGangue].not_matched_flow == 0.0);
  for (const auto& r : recipes) CHECK(r.warning_flags == 0);
}

TEST_CASE("feasible chain without byproducts solves in pass 1, no flags") {
  // mine -> smelt -> 60 plates/min
  std::vector<SolverLink> links(2);
  links[0].name = "iron-ore";
  links[1].name = "iron-plate";
  links[1].amount = 60;

  std::vector<SolverRecipe> recipes(2);
  recipes[0] = {.name = "mine", .products = {{0, 1}}};
  recipes[1] = {.name = "smelt", .products = {{1, 1}}, .ingredients = {{0, 1}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);
  CHECK(recipes[0].recipes_per_second == doctest::Approx(60));
  CHECK(recipes[1].recipes_per_second == doctest::Approx(60));
  CHECK(links[0].not_matched_flow == 0.0);
  CHECK((links[0].flags & LinkFlags::kLinkNotMatched) == 0);
  CHECK((links[1].flags & LinkFlags::kLinkNotMatched) == 0);
}

TEST_CASE("two-sided split byproduct forces the slack pass: OverproductionRequired") {
  // Refinery: 2 gas + 1 tar per craft. Gas demand 10, tar demand 10, and tar
  // IS consumed (burner), so both links are two-sided and pass 1 cannot
  // balance: tar needs 10 burned + 10 demanded => 20 crafts => 40 gas != 10.
  enum : int { kGas, kTar, kLinks };
  std::vector<SolverLink> links(kLinks);
  links[kGas] = {.name = "gas", .amount = 10};
  links[kTar] = {.name = "tar", .amount = 10};

  std::vector<SolverRecipe> recipes(2);
  recipes[0] = {.name = "refinery", .products = {{kGas, 2}, {kTar, 1}}};
  recipes[1] = {.name = "tar-burner", .fixed_rps = 10, .ingredients = {{kTar, 1}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);

  // 20 refinery crafts (tar-bound), 30 gas surplus absorbed by split slack.
  CHECK(recipes[0].recipes_per_second == doctest::Approx(20));
  CHECK(links[kGas].not_matched_flow == doctest::Approx(30));  // cost 1 => goods units
  CHECK((links[kGas].flags & LinkFlags::kLinkRecursiveNotMatched) != 0);
  CHECK((recipes[0].warning_flags & RecipeWarningFlags::kOverproductionRequired) != 0);
  CHECK(links[kTar].not_matched_flow == 0.0);
}

TEST_CASE("net-consuming catalyst loop is a deadlock: injection + warning") {
  // Bootstrap problem: each craft consumes 1 heavy oil, returns only 0.5, and
  // heavy oil has no other source. Driven by a plate demand of 10/min.
  enum : int { kHeavy, kPlate2, kLinks };
  std::vector<SolverLink> links(kLinks);
  links[kHeavy] = {.name = "heavy-oil"};
  links[kPlate2] = {.name = "plate", .amount = 10};

  std::vector<SolverRecipe> recipes(1);
  recipes[0] = {.name = "bootstrap-cracker",
                .products = {{kHeavy, 0.5}, {kPlate2, 1}},
                .ingredients = {{kHeavy, 1}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);

  CHECK(recipes[0].recipes_per_second == doctest::Approx(10));
  // 5/min heavy oil injected by the deadlock slack (negative direction).
  CHECK(links[kHeavy].not_matched_flow == doctest::Approx(-5));
  CHECK((links[kHeavy].flags & LinkFlags::kLinkRecursiveNotMatched) != 0);
  CHECK((recipes[0].warning_flags & RecipeWarningFlags::kDeadlockCandidate) != 0);
}

TEST_CASE("AllowOverProduction on a two-sided link avoids the slack pass") {
  // Same refinery, but the gas link tolerates overproduction.
  enum : int { kGas, kTar, kLinks };
  std::vector<SolverLink> links(kLinks);
  links[kGas] = {.name = "gas", .amount = 10,
                 .algorithm = LinkAlgorithm::AllowOverProduction};
  links[kTar] = {.name = "tar", .amount = 10};

  std::vector<SolverRecipe> recipes(2);
  recipes[0] = {.name = "refinery", .products = {{kGas, 2}, {kTar, 1}}};
  recipes[1] = {.name = "tar-burner", .fixed_rps = 10, .ingredients = {{kTar, 1}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);
  CHECK(recipes[0].recipes_per_second == doctest::Approx(20));
  // No slack ran; the basis pass marks the loose non-Match link instead.
  CHECK(links[kGas].not_matched_flow == 0.0);
  CHECK((links[kGas].flags & LinkFlags::kLinkNotMatched) != 0);
  for (const auto& r : recipes) CHECK(r.warning_flags == 0);
}

TEST_CASE("fixed building count pins the recipe rate") {
  std::vector<SolverLink> links(2);
  links[0].name = "ore";
  links[1] = {.name = "plate", .amount = 30, .algorithm = LinkAlgorithm::AllowOverProduction};

  std::vector<SolverRecipe> recipes(2);
  recipes[0] = {.name = "mine", .products = {{0, 1}}};
  recipes[1] = {.name = "smelt", .fixed_rps = 45, .products = {{1, 1}},
                .ingredients = {{0, 1}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);
  CHECK(recipes[1].recipes_per_second == doctest::Approx(45));
  CHECK(recipes[0].recipes_per_second == doctest::Approx(45));
}

TEST_CASE("one-sided links are disabled, not fatal") {
  std::vector<SolverLink> links(2);
  links[0].name = "input-nobody-makes";
  links[1] = {.name = "output", .amount = 10};

  std::vector<SolverRecipe> recipes(1);
  recipes[0] = {.name = "assemble", .products = {{1, 1}}, .ingredients = {{0, 3}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);
  CHECK(recipes[0].recipes_per_second == doctest::Approx(10));
  CHECK((links[0].flags & LinkFlags::kLinkNotMatched) != 0);
  CHECK((links[0].flags & LinkFlags::kHasProduction) == 0);
}

TEST_CASE("sodium hydroxide: net-zero slaked lime loop solves clean when recycled") {
  // Causticization: soda ash + slaked lime -> sodium hydroxide + calcium
  // carbonate; the calcium loops back via calcination + slaking. With the
  // recycle in place the calcium species circulate at net zero, so the strict
  // Match links are feasible in pass 1 despite the loop (no slack, no
  // warnings) - the LP determines the circulating flow from the NaOH demand.
  enum : int { kSodaAsh, kSlakedLime, kNaOH, kCaCO3, kQuicklime, kLinks };
  std::vector<SolverLink> links(kLinks);
  links[kSodaAsh] = {.name = "soda-ash"};
  links[kSlakedLime] = {.name = "slaked-lime"};
  links[kNaOH] = {.name = "sodium-hydroxide", .amount = 60};
  links[kCaCO3] = {.name = "calcium-carbonate"};
  links[kQuicklime] = {.name = "quicklime"};

  std::vector<SolverRecipe> recipes(4);
  recipes[0] = {.name = "causticize",
                .products = {{kNaOH, 1}, {kCaCO3, 1}},
                .ingredients = {{kSodaAsh, 1}, {kSlakedLime, 1}}};
  recipes[1] = {.name = "calcine", .products = {{kQuicklime, 1}},
                .ingredients = {{kCaCO3, 1}}};  // CO2 vented (unlinked)
  recipes[2] = {.name = "slake", .products = {{kSlakedLime, 1}},
                .ingredients = {{kQuicklime, 1}}};  // water unlinked
  recipes[3] = {.name = "make-soda-ash", .products = {{kSodaAsh, 1}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);

  // The whole loop turns at exactly the NaOH rate.
  for (int i = 0; i < 4; ++i) {
    CHECK(recipes[i].recipes_per_second == doctest::Approx(60));
    CHECK(recipes[i].warning_flags == 0);
  }
  // Every calcium link is perfectly balanced: no slack, no unmatched flags.
  for (int link : {kSlakedLime, kCaCO3, kQuicklime}) {
    CHECK(links[link].production == doctest::Approx(60));
    CHECK(links[link].consumption == doctest::Approx(60));
    CHECK(links[link].not_matched_flow == 0.0);
    CHECK((links[link].flags & LinkFlags::kLinkNotMatched) == 0);
  }
}

TEST_CASE("sodium hydroxide: without the recycle the loop breaks visibly") {
  // Same chain minus calcination/slaking: slaked lime has no producer (its
  // link is one-sided -> dropped constraint, shows as a missing input) and
  // calcium carbonate has no consumer (surplus "extra product").
  enum : int { kSodaAsh, kSlakedLime, kNaOH, kCaCO3, kLinks };
  std::vector<SolverLink> links(kLinks);
  links[kSodaAsh] = {.name = "soda-ash"};
  links[kSlakedLime] = {.name = "slaked-lime"};
  links[kNaOH] = {.name = "sodium-hydroxide", .amount = 60};
  links[kCaCO3] = {.name = "calcium-carbonate"};

  std::vector<SolverRecipe> recipes(2);
  recipes[0] = {.name = "causticize",
                .products = {{kNaOH, 1}, {kCaCO3, 1}},
                .ingredients = {{kSodaAsh, 1}, {kSlakedLime, 1}}};
  recipes[1] = {.name = "make-soda-ash", .products = {{kSodaAsh, 1}}};

  REQUIRE(SolveProductionTable(recipes, links) == TableSolveResult::Ok);
  CHECK(recipes[0].recipes_per_second == doctest::Approx(60));

  // Slaked lime: consumption without production -> disabled link, net import.
  CHECK((links[kSlakedLime].flags & LinkFlags::kHasProduction) == 0);
  CHECK((links[kSlakedLime].flags & LinkFlags::kLinkNotMatched) != 0);
  CHECK(links[kSlakedLime].consumption == doctest::Approx(60));
  CHECK(links[kSlakedLime].production == 0.0);

  // Calcium carbonate: production without consumption -> extra product.
  CHECK((links[kCaCO3].flags & LinkFlags::kHasConsumption) == 0);
  CHECK((links[kCaCO3].flags & LinkFlags::kLinkNotMatched) != 0);
  CHECK(links[kCaCO3].production == doctest::Approx(60));
}
