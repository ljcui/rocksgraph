#include "storage/in_memory_graph.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>

#include "ast/builtin_procedure.h"
#include "common/exception.h"

namespace rg {
namespace {

bool ContainsString(const std::vector<std::string> &items,
                    std::string_view value) {
  return std::find(items.begin(), items.end(), value) != items.end();
}

bool NodeHasLabelSet(const Node &node,
                     const std::unordered_set<std::string> &labels) {
  for (const auto &label : labels) {
    if (!ContainsString(node.labels, label)) {
      return false;
    }
  }
  return true;
}

std::string LowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

double ClampSelectivity(double value) { return std::clamp(value, 0.0, 1.0); }

template <typename Ptr>
void RemovePointer(std::vector<Ptr> *items, const Ptr &ptr) {
  items->erase(std::remove(items->begin(), items->end(), ptr), items->end());
}

void ApplyPropertyMap(Value::Map properties, bool include_existing,
                      Value::Map *target) {
  CHECK(target != nullptr, common::InternalError, "property map is null");
  if (!include_existing) {
    target->clear();
  }
  for (auto &[key, value] : properties) {
    if (value.IsNull()) {
      target->erase(key);
    } else {
      target->insert_or_assign(std::move(key), std::move(value));
    }
  }
}

}  // namespace

class InMemoryGraph::Transaction final : public StorageTransaction {
 public:
  explicit Transaction(InMemoryGraph *graph) : graph_(graph) {
    CHECK(graph_ != nullptr, common::InternalError,
          "transaction graph is null");

    next_node_id_ = graph_->next_node_id_;
    next_relationship_id_ = graph_->next_relationship_id_;
    nodes_ = graph_->nodes_;
    relationships_ = graph_->relationships_;
    node_indexes_ = graph_->node_indexes_;
    relationship_indexes_ = graph_->relationship_indexes_;

    node_states_.reserve(nodes_.size());
    for (const auto &node : nodes_) {
      CHECK(node != nullptr, common::InternalError,
            "transaction contains null node");
      node_states_.push_back({.node = node,
                              .labels = node->labels,
                              .properties = node->properties});
    }
    relationship_states_.reserve(relationships_.size());
    for (const auto &relationship : relationships_) {
      CHECK(relationship != nullptr, common::InternalError,
            "transaction contains null relationship");
      relationship_states_.push_back(
          {.relationship = relationship,
           .start_node_id = relationship->start_node_id,
           .end_node_id = relationship->end_node_id,
           .type = relationship->type,
           .properties = relationship->properties});
    }
  }

  void Commit() override { finished_ = true; }

  void Rollback() override {
    if (finished_) {
      return;
    }
    CHECK(graph_ != nullptr, common::InternalError,
          "transaction graph is null");

    graph_->next_node_id_ = next_node_id_;
    graph_->next_relationship_id_ = next_relationship_id_;
    graph_->nodes_ = nodes_;
    graph_->relationships_ = relationships_;
    graph_->node_indexes_ = node_indexes_;
    graph_->relationship_indexes_ = relationship_indexes_;

    graph_->nodes_by_id_.clear();
    for (const auto &state : node_states_) {
      state.node->labels = state.labels;
      state.node->properties = state.properties;
      graph_->nodes_by_id_.emplace(state.node->id, state.node);
    }
    graph_->relationships_by_id_.clear();
    for (const auto &state : relationship_states_) {
      state.relationship->start_node_id = state.start_node_id;
      state.relationship->end_node_id = state.end_node_id;
      state.relationship->type = state.type;
      state.relationship->properties = state.properties;
      graph_->relationships_by_id_.emplace(state.relationship->id,
                                           state.relationship);
    }

    graph_->node_index_buckets_.clear();
    graph_->relationship_index_buckets_.clear();
    graph_->outgoing_relationships_.clear();
    graph_->incoming_relationships_.clear();
    for (const auto &relationship : graph_->relationships_) {
      graph_->AddRelationshipToAdjacency(relationship);
    }
    for (const auto &node : graph_->nodes_) {
      graph_->AddNodeToIndexes(node);
    }
    for (const auto &relationship : graph_->relationships_) {
      graph_->AddRelationshipToIndexes(relationship);
    }
    finished_ = true;
  }

 private:
  struct NodeState {
    NodePtr node;
    std::vector<std::string> labels;
    Value::Map properties;
  };
  struct RelationshipState {
    RelationshipPtr relationship;
    int64_t start_node_id = 0;
    int64_t end_node_id = 0;
    std::string type;
    Value::Map properties;
  };

  InMemoryGraph *graph_ = nullptr;
  bool finished_ = false;
  int64_t next_node_id_ = 0;
  int64_t next_relationship_id_ = 0;
  std::vector<NodePtr> nodes_;
  std::vector<RelationshipPtr> relationships_;
  std::vector<NodeState> node_states_;
  std::vector<RelationshipState> relationship_states_;
  std::unordered_map<std::string, IndexDescriptor> node_indexes_;
  std::unordered_map<std::string, IndexDescriptor> relationship_indexes_;
};

InMemoryGraph::~InMemoryGraph() = default;

std::unique_ptr<StorageTransaction> InMemoryGraph::BeginTransaction() {
  return std::make_unique<Transaction>(this);
}

InMemoryGraph::NodePtr InMemoryGraph::CreateNode(
    std::vector<std::string> labels, Value::Map properties) {
  auto node = std::make_shared<Node>();
  node->id = next_node_id_++;
  node->labels = std::move(labels);
  ApplyPropertyMap(std::move(properties), false, &node->properties);
  nodes_by_id_.emplace(node->id, node);
  nodes_.push_back(node);
  AddNodeToIndexes(node);
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
  ApplyPropertyMap(std::move(properties), false, &relationship->properties);
  relationships_by_id_.emplace(relationship->id, relationship);
  relationships_.push_back(relationship);
  AddRelationshipToAdjacency(relationship);
  AddRelationshipToIndexes(relationship);
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
  RemoveNodeFromIndexes(node);
  if (value.IsNull()) {
    node->properties.erase(property_key);
  } else {
    node->properties.insert_or_assign(std::move(property_key),
                                      std::move(value));
  }
  AddNodeToIndexes(node);
}

void InMemoryGraph::SetRelationshipProperty(const RelationshipPtr &relationship,
                                            std::string property_key,
                                            Value value) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  RemoveRelationshipFromIndexes(relationship);
  if (value.IsNull()) {
    relationship->properties.erase(property_key);
  } else {
    relationship->properties.insert_or_assign(std::move(property_key),
                                              std::move(value));
  }
  AddRelationshipToIndexes(relationship);
}

void InMemoryGraph::SetNodeProperties(const NodePtr &node,
                                      Value::Map properties,
                                      bool include_existing) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  RemoveNodeFromIndexes(node);
  ApplyPropertyMap(std::move(properties), include_existing, &node->properties);
  AddNodeToIndexes(node);
}

void InMemoryGraph::SetRelationshipProperties(
    const RelationshipPtr &relationship, Value::Map properties,
    bool include_existing) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  RemoveRelationshipFromIndexes(relationship);
  ApplyPropertyMap(std::move(properties), include_existing,
                   &relationship->properties);
  AddRelationshipToIndexes(relationship);
}

void InMemoryGraph::SetLabels(const NodePtr &node,
                              std::vector<std::string> labels) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  RemoveNodeFromIndexes(node);
  for (const auto &label : labels) {
    if (std::find(node->labels.begin(), node->labels.end(), label) ==
        node->labels.end()) {
      node->labels.push_back(label);
    }
  }
  std::sort(node->labels.begin(), node->labels.end());
  AddNodeToIndexes(node);
}

void InMemoryGraph::RemoveNodeProperty(const NodePtr &node,
                                       std::string_view property_key) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  RemoveNodeFromIndexes(node);
  node->properties.erase(std::string(property_key));
  AddNodeToIndexes(node);
}

void InMemoryGraph::RemoveRelationshipProperty(
    const RelationshipPtr &relationship, std::string_view property_key) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  RemoveRelationshipFromIndexes(relationship);
  relationship->properties.erase(std::string(property_key));
  AddRelationshipToIndexes(relationship);
}

void InMemoryGraph::RemoveLabels(const NodePtr &node,
                                 const std::vector<std::string> &labels) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  RemoveNodeFromIndexes(node);
  node->labels.erase(std::remove_if(node->labels.begin(), node->labels.end(),
                                    [&labels](const std::string &label) {
                                      return ContainsString(labels, label);
                                    }),
                     node->labels.end());
  AddNodeToIndexes(node);
}

void InMemoryGraph::DeleteNode(const NodePtr &node) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  RemoveNodeFromIndexes(node);
  nodes_by_id_.erase(node->id);
  nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
}

void InMemoryGraph::DeleteRelationship(const RelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  RemoveRelationshipFromIndexes(relationship);
  RemoveRelationshipFromAdjacency(relationship);
  relationships_by_id_.erase(relationship->id);
  relationships_.erase(
      std::remove(relationships_.begin(), relationships_.end(), relationship),
      relationships_.end());
}

void InMemoryGraph::SetNodeProperty(int64_t node_id, std::string property_key,
                                    Value value) {
  SetNodeProperty(NodeById(node_id), std::move(property_key), std::move(value));
}

void InMemoryGraph::SetRelationshipProperty(int64_t relationship_id,
                                            std::string property_key,
                                            Value value) {
  SetRelationshipProperty(RelationshipById(relationship_id),
                          std::move(property_key), std::move(value));
}

void InMemoryGraph::SetNodeProperties(int64_t node_id, Value::Map properties,
                                      bool include_existing) {
  SetNodeProperties(NodeById(node_id), std::move(properties), include_existing);
}

void InMemoryGraph::SetRelationshipProperties(int64_t relationship_id,
                                              Value::Map properties,
                                              bool include_existing) {
  SetRelationshipProperties(RelationshipById(relationship_id),
                            std::move(properties), include_existing);
}

void InMemoryGraph::SetLabels(int64_t node_id,
                              std::vector<std::string> labels) {
  SetLabels(NodeById(node_id), std::move(labels));
}

void InMemoryGraph::RemoveNodeProperty(int64_t node_id,
                                       std::string_view property_key) {
  RemoveNodeProperty(NodeById(node_id), property_key);
}

void InMemoryGraph::RemoveRelationshipProperty(int64_t relationship_id,
                                               std::string_view property_key) {
  RemoveRelationshipProperty(RelationshipById(relationship_id), property_key);
}

void InMemoryGraph::RemoveLabels(int64_t node_id,
                                 const std::vector<std::string> &labels) {
  RemoveLabels(NodeById(node_id), labels);
}

void InMemoryGraph::DeleteNode(int64_t node_id) {
  DeleteNode(NodeById(node_id));
}

void InMemoryGraph::DeleteRelationship(int64_t relationship_id) {
  DeleteRelationship(RelationshipById(relationship_id));
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
  std::unordered_set<int64_t> seen;
  const auto append = [&](const std::vector<RelationshipPtr> &relationships) {
    for (const auto &relationship : relationships) {
      if (relationship != nullptr && seen.insert(relationship->id).second) {
        result.push_back(relationship);
      }
    }
  };
  const auto outgoing = outgoing_relationships_.find(node_id);
  if (outgoing != outgoing_relationships_.end()) {
    append(outgoing->second);
  }
  const auto incoming = incoming_relationships_.find(node_id);
  if (incoming != incoming_relationships_.end()) {
    append(incoming->second);
  }
  return result;
}

std::vector<InMemoryGraph::RelationshipPtr>
InMemoryGraph::OutgoingRelationships(int64_t node_id) const {
  const auto found = outgoing_relationships_.find(node_id);
  return found == outgoing_relationships_.end() ? std::vector<RelationshipPtr>{}
                                                : found->second;
}

std::vector<InMemoryGraph::RelationshipPtr>
InMemoryGraph::IncomingRelationships(int64_t node_id) const {
  const auto found = incoming_relationships_.find(node_id);
  return found == incoming_relationships_.end() ? std::vector<RelationshipPtr>{}
                                                : found->second;
}

void InMemoryGraph::AddNodeIndex(std::vector<std::string> labels,
                                 std::string_view property_key, bool unique) {
  CHECK(!property_key.empty(), common::InvalidArgumentError,
        "node index property key is empty");
  std::sort(labels.begin(), labels.end());
  IndexDescriptor descriptor{.qualifiers = std::move(labels),
                             .property_key = std::string(property_key),
                             .unique = unique};
  const std::string key = IndexKey(descriptor.qualifiers, property_key);
  node_indexes_[key] = descriptor;
  node_index_buckets_[key].clear();
  for (const auto &node : nodes_) {
    AddNodeToIndex(key, descriptor, node);
  }
}

void InMemoryGraph::AddRelationshipIndex(
    std::vector<std::string> relationship_types, std::string_view property_key,
    bool unique) {
  CHECK(!property_key.empty(), common::InvalidArgumentError,
        "relationship index property key is empty");
  std::sort(relationship_types.begin(), relationship_types.end());
  IndexDescriptor descriptor{.qualifiers = std::move(relationship_types),
                             .property_key = std::string(property_key),
                             .unique = unique};
  const std::string key = IndexKey(descriptor.qualifiers, property_key);
  relationship_indexes_[key] = descriptor;
  relationship_index_buckets_[key].clear();
  for (const auto &relationship : relationships_) {
    AddRelationshipToIndex(key, descriptor, relationship);
  }
}

std::vector<InMemoryGraph::NodePtr> InMemoryGraph::FindNodesByIndex(
    const std::vector<std::string> &labels, std::string_view property_key,
    const Value &value) const {
  std::vector<NodePtr> result;
  const std::string index_key = IndexKey(labels, property_key);
  const auto index_found = node_indexes_.find(index_key);
  if (index_found == node_indexes_.end()) {
    return result;
  }
  const auto buckets_found = node_index_buckets_.find(index_key);
  if (buckets_found == node_index_buckets_.end()) {
    return result;
  }
  const auto bucket_found = buckets_found->second.find(ValueIndexKey(value));
  if (bucket_found == buckets_found->second.end()) {
    return result;
  }
  for (const auto &node : bucket_found->second) {
    if (node == nullptr || !HasNode(node->id) ||
        !NodeHasLabels(*node, index_found->second.qualifiers)) {
      continue;
    }
    const auto property =
        node->properties.find(index_found->second.property_key);
    if (property != node->properties.end() &&
        ValuesEqual(property->second, value)) {
      result.push_back(node);
    }
  }
  return result;
}

std::vector<InMemoryGraph::NodePtr> InMemoryGraph::NodesInIndex(
    const std::vector<std::string> &labels,
    std::string_view property_key) const {
  std::vector<NodePtr> result;
  const std::string index_key = IndexKey(labels, property_key);
  const auto index_found = node_indexes_.find(index_key);
  if (index_found == node_indexes_.end()) {
    return result;
  }
  const auto buckets_found = node_index_buckets_.find(index_key);
  if (buckets_found == node_index_buckets_.end()) {
    return result;
  }
  std::unordered_set<int64_t> seen;
  for (const auto &[value_key, nodes] : buckets_found->second) {
    (void)value_key;
    for (const auto &node : nodes) {
      if (node == nullptr || !HasNode(node->id) ||
          !seen.insert(node->id).second ||
          !NodeHasLabels(*node, index_found->second.qualifiers)) {
        continue;
      }
      if (node->properties.find(index_found->second.property_key) !=
          node->properties.end()) {
        result.push_back(node);
      }
    }
  }
  return result;
}

std::vector<InMemoryGraph::RelationshipPtr>
InMemoryGraph::FindRelationshipsByIndex(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, const Value &value) const {
  std::vector<RelationshipPtr> result;
  const std::string index_key = IndexKey(relationship_types, property_key);
  const auto index_found = relationship_indexes_.find(index_key);
  if (index_found == relationship_indexes_.end()) {
    return result;
  }
  const auto buckets_found = relationship_index_buckets_.find(index_key);
  if (buckets_found == relationship_index_buckets_.end()) {
    return result;
  }
  const auto bucket_found = buckets_found->second.find(ValueIndexKey(value));
  if (bucket_found == buckets_found->second.end()) {
    return result;
  }
  for (const auto &relationship : bucket_found->second) {
    if (relationship == nullptr || !HasRelationship(relationship->id) ||
        !RelationshipHasAnyType(*relationship,
                                index_found->second.qualifiers)) {
      continue;
    }
    const auto property =
        relationship->properties.find(index_found->second.property_key);
    if (property != relationship->properties.end() &&
        ValuesEqual(property->second, value)) {
      result.push_back(relationship);
    }
  }
  return result;
}

std::vector<InMemoryGraph::RelationshipPtr> InMemoryGraph::RelationshipsInIndex(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  std::vector<RelationshipPtr> result;
  const std::string index_key = IndexKey(relationship_types, property_key);
  const auto index_found = relationship_indexes_.find(index_key);
  if (index_found == relationship_indexes_.end()) {
    return result;
  }
  const auto buckets_found = relationship_index_buckets_.find(index_key);
  if (buckets_found == relationship_index_buckets_.end()) {
    return result;
  }
  std::unordered_set<int64_t> seen;
  for (const auto &[value_key, relationships] : buckets_found->second) {
    (void)value_key;
    for (const auto &relationship : relationships) {
      if (relationship == nullptr || !HasRelationship(relationship->id) ||
          !seen.insert(relationship->id).second ||
          !RelationshipHasAnyType(*relationship,
                                  index_found->second.qualifiers)) {
        continue;
      }
      if (relationship->properties.find(index_found->second.property_key) !=
          relationship->properties.end()) {
        result.push_back(relationship);
      }
    }
  }
  return result;
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
                                 .unique = found->second.unique};
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
      .property_key = std::string(property_key),
      .unique = found->second.unique};
}

double InMemoryGraph::EstimateNodeCount(
    const std::unordered_set<std::string> &labels) const {
  double count = 0.0;
  for (const auto &node : nodes_) {
    if (node != nullptr && NodeHasLabelSet(*node, labels)) {
      count += 1.0;
    }
  }
  return count;
}

double InMemoryGraph::EstimateExpandFanout(
    const std::vector<std::string> &relationship_types) const {
  const auto node_count = static_cast<double>(nodes_.size());
  if (node_count <= 0.0) {
    return 0.0;
  }
  return EstimateRelationshipCount(relationship_types) / node_count;
}

double InMemoryGraph::EstimateExpandIntoSelectivity(
    const std::vector<std::string> &relationship_types) const {
  const auto node_count = static_cast<double>(nodes_.size());
  if (node_count <= 0.0) {
    return 0.0;
  }
  return ClampSelectivity(EstimateRelationshipCount(relationship_types) /
                          (node_count * node_count));
}

double InMemoryGraph::EstimateFilterSelectivity() const { return 0.1; }

double InMemoryGraph::EstimateNodeHashJoinSelectivity(
    std::size_t key_count) const {
  return key_count == 0 ? 1.0 : 1.0 / static_cast<double>(key_count);
}

double InMemoryGraph::EstimateNodeIndexSeekSelectivity(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key) const {
  return EqualitySelectivity(NodePropertyDistribution(labels, property_key));
}

double InMemoryGraph::EstimateNodeIndexRangeSeekSelectivity(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key, std::size_t bound_count) const {
  return RangeSelectivity(NodePropertyDistribution(labels, property_key),
                          bound_count);
}

double InMemoryGraph::EstimateRelationshipCount(
    const std::vector<std::string> &relationship_types) const {
  double count = 0.0;
  for (const auto &relationship : relationships_) {
    if (relationship != nullptr &&
        RelationshipHasAnyType(*relationship, relationship_types)) {
      count += 1.0;
    }
  }
  return count;
}

double InMemoryGraph::EstimateRelationshipIndexSeekSelectivity(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  return EqualitySelectivity(
      RelationshipPropertyDistribution(relationship_types, property_key));
}

double InMemoryGraph::EstimateRelationshipIndexRangeSeekSelectivity(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, std::size_t bound_count) const {
  return RangeSelectivity(
      RelationshipPropertyDistribution(relationship_types, property_key),
      bound_count);
}

double InMemoryGraph::EstimateProcedureRows(std::string_view procedure_name,
                                            std::size_t yield_count) const {
  const std::string normalized = LowerAscii(std::string(procedure_name));
  if (normalized == "db.labels") {
    std::unordered_set<std::string> labels;
    for (const auto &node : nodes_) {
      if (node == nullptr) {
        continue;
      }
      for (const auto &label : node->labels) {
        labels.insert(label);
      }
    }
    return static_cast<double>(labels.size());
  }
  if (normalized == "db.relationshiptypes") {
    std::unordered_set<std::string> types;
    for (const auto &relationship : relationships_) {
      if (relationship != nullptr && !relationship->type.empty()) {
        types.insert(relationship->type);
      }
    }
    return static_cast<double>(types.size());
  }
  if (normalized == "db.propertykeys") {
    std::unordered_set<std::string> keys;
    for (const auto &node : nodes_) {
      if (node == nullptr) {
        continue;
      }
      for (const auto &[key, value] : node->properties) {
        (void)value;
        keys.insert(key);
      }
    }
    for (const auto &relationship : relationships_) {
      if (relationship == nullptr) {
        continue;
      }
      for (const auto &[key, value] : relationship->properties) {
        (void)value;
        keys.insert(key);
      }
    }
    return static_cast<double>(keys.size());
  }
  if (normalized == "dbms.procedures") {
    return static_cast<double>(ast::BuiltinProcedures().size());
  }
  return ir::PlannerStatistics::EstimateProcedureRows(procedure_name,
                                                      yield_count);
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

std::string InMemoryGraph::ValueIndexKey(const Value &value) {
  return ValueKey(value);
}

void InMemoryGraph::AddNodeToIndexes(const NodePtr &node) {
  for (const auto &[index_key, descriptor] : node_indexes_) {
    AddNodeToIndex(index_key, descriptor, node);
  }
}

void InMemoryGraph::RemoveNodeFromIndexes(const NodePtr &node) {
  for (const auto &[index_key, descriptor] : node_indexes_) {
    RemoveNodeFromIndex(index_key, descriptor, node);
  }
}

void InMemoryGraph::AddRelationshipToIndexes(
    const RelationshipPtr &relationship) {
  for (const auto &[index_key, descriptor] : relationship_indexes_) {
    AddRelationshipToIndex(index_key, descriptor, relationship);
  }
}

void InMemoryGraph::RemoveRelationshipFromIndexes(
    const RelationshipPtr &relationship) {
  for (const auto &[index_key, descriptor] : relationship_indexes_) {
    RemoveRelationshipFromIndex(index_key, descriptor, relationship);
  }
}

void InMemoryGraph::AddNodeToIndex(const std::string &index_key,
                                   const IndexDescriptor &descriptor,
                                   const NodePtr &node) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  if (!NodeHasLabels(*node, descriptor.qualifiers)) {
    return;
  }
  const auto property = node->properties.find(descriptor.property_key);
  if (property == node->properties.end()) {
    return;
  }
  node_index_buckets_[index_key][ValueIndexKey(property->second)].push_back(
      node);
}

void InMemoryGraph::RemoveNodeFromIndex(const std::string &index_key,
                                        const IndexDescriptor &descriptor,
                                        const NodePtr &node) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  if (!NodeHasLabels(*node, descriptor.qualifiers)) {
    return;
  }
  const auto property = node->properties.find(descriptor.property_key);
  if (property == node->properties.end()) {
    return;
  }
  const auto buckets = node_index_buckets_.find(index_key);
  if (buckets == node_index_buckets_.end()) {
    return;
  }
  auto bucket = buckets->second.find(ValueIndexKey(property->second));
  if (bucket == buckets->second.end()) {
    return;
  }
  RemovePointer(&bucket->second, node);
  if (bucket->second.empty()) {
    buckets->second.erase(bucket);
  }
}

void InMemoryGraph::AddRelationshipToIndex(
    const std::string &index_key, const IndexDescriptor &descriptor,
    const RelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  if (!RelationshipHasAnyType(*relationship, descriptor.qualifiers)) {
    return;
  }
  const auto property = relationship->properties.find(descriptor.property_key);
  if (property == relationship->properties.end()) {
    return;
  }
  relationship_index_buckets_[index_key][ValueIndexKey(property->second)]
      .push_back(relationship);
}

void InMemoryGraph::RemoveRelationshipFromIndex(
    const std::string &index_key, const IndexDescriptor &descriptor,
    const RelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  if (!RelationshipHasAnyType(*relationship, descriptor.qualifiers)) {
    return;
  }
  const auto property = relationship->properties.find(descriptor.property_key);
  if (property == relationship->properties.end()) {
    return;
  }
  const auto buckets = relationship_index_buckets_.find(index_key);
  if (buckets == relationship_index_buckets_.end()) {
    return;
  }
  auto bucket = buckets->second.find(ValueIndexKey(property->second));
  if (bucket == buckets->second.end()) {
    return;
  }
  RemovePointer(&bucket->second, relationship);
  if (bucket->second.empty()) {
    buckets->second.erase(bucket);
  }
}

void InMemoryGraph::AddRelationshipToAdjacency(
    const RelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  outgoing_relationships_[relationship->start_node_id].push_back(relationship);
  incoming_relationships_[relationship->end_node_id].push_back(relationship);
}

void InMemoryGraph::RemoveRelationshipFromAdjacency(
    const RelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  const auto outgoing =
      outgoing_relationships_.find(relationship->start_node_id);
  if (outgoing != outgoing_relationships_.end()) {
    RemovePointer(&outgoing->second, relationship);
    if (outgoing->second.empty()) {
      outgoing_relationships_.erase(outgoing);
    }
  }
  const auto incoming = incoming_relationships_.find(relationship->end_node_id);
  if (incoming != incoming_relationships_.end()) {
    RemovePointer(&incoming->second, relationship);
    if (incoming->second.empty()) {
      incoming_relationships_.erase(incoming);
    }
  }
}

InMemoryGraph::PropertyDistribution InMemoryGraph::NodePropertyDistribution(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key) const {
  PropertyDistribution distribution;
  std::unordered_set<std::string> distinct_values;
  const std::string key(property_key);
  for (const auto &node : nodes_) {
    if (node == nullptr || !NodeHasLabelSet(*node, labels)) {
      continue;
    }
    distribution.total_entities += 1.0;
    const auto property = node->properties.find(key);
    if (property == node->properties.end()) {
      continue;
    }
    distribution.entities_with_property += 1.0;
    distinct_values.insert(ValueIndexKey(property->second));
  }
  distribution.distinct_values = static_cast<double>(distinct_values.size());
  return distribution;
}

InMemoryGraph::PropertyDistribution
InMemoryGraph::RelationshipPropertyDistribution(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  PropertyDistribution distribution;
  std::unordered_set<std::string> distinct_values;
  const std::string key(property_key);
  for (const auto &relationship : relationships_) {
    if (relationship == nullptr ||
        !RelationshipHasAnyType(*relationship, relationship_types)) {
      continue;
    }
    distribution.total_entities += 1.0;
    const auto property = relationship->properties.find(key);
    if (property == relationship->properties.end()) {
      continue;
    }
    distribution.entities_with_property += 1.0;
    distinct_values.insert(ValueIndexKey(property->second));
  }
  distribution.distinct_values = static_cast<double>(distinct_values.size());
  return distribution;
}

double InMemoryGraph::EqualitySelectivity(
    const PropertyDistribution &distribution) {
  if (distribution.total_entities <= 0.0) {
    return 1.0;
  }
  if (distribution.entities_with_property <= 0.0 ||
      distribution.distinct_values <= 0.0) {
    return 0.0;
  }
  return ClampSelectivity(distribution.entities_with_property /
                          distribution.total_entities /
                          distribution.distinct_values);
}

double InMemoryGraph::RangeSelectivity(const PropertyDistribution &distribution,
                                       std::size_t bound_count) {
  if (distribution.total_entities <= 0.0) {
    return 1.0;
  }
  if (distribution.entities_with_property <= 0.0) {
    return 0.0;
  }
  const double property_coverage =
      distribution.entities_with_property / distribution.total_entities;
  if (bound_count == 0) {
    return ClampSelectivity(property_coverage);
  }
  return ClampSelectivity(property_coverage * (bound_count > 1 ? 0.25 : 0.5));
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
