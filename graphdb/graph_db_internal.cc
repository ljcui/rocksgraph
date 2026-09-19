//
// Created by botu.wzy
//

#include "graph_db_internal.h"

#include <boost/endian/conversion.hpp>
#include <filesystem>
#include <memory>

#include "common/byte_utils.h"
#include "common/exception.h"
#include "graphdb/id_codec.h"

namespace fs = std::filesystem;
using namespace boost::endian;
using common::ReadValue;

namespace graphdb::internal {

void ThrowIfIteratorError(rocksdb::Iterator* iter, std::string_view action);

void ThrowIfIteratorError(rocksdb::Iterator* iter, std::string_view action);

std::string BuildFullTextIndexPath(const std::string& graph_path,
                                   const std::string& index_name,
                                   uint32_t index_id) {
  return graph_path + "/ft/" + index_name + "_" + std::to_string(index_id);
}

std::string BuildMetaKey(MetadataType type, const std::string& name) {
  std::string key;
  key.append(1, static_cast<char>(type));
  key.append(name);
  return key;
}

std::string BuildVectorFieldMetaKey(const std::string& label,
                                    const std::string& property) {
  std::string key = label;
  key.push_back('\0');
  key.append(property);
  return key;
}

uint64_t LoadVisibleMaxWalId(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                             uint32_t index_id,
                             const rocksdb::Snapshot* snapshot) {
  std::string prefix = EncodeBigEndianId(index_id);
  std::string seek_key(prefix);
  seek_key.append(sizeof(uint64_t), static_cast<char>(0xFF));
  rocksdb::ReadOptions ro;
  ro.snapshot = snapshot;
  std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(ro, graph_cf->wal));
  iter->SeekForPrev(seek_key);
  if (!iter->Valid()) {
    ThrowIfIteratorError(iter.get(),
                         "index wal iterator failed while loading max wal id");
    return 0;
  }
  auto key = iter->key();
  if (!key.starts_with(prefix)) {
    ThrowIfIteratorError(iter.get(),
                         "index wal iterator failed while loading max wal id");
    return 0;
  }
  key.remove_prefix(sizeof(index_id));
  if (key.size() != sizeof(uint64_t)) {
    RG_THROW_CODE(StorageEngineError,
                  "index wal key has invalid size while loading max wal id, "
                  "expect {}, actual {}",
                  sizeof(uint64_t), key.size());
  }
  ThrowIfIteratorError(iter.get(),
                       "index wal iterator failed while loading max wal id");
  return big_to_native(ReadValue<uint64_t>(key.data()));
}

void DeletePropertyIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                               uint32_t index_id) {
  rocksdb::WriteBatch wb;
  std::string start_key = EncodeBigEndianId(index_id);
  std::string end_key = start_key;
  end_key.append(128, static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->index, start_key, end_key);

  std::string wal_start = EncodeBigEndianId(index_id);
  std::string wal_end = wal_start;
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->wal, wal_start, wal_end);

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write({}, two, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void DeletePropertyIndexWalRange(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                                 uint32_t index_id) {
  rocksdb::WriteBatch wb;
  std::string wal_start = EncodeBigEndianId(index_id);
  std::string wal_end = wal_start;
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->wal, wal_start, wal_end);
  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write({}, two, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void DeleteFullTextIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                               uint32_t index_id) {
  std::string start_key = EncodeBigEndianId(index_id);
  start_key.append(sizeof(int64_t), static_cast<char>(0x00));
  std::string end_key = EncodeBigEndianId(index_id);
  end_key.append(sizeof(int64_t), static_cast<char>(0xFF));

  rocksdb::WriteBatch wb;
  wb.DeleteRange(graph_cf->index, start_key, end_key);
  wb.DeleteRange(graph_cf->wal, start_key, end_key);

  rocksdb::WriteOptions wo;
  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write(wo, two, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void DeleteVectorIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                             uint32_t index_id) {
  rocksdb::WriteBatch wb;
  std::string wal_start = EncodeBigEndianId(index_id);
  std::string wal_end = wal_start;
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->wal, wal_start, wal_end);

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write({}, two, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void ResetFullTextIndexPath(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    RG_THROW_CODE(StorageEngineError,
                  "failed to remove stale fulltext index directory {}: {}",
                  path, ec.message());
  }
  fs::create_directories(path, ec);
  if (ec) {
    RG_THROW_CODE(StorageEngineError,
                  "failed to create fulltext index directory {}: {}", path,
                  ec.message());
  }
}

void ResetIndexPath(const std::string& path, const std::string& kind) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    RG_THROW_CODE(StorageEngineError,
                  "failed to remove stale {} directory {}: {}", kind, path,
                  ec.message());
  }
  fs::create_directories(path, ec);
  if (ec) {
    RG_THROW_CODE(StorageEngineError, "failed to create {} directory {}: {}",
                  kind, path, ec.message());
  }
}

void DeleteAllEntriesInColumnFamily(rocksdb::TransactionDB* db,
                                    rocksdb::ColumnFamilyHandle* cf,
                                    rocksdb::WriteBatch* wb) {
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(ro, cf));
  iter->SeekToFirst();
  if (!iter->Valid()) {
    return;
  }
  std::string begin = iter->key().ToString();
  iter->SeekToLast();
  if (!iter->Valid()) {
    return;
  }
  std::string end = iter->key().ToString();
  end.push_back('\0');
  wb->DeleteRange(cf, begin, end);
}

void CheckNoVectorFieldForNormalIndex(MetaInfo& meta_info,
                                      const std::unordered_set<uint32_t>& lids,
                                      const std::unordered_set<uint32_t>& pids,
                                      const std::string& index_name) {
  for (auto lid : lids) {
    for (auto pid : pids) {
      auto field = meta_info.GetVertexVectorField(lid, pid);
      if (field) {
        RG_THROW_CODE(InvalidParameter,
                      "normal index [{}] can not use vector field [label:{}, "
                      "property:{}]",
                      index_name, field->label(), field->property());
      }
    }
  }
}

void CheckNoNormalIndexForVectorField(MetaInfo& meta_info, uint32_t lid,
                                      uint32_t pid, const std::string& label,
                                      const std::string& property) {
  for (const auto& index : meta_info.GetVertexPropertyIndexes()) {
    if (index->lid() == lid && index->ContainsProperty(pid)) {
      RG_THROW_CODE(InvalidParameter,
                    "vector field [label:{}, property:{}] can not use normal "
                    "index [{}]",
                    label, property, index->Name());
    }
  }
  for (const auto& index : meta_info.GetVertexFullTextIndexes()) {
    if (index->LabelIds().count(lid) && index->PropertyIds().count(pid)) {
      RG_THROW_CODE(InvalidParameter,
                    "vector field [label:{}, property:{}] can not use normal "
                    "index [{}]",
                    label, property, index->Name());
    }
  }
}

void ThrowIfIteratorError(rocksdb::Iterator* iter, std::string_view action) {
  auto status = iter->status();
  if (!status.ok()) {
    RG_THROW_CODE(StorageEngineError, "{}: {}", action, status.ToString());
  }
}

}  // namespace graphdb::internal
