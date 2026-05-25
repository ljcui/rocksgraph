#include "ir/planner/cost_model.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

class FakePlannerStatistics final : public ir::PlannerStatistics {
 public:
  [[nodiscard]] double EstimateNodeCount(
      const std::unordered_set<std::string> &labels) const override {
    return labels.empty() ? 2000.0 : 25.0;
  }

  [[nodiscard]] double EstimateExpandFanout(
      const std::vector<std::string> &relationship_types) const override {
    return relationship_types.empty() ? 7.0 : 2.0;
  }

  [[nodiscard]] double EstimateExpandIntoSelectivity(
      const std::vector<std::string> &relationship_types) const override {
    return relationship_types.empty() ? 0.4 : 0.05;
  }

  [[nodiscard]] double EstimateFilterSelectivity() const override {
    return 0.25;
  }

  [[nodiscard]] double EstimateNodeHashJoinSelectivity(
      std::size_t key_count) const override {
    return key_count == 0 ? 1.0 : 0.2;
  }
};

}  // namespace

TEST(CostModelTest, UsesDefaultHeuristicStatistics) {
  ir::CostModel model;

  ir::CostEstimate all_node_scan = model.EstimateNodeScan({});
  EXPECT_DOUBLE_EQ(all_node_scan.estimated_rows, 1000.0);
  EXPECT_DOUBLE_EQ(all_node_scan.cost, 1000.0);

  ir::CostEstimate label_scan = model.EstimateNodeScan({"Person"});
  EXPECT_DOUBLE_EQ(label_scan.estimated_rows, 100.0);
  EXPECT_DOUBLE_EQ(label_scan.cost, 100.0);

  ir::CostEstimate untyped_expand =
      model.EstimateExpand({.estimated_rows = 10.0, .cost = 5.0}, {});
  EXPECT_DOUBLE_EQ(untyped_expand.estimated_rows, 100.0);
  EXPECT_DOUBLE_EQ(untyped_expand.cost, 105.0);

  ir::CostEstimate typed_expand =
      model.EstimateExpand({.estimated_rows = 10.0, .cost = 5.0}, {"KNOWS"});
  EXPECT_DOUBLE_EQ(typed_expand.estimated_rows, 30.0);
  EXPECT_DOUBLE_EQ(typed_expand.cost, 35.0);

  ir::CostEstimate untyped_expand_into =
      model.EstimateExpandInto({.estimated_rows = 10.0, .cost = 5.0}, {});
  EXPECT_DOUBLE_EQ(untyped_expand_into.estimated_rows, 5.0);
  EXPECT_DOUBLE_EQ(untyped_expand_into.cost, 10.0);

  ir::CostEstimate typed_expand_into = model.EstimateExpandInto(
      {.estimated_rows = 10.0, .cost = 5.0}, {"KNOWS"});
  EXPECT_DOUBLE_EQ(typed_expand_into.estimated_rows, 2.0);
  EXPECT_DOUBLE_EQ(typed_expand_into.cost, 7.0);

  ir::CostEstimate filtered =
      model.ApplyFilter({.estimated_rows = 100.0, .cost = 5.0});
  EXPECT_DOUBLE_EQ(filtered.estimated_rows, 10.0);
  EXPECT_DOUBLE_EQ(filtered.cost, 15.0);

  ir::CostEstimate join =
      model.EstimateNodeHashJoin({.estimated_rows = 100.0, .cost = 10.0},
                                 {.estimated_rows = 50.0, .cost = 20.0}, 2);
  EXPECT_DOUBLE_EQ(join.estimated_rows, 25.0);
  EXPECT_DOUBLE_EQ(join.cost, 205.0);
}

TEST(CostModelTest, UsesInjectedPlannerStatistics) {
  FakePlannerStatistics statistics;
  ir::CostModel model(&statistics);

  ir::CostEstimate all_node_scan = model.EstimateNodeScan({});
  EXPECT_DOUBLE_EQ(all_node_scan.estimated_rows, 2000.0);
  EXPECT_DOUBLE_EQ(all_node_scan.cost, 2000.0);

  ir::CostEstimate label_scan = model.EstimateNodeScan({"Person"});
  EXPECT_DOUBLE_EQ(label_scan.estimated_rows, 25.0);
  EXPECT_DOUBLE_EQ(label_scan.cost, 25.0);

  ir::CostEstimate untyped_expand =
      model.EstimateExpand({.estimated_rows = 10.0, .cost = 5.0}, {});
  EXPECT_DOUBLE_EQ(untyped_expand.estimated_rows, 70.0);
  EXPECT_DOUBLE_EQ(untyped_expand.cost, 75.0);

  ir::CostEstimate typed_expand =
      model.EstimateExpand({.estimated_rows = 10.0, .cost = 5.0}, {"KNOWS"});
  EXPECT_DOUBLE_EQ(typed_expand.estimated_rows, 20.0);
  EXPECT_DOUBLE_EQ(typed_expand.cost, 25.0);

  ir::CostEstimate untyped_expand_into =
      model.EstimateExpandInto({.estimated_rows = 10.0, .cost = 5.0}, {});
  EXPECT_DOUBLE_EQ(untyped_expand_into.estimated_rows, 4.0);
  EXPECT_DOUBLE_EQ(untyped_expand_into.cost, 9.0);

  ir::CostEstimate typed_expand_into = model.EstimateExpandInto(
      {.estimated_rows = 10.0, .cost = 5.0}, {"KNOWS"});
  EXPECT_DOUBLE_EQ(typed_expand_into.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(typed_expand_into.cost, 5.5);

  ir::CostEstimate filtered =
      model.ApplyFilter({.estimated_rows = 100.0, .cost = 5.0});
  EXPECT_DOUBLE_EQ(filtered.estimated_rows, 25.0);
  EXPECT_DOUBLE_EQ(filtered.cost, 30.0);

  ir::CostEstimate join =
      model.EstimateNodeHashJoin({.estimated_rows = 100.0, .cost = 10.0},
                                 {.estimated_rows = 50.0, .cost = 20.0}, 2);
  EXPECT_DOUBLE_EQ(join.estimated_rows, 10.0);
  EXPECT_DOUBLE_EQ(join.cost, 190.0);
}

TEST(CostModelTest, AppliesFilterEstimatesRepeatedly) {
  FakePlannerStatistics statistics;
  ir::CostModel model(&statistics);

  ir::CostEstimate estimate = ir::ApplyFilterEstimates(
      {.estimated_rows = 64.0, .cost = 10.0}, 2, model);

  EXPECT_DOUBLE_EQ(estimate.estimated_rows, 4.0);
  EXPECT_DOUBLE_EQ(estimate.cost, 30.0);
}
