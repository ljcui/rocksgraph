#include <gtest/gtest.h>

#include <boost/endian/conversion.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "graphdb/assistant_pool.h"
#include "graphdb/graph_db.h"
#include "runtime/graphdb_planner_catalog.h"
#include "runtime/query_executor.h"
#include "tests/planner/fake_planner_statistics.h"
#include "transaction/transaction.h"

namespace {

bool WaitUntilVertexIndexReady(graphdb::GraphDB *graph,
                               const std::string &name) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (graph->meta_info().GetReadyVertexPropertyIndex(name)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool WaitUntilEdgeIndexReady(graphdb::GraphDB *graph, const std::string &name) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (graph->meta_info().GetReadyEdgePropertyIndex(name)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

class GraphDBQueryExecutorTest : public testing::Test {
 protected:
  void SetUp() override {
    std::error_code error;
    std::filesystem::remove_all(kPath, error);
    graphdb::GraphDBOptions options;
    options.assistant_pool = std::make_shared<graphdb::AssistantPool>(1);
    graph_ = graphdb::GraphDB::Open(kPath, options);
    graph_->drop_on_close() = true;

    auto transaction = graph_->BeginTransaction();
    ada_ = transaction
               ->CreateVertex({"Person"}, {{"name", rg::Value("Ada")},
                                           {"age", rg::Value(10)}})
               .GetNativeId();
    grace_ = transaction
                 ->CreateVertex({"Person"}, {{"name", rg::Value("Grace")},
                                             {"age", rg::Value(20)}})
                 .GetNativeId();
    other_ = transaction
                 ->CreateVertex({"Person"}, {{"name", rg::Value("Other")},
                                             {"age", rg::Value(30)}})
                 .GetNativeId();
    auto ada = transaction->GetVertexById(boost::endian::native_to_big(ada_));
    auto grace =
        transaction->GetVertexById(boost::endian::native_to_big(grace_));
    auto other =
        transaction->GetVertexById(boost::endian::native_to_big(other_));
    knows_ = transaction
                 ->CreateEdge(ada, grace, "KNOWS", {{"since", rg::Value(2020)}})
                 .GetNativeId();
    rare_ =
        transaction
            ->CreateEdge(grace, other, "RARE_REL", {{"weight", rg::Value(7)}})
            .GetNativeId();
    transaction->Commit();

    graph_->AddVertexPropertyIndex("person_name", false, "Person", {"name"});
    graph_->AddVertexPropertyIndex("person_age", false, "Person", {"age"});
    graph_->AddEdgePropertyIndex("knows_since", false, "KNOWS", {"since"});
    ASSERT_TRUE(WaitUntilVertexIndexReady(graph_.get(), "person_name"));
    ASSERT_TRUE(WaitUntilVertexIndexReady(graph_.get(), "person_age"));
    ASSERT_TRUE(WaitUntilEdgeIndexReady(graph_.get(), "knows_since"));
  }

  static constexpr const char *kPath = "graphdb_runtime_testdb";
  std::unique_ptr<graphdb::GraphDB> graph_;
  std::int64_t ada_ = 0;
  std::int64_t grace_ = 0;
  std::int64_t other_ = 0;
  std::int64_t knows_ = 0;
  std::int64_t rare_ = 0;
};

TEST_F(GraphDBQueryExecutorTest, ExecutesLabelScanExpandAndPropertyReads) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result =
      rg::ExecuteQuery(*transaction,
                       "MATCH (a:Person)-[r:KNOWS]->(b) "
                       "RETURN id(a) AS aid, id(r) AS rid, type(r) AS kind, "
                       "a.name AS source, r.since AS since, b.name AS target");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(ada_), rg::Value(knows_),
                                    rg::Value("KNOWS"), rg::Value("Ada"),
                                    rg::Value(2020), rg::Value("Grace")}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeRelationshipTypeScan) {
  test_support::FakePlannerStatistics statistics;
  statistics.relationship_count_by_type = {{"RARE_REL", 1.0}};
  auto transaction = graph_->BeginTransaction();
  rg::QueryOptions options;
  options.planner_statistics = &statistics;
  rg::QueryResult result =
      rg::ExecuteQuery(*transaction,
                       "MATCH (a)-[r:RARE_REL]->(b) "
                       "RETURN id(a) AS aid, id(r) AS rid, type(r) AS kind, "
                       "r.weight AS weight, id(b) AS bid",
                       options);

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(grace_), rg::Value(rare_),
                                    rg::Value("RARE_REL"), rg::Value(7),
                                    rg::Value(other_)}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeVertexIndexSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result =
      rg::ExecuteQuery(*transaction,
                       "MATCH (n:Person) WHERE n.name = 'Ada' "
                       "RETURN id(n) AS id, n.name AS name");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(ada_), rg::Value("Ada")}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, CatalogExposesReadyGraphDBIndexes) {
  rg::GraphDBPlannerCatalog catalog(*graph_);

  const auto vertex_index = catalog.FindNodeIndex({"Person"}, "name");
  ASSERT_TRUE(vertex_index.has_value());
  EXPECT_EQ(vertex_index->property_key, "name");
  EXPECT_FALSE(vertex_index->unique);

  const auto edge_index = catalog.FindRelationshipIndex({"KNOWS"}, "since");
  ASSERT_TRUE(edge_index.has_value());
  EXPECT_EQ(edge_index->property_key, "since");
  EXPECT_FALSE(edge_index->unique);

  EXPECT_FALSE(catalog.FindNodeIndex({"Person"}, "missing").has_value());
  EXPECT_FALSE(catalog.FindRelationshipIndex({"KNOWS"}, "missing").has_value());
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeVertexIndexRangeSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result =
      rg::ExecuteQuery(*transaction,
                       "MATCH (n:Person) WHERE n.age >= 20 AND n.age < 31 "
                       "RETURN id(n) AS id, n.age AS age");

  ASSERT_EQ(result.rows.size(), 2U);
  EXPECT_EQ(result.rows[0][1], rg::Value(20));
  EXPECT_EQ(result.rows[1][1], rg::Value(30));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeRelationshipIndexSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result =
      rg::ExecuteQuery(*transaction,
                       "MATCH (a)-[r:KNOWS]->(b) WHERE r.since = 2020 "
                       "RETURN id(a) AS aid, id(r) AS rid, id(b) AS bid");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(ada_), rg::Value(knows_),
                                    rg::Value(grace_)}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeRelationshipIndexRangeSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction,
      "MATCH (a)-[r:KNOWS]->(b) WHERE r.since >= 2020 AND r.since < 2021 "
      "RETURN id(a) AS aid, id(r) AS rid, id(b) AS bid");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(ada_), rg::Value(knows_),
                                    rg::Value(grace_)}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeNodeIdSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction, "MATCH (n) WHERE id(n) = " + std::to_string(ada_) +
                        " RETURN id(n) AS id, n.name AS name");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(ada_), rg::Value("Ada")}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeNodeIdInSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction, "MATCH (n) WHERE id(n) IN [" + std::to_string(ada_) +
                        ", 999999] "
                        "RETURN id(n) AS id");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0][0], rg::Value(ada_));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeRelationshipIdSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction,
      "MATCH (a)-[r:KNOWS]->(b) WHERE id(r) = " + std::to_string(knows_) +
          " RETURN id(a) AS aid, id(r) AS rid, id(b) AS bid, r.since AS since");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(ada_), rg::Value(knows_),
                                    rg::Value(grace_), rg::Value(2020)}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeRelationshipIdInSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction, "MATCH (a)-[r:KNOWS]->(b) WHERE id(r) IN [" +
                        std::to_string(knows_) +
                        ", 999999] RETURN id(r) AS id");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0][0], rg::Value(knows_));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeUntypedRelationshipIdSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction,
      "MATCH (a)-[r]->(b) WHERE id(r) = " + std::to_string(rare_) +
          " RETURN id(a) AS aid, id(r) AS rid, id(b) AS bid, r.weight AS "
          "weight");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(grace_), rg::Value(rare_),
                                    rg::Value(other_), rg::Value(7)}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeIncomingRelationshipIdSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction,
      "MATCH (a)<-[r:KNOWS]-(b) WHERE id(r) = " + std::to_string(knows_) +
          " RETURN id(a) AS aid, id(r) AS rid, id(b) AS bid");

  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(grace_), rg::Value(knows_),
                                    rg::Value(ada_)}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, ExecutesNativeUndirectedRelationshipIdSeek) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction,
      "MATCH (a)-[r:KNOWS]-(b) WHERE id(r) = " + std::to_string(knows_) +
          " RETURN id(a) AS aid, id(r) AS rid, id(b) AS bid");

  ASSERT_EQ(result.rows.size(), 2U);
  EXPECT_EQ(result.rows[0],
            (std::vector<rg::Value>{rg::Value(ada_), rg::Value(knows_),
                                    rg::Value(grace_)}));
  EXPECT_EQ(result.rows[1],
            (std::vector<rg::Value>{rg::Value(grace_), rg::Value(knows_),
                                    rg::Value(ada_)}));
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, MissingNativeEntityIdsProduceNoRows) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result = rg::ExecuteQuery(
      *transaction, "MATCH (n) WHERE id(n) = 999999 RETURN id(n) AS id");

  EXPECT_TRUE(result.rows.empty());
  transaction->Commit();
}

TEST_F(GraphDBQueryExecutorTest, MissingNativeRelationshipIdProducesNoRows) {
  auto transaction = graph_->BeginTransaction();
  rg::QueryResult result =
      rg::ExecuteQuery(*transaction,
                       "MATCH (a)-[r:KNOWS]->(b) WHERE id(r) = 999999 "
                       "RETURN id(r) AS id");

  EXPECT_TRUE(result.rows.empty());
  transaction->Commit();
}

}  // namespace
