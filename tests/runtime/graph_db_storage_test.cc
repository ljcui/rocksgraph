#include "storage/graph_db_storage.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "runtime/query_executor.h"

namespace {

class GraphDBStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::path(testing::TempDir()) /
            ("rocksgraph_query_storage_" +
             std::to_string(testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(path_);
  }

  void TearDown() override { std::filesystem::remove_all(path_); }

  [[nodiscard]] rg::QueryOptions OptionsFor(
      const rg::GraphDBStorage &storage) const {
    return {.planner_statistics = &storage, .planner_catalog = &storage};
  }

  std::filesystem::path path_;
};

TEST_F(GraphDBStorageTest, ExecutesAndPersistsReadWriteQueries) {
  std::int64_t ada_id = 0;
  {
    auto storage = rg::GraphDBStorage::Open(path_.string());
    auto transaction = storage->BeginTransaction();
    auto created = rg::ExecuteQuery(
        *transaction,
        "CREATE (a:Person {name: 'Ada'}), (b:Person {name: 'Grace'}), "
        "(a)-[:KNOWS {since: 2020}]->(b) RETURN id(a)",
        OptionsFor(*storage));
    ASSERT_EQ(created.rows.size(), 1U);
    ada_id = created.rows.front().front().AsInteger();
    EXPECT_GT(ada_id, 0);
    transaction->Commit();

    transaction = storage->BeginTransaction();
    auto result = rg::ExecuteQuery(*transaction,
                                   "MATCH (a:Person)-[r:KNOWS]->(b:Person) "
                                   "RETURN a.name, b.name, r.since",
                                   OptionsFor(*storage));
    ASSERT_EQ(result.rows.size(), 1U);
    EXPECT_EQ(result.rows[0][0], rg::Value("Ada"));
    EXPECT_EQ(result.rows[0][1], rg::Value("Grace"));
    EXPECT_EQ(result.rows[0][2], rg::Value(2020));
    transaction->Commit();

    transaction = storage->BeginTransaction();
    auto updated = rg::ExecuteQuery(
        *transaction,
        "MATCH (a:Person {name: 'Ada'}) SET a.age = 37 RETURN a.age",
        OptionsFor(*storage));
    ASSERT_EQ(updated.rows.size(), 1U);
    EXPECT_EQ(updated.rows[0][0], rg::Value(37));
    transaction->Commit();

    transaction = storage->BeginTransaction();
    (void)rg::ExecuteQuery(*transaction, "CREATE (:Temporary)",
                           OptionsFor(*storage));
    transaction->Rollback();
  }

  auto reopened = rg::GraphDBStorage::Open(path_.string());
  auto transaction = reopened->BeginTransaction();
  auto result = rg::ExecuteQuery(*transaction,
                                 "MATCH (a:Person) WHERE id(a) = $id "
                                 "OPTIONAL MATCH (a)-[r:KNOWS]->(b) "
                                 "RETURN a.name, a.age, b.name, r.since",
                                 {.planner_statistics = reopened.get(),
                                  .planner_catalog = reopened.get(),
                                  .parameters = {{"id", rg::Value(ada_id)}}});
  ASSERT_EQ(result.rows.size(), 1U);
  EXPECT_EQ(result.rows[0][0], rg::Value("Ada"));
  EXPECT_EQ(result.rows[0][1], rg::Value(37));
  EXPECT_EQ(result.rows[0][2], rg::Value("Grace"));
  EXPECT_EQ(result.rows[0][3], rg::Value(2020));
  transaction->Commit();

  transaction = reopened->BeginTransaction();
  auto temporary = rg::ExecuteQuery(
      *transaction, "MATCH (n:Temporary) RETURN n", OptionsFor(*reopened));
  EXPECT_TRUE(temporary.rows.empty());
  transaction->Commit();
}

TEST_F(GraphDBStorageTest, ExposesNativeIdsAndDirectionalAdjacency) {
  auto storage = rg::GraphDBStorage::Open(path_.string());
  auto transaction = storage->BeginTransaction();
  rg::Storage *writer = transaction->Writer();
  const auto first = writer->CreateNode({"N"}, {{"name", rg::Value("a")}});
  const auto second = writer->CreateNode({"N"}, {{"name", rg::Value("b")}});
  const auto relationship = writer->CreateRelationship(
      first->id, second->id, "R", {{"weight", rg::Value(2)}});

  EXPECT_GT(first->id, 0);
  EXPECT_GT(second->id, first->id);
  EXPECT_GT(relationship->id, 0);

  auto outgoing = transaction->Reader().OutgoingRelationshipIds(first->id);
  ASSERT_TRUE(outgoing->Next());
  EXPECT_EQ(outgoing->Id(), relationship->id);
  EXPECT_FALSE(outgoing->Next());

  auto incoming = transaction->Reader().IncomingRelationshipIds(second->id);
  ASSERT_TRUE(incoming->Next());
  EXPECT_EQ(incoming->Id(), relationship->id);
  EXPECT_FALSE(incoming->Next());

  EXPECT_EQ(
      transaction->Reader().RelationshipProperty(relationship->id, "weight"),
      rg::Value(2));
  transaction->Commit();
}

}  // namespace
