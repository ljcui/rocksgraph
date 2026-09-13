#include <gtest/gtest.h>

#include <boost/endian/conversion.hpp>
#include <filesystem>
#include <memory>

#include "graphdb/assistant_pool.h"
#include "graphdb/graph_db.h"
#include "runtime/query_executor.h"
#include "tests/planner/fake_planner_statistics.h"
#include "transaction/transaction.h"

namespace {

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
    ada_ = transaction->CreateVertex({"Person"}, {{"name", rg::Value("Ada")}})
               .GetNativeId();
    grace_ =
        transaction->CreateVertex({"Person"}, {{"name", rg::Value("Grace")}})
            .GetNativeId();
    other_ =
        transaction->CreateVertex({"Person"}, {{"name", rg::Value("Other")}})
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

}  // namespace
