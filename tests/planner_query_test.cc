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
#include "ir/planner_query_printer.h"

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
SelectionDependenciesByExpression(const ir::Selections &selections) {
  std::unordered_map<std::string, std::unordered_set<std::string>> result;
  for (const auto &predicate : selections.predicates) {
    CHECK(predicate.expression != nullptr, common::InvalidArgumentError,
          "null selection predicate in QueryGraph");
    result.emplace(ast::ExpressionToString(*predicate.expression),
                   predicate.dependencies);
  }
  return result;
}

std::unordered_map<std::string, std::unordered_set<std::string>>
SelectionDependenciesByExpression(const ir::QueryGraph &query_graph) {
  return SelectionDependenciesByExpression(query_graph.selections);
}

void ExpectPlannerQueryText(const std::string &query,
                            const std::string &expected) {
  auto statement = ParseOrFail(query);
  ASSERT_TRUE(statement);
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  EXPECT_EQ(ir::PlannerQueryToString(*planner_query), expected);
}

}  // namespace

TEST(PlannerQueryPrinterTest, DumpsSimpleMatch) {
  ExpectPlannerQueryText("MATCH (n) RETURN n", R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: n
          expression: n
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsInlineNodePredicateSelection) {
  ExpectPlannerQueryText("MATCH (n:Person) RETURN n", R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      - kind: node_label
        expression: n:Person
        dependencies: [n]
        variable: n
        labels: [Person]
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: n
          expression: n
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsRelationshipPatternWithInlineType) {
  ExpectPlannerQueryText("MATCH (a)-[r:KNOWS]->(b) RETURN a, b",
                         R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [a, b]
    pattern_relationships:
      - variable: r
        left_node: a
        right_node: b
        direction: outgoing
        types: [KNOWS]
        length: fixed(1)
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: a
          expression: a
        - alias: b
          expression: b
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsExplicitRelationshipTypeSelection) {
  ExpectPlannerQueryText("MATCH (a)-[r]->(b) WHERE r:KNOWS RETURN r",
                         R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [a, b]
    pattern_relationships:
      - variable: r
        left_node: a
        right_node: b
        direction: outgoing
        types: []
        length: fixed(1)
    selections:
      - kind: relationship_type
        expression: r:KNOWS
        dependencies: [r]
        variable: r
        relationship_types: [KNOWS]
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: r
          expression: r
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsVariableLengthRelationship) {
  ExpectPlannerQueryText("MATCH (a)-[r*1..3]->(b) RETURN r",
                         R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [a, b]
    pattern_relationships:
      - variable: r
        left_node: a
        right_node: b
        direction: outgoing
        types: []
        length: variable(1..3)
    selections:
      - kind: generic_expression
        expression: ALL(__uniq_rel_0 IN r WHERE SINGLE(__uniq_rel_1 IN r WHERE __uniq_rel_0 = __uniq_rel_1))
        dependencies: [r]
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: r
          expression: r
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsUnwindTail) {
  ExpectPlannerQueryText("UNWIND [1, 2] AS x RETURN x", R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: []
    pattern_relationships:
      []
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    unwind:
      expression: [1, 2]
      alias: x
  tail:
    SinglePlannerQuery
      query_graph:
        argument_ids: [x]
        pattern_nodes: []
        pattern_relationships:
          []
        selections:
          []
        optional_matches: 0
        hints: 0
        mutating_patterns: 0
      horizon:
        regular_projection:
          items:
            - alias: x
              expression: x
          selections:
            []
          required_order:
            []
          pagination:
            skip: null
            limit: null
      tail:
        null
)");
}

TEST(PlannerQueryPrinterTest, DumpsUnionAll) {
  ExpectPlannerQueryText("RETURN 1 AS x UNION ALL RETURN 2 AS x",
                         R"(UnionPlannerQuery
  all: true
  lhs:
    SinglePlannerQuery
      query_graph:
        argument_ids: []
        pattern_nodes: []
        pattern_relationships:
          []
        selections:
          []
        optional_matches: 0
        hints: 0
        mutating_patterns: 0
      horizon:
        regular_projection:
          items:
            - alias: x
              expression: 1
          selections:
            []
          required_order:
            []
          pagination:
            skip: null
            limit: null
      tail:
        null
  rhs:
    SinglePlannerQuery
      query_graph:
        argument_ids: []
        pattern_nodes: []
        pattern_relationships:
          []
        selections:
          []
        optional_matches: 0
        hints: 0
        mutating_patterns: 0
      horizon:
        regular_projection:
          items:
            - alias: x
              expression: 2
          selections:
            []
          required_order:
            []
          pagination:
            skip: null
            limit: null
      tail:
        null
)");
}

TEST(PlannerQueryPrinterTest, DumpsInlinePropertySelection) {
  ExpectPlannerQueryText("MATCH (n {id: 1}) RETURN n", R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      - kind: property_equality
        expression: n.id = 1
        dependencies: [n]
        variable: n
        property_key: id
        comparison_op: =
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: n
          expression: n
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsExplicitWhereSelection) {
  ExpectPlannerQueryText("MATCH (n) WHERE n.age > 30 RETURN n",
                         R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      - kind: property_comparison
        expression: n.age > 30
        dependencies: [n]
        variable: n
        property_key: age
        comparison_op: >
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: n
          expression: n
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsWithTail) {
  ExpectPlannerQueryText("MATCH (n) WITH n.name AS name RETURN name",
                         R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: name
          expression: n.name
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    SinglePlannerQuery
      query_graph:
        argument_ids: [name]
        pattern_nodes: []
        pattern_relationships:
          []
        selections:
          []
        optional_matches: 0
        hints: 0
        mutating_patterns: 0
      horizon:
        regular_projection:
          items:
            - alias: name
              expression: name
          selections:
            []
          required_order:
            []
          pagination:
            skip: null
            limit: null
      tail:
        null
)");
}

TEST(PlannerQueryPrinterTest, DumpsDistinctProjection) {
  ExpectPlannerQueryText("MATCH (n) RETURN DISTINCT n", R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    distinct_projection:
      grouping_items:
        - alias: n
          expression: n
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsCountStarSnapshot) {
  ExpectPlannerQueryText("MATCH (n) RETURN count(*)", R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    aggregating_projection:
      grouping_items:
        []
      aggregation_items:
        - alias: count(*)
          expression: count(*)
      selections:
        []
      required_order:
        []
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsOrderBySkipLimit) {
  ExpectPlannerQueryText("MATCH (n) RETURN n ORDER BY n SKIP 1 LIMIT 2",
                         R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: n
          expression: n
      selections:
        []
      required_order:
        - expression: n
          direction: ascending
      pagination:
        skip: 1
        limit: 2
  tail:
    null
)");
}

TEST(PlannerQueryPrinterTest, DumpsRequiredOrderDirections) {
  ExpectPlannerQueryText("MATCH (n) RETURN n ORDER BY n.name DESC, n.age ASC",
                         R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      []
    optional_matches: 0
    hints: 0
    mutating_patterns: 0
  horizon:
    regular_projection:
      items:
        - alias: n
          expression: n
      selections:
        []
      required_order:
        - expression: n.name
          direction: descending
        - expression: n.age
          direction: ascending
      pagination:
        skip: null
        limit: null
  tail:
    null
)");
}

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
  EXPECT_EQ(main.query_graph.selections.size(), 3U);

  const auto where_dependencies =
      SelectionDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("a:Person"), "a"));
  EXPECT_TRUE(Contains(where_dependencies.at("a.name = 'Alice'"), "a"));
  EXPECT_TRUE(Contains(where_dependencies.at("a.age > 30"), "a"));
  EXPECT_EQ(main.query_graph.selections.predicates[0].kind,
            ir::PredicateKind::kNodeLabel);
  EXPECT_EQ(main.query_graph.selections.predicates[1].kind,
            ir::PredicateKind::kPropertyEquality);
  EXPECT_EQ(main.query_graph.selections.predicates[2].kind,
            ir::PredicateKind::kPropertyComparison);

  ASSERT_EQ(main.query_graph.pattern_relationships.size(), 1U);
  const auto &relationship = main.query_graph.pattern_relationships[0];
  EXPECT_EQ(relationship.variable, "r");
  EXPECT_EQ(relationship.left_node, "a");
  EXPECT_EQ(relationship.right_node, "b");
  EXPECT_EQ(relationship.direction, ir::Direction::kOutgoing);
  EXPECT_EQ(relationship.types, std::vector<std::string>({"KNOWS"}));
  EXPECT_FALSE(relationship.length.variable);
  EXPECT_EQ(relationship.length.fixed, 1);

  ASSERT_EQ(main.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  const ir::RegularQueryProjection &projection =
      main.horizon.RequireRegularProjection();
  ASSERT_EQ(projection.items.size(), 2U);
  EXPECT_EQ(projection.items[0].alias, "a");
  EXPECT_EQ(projection.items[1].alias, "b");
  EXPECT_TRUE(projection.selections.empty());
  EXPECT_EQ(projection.pagination.skip, nullptr);
  EXPECT_EQ(projection.pagination.limit, nullptr);

  EXPECT_EQ(planner_query->Kind(), ir::PlannerQueryKind::kSingle);
}

TEST(PlannerQueryTest, BuildsNamedPathBinding) {
  auto statement = ParseOrFail("MATCH p = (a)-[r]->(b) RETURN p");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_TRUE(Contains(main.query_graph.pattern_paths, "p"));
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "a"));
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "b"));
  ASSERT_EQ(main.query_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(main.query_graph.pattern_relationships[0].variable, "r");

  ASSERT_EQ(main.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  const ir::RegularQueryProjection &projection =
      main.horizon.RequireRegularProjection();
  ASSERT_EQ(projection.items.size(), 1U);
  EXPECT_EQ(projection.items[0].alias, "p");
  ASSERT_NE(projection.items[0].expression, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*projection.items[0].expression), "p");
}

TEST(PlannerQueryTest, NarrowsNamedPathTailArgumentIds) {
  auto statement = ParseOrFail(
      "MATCH p = (a)-[r]->(b) WITH p, 1 AS unused "
      "MATCH (c) RETURN p, c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);
  const ir::SinglePlannerQuery &second = *first.tail;

  EXPECT_TRUE(Contains(second.query_graph.argument_ids, "p"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "unused"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "r"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "b"));
}

TEST(PlannerQueryTest, BuildsRegularProjectionHorizon) {
  auto statement = ParseOrFail("MATCH (n) RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryHorizon &horizon = planner_query->RequireSingle().horizon;
  ASSERT_EQ(horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  const ir::RegularQueryProjection &projection =
      horizon.RequireRegularProjection();

  ASSERT_EQ(projection.items.size(), 1U);
  EXPECT_EQ(projection.items[0].alias, "n");
}

TEST(PlannerQueryTest, BuildsDistinctProjectionHorizon) {
  auto statement = ParseOrFail("MATCH (n) RETURN DISTINCT n.name AS name");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryHorizon &horizon = planner_query->RequireSingle().horizon;
  ASSERT_EQ(horizon.kind, ir::QueryHorizonKind::kDistinctProjection);
  const ir::DistinctQueryProjection &projection =
      horizon.RequireDistinctProjection();

  ASSERT_EQ(projection.grouping_items.size(), 1U);
  EXPECT_EQ(projection.grouping_items[0].alias, "name");
}

TEST(PlannerQueryTest, BuildsAggregatingProjectionItems) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN n.name AS name, count(n) AS c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryHorizon &horizon = planner_query->RequireSingle().horizon;
  ASSERT_EQ(horizon.kind, ir::QueryHorizonKind::kAggregatingProjection);
  const ir::AggregatingQueryProjection &projection =
      horizon.RequireAggregatingProjection();

  ASSERT_EQ(projection.grouping_items.size(), 1U);
  ASSERT_EQ(projection.aggregation_items.size(), 1U);
  EXPECT_EQ(projection.grouping_items[0].alias, "name");
  EXPECT_EQ(projection.aggregation_items[0].alias, "c");
}

TEST(PlannerQueryTest, PreservesCountStarAggregationExpression) {
  auto statement = ParseOrFail("MATCH (n) RETURN count(*) AS c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryHorizon &horizon = planner_query->RequireSingle().horizon;
  ASSERT_EQ(horizon.kind, ir::QueryHorizonKind::kAggregatingProjection);
  const ir::AggregatingQueryProjection &projection =
      horizon.RequireAggregatingProjection();

  ASSERT_EQ(projection.aggregation_items.size(), 1U);
  EXPECT_EQ(projection.aggregation_items[0].alias, "c");
  ASSERT_NE(projection.aggregation_items[0].expression, nullptr);
  EXPECT_TRUE(projection.aggregation_items[0].expression->Is(
      ast::ASTNodeType::kCountStarExpression));
  EXPECT_EQ(
      ast::ExpressionToString(*projection.aggregation_items[0].expression),
      "count(*)");
}

TEST(PlannerQueryTest, QueriesSelectionsByStructuredPredicateFields) {
  auto statement = ParseOrFail(
      "MATCH (a:Person {name: 'Alice'})-[r]->(b) "
      "WHERE a.age > 30 AND r:KNOWS AND b.age = 40 "
      "RETURN a, b, r");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();
  const ir::Selections &selections = main.query_graph.selections;

  EXPECT_EQ(selections.size(), 5U);
  EXPECT_EQ(selections.PredicatesByVariable("a").size(), 3U);
  EXPECT_EQ(selections.PredicatesByVariable("r").size(), 1U);
  EXPECT_EQ(selections.PredicatesDependingOn("a").size(), 3U);
  EXPECT_EQ(
      selections.PredicatesByKind(ir::PredicateKind::kPropertyEquality).size(),
      2U);

  const auto node_labels = selections.NodeLabelPredicates("a");
  ASSERT_EQ(node_labels.size(), 1U);
  EXPECT_EQ(node_labels[0]->labels, std::vector<std::string>({"Person"}));
  EXPECT_TRUE(selections.ContainsNodeLabel("a", "Person"));
  EXPECT_FALSE(selections.ContainsNodeLabel("a", "Employee"));

  const auto relationship_types = selections.RelationshipTypePredicates("r");
  ASSERT_EQ(relationship_types.size(), 1U);
  EXPECT_EQ(relationship_types[0]->relationship_types,
            std::vector<std::string>({"KNOWS"}));
  EXPECT_TRUE(selections.ContainsRelationshipType("r", "KNOWS"));
  EXPECT_FALSE(selections.ContainsRelationshipType("r", "LIKES"));

  const auto age_predicates = selections.PropertyPredicates("a", "age");
  ASSERT_EQ(age_predicates.size(), 1U);
  EXPECT_EQ(age_predicates[0]->comparison_op, ">");
  EXPECT_EQ(selections.PropertyPredicates("a", "age", ">").size(), 1U);
  EXPECT_TRUE(selections.ContainsPropertyPredicate("a", "age"));
  EXPECT_TRUE(selections.ContainsPropertyPredicate("a", "age", ">"));
  EXPECT_FALSE(selections.ContainsPropertyPredicate("a", "age", "="));
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

  ASSERT_EQ(main.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  ASSERT_EQ(main.horizon.RequireRegularProjection().items.size(), 1U);
  EXPECT_FALSE(main.horizon.RequireRegularProjection().items[0].alias.empty());
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

  ASSERT_EQ(first.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  ASSERT_EQ(first.horizon.RequireRegularProjection().items.size(), 1U);
  EXPECT_EQ(first.horizon.RequireRegularProjection().items[0].alias, "n");
  ASSERT_EQ(first.horizon.RequireRegularProjection().selections.size(), 1U);
  const auto &projection_predicate =
      first.horizon.RequireRegularProjection().selections.predicates[0];
  EXPECT_EQ(projection_predicate.kind, ir::PredicateKind::kPropertyComparison);
  EXPECT_TRUE(Contains(projection_predicate.dependencies, "n"));

  EXPECT_TRUE(Contains(second.query_graph.pattern_nodes, "n"));
  EXPECT_TRUE(Contains(second.query_graph.pattern_nodes, "m"));
  EXPECT_TRUE(Contains(second.query_graph.argument_ids, "n"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "m"));
  ASSERT_EQ(second.query_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(second.query_graph.pattern_relationships[0].variable, "r");
  EXPECT_EQ(second.query_graph.pattern_relationships[0].types,
            std::vector<std::string>({"KNOWS"}));
  EXPECT_EQ(second.query_graph.selections.size(), 1U);
  const auto second_where_dependencies =
      SelectionDependenciesByExpression(second.query_graph);
  EXPECT_TRUE(second_where_dependencies.contains("true"));

  ASSERT_EQ(second.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  ASSERT_EQ(second.horizon.RequireRegularProjection().items.size(), 2U);
  EXPECT_EQ(second.horizon.RequireRegularProjection().items[0].alias, "n");
  EXPECT_EQ(second.horizon.RequireRegularProjection().items[1].alias, "m");
  EXPECT_EQ(second.tail, nullptr);
}

TEST(PlannerQueryTest, BuildsProjectionSelectionsForWithWhere) {
  auto statement =
      ParseOrFail("MATCH (n) WITH n AS x WHERE x.age > 30 RETURN x");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);

  const ir::RegularQueryProjection &projection =
      first.horizon.RequireRegularProjection();
  ASSERT_EQ(projection.items.size(), 1U);
  EXPECT_EQ(projection.items[0].alias, "x");
  ASSERT_EQ(projection.selections.size(), 1U);
  const auto &predicate = projection.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kPropertyComparison);
  EXPECT_EQ(predicate.variable, "x");
  EXPECT_EQ(predicate.property_key, "age");
  EXPECT_EQ(predicate.comparison_op, ">");
  EXPECT_TRUE(Contains(predicate.dependencies, "x"));
  EXPECT_FALSE(Contains(predicate.dependencies, "n"));
}

TEST(PlannerQueryTest, NarrowsProjectionExistsSubqueryArgumentIds) {
  auto statement = ParseOrFail(
      "MATCH (a) WITH a, 1 AS unused "
      "WHERE EXISTS { MATCH (a)-[r]->(b) RETURN 1 AS ok } RETURN a");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  const ir::RegularQueryProjection &projection =
      first.horizon.RequireRegularProjection();

  ASSERT_EQ(projection.selections.size(), 1U);
  const auto &predicate = projection.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kExistsSubquery);
  ASSERT_NE(predicate.subquery, nullptr);
  const ir::SinglePlannerQuery &subquery = predicate.subquery->RequireSingle();
  EXPECT_TRUE(Contains(subquery.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "unused"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "b"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "r"));
}

TEST(PlannerQueryTest, BuildsProjectionPagination) {
  auto statement = ParseOrFail("MATCH (n) RETURN n SKIP 1 LIMIT 2");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::RegularQueryProjection &projection =
      planner_query->RequireSingle().horizon.RequireRegularProjection();

  ASSERT_NE(projection.pagination.skip, nullptr);
  ASSERT_NE(projection.pagination.limit, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*projection.pagination.skip), "1");
  EXPECT_EQ(ast::ExpressionToString(*projection.pagination.limit), "2");
}

TEST(PlannerQueryTest, BuildsProjectionRequiredOrder) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN n ORDER BY n.name DESC, n.age ASC");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::RegularQueryProjection &projection =
      planner_query->RequireSingle().horizon.RequireRegularProjection();

  ASSERT_EQ(projection.required_order.size(), 2U);
  EXPECT_EQ(
      ast::ExpressionToString(*projection.required_order.items[0].expression),
      "n.name");
  EXPECT_EQ(projection.required_order.items[0].direction,
            ir::OrderDirection::kDescending);
  EXPECT_EQ(
      ast::ExpressionToString(*projection.required_order.items[1].expression),
      "n.age");
  EXPECT_EQ(projection.required_order.items[1].direction,
            ir::OrderDirection::kAscending);
}

TEST(PlannerQueryTest, NarrowsTailArgumentIdsToActualDependencies) {
  auto statement =
      ParseOrFail("MATCH (a) WITH a, 1 AS b MATCH (a)-->(c) RETURN c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);
  const ir::SinglePlannerQuery &second = *first.tail;

  EXPECT_TRUE(Contains(second.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "b"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "c"));
}

TEST(PlannerQueryTest, BuildsOptionalMatchQueryGraph) {
  auto statement = ParseOrFail(
      "MATCH (a) WITH a, 1 AS unused "
      "OPTIONAL MATCH (a)-[r]->(b) RETURN a, b");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);
  const ir::SinglePlannerQuery &main = *first.tail;

  EXPECT_TRUE(Contains(main.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(main.query_graph.argument_ids, "unused"));
  EXPECT_TRUE(main.query_graph.pattern_nodes.empty());
  ASSERT_EQ(main.query_graph.optional_matches.size(), 1U);
  const ir::QueryGraph &optional = main.query_graph.optional_matches[0];

  EXPECT_TRUE(Contains(optional.argument_ids, "a"));
  EXPECT_FALSE(Contains(optional.argument_ids, "unused"));
  EXPECT_FALSE(Contains(optional.argument_ids, "b"));
  EXPECT_FALSE(Contains(optional.argument_ids, "r"));
  EXPECT_TRUE(Contains(optional.pattern_nodes, "a"));
  EXPECT_TRUE(Contains(optional.pattern_nodes, "b"));
  ASSERT_EQ(optional.pattern_relationships.size(), 1U);
  EXPECT_EQ(optional.pattern_relationships[0].variable, "r");
}

TEST(PlannerQueryTest, ClassifiesArgumentRelationshipTypeWithSemanticTable) {
  auto statement =
      ParseOrFail("MATCH (a)-[r]->(b) WITH r MATCH (n) WHERE r:KNOWS RETURN r");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);
  const ir::SinglePlannerQuery &second = *first.tail;

  EXPECT_TRUE(Contains(second.query_graph.argument_ids, "r"));
  ASSERT_EQ(second.query_graph.selections.size(), 1U);
  const auto &predicate = second.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kRelationshipType);
  EXPECT_EQ(predicate.variable, "r");
  EXPECT_EQ(predicate.relationship_types, std::vector<std::string>({"KNOWS"}));
}

TEST(PlannerQueryTest, ClassifiesProjectionWhereWithScopedVariableType) {
  auto statement =
      ParseOrFail("MATCH (n)-[r]->(b) WITH r AS n WHERE n:KNOWS RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();

  ASSERT_EQ(first.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  const ir::RegularQueryProjection &projection =
      first.horizon.RequireRegularProjection();
  ASSERT_EQ(projection.selections.size(), 1U);
  const ir::Predicate &predicate = projection.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kRelationshipType);
  EXPECT_EQ(predicate.variable, "n");
  EXPECT_EQ(predicate.relationship_types, std::vector<std::string>({"KNOWS"}));
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
  ASSERT_EQ(return_segment.horizon.kind,
            ir::QueryHorizonKind::kRegularProjection);
  ASSERT_EQ(return_segment.horizon.RequireRegularProjection().items.size(), 1U);
  EXPECT_EQ(return_segment.horizon.RequireRegularProjection().items[0].alias,
            "x");
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
  ASSERT_EQ(with_segment.horizon.kind,
            ir::QueryHorizonKind::kRegularProjection);
  ASSERT_TRUE(with_segment.tail);

  const ir::SinglePlannerQuery &return_segment = *with_segment.tail;
  ASSERT_EQ(return_segment.horizon.kind,
            ir::QueryHorizonKind::kRegularProjection);
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
  const auto &predicate = main.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kExistsSubquery);
  const auto &dependencies = predicate.dependencies;
  EXPECT_TRUE(Contains(dependencies, "n"));
  EXPECT_FALSE(Contains(dependencies, "r"));
  EXPECT_FALSE(Contains(dependencies, "m"));

  ASSERT_NE(predicate.subquery, nullptr);
  const ir::SinglePlannerQuery &subquery = predicate.subquery->RequireSingle();
  EXPECT_TRUE(Contains(subquery.query_graph.argument_ids, "n"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "r"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "m"));
  EXPECT_TRUE(Contains(subquery.query_graph.pattern_nodes, "n"));
  EXPECT_TRUE(Contains(subquery.query_graph.pattern_nodes, "m"));
  ASSERT_EQ(subquery.query_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(subquery.query_graph.pattern_relationships[0].variable, "r");
}

TEST(PlannerQueryTest, NarrowsExistsSubqueryArgumentIdsToUsedOuterVariables) {
  auto statement = ParseOrFail(
      "MATCH (a) WITH a, 1 AS unused "
      "MATCH (a) WHERE EXISTS { MATCH (a)-[r]->(b) RETURN 1 AS ok } RETURN a");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);
  const ir::SinglePlannerQuery &main = *first.tail;

  EXPECT_TRUE(Contains(main.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(main.query_graph.argument_ids, "unused"));
  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const auto &predicate = main.query_graph.selections.predicates[0];
  ASSERT_NE(predicate.subquery, nullptr);
  const ir::SinglePlannerQuery &subquery = predicate.subquery->RequireSingle();
  EXPECT_TRUE(Contains(subquery.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "unused"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "b"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "r"));
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

  EXPECT_EQ(main.query_graph.selections.size(), 1U);
  const auto where_dependencies =
      SelectionDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));
  ASSERT_EQ(main.query_graph.pattern_relationships.size(), 2U);
  EXPECT_EQ(main.query_graph.pattern_relationships[0].types,
            std::vector<std::string>({"KNOWS"}));
  EXPECT_EQ(main.query_graph.pattern_relationships[1].types,
            std::vector<std::string>({"KNOWS"}));
}

TEST(PlannerQueryTest, DeduplicatesRepeatedNodeLabelWherePredicates) {
  auto statement =
      ParseOrFail("MATCH (n) WHERE n:Person AND n:Person RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const auto &predicate = main.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kNodeLabel);
  EXPECT_EQ(predicate.variable, "n");
  EXPECT_EQ(predicate.labels, std::vector<std::string>({"Person"}));
}

TEST(PlannerQueryTest, DeduplicatesRepeatedRelationshipTypeWherePredicates) {
  auto statement =
      ParseOrFail("MATCH (a)-[r]->(b) WHERE r:KNOWS AND r:KNOWS RETURN r");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const auto &predicate = main.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kRelationshipType);
  EXPECT_EQ(predicate.variable, "r");
  EXPECT_EQ(predicate.relationship_types, std::vector<std::string>({"KNOWS"}));
}

TEST(PlannerQueryTest, DeduplicatesRepeatedPropertyComparisonWherePredicates) {
  auto statement =
      ParseOrFail("MATCH (n) WHERE n.age > 30 AND n.age > 30 RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const auto &predicate = main.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kPropertyComparison);
  EXPECT_EQ(predicate.variable, "n");
  EXPECT_EQ(predicate.property_key, "age");
  EXPECT_EQ(predicate.comparison_op, ">");
}

TEST(PlannerQueryTest, DeduplicatesRepeatedGenericWherePredicates) {
  auto statement = ParseOrFail("MATCH (n) WHERE true AND true RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  EXPECT_EQ(main.query_graph.selections.predicates[0].kind,
            ir::PredicateKind::kGenericExpression);
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
