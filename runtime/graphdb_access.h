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

namespace graphdb {
class Transaction;
}  // namespace graphdb

namespace rg {

struct RelationshipReference {
  std::int64_t id = -1;
  std::uint32_t type_id = 0;

  bool operator==(const RelationshipReference &) const = default;
};

[[nodiscard]] graphdb::Vertex GraphDBVertexById(
    graphdb::Transaction &transaction, std::int64_t id);
[[nodiscard]] graphdb::Edge GraphDBEdgeById(graphdb::Transaction &transaction,
                                            RelationshipReference relationship);
[[nodiscard]] Value::NodePtr MaterializeGraphDBVertex(
    graphdb::Transaction &transaction, std::int64_t id);
[[nodiscard]] Value::NodePtr MaterializeGraphDBVertex(graphdb::Vertex vertex);
[[nodiscard]] Value::RelationshipPtr MaterializeGraphDBEdge(
    graphdb::Transaction &transaction, RelationshipReference relationship);
[[nodiscard]] Value::RelationshipPtr MaterializeGraphDBEdge(graphdb::Edge edge);

[[nodiscard]] Value::NodePtr CreateGraphDBVertex(
    graphdb::Transaction &transaction, std::vector<std::string> labels,
    Value::Map properties);
[[nodiscard]] Value::RelationshipPtr CreateGraphDBEdge(
    graphdb::Transaction &transaction, std::int64_t start_node_id,
    std::int64_t end_node_id, std::string type, Value::Map properties);
void SetGraphDBVertexProperty(graphdb::Transaction &transaction,
                              std::int64_t node_id, std::string property_key,
                              Value value);
void SetGraphDBEdgeProperty(graphdb::Transaction &transaction,
                            RelationshipReference relationship,
                            std::string property_key, Value value);
void SetGraphDBVertexProperties(graphdb::Transaction &transaction,
                                std::int64_t node_id, Value::Map properties,
                                bool include_existing);
void SetGraphDBEdgeProperties(graphdb::Transaction &transaction,
                              RelationshipReference relationship,
                              Value::Map properties, bool include_existing);
void AddGraphDBVertexLabels(graphdb::Transaction &transaction,
                            std::int64_t node_id,
                            std::vector<std::string> labels);
void RemoveGraphDBVertexProperty(graphdb::Transaction &transaction,
                                 std::int64_t node_id,
                                 std::string_view property_key);
void RemoveGraphDBEdgeProperty(graphdb::Transaction &transaction,
                               RelationshipReference relationship,
                               std::string_view property_key);
void RemoveGraphDBVertexLabels(graphdb::Transaction &transaction,
                               std::int64_t node_id,
                               const std::vector<std::string> &labels);
void DeleteGraphDBVertex(graphdb::Transaction &transaction,
                         std::int64_t node_id);
void DeleteGraphDBEdge(graphdb::Transaction &transaction,
                       RelationshipReference relationship);

}  // namespace rg
