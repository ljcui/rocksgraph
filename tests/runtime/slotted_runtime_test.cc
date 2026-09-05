#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "common/exception.h"
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

TEST(SlottedRuntimeTest, StreamsRowsAndCanCloseEarly) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"}, {{"value", rg::Value(1)}});
  graph.CreateNode({"N"}, {{"value", rg::Value(2)}});

  std::unique_ptr<rg::QueryResultCursor> cursor = rg::ExecuteReadQueryCursor(
      graph, "MATCH (n:N) RETURN n.value AS value ORDER BY value");
  EXPECT_EQ(cursor->Columns(), std::vector<std::string>{"value"});
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  ASSERT_EQ(row.size(), 1U);
  EXPECT_EQ(row[0].AsInteger(), 1);
  cursor->Close();
  EXPECT_FALSE(cursor->Next(&row));
}

TEST(SlottedRuntimeTest, ObservesExternalCancellation) {
  rg::InMemoryGraph graph;
  graph.CreateNode({"N"});
  rg::QueryOptions options;
  options.execution.cancellation =
      std::make_shared<rg::QueryCancellationToken>();
  std::unique_ptr<rg::QueryResultCursor> cursor =
      rg::ExecuteReadQueryCursor(graph, "MATCH (n:N) RETURN n", options);
  options.execution.cancellation->Cancel();

  std::vector<rg::Value> row;
  EXPECT_THROW((void)cursor->Next(&row), common::QueryCancelledError);
}

TEST(SlottedRuntimeTest, RollsBackWritesWhenCursorClosesEarly) {
  rg::InMemoryGraph graph;
  std::unique_ptr<rg::QueryResultCursor> cursor = rg::ExecuteQueryCursor(
      graph, "UNWIND [1, 2, 3] AS x CREATE (:N {value: x}) RETURN x");
  std::vector<rg::Value> row;
  ASSERT_TRUE(cursor->Next(&row));
  ASSERT_EQ(graph.Nodes().size(), 1U);

  cursor->Close();
  EXPECT_TRUE(graph.Nodes().empty());
}

TEST(SlottedRuntimeTest, EnforcesBlockingOperatorMemoryLimit) {
  rg::InMemoryGraph graph;
  rg::QueryOptions options;
  options.execution.memory_limit_bytes = 1;
  std::unique_ptr<rg::QueryResultCursor> cursor = rg::ExecuteReadQueryCursor(
      graph, "UNWIND range(1, 100) AS x RETURN x ORDER BY x DESC", options);

  std::vector<rg::Value> row;
  EXPECT_THROW((void)cursor->Next(&row), common::MemoryLimitExceededError);
}

TEST(SlottedRuntimeTest, ReportsPeakMemoryForBlockingOperators) {
  rg::InMemoryGraph graph;
  const rg::QueryResult result =
      rg::ExecuteReadQuery(graph, "UNWIND [3, 1, 2] AS x RETURN x ORDER BY x");

  EXPECT_GT(result.peak_memory_bytes, 0U);
}

TEST(SlottedRuntimeTest, UsesTypedKeysAcrossSetOperators) {
  rg::InMemoryGraph graph;

  const rg::QueryResult distinct =
      rg::ExecuteReadQuery(graph, "UNWIND [1, 1.0, 2] AS x RETURN DISTINCT x");
  ASSERT_EQ(distinct.rows.size(), 2U);
  EXPECT_TRUE(rg::ValuesEqual(distinct.rows[0][0], rg::Value(1)));
  EXPECT_TRUE(rg::ValuesEqual(distinct.rows[1][0], rg::Value(2)));

  const rg::QueryResult grouped = rg::ExecuteReadQuery(
      graph, "UNWIND [1, 1.0, 2] AS x RETURN x, count(*) AS count");
  ASSERT_EQ(grouped.rows.size(), 2U);
  EXPECT_TRUE(rg::ValuesEqual(grouped.rows[0][0], rg::Value(1)));
  EXPECT_EQ(grouped.rows[0][1], rg::Value(2));
  EXPECT_TRUE(rg::ValuesEqual(grouped.rows[1][0], rg::Value(2)));
  EXPECT_EQ(grouped.rows[1][1], rg::Value(1));

  const rg::QueryResult aggregate_distinct = rg::ExecuteReadQuery(
      graph, "UNWIND [1, 1.0, 2] AS x RETURN count(DISTINCT x) AS count");
  ASSERT_EQ(aggregate_distinct.rows.size(), 1U);
  EXPECT_EQ(aggregate_distinct.rows[0][0], rg::Value(2));

  const rg::QueryResult union_distinct = rg::ExecuteReadQuery(
      graph, "RETURN [1] AS value UNION RETURN [1.0] AS value");
  EXPECT_EQ(union_distinct.rows.size(), 1U);
}
