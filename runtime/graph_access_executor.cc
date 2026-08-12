#include "runtime/graph_access_executor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "common/exception.h"
#include "runtime/expression_evaluator.h"
#include "runtime/query_row_util.h"
#include "value/value.h"

namespace rg {
namespace {

bool NodeHasLabels(const Node &node, const std::vector<std::string> &labels) {
  for (const auto &label : labels) {
    if (std::find(node.labels.begin(), node.labels.end(), label) ==
        node.labels.end()) {
      return false;
    }
  }
  return true;
}

bool RelationshipHasAnyType(const Relationship &relationship,
                            const std::vector<std::string> &types) {
  return types.empty() || std::find(types.begin(), types.end(),
                                    relationship.type) != types.end();
}

}  // namespace

QueryRows GraphAccessExecutor::ExecuteAllNodeScan(
    const ir::AllNodeScanPlan &plan, const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    for (const auto &node : access_path_->ScanNodes()) {
      QueryRow next = row;
      if (TryBindQueryVariable(&next, plan.Variable(), Value(node))) {
        out.push_back(std::move(next));
      }
    }
  }
  return out;
}

QueryRows GraphAccessExecutor::ExecuteNodeByLabelScan(
    const ir::NodeByLabelScanPlan &plan, const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    for (const auto &node : access_path_->ScanNodes()) {
      if (!NodeHasLabels(*node, plan.Labels())) {
        continue;
      }
      QueryRow next = row;
      if (TryBindQueryVariable(&next, plan.Variable(), Value(node))) {
        out.push_back(std::move(next));
      }
    }
  }
  return out;
}

QueryRows GraphAccessExecutor::ExecuteNodeIndexSeek(
    const ir::NodeIndexSeekPlan &plan, const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    Value expected = EvaluateExpression(*plan.ValueExpression(), row);
    for (const auto &node : access_path_->FindNodesByIndex(
             plan.Labels(), plan.PropertyKey(), expected)) {
      QueryRow next = row;
      if (TryBindQueryVariable(&next, plan.Variable(), Value(node))) {
        out.push_back(std::move(next));
      }
    }
  }
  return out;
}

QueryRows GraphAccessExecutor::ExecuteNodeIndexRangeSeek(
    const ir::NodeIndexRangeSeekPlan &plan, const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    for (const auto &node :
         access_path_->NodesInIndex(plan.Labels(), plan.PropertyKey())) {
      QueryRow next = row;
      if (!TryBindQueryVariable(&next, plan.Variable(), Value(node))) {
        continue;
      }
      bool keep = true;
      for (const ast::Expression *predicate : plan.Predicates()) {
        CHECK(predicate != nullptr, common::InvalidArgumentError,
              "index range predicate is null");
        keep = keep && PredicateIsTrue(EvaluateExpression(*predicate, next));
      }
      if (keep) {
        out.push_back(std::move(next));
      }
    }
  }
  return out;
}

QueryRows GraphAccessExecutor::ExecuteRelationshipTypeScan(
    const ir::RelationshipTypeScanPlan &plan, const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    for (const auto &relationship : access_path_->ScanRelationships()) {
      if (!RelationshipHasAnyType(*relationship, plan.Types())) {
        continue;
      }
      AddRelationshipRow(row, *relationship, plan.FromNode(),
                         plan.Relationship(), plan.ToNode(), plan.Direction(),
                         &out);
    }
  }
  return out;
}

QueryRows GraphAccessExecutor::ExecuteRelationshipIndexSeek(
    const ir::RelationshipIndexSeekPlan &plan, const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    Value expected = EvaluateExpression(*plan.ValueExpression(), row);
    for (const auto &relationship : access_path_->FindRelationshipsByIndex(
             plan.Types(), plan.PropertyKey(), expected)) {
      AddRelationshipRow(row, *relationship, plan.FromNode(),
                         plan.Relationship(), plan.ToNode(), plan.Direction(),
                         &out);
    }
  }
  return out;
}

QueryRows GraphAccessExecutor::ExecuteRelationshipIndexRangeSeek(
    const ir::RelationshipIndexRangeSeekPlan &plan,
    const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    for (const auto &relationship :
         access_path_->RelationshipsInIndex(plan.Types(), plan.PropertyKey())) {
      QueryRows candidates;
      AddRelationshipRow(row, *relationship, plan.FromNode(),
                         plan.Relationship(), plan.ToNode(), plan.Direction(),
                         &candidates);
      for (QueryRow &candidate : candidates) {
        bool keep = true;
        for (const ast::Expression *predicate : plan.Predicates()) {
          CHECK(predicate != nullptr, common::InvalidArgumentError,
                "index range predicate is null");
          keep = keep &&
                 PredicateIsTrue(EvaluateExpression(*predicate, candidate));
        }
        if (keep) {
          out.push_back(std::move(candidate));
        }
      }
    }
  }
  return out;
}

void GraphAccessExecutor::AddRelationshipRow(
    const QueryRow &row, const Relationship &relationship,
    const std::string &from_node, const std::string &relationship_variable,
    const std::string &to_node, ir::ExpandDirection direction,
    QueryRows *out) const {
  CHECK(out != nullptr, common::InternalError, "row output is null");
  if (direction == ir::ExpandDirection::kBoth) {
    AddDirectedRelationshipRow(
        row, relationship, from_node, relationship_variable, to_node,
        relationship.start_node_id, relationship.end_node_id, out);
    if (relationship.start_node_id != relationship.end_node_id) {
      AddDirectedRelationshipRow(
          row, relationship, from_node, relationship_variable, to_node,
          relationship.end_node_id, relationship.start_node_id, out);
    }
    return;
  }
  const std::int64_t from_id = direction == ir::ExpandDirection::kOutgoing
                                   ? relationship.start_node_id
                                   : relationship.end_node_id;
  const std::int64_t to_id = direction == ir::ExpandDirection::kOutgoing
                                 ? relationship.end_node_id
                                 : relationship.start_node_id;
  AddDirectedRelationshipRow(row, relationship, from_node,
                             relationship_variable, to_node, from_id, to_id,
                             out);
}

void GraphAccessExecutor::AddDirectedRelationshipRow(
    const QueryRow &row, const Relationship &relationship,
    const std::string &from_node, const std::string &relationship_variable,
    const std::string &to_node, std::int64_t from_id, std::int64_t to_id,
    QueryRows *out) const {
  QueryRow next = row;
  if (!TryBindQueryVariable(&next, from_node,
                            Value(access_path_->NodeById(from_id)))) {
    return;
  }
  if (!TryBindQueryVariable(
          &next, relationship_variable,
          Value(access_path_->RelationshipById(relationship.id)))) {
    return;
  }
  if (!TryBindQueryVariable(&next, to_node,
                            Value(access_path_->NodeById(to_id)))) {
    return;
  }
  out->push_back(std::move(next));
}

QueryRows GraphAccessExecutor::ExecuteExpand(const ir::ExpandPlan &plan,
                                             const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    const Value &from = LookupQueryVariable(row, plan.FromNode());
    CHECK(from.IsNode(), common::InvalidArgumentError,
          "expand source is not a node: " + plan.FromNode());
    const std::int64_t from_id = from.AsNode().id;
    for (const auto &relationship :
         ExpandCandidates(from_id, plan.Direction())) {
      if (!RelationshipHasAnyType(*relationship, plan.Types())) {
        continue;
      }
      if (plan.Direction() == ir::ExpandDirection::kOutgoing &&
          relationship->start_node_id != from_id) {
        continue;
      }
      if (plan.Direction() == ir::ExpandDirection::kIncoming &&
          relationship->end_node_id != from_id) {
        continue;
      }
      if (plan.Direction() == ir::ExpandDirection::kBoth &&
          relationship->start_node_id != from_id &&
          relationship->end_node_id != from_id) {
        continue;
      }
      const std::int64_t to_id = relationship->start_node_id == from_id
                                     ? relationship->end_node_id
                                     : relationship->start_node_id;
      QueryRow next = row;
      if (!TryBindQueryVariable(
              &next, plan.Relationship(),
              Value(access_path_->RelationshipById(relationship->id)))) {
        continue;
      }
      if (!TryBindQueryVariable(&next, plan.ToNode(),
                                Value(access_path_->NodeById(to_id)))) {
        continue;
      }
      out.push_back(std::move(next));
    }
  }
  return out;
}

QueryRows GraphAccessExecutor::ExecuteExpandInto(const ir::ExpandIntoPlan &plan,
                                                 const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    const Value &from = LookupQueryVariable(row, plan.FromNode());
    const Value &to = LookupQueryVariable(row, plan.ToNode());
    CHECK(from.IsNode() && to.IsNode(), common::InvalidArgumentError,
          "expand-into endpoints must be nodes");
    for (const auto &relationship :
         ExpandCandidates(from.AsNode().id, plan.Direction())) {
      if (!RelationshipHasAnyType(*relationship, plan.Types())) {
        continue;
      }
      bool match = false;
      if (plan.Direction() == ir::ExpandDirection::kOutgoing) {
        match = relationship->start_node_id == from.AsNode().id &&
                relationship->end_node_id == to.AsNode().id;
      } else if (plan.Direction() == ir::ExpandDirection::kIncoming) {
        match = relationship->end_node_id == from.AsNode().id &&
                relationship->start_node_id == to.AsNode().id;
      } else {
        match = (relationship->start_node_id == from.AsNode().id &&
                 relationship->end_node_id == to.AsNode().id) ||
                (relationship->end_node_id == from.AsNode().id &&
                 relationship->start_node_id == to.AsNode().id);
      }
      if (!match) {
        continue;
      }
      QueryRow next = row;
      if (TryBindQueryVariable(
              &next, plan.Relationship(),
              Value(access_path_->RelationshipById(relationship->id)))) {
        out.push_back(std::move(next));
      }
    }
  }
  return out;
}

std::vector<AccessPath::RelationshipPtr> GraphAccessExecutor::ExpandCandidates(
    std::int64_t from_node_id, ir::ExpandDirection direction) const {
  if (direction == ir::ExpandDirection::kOutgoing) {
    return access_path_->OutgoingRelationships(from_node_id);
  }
  if (direction == ir::ExpandDirection::kIncoming) {
    return access_path_->IncomingRelationships(from_node_id);
  }
  return access_path_->RelationshipsConnectedTo(from_node_id);
}

QueryRows GraphAccessExecutor::ExecuteVarExpand(const ir::VarExpandPlan &plan,
                                                const QueryRows &input) const {
  QueryRows out;
  for (const auto &row : input) {
    const Value &from = LookupQueryVariable(row, plan.FromNode());
    CHECK(from.IsNode(), common::InvalidArgumentError,
          "variable expand source is not a node: " + plan.FromNode());

    std::optional<std::int64_t> bound_to_id;
    const auto to_found = row.find(plan.ToNode());
    if (to_found != row.end()) {
      CHECK(to_found->second.IsNode(), common::InvalidArgumentError,
            "variable expand target is not a node: " + plan.ToNode());
      bound_to_id = to_found->second.AsNode().id;
    }

    const std::size_t min_length = VarExpandMinLength(plan.Length());
    const std::size_t max_length = VarExpandMaxLength(plan.Length());
    if (max_length < min_length) {
      continue;
    }

    std::vector<AccessPath::RelationshipPtr> path;
    std::unordered_set<std::int64_t> used_relationships;
    ExpandVariableLengthPath(plan, row, from.AsNode().id, bound_to_id,
                             min_length, max_length, &path, &used_relationships,
                             &out);
  }
  return out;
}

std::size_t GraphAccessExecutor::VarExpandMinLength(
    const ir::LogicalVariableLength &length) const {
  if (!length.min.has_value()) {
    return 1;
  }
  CHECK(*length.min >= 0, common::InvalidArgumentError,
        "variable expand minimum length is negative");
  return static_cast<std::size_t>(*length.min);
}

std::size_t GraphAccessExecutor::VarExpandMaxLength(
    const ir::LogicalVariableLength &length) const {
  if (!length.max.has_value()) {
    return access_path_->RelationshipCount();
  }
  CHECK(*length.max >= 0, common::InvalidArgumentError,
        "variable expand maximum length is negative");
  return static_cast<std::size_t>(*length.max);
}

std::optional<std::int64_t> GraphAccessExecutor::NextVarExpandNode(
    const Relationship &relationship, std::int64_t current_node_id,
    ir::ExpandDirection direction) const {
  if (direction == ir::ExpandDirection::kOutgoing) {
    if (relationship.start_node_id != current_node_id) {
      return std::nullopt;
    }
    return relationship.end_node_id;
  }
  if (direction == ir::ExpandDirection::kIncoming) {
    if (relationship.end_node_id != current_node_id) {
      return std::nullopt;
    }
    return relationship.start_node_id;
  }
  if (relationship.start_node_id == current_node_id) {
    return relationship.end_node_id;
  }
  if (relationship.end_node_id == current_node_id) {
    return relationship.start_node_id;
  }
  return std::nullopt;
}

void GraphAccessExecutor::ExpandVariableLengthPath(
    const ir::VarExpandPlan &plan, const QueryRow &row,
    std::int64_t current_node_id, std::optional<std::int64_t> bound_to_id,
    std::size_t min_length, std::size_t max_length,
    std::vector<AccessPath::RelationshipPtr> *path,
    std::unordered_set<std::int64_t> *used_relationships,
    QueryRows *out) const {
  CHECK(path != nullptr, common::InternalError, "variable expand path is null");
  CHECK(used_relationships != nullptr, common::InternalError,
        "variable expand used relationship set is null");
  CHECK(out != nullptr, common::InternalError,
        "variable expand output is null");

  if (path->size() >= min_length &&
      (!bound_to_id.has_value() || *bound_to_id == current_node_id)) {
    EmitVarExpandRow(plan, row, current_node_id, *path, out);
  }
  if (path->size() == max_length) {
    return;
  }

  for (const auto &relationship :
       ExpandCandidates(current_node_id, plan.Direction())) {
    if (used_relationships->contains(relationship->id)) {
      continue;
    }
    if (!RelationshipHasAnyType(*relationship, plan.Types())) {
      continue;
    }
    std::optional<std::int64_t> next_node_id =
        NextVarExpandNode(*relationship, current_node_id, plan.Direction());
    if (!next_node_id.has_value()) {
      continue;
    }

    used_relationships->insert(relationship->id);
    path->push_back(relationship);
    ExpandVariableLengthPath(plan, row, *next_node_id, bound_to_id, min_length,
                             max_length, path, used_relationships, out);
    path->pop_back();
    used_relationships->erase(relationship->id);
  }
}

void GraphAccessExecutor::EmitVarExpandRow(
    const ir::VarExpandPlan &plan, const QueryRow &row,
    std::int64_t current_node_id,
    const std::vector<AccessPath::RelationshipPtr> &path,
    QueryRows *out) const {
  CHECK(out != nullptr, common::InternalError,
        "variable expand output is null");
  Value::List relationships;
  relationships.reserve(path.size());
  for (const auto &relationship : path) {
    CHECK(relationship != nullptr, common::InternalError,
          "variable expand relationship is null");
    relationships.emplace_back(relationship);
  }

  QueryRow next = row;
  if (!TryBindQueryVariable(&next, plan.Relationship(),
                            Value(std::move(relationships)))) {
    return;
  }
  if (!TryBindQueryVariable(&next, plan.ToNode(),
                            Value(access_path_->NodeById(current_node_id)))) {
    return;
  }
  out->push_back(std::move(next));
}

QueryRows GraphAccessExecutor::ExecutePathBuild(const ir::PathBuildPlan &plan,
                                                const QueryRows &input) const {
  QueryRows out;
  out.reserve(input.size());
  for (const auto &row : input) {
    QueryRow next = row;
    if (TryBindQueryVariable(&next, plan.PathVariable(),
                             BuildPathValue(plan.Path(), row))) {
      out.push_back(std::move(next));
    }
  }
  return out;
}

Value GraphAccessExecutor::BuildPathValue(const ir::PathPattern &pattern,
                                          const QueryRow &row) const {
  CHECK(!pattern.nodes.empty(), common::InvalidArgumentError,
        "path has no nodes: " + pattern.variable);
  CHECK(pattern.nodes.size() == pattern.relationships.size() + 1,
        common::InvalidArgumentError,
        "path node and relationship counts do not match: " + pattern.variable);

  auto path = std::make_shared<Path>();
  path->nodes.reserve(pattern.nodes.size());
  path->relationships.reserve(pattern.relationships.size());

  const Value &start_node = LookupQueryVariable(row, pattern.nodes.front());
  CHECK(start_node.IsNode(), common::InvalidArgumentError,
        "path node is not a node: " + pattern.nodes.front());
  std::int64_t current_node_id = start_node.AsNode().id;
  path->nodes.push_back(access_path_->NodeById(current_node_id));

  for (std::size_t index = 0; index < pattern.relationships.size(); ++index) {
    const std::string &relationship_variable = pattern.relationships[index];
    const Value &relationship = LookupQueryVariable(row, relationship_variable);
    std::vector<AccessPath::RelationshipPtr> relationships;
    if (relationship.IsList()) {
      relationships.reserve(relationship.AsList().size());
      for (const auto &item : relationship.AsList()) {
        CHECK(item.IsRelationship(), common::InvalidArgumentError,
              "path relationship list item is not a relationship: " +
                  relationship_variable);
        relationships.push_back(
            access_path_->RelationshipById(item.AsRelationship().id));
      }
    } else {
      CHECK(
          relationship.IsRelationship(), common::InvalidArgumentError,
          "path relationship is not a relationship: " + relationship_variable);
      relationships.push_back(
          access_path_->RelationshipById(relationship.AsRelationship().id));
    }

    const Value &target_node =
        LookupQueryVariable(row, pattern.nodes[index + 1]);
    CHECK(target_node.IsNode(), common::InvalidArgumentError,
          "path node is not a node: " + pattern.nodes[index + 1]);
    const std::int64_t target_node_id = target_node.AsNode().id;

    if (!CanTraverseRelationshipSequence(relationships, current_node_id,
                                         target_node_id)) {
      std::vector<AccessPath::RelationshipPtr> reversed(relationships.rbegin(),
                                                        relationships.rend());
      CHECK(CanTraverseRelationshipSequence(reversed, current_node_id,
                                            target_node_id),
            common::InvalidArgumentError,
            "path relationship sequence does not connect nodes: " +
                relationship_variable);
      relationships = std::move(reversed);
    }

    for (const auto &item : relationships) {
      AppendPathRelationship(*item, path.get(), &current_node_id);
    }
    CHECK(current_node_id == target_node_id, common::InvalidArgumentError,
          "path node does not match traversed relationship endpoint: " +
              pattern.nodes[index + 1]);
  }
  return Value(std::move(path));
}

bool GraphAccessExecutor::CanTraverseRelationshipSequence(
    const std::vector<AccessPath::RelationshipPtr> &relationships,
    std::int64_t start_node_id, std::int64_t target_node_id) const {
  std::int64_t current_node_id = start_node_id;
  for (const auto &relationship : relationships) {
    CHECK(relationship != nullptr, common::InternalError,
          "path relationship is null");
    if (relationship->start_node_id == current_node_id) {
      current_node_id = relationship->end_node_id;
    } else if (relationship->end_node_id == current_node_id) {
      current_node_id = relationship->start_node_id;
    } else {
      return false;
    }
  }
  return current_node_id == target_node_id;
}

void GraphAccessExecutor::AppendPathRelationship(
    const Relationship &relationship, Path *path,
    std::int64_t *current_node_id) const {
  CHECK(path != nullptr, common::InternalError, "path is null");
  CHECK(current_node_id != nullptr, common::InternalError,
        "path current node id is null");
  path->relationships.push_back(
      access_path_->RelationshipById(relationship.id));
  if (relationship.start_node_id == *current_node_id) {
    *current_node_id = relationship.end_node_id;
  } else if (relationship.end_node_id == *current_node_id) {
    *current_node_id = relationship.start_node_id;
  } else {
    THROW(common::InvalidArgumentError,
          "path relationship is not connected to current node");
  }
  path->nodes.push_back(access_path_->NodeById(*current_node_id));
}

}  // namespace rg
