#include "runtime/slotted_row.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "graphdb/graph_entity.h"
#include "tests/runtime/graphdb_test_utils.h"

TEST(SlottedRowTest, StoresEntitiesAsIdsAndReferences) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  const auto node_vertex =
      transaction->CreateVertex({"Person"}, {{"name", rg::Value("Ada")}});
  const auto other_vertex = transaction->CreateVertex({}, {});
  const auto relationship_edge =
      transaction->CreateEdge(node_vertex, other_vertex, "KNOWS", {});
  const auto node =
      rg::MaterializeGraphDBVertex(*transaction, node_vertex.GetNativeId());
  const auto relationship = rg::MaterializeGraphDBEdge(
      *transaction, {.id = relationship_edge.GetNativeId(),
                     .type_id = relationship_edge.GetTypeId()});
  auto slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<rg::SlotDefinition>{
          {.name = "n",
           .type = ast::SemanticVariableType::kNode,
           .nullable = false},
          {.name = "r",
           .type = ast::SemanticVariableType::kRelationship,
           .nullable = true},
          {.name = "value",
           .type = ast::SemanticVariableType::kScalar,
           .nullable = false}});

  rg::SlottedRow row(slots);
  row.Set("n", rg::Value(node));
  row.Set("r", rg::Value(relationship));
  row.Set("value", rg::Value(42));

  EXPECT_EQ(row.EntityIdAt(slots->At("n")), node->id);
  EXPECT_EQ(row.EntityIdAt(slots->At("r")), relationship->id);
  EXPECT_EQ(row.ReferenceAt(slots->At("value")).AsInteger(), 42);
  EXPECT_EQ(row.Get("n", *transaction).AsNode().id, node->id);
  EXPECT_EQ(row.Get("r", *transaction).AsRelationship().id, relationship->id);
  transaction->Rollback();
}

TEST(SlottedRowTest, CopiesBetweenLayoutsAndConvertsEntityRepresentations) {
  rg::test::GraphDBTestDatabase database;
  auto transaction = database.BeginTransaction();
  const auto vertex = transaction->CreateVertex({}, {});
  const auto node =
      rg::MaterializeGraphDBVertex(*transaction, vertex.GetNativeId());
  auto entity_slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<rg::SlotDefinition>{{.name = "x",
                                       .type = ast::SemanticVariableType::kNode,
                                       .nullable = false}});
  auto reference_slots = std::make_shared<const rg::SlotConfiguration>(
      std::vector<rg::SlotDefinition>{
          {.name = "x",
           .type = ast::SemanticVariableType::kUnknown,
           .nullable = true}});

  rg::SlottedRow source(entity_slots);
  source.Set("x", rg::Value(node));
  const std::vector<rg::SlotMapping> mappings{
      {.source_name = "x",
       .target_name = "x",
       .source = entity_slots->At("x"),
       .target = reference_slots->At("x")}};
  rg::SlottedRow copied =
      source.CopyTo(reference_slots, mappings, *transaction);

  EXPECT_EQ(copied.Slots()->At("x").kind, rg::SlotKind::kReference);
  EXPECT_TRUE(copied.Get("x", *transaction).IsNode());
  EXPECT_EQ(copied.Get("x", *transaction).AsNode().id, node->id);
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
