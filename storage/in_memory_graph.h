#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

  void AddNodeIndex(std::vector<std::string> labels,
                    std::string_view property_key, bool unique = false);
  void AddRelationshipIndex(std::vector<std::string> relationship_types,
                            std::string_view property_key, bool unique = false);

  [[nodiscard]] const std::vector<NodePtr> &Nodes() const noexcept {
    return nodes_;
  }
  [[nodiscard]] const std::vector<RelationshipPtr> &Relationships()
      const noexcept {
    return relationships_;
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
  [[nodiscard]] static std::string IndexKey(std::vector<std::string> qualifiers,
                                            std::string_view property_key);

  int64_t next_node_id_ = 0;
  int64_t next_relationship_id_ = 0;
  std::vector<NodePtr> nodes_;
  std::vector<RelationshipPtr> relationships_;
  std::unordered_map<int64_t, NodePtr> nodes_by_id_;
  std::unordered_map<std::string, bool> node_indexes_;
  std::unordered_map<std::string, bool> relationship_indexes_;
};

[[nodiscard]] bool NodeHasLabels(const Node &node,
                                 const std::vector<std::string> &labels);
[[nodiscard]] bool RelationshipHasAnyType(
    const Relationship &relationship, const std::vector<std::string> &types);
[[nodiscard]] const Value *FindProperty(const Value &value,
                                        std::string_view property_key);

}  // namespace rg
