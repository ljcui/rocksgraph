#include "graphdb/edge_index_updater.h"

#include "common/byte_utils.h"
#include "common/exception.h"
#include "graphdb/graph_db.h"
#include "graphdb/id_codec.h"
#include "graphdb/index.h"
#include "graphdb/transaction.h"
#include "graphdb/value_codec.h"

namespace graphdb {

EdgeSerializedProperties LoadEdgeSerializedProperties(Transaction* txn,
                                                      int64_t eid) {
  EdgeSerializedProperties properties;
  rocksdb::ReadOptions ro;
  std::string prefix = EncodeBigEndianId(eid);
  std::unique_ptr<rocksdb::Iterator> iter(
      txn->dbtxn()->GetIterator(ro, txn->db()->graph_cf().edge_property));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key();
    if (key.size() != sizeof(eid) + sizeof(uint32_t)) {
      RG_THROW(common::ErrorCode::StorageEngineError,
               "edge property key has invalid size");
    }
    properties.emplace(ReadBigEndianId<uint32_t>(key.data() + sizeof(eid)),
                       iter->value().ToString());
  }
  if (!iter->status().ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError, iter->status().ToString());
  }
  return properties;
}

namespace {

std::optional<std::vector<rg::Value>> BuildIndexValues(
    const std::shared_ptr<EdgePropertyIndex>& index,
    const EdgeSerializedProperties& properties) {
  std::vector<rg::Value> values;
  values.reserve(index->PropertyCount());
  for (auto pid : index->pids()) {
    auto iter = properties.find(pid);
    if (iter == properties.end()) return std::nullopt;
    values.emplace_back(DeserializeValue(iter->second));
  }
  return values;
}

}  // namespace

void UpdateEdgeIndexes(Transaction* txn, int64_t eid, uint32_t tid,
                       const EdgeSerializedProperties& old_properties,
                       const EdgeSerializedProperties& new_properties) {
  for (const auto& index : txn->db()->meta_info().GetEdgePropertyIndexes()) {
    if (index->tid() != tid) continue;
    auto old_values = BuildIndexValues(index, old_properties);
    auto new_values = BuildIndexValues(index, new_properties);
    index->UpdateIndex(txn, eid, new_values, old_values);
  }
}

}  // namespace graphdb
