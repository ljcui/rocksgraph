#include "runtime/physical_plan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ast/ast_builder.h"
#include "ir/query_ir.h"
#include "planner/logical_plan_builder.h"
#include "runtime/physical_plan_printer.h"
#include "runtime/slotted_executor.h"
#include "storage/in_memory_graph.h"

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

const rg::PhysicalPlanNode *FindPhysicalPlan(const rg::PhysicalPlanNode &node,
                                             rg::PhysicalOperatorKind kind) {
  if (node.kind == kind) {
    return &node;
  }
  for (const auto &child : node.children) {
    if (const rg::PhysicalPlanNode *found = FindPhysicalPlan(*child, kind)) {
      return found;
    }
  }
  return nullptr;
}

rg::PhysicalPlan DetachedPhysicalPlan(const std::string &cypher) {
  PlannedQuery query = Plan(cypher);
  return rg::CreatePhysicalPlan(*query.logical_plan);
}

rg::PhysicalPlan DetachedRelationshipTypeScan() {
  ir::RelationshipTypeScanPlan logical("a", "r", "b",
                                       ir::ExpandDirection::kOutgoing, {"R"});
  return rg::CreatePhysicalPlan(logical);
}

std::vector<std::int64_t> FirstColumnIds(const rg::PhysicalPlan &plan,
                                         const rg::GraphReader &graph,
                                         std::string column) {
  std::unique_ptr<rg::PhysicalResultCursor> cursor = rg::StartPhysicalPlan(
      plan, graph, nullptr, {}, std::vector<std::string>{std::move(column)});
  std::vector<std::int64_t> ids;
  std::vector<rg::Value> row;
  while (cursor->Next(&row)) {
    EXPECT_EQ(row.size(), 1U);
    if (row.front().IsInteger()) {
      ids.push_back(row.front().AsInteger());
    } else if (row.front().IsNode()) {
      ids.push_back(row.front().AsNode().id);
    } else if (row.front().IsRelationship()) {
      ids.push_back(row.front().AsRelationship().id);
    } else {
      ADD_FAILURE() << "first result column is not an entity or integer";
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
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

TEST(PhysicalPlanTest, BuildsOwnedPayloadsForMigratedOperators) {
  PlannedQuery query =
      Plan("MATCH (n) WHERE id(n) > 0 RETURN n AS node SKIP 1 LIMIT 2");
  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);

  const ir::LogicalPlan *scan =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kAllNodeScan);
  const ir::LogicalPlan *filter =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kFilter);
  const ir::LogicalPlan *projection =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kProjection);
  const ir::LogicalPlan *skip =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kSkip);
  const ir::LogicalPlan *limit =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kLimit);
  ASSERT_NE(scan, nullptr);
  ASSERT_NE(filter, nullptr);
  ASSERT_NE(projection, nullptr);
  ASSERT_NE(skip, nullptr);
  ASSERT_NE(limit, nullptr);

  const auto &scan_node = physical.NodeFor(*scan);
  EXPECT_EQ(scan_node.kind, rg::PhysicalOperatorKind::kAllNodeScan);
  EXPECT_EQ(std::get<rg::AllNodeScanOp>(scan_node.data).variable, "n");

  const auto &filter_node = physical.NodeFor(*filter);
  const auto &filter_data = std::get<rg::FilterOp>(filter_node.data);
  EXPECT_EQ(filter_node.kind, rg::PhysicalOperatorKind::kFilter);
  EXPECT_NE(filter_data.predicate.Expression(),
            static_cast<const ir::FilterPlan &>(*filter).Predicate());

  const auto &projection_node = physical.NodeFor(*projection);
  const auto &projection_data =
      std::get<rg::ProjectionOp>(projection_node.data);
  EXPECT_EQ(projection_node.kind, rg::PhysicalOperatorKind::kProjection);
  ASSERT_EQ(projection_data.items.size(), 1U);
  EXPECT_EQ(projection_data.items.front().alias, "node");

  EXPECT_EQ(physical.NodeFor(*skip).kind, rg::PhysicalOperatorKind::kSkip);
  EXPECT_EQ(physical.NodeFor(*limit).kind, rg::PhysicalOperatorKind::kLimit);
  EXPECT_EQ(physical.Root().kind, rg::PhysicalOperatorKind::kProduceResults);
}

TEST(PhysicalPlanTest, ExecutesMigratedPlanAfterLogicalPlanIsDestroyed) {
  rg::PhysicalPlan physical = [] {
    PlannedQuery query =
        Plan("MATCH (n) WHERE id(n) > 0 RETURN id(n) AS id SKIP 1 LIMIT 1");
    return rg::CreatePhysicalPlan(*query.logical_plan);
  }();
  rg::InMemoryGraph graph;
  graph.CreateNode({});
  graph.CreateNode({});
  graph.CreateNode({});

  std::unique_ptr<rg::PhysicalResultCursor> cursor = rg::StartPhysicalPlan(
      physical, graph, nullptr, {}, std::vector<std::string>{"id"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  ASSERT_EQ(row.size(), 1U);
  EXPECT_EQ(row.front(), rg::Value(2));
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(PhysicalPlanTest, BuildsTypedOwnedPayloadsForLeafAccessOperators) {
  {
    PlannedQuery query = Plan("RETURN 1 AS value");
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const rg::PhysicalPlanNode *node =
        FindPhysicalPlan(physical.Root(), rg::PhysicalOperatorKind::kArgument);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(std::holds_alternative<rg::ArgumentOp>(node->data));
  }
  {
    PlannedQuery query = Plan("MATCH (n:N) RETURN id(n) AS id");
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const rg::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), rg::PhysicalOperatorKind::kNodeByLabelScan);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<rg::NodeByLabelScanOp>(node->data);
    EXPECT_EQ(data.variable, "n");
    EXPECT_EQ(data.labels, (std::vector<std::string>{"N"}));
  }
  {
    PlannedQuery query =
        Plan("MATCH (n:N) WHERE n.value = 10 RETURN id(n) AS id");
    const auto *logical = static_cast<const ir::NodeIndexSeekPlan *>(
        FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kNodeIndexSeek));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::NodeIndexSeekOp>(physical.NodeFor(*logical).data);
    EXPECT_EQ(data.property_key, "value");
    EXPECT_NE(data.value.Expression(), logical->ValueExpression());
  }
  {
    PlannedQuery query =
        Plan("MATCH (n:N) WHERE n.value >= 10 RETURN id(n) AS id");
    const auto *logical =
        static_cast<const ir::NodeIndexRangeSeekPlan *>(FindPlan(
            *query.logical_plan, ir::LogicalPlanNodeType::kNodeIndexRangeSeek));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::NodeIndexRangeSeekOp>(physical.NodeFor(*logical).data);
    ASSERT_EQ(data.predicates.size(), logical->Predicates().size());
    EXPECT_NE(data.predicates.front().Expression(),
              logical->Predicates().front());
  }
  {
    ir::RelationshipTypeScanPlan logical("a", "r", "b",
                                         ir::ExpandDirection::kOutgoing, {"R"});
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(logical);
    const auto &data =
        std::get<rg::RelationshipTypeScanOp>(physical.Root().data);
    EXPECT_EQ(data.pattern.relationship, "r");
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
  }
  {
    PlannedQuery query =
        Plan("MATCH ()-[r:R]->() WHERE r.value = 10 RETURN id(r) AS id");
    const auto *logical = static_cast<const ir::RelationshipIndexSeekPlan *>(
        FindPlan(*query.logical_plan,
                 ir::LogicalPlanNodeType::kRelationshipIndexSeek));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::RelationshipIndexSeekOp>(physical.NodeFor(*logical).data);
    EXPECT_EQ(data.pattern.relationship, "r");
    EXPECT_NE(data.value.Expression(), logical->ValueExpression());
  }
  {
    PlannedQuery query =
        Plan("MATCH ()-[r:R]->() WHERE r.value >= 10 RETURN id(r) AS id");
    const auto *logical =
        static_cast<const ir::RelationshipIndexRangeSeekPlan *>(
            FindPlan(*query.logical_plan,
                     ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data = std::get<rg::RelationshipIndexRangeSeekOp>(
        physical.NodeFor(*logical).data);
    ASSERT_EQ(data.predicates.size(), logical->Predicates().size());
    EXPECT_NE(data.predicates.front().Expression(),
              logical->Predicates().front());
  }
  {
    PlannedQuery query =
        Plan("MATCH (n) WHERE id(n) IN [0, 1] RETURN id(n) AS id");
    const auto *logical = static_cast<const ir::NodeByIdSeekPlan *>(
        FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kNodeByIdSeek));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::NodeByIdSeekOp>(physical.NodeFor(*logical).data);
    EXPECT_TRUE(data.many);
    EXPECT_NE(data.ids.Expression(), logical->Ids());
  }
  {
    PlannedQuery query =
        Plan("MATCH ()-[r]->() WHERE id(r) IN [0, 1] RETURN id(r) AS id");
    const auto *logical = static_cast<const ir::RelationshipByIdSeekPlan *>(
        FindPlan(*query.logical_plan,
                 ir::LogicalPlanNodeType::kRelationshipByIdSeek));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::RelationshipByIdSeekOp>(physical.NodeFor(*logical).data);
    EXPECT_TRUE(data.many);
    EXPECT_NE(data.ids.Expression(), logical->Ids());
  }
}

TEST(PhysicalPlanTest, ExecutesLeafAccessAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({"N"}, {{"value", rg::Value(10)}});
  auto second = graph.CreateNode({"N"}, {{"value", rg::Value(20)}});
  auto relationship =
      graph.CreateRelationship(first, second, "R", {{"value", rg::Value(10)}});
  graph.AddNodeIndex({"N"}, "value");
  graph.AddRelationshipIndex({"R"}, "value");

  EXPECT_EQ(
      FirstColumnIds(DetachedPhysicalPlan("RETURN 1 AS value"), graph, "value"),
      (std::vector<std::int64_t>{1}));
  EXPECT_EQ(
      FirstColumnIds(DetachedPhysicalPlan("MATCH (n:N) RETURN id(n) AS id"),
                     graph, "id"),
      (std::vector<std::int64_t>{first->id, second->id}));
  EXPECT_EQ(
      FirstColumnIds(DetachedPhysicalPlan(
                         "MATCH (n:N) WHERE n.value = 10 RETURN id(n) AS id"),
                     graph, "id"),
      (std::vector<std::int64_t>{first->id}));
  EXPECT_EQ(
      FirstColumnIds(DetachedPhysicalPlan(
                         "MATCH (n:N) WHERE n.value >= 20 RETURN id(n) AS id"),
                     graph, "id"),
      (std::vector<std::int64_t>{second->id}));
  EXPECT_EQ(FirstColumnIds(DetachedRelationshipTypeScan(), graph, "r"),
            (std::vector<std::int64_t>{relationship->id}));
  EXPECT_EQ(FirstColumnIds(
                DetachedPhysicalPlan(
                    "MATCH ()-[r:R]->() WHERE r.value = 10 RETURN id(r) AS id"),
                graph, "id"),
            (std::vector<std::int64_t>{relationship->id}));
  EXPECT_EQ(
      FirstColumnIds(
          DetachedPhysicalPlan(
              "MATCH ()-[r:R]->() WHERE r.value >= 10 RETURN id(r) AS id"),
          graph, "id"),
      (std::vector<std::int64_t>{relationship->id}));
  EXPECT_EQ(FirstColumnIds(
                DetachedPhysicalPlan(
                    "MATCH (n) WHERE id(n) IN [0, 0, 99] RETURN id(n) AS id"),
                graph, "id"),
            (std::vector<std::int64_t>{first->id}));
  EXPECT_EQ(FirstColumnIds(DetachedPhysicalPlan(
                               "MATCH ()-[r]->() WHERE id(r) IN [0, 0, 99] "
                               "RETURN id(r) AS id"),
                           graph, "id"),
            (std::vector<std::int64_t>{relationship->id}));
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

  EXPECT_EQ(top_n_node.kind, rg::PhysicalOperatorKind::kTopN);
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
  const rg::PhysicalPlanNode &skipped_limit_node =
      skipped_physical.NodeFor(*skipped_limit);
  EXPECT_EQ(skipped_limit_node.kind, rg::PhysicalOperatorKind::kLimit);
  EXPECT_FALSE(std::get<rg::LimitOp>(skipped_limit_node.data).exhaust_child);

  PlannedQuery write =
      Plan("MATCH (n) SET n.x = 1 RETURN n ORDER BY n LIMIT 2");
  const ir::LogicalPlan *write_limit =
      FindPlan(*write.logical_plan, ir::LogicalPlanNodeType::kLimit);
  ASSERT_NE(write_limit, nullptr);
  rg::PhysicalPlan write_physical = rg::CreatePhysicalPlan(*write.logical_plan);
  const rg::PhysicalPlanNode &write_limit_node =
      write_physical.NodeFor(*write_limit);
  EXPECT_EQ(write_limit_node.kind, rg::PhysicalOperatorKind::kLimit);
  EXPECT_TRUE(std::get<rg::LimitOp>(write_limit_node.data).exhaust_child);
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
  EXPECT_EQ(distinct_node.kind, rg::PhysicalOperatorKind::kOrderedDistinct);
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
  EXPECT_EQ(aggregation_node.kind,
            rg::PhysicalOperatorKind::kOrderedAggregation);
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
  EXPECT_EQ(partial_node.kind, rg::PhysicalOperatorKind::kPartialSort);
  EXPECT_EQ(partial_node.partial_sort_prefix, 1U);

  PlannedQuery fallback_query = Plan("UNWIND [3, 1, 2] AS x RETURN DISTINCT x");
  const ir::LogicalPlan *distinct = FindPlan(
      *fallback_query.logical_plan, ir::LogicalPlanNodeType::kDistinct);
  ASSERT_NE(distinct, nullptr);
  rg::PhysicalPlan fallback_physical =
      rg::CreatePhysicalPlan(*fallback_query.logical_plan);
  EXPECT_EQ(fallback_physical.NodeFor(*distinct).kind,
            rg::PhysicalOperatorKind::kHashDistinct);

  PlannedQuery aggregation_query =
      Plan("UNWIND [3, 1, 2] AS x RETURN x, count(*) AS count");
  const ir::LogicalPlan *aggregation = FindPlan(
      *aggregation_query.logical_plan, ir::LogicalPlanNodeType::kAggregation);
  ASSERT_NE(aggregation, nullptr);
  rg::PhysicalPlan aggregation_physical =
      rg::CreatePhysicalPlan(*aggregation_query.logical_plan);
  EXPECT_EQ(aggregation_physical.NodeFor(*aggregation).kind,
            rg::PhysicalOperatorKind::kHashAggregation);

  PlannedQuery sort_query = Plan("UNWIND [3, 1, 2] AS x RETURN x ORDER BY x");
  const ir::LogicalPlan *sort =
      FindPlan(*sort_query.logical_plan, ir::LogicalPlanNodeType::kSort);
  ASSERT_NE(sort, nullptr);
  rg::PhysicalPlan sort_physical =
      rg::CreatePhysicalPlan(*sort_query.logical_plan);
  EXPECT_EQ(sort_physical.NodeFor(*sort).kind,
            rg::PhysicalOperatorKind::kFullSort);
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

  EXPECT_EQ(node.kind, rg::PhysicalOperatorKind::kPartialTopN);
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
  EXPECT_EQ(printed.find("exec="), std::string::npos);
  EXPECT_NE(printed.find("order=[a ASC, b ASC]"), std::string::npos);
  EXPECT_NE(printed.find("prefix=1"), std::string::npos);
  EXPECT_NE(printed.find("slots=[a:reference@0?"), std::string::npos);
}
