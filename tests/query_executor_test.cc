#include "runtime/query_executor.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

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
