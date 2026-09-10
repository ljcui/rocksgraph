#include <gtest/gtest.h>

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

#include "common/byte_utils.h"
#include "common/value.h"
#include "graphdb/graph_db.h"
#include "test_util.h"
#include "transaction/transaction.h"
namespace fs = std::filesystem;
static std::string testdb = "testdb";
using namespace graphdb;

namespace {

std::vector<int64_t> CollectVertexPropertyIndexVids(
    txn::Transaction* txn, const std::shared_ptr<VertexPropertyIndex>& index,
    const std::vector<Value>& values) {
  rocksdb::ReadOptions ro;
  std::vector<int64_t> vids;
  std::string prefix = index->IndexKey(values);
  if (index->is_unique()) {
    std::string index_val;
    auto s = txn->dbtxn()->Get(ro, index->cf(), prefix, &index_val);
    if (s.ok()) {
      vids.push_back(common::ReadValue<int64_t>(index_val.data()));
    } else if (!s.IsNotFound()) {
      throw std::runtime_error(s.ToString());
    }
    return vids;
  }

  std::unique_ptr<rocksdb::Iterator> iter(
      txn->dbtxn()->GetIterator(ro, index->cf()));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key();
    key.remove_prefix(prefix.size());
    if (key.size() != sizeof(int64_t)) {
      throw std::runtime_error("invalid non-unique vertex index entry");
    }
    vids.push_back(common::ReadValue<int64_t>(key.data()));
  }
  std::sort(vids.begin(), vids.end());
  return vids;
}

std::vector<int64_t> CollectVertexIds(
    std::unique_ptr<graphdb::VertexIterator> viter) {
  std::vector<int64_t> vids;
  for (; viter->Valid(); viter->Next()) {
    vids.push_back(viter->GetVertex().GetId());
  }
  std::sort(vids.begin(), vids.end());
  return vids;
}

}  // namespace

TEST(VertexUniqueIndex, basic) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  txn = graphDB->BeginTransaction();
  for (auto i = 0; i < 100; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_TRUE(viter->Valid());
    EXPECT_EQ(viter->GetVertex().GetProperty("str"),
              Value::String(std::to_string(i)));
  }
  for (auto i = 100; i < 110; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_FALSE(viter->Valid());
  }
  for (auto i = 0; i < 100; i++) {
    auto viter = txn->NewVertexIterator(
        "label2",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<ScanVertexBylabelProperties*>(viter.get()));
    EXPECT_TRUE(viter->Valid());
  }
  for (auto i = 0; i < 100; i++) {
    auto viter = txn->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{
                      {"str", Value::String(std::to_string(i))}});
    EXPECT_TRUE(dynamic_cast<ScanVertexBylabelProperties*>(viter.get()));
    EXPECT_TRUE(viter->Valid());
  }
  txn->Commit();
}

TEST(VertexUniqueIndex, delete) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  txn = graphDB->BeginTransaction();
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    if (viter->GetVertex().GetProperty("id").AsInteger() < 10) {
      viter->GetVertex().Delete();
    }
  }
  for (auto i = 0; i < 10; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_FALSE(viter->Valid());
  }
  for (auto i = 10; i < 100; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_TRUE(viter->Valid());
    EXPECT_EQ(viter->GetVertex().GetProperty("str"),
              Value::String(std::to_string(i)));
  }
  txn->Commit();
}

TEST(VertexUniqueIndex, addLabelMaintainsIndex) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  auto indexed = txn->CreateVertex(
      {"label1"}, {{"id", Value::Integer(1)}, {"str", Value::String("v1")}});
  auto addable = txn->CreateVertex(
      {"label2"}, {{"id", Value::Integer(2)}, {"str", Value::String("v2")}});
  auto conflict = txn->CreateVertex(
      {"label2"}, {{"id", Value::Integer(1)}, {"str", Value::String("v3")}});
  auto indexed_id = indexed.GetId();
  auto addable_id = addable.GetId();
  auto conflict_id = conflict.GetId();
  txn->Commit();

  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));

  txn = graphDB->BeginTransaction();
  txn->GetVertexById(addable_id).AddLabels({"label1"});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(2)}});
  EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetId(), addable_id);
  EXPECT_EQ(viter->GetVertex().GetProperty("str"), Value::String("v2"));
  txn->Commit();

  txn = graphDB->BeginTransaction();
  EXPECT_THROW_CODE(txn->GetVertexById(conflict_id).AddLabels({"label1"}),
                    IndexValueAlreadyExist);
  txn->Rollback();

  txn = graphDB->BeginTransaction();
  viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetId(), indexed_id);
  txn->Commit();
}

TEST(VertexUniqueIndex, deleteLabelMaintainsIndex) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  auto removable = txn->CreateVertex(
      {"label1", "label2"},
      {{"id", Value::Integer(1)}, {"str", Value::String("remove_me")}});
  auto removable_id = removable.GetId();
  txn->Commit();

  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));

  txn = graphDB->BeginTransaction();
  txn->GetVertexById(removable_id).DeleteLabels({"label1"});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
  EXPECT_FALSE(viter->Valid());
  txn->Commit();

  txn = graphDB->BeginTransaction();
  auto replacement = txn->CreateVertex(
      {"label1"},
      {{"id", Value::Integer(1)}, {"str", Value::String("replacement")}});
  auto replacement_id = replacement.GetId();
  txn->Commit();

  txn = graphDB->BeginTransaction();
  viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetId(), replacement_id);
  EXPECT_EQ(viter->GetVertex().GetProperty("str"),
            Value::String("replacement"));
  txn->Commit();
}

TEST(VertexUniqueIndex, update) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  txn = graphDB->BeginTransaction();
  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    auto& v = viter->GetVertex();
    auto id = v.GetProperty("id").AsInteger();
    v.SetProperties({{"id", Value::Integer(id + 100)},
                     {"str", Value::String(std::to_string(id + 100))}});
  }
  for (auto i = 0; i < 100; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_FALSE(viter->Valid());
  }
  for (auto i = 100; i < 200; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_TRUE(viter->Valid());
    EXPECT_EQ(viter->GetVertex().GetProperty("str"),
              Value::String(std::to_string(i)));
  }

  for (auto viter = txn->NewVertexIterator(); viter->Valid(); viter->Next()) {
    auto& v = viter->GetVertex();
    auto id = v.GetProperty("id").AsInteger();
    v.SetProperties({{"id", Value::String(std::to_string(id))}});
  }
  for (auto i = 100; i < 200; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_FALSE(viter->Valid());
  }
  for (auto i = 100; i < 200; i++) {
    auto viter = txn->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{
                      {"id", Value::String(std::to_string(i))}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_TRUE(viter->Valid());
  }
  txn->Commit();
}

TEST(VertexUniqueIndex, idempotentUpdate) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  auto v = txn->CreateVertex({"label1"}, {{"id", Value::Integer(1)},
                                          {"str", Value::String("before")}});
  auto vid = v.GetId();
  txn->Commit();

  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().SetProperties(
      {{"id", Value::Integer(1)}, {"str", Value::String("after")}});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetId(), vid);
  EXPECT_EQ(viter->GetVertex().GetProperty("str"), Value::String("after"));
  txn->Commit();
}

TEST(VertexUniqueIndex, conflict) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  for (auto i = 0; i < 100; i++) {
    txn = graphDB->BeginTransaction();
    EXPECT_THROW_CODE(
        txn->CreateVertex(v1_labels,
                          {{"id", Value::Integer(i)},
                           {"str", Value::String(std::to_string(i))}}),
        IndexValueAlreadyExist);
    txn->Rollback();
  }
  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(10)}});
  EXPECT_TRUE(viter->Valid());
  EXPECT_THROW_CODE(
      viter->GetVertex().SetProperties({{"id", Value::Integer(20)}}),
      IndexValueAlreadyExist);
  txn->Rollback();
  txn = graphDB->BeginTransaction();
  auto v = txn->CreateVertex({"label1"}, {});
  EXPECT_THROW_CODE(v.SetProperties({{"id", Value::Integer(20)}}),
                    IndexValueAlreadyExist);
  txn->Rollback();
}

TEST(VertexUniqueIndex, reopen) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  txn.reset();
  graphDB.reset();
  graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  txn = graphDB->BeginTransaction();
  for (auto i = 0; i < 100; i++) {
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(i)}});
    EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
    EXPECT_TRUE(viter->Valid());
    EXPECT_EQ(viter->GetVertex().GetProperty("str"),
              Value::String(std::to_string(i)));
  }
  txn->Commit();
}

TEST(VertexUniqueIndex, buildConflict) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->CreateVertex(v1_labels, {{"id", Value::Integer(10)}});
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexFailed(graphDB.get(), "label1_id"));
  auto failed_index = graphDB->meta_info().GetVertexPropertyIndex("label1_id");
  ASSERT_TRUE(failed_index);
  EXPECT_EQ(failed_index->state(), meta::IndexBuildState::FAILED);
  EXPECT_NE(
      failed_index->meta().build_error().find("Index value already exist"),
      std::string::npos);
  txn = graphDB->BeginTransaction();
  EXPECT_THROW_CODE_MSG(
      txn->QueryVertexByPropertyIndex("label1_id", Value::Integer(10)),
      IndexNotReady, "build failed");
  txn->Rollback();
  txn.reset();
  graphDB.reset();
  graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB.reset();
  fs::remove_all(testdb);
  graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  txn = graphDB->BeginTransaction();
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->Commit();
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  EXPECT_THROW_CODE(
      graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"}),
      VertexIndexAlreadyExist);
}

TEST(VertexUniqueIndex, buildNonExists) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});
  auto txn = graphDB->BeginTransaction();
  std::unordered_set<std::string> v1_labels = {"label1", "label2"};
  for (auto i = 0; i < 100; i++) {
    txn->CreateVertex(v1_labels, {{"id", Value::Integer(i)},
                                  {"str", Value::String(std::to_string(i))}});
  }
  txn->Commit();
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id"));
  txn = graphDB->BeginTransaction();
  EXPECT_THROW_CODE(txn->CreateVertex(v1_labels, {{"id", Value::Integer(10)}}),
                    IndexValueAlreadyExist);
  txn->Rollback();
}

TEST(VertexPropertyIndex, loadPreservesBuildStartWalId) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto lid = graphDB->id_generator().GetOrCreateLid("label1");
  auto pid = graphDB->id_generator().GetOrCreatePid("id");
  uint32_t index_id = graphDB->id_generator().GetNextIndexId();

  meta::VertexPropertyIndex meta;
  meta.set_name("label1_id");
  meta.set_is_unique(true);
  meta.set_label("label1");
  meta.set_label_id(boost::endian::big_to_native(lid));
  meta.add_properties("id");
  meta.add_property_ids(boost::endian::big_to_native(pid));
  meta.set_index_id(boost::endian::big_to_native(index_id));
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(8);
  meta.set_applied_wal_id(0);

  auto index = std::make_shared<VertexPropertyIndex>(
      graphDB->raw_db(), &graphDB->graph_cf(), meta, graphDB->graph_cf().index,
      index_id, lid, std::vector<uint32_t>{pid});

  index->Load(nullptr, 7);

  EXPECT_EQ(index->meta().build_start_wal_id(), 8u);
  EXPECT_EQ(index->meta().applied_wal_id(), 7u);
}

TEST(VertexUniqueIndex, onlineBuildConflictsWithConcurrentWrite) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto setup = graphDB->BeginTransaction();
  for (int i = 0; i < 50000; ++i) {
    setup->CreateVertex(
        {"label1"},
        {{"id", Value::Integer(i)}, {"str", Value::String(std::to_string(i))}});
  }
  setup->Commit();

  graphDB->AddVertexPropertyIndex("label1_id", true, "label1", {"id"});

  auto txn = graphDB->BeginTransaction();
  bool duplicate_committed = false;
  try {
    txn->CreateVertex({"label1"}, {{"id", Value::Integer(49999)},
                                   {"str", Value::String("dup_during_build")}});
    txn->Commit();
    duplicate_committed = true;
  } catch (LgraphException& e) {
    EXPECT_EQ(e.code(), ErrorCode::IndexValueAlreadyExist);
    txn->Rollback();
  }

  if (duplicate_committed) {
    ASSERT_TRUE(WaitUntilPropertyIndexFailed(graphDB.get(), "label1_id",
                                             std::chrono::seconds(10)));
    auto failed_index =
        graphDB->meta_info().GetVertexPropertyIndex("label1_id");
    ASSERT_TRUE(failed_index);
    EXPECT_EQ(failed_index->state(), meta::IndexBuildState::FAILED);
  } else {
    ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id",
                                            std::chrono::seconds(10)));
  }
}

TEST(VertexPropertyIndex, nonUniqueCompositeIndexMaintainsEntries) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  auto v1 = txn->CreateVertex(
      {"label1"}, {{"id", Value::Integer(1)}, {"str", Value::String("a")}});
  auto v2 = txn->CreateVertex(
      {"label1"}, {{"id", Value::Integer(1)}, {"str", Value::String("a")}});
  auto v3 = txn->CreateVertex(
      {"label1"}, {{"id", Value::Integer(1)}, {"str", Value::String("b")}});
  auto v1_id = v1.GetId();
  auto v2_id = v2.GetId();
  auto v3_id = v3.GetId();
  txn->Commit();

  graphDB->AddVertexPropertyIndex("label1_id_str", false, "label1",
                                  {"id", "str"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id_str"));
  auto index =
      graphDB->meta_info().GetReadyVertexPropertyIndex("label1_id_str");
  ASSERT_TRUE(index);
  EXPECT_FALSE(index->meta().is_unique());
  EXPECT_EQ(index->meta().properties_size(), 2);

  txn = graphDB->BeginTransaction();
  std::vector<int64_t> ab_ids = {v1_id, v2_id};
  std::sort(ab_ids.begin(), ab_ids.end());
  EXPECT_EQ((CollectVertexPropertyIndexVids(
                txn.get(), index, {Value::Integer(1), Value::String("a")})),
            ab_ids);
  EXPECT_EQ((CollectVertexPropertyIndexVids(
                txn.get(), index, {Value::Integer(1), Value::String("b")})),
            (std::vector<int64_t>{v3_id}));
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->GetVertexById(v2_id).SetProperties({{"str", Value::String("c")}});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  EXPECT_EQ((CollectVertexPropertyIndexVids(
                txn.get(), index, {Value::Integer(1), Value::String("a")})),
            (std::vector<int64_t>{v1_id}));
  EXPECT_EQ((CollectVertexPropertyIndexVids(
                txn.get(), index, {Value::Integer(1), Value::String("c")})),
            (std::vector<int64_t>{v2_id}));
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->GetVertexById(v1_id).RemoveProperty("str");
  txn->Commit();

  txn = graphDB->BeginTransaction();
  EXPECT_TRUE((CollectVertexPropertyIndexVids(
                   txn.get(), index, {Value::Integer(1), Value::String("a")}))
                  .empty());
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->GetVertexById(v3_id).DeleteLabels({"label1"});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  EXPECT_TRUE((CollectVertexPropertyIndexVids(
                   txn.get(), index, {Value::Integer(1), Value::String("b")}))
                  .empty());
  txn->Commit();

  txn.reset();
  graphDB.reset();
  graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  index = graphDB->meta_info().GetVertexPropertyIndex("label1_id_str");
  ASSERT_TRUE(index);
  EXPECT_FALSE(index->meta().is_unique());
  EXPECT_EQ(index->meta().properties_size(), 2);

  txn = graphDB->BeginTransaction();
  EXPECT_EQ((CollectVertexPropertyIndexVids(
                txn.get(), index, {Value::Integer(1), Value::String("c")})),
            (std::vector<int64_t>{v2_id}));
  EXPECT_TRUE((CollectVertexPropertyIndexVids(
                   txn.get(), index, {Value::Integer(1), Value::String("a")}))
                  .empty());
  EXPECT_TRUE((CollectVertexPropertyIndexVids(
                   txn.get(), index, {Value::Integer(1), Value::String("b")}))
                  .empty());
  txn->Commit();
}

TEST(VertexPropertyIndex, nonUniqueQueryAndRange) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  auto alice = txn->CreateVertex(
      {"person"},
      {{"id", Value::Integer(1)}, {"name", Value::String("alice")}});
  auto bob = txn->CreateVertex(
      {"person"}, {{"id", Value::Integer(2)}, {"name", Value::String("bob")}});
  auto cindy = txn->CreateVertex(
      {"person"},
      {{"id", Value::Integer(2)}, {"name", Value::String("cindy")}});
  auto david = txn->CreateVertex(
      {"person"},
      {{"id", Value::Integer(3)}, {"name", Value::String("david")}});
  auto alice_id = alice.GetId();
  auto bob_id = bob.GetId();
  auto cindy_id = cindy.GetId();
  auto david_id = david.GetId();
  txn->Commit();

  graphDB->AddVertexPropertyIndex("person_id", false, "person", {"id"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "person_id"));

  txn = graphDB->BeginTransaction();
  auto viter = txn->QueryVertexByPropertyIndex("person_id", Value::Integer(2));
  EXPECT_TRUE(dynamic_cast<GetVertexByPropertyIndex*>(viter.get()));
  EXPECT_EQ(CollectVertexIds(std::move(viter)),
            (std::vector<int64_t>{bob_id, cindy_id}));

  viter = txn->QueryVertexByPropertyRange("person_id", Value::Integer(2),
                                          Value::Integer(3), true, true);
  EXPECT_TRUE(dynamic_cast<GetVertexByPropertyRange*>(viter.get()));
  EXPECT_EQ(CollectVertexIds(std::move(viter)),
            (std::vector<int64_t>{bob_id, cindy_id, david_id}));

  viter = txn->QueryVertexByPropertyRange("person_id", Value::Integer(2),
                                          Value::Integer(3), false, false);
  EXPECT_TRUE(dynamic_cast<GetVertexByPropertyRange*>(viter.get()));
  EXPECT_TRUE(CollectVertexIds(std::move(viter)).empty());

  viter = txn->QueryVertexByPropertyRange("person_id", Value::Integer(4),
                                          Value::Integer(2), true, true);
  EXPECT_TRUE(dynamic_cast<NoVertexFound*>(viter.get()));
  EXPECT_TRUE(CollectVertexIds(std::move(viter)).empty());
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->GetVertexById(alice_id).Delete();
  txn->GetVertexById(cindy_id).SetProperties({{"id", Value::Integer(4)}});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  viter = txn->QueryVertexByPropertyIndex("person_id", Value::Integer(2));
  EXPECT_EQ(CollectVertexIds(std::move(viter)), (std::vector<int64_t>{bob_id}));

  viter = txn->QueryVertexByPropertyRange("person_id", std::nullopt,
                                          Value::Integer(3), true, false);
  EXPECT_EQ(CollectVertexIds(std::move(viter)), (std::vector<int64_t>{bob_id}));

  viter = txn->QueryVertexByPropertyRange("person_id", Value::Integer(4),
                                          std::nullopt, true, true);
  EXPECT_EQ(CollectVertexIds(std::move(viter)),
            (std::vector<int64_t>{cindy_id}));
  txn->Commit();
}

TEST(VertexUniqueIndex, compositeLookupAndConflict) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  auto txn = graphDB->BeginTransaction();
  auto alpha = txn->CreateVertex({"label1"}, {{"id", Value::Integer(1)},
                                              {"country", Value::String("cn")},
                                              {"str", Value::String("alpha")}});
  auto beta = txn->CreateVertex({"label1"}, {{"id", Value::Integer(1)},
                                             {"country", Value::String("us")},
                                             {"str", Value::String("beta")}});
  auto alpha_id = alpha.GetId();
  auto beta_id = beta.GetId();
  txn->Commit();

  graphDB->AddVertexPropertyIndex("label1_id_country", true, "label1",
                                  {"id", "country"});
  ASSERT_TRUE(WaitUntilPropertyIndexReady(graphDB.get(), "label1_id_country"));

  txn = graphDB->BeginTransaction();
  EXPECT_EQ(txn->GetVertexIteratorInfo(
                "label1", std::unordered_set<std::string>{"id", "country"}),
            "GetVertexByUniqueIndex");
  EXPECT_EQ(txn->GetVertexIteratorInfo("label1",
                                       std::unordered_set<std::string>{"id"}),
            "ScanVertexBylabelProperties");
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)},
                                             {"country", Value::String("cn")}});
  EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetId(), alpha_id);
  EXPECT_EQ(viter->GetVertex().GetProperty("str"), Value::String("alpha"));
  txn->Commit();

  txn = graphDB->BeginTransaction();
  EXPECT_THROW_CODE(
      txn->CreateVertex({"label1"}, {{"id", Value::Integer(1)},
                                     {"country", Value::String("cn")}}),
      IndexValueAlreadyExist);
  txn->Rollback();

  txn = graphDB->BeginTransaction();
  EXPECT_THROW_CODE(txn->GetVertexById(beta_id).SetProperties(
                        {{"country", Value::String("cn")}}),
                    IndexValueAlreadyExist);
  txn->Rollback();

  txn = graphDB->BeginTransaction();
  txn->GetVertexById(beta_id).SetProperties(
      {{"id", Value::Integer(2)}, {"country", Value::String("cn")}});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(2)},
                                             {"country", Value::String("cn")}});
  EXPECT_TRUE(dynamic_cast<GetVertexByUniqueIndex*>(viter.get()));
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetId(), beta_id);
  EXPECT_EQ(viter->GetVertex().GetProperty("str"), Value::String("beta"));
  txn->Commit();
}
