#include "runtime/query_executor.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_exception.h"
#include "common/exception.h"
#include "ir/logical_plan_builder.h"
#include "ir/planner_query.h"
#include "storage/in_memory_graph.h"

namespace {

void SeedDemoGraph(rg::InMemoryGraph *graph) {
  auto ada = graph->CreateNode(
      {"Person"}, {{"name", rg::Value("Ada")}, {"age", rg::Value(36)}});
  auto grace = graph->CreateNode(
      {"Person"}, {{"name", rg::Value("Grace")}, {"age", rg::Value(85)}});
  auto cpp = graph->CreateNode({"Language"}, {{"name", rg::Value("C++")}});
  graph->CreateRelationship(ada, grace, "KNOWS", {{"since", rg::Value(2020)}});
  graph->CreateRelationship(grace, cpp, "USES", {{"since", rg::Value(1970)}});
  graph->AddNodeIndex({"Person"}, "name");
  graph->AddRelationshipIndex({"KNOWS"}, "since");
}

rg::QueryOptions QueryOptionsFor(const rg::InMemoryGraph &graph) {
  return rg::QueryOptions{.planner_statistics = &graph,
                          .planner_catalog = &graph};
}

std::vector<std::vector<std::string>> StringRows(
    const rg::QueryResult &result) {
  std::vector<std::vector<std::string>> rows;
  rows.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    std::vector<std::string> values;
    values.reserve(row.size());
    for (const auto &value : row) {
      values.push_back(value.ToString());
    }
    rows.push_back(std::move(values));
  }
  return rows;
}

std::unique_ptr<ir::LogicalPlan> LogicalPlanFor(const rg::InMemoryGraph &graph,
                                                const std::string &cypher) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(cypher);
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  return ir::CreateLogicalPlan(
      *planner_query, ir::LogicalPlanBuilderOptions{
                          .max_idp_candidates_per_relationship_count = 128,
                          .planner_statistics = &graph,
                          .planner_catalog = &graph});
}

const ir::LogicalPlan *FindPlanNode(const ir::LogicalPlan &plan,
                                    ir::LogicalPlanNodeType type) {
  if (plan.Type() == type) {
    return &plan;
  }
  for (const auto &child : plan.Children()) {
    if (child == nullptr) {
      continue;
    }
    const ir::LogicalPlan *found = FindPlanNode(*child, type);
    if (found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

TEST(QueryExecutorTest, ExecutesNodeLabelAndPropertyQuery) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Ada' RETURN n.name AS name");

  ASSERT_EQ(result.columns, std::vector<std::string>{"name"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Ada\""}}));
}

TEST(QueryExecutorTest, ExecutesQueriesWithParameters) {
  rg::InMemoryGraph graph;
  graph.AddNodeIndex({"Person"}, "name");
  rg::QueryOptions options = QueryOptionsFor(graph);
  options.parameters = {
      {"name", rg::Value("Ada")},
      {"ages", rg::Value(rg::Value::List{rg::Value(36), rg::Value(85)})},
      {"s", rg::Value(1)},
      {"l", rg::Value(1)},
      {"properties", rg::Value(rg::Value::Map{{"name", rg::Value("Grace")},
                                              {"age", rg::Value(85)}})}};

  rg::ExecuteWriteQuery(graph, "CREATE (:Person {name: $name, age: $ages[0]})",
                        options);
  rg::ExecuteWriteQuery(graph, "CREATE (:Person $properties)", options);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) WHERE n.name = $name OR n.age IN $ages "
      "RETURN n.name AS name ORDER BY name SKIP $s LIMIT $l",
      options);

  ASSERT_EQ(result.columns, std::vector<std::string>{"name"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Grace\""}}));
}

TEST(QueryExecutorTest, RejectsMissingQueryParameters) {
  rg::InMemoryGraph graph;

  EXPECT_THROW((void)rg::ExecuteReadQuery(graph, "RETURN $missing AS value"),
               common::InvalidArgumentError);
}

TEST(QueryExecutorTest, ImplementsThreeValuedBooleanLogic) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "RETURN true AND null AS true_and_null, "
                           "false AND null AS false_and_null, "
                           "true OR null AS true_or_null, "
                           "false OR null AS false_or_null, "
                           "true XOR null AS xor_null, NOT null AS not_null");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"true_and_null", "false_and_null",
                                      "true_or_null", "false_or_null",
                                      "xor_null", "not_null"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"null", "false", "true", "null", "null", "null"}}));
}

TEST(QueryExecutorTest, ShortCircuitsBooleanExpressions) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "RETURN false AND (1 / 0 = 1) AS conjunction, "
                           "true OR (1 / 0 = 1) AS disjunction");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"conjunction", "disjunction"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"false", "true"}}));
}

TEST(QueryExecutorTest, PropagatesNullThroughScalarOperators) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN null = null AS equals_null, null <> 1 AS not_equals_null, "
      "null < 1 AS less_null, 1 + null AS add_null, -null AS negate_null, "
      "null STARTS WITH 'a' AS string_null, null:Person AS label_null");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"equals_null", "not_equals_null",
                                      "less_null", "add_null", "negate_null",
                                      "string_null", "label_null"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"null", "null", "null", "null", "null", "null", "null"}}));
}

TEST(QueryExecutorTest, PropagatesNullThroughCollectionEquality) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "RETURN [null] = [null] AS list_unknown, "
                           "[1, null] = [2, null] AS list_false, "
                           "[1, null] IN [[1, null]] AS in_unknown");

  ASSERT_EQ(result.columns, (std::vector<std::string>{
                                "list_unknown", "list_false", "in_unknown"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"null", "false", "null"}}));
}

TEST(QueryExecutorTest, ImplementsNullAwareInAndQuantifierSemantics) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN 1 IN [null, 1] AS in_match, 2 IN [null, 1] AS in_unknown, "
      "null IN [] AS null_in_empty, "
      "ALL(x IN [true, null] WHERE x) AS all_unknown, "
      "ALL(x IN [false, null] WHERE x) AS all_false, "
      "ANY(x IN [false, null] WHERE x) AS any_unknown, "
      "ANY(x IN [true, null] WHERE x) AS any_true, "
      "NONE(x IN [false, null] WHERE x) AS none_unknown, "
      "SINGLE(x IN [true, null] WHERE x) AS single_unknown, "
      "SINGLE(x IN [true, true, null] WHERE x) AS single_false");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"in_match", "in_unknown", "null_in_empty",
                                      "all_unknown", "all_false", "any_unknown",
                                      "any_true", "none_unknown",
                                      "single_unknown", "single_false"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"true", "null", "false", "null", "false", "null", "true",
                 "null", "null", "false"}}));
}

TEST(QueryExecutorTest, UsesNumericEqualityAcrossIntegerAndDoubleValues) {
  rg::InMemoryGraph graph;

  rg::QueryResult comparison = rg::ExecuteReadQuery(
      graph, "RETURN 1 = 1.0 AS equal, 1 IN [1.0] AS contained");
  rg::QueryResult aggregation = rg::ExecuteReadQuery(
      graph, "UNWIND [1, 1.0] AS x RETURN count(DISTINCT x) AS distinct_count");
  rg::QueryResult nested = rg::ExecuteReadQuery(
      graph,
      "UNWIND [[1], [1.0]] AS x RETURN count(DISTINCT x) AS distinct_count");
  rg::QueryResult precise =
      rg::ExecuteReadQuery(graph,
                           "UNWIND [1.0000001, 1.0000002] AS x "
                           "RETURN count(DISTINCT x) AS distinct_count");
  rg::QueryResult boundary = rg::ExecuteReadQuery(
      graph,
      "RETURN 9223372036854775807 < 9.223372036854776e18 AS ordered, "
      "9223372036854775807 = 9.223372036854776e18 AS equal");

  ASSERT_EQ(comparison.columns,
            (std::vector<std::string>{"equal", "contained"}));
  EXPECT_EQ(StringRows(comparison),
            (std::vector<std::vector<std::string>>{{"true", "true"}}));
  ASSERT_EQ(aggregation.columns, (std::vector<std::string>{"distinct_count"}));
  EXPECT_EQ(StringRows(aggregation),
            (std::vector<std::vector<std::string>>{{"1"}}));
  EXPECT_EQ(StringRows(nested), (std::vector<std::vector<std::string>>{{"1"}}));
  EXPECT_EQ(StringRows(precise),
            (std::vector<std::vector<std::string>>{{"2"}}));
  EXPECT_EQ(StringRows(boundary),
            (std::vector<std::vector<std::string>>{{"true", "false"}}));
}

TEST(QueryExecutorTest, UsesNumericEqualityForIndexSeeks) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"Item"}, {{"score", rg::Value(1)}});
  graph.AddNodeIndex({"Item"}, "score");

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "MATCH (n:Item) WHERE n.score = 1.0 RETURN count(n) AS matches",
      QueryOptionsFor(graph));

  ASSERT_EQ(result.columns, std::vector<std::string>{"matches"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"1"}}));
}

TEST(QueryExecutorTest, RejectsInvalidPredicatesAndUnsafeIntegerArithmetic) {
  rg::InMemoryGraph graph;

  EXPECT_THROW((void)rg::ExecuteReadQuery(graph, "RETURN NOT 1 AS value"),
               ast::SemanticError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(
                   graph, "RETURN 9223372036854775807 + 1 AS value"),
               common::InvalidArgumentError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(
                   graph, "RETURN -9223372036854775807 - 2 AS value"),
               common::InvalidArgumentError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(
                   graph, "RETURN 3037000500 * 3037000500 AS value"),
               common::InvalidArgumentError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(graph, "RETURN 1 / 0 AS value"),
               common::InvalidArgumentError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(graph, "RETURN 1 % 0 AS value"),
               common::InvalidArgumentError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(
                   graph, "RETURN (-9223372036854775807 - 1) % -1 AS value"),
               common::InvalidArgumentError);
  EXPECT_THROW(
      (void)rg::ExecuteReadQuery(
          graph, "UNWIND [9223372036854775807, 1] AS x RETURN sum(x) AS value"),
      common::InvalidArgumentError);

  rg::QueryResult conversion = rg::ExecuteReadQuery(
      graph, "RETURN toInteger(9.223372036854776e18) AS value");
  EXPECT_EQ(StringRows(conversion),
            (std::vector<std::vector<std::string>>{{"null"}}));
}

TEST(QueryExecutorTest, ExecutesCypherArithmeticAndStringPredicateSemantics) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN [1, 2] + [3, 4] AS concatenated, "
      "[1, 2] + 3 AS appended, 0 + [1, 2] AS prepended, "
      "4 / 2 AS integer_division, 5.0 % 2 AS floating_modulo, "
      "1 STARTS WITH '1' AS non_string_predicate");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"concatenated", "appended", "prepended",
                                      "integer_division", "floating_modulo",
                                      "non_string_predicate"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"[1, 2, 3, 4]", "[1, 2, 3]", "[0, 1, 2]", "2", "1", "null"}}));
}

TEST(QueryExecutorTest, ImplementsIeeeFloatingDivisionAndNanComparisons) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN 0.0 / 0.0 = 0.0 / 0.0 AS equal, "
      "0.0 / 0.0 <> 1 AS not_equal, "
      "0.0 / 0.0 < 1 AS less, 0.0 / 0.0 <= 1 AS less_equal, "
      "0.0 / 0.0 > 'a' AS cross_type, 1 < 'a' AS numeric_string");

  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"false", "true", "false", "false", "null", "null"}}));
}

TEST(QueryExecutorTest, ExecutesRelationshipExpandQuery) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (a:Person)-[r:KNOWS]->(b:Person) "
                           "RETURN a.name AS a, b.name AS b, r.since AS since");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"a", "b", "since"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Ada\"", "\"Grace\"", "2020"}}));
}

TEST(QueryExecutorTest, FiltersOptionalMatchUsingIncomingRelationshipVariable) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({"A"}, {{"name", rg::Value("a")}});
  auto b1 = graph.CreateNode({}, {{"name", rg::Value("b1")}});
  auto b2 = graph.CreateNode({}, {{"name", rg::Value("b2")}});
  auto c1 = graph.CreateNode({}, {{"name", rg::Value("c1")}});
  auto c2 = graph.CreateNode({}, {{"name", rg::Value("c2")}});
  graph.CreateRelationship(a, b1, "KNOWS", {});
  graph.CreateRelationship(b1, c1, "KNOWS", {});
  graph.CreateRelationship(a, b2, "KNOWS", {});
  graph.CreateRelationship(b2, c2, "KNOWS", {});
  graph.CreateRelationship(a, c1, "KNOWS", {});

  rg::QueryResult missing =
      rg::ExecuteReadQuery(graph,
                           "MATCH (a:A)-[:KNOWS]->(b)-->(c) "
                           "OPTIONAL MATCH (a)-[r:KNOWS]->(c) "
                           "WITH c WHERE r IS NULL RETURN c.name");
  EXPECT_EQ(StringRows(missing),
            (std::vector<std::vector<std::string>>{{"\"c2\""}}));

  rg::QueryResult present =
      rg::ExecuteReadQuery(graph,
                           "MATCH (a:A)-[:KNOWS]->(b)-->(c) "
                           "OPTIONAL MATCH (a)-[r:KNOWS]->(c) "
                           "WITH c WHERE r IS NOT NULL RETURN c.name");
  EXPECT_EQ(StringRows(present),
            (std::vector<std::vector<std::string>>{{"\"c1\""}}));
}

TEST(QueryExecutorTest, ExecutesSortSkipLimit) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) RETURN n.name AS name ORDER BY name SKIP 1 "
      "LIMIT 1");

  ASSERT_EQ(result.columns, std::vector<std::string>{"name"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Grace\""}}));
}

TEST(QueryExecutorTest, RejectsInvalidSkipAndLimitCounts) {
  rg::InMemoryGraph graph;

  EXPECT_THROW((void)rg::ExecuteReadQuery(graph, "RETURN 1 AS x SKIP -1"),
               ast::SemanticError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(graph, "RETURN 1 AS x LIMIT 1.5"),
               ast::SemanticError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(
                   graph, "MATCH (n:Missing) RETURN n LIMIT null"),
               common::InvalidArgumentError);
}

TEST(QueryExecutorTest, OrdersByPreProjectionExpression) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) RETURN n.name AS name ORDER BY n.age");

  ASSERT_EQ(result.columns, std::vector<std::string>{"name"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Ada\""}, {"\"Grace\""}}));
}

TEST(QueryExecutorTest, ExecutesCountAggregation) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) RETURN count(*) AS rows, count(n.age) AS ages");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"rows", "ages"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"2", "2"}}));
}

TEST(QueryExecutorTest, ExecutesNumericAggregations) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) RETURN sum(n.age) AS total, avg(n.age) AS average, "
      "min(n.age) AS youngest, max(n.age) AS oldest");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"total", "average",
                                                      "youngest", "oldest"}));
  EXPECT_EQ(
      StringRows(result),
      (std::vector<std::vector<std::string>>{{"121", "60.5", "36", "85"}}));
}

TEST(QueryExecutorTest, ExecutesAggregateSubexpressions) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "RETURN n.age AS age, n.age + count(*) AS total, "
      "{names: collect(n.name), count: count(*)} AS summary "
      "ORDER BY age");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"age", "total", "summary"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"36", "37", "{count: 1, names: [\"Ada\"]}"},
                {"85", "86", "{count: 1, names: [\"Grace\"]}"}}));
}

TEST(QueryExecutorTest, ExecutesAggregateExpressionsInOrderBy) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n) RETURN n:Person AS person, count(*) + 1 AS total "
      "ORDER BY count(1) + total DESC");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"person", "total"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"true", "3"}, {"false", "2"}}));
}

TEST(QueryExecutorTest, ConstructsAndAccessesTemporalValues) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "WITH datetime({year: 1984, month: 11, day: 11, hour: 12, "
      "minute: 31, second: 14, nanosecond: 645876123, "
      "timezone: 'Europe/Stockholm'}) AS d, "
      "duration({years: 1, months: 4, days: 10, hours: 1, minutes: 1, "
      "seconds: 1, nanoseconds: 111111111}) AS span "
      "RETURN toString(d) AS text, d.weekYear AS weekYear, "
      "d.offset AS offset, d.epochSeconds AS epochSeconds, "
      "span.nanoseconds AS spanNanoseconds");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"text", "weekYear", "offset",
                                      "epochSeconds", "spanNanoseconds"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"\"1984-11-11T12:31:14.645876123+01:00[Europe/Stockholm]\"",
                 "1984", "\"+01:00\"", "469020674", "3661111111111"}}));
}

TEST(QueryExecutorTest, ExecutesQuantifierOverCollectedValues) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "RETURN ALL(ok IN collect(n.age >= 36) WHERE ok) AS okay");

  ASSERT_EQ(result.columns, std::vector<std::string>{"okay"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"true"}}));
}

TEST(QueryExecutorTest, ExecutesCollectAggregation) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Person) RETURN collect(n.name) AS names, "
                           "collect(DISTINCT n.age > 40) AS older_flags");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"names", "older_flags"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"[\"Ada\", \"Grace\"]", "[false, true]"}}));
}

TEST(QueryExecutorTest, ExecutesDistinctAggregations) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"Person"}, {{"age", rg::Value(36)}});
  graph.CreateNode({"Person"}, {{"age", rg::Value(36)}});
  graph.CreateNode({"Person"}, {{"age", rg::Value(85)}});

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) RETURN count(DISTINCT n.age) AS ages, "
      "sum(DISTINCT n.age) AS total, avg(DISTINCT n.age) AS average");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"ages", "total", "average"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"2", "121", "60.5"}}));
}

TEST(QueryExecutorTest, ExecutesGroupedAggregations) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n) RETURN labels(n) AS labels, count(*) AS c "
      "ORDER BY c DESC, labels");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"labels", "c"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"[\"Person\"]", "2"},
                                                   {"[\"Language\"]", "1"}}));
}

TEST(QueryExecutorTest, AggregationsIgnoreNullValues) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n) RETURN count(n.missing) AS c, collect(n.missing) AS values, "
      "sum(n.missing) AS total, avg(n.missing) AS average, "
      "min(n.missing) AS minimum, max(n.missing) AS maximum");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"c", "values", "total", "average",
                                      "minimum", "maximum"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"0", "[]", "0", "null", "null", "null"}}));
}

TEST(QueryExecutorTest, GlobalAggregationsProduceRowForEmptyInput) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) RETURN count(*) AS rows, count(n.age) AS ages, "
      "collect(n.name) AS names, sum(n.age) AS total, avg(n.age) AS average, "
      "min(n.age) AS minimum, max(n.age) AS maximum");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"rows", "ages", "names", "total",
                                      "average", "minimum", "maximum"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"0", "0", "[]", "0", "null", "null", "null"}}));
}

TEST(QueryExecutorTest, ExecutesUnionAll) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph, "RETURN 1 AS x UNION ALL RETURN 1 AS x");

  ASSERT_EQ(result.columns, std::vector<std::string>{"x"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"1"}, {"1"}}));
}

TEST(QueryExecutorTest, ExecutesUnionDistinct) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph, "RETURN 1 AS x UNION RETURN 1 AS x");

  ASSERT_EQ(result.columns, std::vector<std::string>{"x"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"1"}}));
}

TEST(QueryExecutorTest, ExecutesDbLabelsProcedure) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(graph, "CALL db.labels()");

  ASSERT_EQ(result.columns, std::vector<std::string>{"label"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Language\""}, {"\"Person\""}}));
}

TEST(QueryExecutorTest, ExecutesDbLabelsProcedureWithYieldWhere) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "CALL db.labels() YIELD label AS l WHERE l = 'Person' RETURN l");

  ASSERT_EQ(result.columns, std::vector<std::string>{"l"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Person\""}}));
}

TEST(QueryExecutorTest, ExecutesDbRelationshipTypesProcedure) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph, "CALL db.relationshipTypes()");

  ASSERT_EQ(result.columns, std::vector<std::string>{"relationshipType"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"KNOWS\""}, {"\"USES\""}}));
}

TEST(QueryExecutorTest, ExecutesDbPropertyKeysProcedureWithYieldWhere) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "CALL db.propertyKeys() YIELD propertyKey AS k "
                           "WHERE k STARTS WITH 's' RETURN k");

  ASSERT_EQ(result.columns, std::vector<std::string>{"k"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"since\""}}));
}

TEST(QueryExecutorTest, ExecutesDbmsProcedures) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "CALL dbms.procedures() "
      "YIELD name, signature, description, mode, worksOnSystem "
      "RETURN name, signature, description, mode, worksOnSystem "
      "ORDER BY name");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"name", "signature", "description",
                                      "mode", "worksOnSystem"}));
  EXPECT_EQ(
      StringRows(result),
      (std::vector<std::vector<std::string>>{
          {"\"db.labels\"", "\"db.labels() :: (label)\"",
           "\"List all node labels in the graph.\"", "\"READ\"", "false"},
          {"\"db.propertyKeys\"", "\"db.propertyKeys() :: (propertyKey)\"",
           "\"List all property keys in the graph.\"", "\"READ\"", "false"},
          {"\"db.relationshipTypes\"",
           "\"db.relationshipTypes() :: (relationshipType)\"",
           "\"List all relationship types in the graph.\"", "\"READ\"",
           "false"},
          {"\"dbms.procedures\"",
           "\"dbms.procedures() :: (name, signature, description, mode, "
           "worksOnSystem)\"",
           "\"List all built-in procedures.\"", "\"READ\"", "false"}}));
}

TEST(QueryExecutorTest, ExecutesNamedPath) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "MATCH p = (a:Person)-[r:KNOWS]->(b:Person) RETURN p");

  ASSERT_EQ(result.columns, std::vector<std::string>{"p"});
  ASSERT_EQ(result.rows.size(), 1U);
  ASSERT_EQ(result.rows[0].size(), 1U);
  ASSERT_TRUE(result.rows[0][0].IsPath());
  EXPECT_EQ(result.rows[0][0].AsPath().nodes.size(), 2U);
  EXPECT_EQ(result.rows[0][0].AsPath().relationships.size(), 1U);
}

TEST(QueryExecutorTest, ExecutesNamedPathLength) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH p = (a:Person)-[r:KNOWS]->(b:Person) "
                           "RETURN length(p) AS len");

  ASSERT_EQ(result.columns, std::vector<std::string>{"len"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"1"}}));
}

TEST(QueryExecutorTest, ExecutesPathBuiltInFunctions) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH p = (a:Person {name: 'Ada'})-[r:KNOWS]->(b:Person) "
      "RETURN size(nodes(p)) AS node_count, "
      "size(relationships(p)) AS rel_count, "
      "nodes(p)[0].name AS first, relationships(p)[0].since AS since");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"node_count", "rel_count",
                                                      "first", "since"}));
  EXPECT_EQ(
      StringRows(result),
      (std::vector<std::vector<std::string>>{{"2", "1", "\"Ada\"", "2020"}}));
}

TEST(QueryExecutorTest, ExecutesNamedCreatePathLength) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteQuery(
      graph, "CREATE p = (a)-[r:KNOWS]->(b) RETURN length(p) AS len");

  ASSERT_EQ(result.columns, std::vector<std::string>{"len"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"1"}}));
}

TEST(QueryExecutorTest, ExecutesQuantifierExpressions) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "RETURN ALL(x IN [1, 2] WHERE x > 0) AS all_ok, "
                           "ANY(x IN [1, 2] WHERE x = 2) AS any_ok, "
                           "NONE(x IN [1, 2] WHERE x = 3) AS none_ok, "
                           "SINGLE(x IN [1, 2] WHERE x = 2) AS single_ok");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"all_ok", "any_ok",
                                                      "none_ok", "single_ok"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"true", "true", "true", "true"}}));
}

TEST(QueryExecutorTest, ExecutesListIndexAndSliceExpressions) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN [10, 20, 30][1] AS second, [10, 20, 30][-1] AS last, "
      "[10, 20, 30][99] AS missing, [10, 20, 30][0..2] AS head, "
      "[10, 20, 30][1..] AS tail, [10, 20, 30][..2] AS prefix");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"second", "last", "missing", "head",
                                      "tail", "prefix"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"20", "30", "null", "[10, 20]", "[20, 30]", "[10, 20]"}}));
}

TEST(QueryExecutorTest, ExecutesScalarBuiltInFunctions) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN coalesce(null, 'fallback') AS c, isEmpty([]) AS empty_list, "
      "isEmpty('') AS empty_string, range(1, 5, 2) AS forward, "
      "range(3, 1, -1) AS backward, split('Ada,Grace', ',') AS parts, "
      "toString(42) AS text, toInteger('42') AS integer, "
      "toInteger('bad') AS bad_integer, toFloat('1.5') AS float, "
      "toBoolean('TRUE') AS truth, toLower('ADA') AS lower, "
      "toUpper('ada') AS upper, trim(' Ada ') AS trimmed");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"c", "empty_list", "empty_string",
                                      "forward", "backward", "parts", "text",
                                      "integer", "bad_integer", "float",
                                      "truth", "lower", "upper", "trimmed"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"\"fallback\"", "true", "true", "[1, 3, 5]", "[3, 2, 1]",
                 "[\"Ada\", \"Grace\"]", "\"42\"", "42", "null", "1.5", "true",
                 "\"ada\"", "\"ADA\"", "\"Ada\""}}));
}

TEST(QueryExecutorTest, ExecutesNumericListAndStringBuiltInFunctions) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN abs(-7) AS absolute, ceil(1.2) AS ceiling, sqrt(12.96) AS root, "
      "sign(-5) AS negative_sign, sign(0) AS zero_sign, "
      "tail([1, 2, 3]) AS rest, last([1, 2, 3]) AS final, "
      "last([]) AS missing, reverse([1, 2, 3]) AS reversed_list, "
      "reverse('raksO') AS reversed_text, "
      "substring('0123456789', 1) AS suffix, "
      "substring('0123456789', 2, 3) AS middle");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"absolute", "ceiling", "root",
                                      "negative_sign", "zero_sign", "rest",
                                      "final", "missing", "reversed_list",
                                      "reversed_text", "suffix", "middle"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"7", "2", "3.6", "-1", "0", "[2, 3]", "3", "null", "[3, 2, 1]",
                 "\"Oskar\"", "\"123456789\"", "\"234\""}}));
}

TEST(QueryExecutorTest, PreservesCompactDoubleLiteralsInResultColumns) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(graph, "RETURN sqrt(12.96)");

  ASSERT_EQ(result.columns, std::vector<std::string>{"sqrt(12.96)"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"3.6"}}));
}

TEST(QueryExecutorTest, RandReturnsValueInUnitInterval) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "RETURN rand() >= 0.0 AND rand() < 1.0 AS in_range");

  ASSERT_EQ(result.columns, std::vector<std::string>{"in_range"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"true"}}));
}

TEST(QueryExecutorTest, ExecutesRegisteredFunctionsCaseInsensitively) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "UNWIND [1, 2, null] AS x "
                           "RETURN COUNT(x) AS count, CoLlEcT(x) AS values, "
                           "MAX(x) AS maximum");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"count", "values", "maximum"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"2", "[1, 2]", "2"}}));
}

TEST(QueryExecutorTest, ExecutesMapAndEntityBuiltInFunctions) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Person {name: 'Ada'})-[r:KNOWS]->() "
                           "RETURN keys({age: 36, name: 'Ada'}) AS map_keys, "
                           "properties({age: 36, name: 'Ada'}) AS map_props, "
                           "keys(n) AS node_keys, properties(n) AS node_props, "
                           "keys(r) AS rel_keys, properties(r) AS rel_props");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"map_keys", "map_props", "node_keys",
                                      "node_props", "rel_keys", "rel_props"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"[\"age\", \"name\"]", "{age: 36, name: \"Ada\"}",
                 "[\"age\", \"name\"]", "{age: 36, name: \"Ada\"}",
                 "[\"since\"]", "{since: 2020}"}}));
}

TEST(QueryExecutorTest, ExecutesCaseExpressions) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) RETURN n.name AS name, "
      "CASE WHEN n.age > 40 THEN 'senior' ELSE 'junior' END AS bucket, "
      "CASE n.name WHEN 'Ada' THEN 1 ELSE 2 END AS rank ORDER BY rank");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"name", "bucket", "rank"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Ada\"", "\"junior\"", "1"},
                                    {"\"Grace\"", "\"senior\"", "2"}}));
}

TEST(QueryExecutorTest, ExecutesListComprehension) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "RETURN [x IN [1, 2, 3] WHERE x > 1 | x * 10] AS scaled, "
      "[x IN [1, 2, 3] WHERE x > 1] AS filtered, "
      "[x IN [1, 2, 3]] AS copy");

  ASSERT_EQ(result.columns,
            (std::vector<std::string>{"scaled", "filtered", "copy"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"[20, 30]", "[2, 3]", "[1, 2, 3]"}}));
}

TEST(QueryExecutorTest, ExecutesExistsSubqueryProjection) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Person) "
                           "RETURN n.name AS name, "
                           "EXISTS { MATCH (n)-[:KNOWS]->(m) RETURN 1 } AS has "
                           "ORDER BY name");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"name", "has"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Ada\"", "true"},
                                                   {"\"Grace\"", "false"}}));
}

TEST(QueryExecutorTest, OrdersByExistsSubquery) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "RETURN n.name AS name "
      "ORDER BY EXISTS { MATCH (n)-[:KNOWS]->(m) RETURN 1 }, name");

  ASSERT_EQ(result.columns, std::vector<std::string>{"name"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Grace\""}, {"\"Ada\""}}));
}

TEST(QueryExecutorTest, ExecutesNotExistsSubqueryProjection) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "RETURN n.name AS name, "
      "NOT EXISTS { MATCH (n)-[:KNOWS]->(m) RETURN 1 } AS no_out "
      "ORDER BY name");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"name", "no_out"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Ada\"", "false"},
                                                   {"\"Grace\"", "true"}}));
}

TEST(QueryExecutorTest, ExecutesNestedExistsInFilterAndCaseExpression) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "WITH n "
      "WHERE EXISTS { MATCH (n)-[:KNOWS]->(m) RETURN 1 } = true "
      "RETURN n.name AS name, "
      "CASE WHEN EXISTS { MATCH (n)-[:KNOWS]->(m) RETURN 1 } "
      "THEN 'yes' ELSE 'no' END AS outgoing");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"name", "outgoing"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Ada\"", "\"yes\""}}));
}

TEST(QueryExecutorTest, ExecutesPatternComprehension) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "RETURN n.name AS name, [(n)-[:KNOWS]->(m) | m.name] AS names "
      "ORDER BY name");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"name", "names"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Ada\"", "[\"Grace\"]"},
                                                   {"\"Grace\"", "[]"}}));
}

TEST(QueryExecutorTest, UsesPatternComprehensionInPagination) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Person) "
                           "RETURN n.name AS name "
                           "ORDER BY name "
                           "SKIP size([()-[:KNOWS]->(m) | m])");

  ASSERT_EQ(result.columns, std::vector<std::string>{"name"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Grace\""}}));
}

TEST(QueryExecutorTest, WithDropsOrderByPassthroughColumnsAfterOrdering) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Person) "
                           "WITH n.name AS name ORDER BY n.age "
                           "RETURN name");

  ASSERT_EQ(result.columns, std::vector<std::string>{"name"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Ada\""}, {"\"Grace\""}}));
}

TEST(QueryExecutorTest, ExecutesDistinctExistsSubqueryProjection) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "RETURN DISTINCT EXISTS { MATCH (n)-[:KNOWS]->(m) RETURN 1 } AS has "
      "ORDER BY has");

  ASSERT_EQ(result.columns, std::vector<std::string>{"has"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"false"}, {"true"}}));
}

TEST(QueryExecutorTest, ExecutesExistsSubqueryGroupedAggregation) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) "
      "RETURN EXISTS { MATCH (n)-[:KNOWS]->(m) RETURN 1 } AS has, "
      "count(*) AS c "
      "ORDER BY has");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"has", "c"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"false", "1"}, {"true", "1"}}));
}

TEST(QueryExecutorTest, ExecutesUnwind) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph, "UNWIND [1, 2, 3] AS x RETURN x, x * 10 AS y ORDER BY x");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"x", "y"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"1", "10"}, {"2", "20"}, {"3", "30"}}));
}

TEST(QueryExecutorTest, ExecutesVariableLengthExpand) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (a:Person {name: 'Ada'})-[r*0..2]->(b) "
      "RETURN b.name AS name, size(r) AS hops ORDER BY hops, name");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"name", "hops"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{
                {"\"Ada\"", "0"}, {"\"Grace\"", "1"}, {"\"C++\"", "2"}}));
}

TEST(QueryExecutorTest, ExecutesVariableLengthExpandWithTypeFilter) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (a:Person {name: 'Ada'})-[r:KNOWS*1..2]->(b) "
                           "RETURN b.name AS name, size(r) AS hops");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"name", "hops"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"\"Grace\"", "1"}}));
}

TEST(QueryExecutorTest, FiltersPropertiesOnVariableLengthRelationships) {
  rg::InMemoryGraph graph;
  auto a = graph.CreateNode({"Artist"});
  auto b = graph.CreateNode({"Artist"});
  auto c = graph.CreateNode({"Artist"});
  graph.CreateRelationship(a, b, "WORKED_WITH", {{"year", rg::Value(1987)}});
  graph.CreateRelationship(b, c, "WORKED_WITH", {{"year", rg::Value(1988)}});

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (a:Artist)-[:WORKED_WITH* {year: 1988}]->(b:Artist) "
      "RETURN b");

  ASSERT_EQ(result.columns, std::vector<std::string>{"b"});
  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0][0].AsNode().id, c->id);
}

TEST(QueryExecutorTest, ExecutesVariableLengthExpandIntoBoundEndpoint) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (a:Person {name: 'Ada'}), (b:Language {name: 'C++'}) "
      "WITH a, b MATCH (a)-[r*1..2]->(b) RETURN size(r) AS hops");

  ASSERT_EQ(result.columns, std::vector<std::string>{"hops"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"2"}}));
}

TEST(QueryExecutorTest, ExecutesVariableLengthNamedPath) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH p = (a:Person {name: 'Ada'})-[r*1..2]->"
                           "(b:Language) RETURN length(p) AS len");

  ASSERT_EQ(result.columns, std::vector<std::string>{"len"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"2"}}));
}

TEST(QueryExecutorTest, ExecutesReversePlannedVariableLengthNamedPath) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH p = (a)-[r*1..2]->(b:Language) "
                           "RETURN length(p) AS len ORDER BY len");

  ASSERT_EQ(result.columns, std::vector<std::string>{"len"});
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"1"}, {"2"}}));
}

TEST(QueryExecutorTest, VariableLengthExpandReturnsNoRowsWhenBoundsDoNotMatch) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (a:Person {name: 'Ada'})-[r:KNOWS*2..2]->(b) "
                           "RETURN count(r) AS c");

  ASSERT_EQ(result.columns, std::vector<std::string>{"c"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"0"}}));
}

TEST(QueryExecutorTest, OptionalMatchNullExtendsMissingRows) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Language) "
                           "OPTIONAL MATCH (n)-[r]->(m) "
                           "RETURN n.name AS n, m.name AS m, type(r) AS rel");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"n", "m", "rel"}));
  EXPECT_EQ(
      StringRows(result),
      (std::vector<std::vector<std::string>>{{"\"C++\"", "null", "null"}}));
}

TEST(QueryExecutorTest, NullExpandSourceProducesNoMatches) {
  rg::InMemoryGraph graph;

  rg::QueryResult required = rg::ExecuteQuery(
      graph, "OPTIONAL MATCH (a) WITH a MATCH (a)-->(b) RETURN b");
  EXPECT_TRUE(required.rows.empty());

  rg::QueryResult optional = rg::ExecuteQuery(
      graph, "OPTIONAL MATCH (a) WITH a OPTIONAL MATCH (a)-->(b) RETURN b");
  EXPECT_EQ(StringRows(optional),
            (std::vector<std::vector<std::string>>{{"null"}}));
}

TEST(QueryExecutorTest, OptionalMatchPreservesMatchedRows) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Person {name: 'Ada'}) "
                           "OPTIONAL MATCH (n)-[r:KNOWS]->(m) "
                           "RETURN n.name AS n, m.name AS m, type(r) AS rel");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"n", "m", "rel"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Ada\"", "\"Grace\"", "\"KNOWS\""}}));
}

TEST(QueryExecutorTest, OptionalMatchNullExtendsWhenLocalWhereRejectsRows) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph,
                           "MATCH (n:Person {name: 'Ada'}) "
                           "OPTIONAL MATCH (n)-[r:KNOWS]->(m) "
                           "WHERE m.name = 'Missing' "
                           "RETURN n.name AS n, m.name AS m, type(r) AS rel");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"n", "m", "rel"}));
  EXPECT_EQ(
      StringRows(result),
      (std::vector<std::vector<std::string>>{{"\"Ada\"", "null", "null"}}));
}

TEST(QueryExecutorTest, OptionalMatchNullsCanBeAggregated) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Language) "
      "OPTIONAL MATCH (n)-[r]->(m) "
      "RETURN count(m) AS matched, collect(m.name) AS names");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"matched", "names"}));
  EXPECT_EQ(StringRows(result),
            (std::vector<std::vector<std::string>>{{"0", "[]"}}));
}

TEST(QueryExecutorTest, ExecutesCreateNodeAndRelationship) {
  rg::InMemoryGraph graph;

  rg::QueryResult result = rg::ExecuteQuery(
      graph,
      "CREATE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2026}]->"
      "(b:Person {name: 'Grace'}) "
      "RETURN a.name AS a, b.name AS b, r.since AS since");

  ASSERT_EQ(result.columns, (std::vector<std::string>{"a", "b", "since"}));
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"\"Ada\"", "\"Grace\"", "2026"}}));

  rg::QueryResult check = rg::ExecuteReadQuery(
      graph, "MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN count(r) AS c");
  EXPECT_EQ(StringRows(check), (std::vector<std::vector<std::string>>{{"1"}}));
}

TEST(QueryExecutorTest, ExecuteReadQueryRejectsWritesWithAccessPathOnly) {
  rg::InMemoryGraph graph;
  const rg::AccessPath &access_path = graph;

  EXPECT_THROW((void)rg::ExecuteReadQuery(access_path, "CREATE (n) RETURN n"),
               common::InvalidArgumentError);
  EXPECT_THROW((void)rg::ExecuteReadQuery(
                   access_path, "MATCH (n:Missing) SET n.value = 1 RETURN n"),
               common::InvalidArgumentError);

  rg::QueryResult check =
      rg::ExecuteReadQuery(access_path, "MATCH (n) RETURN count(n) AS c");
  EXPECT_EQ(StringRows(check), (std::vector<std::vector<std::string>>{{"0"}}));
}

TEST(QueryExecutorTest, ExecutesSetRemoveAndDetachDelete) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Ada' "
                        "SET n.score = 7, n += {team: 'db'}, n:Engineer "
                        "REMOVE n.age");

  rg::QueryResult updated = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Engineer) WHERE n.name = 'Ada' "
      "RETURN n.score AS score, n.team AS team, n.age AS age");
  ASSERT_EQ(updated.columns,
            (std::vector<std::string>{"score", "team", "age"}));
  EXPECT_EQ(StringRows(updated),
            (std::vector<std::vector<std::string>>{{"7", "\"db\"", "null"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Engineer) WHERE n.name = 'Ada' "
                        "DETACH DELETE n");
  rg::QueryResult deleted = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Ada' RETURN count(n) AS c");
  EXPECT_EQ(StringRows(deleted),
            (std::vector<std::vector<std::string>>{{"0"}}));
}

TEST(QueryExecutorTest, NullWriteTargetsAreIgnored) {
  rg::InMemoryGraph graph;

  const std::vector<std::string> queries = {
      "OPTIONAL MATCH (a:Missing) SET a.value = 1 RETURN a",
      "OPTIONAL MATCH (a:Missing) SET a = {value: 1} RETURN a",
      "OPTIONAL MATCH (a:Missing) SET a += {value: 1} RETURN a",
      "OPTIONAL MATCH (a:Missing) SET a:Label RETURN a",
      "OPTIONAL MATCH (a:Missing) REMOVE a.value RETURN a",
      "OPTIONAL MATCH (a:Missing) REMOVE a:Label RETURN a",
      "OPTIONAL MATCH (a:Missing) DELETE a RETURN a",
      "OPTIONAL MATCH (a:Missing) DETACH DELETE a RETURN a",
  };
  for (const auto &query : queries) {
    rg::QueryResult result = rg::ExecuteQuery(graph, query);
    EXPECT_EQ(StringRows(result),
              (std::vector<std::vector<std::string>>{{"null"}}))
        << query;
  }
  EXPECT_TRUE(graph.Nodes().empty());
  EXPECT_TRUE(graph.Relationships().empty());
}

TEST(QueryExecutorTest, RemovesNullPropertiesAndUpdatesRelationshipMaps) {
  rg::InMemoryGraph graph;
  graph.AddNodeIndex({"Person"}, "name");
  graph.AddRelationshipIndex({"KNOWS"}, "since");
  const rg::QueryOptions options = QueryOptionsFor(graph);
  rg::ExecuteWriteQuery(
      graph,
      "CREATE (:Person {name: 'Ada', missing: null})"
      "-[r:KNOWS {since: 2020, missing: null}]->(:Person {name: 'Grace'})",
      options);

  rg::ExecuteWriteQuery(
      graph,
      "MATCH (a:Person {name: 'Ada'})-[r:KNOWS]->() "
      "SET a.name = null, r = {kind: 'friend', missing: null}, "
      "r += {since: 2026, kind: null}",
      options);

  rg::QueryResult result = rg::ExecuteReadQuery(
      graph,
      "MATCH (a:Person)-[r:KNOWS]->() "
      "RETURN a.name AS name, a.missing AS node_missing, "
      "r.since AS since, r.kind AS kind, r.missing AS rel_missing",
      options);
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{
                                    {"null", "null", "2026", "null", "null"}}));
  EXPECT_TRUE(graph.Nodes()[0]->properties.empty());
  EXPECT_EQ(graph.Relationships()[0]->properties,
            (rg::Value::Map{{"since", rg::Value(2026)}}));
  EXPECT_TRUE(
      graph.FindNodesByIndex({"Person"}, "name", rg::Value("Ada")).empty());
  EXPECT_TRUE(
      graph.FindRelationshipsByIndex({"KNOWS"}, "since", rg::Value(2020))
          .empty());
  EXPECT_EQ(graph.FindRelationshipsByIndex({"KNOWS"}, "since", rg::Value(2026))
                .size(),
            1U);
}

TEST(QueryExecutorTest, DeduplicatesDeleteTargetsAcrossRows) {
  rg::InMemoryGraph graph;
  rg::ExecuteWriteQuery(graph,
                        "CREATE (a:Node)-[:LINK]->(:Node), "
                        "(a)-[:LINK]->(:Node)");

  EXPECT_NO_THROW(
      rg::ExecuteWriteQuery(graph, "MATCH (a:Node)-[r:LINK]->() DELETE r, a"));
  EXPECT_EQ(graph.Nodes().size(), 2U);
  EXPECT_TRUE(graph.Relationships().empty());
}

TEST(QueryExecutorTest, ExecutesMergeCreateAndMatchActions) {
  rg::InMemoryGraph graph;

  rg::QueryResult created = rg::ExecuteQuery(graph,
                                             "MERGE (n:Person {name: 'Ada'}) "
                                             "ON CREATE SET n.created = true "
                                             "RETURN n.created AS created");
  ASSERT_EQ(StringRows(created),
            (std::vector<std::vector<std::string>>{{"true"}}));

  rg::QueryResult matched =
      rg::ExecuteQuery(graph,
                       "MERGE (n:Person {name: 'Ada'}) "
                       "ON MATCH SET n.seen = true "
                       "RETURN n.created AS created, n.seen AS seen");
  ASSERT_EQ(matched.columns, (std::vector<std::string>{"created", "seen"}));
  EXPECT_EQ(StringRows(matched),
            (std::vector<std::vector<std::string>>{{"true", "true"}}));
}

TEST(QueryExecutorTest, PreservesBindingsAcrossConsecutiveMerges) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteQuery(graph,
                       "CREATE (a) WITH a MERGE (x) MERGE (y) "
                       "MERGE (x)-[:T]->(y) CREATE (b) CREATE (a)<-[:T]-(b)");

  EXPECT_TRUE(result.columns.empty());
  EXPECT_TRUE(result.rows.empty());
  EXPECT_EQ(graph.Nodes().size(), 2U);
  EXPECT_EQ(graph.Relationships().size(), 2U);
}

TEST(QueryExecutorTest, PreservesBindingsAcrossUnwind) {
  rg::InMemoryGraph graph;

  rg::QueryResult result =
      rg::ExecuteQuery(graph,
                       "CREATE (a) WITH a UNWIND [0] AS i CREATE (b) "
                       "CREATE (a)<-[:T]-(b)");

  EXPECT_TRUE(result.columns.empty());
  EXPECT_TRUE(result.rows.empty());
  EXPECT_EQ(graph.Nodes().size(), 2U);
  EXPECT_EQ(graph.Relationships().size(), 1U);
}

TEST(QueryExecutorTest, ExecutesNamedMergePaths) {
  rg::InMemoryGraph graph;

  rg::QueryResult node_path =
      rg::ExecuteQuery(graph, "MERGE p = (a {num: 1}) RETURN p");
  ASSERT_EQ(node_path.rows.size(), 1U);
  ASSERT_TRUE(node_path.rows[0][0].IsPath());
  EXPECT_EQ(node_path.rows[0][0].AsPath().nodes.size(), 1U);
  EXPECT_TRUE(node_path.rows[0][0].AsPath().relationships.empty());

  rg::QueryResult matched_node_path =
      rg::ExecuteQuery(graph, "MERGE p = (a {num: 1}) RETURN p");
  ASSERT_EQ(matched_node_path.rows.size(), 1U);
  ASSERT_TRUE(matched_node_path.rows[0][0].IsPath());
  EXPECT_EQ(matched_node_path.rows[0][0].AsPath().nodes.size(), 1U);

  rg::QueryResult relationship_path =
      rg::ExecuteQuery(graph,
                       "MERGE (a {num: 1}) MERGE (b {num: 2}) "
                       "MERGE p = (a)-[:R]->(b) RETURN p");
  ASSERT_EQ(relationship_path.rows.size(), 1U);
  ASSERT_TRUE(relationship_path.rows[0][0].IsPath());
  EXPECT_EQ(relationship_path.rows[0][0].AsPath().nodes.size(), 2U);
  EXPECT_EQ(relationship_path.rows[0][0].AsPath().relationships.size(), 1U);

  rg::QueryResult matched_relationship_path =
      rg::ExecuteQuery(graph,
                       "MERGE (a {num: 1}) MERGE (b {num: 2}) "
                       "MERGE p = (a)-[:R]->(b) RETURN p");
  ASSERT_EQ(matched_relationship_path.rows.size(), 1U);
  ASSERT_TRUE(matched_relationship_path.rows[0][0].IsPath());
  EXPECT_EQ(matched_relationship_path.rows[0][0].AsPath().nodes.size(), 2U);
  EXPECT_EQ(matched_relationship_path.rows[0][0].AsPath().relationships.size(),
            1U);
  EXPECT_EQ(graph.Nodes().size(), 2U);
  EXPECT_EQ(graph.Relationships().size(), 1U);
}

TEST(QueryExecutorTest,
     InMemoryGraphEstimatesPlannerStatisticsFromCurrentData) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  EXPECT_DOUBLE_EQ(graph.EstimateNodeCount(std::unordered_set<std::string>{}),
                   3.0);
  EXPECT_DOUBLE_EQ(
      graph.EstimateNodeCount(std::unordered_set<std::string>{"Person"}), 2.0);
  EXPECT_DOUBLE_EQ(graph.EstimateNodeCount(
                       std::unordered_set<std::string>{"Person", "Language"}),
                   0.0);
  EXPECT_DOUBLE_EQ(graph.EstimateRelationshipCount({}), 2.0);
  EXPECT_DOUBLE_EQ(graph.EstimateRelationshipCount({"KNOWS"}), 1.0);
  EXPECT_DOUBLE_EQ(graph.EstimateRelationshipCount({"MISSING"}), 0.0);

  EXPECT_DOUBLE_EQ(graph.EstimateExpandFanout({"KNOWS"}), 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(graph.EstimateExpandIntoSelectivity({}), 2.0 / 9.0);
  EXPECT_DOUBLE_EQ(graph.EstimateNodeIndexSeekSelectivity(
                       std::unordered_set<std::string>{"Person"}, "name"),
                   0.5);
  EXPECT_DOUBLE_EQ(graph.EstimateNodeIndexRangeSeekSelectivity(
                       std::unordered_set<std::string>{"Person"}, "age", 2),
                   0.25);
  EXPECT_DOUBLE_EQ(graph.EstimateRelationshipIndexSeekSelectivity({}, "since"),
                   0.5);
  EXPECT_DOUBLE_EQ(graph.EstimateRelationshipIndexRangeSeekSelectivity(
                       {"KNOWS"}, "since", 1),
                   0.5);

  EXPECT_DOUBLE_EQ(graph.EstimateProcedureRows("db.labels", 1), 2.0);
  EXPECT_DOUBLE_EQ(graph.EstimateProcedureRows("db.relationshipTypes", 1), 2.0);
  EXPECT_DOUBLE_EQ(graph.EstimateProcedureRows("db.propertyKeys", 1), 3.0);
  EXPECT_DOUBLE_EQ(graph.EstimateProcedureRows("dbms.procedures", 5), 4.0);
}

TEST(QueryExecutorTest, LogicalPlanUsesInMemoryGraphStatistics) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  std::unique_ptr<ir::LogicalPlan> label_scan_plan =
      LogicalPlanFor(graph, "MATCH (n:Person) RETURN n");
  ASSERT_NE(label_scan_plan, nullptr);
  const ir::LogicalPlan *label_scan =
      FindPlanNode(*label_scan_plan, ir::LogicalPlanNodeType::kNodeByLabelScan);
  ASSERT_NE(label_scan, nullptr);
  ASSERT_TRUE(label_scan->EstimatedRows().has_value());
  EXPECT_EQ(label_scan->EstimatedRows(), 2.0);

  std::unique_ptr<ir::LogicalPlan> all_scan_plan =
      LogicalPlanFor(graph, "MATCH (n) RETURN n");
  ASSERT_NE(all_scan_plan, nullptr);
  const ir::LogicalPlan *all_scan =
      FindPlanNode(*all_scan_plan, ir::LogicalPlanNodeType::kAllNodeScan);
  ASSERT_NE(all_scan, nullptr);
  ASSERT_TRUE(all_scan->EstimatedRows().has_value());
  EXPECT_EQ(all_scan->EstimatedRows(), 3.0);

  std::unique_ptr<ir::LogicalPlan> procedure_plan =
      LogicalPlanFor(graph, "CALL db.propertyKeys()");
  ASSERT_NE(procedure_plan, nullptr);
  ASSERT_TRUE(procedure_plan->EstimatedRows().has_value());
  EXPECT_EQ(procedure_plan->EstimatedRows(), 3.0);
}

TEST(QueryExecutorTest, MaintainsNodeIndexAcrossWrites) {
  rg::InMemoryGraph graph;
  graph.AddNodeIndex({"Person"}, "name");
  const rg::QueryOptions options = QueryOptionsFor(graph);

  rg::ExecuteWriteQuery(
      graph, "CREATE (:Person {name: 'Ada'}), (:Person {name: 'Grace'})",
      options);

  rg::QueryResult created = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Ada' RETURN count(n) AS c",
      options);
  EXPECT_EQ(StringRows(created),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Ada' "
                        "SET n.name = 'Lovelace'",
                        options);
  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Lovelace' "
                        "SET n.name = 'Lovelace'",
                        options);

  rg::QueryResult old_name = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Ada' RETURN count(n) AS c",
      options);
  EXPECT_EQ(StringRows(old_name),
            (std::vector<std::vector<std::string>>{{"0"}}));

  rg::QueryResult new_name = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Lovelace' RETURN count(n) AS c",
      options);
  EXPECT_EQ(StringRows(new_name),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Lovelace' "
                        "REMOVE n.name",
                        options);
  rg::QueryResult removed = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Lovelace' RETURN count(n) AS c",
      options);
  EXPECT_EQ(StringRows(removed),
            (std::vector<std::vector<std::string>>{{"0"}}));
}

TEST(QueryExecutorTest, MaintainsRelationshipIndexAcrossWrites) {
  rg::InMemoryGraph graph;
  graph.AddRelationshipIndex({"KNOWS"}, "since");
  const rg::QueryOptions options = QueryOptionsFor(graph);

  rg::ExecuteWriteQuery(
      graph,
      "CREATE (:Person {name: 'Ada'})-[r:KNOWS {since: 2020}]->"
      "(:Person {name: 'Grace'})",
      options);

  rg::QueryResult created = rg::ExecuteReadQuery(
      graph, "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 RETURN count(r) AS c",
      options);
  EXPECT_EQ(StringRows(created),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 "
                        "SET r.since = 2021",
                        options);
  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 "
                        "SET r.since = 2021",
                        options);

  rg::QueryResult old_since = rg::ExecuteReadQuery(
      graph, "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 RETURN count(r) AS c",
      options);
  EXPECT_EQ(StringRows(old_since),
            (std::vector<std::vector<std::string>>{{"0"}}));

  rg::QueryResult new_since = rg::ExecuteReadQuery(
      graph, "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 RETURN count(r) AS c",
      options);
  EXPECT_EQ(StringRows(new_since),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 "
                        "REMOVE r.since",
                        options);
  rg::QueryResult removed = rg::ExecuteReadQuery(
      graph, "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 RETURN count(r) AS c",
      options);
  EXPECT_EQ(StringRows(removed),
            (std::vector<std::vector<std::string>>{{"0"}}));
}

TEST(QueryExecutorTest, UsesMaintainedNodeRangeIndexCandidates) {
  rg::InMemoryGraph graph;
  graph.AddNodeIndex({"Person"}, "age");
  const rg::QueryOptions options = QueryOptionsFor(graph);

  rg::ExecuteWriteQuery(graph,
                        "CREATE (:Person {name: 'Ada', age: 36}), "
                        "(:Person {name: 'Grace', age: 85})",
                        options);

  rg::QueryResult initial = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) WHERE n.age >= 40 RETURN n.name AS name "
      "ORDER BY name",
      options);
  EXPECT_EQ(StringRows(initial),
            (std::vector<std::vector<std::string>>{{"\"Grace\""}}));

  rg::ExecuteWriteQuery(
      graph, "MATCH (n:Person) WHERE n.age = 36 SET n.age = 41", options);

  rg::QueryResult updated = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) WHERE n.age >= 40 RETURN n.name AS name "
      "ORDER BY name",
      options);
  EXPECT_EQ(StringRows(updated), (std::vector<std::vector<std::string>>{
                                     {"\"Ada\""}, {"\"Grace\""}}));
}

TEST(QueryExecutorTest, UsesMaintainedRelationshipRangeIndexCandidates) {
  rg::InMemoryGraph graph;
  graph.AddRelationshipIndex({"KNOWS"}, "since");
  const rg::QueryOptions options = QueryOptionsFor(graph);

  rg::ExecuteWriteQuery(
      graph,
      "CREATE (:Person {name: 'Ada'})-[:KNOWS {since: 2020}]->"
      "(:Person {name: 'Grace'}), "
      "(:Person {name: 'Grace'})-[:KNOWS {since: 2026}]->"
      "(:Person {name: 'Katherine'})",
      options);

  rg::QueryResult initial =
      rg::ExecuteReadQuery(graph,
                           "MATCH ()-[r:KNOWS]->() WHERE r.since >= 2021 "
                           "RETURN r.since AS since ORDER BY since",
                           options);
  EXPECT_EQ(StringRows(initial),
            (std::vector<std::vector<std::string>>{{"2026"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 "
                        "SET r.since = 2027",
                        options);

  rg::QueryResult updated =
      rg::ExecuteReadQuery(graph,
                           "MATCH ()-[r:KNOWS]->() WHERE r.since >= 2021 "
                           "RETURN r.since AS since ORDER BY since",
                           options);
  EXPECT_EQ(StringRows(updated),
            (std::vector<std::vector<std::string>>{{"2026"}, {"2027"}}));
}

TEST(QueryExecutorTest, MaintainsAdjacencyAfterDetachDelete) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Ada' "
                        "DETACH DELETE n");

  rg::QueryResult incoming =
      rg::ExecuteReadQuery(graph,
                           "MATCH (g:Person) WHERE g.name = 'Grace' "
                           "MATCH ()-[r:KNOWS]->(g) RETURN count(r) AS c");
  EXPECT_EQ(StringRows(incoming),
            (std::vector<std::vector<std::string>>{{"0"}}));
}
