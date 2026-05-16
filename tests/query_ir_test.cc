#include "ir/query_ir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "ast/expression_to_string.h"
#include "common/exception.h"

namespace {

std::unique_ptr<ast::Statement> ParseOrFail(const std::string &query) {
  try {
    return ast::ParseCypherAndRewrite(query);
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

std::unordered_map<std::string, std::unordered_set<std::string>>
WhereDependenciesByExpression(const ir::QueryGraph &query_graph) {
  std::unordered_map<std::string, std::unordered_set<std::string>> result;
  for (const auto &predicate : query_graph.where) {
    CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
          "null WHERE predicate in QueryGraph");
    result.emplace(ast::ExpressionToString(*predicate.expression),
                   predicate.dependencies);
  }
  return result;
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
  EXPECT_EQ(main.query_graph.where.size(), 4U);

  const auto where_dependencies =
      WhereDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("a:Person"), "a"));
  EXPECT_TRUE(Contains(where_dependencies.at("a.name = 'Alice'"), "a"));
  EXPECT_TRUE(Contains(where_dependencies.at("r:KNOWS"), "r"));
  EXPECT_TRUE(Contains(where_dependencies.at("a.age > 30"), "a"));

  ASSERT_EQ(main.query_graph.relationships.size(), 1U);
  const auto &relationship = main.query_graph.relationships[0];
  EXPECT_EQ(relationship.name, "r");
  EXPECT_EQ(relationship.left_node, "a");
  EXPECT_EQ(relationship.right_node, "b");
  EXPECT_EQ(relationship.direction, ir::QueryGraph::Direction::kOutgoing);
  EXPECT_TRUE(relationship.types.empty());

  EXPECT_FALSE(main.horizon.RequireProjection().distinct);
  ASSERT_EQ(main.horizon.RequireProjection().items.size(), 2U);
  EXPECT_EQ(main.horizon.RequireProjection().items[0].alias, "a");
  EXPECT_EQ(main.horizon.RequireProjection().items[1].alias, "b");
  EXPECT_EQ(main.horizon.RequireProjection().where, nullptr);
  EXPECT_EQ(main.horizon.RequireProjection().skip, nullptr);
  EXPECT_EQ(main.horizon.RequireProjection().limit, nullptr);

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

  ASSERT_EQ(main.horizon.RequireProjection().items.size(), 1U);
  EXPECT_FALSE(main.horizon.RequireProjection().items[0].alias.empty());
}

TEST(PlannerQueryTest, BuildsTailForMultiPartQuery) {
  auto statement = ParseOrFail(
      "MATCH (n:Person) WHERE true WITH n WHERE n.age > 30 "
      "MATCH (n)-[r:KNOWS]->(m) WHERE true RETURN n, m");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &first = planner_query.regular.main;

  ASSERT_TRUE(first.tail);
  const ir::SingleQueryIR &second = *first.tail;

  EXPECT_TRUE(Contains(first.query_graph.nodes, "n"));
  EXPECT_EQ(first.query_graph.where.size(), 2U);
  const auto first_where_dependencies =
      WhereDependenciesByExpression(first.query_graph);
  EXPECT_TRUE(Contains(first_where_dependencies.at("n:Person"), "n"));
  EXPECT_TRUE(first_where_dependencies.contains("true"));

  ASSERT_EQ(first.horizon.RequireProjection().items.size(), 1U);
  EXPECT_EQ(first.horizon.RequireProjection().items[0].alias, "n");
  EXPECT_NE(first.horizon.RequireProjection().where, nullptr);

  EXPECT_TRUE(Contains(second.query_graph.nodes, "n"));
  EXPECT_TRUE(Contains(second.query_graph.nodes, "m"));
  ASSERT_EQ(second.query_graph.relationships.size(), 1U);
  EXPECT_EQ(second.query_graph.relationships[0].name, "r");
  EXPECT_EQ(second.query_graph.where.size(), 2U);
  const auto second_where_dependencies =
      WhereDependenciesByExpression(second.query_graph);
  EXPECT_TRUE(Contains(second_where_dependencies.at("r:KNOWS"), "r"));
  EXPECT_TRUE(second_where_dependencies.contains("true"));

  ASSERT_EQ(second.horizon.RequireProjection().items.size(), 2U);
  EXPECT_EQ(second.horizon.RequireProjection().items[0].alias, "n");
  EXPECT_EQ(second.horizon.RequireProjection().items[1].alias, "m");
  EXPECT_EQ(second.tail, nullptr);
}

TEST(PlannerQueryTest, BuildsUnwindHorizonSegment) {
  auto statement = ParseOrFail("UNWIND [1, 2] AS x RETURN x");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &unwind_segment = planner_query.regular.main;

  EXPECT_TRUE(unwind_segment.query_graph.nodes.empty());
  EXPECT_TRUE(unwind_segment.query_graph.relationships.empty());
  ASSERT_EQ(unwind_segment.horizon.kind, ir::QueryHorizonKind::kUnwind);
  EXPECT_NE(unwind_segment.horizon.RequireUnwind().expression, nullptr);
  EXPECT_EQ(unwind_segment.horizon.RequireUnwind().alias, "x");

  ASSERT_TRUE(unwind_segment.tail);
  const ir::SingleQueryIR &return_segment = *unwind_segment.tail;
  ASSERT_EQ(return_segment.horizon.kind, ir::QueryHorizonKind::kProjection);
  ASSERT_EQ(return_segment.horizon.RequireProjection().items.size(), 1U);
  EXPECT_EQ(return_segment.horizon.RequireProjection().items[0].alias, "x");
  EXPECT_EQ(return_segment.tail, nullptr);
}

TEST(PlannerQueryTest, PreservesUnwindSegmentBeforeWithTail) {
  auto statement = ParseOrFail("UNWIND [1, 2] AS x WITH x RETURN x");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &unwind_segment = planner_query.regular.main;
  ASSERT_EQ(unwind_segment.horizon.kind, ir::QueryHorizonKind::kUnwind);
  ASSERT_TRUE(unwind_segment.tail);

  const ir::SingleQueryIR &with_segment = *unwind_segment.tail;
  ASSERT_EQ(with_segment.horizon.kind, ir::QueryHorizonKind::kProjection);
  ASSERT_TRUE(with_segment.tail);

  const ir::SingleQueryIR &return_segment = *with_segment.tail;
  ASSERT_EQ(return_segment.horizon.kind, ir::QueryHorizonKind::kProjection);
  EXPECT_EQ(return_segment.tail, nullptr);
}

TEST(PlannerQueryTest, SplitsConjunctiveWhereIntoPredicates) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE n.age > 30 AND (n.name = 'Alice' AND n.active = true) "
      "RETURN n");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &main = planner_query.regular.main;

  EXPECT_EQ(main.query_graph.where.size(), 3U);
  const auto where_dependencies =
      WhereDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));
  EXPECT_TRUE(Contains(where_dependencies.at("n.name = 'Alice'"), "n"));
  EXPECT_TRUE(Contains(where_dependencies.at("n.active = true"), "n"));
}

TEST(PlannerQueryTest, AcceptsNonConjunctiveWhereExpression) {
  auto statement = ParseOrFail("MATCH (n) WHERE n.age > 30 RETURN n");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &main = planner_query.regular.main;

  ASSERT_EQ(main.query_graph.where.size(), 1U);
  const auto where_dependencies =
      WhereDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));
}

TEST(PlannerQueryTest, ExcludesScopedQuantifierVariablesFromDependencies) {
  auto statement =
      ParseOrFail("MATCH (n) WHERE ANY(x IN [1, 2] WHERE x = n.age) RETURN n");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &main = planner_query.regular.main;

  ASSERT_EQ(main.query_graph.where.size(), 1U);
  const auto &dependencies = main.query_graph.where[0].dependencies;
  EXPECT_TRUE(Contains(dependencies, "n"));
  EXPECT_FALSE(Contains(dependencies, "x"));
}

TEST(PlannerQueryTest, TreatsOnlyOuterPatternVariablesAsExistsDependencies) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE EXISTS { MATCH (n)-[r]->(m) RETURN 1 } RETURN n");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &main = planner_query.regular.main;

  ASSERT_EQ(main.query_graph.where.size(), 1U);
  const auto &dependencies = main.query_graph.where[0].dependencies;
  EXPECT_TRUE(Contains(dependencies, "n"));
  EXPECT_FALSE(Contains(dependencies, "r"));
  EXPECT_FALSE(Contains(dependencies, "m"));
}

TEST(PlannerQueryTest, DeduplicatesRepeatedWherePredicatesAcrossMatches) {
  auto statement = ParseOrFail(
      "MATCH (n)-[r:KNOWS]->(m) WHERE n.age > 30 "
      "MATCH (n)-[r:KNOWS]->(m) WHERE n.age > 30 "
      "RETURN n, m");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  const ir::SingleQueryIR &main = planner_query.regular.main;

  EXPECT_EQ(main.query_graph.where.size(), 2U);
  const auto where_dependencies =
      WhereDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));
  EXPECT_TRUE(Contains(where_dependencies.at("r:KNOWS"), "r"));
}

TEST(PlannerQueryTest, BuildsUnionBranch) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN n AS x UNION MATCH (m) RETURN m AS x");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  ASSERT_EQ(planner_query.regular.unions.size(), 1U);

  const ir::UnionBranch &branch = planner_query.regular.unions[0];
  EXPECT_FALSE(branch.all);
}

TEST(PlannerQueryTest, BuildsUnionAllBranch) {
  auto statement = ParseOrFail("RETURN 1 AS a UNION ALL RETURN 2 AS a");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  ASSERT_EQ(planner_query.regular.unions.size(), 1U);

  const ir::UnionBranch &branch = planner_query.regular.unions[0];
  EXPECT_TRUE(branch.all);
}

TEST(PlannerQueryTest, CopySingleQueryIRDeeplyCopiesTail) {
  auto statement = ParseOrFail("MATCH (n) WITH n RETURN n");
  ASSERT_TRUE(statement);

  ir::QueryIR planner_query = ir::BuildStatement(*statement);
  ir::SingleQueryIR copy = planner_query.regular.main;

  ASSERT_TRUE(planner_query.regular.main.tail);
  ASSERT_TRUE(copy.tail);
  EXPECT_NE(copy.tail.get(), planner_query.regular.main.tail.get());

  copy.tail->horizon.RequireProjection().items[0].alias = "renamed";
  EXPECT_EQ(planner_query.regular.main.tail->horizon.RequireProjection()
                .items[0]
                .alias,
            "n");
}
