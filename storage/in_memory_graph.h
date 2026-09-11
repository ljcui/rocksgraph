#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "planner/catalog.h"
#include "storage/storage.h"
#include "value/value.h"

namespace rg {

class InMemoryGraph final : public Storage, public ir::PlannerCatalog {
 public:
  using NodePtr = Value::NodePtr;
  using RelationshipPtr = Value::RelationshipPtr;
  using Storage::CreateNode;
  using Storage::CreateRelationship;

  ~InMemoryGraph() override;

  [[nodiscard]] std::unique_ptr<StorageTransaction> BeginTransaction() override;

  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIds() const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIds()
      const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIdsByLabels(
      const std::vector<std::string> &labels) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIdsByTypes(
      const std::vector<std::string> &types) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> RelationshipIdsConnectedTo(
      int64_t node_id) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OutgoingRelationshipIds(
      int64_t node_id) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> IncomingRelationshipIds(
      int64_t node_id) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindNodeIdsByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindNodeIdsByIndexRange(
      const std::vector<std::string> &labels, std::string_view property_key,
      const IndexRange &range) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindRelationshipIdsByIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const Value &value) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> FindRelationshipIdsByIndexRange(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const IndexRange &range) const override;

  NodePtr CreateNode(std::vector<std::string> labels,
                     Value::Map properties) override;
  RelationshipPtr CreateRelationship(int64_t start_node_id, int64_t end_node_id,
                                     std::string type,
                                     Value::Map properties) override;
  RelationshipPtr CreateRelationship(const NodePtr &start_node,
                                     const NodePtr &end_node, std::string type,
                                     Value::Map properties = {});

  void SetNodeProperty(int64_t node_id, std::string property_key,
                       Value value) override;
  void SetRelationshipProperty(int64_t relationship_id,
                               std::string property_key, Value value) override;
  void SetNodeProperties(int64_t node_id, Value::Map properties,
                         bool include_existing) override;
  void SetRelationshipProperties(int64_t relationship_id, Value::Map properties,
                                 bool include_existing) override;
  void SetLabels(int64_t node_id, std::vector<std::string> labels) override;
  void RemoveNodeProperty(int64_t node_id,
                          std::string_view property_key) override;
  void RemoveRelationshipProperty(int64_t relationship_id,
                                  std::string_view property_key) override;
  void RemoveLabels(int64_t node_id,
                    const std::vector<std::string> &labels) override;
  void DeleteNode(int64_t node_id) override;
  void DeleteRelationship(int64_t relationship_id) override;

  [[nodiscard]] RelationshipPtr RelationshipById(int64_t id) const override;

  void AddNodeIndex(std::vector<std::string> labels,
                    std::string_view property_key, bool unique = false);
  void AddRelationshipIndex(std::vector<std::string> relationship_types,
                            std::string_view property_key, bool unique = false);

  [[nodiscard]] std::size_t RelationshipCount() const override {
    return relationships_.size();
  }
  [[nodiscard]] std::vector<NodePtr> Nodes() const;
  [[nodiscard]] std::vector<RelationshipPtr> Relationships() const;
  [[nodiscard]] NodePtr NodeById(int64_t id) const override;
  [[nodiscard]] Value NodeProperty(
      int64_t node_id, std::string_view property_key) const override;
  [[nodiscard]] Value RelationshipProperty(
      int64_t relationship_id, std::string_view property_key) const override;
  [[nodiscard]] std::optional<ir::NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override;
  [[nodiscard]] std::optional<ir::RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override;
 private:
  class Transaction;
  using MutableNodePtr = std::shared_ptr<Node>;
  using MutableRelationshipPtr = std::shared_ptr<Relationship>;

  void SetNodeProperty(const MutableNodePtr &node, std::string property_key,
                       Value value);
  void SetRelationshipProperty(const MutableRelationshipPtr &relationship,
                               std::string property_key, Value value);
  void SetNodeProperties(const MutableNodePtr &node, Value::Map properties,
                         bool include_existing);
  void SetRelationshipProperties(const MutableRelationshipPtr &relationship,
                                 Value::Map properties, bool include_existing);
  void SetLabels(const MutableNodePtr &node, std::vector<std::string> labels);
  void RemoveNodeProperty(const MutableNodePtr &node,
                          std::string_view property_key);
  void RemoveRelationshipProperty(const MutableRelationshipPtr &relationship,
                                  std::string_view property_key);
  void RemoveLabels(const MutableNodePtr &node,
                    const std::vector<std::string> &labels);
  void DeleteNode(const MutableNodePtr &node);
  void DeleteRelationship(const MutableRelationshipPtr &relationship);
  [[nodiscard]] const MutableNodePtr &MutableNodeById(int64_t id) const;
  [[nodiscard]] const MutableRelationshipPtr &MutableRelationshipById(
      int64_t id) const;
  [[nodiscard]] bool HasRelationship(int64_t id) const noexcept;
  [[nodiscard]] bool HasNode(int64_t id) const noexcept;

  struct IndexKey {
    std::vector<std::string> qualifiers;
    std::string property_key;

    bool operator==(const IndexKey &other) const = default;
  };

  struct IndexKeyHash {
    [[nodiscard]] std::size_t operator()(const IndexKey &key) const noexcept;
  };

  struct IndexDescriptor {
    std::vector<std::string> qualifiers;
    std::string property_key;
    bool unique = false;
  };

  using NodeIndexBuckets =
      std::unordered_map<Value, std::vector<MutableNodePtr>, ValueHash,
                         ValueEqual>;
  using RelationshipIndexBuckets =
      std::unordered_map<Value, std::vector<MutableRelationshipPtr>, ValueHash,
                         ValueEqual>;

  struct IndexValueLess {
    bool operator()(const Value &left, const Value &right) const;
  };
  template <typename EntityPtr>
  using RangeIndexBuckets = std::unordered_map<
      ValueType, std::map<Value, std::vector<EntityPtr>, IndexValueLess>>;

  [[nodiscard]] static IndexKey MakeIndexKey(
      std::vector<std::string> qualifiers, std::string_view property_key);

  void ValidateNodeUniqueIndexes(const Node &node) const;
  void ValidateRelationshipUniqueIndexes(
      const Relationship &relationship) const;
  void AddNodeToIndexes(const MutableNodePtr &node);
  void RemoveNodeFromIndexes(const MutableNodePtr &node);
  void AddRelationshipToIndexes(const MutableRelationshipPtr &relationship);
  void RemoveRelationshipFromIndexes(
      const MutableRelationshipPtr &relationship);
  void AddNodeToIndex(const IndexKey &index_key,
                      const IndexDescriptor &descriptor,
                      const MutableNodePtr &node);
  void RemoveNodeFromIndex(const IndexKey &index_key,
                           const IndexDescriptor &descriptor,
                           const MutableNodePtr &node);
  void AddRelationshipToIndex(const IndexKey &index_key,
                              const IndexDescriptor &descriptor,
                              const MutableRelationshipPtr &relationship);
  void RemoveRelationshipFromIndex(const IndexKey &index_key,
                                   const IndexDescriptor &descriptor,
                                   const MutableRelationshipPtr &relationship);
  void AddRelationshipToAdjacency(const MutableRelationshipPtr &relationship);
  void RemoveRelationshipFromAdjacency(
      const MutableRelationshipPtr &relationship);
  void AddNodeToLabels(const MutableNodePtr &node);
  void RemoveNodeFromLabels(const MutableNodePtr &node);
  int64_t next_node_id_ = 0;
  int64_t next_relationship_id_ = 0;
  std::vector<MutableNodePtr> nodes_;
  std::vector<MutableRelationshipPtr> relationships_;
  std::unordered_map<int64_t, MutableNodePtr> nodes_by_id_;
  std::unordered_map<int64_t, MutableRelationshipPtr> relationships_by_id_;
  std::unordered_map<IndexKey, IndexDescriptor, IndexKeyHash> node_indexes_;
  std::unordered_map<IndexKey, IndexDescriptor, IndexKeyHash>
      relationship_indexes_;
  std::unordered_map<IndexKey, NodeIndexBuckets, IndexKeyHash>
      node_index_buckets_;
  std::unordered_map<IndexKey, RelationshipIndexBuckets, IndexKeyHash>
      relationship_index_buckets_;
  std::unordered_map<IndexKey, RangeIndexBuckets<MutableNodePtr>, IndexKeyHash>
      node_range_index_buckets_;
  std::unordered_map<IndexKey, RangeIndexBuckets<MutableRelationshipPtr>,
                     IndexKeyHash>
      relationship_range_index_buckets_;
  std::unordered_map<std::string, std::vector<MutableNodePtr>> nodes_by_label_;
  std::unordered_map<std::string, std::vector<MutableRelationshipPtr>>
      relationships_by_type_;
  std::unordered_map<int64_t, std::vector<MutableRelationshipPtr>>
      outgoing_relationships_;
  std::unordered_map<int64_t, std::vector<MutableRelationshipPtr>>
      incoming_relationships_;
  bool transaction_active_ = false;
};

}  // namespace rg
