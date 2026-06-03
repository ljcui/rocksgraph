#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "value/value.h"

namespace rg {

class AccessPath {
 public:
  using NodePtr = Value::NodePtr;
  using RelationshipPtr = Value::RelationshipPtr;

  AccessPath() = default;
  AccessPath(const AccessPath &) = delete;
  AccessPath &operator=(const AccessPath &) = delete;
  virtual ~AccessPath() = default;

  [[nodiscard]] virtual std::vector<NodePtr> ScanNodes() const = 0;
  [[nodiscard]] virtual std::vector<RelationshipPtr> ScanRelationships()
      const = 0;
  [[nodiscard]] virtual std::size_t RelationshipCount() const = 0;

  [[nodiscard]] virtual const NodePtr &NodeById(std::int64_t id) const = 0;
  [[nodiscard]] virtual const RelationshipPtr &RelationshipById(
      std::int64_t id) const = 0;

  [[nodiscard]] virtual std::vector<RelationshipPtr> RelationshipsConnectedTo(
      std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::vector<RelationshipPtr> OutgoingRelationships(
      std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::vector<RelationshipPtr> IncomingRelationships(
      std::int64_t node_id) const = 0;

  [[nodiscard]] virtual std::vector<NodePtr> FindNodesByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const = 0;
  [[nodiscard]] virtual std::vector<NodePtr> NodesInIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const = 0;
  [[nodiscard]] virtual std::vector<RelationshipPtr> FindRelationshipsByIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const Value &value) const = 0;
  [[nodiscard]] virtual std::vector<RelationshipPtr> RelationshipsInIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const = 0;
};

}  // namespace rg
