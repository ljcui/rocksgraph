#include "runtime/write_executor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "common/exception.h"
#include "ir/logical_plan.h"
#include "runtime/query_row.h"
#include "storage/in_memory_graph.h"
#include "value/value.h"

namespace {

std::unique_ptr<ir::ArgumentPlan> Arguments(std::vector<std::string> columns) {
  return std::make_unique<ir::ArgumentPlan>(std::move(columns));
}

std::unique_ptr<ast::Variable> Variable(std::string name) {
  auto variable = std::make_unique<ast::Variable>();
  variable->name = std::move(name);
  return variable;
}

std::unique_ptr<ast::IntegerLiteral> Integer(std::int64_t value) {
  auto literal = std::make_unique<ast::IntegerLiteral>();
  literal->value = value;
  return literal;
}

std::unique_ptr<ast::StringLiteral> String(std::string value) {
  auto literal = std::make_unique<ast::StringLiteral>();
  literal->value = std::move(value);
  return literal;
}

std::unique_ptr<ast::NullLiteral> Null() {
  return std::make_unique<ast::NullLiteral>();
}

rg::WritePlanExecutor SourceRows(const ir::LogicalPlan &plan,
                                 rg::QueryRows rows) {
  return [&plan, rows = std::move(rows)](const ir::LogicalPlan &child,
                                         const rg::QueryRows &) {
    EXPECT_EQ(&child, &plan.Child(0));
    return rows;
  };
}

const rg::Value &Column(const rg::QueryRow &row, const std::string &name) {
  return row.at(name);
}

}  // namespace

TEST(WriteExecutorTest, CreatesNodesAndRelationshipsWithoutNullProperties) {
  rg::InMemoryGraph graph;
  rg::WriteExecutor executor(&graph);
  auto name = String("Ada");
  auto missing = Null();
  ir::CreateNodePattern node_pattern{
      .variable = "n",
      .labels = {"Person"},
      .properties = {
          .entries = {{"name", name.get()}, {"missing", missing.get()}}}};
  ir::CreateNodePlan node_plan(Arguments({}), std::move(node_pattern));

  const rg::QueryRows node_rows = executor.Execute(
      node_plan, {rg::QueryRow{}}, SourceRows(node_plan, {rg::QueryRow{}}));
  ASSERT_EQ(node_rows.size(), 1U);
  const rg::Node &created = Column(node_rows[0], "n").AsNode();
  EXPECT_EQ(created.properties.at("name"), rg::Value("Ada"));
  EXPECT_FALSE(created.properties.contains("missing"));

  auto since = Integer(2026);
  auto relationship_null = Null();
  ir::CreateRelationshipPattern relationship_pattern{
      .variable = "r",
      .left_node = "left",
      .right_node = "right",
      .types = {"LINK"},
      .properties = {.entries = {{"since", since.get()},
                                 {"missing", relationship_null.get()}}}};
  ir::CreateRelationshipPlan relationship_plan(Arguments({"left", "right"}),
                                               std::move(relationship_pattern));
  auto right = graph.CreateNode({"Person"});
  const rg::QueryRows relationship_rows = executor.Execute(
      relationship_plan, {rg::QueryRow{}},
      SourceRows(relationship_plan,
                 {{{"left", rg::Value(graph.NodeById(created.id))},
                   {"right", rg::Value(right)}}}));

  ASSERT_EQ(relationship_rows.size(), 1U);
  const rg::Relationship &relationship =
      Column(relationship_rows[0], "r").AsRelationship();
  EXPECT_EQ(relationship.properties.at("since"), rg::Value(2026));
  EXPECT_FALSE(relationship.properties.contains("missing"));
}

TEST(WriteExecutorTest, SetsNodeAndRelationshipPropertiesAndLabels) {
  rg::InMemoryGraph graph;
  auto left = graph.CreateNode({"Old"}, {{"remove", rg::Value(1)}});
  auto right = graph.CreateNode({"Node"});
  auto relationship =
      graph.CreateRelationship(left, right, "LINK", {{"old", rg::Value(1)}});
  rg::WriteExecutor executor(&graph);

  auto node = Variable("n");
  auto null_value = Null();
  ir::SetPropertyPlan set_null(Arguments({"n"}), node.get(), "remove",
                               null_value.get());
  (void)executor.Execute(set_null, {rg::QueryRow{}},
                         SourceRows(set_null, {{{"n", rg::Value(left)}}}));
  EXPECT_FALSE(left->properties.contains("remove"));

  auto rel = Variable("r");
  auto exact_map = std::make_unique<ast::MapLiteral>();
  exact_map->entries.emplace_back("fresh", String("value"));
  exact_map->entries.emplace_back("null", Null());
  ir::SetPropertiesPlan set_exact(Arguments({"r"}), rel.get(), exact_map.get(),
                                  false);
  (void)executor.Execute(
      set_exact, {rg::QueryRow{}},
      SourceRows(set_exact, {{{"r", rg::Value(relationship)}}}));
  EXPECT_EQ(relationship->properties,
            (rg::Value::Map{{"fresh", rg::Value("value")}}));

  auto merge_map = std::make_unique<ast::MapLiteral>();
  merge_map->entries.emplace_back("extra", Integer(2));
  merge_map->entries.emplace_back("fresh", Null());
  ir::SetPropertiesPlan set_including(Arguments({"r"}), rel.get(),
                                      merge_map.get(), true);
  (void)executor.Execute(
      set_including, {rg::QueryRow{}},
      SourceRows(set_including, {{{"r", rg::Value(relationship)}}}));
  EXPECT_EQ(relationship->properties,
            (rg::Value::Map{{"extra", rg::Value(2)}}));

  ir::SetLabelsPlan set_labels(Arguments({"n"}), node.get(), {"New"});
  (void)executor.Execute(set_labels, {rg::QueryRow{}},
                         SourceRows(set_labels, {{{"n", rg::Value(left)}}}));
  EXPECT_EQ(left->labels, (std::vector<std::string>{"New", "Old"}));
}

TEST(WriteExecutorTest, RemovesPropertiesAndLabels) {
  rg::InMemoryGraph graph;
  auto left = graph.CreateNode({"Keep", "Remove"}, {{"key", rg::Value(1)}});
  auto right = graph.CreateNode({"Node"});
  auto relationship =
      graph.CreateRelationship(left, right, "LINK", {{"key", rg::Value(2)}});
  rg::WriteExecutor executor(&graph);
  auto node = Variable("n");
  auto rel = Variable("r");

  ir::RemovePropertyPlan remove_node(Arguments({"n"}), node.get(), "key");
  (void)executor.Execute(remove_node, {rg::QueryRow{}},
                         SourceRows(remove_node, {{{"n", rg::Value(left)}}}));
  EXPECT_FALSE(left->properties.contains("key"));

  ir::RemovePropertyPlan remove_relationship(Arguments({"r"}), rel.get(),
                                             "key");
  (void)executor.Execute(
      remove_relationship, {rg::QueryRow{}},
      SourceRows(remove_relationship, {{{"r", rg::Value(relationship)}}}));
  EXPECT_FALSE(relationship->properties.contains("key"));

  ir::RemoveLabelsPlan remove_labels(Arguments({"n"}), node.get(), {"Remove"});
  (void)executor.Execute(remove_labels, {rg::QueryRow{}},
                         SourceRows(remove_labels, {{{"n", rg::Value(left)}}}));
  EXPECT_EQ(left->labels, (std::vector<std::string>{"Keep"}));
}

TEST(WriteExecutorTest, MergeRunsCreateAndMatchActions) {
  rg::InMemoryGraph graph;
  rg::WriteExecutor executor(&graph);
  auto node_value = Variable("n");
  auto created_value = Integer(1);
  auto matched_value = Integer(2);
  ir::MergePattern merge;
  merge.create_pattern.nodes.push_back({.variable = "n", .labels = {"Person"}});
  merge.create_pattern.commands.push_back(
      {.kind = ir::CreateEntityKind::kNode, .index = 0});
  merge.actions = {
      {.on_match = false,
       .set_patterns = {{.kind = ir::SetMutatingPatternKind::kSetProperty,
                         .entity = node_value.get(),
                         .property_key = "created",
                         .value = created_value.get()}}},
      {.on_match = true,
       .set_patterns = {{.kind = ir::SetMutatingPatternKind::kSetProperty,
                         .entity = node_value.get(),
                         .property_key = "matched",
                         .value = matched_value.get()}}}};
  ir::MergePlan create_plan(Arguments({}), Arguments({"n"}), merge);
  const rg::WritePlanExecutor create_executor =
      [&create_plan](const ir::LogicalPlan &child, const rg::QueryRows &) {
        if (&child == &create_plan.Child(0)) {
          return rg::QueryRows{rg::QueryRow{}};
        }
        return rg::QueryRows{};
      };

  const rg::QueryRows created =
      executor.Execute(create_plan, {rg::QueryRow{}}, create_executor);
  ASSERT_EQ(created.size(), 1U);
  const auto created_node = graph.NodeById(Column(created[0], "n").AsNode().id);
  EXPECT_EQ(created_node->properties.at("created"), rg::Value(1));

  ir::MergePlan match_plan(Arguments({}), Arguments({"n"}), merge);
  const rg::WritePlanExecutor match_executor = [&match_plan, created_node](
                                                   const ir::LogicalPlan &child,
                                                   const rg::QueryRows &) {
    if (&child == &match_plan.Child(0)) {
      return rg::QueryRows{rg::QueryRow{}};
    }
    return rg::QueryRows{{{"n", rg::Value(created_node)}}};
  };
  (void)executor.Execute(match_plan, {rg::QueryRow{}}, match_executor);
  EXPECT_EQ(created_node->properties.at("matched"), rg::Value(2));
}

TEST(WriteExecutorTest, RejectsNullMergeProperties) {
  rg::InMemoryGraph graph;
  rg::WriteExecutor executor(&graph);
  auto null_value = Null();
  ir::MergePattern merge;
  merge.create_pattern.nodes.push_back(
      {.variable = "n",
       .labels = {"Person"},
       .properties = {.entries = {{"key", null_value.get()}}}});
  merge.create_pattern.commands.push_back(
      {.kind = ir::CreateEntityKind::kNode, .index = 0});
  ir::MergePlan plan(Arguments({}), Arguments({"n"}), std::move(merge));
  const rg::WritePlanExecutor execute_plan =
      [&plan](const ir::LogicalPlan &child, const rg::QueryRows &) {
        if (&child == &plan.Child(0)) {
          return rg::QueryRows{rg::QueryRow{}};
        }
        return rg::QueryRows{};
      };

  EXPECT_THROW((void)executor.Execute(plan, {rg::QueryRow{}}, execute_plan),
               common::InvalidArgumentError);
  EXPECT_TRUE(graph.Nodes().empty());
}

TEST(WriteExecutorTest, RejectsReadOnlyWritesEvenWithEmptySource) {
  rg::WriteExecutor executor(nullptr);
  auto node = Variable("n");
  auto value = Integer(1);
  ir::SetPropertyPlan plan(Arguments({"n"}), node.get(), "key", value.get());
  const rg::WritePlanExecutor empty_source =
      [&plan](const ir::LogicalPlan &child, const rg::QueryRows &) {
        EXPECT_EQ(&child, &plan.Child(0));
        return rg::QueryRows{};
      };

  EXPECT_THROW((void)executor.Execute(plan, {rg::QueryRow{}}, empty_source),
               common::InvalidArgumentError);
}

TEST(WriteExecutorTest, DeletesEntitiesOnceAndValidatesConnectedNodes) {
  rg::InMemoryGraph graph;
  auto left = graph.CreateNode({"Node"});
  auto right = graph.CreateNode({"Node"});
  auto relationship = graph.CreateRelationship(left, right, "LINK");
  rg::WriteExecutor executor(&graph);
  auto node = Variable("n");
  auto rel = Variable("r");

  ir::DeletePlan invalid(Arguments({"n"}), {node.get()});
  EXPECT_THROW(
      (void)executor.Execute(invalid, {rg::QueryRow{}},
                             SourceRows(invalid, {{{"n", rg::Value(left)}}})),
      common::InvalidArgumentError);
  EXPECT_TRUE(graph.HasNode(left->id));

  ir::DeletePlan valid(Arguments({"n", "r"}), {node.get(), rel.get()});
  const rg::QueryRows duplicate_rows = {
      {{"n", rg::Value(left)}, {"r", rg::Value(relationship)}},
      {{"n", rg::Value(left)}, {"r", rg::Value(relationship)}}};
  (void)executor.Execute(valid, {rg::QueryRow{}},
                         SourceRows(valid, duplicate_rows));
  EXPECT_FALSE(graph.HasNode(left->id));
  EXPECT_FALSE(graph.HasRelationship(relationship->id));
  EXPECT_TRUE(graph.HasNode(right->id));
}

TEST(WriteExecutorTest, DetachDeleteDeduplicatesNodesAndRelationships) {
  rg::InMemoryGraph graph;
  auto left = graph.CreateNode({"Node"});
  auto right = graph.CreateNode({"Node"});
  auto relationship = graph.CreateRelationship(left, right, "LINK");
  rg::WriteExecutor executor(&graph);
  auto node = Variable("n");
  ir::DetachDeletePlan plan(Arguments({"n"}), {node.get()});
  const rg::QueryRows duplicate_rows = {{{"n", rg::Value(left)}},
                                        {{"n", rg::Value(left)}}};

  (void)executor.Execute(plan, {rg::QueryRow{}},
                         SourceRows(plan, duplicate_rows));

  EXPECT_FALSE(graph.HasNode(left->id));
  EXPECT_FALSE(graph.HasRelationship(relationship->id));
  EXPECT_TRUE(graph.HasNode(right->id));
}
