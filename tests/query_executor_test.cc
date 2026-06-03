#include "runtime/query_executor.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast_builder.h"
#include "ir/logical_plan_builder.h"
#include "ir/planner_query.h"

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
                           "ORDER BY name SKIP size([(n)-[:KNOWS]->(m) | m])");

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
  EXPECT_DOUBLE_EQ(*label_scan->EstimatedRows(), 2.0);

  std::unique_ptr<ir::LogicalPlan> all_scan_plan =
      LogicalPlanFor(graph, "MATCH (n) RETURN n");
  ASSERT_NE(all_scan_plan, nullptr);
  const ir::LogicalPlan *all_scan =
      FindPlanNode(*all_scan_plan, ir::LogicalPlanNodeType::kAllNodeScan);
  ASSERT_NE(all_scan, nullptr);
  ASSERT_TRUE(all_scan->EstimatedRows().has_value());
  EXPECT_DOUBLE_EQ(*all_scan->EstimatedRows(), 3.0);

  std::unique_ptr<ir::LogicalPlan> procedure_plan =
      LogicalPlanFor(graph, "CALL db.propertyKeys()");
  ASSERT_NE(procedure_plan, nullptr);
  ASSERT_TRUE(procedure_plan->EstimatedRows().has_value());
  EXPECT_DOUBLE_EQ(*procedure_plan->EstimatedRows(), 3.0);
}

TEST(QueryExecutorTest, MaintainsNodeIndexAcrossWrites) {
  rg::InMemoryGraph graph;
  graph.AddNodeIndex({"Person"}, "name");

  rg::ExecuteWriteQuery(
      graph, "CREATE (:Person {name: 'Ada'}), (:Person {name: 'Grace'})");

  rg::QueryResult created = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Ada' RETURN count(n) AS c");
  EXPECT_EQ(StringRows(created),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Ada' "
                        "SET n.name = 'Lovelace'");
  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Lovelace' "
                        "SET n.name = 'Lovelace'");

  rg::QueryResult old_name = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Ada' RETURN count(n) AS c");
  EXPECT_EQ(StringRows(old_name),
            (std::vector<std::vector<std::string>>{{"0"}}));

  rg::QueryResult new_name = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Lovelace' RETURN count(n) AS c");
  EXPECT_EQ(StringRows(new_name),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.name = 'Lovelace' "
                        "REMOVE n.name");
  rg::QueryResult removed = rg::ExecuteReadQuery(
      graph, "MATCH (n:Person) WHERE n.name = 'Lovelace' RETURN count(n) AS c");
  EXPECT_EQ(StringRows(removed),
            (std::vector<std::vector<std::string>>{{"0"}}));
}

TEST(QueryExecutorTest, MaintainsRelationshipIndexAcrossWrites) {
  rg::InMemoryGraph graph;
  graph.AddRelationshipIndex({"KNOWS"}, "since");

  rg::ExecuteWriteQuery(
      graph,
      "CREATE (:Person {name: 'Ada'})-[r:KNOWS {since: 2020}]->"
      "(:Person {name: 'Grace'})");

  rg::QueryResult created = rg::ExecuteReadQuery(
      graph,
      "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 RETURN count(r) AS c");
  EXPECT_EQ(StringRows(created),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 "
                        "SET r.since = 2021");
  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 "
                        "SET r.since = 2021");

  rg::QueryResult old_since = rg::ExecuteReadQuery(
      graph,
      "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 RETURN count(r) AS c");
  EXPECT_EQ(StringRows(old_since),
            (std::vector<std::vector<std::string>>{{"0"}}));

  rg::QueryResult new_since = rg::ExecuteReadQuery(
      graph,
      "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 RETURN count(r) AS c");
  EXPECT_EQ(StringRows(new_since),
            (std::vector<std::vector<std::string>>{{"1"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 "
                        "REMOVE r.since");
  rg::QueryResult removed = rg::ExecuteReadQuery(
      graph,
      "MATCH ()-[r:KNOWS]->() WHERE r.since = 2021 RETURN count(r) AS c");
  EXPECT_EQ(StringRows(removed),
            (std::vector<std::vector<std::string>>{{"0"}}));
}

TEST(QueryExecutorTest, UsesMaintainedNodeRangeIndexCandidates) {
  rg::InMemoryGraph graph;
  graph.AddNodeIndex({"Person"}, "age");

  rg::ExecuteWriteQuery(graph,
                        "CREATE (:Person {name: 'Ada', age: 36}), "
                        "(:Person {name: 'Grace', age: 85})");

  rg::QueryResult initial = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) WHERE n.age >= 40 RETURN n.name AS name "
      "ORDER BY name");
  EXPECT_EQ(StringRows(initial),
            (std::vector<std::vector<std::string>>{{"\"Grace\""}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH (n:Person) WHERE n.age = 36 SET n.age = 41");

  rg::QueryResult updated = rg::ExecuteReadQuery(
      graph,
      "MATCH (n:Person) WHERE n.age >= 40 RETURN n.name AS name "
      "ORDER BY name");
  EXPECT_EQ(StringRows(updated), (std::vector<std::vector<std::string>>{
                                     {"\"Ada\""}, {"\"Grace\""}}));
}

TEST(QueryExecutorTest, UsesMaintainedRelationshipRangeIndexCandidates) {
  rg::InMemoryGraph graph;
  graph.AddRelationshipIndex({"KNOWS"}, "since");

  rg::ExecuteWriteQuery(
      graph,
      "CREATE (:Person {name: 'Ada'})-[:KNOWS {since: 2020}]->"
      "(:Person {name: 'Grace'}), "
      "(:Person {name: 'Grace'})-[:KNOWS {since: 2026}]->"
      "(:Person {name: 'Katherine'})");

  rg::QueryResult initial =
      rg::ExecuteReadQuery(graph,
                           "MATCH ()-[r:KNOWS]->() WHERE r.since >= 2021 "
                           "RETURN r.since AS since ORDER BY since");
  EXPECT_EQ(StringRows(initial),
            (std::vector<std::vector<std::string>>{{"2026"}}));

  rg::ExecuteWriteQuery(graph,
                        "MATCH ()-[r:KNOWS]->() WHERE r.since = 2020 "
                        "SET r.since = 2027");

  rg::QueryResult updated =
      rg::ExecuteReadQuery(graph,
                           "MATCH ()-[r:KNOWS]->() WHERE r.since >= 2021 "
                           "RETURN r.since AS since ORDER BY since");
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
