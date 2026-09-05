#include "runtime/slotted_row.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "storage/in_memory_graph.h"

TEST(SlottedRowTest, StoresEntitiesAsIdsAndMaterializesAtTheBoundary) {
  rg::InMemoryGraph graph;
  auto node = graph.CreateNode({"Person"}, {{"name", rg::Value("Ada")}});
  auto other = graph.CreateNode({});
  auto relationship = graph.CreateRelationship(node, other, "KNOWS");
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
  rg::QueryRow materialized = row.Materialize(graph);
  EXPECT_EQ(materialized.at("n").AsNode().id, node->id);
  EXPECT_EQ(materialized.at("r").AsRelationship().id, relationship->id);
}

TEST(SlottedRowTest, CopiesBetweenLayoutsAndConvertsEntityRepresentations) {
  rg::InMemoryGraph graph;
  auto node = graph.CreateNode({});
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
  rg::SlottedRow copied = source.CopyTo(reference_slots, graph);

  EXPECT_EQ(copied.Slots()->At("x").kind, rg::SlotKind::kReference);
  EXPECT_TRUE(copied.Get("x", graph).IsNode());
  EXPECT_EQ(copied.Get("x", graph).AsNode().id, node->id);
}

TEST(SlottedRowTest, InMemoryGraphExposesClosableIdCursors) {
  rg::InMemoryGraph graph;
  auto first = graph.CreateNode({});
  auto second = graph.CreateNode({});

  std::unique_ptr<rg::EntityIdCursor> cursor = graph.ScanNodeIds();
  ASSERT_TRUE(cursor->Next());
  EXPECT_EQ(cursor->Id(), first->id);
  ASSERT_TRUE(cursor->Next());
  EXPECT_EQ(cursor->Id(), second->id);
  EXPECT_FALSE(cursor->Next());
  cursor->Close();
  EXPECT_FALSE(cursor->Next());
}
