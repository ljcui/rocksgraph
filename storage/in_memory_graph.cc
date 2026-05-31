#include "storage/in_memory_graph.h"

#include <algorithm>
#include <utility>

#include "common/exception.h"

namespace rg {
namespace {

bool ContainsString(const std::vector<std::string> &items,
                    std::string_view value) {
  return std::find(items.begin(), items.end(), value) != items.end();
}

}  // namespace

InMemoryGraph::NodePtr InMemoryGraph::CreateNode(
    std::vector<std::string> labels, Value::Map properties) {
  auto node = std::make_shared<Node>();
  node->id = next_node_id_++;
  node->labels = std::move(labels);
  node->properties = std::move(properties);
  nodes_by_id_.emplace(node->id, node);
  nodes_.push_back(node);
  return node;
}

InMemoryGraph::RelationshipPtr InMemoryGraph::CreateRelationship(
    int64_t start_node_id, int64_t end_node_id, std::string type,
    Value::Map properties) {
  CHECK(HasNode(start_node_id), common::InvalidArgumentError,
        "relationship start node does not exist");
  CHECK(HasNode(end_node_id), common::InvalidArgumentError,
        "relationship end node does not exist");

  auto relationship = std::make_shared<Relationship>();
  relationship->id = next_relationship_id_++;
  relationship->start_node_id = start_node_id;
  relationship->end_node_id = end_node_id;
  relationship->type = std::move(type);
  relationship->properties = std::move(properties);
  relationships_by_id_.emplace(relationship->id, relationship);
  relationships_.push_back(relationship);
  return relationship;
}

InMemoryGraph::RelationshipPtr InMemoryGraph::CreateRelationship(
    const NodePtr &start_node, const NodePtr &end_node, std::string type,
    Value::Map properties) {
  CHECK(start_node != nullptr, common::InvalidArgumentError,
        "relationship start node is null");
  CHECK(end_node != nullptr, common::InvalidArgumentError,
        "relationship end node is null");
  return CreateRelationship(start_node->id, end_node->id, std::move(type),
                            std::move(properties));
}

void InMemoryGraph::SetNodeProperty(const NodePtr &node,
                                    std::string property_key, Value value) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  node->properties[std::move(property_key)] = std::move(value);
}

void InMemoryGraph::SetRelationshipProperty(const RelationshipPtr &relationship,
                                            std::string property_key,
                                            Value value) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  relationship->properties[std::move(property_key)] = std::move(value);
}

void InMemoryGraph::SetNodeProperties(const NodePtr &node,
                                      Value::Map properties,
                                      bool include_existing) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  if (!include_existing) {
    node->properties.clear();
  }
  for (auto &entry : properties) {
    node->properties[std::move(entry.first)] = std::move(entry.second);
  }
}

void InMemoryGraph::SetLabels(const NodePtr &node,
                              std::vector<std::string> labels) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  for (const auto &label : labels) {
    if (std::find(node->labels.begin(), node->labels.end(), label) ==
        node->labels.end()) {
      node->labels.push_back(label);
    }
  }
  std::sort(node->labels.begin(), node->labels.end());
}

void InMemoryGraph::RemoveNodeProperty(const NodePtr &node,
                                       std::string_view property_key) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  node->properties.erase(std::string(property_key));
}

void InMemoryGraph::RemoveRelationshipProperty(
    const RelationshipPtr &relationship, std::string_view property_key) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  relationship->properties.erase(std::string(property_key));
}

void InMemoryGraph::RemoveLabels(const NodePtr &node,
                                 const std::vector<std::string> &labels) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  node->labels.erase(std::remove_if(node->labels.begin(), node->labels.end(),
                                    [&labels](const std::string &label) {
                                      return ContainsString(labels, label);
                                    }),
                     node->labels.end());
}

void InMemoryGraph::DeleteNode(const NodePtr &node) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  nodes_by_id_.erase(node->id);
  nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
}

void InMemoryGraph::DeleteRelationship(const RelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  relationships_by_id_.erase(relationship->id);
  relationships_.erase(
      std::remove(relationships_.begin(), relationships_.end(), relationship),
      relationships_.end());
}

bool InMemoryGraph::HasRelationship(int64_t id) const noexcept {
  return relationships_by_id_.find(id) != relationships_by_id_.end();
}

const InMemoryGraph::RelationshipPtr &InMemoryGraph::RelationshipById(
    int64_t id) const {
  const auto found = relationships_by_id_.find(id);
  CHECK(found != relationships_by_id_.end(), common::NotFoundError,
        "relationship does not exist");
  return found->second;
}

std::vector<InMemoryGraph::RelationshipPtr>
InMemoryGraph::RelationshipsConnectedTo(int64_t node_id) const {
  std::vector<RelationshipPtr> result;
  for (const auto &relationship : relationships_) {
    if (relationship->start_node_id == node_id ||
        relationship->end_node_id == node_id) {
      result.push_back(relationship);
    }
  }
  return result;
}

void InMemoryGraph::AddNodeIndex(std::vector<std::string> labels,
                                 std::string_view property_key, bool unique) {
  CHECK(!property_key.empty(), common::InvalidArgumentError,
        "node index property key is empty");
  node_indexes_[IndexKey(std::move(labels), property_key)] = unique;
}

void InMemoryGraph::AddRelationshipIndex(
    std::vector<std::string> relationship_types, std::string_view property_key,
    bool unique) {
  CHECK(!property_key.empty(), common::InvalidArgumentError,
        "relationship index property key is empty");
  relationship_indexes_[IndexKey(std::move(relationship_types), property_key)] =
      unique;
}

const InMemoryGraph::NodePtr &InMemoryGraph::NodeById(int64_t id) const {
  const auto found = nodes_by_id_.find(id);
  CHECK(found != nodes_by_id_.end(), common::NotFoundError,
        "node does not exist");
  return found->second;
}

bool InMemoryGraph::HasNode(int64_t id) const noexcept {
  return nodes_by_id_.find(id) != nodes_by_id_.end();
}

std::optional<ir::NodeIndexDescriptor> InMemoryGraph::FindNodeIndex(
    const std::vector<std::string> &labels,
    std::string_view property_key) const {
  const auto found = node_indexes_.find(IndexKey(labels, property_key));
  if (found == node_indexes_.end()) {
    return std::nullopt;
  }
  return ir::NodeIndexDescriptor{.property_key = std::string(property_key),
                                 .unique = found->second};
}

std::optional<ir::RelationshipIndexDescriptor>
InMemoryGraph::FindRelationshipIndex(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  const auto found =
      relationship_indexes_.find(IndexKey(relationship_types, property_key));
  if (found == relationship_indexes_.end()) {
    return std::nullopt;
  }
  return ir::RelationshipIndexDescriptor{
      .property_key = std::string(property_key), .unique = found->second};
}

std::string InMemoryGraph::IndexKey(std::vector<std::string> qualifiers,
                                    std::string_view property_key) {
  std::sort(qualifiers.begin(), qualifiers.end());
  std::string key(property_key);
  key.push_back('\n');
  for (const auto &qualifier : qualifiers) {
    key += qualifier;
    key.push_back('\n');
  }
  return key;
}

bool NodeHasLabels(const Node &node, const std::vector<std::string> &labels) {
  for (const auto &label : labels) {
    if (!ContainsString(node.labels, label)) {
      return false;
    }
  }
  return true;
}

bool RelationshipHasAnyType(const Relationship &relationship,
                            const std::vector<std::string> &types) {
  return types.empty() || ContainsString(types, relationship.type);
}

const Value *FindProperty(const Value &value, std::string_view property_key) {
  if (value.IsNode()) {
    const auto &properties = value.AsNode().properties;
    const auto found = properties.find(std::string(property_key));
    return found == properties.end() ? nullptr : &found->second;
  }
  if (value.IsRelationship()) {
    const auto &properties = value.AsRelationship().properties;
    const auto found = properties.find(std::string(property_key));
    return found == properties.end() ? nullptr : &found->second;
  }
  if (value.IsMap()) {
    const auto &properties = value.AsMap();
    const auto found = properties.find(std::string(property_key));
    return found == properties.end() ? nullptr : &found->second;
  }
  return nullptr;
}

Value::Map CopyProperties(const Value::Map &properties) { return properties; }

}  // namespace rg
