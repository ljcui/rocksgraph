#include "runtime/slotted_row.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

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
  row.SetVertex(slots->At("n"), node_vertex);
  row.SetEdge(slots->At("r"), relationship_edge);
  row.Set(slots->At("value"), rg::Value(42));

  EXPECT_EQ(row.VertexAt(slots->At("n")).GetNativeId(),
            node_vertex.GetNativeId());
  EXPECT_EQ(row.EdgeAt(slots->At("r")).GetNativeId(),
            relationship_edge.GetNativeId());
  EXPECT_EQ(row.ValueAt(slots->At("value")).AsInteger(), 42);
  EXPECT_EQ(row.Get("n").AsNode().id, node_vertex.GetNativeId());
  EXPECT_EQ(row.Get("r").AsRelationship().id, relationship_edge.GetNativeId());
  transaction->Rollback();
}

TEST(SlottedRowTest, DistinguishesUninitializedSlotsFromNull) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  auto slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<std::string>{"n"});

  rg::SlottedRow row(slots);
  EXPECT_FALSE(row.IsInitialized("n"));

  row.SetNull("n");
  EXPECT_TRUE(row.IsInitialized("n"));
  EXPECT_TRUE(row.Get("n").IsNull());
  transaction->Rollback();
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
  source.SetVertex(entity_slots->At("x"), vertex);
  const std::vector<rg::SlotMapping> mappings{
      {.source_name = "x",
       .target_name = "x",
       .source = entity_slots->At("x"),
       .target = target_slots->At("x")}};
  rg::SlottedRow copied = source.CopyTo(target_slots, mappings);

  EXPECT_EQ(copied.VertexAt(target_slots->At("x")).GetNativeId(),
            vertex.GetNativeId());
  EXPECT_TRUE(copied.Get("x").IsNode());
  EXPECT_EQ(copied.Get("x").AsNode().id, vertex.GetNativeId());

  const std::vector<rg::SlotMapping> reverse_mappings{
      {.source_name = "x",
       .target_name = "x",
       .source = target_slots->At("x"),
       .target = entity_slots->At("x")}};
  rg::SlottedRow copied_back = copied.CopyTo(entity_slots, reverse_mappings);
  EXPECT_EQ(copied_back.VertexAt(entity_slots->At("x")).GetNativeId(),
            vertex.GetNativeId());

  copied.MaterializeGraphEntities();
  EXPECT_EQ(copied.ValueAt(target_slots->At("x")).AsNode().id,
            vertex.GetNativeId());
  transaction->Rollback();
}

TEST(SlottedRowTest, GraphDBExposesClosableVertexIterators) {
  rg::test::GraphDBTestDatabase database;
  const auto first = database.CreateVertex();
  const auto second = database.CreateVertex();

  auto transaction = database.BeginTransaction();
  auto cursor = transaction->NewVertexIterator();
  ASSERT_TRUE(cursor->Valid());
  EXPECT_EQ(cursor->GetVertex().GetNativeId(), first);
  cursor->Next();
  ASSERT_TRUE(cursor->Valid());
  EXPECT_EQ(cursor->GetVertex().GetNativeId(), second);
  cursor->Next();
  EXPECT_FALSE(cursor->Valid());
  transaction->Commit();
}
