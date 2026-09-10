#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/byte_utils.h"
#include "common/logger.h"
#include "graphdb/ftindex/include/lib.rs.h"
#include "graphdb/graph_db.h"
#include "proto/meta.pb.h"
#include "test_util.h"
#include "transaction/transaction.h"
#include "value/value.h"

using rg::Value;
using namespace graphdb;
using common::AsChars;
namespace fs = std::filesystem;
static std::string testdb = "testdb";
static std::string test_ftindex = "test_ftindex";

constexpr size_t kDefaultFTWriterThreads = 1;
constexpr uint64_t kDefaultFTWriterMemoryBudget = 50 * 1000 * 1000;

namespace {

bool WaitUntilFullTextIndexReady(
    GraphDB* graph_db, const std::string& index_name,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (graph_db->meta_info().GetReadyVertexFullTextIndex(index_name)) {
      return true;
    }
    auto index = graph_db->meta_info().GetVertexFullTextIndex(index_name);
    if (index && index->state() == meta::IndexBuildState::FAILED) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool WaitUntilQueryCount(
    GraphDB* graph_db, const std::string& index_name, const std::string& query,
    size_t expected_count,
    std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    auto txn = graph_db->BeginTransaction();
    size_t actual_count = 0;
    bool query_succeeded = false;
    try {
      for (auto result = txn->QueryVertexByFTIndex(index_name, query, 10);
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

std::vector<meta::FullTextIndexUpdate> ReadFullTextWalUpdates(
    GraphDB* graph_db, const std::shared_ptr<VertexFullTextIndex>& index) {
  auto txn = graph_db->BeginTransaction();
  rocksdb::ReadOptions ro;
  std::string prefix(AsChars(index->index_id()), sizeof(index->index_id()));
  std::vector<meta::FullTextIndexUpdate> updates;
  std::unique_ptr<rocksdb::Iterator> iter(
      txn->dbtxn()->GetIterator(ro, graph_db->graph_cf().wal));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    meta::FullTextIndexUpdate update;
    auto val = iter->value();
    EXPECT_TRUE(update.ParseFromArray(val.data(), val.size()));
    updates.emplace_back(std::move(update));
  }
  iter.reset();
  txn->Rollback();
  return updates;
}

}  // namespace

TEST(FTIndex, basic_v1) {
  fs::remove_all(test_ftindex);
  ::rust::Vec<::rust::String> properties;
  properties.push_back("title");
  properties.push_back("body");
  auto ft = new_ftindex(test_ftindex, properties, kDefaultFTWriterThreads,
                        kDefaultFTWriterMemoryBudget);
  ::rust::Vec<::rust::String> fields = {"title", "body"};
  {
    ::rust::Vec<::rust::String> values = {"title1 common_title title2",
                                          "body1 common_body body2"};
    ft_add_document(*ft, 1, fields, values);
  }
  {
    ::rust::Vec<::rust::String> values = {"title3 common_title title4",
                                          "body3 common_body, body4"};
    ft_add_document(*ft, 2, fields, values);
  }
  ft_commit(*ft, "payload");
  auto ret = ft_query(*ft, "common_title", {10});
  EXPECT_EQ(ret.size(), 2);
  ret = ft_query(*ft, "common_body", {10});
  EXPECT_EQ(ret.size(), 2);
  ret = ft_query(*ft, "common_title AND title1", {10});
  EXPECT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].id, 1);
  ret = ft_query(*ft, "title3", {10});
  EXPECT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].id, 2);
}

TEST(FTIndex, chinese_segmentation) {
  fs::remove_all(test_ftindex);
  ::rust::Vec<::rust::String> properties;
  properties.push_back("title");
  properties.push_back("body");
  auto ft = new_ftindex(test_ftindex, properties, kDefaultFTWriterThreads,
                        kDefaultFTWriterMemoryBudget);
  ::rust::Vec<::rust::String> fields = {"title", "body"};
  {
    ::rust::Vec<::rust::String> values = {"图数据库支持知识检索",
                                          "恶性肿瘤属于重大疾病"};
    ft_add_document(*ft, 1, fields, values);
  }
  {
    ::rust::Vec<::rust::String> values = {"时序数据库支持监控分析",
                                          "糖尿病需要长期管理"};
    ft_add_document(*ft, 2, fields, values);
  }
  ft_commit(*ft, "payload");

  auto ret = ft_query(*ft, "图数据库", {10});
  ASSERT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].id, 1);

  ret = ft_query(*ft, "恶性肿瘤", {10});
  ASSERT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].id, 1);

  std::string zero_width = "\xE2\x80\x8B";
  ret = ft_query(*ft, zero_width + "恶性肿瘤", {10});
  ASSERT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].id, 1);
}

TEST(FTIndex, jieba_tokenize_output) {
  const std::string text = "图数据库支持知识检索，恶性肿瘤属于重大疾病";
  const auto rust_tokens = ft_tokenize(text);

  std::vector<std::string> tokens;
  tokens.reserve(rust_tokens.size());
  for (const auto& token : rust_tokens) {
    tokens.emplace_back(token.data(), token.size());
  }

  const std::vector<std::string> expected = {
      "图",   "数据", "据库",     "数据库", "支持", "知识", "检索",
      "恶性", "肿瘤", "恶性肿瘤", "属于",   "重大", "疾病"};
  EXPECT_EQ(tokens, expected);

  LOG_INFO("jieba tokens: [{}]", fmt::join(tokens, "] ["));
}

TEST(FTIndex, chinese_stop_words) {
  const std::string text = "图数据库的检索是在知识图谱中进行的";
  const auto rust_tokens = ft_tokenize(text);

  std::vector<std::string> tokens;
  tokens.reserve(rust_tokens.size());
  for (const auto& token : rust_tokens) {
    tokens.emplace_back(token.data(), token.size());
  }

  EXPECT_NE(std::find(tokens.begin(), tokens.end(), "数据库"), tokens.end());
  EXPECT_NE(std::find(tokens.begin(), tokens.end(), "检索"), tokens.end());
  EXPECT_NE(std::find(tokens.begin(), tokens.end(), "图谱"), tokens.end());

  EXPECT_EQ(std::find(tokens.begin(), tokens.end(), "的"), tokens.end());
  EXPECT_EQ(std::find(tokens.begin(), tokens.end(), "是"), tokens.end());
  EXPECT_EQ(std::find(tokens.begin(), tokens.end(), "在"), tokens.end());
}

TEST(FTIndex, english_stop_words) {
  const std::string text = "The graph database is in the cloud";
  const auto rust_tokens = ft_tokenize(text);

  std::vector<std::string> tokens;
  tokens.reserve(rust_tokens.size());
  for (const auto& token : rust_tokens) {
    tokens.emplace_back(token.data(), token.size());
  }

  EXPECT_NE(std::find(tokens.begin(), tokens.end(), "graph"), tokens.end());
  EXPECT_NE(std::find(tokens.begin(), tokens.end(), "database"), tokens.end());
  EXPECT_NE(std::find(tokens.begin(), tokens.end(), "cloud"), tokens.end());

  EXPECT_EQ(std::find(tokens.begin(), tokens.end(), "the"), tokens.end());
  EXPECT_EQ(std::find(tokens.begin(), tokens.end(), "is"), tokens.end());
  EXPECT_EQ(std::find(tokens.begin(), tokens.end(), "in"), tokens.end());
}

TEST(FTIndex, update) {
  fs::remove_all(test_ftindex);
  ::rust::Vec<::rust::String> properties;
  properties.push_back("title");
  properties.push_back("body");
  auto ft = new_ftindex(test_ftindex, properties, kDefaultFTWriterThreads,
                        kDefaultFTWriterMemoryBudget);
  ::rust::Vec<::rust::String> fields = {"title", "body"};
  {
    ::rust::Vec<::rust::String> values = {"title1 common_title title2",
                                          "body1 common_body body2"};
    ft_add_document(*ft, 1, fields, values);
  }
  {
    ::rust::Vec<::rust::String> values = {"title3 common_title title4",
                                          "body3 common_body, body4"};
    ft_add_document(*ft, 2, fields, values);
  }
  ft_commit(*ft, "payload");
  ft_delete_document(*ft, 1);
  ft_commit(*ft, "payload");
  auto ret = ft_query(*ft, "common_title", {10});
  EXPECT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].id, 2);
  {
    ::rust::Vec<::rust::String> values = {"title11 common_title title22",
                                          "body11 common_body body22"};
    ft_add_document(*ft, 1, fields, values);
  }
  ft_commit(*ft, "payload");
  ret = ft_query(*ft, "title11", {10});
  EXPECT_EQ(ret.size(), 1);
  EXPECT_EQ(ret[0].id, 1);
}

static std::unordered_map<std::string, Value> properties = {
    {"property1", Value(true)},
    {"property2", Value(100)},
    {"property3", Value("string")},
    {"property4", Value(1.1314)},
    {"property5", Value(Value::List{Value(true), Value(false)})},
    {"property6", Value(Value::List{Value(1), Value(2), Value(3)})},
    {"property7", Value(Value::List{Value("string1"), Value("string2")})},
    {"property8", Value(Value::List{Value(11.11), Value(22.22)})}};

TEST(FTIndex, indexVertex) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));
  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex(
      {"label1", "label2"},
      {{"id", Value(1)}, {"str", Value("string1 string string11")}});
  txn->CreateVertex(
      {"label1", "label2"},
      {{"id", Value(2)}, {"str", Value("string2 string string22")}});
  txn->CreateVertex({"label3"}, {{"id", Value(3)},
                                 {"str", Value("string3 string string33")}});
  txn->CreateVertex({"label1"}, {{"id", Value(4)}, {"str", Value(4)}});
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string1", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(1));
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string2", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(2));
    count++;
  }
  EXPECT_EQ(count, 1);
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string", 10);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 2);
  txn->Commit();
}

TEST(FTIndex, committedWalIsAppliedByPeriodicTimer) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 1;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value(1)}, {"str", Value("near_real_time_token")}});
  txn->Commit();

  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index",
                                  "near_real_time_token", 1,
                                  std::chrono::milliseconds(2500)));

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().Delete();
  txn->Commit();

  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index",
                                  "near_real_time_token", 0,
                                  std::chrono::milliseconds(2500)));
}

TEST(FTIndex, corruptedWalIsRejected) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);

  auto txn = graphDB->BeginTransaction();
  auto s = txn->dbtxn()->GetWriteBatch()->Put(graphDB->graph_cf().wal,
                                              index->NextWALKey(), "bad_wal");
  ASSERT_TRUE(s.ok());
  txn->Commit();

  EXPECT_THROW_CODE(index->ApplyWAL(), StorageEngineError);
}

TEST(FTIndex, periodicTimerSurvivesWalApplyFailure) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 1;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
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
  txn->CreateVertex(
      {"label1"}, {{"id", Value(1)}, {"str", Value("timer_survives_bad_wal")}});
  txn->Commit();

  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index",
                                  "timer_survives_bad_wal", 1,
                                  std::chrono::milliseconds(2500)));
}

TEST(FTIndex, deleteIndexRejectsPendingTransactionCommit) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);
  std::string prefix(AsChars(index->index_id()), sizeof(index->index_id()));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value(1)}, {"str", Value("should_not_commit")}});

  graphDB->DeleteVertexFullTextIndex("ft_index");

  EXPECT_THROW_CODE_MSG(txn->Commit(), FullTextIndexNotFound,
                        "was deleted during transaction");
  txn->Rollback();

  auto read_txn = graphDB->BeginTransaction();
  auto viter = read_txn->NewVertexIterator(
      "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
  EXPECT_FALSE(viter->Valid());
  read_txn->Rollback();

  EXPECT_EQ(
      CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().index, prefix), 0);
  EXPECT_EQ(CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().wal, prefix),
            0);
}

TEST(FTIndex, rollbackDoesNotBreakWalApply) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex(
      {"label1"}, {{"id", Value(1)}, {"str", Value("shared_token keep_one")}});
  txn->Commit();

  txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"}, {{"id", Value(2)},
                                 {"str", Value("shared_token rolled_back")}});
  txn->Rollback();

  txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"}, {{"id", Value(3)},
                                 {"str", Value("shared_token keep_three")}});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  std::set<int64_t> ids;
  for (auto result = txn->QueryVertexByFTIndex("ft_index", "shared_token", 10);
       result->Valid(); result->Next()) {
    ids.insert(result->GetVertexScore().vertex.GetProperty("id").AsInteger());
  }
  EXPECT_EQ(ids, (std::set<int64_t>{1, 3}));
  txn->Commit();
}

TEST(FTIndex, createVertexWritesSingleAddWal) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value(1)}, {"str", Value("written_once")}});
  txn->Commit();

  auto updates = ReadFullTextWalUpdates(graphDB.get(), index);
  ASSERT_EQ(updates.size(), 1);
  EXPECT_EQ(updates[0].type(), meta::UpdateType::Add);
  ASSERT_EQ(updates[0].fields_size(), 1);
  EXPECT_EQ(updates[0].fields(0), "str");
  EXPECT_EQ(updates[0].values(0), "written_once");
}

TEST(FTIndex, createVertexAndRewriteInOneTransactionPreservesWalOrder) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);

  auto txn = graphDB->BeginTransaction();
  auto vertex = txn->CreateVertex(
      {"label1"}, {{"id", Value(1)}, {"str", Value("before_rewrite")}});
  vertex.SetProperties({{"str", Value("after_rewrite")}});
  txn->Commit();

  auto updates = ReadFullTextWalUpdates(graphDB.get(), index);
  ASSERT_EQ(updates.size(), 3);
  EXPECT_EQ(updates[0].type(), meta::UpdateType::Add);
  EXPECT_EQ(updates[1].type(), meta::UpdateType::Delete);
  EXPECT_EQ(updates[2].type(), meta::UpdateType::Add);
  ASSERT_EQ(updates[2].fields_size(), 1);
  EXPECT_EQ(updates[2].fields(0), "str");
  EXPECT_EQ(updates[2].values(0), "after_rewrite");
}

TEST(FTIndex, updatingIndexedVertexWritesDeleteThenAddWal) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value(1)}, {"str", Value("before_update")}});
  txn->Commit();
  index->ApplyWAL();

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().SetProperties({{"str", Value("after_update")}});
  txn->Commit();

  auto updates = ReadFullTextWalUpdates(graphDB.get(), index);
  ASSERT_EQ(updates.size(), 2);
  EXPECT_EQ(updates[0].type(), meta::UpdateType::Delete);
  EXPECT_EQ(updates[1].type(), meta::UpdateType::Add);
  ASSERT_EQ(updates[1].fields_size(), 1);
  EXPECT_EQ(updates[1].fields(0), "str");
  EXPECT_EQ(updates[1].values(0), "after_update");
}

TEST(FTIndex, outOfOrderCommitsApplyCleanly) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto txn1 = graphDB->BeginTransaction();
  txn1->CreateVertex({"label1"}, {{"id", Value(1)},
                                  {"str", Value("shared_token commit_later")}});

  auto txn2 = graphDB->BeginTransaction();
  txn2->CreateVertex({"label1"}, {{"id", Value(2)},
                                  {"str", Value("shared_token commit_first")}});

  txn2->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }

  txn1->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }

  auto read_txn = graphDB->BeginTransaction();
  std::set<int64_t> ids;
  for (auto result =
           read_txn->QueryVertexByFTIndex("ft_index", "shared_token", 10);
       result->Valid(); result->Next()) {
    ids.insert(result->GetVertexScore().vertex.GetProperty("id").AsInteger());
  }
  EXPECT_EQ(ids, (std::set<int64_t>{1, 2}));
  read_txn->Commit();
}

TEST(FTIndex, buildDeduplicatesVerticesWithMultipleMatchedLabels) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1", "label2"},
                    {{"id", Value(1)}, {"str", Value("retain_me only_once")}});
  txn->CreateVertex({"label1"},
                    {{"id", Value(2)}, {"str", Value("retain_me second_doc")}});
  txn->Commit();

  graphDB->AddVertexFullTextIndex("ft_index", {"label1", "label2"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  txn = graphDB->BeginTransaction();
  int count = 0;
  std::set<int64_t> ids;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "retain_me", 10);
       viter->Valid(); viter->Next()) {
    auto id = viter->GetVertexScore().vertex.GetProperty("id").AsInteger();
    ids.insert(id);
    count++;
  }
  EXPECT_EQ(count, 2);
  EXPECT_EQ(ids, (std::set<int64_t>{1, 2}));
  txn->Commit();
}

TEST(FTIndex, deleteVertex) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));
  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex(
      {"label1", "label2"},
      {{"id", Value(1)}, {"str", Value("string1 string string11")}});
  txn->CreateVertex(
      {"label1", "label2"},
      {{"id", Value(2)}, {"str", Value("string2 string string22")}});
  txn->CreateVertex({"label3"}, {{"id", Value(3)},
                                 {"str", Value("string3 string string33")}});
  txn->CreateVertex({"label1"}, {{"id", Value(4)}, {"str", Value(4)}});
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string1", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(1));
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Rollback();
  {
    txn = graphDB->BeginTransaction();
    auto viter = txn->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
    EXPECT_TRUE(viter->Valid());
    viter->GetVertex().Delete();
    txn->Commit();
  }
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string1", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(1));
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(2));
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();
  {
    txn = graphDB->BeginTransaction();
    auto viter = txn->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(2)}});
    EXPECT_TRUE(viter->Valid());
    viter->GetVertex().RemoveProperty("str");
    txn->Commit();
  }
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string2", 10);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string", 10);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  txn->Commit();
}

TEST(FTIndex, buildDoesNotBlockWrites) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());

  auto txn = graphDB->BeginTransaction();
  for (int i = 0; i < 20000; ++i) {
    txn->CreateVertex(
        {"label1"},
        {{"id", Value(i)}, {"str", Value("token" + std::to_string(i))}});
  }
  txn->Commit();

  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});

  auto write_txn = graphDB->BeginTransaction();
  write_txn->CreateVertex({"label1"}, {{"id", Value(30000)},
                                       {"str", Value("written_during_build")}});
  write_txn->Commit();

  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index",
                                          std::chrono::seconds(15)));

  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "token42", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(42));
    count++;
  }
  EXPECT_EQ(count, 1);

  count = 0;
  for (auto viter =
           txn->QueryVertexByFTIndex("ft_index", "written_during_build", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(30000));
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();
}

TEST(FTIndex, updateVertex) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));
  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex(
      {"label1", "label2"},
      {{"id", Value(1)}, {"str", Value("string1 string string11")}});
  txn->CreateVertex(
      {"label1", "label2"},
      {{"id", Value(2)}, {"str", Value("string2 string string22")}});
  txn->CreateVertex({"label3"}, {{"id", Value(3)},
                                 {"str", Value("string3 string string33")}});
  txn->CreateVertex({"label1"}, {{"id", Value(4)}, {"str", Value(4)}});
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string1", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(1));
    viter->GetVertexScore().vertex.SetProperties({{"str", Value(10)}});
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string1", 10);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 0);
  {
    auto viter = txn->NewVertexIterator(
        "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
    EXPECT_TRUE(viter->Valid());
    viter->GetVertex().SetProperties(
        {{"str", Value("string1 string string11")}});
  }
  txn->Commit();
  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }
  txn = graphDB->BeginTransaction();
  count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "string1", 10);
       viter->Valid(); viter->Next()) {
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();
}

TEST(FTIndex, repeatedUpdatesInSingleTransactionApplyLatestDocument) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 1;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value(1)}, {"str", Value("original_token")}});
  txn->Commit();
  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index", "original_token",
                                  1, std::chrono::milliseconds(2500)));

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().SetProperties(
      {{"str", Value("middle_token temporary_token")}});
  viter->GetVertex().SetProperties(
      {{"str", Value("latest_token final_token")}});
  txn->Commit();

  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index", "original_token",
                                  0, std::chrono::milliseconds(2500)));
  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index", "middle_token", 0,
                                  std::chrono::milliseconds(2500)));
  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index", "latest_token", 1,
                                  std::chrono::milliseconds(2500)));
  EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index", "final_token", 1,
                                  std::chrono::milliseconds(2500)));
}

TEST(FTIndex, deleteOneMatchedLabelKeepsDocumentIndexed) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());
  graphDB->AddVertexFullTextIndex("ft_index", {"label1", "label2"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1", "label2"},
                    {{"id", Value(1)}, {"str", Value("retain_me only_once")}});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  int count = 0;
  for (auto viter = txn->QueryVertexByFTIndex("ft_index", "retain_me", 10);
       viter->Valid(); viter->Next()) {
    EXPECT_EQ(viter->GetVertexScore().vertex.GetProperty("id"), Value(1));
    count++;
  }
  EXPECT_EQ(count, 1);
  txn->Commit();

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
  EXPECT_TRUE(viter->Valid());
  viter->GetVertex().DeleteLabels({"label1"});
  txn->Commit();

  for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
    index->ApplyWAL();
  }

  txn = graphDB->BeginTransaction();
  count = 0;
  for (auto result = txn->QueryVertexByFTIndex("ft_index", "retain_me", 10);
       result->Valid(); result->Next()) {
    EXPECT_EQ(result->GetVertexScore().vertex.GetProperty("id"), Value(1));
    count++;
  }
  EXPECT_EQ(count, 1);
  auto updated = txn->NewVertexIterator(
      "label2", std::unordered_map<std::string, Value>{{"id", Value(1)}});
  EXPECT_TRUE(updated->Valid());
  EXPECT_EQ(updated->GetVertex().GetLabels(),
            (std::unordered_set<std::string>{"label2"}));
  txn->Commit();
}

TEST(FTIndex, reopenRecoversPendingWalAndContinuesFromPayload) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 1;
  {
    auto graphDB = GraphDB::Open(testdb, options);
    graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
    ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex({"label1"}, {{"id", Value(1)},
                                   {"str", Value("pending_restart_token")}});
    txn->Commit();
  }

  {
    auto graphDB = GraphDB::Open(testdb, options);
    EXPECT_TRUE(WaitUntilQueryCount(graphDB.get(), "ft_index",
                                    "pending_restart_token", 1,
                                    std::chrono::milliseconds(2500)));

    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex({"label1"}, {{"id", Value(2)},
                                   {"str", Value("before_payload_restart")}});
    txn->Commit();

    for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
      index->ApplyWAL();
    }

    txn = graphDB->BeginTransaction();
    for (auto result = txn->QueryVertexByFTIndex("ft_index",
                                                 "before_payload_restart", 10);
         result->Valid(); result->Next()) {
      EXPECT_EQ(result->GetVertexScore().vertex.GetProperty("id"), Value(2));
    }
    txn->Commit();
  }

  {
    auto graphDB = GraphDB::Open(testdb, options);
    auto txn = graphDB->BeginTransaction();
    txn->CreateVertex({"label1"}, {{"id", Value(3)},
                                   {"str", Value("after_payload_restart")}});
    txn->Commit();

    for (const auto& index : graphDB->meta_info().GetVertexFullTextIndexes()) {
      index->ApplyWAL();
    }

    txn = graphDB->BeginTransaction();
    int count = 0;
    for (auto result =
             txn->QueryVertexByFTIndex("ft_index", "after_payload_restart", 10);
         result->Valid(); result->Next()) {
      EXPECT_EQ(result->GetVertexScore().vertex.GetProperty("id"), Value(3));
      count++;
    }
    EXPECT_EQ(count, 1);
    txn->Commit();
  }
}

TEST(FTIndex, createIndexClearsStaleArtifactsFromPreviousFailedBuild) {
  fs::remove_all(testdb);
  auto graphDB = GraphDB::Open(testdb, testutil::NewGraphDBOptions());

  std::string stale_path = testdb + "/ft/ft_index_1";
  ::rust::Vec<::rust::String> properties = {"str"};
  auto stale_ft = new_ftindex(stale_path, properties, kDefaultFTWriterThreads,
                              kDefaultFTWriterMemoryBudget);
  ::rust::Vec<::rust::String> fields = {"str"};
  ::rust::Vec<::rust::String> values = {"stale_from_directory"};
  ft_add_document(*stale_ft, 777, fields, values);
  ft_commit(*stale_ft, "0");

  uint32_t stale_index_id =
      boost::endian::native_to_big(static_cast<uint32_t>(1));
  int64_t stale_vid = boost::endian::native_to_big(static_cast<int64_t>(888));

  std::string stale_index_key(AsChars(stale_index_id), sizeof(stale_index_id));
  stale_index_key.append(AsChars(stale_vid), sizeof(stale_vid));

  uint64_t stale_wal_id =
      boost::endian::native_to_big(static_cast<uint64_t>(1));
  std::string stale_wal_key(AsChars(stale_index_id), sizeof(stale_index_id));
  stale_wal_key.append(AsChars(stale_wal_id), sizeof(stale_wal_id));

  meta::FullTextIndexUpdate stale_update;
  stale_update.set_type(meta::UpdateType::Add);
  stale_update.set_vid(stale_vid);
  stale_update.add_fields("str");
  stale_update.add_values("stale_from_wal");

  auto txn = graphDB->BeginTransaction();
  auto s = txn->dbtxn()->GetWriteBatch()->Put(graphDB->graph_cf().index,
                                              stale_index_key, {});
  ASSERT_TRUE(s.ok());
  s = txn->dbtxn()->GetWriteBatch()->Put(graphDB->graph_cf().wal, stale_wal_key,
                                         stale_update.SerializeAsString());
  ASSERT_TRUE(s.ok());
  txn->Commit();

  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value(1)}, {"str", Value("fresh_token")}});
  txn->Commit();

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);
  index->ApplyWAL();

  std::string prefix(AsChars(index->index_id()), sizeof(index->index_id()));
  EXPECT_EQ(
      CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().index, prefix), 0);

  txn = graphDB->BeginTransaction();
  int stale_dir_count = 0;
  for (auto result =
           txn->QueryVertexByFTIndex("ft_index", "stale_from_directory", 10);
       result->Valid(); result->Next()) {
    stale_dir_count++;
  }
  EXPECT_EQ(stale_dir_count, 0);

  int stale_wal_count = 0;
  for (auto result =
           txn->QueryVertexByFTIndex("ft_index", "stale_from_wal", 10);
       result->Valid(); result->Next()) {
    stale_wal_count++;
  }
  EXPECT_EQ(stale_wal_count, 0);

  int fresh_count = 0;
  for (auto result = txn->QueryVertexByFTIndex("ft_index", "fresh_token", 10);
       result->Valid(); result->Next()) {
    EXPECT_EQ(result->GetVertexScore().vertex.GetProperty("id"), Value(1));
    fresh_count++;
  }
  EXPECT_EQ(fresh_count, 1);
  txn->Commit();
}

TEST(FTIndex, applyWalDoesNotWriteIndexMarkers) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);

  auto txn = graphDB->BeginTransaction();
  auto vertex = txn->CreateVertex({"label1"}, {{"id", Value(1)}});
  int64_t vid = vertex.GetId();
  txn->Commit();
  std::string prefix(AsChars(index->index_id()), sizeof(index->index_id()));
  EXPECT_EQ(
      CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().index, prefix), 0);

  meta::FullTextIndexUpdate add;
  add.set_type(meta::UpdateType::Add);
  add.set_vid(vid);
  add.add_fields("str");
  add.add_values("manual_token");

  txn = graphDB->BeginTransaction();
  auto s = txn->dbtxn()->GetWriteBatch()->Put(
      graphDB->graph_cf().wal, index->NextWALKey(), add.SerializeAsString());
  ASSERT_TRUE(s.ok());
  txn->Commit();

  index->ApplyWAL();
  EXPECT_EQ(
      CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().index, prefix), 0);
  EXPECT_TRUE(
      WaitUntilQueryCount(graphDB.get(), "ft_index", "manual_token", 1));

  meta::FullTextIndexUpdate del;
  del.set_type(meta::UpdateType::Delete);
  del.set_vid(vid);

  txn = graphDB->BeginTransaction();
  s = txn->dbtxn()->GetWriteBatch()->Put(
      graphDB->graph_cf().wal, index->NextWALKey(), del.SerializeAsString());
  ASSERT_TRUE(s.ok());
  txn->Commit();

  index->ApplyWAL();
  EXPECT_EQ(
      CountKeysWithPrefix(graphDB.get(), graphDB->graph_cf().index, prefix), 0);
  EXPECT_TRUE(
      WaitUntilQueryCount(graphDB.get(), "ft_index", "manual_token", 0));
}

TEST(FTIndex, buildingSetPropertiesWritesDeleteWalFromPreviousProperties) {
  fs::remove_all(testdb);
  GraphDBOptions options = testutil::NewGraphDBOptions();
  options.ft_apply_interval_ = 3600;
  auto graphDB = GraphDB::Open(testdb, options);
  graphDB->AddVertexFullTextIndex("ft_index", {"label1"}, {"str"});
  ASSERT_TRUE(WaitUntilFullTextIndexReady(graphDB.get(), "ft_index"));

  auto index = graphDB->meta_info().GetVertexFullTextIndex("ft_index");
  ASSERT_TRUE(index != nullptr);

  auto txn = graphDB->BeginTransaction();
  txn->CreateVertex({"label1"},
                    {{"id", Value(1)}, {"str", Value("before_update")}});
  txn->Commit();

  index->ApplyWAL();

  index->SetState(meta::IndexBuildState::BUILDING);

  txn = graphDB->BeginTransaction();
  auto viter = txn->NewVertexIterator(
      "label1", std::unordered_map<std::string, Value>{{"id", Value(1)}});
  ASSERT_TRUE(viter->Valid());
  viter->GetVertex().SetProperties({{"str", Value(10)}});
  txn->Commit();

  auto updates = ReadFullTextWalUpdates(graphDB.get(), index);
  ASSERT_EQ(updates.size(), 1);
  EXPECT_EQ(updates[0].type(), meta::UpdateType::Delete);
  EXPECT_EQ(updates[0].fields_size(), 0);
  EXPECT_EQ(updates[0].values_size(), 0);
}
