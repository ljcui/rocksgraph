#pragma once

#include <cstdint>

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

}  // namespace rg
