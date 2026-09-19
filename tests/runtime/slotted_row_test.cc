#include "runtime/slotted_row.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "common/exception.h"
#include "graphdb/graph_entity.h"
#include "tests/runtime/graphdb_test_utils.h"

TEST(SlottedRowTest, StoresGraphDBEntitiesAndValues) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  const auto node_vertex =
      transaction->CreateVertex({"Person"}, {{"name", rg::Value("Ada")}});
  const auto other_vertex = transaction->CreateVertex({}, {});
  const auto relationship_edge =
      transaction->CreateEdge(node_vertex, other_vertex, "KNOWS", {});
  auto slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<std::string>{"n", "r", "value"});

  rg::SlottedRow row(slots);
  EXPECT_EQ(slots->At("n"), 0);
  EXPECT_EQ(slots->At("r"), 1);
  EXPECT_EQ(slots->At("value"), 2);
  row.Set(slots->At("n"), node_vertex);
  row.Set(slots->At("r"), relationship_edge);
  row.Set(slots->At("value"), rg::Value(42));

  EXPECT_EQ(row.VertexAt(slots->At("n")).GetId(), node_vertex.GetId());
  EXPECT_EQ(row.EdgeAt(slots->At("r")).GetId(), relationship_edge.GetId());
  EXPECT_EQ(row.ValueAt(slots->At("value")).AsInteger(), 42);
  EXPECT_EQ(row.Get(slots->At("n")).AsNode().id, node_vertex.GetId());
  EXPECT_EQ(row.Get(slots->At("r")).AsRelationship().id,
            relationship_edge.GetId());
  transaction->Rollback();
}

TEST(SlottedRowTest, DistinguishesUninitializedSlotsFromNull) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  auto slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<std::string>{"n"});

  rg::SlottedRow row(slots);
  EXPECT_FALSE(row.IsInitialized(slots->At("n")));

  row.SetNull(slots->At("n"));
  EXPECT_TRUE(row.IsInitialized(slots->At("n")));
  EXPECT_TRUE(row.Get(slots->At("n")).IsNull());
  transaction->Rollback();
}

TEST(SlottedRowTest, RejectsOutOfRangeWrites) {
  auto slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<std::string>{"value"});
  rg::SlottedRow row(slots);

  EXPECT_THROW(row.Set(slots->SlotCount(), rg::Value(42)),
               common::InternalError);
  EXPECT_THROW(row.SetNull(slots->SlotCount()), common::InternalError);
}

TEST(SlottedRowTest, ResetsRowsWithoutChangingCompatibleStorage) {
  auto slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<std::string>{"value"});
  rg::SlottedRow row(slots);
  row.Set(slots->At("value"), rg::Value(42));
  row.Reset();
  EXPECT_FALSE(row.IsInitialized(slots->At("value")));

  row.Set(slots->At("value"), rg::Value(7));
  row.Reset(slots);
  EXPECT_FALSE(row.IsInitialized(slots->At("value")));
}

TEST(SlottedRowTest, CopiesBetweenLayoutsWithoutChangingStoredValues) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  const auto vertex = transaction->CreateVertex({}, {});
  auto entity_slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<std::string>{"x"});
  auto target_slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<std::string>{"x"});

  rg::SlottedRow source(entity_slots);
  source.Set(entity_slots->At("x"), vertex);
  const std::vector<rg::SlotMapping> mappings{
      {.source_offset = entity_slots->At("x"),
       .target_offset = target_slots->At("x")}};
  rg::SlottedRow copied = source.CopyTo(target_slots, mappings);

  EXPECT_EQ(copied.VertexAt(target_slots->At("x")).GetId(), vertex.GetId());
  EXPECT_TRUE(copied.Get(target_slots->At("x")).IsNode());
  EXPECT_EQ(copied.Get(target_slots->At("x")).AsNode().id, vertex.GetId());

  const std::vector<rg::SlotMapping> reverse_mappings{
      {.source_offset = target_slots->At("x"),
       .target_offset = entity_slots->At("x")}};
  rg::SlottedRow copied_back = copied.CopyTo(entity_slots, reverse_mappings);
  EXPECT_EQ(copied_back.VertexAt(entity_slots->At("x")).GetId(),
            vertex.GetId());

  copied.MaterializeGraphEntities();
  EXPECT_EQ(copied.ValueAt(target_slots->At("x")).AsNode().id, vertex.GetId());
  transaction->Rollback();
}

TEST(SlottedRowTest, GraphDBExposesClosableVertexIterators) {
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
