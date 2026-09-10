#include <gtest/gtest.h>
#include <rocksdb/write_batch.h>

#include <filesystem>

#include "common/value.h"
#include "graphdb/graph_db.h"
#include "proto/meta.pb.h"
#include "test_util.h"
#include "transaction/transaction.h"
using namespace graphdb;
namespace fs = std::filesystem;
static std::string testdb = "testdb";
static std::unordered_map<std::string, Value> properties = {
    {"property1", Value::Bool(true)},
    {"property2", Value::Integer(100)},
    {"property3", Value::String("string")},
    {"property4", Value::Double(1.1314)},
    {"property5", Value::BoolArray({true, false})},
    {"property6", Value::IntegerArray({1, 2, 3})},
    {"property7", Value::StringArray({"string1", "string2"})},
    {"property8", Value::DoubleArray({11.11, 22.22})}};

TEST(Transaction, raftRequestCarriesWbData) {
  rocksdb::WriteBatch wb;
  ASSERT_TRUE(wb.Put("vertex", "alice").ok());
  ASSERT_TRUE(wb.Delete("stale").ok());

  meta::RaftRequest request;
  request.set_wb_kind(meta::WriteBatchKind::GRAPH_WRITE);
  request.set_wb_data(wb.Data());

  rocksdb::WriteBatch restored(request.wb_data());
  EXPECT_EQ(restored.Data(), wb.Data());
  EXPECT_EQ(restored.Count(), wb.Count());
  EXPECT_EQ(request.wb_kind(), meta::WriteBatchKind::GRAPH_WRITE);
}

TEST(Transaction, commitAndRollback) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  std::unordered_set<std::string> v2_labels = {"label3", "label4"};
  std::unordered_set<std::string> v3_labels = {"label5", "label6"};
  std::unordered_set<std::string> v4_labels = {"label7", "label8"};
  auto v1 = txn->CreateVertex(v1_labels, properties);
  auto v2 = txn->CreateVertex(v2_labels, properties);
  auto v3 = txn->CreateVertex(v3_labels, properties);
  auto v4 = txn->CreateVertex(v4_labels, properties);
  auto e1 = txn->CreateEdge(v1, v2, "edge_type12", properties);
  auto e2 = txn->CreateEdge(v2, v3, "edge_type23", properties);
  auto e3 = txn->CreateEdge(v3, v4, "edge_type34", properties);
  auto e4 = txn->CreateEdge(v4, v1, "edge_type41", properties);
  txn->Rollback();
  txn = graphDB->BeginTransaction();
  EXPECT_THROW_CODE(txn->GetVertexById(v1.GetId()), VertexIdNotFound);
  EXPECT_THROW_CODE(txn->GetVertexById(v2.GetId()), VertexIdNotFound);
  EXPECT_THROW_CODE(txn->GetVertexById(v3.GetId()), VertexIdNotFound);
  EXPECT_THROW_CODE(txn->GetVertexById(v4.GetId()), VertexIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e1.GetTypeId(), e1.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e2.GetTypeId(), e2.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e3.GetTypeId(), e3.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e4.GetTypeId(), e4.GetId()),
                    EdgeIdNotFound);
  txn->Commit();
  txn = graphDB->BeginTransaction();
  v1 = txn->CreateVertex(v1_labels, properties);
  v2 = txn->CreateVertex(v2_labels, properties);
  v3 = txn->CreateVertex(v3_labels, properties);
  v4 = txn->CreateVertex(v4_labels, properties);
  e1 = txn->CreateEdge(v1, v2, "edge_type12", properties);
  e2 = txn->CreateEdge(v2, v3, "edge_type23", properties);
  txn->Commit();
  txn = graphDB->BeginTransaction();
  EXPECT_NO_THROW(txn->GetVertexById(v1.GetId()));
  EXPECT_NO_THROW(txn->GetVertexById(v2.GetId()));
  EXPECT_NO_THROW(txn->GetVertexById(v3.GetId()));
  EXPECT_NO_THROW(txn->GetVertexById(v4.GetId()));
  EXPECT_NO_THROW(txn->GetEdgeById(e1.GetTypeId(), e1.GetId()));
  EXPECT_NO_THROW(txn->GetEdgeById(e2.GetTypeId(), e2.GetId()));
  EXPECT_THROW_CODE(txn->GetEdgeById(e3.GetTypeId(), e3.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e4.GetTypeId(), e4.GetId()),
                    EdgeIdNotFound);
  txn->Commit();
  txn = graphDB->BeginTransaction();
  e3 = txn->CreateEdge(v3, v4, "edge_type34", properties);
  e4 = txn->CreateEdge(v4, v1, "edge_type41", properties);
  txn->Rollback();
  txn = graphDB->BeginTransaction();
  EXPECT_THROW_CODE(txn->GetEdgeById(e3.GetTypeId(), e3.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e4.GetTypeId(), e4.GetId()),
                    EdgeIdNotFound);
  txn->Commit();
  txn = graphDB->BeginTransaction();
  e3 = txn->CreateEdge(v3, v4, "edge_type34", properties);
  e4 = txn->CreateEdge(v4, v1, "edge_type41", properties);
  txn->Commit();
  txn = graphDB->BeginTransaction();
  EXPECT_NO_THROW(txn->GetEdgeById(e3.GetTypeId(), e3.GetId()));
  EXPECT_NO_THROW(txn->GetEdgeById(e4.GetTypeId(), e4.GetId()));
  txn->Commit();
}

TEST(Transaction, commitWithRaftPersistsDataAndApplyIndex) {
  const std::string raft_testdb = "testdb_raft_txn_commit";
  fs::remove_all(raft_testdb);
  auto graphDB = GraphDB::Open(raft_testdb, testutil::NewGraphDBOptions());
  graphDB->db_meta().set_graph_name("txn_commit_graph");

  auto raft_driver = testutil::NewSingleNodeRaftDriver(
      graphDB.get(), "txn_commit_graph", raft_testdb + "/raft", 17687, 17688);
  auto* raft_driver_ptr = raft_driver.get();
  auto err = raft_driver->Run();
  if (err != nullptr) {
    FAIL() << err.String();
  }
  graphDB->SetRaftDriver(std::move(raft_driver));
  ASSERT_TRUE(testutil::WaitUntilRaftLeader(raft_driver_ptr));

  auto txn = graphDB->BeginTransaction();
  auto v1 = txn->CreateVertex({"label1"}, properties);
  auto v2 = txn->CreateVertex({"label2"}, properties);
  auto e1 = txn->CreateEdge(v1, v2, "edge_type12", properties);

  auto before_commit_index = graphDB->GetRaftApplyIndex();
  ASSERT_GT(before_commit_index, 0U);

  txn->Commit();

  auto after_commit_index = graphDB->GetRaftApplyIndex();
  EXPECT_GT(after_commit_index, before_commit_index);

  txn = graphDB->BeginTransaction();
  auto persisted_v1 = txn->GetVertexById(v1.GetId());
  auto persisted_v2 = txn->GetVertexById(v2.GetId());
  auto persisted_e1 = txn->GetEdgeById(e1.GetTypeId(), e1.GetId());
  EXPECT_EQ(persisted_v1.GetAllProperty(), properties);
  EXPECT_EQ(persisted_v2.GetAllProperty(), properties);
  EXPECT_EQ(persisted_e1.GetAllProperty(), properties);
  txn->Commit();

  txn.reset();
  graphDB.reset();
  graphDB = GraphDB::Open(raft_testdb, testutil::NewGraphDBOptions());
  EXPECT_EQ(graphDB->GetRaftApplyIndex(), after_commit_index);

  txn = graphDB->BeginTransaction();
  EXPECT_EQ(txn->GetVertexById(v1.GetId()).GetAllProperty(), properties);
  EXPECT_EQ(txn->GetVertexById(v2.GetId()).GetAllProperty(), properties);
  EXPECT_EQ(txn->GetEdgeById(e1.GetTypeId(), e1.GetId()).GetAllProperty(),
            properties);
  txn->Commit();
}

TEST(Transaction, rollbackWithRaftDoesNotAdvanceApplyIndex) {
  const std::string raft_testdb = "testdb_raft_txn_rollback";
  fs::remove_all(raft_testdb);
  auto graphDB = GraphDB::Open(raft_testdb, testutil::NewGraphDBOptions());
  graphDB->db_meta().set_graph_name("txn_rollback_graph");

  auto raft_driver = testutil::NewSingleNodeRaftDriver(
      graphDB.get(), "txn_rollback_graph", raft_testdb + "/raft", 17689, 17690);
  auto* raft_driver_ptr = raft_driver.get();
  auto err = raft_driver->Run();
  if (err != nullptr) {
    FAIL() << err.String();
  }
  graphDB->SetRaftDriver(std::move(raft_driver));
  ASSERT_TRUE(testutil::WaitUntilRaftLeader(raft_driver_ptr));

  auto txn = graphDB->BeginTransaction();
  auto v1 = txn->CreateVertex({"label1"}, properties);
  txn->Commit();

  auto committed_index = graphDB->GetRaftApplyIndex();
  ASSERT_GT(committed_index, 0U);

  txn = graphDB->BeginTransaction();
  auto vertex = txn->GetVertexById(v1.GetId());
  vertex.SetProperties({{"property2", Value::Integer(999)}});
  txn->Rollback();

  EXPECT_EQ(graphDB->GetRaftApplyIndex(), committed_index);

  txn = graphDB->BeginTransaction();
  vertex = txn->GetVertexById(v1.GetId());
  EXPECT_EQ(vertex.GetProperty("property2"), Value::Integer(100));
  txn->Commit();
}
