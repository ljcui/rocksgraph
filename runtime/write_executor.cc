#include "runtime/write_executor.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "common/exception.h"
#include "runtime/expression_evaluator.h"
#include "runtime/query_row_util.h"
#include "value/value.h"

namespace rg {
namespace {

enum class EntityKind { kNode, kRelationship };

struct EntityRef {
  EntityKind kind = EntityKind::kNode;
  std::int64_t id = -1;
};

void CheckExecutor(const WritePlanExecutor &execute_plan) {
  CHECK(static_cast<bool>(execute_plan), common::InvalidArgumentError,
        "write plan executor is empty");
}

QueryRows ExecuteSource(const ir::LogicalPlan &plan, const QueryRows &input,
                        const WritePlanExecutor &execute_plan) {
  CheckExecutor(execute_plan);
  return execute_plan(plan.Child(0), input);
}

Value::Map EvaluateProperties(const ir::PatternPropertyMap &property_map,
                              const QueryRow &row, std::string_view context,
                              ExecutionContext execution_context) {
  if (property_map.parameter != nullptr) {
    Value value =
        EvaluateExpression(*property_map.parameter, row, {}, execution_context);
    CHECK(value.IsMap(), common::InvalidArgumentError,
          std::string(context) + " properties parameter must be a map");
    return value.AsMap();
  }
  Value::Map properties;
  for (const auto &entry : property_map.entries) {
    CHECK(entry.value != nullptr, common::InvalidArgumentError,
          std::string(context) + " property value is null");
    properties[entry.key] =
        EvaluateExpression(*entry.value, row, {}, execution_context);
  }
  return properties;
}

EntityRef MakeEntityRef(const Value &value) {
  CHECK(value.IsNode() || value.IsRelationship(), common::InvalidArgumentError,
        "expected node or relationship value");
  if (value.IsNode()) {
    return {EntityKind::kNode, value.AsNode().id};
  }
  return {EntityKind::kRelationship, value.AsRelationship().id};
}

}  // namespace

QueryRows WriteExecutor::Execute(const ir::CreateNodePlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  Storage &storage = WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  QueryRows out;
  out.reserve(rows.size());
  for (const auto &row : rows) {
    auto node = storage.CreateNode(
        plan.Node().labels, EvaluateProperties(plan.Node().properties, row,
                                               "CREATE node", context_));
    QueryRow next = row;
    next[plan.Node().variable] = Value(node);
    out.push_back(std::move(next));
  }
  return out;
}

QueryRows WriteExecutor::Execute(const ir::CreateRelationshipPlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  Storage &storage = WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  QueryRows out;
  out.reserve(rows.size());
  for (const auto &row : rows) {
    const auto &relationship_pattern = plan.Relationship();
    const Value &left =
        LookupQueryVariable(row, relationship_pattern.left_node);
    const Value &right =
        LookupQueryVariable(row, relationship_pattern.right_node);
    CHECK(left.IsNode() && right.IsNode(), common::InvalidArgumentError,
          "CREATE relationship endpoints must be nodes");
    auto relationship = storage.CreateRelationship(
        left.AsNode().id, right.AsNode().id,
        relationship_pattern.types.empty() ? std::string()
                                           : relationship_pattern.types[0],
        EvaluateProperties(relationship_pattern.properties, row,
                           "CREATE relationship", context_));
    QueryRow next = row;
    next[relationship_pattern.variable] = Value(relationship);
    out.push_back(std::move(next));
  }
  return out;
}

QueryRows WriteExecutor::Execute(const ir::MergePlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  (void)WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  QueryRows out;
  for (const auto &row : rows) {
    QueryRows matches = execute_plan(plan.Child(1), QueryRows{row});
    if (!matches.empty()) {
      for (const auto &match : matches) {
        QueryRow next;
        if (!MergeQueryRows(row, match, &next)) {
          continue;
        }
        for (const auto &action : plan.Merge().actions) {
          if (action.on_match) {
            ExecuteSetPatterns(action.set_patterns, &next);
          }
        }
        out.push_back(std::move(next));
      }
      continue;
    }

    QueryRow next = ExecuteMergeCreate(plan.Merge().create_pattern, row);
    for (const auto &action : plan.Merge().actions) {
      if (!action.on_match) {
        ExecuteSetPatterns(action.set_patterns, &next);
      }
    }
    out.push_back(std::move(next));
  }
  return out;
}

QueryRow WriteExecutor::ExecuteMergeCreate(const ir::CreatePattern &pattern,
                                           QueryRow row) const {
  Storage &storage = WritableStorage();
  for (const auto &command : pattern.commands) {
    if (command.kind == ir::CreateEntityKind::kNode) {
      const auto &node_pattern = pattern.nodes.at(command.index);
      Value::Map properties = EvaluateProperties(node_pattern.properties, row,
                                                 "MERGE node", context_);
      for (const auto &[key, value] : properties) {
        (void)key;
        CHECK(!value.IsNull(), common::InvalidArgumentError,
              "MERGE node property value is null");
      }
      auto node =
          storage.CreateNode(node_pattern.labels, std::move(properties));
      row[node_pattern.variable] = Value(node);
      continue;
    }

    const auto &relationship_pattern = pattern.relationships.at(command.index);
    const Value &left =
        LookupQueryVariable(row, relationship_pattern.left_node);
    const Value &right =
        LookupQueryVariable(row, relationship_pattern.right_node);
    CHECK(left.IsNode() && right.IsNode(), common::InvalidArgumentError,
          "MERGE relationship endpoints must be nodes");
    Value::Map properties = EvaluateProperties(
        relationship_pattern.properties, row, "MERGE relationship", context_);
    for (const auto &[key, value] : properties) {
      (void)key;
      CHECK(!value.IsNull(), common::InvalidArgumentError,
            "MERGE relationship property value is null");
    }
    auto relationship = storage.CreateRelationship(
        left.AsNode().id, right.AsNode().id,
        relationship_pattern.types.empty() ? std::string()
                                           : relationship_pattern.types[0],
        std::move(properties));
    row[relationship_pattern.variable] = Value(relationship);
  }
  return row;
}

void WriteExecutor::ExecuteSetPatterns(
    const std::vector<ir::SetMutatingPattern> &patterns, QueryRow *row) const {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  for (const auto &pattern : patterns) {
    switch (pattern.kind) {
      case ir::SetMutatingPatternKind::kSetProperty:
        ApplySetProperty(pattern, row);
        break;
      case ir::SetMutatingPatternKind::kSetExactPropertiesFromMap:
        ApplySetProperties(pattern, row, false);
        break;
      case ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap:
        ApplySetProperties(pattern, row, true);
        break;
      case ir::SetMutatingPatternKind::kSetLabels:
        ApplySetLabels(pattern, row);
        break;
    }
  }
}

void WriteExecutor::ApplySetProperty(const ir::SetMutatingPattern &pattern,
                                     QueryRow *row) const {
  Storage &storage = WritableStorage();
  CHECK(pattern.entity != nullptr && pattern.value != nullptr,
        common::InvalidArgumentError, "SET property expression is null");
  const Value &entity = EvaluateExpression(*pattern.entity, *row, {}, context_);
  if (entity.IsNull()) {
    return;
  }
  Value value = EvaluateExpression(*pattern.value, *row, {}, context_);
  if (entity.IsNode()) {
    storage.SetNodeProperty(entity.AsNode().id, pattern.property_key,
                            std::move(value));
    return;
  }
  if (entity.IsRelationship()) {
    storage.SetRelationshipProperty(entity.AsRelationship().id,
                                    pattern.property_key, std::move(value));
    return;
  }
  THROW(common::InvalidArgumentError, "SET property target is not an entity");
}

void WriteExecutor::ApplySetProperties(const ir::SetMutatingPattern &pattern,
                                       QueryRow *row,
                                       bool include_existing) const {
  Storage &storage = WritableStorage();
  CHECK(pattern.entity != nullptr && pattern.value != nullptr,
        common::InvalidArgumentError, "SET properties expression is null");
  const Value &entity = EvaluateExpression(*pattern.entity, *row, {}, context_);
  if (entity.IsNull()) {
    return;
  }
  Value map_value = EvaluateExpression(*pattern.value, *row, {}, context_);
  CHECK(map_value.IsMap(), common::InvalidArgumentError,
        "SET properties requires a map value");
  if (entity.IsNode()) {
    storage.SetNodeProperties(entity.AsNode().id, std::move(map_value.AsMap()),
                              include_existing);
    return;
  }
  if (entity.IsRelationship()) {
    storage.SetRelationshipProperties(entity.AsRelationship().id,
                                      std::move(map_value.AsMap()),
                                      include_existing);
    return;
  }
  THROW(common::InvalidArgumentError, "SET properties target is not an entity");
}

void WriteExecutor::ApplySetLabels(const ir::SetMutatingPattern &pattern,
                                   QueryRow *row) const {
  CHECK(pattern.entity != nullptr, common::InvalidArgumentError,
        "SET labels expression is null");
  const Value &entity = EvaluateExpression(*pattern.entity, *row, {}, context_);
  if (entity.IsNull()) {
    return;
  }
  CHECK(entity.IsNode(), common::InvalidArgumentError,
        "SET labels target is not a node");
  WritableStorage().SetLabels(entity.AsNode().id, pattern.labels);
}

QueryRows WriteExecutor::Execute(const ir::SetPropertyPlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  (void)WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  for (auto &row : rows) {
    ExecuteSetPatterns({{.kind = ir::SetMutatingPatternKind::kSetProperty,
                         .entity = plan.Entity(),
                         .property_key = plan.PropertyKey(),
                         .value = plan.Value()}},
                       &row);
  }
  return rows;
}

QueryRows WriteExecutor::Execute(const ir::SetPropertiesPlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  (void)WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  const auto kind =
      plan.IncludeExisting()
          ? ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap
          : ir::SetMutatingPatternKind::kSetExactPropertiesFromMap;
  for (auto &row : rows) {
    ExecuteSetPatterns(
        {{.kind = kind, .entity = plan.Entity(), .value = plan.Value()}}, &row);
  }
  return rows;
}

QueryRows WriteExecutor::Execute(const ir::SetLabelsPlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  (void)WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  for (auto &row : rows) {
    ExecuteSetPatterns({{.kind = ir::SetMutatingPatternKind::kSetLabels,
                         .entity = plan.Entity(),
                         .labels = plan.Labels()}},
                       &row);
  }
  return rows;
}

QueryRows WriteExecutor::Execute(const ir::RemovePropertyPlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  Storage &storage = WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  for (auto &row : rows) {
    CHECK(plan.Entity() != nullptr, common::InvalidArgumentError,
          "REMOVE property expression is null");
    const Value &entity = EvaluateExpression(*plan.Entity(), row, {}, context_);
    if (entity.IsNull()) {
      continue;
    }
    if (entity.IsNode()) {
      storage.RemoveNodeProperty(entity.AsNode().id, plan.PropertyKey());
    } else if (entity.IsRelationship()) {
      storage.RemoveRelationshipProperty(entity.AsRelationship().id,
                                         plan.PropertyKey());
    } else {
      THROW(common::InvalidArgumentError,
            "REMOVE property target is not an entity");
    }
  }
  return rows;
}

QueryRows WriteExecutor::Execute(const ir::RemoveLabelsPlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  Storage &storage = WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  for (auto &row : rows) {
    CHECK(plan.Entity() != nullptr, common::InvalidArgumentError,
          "REMOVE labels expression is null");
    const Value &entity = EvaluateExpression(*plan.Entity(), row, {}, context_);
    if (entity.IsNull()) {
      continue;
    }
    CHECK(entity.IsNode(), common::InvalidArgumentError,
          "REMOVE labels target is not a node");
    storage.RemoveLabels(entity.AsNode().id, plan.Labels());
  }
  return rows;
}

QueryRows WriteExecutor::Execute(const ir::DeletePlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  return ExecuteDelete(plan, plan.Expressions(), input, execute_plan, false);
}

QueryRows WriteExecutor::Execute(const ir::DetachDeletePlan &plan,
                                 const QueryRows &input,
                                 const WritePlanExecutor &execute_plan) const {
  return ExecuteDelete(plan, plan.Expressions(), input, execute_plan, true);
}

QueryRows WriteExecutor::ExecuteDelete(
    const ir::LogicalPlan &plan,
    const std::vector<const ast::Expression *> &expressions,
    const QueryRows &input, const WritePlanExecutor &execute_plan,
    bool detach) const {
  Storage &storage = WritableStorage();
  QueryRows rows = ExecuteSource(plan, input, execute_plan);
  std::set<std::int64_t> node_ids;
  std::set<std::int64_t> relationship_ids;
  for (const auto &row : rows) {
    for (const ast::Expression *expression : expressions) {
      CHECK(expression != nullptr, common::InvalidArgumentError,
            "DELETE expression is null");
      const Value value = EvaluateExpression(*expression, row, {}, context_);
      if (value.IsNull()) {
        continue;
      }
      const EntityRef entity = MakeEntityRef(value);
      if (entity.kind == EntityKind::kNode) {
        node_ids.insert(entity.id);
      } else {
        relationship_ids.insert(entity.id);
      }
    }
  }

  if (!detach) {
    for (std::int64_t node_id : node_ids) {
      for (const auto &relationship :
           storage.RelationshipsConnectedTo(node_id)) {
        CHECK(relationship_ids.contains(relationship->id),
              common::InvalidArgumentError,
              "DELETE node still has relationships");
      }
    }
  }
  for (std::int64_t relationship_id : relationship_ids) {
    storage.DeleteRelationship(relationship_id);
  }
  for (std::int64_t node_id : node_ids) {
    if (detach) {
      for (const auto &relationship :
           storage.RelationshipsConnectedTo(node_id)) {
        storage.DeleteRelationship(relationship->id);
      }
    }
    storage.DeleteNode(node_id);
  }
  return rows;
}

Storage &WriteExecutor::WritableStorage() const {
  CHECK(storage_ != nullptr, common::InvalidArgumentError,
        "write execution requires storage");
  return *storage_;
}

}  // namespace rg
