#include "ir/planner_query.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
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

std::unique_ptr<ast::Statement> ParseRawOrFail(const std::string &query) {
  try {
    return ast::ParseCypher(query);
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

void ExpectPlannerQueryContractError(const std::string &query,
                                     const std::string &expected_message) {
  auto statement = ParseRawOrFail(query);
  ASSERT_TRUE(statement);

  try {
    (void)ir::CreatePlannerQuery(*statement);
    FAIL() << "expected planner query input contract error";
  } catch (const common::InvalidArgumentError &e) {
    EXPECT_NE(e.Message().find(expected_message), std::string::npos)
        << "actual message: " << e.Message();
  }
}

}  // namespace

TEST(PlannerQueryInputContractTest, RejectsRawReturnStar) {
  ExpectPlannerQueryContractError("MATCH (n) RETURN *",
                                  "projection star must be expanded");
}

TEST(PlannerQueryInputContractTest, RejectsRawProjectionWithoutAlias) {
  ExpectPlannerQueryContractError("MATCH (n) RETURN n",
                                  "projection item alias must be filled");
}

TEST(PlannerQueryInputContractTest, RejectsRawInlineReadPatternPredicates) {
  ExpectPlannerQueryContractError(
      "MATCH (n:Person {id: 1}) RETURN n AS n",
      "inline node labels in read patterns must be normalized");
}

TEST(PlannerQueryInputContractTest, RejectsRawPatternPredicateExpression) {
  ExpectPlannerQueryContractError(
      "MATCH (n) WHERE (n)-->(m) RETURN n AS n",
      "pattern predicates must be rewritten to existential subqueries");
}

TEST(PlannerQueryInputContractTest, RejectsRawAnonymousPatternNames) {
  ExpectPlannerQueryContractError("MATCH () RETURN 1 AS x",
                                  "anonymous nodes must be named");
}

TEST(PlannerQueryInputContractTest, AcceptsRewrittenReturnStar) {
  auto statement = ParseOrFail("MATCH (n) RETURN *");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::RegularQueryProjection &projection =
      planner_query->RequireSingle().horizon.RequireRegularProjection();

  ASSERT_EQ(projection.items.size(), 1U);
  EXPECT_EQ(projection.items[0].alias, "n");
}

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
  distinct: false
  mappings:
    - output: x
      lhs: x
      rhs: x
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
        property_value: 1
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
        property_value: 30
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

TEST(PlannerQueryPrinterTest, DumpsStructuredPropertyPredicates) {
  ExpectPlannerQueryText(
      "MATCH (n) WHERE 1 = n.id AND n.name STARTS WITH 'A' AND "
      "n.prop IN [1, 2] AND n.deleted IS NULL AND n.email IS NOT NULL RETURN n",
      R"(SinglePlannerQuery
  query_graph:
    argument_ids: []
    pattern_nodes: [n]
    pattern_relationships:
      []
    selections:
      - kind: property_equality
        expression: 1 = n.id
        dependencies: [n]
        variable: n
        property_key: id
        property_value: 1
        comparison_op: =
      - kind: property_string_predicate
        expression: n.name STARTS WITH 'A'
        dependencies: [n]
        variable: n
        property_key: name
        property_value: 'A'
        comparison_op: STARTS WITH
      - kind: property_in
        expression: n.prop IN [1, 2]
        dependencies: [n]
        variable: n
        property_key: prop
        property_value: [1, 2]
        comparison_op: IN
      - kind: property_is_null
        expression: n.deleted IS NULL
        dependencies: [n]
        variable: n
        property_key: deleted
        comparison_op: IS NULL
      - kind: property_is_not_null
        expression: n.email IS NOT NULL
        dependencies: [n]
        variable: n
        property_key: email
        comparison_op: IS NOT NULL
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

TEST(PlannerQueryTest, ClassifiesNormalizedPropertyPredicates) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE 1 = n.id AND 5 <= n.age AND n.prop IN [1, 2] AND "
      "n.name STARTS WITH 'A' AND n.deleted IS NULL AND "
      "n.email IS NOT NULL RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::Selections &selections =
      planner_query->RequireSingle().query_graph.selections;

  ASSERT_EQ(selections.size(), 6U);

  const auto id_predicates = selections.PropertyPredicates("n", "id", "=");
  ASSERT_EQ(id_predicates.size(), 1U);
  EXPECT_EQ(id_predicates[0]->kind, ir::PredicateKind::kPropertyEquality);
  ASSERT_NE(id_predicates[0]->property_value, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*id_predicates[0]->property_value), "1");

  const auto age_predicates = selections.PropertyPredicates("n", "age", ">=");
  ASSERT_EQ(age_predicates.size(), 1U);
  EXPECT_EQ(age_predicates[0]->kind, ir::PredicateKind::kPropertyComparison);
  ASSERT_NE(age_predicates[0]->property_value, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*age_predicates[0]->property_value), "5");

  const auto in_predicates = selections.PropertyPredicates("n", "prop", "IN");
  ASSERT_EQ(in_predicates.size(), 1U);
  EXPECT_EQ(in_predicates[0]->kind, ir::PredicateKind::kPropertyIn);
  ASSERT_NE(in_predicates[0]->property_value, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*in_predicates[0]->property_value),
            "[1, 2]");

  const auto string_predicates =
      selections.PropertyPredicates("n", "name", "STARTS WITH");
  ASSERT_EQ(string_predicates.size(), 1U);
  EXPECT_EQ(string_predicates[0]->kind,
            ir::PredicateKind::kPropertyStringPredicate);
  ASSERT_NE(string_predicates[0]->property_value, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*string_predicates[0]->property_value),
            "'A'");

  const auto null_predicates =
      selections.PropertyPredicates("n", "deleted", "IS NULL");
  ASSERT_EQ(null_predicates.size(), 1U);
  EXPECT_EQ(null_predicates[0]->kind, ir::PredicateKind::kPropertyIsNull);
  EXPECT_EQ(null_predicates[0]->property_value, nullptr);

  const auto not_null_predicates =
      selections.PropertyPredicates("n", "email", "IS NOT NULL");
  ASSERT_EQ(not_null_predicates.size(), 1U);
  EXPECT_EQ(not_null_predicates[0]->kind,
            ir::PredicateKind::kPropertyIsNotNull);
  EXPECT_EQ(not_null_predicates[0]->property_value, nullptr);
}

TEST(PlannerQueryTest, SelectionsExposePushdownAndInequalityGroups) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE n.age > 30 AND n.age <= 50 AND n.name = 'Ada' "
      "RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::Selections &selections =
      planner_query->RequireSingle().query_graph.selections;

  const std::unordered_set<std::string> no_bound_symbols;
  EXPECT_TRUE(selections.PredicatesGiven(no_bound_symbols).empty());
  const std::unordered_set<std::string> bound_n = {"n"};
  EXPECT_EQ(selections.PredicatesGiven(bound_n).size(), 3U);

  const auto groups = selections.PropertyInequalityGroups();
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_EQ(groups[0].variable, "n");
  EXPECT_EQ(groups[0].property_key, "age");
  ASSERT_EQ(groups[0].lower_bounds.size(), 1U);
  ASSERT_EQ(groups[0].upper_bounds.size(), 1U);
  EXPECT_EQ(groups[0].lower_bounds[0]->comparison_op, ">");
  EXPECT_EQ(groups[0].upper_bounds[0]->comparison_op, "<=");
}

TEST(PlannerQueryTest, SelectionsExtractNestedPredicateInfo) {
  auto statement =
      ParseOrFail("MATCH (n) WHERE n.age > 30 OR n:Person RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::Selections &selections =
      planner_query->RequireSingle().query_graph.selections;

  ASSERT_EQ(selections.size(), 1U);
  const ir::Predicate &predicate = selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kGenericExpression);
  ASSERT_EQ(predicate.nested_properties.size(), 1U);
  EXPECT_EQ(predicate.nested_properties[0].variable, "n");
  EXPECT_EQ(predicate.nested_properties[0].property_key, "age");
  ASSERT_EQ(predicate.nested_node_labels.size(), 1U);
  EXPECT_EQ(predicate.nested_node_labels[0].variable, "n");
  EXPECT_EQ(predicate.nested_node_labels[0].labels,
            std::vector<std::string>({"Person"}));
}

TEST(PlannerQueryTest, QueryGraphApisExposeCoveredIdsAndAvailability) {
  auto statement = ParseOrFail(
      "MATCH p = (a)-[r]->(b) OPTIONAL MATCH (b)-[s]->(c) RETURN a, c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryGraph &query_graph =
      planner_query->RequireSingle().query_graph;

  EXPECT_TRUE(query_graph.HasLocalWork());
  EXPECT_FALSE(query_graph.ContainsUpdates());
  EXPECT_TRUE(query_graph.ReadOnly());
  EXPECT_TRUE(query_graph.CouldContainRead());

  const auto covered = query_graph.CoveredIdsForPatterns();
  EXPECT_TRUE(Contains(covered, "p"));
  EXPECT_TRUE(Contains(covered, "a"));
  EXPECT_TRUE(Contains(covered, "r"));
  EXPECT_TRUE(Contains(covered, "b"));
  EXPECT_FALSE(Contains(covered, "s"));
  EXPECT_FALSE(Contains(covered, "c"));

  const auto ids_without_optional =
      query_graph.IdsWithoutOptionalMatchesOrUpdates();
  EXPECT_TRUE(Contains(ids_without_optional, "a"));
  EXPECT_TRUE(Contains(ids_without_optional, "b"));
  EXPECT_FALSE(Contains(ids_without_optional, "c"));

  const auto local_available = query_graph.LocalAvailableSymbols();
  EXPECT_TRUE(Contains(local_available, "a"));
  EXPECT_TRUE(Contains(local_available, "b"));
  EXPECT_FALSE(Contains(local_available, "c"));

  const auto available = query_graph.AvailableSymbols();
  EXPECT_TRUE(Contains(available, "a"));
  EXPECT_TRUE(Contains(available, "b"));
  EXPECT_TRUE(Contains(available, "s"));
  EXPECT_TRUE(Contains(available, "c"));

  const auto all_covered = query_graph.AllCoveredIds();
  EXPECT_TRUE(Contains(all_covered, "p"));
  EXPECT_TRUE(Contains(all_covered, "r"));
  EXPECT_TRUE(Contains(all_covered, "s"));
  EXPECT_TRUE(Contains(all_covered, "c"));
}

TEST(PlannerQueryTest, QueryGraphApisSummarizeLabelsTypesAndProperties) {
  auto statement = ParseOrFail(
      "MATCH (a:Person {name: 'Alice'})-[r:KNOWS]->(b) "
      "WHERE a.age > 30 AND r:FRIEND AND b.score = 7 "
      "OPTIONAL MATCH (b:Company {size: 10})-[s:OWNS]->(c) "
      "RETURN a, b, c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryGraph &query_graph =
      planner_query->RequireSingle().query_graph;

  const auto a_labels = query_graph.LabelsOnNode("a");
  EXPECT_TRUE(Contains(a_labels, "Person"));
  EXPECT_FALSE(Contains(a_labels, "Company"));

  const auto r_types = query_graph.TypesOnRelationship("r");
  EXPECT_TRUE(Contains(r_types, "KNOWS"));
  EXPECT_TRUE(Contains(r_types, "FRIEND"));
  EXPECT_FALSE(Contains(r_types, "OWNS"));

  const auto a_properties = query_graph.KnownProperties("a");
  EXPECT_TRUE(Contains(a_properties, "name"));
  EXPECT_TRUE(Contains(a_properties, "age"));
  EXPECT_FALSE(Contains(a_properties, "size"));

  const auto b_labels = query_graph.AllPossibleLabelsOnNode("b");
  EXPECT_TRUE(Contains(b_labels, "Company"));

  const auto s_types = query_graph.AllPossibleTypesOnRelationship("s");
  EXPECT_TRUE(Contains(s_types, "OWNS"));

  const auto b_properties = query_graph.AllKnownPropertiesOnIdentifier("b");
  EXPECT_TRUE(Contains(b_properties, "score"));
  EXPECT_TRUE(Contains(b_properties, "size"));
}

TEST(PlannerQueryTest, QueryGraphApisFindConnectedComponents) {
  auto statement =
      ParseOrFail("MATCH (a)-[r]->(b), (c), (d)-[s]->(e) RETURN a, c, d");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryGraph &query_graph =
      planner_query->RequireSingle().query_graph;

  const auto components = query_graph.ConnectedComponents();
  ASSERT_EQ(components.size(), 3U);

  auto ab_component = std::find_if(
      components.begin(), components.end(), [](const auto &component) {
        return Contains(component.pattern_nodes, "a") &&
               Contains(component.pattern_nodes, "b");
      });
  ASSERT_NE(ab_component, components.end());
  ASSERT_EQ(ab_component->pattern_relationship_indices.size(), 1U);
  EXPECT_EQ(
      query_graph
          .pattern_relationships[ab_component->pattern_relationship_indices[0]]
          .variable,
      "r");
  EXPECT_TRUE(Contains(ab_component->covered_ids, "a"));
  EXPECT_TRUE(Contains(ab_component->covered_ids, "r"));
  EXPECT_TRUE(Contains(ab_component->covered_ids, "b"));

  auto c_component = std::find_if(
      components.begin(), components.end(), [](const auto &component) {
        return Contains(component.pattern_nodes, "c");
      });
  ASSERT_NE(c_component, components.end());
  EXPECT_TRUE(c_component->pattern_relationship_indices.empty());
  EXPECT_TRUE(Contains(c_component->covered_ids, "c"));

  auto de_component = std::find_if(
      components.begin(), components.end(), [](const auto &component) {
        return Contains(component.pattern_nodes, "d") &&
               Contains(component.pattern_nodes, "e");
      });
  ASSERT_NE(de_component, components.end());
  ASSERT_EQ(de_component->pattern_relationship_indices.size(), 1U);
  EXPECT_EQ(
      query_graph
          .pattern_relationships[de_component->pattern_relationship_indices[0]]
          .variable,
      "s");
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

TEST(PlannerQueryTest, InlinesPassthroughWithWhereForMultiPartQuery) {
  auto statement = ParseOrFail(
      "MATCH (n:Person) WHERE true WITH n WHERE n.age > 30 "
      "MATCH (n)-[r:KNOWS]->(m) WHERE true RETURN n, m");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_TRUE(main.query_graph.argument_ids.empty());
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "n"));
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "m"));
  ASSERT_EQ(main.query_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(main.query_graph.pattern_relationships[0].variable, "r");
  EXPECT_EQ(main.query_graph.pattern_relationships[0].types,
            std::vector<std::string>({"KNOWS"}));
  EXPECT_EQ(main.query_graph.selections.size(), 3U);
  const auto where_dependencies =
      SelectionDependenciesByExpression(main.query_graph);
  EXPECT_TRUE(Contains(where_dependencies.at("n:Person"), "n"));
  EXPECT_TRUE(where_dependencies.contains("true"));
  EXPECT_TRUE(Contains(where_dependencies.at("n.age > 30"), "n"));

  ASSERT_EQ(main.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  ASSERT_EQ(main.horizon.RequireRegularProjection().items.size(), 2U);
  EXPECT_EQ(main.horizon.RequireRegularProjection().items[0].alias, "n");
  EXPECT_EQ(main.horizon.RequireRegularProjection().items[1].alias, "m");
  EXPECT_TRUE(main.horizon.RequireRegularProjection().selections.empty());
  EXPECT_EQ(main.tail, nullptr);
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

TEST(PlannerQueryTest, KeepsWithHorizonWhenProjectionDropsVariables) {
  auto statement =
      ParseOrFail("MATCH (a), (b) WITH a MATCH (a)-->(c) RETURN c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);
  const ir::SinglePlannerQuery &second = *first.tail;

  EXPECT_TRUE(Contains(first.query_graph.pattern_nodes, "a"));
  EXPECT_TRUE(Contains(first.query_graph.pattern_nodes, "b"));
  ASSERT_EQ(first.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  ASSERT_EQ(first.horizon.RequireRegularProjection().items.size(), 1U);
  EXPECT_EQ(first.horizon.RequireRegularProjection().items[0].alias, "a");

  EXPECT_TRUE(Contains(second.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(second.query_graph.argument_ids, "b"));
  EXPECT_TRUE(Contains(second.query_graph.pattern_nodes, "c"));
}

TEST(PlannerQueryTest, KeepsDistinctWithAsEventHorizon) {
  auto statement =
      ParseOrFail("MATCH (n) WITH DISTINCT n MATCH (n)-->(m) RETURN m");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);

  ASSERT_EQ(first.horizon.kind, ir::QueryHorizonKind::kDistinctProjection);
  ASSERT_EQ(first.horizon.RequireDistinctProjection().grouping_items.size(),
            1U);
  EXPECT_EQ(first.horizon.RequireDistinctProjection().grouping_items[0].alias,
            "n");
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

TEST(PlannerQueryTest, ClassifiesNotExistsSubqueryPredicate) {
  auto statement = ParseOrFail(
      "MATCH (a) WHERE NOT EXISTS { MATCH (a)-[r]->(b) RETURN 1 AS ok } "
      "RETURN a");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &query = planner_query->RequireSingle();

  ASSERT_EQ(query.query_graph.selections.size(), 1U);
  const auto &predicate = query.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kNotExistsSubquery);
  ASSERT_NE(predicate.subquery, nullptr);
  const ir::SinglePlannerQuery &subquery = predicate.subquery->RequireSingle();
  EXPECT_TRUE(Contains(subquery.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "b"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "r"));
}

TEST(PlannerQueryTest, InlinedWithExistsDoesNotCaptureFutureVariables) {
  auto statement = ParseOrFail(
      "MATCH (a) WITH a WHERE EXISTS { MATCH (b) RETURN 1 AS ok } "
      "MATCH (b) RETURN a");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();
  EXPECT_EQ(main.tail, nullptr);
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "a"));
  EXPECT_TRUE(Contains(main.query_graph.pattern_nodes, "b"));
  ASSERT_EQ(main.query_graph.selections.size(), 1U);

  const ir::Predicate &predicate = main.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kExistsSubquery);
  ASSERT_NE(predicate.subquery, nullptr);
  const ir::SinglePlannerQuery &subquery = predicate.subquery->RequireSingle();
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "a"));
  EXPECT_FALSE(Contains(subquery.query_graph.argument_ids, "b"));
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

TEST(PlannerQueryTest, BuildsProjectionPositionAndInterestingOrder) {
  auto statement = ParseOrFail("MATCH (n) RETURN n.name AS name ORDER BY name");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::RegularQueryProjection &projection =
      planner_query->RequireSingle().horizon.RequireRegularProjection();

  EXPECT_EQ(projection.position, ir::ProjectionPosition::kFinal);
  ASSERT_EQ(projection.interesting_order.required_order.size(), 1U);
  ASSERT_EQ(projection.interesting_order.candidates.size(), 1U);
  ASSERT_EQ(projection.interesting_order.reverse_projection.size(), 1U);
  EXPECT_EQ(projection.interesting_order.reverse_projection[0].projected_alias,
            "name");
  ASSERT_NE(
      projection.interesting_order.reverse_projection[0].source_expression,
      nullptr);
  EXPECT_EQ(ast::ExpressionToString(
                *projection.interesting_order.reverse_projection[0]
                     .source_expression),
            "n.name");
}

TEST(PlannerQueryTest, MarksWithProjectionAsIntermediate) {
  auto statement = ParseOrFail("MATCH (n) WITH n.name AS name RETURN name");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_EQ(first.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
  EXPECT_EQ(first.horizon.RequireRegularProjection().position,
            ir::ProjectionPosition::kIntermediate);
  ASSERT_TRUE(first.tail);
  EXPECT_EQ(first.tail->horizon.RequireRegularProjection().position,
            ir::ProjectionPosition::kFinal);
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

TEST(PlannerQueryTest, AccumulatesSequentialOptionalMatchArguments) {
  auto statement = ParseOrFail(
      "MATCH (a) OPTIONAL MATCH (a)-[r]->(b) "
      "OPTIONAL MATCH (b)-[s]->(c) RETURN c");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::QueryGraph &query_graph =
      planner_query->RequireSingle().query_graph;

  ASSERT_EQ(query_graph.optional_matches.size(), 2U);
  const ir::QueryGraph &first_optional = query_graph.optional_matches[0];
  EXPECT_TRUE(Contains(first_optional.argument_ids, "a"));
  EXPECT_FALSE(Contains(first_optional.argument_ids, "b"));

  const ir::QueryGraph &second_optional = query_graph.optional_matches[1];
  EXPECT_TRUE(Contains(second_optional.argument_ids, "b"));
  EXPECT_FALSE(Contains(second_optional.argument_ids, "a"));
  EXPECT_FALSE(Contains(second_optional.argument_ids, "c"));
}

TEST(PlannerQueryTest, AddsAssertIsNodeForStandaloneArgumentPatternNode) {
  auto statement =
      ParseOrFail("MATCH (n) WITH n, 1 AS keep MATCH (n) RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &first = planner_query->RequireSingle();
  ASSERT_TRUE(first.tail);
  const ir::QueryGraph &second_graph = first.tail->query_graph;

  EXPECT_TRUE(Contains(second_graph.argument_ids, "n"));
  EXPECT_TRUE(Contains(second_graph.pattern_nodes, "n"));
  EXPECT_TRUE(Contains(second_graph.assert_is_node_variables, "n"));
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

TEST(PlannerQueryTest, BuildsStandaloneProcedureCallHorizon) {
  auto statement = ParseOrFail("CALL db.labels()");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_TRUE(main.query_graph.pattern_nodes.empty());
  ASSERT_EQ(main.horizon.kind, ir::QueryHorizonKind::kProcedureCall);
  const ir::ProcedureCallHorizon &call = main.horizon.RequireProcedureCall();
  EXPECT_EQ(call.procedure_name, "db.labels");
  EXPECT_TRUE(call.read_only);
  ASSERT_EQ(call.yield_items.size(), 1U);
  EXPECT_EQ(call.yield_items[0].result_field,
            std::optional<std::string>("label"));
  EXPECT_EQ(call.yield_items[0].variable, "label");
}

TEST(PlannerQueryTest, BuildsInQueryProcedureCallWithYieldWhere) {
  auto statement = ParseOrFail(
      "MATCH (n) CALL db.labels() YIELD label AS l WHERE l <> '' "
      "RETURN n, l");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &call_segment = planner_query->RequireSingle();

  EXPECT_TRUE(Contains(call_segment.query_graph.pattern_nodes, "n"));
  ASSERT_EQ(call_segment.horizon.kind, ir::QueryHorizonKind::kProcedureCall);
  const ir::ProcedureCallHorizon &call =
      call_segment.horizon.RequireProcedureCall();
  EXPECT_EQ(call.procedure_name, "db.labels");
  ASSERT_EQ(call.yield_items.size(), 1U);
  EXPECT_EQ(call.yield_items[0].result_field,
            std::optional<std::string>("label"));
  EXPECT_EQ(call.yield_items[0].variable, "l");
  ASSERT_EQ(call.yield_selections.size(), 1U);
  EXPECT_TRUE(Contains(call.yield_selections.predicates[0].dependencies, "l"));

  ASSERT_TRUE(call_segment.tail);
  const ir::SinglePlannerQuery &return_segment = *call_segment.tail;
  EXPECT_TRUE(Contains(return_segment.query_graph.argument_ids, "n"));
  EXPECT_TRUE(Contains(return_segment.query_graph.argument_ids, "l"));
}

TEST(PlannerQueryTest, MarksUnknownProcedureCallsAsNotReadOnly) {
  auto statement =
      ParseOrFail("MATCH (n) CALL db.unknown(n) YIELD value RETURN value");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::ProcedureCallHorizon &call =
      planner_query->RequireSingle().horizon.RequireProcedureCall();

  EXPECT_FALSE(call.read_only);
  ASSERT_EQ(call.arguments.size(), 1U);
  EXPECT_EQ(ast::ExpressionToString(*call.arguments[0]), "n");
  ASSERT_EQ(call.yield_items.size(), 1U);
  EXPECT_EQ(call.yield_items[0].variable, "value");
}

TEST(PlannerQueryTest, SkipsPassthroughWithAfterUnwindSegment) {
  auto statement = ParseOrFail("UNWIND [1, 2] AS x WITH x RETURN x");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &unwind_segment = planner_query->RequireSingle();
  ASSERT_EQ(unwind_segment.horizon.kind, ir::QueryHorizonKind::kUnwind);
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

TEST(PlannerQueryTest, BuildsNestedIRExpressionForPatternExists) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE EXISTS { (n)-[r]->(m) WHERE m.age > 1 } RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.selections.size(), 1U);
  const ir::Predicate &predicate = main.query_graph.selections.predicates[0];
  EXPECT_EQ(predicate.kind, ir::PredicateKind::kExistsSubquery);
  ASSERT_EQ(predicate.nested_expressions.size(), 1U);
  const ir::NestedIRExpression &nested = predicate.nested_expressions[0];
  EXPECT_EQ(nested.kind, ir::NestedIRExpressionKind::kExists);
  EXPECT_TRUE(Contains(nested.dependencies, "n"));
  ASSERT_NE(nested.query, nullptr);
  EXPECT_EQ(predicate.subquery, nested.query.get());

  const ir::SinglePlannerQuery &subquery = nested.query->RequireSingle();
  EXPECT_TRUE(Contains(subquery.query_graph.argument_ids, "n"));
  EXPECT_TRUE(Contains(subquery.query_graph.pattern_nodes, "n"));
  EXPECT_TRUE(Contains(subquery.query_graph.pattern_nodes, "m"));
  ASSERT_EQ(subquery.query_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(subquery.query_graph.pattern_relationships[0].variable, "r");
}

TEST(PlannerQueryTest, BuildsNestedIRExpressionForPatternComprehension) {
  auto statement =
      ParseOrFail("MATCH (n) RETURN [(n)-[r]->(m) | m.name] AS names");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::RegularQueryProjection &projection =
      planner_query->RequireSingle().horizon.RequireRegularProjection();

  ASSERT_EQ(projection.nested_expressions.size(), 1U);
  const ir::NestedIRExpression &nested = projection.nested_expressions[0];
  EXPECT_EQ(nested.kind, ir::NestedIRExpressionKind::kList);
  EXPECT_TRUE(Contains(nested.dependencies, "n"));
  EXPECT_FALSE(nested.value_variable.empty());
  EXPECT_FALSE(nested.collection_variable.empty());
  ASSERT_NE(nested.query, nullptr);

  const ir::SinglePlannerQuery &subquery = nested.query->RequireSingle();
  EXPECT_TRUE(Contains(subquery.query_graph.argument_ids, "n"));
  EXPECT_TRUE(Contains(subquery.query_graph.pattern_nodes, "n"));
  EXPECT_TRUE(Contains(subquery.query_graph.pattern_nodes, "m"));
  ASSERT_EQ(subquery.query_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(subquery.query_graph.pattern_relationships[0].variable, "r");
  const ir::RegularQueryProjection &nested_projection =
      subquery.horizon.RequireRegularProjection();
  ASSERT_EQ(nested_projection.items.size(), 1U);
  EXPECT_EQ(ast::ExpressionToString(*nested_projection.items[0].expression),
            "m.name");
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

TEST(PlannerQueryTest, DeduplicatesNormalizedPropertyPredicates) {
  auto statement = ParseOrFail(
      "MATCH (n) WHERE 1 = n.id AND n.id = 1 AND "
      "n.name STARTS WITH 'A' AND n.name STARTS WITH 'A' AND "
      "n.prop IN [1, 2] AND n.prop IN [1, 2] AND "
      "n.deleted IS NULL AND n.deleted IS NULL RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  EXPECT_EQ(main.query_graph.selections.size(), 4U);
  EXPECT_EQ(main.query_graph.selections
                .PredicatesByKind(ir::PredicateKind::kPropertyEquality)
                .size(),
            1U);
  EXPECT_EQ(main.query_graph.selections
                .PredicatesByKind(ir::PredicateKind::kPropertyStringPredicate)
                .size(),
            1U);
  EXPECT_EQ(main.query_graph.selections
                .PredicatesByKind(ir::PredicateKind::kPropertyIn)
                .size(),
            1U);
  EXPECT_EQ(main.query_graph.selections
                .PredicatesByKind(ir::PredicateKind::kPropertyIsNull)
                .size(),
            1U);
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
  EXPECT_TRUE(union_query.distinct);
  ASSERT_EQ(union_query.mappings.size(), 1U);
  EXPECT_EQ(union_query.mappings[0].output_variable, "x");
  EXPECT_EQ(union_query.mappings[0].lhs_variable, "x");
  EXPECT_EQ(union_query.mappings[0].rhs_variable, "x");
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
  EXPECT_FALSE(union_query.distinct);
  ASSERT_EQ(union_query.mappings.size(), 1U);
  EXPECT_EQ(union_query.mappings[0].output_variable, "a");
  EXPECT_EQ(union_query.mappings[0].lhs_variable, "a");
  EXPECT_EQ(union_query.mappings[0].rhs_variable, "a");
}

TEST(PlannerQueryTest, BuildsCreateMutatingPattern) {
  auto statement = ParseOrFail(
      "CREATE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2024}]->(b) "
      "RETURN a");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.mutating_patterns.size(), 1U);
  const ir::MutatingPattern &mutation = main.query_graph.mutating_patterns[0];
  EXPECT_EQ(mutation.kind, ir::MutatingPatternKind::kCreate);
  EXPECT_NE(mutation.clause, nullptr);
  ASSERT_EQ(mutation.create.nodes.size(), 2U);
  EXPECT_EQ(mutation.create.nodes[0].variable, "a");
  EXPECT_EQ(mutation.create.nodes[0].labels,
            std::vector<std::string>({"Person"}));
  ASSERT_EQ(mutation.create.nodes[0].properties.entries.size(), 1U);
  EXPECT_EQ(mutation.create.nodes[0].properties.entries[0].key, "name");
  EXPECT_EQ(mutation.create.nodes[1].variable, "b");
  ASSERT_EQ(mutation.create.relationships.size(), 1U);
  EXPECT_EQ(mutation.create.relationships[0].variable, "r");
  EXPECT_EQ(mutation.create.relationships[0].left_node, "a");
  EXPECT_EQ(mutation.create.relationships[0].right_node, "b");
  EXPECT_EQ(mutation.create.relationships[0].types,
            std::vector<std::string>({"KNOWS"}));
  ASSERT_EQ(mutation.create.relationships[0].properties.entries.size(), 1U);
  EXPECT_EQ(mutation.create.relationships[0].properties.entries[0].key,
            "since");
  EXPECT_EQ(mutation.create.commands.size(), 3U);
  EXPECT_EQ(main.horizon.kind, ir::QueryHorizonKind::kRegularProjection);
}

TEST(PlannerQueryTest, KeepsBoundCreateEndpointsOutOfCreateCommands) {
  auto statement = ParseOrFail("MATCH (a) CREATE (a)-[r:KNOWS]->(b) RETURN r");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.mutating_patterns.size(), 1U);
  const ir::MutatingPattern &mutation = main.query_graph.mutating_patterns[0];
  ASSERT_EQ(mutation.kind, ir::MutatingPatternKind::kCreate);
  ASSERT_EQ(mutation.create.nodes.size(), 2U);
  EXPECT_EQ(mutation.create.nodes[0].variable, "a");
  EXPECT_TRUE(mutation.create.nodes[0].previously_bound);
  EXPECT_EQ(mutation.create.nodes[1].variable, "b");
  EXPECT_FALSE(mutation.create.nodes[1].previously_bound);
  ASSERT_EQ(mutation.create.relationships.size(), 1U);
  EXPECT_EQ(mutation.create.relationships[0].left_node, "a");
  EXPECT_EQ(mutation.create.relationships[0].right_node, "b");
  ASSERT_EQ(mutation.create.commands.size(), 2U);
  EXPECT_EQ(mutation.create.commands[0].kind, ir::CreateEntityKind::kNode);
  EXPECT_EQ(mutation.create.commands[0].index, 1U);
  EXPECT_EQ(mutation.create.commands[1].kind,
            ir::CreateEntityKind::kRelationship);
  EXPECT_EQ(mutation.create.commands[1].index, 0U);
}

TEST(PlannerQueryTest, BuildsUpdatingOnlyCreateWithPassthroughHorizon) {
  auto statement = ParseOrFail("CREATE (n)");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.mutating_patterns.size(), 1U);
  EXPECT_EQ(main.query_graph.mutating_patterns[0].kind,
            ir::MutatingPatternKind::kCreate);
  EXPECT_EQ(main.horizon.kind, ir::QueryHorizonKind::kPassthrough);
  EXPECT_EQ(main.tail, nullptr);

  const std::string printed = ir::PlannerQueryToString(*planner_query);
  EXPECT_NE(printed.find("passthrough"), std::string::npos);
  EXPECT_NE(printed.find("clause: CREATE (n)"), std::string::npos);
}

TEST(PlannerQueryTest, BuildsSetDeleteAndRemoveMutatingPatterns) {
  auto statement = ParseOrFail(
      "MATCH (n) SET n.name = 'Ada', n = {age: 42}, n += {score: 7}, "
      "n:New REMOVE n:Old, n.name DELETE n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.mutating_patterns.size(), 3U);
  const ir::MutatingPattern &set = main.query_graph.mutating_patterns[0];
  EXPECT_EQ(set.kind, ir::MutatingPatternKind::kSet);
  ASSERT_EQ(set.set_patterns.size(), 4U);
  EXPECT_EQ(set.set_patterns[0].kind, ir::SetMutatingPatternKind::kSetProperty);
  EXPECT_EQ(set.set_patterns[0].property_key, "name");
  EXPECT_EQ(set.set_patterns[1].kind,
            ir::SetMutatingPatternKind::kSetExactPropertiesFromMap);
  EXPECT_EQ(set.set_patterns[2].kind,
            ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap);
  EXPECT_EQ(set.set_patterns[3].kind, ir::SetMutatingPatternKind::kSetLabels);
  EXPECT_EQ(set.set_patterns[3].labels, std::vector<std::string>({"New"}));

  const ir::MutatingPattern &remove = main.query_graph.mutating_patterns[1];
  EXPECT_EQ(remove.kind, ir::MutatingPatternKind::kRemove);
  ASSERT_EQ(remove.remove_patterns.size(), 2U);
  EXPECT_EQ(remove.remove_patterns[0].kind,
            ir::RemoveMutatingPatternKind::kRemoveLabels);
  EXPECT_EQ(remove.remove_patterns[0].labels,
            std::vector<std::string>({"Old"}));
  EXPECT_EQ(remove.remove_patterns[1].kind,
            ir::RemoveMutatingPatternKind::kRemoveProperty);
  EXPECT_EQ(remove.remove_patterns[1].property_key, "name");

  const ir::MutatingPattern &del = main.query_graph.mutating_patterns[2];
  EXPECT_EQ(del.kind, ir::MutatingPatternKind::kDelete);
  ASSERT_EQ(del.delete_patterns.size(), 1U);
  EXPECT_FALSE(del.delete_patterns[0].detach);
  ASSERT_NE(del.delete_patterns[0].expression, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*del.delete_patterns[0].expression), "n");
  EXPECT_EQ(main.horizon.kind, ir::QueryHorizonKind::kPassthrough);
}

TEST(PlannerQueryTest, BuildsDetachDeleteMutatingPattern) {
  auto statement = ParseOrFail("MATCH (n) DETACH DELETE n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &main = planner_query->RequireSingle();

  ASSERT_EQ(main.query_graph.mutating_patterns.size(), 1U);
  const ir::MutatingPattern &mutation = main.query_graph.mutating_patterns[0];
  EXPECT_EQ(mutation.kind, ir::MutatingPatternKind::kDelete);
  ASSERT_EQ(mutation.delete_patterns.size(), 1U);
  EXPECT_TRUE(mutation.delete_patterns[0].detach);
  ASSERT_NE(mutation.delete_patterns[0].expression, nullptr);
  EXPECT_EQ(ast::ExpressionToString(*mutation.delete_patterns[0].expression),
            "n");
}

TEST(PlannerQueryTest, IsolatesMergeInPassthroughSegment) {
  auto statement = ParseOrFail(
      "MATCH (m) MERGE (n) ON CREATE SET n.created = true RETURN n");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &match_segment = planner_query->RequireSingle();

  EXPECT_TRUE(Contains(match_segment.query_graph.pattern_nodes, "m"));
  EXPECT_TRUE(match_segment.query_graph.mutating_patterns.empty());
  EXPECT_EQ(match_segment.horizon.kind, ir::QueryHorizonKind::kPassthrough);
  ASSERT_NE(match_segment.tail, nullptr);

  const ir::SinglePlannerQuery &merge_segment = *match_segment.tail;
  ASSERT_EQ(merge_segment.query_graph.mutating_patterns.size(), 1U);
  const ir::MutatingPattern &merge =
      merge_segment.query_graph.mutating_patterns[0];
  EXPECT_EQ(merge.kind, ir::MutatingPatternKind::kMerge);
  ASSERT_EQ(merge.merge.create_pattern.nodes.size(), 1U);
  EXPECT_EQ(merge.merge.create_pattern.nodes[0].variable, "n");
  EXPECT_TRUE(Contains(merge.merge.match_graph.pattern_nodes, "n"));
  EXPECT_TRUE(merge.merge.match_graph.pattern_relationships.empty());
  ASSERT_EQ(merge.merge.actions.size(), 1U);
  EXPECT_FALSE(merge.merge.actions[0].on_match);
  ASSERT_EQ(merge.merge.actions[0].set_patterns.size(), 1U);
  EXPECT_EQ(merge.merge.actions[0].set_patterns[0].kind,
            ir::SetMutatingPatternKind::kSetProperty);
  EXPECT_EQ(merge.merge.actions[0].set_patterns[0].property_key, "created");
  EXPECT_EQ(merge_segment.horizon.kind, ir::QueryHorizonKind::kPassthrough);
  ASSERT_NE(merge_segment.tail, nullptr);

  const ir::SinglePlannerQuery &return_segment = *merge_segment.tail;
  EXPECT_TRUE(return_segment.query_graph.mutating_patterns.empty());
  EXPECT_TRUE(Contains(return_segment.query_graph.argument_ids, "n"));
  EXPECT_EQ(return_segment.horizon.kind,
            ir::QueryHorizonKind::kRegularProjection);
}

TEST(PlannerQueryTest, BuildsRelationshipMergeMatchGraph) {
  auto statement = ParseOrFail(
      "MATCH (m) MERGE (m)-[r:KNOWS {since: 2020}]->"
      "(n:Person {id: m.id}) ON MATCH SET r.seen = true RETURN r");
  ASSERT_TRUE(statement);

  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  const ir::SinglePlannerQuery &match_segment = planner_query->RequireSingle();
  ASSERT_NE(match_segment.tail, nullptr);
  const ir::SinglePlannerQuery &merge_segment = *match_segment.tail;

  ASSERT_EQ(merge_segment.query_graph.mutating_patterns.size(), 1U);
  const ir::MutatingPattern &merge =
      merge_segment.query_graph.mutating_patterns[0];
  ASSERT_EQ(merge.kind, ir::MutatingPatternKind::kMerge);
  EXPECT_TRUE(Contains(merge.merge.match_graph.argument_ids, "m"));
  ASSERT_EQ(merge.merge.create_pattern.nodes.size(), 2U);
  EXPECT_TRUE(merge.merge.create_pattern.nodes[0].previously_bound);
  EXPECT_FALSE(merge.merge.create_pattern.nodes[1].previously_bound);
  ASSERT_EQ(merge.merge.create_pattern.commands.size(), 2U);
  EXPECT_EQ(merge.merge.create_pattern.commands[0].kind,
            ir::CreateEntityKind::kNode);
  EXPECT_EQ(merge.merge.create_pattern.commands[0].index, 1U);
  EXPECT_EQ(merge.merge.create_pattern.commands[1].kind,
            ir::CreateEntityKind::kRelationship);
  EXPECT_EQ(merge.merge.create_pattern.commands[1].index, 0U);
  EXPECT_TRUE(Contains(merge.merge.match_graph.pattern_nodes, "m"));
  EXPECT_TRUE(Contains(merge.merge.match_graph.pattern_nodes, "n"));
  ASSERT_EQ(merge.merge.match_graph.pattern_relationships.size(), 1U);
  EXPECT_EQ(merge.merge.match_graph.pattern_relationships[0].variable, "r");
  EXPECT_EQ(merge.merge.match_graph.pattern_relationships[0].types,
            std::vector<std::string>({"KNOWS"}));
  ASSERT_EQ(merge.merge.match_graph.node_labels.size(), 1U);
  EXPECT_EQ(merge.merge.match_graph.node_labels[0].variable, "n");
  EXPECT_EQ(merge.merge.match_graph.node_labels[0].labels,
            std::vector<std::string>({"Person"}));
  ASSERT_EQ(merge.merge.match_graph.property_equalities.size(), 2U);
  EXPECT_EQ(merge.merge.match_graph.property_equalities[0].variable, "n");
  EXPECT_EQ(merge.merge.match_graph.property_equalities[0].property_key, "id");
  EXPECT_EQ(merge.merge.match_graph.property_equalities[1].variable, "r");
  EXPECT_EQ(merge.merge.match_graph.property_equalities[1].property_key,
            "since");
  ASSERT_EQ(merge.merge.actions.size(), 1U);
  EXPECT_TRUE(merge.merge.actions[0].on_match);
  ASSERT_EQ(merge.merge.actions[0].set_patterns.size(), 1U);
  EXPECT_EQ(merge.merge.actions[0].set_patterns[0].property_key, "seen");
}

TEST(PlannerQueryTest, SinglePlannerQueryIsMoveOnly) {
  EXPECT_FALSE(std::is_copy_constructible_v<ir::SinglePlannerQuery>);
  EXPECT_FALSE(std::is_copy_assignable_v<ir::SinglePlannerQuery>);
  EXPECT_TRUE(std::is_move_constructible_v<ir::SinglePlannerQuery>);
  EXPECT_TRUE(std::is_move_assignable_v<ir::SinglePlannerQuery>);
}
