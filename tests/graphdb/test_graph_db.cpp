#include <gtest/gtest.h>

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <filesystem>
#include <unordered_set>
#include <vector>

#include "common/byte_utils.h"
#include "common/logger.h"
#include "graphdb/graph_db.h"
#include "test_util.h"
#include "transaction/transaction.h"
#include "value/value.h"

using rg::Value;
namespace fs = std::filesystem;
using boost::endian::big_to_native;
using boost::endian::native_to_big_inplace;
using namespace graphdb;
static std::string testdb = "testdb";
static std::unordered_map<std::string, Value> properties = {
    {"property1", Value(true)},
    {"property2", Value(100)},
    {"property3", Value("string")},
    {"property4", Value(1.1314)},
    {"property5", Value(Value::List{Value(true), Value(false)})},
    {"property6", Value(Value::List{Value(1), Value(2), Value(3)})},
    {"property7", Value(Value::List{Value("string1"), Value("string2")})},
    {"property8", Value(Value::List{Value(11.11), Value(22.22)})}};

TEST(GraphDB, assistantPoolRequired) {
  fs::remove_all(testdb);
  EXPECT_THROW_CODE_MSG(GraphDB::Open(testdb, {}), InvalidParameter,
                        "assistant_pool");
  EXPECT_THROW_CODE_MSG(AssistantPool(0), InvalidParameter,
                        "assistant thread num");
}

TEST(GraphDB, clearDataRejectsRaftGraph) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->db_meta().set_graph_name("raft_graph");
  graphDB->db_meta().set_enable_raft(true);

  EXPECT_THROW_CODE_MSG(graphDB->ClearData(), InvalidParameter,
                        "Galaxy::ClearGraph");
}

TEST(GraphDB, basicCreate) {
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
  EXPECT_LT(big_to_native(v1.GetId()), big_to_native(v2.GetId()));
  EXPECT_LT(big_to_native(v2.GetId()), big_to_native(v3.GetId()));
  EXPECT_LT(big_to_native(v3.GetId()), big_to_native(v4.GetId()));
  EXPECT_LT(big_to_native(e1.GetId()), big_to_native(e2.GetId()));
  EXPECT_LT(big_to_native(e2.GetId()), big_to_native(e3.GetId()));
  EXPECT_LT(big_to_native(e3.GetId()), big_to_native(e4.GetId()));
  EXPECT_EQ(v1.GetAllProperty(), properties);
  EXPECT_EQ(v2.GetAllProperty(), properties);
  EXPECT_EQ(v3.GetAllProperty(), properties);
  EXPECT_EQ(v4.GetAllProperty(), properties);
  EXPECT_EQ(e1.GetAllProperty(), properties);
  EXPECT_EQ(e2.GetAllProperty(), properties);
  EXPECT_EQ(e3.GetAllProperty(), properties);
  EXPECT_EQ(e4.GetAllProperty(), properties);
  EXPECT_EQ(v1.GetLabels(), v1_labels);
  EXPECT_EQ(v2.GetLabels(), v2_labels);
  EXPECT_EQ(v3.GetLabels(), v3_labels);
  EXPECT_EQ(v4.GetLabels(), v4_labels);
  EXPECT_EQ(e1.GetType(), "edge_type12");
  EXPECT_EQ(e2.GetType(), "edge_type23");
  EXPECT_EQ(e3.GetType(), "edge_type34");
  EXPECT_EQ(e4.GetType(), "edge_type41");
  txn->Commit();
}

TEST(GraphDB, reOpen) {
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
  txn->Commit();
  txn.reset();
  graphDB.reset();
  // reopen
  graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  txn = graphDB->BeginTransaction();
  v1 = txn->GetVertexById(v1.GetId());
  v2 = txn->GetVertexById(v2.GetId());
  v3 = txn->GetVertexById(v3.GetId());
  v4 = txn->GetVertexById(v4.GetId());
  e1 = txn->GetEdgeById(e1.GetTypeId(), e1.GetId());
  e2 = txn->GetEdgeById(e2.GetTypeId(), e2.GetId());
  e3 = txn->GetEdgeById(e3.GetTypeId(), e3.GetId());
  e4 = txn->GetEdgeById(e4.GetTypeId(), e4.GetId());
  EXPECT_LT(big_to_native(v1.GetId()), big_to_native(v2.GetId()));
  EXPECT_LT(big_to_native(v2.GetId()), big_to_native(v3.GetId()));
  EXPECT_LT(big_to_native(v3.GetId()), big_to_native(v4.GetId()));
  EXPECT_LT(big_to_native(e1.GetId()), big_to_native(e2.GetId()));
  EXPECT_LT(big_to_native(e2.GetId()), big_to_native(e3.GetId()));
  EXPECT_LT(big_to_native(e3.GetId()), big_to_native(e4.GetId()));
  EXPECT_EQ(v1.GetAllProperty(), properties);
  EXPECT_EQ(v2.GetAllProperty(), properties);
  EXPECT_EQ(v3.GetAllProperty(), properties);
  EXPECT_EQ(v4.GetAllProperty(), properties);
  EXPECT_EQ(e1.GetAllProperty(), properties);
  EXPECT_EQ(e2.GetAllProperty(), properties);
  EXPECT_EQ(e3.GetAllProperty(), properties);
  EXPECT_EQ(e4.GetAllProperty(), properties);
  EXPECT_EQ(v1.GetLabels(), v1_labels);
  EXPECT_EQ(v2.GetLabels(), v2_labels);
  EXPECT_EQ(v3.GetLabels(), v3_labels);
  EXPECT_EQ(v4.GetLabels(), v4_labels);
  EXPECT_EQ(e1.GetType(), "edge_type12");
  EXPECT_EQ(e2.GetType(), "edge_type23");
  EXPECT_EQ(e3.GetType(), "edge_type34");
  EXPECT_EQ(e4.GetType(), "edge_type41");
  txn->Commit();
}

TEST(GraphDB, entityIdRangeReOpen) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  auto v1 = txn->CreateVertex({"label1"}, {});
  auto v2 = txn->CreateVertex({"label2"}, {});
  auto e1 = txn->CreateEdge(v1, v2, "edge_type12", {});
  txn->Commit();

  auto max_vid = std::max(big_to_native(v1.GetId()), big_to_native(v2.GetId()));
  auto max_eid = big_to_native(e1.GetId());
  txn.reset();
  graphDB.reset();

  graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  txn = graphDB->BeginTransaction();
  auto existing = txn->GetVertexById(v2.GetId());
  auto v3 = txn->CreateVertex({"label3"}, {});
  auto e2 = txn->CreateEdge(existing, v3, "edge_type23", {});
  EXPECT_GT(big_to_native(v3.GetId()), max_vid);
  EXPECT_GT(big_to_native(e2.GetId()), max_eid);
  txn->Commit();
}

TEST(GraphDB, entityIdRangeRefill) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();

  std::vector<int64_t> vids;
  vids.reserve(1030);
  for (int i = 0; i < 1030; ++i) {
    auto v = txn->CreateVertex({"label1"}, {});
    vids.push_back(big_to_native(v.GetId()));
  }
  for (size_t i = 1; i < vids.size(); ++i) {
    EXPECT_LT(vids[i - 1], vids[i]);
  }

  auto start =
      txn->GetVertexById(boost::endian::native_to_big(static_cast<int64_t>(1)));
  auto end =
      txn->GetVertexById(boost::endian::native_to_big(static_cast<int64_t>(2)));
  std::vector<int64_t> eids;
  eids.reserve(1030);
  for (int i = 0; i < 1030; ++i) {
    auto e = txn->CreateEdge(start, end, "edge_type12", {});
    eids.push_back(big_to_native(e.GetId()));
  }
  for (size_t i = 1; i < eids.size(); ++i) {
    EXPECT_LT(eids[i - 1], eids[i]);
  }

  txn->Commit();
}

TEST(GraphDB, raftIdGeneratorPersistsStateAndApplyIndex) {
  const std::string raft_testdb = "testdb_raft_id_generator";
  fs::remove_all(raft_testdb);
  auto graphDB = GraphDB::Open(raft_testdb, testutil::NewGraphDBOptions());
  graphDB->db_meta().set_graph_name("id_generator_graph");

  auto raft_driver = testutil::NewSingleNodeRaftDriver(
      graphDB.get(), "id_generator_graph", raft_testdb + "/raft", 17691, 17692);
  auto* raft_driver_ptr = raft_driver.get();
  auto err = raft_driver->Run();
  if (err != nullptr) {
    FAIL() << err.String();
  }
  graphDB->SetRaftDriver(std::move(raft_driver));
  ASSERT_TRUE(testutil::WaitUntilRaftLeader(raft_driver_ptr));

  auto lid = graphDB->id_generator().GetOrCreateLid("person");
  auto pid = graphDB->id_generator().GetOrCreatePid("name");
  auto tid = graphDB->id_generator().GetOrCreateTid("knows");
  auto vid = graphDB->id_generator().GetNextVid();
  auto eid = graphDB->id_generator().GetNextEid();
  auto apply_index = graphDB->GetRaftApplyIndex();

  EXPECT_GT(apply_index, 0U);
  EXPECT_EQ(graphDB->id_generator().GetLid("person"), lid);
  EXPECT_EQ(graphDB->id_generator().GetPid("name"), pid);
  EXPECT_EQ(graphDB->id_generator().GetTid("knows"), tid);

  graphDB.reset();
  graphDB = GraphDB::Open(raft_testdb, testutil::NewGraphDBOptions());
  graphDB->db_meta().set_graph_name("id_generator_graph");
  EXPECT_EQ(graphDB->GetRaftApplyIndex(), apply_index);
  EXPECT_EQ(graphDB->id_generator().GetLid("person"), lid);
  EXPECT_EQ(graphDB->id_generator().GetPid("name"), pid);
  EXPECT_EQ(graphDB->id_generator().GetTid("knows"), tid);

  raft_driver = testutil::NewSingleNodeRaftDriver(
      graphDB.get(), "id_generator_graph", raft_testdb + "/raft", 17691, 17692);
  raft_driver_ptr = raft_driver.get();
  err = raft_driver->Run();
  if (err != nullptr) {
    FAIL() << err.String();
  }
  graphDB->SetRaftDriver(std::move(raft_driver));
  ASSERT_TRUE(testutil::WaitUntilRaftLeader(raft_driver_ptr));

  auto next_vid = graphDB->id_generator().GetNextVid();
  auto next_eid = graphDB->id_generator().GetNextEid();
  EXPECT_GT(big_to_native(next_vid), big_to_native(vid));
  EXPECT_GT(big_to_native(next_eid), big_to_native(eid));
  EXPECT_GT(graphDB->GetRaftApplyIndex(), apply_index);
}

TEST(GraphDB, raftApplyUpdatesIdGeneratorCacheWithoutRestart) {
  const std::string raft_testdb = "testdb_raft_apply_id_generator_cache";
  fs::remove_all(raft_testdb);
  auto graphDB = GraphDB::Open(raft_testdb, testutil::NewGraphDBOptions());
  graphDB->db_meta().set_graph_name("id_generator_graph");

  constexpr uint32_t kLabelId = 7;
  constexpr uint32_t kPropertyId = 9;
  constexpr uint32_t kEdgeTypeId = 11;
  constexpr int64_t kNextVid = 1025;
  constexpr int64_t kNextEid = 2049;

  uint32_t lid = boost::endian::native_to_big(kLabelId);
  uint32_t pid = boost::endian::native_to_big(kPropertyId);
  uint32_t tid = boost::endian::native_to_big(kEdgeTypeId);
  int64_t next_vid = boost::endian::native_to_big(kNextVid);
  int64_t next_eid = boost::endian::native_to_big(kNextEid);

  rocksdb::WriteBatch wb;
  auto s = wb.Put(
      graphDB->graph_cf().meta_info,
      std::string(1, static_cast<char>(MetaDataType::VertexLabel)) + "person",
      std::string(common::AsChars(lid), sizeof(lid)));
  ASSERT_TRUE(s.ok());
  s = wb.Put(graphDB->graph_cf().meta_info,
             std::string(1, static_cast<char>(MetaDataType::Property)) + "name",
             std::string(common::AsChars(pid), sizeof(pid)));
  ASSERT_TRUE(s.ok());
  s = wb.Put(
      graphDB->graph_cf().meta_info,
      std::string(1, static_cast<char>(MetaDataType::EdgeType)) + "knows",
      std::string(common::AsChars(tid), sizeof(tid)));
  ASSERT_TRUE(s.ok());
  s = wb.Put(graphDB->graph_cf().meta_info,
             std::string(1, static_cast<char>(MetaDataType::NextVertexId)),
             std::string(common::AsChars(next_vid), sizeof(next_vid)));
  ASSERT_TRUE(s.ok());
  s = wb.Put(graphDB->graph_cf().meta_info,
             std::string(1, static_cast<char>(MetaDataType::NextEdgeId)),
             std::string(common::AsChars(next_eid), sizeof(next_eid)));
  ASSERT_TRUE(s.ok());

  meta::RaftRequest request;
  request.set_wb_kind(meta::WriteBatchKind::ID_GENERATOR);
  request.set_wb_data(wb.Data());
  graphDB->ApplyRaftRequest(1, request);

  EXPECT_EQ(graphDB->GetRaftApplyIndex(), 1U);
  EXPECT_EQ(graphDB->id_generator().GetLid("person"), lid);
  EXPECT_EQ(graphDB->id_generator().GetPid("name"), pid);
  EXPECT_EQ(graphDB->id_generator().GetTid("knows"), tid);
  EXPECT_EQ(graphDB->id_generator().GetOrCreateLid("company"),
            boost::endian::native_to_big(kLabelId + 1));
  EXPECT_EQ(graphDB->id_generator().GetOrCreatePid("age"),
            boost::endian::native_to_big(kPropertyId + 1));
  EXPECT_EQ(graphDB->id_generator().GetOrCreateTid("likes"),
            boost::endian::native_to_big(kEdgeTypeId + 1));
  EXPECT_EQ(big_to_native(graphDB->id_generator().GetNextVid()), kNextVid);
  EXPECT_EQ(big_to_native(graphDB->id_generator().GetNextEid()), kNextEid);
}

TEST(GraphDB, updateProperty) {
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
  txn->CreateEdge(v2, v3, "edge_type23", properties);
  txn->CreateEdge(v3, v4, "edge_type34", properties);
  txn->CreateEdge(v4, v1, "edge_type41", properties);
  v1.SetProperties({{"property1", Value(false)}});
  EXPECT_EQ(v1.GetProperty("property1"), Value(false));
  v1.SetProperties({{"property1", Value("str1")}});
  EXPECT_EQ(v1.GetProperty("property1"), Value("str1"));
  v1.RemoveProperty("property1");
  EXPECT_EQ(v1.GetAllProperty().size(), 7);
  v1.SetProperties(
      {{"property9", Value(Value::List{Value(10), Value(20), Value(30)})}});
  EXPECT_EQ(v1.GetProperty("property9"),
            Value(Value::List{Value(10), Value(20), Value(30)}));
  EXPECT_EQ(v1.GetAllProperty().size(), 8);

  e1.SetProperties({{"property1", Value(false)}});
  EXPECT_EQ(e1.GetProperty("property1"), Value(false));
  e1.SetProperties({{"property1", Value("str1")}});
  EXPECT_EQ(e1.GetProperty("property1"), Value("str1"));
  e1.RemoveProperty("property1");
  EXPECT_EQ(e1.GetAllProperty().size(), 7);
  e1.SetProperties(
      {{"property9", Value(Value::List{Value(10), Value(20), Value(30)})}});
  EXPECT_EQ(e1.GetProperty("property9"),
            Value(Value::List{Value(10), Value(20), Value(30)}));
  EXPECT_EQ(e1.GetAllProperty().size(), 8);
}

TEST(GraphDB, vertexIterator) {
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
  txn->CreateEdge(v1, v2, "edge_type12", properties);
  txn->CreateEdge(v2, v3, "edge_type23", properties);
  txn->CreateEdge(v3, v4, "edge_type34", properties);
  txn->CreateEdge(v4, v1, "edge_type41", properties);
  int count = 0;
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    count++;
    EXPECT_EQ(viter->GetVertex().GetAllProperty(), properties);
    if (count == 1) {
      EXPECT_EQ(viter->GetVertex().GetLabels(), v1_labels);
    } else if (count == 2) {
      EXPECT_EQ(viter->GetVertex().GetLabels(), v2_labels);
    } else if (count == 3) {
      EXPECT_EQ(viter->GetVertex().GetLabels(), v3_labels);
    } else if (count == 4) {
      EXPECT_EQ(viter->GetVertex().GetLabels(), v4_labels);
    }
  }
  EXPECT_EQ(count, 4);
  for (int i = 1; i <= 8; i++) {
    std::string label = "label" + std::to_string(i);
    count = 0;
    for (auto viter = txn->NewVertexIterator(label); viter->Valid();
         viter->Next()) {
      count++;
    }
    EXPECT_EQ(count, 1);
  }
  for (int i = 1; i <= 8; i++) {
    std::string label = "label" + std::to_string(i);
    count = 0;
    for (auto viter = txn->NewVertexIterator(
             label, std::unordered_map<std::string, Value>{{"property3",
                                                            Value("string")}});
         viter->Valid(); viter->Next()) {
      count++;
    }
    EXPECT_EQ(count, 1);
  }
  for (int i = 1; i <= 8; i++) {
    std::string label = "label" + std::to_string(i);
    count = 0;
    for (auto viter = txn->NewVertexIterator(
             label, std::unordered_map<std::string, Value>{{"property3",
                                                            Value("string")}});
         viter->Valid(); viter->Next()) {
      count++;
    }
    EXPECT_EQ(count, 1);
  }
  for (int i = 1; i <= 8; i++) {
    std::string label = "label" + std::to_string(i);
    count = 0;
    for (auto viter =
             txn->NewVertexIterator(label,
                                    std::unordered_map<std::string, Value>{
                                        {"property3", Value("wrong_string")}});
         viter->Valid(); viter->Next()) {
      count++;
    }
    EXPECT_EQ(count, 0);
  }
  for (int i = 1; i <= 8; i++) {
    std::string label = "label" + std::to_string(i);
    count = 0;
    for (auto viter = txn->NewVertexIterator(
             label, std::unordered_map<std::string, Value>{{"wrong_property",
                                                            Value("string")}});
         viter->Valid(); viter->Next()) {
      count++;
    }
    EXPECT_EQ(count, 0);
  }

  count = 0;
  for (auto viter = txn->NewVertexIterator("wrong_label"); viter->Valid();
       viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);

  txn->Commit();
}

TEST(GraphDB, edgeIterator) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  std::unordered_set<std::string> v2_labels = {"label3", "label4"};
  std::unordered_set<std::string> v3_labels = {"label5", "label6"};
  std::unordered_set<std::string> v4_labels = {"label7", "label8"};
  auto v0 = txn->CreateVertex({}, {});
  auto v1 = txn->CreateVertex(v1_labels, properties);
  auto v2 = txn->CreateVertex(v2_labels, properties);
  auto v3 = txn->CreateVertex(v3_labels, properties);
  auto v4 = txn->CreateVertex(v4_labels, properties);
  auto e1 = txn->CreateEdge(v1, v2, "edge_type12", properties);
  auto e2 = txn->CreateEdge(v2, v3, "edge_type23", properties);
  auto e3 = txn->CreateEdge(v3, v4, "edge_type34", properties);
  auto e4 = txn->CreateEdge(v4, v1, "edge_type41", properties);
  int count = 0;

  for (auto eiter = v0.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto eiter = v0.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);

  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e1);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type12");
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e4);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type41");
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v2.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e2);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type23");
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v2.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e1);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type12");
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v3.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e3);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type34");
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v3.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e2);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type23");
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v4.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e4);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type41");
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v4.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    EXPECT_EQ(eiter->GetEdge(), e3);
    EXPECT_EQ(eiter->GetEdge().GetType(), "edge_type34");
  }
  EXPECT_EQ(count, 1);

  count = 0;
  for (auto eiter = v0.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v2.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v3.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v4.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::OUTGOING, {},
                                       {{"property2", Value(100)}});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {},
                                       {{"property2", Value(100)}});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {"wrong_edge_type"},
                                       {{"property2", Value(100)}});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {},
                                       {{"wrong_property", Value(100)}});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {},
                                       {{"property2", Value(1000)}});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);

  txn->Commit();
}

TEST(GraphDB, scanEdgesByTypes) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());

  auto count_edges = [](std::unique_ptr<EdgeIterator> iterator) {
    size_t count = 0;
    for (; iterator->Valid(); iterator->Next()) {
      ++count;
    }
    return count;
  };

  {
    auto txn = graphDB->BeginTransaction();
    EXPECT_EQ(count_edges(txn->NewEdgeIterator()), 0);
    auto from = txn->CreateVertex({}, {});
    auto to = txn->CreateVertex({}, {});
    txn->CreateEdge(from, to, "ROLLS_BACK", {});
    EXPECT_EQ(count_edges(txn->NewEdgeIterator()), 1);
    txn->Rollback();
  }

  int64_t first_id;
  int64_t second_id;
  int64_t third_id;
  {
    auto txn = graphDB->BeginTransaction();
    EXPECT_EQ(count_edges(txn->NewEdgeIterator()), 0);
    EXPECT_EQ(count_edges(txn->NewEdgeIterator({"ROLLS_BACK"})), 0);
    auto first_vertex = txn->CreateVertex({}, {});
    auto second_vertex = txn->CreateVertex({}, {});
    auto first = txn->CreateEdge(first_vertex, second_vertex, "KNOWS", {});
    auto second = txn->CreateEdge(second_vertex, second_vertex, "KNOWS", {});
    auto third = txn->CreateEdge(second_vertex, first_vertex, "LIKES", {});
    first_id = first.GetId();
    second_id = second.GetId();
    third_id = third.GetId();

    std::unordered_set<int64_t> ids;
    auto iterator = txn->NewEdgeIterator();
    for (; iterator->Valid(); iterator->Next()) {
      const auto& edge = iterator->GetEdge();
      ids.insert(edge.GetId());
      if (edge.GetId() == first_id) {
        EXPECT_EQ(edge, first);
      } else if (edge.GetId() == second_id) {
        EXPECT_EQ(edge, second);
      } else if (edge.GetId() == third_id) {
        EXPECT_EQ(edge, third);
      } else {
        ADD_FAILURE() << "unexpected edge id";
      }
    }
    EXPECT_EQ(ids,
              (std::unordered_set<int64_t>{first_id, second_id, third_id}));
    EXPECT_EQ(count_edges(txn->NewEdgeIterator({"KNOWS"})), 2);
    EXPECT_EQ(count_edges(txn->NewEdgeIterator({"KNOWS", "MISSING"})), 2);
    EXPECT_EQ(count_edges(txn->NewEdgeIterator({"LIKES"})), 1);
    EXPECT_EQ(count_edges(txn->NewEdgeIterator({"MISSING"})), 0);
    txn->Commit();
  }

  graphDB.reset();
  graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  {
    auto txn = graphDB->BeginTransaction();
    EXPECT_EQ(count_edges(txn->NewEdgeIterator()), 3);
    EXPECT_EQ(count_edges(txn->NewEdgeIterator({"KNOWS"})), 2);

    auto iterator = txn->NewEdgeIterator({"LIKES"});
    ASSERT_TRUE(iterator->Valid());
    EXPECT_EQ(iterator->GetEdge().GetId(), third_id);
    EXPECT_EQ(iterator->GetEdge().GetType(), "LIKES");
    auto edge = iterator->GetEdge();
    iterator.reset();
    edge.Delete();
    EXPECT_EQ(count_edges(txn->NewEdgeIterator()), 2);
    EXPECT_EQ(count_edges(txn->NewEdgeIterator({"LIKES"})), 0);
    txn->Commit();
  }
}

TEST(GraphDB, deleteVertex) {
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
  txn->CreateEdge(v2, v3, "edge_type23", properties);
  txn->CreateEdge(v3, v4, "edge_type34", properties);
  auto e4 = txn->CreateEdge(v4, v1, "edge_type41", properties);

  v1.Delete();
  EXPECT_THROW_CODE(txn->GetVertexById(v1.GetId()), VertexIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e1.GetTypeId(), e1.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e4.GetTypeId(), e4.GetId()),
                    EdgeIdNotFound);
  int count = 0;
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 3);
  count = 0;
  for (auto eiter = v2.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto eiter = v2.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v3.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v4.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto eiter = v4.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();
}

TEST(GraphDB, deleteEdge) {
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
  txn->CreateEdge(v2, v3, "edge_type23", properties);
  txn->CreateEdge(v3, v4, "edge_type34", properties);
  txn->CreateEdge(v4, v1, "edge_type41", properties);

  e1.Delete();
  EXPECT_THROW_CODE(txn->GetEdgeById(e1.GetTypeId(), e1.GetId()),
                    EdgeIdNotFound);
  int count = 0;
  for (auto viter = v1.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto viter = v2.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto viter = v2.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto viter = v3.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto viter = v4.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  txn->Commit();
}

TEST(GraphDB, deleteAllVertex) {
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
  int count = 0;
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    viter->GetVertex().Delete();
    count++;
  }
  EXPECT_EQ(count, 4);
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
  count = 0;
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  txn->Commit();
}

TEST(GraphDB, scanAndUpdate) {
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
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    viter->GetVertex().SetProperties({{"property9", Value(100)}});
    EXPECT_EQ(viter->GetVertex().GetProperty("property9"), Value(100));
    EXPECT_EQ(viter->GetVertex().GetAllProperty().size(), 9);
  }
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertex().GetProperty("property9"), Value(100));
    EXPECT_EQ(viter->GetVertex().GetAllProperty().size(), 9);
  }
  int count = 0;
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    for (auto eiter = viter->GetVertex().NewEdgeIterator(
             EdgeDirection::OUTGOING, {}, {});
         eiter->Valid(); eiter->Next()) {
      eiter->GetEdge().Delete();
      count++;
    }
  }
  EXPECT_EQ(count, 4);
  EXPECT_THROW_CODE(txn->GetEdgeById(e1.GetTypeId(), e1.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e2.GetTypeId(), e2.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e3.GetTypeId(), e3.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e4.GetTypeId(), e4.GetId()),
                    EdgeIdNotFound);
  txn->Commit();
}

TEST(GraphDB, addDeleteLabel) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  auto v1 = txn->CreateVertex(v1_labels, properties);
  v1.AddLabels({"label1", "labelv1"});
  {
    std::unordered_set<std::string> labels = {"label1", "label2", "labelv1"};
    EXPECT_EQ(v1.GetLabels(), labels);
  }
  int count = 0;
  for (auto viter = txn->NewVertexIterator("labelv1"); viter->Valid();
       viter->Next()) {
    EXPECT_EQ(viter->GetVertex(), v1);
    std::unordered_set<std::string> labels = {"label1", "label2", "labelv1"};
    EXPECT_EQ(viter->GetVertex().GetLabels(), labels);
    count++;
  }
  EXPECT_EQ(count, 1);
  v1.DeleteLabels({"labelv1"});
  {
    std::unordered_set<std::string> labels = {"label1", "label2"};
    EXPECT_EQ(v1.GetLabels(), labels);
  }
  v1.DeleteLabels({"label_no_exist"});
  {
    std::unordered_set<std::string> labels = {"label1", "label2"};
    EXPECT_EQ(v1.GetLabels(), labels);
  }
  count = 0;
  for (auto viter = txn->NewVertexIterator("labelv1"); viter->Valid();
       viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  txn->Commit();
}

TEST(GraphDB, expandEdge) {
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
  txn->CreateEdge(v1, v2, "edge_type12", properties);
  txn->CreateEdge(v2, v3, "edge_type23", properties);
  txn->CreateEdge(v3, v4, "edge_type34", properties);
  txn->CreateEdge(v4, v1, "edge_type41", properties);
  bool found = false;
  for (auto viter = txn->NewVertexIterator("label1"); viter->Valid();
       viter->Next()) {
    found = true;
    Vertex v = v1;
    int count = 0;
    for (auto eiter = viter->GetVertex().NewEdgeIterator(
             EdgeDirection::OUTGOING, {}, {});
         eiter->Valid(); eiter->Next()) {
      v = eiter->GetEdge().GetEnd();
      EXPECT_EQ(v, v2);
      count++;
      break;
    }
    EXPECT_EQ(count, 1);
    count = 0;
    for (auto eiter = v.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
         eiter->Valid(); eiter->Next()) {
      v = eiter->GetEdge().GetEnd();
      EXPECT_EQ(v, v3);
      count++;
      break;
    }
    EXPECT_EQ(count, 1);
    count = 0;
    for (auto eiter = v.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
         eiter->Valid(); eiter->Next()) {
      v = eiter->GetEdge().GetEnd();
      EXPECT_EQ(v, v4);
      count++;
      break;
    }
    EXPECT_EQ(count, 1);
    count = 0;
    for (auto eiter = v.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
         eiter->Valid(); eiter->Next()) {
      v = eiter->GetEdge().GetEnd();
      EXPECT_EQ(v, v1);
      count++;
      break;
    }
    EXPECT_EQ(count, 1);
    break;
  }
  EXPECT_EQ(found, true);
  txn->Commit();
}

TEST(GraphDB, graphTypes) {
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
  txn->CreateEdge(v1, v2, "edge_type12", properties);
  txn->CreateEdge(v2, v3, "edge_type23", properties);
  txn->CreateEdge(v3, v4, "edge_type34", properties);
  txn->CreateEdge(v4, v1, "edge_type41", properties);
  std::unordered_set<std::string> vertex_labels = {"label1", "label2", "label3",
                                                   "label4", "label5", "label6",
                                                   "label7", "label8"};
  std::unordered_set<std::string> edge_types = {"edge_type12", "edge_type23",
                                                "edge_type34", "edge_type41"};
  std::unordered_set<std::string> properties_ = {
      "property1", "property2", "property3", "property4",
      "property5", "property6", "property7", "property8"};
  EXPECT_EQ(txn->db()->id_generator().GetVertexLabels(), vertex_labels);
  EXPECT_EQ(txn->db()->id_generator().GetEdgeTypes(), edge_types);
  EXPECT_EQ(txn->db()->id_generator().GetProperties(), properties_);
  txn->Commit();
}

TEST(GraphDB, pointToSelf) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  auto v1 = txn->CreateVertex(v1_labels, properties);
  auto e1 = txn->CreateEdge(v1, v1, "edge_type1", properties);
  int count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge(), e1);
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge(), e1);
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge(), e1);
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 2);

  auto e2 = txn->CreateEdge(v1, v1, "edge_type1", properties);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 2);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 4);

  e2.Delete();
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::OUTGOING, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::INCOMING, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto eiter = v1.NewEdgeIterator(EdgeDirection::BOTH, {}, {});
       eiter->Valid(); eiter->Next()) {
    EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
    count++;
  }
  EXPECT_EQ(count, 2);

  v1.Delete();

  EXPECT_THROW_CODE(txn->GetVertexById(v1.GetId()), VertexIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e1.GetTypeId(), e1.GetId()),
                    EdgeIdNotFound);
  EXPECT_THROW_CODE(txn->GetEdgeById(e2.GetTypeId(), e2.GetId()),
                    EdgeIdNotFound);

  txn->Commit();
}
