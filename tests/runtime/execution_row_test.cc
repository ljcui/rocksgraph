#include "runtime/execution_row.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "common/exception.h"
#include "graphdb/graph_entity.h"
#include "tests/common/exception_test_utils.h"
#include "tests/runtime/graphdb_test_utils.h"

TEST(ExecutionRowTest, StoresGraphDBEntitiesAndValues) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  const auto node_vertex =
      transaction->CreateVertex({"Person"}, {{"name", rg::Value("Ada")}});
  const auto other_vertex = transaction->CreateVertex({}, {});
  const auto relationship_edge =
      transaction->CreateEdge(node_vertex, other_vertex, "KNOWS", {});
  auto layout = std::make_shared<const rg::RowLayout>(
      std::vector<std::string>{"n", "r", "value"});

  rg::ExecutionRow row(layout);
  EXPECT_EQ(layout->At("n"), 0);
  EXPECT_EQ(layout->At("r"), 1);
  EXPECT_EQ(layout->At("value"), 2);
  row.Set(layout->At("n"), node_vertex);
  row.Set(layout->At("r"), relationship_edge);
  row.Set(layout->At("value"), rg::Value(42));

  EXPECT_EQ(row.VertexAt(layout->At("n")).GetId(), node_vertex.GetId());
  EXPECT_EQ(row.EdgeAt(layout->At("r")).GetId(), relationship_edge.GetId());
  EXPECT_EQ(row.ValueAt(layout->At("value")).AsInteger(), 42);
  EXPECT_EQ(row.Get(layout->At("n")).AsNode().id, node_vertex.GetId());
  EXPECT_EQ(row.Get(layout->At("r")).AsRelationship().id,
            relationship_edge.GetId());
  transaction->Rollback();
}

TEST(ExecutionRowTest, DistinguishesUninitializedCellsFromNull) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  auto layout =
      std::make_shared<const rg::RowLayout>(std::vector<std::string>{"n"});

  rg::ExecutionRow row(layout);
  EXPECT_FALSE(row.IsInitialized(layout->At("n")));

  row.SetNull(layout->At("n"));
  EXPECT_TRUE(row.IsInitialized(layout->At("n")));
  EXPECT_TRUE(row.Get(layout->At("n")).IsNull());
  transaction->Rollback();
}

TEST(ExecutionRowTest, RejectsOutOfRangeWrites) {
  auto layout =
      std::make_shared<const rg::RowLayout>(std::vector<std::string>{"value"});
  rg::ExecutionRow row(layout);

  RG_EXPECT_ERROR(row.Set(layout->CellCount(), rg::Value(42)),
                  common::ErrorCode::InternalError);
  RG_EXPECT_ERROR(row.SetNull(layout->CellCount()),
                  common::ErrorCode::InternalError);
}

TEST(ExecutionRowTest, ResetsRowsWithoutChangingCompatibleStorage) {
  auto layout =
      std::make_shared<const rg::RowLayout>(std::vector<std::string>{"value"});
  rg::ExecutionRow row(layout);
  row.Set(layout->At("value"), rg::Value(42));
  row.Reset();
  EXPECT_FALSE(row.IsInitialized(layout->At("value")));

  row.Set(layout->At("value"), rg::Value(7));
  row.Reset(layout);
  EXPECT_FALSE(row.IsInitialized(layout->At("value")));
}

TEST(ExecutionRowTest, CopiesBetweenLayoutsWithoutChangingStoredValues) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  const auto vertex = transaction->CreateVertex({}, {});
  auto entity_offsets =
      std::make_shared<const rg::RowLayout>(std::vector<std::string>{"x"});
  auto target_offsets =
      std::make_shared<const rg::RowLayout>(std::vector<std::string>{"x"});

  rg::ExecutionRow source(entity_offsets);
  source.Set(entity_offsets->At("x"), vertex);
  const std::vector<rg::RowMapping> mappings{
      {.source_offset = entity_offsets->At("x"),
       .target_offset = target_offsets->At("x")}};
  rg::ExecutionRow copied = source.CopyTo(target_offsets, mappings);

  EXPECT_EQ(copied.VertexAt(target_offsets->At("x")).GetId(), vertex.GetId());
  EXPECT_TRUE(copied.Get(target_offsets->At("x")).IsNode());
  EXPECT_EQ(copied.Get(target_offsets->At("x")).AsNode().id, vertex.GetId());

  const std::vector<rg::RowMapping> reverse_mappings{
      {.source_offset = target_offsets->At("x"),
       .target_offset = entity_offsets->At("x")}};
  rg::ExecutionRow copied_back =
      copied.CopyTo(entity_offsets, reverse_mappings);
  EXPECT_EQ(copied_back.VertexAt(entity_offsets->At("x")).GetId(),
            vertex.GetId());

  copied.MaterializeGraphEntities();
  EXPECT_EQ(copied.ValueAt(target_offsets->At("x")).AsNode().id,
            vertex.GetId());
  transaction->Rollback();
}

TEST(ExecutionRowTest, GraphDBExposesClosableVertexIterators) {
  rg::test::GraphDBTestDatabase database;
  const auto first = database.CreateVertex();
  const auto second = database.CreateVertex();

  auto transaction = database.BeginTransaction();
  auto cursor = transaction->NewVertexIterator();
  ASSERT_TRUE(cursor->Valid());
  EXPECT_EQ(cursor->GetVertex().GetId(), first);
  cursor->Next();
  ASSERT_TRUE(cursor->Valid());
  EXPECT_EQ(cursor->GetVertex().GetId(), second);
  cursor->Next();
  EXPECT_FALSE(cursor->Valid());
  transaction->Commit();
}
