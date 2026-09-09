#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "value/value.h"

namespace rg {

class GraphReader;
class Storage;

class StorageTransaction {
 public:
  StorageTransaction() = default;
  StorageTransaction(const StorageTransaction &) = delete;
  StorageTransaction &operator=(const StorageTransaction &) = delete;
  virtual ~StorageTransaction() = default;

  // The reader and optional writer expose the transaction's data view. A
  // read-only graph may return nullptr from Writer().
  [[nodiscard]] virtual const GraphReader &Reader() const = 0;
  [[nodiscard]] virtual Storage *Writer() = 0;
  virtual void Commit() = 0;
  virtual void Rollback() = 0;
};

struct IndexRangeBound {
  Value value;
  bool inclusive = false;
};

// Bounds are intersected. Null or incomparable bounds produce no matches.
struct IndexRange {
  std::vector<IndexRangeBound> lower_bounds;
  std::vector<IndexRangeBound> upper_bounds;
  std::optional<Value> prefix;
};

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

  // Every Cypher execution must run inside one transaction. The transaction
  // uses this reader's view for both reads and writes where supported.
  [[nodiscard]] virtual std::unique_ptr<StorageTransaction>
  BeginTransaction() = 0;

  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> ScanNodeIds() const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> ScanRelationshipIds()
      const = 0;
  // Labels are intersected; relationship types are unioned. Empty means all.
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> ScanNodeIdsByLabels(
      const std::vector<std::string> &labels) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor>
  ScanRelationshipIdsByTypes(const std::vector<std::string> &types) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor>
  RelationshipIdsConnectedTo(std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> OutgoingRelationshipIds(
      std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> IncomingRelationshipIds(
      std::int64_t node_id) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> FindNodeIdsByIndex(
      const std::vector<std::string> &labels, std::string_view property_key,
      const Value &value) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor> FindNodeIdsByIndexRange(
      const std::vector<std::string> &labels, std::string_view property_key,
      const IndexRange &range) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor>
  FindRelationshipIdsByIndex(const std::vector<std::string> &relationship_types,
                             std::string_view property_key,
                             const Value &value) const = 0;
  [[nodiscard]] virtual std::unique_ptr<EntityIdCursor>
  FindRelationshipIdsByIndexRange(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, const IndexRange &range) const = 0;

  [[nodiscard]] virtual std::size_t RelationshipCount() const = 0;

  [[nodiscard]] virtual NodePtr NodeById(std::int64_t id) const = 0;
  [[nodiscard]] virtual RelationshipPtr RelationshipById(
      std::int64_t id) const = 0;
  [[nodiscard]] virtual Value NodeProperty(
      std::int64_t node_id, std::string_view property_key) const = 0;
  [[nodiscard]] virtual Value RelationshipProperty(
      std::int64_t relationship_id, std::string_view property_key) const = 0;
};

}  // namespace rg
