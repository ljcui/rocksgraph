#include "ir/logical_plan.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/exception.h"

namespace {

bool Contains(const std::unordered_set<std::string> &values,
              const std::string &value) {
  return values.find(value) != values.end();
}

}  // namespace

TEST(LogicalPlanTest, LeafPlansExposeNamesAndMetadata) {
  ir::AllNodeScanPlan all_scan("n");
  EXPECT_EQ(all_scan.Type(), ir::LogicalPlanNodeType::kAllNodeScan);
  EXPECT_EQ(all_scan.Name(), "AllNodeScan");
  EXPECT_EQ(all_scan.Variable(), "n");
  EXPECT_EQ(all_scan.Details(), "n");
  EXPECT_EQ(all_scan.OutputColumns(), std::vector<std::string>({"n"}));
  EXPECT_TRUE(Contains(all_scan.SolvedSymbols(), "n"));

  ir::NodeByLabelScanPlan label_scan("p", "Person");
  EXPECT_EQ(label_scan.Name(), "NodeByLabelScan");
  EXPECT_EQ(label_scan.Variable(), "p");
  EXPECT_EQ(label_scan.Label(), "Person");
  EXPECT_EQ(label_scan.Details(), "p:Person");
  EXPECT_EQ(label_scan.OutputColumns(), std::vector<std::string>({"p"}));
  EXPECT_TRUE(Contains(label_scan.SolvedSymbols(), "p"));
}

TEST(LogicalPlanTest, ExpandCopiesChildMetadataAndAddsPathSymbols) {
  auto source = std::make_unique<ir::NodeByLabelScanPlan>("a", "Person");
  ir::ExpandPlan expand(std::move(source), "a", "r", "b",
                        ir::ExpandDirection::kOutgoing, {"KNOWS"});

  EXPECT_EQ(expand.Name(), "Expand");
  EXPECT_EQ(expand.ChildCount(), 1U);
  EXPECT_EQ(expand.Child(0).Name(), "NodeByLabelScan");
  EXPECT_EQ(expand.FromNode(), "a");
  EXPECT_EQ(expand.Relationship(), "r");
  EXPECT_EQ(expand.ToNode(), "b");
  EXPECT_EQ(expand.Direction(), ir::ExpandDirection::kOutgoing);
  EXPECT_EQ(expand.Types(), std::vector<std::string>({"KNOWS"}));
  EXPECT_EQ(expand.Details(), "(a)-[r:KNOWS]->(b)");
  EXPECT_EQ(expand.OutputColumns(), std::vector<std::string>({"a", "r", "b"}));
  EXPECT_TRUE(Contains(expand.SolvedSymbols(), "a"));
  EXPECT_TRUE(Contains(expand.SolvedSymbols(), "r"));
  EXPECT_TRUE(Contains(expand.SolvedSymbols(), "b"));

  auto into_source =
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"a", "b"}));
  ir::ExpandIntoPlan expand_into(std::move(into_source), "a", "r", "b",
                                 ir::ExpandDirection::kOutgoing, {"KNOWS"});
  EXPECT_EQ(expand_into.Name(), "ExpandInto");
  EXPECT_EQ(expand_into.ChildCount(), 1U);
  EXPECT_EQ(expand_into.Child(0).Name(), "Argument");
  EXPECT_EQ(expand_into.FromNode(), "a");
  EXPECT_EQ(expand_into.Relationship(), "r");
  EXPECT_EQ(expand_into.ToNode(), "b");
  EXPECT_EQ(expand_into.Direction(), ir::ExpandDirection::kOutgoing);
  EXPECT_EQ(expand_into.Types(), std::vector<std::string>({"KNOWS"}));
  EXPECT_EQ(expand_into.Details(), "(a)-[r:KNOWS]->(b)");
  EXPECT_EQ(expand_into.OutputColumns(),
            std::vector<std::string>({"a", "b", "r"}));
  EXPECT_TRUE(Contains(expand_into.SolvedSymbols(), "a"));
  EXPECT_TRUE(Contains(expand_into.SolvedSymbols(), "r"));
  EXPECT_TRUE(Contains(expand_into.SolvedSymbols(), "b"));
}

TEST(LogicalPlanTest, UnaryPlansPreserveOrReplaceOutputColumns) {
  auto scan = std::make_unique<ir::AllNodeScanPlan>("n");
  ir::FilterPlan filter(std::move(scan), nullptr);
  EXPECT_EQ(filter.Name(), "Filter");
  EXPECT_EQ(filter.Predicate(), nullptr);
  EXPECT_EQ(filter.OutputColumns(), std::vector<std::string>({"n"}));
  EXPECT_TRUE(Contains(filter.SolvedSymbols(), "n"));

  std::vector<ir::LogicalProjectionItem> items;
  items.push_back({.expression = nullptr, .alias = "name"});
  auto filtered = std::make_unique<ir::FilterPlan>(
      std::make_unique<ir::AllNodeScanPlan>("n"), nullptr);
  ir::ProjectionPlan projection(std::move(filtered), std::move(items));
  EXPECT_EQ(projection.Name(), "Projection");
  EXPECT_EQ(projection.Details(), "name");
  EXPECT_EQ(projection.OutputColumns(), std::vector<std::string>({"name"}));
  EXPECT_TRUE(Contains(projection.SolvedSymbols(), "n"));
  EXPECT_TRUE(Contains(projection.SolvedSymbols(), "name"));

  auto distinct_source = std::make_unique<ir::AllNodeScanPlan>("n");
  ir::DistinctPlan distinct(std::move(distinct_source),
                            std::vector<ir::LogicalProjectionItem>{
                                {.expression = nullptr, .alias = "name"}});
  EXPECT_EQ(distinct.Name(), "Distinct");
  EXPECT_EQ(distinct.Details(), "name");
  EXPECT_EQ(distinct.OutputColumns(), std::vector<std::string>({"name"}));
  EXPECT_TRUE(Contains(distinct.SolvedSymbols(), "n"));
  EXPECT_TRUE(Contains(distinct.SolvedSymbols(), "name"));

  auto aggregation_source = std::make_unique<ir::AllNodeScanPlan>("n");
  ir::AggregationPlan aggregation(
      std::move(aggregation_source),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = nullptr, .alias = "name"}},
      std::vector<ir::LogicalProjectionItem>{
          {.expression = nullptr, .alias = "count"}});
  EXPECT_EQ(aggregation.Name(), "Aggregation");
  EXPECT_EQ(aggregation.Details(), "name, count");
  EXPECT_EQ(aggregation.OutputColumns(),
            std::vector<std::string>({"name", "count"}));
  EXPECT_TRUE(Contains(aggregation.SolvedSymbols(), "n"));
  EXPECT_TRUE(Contains(aggregation.SolvedSymbols(), "name"));
  EXPECT_TRUE(Contains(aggregation.SolvedSymbols(), "count"));

  auto projected = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::AllNodeScanPlan>("n"),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = nullptr, .alias = "n"}});
  ir::ProduceResultsPlan results(std::move(projected), {"n"});
  EXPECT_EQ(results.Name(), "ProduceResults");
  EXPECT_EQ(results.OutputColumns(), std::vector<std::string>({"n"}));
  EXPECT_TRUE(Contains(results.SolvedSymbols(), "n"));
}

TEST(LogicalPlanTest, BinaryPlansMergeSolvedSymbolsAndOutputs) {
  auto left = std::make_unique<ir::AllNodeScanPlan>("a");
  auto right = std::make_unique<ir::AllNodeScanPlan>("b");
  ir::CartesianProductPlan product(std::move(left), std::move(right));
  EXPECT_EQ(product.Name(), "CartesianProduct");
  EXPECT_EQ(product.ChildCount(), 2U);
  EXPECT_EQ(product.OutputColumns(), std::vector<std::string>({"a", "b"}));
  EXPECT_TRUE(Contains(product.SolvedSymbols(), "a"));
  EXPECT_TRUE(Contains(product.SolvedSymbols(), "b"));

  ir::ApplyPlan apply(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"outer"})),
      std::make_unique<ir::AllNodeScanPlan>("inner"));
  EXPECT_EQ(apply.OutputColumns(),
            std::vector<std::string>({"outer", "inner"}));
  EXPECT_TRUE(Contains(apply.SolvedSymbols(), "outer"));
  EXPECT_TRUE(Contains(apply.SolvedSymbols(), "inner"));

  ir::SemiApplyPlan semi_apply(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"outer"})),
      std::make_unique<ir::AllNodeScanPlan>("inner"));
  EXPECT_EQ(semi_apply.OutputColumns(), std::vector<std::string>({"outer"}));
  EXPECT_TRUE(Contains(semi_apply.SolvedSymbols(), "outer"));
  EXPECT_TRUE(Contains(semi_apply.SolvedSymbols(), "inner"));
}

TEST(LogicalPlanTest, InvalidChildAccessThrows) {
  ir::AllNodeScanPlan scan("n");
  EXPECT_THROW((void)scan.Child(0), common::InvalidArgumentError);
}
