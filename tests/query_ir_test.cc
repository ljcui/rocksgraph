#include "ir/query_ir.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "common/exception.h"

namespace {

std::unique_ptr<ast::Statement> ParseOrFail(const std::string &query) {
  try {
    return ast::parseCypherAndRewrite(query);
  } catch (const ast::ParseError &e) {
    ADD_FAILURE() << "parse errors for query: " << query
                  << " message: " << e.what();
  } catch (const ast::SemanticError &e) {
    ADD_FAILURE() << "semantic errors for query: " << query
                  << " message: " << e.what();
  }
  return {};
}

bool Contains(const std::unordered_set<std::string> &set,
              const std::string &value) {
  return set.find(value) != set.end();
}

}  // namespace

TEST(PlannerQueryTest, BuildsGraphFromMatch) {
  auto statement = ParseOrFail(
      "MATCH (a:Person {name: 'Alice'})-[r:KNOWS]->(b) "
      "WHERE a.age > 30 RETURN a, b");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &main = planner_query.regular.main;

  EXPECT_EQ(main.tail, nullptr);
  EXPECT_TRUE(Contains(main.query_graph.nodes, "a"));
  EXPECT_TRUE(Contains(main.query_graph.nodes, "b"));
  EXPECT_EQ(main.query_graph.where.size(), 1U);

  ASSERT_EQ(main.query_graph.relationships.size(), 1U);
  const auto &relationship = main.query_graph.relationships[0];
  EXPECT_EQ(relationship.name, "r");
  EXPECT_EQ(relationship.left_node, "a");
  EXPECT_EQ(relationship.right_node, "b");
  EXPECT_EQ(relationship.direction, ir::QueryGraph::Direction::kOutgoing);
  EXPECT_TRUE(relationship.types.empty());

  EXPECT_FALSE(main.projection.distinct);
  ASSERT_EQ(main.projection.items.size(), 2U);
  EXPECT_EQ(main.projection.items[0].alias, "a");
  EXPECT_EQ(main.projection.items[1].alias, "b");
  EXPECT_EQ(main.projection.where, nullptr);
  EXPECT_EQ(main.projection.skip, nullptr);
  EXPECT_EQ(main.projection.limit, nullptr);

  EXPECT_TRUE(planner_query.regular.unions.empty());
}

TEST(PlannerQueryTest, AcceptsAnonymousPatternAfterRewrite) {
  auto statement = ParseOrFail("MATCH ()-[]->() RETURN 1");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &main = planner_query.regular.main;

  EXPECT_EQ(main.query_graph.nodes.size(), 2U);
  ASSERT_EQ(main.query_graph.relationships.size(), 1U);

  const auto &relationship = main.query_graph.relationships[0];
  EXPECT_FALSE(relationship.name.empty());
  EXPECT_FALSE(relationship.left_node.empty());
  EXPECT_FALSE(relationship.right_node.empty());

  ASSERT_EQ(main.projection.items.size(), 1U);
  EXPECT_FALSE(main.projection.items[0].alias.empty());
}

TEST(PlannerQueryTest, BuildsTailForMultiPartQuery) {
  auto statement = ParseOrFail(
      "MATCH (n:Person) WITH n WHERE n.age > 30 "
      "MATCH (n)-[r:KNOWS]->(m) RETURN n, m");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &first = planner_query.regular.main;

  ASSERT_TRUE(first.tail);
  const ir::SingleQueryIR &second = *first.tail;

  EXPECT_TRUE(Contains(first.query_graph.nodes, "n"));
  EXPECT_EQ(first.query_graph.where.size(), 1U);
  ASSERT_EQ(first.projection.items.size(), 1U);
  EXPECT_EQ(first.projection.items[0].alias, "n");
  EXPECT_NE(first.projection.where, nullptr);

  EXPECT_TRUE(Contains(second.query_graph.nodes, "n"));
  EXPECT_TRUE(Contains(second.query_graph.nodes, "m"));
  ASSERT_EQ(second.query_graph.relationships.size(), 1U);
  EXPECT_EQ(second.query_graph.relationships[0].name, "r");
  ASSERT_EQ(second.projection.items.size(), 2U);
  EXPECT_EQ(second.projection.items[0].alias, "n");
  EXPECT_EQ(second.projection.items[1].alias, "m");
  EXPECT_EQ(second.tail, nullptr);
}

TEST(PlannerQueryTest, BuildsUnionMappings) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN n AS x UNION MATCH (m) RETURN m AS y");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  ASSERT_EQ(planner_query.regular.unions.size(), 1U);

  const ir::UnionBranch &branch = planner_query.regular.unions[0];
  EXPECT_FALSE(branch.all);
  ASSERT_EQ(branch.mappings.size(), 1U);
  EXPECT_EQ(branch.mappings[0].output, "x");
  EXPECT_EQ(branch.mappings[0].from_main, "x");
  EXPECT_EQ(branch.mappings[0].from_branch, "y");
}

TEST(PlannerQueryTest, BuildsUnionAllBranch) {
  auto statement = ParseOrFail("RETURN 1 AS a UNION ALL RETURN 2 AS b");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  ASSERT_EQ(planner_query.regular.unions.size(), 1U);

  const ir::UnionBranch &branch = planner_query.regular.unions[0];
  EXPECT_TRUE(branch.all);
  ASSERT_EQ(branch.mappings.size(), 1U);
  EXPECT_EQ(branch.mappings[0].output, "a");
  EXPECT_EQ(branch.mappings[0].from_main, "a");
  EXPECT_EQ(branch.mappings[0].from_branch, "b");
}

TEST(PlannerQueryTest, RejectsUnionColumnCountMismatch) {
  auto statement = ParseOrFail("RETURN 1 AS a UNION RETURN 1 AS b, 2 AS c");
  ASSERT_TRUE(statement);

  EXPECT_THROW(
      { (void)ir::BuildStatement(*statement); }, common::InvalidArgumentError);
}

TEST(PlannerQueryTest, CopySingleQueryIRDeeplyCopiesTail) {
  auto statement = ParseOrFail("MATCH (n) WITH n RETURN n");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  ir::SingleQueryIR copy = planner_query.regular.main;

  ASSERT_TRUE(planner_query.regular.main.tail);
  ASSERT_TRUE(copy.tail);
  EXPECT_NE(copy.tail.get(), planner_query.regular.main.tail.get());

  copy.tail->projection.items[0].alias = "renamed";
  EXPECT_EQ(planner_query.regular.main.tail->projection.items[0].alias, "n");
}
