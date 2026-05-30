#include "ir/planner/cost_model.h"

#include <gtest/gtest.h>

#include <optional>

#include "tests/fake_planner_statistics.h"

TEST(CostModelTest, UsesDefaultHeuristicStatistics) {
  ir::CostModel model;

  ir::CostEstimate all_node_scan = model.EstimateNodeScan({});
  EXPECT_DOUBLE_EQ(all_node_scan.estimated_rows, 1000.0);
  EXPECT_DOUBLE_EQ(all_node_scan.cost, 1000.0);

  ir::CostEstimate label_scan = model.EstimateNodeScan({"Person"});
  EXPECT_DOUBLE_EQ(label_scan.estimated_rows, 100.0);
  EXPECT_DOUBLE_EQ(label_scan.cost, 100.0);

  ir::CostEstimate node_index_seek =
      model.EstimateNodeIndexSeek({"Person"}, "name");
  EXPECT_DOUBLE_EQ(node_index_seek.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(node_index_seek.cost, 1.0);

  ir::CostEstimate unique_node_index_seek =
      model.EstimateNodeIndexSeek({}, "id", true);
  EXPECT_DOUBLE_EQ(unique_node_index_seek.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(unique_node_index_seek.cost, 1.0);

  ir::CostEstimate node_index_range =
      model.EstimateNodeIndexRangeSeek({"Person"}, "age", 1);
  EXPECT_DOUBLE_EQ(node_index_range.estimated_rows, 10.0);
  EXPECT_DOUBLE_EQ(node_index_range.cost, 10.0);

  ir::CostEstimate relationship_type_scan =
      model.EstimateRelationshipTypeScan({"KNOWS"});
  EXPECT_DOUBLE_EQ(relationship_type_scan.estimated_rows, 5000.0);
  EXPECT_DOUBLE_EQ(relationship_type_scan.cost, 5000.0);

  ir::CostEstimate relationship_index_seek =
      model.EstimateRelationshipIndexSeek({"KNOWS"}, "since");
  EXPECT_DOUBLE_EQ(relationship_index_seek.estimated_rows, 50.0);
  EXPECT_DOUBLE_EQ(relationship_index_seek.cost, 50.0);

  ir::CostEstimate unique_relationship_index_seek =
      model.EstimateRelationshipIndexSeek({}, "id", true);
  EXPECT_DOUBLE_EQ(unique_relationship_index_seek.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(unique_relationship_index_seek.cost, 1.0);

  ir::CostEstimate relationship_index_range =
      model.EstimateRelationshipIndexRangeSeek({"KNOWS"}, "since", 1);
  EXPECT_DOUBLE_EQ(relationship_index_range.estimated_rows, 500.0);
  EXPECT_DOUBLE_EQ(relationship_index_range.cost, 500.0);

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

  ir::CostEstimate value_join =
      model.EstimateValueHashJoin({.estimated_rows = 100.0, .cost = 10.0},
                                  {.estimated_rows = 50.0, .cost = 20.0}, 2);
  EXPECT_DOUBLE_EQ(value_join.estimated_rows, 25.0);
  EXPECT_DOUBLE_EQ(value_join.cost, 205.0);

  ir::CostEstimate predicate_join =
      model.EstimatePredicateJoin({.estimated_rows = 100.0, .cost = 10.0},
                                  {.estimated_rows = 50.0, .cost = 20.0}, 2);
  EXPECT_DOUBLE_EQ(predicate_join.estimated_rows, 50.0);
  EXPECT_DOUBLE_EQ(predicate_join.cost, 230.0);
}

TEST(CostModelTest, UsesInjectedPlannerStatistics) {
  test_support::FakePlannerStatistics statistics;
  statistics.all_node_count = 2000.0;
  statistics.labeled_node_count = 25.0;
  statistics.untyped_expand_fanout = 7.0;
  statistics.typed_expand_fanout = 2.0;
  statistics.node_index_seek_selectivity = 0.02;
  statistics.node_index_range_seek_selectivity = 0.4;
  statistics.relationship_count_by_type = {{"KNOWS", 30.0}};
  statistics.relationship_index_seek_selectivity = 0.1;
  statistics.relationship_index_range_seek_selectivity = 0.5;
  statistics.untyped_expand_into_selectivity = 0.4;
  statistics.typed_expand_into_selectivity = 0.05;
  statistics.filter_selectivity = 0.25;
  statistics.node_hash_join_selectivity = 0.2;
  statistics.value_hash_join_selectivity = 0.3;
  statistics.predicate_join_selectivity = 0.4;
  ir::CostModel model(&statistics);

  ir::CostEstimate all_node_scan = model.EstimateNodeScan({});
  EXPECT_DOUBLE_EQ(all_node_scan.estimated_rows, 2000.0);
  EXPECT_DOUBLE_EQ(all_node_scan.cost, 2000.0);

  ir::CostEstimate label_scan = model.EstimateNodeScan({"Person"});
  EXPECT_DOUBLE_EQ(label_scan.estimated_rows, 25.0);
  EXPECT_DOUBLE_EQ(label_scan.cost, 25.0);

  ir::CostEstimate node_index_seek =
      model.EstimateNodeIndexSeek({"Person"}, "name");
  EXPECT_DOUBLE_EQ(node_index_seek.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(node_index_seek.cost, 1.0);

  ir::CostEstimate unique_node_index_seek =
      model.EstimateNodeIndexSeek({}, "id", true);
  EXPECT_DOUBLE_EQ(unique_node_index_seek.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(unique_node_index_seek.cost, 1.0);

  ir::CostEstimate node_index_range =
      model.EstimateNodeIndexRangeSeek({"Person"}, "age", 1);
  EXPECT_DOUBLE_EQ(node_index_range.estimated_rows, 10.0);
  EXPECT_DOUBLE_EQ(node_index_range.cost, 10.0);

  ir::CostEstimate relationship_type_scan =
      model.EstimateRelationshipTypeScan({"KNOWS"});
  EXPECT_DOUBLE_EQ(relationship_type_scan.estimated_rows, 30.0);
  EXPECT_DOUBLE_EQ(relationship_type_scan.cost, 30.0);

  ir::CostEstimate relationship_index_seek =
      model.EstimateRelationshipIndexSeek({"KNOWS"}, "since");
  EXPECT_DOUBLE_EQ(relationship_index_seek.estimated_rows, 3.0);
  EXPECT_DOUBLE_EQ(relationship_index_seek.cost, 3.0);

  ir::CostEstimate unique_relationship_index_seek =
      model.EstimateRelationshipIndexSeek({"KNOWS"}, "id", true);
  EXPECT_DOUBLE_EQ(unique_relationship_index_seek.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(unique_relationship_index_seek.cost, 1.0);

  ir::CostEstimate relationship_index_range =
      model.EstimateRelationshipIndexRangeSeek({"KNOWS"}, "since", 1);
  EXPECT_DOUBLE_EQ(relationship_index_range.estimated_rows, 15.0);
  EXPECT_DOUBLE_EQ(relationship_index_range.cost, 15.0);

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

  ir::CostEstimate value_join =
      model.EstimateValueHashJoin({.estimated_rows = 100.0, .cost = 10.0},
                                  {.estimated_rows = 50.0, .cost = 20.0}, 2);
  EXPECT_DOUBLE_EQ(value_join.estimated_rows, 15.0);
  EXPECT_DOUBLE_EQ(value_join.cost, 195.0);

  ir::CostEstimate predicate_join =
      model.EstimatePredicateJoin({.estimated_rows = 100.0, .cost = 10.0},
                                  {.estimated_rows = 50.0, .cost = 20.0}, 2);
  EXPECT_DOUBLE_EQ(predicate_join.estimated_rows, 2000.0);
  EXPECT_DOUBLE_EQ(predicate_join.cost, 2180.0);
}

TEST(CostModelTest, AppliesFilterEstimatesRepeatedly) {
  test_support::FakePlannerStatistics statistics;
  statistics.filter_selectivity = 0.25;
  ir::CostModel model(&statistics);

  ir::CostEstimate estimate = ir::ApplyFilterEstimates(
      {.estimated_rows = 64.0, .cost = 10.0}, 2, model);

  EXPECT_DOUBLE_EQ(estimate.estimated_rows, 4.0);
  EXPECT_DOUBLE_EQ(estimate.cost, 30.0);
}

TEST(CostModelTest, UsesCombinedFilterSelectivityForMultiplePredicates) {
  test_support::FakePlannerStatistics statistics;
  statistics.filter_selectivity = 0.2;
  statistics.combined_filter_selectivity = 0.05;
  ir::CostModel model(&statistics);

  ir::CostEstimate estimate = ir::ApplyFilterEstimates(
      {.estimated_rows = 100.0, .cost = 10.0}, 2, model);

  EXPECT_DOUBLE_EQ(estimate.estimated_rows, 5.0);
  EXPECT_DOUBLE_EQ(estimate.cost, 34.0);
}

TEST(CostModelTest, EstimatesRelationalOperatorMetadata) {
  test_support::FakePlannerStatistics statistics;
  statistics.distinct_selectivity = 0.25;
  statistics.aggregation_group_selectivity = 0.2;
  statistics.unwind_rows_per_input = 4.0;
  statistics.procedure_rows = 7.0;
  ir::CostModel model(&statistics);

  ir::CostEstimate input{.estimated_rows = 100.0, .cost = 10.0};

  ir::CostEstimate projection = model.EstimateProjection(input, 3);
  EXPECT_DOUBLE_EQ(projection.estimated_rows, 100.0);
  EXPECT_DOUBLE_EQ(projection.cost, 13.0);

  ir::CostEstimate distinct = model.EstimateDistinct(input, 1);
  EXPECT_DOUBLE_EQ(distinct.estimated_rows, 25.0);
  EXPECT_DOUBLE_EQ(distinct.cost, 135.0);

  ir::CostEstimate aggregation = model.EstimateAggregation(input, 1, 2);
  EXPECT_DOUBLE_EQ(aggregation.estimated_rows, 20.0);
  EXPECT_DOUBLE_EQ(aggregation.cost, 150.0);

  ir::CostEstimate global_aggregation = model.EstimateAggregation(input, 0, 1);
  EXPECT_DOUBLE_EQ(global_aggregation.estimated_rows, 1.0);
  EXPECT_DOUBLE_EQ(global_aggregation.cost, 121.0);

  ir::CostEstimate skipped = model.EstimateSkip(input, 10.0);
  EXPECT_DOUBLE_EQ(skipped.estimated_rows, 90.0);
  EXPECT_DOUBLE_EQ(skipped.cost, 110.0);

  ir::CostEstimate limited = model.EstimateLimit(input, 5.0);
  EXPECT_DOUBLE_EQ(limited.estimated_rows, 5.0);
  EXPECT_DOUBLE_EQ(limited.cost, 15.0);

  ir::CostEstimate unwind_unknown = model.EstimateUnwind(input, std::nullopt);
  EXPECT_DOUBLE_EQ(unwind_unknown.estimated_rows, 400.0);
  EXPECT_DOUBLE_EQ(unwind_unknown.cost, 410.0);

  ir::CostEstimate unwind_literal = model.EstimateUnwind(input, 2.0);
  EXPECT_DOUBLE_EQ(unwind_literal.estimated_rows, 200.0);
  EXPECT_DOUBLE_EQ(unwind_literal.cost, 210.0);

  ir::CostEstimate procedure = model.EstimateProcedureCall("db.labels", 1, 2);
  EXPECT_DOUBLE_EQ(procedure.estimated_rows, 7.0);
  EXPECT_DOUBLE_EQ(procedure.cost, 8.4);
}
