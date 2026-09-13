#include "runtime/graphdb_access.h"

#include <algorithm>
#include <boost/endian/conversion.hpp>

#include "graphdb/graph_entity.h"
#include "transaction/transaction.h"

namespace rg {

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

}  // namespace rg
