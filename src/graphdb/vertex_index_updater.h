//
// Created by botu.wzy
//

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace txn {
class Transaction;
}

namespace graphdb {

using VertexSerializedProperties = std::unordered_map<uint32_t, std::string>;
using VertexVectorProperties = std::unordered_map<uint32_t, std::vector<float>>;

void UpdateVertexIndexes(txn::Transaction* txn, int64_t vid,
                         const std::unordered_set<uint32_t>& old_lids,
                         const std::unordered_set<uint32_t>& new_lids,
                         const VertexSerializedProperties& old_properties,
                         const VertexSerializedProperties& new_properties,
                         const VertexVectorProperties& old_vector_properties,
                         const VertexVectorProperties& new_vector_properties,
                         const std::unordered_set<uint32_t>& touched_pids);

}  // namespace graphdb
