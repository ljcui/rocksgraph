#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "planner/catalog.h"
#include "planner/cost_model.h"
#include "storage/storage.h"

namespace graphdb {
class GraphDB;
struct GraphDBOptions;
}  // namespace graphdb

namespace rg {

// Adapts the persistent graphdb engine to the interfaces consumed by the
// planner and slotted runtime. Query execution uses the transaction-owned
// reader/writer returned by BeginTransaction().
class GraphDBStorage final : public Storage,
                             public ir::PlannerCatalog,
                             public ir::PlannerStatistics {
 public:
  using Storage::CreateNode;
  using Storage::CreateRelationship;

  static std::unique_ptr<GraphDBStorage> Open(const std::string &path);
  static std::unique_ptr<GraphDBStorage> Open(
      const std::string &path, const graphdb::GraphDBOptions &options);

  explicit GraphDBStorage(std::unique_ptr<graphdb::GraphDB> graph_db);
  ~GraphDBStorage() override;

  [[nodiscard]] graphdb::GraphDB &RawGraphDB() noexcept { return *graph_db_; }
  [[nodiscard]] const graphdb::GraphDB &RawGraphDB() const noexcept {
    return *graph_db_;
  }

  [[nodiscard]] std::unique_ptr<StorageTransaction> BeginTransaction() override;

  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIds() const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIds()
      const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanNodeIdsByLabels(
      const std::vector<std::string> &labels) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> ScanRelationshipIdsByTypes(
      const std::vector<std::string> &types) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> RelationshipIdsConnectedTo(
      std::int64_t node_id) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> OutgoingRelationshipIds(
      std::int64_t node_id) const override;
  [[nodiscard]] std::unique_ptr<EntityIdCursor> IncomingRelationshipIds(
      std::int64_t node_id) const override;
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

  [[nodiscard]] std::size_t RelationshipCount() const override;
  [[nodiscard]] NodePtr NodeById(std::int64_t id) const override;
  [[nodiscard]] RelationshipPtr RelationshipById(
      std::int64_t id) const override;
  [[nodiscard]] Value NodeProperty(
      std::int64_t node_id, std::string_view property_key) const override;
  [[nodiscard]] Value RelationshipProperty(
      std::int64_t relationship_id,
      std::string_view property_key) const override;

  NodePtr CreateNode(std::vector<std::string> labels,
                     Value::Map properties) override;
  RelationshipPtr CreateRelationship(std::int64_t start_node_id,
                                     std::int64_t end_node_id, std::string type,
                                     Value::Map properties) override;
  void SetNodeProperty(std::int64_t node_id, std::string property_key,
                       Value value) override;
  void SetRelationshipProperty(std::int64_t relationship_id,
                               std::string property_key, Value value) override;
  void SetNodeProperties(std::int64_t node_id, Value::Map properties,
                         bool include_existing) override;
  void SetRelationshipProperties(std::int64_t relationship_id,
                                 Value::Map properties,
                                 bool include_existing) override;
  void SetLabels(std::int64_t node_id,
                 std::vector<std::string> labels) override;
  void RemoveNodeProperty(std::int64_t node_id,
                          std::string_view property_key) override;
  void RemoveRelationshipProperty(std::int64_t relationship_id,
                                  std::string_view property_key) override;
  void RemoveLabels(std::int64_t node_id,
                    const std::vector<std::string> &labels) override;
  void DeleteNode(std::int64_t node_id) override;
  void DeleteRelationship(std::int64_t relationship_id) override;

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
  [[nodiscard]] double EstimateRelationshipCount(
      const std::vector<std::string> &relationship_types) const override;

 private:
  class Transaction;

  [[nodiscard]] std::unique_ptr<StorageTransaction> BeginReadTransaction()
      const;

  std::unique_ptr<graphdb::GraphDB> graph_db_;
};

}  // namespace rg
