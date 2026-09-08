#include "runtime/physical_plan.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ast/ast_builder.h"
#include "common/exception.h"
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

std::unique_ptr<ast::Parameter> Parameter(std::string name) {
  auto parameter = std::make_unique<ast::Parameter>();
  parameter->name = std::move(name);
  return parameter;
}

std::unique_ptr<ast::IntegerLiteral> Integer(std::int64_t value) {
  auto literal = std::make_unique<ast::IntegerLiteral>();
  literal->value = value;
  return literal;
}

rg::PhysicalPlan DetachedParameterNodeHashJoin() {
  std::unique_ptr<ast::Parameter> left_node = Parameter("left_node");
  std::unique_ptr<ast::Parameter> left_value = Parameter("left_value");
  std::unique_ptr<ast::Parameter> right_node = Parameter("right_node");
  std::unique_ptr<ast::Parameter> right_value = Parameter("right_value");
  auto left = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = left_node.get(),
           .alias = "n",
           .semantic_type = ast::SemanticVariableType::kNode},
          {.expression = left_value.get(), .alias = "value"}});
  auto right = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = right_node.get(),
           .alias = "n",
           .semantic_type = ast::SemanticVariableType::kNode},
          {.expression = right_value.get(), .alias = "value"}});
  ir::NodeHashJoinPlan logical(std::move(left), std::move(right), {"n"});
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedLeftOuterHashJoin() {
  auto left = std::make_unique<ir::NodeByLabelScanPlan>("a", "Input");
  auto right = std::make_unique<ir::ExpandPlan>(
      std::make_unique<ir::NodeByLabelScanPlan>("a", "Input"), "a", "r", "b",
      ir::ExpandDirection::kOutgoing, std::vector<std::string>{"R"});
  ir::LeftOuterHashJoinPlan logical(std::move(left), std::move(right), {"a"});
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedCartesianProduct() {
  ir::CartesianProductPlan logical(
      std::make_unique<ir::NodeByLabelScanPlan>("a", "Left"),
      std::make_unique<ir::NodeByLabelScanPlan>("b", "Right"));
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedParameterCartesianProduct() {
  std::unique_ptr<ast::Parameter> left_shared = Parameter("left_shared");
  std::unique_ptr<ast::Parameter> left_value = Parameter("left_value");
  std::unique_ptr<ast::Parameter> right_shared = Parameter("right_shared");
  std::unique_ptr<ast::Parameter> right_value = Parameter("right_value");
  auto left = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = left_shared.get(), .alias = "shared"},
          {.expression = left_value.get(), .alias = "left"}});
  auto right = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = right_shared.get(), .alias = "shared"},
          {.expression = right_value.get(), .alias = "right"}});
  ir::CartesianProductPlan logical(std::move(left), std::move(right));
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedLimitedApply() {
  std::unique_ptr<ast::IntegerLiteral> one = Integer(1);
  auto right = std::make_unique<ir::LimitPlan>(
      std::make_unique<ir::ExpandPlan>(
          std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"n"}),
          "n", "r", "m", ir::ExpandDirection::kOutgoing,
          std::vector<std::string>{"R"}),
      one.get());
  ir::ApplyPlan logical(std::make_unique<ir::NodeByLabelScanPlan>("n", "Input"),
                        std::move(right));
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedOptionalApply() {
  auto right = std::make_unique<ir::ExpandPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"n"}), "n",
      "r", "m", ir::ExpandDirection::kOutgoing, std::vector<std::string>{"R"});
  ir::OptionalApplyPlan logical(
      std::make_unique<ir::NodeByLabelScanPlan>("n", "Input"),
      std::move(right));
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedConflictingApply() {
  std::unique_ptr<ast::Parameter> left_shared = Parameter("left_shared");
  std::unique_ptr<ast::Parameter> left_value = Parameter("left_value");
  std::unique_ptr<ast::Parameter> right_shared = Parameter("right_shared");
  std::unique_ptr<ast::Parameter> right_value = Parameter("right_value");
  auto left = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = left_shared.get(), .alias = "shared"},
          {.expression = left_value.get(), .alias = "left"}});
  auto right = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"shared"}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = right_shared.get(), .alias = "shared"},
          {.expression = right_value.get(), .alias = "right"}});
  ir::ApplyPlan logical(std::move(left), std::move(right));
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedExpandInto() {
  std::unique_ptr<ast::Parameter> from = Parameter("from");
  std::unique_ptr<ast::Parameter> to = Parameter("to");
  auto source = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = from.get(),
           .alias = "a",
           .semantic_type = ast::SemanticVariableType::kNode},
          {.expression = to.get(),
           .alias = "b",
           .semantic_type = ast::SemanticVariableType::kNode}});
  ir::ExpandIntoPlan logical(std::move(source), "a", "r", "b",
                             ir::ExpandDirection::kOutgoing, {"R"});
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedProjectEndpoints(bool variable_length) {
  std::unique_ptr<ast::Parameter> relationship = Parameter("relationship");
  auto source = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = relationship.get(),
           .alias = "r",
           .semantic_type = variable_length
                                ? ast::SemanticVariableType::kList
                                : ast::SemanticVariableType::kRelationship}});
  ir::PatternRelationship pattern{.variable = "r",
                                  .left_node = "a",
                                  .right_node = "b",
                                  .direction = ir::Direction::kOutgoing,
                                  .types = {"R"}};
  pattern.length.variable = variable_length;
  if (variable_length) {
    pattern.length.min = 2;
    pattern.length.max = 2;
  }
  ir::ProjectEndpointsPlan logical(std::move(source), std::move(pattern));
  return rg::CreatePhysicalPlan(logical);
}

rg::PhysicalPlan DetachedPruningVarExpand() {
  std::unique_ptr<ast::Parameter> from = Parameter("from");
  auto source = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = from.get(),
           .alias = "a",
           .semantic_type = ast::SemanticVariableType::kNode}});
  ir::PatternRelationship pattern{.variable = "rs",
                                  .left_node = "a",
                                  .right_node = "b",
                                  .direction = ir::Direction::kOutgoing,
                                  .types = {"R"}};
  pattern.length.variable = true;
  pattern.length.min = 0;
  pattern.length.max = 2;
  ir::PruningVarExpandPlan logical(std::move(source), std::move(pattern));
  return rg::CreatePhysicalPlan(logical);
}

std::vector<std::vector<rg::Value>> PhysicalRows(
    const rg::PhysicalPlan &plan, const rg::GraphReader &graph,
    const std::vector<std::string> &columns,
    const rg::QueryParameters &parameters = {}) {
  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(plan, graph, nullptr, parameters, columns);
  std::vector<std::vector<rg::Value>> rows;
  std::vector<rg::Value> row;
  while (cursor->Next(&row)) {
    rows.push_back(row);
  }
  return rows;
}

std::vector<std::vector<rg::Value>> PhysicalWriteRows(
    const rg::PhysicalPlan &plan, rg::InMemoryGraph *graph,
    const std::vector<std::string> &columns,
    const rg::QueryParameters &parameters = {}) {
  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(plan, *graph, graph, parameters, columns);
  std::vector<std::vector<rg::Value>> rows;
  std::vector<rg::Value> row;
  while (cursor->Next(&row)) {
    rows.push_back(row);
  }
  return rows;
}

std::vector<std::int64_t> FirstColumnIds(
    const rg::PhysicalPlan &plan, const rg::GraphReader &graph,
    std::string column, const rg::QueryParameters &parameters = {}) {
  std::vector<std::int64_t> ids;
  for (const auto &row :
       PhysicalRows(plan, graph, std::vector<std::string>{std::move(column)},
                    parameters)) {
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
  EXPECT_EQ(union_node.kind, rg::PhysicalOperatorKind::kUnionAll);
  EXPECT_TRUE(std::holds_alternative<rg::UnionAllOp>(union_node.data));
  ASSERT_EQ(union_node.child_mappings.size(), 2U);
  ASSERT_EQ(union_node.child_mappings[0].size(), 1U);
  ASSERT_EQ(union_node.child_mappings[1].size(), 1U);
}

TEST(PhysicalPlanTest, SelectsDedicatedUnionPhysicalAlgorithms) {
  PlannedQuery query = Plan("RETURN 1 AS value UNION RETURN 2 AS value");
  const ir::LogicalPlan *logical_union =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kUnion);
  ASSERT_NE(logical_union, nullptr);

  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
  const rg::PhysicalPlanNode &union_node = physical.NodeFor(*logical_union);
  EXPECT_EQ(union_node.kind, rg::PhysicalOperatorKind::kUnionDistinct);
  const auto &data = std::get<rg::UnionDistinctOp>(union_node.data);
  ASSERT_EQ(data.key_slots.size(), 1U);
  const rg::Slot &output_slot = union_node.output_slots->At("value");
  EXPECT_EQ(data.key_slots.front().offset, output_slot.offset);
  EXPECT_EQ(data.key_slots.front().kind, output_slot.kind);
  EXPECT_EQ(data.key_slots.front().type, output_slot.type);
  EXPECT_EQ(data.key_slots.front().nullable, output_slot.nullable);

  const std::string printed = rg::PhysicalPlanToString(physical);
  EXPECT_NE(printed.find("UnionDistinct"), std::string::npos);
  EXPECT_NE(printed.find("logical=Union"), std::string::npos);
}

TEST(PhysicalPlanTest, ExecutesDetachedUnionAlgorithmsAndMappings) {
  rg::InMemoryGraph graph;
  rg::PhysicalPlan all = DetachedPhysicalPlan(
      "RETURN 1 AS value UNION ALL RETURN 1.0 AS value "
      "UNION ALL RETURN null AS value");
  ASSERT_NE(FindPhysicalPlan(all.Root(), rg::PhysicalOperatorKind::kUnionAll),
            nullptr);
  const auto all_rows = PhysicalRows(all, graph, {"value"});
  ASSERT_EQ(all_rows.size(), 3U);
  ASSERT_EQ(all_rows[0].size(), 1U);
  ASSERT_EQ(all_rows[1].size(), 1U);
  ASSERT_EQ(all_rows[2].size(), 1U);
  EXPECT_TRUE(all_rows[0][0].IsInteger());
  EXPECT_TRUE(all_rows[1][0].IsDouble());
  EXPECT_TRUE(all_rows[2][0].IsNull());
  EXPECT_TRUE(all.Root().output_slots->At("value").nullable);

  rg::PhysicalPlan distinct = DetachedPhysicalPlan(
      "RETURN 1 AS value UNION RETURN 1.0 AS value "
      "UNION RETURN null AS value UNION RETURN null AS value");
  ASSERT_NE(FindPhysicalPlan(distinct.Root(),
                             rg::PhysicalOperatorKind::kUnionDistinct),
            nullptr);
  const auto distinct_rows = PhysicalRows(distinct, graph, {"value"});
  ASSERT_EQ(distinct_rows.size(), 2U);
  EXPECT_EQ(std::count_if(distinct_rows.begin(), distinct_rows.end(),
                          [](const auto &row) { return row[0].IsNull(); }),
            1);
  EXPECT_EQ(std::count_if(distinct_rows.begin(), distinct_rows.end(),
                          [](const auto &row) {
                            return rg::ValuesEqual(row[0], rg::Value(1));
                          }),
            1);

  const auto node = graph.CreateNode({});
  rg::PhysicalPlan merged_layout = DetachedPhysicalPlan(
      "MATCH (n) RETURN n AS value UNION ALL RETURN 1 AS value");
  const auto mapped_rows = PhysicalRows(merged_layout, graph, {"value"});
  ASSERT_EQ(mapped_rows.size(), 2U);
  EXPECT_EQ(mapped_rows[0][0], rg::Value(node));
  EXPECT_EQ(mapped_rows[1][0], rg::Value(1));
  EXPECT_EQ(merged_layout.Root().output_slots->At("value").kind,
            rg::SlotKind::kReference);
}

TEST(PhysicalPlanTest, UnionOperatorsHandleResourcesAndEarlyClose) {
  rg::InMemoryGraph graph;
  rg::PhysicalPlan all = DetachedPhysicalPlan(
      "RETURN 'left' AS value UNION ALL RETURN 'right' AS value");
  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(all, graph, nullptr, {}, {"value"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  rg::QueryExecutionOptions all_memory_options;
  all_memory_options.memory_limit_bytes = 1;
  cursor = rg::StartPhysicalPlan(all, graph, nullptr, {}, {"value"},
                                 all_memory_options);
  EXPECT_TRUE(cursor->Next(&row));
  EXPECT_TRUE(cursor->Next(&row));
  EXPECT_FALSE(cursor->Next(&row));
  EXPECT_EQ(cursor->PeakMemoryBytes(), 0U);

  rg::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<rg::QueryCancellationToken>();
  cursor = rg::StartPhysicalPlan(all, graph, nullptr, {}, {"value"},
                                 cancellation_options);
  cancellation_options.cancellation->Cancel();
  EXPECT_THROW((void)cursor->Next(&row), common::QueryCancelledError);

  rg::PhysicalPlan distinct = DetachedPhysicalPlan(
      "RETURN 'left' AS value UNION RETURN 'right' AS value");
  rg::QueryExecutionOptions distinct_memory_options;
  distinct_memory_options.memory_limit_bytes = 1;
  cursor = rg::StartPhysicalPlan(distinct, graph, nullptr, {}, {"value"},
                                 distinct_memory_options);
  EXPECT_THROW((void)cursor->Next(&row), common::MemoryLimitExceededError);
}

TEST(PhysicalPlanTest, BuildsTypedApplyAndOptionalApplyPayloads) {
  PlannedQuery query =
      Plan("MATCH (n) WITH DISTINCT n MATCH (n)-[r]->(m) RETURN n, r, m");
  const ir::LogicalPlan *logical_apply =
      FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kApply);
  ASSERT_NE(logical_apply, nullptr);
  rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
  const rg::PhysicalPlanNode &apply_node = physical.NodeFor(*logical_apply);
  EXPECT_EQ(apply_node.kind, rg::PhysicalOperatorKind::kApply);
  EXPECT_TRUE(std::holds_alternative<rg::ApplyOp>(apply_node.data));

  rg::PhysicalPlan optional = DetachedOptionalApply();
  EXPECT_EQ(optional.Root().kind, rg::PhysicalOperatorKind::kOptionalApply);
  EXPECT_TRUE(
      std::holds_alternative<rg::OptionalApplyOp>(optional.Root().data));
  EXPECT_TRUE(optional.Root().output_slots->At("r").nullable);
  EXPECT_TRUE(optional.Root().output_slots->At("m").nullable);
}

TEST(PhysicalPlanTest, BuildsOwnedExistenceApplyPayloads) {
  rg::PhysicalPlan semi = DetachedPhysicalPlan(
      "MATCH (n) WHERE EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } RETURN n");
  const rg::PhysicalPlanNode *semi_node =
      FindPhysicalPlan(semi.Root(), rg::PhysicalOperatorKind::kSemiApply);
  ASSERT_NE(semi_node, nullptr);
  EXPECT_TRUE(std::holds_alternative<rg::SemiApplyOp>(semi_node->data));

  rg::PhysicalPlan anti = DetachedPhysicalPlan(
      "MATCH (n) WHERE NOT EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } "
      "RETURN n");
  const rg::PhysicalPlanNode *anti_node =
      FindPhysicalPlan(anti.Root(), rg::PhysicalOperatorKind::kAntiSemiApply);
  ASSERT_NE(anti_node, nullptr);
  EXPECT_TRUE(std::holds_alternative<rg::AntiSemiApplyOp>(anti_node->data));

  rg::PhysicalPlan let = DetachedPhysicalPlan(
      "MATCH (n) RETURN EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } AS has");
  const rg::PhysicalPlanNode *let_node =
      FindPhysicalPlan(let.Root(), rg::PhysicalOperatorKind::kLetSemiApply);
  ASSERT_NE(let_node, nullptr);
  const auto &let_data = std::get<rg::LetSemiApplyOp>(let_node->data);
  EXPECT_EQ(let_data.value_slot.kind, rg::SlotKind::kReference);
  EXPECT_EQ(let_data.value_slot.type, ast::SemanticVariableType::kScalar);
  EXPECT_FALSE(let_data.value_slot.nullable);

  rg::PhysicalPlan select =
      DetachedPhysicalPlan("MATCH (n) WHERE n.active OR (n)-[:R]->() RETURN n");
  const rg::PhysicalPlanNode *select_node = FindPhysicalPlan(
      select.Root(), rg::PhysicalOperatorKind::kSelectOrSemiApply);
  ASSERT_NE(select_node, nullptr);
  const auto &select_data =
      std::get<rg::SelectOrSemiApplyOp>(select_node->data);
  EXPECT_NE(select_data.predicate.Expression(), nullptr);
  EXPECT_FALSE(select_data.anti);

  rg::PhysicalPlan select_anti = DetachedPhysicalPlan(
      "MATCH (n) WHERE n.active OR NOT (n)-[:R]->() RETURN n");
  const rg::PhysicalPlanNode *select_anti_node = FindPhysicalPlan(
      select_anti.Root(), rg::PhysicalOperatorKind::kSelectOrSemiApply);
  ASSERT_NE(select_anti_node, nullptr);
  EXPECT_TRUE(std::get<rg::SelectOrSemiApplyOp>(select_anti_node->data).anti);
}

TEST(PhysicalPlanTest, ExecutesDetachedExistenceApplyOperators) {
  rg::InMemoryGraph graph;
  const auto matched =
      graph.CreateNode({"Input"}, {{"active", rg::Value(false)}});
  const auto unmatched =
      graph.CreateNode({"Input"}, {{"active", rg::Value(false)}});
  const auto short_circuited =
      graph.CreateNode({"Input"}, {{"active", rg::Value(true)}});
  const auto first_target = graph.CreateNode({});
  const auto second_target = graph.CreateNode({});
  graph.CreateRelationship(matched, first_target, "R");
  graph.CreateRelationship(matched, second_target, "R");

  rg::PhysicalPlan semi = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } "
      "RETURN id(n) AS id");
  EXPECT_EQ(FirstColumnIds(semi, graph, "id"),
            (std::vector<std::int64_t>{matched->id}));

  rg::PhysicalPlan anti = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE NOT EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } "
      "RETURN id(n) AS id");
  std::vector<std::int64_t> expected_anti{unmatched->id, short_circuited->id};
  std::sort(expected_anti.begin(), expected_anti.end());
  EXPECT_EQ(FirstColumnIds(anti, graph, "id"), expected_anti);

  rg::PhysicalPlan let = DetachedPhysicalPlan(
      "MATCH (n:Input) RETURN id(n) AS id, "
      "EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } AS has");
  const auto let_rows = PhysicalRows(let, graph, {"id", "has"});
  ASSERT_EQ(let_rows.size(), 3U);
  for (const auto &result : let_rows) {
    ASSERT_EQ(result.size(), 2U);
    ASSERT_TRUE(result[0].IsInteger());
    ASSERT_TRUE(result[1].IsBool());
    EXPECT_EQ(result[1].AsBool(), result[0].AsInteger() == matched->id);
  }

  rg::PhysicalPlan select = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE n.active OR (n)-[:R]->() "
      "RETURN id(n) AS id");
  std::vector<std::int64_t> expected_select{matched->id, short_circuited->id};
  std::sort(expected_select.begin(), expected_select.end());
  EXPECT_EQ(FirstColumnIds(select, graph, "id"), expected_select);

  rg::PhysicalPlan select_anti = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE n.active OR NOT (n)-[:R]->() "
      "RETURN id(n) AS id");
  std::vector<std::int64_t> expected_select_anti{unmatched->id,
                                                 short_circuited->id};
  std::sort(expected_select_anti.begin(), expected_select_anti.end());
  EXPECT_EQ(FirstColumnIds(select_anti, graph, "id"), expected_select_anti);

  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(let, graph, nullptr, {}, {"id", "has"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(PhysicalPlanTest, BuildsOwnedRollUpApplyPayload) {
  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (n:Input) RETURN id(n) AS id, "
      "[(n)-[:R]->(m) | m.name] AS names");
  const rg::PhysicalPlanNode *roll_up =
      FindPhysicalPlan(physical.Root(), rg::PhysicalOperatorKind::kRollUpApply);
  ASSERT_NE(roll_up, nullptr);
  const auto &data = std::get<rg::RollUpApplyOp>(roll_up->data);
  EXPECT_EQ(data.collection_slot.kind, rg::SlotKind::kReference);
  EXPECT_EQ(data.collection_slot.type, ast::SemanticVariableType::kList);
  EXPECT_FALSE(data.collection_slot.nullable);
  EXPECT_EQ(data.value_slot.kind, rg::SlotKind::kReference);
  EXPECT_EQ(data.value_slot.type, ast::SemanticVariableType::kScalar);
  EXPECT_NE(rg::PhysicalPlanToString(physical).find("RollUpApply"),
            std::string::npos);
}

TEST(PhysicalPlanTest, ExecutesDetachedRollUpApplyAndHandlesResources) {
  rg::InMemoryGraph graph;
  const auto matched = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first =
      graph.CreateNode({}, {{"name", rg::Value("first target")}});
  const auto second =
      graph.CreateNode({}, {{"name", rg::Value("second target")}});
  graph.CreateRelationship(matched, first, "R");
  graph.CreateRelationship(matched, second, "R");

  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (n:Input) RETURN id(n) AS id, "
      "[(n)-[:R]->(m) | m.name] AS names");
  const auto rows = PhysicalRows(physical, graph, {"id", "names"});
  ASSERT_EQ(rows.size(), 2U);
  for (const auto &result : rows) {
    ASSERT_EQ(result.size(), 2U);
    ASSERT_TRUE(result[0].IsInteger());
    ASSERT_TRUE(result[1].IsList());
    const rg::Value::List &names = result[1].AsList();
    if (result[0].AsInteger() == matched->id) {
      ASSERT_EQ(names.size(), 2U);
      EXPECT_EQ(names[0], rg::Value("first target"));
      EXPECT_EQ(names[1], rg::Value("second target"));
    } else {
      EXPECT_EQ(result[0].AsInteger(), unmatched->id);
      EXPECT_TRUE(names.empty());
    }
  }

  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"id", "names"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  rg::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<rg::QueryCancellationToken>();
  cursor = rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"id", "names"},
                                 cancellation_options);
  cancellation_options.cancellation->Cancel();
  EXPECT_THROW((void)cursor->Next(&row), common::QueryCancelledError);

  rg::QueryExecutionOptions memory_options;
  memory_options.memory_limit_bytes = 1;
  cursor = rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"id", "names"},
                                 memory_options);
  EXPECT_THROW((void)cursor->Next(&row), common::MemoryLimitExceededError);
}

TEST(PhysicalPlanTest, BuildsOwnedStreamingWritePayloads) {
  rg::PhysicalPlan create =
      DetachedPhysicalPlan("CREATE (a:Made $properties) RETURN a");
  const rg::PhysicalPlanNode *create_node =
      FindPhysicalPlan(create.Root(), rg::PhysicalOperatorKind::kCreateNode);
  ASSERT_NE(create_node, nullptr);
  const auto &create_node_data = std::get<rg::CreateNodeOp>(create_node->data);
  EXPECT_EQ(create_node_data.node_slot.kind, rg::SlotKind::kNode);
  EXPECT_EQ(create_node_data.labels, std::vector<std::string>{"Made"});
  ASSERT_TRUE(create_node_data.properties.parameter.has_value());
  EXPECT_NE(create_node_data.properties.parameter->Expression(), nullptr);
  const rg::PhysicalPlanNode *barrier =
      FindPhysicalPlan(create.Root(), rg::PhysicalOperatorKind::kWriteBarrier);
  ASSERT_NE(barrier, nullptr);
  EXPECT_TRUE(std::holds_alternative<rg::WriteBarrierOp>(barrier->data));

  rg::PhysicalPlan create_relationship_plan = DetachedPhysicalPlan(
      "CREATE (a:Made), (b:Made), "
      "(a)-[r:LINK {weight: 2}]->(b) RETURN a, r, b");
  const rg::PhysicalPlanNode *create_relationship =
      FindPhysicalPlan(create_relationship_plan.Root(),
                       rg::PhysicalOperatorKind::kCreateRelationship);
  ASSERT_NE(create_relationship, nullptr);
  const auto &relationship_data =
      std::get<rg::CreateRelationshipOp>(create_relationship->data);
  EXPECT_EQ(relationship_data.relationship_slot.kind,
            rg::SlotKind::kRelationship);
  EXPECT_EQ(relationship_data.left_node_slot.kind, rg::SlotKind::kNode);
  EXPECT_EQ(relationship_data.right_node_slot.kind, rg::SlotKind::kNode);
  EXPECT_EQ(relationship_data.type, "LINK");
  ASSERT_EQ(relationship_data.properties.entries.size(), 1U);
  EXPECT_NE(relationship_data.properties.entries[0].value.Expression(),
            nullptr);

  rg::PhysicalPlan mutations = DetachedPhysicalPlan(
      "MATCH (n) SET n.changed = 'yes', n += {added: 2}, n:Added "
      "REMOVE n.old, n:Old RETURN n");
  const rg::PhysicalPlanNode *set_property = FindPhysicalPlan(
      mutations.Root(), rg::PhysicalOperatorKind::kSetProperty);
  ASSERT_NE(set_property, nullptr);
  const auto &set_property_data =
      std::get<rg::SetPropertyOp>(set_property->data);
  EXPECT_EQ(set_property_data.property_key, "changed");
  EXPECT_NE(set_property_data.entity.Expression(), nullptr);
  EXPECT_NE(set_property_data.value.Expression(), nullptr);

  const rg::PhysicalPlanNode *set_properties = FindPhysicalPlan(
      mutations.Root(), rg::PhysicalOperatorKind::kSetProperties);
  ASSERT_NE(set_properties, nullptr);
  EXPECT_TRUE(
      std::get<rg::SetPropertiesOp>(set_properties->data).include_existing);
  const rg::PhysicalPlanNode *set_labels =
      FindPhysicalPlan(mutations.Root(), rg::PhysicalOperatorKind::kSetLabels);
  ASSERT_NE(set_labels, nullptr);
  EXPECT_EQ(std::get<rg::SetLabelsOp>(set_labels->data).labels,
            std::vector<std::string>{"Added"});

  const rg::PhysicalPlanNode *remove_property = FindPhysicalPlan(
      mutations.Root(), rg::PhysicalOperatorKind::kRemoveProperty);
  ASSERT_NE(remove_property, nullptr);
  EXPECT_EQ(std::get<rg::RemovePropertyOp>(remove_property->data).property_key,
            "old");
  const rg::PhysicalPlanNode *remove_labels = FindPhysicalPlan(
      mutations.Root(), rg::PhysicalOperatorKind::kRemoveLabels);
  ASSERT_NE(remove_labels, nullptr);
  EXPECT_EQ(std::get<rg::RemoveLabelsOp>(remove_labels->data).labels,
            std::vector<std::string>{"Old"});

  rg::PhysicalPlan exact =
      DetachedPhysicalPlan("MATCH (n) SET n = {only: 1} RETURN n");
  const rg::PhysicalPlanNode *exact_set =
      FindPhysicalPlan(exact.Root(), rg::PhysicalOperatorKind::kSetProperties);
  ASSERT_NE(exact_set, nullptr);
  EXPECT_FALSE(std::get<rg::SetPropertiesOp>(exact_set->data).include_existing);
}

TEST(PhysicalPlanTest, ExecutesDetachedStreamingWriteOperators) {
  rg::InMemoryGraph graph;
  rg::PhysicalPlan create = DetachedPhysicalPlan(
      "CREATE (a:Made $properties), (b:Made), "
      "(a)-[r:LINK {weight: 2}]->(b) RETURN a, b, r");
  const auto create_rows = PhysicalWriteRows(
      create, &graph, {"a", "b", "r"},
      {{"properties", rg::Value(rg::Value::Map{{"name", rg::Value("left")}})}});
  ASSERT_EQ(create_rows.size(), 1U);
  ASSERT_EQ(create_rows[0].size(), 3U);
  ASSERT_TRUE(create_rows[0][0].IsNode());
  ASSERT_TRUE(create_rows[0][1].IsNode());
  ASSERT_TRUE(create_rows[0][2].IsRelationship());
  EXPECT_EQ(create_rows[0][0].AsNode().properties.at("name"),
            rg::Value("left"));
  EXPECT_EQ(create_rows[0][2].AsRelationship().type, "LINK");
  EXPECT_EQ(create_rows[0][2].AsRelationship().properties.at("weight"),
            rg::Value(2));

  const auto target =
      graph.CreateNode({"Target", "Old"},
                       {{"old", rg::Value("remove")}, {"keep", rg::Value(1)}});
  rg::PhysicalPlan mutations = DetachedPhysicalPlan(
      "MATCH (n:Target) SET n.changed = 'yes', n += {added: 2}, n:Added "
      "REMOVE n.old, n:Old RETURN n");
  ASSERT_EQ(PhysicalWriteRows(mutations, &graph, {"n"}).size(), 1U);
  EXPECT_EQ(target->properties.at("changed"), rg::Value("yes"));
  EXPECT_EQ(target->properties.at("added"), rg::Value(2));
  EXPECT_EQ(target->properties.at("keep"), rg::Value(1));
  EXPECT_FALSE(target->properties.contains("old"));
  EXPECT_NE(std::find(target->labels.begin(), target->labels.end(), "Added"),
            target->labels.end());
  EXPECT_EQ(std::find(target->labels.begin(), target->labels.end(), "Old"),
            target->labels.end());

  rg::PhysicalPlan exact =
      DetachedPhysicalPlan("MATCH (n:Target) SET n = {only: 9} RETURN n");
  ASSERT_EQ(PhysicalWriteRows(exact, &graph, {"n"}).size(), 1U);
  EXPECT_EQ(target->properties, (rg::Value::Map{{"only", rg::Value(9)}}));

  rg::PhysicalPlan relationship_mutation = DetachedPhysicalPlan(
      "MATCH ()-[r:LINK]->() SET r.flag = true REMOVE r.weight RETURN r");
  ASSERT_EQ(PhysicalWriteRows(relationship_mutation, &graph, {"r"}).size(), 1U);
  const rg::Relationship &relationship = create_rows[0][2].AsRelationship();
  EXPECT_EQ(graph.RelationshipById(relationship.id)->properties.at("flag"),
            rg::Value(true));
  EXPECT_FALSE(
      graph.RelationshipById(relationship.id)->properties.contains("weight"));
}

TEST(PhysicalPlanTest, BuildsOwnedDeletePayloads) {
  rg::PhysicalPlan delete_plan =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN n");
  const rg::PhysicalPlanNode *delete_node =
      FindPhysicalPlan(delete_plan.Root(), rg::PhysicalOperatorKind::kDelete);
  ASSERT_NE(delete_node, nullptr);
  const auto &delete_data = std::get<rg::DeleteOp>(delete_node->data);
  ASSERT_EQ(delete_data.expressions.size(), 1U);
  EXPECT_NE(delete_data.expressions.front().Expression(), nullptr);

  rg::PhysicalPlan detach_plan =
      DetachedPhysicalPlan("MATCH (n:N) DETACH DELETE n RETURN n");
  const rg::PhysicalPlanNode *detach_node = FindPhysicalPlan(
      detach_plan.Root(), rg::PhysicalOperatorKind::kDetachDelete);
  ASSERT_NE(detach_node, nullptr);
  const auto &detach_data = std::get<rg::DetachDeleteOp>(detach_node->data);
  ASSERT_EQ(detach_data.expressions.size(), 1U);
  EXPECT_NE(detach_data.expressions.front().Expression(), nullptr);
}

TEST(PhysicalPlanTest, ExecutesDetachedDeleteOperators) {
  rg::InMemoryGraph graph;
  const auto isolated = graph.CreateNode({"N"});
  rg::PhysicalPlan delete_plan =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN n");
  const auto deleted_rows = PhysicalWriteRows(delete_plan, &graph, {"n"});
  ASSERT_EQ(deleted_rows.size(), 1U);
  ASSERT_EQ(deleted_rows.front().size(), 1U);
  EXPECT_EQ(deleted_rows.front().front().AsNode().id, isolated->id);
  EXPECT_TRUE(graph.Nodes().empty());

  const auto left = graph.CreateNode({"N"});
  const auto right = graph.CreateNode({"N"});
  graph.CreateRelationship(left, right, "R");
  rg::PhysicalPlan relationship_delete =
      DetachedPhysicalPlan("MATCH ()-[r:R]->() DELETE r RETURN r");
  const auto relationship_rows =
      PhysicalWriteRows(relationship_delete, &graph, {"r"});
  ASSERT_EQ(relationship_rows.size(), 1U);
  EXPECT_TRUE(graph.Relationships().empty());
  EXPECT_EQ(graph.Nodes().size(), 2U);

  const auto connected_left = graph.CreateNode({"N"});
  const auto connected_right = graph.CreateNode({"N"});
  graph.CreateRelationship(connected_left, connected_right, "R");
  rg::PhysicalPlan invalid_delete =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN n");
  EXPECT_THROW((void)PhysicalWriteRows(invalid_delete, &graph, {"n"}),
               common::InvalidArgumentError);
  EXPECT_EQ(graph.Nodes().size(), 4U);
  EXPECT_EQ(graph.Relationships().size(), 1U);

  rg::PhysicalPlan detach_plan =
      DetachedPhysicalPlan("MATCH (n:N) DETACH DELETE n RETURN n");
  const auto detach_rows = PhysicalWriteRows(detach_plan, &graph, {"n"});
  EXPECT_EQ(detach_rows.size(), 4U);
  EXPECT_TRUE(graph.Nodes().empty());
  EXPECT_TRUE(graph.Relationships().empty());
}

TEST(PhysicalPlanTest, DeleteOperatorHandlesCloseCancellationAndMemoryLimit) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"});
  rg::PhysicalPlan physical =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN n");
  std::vector<rg::Value> row;

  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(physical, graph, &graph, {}, {"n"});
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));
  EXPECT_EQ(graph.Nodes().size(), 1U);

  rg::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<rg::QueryCancellationToken>();
  cursor = rg::StartPhysicalPlan(physical, graph, &graph, {}, {"n"},
                                 cancellation_options);
  cancellation_options.cancellation->Cancel();
  EXPECT_THROW((void)cursor->Next(&row), common::QueryCancelledError);
  EXPECT_EQ(graph.Nodes().size(), 1U);

  rg::QueryExecutionOptions memory_options;
  memory_options.memory_limit_bytes = 1;
  cursor = rg::StartPhysicalPlan(physical, graph, &graph, {}, {"n"},
                                 memory_options);
  EXPECT_THROW((void)cursor->Next(&row), common::MemoryLimitExceededError);
  EXPECT_EQ(graph.Nodes().size(), 1U);
}

TEST(PhysicalPlanTest, ReinstantiatesStatefulApplyRightSideForEachLeftRow) {
  rg::InMemoryGraph graph;
  const auto first = graph.CreateNode({"Input"});
  const auto second = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first_target = graph.CreateNode({});
  const auto second_target = graph.CreateNode({});
  graph.CreateRelationship(first, first_target, "R");
  graph.CreateRelationship(first, second_target, "R");
  graph.CreateRelationship(second, first_target, "R");
  graph.CreateRelationship(second, second_target, "R");

  rg::PhysicalPlan physical = DetachedLimitedApply();
  ASSERT_EQ(physical.Root().kind, rg::PhysicalOperatorKind::kApply);
  ASSERT_NE(FindPhysicalPlan(physical.Root(), rg::PhysicalOperatorKind::kLimit),
            nullptr);
  const auto rows = PhysicalRows(physical, graph, {"n", "m"});
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(),
                          [&](const auto &row) {
                            return row[0] == rg::Value(first) &&
                                   row[1].IsNode();
                          }),
            1);
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(),
                          [&](const auto &row) {
                            return row[0] == rg::Value(second) &&
                                   row[1].IsNode();
                          }),
            1);
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(),
                          [&](const auto &row) {
                            return row[0] == rg::Value(unmatched);
                          }),
            0);

  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"n", "m"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  rg::QueryExecutionOptions options;
  options.cancellation = std::make_shared<rg::QueryCancellationToken>();
  cursor =
      rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"n", "m"}, options);
  options.cancellation->Cancel();
  EXPECT_THROW((void)cursor->Next(&row), common::QueryCancelledError);
}

TEST(PhysicalPlanTest, OptionalApplyNullExtendsAfterLogicalPlanIsDestroyed) {
  rg::InMemoryGraph graph;
  const auto matched = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first = graph.CreateNode({});
  const auto second = graph.CreateNode({});
  graph.CreateRelationship(matched, first, "R");
  graph.CreateRelationship(matched, second, "R");

  rg::PhysicalPlan physical = DetachedOptionalApply();
  const auto rows = PhysicalRows(physical, graph, {"n", "r", "m"});
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(),
                          [&](const auto &row) {
                            return row[0] == rg::Value(matched) &&
                                   row[1].IsRelationship() && row[2].IsNode();
                          }),
            2);
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(),
                          [&](const auto &row) {
                            return row[0] == rg::Value(unmatched) &&
                                   row[1].IsNull() && row[2].IsNull();
                          }),
            1);
}

TEST(PhysicalPlanTest, ApplyRejectsConflictingSharedSlots) {
  rg::InMemoryGraph graph;
  rg::PhysicalPlan physical = DetachedConflictingApply();

  EXPECT_EQ(PhysicalRows(physical, graph, {"shared", "left", "right"},
                         {{"left_shared", rg::Value(1)},
                          {"left_value", rg::Value("left")},
                          {"right_shared", rg::Value(1)},
                          {"right_value", rg::Value("right")}}),
            (std::vector<std::vector<rg::Value>>{
                {rg::Value(1), rg::Value("left"), rg::Value("right")}}));
  EXPECT_TRUE(PhysicalRows(physical, graph, {"shared", "left", "right"},
                           {{"left_shared", rg::Value(1)},
                            {"left_value", rg::Value("left")},
                            {"right_shared", rg::Value(2)},
                            {"right_value", rg::Value("right")}})
                  .empty());
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

TEST(PhysicalPlanTest, BuildsTypedOwnedPayloadsForFixedTraversalOperators) {
  {
    PlannedQuery query =
        Plan("MATCH (a)-[r:R]->(b) RETURN id(a) AS a, id(r) AS r, id(b) AS b");
    const auto *logical = static_cast<const ir::ExpandPlan *>(
        FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kExpand));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data = std::get<rg::ExpandOp>(physical.NodeFor(*logical).data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.relationship, "r");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction, rg::PhysicalExpandDirection::kOutgoing);
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
  }
  {
    PlannedQuery query =
        Plan("MATCH (a) WITH a, a AS b MATCH (a)-[r:R]->(b) RETURN id(r) AS r");
    const auto *logical = static_cast<const ir::ExpandIntoPlan *>(
        FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kExpandInto));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::ExpandIntoOp>(physical.NodeFor(*logical).data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction, rg::PhysicalExpandDirection::kOutgoing);
  }
  {
    PlannedQuery query = Plan(
        "MATCH (a) OPTIONAL MATCH (a)-[r:R]->(b) WHERE r.value = 10 "
        "RETURN id(a) AS a, id(r) AS r, id(b) AS b");
    const auto *logical = static_cast<const ir::OptionalExpandPlan *>(FindPlan(
        *query.logical_plan, ir::LogicalPlanNodeType::kOptionalExpand));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::OptionalExpandOp>(physical.NodeFor(*logical).data);
    ASSERT_EQ(data.predicates.size(), logical->Predicates().size());
    EXPECT_NE(data.predicates.front().Expression(),
              logical->Predicates().front());
  }
  {
    ir::PatternRelationship pattern{.variable = "rs",
                                    .left_node = "a",
                                    .right_node = "b",
                                    .direction = ir::Direction::kBoth,
                                    .types = {"R"}};
    pattern.length.variable = true;
    pattern.length.min = 2;
    pattern.length.max = 3;
    ir::ProjectEndpointsPlan logical(
        std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"rs"}),
        std::move(pattern));
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(logical);
    const auto &data = std::get<rg::ProjectEndpointsOp>(physical.Root().data);
    EXPECT_EQ(data.pattern.relationship, "rs");
    EXPECT_EQ(data.pattern.direction, rg::PhysicalExpandDirection::kBoth);
    EXPECT_TRUE(data.length.variable);
    EXPECT_EQ(data.length.min, 2);
    EXPECT_EQ(data.length.max, 3);
  }
}

TEST(PhysicalPlanTest, BuildsTypedOwnedPayloadsForVariableTraversalOperators) {
  {
    PlannedQuery query =
        Plan("MATCH (a)-[rs:R*0..3]->(b) RETURN size(rs) AS hops");
    const auto *logical = static_cast<const ir::VarExpandPlan *>(
        FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kVarExpand));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::VarExpandOp>(physical.NodeFor(*logical).data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.relationship, "rs");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction, rg::PhysicalExpandDirection::kOutgoing);
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
    EXPECT_TRUE(data.length.variable);
    EXPECT_EQ(data.length.min, 0);
    EXPECT_EQ(data.length.max, 3);
  }
  {
    rg::PhysicalPlan physical = DetachedPruningVarExpand();
    ASSERT_EQ(physical.Root().kind,
              rg::PhysicalOperatorKind::kPruningVarExpand);
    const auto &data = std::get<rg::PruningVarExpandOp>(physical.Root().data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction, rg::PhysicalExpandDirection::kOutgoing);
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
    EXPECT_TRUE(data.length.variable);
    EXPECT_EQ(data.length.min, 0);
    EXPECT_EQ(data.length.max, 2);
  }
  {
    PlannedQuery query = Plan("MATCH p = (a)-[rs:R*1..2]->(b) RETURN p");
    const auto *logical = static_cast<const ir::PathBuildPlan *>(
        FindPlan(*query.logical_plan, ir::LogicalPlanNodeType::kPathBuild));
    ASSERT_NE(logical, nullptr);
    rg::PhysicalPlan physical = rg::CreatePhysicalPlan(*query.logical_plan);
    const auto &data =
        std::get<rg::PathBuildOp>(physical.NodeFor(*logical).data);
    EXPECT_EQ(data.path.variable, "p");
    EXPECT_EQ(data.path.nodes, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(data.path.relationships, (std::vector<std::string>{"rs"}));
  }
}

TEST(PhysicalPlanTest,
     ExecutesFixedTraversalAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});
  auto third = graph.CreateNode({});
  auto first_relationship =
      graph.CreateRelationship(first, second, "R", {{"value", rg::Value(10)}});
  auto second_relationship =
      graph.CreateRelationship(second, third, "R", {{"value", rg::Value(20)}});

  EXPECT_EQ(FirstColumnIds(
                DetachedPhysicalPlan("MATCH (a)-[r:R]->(b) RETURN id(r) AS r"),
                graph, "r"),
            (std::vector<std::int64_t>{first_relationship->id,
                                       second_relationship->id}));

  const auto optional_rows = PhysicalRows(
      DetachedPhysicalPlan(
          "MATCH (a) OPTIONAL MATCH (a)-[r:R]->(b) WHERE r.value = 10 "
          "RETURN id(a) AS a, id(r) AS r, id(b) AS b"),
      graph, {"a", "r", "b"});
  ASSERT_EQ(optional_rows.size(), 3U);
  EXPECT_EQ(std::count_if(optional_rows.begin(), optional_rows.end(),
                          [](const auto &row) { return row[1].IsNull(); }),
            2);
  const auto matched =
      std::find_if(optional_rows.begin(), optional_rows.end(),
                   [](const auto &row) { return !row[1].IsNull(); });
  ASSERT_NE(matched, optional_rows.end());
  EXPECT_EQ((*matched)[0], rg::Value(first->id));
  EXPECT_EQ((*matched)[1], rg::Value(first_relationship->id));
  EXPECT_EQ((*matched)[2], rg::Value(second->id));

  rg::PhysicalPlan expand_into = DetachedExpandInto();
  EXPECT_EQ(
      FirstColumnIds(expand_into, graph, "r",
                     {{"from", rg::Value(first)}, {"to", rg::Value(second)}}),
      (std::vector<std::int64_t>{first_relationship->id}));
  EXPECT_TRUE(
      FirstColumnIds(expand_into, graph, "r",
                     {{"from", rg::Value(first)}, {"to", rg::Value(third)}})
          .empty());

  auto self = graph.CreateRelationship(first, first, "R");
  EXPECT_EQ(
      FirstColumnIds(expand_into, graph, "r",
                     {{"from", rg::Value(first)}, {"to", rg::Value(first)}}),
      (std::vector<std::int64_t>{self->id}));

  rg::PhysicalPlan project_fixed = DetachedProjectEndpoints(false);
  const rg::QueryParameters fixed_parameters{
      {"relationship", rg::Value(first_relationship)}};
  EXPECT_EQ(FirstColumnIds(project_fixed, graph, "a", fixed_parameters),
            (std::vector<std::int64_t>{first->id}));
  EXPECT_EQ(FirstColumnIds(project_fixed, graph, "b", fixed_parameters),
            (std::vector<std::int64_t>{second->id}));

  rg::PhysicalPlan project_variable = DetachedProjectEndpoints(true);
  const rg::QueryParameters variable_parameters{
      {"relationship",
       rg::Value(rg::Value::List{rg::Value(first_relationship),
                                 rg::Value(second_relationship)})}};
  EXPECT_EQ(FirstColumnIds(project_variable, graph, "a", variable_parameters),
            (std::vector<std::int64_t>{first->id}));
  EXPECT_EQ(FirstColumnIds(project_variable, graph, "b", variable_parameters),
            (std::vector<std::int64_t>{third->id}));

  EXPECT_EQ(FirstColumnIds(
                DetachedPhysicalPlan("MATCH (a)-[r:R]-(b) RETURN id(r) AS r"),
                graph, "r"),
            (std::vector<std::int64_t>{
                first_relationship->id, first_relationship->id,
                second_relationship->id, second_relationship->id, self->id}));
}

TEST(PhysicalPlanTest, ClosesFixedExpandCursorEarly) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});
  graph.CreateRelationship(first, second, "R");
  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a)-[r:R]->(b) RETURN id(r) AS relationship_id");
  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"relationship_id"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(PhysicalPlanTest,
     ExecutesVariableTraversalAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});
  auto third = graph.CreateNode({});
  auto first_relationship = graph.CreateRelationship(first, second, "R");
  auto second_relationship = graph.CreateRelationship(second, third, "R");
  graph.CreateRelationship(first, third, "S");

  rg::PhysicalPlan variable = DetachedPhysicalPlan(
      "MATCH (a)-[rs:R*0..3]->(b) WHERE id(a) = " + std::to_string(first->id) +
      " RETURN size(rs) AS hops");
  EXPECT_EQ(FirstColumnIds(variable, graph, "hops"),
            (std::vector<std::int64_t>{0, 1, 2}));

  rg::PhysicalPlan pruning = DetachedPruningVarExpand();
  EXPECT_EQ(FirstColumnIds(pruning, graph, "b", {{"from", rg::Value(first)}}),
            (std::vector<std::int64_t>{first->id, second->id, third->id}));

  rg::PhysicalPlan path =
      DetachedPhysicalPlan("MATCH p = (a)-[rs:R*2..2]->(b) WHERE id(b) = " +
                           std::to_string(third->id) + " RETURN p");
  const auto rows = PhysicalRows(path, graph, {"p"});
  ASSERT_EQ(rows.size(), 1U);
  ASSERT_EQ(rows.front().size(), 1U);
  ASSERT_TRUE(rows.front().front().IsPath());
  const rg::Path &value = rows.front().front().AsPath();
  ASSERT_EQ(value.nodes.size(), 3U);
  ASSERT_EQ(value.relationships.size(), 2U);
  EXPECT_EQ(value.nodes[0]->id, first->id);
  EXPECT_EQ(value.nodes[1]->id, second->id);
  EXPECT_EQ(value.nodes[2]->id, third->id);
  EXPECT_EQ(value.relationships[0]->id, first_relationship->id);
  EXPECT_EQ(value.relationships[1]->id, second_relationship->id);
}

TEST(PhysicalPlanTest, ClosesVariableExpandCursorEarlyAndEnforcesMemoryLimit) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});
  graph.CreateRelationship(first, second, "R");
  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a)-[rs:R*1..2]->(b) WHERE id(a) = " + std::to_string(first->id) +
      " RETURN id(b) AS b");

  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"b"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  rg::QueryExecutionOptions options;
  options.memory_limit_bytes = 1;
  cursor = rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"b"}, options);
  EXPECT_THROW((void)cursor->Next(&row), common::MemoryLimitExceededError);
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
  const rg::PhysicalPlanNode &join_node = physical.NodeFor(*logical_join);
  const auto &data = std::get<rg::ValueHashJoinOp>(join_node.data);
  EXPECT_EQ(data.build_child, 0U);
  const auto &logical =
      static_cast<const ir::ValueHashJoinPlan &>(*logical_join);
  ASSERT_EQ(data.keys.size(), logical.JoinKeys().size());
  ASSERT_EQ(data.predicates.size(), logical.Predicates().size());
  EXPECT_NE(data.keys.front().left.Expression(),
            logical.JoinKeys().front().left);
  EXPECT_NE(data.keys.front().right.Expression(),
            logical.JoinKeys().front().right);
  EXPECT_NE(data.predicates.front().Expression(), logical.Predicates().front());
}

TEST(PhysicalPlanTest, BuildsTypedNestedLoopBinaryPayloads) {
  PlannedQuery product_query = Plan("MATCH (a), (b) RETURN a, b");
  const ir::LogicalPlan *logical_product = FindPlan(
      *product_query.logical_plan, ir::LogicalPlanNodeType::kCartesianProduct);
  ASSERT_NE(logical_product, nullptr);
  rg::PhysicalPlan product =
      rg::CreatePhysicalPlan(*product_query.logical_plan);
  const auto &product_node = product.NodeFor(*logical_product);
  const auto &product_data =
      std::get<rg::CartesianProductOp>(product_node.data);
  EXPECT_EQ(product_data.cached_child, 1U);
  EXPECT_NE(rg::PhysicalPlanToString(product).find("cache=right"),
            std::string::npos);

  PlannedQuery predicate_query =
      Plan("MATCH (a), (b) WHERE a.age > b.age RETURN a, b");
  const ir::LogicalPlan *logical_predicate = FindPlan(
      *predicate_query.logical_plan, ir::LogicalPlanNodeType::kPredicateJoin);
  ASSERT_NE(logical_predicate, nullptr);
  rg::PhysicalPlan predicate =
      rg::CreatePhysicalPlan(*predicate_query.logical_plan);
  const auto &predicate_node = predicate.NodeFor(*logical_predicate);
  const auto &predicate_data =
      std::get<rg::PredicateJoinOp>(predicate_node.data);
  ASSERT_EQ(predicate_data.predicates.size(), 1U);
  EXPECT_EQ(predicate_data.cached_child, 1U);
  EXPECT_NE(predicate_data.predicates.front().Expression(),
            static_cast<const ir::PredicateJoinPlan &>(*logical_predicate)
                .Predicates()
                .front());
  EXPECT_NE(rg::PhysicalPlanToString(predicate).find("cache=right"),
            std::string::npos);
}

TEST(PhysicalPlanTest, StreamsCartesianProductAfterLogicalPlanIsDestroyed) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"Left"});
  graph.CreateNode({"Left"});
  graph.CreateNode({"Right"});
  graph.CreateNode({"Right"});
  graph.CreateNode({"Right"});
  rg::PhysicalPlan physical = DetachedCartesianProduct();
  ASSERT_EQ(physical.Root().kind, rg::PhysicalOperatorKind::kCartesianProduct);

  const auto rows = PhysicalRows(physical, graph, {"a", "b"});
  ASSERT_EQ(rows.size(), 6U);
  std::set<std::string> pairs;
  for (const auto &row : rows) {
    ASSERT_EQ(row.size(), 2U);
    ASSERT_TRUE(row[0].IsNode());
    ASSERT_TRUE(row[1].IsNode());
    pairs.insert(std::to_string(row[0].AsNode().id) + ":" +
                 std::to_string(row[1].AsNode().id));
  }
  EXPECT_EQ(pairs.size(), 6U);

  std::unique_ptr<rg::PhysicalResultCursor> cursor =
      rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"a", "b"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  rg::QueryExecutionOptions memory_options;
  memory_options.memory_limit_bytes = 1;
  cursor = rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"a", "b"},
                                 memory_options);
  EXPECT_THROW((void)cursor->Next(&row), common::MemoryLimitExceededError);

  rg::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<rg::QueryCancellationToken>();
  cursor = rg::StartPhysicalPlan(physical, graph, nullptr, {}, {"a", "b"},
                                 cancellation_options);
  cancellation_options.cancellation->Cancel();
  EXPECT_THROW((void)cursor->Next(&row), common::QueryCancelledError);

  rg::InMemoryGraph empty_right;
  empty_right.CreateNode({"Left"});
  EXPECT_TRUE(PhysicalRows(physical, empty_right, {"a", "b"}).empty());
}

TEST(PhysicalPlanTest,
     ExecutesPredicateJoinAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  graph.CreateNode({}, {{"age", rg::Value(3)}, {"name", rg::Value("old")}});
  graph.CreateNode({}, {{"age", rg::Value(2)}, {"name", rg::Value("middle")}});
  graph.CreateNode({}, {{"age", rg::Value(1)}, {"name", rg::Value("young")}});
  graph.CreateNode({}, {{"name", rg::Value("unknown")}});
  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a), (b) WHERE a.age > b.age "
      "RETURN a.name AS older, b.name AS younger");
  ASSERT_NE(FindPhysicalPlan(physical.Root(),
                             rg::PhysicalOperatorKind::kPredicateJoin),
            nullptr);

  const auto rows = PhysicalRows(physical, graph, {"older", "younger"});
  ASSERT_EQ(rows.size(), 3U);
  std::set<std::string> pairs;
  for (const auto &row : rows) {
    ASSERT_EQ(row.size(), 2U);
    ASSERT_TRUE(row[0].IsString());
    ASSERT_TRUE(row[1].IsString());
    pairs.insert(row[0].AsString() + ":" + row[1].AsString());
  }
  EXPECT_EQ(pairs,
            (std::set<std::string>{"middle:young", "old:middle", "old:young"}));
}

TEST(PhysicalPlanTest, CartesianProductRejectsConflictingSharedSlots) {
  rg::InMemoryGraph graph;
  rg::PhysicalPlan physical = DetachedParameterCartesianProduct();

  EXPECT_EQ(PhysicalRows(physical, graph, {"shared", "left", "right"},
                         {{"left_shared", rg::Value(1)},
                          {"left_value", rg::Value("left")},
                          {"right_shared", rg::Value(1)},
                          {"right_value", rg::Value("right")}}),
            (std::vector<std::vector<rg::Value>>{
                {rg::Value(1), rg::Value("left"), rg::Value("right")}}));
  EXPECT_TRUE(PhysicalRows(physical, graph, {"shared", "left", "right"},
                           {{"left_shared", rg::Value(1)},
                            {"left_value", rg::Value("left")},
                            {"right_shared", rg::Value(2)},
                            {"right_value", rg::Value("right")}})
                  .empty());
}

TEST(PhysicalPlanTest, BuildsTypedNodeAndLeftOuterHashJoinPayloads) {
  auto left = std::make_unique<ir::AllNodeScanPlan>("n");
  auto right = std::make_unique<ir::AllNodeScanPlan>("n");
  left->SetCostEstimate(1.0, 1.0);
  right->SetCostEstimate(2.0, 2.0);
  ir::NodeHashJoinPlan logical(std::move(left), std::move(right), {"n"});

  rg::PhysicalPlan node_join = rg::CreatePhysicalPlan(logical);
  const auto &node_data = std::get<rg::NodeHashJoinOp>(node_join.Root().data);
  EXPECT_EQ(node_data.join_keys, (std::vector<std::string>{"n"}));
  EXPECT_EQ(node_data.build_child, 0U);
  EXPECT_NE(rg::PhysicalPlanToString(node_join).find("build=left"),
            std::string::npos);

  rg::PhysicalPlan outer_join = DetachedLeftOuterHashJoin();
  const auto &outer_data =
      std::get<rg::LeftOuterHashJoinOp>(outer_join.Root().data);
  EXPECT_EQ(outer_data.join_keys, (std::vector<std::string>{"a"}));
}

TEST(PhysicalPlanTest, ExecutesNodeHashJoinAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  const auto first =
      graph.CreateNode({"Person"}, {{"name", rg::Value("first")}});
  const auto second =
      graph.CreateNode({"Person"}, {{"name", rg::Value("second")}});
  const auto third =
      graph.CreateNode({"Person"}, {{"name", rg::Value("third")}});
  const auto center = graph.CreateNode({});
  graph.CreateRelationship(first, center, "R");
  graph.CreateRelationship(second, center, "R");
  graph.CreateRelationship(third, center, "R");

  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a:Person)-[r1]->(b)<-[r2]-(c:Person) "
      "RETURN a.name AS left, c.name AS right");
  ASSERT_NE(FindPhysicalPlan(physical.Root(),
                             rg::PhysicalOperatorKind::kNodeHashJoin),
            nullptr);
  const auto rows = PhysicalRows(physical, graph, {"left", "right"});
  ASSERT_EQ(rows.size(), 6U);
  std::set<std::string> pairs;
  for (const auto &row : rows) {
    ASSERT_EQ(row.size(), 2U);
    ASSERT_TRUE(row[0].IsString());
    ASSERT_TRUE(row[1].IsString());
    EXPECT_NE(row[0], row[1]);
    pairs.insert(row[0].AsString() + ":" + row[1].AsString());
  }
  EXPECT_EQ(pairs.size(), 6U);
}

TEST(PhysicalPlanTest, NodeHashJoinRejectsNullKeysAndConflictingSharedSlots) {
  rg::InMemoryGraph graph;
  const auto node = graph.CreateNode({});
  rg::PhysicalPlan physical = DetachedParameterNodeHashJoin();
  ASSERT_EQ(physical.Root().kind, rg::PhysicalOperatorKind::kNodeHashJoin);

  EXPECT_EQ(
      PhysicalRows(physical, graph, {"n", "value"},
                   {{"left_node", rg::Value(node)},
                    {"left_value", rg::Value(1)},
                    {"right_node", rg::Value(node)},
                    {"right_value", rg::Value(1)}}),
      (std::vector<std::vector<rg::Value>>{{rg::Value(node), rg::Value(1)}}));
  EXPECT_TRUE(PhysicalRows(physical, graph, {"n", "value"},
                           {{"left_node", rg::Value(node)},
                            {"left_value", rg::Value(1)},
                            {"right_node", rg::Value(node)},
                            {"right_value", rg::Value(2)}})
                  .empty());
  EXPECT_TRUE(PhysicalRows(physical, graph, {"n", "value"},
                           {{"left_node", rg::Value::Null()},
                            {"left_value", rg::Value(1)},
                            {"right_node", rg::Value::Null()},
                            {"right_value", rg::Value(1)}})
                  .empty());
}

TEST(PhysicalPlanTest, ExecutesLeftOuterHashJoinAfterLogicalPlanIsDestroyed) {
  rg::InMemoryGraph graph;
  const auto matched = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first = graph.CreateNode({});
  const auto second = graph.CreateNode({});
  graph.CreateRelationship(matched, first, "R");
  graph.CreateRelationship(matched, second, "R");

  rg::PhysicalPlan physical = DetachedLeftOuterHashJoin();
  ASSERT_EQ(physical.Root().kind, rg::PhysicalOperatorKind::kLeftOuterHashJoin);
  const auto rows = PhysicalRows(physical, graph, {"a", "b"});
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(),
                          [&](const auto &row) {
                            return row[0] == rg::Value(unmatched) &&
                                   row[1].IsNull();
                          }),
            1);
  EXPECT_EQ(std::count_if(rows.begin(), rows.end(),
                          [&](const auto &row) {
                            return row[0] == rg::Value(matched) &&
                                   row[1].IsNode();
                          }),
            2);
}

TEST(PhysicalPlanTest,
     ExecutesValueHashJoinAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"Small"}, {{"first", rg::Value(1)},
                               {"second", rg::Value("x")},
                               {"name", rg::Value("match")}});
  graph.CreateNode({"Small"}, {{"second", rg::Value("x")},
                               {"name", rg::Value("null-left")}});
  graph.CreateNode({"Large"}, {{"first", rg::Value(1.0)},
                               {"second", rg::Value("x")},
                               {"name", rg::Value("first")}});
  graph.CreateNode({"Large"}, {{"first", rg::Value(1)},
                               {"second", rg::Value("y")},
                               {"name", rg::Value("wrong")}});
  graph.CreateNode({"Large"}, {{"second", rg::Value("x")},
                               {"name", rg::Value("null-right")}});

  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a:Small), (b:Large) "
      "WHERE a.first = b.first AND a.second = b.second "
      "RETURN a.name AS left, b.name AS right");
  ASSERT_NE(FindPhysicalPlan(physical.Root(),
                             rg::PhysicalOperatorKind::kValueHashJoin),
            nullptr);
  EXPECT_EQ(PhysicalRows(physical, graph, {"left", "right"}),
            (std::vector<std::vector<rg::Value>>{
                {rg::Value("match"), rg::Value("first")}}));
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
  const auto &data = std::get<rg::TopNOp>(top_n_node.data);
  ASSERT_EQ(data.items.size(), 1U);
  EXPECT_NE(
      data.items.front().expression.Expression(),
      static_cast<const ir::TopNPlan &>(*top_n).Items().front().expression);
  EXPECT_EQ(data.items.front().direction,
            rg::PhysicalSortDirection::kAscending);
  EXPECT_NE(data.limit.Expression(),
            static_cast<const ir::TopNPlan &>(*top_n).Limit());
  ASSERT_EQ(top_n_node.provided_order.size(), 1U);
  EXPECT_NE(
      top_n_node.provided_order.front().expression.get(),
      static_cast<const ir::TopNPlan &>(*top_n).Items().front().expression);
  EXPECT_EQ(top_n_node.provided_order.front().direction,
            rg::PhysicalSortDirection::kAscending);
  ASSERT_EQ(top_n_node.children.size(), 1U);
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
  const auto &distinct_data =
      std::get<rg::OrderedDistinctOp>(distinct_node.data);
  ASSERT_EQ(distinct_data.grouping_items.size(), 1U);
  EXPECT_EQ(distinct_data.grouping_items.front().alias, "x");
  EXPECT_NE(distinct_data.grouping_items.front().expression.Expression(),
            static_cast<const ir::DistinctPlan &>(*distinct)
                .GroupingItems()
                .front()
                .expression);

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
  const auto &aggregation_data =
      std::get<rg::OrderedAggregationOp>(aggregation_node.data);
  ASSERT_EQ(aggregation_data.grouping_items.size(), 1U);
  ASSERT_EQ(aggregation_data.aggregation_items.size(), 1U);
  EXPECT_EQ(aggregation_data.grouping_items.front().alias, "x");
  EXPECT_EQ(aggregation_data.aggregation_items.front().alias, "count");
  const auto &logical_aggregation =
      static_cast<const ir::AggregationPlan &>(*aggregation);
  EXPECT_NE(aggregation_data.grouping_items.front().expression.Expression(),
            logical_aggregation.GroupingItems().front().expression);
  EXPECT_NE(aggregation_data.aggregation_items.front().expression.Expression(),
            logical_aggregation.AggregationItems().front().expression);
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
  const auto &partial_data = std::get<rg::PartialSortOp>(partial_node.data);
  EXPECT_EQ(partial_data.prefix, 1U);
  ASSERT_EQ(partial_data.items.size(), 2U);
  EXPECT_NE(partial_data.items.front().expression.Expression(),
            static_cast<const ir::SortPlan &>(*outer_sort)
                .Items()
                .front()
                .expression);
  ASSERT_EQ(partial_node.provided_order.size(), 2U);
  EXPECT_NE(partial_node.provided_order.front().expression.get(),
            static_cast<const ir::SortPlan &>(*outer_sort)
                .Items()
                .front()
                .expression);
  EXPECT_EQ(partial_node.provided_order.front().direction,
            rg::PhysicalSortDirection::kAscending);

  PlannedQuery fallback_query = Plan("UNWIND [3, 1, 2] AS x RETURN DISTINCT x");
  const ir::LogicalPlan *distinct = FindPlan(
      *fallback_query.logical_plan, ir::LogicalPlanNodeType::kDistinct);
  ASSERT_NE(distinct, nullptr);
  rg::PhysicalPlan fallback_physical =
      rg::CreatePhysicalPlan(*fallback_query.logical_plan);
  const rg::PhysicalPlanNode &distinct_node =
      fallback_physical.NodeFor(*distinct);
  EXPECT_EQ(distinct_node.kind, rg::PhysicalOperatorKind::kHashDistinct);
  const auto &distinct_data = std::get<rg::HashDistinctOp>(distinct_node.data);
  ASSERT_EQ(distinct_data.grouping_items.size(), 1U);
  EXPECT_EQ(distinct_data.grouping_items.front().alias, "x");
  EXPECT_NE(distinct_data.grouping_items.front().expression.Expression(),
            static_cast<const ir::DistinctPlan &>(*distinct)
                .GroupingItems()
                .front()
                .expression);

  PlannedQuery aggregation_query =
      Plan("UNWIND [3, 1, 2] AS x RETURN x, count(*) AS count");
  const ir::LogicalPlan *aggregation = FindPlan(
      *aggregation_query.logical_plan, ir::LogicalPlanNodeType::kAggregation);
  ASSERT_NE(aggregation, nullptr);
  rg::PhysicalPlan aggregation_physical =
      rg::CreatePhysicalPlan(*aggregation_query.logical_plan);
  const rg::PhysicalPlanNode &aggregation_node =
      aggregation_physical.NodeFor(*aggregation);
  EXPECT_EQ(aggregation_node.kind, rg::PhysicalOperatorKind::kHashAggregation);
  const auto &aggregation_data =
      std::get<rg::HashAggregationOp>(aggregation_node.data);
  ASSERT_EQ(aggregation_data.grouping_items.size(), 1U);
  ASSERT_EQ(aggregation_data.aggregation_items.size(), 1U);
  EXPECT_EQ(aggregation_data.grouping_items.front().alias, "x");
  EXPECT_EQ(aggregation_data.aggregation_items.front().alias, "count");
  const auto &logical_aggregation =
      static_cast<const ir::AggregationPlan &>(*aggregation);
  EXPECT_NE(aggregation_data.grouping_items.front().expression.Expression(),
            logical_aggregation.GroupingItems().front().expression);
  EXPECT_NE(aggregation_data.aggregation_items.front().expression.Expression(),
            logical_aggregation.AggregationItems().front().expression);

  PlannedQuery sort_query = Plan("UNWIND [3, 1, 2] AS x RETURN x ORDER BY x");
  const ir::LogicalPlan *sort =
      FindPlan(*sort_query.logical_plan, ir::LogicalPlanNodeType::kSort);
  ASSERT_NE(sort, nullptr);
  rg::PhysicalPlan sort_physical =
      rg::CreatePhysicalPlan(*sort_query.logical_plan);
  const rg::PhysicalPlanNode &sort_node = sort_physical.NodeFor(*sort);
  EXPECT_EQ(sort_node.kind, rg::PhysicalOperatorKind::kFullSort);
  const auto &sort_data = std::get<rg::FullSortOp>(sort_node.data);
  ASSERT_EQ(sort_data.items.size(), 1U);
  EXPECT_NE(
      sort_data.items.front().expression.Expression(),
      static_cast<const ir::SortPlan &>(*sort).Items().front().expression);
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
  const auto &data = std::get<rg::PartialTopNOp>(node.data);
  EXPECT_EQ(data.prefix, 1U);
  ASSERT_EQ(data.items.size(), 2U);
  EXPECT_NE(
      data.items.front().expression.Expression(),
      static_cast<const ir::TopNPlan &>(*top_n).Items().front().expression);
  EXPECT_NE(data.limit.Expression(),
            static_cast<const ir::TopNPlan &>(*top_n).Limit());
}

TEST(PhysicalPlanTest,
     ExecutesSortOperatorsAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"}, {{"g", rg::Value(2)}, {"v", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"g", rg::Value(1)}, {"v", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"g", rg::Value(2)}, {"v", rg::Value(1)}});
  graph.CreateNode({"N"}, {{"g", rg::Value(1)}, {"v", rg::Value(1)}});

  rg::PhysicalPlan full_sort =
      DetachedPhysicalPlan("MATCH (n:N) RETURN n.v AS v ORDER BY v");
  ASSERT_NE(
      FindPhysicalPlan(full_sort.Root(), rg::PhysicalOperatorKind::kFullSort),
      nullptr);
  EXPECT_EQ(
      PhysicalRows(full_sort, graph, {"v"}),
      (std::vector<std::vector<rg::Value>>{
          {rg::Value(1)}, {rg::Value(1)}, {rg::Value(2)}, {rg::Value(2)}}));

  rg::PhysicalPlan top_n = DetachedPhysicalPlan(
      "MATCH (n:N) RETURN n.v AS v ORDER BY v DESC LIMIT $l");
  ASSERT_NE(FindPhysicalPlan(top_n.Root(), rg::PhysicalOperatorKind::kTopN),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(top_n, graph, {"v"}, {{"l", rg::Value(2)}}),
      (std::vector<std::vector<rg::Value>>{{rg::Value(2)}, {rg::Value(2)}}));

  const std::string partial_query =
      "MATCH (n:N) WITH n ORDER BY n.g "
      "RETURN n.g AS g, n.v AS v ORDER BY g, v";
  rg::PhysicalPlan partial_sort = DetachedPhysicalPlan(partial_query);
  ASSERT_NE(FindPhysicalPlan(partial_sort.Root(),
                             rg::PhysicalOperatorKind::kPartialSort),
            nullptr);
  const std::vector<std::vector<rg::Value>> sorted{
      {rg::Value(1), rg::Value(1)},
      {rg::Value(1), rg::Value(2)},
      {rg::Value(2), rg::Value(1)},
      {rg::Value(2), rg::Value(2)}};
  EXPECT_EQ(PhysicalRows(partial_sort, graph, {"g", "v"}), sorted);

  rg::PhysicalPlan partial_top_n =
      DetachedPhysicalPlan(partial_query + " LIMIT $l");
  ASSERT_NE(FindPhysicalPlan(partial_top_n.Root(),
                             rg::PhysicalOperatorKind::kPartialTopN),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(partial_top_n, graph, {"g", "v"}, {{"l", rg::Value(3)}}),
      (std::vector<std::vector<rg::Value>>(sorted.begin(),
                                           sorted.begin() + 3)));
}

TEST(PhysicalPlanTest,
     ExecutesDistinctOperatorsAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"}, {{"value", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(1)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(1.0)}});
  graph.CreateNode({"N"});
  graph.CreateNode({"N"});

  rg::PhysicalPlan hash_distinct =
      DetachedPhysicalPlan("MATCH (n:N) RETURN DISTINCT n.value AS value");
  ASSERT_NE(FindPhysicalPlan(hash_distinct.Root(),
                             rg::PhysicalOperatorKind::kHashDistinct),
            nullptr);
  const auto hash_rows = PhysicalRows(hash_distinct, graph, {"value"});
  ASSERT_EQ(hash_rows.size(), 3U);
  EXPECT_TRUE(rg::ValuesEqual(hash_rows[0][0], rg::Value(2)));
  EXPECT_TRUE(rg::ValuesEqual(hash_rows[1][0], rg::Value(1)));
  EXPECT_TRUE(hash_rows[2][0].IsNull());

  rg::PhysicalPlan ordered_distinct = DetachedPhysicalPlan(
      "MATCH (n:N) WITH n ORDER BY n.value "
      "RETURN DISTINCT n.value AS value");
  ASSERT_NE(FindPhysicalPlan(ordered_distinct.Root(),
                             rg::PhysicalOperatorKind::kOrderedDistinct),
            nullptr);
  const auto ordered_rows = PhysicalRows(ordered_distinct, graph, {"value"});
  ASSERT_EQ(ordered_rows.size(), 3U);
  EXPECT_TRUE(rg::ValuesEqual(ordered_rows[0][0], rg::Value(1)));
  EXPECT_TRUE(rg::ValuesEqual(ordered_rows[1][0], rg::Value(2)));
  EXPECT_TRUE(ordered_rows[2][0].IsNull());
}

TEST(PhysicalPlanTest,
     ExecutesAggregationOperatorsAfterLogicalPlanAndAstAreDestroyed) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"}, {{"group", rg::Value(2)}, {"value", rg::Value(20)}});
  graph.CreateNode({"N"}, {{"group", rg::Value(1)}, {"value", rg::Value(10)}});
  graph.CreateNode({"N"},
                   {{"group", rg::Value(2)}, {"value", rg::Value(20.0)}});
  graph.CreateNode({"N"}, {{"group", rg::Value(1)}, {"value", rg::Value(5)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(7)}});

  const std::string aggregation =
      "RETURN n.group AS group, count(*) AS count, "
      "sum(n.value) AS total, count(DISTINCT n.value) AS unique_count";
  rg::PhysicalPlan hash_aggregation =
      DetachedPhysicalPlan("MATCH (n:N) " + aggregation);
  ASSERT_NE(FindPhysicalPlan(hash_aggregation.Root(),
                             rg::PhysicalOperatorKind::kHashAggregation),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(hash_aggregation, graph,
                   {"group", "count", "total", "unique_count"}),
      (std::vector<std::vector<rg::Value>>{
          {rg::Value(2), rg::Value(2), rg::Value(40.0), rg::Value(1)},
          {rg::Value(1), rg::Value(2), rg::Value(15), rg::Value(2)},
          {rg::Value::Null(), rg::Value(1), rg::Value(7), rg::Value(1)}}));

  rg::PhysicalPlan ordered_aggregation = DetachedPhysicalPlan(
      "MATCH (n:N) WITH n ORDER BY n.group " + aggregation);
  ASSERT_NE(FindPhysicalPlan(ordered_aggregation.Root(),
                             rg::PhysicalOperatorKind::kOrderedAggregation),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(ordered_aggregation, graph,
                   {"group", "count", "total", "unique_count"}),
      (std::vector<std::vector<rg::Value>>{
          {rg::Value(1), rg::Value(2), rg::Value(15), rg::Value(2)},
          {rg::Value(2), rg::Value(2), rg::Value(40.0), rg::Value(1)},
          {rg::Value::Null(), rg::Value(1), rg::Value(7), rg::Value(1)}}));

  rg::PhysicalPlan empty_aggregation = DetachedPhysicalPlan(
      "MATCH (n:Missing) RETURN count(*) AS count, sum(n.value) AS total");
  ASSERT_NE(FindPhysicalPlan(empty_aggregation.Root(),
                             rg::PhysicalOperatorKind::kHashAggregation),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(empty_aggregation, graph, {"count", "total"}),
      (std::vector<std::vector<rg::Value>>{{rg::Value(0), rg::Value(0)}}));
}

TEST(PhysicalPlanTest, PrintsOwnedOrderingAfterLogicalPlanAndAstAreDestroyed) {
  rg::PhysicalPlan physical = DetachedPhysicalPlan(
      "UNWIND [{a:1,b:2},{a:1,b:1}] AS x "
      "WITH x ORDER BY x.a "
      "RETURN x.a AS a, x.b AS b ORDER BY a, b");

  const std::string printed = rg::PhysicalPlanToString(physical);

  EXPECT_NE(printed.find("PartialSort"), std::string::npos);
  EXPECT_NE(printed.find("logical=Sort"), std::string::npos);
  EXPECT_EQ(printed.find("exec="), std::string::npos);
  EXPECT_NE(printed.find("order=[a ASC, b ASC]"), std::string::npos);
  EXPECT_NE(printed.find("prefix=1"), std::string::npos);
  EXPECT_NE(printed.find("slots=[a:reference@0?"), std::string::npos);
}
