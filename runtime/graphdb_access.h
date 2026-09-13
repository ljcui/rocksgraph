#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "value/value.h"

namespace graphdb {
class Edge;
class Vertex;
}  // namespace graphdb

namespace txn {
class Transaction;
}  // namespace txn

namespace rg {

struct RelationshipReference {
  std::int64_t id = -1;
  std::uint32_t type_id = 0;

  bool operator==(const RelationshipReference &) const = default;
};

[[nodiscard]] graphdb::Vertex GraphDBVertexById(txn::Transaction &transaction,
                                                std::int64_t id);
[[nodiscard]] graphdb::Edge GraphDBEdgeById(txn::Transaction &transaction,
                                            RelationshipReference relationship);
[[nodiscard]] Value::NodePtr MaterializeGraphDBVertex(
    txn::Transaction &transaction, std::int64_t id);
[[nodiscard]] Value::RelationshipPtr MaterializeGraphDBEdge(
    txn::Transaction &transaction, RelationshipReference relationship);

[[nodiscard]] Value::NodePtr CreateGraphDBVertex(
    txn::Transaction &transaction, std::vector<std::string> labels,
    Value::Map properties);
[[nodiscard]] Value::RelationshipPtr CreateGraphDBEdge(
    txn::Transaction &transaction, std::int64_t start_node_id,
    std::int64_t end_node_id, std::string type, Value::Map properties);
void SetGraphDBVertexProperty(txn::Transaction &transaction,
                              std::int64_t node_id, std::string property_key,
                              Value value);
void SetGraphDBEdgeProperty(txn::Transaction &transaction,
                            RelationshipReference relationship,
                            std::string property_key, Value value);
void SetGraphDBVertexProperties(txn::Transaction &transaction,
                                std::int64_t node_id, Value::Map properties,
                                bool include_existing);
void SetGraphDBEdgeProperties(txn::Transaction &transaction,
                              RelationshipReference relationship,
                              Value::Map properties, bool include_existing);
void AddGraphDBVertexLabels(txn::Transaction &transaction, std::int64_t node_id,
                            std::vector<std::string> labels);
void RemoveGraphDBVertexProperty(txn::Transaction &transaction,
                                 std::int64_t node_id,
                                 std::string_view property_key);
void RemoveGraphDBEdgeProperty(txn::Transaction &transaction,
                               RelationshipReference relationship,
                               std::string_view property_key);
void RemoveGraphDBVertexLabels(txn::Transaction &transaction,
                               std::int64_t node_id,
                               const std::vector<std::string> &labels);
void DeleteGraphDBVertex(txn::Transaction &transaction, std::int64_t node_id);
void DeleteGraphDBEdge(txn::Transaction &transaction,
                       RelationshipReference relationship);

}  // namespace rg
