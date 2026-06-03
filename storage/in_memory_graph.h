#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/planner/catalog.h"
#include "value/value.h"

namespace rg {

class InMemoryGraph final : public ir::PlannerCatalog {
 public:
  using NodePtr = Value::NodePtr;
  using RelationshipPtr = Value::RelationshipPtr;

  NodePtr CreateNode(std::vector<std::string> labels,
                     Value::Map properties = {});
  RelationshipPtr CreateRelationship(int64_t start_node_id, int64_t end_node_id,
                                     std::string type,
                                     Value::Map properties = {});
  RelationshipPtr CreateRelationship(const NodePtr &start_node,
                                     const NodePtr &end_node, std::string type,
                                     Value::Map properties = {});

  void SetNodeProperty(const NodePtr &node, std::string property_key,
                       Value value);
  void SetRelationshipProperty(const RelationshipPtr &relationship,
                               std::string property_key, Value value);
  void SetNodeProperties(const NodePtr &node, Value::Map properties,
                         bool include_existing);
  void SetLabels(const NodePtr &node, std::vector<std::string> labels);
  void RemoveNodeProperty(const NodePtr &node, std::string_view property_key);
  void RemoveRelationshipProperty(const RelationshipPtr &relationship,
                                  std::string_view property_key);
  void RemoveLabels(const NodePtr &node,
                    const std::vector<std::string> &labels);
  void DeleteNode(const NodePtr &node);
  void DeleteRelationship(const RelationshipPtr &relationship);

  [[nodiscard]] bool HasRelationship(int64_t id) const noexcept;
  [[nodiscard]] const RelationshipPtr &RelationshipById(int64_t id) const;
  [[nodiscard]] std::vector<RelationshipPtr> RelationshipsConnectedTo(
      int64_t node_id) const;
  [[nodiscard]] std::vector<RelationshipPtr> OutgoingRelationships(
      int64_t node_id) const;
  [[nodiscard]] std::vector<RelationshipPtr> IncomingRelationships(
      int64_t node_id) const;

  void AddNodeIndex(std::vector<std::string> labels,
                    std::string_view property_key, bool unique = false);
  void AddRelationshipIndex(std::vector<std::string> relationship_types,
                            std::string_view property_key, bool unique = false);
  [[nodiscard]] std::vector<NodePtr> FindNodesByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const;
  [[nodiscard]] std::vector<NodePtr> NodesInIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const;
  [[nodiscard]] std::vector<RelationshipPtr> FindRelationshipsByIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const Value &value) const;
  [[nodiscard]] std::vector<RelationshipPtr> RelationshipsInIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const;

  [[nodiscard]] const std::vector<NodePtr> &Nodes() const noexcept {
    return nodes_;
  }
  [[nodiscard]] const std::vector<RelationshipPtr> &Relationships()
      const noexcept {
    return relationships_;
  }
  [[nodiscard]] const std::unordered_map<int64_t, RelationshipPtr> &
  RelationshipsById() const noexcept {
    return relationships_by_id_;
  }

  [[nodiscard]] const NodePtr &NodeById(int64_t id) const;
  [[nodiscard]] bool HasNode(int64_t id) const noexcept;

  [[nodiscard]] std::optional<ir::NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override;
  [[nodiscard]] std::optional<ir::RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override;

 private:
  struct IndexDescriptor {
    std::vector<std::string> qualifiers;
    std::string property_key;
    bool unique = false;
  };

  using NodeIndexBuckets =
      std::unordered_map<std::string, std::vector<NodePtr>>;
  using RelationshipIndexBuckets =
      std::unordered_map<std::string, std::vector<RelationshipPtr>>;

  [[nodiscard]] static std::string IndexKey(std::vector<std::string> qualifiers,
                                            std::string_view property_key);
  [[nodiscard]] static std::string ValueIndexKey(const Value &value);

  void AddNodeToIndexes(const NodePtr &node);
  void RemoveNodeFromIndexes(const NodePtr &node);
  void AddRelationshipToIndexes(const RelationshipPtr &relationship);
  void RemoveRelationshipFromIndexes(const RelationshipPtr &relationship);
  void AddNodeToIndex(const std::string &index_key,
                      const IndexDescriptor &descriptor, const NodePtr &node);
  void RemoveNodeFromIndex(const std::string &index_key,
                           const IndexDescriptor &descriptor,
                           const NodePtr &node);
  void AddRelationshipToIndex(const std::string &index_key,
                              const IndexDescriptor &descriptor,
                              const RelationshipPtr &relationship);
  void RemoveRelationshipFromIndex(const std::string &index_key,
                                   const IndexDescriptor &descriptor,
                                   const RelationshipPtr &relationship);
  void AddRelationshipToAdjacency(const RelationshipPtr &relationship);
  void RemoveRelationshipFromAdjacency(const RelationshipPtr &relationship);

  int64_t next_node_id_ = 0;
  int64_t next_relationship_id_ = 0;
  std::vector<NodePtr> nodes_;
  std::vector<RelationshipPtr> relationships_;
  std::unordered_map<int64_t, NodePtr> nodes_by_id_;
  std::unordered_map<int64_t, RelationshipPtr> relationships_by_id_;
  std::unordered_map<std::string, IndexDescriptor> node_indexes_;
  std::unordered_map<std::string, IndexDescriptor> relationship_indexes_;
  std::unordered_map<std::string, NodeIndexBuckets> node_index_buckets_;
  std::unordered_map<std::string, RelationshipIndexBuckets>
      relationship_index_buckets_;
  std::unordered_map<int64_t, std::vector<RelationshipPtr>>
      outgoing_relationships_;
  std::unordered_map<int64_t, std::vector<RelationshipPtr>>
      incoming_relationships_;
};

[[nodiscard]] bool NodeHasLabels(const Node &node,
                                 const std::vector<std::string> &labels);
[[nodiscard]] bool RelationshipHasAnyType(
    const Relationship &relationship, const std::vector<std::string> &types);
[[nodiscard]] const Value *FindProperty(const Value &value,
                                        std::string_view property_key);
[[nodiscard]] Value::Map CopyProperties(const Value::Map &properties);

}  // namespace rg
