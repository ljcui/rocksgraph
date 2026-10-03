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

namespace runtime {

struct RelationshipReference {
  std::int64_t id = -1;
  std::uint32_t type_id = 0;

  bool operator==(const RelationshipReference &) const = default;
};

[[nodiscard]] graphdb::Vertex GraphDBVertexById(
    graphdb::Transaction &transaction, std::int64_t id);
[[nodiscard]] graphdb::Edge GraphDBEdgeById(graphdb::Transaction &transaction,
                                            RelationshipReference relationship);
[[nodiscard]] rg::Value::NodePtr MaterializeGraphDBVertex(
    graphdb::Transaction &transaction, std::int64_t id);
[[nodiscard]] rg::Value::NodePtr MaterializeGraphDBVertex(
    graphdb::Vertex vertex);
[[nodiscard]] rg::Value::RelationshipPtr MaterializeGraphDBEdge(
    graphdb::Transaction &transaction, RelationshipReference relationship);
[[nodiscard]] rg::Value::RelationshipPtr MaterializeGraphDBEdge(
    graphdb::Edge edge);

[[nodiscard]] rg::Value::NodePtr CreateGraphDBVertex(
    graphdb::Transaction &transaction, std::vector<std::string> labels,
    rg::Value::Map properties);
[[nodiscard]] rg::Value::RelationshipPtr CreateGraphDBEdge(
    graphdb::Transaction &transaction, std::int64_t start_node_id,
    std::int64_t end_node_id, std::string type, rg::Value::Map properties);
void SetGraphDBVertexProperty(graphdb::Transaction &transaction,
                              std::int64_t node_id, std::string property_key,
                              rg::Value value);
void SetGraphDBEdgeProperty(graphdb::Transaction &transaction,
                            RelationshipReference relationship,
                            std::string property_key, rg::Value value);
void SetGraphDBVertexProperties(graphdb::Transaction &transaction,
                                std::int64_t node_id, rg::Value::Map properties,
                                bool include_existing);
void SetGraphDBEdgeProperties(graphdb::Transaction &transaction,
                              RelationshipReference relationship,
                              rg::Value::Map properties, bool include_existing);
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

}  // namespace runtime
