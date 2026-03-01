#include "ir/idp_planner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/exception.h"

namespace {

bool Contains(const std::unordered_set<std::string> &set,
              const std::string &value) {
  return set.find(value) != set.end();
}

std::size_t CountNodeType(const std::vector<const ir::LogicalPlan *> &plans,
                          ir::LogicalPlanNodeType type) {
  return static_cast<std::size_t>(std::count_if(
      plans.begin(), plans.end(),
      [type](const ir::LogicalPlan *plan) { return plan->node_type == type; }));
}

ir::QueryGraph::Relationship MakeRelationship(
    std::string name, std::string left, std::string right,
    ir::QueryGraph::Direction direction =
        ir::QueryGraph::Direction::kOutgoing) {
  ir::QueryGraph::Relationship relationship;
  relationship.name = std::move(name);
  relationship.left_node = std::move(left);
  relationship.right_node = std::move(right);
  relationship.direction = direction;
  return relationship;
}

TEST(IDPPlannerTest, PlansConnectedRelationshipsWithExpandChain) {
  ir::QueryGraph query_graph;
  query_graph.nodes = {"a", "b", "c"};
  query_graph.relationships.push_back(MakeRelationship("r1", "a", "b"));
  query_graph.relationships.push_back(MakeRelationship("r2", "b", "c"));

  auto plan = ir::BuildIDPLogicalPlan(query_graph);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kExpand), 2U);
  EXPECT_EQ(
      CountNodeType(flattened, ir::LogicalPlanNodeType::kCartesianProduct), 0U);

  const auto symbols = plan->AvailableSymbols();
  EXPECT_TRUE(Contains(symbols, "a"));
  EXPECT_TRUE(Contains(symbols, "b"));
  EXPECT_TRUE(Contains(symbols, "c"));
  EXPECT_TRUE(Contains(symbols, "r1"));
  EXPECT_TRUE(Contains(symbols, "r2"));
}

TEST(IDPPlannerTest, UsesCartesianProductForDisconnectedRelationships) {
  ir::QueryGraph query_graph;
  query_graph.nodes = {"a", "b", "c", "d"};
  query_graph.relationships.push_back(MakeRelationship("r1", "a", "b"));
  query_graph.relationships.push_back(MakeRelationship("r2", "c", "d"));

  auto plan = ir::BuildIDPLogicalPlan(query_graph);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kExpand), 2U);
  EXPECT_EQ(
      CountNodeType(flattened, ir::LogicalPlanNodeType::kCartesianProduct), 1U);
}

TEST(IDPPlannerTest, AppendsWhereSelectionWhenDependenciesAreAvailable) {
  ast::BooleanLiteral always_true;
  always_true.value = true;

  ir::QueryGraph query_graph;
  query_graph.nodes = {"n"};
  query_graph.where.push_back(
      ir::QueryGraph::WherePredicate{&always_true, {"n"}});

  auto plan = ir::BuildIDPLogicalPlan(query_graph);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  ASSERT_EQ(flattened.size(), 2U);
  EXPECT_EQ(flattened[0]->node_type, ir::LogicalPlanNodeType::kSelection);
  EXPECT_EQ(flattened[1]->node_type, ir::LogicalPlanNodeType::kAllNodesScan);
}

TEST(IDPPlannerTest, UsesArgumentSymbolsAsSeedInsteadOfRescanningNode) {
  ir::QueryGraph query_graph;
  query_graph.nodes = {"n", "m"};
  query_graph.relationships.push_back(MakeRelationship("r", "n", "m"));

  auto plan = ir::BuildIDPLogicalPlan(query_graph, {"n"});
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kExpand), 1U);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kAllNodesScan),
            0U);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kArgument), 1U);
}

TEST(IDPPlannerTest, ChoosesNodeHashJoinForArgumentAnchoredBranches) {
  ir::QueryGraph query_graph;
  query_graph.nodes = {"n", "a", "b"};
  query_graph.relationships.push_back(MakeRelationship("r1", "n", "a"));
  query_graph.relationships.push_back(MakeRelationship("r2", "n", "b"));

  auto plan = ir::BuildIDPLogicalPlan(query_graph, {"n"});
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kNodeHashJoin),
            1U);
}

TEST(IDPPlannerTest, PushesDownWherePredicatesToChildrenWhenPossible) {
  ast::BooleanLiteral left_predicate;
  left_predicate.value = true;
  ast::BooleanLiteral right_predicate;
  right_predicate.value = true;
  ast::BooleanLiteral cross_predicate;
  cross_predicate.value = true;

  ir::QueryGraph query_graph;
  query_graph.nodes = {"a", "b", "c", "d"};
  query_graph.relationships.push_back(MakeRelationship("r1", "a", "b"));
  query_graph.relationships.push_back(MakeRelationship("r2", "c", "d"));
  query_graph.where.push_back(
      ir::QueryGraph::WherePredicate{&left_predicate, {"a"}});
  query_graph.where.push_back(
      ir::QueryGraph::WherePredicate{&right_predicate, {"c"}});
  query_graph.where.push_back(
      ir::QueryGraph::WherePredicate{&cross_predicate, {"a", "c"}});

  auto plan = ir::BuildIDPLogicalPlan(query_graph);
  ASSERT_TRUE(plan);

  const auto flattened = ir::FlattenLogicalPlan(*plan);
  EXPECT_EQ(CountNodeType(flattened, ir::LogicalPlanNodeType::kSelection), 3U);
}

TEST(IDPPlannerTest, RejectsWherePredicateWithMissingDependency) {
  ast::BooleanLiteral always_true;
  always_true.value = true;

  ir::QueryGraph query_graph;
  query_graph.nodes = {"n"};
  query_graph.where.push_back(
      ir::QueryGraph::WherePredicate{&always_true, {"m"}});

  EXPECT_THROW((void)ir::BuildIDPLogicalPlan(query_graph),
               common::InvalidArgumentError);
}

TEST(IDPPlannerTest, RejectsTooManyRelationshipsForIDPTable) {
  ir::QueryGraph query_graph;
  constexpr std::size_t kRelationshipCount = 21;
  for (std::size_t i = 0; i <= kRelationshipCount; ++i) {
    query_graph.nodes.insert("n" + std::to_string(i));
  }
  for (std::size_t i = 0; i < kRelationshipCount; ++i) {
    query_graph.relationships.push_back(
        MakeRelationship("r" + std::to_string(i), "n" + std::to_string(i),
                         "n" + std::to_string(i + 1)));
  }

  EXPECT_THROW((void)ir::BuildIDPLogicalPlan(query_graph),
               common::InvalidArgumentError);
}

}  // namespace
