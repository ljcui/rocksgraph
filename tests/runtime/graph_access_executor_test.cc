#include "runtime/graph_access_executor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "runtime/query_row_util.h"
#include "storage/in_memory_graph.h"
#include "value/value.h"

namespace {

std::unique_ptr<ast::Variable> Variable(std::string name) {
  auto variable = std::make_unique<ast::Variable>();
  variable->name = std::move(name);
  return variable;
}

std::unique_ptr<ast::PropertyExpression> Property(std::string variable,
                                                  std::string property_key) {
  auto property = std::make_unique<ast::PropertyExpression>();
  property->object = Variable(std::move(variable));
  property->property_key = std::move(property_key);
  return property;
}

std::unique_ptr<ast::ComparisonExpression> GreaterThan(std::string variable,
                                                       std::string property_key,
                                                       std::int64_t value) {
  auto literal = std::make_unique<ast::IntegerLiteral>();
  literal->value = value;
  auto comparison = std::make_unique<ast::ComparisonExpression>();
  comparison->left = Property(std::move(variable), std::move(property_key));
  comparison->op = ">";
  comparison->right = std::move(literal);
  return comparison;
}

const rg::Value &Column(const rg::QueryRow &row, const std::string &name) {
  return row.at(name);
}

std::vector<std::int64_t> NodeIds(const rg::QueryRows &rows,
                                  const std::string &column) {
  std::vector<std::int64_t> ids;
  ids.reserve(rows.size());
  for (const auto &row : rows) {
    ids.push_back(Column(row, column).AsNode().id);
  }
  return ids;
}

}  // namespace

TEST(QueryRowUtilTest, BindsAndMergesUsingQueryValueSemantics) {
  rg::QueryRow row{{"value", rg::Value(1)}};

  EXPECT_TRUE(rg::TryBindQueryVariable(&row, "value", rg::Value(1.0)));
  EXPECT_FALSE(rg::TryBindQueryVariable(&row, "value", rg::Value(2)));
  EXPECT_EQ(rg::LookupQueryVariable(row, "value"), rg::Value(1));

  rg::QueryRow merged;
  EXPECT_TRUE(rg::MergeQueryRows(row, {{"other", rg::Value("ok")}}, &merged));
  EXPECT_EQ(Column(merged, "other"), rg::Value("ok"));
  EXPECT_FALSE(rg::MergeQueryRows(row, {{"value", rg::Value(2)}}, &merged));
}

TEST(GraphAccessExecutorTest, ExecutesNodeScansAndIndexSeeks) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({"Person"}, {{"score", rg::Value(1)}});
  auto second = graph.CreateNode({"Person"}, {{"score", rg::Value(2)}});
  graph.CreateNode({"Other"}, {{"score", rg::Value(3)}});
  graph.AddNodeIndex({"Person"}, "score");
  rg::GraphAccessExecutor executor(graph);

  ir::AllNodeScanPlan all_nodes("n");
  const rg::QueryRows scanned =
      executor.ExecuteAllNodeScan(all_nodes, {{{"seed", rg::Value(7)}}});
  ASSERT_EQ(scanned.size(), 3U);
  EXPECT_EQ(Column(scanned.front(), "seed"), rg::Value(7));

  ir::NodeByLabelScanPlan people("n", "Person");
  const rg::QueryRows labeled =
      executor.ExecuteNodeByLabelScan(people, {rg::QueryRow{}});
  EXPECT_EQ(NodeIds(labeled, "n"),
            (std::vector<std::int64_t>{first->id, second->id}));

  ast::DoubleLiteral expected;
  expected.value = 1.0;
  ir::NodeIndexSeekPlan seek("n", {"Person"}, "score", &expected);
  const rg::QueryRows sought =
      executor.ExecuteNodeIndexSeek(seek, {rg::QueryRow{}});
  ASSERT_EQ(sought.size(), 1U);
  EXPECT_EQ(Column(sought[0], "n").AsNode().id, first->id);

  auto predicate = GreaterThan("n", "score", 1);
  ir::NodeIndexRangeSeekPlan range("n", {"Person"}, "score", {predicate.get()});
  const rg::QueryRows ranged =
      executor.ExecuteNodeIndexRangeSeek(range, {rg::QueryRow{}});
  ASSERT_EQ(ranged.size(), 1U);
  EXPECT_EQ(Column(ranged[0], "n").AsNode().id, second->id);
}

TEST(GraphAccessExecutorTest, HonorsRelationshipDirectionAndEmitsSelfLoopOnce) {
  rg::InMemoryGraph graph;
  auto left = graph.CreateNode({"Node"});
  auto right = graph.CreateNode({"Node"});
  auto edge = graph.CreateRelationship(left, right, "LINK");
  auto loop = graph.CreateRelationship(left, left, "LINK");
  rg::GraphAccessExecutor executor(graph);

  ir::RelationshipTypeScanPlan both("from", "r", "to",
                                    ir::ExpandDirection::kBoth, {"LINK"});
  const rg::QueryRows rows =
      executor.ExecuteRelationshipTypeScan(both, {rg::QueryRow{}});
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(Column(rows[0], "r").AsRelationship().id, edge->id);
  EXPECT_EQ(Column(rows[0], "from").AsNode().id, left->id);
  EXPECT_EQ(Column(rows[0], "to").AsNode().id, right->id);
  EXPECT_EQ(Column(rows[1], "from").AsNode().id, right->id);
  EXPECT_EQ(Column(rows[1], "to").AsNode().id, left->id);
  EXPECT_EQ(Column(rows[2], "r").AsRelationship().id, loop->id);

  ir::RelationshipTypeScanPlan incoming(
      "from", "r", "to", ir::ExpandDirection::kIncoming, {"LINK"});
  const rg::QueryRows incoming_rows =
      executor.ExecuteRelationshipTypeScan(incoming, {rg::QueryRow{}});
  ASSERT_EQ(incoming_rows.size(), 2U);
  EXPECT_EQ(Column(incoming_rows[0], "from").AsNode().id, right->id);
  EXPECT_EQ(Column(incoming_rows[0], "to").AsNode().id, left->id);
}

TEST(GraphAccessExecutorTest, ExpandsOnlyBetweenBoundEndpoints) {
  rg::InMemoryGraph graph;
  auto left = graph.CreateNode({"Node"});
  auto right = graph.CreateNode({"Node"});
  auto other = graph.CreateNode({"Node"});
  auto edge = graph.CreateRelationship(left, right, "LINK");
  graph.CreateRelationship(left, other, "LINK");
  rg::GraphAccessExecutor executor(graph);

  ir::ExpandPlan expand(std::make_unique<ir::ArgumentPlan>(
                            std::vector<std::string>{"from", "to"}),
                        "from", "r", "to", ir::ExpandDirection::kOutgoing,
                        {"LINK"});
  const rg::QueryRows expanded = executor.ExecuteExpand(
      expand, {{{"from", rg::Value(left)}, {"to", rg::Value(right)}}});
  ASSERT_EQ(expanded.size(), 1U);
  EXPECT_EQ(Column(expanded[0], "r").AsRelationship().id, edge->id);

  ir::ExpandIntoPlan reverse(std::make_unique<ir::ArgumentPlan>(
                                 std::vector<std::string>{"from", "to"}),
                             "from", "r", "to", ir::ExpandDirection::kIncoming,
                             {"LINK"});
  EXPECT_TRUE(executor
                  .ExecuteExpandInto(reverse, {{{"from", rg::Value(left)},
                                                {"to", rg::Value(right)}}})
                  .empty());
}

TEST(GraphAccessExecutorTest, HandlesZeroLengthAndDoesNotReuseRelationships) {
  rg::InMemoryGraph graph;
  auto left = graph.CreateNode({"Node"});
  auto right = graph.CreateNode({"Node"});
  graph.CreateRelationship(left, right, "LINK");
  rg::GraphAccessExecutor executor(graph);

  ir::VarExpandPlan zero(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"from"}),
      "from", "rs", "to", ir::ExpandDirection::kBoth, {"LINK"},
      {.min = 0, .max = 0});
  const rg::QueryRows zero_rows =
      executor.ExecuteVarExpand(zero, {{{"from", rg::Value(left)}}});
  ASSERT_EQ(zero_rows.size(), 1U);
  EXPECT_EQ(Column(zero_rows[0], "to").AsNode().id, left->id);
  EXPECT_TRUE(Column(zero_rows[0], "rs").AsList().empty());

  ir::VarExpandPlan two_hops(
      std::make_unique<ir::ArgumentPlan>(std::vector<std::string>{"from"}),
      "from", "rs", "to", ir::ExpandDirection::kBoth, {"LINK"},
      {.min = 2, .max = 2});
  EXPECT_TRUE(executor.ExecuteVarExpand(two_hops, {{{"from", rg::Value(left)}}})
                  .empty());
}

TEST(GraphAccessExecutorTest, BuildsPathFromReversedRelationshipList) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({"Node"});
  auto second = graph.CreateNode({"Node"});
  auto third = graph.CreateNode({"Node"});
  auto first_edge = graph.CreateRelationship(first, second, "LINK");
  auto second_edge = graph.CreateRelationship(second, third, "LINK");
  rg::GraphAccessExecutor executor(graph);

  ir::PathBuildPlan plan(
      std::make_unique<ir::ArgumentPlan>(
          std::vector<std::string>{"first", "edges", "third"}),
      {.variable = "path",
       .nodes = {"first", "third"},
       .relationships = {"edges"}});
  const rg::QueryRows rows = executor.ExecutePathBuild(
      plan, {{{"first", rg::Value(first)},
              {"edges", rg::Value(rg::Value::List{rg::Value(second_edge),
                                                  rg::Value(first_edge)})},
              {"third", rg::Value(third)}}});

  ASSERT_EQ(rows.size(), 1U);
  const rg::Path &path = Column(rows[0], "path").AsPath();
  ASSERT_EQ(path.nodes.size(), 3U);
  ASSERT_EQ(path.relationships.size(), 2U);
  EXPECT_EQ(path.nodes[0]->id, first->id);
  EXPECT_EQ(path.nodes[1]->id, second->id);
  EXPECT_EQ(path.nodes[2]->id, third->id);
  EXPECT_EQ(path.relationships[0]->id, first_edge->id);
  EXPECT_EQ(path.relationships[1]->id, second_edge->id);
}
