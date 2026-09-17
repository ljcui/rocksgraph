#pragma once

#include <rocksdb/utilities/transaction_db.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include "graph_cf.h"
#include "meta_info.h"

namespace graphdb::internal {

std::string BuildFullTextIndexPath(const std::string& graph_path,
                                   const std::string& index_name,
                                   uint32_t index_id);
std::string BuildMetaKey(MetadataType type, const std::string& name);
std::string BuildVectorFieldMetaKey(const std::string& label,
                                    const std::string& property);
uint64_t LoadVisibleMaxWalId(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                             uint32_t index_id,
                             const rocksdb::Snapshot* snapshot);
void DeletePropertyIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                               uint32_t index_id);
void DeletePropertyIndexWalRange(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                                 uint32_t index_id);
void DeleteFullTextIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                               uint32_t index_id);
void DeleteVectorIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                             uint32_t index_id);
void ResetFullTextIndexPath(const std::string& path);
void ResetIndexPath(const std::string& path, const std::string& kind);
void DeleteAllEntriesInColumnFamily(rocksdb::TransactionDB* db,
                                    rocksdb::ColumnFamilyHandle* cf,
                                    rocksdb::WriteBatch* wb);
void CheckNoVectorFieldForNormalIndex(MetaInfo& meta_info,
                                      const std::unordered_set<uint32_t>& lids,
                                      const std::unordered_set<uint32_t>& pids,
                                      const std::string& index_name);
void CheckNoNormalIndexForVectorField(MetaInfo& meta_info, uint32_t lid,
                                      uint32_t pid, const std::string& label,
                                      const std::string& property);
void ThrowIfIteratorError(rocksdb::Iterator* iter, std::string_view action);

}  // namespace graphdb::internal
