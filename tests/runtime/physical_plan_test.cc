#include "runtime/physical_plan.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "ast/ast_builder.h"
#include "ir/query_ir.h"
#include "planner/logical_plan_builder.h"
#include "runtime/physical_plan_printer.h"

namespace {

struct PlannedQuery {
  std::unique_ptr<ast::Statement> statement;
  std::unique_ptr<ir::QueryIR> query_ir;
  std::unique_ptr<ir::LogicalPlan> logical_plan;
};

PlannedQuery Plan(std::string cypher) {
  PlannedQuery query;
  query.statement = ast::ParseCypherAndRewrite(std::move(cypher));
  query.query_ir = ir::CreateQueryIR(*query.statement);
  query.logical_plan = ir::CreateLogicalPlan(*query.query_ir);
  return query;
}

const ir::LogicalPlan *FindPlan(const ir::LogicalPlan &plan,
                                ir::LogicalPlanNodeType type) {
  if (plan.Type() == type) {
    return &plan;
  }
  for (const auto &child : plan.Children()) {
    if (const ir::LogicalPlan *found = FindPlan(*child, type)) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

TEST(PhysicalPlanTest, AllocatesTypedNullableSlotsForOptionalExpand) {
  PlannedQuery query =
      Plan("MATCH (n) OPTIONAL MATCH (n)-[r]->(m) RETURN n, r, m");
  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);

  const rg::Slot &n = physical.Root().output_slots->At("n");
  const rg::Slot &r = physical.Root().output_slots->At("r");
  const rg::Slot &m = physical.Root().output_slots->At("m");
  EXPECT_EQ(n.kind, rg::SlotKind::kNode);
  EXPECT_FALSE(n.nullable);
  EXPECT_EQ(r.kind, rg::SlotKind::kRelationship);
  EXPECT_TRUE(r.nullable);
  EXPECT_EQ(m.kind, rg::SlotKind::kNode);
  EXPECT_TRUE(m.nullable);

  const ir::LogicalPlan *expand =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kOptionalExpand);
  ASSERT_NE(expand, nullptr);
  const auto &expand_node = physical.NodeFor(*expand);
  ASSERT_EQ(expand_node.children.size(), 1U);
  EXPECT_EQ(expand_node.children[0]->output_slots->At("n").kind,
            rg::SlotKind::kNode);
}

TEST(PhysicalPlanTest, UnifiesIncompatibleUnionSlotsAsReferences) {
  PlannedQuery query =
      Plan("MATCH (n) RETURN n AS value UNION ALL RETURN 1 AS value");
  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);

  const rg::Slot &value = physical.Root().output_slots->At("value");
  EXPECT_EQ(value.kind, rg::SlotKind::kReference);
  EXPECT_FALSE(value.nullable);
  const ir::LogicalPlan *union_plan =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kUnion);
  ASSERT_NE(union_plan, nullptr);
  const rg::PhysicalPlanNode &union_node = physical.NodeFor(*union_plan);
  ASSERT_EQ(union_node.child_mappings.size(), 2U);
  ASSERT_EQ(union_node.child_mappings[0].size(), 1U);
  ASSERT_EQ(union_node.child_mappings[1].size(), 1U);
}

TEST(PhysicalPlanTest, PreservesScopedSemanticTypesForExpressions) {
  PlannedQuery query = Plan("MATCH (n) RETURN [n][0] AS x");
  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);

  const rg::Slot &x = physical.Root().output_slots->At("x");
  EXPECT_EQ(x.type, ast::SemanticVariableType::kNode);
  EXPECT_EQ(x.kind, rg::SlotKind::kNode);
}

TEST(PhysicalPlanTest, ChoosesSmallerValueHashJoinBuildSide) {
  PlannedQuery query =
      Plan("MATCH (a:Person), (b) WHERE a.id = b.id RETURN a, b");
  const ir::LogicalPlan *logical_join =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kValueHashJoin);
  ASSERT_NE(logical_join, nullptr);
  ASSERT_TRUE(logical_join->Child(0).EstimatedRows().has_value());
  ASSERT_TRUE(logical_join->Child(1).EstimatedRows().has_value());
  ASSERT_LT(*logical_join->Child(0).EstimatedRows(),
            *logical_join->Child(1).EstimatedRows());

  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);

  EXPECT_EQ(physical.NodeFor(*logical_join).value_hash_join_build_child, 0U);
}

TEST(PhysicalPlanTest, SelectsTopNForRewrittenLogicalPlan) {
  PlannedQuery query =
      Plan("UNWIND [3, 1, 2] AS x RETURN x ORDER BY x LIMIT $l");
  const ir::LogicalPlan *top_n =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kTopN);
  ASSERT_NE(top_n, nullptr);
  EXPECT_EQ(FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kLimit),
            nullptr);
  EXPECT_EQ(FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kSort),
            nullptr);

  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
  const rg::PhysicalPlanNode &top_n_node = physical.NodeFor(*top_n);

  EXPECT_EQ(top_n_node.type, rg::PhysicalOperatorType::kTopN);
  EXPECT_EQ(top_n_node.execution_kind, rg::PhysicalExecutionType::kTopN);
  EXPECT_EQ(top_n_node.top_n, top_n);
  ASSERT_EQ(top_n_node.children.size(), 1U);
  EXPECT_EQ(top_n_node.children[0]->logical, &top_n->Child(0));
}

TEST(PhysicalPlanTest, DoesNotRewriteTopNAcrossSkipOrWrites) {
  PlannedQuery skipped =
      Plan("UNWIND [3, 1, 2] AS x RETURN x ORDER BY x SKIP 1 LIMIT 1");
  const ir::LogicalPlan *skipped_limit =
      FindPlan(*skipped.logical_plan, ir::LogicalPlanNodeType::kLimit);
  ASSERT_NE(skipped_limit, nullptr);
  rg::PhysicalPlan skipped_physical =
      rg::CreatePhysicalPlan(*skipped.logical_plan);
  EXPECT_EQ(skipped_physical.NodeFor(*skipped_limit).type,
            rg::PhysicalOperatorType::kLogical);
  EXPECT_EQ(skipped_physical.NodeFor(*skipped_limit).execution_kind,
            rg::PhysicalExecutionType::kStreamingUnary);

  PlannedQuery write =
      Plan("MATCH (n) SET n.x = 1 RETURN n ORDER BY n LIMIT 2");
  const ir::LogicalPlan *write_limit =
      FindPlan(*write.logical_plan, ir::LogicalPlanNodeType::kLimit);
  ASSERT_NE(write_limit, nullptr);
  rg::PhysicalPlan write_physical = rg::CreatePhysicalPlan(*write.logical_plan);
  EXPECT_EQ(write_physical.NodeFor(*write_limit).type,
            rg::PhysicalOperatorType::kLogical);
  EXPECT_EQ(write_physical.NodeFor(*write_limit).execution_kind,
            rg::PhysicalExecutionType::kBlockingUnary);
}

TEST(PhysicalPlanTest, SelectsOrderedGroupingAlgorithms) {
  PlannedQuery distinct_query = Plan(
      "UNWIND [3, 1, 2, 1] AS x "
      "WITH x ORDER BY x RETURN DISTINCT x");
  const ir::LogicalPlan *distinct = FindPlan(
      *distinct_query.logical_plan, ir::LogicalPlanNodeType::kDistinct);
  ASSERT_NE(distinct, nullptr);
  rg::PhysicalPlan distinct_physical =
      rg::CreatePhysicalPlan(*distinct_query.logical_plan);
  const rg::PhysicalPlanNode &distinct_node =
      distinct_physical.NodeFor(*distinct);
  EXPECT_EQ(distinct_node.type, rg::PhysicalOperatorType::kOrderedDistinct);
  EXPECT_EQ(distinct_node.execution_kind,
            rg::PhysicalExecutionType::kOrderedDistinct);
  ASSERT_EQ(distinct_node.provided_order.size(), 1U);

  PlannedQuery aggregation_query = Plan(
      "UNWIND [3, 1, 2, 1] AS x "
      "WITH x ORDER BY x RETURN x, count(*) AS count");
  const ir::LogicalPlan *aggregation = FindPlan(
      *aggregation_query.logical_plan, ir::LogicalPlanNodeType::kAggregation);
  ASSERT_NE(aggregation, nullptr);
  rg::PhysicalPlan aggregation_physical =
      rg::CreatePhysicalPlan(*aggregation_query.logical_plan);
  const rg::PhysicalPlanNode &aggregation_node =
      aggregation_physical.NodeFor(*aggregation);
  EXPECT_EQ(aggregation_node.type,
            rg::PhysicalOperatorType::kOrderedAggregation);
  EXPECT_EQ(aggregation_node.execution_kind,
            rg::PhysicalExecutionType::kOrderedAggregation);
  ASSERT_EQ(aggregation_node.provided_order.size(), 1U);
  EXPECT_EQ(aggregation_physical.Root().provided_order.size(), 1U);
}

TEST(PhysicalPlanTest, SelectsPartialSortAndHashFallbacks) {
  PlannedQuery partial_query = Plan(
      "UNWIND [{a:1,b:2},{a:1,b:1},{a:2,b:1}] AS x "
      "WITH x ORDER BY x.a "
      "RETURN x.a AS a, x.b AS b ORDER BY a, b");
  const ir::LogicalPlan *outer_sort =
      FindPlan(*partial_query.logical_plan, ir::LogicalPlanNodeType::kSort);
  ASSERT_NE(outer_sort, nullptr);
  rg::PhysicalPlan partial_physical =
      rg::CreatePhysicalPlan(*partial_query.logical_plan);
  const rg::PhysicalPlanNode &partial_node =
      partial_physical.NodeFor(*outer_sort);
  EXPECT_EQ(partial_node.type, rg::PhysicalOperatorType::kPartialSort);
  EXPECT_EQ(partial_node.execution_kind,
            rg::PhysicalExecutionType::kPartialSort);
  EXPECT_EQ(partial_node.partial_sort_prefix, 1U);

  PlannedQuery fallback_query = Plan("UNWIND [3, 1, 2] AS x RETURN DISTINCT x");
  const ir::LogicalPlan *distinct = FindPlan(
      *fallback_query.logical_plan, ir::LogicalPlanNodeType::kDistinct);
  ASSERT_NE(distinct, nullptr);
  rg::PhysicalPlan fallback_physical =
      rg::CreatePhysicalPlan(*fallback_query.logical_plan);
  EXPECT_EQ(fallback_physical.NodeFor(*distinct).type,
            rg::PhysicalOperatorType::kHashDistinct);
  EXPECT_EQ(fallback_physical.NodeFor(*distinct).execution_kind,
            rg::PhysicalExecutionType::kBlockingUnary);

  PlannedQuery aggregation_query =
      Plan("UNWIND [3, 1, 2] AS x RETURN x, count(*) AS count");
  const ir::LogicalPlan *aggregation = FindPlan(
      *aggregation_query.logical_plan, ir::LogicalPlanNodeType::kAggregation);
  ASSERT_NE(aggregation, nullptr);
  rg::PhysicalPlan aggregation_physical =
      rg::CreatePhysicalPlan(*aggregation_query.logical_plan);
  EXPECT_EQ(aggregation_physical.NodeFor(*aggregation).type,
            rg::PhysicalOperatorType::kHashAggregation);
  EXPECT_EQ(aggregation_physical.NodeFor(*aggregation).execution_kind,
            rg::PhysicalExecutionType::kBlockingUnary);

  PlannedQuery sort_query = Plan("UNWIND [3, 1, 2] AS x RETURN x ORDER BY x");
  const ir::LogicalPlan *sort =
      FindPlan(*sort_query.logical_plan, ir::LogicalPlanNodeType::kSort);
  ASSERT_NE(sort, nullptr);
  rg::PhysicalPlan sort_physical =
      rg::CreatePhysicalPlan(*sort_query.logical_plan);
  EXPECT_EQ(sort_physical.NodeFor(*sort).type,
            rg::PhysicalOperatorType::kFullSort);
  EXPECT_EQ(sort_physical.NodeFor(*sort).execution_kind,
            rg::PhysicalExecutionType::kBlockingUnary);
}

TEST(PhysicalPlanTest, SelectsPartialTopNForProvidedOrderingPrefix) {
  PlannedQuery query = Plan(
      "UNWIND [{a:1,b:2},{a:1,b:1},{a:2,b:1}] AS x "
      "WITH x ORDER BY x.a "
      "RETURN x.a AS a, x.b AS b ORDER BY a, b LIMIT 2");
  const ir::LogicalPlan *top_n =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kTopN);
  ASSERT_NE(top_n, nullptr);

  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
  const rg::PhysicalPlanNode &node = physical.NodeFor(*top_n);

  EXPECT_EQ(node.type, rg::PhysicalOperatorType::kPartialTopN);
  EXPECT_EQ(node.execution_kind, rg::PhysicalExecutionType::kPartialTopN);
  EXPECT_EQ(node.partial_top_n_prefix, 1U);
  EXPECT_EQ(node.top_n, top_n);
}

TEST(PhysicalPlanTest, PrintsAlgorithmsPropertiesAndSlots) {
  PlannedQuery query = Plan(
      "UNWIND [{a:1,b:2},{a:1,b:1}] AS x "
      "WITH x ORDER BY x.a "
      "RETURN x.a AS a, x.b AS b ORDER BY a, b");
  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);

  const std::string printed = rg::PhysicalPlanToString(physical);

  EXPECT_NE(printed.find("PartialSort"), std::string::npos);
  EXPECT_NE(printed.find("logical=Sort"), std::string::npos);
  EXPECT_NE(printed.find("exec=PartialSort"), std::string::npos);
  EXPECT_NE(printed.find("order=[a ASC, b ASC]"), std::string::npos);
  EXPECT_NE(printed.find("prefix=1"), std::string::npos);
  EXPECT_NE(printed.find("slots=[a:reference@0?"), std::string::npos);
}
