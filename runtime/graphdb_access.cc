#include "runtime/graphdb_access.h"

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <unordered_map>
#include <unordered_set>

#include "common/exception.h"
#include "graphdb/graph_entity.h"
#include "transaction/transaction.h"

namespace rg {

namespace {

std::unordered_map<std::string, Value> ToGraphDBProperties(
    const Value::Map &properties) {
  std::unordered_map<std::string, Value> result;
  result.reserve(properties.size());
  for (const auto &[key, value] : properties) {
    if (!value.IsNull()) {
      result.emplace(key, value);
    }
  }
  return result;
}

template <typename Entity>
void ApplyGraphDBProperties(Entity *entity, Value::Map properties,
                            bool include_existing) {
  CHECK(entity != nullptr, common::InternalError, "graph entity is null");
  if (!include_existing) {
    entity->RemoveAllProperty();
  }
  std::unordered_map<std::string, Value> values;
  values.reserve(properties.size());
  for (auto &[key, value] : properties) {
    if (value.IsNull()) {
      entity->RemoveProperty(key);
    } else {
      values.emplace(std::move(key), std::move(value));
    }
  }
  if (!values.empty()) {
    entity->SetProperties(values);
  }
}

}  // namespace

graphdb::Vertex GraphDBVertexById(txn::Transaction &transaction,
                                  std::int64_t id) {
  return transaction.GetVertexById(boost::endian::native_to_big(id));
}

graphdb::Edge GraphDBEdgeById(txn::Transaction &transaction,
                              RelationshipReference relationship) {
  return transaction.GetEdgeById(relationship.type_id,
                                 boost::endian::native_to_big(relationship.id));
}

Value::NodePtr MaterializeGraphDBVertex(txn::Transaction &transaction,
                                        std::int64_t id) {
  graphdb::Vertex vertex = GraphDBVertexById(transaction, id);
  auto node = std::make_shared<Node>();
  node->id = id;
  const auto labels = vertex.GetLabels();
  node->labels.assign(labels.begin(), labels.end());
  std::sort(node->labels.begin(), node->labels.end());
  auto properties = vertex.GetAllProperty();
  node->properties.insert(properties.begin(), properties.end());
  return node;
}

Value::RelationshipPtr MaterializeGraphDBEdge(
    txn::Transaction &transaction, RelationshipReference relationship) {
  graphdb::Edge edge = GraphDBEdgeById(transaction, relationship);
  auto value = std::make_shared<Relationship>();
  value->id = relationship.id;
  value->start_node_id = edge.GetNativeStartId();
  value->end_node_id = edge.GetNativeEndId();
  value->type_id = edge.GetTypeId();
  value->type = edge.GetType();
  auto properties = edge.GetAllProperty();
  value->properties.insert(properties.begin(), properties.end());
  return value;
}

Value::NodePtr CreateGraphDBVertex(txn::Transaction &transaction,
                                   std::vector<std::string> labels,
                                   Value::Map properties) {
  std::unordered_set<std::string> graphdb_labels(labels.begin(), labels.end());
  graphdb::Vertex vertex =
      transaction.CreateVertex(graphdb_labels, ToGraphDBProperties(properties));
  return MaterializeGraphDBVertex(transaction, vertex.GetNativeId());
}

Value::RelationshipPtr CreateGraphDBEdge(txn::Transaction &transaction,
                                         std::int64_t start_node_id,
                                         std::int64_t end_node_id,
                                         std::string type,
                                         Value::Map properties) {
  graphdb::Vertex start = GraphDBVertexById(transaction, start_node_id);
  graphdb::Vertex end = GraphDBVertexById(transaction, end_node_id);
  graphdb::Edge edge =
      transaction.CreateEdge(start, end, type, ToGraphDBProperties(properties));
  return MaterializeGraphDBEdge(transaction,
                                {edge.GetNativeId(), edge.GetTypeId()});
}

void SetGraphDBVertexProperty(txn::Transaction &transaction,
                              std::int64_t node_id, std::string property_key,
                              Value value) {
  graphdb::Vertex vertex = GraphDBVertexById(transaction, node_id);
  if (value.IsNull()) {
    vertex.RemoveProperty(property_key);
  } else {
    vertex.SetProperties({{std::move(property_key), std::move(value)}});
  }
}

void SetGraphDBEdgeProperty(txn::Transaction &transaction,
                            RelationshipReference relationship,
                            std::string property_key, Value value) {
  graphdb::Edge edge = GraphDBEdgeById(transaction, relationship);
  if (value.IsNull()) {
    edge.RemoveProperty(property_key);
  } else {
    edge.SetProperties({{std::move(property_key), std::move(value)}});
  }
}

void SetGraphDBVertexProperties(txn::Transaction &transaction,
                                std::int64_t node_id, Value::Map properties,
                                bool include_existing) {
  graphdb::Vertex vertex = GraphDBVertexById(transaction, node_id);
  ApplyGraphDBProperties(&vertex, std::move(properties), include_existing);
}

void SetGraphDBEdgeProperties(txn::Transaction &transaction,
                              RelationshipReference relationship,
                              Value::Map properties, bool include_existing) {
  graphdb::Edge edge = GraphDBEdgeById(transaction, relationship);
  ApplyGraphDBProperties(&edge, std::move(properties), include_existing);
}

void AddGraphDBVertexLabels(txn::Transaction &transaction, std::int64_t node_id,
                            std::vector<std::string> labels) {
  graphdb::Vertex vertex = GraphDBVertexById(transaction, node_id);
  vertex.AddLabels({labels.begin(), labels.end()});
}

void RemoveGraphDBVertexProperty(txn::Transaction &transaction,
                                 std::int64_t node_id,
                                 std::string_view property_key) {
  graphdb::Vertex vertex = GraphDBVertexById(transaction, node_id);
  vertex.RemoveProperty(std::string(property_key));
}

void RemoveGraphDBEdgeProperty(txn::Transaction &transaction,
                               RelationshipReference relationship,
                               std::string_view property_key) {
  graphdb::Edge edge = GraphDBEdgeById(transaction, relationship);
  edge.RemoveProperty(std::string(property_key));
}

void RemoveGraphDBVertexLabels(txn::Transaction &transaction,
                               std::int64_t node_id,
                               const std::vector<std::string> &labels) {
  graphdb::Vertex vertex = GraphDBVertexById(transaction, node_id);
  vertex.DeleteLabels({labels.begin(), labels.end()});
}

void DeleteGraphDBVertex(txn::Transaction &transaction, std::int64_t node_id) {
  graphdb::Vertex vertex = GraphDBVertexById(transaction, node_id);
  (void)vertex.Delete();
}

void DeleteGraphDBEdge(txn::Transaction &transaction,
                       RelationshipReference relationship) {
  GraphDBEdgeById(transaction, relationship).Delete();
}

}  // namespace rg
