#include "planner/idp.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

ir::PlanCandidate Candidate(std::vector<std::string> symbols,
                            std::vector<std::size_t> relationship_indices,
                            double cost) {
  return ir::MakePlanCandidate(
      std::make_unique<ir::ArgumentPlan>(std::move(symbols)),
      std::move(relationship_indices),
      ir::CostEstimate{.estimated_rows = cost, .cost = cost});
}

ir::PlanKey Key(std::vector<std::size_t> relationship_indices,
                std::vector<std::string> covered_symbols) {
  return {.relationship_indices = std::move(relationship_indices),
          .covered_symbols = std::move(covered_symbols)};
}

}  // namespace

TEST(IdpPlanTableTest, PutBestKeepsLowestCostForSameKey) {
  ir::PlanTable table;
  table.PutBest(Candidate({"a"}, {1}, 20.0));
  table.PutBest(Candidate({"a"}, {1}, 10.0));
  table.PutBest(Candidate({"a"}, {1}, 30.0));

  const ir::PlanCandidate *best = table.Best(Key({1}, {"a"}));
  ASSERT_NE(best, nullptr);
  EXPECT_EQ(best->cost, 10.0);
}

TEST(IdpPlanTableTest, PruneRelationshipCountOnlyPrunesRequestedSize) {
  ir::PlanTable table;
  table.PutBest(Candidate({"expensive"}, {1}, 30.0));
  table.PutBest(Candidate({"cheap"}, {2}, 10.0));
  table.PutBest(Candidate({"two_hop"}, {1, 2}, 100.0));

  table.PruneRelationshipCount(1, 1);

  EXPECT_EQ(table.Best(Key({1}, {"expensive"})), nullptr);
  EXPECT_NE(table.Best(Key({2}, {"cheap"})), nullptr);
  EXPECT_NE(table.Best(Key({1, 2}, {"two_hop"})), nullptr);
}

TEST(IdpPlanTableTest, PruneRelationshipCountKeepsCheapestCandidates) {
  ir::PlanTable table;
  table.PutBest(Candidate({"expensive"}, {1}, 30.0));
  table.PutBest(Candidate({"mid"}, {2}, 20.0));
  table.PutBest(Candidate({"cheap"}, {3}, 10.0));

  table.PruneRelationshipCount(1, 2);

  EXPECT_EQ(table.Best(Key({1}, {"expensive"})), nullptr);
  EXPECT_NE(table.Best(Key({2}, {"mid"})), nullptr);
  EXPECT_NE(table.Best(Key({3}, {"cheap"})), nullptr);
}

TEST(IdpPlanTableTest, PruneRelationshipCountUsesPlanKeyForStableTies) {
  ir::PlanTable table;
  table.PutBest(Candidate({"c"}, {3}, 10.0));
  table.PutBest(Candidate({"a"}, {1}, 10.0));
  table.PutBest(Candidate({"b"}, {2}, 10.0));

  table.PruneRelationshipCount(1, 2);

  EXPECT_NE(table.Best(Key({1}, {"a"})), nullptr);
  EXPECT_NE(table.Best(Key({2}, {"b"})), nullptr);
  EXPECT_EQ(table.Best(Key({3}, {"c"})), nullptr);
}
