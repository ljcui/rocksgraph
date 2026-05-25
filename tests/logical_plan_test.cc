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

TEST(LogicalPlanTest, VarExpandAndPathBuildExposeMetadata) {
  auto source = std::make_unique<ir::AllNodeScanPlan>("a");
  ir::VarExpandPlan var_expand(std::move(source), "a", "r", "b",
                               ir::ExpandDirection::kOutgoing, {"KNOWS"},
                               {.min = 1, .max = 3});

  EXPECT_EQ(var_expand.Name(), "VarExpand");
  EXPECT_EQ(var_expand.ChildCount(), 1U);
  EXPECT_EQ(var_expand.FromNode(), "a");
  EXPECT_EQ(var_expand.Relationship(), "r");
  EXPECT_EQ(var_expand.ToNode(), "b");
  EXPECT_EQ(var_expand.Direction(), ir::ExpandDirection::kOutgoing);
  EXPECT_EQ(var_expand.Types(), std::vector<std::string>({"KNOWS"}));
  ASSERT_TRUE(var_expand.Length().min.has_value());
  ASSERT_TRUE(var_expand.Length().max.has_value());
  EXPECT_EQ(*var_expand.Length().min, 1);
  EXPECT_EQ(*var_expand.Length().max, 3);
  EXPECT_EQ(var_expand.Details(), "(a)-[r:KNOWS*1..3]->(b)");
  EXPECT_EQ(var_expand.OutputColumns(),
            std::vector<std::string>({"a", "r", "b"}));
  EXPECT_TRUE(Contains(var_expand.SolvedSymbols(), "a"));
  EXPECT_TRUE(Contains(var_expand.SolvedSymbols(), "r"));
  EXPECT_TRUE(Contains(var_expand.SolvedSymbols(), "b"));

  auto path_source = std::make_unique<ir::ArgumentPlan>(
      std::vector<std::string>({"a", "r", "b"}));
  ir::PathBuildPlan path(std::move(path_source), "p");
  EXPECT_EQ(path.Name(), "PathBuild");
  EXPECT_EQ(path.PathVariable(), "p");
  EXPECT_EQ(path.Details(), "p");
  EXPECT_EQ(path.OutputColumns(),
            std::vector<std::string>({"a", "r", "b", "p"}));
  EXPECT_TRUE(Contains(path.SolvedSymbols(), "p"));
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

  ir::NodeHashJoinPlan join(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"b", "a"})),
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"b", "c"})),
      {"b"});
  EXPECT_EQ(join.Name(), "NodeHashJoin");
  EXPECT_EQ(join.Details(), "b");
  EXPECT_EQ(join.JoinKeys(), std::vector<std::string>({"b"}));
  EXPECT_EQ(join.OutputColumns(), std::vector<std::string>({"b", "a", "c"}));
  EXPECT_TRUE(Contains(join.SolvedSymbols(), "a"));
  EXPECT_TRUE(Contains(join.SolvedSymbols(), "b"));
  EXPECT_TRUE(Contains(join.SolvedSymbols(), "c"));

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

  ir::OptionalApplyPlan optional_apply(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"outer"})),
      std::make_unique<ir::AllNodeScanPlan>("inner"));
  EXPECT_EQ(optional_apply.Name(), "OptionalApply");
  EXPECT_EQ(optional_apply.OutputColumns(),
            std::vector<std::string>({"outer", "inner"}));
  EXPECT_TRUE(Contains(optional_apply.SolvedSymbols(), "outer"));
  EXPECT_TRUE(Contains(optional_apply.SolvedSymbols(), "inner"));
}

TEST(LogicalPlanTest, NestedApplyPlansExposeComputedOutputs) {
  ir::LetSemiApplyPlan let_apply(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"outer"})),
      std::make_unique<ir::AllNodeScanPlan>("inner"), "__exists");
  EXPECT_EQ(let_apply.Name(), "LetSemiApply");
  EXPECT_EQ(let_apply.ValueVariable(), "__exists");
  EXPECT_EQ(let_apply.Details(), "__exists");
  EXPECT_EQ(let_apply.OutputColumns(),
            std::vector<std::string>({"outer", "__exists"}));
  EXPECT_TRUE(Contains(let_apply.SolvedSymbols(), "outer"));
  EXPECT_TRUE(Contains(let_apply.SolvedSymbols(), "inner"));
  EXPECT_TRUE(Contains(let_apply.SolvedSymbols(), "__exists"));

  ir::RollUpApplyPlan roll_up(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"outer"})),
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"__value"})),
      "__list", "__value");
  EXPECT_EQ(roll_up.Name(), "RollUpApply");
  EXPECT_EQ(roll_up.CollectionVariable(), "__list");
  EXPECT_EQ(roll_up.ValueVariable(), "__value");
  EXPECT_EQ(roll_up.Details(), "__list <- __value");
  EXPECT_EQ(roll_up.OutputColumns(),
            std::vector<std::string>({"outer", "__list"}));
  EXPECT_TRUE(Contains(roll_up.SolvedSymbols(), "outer"));
  EXPECT_TRUE(Contains(roll_up.SolvedSymbols(), "__list"));
  EXPECT_TRUE(Contains(roll_up.SolvedSymbols(), "__value"));
}

TEST(LogicalPlanTest, UnwindAndUnionPlansExposeOutputs) {
  ir::UnwindPlan unwind(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"row"})),
      nullptr, "x");
  EXPECT_EQ(unwind.Name(), "Unwind");
  EXPECT_EQ(unwind.Expression(), nullptr);
  EXPECT_EQ(unwind.Alias(), "x");
  EXPECT_EQ(unwind.Details(), "null AS x");
  EXPECT_EQ(unwind.OutputColumns(), std::vector<std::string>({"row", "x"}));
  EXPECT_TRUE(Contains(unwind.SolvedSymbols(), "row"));
  EXPECT_TRUE(Contains(unwind.SolvedSymbols(), "x"));

  ir::UnionPlan union_plan(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"a"})),
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"b"})),
      std::vector<ir::LogicalUnionMapping>{
          {.output_variable = "x", .lhs_variable = "a", .rhs_variable = "b"}},
      false);
  EXPECT_EQ(union_plan.Name(), "Union");
  EXPECT_FALSE(union_plan.All());
  ASSERT_EQ(union_plan.Mappings().size(), 1U);
  EXPECT_EQ(union_plan.Mappings()[0].output_variable, "x");
  EXPECT_EQ(union_plan.Details(), "DISTINCT x");
  EXPECT_EQ(union_plan.OutputColumns(), std::vector<std::string>({"x"}));
  EXPECT_TRUE(Contains(union_plan.SolvedSymbols(), "x"));
}

TEST(LogicalPlanTest, AssertIsNodePreservesInputs) {
  ir::AssertIsNodePlan assert_is_node(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>({"n"})),
      {"n"});

  EXPECT_EQ(assert_is_node.Name(), "AssertIsNode");
  EXPECT_EQ(assert_is_node.Variables(), std::vector<std::string>({"n"}));
  EXPECT_EQ(assert_is_node.Details(), "n");
  EXPECT_EQ(assert_is_node.OutputColumns(), std::vector<std::string>({"n"}));
  EXPECT_TRUE(Contains(assert_is_node.SolvedSymbols(), "n"));
}

TEST(LogicalPlanTest, InvalidChildAccessThrows) {
  ir::AllNodeScanPlan scan("n");
  EXPECT_THROW((void)scan.Child(0), common::InvalidArgumentError);
}
