#include "storage/in_memory_graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>

#include "ast/builtin_procedure.h"
#include "common/exception.h"

namespace rg {
namespace {

template <typename EntityPtr>
class PointerVectorEntityIdCursor final : public EntityIdCursor {
 public:
  using EntityVector = std::vector<EntityPtr>;
  using Predicate = std::function<bool(const EntityPtr &)>;

  explicit PointerVectorEntityIdCursor(
      std::vector<const EntityVector *> sources, Predicate predicate = {},
      bool deduplicate = false)
      : sources_(std::move(sources)),
        predicate_(std::move(predicate)),
        deduplicate_(deduplicate) {}

  ~PointerVectorEntityIdCursor() override { Close(); }

  [[nodiscard]] bool Next() override {
    while (!closed_ && source_index_ < sources_.size()) {
      const EntityVector *source = sources_[source_index_];
      if (source == nullptr || item_index_ >= source->size()) {
        ++source_index_;
        item_index_ = 0;
        continue;
      }
      const EntityPtr &entity = (*source)[item_index_++];
      if (entity == nullptr || (predicate_ && !predicate_(entity)) ||
          (deduplicate_ && !seen_.insert(entity->id).second)) {
        continue;
      }
      current_ = entity->id;
      positioned_ = true;
      return true;
    }
    Close();
    return false;
  }

  [[nodiscard]] std::int64_t Id() const override {
    CHECK(positioned_ && !closed_, common::InvalidArgumentError,
          "entity cursor is not positioned");
    return current_;
  }

  void Close() noexcept override {
    closed_ = true;
    sources_.clear();
    seen_.clear();
  }

 private:
  std::vector<const EntityVector *> sources_;
  Predicate predicate_;
  std::unordered_set<std::int64_t> seen_;
  std::size_t source_index_ = 0;
  std::size_t item_index_ = 0;
  std::int64_t current_ = -1;
  bool deduplicate_ = false;
  bool positioned_ = false;
  bool closed_ = false;
};

template <typename EntityPtr>
std::unique_ptr<EntityIdCursor> MakePointerCursor(
    std::vector<const std::vector<EntityPtr> *> sources,
    std::function<bool(const EntityPtr &)> predicate = {},
    bool deduplicate = false) {
  return std::make_unique<PointerVectorEntityIdCursor<EntityPtr>>(
      std::move(sources), std::move(predicate), deduplicate);
}

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

ValueType IndexValueGroup(const Value &value) {
  return value.IsDouble() ? ValueType::kInteger : value.Type();
}

bool IsOrderedIndexValue(const Value &value) {
  return value.IsInteger() || value.IsDouble() || value.IsString() ||
         value.IsBool();
}

bool IsInvalidBound(const Value &value) {
  return value.IsNull() || (value.IsDouble() && std::isnan(value.AsDouble()));
}

bool MatchesRange(const Value &value, const IndexRange &range) {
  if (IsInvalidBound(value)) {
    return false;
  }
  if (range.prefix.has_value() &&
      (!value.IsString() || !range.prefix->IsString() ||
       !value.AsString().starts_with(range.prefix->AsString()))) {
    return false;
  }
  for (const auto &bound : range.lower_bounds) {
    if (IsInvalidBound(bound.value) ||
        IndexValueGroup(value) != IndexValueGroup(bound.value) ||
        !(ValueLess(bound.value, value) ||
          (bound.inclusive && ValuesEqual(value, bound.value)))) {
      return false;
    }
  }
  for (const auto &bound : range.upper_bounds) {
    if (IsInvalidBound(bound.value) ||
        IndexValueGroup(value) != IndexValueGroup(bound.value) ||
        !(ValueLess(value, bound.value) ||
          (bound.inclusive && ValuesEqual(value, bound.value)))) {
      return false;
    }
  }
  return true;
}

template <typename EntityPtr, typename Groups>
std::vector<const std::vector<EntityPtr> *> RangeSources(
    const Groups &groups, const IndexRange &range) {
  const Value *sample = range.prefix.has_value() ? &*range.prefix : nullptr;
  if (sample == nullptr && !range.lower_bounds.empty()) {
    sample = &range.lower_bounds.front().value;
  }
  if (sample == nullptr && !range.upper_bounds.empty()) {
    sample = &range.upper_bounds.front().value;
  }
  CHECK(sample != nullptr, common::InvalidArgumentError,
        "index range has no bounds");
  if (IsInvalidBound(*sample) ||
      (range.prefix.has_value() && !range.prefix->IsString())) {
    return {};
  }
  const auto group = groups.find(IndexValueGroup(*sample));
  if (group == groups.end()) {
    return {};
  }
  const auto &buckets = group->second;
  auto first = buckets.begin();
  auto last = buckets.end();
  const auto less = buckets.key_comp();
  for (const auto &bound : range.lower_bounds) {
    if (IsInvalidBound(bound.value) ||
        IndexValueGroup(bound.value) != group->first) {
      return {};
    }
    const auto next = bound.inclusive || !IsOrderedIndexValue(bound.value)
                          ? buckets.lower_bound(bound.value)
                          : buckets.upper_bound(bound.value);
    if (next == buckets.end()) {
      return {};
    }
    if (first != buckets.end() && less(first->first, next->first)) {
      first = next;
    }
  }
  for (const auto &bound : range.upper_bounds) {
    if (IsInvalidBound(bound.value) ||
        IndexValueGroup(bound.value) != group->first) {
      return {};
    }
    const auto next = bound.inclusive || !IsOrderedIndexValue(bound.value)
                          ? buckets.upper_bound(bound.value)
                          : buckets.lower_bound(bound.value);
    if (next != buckets.end() &&
        (last == buckets.end() || less(next->first, last->first))) {
      last = next;
    }
  }
  if (range.prefix.has_value()) {
    const auto next = buckets.lower_bound(*range.prefix);
    if (next == buckets.end()) {
      return {};
    }
    if (first != buckets.end() && less(first->first, next->first)) {
      first = next;
    }
  }
  if (first == buckets.end() ||
      (last != buckets.end() && !less(first->first, last->first))) {
    return {};
  }
  std::vector<const std::vector<EntityPtr> *> sources;
  for (auto it = first; it != last; ++it) {
    if (range.prefix.has_value() &&
        !it->first.AsString().starts_with(range.prefix->AsString())) {
      break;
    }
    if (!IsOrderedIndexValue(it->first) || MatchesRange(it->first, range)) {
      sources.push_back(&it->second);
    }
  }
  return sources;
}

template <typename Groups, typename EntityPtr>
void RemoveRangeEntry(Groups *groups, const Value &value,
                      const EntityPtr &entity) {
  const auto group = groups->find(IndexValueGroup(value));
  if (group == groups->end()) {
    return;
  }
  auto bucket = group->second.find(value);
  if (bucket != group->second.end()) {
    std::erase(bucket->second, entity);
    if (bucket->second.empty()) {
      group->second.erase(bucket);
    }
  }
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
    graph_->node_range_index_buckets_.clear();
    graph_->relationship_range_index_buckets_.clear();
    graph_->nodes_by_label_.clear();
    graph_->relationships_by_type_.clear();
    graph_->outgoing_relationships_.clear();
    graph_->incoming_relationships_.clear();
    for (const auto &relationship : graph_->relationships_) {
      graph_->AddRelationshipToAdjacency(relationship);
      graph_->relationships_by_type_[relationship->type].push_back(
          relationship);
    }
    for (const auto &node : graph_->nodes_) {
      graph_->AddNodeToLabels(node);
      graph_->AddNodeToIndexes(node);
    }
    for (const auto &relationship : graph_->relationships_) {
      graph_->AddRelationshipToIndexes(relationship);
    }
    finished_ = true;
  }

 private:
  struct NodeState {
    MutableNodePtr node;
    std::vector<std::string> labels;
    Value::Map properties;
  };
  struct RelationshipState {
    MutableRelationshipPtr relationship;
    int64_t start_node_id = 0;
    int64_t end_node_id = 0;
    std::string type;
    Value::Map properties;
  };

  InMemoryGraph *graph_ = nullptr;
  bool finished_ = false;
  int64_t next_node_id_ = 0;
  int64_t next_relationship_id_ = 0;
  std::vector<MutableNodePtr> nodes_;
  std::vector<MutableRelationshipPtr> relationships_;
  std::vector<NodeState> node_states_;
  std::vector<RelationshipState> relationship_states_;
  std::unordered_map<IndexKey, IndexDescriptor, IndexKeyHash> node_indexes_;
  std::unordered_map<IndexKey, IndexDescriptor, IndexKeyHash>
      relationship_indexes_;
};

InMemoryGraph::~InMemoryGraph() = default;

bool InMemoryGraph::IndexValueLess::operator()(const Value &left,
                                               const Value &right) const {
  // Composite values use a type bucket: their query equality need not agree
  // with their ordering. The executor rechecks the original predicates.
  if (!IsOrderedIndexValue(left)) {
    return false;
  }
  const bool left_nan = left.IsDouble() && std::isnan(left.AsDouble());
  const bool right_nan = right.IsDouble() && std::isnan(right.AsDouble());
  if (left_nan || right_nan) {
    return !left_nan && right_nan;
  }
  return ValueLess(left, right);
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::ScanNodeIds() const {
  return MakePointerCursor<MutableNodePtr>({&nodes_});
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::ScanRelationshipIds() const {
  return MakePointerCursor<MutableRelationshipPtr>({&relationships_});
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::ScanNodeIdsByLabels(
    const std::vector<std::string> &labels) const {
  if (labels.empty()) {
    return ScanNodeIds();
  }
  const std::vector<MutableNodePtr> *smallest = nullptr;
  for (const auto &label : labels) {
    const auto found = nodes_by_label_.find(label);
    if (found == nodes_by_label_.end()) {
      return MakePointerCursor<MutableNodePtr>({});
    }
    if (smallest == nullptr || found->second.size() < smallest->size()) {
      smallest = &found->second;
    }
  }
  return MakePointerCursor<MutableNodePtr>(
      {smallest}, [labels](const MutableNodePtr &node) {
        return NodeHasLabels(*node, labels);
      });
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::ScanRelationshipIdsByTypes(
    const std::vector<std::string> &types) const {
  if (types.empty()) {
    return ScanRelationshipIds();
  }
  std::vector<const std::vector<MutableRelationshipPtr> *> sources;
  std::unordered_set<std::string> seen;
  for (const auto &type : types) {
    const auto found = relationships_by_type_.find(type);
    if (found != relationships_by_type_.end() && seen.insert(type).second) {
      sources.push_back(&found->second);
    }
  }
  return MakePointerCursor<MutableRelationshipPtr>(std::move(sources));
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::RelationshipIdsConnectedTo(
    int64_t node_id) const {
  std::vector<const std::vector<MutableRelationshipPtr> *> sources;
  const auto outgoing = outgoing_relationships_.find(node_id);
  if (outgoing != outgoing_relationships_.end()) {
    sources.push_back(&outgoing->second);
  }
  const auto incoming = incoming_relationships_.find(node_id);
  if (incoming != incoming_relationships_.end()) {
    sources.push_back(&incoming->second);
  }
  return MakePointerCursor<MutableRelationshipPtr>(std::move(sources), {},
                                                   true);
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::OutgoingRelationshipIds(
    int64_t node_id) const {
  const auto found = outgoing_relationships_.find(node_id);
  return MakePointerCursor<MutableRelationshipPtr>(
      found == outgoing_relationships_.end()
          ? std::vector<const std::vector<MutableRelationshipPtr> *>{}
          : std::vector<const std::vector<MutableRelationshipPtr> *>{
                &found->second});
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::IncomingRelationshipIds(
    int64_t node_id) const {
  const auto found = incoming_relationships_.find(node_id);
  return MakePointerCursor<MutableRelationshipPtr>(
      found == incoming_relationships_.end()
          ? std::vector<const std::vector<MutableRelationshipPtr> *>{}
          : std::vector<const std::vector<MutableRelationshipPtr> *>{
                &found->second});
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::FindNodeIdsByIndex(
    const std::vector<std::string> &labels, std::string_view property_key,
    const Value &value) const {
  const IndexKey index_key = MakeIndexKey(labels, property_key);
  const auto indexes = node_indexes_.find(index_key);
  const auto buckets = node_index_buckets_.find(index_key);
  if (indexes == node_indexes_.end() || buckets == node_index_buckets_.end()) {
    return MakePointerCursor<MutableNodePtr>({});
  }
  const auto bucket = buckets->second.find(value);
  if (bucket == buckets->second.end()) {
    return MakePointerCursor<MutableNodePtr>({});
  }
  const IndexDescriptor descriptor = indexes->second;
  return MakePointerCursor<MutableNodePtr>(
      {&bucket->second}, [this, descriptor, value](const MutableNodePtr &node) {
        if (!HasNode(node->id) ||
            !NodeHasLabels(*node, descriptor.qualifiers)) {
          return false;
        }
        const auto property = node->properties.find(descriptor.property_key);
        return property != node->properties.end() &&
               ValuesEqual(property->second, value);
      });
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::FindNodeIdsByIndexRange(
    const std::vector<std::string> &labels, std::string_view property_key,
    const IndexRange &range) const {
  const IndexKey index_key = MakeIndexKey(labels, property_key);
  const auto indexes = node_indexes_.find(index_key);
  const auto buckets = node_range_index_buckets_.find(index_key);
  if (indexes == node_indexes_.end() ||
      buckets == node_range_index_buckets_.end()) {
    return MakePointerCursor<MutableNodePtr>({});
  }
  const IndexDescriptor descriptor = indexes->second;
  return MakePointerCursor<MutableNodePtr>(
      RangeSources<MutableNodePtr>(buckets->second, range),
      [this, descriptor, range](const MutableNodePtr &node) {
        return HasNode(node->id) &&
               NodeHasLabels(*node, descriptor.qualifiers) &&
               node->properties.contains(descriptor.property_key) &&
               MatchesRange(node->properties.at(descriptor.property_key),
                            range);
      },
      true);
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::FindRelationshipIdsByIndex(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, const Value &value) const {
  const IndexKey index_key = MakeIndexKey(relationship_types, property_key);
  const auto indexes = relationship_indexes_.find(index_key);
  const auto buckets = relationship_index_buckets_.find(index_key);
  if (indexes == relationship_indexes_.end() ||
      buckets == relationship_index_buckets_.end()) {
    return MakePointerCursor<MutableRelationshipPtr>({});
  }
  const auto bucket = buckets->second.find(value);
  if (bucket == buckets->second.end()) {
    return MakePointerCursor<MutableRelationshipPtr>({});
  }
  const IndexDescriptor descriptor = indexes->second;
  return MakePointerCursor<MutableRelationshipPtr>(
      {&bucket->second},
      [this, descriptor, value](const MutableRelationshipPtr &relationship) {
        if (!HasRelationship(relationship->id) ||
            !RelationshipHasAnyType(*relationship, descriptor.qualifiers)) {
          return false;
        }
        const auto property =
            relationship->properties.find(descriptor.property_key);
        return property != relationship->properties.end() &&
               ValuesEqual(property->second, value);
      });
}

std::unique_ptr<EntityIdCursor> InMemoryGraph::FindRelationshipIdsByIndexRange(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, const IndexRange &range) const {
  const IndexKey index_key = MakeIndexKey(relationship_types, property_key);
  const auto indexes = relationship_indexes_.find(index_key);
  const auto buckets = relationship_range_index_buckets_.find(index_key);
  if (indexes == relationship_indexes_.end() ||
      buckets == relationship_range_index_buckets_.end()) {
    return MakePointerCursor<MutableRelationshipPtr>({});
  }
  const IndexDescriptor descriptor = indexes->second;
  return MakePointerCursor<MutableRelationshipPtr>(
      RangeSources<MutableRelationshipPtr>(buckets->second, range),
      [this, descriptor, range](const MutableRelationshipPtr &relationship) {
        return HasRelationship(relationship->id) &&
               RelationshipHasAnyType(*relationship, descriptor.qualifiers) &&
               relationship->properties.contains(descriptor.property_key) &&
               MatchesRange(
                   relationship->properties.at(descriptor.property_key), range);
      },
      true);
}

std::unique_ptr<StorageTransaction> InMemoryGraph::BeginTransaction() {
  return std::make_unique<Transaction>(this);
}

InMemoryGraph::NodePtr InMemoryGraph::CreateNode(
    std::vector<std::string> labels, Value::Map properties) {
  auto node = std::make_shared<Node>();
  node->id = next_node_id_;
  node->labels = std::move(labels);
  ApplyPropertyMap(std::move(properties), false, &node->properties);
  ValidateNodeUniqueIndexes(*node);
  ++next_node_id_;
  nodes_by_id_.emplace(node->id, node);
  nodes_.push_back(node);
  AddNodeToLabels(node);
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
  relationship->id = next_relationship_id_;
  relationship->start_node_id = start_node_id;
  relationship->end_node_id = end_node_id;
  relationship->type = std::move(type);
  ApplyPropertyMap(std::move(properties), false, &relationship->properties);
  ValidateRelationshipUniqueIndexes(*relationship);
  ++next_relationship_id_;
  relationships_by_id_.emplace(relationship->id, relationship);
  relationships_.push_back(relationship);
  relationships_by_type_[relationship->type].push_back(relationship);
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

void InMemoryGraph::SetNodeProperty(const MutableNodePtr &node,
                                    std::string property_key, Value value) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  Node updated = *node;
  if (value.IsNull()) {
    updated.properties.erase(property_key);
  } else {
    updated.properties.insert_or_assign(property_key, value);
  }
  ValidateNodeUniqueIndexes(updated);
  RemoveNodeFromIndexes(node);
  node->properties = std::move(updated.properties);
  AddNodeToIndexes(node);
}

void InMemoryGraph::SetRelationshipProperty(
    const MutableRelationshipPtr &relationship, std::string property_key,
    Value value) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  Relationship updated = *relationship;
  if (value.IsNull()) {
    updated.properties.erase(property_key);
  } else {
    updated.properties.insert_or_assign(property_key, value);
  }
  ValidateRelationshipUniqueIndexes(updated);
  RemoveRelationshipFromIndexes(relationship);
  relationship->properties = std::move(updated.properties);
  AddRelationshipToIndexes(relationship);
}

void InMemoryGraph::SetNodeProperties(const MutableNodePtr &node,
                                      Value::Map properties,
                                      bool include_existing) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  Node updated = *node;
  ApplyPropertyMap(std::move(properties), include_existing,
                   &updated.properties);
  ValidateNodeUniqueIndexes(updated);
  RemoveNodeFromIndexes(node);
  node->properties = std::move(updated.properties);
  AddNodeToIndexes(node);
}

void InMemoryGraph::SetRelationshipProperties(
    const MutableRelationshipPtr &relationship, Value::Map properties,
    bool include_existing) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  Relationship updated = *relationship;
  ApplyPropertyMap(std::move(properties), include_existing,
                   &updated.properties);
  ValidateRelationshipUniqueIndexes(updated);
  RemoveRelationshipFromIndexes(relationship);
  relationship->properties = std::move(updated.properties);
  AddRelationshipToIndexes(relationship);
}

void InMemoryGraph::SetLabels(const MutableNodePtr &node,
                              std::vector<std::string> labels) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  Node updated = *node;
  for (const auto &label : labels) {
    if (!ContainsString(updated.labels, label)) {
      updated.labels.push_back(label);
    }
  }
  std::sort(updated.labels.begin(), updated.labels.end());
  ValidateNodeUniqueIndexes(updated);
  RemoveNodeFromIndexes(node);
  RemoveNodeFromLabels(node);
  node->labels = std::move(updated.labels);
  AddNodeToLabels(node);
  AddNodeToIndexes(node);
}

void InMemoryGraph::RemoveNodeProperty(const MutableNodePtr &node,
                                       std::string_view property_key) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  Node updated = *node;
  updated.properties.erase(std::string(property_key));
  ValidateNodeUniqueIndexes(updated);
  RemoveNodeFromIndexes(node);
  node->properties = std::move(updated.properties);
  AddNodeToIndexes(node);
}

void InMemoryGraph::RemoveRelationshipProperty(
    const MutableRelationshipPtr &relationship, std::string_view property_key) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  Relationship updated = *relationship;
  updated.properties.erase(std::string(property_key));
  ValidateRelationshipUniqueIndexes(updated);
  RemoveRelationshipFromIndexes(relationship);
  relationship->properties = std::move(updated.properties);
  AddRelationshipToIndexes(relationship);
}

void InMemoryGraph::RemoveLabels(const MutableNodePtr &node,
                                 const std::vector<std::string> &labels) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  Node updated = *node;
  updated.labels.erase(
      std::remove_if(updated.labels.begin(), updated.labels.end(),
                     [&labels](const std::string &label) {
                       return ContainsString(labels, label);
                     }),
      updated.labels.end());
  ValidateNodeUniqueIndexes(updated);
  RemoveNodeFromIndexes(node);
  RemoveNodeFromLabels(node);
  node->labels = std::move(updated.labels);
  AddNodeToLabels(node);
  AddNodeToIndexes(node);
}

void InMemoryGraph::DeleteNode(const MutableNodePtr &node) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  const auto outgoing = outgoing_relationships_.find(node->id);
  const auto incoming = incoming_relationships_.find(node->id);
  CHECK(
      (outgoing == outgoing_relationships_.end() || outgoing->second.empty()) &&
          (incoming == incoming_relationships_.end() ||
           incoming->second.empty()),
      common::InvalidArgumentError, "node still has relationships");
  RemoveNodeFromIndexes(node);
  RemoveNodeFromLabels(node);
  nodes_by_id_.erase(node->id);
  nodes_.erase(std::remove(nodes_.begin(), nodes_.end(), node), nodes_.end());
}

void InMemoryGraph::DeleteRelationship(
    const MutableRelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  RemoveRelationshipFromIndexes(relationship);
  RemovePointer(&relationships_by_type_.at(relationship->type), relationship);
  RemoveRelationshipFromAdjacency(relationship);
  relationships_by_id_.erase(relationship->id);
  relationships_.erase(
      std::remove(relationships_.begin(), relationships_.end(), relationship),
      relationships_.end());
}

void InMemoryGraph::SetNodeProperty(int64_t node_id, std::string property_key,
                                    Value value) {
  SetNodeProperty(MutableNodeById(node_id), std::move(property_key),
                  std::move(value));
}

void InMemoryGraph::SetRelationshipProperty(int64_t relationship_id,
                                            std::string property_key,
                                            Value value) {
  SetRelationshipProperty(MutableRelationshipById(relationship_id),
                          std::move(property_key), std::move(value));
}

void InMemoryGraph::SetNodeProperties(int64_t node_id, Value::Map properties,
                                      bool include_existing) {
  SetNodeProperties(MutableNodeById(node_id), std::move(properties),
                    include_existing);
}

void InMemoryGraph::SetRelationshipProperties(int64_t relationship_id,
                                              Value::Map properties,
                                              bool include_existing) {
  SetRelationshipProperties(MutableRelationshipById(relationship_id),
                            std::move(properties), include_existing);
}

void InMemoryGraph::SetLabels(int64_t node_id,
                              std::vector<std::string> labels) {
  SetLabels(MutableNodeById(node_id), std::move(labels));
}

void InMemoryGraph::RemoveNodeProperty(int64_t node_id,
                                       std::string_view property_key) {
  RemoveNodeProperty(MutableNodeById(node_id), property_key);
}

void InMemoryGraph::RemoveRelationshipProperty(int64_t relationship_id,
                                               std::string_view property_key) {
  RemoveRelationshipProperty(MutableRelationshipById(relationship_id),
                             property_key);
}

void InMemoryGraph::RemoveLabels(int64_t node_id,
                                 const std::vector<std::string> &labels) {
  RemoveLabels(MutableNodeById(node_id), labels);
}

void InMemoryGraph::DeleteNode(int64_t node_id) {
  DeleteNode(MutableNodeById(node_id));
}

void InMemoryGraph::DeleteRelationship(int64_t relationship_id) {
  DeleteRelationship(MutableRelationshipById(relationship_id));
}

bool InMemoryGraph::HasRelationship(int64_t id) const noexcept {
  return relationships_by_id_.find(id) != relationships_by_id_.end();
}

const InMemoryGraph::MutableRelationshipPtr &
InMemoryGraph::MutableRelationshipById(int64_t id) const {
  const auto found = relationships_by_id_.find(id);
  CHECK(found != relationships_by_id_.end(), common::NotFoundError,
        "relationship does not exist");
  return found->second;
}

InMemoryGraph::RelationshipPtr InMemoryGraph::RelationshipById(
    int64_t id) const {
  return MutableRelationshipById(id);
}

Value InMemoryGraph::NodeProperty(int64_t node_id,
                                  std::string_view property_key) const {
  const auto &properties = MutableNodeById(node_id)->properties;
  const auto found = properties.find(std::string(property_key));
  return found == properties.end() ? Value::Null() : found->second;
}

Value InMemoryGraph::RelationshipProperty(int64_t relationship_id,
                                          std::string_view property_key) const {
  const auto &properties = MutableRelationshipById(relationship_id)->properties;
  const auto found = properties.find(std::string(property_key));
  return found == properties.end() ? Value::Null() : found->second;
}

void InMemoryGraph::AddNodeIndex(std::vector<std::string> labels,
                                 std::string_view property_key, bool unique) {
  CHECK(!property_key.empty(), common::InvalidArgumentError,
        "node index property key is empty");
  IndexKey key = MakeIndexKey(std::move(labels), property_key);
  IndexDescriptor descriptor{.qualifiers = key.qualifiers,
                             .property_key = key.property_key,
                             .unique = unique};
  if (unique) {
    std::unordered_set<Value, ValueHash, ValueEqual> values;
    for (const auto &node : nodes_) {
      if (!NodeHasLabels(*node, descriptor.qualifiers)) {
        continue;
      }
      const auto property = node->properties.find(descriptor.property_key);
      if (property != node->properties.end()) {
        CHECK(values.insert(property->second).second,
              common::InvalidArgumentError,
              "duplicate value for unique node index");
      }
    }
  }
  node_indexes_[key] = descriptor;
  node_index_buckets_[key].clear();
  node_range_index_buckets_[key].clear();
  for (const auto &node : nodes_) {
    AddNodeToIndex(key, descriptor, node);
  }
}

void InMemoryGraph::AddRelationshipIndex(
    std::vector<std::string> relationship_types, std::string_view property_key,
    bool unique) {
  CHECK(!property_key.empty(), common::InvalidArgumentError,
        "relationship index property key is empty");
  IndexKey key = MakeIndexKey(std::move(relationship_types), property_key);
  IndexDescriptor descriptor{.qualifiers = key.qualifiers,
                             .property_key = key.property_key,
                             .unique = unique};
  if (unique) {
    std::unordered_set<Value, ValueHash, ValueEqual> values;
    for (const auto &relationship : relationships_) {
      if (!RelationshipHasAnyType(*relationship, descriptor.qualifiers)) {
        continue;
      }
      const auto property =
          relationship->properties.find(descriptor.property_key);
      if (property != relationship->properties.end()) {
        CHECK(values.insert(property->second).second,
              common::InvalidArgumentError,
              "duplicate value for unique relationship index");
      }
    }
  }
  relationship_indexes_[key] = descriptor;
  relationship_index_buckets_[key].clear();
  relationship_range_index_buckets_[key].clear();
  for (const auto &relationship : relationships_) {
    AddRelationshipToIndex(key, descriptor, relationship);
  }
}

const InMemoryGraph::MutableNodePtr &InMemoryGraph::MutableNodeById(
    int64_t id) const {
  const auto found = nodes_by_id_.find(id);
  CHECK(found != nodes_by_id_.end(), common::NotFoundError,
        "node does not exist");
  return found->second;
}

InMemoryGraph::NodePtr InMemoryGraph::NodeById(int64_t id) const {
  return MutableNodeById(id);
}

std::vector<InMemoryGraph::NodePtr> InMemoryGraph::Nodes() const {
  return {nodes_.begin(), nodes_.end()};
}

std::vector<InMemoryGraph::RelationshipPtr> InMemoryGraph::Relationships()
    const {
  return {relationships_.begin(), relationships_.end()};
}

bool InMemoryGraph::HasNode(int64_t id) const noexcept {
  return nodes_by_id_.find(id) != nodes_by_id_.end();
}

std::optional<ir::NodeIndexDescriptor> InMemoryGraph::FindNodeIndex(
    const std::vector<std::string> &labels,
    std::string_view property_key) const {
  const auto found = node_indexes_.find(MakeIndexKey(labels, property_key));
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
  const auto found = relationship_indexes_.find(
      MakeIndexKey(relationship_types, property_key));
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

std::size_t InMemoryGraph::IndexKeyHash::operator()(
    const IndexKey &key) const noexcept {
  std::size_t seed = std::hash<std::string>{}(key.property_key);
  for (const auto &qualifier : key.qualifiers) {
    const std::size_t value = std::hash<std::string>{}(qualifier);
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
  }
  return seed;
}

InMemoryGraph::IndexKey InMemoryGraph::MakeIndexKey(
    std::vector<std::string> qualifiers, std::string_view property_key) {
  std::sort(qualifiers.begin(), qualifiers.end());
  qualifiers.erase(std::unique(qualifiers.begin(), qualifiers.end()),
                   qualifiers.end());
  return {.qualifiers = std::move(qualifiers),
          .property_key = std::string(property_key)};
}

void InMemoryGraph::ValidateNodeUniqueIndexes(const Node &node) const {
  for (const auto &[index_key, descriptor] : node_indexes_) {
    if (!descriptor.unique || !NodeHasLabels(node, descriptor.qualifiers)) {
      continue;
    }
    const auto property = node.properties.find(descriptor.property_key);
    if (property == node.properties.end()) {
      continue;
    }
    const auto indexes = node_index_buckets_.find(index_key);
    if (indexes == node_index_buckets_.end()) {
      continue;
    }
    const auto bucket = indexes->second.find(property->second);
    if (bucket == indexes->second.end()) {
      continue;
    }
    for (const auto &existing : bucket->second) {
      CHECK(existing == nullptr || existing->id == node.id ||
                !HasNode(existing->id),
            common::InvalidArgumentError,
            "duplicate value for unique node index");
    }
  }
}

void InMemoryGraph::ValidateRelationshipUniqueIndexes(
    const Relationship &relationship) const {
  for (const auto &[index_key, descriptor] : relationship_indexes_) {
    if (!descriptor.unique ||
        !RelationshipHasAnyType(relationship, descriptor.qualifiers)) {
      continue;
    }
    const auto property = relationship.properties.find(descriptor.property_key);
    if (property == relationship.properties.end()) {
      continue;
    }
    const auto indexes = relationship_index_buckets_.find(index_key);
    if (indexes == relationship_index_buckets_.end()) {
      continue;
    }
    const auto bucket = indexes->second.find(property->second);
    if (bucket == indexes->second.end()) {
      continue;
    }
    for (const auto &existing : bucket->second) {
      CHECK(existing == nullptr || existing->id == relationship.id ||
                !HasRelationship(existing->id),
            common::InvalidArgumentError,
            "duplicate value for unique relationship index");
    }
  }
}

void InMemoryGraph::AddNodeToIndexes(const MutableNodePtr &node) {
  for (const auto &[index_key, descriptor] : node_indexes_) {
    AddNodeToIndex(index_key, descriptor, node);
  }
}

void InMemoryGraph::RemoveNodeFromIndexes(const MutableNodePtr &node) {
  for (const auto &[index_key, descriptor] : node_indexes_) {
    RemoveNodeFromIndex(index_key, descriptor, node);
  }
}

void InMemoryGraph::AddRelationshipToIndexes(
    const MutableRelationshipPtr &relationship) {
  for (const auto &[index_key, descriptor] : relationship_indexes_) {
    AddRelationshipToIndex(index_key, descriptor, relationship);
  }
}

void InMemoryGraph::RemoveRelationshipFromIndexes(
    const MutableRelationshipPtr &relationship) {
  for (const auto &[index_key, descriptor] : relationship_indexes_) {
    RemoveRelationshipFromIndex(index_key, descriptor, relationship);
  }
}

void InMemoryGraph::AddNodeToIndex(const IndexKey &index_key,
                                   const IndexDescriptor &descriptor,
                                   const MutableNodePtr &node) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  if (!NodeHasLabels(*node, descriptor.qualifiers)) {
    return;
  }
  const auto property = node->properties.find(descriptor.property_key);
  if (property == node->properties.end()) {
    return;
  }
  auto &bucket = node_index_buckets_[index_key][property->second];
  if (descriptor.unique) {
    for (const auto &existing : bucket) {
      CHECK(existing == nullptr || existing->id == node->id,
            common::InternalError, "unique node index invariant violated");
    }
  }
  bucket.push_back(node);
  node_range_index_buckets_[index_key][IndexValueGroup(property->second)]
                           [property->second]
                               .push_back(node);
}

void InMemoryGraph::RemoveNodeFromIndex(const IndexKey &index_key,
                                        const IndexDescriptor &descriptor,
                                        const MutableNodePtr &node) {
  CHECK(node != nullptr, common::InvalidArgumentError, "node is null");
  if (!NodeHasLabels(*node, descriptor.qualifiers)) {
    return;
  }
  const auto property = node->properties.find(descriptor.property_key);
  if (property == node->properties.end()) {
    return;
  }
  RemoveRangeEntry(&node_range_index_buckets_[index_key], property->second,
                   node);
  const auto buckets = node_index_buckets_.find(index_key);
  if (buckets == node_index_buckets_.end()) {
    return;
  }
  auto bucket = buckets->second.find(property->second);
  if (bucket == buckets->second.end()) {
    return;
  }
  RemovePointer(&bucket->second, node);
  if (bucket->second.empty()) {
    buckets->second.erase(bucket);
  }
}

void InMemoryGraph::AddRelationshipToIndex(
    const IndexKey &index_key, const IndexDescriptor &descriptor,
    const MutableRelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  if (!RelationshipHasAnyType(*relationship, descriptor.qualifiers)) {
    return;
  }
  const auto property = relationship->properties.find(descriptor.property_key);
  if (property == relationship->properties.end()) {
    return;
  }
  auto &bucket = relationship_index_buckets_[index_key][property->second];
  if (descriptor.unique) {
    for (const auto &existing : bucket) {
      CHECK(existing == nullptr || existing->id == relationship->id,
            common::InternalError,
            "unique relationship index invariant violated");
    }
  }
  bucket.push_back(relationship);
  relationship_range_index_buckets_[index_key][IndexValueGroup(
      property->second)][property->second]
      .push_back(relationship);
}

void InMemoryGraph::RemoveRelationshipFromIndex(
    const IndexKey &index_key, const IndexDescriptor &descriptor,
    const MutableRelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  if (!RelationshipHasAnyType(*relationship, descriptor.qualifiers)) {
    return;
  }
  const auto property = relationship->properties.find(descriptor.property_key);
  if (property == relationship->properties.end()) {
    return;
  }
  RemoveRangeEntry(&relationship_range_index_buckets_[index_key],
                   property->second, relationship);
  const auto buckets = relationship_index_buckets_.find(index_key);
  if (buckets == relationship_index_buckets_.end()) {
    return;
  }
  auto bucket = buckets->second.find(property->second);
  if (bucket == buckets->second.end()) {
    return;
  }
  RemovePointer(&bucket->second, relationship);
  if (bucket->second.empty()) {
    buckets->second.erase(bucket);
  }
}

void InMemoryGraph::AddNodeToLabels(const MutableNodePtr &node) {
  std::unordered_set<std::string> seen;
  for (const auto &label : node->labels) {
    if (seen.insert(label).second) {
      nodes_by_label_[label].push_back(node);
    }
  }
}

void InMemoryGraph::RemoveNodeFromLabels(const MutableNodePtr &node) {
  for (const auto &label : node->labels) {
    const auto found = nodes_by_label_.find(label);
    if (found != nodes_by_label_.end()) {
      RemovePointer(&found->second, node);
    }
  }
}

void InMemoryGraph::AddRelationshipToAdjacency(
    const MutableRelationshipPtr &relationship) {
  CHECK(relationship != nullptr, common::InvalidArgumentError,
        "relationship is null");
  outgoing_relationships_[relationship->start_node_id].push_back(relationship);
  incoming_relationships_[relationship->end_node_id].push_back(relationship);
}

void InMemoryGraph::RemoveRelationshipFromAdjacency(
    const MutableRelationshipPtr &relationship) {
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
  std::unordered_set<Value, ValueHash, ValueEqual> distinct_values;
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
    distinct_values.insert(property->second);
  }
  distribution.distinct_values = static_cast<double>(distinct_values.size());
  return distribution;
}

InMemoryGraph::PropertyDistribution
InMemoryGraph::RelationshipPropertyDistribution(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  PropertyDistribution distribution;
  std::unordered_set<Value, ValueHash, ValueEqual> distinct_values;
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
    distinct_values.insert(property->second);
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

}  // namespace rg
