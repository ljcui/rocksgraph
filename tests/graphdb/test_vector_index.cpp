#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <random>
#include <thread>

#include "common/byte_utils.h"
#include "common/flags.h"
#include "common/logger.h"
#include "common/value.h"
#include "graphdb/graph_db.h"
#include "graphdb/vector_property.h"
#include "graphdb/vector_store.h"
#include "test_util.h"
#include "transaction/transaction.h"
using namespace graphdb;
namespace fs = std::filesystem;
static std::string testdb = "testdb";

namespace {

struct ScopedSerializeInterval {
  explicit ScopedSerializeInterval(uint64_t interval)
      : previous_(FLAGS_vt_serialize_interval) {
    FLAGS_vt_serialize_interval = interval;
  }

  ~ScopedSerializeInterval() { FLAGS_vt_serialize_interval = previous_; }

 private:
  uint64_t previous_;
};

bool WaitUntilVectorIndexReady(
    GraphDB* graph_db, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (graph_db->meta_info().GetReadyVertexVectorIndex(index_name)) {
      return true;
    }
    auto index = graph_db->meta_info().GetVertexVectorIndex(index_name);
    if (index && index->state() == meta::IndexBuildState::FAILED) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool WaitUntilVectorQueryCount(
    GraphDB* graph_db, const std::string& index_name,
    const std::vector<float>& query, size_t expected_count,
    std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    auto txn = graph_db->BeginTransaction();
    size_t actual_count = 0;
    bool query_succeeded = false;
    try {
      for (auto result =
               txn->QueryVertexByKnnSearch(index_name, query, 10, 100);
           result->Valid(); result->Next()) {
        actual_count++;
      }
      txn->Commit();
      query_succeeded = true;
    } catch (LgraphException& e) {
      txn->Rollback();
      if (e.code() != ErrorCode::IndexNotReady) {
        throw;
      }
    } catch (...) {
      txn->Rollback();
      throw;
    }
    if (query_succeeded && actual_count == expected_count) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

size_t CountKeysWithPrefix(GraphDB* graph_db, rocksdb::ColumnFamilyHandle* cf,
                           const std::string& prefix) {
  auto txn = graph_db->BeginTransaction();
  rocksdb::ReadOptions ro;
  size_t count = 0;
  std::unique_ptr<rocksdb::Iterator> iter(txn->dbtxn()->GetIterator(ro, cf));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    count++;
  }
  iter.reset();
  txn->Rollback();
  return count;
}

void EnsureVectorField(GraphDB* graph_db, const std::string& label,
                       const std::string& property, int dimension) {
  auto lid = graph_db->id_generator().GetLid(label);
  auto pid = graph_db->id_generator().GetPid(property);
  if (lid && pid && graph_db->meta_info().GetVertexVectorField(*lid, *pid)) {
    return;
  }
  graph_db->AddVertexVectorField(label, property, dimension);
}

void AddVectorIndex(GraphDB* graph_db, const std::string& index_name,
                    const std::string& label, const std::string& property,
                    int dimension, const std::string& distance_type, int hnsw_m,
                    int hnsw_ef_construction) {
  EnsureVectorField(graph_db, label, property, dimension);
  graph_db->AddVertexVectorIndex(index_name, label, property, dimension,
                                 distance_type, hnsw_m, hnsw_ef_construction);
}

}  // namespace

TEST(VectorIndex, build) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  graphDB->AddVertexVectorField("label1", "embedding", 4);
  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(3)},
                     {"embedding", Value::DoubleArray({3.0, 3.0, 3.0, 3.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(4)},
                     {"embedding", Value::DoubleArray({4.0, 4.0, 4.0, 4.0})}});
  txn->Commit();
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));
  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 2.0, 3.0, 4.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 4);
  txn->Commit();
}

TEST(VectorIndex, invalidCreateParametersAreRejectedSynchronously) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());

  EXPECT_THROW_CODE_MSG(AddVectorIndex(graphDB.get(), "invalid_dimension_zero",
                                       "label1", "embedding", 0, "l2", 16, 100),
                        InvalidParameter, "dimension");
  EXPECT_THROW_CODE_MSG(
      AddVectorIndex(graphDB.get(), "invalid_dimension_negative", "label1",
                     "embedding", -1, "l2", 16, 100),
      InvalidParameter, "dimension");
  EXPECT_THROW_CODE_MSG(AddVectorIndex(graphDB.get(), "invalid_hnsw_m",
                                       "label1", "embedding", 4, "l2", 4, 100),
                        InvalidParameter, "hnsw.m");
  EXPECT_THROW_CODE_MSG(AddVectorIndex(graphDB.get(), "invalid_hnsw_ef",
                                       "label1", "embedding", 4, "l2", 16, 15),
                        InvalidParameter, "hnsw.efConstruction");
}

TEST(VectorIndex, createIndexRequiresPredefinedVectorField) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());

  EXPECT_THROW_CODE_MSG(
      graphDB->AddVertexVectorIndex("vector_index", "label1", "embedding", 4,
                                    "l2", 16, 100),
      InvalidParameter, "not defined");

  graphDB->AddVertexVectorField("label1", "embedding", 4);
  EXPECT_THROW_CODE_MSG(
      graphDB->AddVertexVectorIndex("wrong_dim_index", "label1", "embedding", 8,
                                    "l2", 16, 100),
      InvalidParameter, "dimension mismatch");
  graphDB->AddVertexVectorIndex("vector_index", "label1", "embedding", 4, "l2",
                                16, 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), "vector_index"));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"late_field"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();
  graphDB->AddVertexVectorField("late_field", "embedding", 4);
}

TEST(VectorIndex, vectorFieldRejectsNormalIndexes) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  AddVectorIndex(graphDB.get(), "vector_index", "label1", "embedding", 4, "l2",
                 16, 100);

  EXPECT_THROW_CODE_MSG(graphDB->AddVertexPropertyIndex(
                            "property_index", false, "label1", {"embedding"}),
                        InvalidParameter, "vector field");
  EXPECT_THROW_CODE_MSG(
      graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"embedding"}),
      InvalidParameter, "vector field");
}

TEST(VectorIndex, normalIndexRejectsVectorField) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexPropertyIndex("property_index", false, "label1",
                                  {"embedding"});

  EXPECT_THROW_CODE_MSG(AddVectorIndex(graphDB.get(), "vector_index", "label1",
                                       "embedding", 4, "l2", 16, 100),
                        InvalidParameter, "normal index");
}

class VectorIndexParamTest : public ::testing::TestWithParam<int> {};

TEST_P(VectorIndexParamTest, dim) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  int dim = GetParam();
  AddVectorIndex(graphDB.get(), index_name, "person", "embedding", dim, "l2",
                 16, 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));
  auto txn = graphDB->BeginTransaction();
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dis(0.0, 1.0);
  for (auto i = 0; i < 1000; i++) {
    std::vector<Value> embedding;
    embedding.reserve(dim);
    for (auto j = 0; j < dim; j++) {
      embedding.push_back(Value::Float(dis(gen)));
    }
    txn->CreateVertex({"person"}, {{"id", Value::Integer(1)},
                                   {"embedding", Value::Array(embedding)}});
  }
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }
  std::vector<float> query;
  query.reserve(dim);
  for (auto j = 0; j < dim; j++) {
    query.emplace_back(dis(gen));
  }
  txn = graphDB->BeginTransaction();
  for (auto viter = txn->QueryVertexByKnnSearch(index_name, query, 20, 100);
       viter->Valid(); viter->Next()) {
  }
  txn->Commit();
}

INSTANTIATE_TEST_SUITE_P(VectorIndex, VectorIndexParamTest,
                         testing::Values(128, 512, 1024, 2048, 4096));

TEST(VectorIndex, DISABLED_read_benchmark) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  int vector_count = 100000;
  int dim = 1024;
  AddVectorIndex(graphDB.get(), index_name, "person", "embedding", dim, "l2",
                 16, 100);
  auto txn = graphDB->BeginTransaction();
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<float> dis(0.0, 1.0);
  for (auto i = 0; i < vector_count; i++) {
    std::vector<Value> embedding;
    embedding.reserve(dim);
    for (auto j = 0; j < dim; j++) {
      embedding.push_back(Value::Float(dis(gen)));
    }
    txn->CreateVertex({"person"}, {{"id", Value::Integer(1)},
                                   {"embedding", Value::Array(embedding)}});
  }
  txn->Commit();
  txn.reset();
  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  auto start = std::chrono::high_resolution_clock::now();
  std::vector<float> query;
  query.reserve(dim);
  for (auto j = 0; j < dim; j++) {
    query.emplace_back(dis(gen));
  }
  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByKnnSearch(index_name, query, 10, 100);
       viter->Valid(); viter->Next()) {
    count++;
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end - start;
  LOG_INFO("count:{}, elapsed: {}", count, elapsed.count());
  txn->Commit();
}

TEST(VectorIndex, del) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));
  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(3)},
                     {"embedding", Value::DoubleArray({3.0, 3.0, 3.0, 3.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(4)},
                     {"embedding", Value::DoubleArray({4.0, 4.0, 4.0, 4.0})}});
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  std::set<int64_t> ids, expect;
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 2.0, 3.0, 4.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    auto id = viter->GetVertexScore().vertex.GetProperty("id").AsInteger();
    ids.insert(id);
  }
  expect = {1, 2, 3, 4};
  EXPECT_EQ(ids, expect);
  for (auto viter = txn->NewVertexIterator("label1"); viter->Valid();
       viter->Next()) {
    if (viter->GetVertex().GetProperty("id").AsInteger() == 1) {
      viter->GetVertex().Delete();
    }
  }
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  ids.clear();
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 2.0, 3.0, 4.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    auto id = viter->GetVertexScore().vertex.GetProperty("id").AsInteger();
    ids.insert(id);
  }
  expect = {2, 3, 4};
  EXPECT_EQ(ids, expect);
  txn->Commit();
}

TEST(VectorIndex, restart) {
  fs::remove_all(testdb);
  std::string index_name = "vector_index";
  {
    auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
    AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2",
                   16, 100);
    ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));
    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(3)},
                     {"embedding", Value::DoubleArray({3.0, 3.0, 3.0, 3.0})}});
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(4)},
                     {"embedding", Value::DoubleArray({4.0, 4.0, 4.0, 4.0})}});
    txn->Commit();
  }
  {
    auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
    for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
      index->ApplyWAL();
    }
    auto txn = graphDB->BeginTransaction();
    std::set<int64_t> ids, expect;
    for (auto viter = txn->QueryVertexByKnnSearch(
             index_name, {1.0, 2.0, 3.0, 4.0}, 10, 100);
         viter->Valid(); viter->Next()) {
      auto id = viter->GetVertexScore().vertex.GetProperty("id").AsInteger();
      ids.insert(id);
    }
    expect = {1, 2, 3, 4};
    EXPECT_EQ(ids, expect);
  }
}

TEST(VectorIndex, serialize) {
  fs::remove_all(testdb);
  std::string index_name = "vector_index";
  ScopedSerializeInterval scoped_interval(3);
  {
    auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
    AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2",
                   16, 100);
    ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));
    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(3)},
                     {"embedding", Value::DoubleArray({3.0, 3.0, 3.0, 3.0})}});
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(4)},
                     {"embedding", Value::DoubleArray({4.0, 4.0, 4.0, 4.0})}});
    txn->Commit();
    for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
      index->ApplyWAL();
    }
  }
  {
    LOG_INFO("restart graphdb");
    auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
    for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
      index->ApplyWAL();
    }
    auto txn = graphDB->BeginTransaction();
    std::set<int64_t> ids, expect;
    for (auto viter = txn->QueryVertexByKnnSearch(
             index_name, {1.0, 2.0, 3.0, 4.0}, 10, 100);
         viter->Valid(); viter->Next()) {
      auto id = viter->GetVertexScore().vertex.GetProperty("id").AsInteger();
      ids.insert(id);
    }
    expect = {1, 2, 3, 4};
    EXPECT_EQ(ids, expect);
  }
}

TEST(VectorIndex, usesDedicatedVectorStore) {
  fs::remove_all(testdb);
  ScopedSerializeInterval interval(1);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();

  auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);
  index->ApplyWAL();

  EXPECT_TRUE(fs::exists(testdb + "/vt/" + index_name + "/state_db/CURRENT"));

  txn = graphDB->BeginTransaction();
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> iter(
      txn->dbtxn()->GetIterator(ro, graphDB->graph_cf().index));
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  txn->Commit();
  iter.reset();
  txn.reset();
  index.reset();

  graphDB.reset();

  rocksdb::Options options;
  std::unique_ptr<rocksdb::DB> vector_db;
  auto s = rocksdb::DB::OpenForReadOnly(
      options, testdb + "/vt/" + index_name + "/state_db", &vector_db);
  ASSERT_TRUE(s.ok()) << s.ToString();

  int vid_keys = 0;
  int delete_keys = 0;
  std::unique_ptr<rocksdb::Iterator> vector_iter(vector_db->NewIterator({}));
  for (vector_iter->SeekToFirst(); vector_iter->Valid(); vector_iter->Next()) {
    ASSERT_FALSE(vector_iter->key().empty());
    auto prefix = vector_iter->key()[0];
    if (prefix == static_cast<char>(0)) {
      continue;
    }
    if (prefix == static_cast<char>(1)) {
      vid_keys++;
      continue;
    }
    if (prefix == static_cast<char>(2)) {
      delete_keys++;
      continue;
    }
    FAIL() << "unexpected vector store key prefix: "
           << static_cast<int>(prefix);
  }

  EXPECT_EQ(vid_keys, 1);
  EXPECT_EQ(delete_keys, 0);

  vector_iter.reset();
  ASSERT_TRUE(vector_db->Close().ok());
}

TEST(VectorIndex,
     vectorFieldIsExplicitlyReadableButNotReturnedInAllProperties) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetProperty("embedding"),
            Value(std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f}));
  auto props = viter->GetVertex().GetAllProperty();
  EXPECT_EQ(props.size(), 1);
  EXPECT_EQ(props.at("id"), Value::Integer(1));
  EXPECT_EQ(props.count("embedding"), 0);

  auto knn =
      txn->QueryVertexByKnnSearch(index_name, {1.0, 1.0, 1.0, 1.0}, 1, 100);
  ASSERT_TRUE(knn->Valid());
  EXPECT_EQ(knn->GetVertexScore().vertex.GetProperty("id").AsInteger(), 1);
  txn->Commit();
}

TEST(VectorIndex, createIndexClearsStaleArtifactsFromPreviousFailedBuild) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  std::string stale_path = testdb + "/vt/" + index_name;

  {
    VectorStore stale_store(stale_path, 4, meta::VectorDistanceType::L2, 16,
                            100);
    std::array<float, 4> stale_embedding = {99.0f, 99.0f, 99.0f, 99.0f};
    stale_store.Add(777, stale_embedding.data());
    stale_store.Checkpoint(0);
  }

  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);
  EXPECT_EQ(index->NumElements(), 0);

  auto txn = graphDB->BeginTransaction();
  auto result = txn->QueryVertexByKnnSearch(index_name,
                                            {99.0, 99.0, 99.0, 99.0}, 10, 100);
  EXPECT_FALSE(result->Valid());
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();

  index->ApplyWAL();

  txn = graphDB->BeginTransaction();
  result =
      txn->QueryVertexByKnnSearch(index_name, {1.0, 1.0, 1.0, 1.0}, 10, 100);
  ASSERT_TRUE(result->Valid());
  EXPECT_EQ(result->GetVertexScore().vertex.GetProperty("id").AsInteger(), 1);
  txn->Commit();
}

TEST(VectorIndex, vectorStorePersistsOnlyAtCheckpoint) {
  fs::remove_all(testdb);
  ScopedSerializeInterval interval(1000);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.vt_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();

  auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);
  index->ApplyWAL();

  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 1.0, 1.0, 1.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();
  txn.reset();
  index.reset();

  graphDB.reset();

  rocksdb::Options db_options;
  std::unique_ptr<rocksdb::DB> vector_db;
  auto s = rocksdb::DB::OpenForReadOnly(
      db_options, testdb + "/vt/" + index_name + "/state_db", &vector_db);
  ASSERT_TRUE(s.ok()) << s.ToString();

  int state_keys = 0;
  std::unique_ptr<rocksdb::Iterator> vector_iter(vector_db->NewIterator({}));
  for (vector_iter->SeekToFirst(); vector_iter->Valid(); vector_iter->Next()) {
    ASSERT_FALSE(vector_iter->key().empty());
    auto prefix = vector_iter->key()[0];
    if (prefix == static_cast<char>(1) || prefix == static_cast<char>(2)) {
      state_keys++;
    }
  }
  EXPECT_EQ(state_keys, 0);
  vector_iter.reset();
  ASSERT_TRUE(vector_db->Close().ok());

  graphDB = GraphDB::Open(testdb, options);
  index = graphDB->meta_info().GetVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);
  index->ApplyWAL();

  txn = graphDB->BeginTransaction();
  count = 0;
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 1.0, 1.0, 1.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();
}

TEST(VectorIndex, corruptedWalIsRejected) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.vt_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);

  auto txn = graphDB->BeginTransaction();
  auto s = txn->dbtxn()->GetWriteBatch()->Put(graphDB->graph_cf().wal,
                                              index->NextWALKey(), "bad_wal");
  ASSERT_TRUE(s.ok());
  txn->Commit();

  EXPECT_THROW_CODE(index->ApplyWAL(), VectorIndexException);
}

TEST(VectorIndex, periodicTimerSurvivesWalApplyFailure) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.vt_apply_interval_ = 1;
  auto graphDB = GraphDB::Open(testdb, options);
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);

  std::string bad_wal_key = index->NextWALKey();
  auto txn = graphDB->BeginTransaction();
  auto s = txn->dbtxn()->GetWriteBatch()->Put(graphDB->graph_cf().wal,
                                              bad_wal_key, "bad_wal");
  ASSERT_TRUE(s.ok());
  txn->Commit();

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  txn = graphDB->BeginTransaction();
  s = txn->dbtxn()->GetWriteBatch()->Delete(graphDB->graph_cf().wal,
                                            bad_wal_key);
  ASSERT_TRUE(s.ok());
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();

  EXPECT_TRUE(WaitUntilVectorQueryCount(graphDB.get(), index_name,
                                        {1.0, 1.0, 1.0, 1.0}, 1,
                                        std::chrono::milliseconds(2500)));
}

TEST(VectorIndex, deleteOnlyWalIsCheckpointedAndTrimmed) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.vt_apply_interval_ = 3600;
  ScopedSerializeInterval interval(1);
  std::string index_name = "vector_index";
  {
    auto graphDB = GraphDB::Open(testdb, options);
    AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2",
                   16, 100);
    ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

    auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
    ASSERT_TRUE(index != nullptr);
    std::string prefix(common::AsChars(index->index_id()),
                       sizeof(index->index_id()));

    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
    txn->Commit();

    index->ApplyWAL();
    EXPECT_EQ(
        CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().wal, prefix), 0);

    txn = graphDB->BeginTransaction();
    auto viter = txn->NewVertexIterator(
        "label1",
        std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
    ASSERT_TRUE(viter->Valid());
    viter->GetVertex().Delete();
    txn->Commit();

    index->ApplyWAL();
    EXPECT_EQ(
        CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().wal, prefix), 0);
  }

  {
    auto graphDB = GraphDB::Open(testdb, options);
    auto txn = graphDB->BeginTransaction();
    auto result =
        txn->QueryVertexByKnnSearch(index_name, {1.0, 1.0, 1.0, 1.0}, 10, 100);
    EXPECT_FALSE(result->Valid());
    txn->Commit();
  }
}

TEST(VectorIndex, restartAfterCheckpointContinuesWalSequence) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.vt_apply_interval_ = 3600;
  ScopedSerializeInterval interval(1);
  std::string index_name = "vector_index";

  {
    auto graphDB = GraphDB::Open(testdb, options);
    AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2",
                   16, 100);
    ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

    auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
    ASSERT_TRUE(index != nullptr);
    std::string prefix(common::AsChars(index->index_id()),
                       sizeof(index->index_id()));

    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
    txn->Commit();

    index->ApplyWAL();
    EXPECT_EQ(
        CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().wal, prefix), 0);
  }

  {
    auto graphDB = GraphDB::Open(testdb, options);
    auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
    ASSERT_TRUE(index != nullptr);

    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
    txn->Commit();

    index->ApplyWAL();

    txn = graphDB->BeginTransaction();
    auto nearest =
        txn->QueryVertexByKnnSearch(index_name, {2.0, 2.0, 2.0, 2.0}, 1, 100);
    ASSERT_TRUE(nearest->Valid());
    EXPECT_EQ(nearest->GetVertexScore().vertex.GetProperty("id").AsInteger(),
              2);
    txn->Commit();
  }
}

TEST(VectorIndex, duplicateAddWalReplacesPreviousVector) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.vt_apply_interval_ = 3600;
  ScopedSerializeInterval interval(1000);
  auto graphDB = GraphDB::Open(testdb, options);
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->CreateVertex(
      {"label1"},
      {{"id", Value::Integer(2)},
       {"embedding", Value::DoubleArray({10.0, 10.0, 10.0, 10.0})}});
  txn->Commit();

  auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);
  index->ApplyWAL();

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  ASSERT_TRUE(viter->Valid());
  auto vid = viter->GetVertex().GetId();

  meta::VectorIndexUpdate update;
  update.set_type(meta::UpdateType::Add);
  update.set_vid(vid);
  update.add_vector(100.0f);
  update.add_vector(100.0f);
  update.add_vector(100.0f);
  update.add_vector(100.0f);

  std::string payload;
  ASSERT_TRUE(update.SerializeToString(&payload));
  auto s = txn->dbtxn()->GetWriteBatch()->Put(graphDB->graph_cf().wal,
                                              index->NextWALKey(), payload);
  ASSERT_TRUE(s.ok());
  txn->Commit();

  EXPECT_NO_THROW(index->ApplyWAL());

  txn = graphDB->BeginTransaction();
  auto near_old =
      txn->QueryVertexByKnnSearch(index_name, {1.0, 1.0, 1.0, 1.0}, 1, 100);
  ASSERT_TRUE(near_old->Valid());
  EXPECT_EQ(near_old->GetVertexScore().vertex.GetProperty("id").AsInteger(), 2);

  auto near_new = txn->QueryVertexByKnnSearch(
      index_name, {100.0, 100.0, 100.0, 100.0}, 1, 100);
  ASSERT_TRUE(near_new->Valid());
  EXPECT_EQ(near_new->GetVertexScore().vertex.GetProperty("id").AsInteger(), 1);
  txn->Commit();
}

TEST(VectorIndex, checkpointMetaWriteFailureIsReported) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.vt_apply_interval_ = 3600;
  ScopedSerializeInterval scoped_interval(1);
  std::string index_name = "vector_index";
  std::string checkpoint_path = testdb + "/vt/" + index_name + "/checkpoint.1";
  {
    auto graphDB = GraphDB::Open(testdb, options);
    AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2",
                   16, 100);
    ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
    txn->Commit();

    ASSERT_TRUE(fs::create_directory(checkpoint_path));
    auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
    ASSERT_TRUE(index != nullptr);
    EXPECT_THROW_CODE(index->ApplyWAL(), IOException);
  }

  fs::remove_all(checkpoint_path);

  {
    auto graphDB = GraphDB::Open(testdb, options);
    auto index = graphDB->meta_info().GetVertexVectorIndex(index_name);
    ASSERT_TRUE(index != nullptr);
    index->ApplyWAL();

    auto txn = graphDB->BeginTransaction();
    std::set<int64_t> ids;
    for (auto viter = txn->QueryVertexByKnnSearch(
             index_name, {1.0, 1.0, 1.0, 1.0}, 10, 100);
         viter->Valid(); viter->Next()) {
      ids.insert(viter->GetVertexScore().vertex.GetProperty("id").AsInteger());
    }
    EXPECT_EQ(ids, (std::set<int64_t>{1}));
    txn->Commit();
  }
}

TEST(VectorIndex, rollbackDoesNotBreakWalApply) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
  txn->Rollback();

  txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(3)},
                     {"embedding", Value::DoubleArray({3.0, 3.0, 3.0, 3.0})}});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  std::set<int64_t> ids;
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 1.0, 1.0, 1.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    ids.insert(viter->GetVertexScore().vertex.GetProperty("id").AsInteger());
  }
  EXPECT_EQ(ids, (std::set<int64_t>{1, 3}));
  txn->Commit();
}

TEST(VectorIndex, outOfOrderCommitsApplyCleanly) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn1 = graphDB->BeginTransaction();
  txn1->CreateVertex({"label1"},
                     {{"id", Value::Integer(1)},
                      {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});

  auto txn2 = graphDB->BeginTransaction();
  txn2->CreateVertex({"label1"},
                     {{"id", Value::Integer(2)},
                      {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});

  txn2->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn1->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  auto read_txn = graphDB->BeginTransaction();
  std::set<int64_t> ids;
  for (auto viter = read_txn->QueryVertexByKnnSearch(
           index_name, {1.0, 1.0, 1.0, 1.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    ids.insert(viter->GetVertexScore().vertex.GetProperty("id").AsInteger());
  }
  EXPECT_EQ(ids, (std::set<int64_t>{1, 2}));
  read_txn->Commit();
}

TEST(VectorIndex, deleteLabelsUpdatesMembershipCorrectly) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1", "label2"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  std::set<int64_t> ids;
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 1.0, 1.0, 1.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    ids.insert(viter->GetVertexScore().vertex.GetProperty("id").AsInteger());
  }
  EXPECT_EQ(ids, (std::set<int64_t>{1, 2}));
  txn->Commit();

  txn = graphDB->BeginTransaction();
  auto keep_vertex = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  EXPECT_TRUE(keep_vertex->Valid());
  keep_vertex->GetVertex().DeleteLabels({"label2"});

  auto remove_vertex = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(2)}});
  EXPECT_TRUE(remove_vertex->Valid());
  remove_vertex->GetVertex().DeleteLabels({"label1"});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  ids.clear();
  for (auto viter = txn->QueryVertexByKnnSearch(index_name,
                                                {1.0, 1.0, 1.0, 1.0}, 10, 100);
       viter->Valid(); viter->Next()) {
    ids.insert(viter->GetVertexScore().vertex.GetProperty("id").AsInteger());
  }
  EXPECT_EQ(ids, (std::set<int64_t>{1}));

  auto remaining = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  EXPECT_TRUE(remaining->Valid());
  EXPECT_EQ(remaining->GetVertex().GetLabels(),
            (std::unordered_set<std::string>{"label1"}));

  auto removed = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(2)}});
  EXPECT_FALSE(removed->Valid());
  txn->Commit();
}

TEST(VectorIndex, addLabelWritesExistingPropertyToVectorStore) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label2"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->Commit();

  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label2",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().AddLabels({"label1"});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  auto knn =
      txn->QueryVertexByKnnSearch(index_name, {1.0, 1.0, 1.0, 1.0}, 1, 100);
  ASSERT_TRUE(knn->Valid());
  EXPECT_EQ(knn->GetVertexScore().vertex.GetProperty("id").AsInteger(), 1);

  viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetProperty("embedding"),
            Value(std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f}));
  auto props = viter->GetVertex().GetAllProperty();
  EXPECT_EQ(props.size(), 1);
  EXPECT_EQ(props.at("id"), Value::Integer(1));
  EXPECT_EQ(props.count("embedding"), 0);
  txn->Commit();
}

TEST(VectorIndex, buildIgnoresLegacyPropertyWithoutDedicatedVectorStore) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";

  graphDB->AddVertexVectorField("label1", "embedding", 4);
  auto txn = graphDB->BeginTransaction();
  auto legacy_vid =
      txn->CreateVertex(
             {"label1"},
             {{"id", Value::Integer(1)},
              {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}})
          .GetId();
  txn->Commit();

  auto lid = graphDB->id_generator().GetLid("label1");
  auto pid = graphDB->id_generator().GetPid("embedding");
  ASSERT_TRUE(lid.has_value());
  ASSERT_TRUE(pid.has_value());
  std::string legacy_property_key(common::AsChars(legacy_vid),
                                  sizeof(legacy_vid));
  legacy_property_key.append(common::AsChars(pid.value()), sizeof(pid.value()));

  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  rocksdb::WriteBatch wb;
  auto s =
      wb.Delete(graphDB->graph_cf().vertex_vector_property,
                VertexVectorPropertyKey(lid.value(), pid.value(), legacy_vid));
  ASSERT_TRUE(s.ok()) << s.ToString();
  s = wb.Put(graphDB->graph_cf().vertex_property, legacy_property_key,
             Value::DoubleArray({1.0, 1.0, 1.0, 1.0}).Serialize());
  ASSERT_TRUE(s.ok()) << s.ToString();
  s = graphDB->raw_db()->Write({}, &wb);
  ASSERT_TRUE(s.ok()) << s.ToString();

  graphDB->DeleteVertexVectorIndex(index_name);
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  txn = graphDB->BeginTransaction();
  auto knn =
      txn->QueryVertexByKnnSearch(index_name, {1.0, 1.0, 1.0, 1.0}, 1, 100);
  EXPECT_FALSE(knn->Valid());
  txn->Commit();
}

TEST(VectorIndex, updateAndRemoveEmbeddingMaintainMembership) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";
  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 4, "l2", 16,
                 100);
  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(1)},
                     {"embedding", Value::DoubleArray({1.0, 1.0, 1.0, 1.0})}});
  txn->CreateVertex({"label1"},
                    {{"id", Value::Integer(2)},
                     {"embedding", Value::DoubleArray({2.0, 2.0, 2.0, 2.0})}});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().SetProperties(
      {{"embedding", Value::DoubleArray({10.0, 10.0, 10.0, 10.0})}});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  auto near_new =
      txn->QueryVertexByKnnSearch(index_name, {10.0, 10.0, 10.0, 10.0}, 1, 100);
  ASSERT_TRUE(near_new->Valid());
  EXPECT_EQ(near_new->GetVertexScore().vertex.GetProperty("id").AsInteger(), 1);

  auto near_old =
      txn->QueryVertexByKnnSearch(index_name, {1.0, 1.0, 1.0, 1.0}, 1, 100);
  ASSERT_TRUE(near_old->Valid());
  EXPECT_EQ(near_old->GetVertexScore().vertex.GetProperty("id").AsInteger(), 2);
  txn->Commit();

  txn = graphDB->BeginTransaction();
  viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().RemoveProperty("embedding");
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexVectorIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  std::set<int64_t> ids;
  for (auto result = txn->QueryVertexByKnnSearch(
           index_name, {10.0, 10.0, 10.0, 10.0}, 10, 100);
       result->Valid(); result->Next()) {
    ids.insert(result->GetVertexScore().vertex.GetProperty("id").AsInteger());
  }
  EXPECT_EQ(ids, (std::set<int64_t>{2}));
  txn->Commit();
}

TEST(VectorIndex, buildDoesNotBlockWrites) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  std::string index_name = "vector_index";

  graphDB->AddVertexVectorField("label1", "embedding", 8);
  auto txn = graphDB->BeginTransaction();
  for (int i = 0; i < 10000; ++i) {
    txn->CreateVertex(
        {"label1"}, {{"id", Value::Integer(i)},
                     {"embedding", Value::DoubleArray({1.0, 2.0, 3.0, 4.0, 5.0,
                                                       6.0, 7.0, 8.0})}});
  }
  txn->Commit();

  AddVectorIndex(graphDB.get(), index_name, "label1", "embedding", 8, "l2", 16,
                 100);

  auto write_txn = graphDB->BeginTransaction();
  write_txn->CreateVertex(
      {"label1"},
      {{"id", Value::Integer(20000)},
       {"embedding",
        Value::DoubleArray({42.0, 42.0, 42.0, 42.0, 42.0, 42.0, 42.0, 42.0})}});
  write_txn->Commit();

  ASSERT_TRUE(WaitUntilVectorIndexReady(graphDB.get(), index_name,
                                        std::chrono::seconds(15)));

  auto index = graphDB->meta_info().GetReadyVertexVectorIndex(index_name);
  ASSERT_TRUE(index != nullptr);
  index->ApplyWAL();
  EXPECT_EQ(index->NumElements(), 10001);

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1",
      std::unordered_map<std::string, Value>{{"id", Value::Integer(20000)}});
  ASSERT_TRUE(viter->Valid());
  EXPECT_EQ(viter->GetVertex().GetProperty("embedding"),
            Value(std::vector<float>{42.0f, 42.0f, 42.0f, 42.0f, 42.0f, 42.0f,
                                     42.0f, 42.0f}));
  txn->Commit();
}
