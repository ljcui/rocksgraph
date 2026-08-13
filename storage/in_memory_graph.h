#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/planner/catalog.h"
#include "ir/planner/cost_model.h"
#include "storage/storage.h"
#include "value/value.h"

namespace rg {

class InMemoryGraph final : public Storage,
                            public ir::PlannerCatalog,
                            public ir::PlannerStatistics {
 public:
  using NodePtr = Value::NodePtr;
  using RelationshipPtr = Value::RelationshipPtr;

  NodePtr CreateNode(std::vector<std::string> labels,
                     Value::Map properties = {}) override;
  RelationshipPtr CreateRelationship(int64_t start_node_id, int64_t end_node_id,
                                     std::string type,
                                     Value::Map properties = {}) override;
  RelationshipPtr CreateRelationship(const NodePtr &start_node,
                                     const NodePtr &end_node, std::string type,
                                     Value::Map properties = {});

  void SetNodeProperty(const NodePtr &node, std::string property_key,
                       Value value);
  void SetRelationshipProperty(const RelationshipPtr &relationship,
                               std::string property_key, Value value);
  void SetNodeProperties(const NodePtr &node, Value::Map properties,
                         bool include_existing);
  void SetRelationshipProperties(const RelationshipPtr &relationship,
                                 Value::Map properties, bool include_existing);
  void SetLabels(const NodePtr &node, std::vector<std::string> labels);
  void RemoveNodeProperty(const NodePtr &node, std::string_view property_key);
  void RemoveRelationshipProperty(const RelationshipPtr &relationship,
                                  std::string_view property_key);
  void RemoveLabels(const NodePtr &node,
                    const std::vector<std::string> &labels);
  void DeleteNode(const NodePtr &node);
  void DeleteRelationship(const RelationshipPtr &relationship);
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

  [[nodiscard]] bool HasRelationship(int64_t id) const noexcept;
  [[nodiscard]] const RelationshipPtr &RelationshipById(
      int64_t id) const override;
  [[nodiscard]] std::vector<RelationshipPtr> RelationshipsConnectedTo(
      int64_t node_id) const override;
  [[nodiscard]] std::vector<RelationshipPtr> OutgoingRelationships(
      int64_t node_id) const override;
  [[nodiscard]] std::vector<RelationshipPtr> IncomingRelationships(
      int64_t node_id) const override;

  void AddNodeIndex(std::vector<std::string> labels,
                    std::string_view property_key, bool unique = false);
  void AddRelationshipIndex(std::vector<std::string> relationship_types,
                            std::string_view property_key, bool unique = false);
  [[nodiscard]] std::vector<NodePtr> FindNodesByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const override;
  [[nodiscard]] std::vector<NodePtr> NodesInIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override;
  [[nodiscard]] std::vector<RelationshipPtr> FindRelationshipsByIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const Value &value) const override;
  [[nodiscard]] std::vector<RelationshipPtr> RelationshipsInIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const override;

  [[nodiscard]] std::vector<NodePtr> ScanNodes() const override {
    return nodes_;
  }
  [[nodiscard]] std::vector<RelationshipPtr> ScanRelationships()
      const override {
    return relationships_;
  }
  [[nodiscard]] std::size_t RelationshipCount() const override {
    return relationships_.size();
  }
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

  [[nodiscard]] const NodePtr &NodeById(int64_t id) const override;
  [[nodiscard]] bool HasNode(int64_t id) const noexcept;

  [[nodiscard]] std::optional<ir::NodeIndexDescriptor> FindNodeIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const override;
  [[nodiscard]] std::optional<ir::RelationshipIndexDescriptor>
  FindRelationshipIndex(const std::vector<std::string> &relationship_types,
                        std::string_view property_key) const override;
  [[nodiscard]] double EstimateNodeCount(
      const std::unordered_set<std::string> &labels) const override;
  [[nodiscard]] double EstimateExpandFanout(
      const std::vector<std::string> &relationship_types) const override;
  [[nodiscard]] double EstimateExpandIntoSelectivity(
      const std::vector<std::string> &relationship_types) const override;
  [[nodiscard]] double EstimateFilterSelectivity() const override;
  [[nodiscard]] double EstimateNodeHashJoinSelectivity(
      std::size_t key_count) const override;
  [[nodiscard]] double EstimateNodeIndexSeekSelectivity(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key) const override;
  [[nodiscard]] double EstimateNodeIndexRangeSeekSelectivity(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key, std::size_t bound_count) const override;
  [[nodiscard]] double EstimateRelationshipCount(
      const std::vector<std::string> &relationship_types) const override;
  [[nodiscard]] double EstimateRelationshipIndexSeekSelectivity(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const override;
  [[nodiscard]] double EstimateRelationshipIndexRangeSeekSelectivity(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, std::size_t bound_count) const override;
  [[nodiscard]] double EstimateProcedureRows(
      std::string_view procedure_name, std::size_t yield_count) const override;

 private:
  struct IndexDescriptor {
    std::vector<std::string> qualifiers;
    std::string property_key;
    bool unique = false;
  };

  struct PropertyDistribution {
    double total_entities = 0.0;
    double entities_with_property = 0.0;
    double distinct_values = 0.0;
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
  [[nodiscard]] PropertyDistribution NodePropertyDistribution(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key) const;
  [[nodiscard]] PropertyDistribution RelationshipPropertyDistribution(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const;
  [[nodiscard]] static double EqualitySelectivity(
      const PropertyDistribution &distribution);
  [[nodiscard]] static double RangeSelectivity(
      const PropertyDistribution &distribution, std::size_t bound_count);

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
