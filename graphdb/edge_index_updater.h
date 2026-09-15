#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace graphdb {
class Transaction;

using EdgeSerializedProperties = std::unordered_map<uint32_t, std::string>;

EdgeSerializedProperties LoadEdgeSerializedProperties(Transaction* txn,
                                                      int64_t eid);

void UpdateEdgeIndexes(Transaction* txn, int64_t eid, uint32_t tid,
                       const EdgeSerializedProperties& old_properties,
                       const EdgeSerializedProperties& new_properties);

}  // namespace graphdb
