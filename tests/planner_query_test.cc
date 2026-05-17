#include "ir/planner_query.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
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
SelectionDependenciesByExpression(const ir::QueryGraph &query_graph) {
  std::unordered_map<std::string, std::unordered_set<std::string>> result;
  for (const auto &predicate : query_graph.selections.predicates) {
    CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
          "null selection predicate in QueryGraph");
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

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_EQ(main.tail, nullptr);
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "a"));
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "b"));
  EXPECT_EQ(main.query_graph.selections.size(), 4U);

  const auto where_dependencies =
      SelectionDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("a:Person"), "a"));
  EXPECT_TRUE(Contains(where_dependencies.at("a.name = 'Alice'"), "a"));
  EXPECT_TRUE(Contains(where_dependencies.at("r:KNOWS"), "r"));
  EXPECT_TRUE(Contains(where_dependencies.at("a.age > 30"), "a"));
  EXPECT_EQ(main.query_graph.selections.predicates[0].kind,
            ir::PredicateKind::kNodeLabel);
  EXPECT_EQ(main.query_graph.selections.predicates[1].kind,
            ir::PredicateKind::kPropertyEquality);
  EXPECT_EQ(main.query_graph.selections.predicates[2].kind,
            ir::PredicateKind::kRelationshipType);
  EXPECT_EQ(main.query_graph.selections.predicates[3].kind,
            ir::PredicateKind::kPropertyComparison);

  ASSERT_EQ(main.query_graph.pattern_relationships.size(), 1U);
  const auto &relationship = main.query_graph.pattern_relationships[0];
  EXPECT_EQ(relationship.variable, "r");
  EXPECT_EQ(relationship.left_node, "a");
  EXPECT_EQ(relationship.right_node, "b");
  EXPECT_EQ(relationship.direction, ir::Direction::kOutgoing);
  EXPECT_TRUE(relationship.types.empty());
  EXPECT_FALSE(relationship.length.variable);
  EXPECT_EQ(relationship.length.fixed, 1);

  EXPECT_FALSE(main.horizon.RequireProjection().distinct);
  ASSERT_EQ(main.horizon.RequireProjection().items.size(), 2U);
  EXPECT_EQ(main.horizon.RequireProjection().items[0].alias, "a");
  EXPECT_EQ(main.horizon.RequireProjection().items[1].alias, "b");
  EXPECT_EQ(main.horizon.RequireProjection().where, nullptr);
  EXPECT_EQ(main.horizon.RequireProjection().skip, nullptr);
  EXPECT_EQ(main.horizon.RequireProjection().limit, nullptr);

  EXPECT_EQ(planner_query->Kind(), ir::PlannerQueryKind::kSingle);
}

TEST(PlannerQueryTest, AcceptsAnonymousPatternAfterRewrite) {
  auto statement = ParseOrFail("MATCH ()-[]->() RETURN 1");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_EQ(main.query_graph.pattern_nodes.size(), 2U);
  ASSERT_EQ(main.query_graph.pattern_relationships.size(), 1U);

  const auto &relationship = main.query_graph.pattern_relationships[0];
  EXPECT_FALSE(relationship.variable.empty());
  EXPECT_FALSE(relationship.left_node.empty());
  EXPECT_FALSE(relationship.right_node.empty());

  ASSERT_EQ(main.horizon.RequireProjection().items.size(), 1U);
  EXPECT_FALSE(main.horizon.RequireProjection().items[0].alias.empty());
}

TEST(PlannerQueryTest, BuildsVariableLengthRelationshipPattern) {
  auto statement = ParseOrFail("MATCH (a)-[r*1..3]->(b) RETURN r");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.pattern_relationships.size(), 1U);
  const auto &relationship = main.query_graph.pattern_relationships[0];
  EXPECT_EQ(relationship.variable, "r");
  EXPECT_EQ(relationship.left_node, "a");
  EXPECT_EQ(relationship.right_node, "b");
  EXPECT_EQ(relationship.direction, ir::Direction::kOutgoing);
  EXPECT_TRUE(relationship.length.variable);
  ASSERT_TRUE(relationship.length.min.has_value());
  ASSERT_TRUE(relationship.length.max.has_value());
  EXPECT_EQ(*relationship.length.min, 1);
  EXPECT_EQ(*relationship.length.max, 3);
}

TEST(PlannerQueryTest, BuildsTailForMultiPartQuery) {
  auto statement = ParseOrFail(
      "MATCH (n:Person) WHERE true WITH n WHERE n.age > 30 "
      "MATCH (n)-[r:KNOWS]->(m) WHERE true RETURN n, m");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();

  ASSERT_TRUE(first.tail);
  const ir::SinglePlannerQuery &second = *first.tail;

  EXPECT_TRUE(first.query_graph.argument_ids.empty());
  EXPECT_TRUE(Contains(first.query_graph.pattern_nodes, "n"));
  EXPECT_EQ(first.query_graph.selections.size(), 2U);
  const auto first_where_dependencies =
      SelectionDependenciesByExpression(first.query_graph);
  EXPECT_TRUE(Contains(first_where_dependencies.at("n:Person"), "n"));
  EXPECT_TRUE(first_where_dependencies.contains("true"));

  ASSERT_EQ(first.horizon.RequireProjection().items.size(), 1U);
  EXPECT_EQ(first.horizon.RequireProjection().items[0].alias, "n");
  EXPECT_NE(first.horizon.RequireProjection().where, nullptr);

  EXPECT_TRUE(Contains(second.query_graph.pattern_nodes, "n"));
  EXPECT_TRUE(Contains(second.query_graph.pattern_nodes, "m"));
  EXPECT_TRUE(Contains(second.query_graph.argument_ids, "n"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "m"));
  ASSERT_EQ(second.query_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(second.query_graph.pattern_relationships[0].variable, "r");
  EXPECT_EQ(second.query_graph.selections.size(), 2U);
  const auto second_where_dependencies =
      SelectionDependenciesByExpression(second.query_graph);
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

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &unwind_segment = planner_query->RequireSingle();

  EXPECT_TRUE(unwind_segment.query_graph.pattern_nodes.empty());
  EXPECT_TRUE(unwind_segment.query_graph.pattern_relationships.empty());
  ASSERT_EQ(unwind_segment.horizon.kind, ir::QueryHorizonKind::kUnwind);
  EXPECT_NE(unwind_segment.horizon.RequireUnwind().expression, nullptr);
  EXPECT_EQ(unwind_segment.horizon.RequireUnwind().alias, "x");

  ASSERT_TRUE(unwind_segment.tail);
  const ir::SinglePlannerQuery &return_segment = *unwind_segment.tail;
  EXPECT_TRUE(Contains(return_segment.query_graph.argument_ids, "x"));
  ASSERT_EQ(return_segment.horizon.kind, ir::QueryHorizonKind::kProjection);
  ASSERT_EQ(return_segment.horizon.RequireProjection().items.size(), 1U);
  EXPECT_EQ(return_segment.horizon.RequireProjection().items[0].alias, "x");
  EXPECT_EQ(return_segment.tail, nullptr);
}

TEST(PlannerQueryTest, PreservesUnwindSegmentBeforeWithTail) {
  auto statement = ParseOrFail("UNWIND [1, 2] AS x WITH x RETURN x");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &unwind_segment = planner_query->RequireSingle();
  ASSERT_EQ(unwind_segment.horizon.kind, ir::QueryHorizonKind::kUnwind);
  ASSERT_TRUE(unwind_segment.tail);

  const ir::SinglePlannerQuery &with_segment = *unwind_segment.tail;
  ASSERT_EQ(with_segment.horizon.kind, ir::QueryHorizonKind::kProjection);
  ASSERT_TRUE(with_segment.tail);

  const ir::SinglePlannerQuery &return_segment = *with_segment.tail;
  ASSERT_EQ(return_segment.horizon.kind, ir::QueryHorizonKind::kProjection);
  EXPECT_EQ(return_segment.tail, nullptr);
}

TEST(PlannerQueryTest, SplitsConjunctiveWhereIntoPredicates) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE n.age > 30 AND (n.name = 'Alice' AND n.active = true) "
      "RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_EQ(main.query_graph.selections.size(), 3U);
  const auto where_dependencies =
      SelectionDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));
  EXPECT_TRUE(Contains(where_dependencies.at("n.name = 'Alice'"), "n"));
  EXPECT_TRUE(Contains(where_dependencies.at("n.active = true"), "n"));
}

TEST(PlannerQueryTest, AcceptsNonConjunctiveWhereExpression) {
  auto statement = ParseOrFail("MATCH (n) WHERE n.age > 30 RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const auto where_dependencies =
      SelectionDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));
}

TEST(PlannerQueryTest, ExcludesScopedQuantifierVariablesFromDependencies) {
  auto statement =
      ParseOrFail("MATCH (n) WHERE ANY(x IN [1, 2] WHERE x = n.age) RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const auto &dependencies =
      main.query_graph.selections.predicates[0].dependencies;
  EXPECT_TRUE(Contains(dependencies, "n"));
  EXPECT_FALSE(Contains(dependencies, "x"));
}

TEST(PlannerQueryTest, TreatsOnlyOuterPatternVariablesAsExistsDependencies) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE EXISTS { MATCH (n)-[r]->(m) RETURN 1 } RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const auto &dependencies =
      main.query_graph.selections.predicates[0].dependencies;
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

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_EQ(main.query_graph.selections.size(), 2U);
  const auto where_dependencies =
      SelectionDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));
  EXPECT_TRUE(Contains(where_dependencies.at("r:KNOWS"), "r"));
}

TEST(PlannerQueryTest, BuildsUnionBranch) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN n AS x UNION MATCH (m) RETURN m AS x");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  ASSERT_EQ(planner_query->Kind(), ir::PlannerQueryKind::kUnion);

  const ir::UnionPlannerQuery &union_query = planner_query->RequireUnion();
  ASSERT_NE(union_query.lhs, nullptr);
  EXPECT_EQ(union_query.lhs->Kind(), ir::PlannerQueryKind::kSingle);
  EXPECT_FALSE(union_query.all);
}

TEST(PlannerQueryTest, BuildsUnionAllBranch) {
  auto statement = ParseOrFail("RETURN 1 AS a UNION ALL RETURN 2 AS a");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  ASSERT_EQ(planner_query->Kind(), ir::PlannerQueryKind::kUnion);

  const ir::UnionPlannerQuery &union_query = planner_query->RequireUnion();
  ASSERT_NE(union_query.lhs, nullptr);
  EXPECT_EQ(union_query.lhs->Kind(), ir::PlannerQueryKind::kSingle);
  EXPECT_TRUE(union_query.all);
}

TEST(PlannerQueryTest, SinglePlannerQueryIsMoveOnly) {
  EXPECT_FALSE(std::is_copy_constructible_v<ir::SinglePlannerQuery>);
  EXPECT_FALSE(std::is_copy_assignable_v<ir::SinglePlannerQuery>);
  EXPECT_TRUE(std::is_move_constructible_v<ir::SinglePlannerQuery>);
  EXPECT_TRUE(std::is_move_assignable_v<ir::SinglePlannerQuery>);
}
