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
#include "planner/planned_query.h"
#include "runtime/physical_executor.h"
#include "runtime/physical_plan_printer.h"
#include "tests/common/exception_test_utils.h"
#include "tests/planner/assume_all_indexes_catalog.h"
#include "tests/runtime/graphdb_test_utils.h"

namespace {

using PlannedQuery = planner::PlannedQuery;

planner::PlannedQuery Plan(std::string cypher) {
  return planner::PlanCypher(
      cypher, {.planner_catalog = &test_support::AssumeAllIndexesCatalog()});
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

const runtime::PhysicalPlanNode *FindPhysicalPlan(
    const runtime::PhysicalPlanNode &node, runtime::PhysicalOperatorKind kind) {
  if (node.kind == kind) {
    return &node;
  }
  for (const auto &child : node.children) {
    if (const runtime::PhysicalPlanNode *found =
            FindPhysicalPlan(*child, kind)) {
      return found;
    }
  }
  return nullptr;
}

runtime::PhysicalPlan DetachedPhysicalPlan(const std::string &cypher) {
  planner::PlannedQuery query = Plan(cypher);
  return runtime::CreatePhysicalPlan(query.LogicalPlan());
}

runtime::PhysicalPlan DetachedRelationshipTypeScan() {
  ir::RelationshipTypeScanPlan logical("a", "r", "b",
                                       ir::ExpandDirection::kOutgoing, {"R"});
  return runtime::CreatePhysicalPlan(logical);
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

runtime::PhysicalPlan DetachedParameterNodeHashJoin() {
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
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedLeftOuterHashJoin() {
  auto left = std::make_unique<ir::NodeByLabelScanPlan>("a", "Input");
  auto right = std::make_unique<ir::ExpandPlan>(
      std::make_unique<ir::NodeByLabelScanPlan>("a", "Input"), "a", "r", "b",
      ir::ExpandDirection::kOutgoing, std::vector<std::string>{"R"});
  ir::LeftOuterHashJoinPlan logical(std::move(left), std::move(right), {"a"});
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedCartesianProduct() {
  ir::CartesianProductPlan logical(
      std::make_unique<ir::NodeByLabelScanPlan>("a", "Left"),
      std::make_unique<ir::NodeByLabelScanPlan>("b", "Right"));
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedParameterCartesianProduct() {
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
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedLimitedApply() {
  std::unique_ptr<ast::IntegerLiteral> one = Integer(1);
  auto right = std::make_unique<ir::LimitPlan>(
      std::make_unique<ir::ExpandPlan>(
          std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"n"}),
          "n", "r", "m", ir::ExpandDirection::kOutgoing,
          std::vector<std::string>{"R"}),
      one.get());
  ir::ApplyPlan logical(std::make_unique<ir::NodeByLabelScanPlan>("n", "Input"),
                        std::move(right));
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedOptionalApply() {
  auto right = std::make_unique<ir::ExpandPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"n"}), "n",
      "r", "m", ir::ExpandDirection::kOutgoing, std::vector<std::string>{"R"});
  ir::OptionalApplyPlan logical(
      std::make_unique<ir::NodeByLabelScanPlan>("n", "Input"),
      std::move(right));
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedConflictingApply() {
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
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedExpandInto() {
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
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedProjectEndpoints(bool variable_length) {
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
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedPruningVarExpand() {
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
  return runtime::CreatePhysicalPlan(logical);
}

runtime::PhysicalPlan DetachedAssertIsNode() {
  std::unique_ptr<ast::Parameter> value = Parameter("value");
  auto source = std::make_unique<ir::ProjectionPlan>(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      std::vector<ir::LogicalProjectionItem>{
          {.expression = value.get(), .alias = "n"}});
  ir::AssertIsNodePlan logical(std::move(source), {"n"});
  return runtime::CreatePhysicalPlan(logical);
}

std::vector<std::vector<rg::Value>> PhysicalRows(
    const runtime::PhysicalPlan &plan,
    runtime::test::GraphDBTestDatabase &graph,
    const std::vector<std::string> &columns,
    const runtime::QueryParameters &parameters = {}) {
  auto transaction = graph.BeginTransaction();
  try {
    std::unique_ptr<runtime::PhysicalResultCursor> cursor =
        runtime::StartPhysicalPlan(plan, *transaction, parameters, columns);
    std::vector<std::vector<rg::Value>> rows;
    std::vector<rg::Value> row;
    while (cursor->Next(&row)) {
      rows.push_back(row);
    }
    cursor->Close();
    transaction->Commit();
    return rows;
  } catch (...) {
    runtime::test::RollbackGraphDBTransaction(transaction.get());
    throw;
  }
}

std::vector<std::vector<rg::Value>> PhysicalWriteRows(
    const runtime::PhysicalPlan &plan,
    runtime::test::GraphDBTestDatabase *graph,
    const std::vector<std::string> &columns,
    const runtime::QueryParameters &parameters = {}) {
  auto transaction = graph->BeginTransaction();
  try {
    std::unique_ptr<runtime::PhysicalResultCursor> cursor =
        runtime::StartPhysicalPlan(plan, *transaction, parameters, columns);
    std::vector<std::vector<rg::Value>> rows;
    std::vector<rg::Value> row;
    while (cursor->Next(&row)) {
      rows.push_back(row);
    }
    cursor->Close();
    transaction->Commit();
    return rows;
  } catch (...) {
    runtime::test::RollbackGraphDBTransaction(transaction.get());
    throw;
  }
}

std::vector<std::int64_t> FirstColumnIds(
    const runtime::PhysicalPlan &plan,
    runtime::test::GraphDBTestDatabase &graph, std::string column,
    const runtime::QueryParameters &parameters = {}) {
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

class GraphDBPhysicalResultCursor final : public runtime::PhysicalResultCursor {
 public:
  GraphDBPhysicalResultCursor(
      std::unique_ptr<graphdb::Transaction> transaction,
      std::unique_ptr<runtime::PhysicalResultCursor> cursor)
      : transaction_(std::move(transaction)), cursor_(std::move(cursor)) {}

  ~GraphDBPhysicalResultCursor() override { Close(); }

  [[nodiscard]] bool Next(std::vector<rg::Value> *row) override {
    if (closed_) {
      return false;
    }
    try {
      if (cursor_->Next(row)) {
        return true;
      }
      exhausted_ = true;
      Commit();
      closed_ = true;
      return false;
    } catch (...) {
      Rollback();
      closed_ = true;
      throw;
    }
  }

  void Cancel() noexcept override { cursor_->Cancel(); }

  void Close() noexcept override {
    if (closed_) {
      return;
    }
    cursor_->Close();
    if (exhausted_) {
      Commit();
    } else {
      Rollback();
    }
    closed_ = true;
  }

  [[nodiscard]] std::size_t PeakMemoryBytes() const noexcept override {
    return cursor_->PeakMemoryBytes();
  }

 private:
  void Commit() noexcept {
    if (transaction_ != nullptr &&
        transaction_->GetState() == graphdb::Transaction::State::kActive) {
      try {
        transaction_->Commit();
      } catch (...) {
        Rollback();
      }
    }
  }

  void Rollback() noexcept {
    runtime::test::RollbackGraphDBTransaction(transaction_.get());
  }

  std::unique_ptr<graphdb::Transaction> transaction_;
  std::unique_ptr<runtime::PhysicalResultCursor> cursor_;
  bool exhausted_ = false;
  bool closed_ = false;
};

void ExpectSameOffset(std::size_t actual, std::size_t expected) {
  EXPECT_EQ(actual, expected);
}

}  // namespace

namespace runtime {

std::unique_ptr<PhysicalResultCursor> StartGraphDBPhysicalPlan(
    const PhysicalPlan &plan, test::GraphDBTestDatabase &database,
    const QueryParameters &parameters,
    const std::vector<std::string> &result_columns,
    QueryExecutionOptions options = {}) {
  auto transaction = database.BeginTransaction();
  try {
    auto cursor = StartPhysicalPlan(plan, *transaction, parameters,
                                    result_columns, std::move(options));
    return std::make_unique<GraphDBPhysicalResultCursor>(std::move(transaction),
                                                         std::move(cursor));
  } catch (...) {
    test::RollbackGraphDBTransaction(transaction.get());
    throw;
  }
}

}  // namespace runtime

TEST(PhysicalPlanTest, ReusesIdenticalUnaryRowLayouts) {
  PlannedQuery query =
      Plan("MATCH (n) WHERE id(n) > 0 RETURN n SKIP 1 LIMIT 2");
  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());

  const runtime::PhysicalPlanNode *filter =
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kFilter);
  const runtime::PhysicalPlanNode *projection = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kProjection);
  const runtime::PhysicalPlanNode *skip =
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kSkip);
  const runtime::PhysicalPlanNode *limit =
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kLimit);
  ASSERT_NE(filter, nullptr);
  ASSERT_NE(projection, nullptr);
  ASSERT_NE(skip, nullptr);
  ASSERT_NE(limit, nullptr);
  ASSERT_EQ(filter->children.size(), 1U);
  ASSERT_EQ(projection->children.size(), 1U);
  ASSERT_EQ(skip->children.size(), 1U);
  ASSERT_EQ(limit->children.size(), 1U);
  ASSERT_EQ(physical.Root().children.size(), 1U);

  EXPECT_EQ(filter->output_layout, filter->children[0]->output_layout);
  EXPECT_EQ(projection->output_layout, projection->children[0]->output_layout);
  EXPECT_EQ(skip->output_layout, skip->children[0]->output_layout);
  EXPECT_EQ(limit->output_layout, limit->children[0]->output_layout);
  EXPECT_EQ(physical.Root().output_layout,
            physical.Root().children[0]->output_layout);
}

TEST(PhysicalPlanTest, CapturesExternallyVisibleResultColumns) {
  ir::AllNodeScanPlan read_plan("n");
  runtime::PhysicalPlan read_physical = runtime::CreatePhysicalPlan(read_plan);
  EXPECT_EQ(read_physical.ResultColumns(), (std::vector<std::string>{"n"}));

  runtime::PhysicalPlan returning_write =
      DetachedPhysicalPlan("CREATE (n:Made) RETURN n");
  EXPECT_EQ(returning_write.ResultColumns(), (std::vector<std::string>{"n"}));

  runtime::PhysicalPlan silent_write = DetachedPhysicalPlan("CREATE (:Made)");
  EXPECT_TRUE(silent_write.ResultColumns().empty());
}

TEST(PhysicalPlanTest, ResolvesPassthroughOffsetsWithoutEntityConversion) {
  auto source = std::make_unique<ir::RelationshipTypeScanPlan>(
      "a", "r", "b", ir::ExpandDirection::kOutgoing,
      std::vector<std::string>{"R"});
  ir::ProjectionPlan logical(std::move(source),
                             std::vector<ir::LogicalProjectionItem>{
                                 {.alias = "r", .passthrough = true}});
  runtime::PhysicalPlan physical = runtime::CreatePhysicalPlan(logical);

  ASSERT_EQ(physical.Root().kind, runtime::PhysicalOperatorKind::kProjection);
  ASSERT_EQ(physical.Root().children.size(), 1U);
  EXPECT_NE(physical.Root().output_layout,
            physical.Root().children[0]->output_layout);
  const auto &data = std::get<runtime::ProjectionOp>(physical.Root().data);
  ASSERT_EQ(data.items.size(), 1U);
  ASSERT_TRUE(data.items[0].source_offset.has_value());
  EXPECT_EQ(*data.items[0].source_offset,
            physical.Root().children[0]->output_layout->At("r"));

  runtime::test::GraphDBTestDatabase graph;
  const auto first = graph.CreateNode({});
  const auto second = graph.CreateNode({});
  const auto relationship = graph.CreateRelationship(first, second, "R");
  const auto rows = PhysicalRows(physical, graph, {"r"});
  ASSERT_EQ(rows.size(), 1U);
  ASSERT_EQ(rows[0].size(), 1U);
  ASSERT_TRUE(rows[0][0].IsRelationship());
  EXPECT_EQ(rows[0][0].AsRelationship().id, relationship->id);
  EXPECT_EQ(rows[0][0].AsRelationship().type_id, relationship->type_id);
}

TEST(PhysicalPlanTest, AllocatesOffsetsForOptionalExpand) {
  PlannedQuery query =
      Plan("MATCH (n) OPTIONAL MATCH (n)-[r]->(m) RETURN n, r, m");
  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());

  EXPECT_TRUE(physical.Root().output_layout->Contains("n"));
  EXPECT_TRUE(physical.Root().output_layout->Contains("r"));
  EXPECT_TRUE(physical.Root().output_layout->Contains("m"));
  const runtime::PhysicalPlanNode *expand_node = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kOptionalExpand);
  ASSERT_NE(expand_node, nullptr);
  ASSERT_EQ(expand_node->children.size(), 1U);
  EXPECT_TRUE(expand_node->children[0]->output_layout->Contains("n"));
}

TEST(PhysicalPlanTest, BuildsUnionRowLayout) {
  PlannedQuery query =
      Plan("MATCH (n) RETURN n AS value UNION ALL RETURN 1 AS value");
  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());

  EXPECT_TRUE(physical.Root().output_layout->Contains("value"));
  const runtime::PhysicalPlanNode *union_node = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kUnionAll);
  ASSERT_NE(union_node, nullptr);
  EXPECT_TRUE(std::holds_alternative<runtime::UnionAllOp>(union_node->data));
  ASSERT_EQ(union_node->child_mappings.size(), 2U);
  ASSERT_EQ(union_node->child_mappings[0].size(), 1U);
  ASSERT_EQ(union_node->child_mappings[1].size(), 1U);
}

TEST(PhysicalPlanTest, SelectsDedicatedUnionPhysicalAlgorithms) {
  PlannedQuery query = Plan("RETURN 1 AS value UNION RETURN 2 AS value");
  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());
  const runtime::PhysicalPlanNode *union_node = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kUnionDistinct);
  ASSERT_NE(union_node, nullptr);
  const auto &data = std::get<runtime::UnionDistinctOp>(union_node->data);
  ASSERT_EQ(data.key_offsets.size(), 1U);
  const std::size_t output_offset = union_node->output_layout->At("value");
  EXPECT_EQ(data.key_offsets.front(), output_offset);

  const std::string printed = runtime::PhysicalPlanToString(physical);
  EXPECT_NE(printed.find("UnionDistinct"), std::string::npos);
  EXPECT_NE(printed.find("logical=Union"), std::string::npos);
}

TEST(PhysicalPlanTest, ExecutesDetachedUnionAlgorithmsAndMappings) {
  runtime::test::GraphDBTestDatabase graph;
  runtime::PhysicalPlan all = DetachedPhysicalPlan(
      "RETURN 1 AS value UNION ALL RETURN 1.0 AS value "
      "UNION ALL RETURN null AS value");
  ASSERT_NE(
      FindPhysicalPlan(all.Root(), runtime::PhysicalOperatorKind::kUnionAll),
      nullptr);
  const auto all_rows = PhysicalRows(all, graph, {"value"});
  ASSERT_EQ(all_rows.size(), 3U);
  ASSERT_EQ(all_rows[0].size(), 1U);
  ASSERT_EQ(all_rows[1].size(), 1U);
  ASSERT_EQ(all_rows[2].size(), 1U);
  EXPECT_TRUE(all_rows[0][0].IsInteger());
  EXPECT_TRUE(all_rows[1][0].IsDouble());
  EXPECT_TRUE(all_rows[2][0].IsNull());

  runtime::PhysicalPlan distinct = DetachedPhysicalPlan(
      "RETURN 1 AS value UNION RETURN 1.0 AS value "
      "UNION RETURN null AS value UNION RETURN null AS value");
  ASSERT_NE(FindPhysicalPlan(distinct.Root(),
                             runtime::PhysicalOperatorKind::kUnionDistinct),
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
  runtime::PhysicalPlan merged_layout = DetachedPhysicalPlan(
      "MATCH (n) RETURN n AS value UNION ALL RETURN 1 AS value");
  const auto mapped_rows = PhysicalRows(merged_layout, graph, {"value"});
  ASSERT_EQ(mapped_rows.size(), 2U);
  EXPECT_EQ(mapped_rows[0][0], rg::Value(node));
  EXPECT_EQ(mapped_rows[1][0], rg::Value(1));
}

TEST(PhysicalPlanTest, UnionOperatorsHandleResourcesAndEarlyClose) {
  runtime::test::GraphDBTestDatabase graph;
  runtime::PhysicalPlan all = DetachedPhysicalPlan(
      "RETURN 'left' AS value UNION ALL RETURN 'right' AS value");
  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(all, graph, {}, {"value"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  runtime::QueryExecutionOptions all_memory_options;
  all_memory_options.memory_limit_bytes = 1;
  cursor = runtime::StartGraphDBPhysicalPlan(all, graph, {}, {"value"},
                                             all_memory_options);
  EXPECT_TRUE(cursor->Next(&row));
  EXPECT_TRUE(cursor->Next(&row));
  EXPECT_FALSE(cursor->Next(&row));
  EXPECT_EQ(cursor->PeakMemoryBytes(), 0U);

  runtime::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<runtime::QueryCancellationToken>();
  cursor = runtime::StartGraphDBPhysicalPlan(all, graph, {}, {"value"},
                                             cancellation_options);
  cancellation_options.cancellation->Cancel();
  RG_EXPECT_ERROR((void)cursor->Next(&row), common::ErrorCode::QueryCancelled);

  runtime::PhysicalPlan distinct = DetachedPhysicalPlan(
      "RETURN 'left' AS value UNION RETURN 'right' AS value");
  runtime::QueryExecutionOptions distinct_memory_options;
  distinct_memory_options.memory_limit_bytes = 1;
  cursor = runtime::StartGraphDBPhysicalPlan(distinct, graph, {}, {"value"},
                                             distinct_memory_options);
  RG_EXPECT_ERROR((void)cursor->Next(&row),
                  common::ErrorCode::MemoryLimitExceeded);
}

TEST(PhysicalPlanTest, BuildsTypedApplyAndOptionalApplyPayloads) {
  PlannedQuery query =
      Plan("MATCH (n) WITH DISTINCT n MATCH (n)-[r]->(m) RETURN n, r, m");
  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());
  const runtime::PhysicalPlanNode *apply_node =
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kApply);
  ASSERT_NE(apply_node, nullptr);
  EXPECT_TRUE(std::holds_alternative<runtime::ApplyOp>(apply_node->data));

  runtime::PhysicalPlan optional = DetachedOptionalApply();
  EXPECT_EQ(optional.Root().kind,
            runtime::PhysicalOperatorKind::kOptionalApply);
  EXPECT_TRUE(
      std::holds_alternative<runtime::OptionalApplyOp>(optional.Root().data));
}

TEST(PhysicalPlanTest, BuildsOwnedExistenceApplyPayloads) {
  runtime::PhysicalPlan semi = DetachedPhysicalPlan(
      "MATCH (n) WHERE EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } RETURN n");
  const runtime::PhysicalPlanNode *semi_node =
      FindPhysicalPlan(semi.Root(), runtime::PhysicalOperatorKind::kSemiApply);
  ASSERT_NE(semi_node, nullptr);
  EXPECT_TRUE(std::holds_alternative<runtime::SemiApplyOp>(semi_node->data));

  runtime::PhysicalPlan anti = DetachedPhysicalPlan(
      "MATCH (n) WHERE NOT EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } "
      "RETURN n");
  const runtime::PhysicalPlanNode *anti_node = FindPhysicalPlan(
      anti.Root(), runtime::PhysicalOperatorKind::kAntiSemiApply);
  ASSERT_NE(anti_node, nullptr);
  EXPECT_TRUE(
      std::holds_alternative<runtime::AntiSemiApplyOp>(anti_node->data));

  runtime::PhysicalPlan let = DetachedPhysicalPlan(
      "MATCH (n) RETURN EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } AS has");
  const runtime::PhysicalPlanNode *let_node = FindPhysicalPlan(
      let.Root(), runtime::PhysicalOperatorKind::kLetSemiApply);
  ASSERT_NE(let_node, nullptr);
  EXPECT_TRUE(std::holds_alternative<runtime::LetSemiApplyOp>(let_node->data));

  runtime::PhysicalPlan select =
      DetachedPhysicalPlan("MATCH (n) WHERE n.active OR (n)-[:R]->() RETURN n");
  const runtime::PhysicalPlanNode *select_node = FindPhysicalPlan(
      select.Root(), runtime::PhysicalOperatorKind::kSelectOrSemiApply);
  ASSERT_NE(select_node, nullptr);
  const auto &select_data =
      std::get<runtime::SelectOrSemiApplyOp>(select_node->data);
  EXPECT_NE(select_data.predicate.Expression(), nullptr);
  EXPECT_FALSE(select_data.anti);

  runtime::PhysicalPlan select_anti = DetachedPhysicalPlan(
      "MATCH (n) WHERE n.active OR NOT (n)-[:R]->() RETURN n");
  const runtime::PhysicalPlanNode *select_anti_node = FindPhysicalPlan(
      select_anti.Root(), runtime::PhysicalOperatorKind::kSelectOrSemiApply);
  ASSERT_NE(select_anti_node, nullptr);
  EXPECT_TRUE(
      std::get<runtime::SelectOrSemiApplyOp>(select_anti_node->data).anti);
}

TEST(PhysicalPlanTest, ExecutesDetachedExistenceApplyOperators) {
  runtime::test::GraphDBTestDatabase graph;
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

  runtime::PhysicalPlan semi = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } "
      "RETURN id(n) AS id");
  EXPECT_EQ(FirstColumnIds(semi, graph, "id"),
            (std::vector<std::int64_t>{matched->id}));

  runtime::PhysicalPlan anti = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE NOT EXISTS { MATCH (n)-[:R]->() RETURN 1 AS ok } "
      "RETURN id(n) AS id");
  std::vector<std::int64_t> expected_anti{unmatched->id, short_circuited->id};
  std::sort(expected_anti.begin(), expected_anti.end());
  EXPECT_EQ(FirstColumnIds(anti, graph, "id"), expected_anti);

  runtime::PhysicalPlan let = DetachedPhysicalPlan(
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

  runtime::PhysicalPlan select = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE n.active OR (n)-[:R]->() "
      "RETURN id(n) AS id");
  std::vector<std::int64_t> expected_select{matched->id, short_circuited->id};
  std::sort(expected_select.begin(), expected_select.end());
  EXPECT_EQ(FirstColumnIds(select, graph, "id"), expected_select);

  runtime::PhysicalPlan select_anti = DetachedPhysicalPlan(
      "MATCH (n:Input) WHERE n.active OR NOT (n)-[:R]->() "
      "RETURN id(n) AS id");
  std::vector<std::int64_t> expected_select_anti{unmatched->id,
                                                 short_circuited->id};
  std::sort(expected_select_anti.begin(), expected_select_anti.end());
  EXPECT_EQ(FirstColumnIds(select_anti, graph, "id"), expected_select_anti);

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(let, graph, {}, {"id", "has"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(PhysicalPlanTest, BuildsOwnedRollUpApplyPayload) {
  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (n:Input) RETURN id(n) AS id, "
      "[(n)-[:R]->(m) | m.name] AS names");
  const runtime::PhysicalPlanNode *roll_up = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kRollUpApply);
  ASSERT_NE(roll_up, nullptr);
  EXPECT_TRUE(std::holds_alternative<runtime::RollUpApplyOp>(roll_up->data));
  EXPECT_NE(runtime::PhysicalPlanToString(physical).find("RollUpApply"),
            std::string::npos);
}

TEST(PhysicalPlanTest, ExecutesDetachedRollUpApplyAndHandlesResources) {
  runtime::test::GraphDBTestDatabase graph;
  const auto matched = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first =
      graph.CreateNode({}, {{"name", rg::Value("first target")}});
  const auto second =
      graph.CreateNode({}, {{"name", rg::Value("second target")}});
  graph.CreateRelationship(matched, first, "R");
  graph.CreateRelationship(matched, second, "R");

  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
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

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"id", "names"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  runtime::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<runtime::QueryCancellationToken>();
  cursor = runtime::StartGraphDBPhysicalPlan(
      physical, graph, {}, {"id", "names"}, cancellation_options);
  cancellation_options.cancellation->Cancel();
  RG_EXPECT_ERROR((void)cursor->Next(&row), common::ErrorCode::QueryCancelled);

  runtime::QueryExecutionOptions memory_options;
  memory_options.memory_limit_bytes = 1;
  cursor = runtime::StartGraphDBPhysicalPlan(physical, graph, {},
                                             {"id", "names"}, memory_options);
  RG_EXPECT_ERROR((void)cursor->Next(&row),
                  common::ErrorCode::MemoryLimitExceeded);
}

TEST(PhysicalPlanTest, BuildsOwnedStreamingWritePayloads) {
  runtime::PhysicalPlan create =
      DetachedPhysicalPlan("CREATE (a:Made $properties) RETURN a");
  EXPECT_TRUE(create.Effects().writes);
  EXPECT_TRUE(create.Effects().contains_write_barrier);
  const runtime::PhysicalPlanNode *create_node = FindPhysicalPlan(
      create.Root(), runtime::PhysicalOperatorKind::kCreateNode);
  ASSERT_NE(create_node, nullptr);
  EXPECT_TRUE(create_node->traits.writes);
  EXPECT_FALSE(create_node->traits.write_barrier);
  const auto &create_node_data =
      std::get<runtime::CreateNodeOp>(create_node->data);
  EXPECT_EQ(create_node_data.labels, std::vector<std::string>{"Made"});
  ASSERT_TRUE(create_node_data.properties.parameter.has_value());
  EXPECT_NE(create_node_data.properties.parameter->Expression(), nullptr);
  const runtime::PhysicalPlanNode *barrier = FindPhysicalPlan(
      create.Root(), runtime::PhysicalOperatorKind::kWriteBarrier);
  ASSERT_NE(barrier, nullptr);
  EXPECT_TRUE(std::holds_alternative<runtime::WriteBarrierOp>(barrier->data));
  EXPECT_FALSE(barrier->traits.writes);
  EXPECT_TRUE(barrier->traits.write_barrier);
  EXPECT_FALSE(barrier->subtree_effects.writes);
  EXPECT_TRUE(barrier->subtree_effects.contains_write_barrier);

  runtime::PhysicalPlan create_relationship_plan = DetachedPhysicalPlan(
      "CREATE (a:Made), (b:Made), "
      "(a)-[r:LINK {weight: 2}]->(b) RETURN a, r, b");
  const runtime::PhysicalPlanNode *create_relationship =
      FindPhysicalPlan(create_relationship_plan.Root(),
                       runtime::PhysicalOperatorKind::kCreateRelationship);
  ASSERT_NE(create_relationship, nullptr);
  EXPECT_TRUE(create_relationship->traits.writes);
  const auto &relationship_data =
      std::get<runtime::CreateRelationshipOp>(create_relationship->data);
  EXPECT_EQ(relationship_data.type, "LINK");
  ASSERT_EQ(relationship_data.properties.entries.size(), 1U);
  EXPECT_NE(relationship_data.properties.entries[0].value.Expression(),
            nullptr);

  runtime::PhysicalPlan mutations = DetachedPhysicalPlan(
      "MATCH (n) SET n.changed = 'yes', n += {added: 2}, n:Added "
      "REMOVE n.old, n:Old RETURN n");
  const runtime::PhysicalPlanNode *set_property = FindPhysicalPlan(
      mutations.Root(), runtime::PhysicalOperatorKind::kSetProperty);
  ASSERT_NE(set_property, nullptr);
  EXPECT_TRUE(set_property->traits.writes);
  const auto &set_property_data =
      std::get<runtime::SetPropertyOp>(set_property->data);
  EXPECT_EQ(set_property_data.property_key, "changed");
  EXPECT_NE(set_property_data.entity.Expression(), nullptr);
  EXPECT_NE(set_property_data.value.Expression(), nullptr);

  const runtime::PhysicalPlanNode *set_properties = FindPhysicalPlan(
      mutations.Root(), runtime::PhysicalOperatorKind::kSetProperties);
  ASSERT_NE(set_properties, nullptr);
  EXPECT_TRUE(set_properties->traits.writes);
  EXPECT_TRUE(std::get<runtime::SetPropertiesOp>(set_properties->data)
                  .include_existing);
  const runtime::PhysicalPlanNode *set_labels = FindPhysicalPlan(
      mutations.Root(), runtime::PhysicalOperatorKind::kSetLabels);
  ASSERT_NE(set_labels, nullptr);
  EXPECT_TRUE(set_labels->traits.writes);
  EXPECT_EQ(std::get<runtime::SetLabelsOp>(set_labels->data).labels,
            std::vector<std::string>{"Added"});

  const runtime::PhysicalPlanNode *remove_property = FindPhysicalPlan(
      mutations.Root(), runtime::PhysicalOperatorKind::kRemoveProperty);
  ASSERT_NE(remove_property, nullptr);
  EXPECT_TRUE(remove_property->traits.writes);
  EXPECT_EQ(
      std::get<runtime::RemovePropertyOp>(remove_property->data).property_key,
      "old");
  const runtime::PhysicalPlanNode *remove_labels = FindPhysicalPlan(
      mutations.Root(), runtime::PhysicalOperatorKind::kRemoveLabels);
  ASSERT_NE(remove_labels, nullptr);
  EXPECT_TRUE(remove_labels->traits.writes);
  EXPECT_EQ(std::get<runtime::RemoveLabelsOp>(remove_labels->data).labels,
            std::vector<std::string>{"Old"});

  runtime::PhysicalPlan exact =
      DetachedPhysicalPlan("MATCH (n) SET n = {only: 1} RETURN n");
  const runtime::PhysicalPlanNode *exact_set = FindPhysicalPlan(
      exact.Root(), runtime::PhysicalOperatorKind::kSetProperties);
  ASSERT_NE(exact_set, nullptr);
  EXPECT_FALSE(
      std::get<runtime::SetPropertiesOp>(exact_set->data).include_existing);
}

TEST(PhysicalPlanTest, ExecutesDetachedStreamingWriteOperators) {
  runtime::test::GraphDBTestDatabase graph;
  runtime::PhysicalPlan create = DetachedPhysicalPlan(
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
  runtime::PhysicalPlan mutations = DetachedPhysicalPlan(
      "MATCH (n:Target) SET n.changed = 'yes', n += {added: 2}, n:Added "
      "REMOVE n.old, n:Old RETURN n");
  ASSERT_EQ(PhysicalWriteRows(mutations, &graph, {"n"}).size(), 1U);
  const auto target_after_mutation = graph.Nodes();
  ASSERT_EQ(target_after_mutation.size(), 3U);
  const auto target_it = std::find_if(
      target_after_mutation.begin(), target_after_mutation.end(),
      [&](const auto &candidate) { return candidate->id == target->id; });
  ASSERT_NE(target_it, target_after_mutation.end());
  EXPECT_EQ((*target_it)->properties.at("changed"), rg::Value("yes"));
  EXPECT_EQ((*target_it)->properties.at("added"), rg::Value(2));
  EXPECT_EQ((*target_it)->properties.at("keep"), rg::Value(1));
  EXPECT_FALSE((*target_it)->properties.contains("old"));
  EXPECT_NE(std::find((*target_it)->labels.begin(), (*target_it)->labels.end(),
                      "Added"),
            (*target_it)->labels.end());
  EXPECT_EQ(std::find((*target_it)->labels.begin(), (*target_it)->labels.end(),
                      "Old"),
            (*target_it)->labels.end());

  runtime::PhysicalPlan exact =
      DetachedPhysicalPlan("MATCH (n:Target) SET n = {only: 9} RETURN n");
  ASSERT_EQ(PhysicalWriteRows(exact, &graph, {"n"}).size(), 1U);
  const auto target_after_exact = graph.Nodes();
  const auto exact_target_it = std::find_if(
      target_after_exact.begin(), target_after_exact.end(),
      [&](const auto &candidate) { return candidate->id == target->id; });
  ASSERT_NE(exact_target_it, target_after_exact.end());
  EXPECT_EQ((*exact_target_it)->properties,
            (rg::Value::Map{{"only", rg::Value(9)}}));

  runtime::PhysicalPlan relationship_mutation = DetachedPhysicalPlan(
      "MATCH ()-[r:LINK]->() SET r.flag = true REMOVE r.weight RETURN r");
  ASSERT_EQ(PhysicalWriteRows(relationship_mutation, &graph, {"r"}).size(), 1U);
  const rg::Relationship &relationship = create_rows[0][2].AsRelationship();
  const auto relationships = graph.Relationships();
  const auto relationship_it = std::find_if(
      relationships.begin(), relationships.end(),
      [&](const auto &candidate) { return candidate->id == relationship.id; });
  ASSERT_NE(relationship_it, relationships.end());
  EXPECT_EQ((*relationship_it)->properties.at("flag"), rg::Value(true));
  EXPECT_FALSE((*relationship_it)->properties.contains("weight"));
}

TEST(PhysicalPlanTest, BuildsOwnedMergePayload) {
  PlannedQuery query = Plan(
      "MERGE (n:Person {key: $key}) "
      "ON CREATE SET n.created = true, n += {extra: 1}, n:Fresh "
      "ON MATCH SET n.seen = true RETURN n");
  const ir::LogicalPlan *logical =
      FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kMerge);
  ASSERT_NE(logical, nullptr);
  const auto &logical_merge = static_cast<const ir::MergePlan &>(*logical);
  const ir::MergePattern &logical_data = logical_merge.Merge();
  ASSERT_EQ(logical_data.create_pattern.nodes.size(), 1U);
  ASSERT_EQ(logical_data.create_pattern.nodes[0].properties.entries.size(), 1U);
  const ast::Expression *logical_property =
      logical_data.create_pattern.nodes[0].properties.entries[0].value;

  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());
  const runtime::PhysicalPlanNode *merge =
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kMerge);
  ASSERT_NE(merge, nullptr);
  EXPECT_TRUE(merge->traits.writes);
  EXPECT_TRUE(physical.Effects().writes);
  const auto &data = std::get<runtime::MergeOp>(merge->data);
  ASSERT_EQ(data.create_commands.size(), 1U);
  const auto &create =
      std::get<runtime::CreateNodeOp>(data.create_commands.front());
  EXPECT_EQ(create.labels, std::vector<std::string>{"Person"});
  ASSERT_EQ(create.properties.entries.size(), 1U);
  EXPECT_NE(create.properties.entries.front().value.Expression(),
            logical_property);

  ASSERT_EQ(data.actions.size(), 2U);
  const runtime::PhysicalMergeAction *on_create = nullptr;
  const runtime::PhysicalMergeAction *on_match = nullptr;
  for (const auto &action : data.actions) {
    (action.on_match ? on_match : on_create) = &action;
  }
  ASSERT_NE(on_create, nullptr);
  ASSERT_NE(on_match, nullptr);
  ASSERT_EQ(on_create->set_operations.size(), 3U);
  EXPECT_TRUE(std::holds_alternative<runtime::SetPropertyOp>(
      on_create->set_operations[0]));
  ASSERT_TRUE(std::holds_alternative<runtime::SetPropertiesOp>(
      on_create->set_operations[1]));
  EXPECT_TRUE(std::get<runtime::SetPropertiesOp>(on_create->set_operations[1])
                  .include_existing);
  EXPECT_TRUE(std::holds_alternative<runtime::SetLabelsOp>(
      on_create->set_operations[2]));
  ASSERT_EQ(on_match->set_operations.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<runtime::SetPropertyOp>(
      on_match->set_operations.front()));
}

TEST(PhysicalPlanTest, ExecutesDetachedMergeCreateAndMatchActions) {
  runtime::test::GraphDBTestDatabase graph;
  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MERGE (n:Person) "
      "ON CREATE SET n = {created: true}, n += {extra: 1}, n:Fresh "
      "ON MATCH SET n.seen = true "
      "RETURN n.created AS created, n.seen AS seen");

  const auto created = PhysicalWriteRows(physical, &graph, {"created", "seen"});
  ASSERT_EQ(created.size(), 1U);
  ASSERT_EQ(created.front().size(), 2U);
  EXPECT_EQ(created.front()[0], rg::Value(true));
  EXPECT_TRUE(created.front()[1].IsNull());
  const auto nodes = graph.Nodes();
  ASSERT_EQ(nodes.size(), 1U);
  EXPECT_EQ(nodes.front()->properties.at("extra"), rg::Value(1));
  EXPECT_NE(std::find(nodes.front()->labels.begin(),
                      nodes.front()->labels.end(), "Fresh"),
            nodes.front()->labels.end());

  const auto matched = PhysicalWriteRows(physical, &graph, {"created", "seen"});
  ASSERT_EQ(matched.size(), 1U);
  ASSERT_EQ(matched.front().size(), 2U);
  EXPECT_EQ(matched.front()[0], rg::Value(true));
  EXPECT_EQ(matched.front()[1], rg::Value(true));
  EXPECT_EQ(graph.Nodes().size(), 1U);

  runtime::PhysicalPlan null_property =
      DetachedPhysicalPlan("MERGE (n:Invalid {key: $key}) RETURN n");
  RG_EXPECT_ERROR((void)PhysicalWriteRows(null_property, &graph, {"n"},
                                          {{"key", rg::Value::Null()}}),
                  common::ErrorCode::InvalidParameter);
}

TEST(PhysicalPlanTest, ExecutesDetachedRelationshipMerge) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"Source"}, {{"key", rg::Value(7)}});
  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (m:Source) "
      "MERGE (m)-[r:LINK {weight: m.key}]->(n:Target {key: m.key}) "
      "ON CREATE SET r.created = true "
      "ON MATCH SET r.seen = true RETURN n, r");

  ASSERT_EQ(PhysicalWriteRows(physical, &graph, {"n", "r"}).size(), 1U);
  ASSERT_EQ(graph.Nodes().size(), 2U);
  ASSERT_EQ(graph.Relationships().size(), 1U);
  EXPECT_EQ(graph.Relationships().front()->properties.at("weight"),
            rg::Value(7));
  EXPECT_EQ(graph.Relationships().front()->properties.at("created"),
            rg::Value(true));

  ASSERT_EQ(PhysicalWriteRows(physical, &graph, {"n", "r"}).size(), 1U);
  EXPECT_EQ(graph.Nodes().size(), 2U);
  EXPECT_EQ(graph.Relationships().size(), 1U);
  EXPECT_EQ(graph.Relationships().front()->properties.at("seen"),
            rg::Value(true));
}

TEST(PhysicalPlanTest, BuildsOwnedRemainingUnaryPayloads) {
  PlannedQuery unwind_query = Plan("UNWIND $values AS x RETURN x");
  const ir::LogicalPlan *logical_unwind =
      FindPlan(unwind_query.LogicalPlan(), ir::LogicalPlanNodeType::kUnwind);
  ASSERT_NE(logical_unwind, nullptr);
  const ast::Expression *logical_expression =
      static_cast<const ir::UnwindPlan &>(*logical_unwind).Expression();
  runtime::PhysicalPlan unwind =
      runtime::CreatePhysicalPlan(unwind_query.LogicalPlan());
  const runtime::PhysicalPlanNode *unwind_node =
      FindPhysicalPlan(unwind.Root(), runtime::PhysicalOperatorKind::kUnwind);
  ASSERT_NE(unwind_node, nullptr);
  const auto &unwind_data = std::get<runtime::UnwindOp>(unwind_node->data);
  EXPECT_NE(unwind_data.expression.Expression(), logical_expression);

  std::unique_ptr<ast::Parameter> argument = Parameter("argument");
  ir::ProcedureCallPlan logical_procedure(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      "db.labels", {argument.get()},
      {{.result_field = "label", .variable = "l"}}, false, true);
  runtime::PhysicalPlan procedure =
      runtime::CreatePhysicalPlan(logical_procedure);
  const auto &procedure_data =
      std::get<runtime::ProcedureCallOp>(procedure.Root().data);
  EXPECT_EQ(procedure_data.procedure_name, "db.labels");
  ASSERT_EQ(procedure_data.arguments.size(), 1U);
  EXPECT_NE(procedure_data.arguments.front().Expression(), argument.get());
  ASSERT_EQ(procedure_data.yields.size(), 1U);
  EXPECT_EQ(procedure_data.yields.front().result_field, "label");
  EXPECT_TRUE(procedure_data.read_only);
  EXPECT_FALSE(procedure.Effects().writes);

  ir::ProcedureCallPlan logical_write_procedure(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{}),
      "custom.write", {}, {}, false, false);
  runtime::PhysicalPlan write_procedure =
      runtime::CreatePhysicalPlan(logical_write_procedure);
  EXPECT_TRUE(write_procedure.Root().traits.writes);
  EXPECT_TRUE(write_procedure.Effects().writes);
  EXPECT_FALSE(write_procedure.Effects().contains_write_barrier);

  runtime::PhysicalPlan assertion = DetachedAssertIsNode();
  const auto &assertion_data =
      std::get<runtime::AssertIsNodeOp>(assertion.Root().data);
  ASSERT_EQ(assertion_data.nodes.size(), 1U);
  EXPECT_EQ(assertion_data.nodes.front().variable, "n");
}

TEST(PhysicalPlanTest, ExecutesDetachedUnwindAndProcedureCall) {
  runtime::test::GraphDBTestDatabase graph;
  runtime::PhysicalPlan unwind =
      DetachedPhysicalPlan("UNWIND $values AS x RETURN x");

  const auto list_rows = PhysicalRows(
      unwind, graph, {"x"},
      {{"values", rg::Value(rg::Value::List{rg::Value(1), rg::Value(2)})}});
  ASSERT_EQ(list_rows.size(), 2U);
  EXPECT_EQ(list_rows[0][0], rg::Value(1));
  EXPECT_EQ(list_rows[1][0], rg::Value(2));
  const auto scalar_rows =
      PhysicalRows(unwind, graph, {"x"}, {{"values", rg::Value(3)}});
  ASSERT_EQ(scalar_rows.size(), 1U);
  EXPECT_EQ(scalar_rows[0][0], rg::Value(3));
  EXPECT_TRUE(
      PhysicalRows(unwind, graph, {"x"}, {{"values", rg::Value::Null()}})
          .empty());

  graph.CreateNode({"B", "A"});
  runtime::PhysicalPlan procedure = DetachedPhysicalPlan(
      "UNWIND [0, 1] AS i "
      "CALL db.labels() YIELD label AS l RETURN i, l");
  const auto procedure_rows = PhysicalRows(procedure, graph, {"i", "l"});
  ASSERT_EQ(procedure_rows.size(), 4U);
  EXPECT_EQ(procedure_rows[0],
            (std::vector<rg::Value>{rg::Value(0), rg::Value("A")}));
  EXPECT_EQ(procedure_rows[1],
            (std::vector<rg::Value>{rg::Value(0), rg::Value("B")}));
  EXPECT_EQ(procedure_rows[2],
            (std::vector<rg::Value>{rg::Value(1), rg::Value("A")}));
  EXPECT_EQ(procedure_rows[3],
            (std::vector<rg::Value>{rg::Value(1), rg::Value("B")}));
}

TEST(PhysicalPlanTest, ExecutesDetachedAssertIsNode) {
  runtime::test::GraphDBTestDatabase graph;
  const auto node = graph.CreateNode({"N"});
  runtime::PhysicalPlan assertion = DetachedAssertIsNode();

  const auto node_rows =
      PhysicalRows(assertion, graph, {"n"}, {{"value", rg::Value(node)}});
  ASSERT_EQ(node_rows.size(), 1U);
  ASSERT_TRUE(node_rows[0][0].IsNode());
  EXPECT_EQ(node_rows[0][0].AsNode().id, node->id);
  const auto null_rows =
      PhysicalRows(assertion, graph, {"n"}, {{"value", rg::Value::Null()}});
  ASSERT_EQ(null_rows.size(), 1U);
  EXPECT_TRUE(null_rows[0][0].IsNull());
  RG_EXPECT_ERROR(
      (void)PhysicalRows(assertion, graph, {"n"}, {{"value", rg::Value(1)}}),
      common::ErrorCode::InvalidParameter);
}

TEST(PhysicalPlanTest, RemainingUnaryOperatorsHandleResources) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"Label"});
  runtime::PhysicalPlan procedure = DetachedPhysicalPlan("CALL db.labels()");
  std::vector<rg::Value> row;

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(procedure, graph, {}, {"label"});
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  runtime::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<runtime::QueryCancellationToken>();
  cursor = runtime::StartGraphDBPhysicalPlan(procedure, graph, {}, {"label"},
                                             cancellation_options);
  cancellation_options.cancellation->Cancel();
  RG_EXPECT_ERROR((void)cursor->Next(&row), common::ErrorCode::QueryCancelled);

  runtime::QueryExecutionOptions memory_options;
  memory_options.memory_limit_bytes = 1;
  cursor = runtime::StartGraphDBPhysicalPlan(procedure, graph, {}, {"label"},
                                             memory_options);
  RG_EXPECT_ERROR((void)cursor->Next(&row),
                  common::ErrorCode::MemoryLimitExceeded);

  runtime::PhysicalPlan unwind =
      DetachedPhysicalPlan("UNWIND ['value'] AS x RETURN x");
  cursor = runtime::StartGraphDBPhysicalPlan(unwind, graph, {}, {"x"},
                                             memory_options);
  RG_EXPECT_ERROR((void)cursor->Next(&row),
                  common::ErrorCode::MemoryLimitExceeded);
}

TEST(PhysicalPlanTest, BuildsOwnedDeletePayloads) {
  runtime::PhysicalPlan delete_plan =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN n");
  const runtime::PhysicalPlanNode *delete_node = FindPhysicalPlan(
      delete_plan.Root(), runtime::PhysicalOperatorKind::kDelete);
  ASSERT_NE(delete_node, nullptr);
  EXPECT_TRUE(delete_node->traits.writes);
  const auto &delete_data = std::get<runtime::DeleteOp>(delete_node->data);
  ASSERT_EQ(delete_data.expressions.size(), 1U);
  EXPECT_NE(delete_data.expressions.front().Expression(), nullptr);

  runtime::PhysicalPlan detach_plan =
      DetachedPhysicalPlan("MATCH (n:N) DETACH DELETE n RETURN n");
  const runtime::PhysicalPlanNode *detach_node = FindPhysicalPlan(
      detach_plan.Root(), runtime::PhysicalOperatorKind::kDetachDelete);
  ASSERT_NE(detach_node, nullptr);
  EXPECT_TRUE(detach_node->traits.writes);
  const auto &detach_data =
      std::get<runtime::DetachDeleteOp>(detach_node->data);
  ASSERT_EQ(detach_data.expressions.size(), 1U);
  EXPECT_NE(detach_data.expressions.front().Expression(), nullptr);
}

TEST(PhysicalPlanTest, ExecutesDetachedDeleteOperators) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"N"});
  runtime::PhysicalPlan delete_plan =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN 1 AS deleted");
  const auto deleted_rows = PhysicalWriteRows(delete_plan, &graph, {"deleted"});
  ASSERT_EQ(deleted_rows.size(), 1U);
  ASSERT_EQ(deleted_rows.front().size(), 1U);
  EXPECT_EQ(deleted_rows.front().front(), rg::Value(1));
  EXPECT_TRUE(graph.Nodes().empty());

  const auto left = graph.CreateNode({"N"});
  const auto right = graph.CreateNode({"N"});
  graph.CreateRelationship(left, right, "R");
  runtime::PhysicalPlan relationship_delete = DetachedPhysicalPlan(
      "MATCH ()-[r:R]->() DELETE r RETURN type(r) AS type");
  const auto relationship_rows =
      PhysicalWriteRows(relationship_delete, &graph, {"type"});
  ASSERT_EQ(relationship_rows.size(), 1U);
  EXPECT_EQ(relationship_rows.front().front(), rg::Value("R"));
  EXPECT_TRUE(graph.Relationships().empty());
  EXPECT_EQ(graph.Nodes().size(), 2U);

  const auto connected_left = graph.CreateNode({"N"});
  const auto connected_right = graph.CreateNode({"N"});
  graph.CreateRelationship(connected_left, connected_right, "R");
  runtime::PhysicalPlan invalid_delete =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN 1 AS deleted");
  RG_EXPECT_ERROR((void)PhysicalWriteRows(invalid_delete, &graph, {"deleted"}),
                  common::ErrorCode::InvalidParameter);
  EXPECT_EQ(graph.Nodes().size(), 4U);
  EXPECT_EQ(graph.Relationships().size(), 1U);

  runtime::PhysicalPlan detach_plan =
      DetachedPhysicalPlan("MATCH (n:N) DETACH DELETE n RETURN 1 AS deleted");
  const auto detach_rows = PhysicalWriteRows(detach_plan, &graph, {"deleted"});
  EXPECT_EQ(detach_rows.size(), 4U);
  EXPECT_TRUE(graph.Nodes().empty());
  EXPECT_TRUE(graph.Relationships().empty());
}

TEST(PhysicalPlanTest, DeleteOperatorHandlesCloseCancellationAndMemoryLimit) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"N"});
  runtime::PhysicalPlan physical =
      DetachedPhysicalPlan("MATCH (n:N) DELETE n RETURN n");
  std::vector<rg::Value> row;

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"n"});
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));
  EXPECT_EQ(graph.Nodes().size(), 1U);

  runtime::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<runtime::QueryCancellationToken>();
  cursor = runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"n"},
                                             cancellation_options);
  cancellation_options.cancellation->Cancel();
  RG_EXPECT_ERROR((void)cursor->Next(&row), common::ErrorCode::QueryCancelled);
  EXPECT_EQ(graph.Nodes().size(), 1U);

  runtime::QueryExecutionOptions memory_options;
  memory_options.memory_limit_bytes = 1;
  cursor = runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"n"},
                                             memory_options);
  RG_EXPECT_ERROR((void)cursor->Next(&row),
                  common::ErrorCode::MemoryLimitExceeded);
  EXPECT_EQ(graph.Nodes().size(), 1U);
}

TEST(PhysicalPlanTest, ReinstantiatesStatefulApplyRightSideForEachLeftRow) {
  runtime::test::GraphDBTestDatabase graph;
  const auto first = graph.CreateNode({"Input"});
  const auto second = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first_target = graph.CreateNode({});
  const auto second_target = graph.CreateNode({});
  graph.CreateRelationship(first, first_target, "R");
  graph.CreateRelationship(first, second_target, "R");
  graph.CreateRelationship(second, first_target, "R");
  graph.CreateRelationship(second, second_target, "R");

  runtime::PhysicalPlan physical = DetachedLimitedApply();
  ASSERT_EQ(physical.Root().kind, runtime::PhysicalOperatorKind::kApply);
  ASSERT_NE(
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kLimit),
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

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"n", "m"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  runtime::QueryExecutionOptions options;
  options.cancellation = std::make_shared<runtime::QueryCancellationToken>();
  cursor = runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"n", "m"},
                                             options);
  options.cancellation->Cancel();
  RG_EXPECT_ERROR((void)cursor->Next(&row), common::ErrorCode::QueryCancelled);
}

TEST(PhysicalPlanTest, OptionalApplyNullExtendsAfterLogicalPlanIsDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
  const auto matched = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first = graph.CreateNode({});
  const auto second = graph.CreateNode({});
  graph.CreateRelationship(matched, first, "R");
  graph.CreateRelationship(matched, second, "R");

  runtime::PhysicalPlan physical = DetachedOptionalApply();
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

TEST(PhysicalPlanTest, ApplyRejectsConflictingSharedOffsets) {
  runtime::test::GraphDBTestDatabase graph;
  runtime::PhysicalPlan physical = DetachedConflictingApply();

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

TEST(PhysicalPlanTest, BuildsOffsetsForExpressions) {
  PlannedQuery query = Plan("MATCH (n) RETURN [n][0] AS x");
  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());

  EXPECT_TRUE(physical.Root().output_layout->Contains("x"));
}

TEST(PhysicalPlanTest, BuildsOwnedPayloadsForMigratedOperators) {
  PlannedQuery query =
      Plan("MATCH (n) WHERE id(n) > 0 RETURN n AS node SKIP 1 LIMIT 2");
  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());
  EXPECT_FALSE(physical.Effects().writes);
  EXPECT_FALSE(physical.Effects().contains_write_barrier);

  const ir::LogicalPlan *filter =
      FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kFilter);
  ASSERT_NE(filter, nullptr);

  const runtime::PhysicalPlanNode *scan_node = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kAllNodeScan);
  ASSERT_NE(scan_node, nullptr);
  const auto &scan_data = std::get<runtime::AllNodeScanOp>(scan_node->data);
  EXPECT_EQ(scan_data.variable, "n");
  EXPECT_EQ(scan_data.output_offset, scan_node->output_layout->At("n"));

  const runtime::PhysicalPlanNode *filter_node =
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kFilter);
  ASSERT_NE(filter_node, nullptr);
  const auto &filter_data = std::get<runtime::FilterOp>(filter_node->data);
  EXPECT_NE(filter_data.predicate.Expression(),
            static_cast<const ir::FilterPlan &>(*filter).Predicate());

  const runtime::PhysicalPlanNode *projection_node = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kProjection);
  ASSERT_NE(projection_node, nullptr);
  const auto &projection_data =
      std::get<runtime::ProjectionOp>(projection_node->data);
  ASSERT_EQ(projection_data.items.size(), 1U);
  EXPECT_EQ(projection_data.items.front().alias, "node");
  EXPECT_EQ(projection_data.items.front().output_offset,
            projection_node->output_layout->At("node"));

  EXPECT_NE(
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kSkip),
      nullptr);
  EXPECT_NE(
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kLimit),
      nullptr);
  EXPECT_EQ(physical.Root().kind,
            runtime::PhysicalOperatorKind::kProduceResults);
}

TEST(PhysicalPlanTest, ExecutesMigratedPlanAfterLogicalPlanIsDestroyed) {
  runtime::PhysicalPlan physical = [] {
    PlannedQuery query =
        Plan("MATCH (n) WHERE id(n) > 0 RETURN id(n) AS id SKIP 1 LIMIT 1");
    return runtime::CreatePhysicalPlan(query.LogicalPlan());
  }();
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({});
  graph.CreateNode({});
  graph.CreateNode({});

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {},
                                        std::vector<std::string>{"id"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  ASSERT_EQ(row.size(), 1U);
  EXPECT_EQ(row.front(), rg::Value(2));
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(PhysicalPlanTest, BuildsTypedOwnedPayloadsForLeafAccessOperators) {
  {
    PlannedQuery query = Plan("RETURN 1 AS value");
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kArgument);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(std::holds_alternative<runtime::ArgumentOp>(node->data));
  }
  {
    PlannedQuery query = Plan("MATCH (n:N) RETURN id(n) AS id");
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kNodeByLabelScan);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::NodeByLabelScanOp>(node->data);
    EXPECT_EQ(data.variable, "n");
    EXPECT_EQ(data.output_offset, node->output_layout->At("n"));
    EXPECT_EQ(data.labels, (std::vector<std::string>{"N"}));
  }
  {
    PlannedQuery query =
        Plan("MATCH (n:N) WHERE n.value = 10 RETURN id(n) AS id");
    const auto *logical = static_cast<const ir::NodeIndexSeekPlan *>(
        FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kNodeIndexSeek));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kNodeIndexSeek);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::NodeIndexSeekOp>(node->data);
    EXPECT_EQ(data.output_offset, node->output_layout->At("n"));
    EXPECT_EQ(data.property_key, "value");
    EXPECT_NE(data.value.Expression(), logical->ValueExpression());
  }
  {
    PlannedQuery query =
        Plan("MATCH (n:N) WHERE n.value >= 10 RETURN id(n) AS id");
    const auto *logical =
        static_cast<const ir::NodeIndexRangeSeekPlan *>(FindPlan(
            query.LogicalPlan(), ir::LogicalPlanNodeType::kNodeIndexRangeSeek));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kNodeIndexRangeSeek);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::NodeIndexRangeSeekOp>(node->data);
    EXPECT_EQ(data.output_offset, node->output_layout->At("n"));
    ASSERT_EQ(data.predicates.size(), logical->Predicates().size());
    EXPECT_NE(data.predicates.front().Expression(),
              logical->Predicates().front());
  }
  {
    ir::RelationshipTypeScanPlan logical("a", "r", "b",
                                         ir::ExpandDirection::kOutgoing, {"R"});
    runtime::PhysicalPlan physical = runtime::CreatePhysicalPlan(logical);
    const auto &data =
        std::get<runtime::RelationshipTypeScanOp>(physical.Root().data);
    EXPECT_EQ(data.pattern.relationship, "r");
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
    EXPECT_EQ(data.offsets.from_node_output_offset,
              physical.Root().output_layout->Find("a"));
    EXPECT_EQ(data.offsets.relationship_output_offset,
              physical.Root().output_layout->Find("r"));
    EXPECT_EQ(data.offsets.to_node_output_offset,
              physical.Root().output_layout->Find("b"));
  }
  {
    PlannedQuery query =
        Plan("MATCH ()-[r:R]->() WHERE r.value = 10 RETURN id(r) AS id");
    const auto *logical = static_cast<const ir::RelationshipIndexSeekPlan *>(
        FindPlan(query.LogicalPlan(),
                 ir::LogicalPlanNodeType::kRelationshipIndexSeek));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kRelationshipIndexSeek);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::RelationshipIndexSeekOp>(node->data);
    EXPECT_EQ(data.pattern.relationship, "r");
    EXPECT_EQ(data.offsets.relationship_output_offset,
              node->output_layout->Find("r"));
    EXPECT_NE(data.value.Expression(), logical->ValueExpression());
  }
  {
    PlannedQuery query =
        Plan("MATCH ()-[r:R]->() WHERE r.value >= 10 RETURN id(r) AS id");
    const auto *logical =
        static_cast<const ir::RelationshipIndexRangeSeekPlan *>(
            FindPlan(query.LogicalPlan(),
                     ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(),
        runtime::PhysicalOperatorKind::kRelationshipIndexRangeSeek);
    ASSERT_NE(node, nullptr);
    const auto &data =
        std::get<runtime::RelationshipIndexRangeSeekOp>(node->data);
    ASSERT_EQ(data.predicates.size(), logical->Predicates().size());
    EXPECT_EQ(data.offsets.relationship_output_offset,
              node->output_layout->Find("r"));
    EXPECT_NE(data.predicates.front().Expression(),
              logical->Predicates().front());
  }
  {
    PlannedQuery query =
        Plan("MATCH (n) WHERE id(n) IN [0, 1] RETURN id(n) AS id");
    const auto *logical = static_cast<const ir::NodeByIdSeekPlan *>(
        FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kNodeByIdSeek));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kNodeByIdSeek);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::NodeByIdSeekOp>(node->data);
    EXPECT_TRUE(data.many);
    EXPECT_EQ(data.output_offset, node->output_layout->At("n"));
    EXPECT_NE(data.ids.Expression(), logical->Ids());
  }
  {
    PlannedQuery query =
        Plan("MATCH ()-[r]->() WHERE id(r) IN [0, 1] RETURN id(r) AS id");
    const auto *logical = static_cast<const ir::RelationshipByIdSeekPlan *>(
        FindPlan(query.LogicalPlan(),
                 ir::LogicalPlanNodeType::kRelationshipByIdSeek));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kRelationshipByIdSeek);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::RelationshipByIdSeekOp>(node->data);
    EXPECT_TRUE(data.many);
    EXPECT_EQ(data.offsets.relationship_output_offset,
              node->output_layout->Find("r"));
    EXPECT_NE(data.ids.Expression(), logical->Ids());
  }
}

TEST(PhysicalPlanTest, ExecutesLeafAccessAfterLogicalPlanAndAstAreDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
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
                    "MATCH (n) WHERE id(n) IN [1, 1, 99] RETURN id(n) AS id"),
                graph, "id"),
            (std::vector<std::int64_t>{first->id}));
  EXPECT_EQ(FirstColumnIds(DetachedPhysicalPlan(
                               "MATCH ()-[r]->() WHERE id(r) IN [1, 1, 99] "
                               "RETURN id(r) AS id"),
                           graph, "id"),
            (std::vector<std::int64_t>{relationship->id}));
}

TEST(PhysicalPlanTest, BuildsTypedOwnedPayloadsForFixedTraversalOperators) {
  {
    PlannedQuery query =
        Plan("MATCH (a)-[r:R]->(b) RETURN id(a) AS a, id(r) AS r, id(b) AS b");
    const auto *logical = static_cast<const ir::ExpandPlan *>(
        FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kExpand));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kExpand);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::ExpandOp>(node->data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.relationship, "r");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction,
              runtime::PhysicalExpandDirection::kOutgoing);
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
    ExpectSameOffset(data.from_node_input_offset,
                     node->children[0]->output_layout->At("a"));
    ExpectSameOffset(data.relationship_output_offset,
                     node->output_layout->At("r"));
    ExpectSameOffset(data.to_node_output_offset, node->output_layout->At("b"));
  }
  {
    PlannedQuery query =
        Plan("MATCH (a) WITH a, a AS b MATCH (a)-[r:R]->(b) RETURN id(r) AS r");
    const auto *logical = static_cast<const ir::ExpandIntoPlan *>(
        FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kExpandInto));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kExpandInto);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::ExpandIntoOp>(node->data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction,
              runtime::PhysicalExpandDirection::kOutgoing);
    ExpectSameOffset(data.from_node_input_offset,
                     node->children[0]->output_layout->At("a"));
    ExpectSameOffset(data.to_node_input_offset,
                     node->children[0]->output_layout->At("b"));
    ExpectSameOffset(data.relationship_output_offset,
                     node->output_layout->At("r"));
  }
  {
    PlannedQuery query = Plan(
        "MATCH (a) OPTIONAL MATCH (a)-[r:R]->(b) WHERE r.value = 10 "
        "RETURN id(a) AS a, id(r) AS r, id(b) AS b");
    const auto *logical = static_cast<const ir::OptionalExpandPlan *>(FindPlan(
        query.LogicalPlan(), ir::LogicalPlanNodeType::kOptionalExpand));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kOptionalExpand);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::OptionalExpandOp>(node->data);
    ASSERT_EQ(data.predicates.size(), logical->Predicates().size());
    EXPECT_NE(data.predicates.front().Expression(),
              logical->Predicates().front());
    ExpectSameOffset(data.from_node_input_offset,
                     node->children[0]->output_layout->At("a"));
    ExpectSameOffset(data.relationship_output_offset,
                     node->output_layout->At("r"));
    ExpectSameOffset(data.to_node_output_offset, node->output_layout->At("b"));
    EXPECT_EQ(data.output_offsets.size(),
              node->output_layout->Columns().size());
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
    runtime::PhysicalPlan physical = runtime::CreatePhysicalPlan(logical);
    const auto &data =
        std::get<runtime::ProjectEndpointsOp>(physical.Root().data);
    EXPECT_EQ(data.pattern.relationship, "rs");
    EXPECT_EQ(data.pattern.direction, runtime::PhysicalExpandDirection::kBoth);
    EXPECT_TRUE(data.length.variable);
    EXPECT_EQ(data.length.min, 2);
    EXPECT_EQ(data.length.max, 3);
    ExpectSameOffset(data.relationship_input_offset,
                     physical.Root().children[0]->output_layout->At("rs"));
    EXPECT_FALSE(data.from_node_input_offset.has_value());
    EXPECT_FALSE(data.to_node_input_offset.has_value());
    ExpectSameOffset(data.from_node_output_offset,
                     physical.Root().output_layout->At("a"));
    ExpectSameOffset(data.to_node_output_offset,
                     physical.Root().output_layout->At("b"));
  }
}

TEST(PhysicalPlanTest, BuildsTypedOwnedPayloadsForVariableTraversalOperators) {
  {
    PlannedQuery query =
        Plan("MATCH (a)-[rs:R*0..3]->(b) RETURN size(rs) AS hops");
    const auto *logical = static_cast<const ir::VarExpandPlan *>(
        FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kVarExpand));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kVarExpand);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::VarExpandOp>(node->data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.relationship, "rs");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction,
              runtime::PhysicalExpandDirection::kOutgoing);
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
    EXPECT_TRUE(data.length.variable);
    EXPECT_EQ(data.length.min, 0);
    EXPECT_EQ(data.length.max, 3);
    EXPECT_FALSE(data.reverse_relationships);
    ExpectSameOffset(data.from_node_input_offset,
                     node->children[0]->output_layout->At("a"));
    EXPECT_FALSE(data.to_node_input_offset.has_value());
    ExpectSameOffset(data.relationship_output_offset,
                     node->output_layout->At("rs"));
    ExpectSameOffset(data.to_node_output_offset, node->output_layout->At("b"));
  }
  {
    runtime::PhysicalPlan physical = DetachedPruningVarExpand();
    ASSERT_EQ(physical.Root().kind,
              runtime::PhysicalOperatorKind::kPruningVarExpand);
    const auto &data =
        std::get<runtime::PruningVarExpandOp>(physical.Root().data);
    EXPECT_EQ(data.pattern.from_node, "a");
    EXPECT_EQ(data.pattern.to_node, "b");
    EXPECT_EQ(data.pattern.direction,
              runtime::PhysicalExpandDirection::kOutgoing);
    EXPECT_EQ(data.pattern.types, (std::vector<std::string>{"R"}));
    EXPECT_TRUE(data.length.variable);
    EXPECT_EQ(data.length.min, 0);
    EXPECT_EQ(data.length.max, 2);
    ExpectSameOffset(data.from_node_input_offset,
                     physical.Root().children[0]->output_layout->At("a"));
    ExpectSameOffset(data.to_node_output_offset,
                     physical.Root().output_layout->At("b"));
  }
  {
    PlannedQuery query = Plan("MATCH p = (a)-[rs:R*1..2]->(b) RETURN p");
    const auto *logical = static_cast<const ir::PathBuildPlan *>(
        FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kPathBuild));
    ASSERT_NE(logical, nullptr);
    runtime::PhysicalPlan physical =
        runtime::CreatePhysicalPlan(query.LogicalPlan());
    const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
        physical.Root(), runtime::PhysicalOperatorKind::kPathBuild);
    ASSERT_NE(node, nullptr);
    const auto &data = std::get<runtime::PathBuildOp>(node->data);
    EXPECT_EQ(data.path.variable, "p");
    EXPECT_EQ(data.path.nodes, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(data.path.relationships, (std::vector<std::string>{"rs"}));
    ASSERT_EQ(data.node_input_offsets.size(), 2U);
    ASSERT_EQ(data.relationship_input_offsets.size(), 1U);
    ExpectSameOffset(data.node_input_offsets[0],
                     node->children[0]->output_layout->At("a"));
    ExpectSameOffset(data.node_input_offsets[1],
                     node->children[0]->output_layout->At("b"));
    ExpectSameOffset(data.relationship_input_offsets[0],
                     node->children[0]->output_layout->At("rs"));
    ExpectSameOffset(data.path_output_offset, node->output_layout->At("p"));
  }

  {
    ir::VarExpandPlan logical(
        std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"a", "b"}),
        "a", "rs", "b", ir::ExpandDirection::kOutgoing, {"R"},
        {.min = 1, .max = 2}, true);
    runtime::PhysicalPlan physical = runtime::CreatePhysicalPlan(logical);
    const auto &data = std::get<runtime::VarExpandOp>(physical.Root().data);
    ASSERT_TRUE(data.to_node_input_offset.has_value());
    ExpectSameOffset(*data.to_node_input_offset,
                     physical.Root().children[0]->output_layout->At("b"));
    EXPECT_TRUE(data.reverse_relationships);
  }
}

TEST(PhysicalPlanTest,
     ExecutesFixedTraversalAfterLogicalPlanAndAstAreDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
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

  runtime::PhysicalPlan expand_into = DetachedExpandInto();
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

  runtime::PhysicalPlan project_fixed = DetachedProjectEndpoints(false);
  const runtime::QueryParameters fixed_parameters{
      {"relationship", rg::Value(first_relationship)}};
  EXPECT_EQ(FirstColumnIds(project_fixed, graph, "a", fixed_parameters),
            (std::vector<std::int64_t>{first->id}));
  EXPECT_EQ(FirstColumnIds(project_fixed, graph, "b", fixed_parameters),
            (std::vector<std::int64_t>{second->id}));

  runtime::PhysicalPlan project_variable = DetachedProjectEndpoints(true);
  const runtime::QueryParameters variable_parameters{
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
  runtime::test::GraphDBTestDatabase graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});
  graph.CreateRelationship(first, second, "R");
  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a)-[r:R]->(b) RETURN id(r) AS relationship_id");
  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {},
                                        {"relationship_id"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(PhysicalPlanTest,
     ExecutesVariableTraversalAfterLogicalPlanAndAstAreDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});
  auto third = graph.CreateNode({});
  auto first_relationship = graph.CreateRelationship(first, second, "R");
  auto second_relationship = graph.CreateRelationship(second, third, "R");
  graph.CreateRelationship(first, third, "S");

  runtime::PhysicalPlan variable = DetachedPhysicalPlan(
      "MATCH (a)-[rs:R*0..3]->(b) WHERE id(a) = " + std::to_string(first->id) +
      " RETURN size(rs) AS hops");
  EXPECT_EQ(FirstColumnIds(variable, graph, "hops"),
            (std::vector<std::int64_t>{0, 1, 2}));

  runtime::PhysicalPlan pruning = DetachedPruningVarExpand();
  EXPECT_EQ(FirstColumnIds(pruning, graph, "b", {{"from", rg::Value(first)}}),
            (std::vector<std::int64_t>{first->id, second->id, third->id}));

  runtime::PhysicalPlan path =
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
  runtime::test::GraphDBTestDatabase graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});
  graph.CreateRelationship(first, second, "R");
  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a)-[rs:R*1..2]->(b) WHERE id(a) = " + std::to_string(first->id) +
      " RETURN id(b) AS b");

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"b"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  runtime::QueryExecutionOptions options;
  options.memory_limit_bytes = 1;
  cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"b"}, options);
  RG_EXPECT_ERROR((void)cursor->Next(&row),
                  common::ErrorCode::MemoryLimitExceeded);
}

TEST(PhysicalPlanTest, ChoosesSmallerValueHashJoinBuildSide) {
  PlannedQuery query =
      Plan("MATCH (a:Person), (b) WHERE a.id = b.id RETURN a, b");
  const ir::LogicalPlan *logical_join =
      FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kValueHashJoin);
  ASSERT_NE(logical_join, nullptr);
  ASSERT_TRUE(logical_join->Child(0).EstimatedRows().has_value());
  ASSERT_TRUE(logical_join->Child(1).EstimatedRows().has_value());
  ASSERT_LT(*logical_join->Child(0).EstimatedRows(),
            *logical_join->Child(1).EstimatedRows());

  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());
  const runtime::PhysicalPlanNode *join_node = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kValueHashJoin);
  ASSERT_NE(join_node, nullptr);
  const auto &data = std::get<runtime::ValueHashJoinOp>(join_node->data);
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
      product_query.LogicalPlan(), ir::LogicalPlanNodeType::kCartesianProduct);
  ASSERT_NE(logical_product, nullptr);
  runtime::PhysicalPlan product =
      runtime::CreatePhysicalPlan(product_query.LogicalPlan());
  const runtime::PhysicalPlanNode *product_node = FindPhysicalPlan(
      product.Root(), runtime::PhysicalOperatorKind::kCartesianProduct);
  ASSERT_NE(product_node, nullptr);
  const auto &product_data =
      std::get<runtime::CartesianProductOp>(product_node->data);
  EXPECT_EQ(product_data.cached_child, 1U);
  EXPECT_NE(runtime::PhysicalPlanToString(product).find("cache=right"),
            std::string::npos);

  PlannedQuery predicate_query =
      Plan("MATCH (a), (b) WHERE a.age > b.age RETURN a, b");
  const ir::LogicalPlan *logical_predicate = FindPlan(
      predicate_query.LogicalPlan(), ir::LogicalPlanNodeType::kPredicateJoin);
  ASSERT_NE(logical_predicate, nullptr);
  runtime::PhysicalPlan predicate =
      runtime::CreatePhysicalPlan(predicate_query.LogicalPlan());
  const runtime::PhysicalPlanNode *predicate_node = FindPhysicalPlan(
      predicate.Root(), runtime::PhysicalOperatorKind::kPredicateJoin);
  ASSERT_NE(predicate_node, nullptr);
  const auto &predicate_data =
      std::get<runtime::PredicateJoinOp>(predicate_node->data);
  ASSERT_EQ(predicate_data.predicates.size(), 1U);
  EXPECT_EQ(predicate_data.cached_child, 1U);
  EXPECT_NE(predicate_data.predicates.front().Expression(),
            static_cast<const ir::PredicateJoinPlan &>(*logical_predicate)
                .Predicates()
                .front());
  EXPECT_NE(runtime::PhysicalPlanToString(predicate).find("cache=right"),
            std::string::npos);
}

TEST(PhysicalPlanTest, StreamsCartesianProductAfterLogicalPlanIsDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"Left"});
  graph.CreateNode({"Left"});
  graph.CreateNode({"Right"});
  graph.CreateNode({"Right"});
  graph.CreateNode({"Right"});
  runtime::PhysicalPlan physical = DetachedCartesianProduct();
  ASSERT_EQ(physical.Root().kind,
            runtime::PhysicalOperatorKind::kCartesianProduct);

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

  std::unique_ptr<runtime::PhysicalResultCursor> cursor =
      runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"a", "b"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));

  runtime::QueryExecutionOptions memory_options;
  memory_options.memory_limit_bytes = 1;
  cursor = runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"a", "b"},
                                             memory_options);
  RG_EXPECT_ERROR((void)cursor->Next(&row),
                  common::ErrorCode::MemoryLimitExceeded);

  runtime::QueryExecutionOptions cancellation_options;
  cancellation_options.cancellation =
      std::make_shared<runtime::QueryCancellationToken>();
  cursor = runtime::StartGraphDBPhysicalPlan(physical, graph, {}, {"a", "b"},
                                             cancellation_options);
  cancellation_options.cancellation->Cancel();
  RG_EXPECT_ERROR((void)cursor->Next(&row), common::ErrorCode::QueryCancelled);

  runtime::test::GraphDBTestDatabase empty_right;
  empty_right.CreateNode({"Left"});
  EXPECT_TRUE(PhysicalRows(physical, empty_right, {"a", "b"}).empty());
}

TEST(PhysicalPlanTest,
     ExecutesPredicateJoinAfterLogicalPlanAndAstAreDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({}, {{"age", rg::Value(3)}, {"name", rg::Value("old")}});
  graph.CreateNode({}, {{"age", rg::Value(2)}, {"name", rg::Value("middle")}});
  graph.CreateNode({}, {{"age", rg::Value(1)}, {"name", rg::Value("young")}});
  graph.CreateNode({}, {{"name", rg::Value("unknown")}});
  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a), (b) WHERE a.age > b.age "
      "RETURN a.name AS older, b.name AS younger");
  ASSERT_NE(FindPhysicalPlan(physical.Root(),
                             runtime::PhysicalOperatorKind::kPredicateJoin),
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

TEST(PhysicalPlanTest, CartesianProductRejectsConflictingSharedOffsets) {
  runtime::test::GraphDBTestDatabase graph;
  runtime::PhysicalPlan physical = DetachedParameterCartesianProduct();

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

  runtime::PhysicalPlan node_join = runtime::CreatePhysicalPlan(logical);
  const auto &node_data =
      std::get<runtime::NodeHashJoinOp>(node_join.Root().data);
  EXPECT_EQ(node_data.join_keys, (std::vector<std::string>{"n"}));
  EXPECT_EQ(node_data.left_key_offsets,
            std::vector<std::size_t>{
                node_join.Root().children[0]->output_layout->At("n")});
  EXPECT_EQ(node_data.right_key_offsets,
            std::vector<std::size_t>{
                node_join.Root().children[1]->output_layout->At("n")});
  EXPECT_EQ(node_data.build_child, 0U);
  EXPECT_NE(runtime::PhysicalPlanToString(node_join).find("build=left"),
            std::string::npos);

  runtime::PhysicalPlan outer_join = DetachedLeftOuterHashJoin();
  const auto &outer_data =
      std::get<runtime::LeftOuterHashJoinOp>(outer_join.Root().data);
  EXPECT_EQ(outer_data.join_keys, (std::vector<std::string>{"a"}));
  EXPECT_EQ(outer_data.left_key_offsets.size(), 1U);
  EXPECT_EQ(outer_data.right_key_offsets.size(), 1U);
  EXPECT_EQ(outer_data.nullable_output_offsets.size(),
            outer_join.Root().output_layout->CellCount());
}

TEST(PhysicalPlanTest, ExecutesNodeHashJoinAfterLogicalPlanAndAstAreDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
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

  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a:Person)-[r1]->(b)<-[r2]-(c:Person) "
      "RETURN a.name AS left, c.name AS right");
  ASSERT_NE(FindPhysicalPlan(physical.Root(),
                             runtime::PhysicalOperatorKind::kNodeHashJoin),
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

TEST(PhysicalPlanTest, NodeHashJoinRejectsNullKeysAndConflictingSharedOffsets) {
  runtime::test::GraphDBTestDatabase graph;
  const auto node = graph.CreateNode({});
  runtime::PhysicalPlan physical = DetachedParameterNodeHashJoin();
  ASSERT_EQ(physical.Root().kind, runtime::PhysicalOperatorKind::kNodeHashJoin);

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
  runtime::test::GraphDBTestDatabase graph;
  const auto matched = graph.CreateNode({"Input"});
  const auto unmatched = graph.CreateNode({"Input"});
  const auto first = graph.CreateNode({});
  const auto second = graph.CreateNode({});
  graph.CreateRelationship(matched, first, "R");
  graph.CreateRelationship(matched, second, "R");

  runtime::PhysicalPlan physical = DetachedLeftOuterHashJoin();
  ASSERT_EQ(physical.Root().kind,
            runtime::PhysicalOperatorKind::kLeftOuterHashJoin);
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
  runtime::test::GraphDBTestDatabase graph;
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

  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "MATCH (a:Small), (b:Large) "
      "WHERE a.first = b.first AND a.second = b.second "
      "RETURN a.name AS left, b.name AS right");
  ASSERT_NE(FindPhysicalPlan(physical.Root(),
                             runtime::PhysicalOperatorKind::kValueHashJoin),
            nullptr);
  EXPECT_EQ(PhysicalRows(physical, graph, {"left", "right"}),
            (std::vector<std::vector<rg::Value>>{
                {rg::Value("match"), rg::Value("first")}}));
}

TEST(PhysicalPlanTest, SelectsTopNForRewrittenLogicalPlan) {
  PlannedQuery query =
      Plan("UNWIND [3, 1, 2] AS x RETURN x ORDER BY x LIMIT $l");
  const ir::LogicalPlan *top_n =
      FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kTopN);
  ASSERT_NE(top_n, nullptr);
  EXPECT_EQ(FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kLimit),
            nullptr);
  EXPECT_EQ(FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kSort),
            nullptr);

  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());
  const runtime::PhysicalPlanNode *top_n_node =
      FindPhysicalPlan(physical.Root(), runtime::PhysicalOperatorKind::kTopN);
  ASSERT_NE(top_n_node, nullptr);

  const auto &data = std::get<runtime::TopNOp>(top_n_node->data);
  ASSERT_EQ(data.items.size(), 1U);
  EXPECT_NE(
      data.items.front().expression.Expression(),
      static_cast<const ir::TopNPlan &>(*top_n).Items().front().expression);
  EXPECT_EQ(data.items.front().direction,
            runtime::PhysicalSortDirection::kAscending);
  EXPECT_NE(data.limit.Expression(),
            static_cast<const ir::TopNPlan &>(*top_n).Limit());
  ASSERT_EQ(top_n_node->provided_order.size(), 1U);
  EXPECT_NE(
      top_n_node->provided_order.front().expression.get(),
      static_cast<const ir::TopNPlan &>(*top_n).Items().front().expression);
  EXPECT_EQ(top_n_node->provided_order.front().direction,
            runtime::PhysicalSortDirection::kAscending);
  ASSERT_EQ(top_n_node->children.size(), 1U);
}

TEST(PhysicalPlanTest, DoesNotRewriteTopNAcrossSkipOrWrites) {
  PlannedQuery skipped =
      Plan("UNWIND [3, 1, 2] AS x RETURN x ORDER BY x SKIP 1 LIMIT 1");
  runtime::PhysicalPlan skipped_physical =
      runtime::CreatePhysicalPlan(skipped.LogicalPlan());
  const runtime::PhysicalPlanNode *skipped_limit_node = FindPhysicalPlan(
      skipped_physical.Root(), runtime::PhysicalOperatorKind::kLimit);
  ASSERT_NE(skipped_limit_node, nullptr);
  EXPECT_FALSE(skipped_physical.Effects().writes);
  EXPECT_FALSE(
      std::get<runtime::LimitOp>(skipped_limit_node->data).exhaust_child);

  PlannedQuery write =
      Plan("MATCH (n) SET n.x = 1 RETURN n ORDER BY n LIMIT 2");
  runtime::PhysicalPlan write_physical =
      runtime::CreatePhysicalPlan(write.LogicalPlan());
  const runtime::PhysicalPlanNode *write_limit_node = FindPhysicalPlan(
      write_physical.Root(), runtime::PhysicalOperatorKind::kLimit);
  ASSERT_NE(write_limit_node, nullptr);
  EXPECT_TRUE(write_physical.Effects().writes);
  EXPECT_TRUE(write_physical.Effects().contains_write_barrier);
  EXPECT_TRUE(std::get<runtime::LimitOp>(write_limit_node->data).exhaust_child);
}

TEST(PhysicalPlanTest, SelectsOrderedGroupingAlgorithms) {
  PlannedQuery distinct_query = Plan(
      "UNWIND [3, 1, 2, 1] AS x "
      "WITH x ORDER BY x RETURN DISTINCT x");
  const ir::LogicalPlan *distinct = FindPlan(
      distinct_query.LogicalPlan(), ir::LogicalPlanNodeType::kDistinct);
  ASSERT_NE(distinct, nullptr);
  runtime::PhysicalPlan distinct_physical =
      runtime::CreatePhysicalPlan(distinct_query.LogicalPlan());
  const runtime::PhysicalPlanNode *distinct_node =
      FindPhysicalPlan(distinct_physical.Root(),
                       runtime::PhysicalOperatorKind::kOrderedDistinct);
  ASSERT_NE(distinct_node, nullptr);
  ASSERT_EQ(distinct_node->provided_order.size(), 1U);
  const auto &distinct_data =
      std::get<runtime::OrderedDistinctOp>(distinct_node->data);
  ASSERT_EQ(distinct_data.grouping_items.size(), 1U);
  EXPECT_EQ(distinct_data.grouping_items.front().alias, "x");
  EXPECT_EQ(distinct_data.grouping_items.front().output_offset,
            distinct_node->output_layout->At("x"));
  EXPECT_NE(distinct_data.grouping_items.front().expression.Expression(),
            static_cast<const ir::DistinctPlan &>(*distinct)
                .GroupingItems()
                .front()
                .expression);

  PlannedQuery aggregation_query = Plan(
      "UNWIND [3, 1, 2, 1] AS x "
      "WITH x ORDER BY x RETURN x, count(*) AS count");
  const ir::LogicalPlan *aggregation = FindPlan(
      aggregation_query.LogicalPlan(), ir::LogicalPlanNodeType::kAggregation);
  ASSERT_NE(aggregation, nullptr);
  runtime::PhysicalPlan aggregation_physical =
      runtime::CreatePhysicalPlan(aggregation_query.LogicalPlan());
  const runtime::PhysicalPlanNode *aggregation_node =
      FindPhysicalPlan(aggregation_physical.Root(),
                       runtime::PhysicalOperatorKind::kOrderedAggregation);
  ASSERT_NE(aggregation_node, nullptr);
  ASSERT_EQ(aggregation_node->provided_order.size(), 1U);
  const auto &aggregation_data =
      std::get<runtime::OrderedAggregationOp>(aggregation_node->data);
  ASSERT_EQ(aggregation_data.grouping_items.size(), 1U);
  ASSERT_EQ(aggregation_data.aggregation_items.size(), 1U);
  EXPECT_EQ(aggregation_data.grouping_items.front().alias, "x");
  EXPECT_EQ(aggregation_data.aggregation_items.front().alias, "count");
  EXPECT_EQ(aggregation_data.grouping_items.front().output_offset,
            aggregation_node->output_layout->At("x"));
  EXPECT_EQ(aggregation_data.aggregation_items.front().output_offset,
            aggregation_node->output_layout->At("count"));
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
      FindPlan(partial_query.LogicalPlan(), ir::LogicalPlanNodeType::kSort);
  ASSERT_NE(outer_sort, nullptr);
  runtime::PhysicalPlan partial_physical =
      runtime::CreatePhysicalPlan(partial_query.LogicalPlan());
  const runtime::PhysicalPlanNode *partial_node = FindPhysicalPlan(
      partial_physical.Root(), runtime::PhysicalOperatorKind::kPartialSort);
  ASSERT_NE(partial_node, nullptr);
  const auto &partial_data =
      std::get<runtime::PartialSortOp>(partial_node->data);
  EXPECT_EQ(partial_data.prefix, 1U);
  ASSERT_EQ(partial_data.items.size(), 2U);
  EXPECT_NE(partial_data.items.front().expression.Expression(),
            static_cast<const ir::SortPlan &>(*outer_sort)
                .Items()
                .front()
                .expression);
  ASSERT_EQ(partial_node->provided_order.size(), 2U);
  EXPECT_NE(partial_node->provided_order.front().expression.get(),
            static_cast<const ir::SortPlan &>(*outer_sort)
                .Items()
                .front()
                .expression);
  EXPECT_EQ(partial_node->provided_order.front().direction,
            runtime::PhysicalSortDirection::kAscending);

  PlannedQuery fallback_query = Plan("UNWIND [3, 1, 2] AS x RETURN DISTINCT x");
  const ir::LogicalPlan *distinct = FindPlan(
      fallback_query.LogicalPlan(), ir::LogicalPlanNodeType::kDistinct);
  ASSERT_NE(distinct, nullptr);
  runtime::PhysicalPlan fallback_physical =
      runtime::CreatePhysicalPlan(fallback_query.LogicalPlan());
  const runtime::PhysicalPlanNode *distinct_node = FindPhysicalPlan(
      fallback_physical.Root(), runtime::PhysicalOperatorKind::kHashDistinct);
  ASSERT_NE(distinct_node, nullptr);
  const auto &distinct_data =
      std::get<runtime::HashDistinctOp>(distinct_node->data);
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
      aggregation_query.LogicalPlan(), ir::LogicalPlanNodeType::kAggregation);
  ASSERT_NE(aggregation, nullptr);
  runtime::PhysicalPlan aggregation_physical =
      runtime::CreatePhysicalPlan(aggregation_query.LogicalPlan());
  const runtime::PhysicalPlanNode *aggregation_node =
      FindPhysicalPlan(aggregation_physical.Root(),
                       runtime::PhysicalOperatorKind::kHashAggregation);
  ASSERT_NE(aggregation_node, nullptr);
  const auto &aggregation_data =
      std::get<runtime::HashAggregationOp>(aggregation_node->data);
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
      FindPlan(sort_query.LogicalPlan(), ir::LogicalPlanNodeType::kSort);
  ASSERT_NE(sort, nullptr);
  runtime::PhysicalPlan sort_physical =
      runtime::CreatePhysicalPlan(sort_query.LogicalPlan());
  const runtime::PhysicalPlanNode *sort_node = FindPhysicalPlan(
      sort_physical.Root(), runtime::PhysicalOperatorKind::kFullSort);
  ASSERT_NE(sort_node, nullptr);
  const auto &sort_data = std::get<runtime::FullSortOp>(sort_node->data);
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
      FindPlan(query.LogicalPlan(), ir::LogicalPlanNodeType::kTopN);
  ASSERT_NE(top_n, nullptr);

  runtime::PhysicalPlan physical =
      runtime::CreatePhysicalPlan(query.LogicalPlan());
  const runtime::PhysicalPlanNode *node = FindPhysicalPlan(
      physical.Root(), runtime::PhysicalOperatorKind::kPartialTopN);
  ASSERT_NE(node, nullptr);

  const auto &data = std::get<runtime::PartialTopNOp>(node->data);
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
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"N"}, {{"g", rg::Value(2)}, {"v", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"g", rg::Value(1)}, {"v", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"g", rg::Value(2)}, {"v", rg::Value(1)}});
  graph.CreateNode({"N"}, {{"g", rg::Value(1)}, {"v", rg::Value(1)}});

  runtime::PhysicalPlan full_sort =
      DetachedPhysicalPlan("MATCH (n:N) RETURN n.v AS v ORDER BY v");
  ASSERT_NE(FindPhysicalPlan(full_sort.Root(),
                             runtime::PhysicalOperatorKind::kFullSort),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(full_sort, graph, {"v"}),
      (std::vector<std::vector<rg::Value>>{
          {rg::Value(1)}, {rg::Value(1)}, {rg::Value(2)}, {rg::Value(2)}}));

  runtime::PhysicalPlan top_n = DetachedPhysicalPlan(
      "MATCH (n:N) RETURN n.v AS v ORDER BY v DESC LIMIT $l");
  ASSERT_NE(
      FindPhysicalPlan(top_n.Root(), runtime::PhysicalOperatorKind::kTopN),
      nullptr);
  EXPECT_EQ(
      PhysicalRows(top_n, graph, {"v"}, {{"l", rg::Value(2)}}),
      (std::vector<std::vector<rg::Value>>{{rg::Value(2)}, {rg::Value(2)}}));

  const std::string partial_query =
      "MATCH (n:N) WITH n ORDER BY n.g "
      "RETURN n.g AS g, n.v AS v ORDER BY g, v";
  runtime::PhysicalPlan partial_sort = DetachedPhysicalPlan(partial_query);
  ASSERT_NE(FindPhysicalPlan(partial_sort.Root(),
                             runtime::PhysicalOperatorKind::kPartialSort),
            nullptr);
  const std::vector<std::vector<rg::Value>> sorted{
      {rg::Value(1), rg::Value(1)},
      {rg::Value(1), rg::Value(2)},
      {rg::Value(2), rg::Value(1)},
      {rg::Value(2), rg::Value(2)}};
  EXPECT_EQ(PhysicalRows(partial_sort, graph, {"g", "v"}), sorted);

  runtime::PhysicalPlan partial_top_n =
      DetachedPhysicalPlan(partial_query + " LIMIT $l");
  ASSERT_NE(FindPhysicalPlan(partial_top_n.Root(),
                             runtime::PhysicalOperatorKind::kPartialTopN),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(partial_top_n, graph, {"g", "v"}, {{"l", rg::Value(3)}}),
      (std::vector<std::vector<rg::Value>>(sorted.begin(),
                                           sorted.begin() + 3)));
}

TEST(PhysicalPlanTest,
     ExecutesDistinctOperatorsAfterLogicalPlanAndAstAreDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"N"}, {{"value", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(1)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(2)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(1.0)}});
  graph.CreateNode({"N"});
  graph.CreateNode({"N"});

  runtime::PhysicalPlan hash_distinct =
      DetachedPhysicalPlan("MATCH (n:N) RETURN DISTINCT n.value AS value");
  ASSERT_NE(FindPhysicalPlan(hash_distinct.Root(),
                             runtime::PhysicalOperatorKind::kHashDistinct),
            nullptr);
  const auto hash_rows = PhysicalRows(hash_distinct, graph, {"value"});
  ASSERT_EQ(hash_rows.size(), 3U);
  EXPECT_TRUE(rg::ValuesEqual(hash_rows[0][0], rg::Value(2)));
  EXPECT_TRUE(rg::ValuesEqual(hash_rows[1][0], rg::Value(1)));
  EXPECT_TRUE(hash_rows[2][0].IsNull());

  runtime::PhysicalPlan ordered_distinct = DetachedPhysicalPlan(
      "MATCH (n:N) WITH n ORDER BY n.value "
      "RETURN DISTINCT n.value AS value");
  ASSERT_NE(FindPhysicalPlan(ordered_distinct.Root(),
                             runtime::PhysicalOperatorKind::kOrderedDistinct),
            nullptr);
  const auto ordered_rows = PhysicalRows(ordered_distinct, graph, {"value"});
  ASSERT_EQ(ordered_rows.size(), 3U);
  EXPECT_TRUE(rg::ValuesEqual(ordered_rows[0][0], rg::Value(1)));
  EXPECT_TRUE(rg::ValuesEqual(ordered_rows[1][0], rg::Value(2)));
  EXPECT_TRUE(ordered_rows[2][0].IsNull());
}

TEST(PhysicalPlanTest,
     ExecutesAggregationOperatorsAfterLogicalPlanAndAstAreDestroyed) {
  runtime::test::GraphDBTestDatabase graph;
  graph.CreateNode({"N"}, {{"group", rg::Value(2)}, {"value", rg::Value(20)}});
  graph.CreateNode({"N"}, {{"group", rg::Value(1)}, {"value", rg::Value(10)}});
  graph.CreateNode({"N"},
                   {{"group", rg::Value(2)}, {"value", rg::Value(20.0)}});
  graph.CreateNode({"N"}, {{"group", rg::Value(1)}, {"value", rg::Value(5)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(7)}});

  const std::string aggregation =
      "RETURN n.group AS group, count(*) AS count, "
      "sum(n.value) AS total, count(DISTINCT n.value) AS unique_count";
  runtime::PhysicalPlan hash_aggregation =
      DetachedPhysicalPlan("MATCH (n:N) " + aggregation);
  ASSERT_NE(FindPhysicalPlan(hash_aggregation.Root(),
                             runtime::PhysicalOperatorKind::kHashAggregation),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(hash_aggregation, graph,
                   {"group", "count", "total", "unique_count"}),
      (std::vector<std::vector<rg::Value>>{
          {rg::Value(2), rg::Value(2), rg::Value(40.0), rg::Value(1)},
          {rg::Value(1), rg::Value(2), rg::Value(15), rg::Value(2)},
          {rg::Value::Null(), rg::Value(1), rg::Value(7), rg::Value(1)}}));

  runtime::PhysicalPlan ordered_aggregation = DetachedPhysicalPlan(
      "MATCH (n:N) WITH n ORDER BY n.group " + aggregation);
  ASSERT_NE(
      FindPhysicalPlan(ordered_aggregation.Root(),
                       runtime::PhysicalOperatorKind::kOrderedAggregation),
      nullptr);
  EXPECT_EQ(
      PhysicalRows(ordered_aggregation, graph,
                   {"group", "count", "total", "unique_count"}),
      (std::vector<std::vector<rg::Value>>{
          {rg::Value(1), rg::Value(2), rg::Value(15), rg::Value(2)},
          {rg::Value(2), rg::Value(2), rg::Value(40.0), rg::Value(1)},
          {rg::Value::Null(), rg::Value(1), rg::Value(7), rg::Value(1)}}));

  runtime::PhysicalPlan empty_aggregation = DetachedPhysicalPlan(
      "MATCH (n:Missing) RETURN count(*) AS count, sum(n.value) AS total");
  ASSERT_NE(FindPhysicalPlan(empty_aggregation.Root(),
                             runtime::PhysicalOperatorKind::kHashAggregation),
            nullptr);
  EXPECT_EQ(
      PhysicalRows(empty_aggregation, graph, {"count", "total"}),
      (std::vector<std::vector<rg::Value>>{{rg::Value(0), rg::Value(0)}}));
}

TEST(PhysicalPlanTest, PrintsOwnedOrderingAfterLogicalPlanAndAstAreDestroyed) {
  runtime::PhysicalPlan physical = DetachedPhysicalPlan(
      "UNWIND [{a:1,b:2},{a:1,b:1}] AS x "
      "WITH x ORDER BY x.a "
      "RETURN x.a AS a, x.b AS b ORDER BY a, b");

  const std::string printed = runtime::PhysicalPlanToString(physical);

  EXPECT_NE(printed.find("PartialSort"), std::string::npos);
  EXPECT_NE(printed.find("logical=Sort"), std::string::npos);
  EXPECT_EQ(printed.find("exec="), std::string::npos);
  EXPECT_NE(printed.find("order=[a ASC, b ASC]"), std::string::npos);
  EXPECT_NE(printed.find("prefix=1"), std::string::npos);
  EXPECT_NE(printed.find("layout=[a@0"), std::string::npos);
}
