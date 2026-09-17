#include "planner/idp.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

planner::PlanCandidate Candidate(std::vector<std::string> symbols,
                                 std::vector<std::size_t> relationship_indices,
                                 double cost) {
  return planner::MakePlanCandidate(
      std::make_unique<ir::ArgumentPlan>(std::move(symbols)),
      std::move(relationship_indices),
      planner::CostEstimate{.estimated_rows = cost, .cost = cost});
}

planner::PlanKey Key(std::vector<std::size_t> relationship_indices,
                     std::vector<std::string> covered_symbols,
                     std::vector<planner::OrderingKeyItem> ordering = {}) {
  return {.relationship_indices = std::move(relationship_indices),
          .covered_symbols = std::move(covered_symbols),
          .ordering = std::move(ordering)};
}

}  // namespace

TEST(IdpPlanTableTest, PutBestKeepsLowestCostForSameKey) {
  planner::PlanTable table;
  table.PutBest(Candidate({"a"}, {1}, 20.0));
  table.PutBest(Candidate({"a"}, {1}, 10.0));
  table.PutBest(Candidate({"a"}, {1}, 30.0));

  const planner::PlanCandidate *best = table.Best(Key({1}, {"a"}));
  ASSERT_NE(best, nullptr);
  EXPECT_EQ(best->cost, 10.0);
}

TEST(IdpPlanTableTest, PruneRelationshipCountOnlyPrunesRequestedSize) {
  planner::PlanTable table;
  table.PutBest(Candidate({"expensive"}, {1}, 30.0));
  table.PutBest(Candidate({"cheap"}, {2}, 10.0));
  table.PutBest(Candidate({"two_hop"}, {1, 2}, 100.0));

  table.PruneRelationshipCount(1, 1);

  EXPECT_EQ(table.Best(Key({1}, {"expensive"})), nullptr);
  EXPECT_NE(table.Best(Key({2}, {"cheap"})), nullptr);
  EXPECT_NE(table.Best(Key({1, 2}, {"two_hop"})), nullptr);
}

TEST(IdpPlanTableTest, PruneRelationshipCountKeepsCheapestCandidates) {
  planner::PlanTable table;
  table.PutBest(Candidate({"expensive"}, {1}, 30.0));
  table.PutBest(Candidate({"mid"}, {2}, 20.0));
  table.PutBest(Candidate({"cheap"}, {3}, 10.0));

  table.PruneRelationshipCount(1, 2);

  EXPECT_EQ(table.Best(Key({1}, {"expensive"})), nullptr);
  EXPECT_NE(table.Best(Key({2}, {"mid"})), nullptr);
  EXPECT_NE(table.Best(Key({3}, {"cheap"})), nullptr);
}

TEST(IdpPlanTableTest, PruneRelationshipCountUsesPlanKeyForStableTies) {
  planner::PlanTable table;
  table.PutBest(Candidate({"c"}, {3}, 10.0));
  table.PutBest(Candidate({"a"}, {1}, 10.0));
  table.PutBest(Candidate({"b"}, {2}, 10.0));

  table.PruneRelationshipCount(1, 2);

  EXPECT_NE(table.Best(Key({1}, {"a"})), nullptr);
  EXPECT_NE(table.Best(Key({2}, {"b"})), nullptr);
  EXPECT_EQ(table.Best(Key({3}, {"c"})), nullptr);
}

TEST(IdpPlanTableTest, KeepsCheapestCandidateForEachOrdering) {
  ast::Variable variable;
  variable.name = "a";
  ir::LogicalSortItem order{.expression = &variable};

  planner::PlanTable table;
  table.PutBest(Candidate({"a"}, {1}, 10.0));
  planner::PlanCandidate ordered = Candidate({"a"}, {1}, 20.0);
  ordered.provided_order = {order};
  ordered.plan->SetOrderingTrait(ordered.provided_order);
  table.PutBest(std::move(ordered));

  EXPECT_NE(table.Best(Key({1}, {"a"})), nullptr);
  EXPECT_NE(
      table.Best(Key({1}, {"a"},
                     {{.expression = "a",
                       .direction = ir::LogicalOrderDirection::kAscending}})),
      nullptr);
}

TEST(IdpPlanTableTest, PruningRetainsOrderingVariantForKeptState) {
  ast::Variable variable;
  variable.name = "a";
  ir::LogicalSortItem order{.expression = &variable};

  planner::PlanTable table;
  table.PutBest(Candidate({"a"}, {1}, 10.0));
  planner::PlanCandidate ordered = Candidate({"a"}, {1}, 20.0);
  ordered.provided_order = {order};
  ordered.plan->SetOrderingTrait(ordered.provided_order);
  table.PutBest(std::move(ordered));
  table.PutBest(Candidate({"b"}, {2}, 30.0));

  table.PruneRelationshipCount(1, 1);

  EXPECT_NE(table.Best(Key({1}, {"a"})), nullptr);
  EXPECT_NE(
      table.Best(Key({1}, {"a"},
                     {{.expression = "a",
                       .direction = ir::LogicalOrderDirection::kAscending}})),
      nullptr);
  EXPECT_EQ(table.Best(Key({2}, {"b"})), nullptr);
}
