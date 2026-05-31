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

TEST(QueryExecutorTest, ExecutesCountAggregation) {
  rg::InMemoryGraph graph;
  SeedDemoGraph(&graph);

  rg::QueryResult result =
      rg::ExecuteReadQuery(graph, "MATCH (n:Person) RETURN count(n) AS c");

  ASSERT_EQ(result.columns, std::vector<std::string>{"c"});
  EXPECT_EQ(StringRows(result), (std::vector<std::vector<std::string>>{{"2"}}));
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
