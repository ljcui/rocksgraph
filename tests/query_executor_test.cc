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
