#include <gtest/gtest.h>

#include <filesystem>
#include <random>

#include "common/logger.h"
#include "graphdb/graph_db.h"
#include "test_util.h"
#include "transaction/transaction.h"
#include "value/value.h"

using rg::Value;
namespace fs = std::filesystem;
using namespace graphdb;
static std::string testdb = "testdb";
std::unordered_set<std::string> labels = {"label1", "label2"};
static std::unordered_map<std::string, Value> properties = {
    {"property1", Value(true)},
    {"property2", Value(100)},
    {"property3", Value("string")},
    {"property4", Value(1.1314)},
    {"property5", Value(Value::List{Value(true), Value(false)})},
    {"property6", Value(Value::List{Value(1), Value(2), Value(3)})},
    {"property7", Value(Value::List{Value("string1"), Value("string2")})},
    {"property8", Value(Value::List{Value(11.11), Value(22.22)})}};

static void CreateVertex(GraphDB* db) {
  auto txn = db->BeginTransaction();
  for (auto i = 0; i < 1000; i++) {
    txn->CreateVertex(labels, properties);
  }
  txn->Commit();
}

TEST(Concurrency, vertex) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::vector<std::thread> threads;
  for (size_t i = 0; i < 10; i++) {
    threads.emplace_back(CreateVertex, graphDB.get());
  }
  for (auto& t : threads) {
    t.join();
  }
  auto txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    auto& v = viter->GetVertex();
    count++;
    EXPECT_EQ(v.GetAllProperty(), properties);
    EXPECT_EQ(v.GetLabels(), labels);
  }
  EXPECT_EQ(count, 10000);
  txn->Commit();
}

static void CreateEdge(GraphDB* db, int index) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(index * 10, index * 10 + 9);
  auto txn = db->BeginTransaction();
  for (auto i = 0; i < 1000; i++) {
    int start = dis(gen);
    int end = dis(gen);
    auto v1iter = txn->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(start)}});
    EXPECT_TRUE(v1iter->Valid());
    auto v2iter = txn->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(end)}});
    EXPECT_TRUE(v2iter->Valid());
    txn->CreateEdge(v1iter->GetVertex(), v2iter->GetVertex(), "edge1",
                    properties);
  }
  txn->Commit();
}

TEST(Concurrency, edge) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels,
                      {{"id", Value(i)}, {"str", Value(std::to_string(i))}});
  }
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  std::vector<std::thread> threads;
  for (size_t i = 0; i < 10; i++) {
    threads.emplace_back(CreateEdge, graphDB.get(), i);
  }
  for (auto& t : threads) {
    t.join();
  }
  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    for (auto eiter = viter->GetVertex().NewEdgeIterator(
             EdgeDirection::OUTGOING, {}, {});
         eiter->Valid(); eiter->Next()) {
      EXPECT_EQ(eiter->GetEdge().GetAllProperty(), properties);
      count++;
    }
  }
  EXPECT_EQ(count, 10000);
  txn->Commit();
}

TEST(Concurrency, vertexConflict) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  auto txn1 = graphDB->BeginTransaction();
  txn1->CreateVertex({"label1"}, {{"id", Value(1)}, {"str", Value("1")}});
  auto txn2 = graphDB->BeginTransaction();
  EXPECT_THROW_CODE_MSG(
      txn2->CreateVertex({"label1"}, {{"id", Value(1)}, {"str", Value("2")}}),
      StorageEngineError, "Timeout waiting to lock key");
  txn2->Rollback();
  txn1->Commit();
  auto txn3 = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn3->NewVertexIterator(); viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertex().GetProperty("str"), Value("1"));
    count++;
  }
  EXPECT_EQ(count, 1);
  txn3->Commit();
}

TEST(Concurrency, edgeConflict) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 3; i++) {
    txn->CreateVertex(v1_labels,
                      {{"id", Value(i)}, {"str", Value(std::to_string(i))}});
  }
  txn->Commit();
  auto txn1 = graphDB->BeginTransaction();
  {
    auto v1iter = txn1->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(0)}});
    EXPECT_TRUE(v1iter->Valid());
    auto v2iter = txn1->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
    EXPECT_TRUE(v2iter->Valid());
    txn1->CreateEdge(v1iter->GetVertex(), v2iter->GetVertex(), "edge", {});
  }
  auto txn2 = graphDB->BeginTransaction();
  {
    auto v1iter = txn1->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
    EXPECT_TRUE(v1iter->Valid());
    auto v2iter = txn1->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(2)}});
    EXPECT_TRUE(v2iter->Valid());
    EXPECT_THROW_CODE_MSG(
        txn2->CreateEdge(v1iter->GetVertex(), v2iter->GetVertex(), "edge", {}),
        StorageEngineError, "Timeout waiting to lock key");
  }
  txn2->Rollback();
  txn1->Commit();
  auto txn3 = graphDB->BeginTransaction();
  {
    int count = 0;
    for (auto viter = txn1->NewVertexIterator(
             "label1",
             std::unordered_map<std::string, Value>{{"id", Value(0)}});
         viter->Valid(); viter->Next()) {
      for (auto eiter = viter->GetVertex().NewEdgeIterator(
               EdgeDirection::OUTGOING, {}, {});
           eiter->Valid(); eiter->Next()) {
        count++;
      }
    }
    EXPECT_EQ(count, 1);
    count = 0;
    for (auto viter = txn1->NewVertexIterator(
             "label1",
             std::unordered_map<std::string, Value>{{"id", Value(1)}});
         viter->Valid(); viter->Next()) {
      for (auto eiter = viter->GetVertex().NewEdgeIterator(
               EdgeDirection::OUTGOING, {}, {});
           eiter->Valid(); eiter->Next()) {
        count++;
      }
    }
    EXPECT_EQ(count, 0);
  }
  txn3->Commit();
}

TEST(Concurrency, edgeEntityLockSerializesDifferentPropertyUpdates) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());

  auto setup = graphDB->BeginTransaction();
  auto v1 = setup->CreateVertex({"label1"}, {{"id", Value(1)}});
  auto v2 = setup->CreateVertex({"label1"}, {{"id", Value(2)}});
  auto edge = setup->CreateEdge(v1, v2, "edge", {});
  setup->Commit();

  auto txn1 = graphDB->BeginTransaction();
  auto edge1 = txn1->GetEdgeById(edge.GetTypeId(), edge.GetId());
  edge1.SetProperties({{"property_a", Value(1)}});

  auto txn2 = graphDB->BeginTransaction();
  auto edge2 = txn2->GetEdgeById(edge.GetTypeId(), edge.GetId());
  EXPECT_THROW_CODE_MSG(edge2.SetProperties({{"property_b", Value(2)}}),
                        StorageEngineError, "Timeout waiting to lock key");
  txn2->Rollback();

  txn1->Commit();

  auto verify = graphDB->BeginTransaction();
  auto persisted = verify->GetEdgeById(edge.GetTypeId(), edge.GetId());
  auto all_properties = persisted.GetAllProperty();
  EXPECT_EQ(all_properties.size(), 1);
  auto iter = all_properties.find("property_a");
  ASSERT_NE(iter, all_properties.end());
  EXPECT_EQ(iter->second, Value(1));
  EXPECT_EQ(all_properties.count("property_b"), 0);
  verify->Commit();
}
