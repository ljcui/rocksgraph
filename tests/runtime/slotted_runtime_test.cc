#include <gtest/gtest.h>

#include "runtime/query_executor.h"
#include "storage/in_memory_graph.h"

TEST(SlottedRuntimeTest, ExhaustsWritesBelowLimit) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"});
  graph.CreateNode({"N"});
  graph.CreateNode({"N"});

  const rg::QueryResult result = rg::ExecuteQuery(
      graph, "MATCH (n:N) SET n.marked = true RETURN n LIMIT 1");

  ASSERT_EQ(result.rows.size(), 1U);
  ASSERT_EQ(graph.Nodes().size(), 3U);
  for (const auto &node : graph.Nodes()) {
    const auto marked = node->properties.find("marked");
    ASSERT_NE(marked, node->properties.end());
    EXPECT_TRUE(marked->second.AsBool());
  }
}

TEST(SlottedRuntimeTest, KeepsDeletedEntitiesAvailableToResults) {
  rg::InMemoryGraph graph;
  auto node = graph.CreateNode({"N"}, {{"name", rg::Value("deleted node")}});

  const rg::QueryResult result =
      rg::ExecuteQuery(graph, "MATCH (n:N) DELETE n RETURN n, n.name AS name");

  ASSERT_EQ(result.rows.size(), 1U);
  ASSERT_EQ(result.rows.front().size(), 2U);
  EXPECT_TRUE(result.rows.front()[0].IsNode());
  EXPECT_EQ(result.rows.front()[0].AsNode().id, node->id);
  EXPECT_EQ(result.rows.front()[1].AsString(), "deleted node");
  EXPECT_TRUE(graph.Nodes().empty());
}
