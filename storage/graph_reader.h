#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "value/value.h"

namespace rg {

class EntityIdCursor {
 public:
  EntityIdCursor() = default;
  EntityIdCursor(const EntityIdCursor &) = delete;
  EntityIdCursor &operator=(const EntityIdCursor &) = delete;
  virtual ~EntityIdCursor() = default;

  [[nodiscard]] virtual bool Next() = 0;
  [[nodiscard]] virtual std::int64_t Id() const = 0;
  virtual void Close() noexcept = 0;
};

class GraphReader {
 public:
  using NodePtr = Value::NodePtr;
  using RelationshipPtr = Value::RelationshipPtr;

  GraphReader() = default;
  GraphReader(const GraphReader &) = delete;
  GraphReader &operator=(const GraphReader &) = delete;
  virtual ~GraphReader() = default;

  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> ScanNodeIds() const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> ScanRelationshipIds()
      const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor>
  RelationshipIdsConnectedTo(std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> OutgoingRelationshipIds(
      std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> IncomingRelationshipIds(
      std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> FindNodeIdsByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> NodeIdsInIndex(
      const std::vector<std::string> &labels,
      std::string_view property_key) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor>
  FindRelationshipIdsByIndex(const std::vector<std::string> &relationship_types,
                             std::string_view property_key,
                             const Value &value) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> RelationshipIdsInIndex(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const = 0;

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
