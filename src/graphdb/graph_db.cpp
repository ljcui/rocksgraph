/**
 * Copyright 2024 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

//
// Created by botu.wzy
//
#include "graph_db.h"

#include <filesystem>
#include <future>
#include <string_view>

#include "common/byte_utils.h"
#include "common/logger.h"
#include "meta_info.h"
#include "proto/meta.pb.h"
#include "transaction/transaction.h"
namespace fs = std::filesystem;
using namespace boost::endian;
using common::AsChars;
using common::ReadValue;
namespace graphdb {
namespace {

void ThrowIfIteratorError(rocksdb::Iterator* iter, std::string_view action);

std::string BuildFullTextIndexPath(const std::string& graph_path,
                                   const std::string& index_name,
                                   uint32_t index_id) {
  return graph_path + "/ft/" + index_name + "_" +
         std::to_string(big_to_native(index_id));
}

std::string BuildMetaKey(MetaDataType type, const std::string& name) {
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

const std::string kRaftApplyIndexKey(
    1, static_cast<char>(MetaDataType::RaftApplyIndex));

class IdGeneratorMetaBatchHandler : public rocksdb::WriteBatch::Handler {
 public:
  explicit IdGeneratorMetaBatchHandler(GraphDB* graph_db)
      : id_generator_(graph_db->id_generator()),
        meta_info_cf_id_(graph_db->graph_cf().meta_info->GetID()) {}

  rocksdb::Status PutCF(uint32_t column_family_id, const rocksdb::Slice& key,
                        const rocksdb::Slice& value) override {
    if (column_family_id != meta_info_cf_id_ || key.empty()) {
      return rocksdb::Status::OK();
    }
    auto type = static_cast<MetaDataType>(key.data()[0]);
    rocksdb::Slice key_suffix(key.data() + 1, key.size() - 1);
    id_generator_.ApplyMetaRecord(type, key_suffix, value);
    return rocksdb::Status::OK();
  }

  rocksdb::Status DeleteCF(uint32_t, const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status SingleDeleteCF(uint32_t, const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status DeleteRangeCF(uint32_t, const rocksdb::Slice&,
                                const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status MergeCF(uint32_t, const rocksdb::Slice&,
                          const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

 private:
  IdGenerator& id_generator_;
  uint32_t meta_info_cf_id_;
};

uint64_t LoadVisibleMaxWalId(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                             uint32_t index_id,
                             const rocksdb::Snapshot* snapshot) {
  std::string prefix(AsChars(index_id), sizeof(index_id));
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
    THROW_CODE(StorageEngineError,
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
  std::string start_key(AsChars(index_id), sizeof(index_id));
  std::string end_key(AsChars(index_id), sizeof(index_id));
  end_key.append(128, static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->index, start_key, end_key);

  std::string wal_start(AsChars(index_id), sizeof(index_id));
  std::string wal_end(AsChars(index_id), sizeof(index_id));
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->wal, wal_start, wal_end);

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write({}, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void DeletePropertyIndexWalRange(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                                 uint32_t index_id) {
  rocksdb::WriteBatch wb;
  std::string wal_start(AsChars(index_id), sizeof(index_id));
  std::string wal_end(AsChars(index_id), sizeof(index_id));
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->wal, wal_start, wal_end);
  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write({}, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void DeleteFullTextIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                               uint32_t index_id) {
  std::string start_key(AsChars(index_id), sizeof(index_id));
  start_key.append(sizeof(int64_t), static_cast<char>(0x00));
  std::string end_key(AsChars(index_id), sizeof(index_id));
  end_key.append(sizeof(int64_t), static_cast<char>(0xFF));

  rocksdb::WriteBatch wb;
  wb.DeleteRange(graph_cf->index, start_key, end_key);
  wb.DeleteRange(graph_cf->wal, start_key, end_key);

  rocksdb::WriteOptions wo;
  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write(wo, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void DeleteVectorIndexRanges(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                             uint32_t index_id) {
  rocksdb::WriteBatch wb;
  std::string wal_start(AsChars(index_id), sizeof(index_id));
  std::string wal_end(AsChars(index_id), sizeof(index_id));
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf->wal, wal_start, wal_end);

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db->Write({}, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void ResetFullTextIndexPath(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    THROW_CODE(StorageEngineError,
               "failed to remove stale fulltext index directory {}: {}", path,
               ec.message());
  }
  fs::create_directories(path, ec);
  if (ec) {
    THROW_CODE(StorageEngineError,
               "failed to create fulltext index directory {}: {}", path,
               ec.message());
  }
}

void ResetIndexPath(const std::string& path, const std::string& kind) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec) {
    THROW_CODE(StorageEngineError, "failed to remove stale {} directory {}: {}",
               kind, path, ec.message());
  }
  fs::create_directories(path, ec);
  if (ec) {
    THROW_CODE(StorageEngineError, "failed to create {} directory {}: {}", kind,
               path, ec.message());
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
        THROW_CODE(InvalidParameter,
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
      THROW_CODE(InvalidParameter,
                 "vector field [label:{}, property:{}] can not use normal "
                 "index [{}]",
                 label, property, index->Name());
    }
  }
  for (const auto& index : meta_info.GetVertexFullTextIndexes()) {
    if (index->LabelIds().count(lid) && index->PropertyIds().count(pid)) {
      THROW_CODE(InvalidParameter,
                 "vector field [label:{}, property:{}] can not use normal "
                 "index [{}]",
                 label, property, index->Name());
    }
  }
}

void ThrowIfIteratorError(rocksdb::Iterator* iter, std::string_view action) {
  auto status = iter->status();
  if (!status.ok()) {
    THROW_CODE(StorageEngineError, "{}: {}", action, status.ToString());
  }
}

}  // namespace

std::unique_ptr<GraphDB> GraphDB::Open(const std::string& path,
                                       const GraphDBOptions& graph_options) {
  if (!graph_options.assistant_pool) {
    THROW_CODE(InvalidParameter, "GraphDB assistant_pool must be provided");
  }
  std::string rocksdb_path = path + "/data";
  std::filesystem::create_directories(rocksdb_path);
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;
  options.enable_pipelined_write = true;
  rocksdb::BlockBasedTableOptions table_options;
  table_options.cache_index_and_filter_blocks = true;
  if (graph_options.block_cache) {
    table_options.block_cache = graph_options.block_cache;
  } else {
    table_options.block_cache = rocksdb::NewLRUCache(1 * 1024 * 1024 * 1024L);
  }
  table_options.data_block_index_type =
      rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash;
  table_options.partition_filters = true;
  table_options.index_type =
      rocksdb::BlockBasedTableOptions::IndexType::kTwoLevelIndexSearch;
  options.table_factory.reset(
      rocksdb::NewBlockBasedTableFactory(table_options));
  options.IncreaseParallelism();
  options.OptimizeLevelStyleCompaction();
  std::vector<std::string> built_in_cfs = {rocksdb::kDefaultColumnFamilyName,
                                           "vertex_property",
                                           "vertex_vector_property",
                                           "edge_property",
                                           "vertex_label_vid",
                                           "edge_type_eid",
                                           "meta_info",
                                           "index",
                                           "wal"};
  std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
  cfs.reserve(built_in_cfs.size());
  for (const auto& name : built_in_cfs) {
    cfs.emplace_back(name, options);
  }
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles;
  rocksdb::TransactionDBOptions txn_db_options;
  rocksdb::TransactionDB* db;
  auto s = rocksdb::TransactionDB::Open(options, txn_db_options, rocksdb_path,
                                        cfs, &cf_handles, &db);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  auto graph_db = std::make_unique<GraphDB>();
  graph_db->db_ = db;
  graph_db->path_ = path;
  graph_db->graph_cf_.graph_topology = cf_handles[0];
  graph_db->graph_cf_.vertex_property = cf_handles[1];
  graph_db->graph_cf_.vertex_vector_property = cf_handles[2];
  graph_db->graph_cf_.edge_property = cf_handles[3];
  graph_db->graph_cf_.vertex_label_vid = cf_handles[4];
  graph_db->graph_cf_.edge_type_eid = cf_handles[5];
  graph_db->graph_cf_.meta_info = cf_handles[6];
  graph_db->graph_cf_.index = cf_handles[7];
  graph_db->graph_cf_.wal = cf_handles[8];
  graph_db->cf_handles_ = std::move(cf_handles);
  graph_db->options_ = graph_options;
  graph_db->assistant_pool_ = graph_options.assistant_pool;
  graph_db->assistant_strand_ =
      std::make_unique<boost::asio::io_service::strand>(
          graph_db->assistant_pool_->Service());
  graph_db->meta_info_.Init(graph_db->db_, graph_db->assistant_pool_->Service(),
                            graph_db->assistant_strand_.get(),
                            &graph_db->graph_cf_,
                            graph_db->options_.ft_apply_interval_,
                            graph_db->options_.ft_writer_threads_,
                            graph_db->options_.ft_writer_memory_budget_,
                            graph_db->options_.vt_apply_interval_);
  graph_db->ResumeBackgroundIndexBuilds();

  return graph_db;
}

GraphDB::~GraphDB() {
  LOG_INFO("Close graph: {}", db_meta_.graph_name());
  StopRaft();
  for (const auto& index : meta_info_.GetVertexVectorIndexes()) {
    index->Stop();
  }
  for (const auto& index : meta_info_.GetVertexFullTextIndexes()) {
    index->Stop();
  }
  DrainAssistant();
  meta_info_.ClearVertexVectorIndexes();
  meta_info_.ClearVertexFullTextIndexes();
  for (auto handle : cf_handles_) {
    auto s = db_->DestroyColumnFamilyHandle(handle);
    assert(s.ok());
  }
  auto s = db_->Close();
  if (!s.ok()) {
    LOG_WARN("graph db close error : {}", s.ToString());
  }
  delete db_;
  db_ = nullptr;
  if (drop_on_close_) {
    std::filesystem::remove_all(path_);
    LOG_INFO("filesystem remove_all {}", path_);
  }
}

std::unique_ptr<txn::Transaction> GraphDB::BeginTransaction() {
  rocksdb::WriteOptions wo;
  rocksdb::TransactionOptions to;
  rocksdb::Transaction* txn = db_->BeginTransaction(wo, to);
  return std::make_unique<txn::Transaction>(txn, this);
}

raft::RaftDriver* GraphDB::raft_driver() const {
  std::shared_lock<std::shared_mutex> lock(raft_mutex_);
  return raft_driver_.get();
}

void GraphDB::SetRaftDriver(std::unique_ptr<raft::RaftDriver> raft_driver) {
  std::unique_lock<std::shared_mutex> lock(raft_mutex_);
  raft_driver_ = std::move(raft_driver);
  meta_info_.id_generator().SetRaftDriver(raft_driver_.get());
}

void GraphDB::StopRaft() {
  std::unique_lock<std::shared_mutex> lock(raft_mutex_);
  if (raft_driver_) {
    raft_driver_->Stop();
    raft_driver_.reset();
  }
  meta_info_.id_generator().SetRaftDriver(nullptr);
}

uint64_t GraphDB::GetRaftApplyIndex() const {
  std::string val;
  auto s = db_->Get({}, graph_cf_.meta_info, kRaftApplyIndexKey, &val);
  if (s.IsNotFound()) {
    return 0;
  }
  if (!s.ok()) {
    THROW_CODE(StorageEngineError, "failed to load raft apply index: {}",
               s.ToString());
  }
  if (val.size() != sizeof(uint64_t)) {
    THROW_CODE(StorageEngineError,
               "raft apply index has invalid size, expect {}, actual {}",
               sizeof(uint64_t), val.size());
  }
  return ReadValue<uint64_t>(val.data());
}

void GraphDB::ApplyRaftRequest(uint64_t index,
                               const meta::RaftRequest& request) {
  switch (request.wb_kind()) {
    case meta::WriteBatchKind::GRAPH_WRITE:
    case meta::WriteBatchKind::ID_GENERATOR:
      ApplyRaftWriteBatch(index, request);
      return;
    case meta::WriteBatchKind::GRAPH_INDEX_DDL: {
      meta::GraphIndexDdlRequest ddl_request;
      if (!ddl_request.ParseFromString(request.wb_data())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse graph index ddl request for graph [{}] at "
                   "index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyGraphIndexDdlRequest(index, ddl_request);
      return;
    }
    case meta::WriteBatchKind::UNKNOWN:
      THROW_CODE(InvalidParameter,
                 "write batch kind must be specified for graph [{}] at index "
                 "{}",
                 db_meta_.graph_name(), index);
    default:
      THROW_CODE(InvalidParameter,
                 "unsupported write batch kind {} for graph [{}] at index {}",
                 static_cast<int>(request.wb_kind()), db_meta_.graph_name(),
                 index);
  }
}

void GraphDB::ApplyRaftWriteBatch(uint64_t index,
                                  const meta::RaftRequest& request) {
  rocksdb::WriteBatch wb(request.wb_data());

  auto s = SetRaftApplyIndex(index, &wb);
  if (!s.ok()) {
    THROW_CODE(StorageEngineError,
               "failed to persist raft apply index for graph [{}] at index {}: "
               "{}",
               db_meta_.graph_name(), index, s.ToString());
  }

  auto* base_db = db_->GetBaseDB();
  if (!base_db) {
    THROW_CODE(StorageEngineError,
               "failed to access base rocksdb::DB for graph [{}]",
               db_meta_.graph_name());
  }

  s = base_db->Write({}, &wb);
  if (!s.ok()) {
    THROW_CODE(StorageEngineError,
               "failed to apply raft request for graph [{}] at index {}: {}",
               db_meta_.graph_name(), index, s.ToString());
  }

  switch (request.wb_kind()) {
    case meta::WriteBatchKind::GRAPH_WRITE:
      return;
    case meta::WriteBatchKind::ID_GENERATOR:
      SyncIdGeneratorFromRaftBatch(wb);
      return;
    default:
      THROW_CODE(InvalidParameter,
                 "unsupported write batch kind {} for graph write batch [{}] "
                 "at index {}",
                 static_cast<int>(request.wb_kind()), db_meta_.graph_name(),
                 index);
  }
}

rocksdb::Status GraphDB::SetRaftApplyIndex(uint64_t apply_index,
                                           rocksdb::WriteBatch* wb) const {
  return wb->Put(graph_cf_.meta_info, kRaftApplyIndexKey,
                 std::string(AsChars(apply_index), sizeof(apply_index)));
}

void GraphDB::SyncIdGeneratorFromRaftBatch(const rocksdb::WriteBatch& wb) {
  IdGeneratorMetaBatchHandler handler(this);
  auto s = wb.Iterate(&handler);
  if (!s.ok()) {
    THROW_CODE(StorageEngineError,
               "failed to sync id generator cache from raft batch for graph "
               "[{}]: {}",
               db_meta_.graph_name(), s.ToString());
  }
}

void GraphDB::ProposeGraphIndexDdl(
    meta::GraphIndexDdlRequest::Operation operation, std::string payload) {
  auto* driver = raft_driver();
  if (driver == nullptr) {
    THROW_CODE(StorageEngineError,
               "raft driver is required to propose index ddl for graph [{}]",
               db_meta_.graph_name());
  }
  meta::GraphIndexDdlRequest ddl_request;
  ddl_request.set_operation(operation);
  ddl_request.set_payload(std::move(payload));

  meta::RaftRequest raft_request;
  raft_request.set_wb_kind(meta::WriteBatchKind::GRAPH_INDEX_DDL);
  raft_request.set_wb_data(ddl_request.SerializeAsString());
  auto apply_result =
      driver->ProposeRaftRequestAndWait(std::move(raft_request));
  if (apply_result.err != nullptr) {
    THROW_CODE(StorageEngineError, apply_result.err.String());
  }
}

void GraphDB::ApplyGraphIndexDdlRequest(
    uint64_t index, const meta::GraphIndexDdlRequest& request) {
  switch (request.operation()) {
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_PROPERTY_INDEX: {
      meta::VertexPropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse create vertex property index request for "
                   "graph [{}] at index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyCreateVertexPropertyIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_VERTEX_PROPERTY_INDEX: {
      meta::VertexPropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse delete vertex property index request for "
                   "graph [{}] at index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyDeleteVertexPropertyIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_FULLTEXT_INDEX: {
      meta::VertexFullTextIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse create vertex fulltext index request for "
                   "graph [{}] at index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyCreateVertexFullTextIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_VERTEX_FULLTEXT_INDEX: {
      meta::VertexFullTextIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse delete vertex fulltext index request for "
                   "graph [{}] at index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyDeleteVertexFullTextIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_INDEX: {
      meta::VertexVectorIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse create vertex vector index request for "
                   "graph [{}] at index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyCreateVertexVectorIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_VERTEX_VECTOR_INDEX: {
      meta::VertexVectorIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse delete vertex vector index request for "
                   "graph [{}] at index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyDeleteVertexVectorIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_FIELD: {
      meta::VertexVectorField meta;
      if (!meta.ParseFromString(request.payload())) {
        THROW_CODE(InvalidParameter,
                   "failed to parse create vertex vector field request for "
                   "graph [{}] at index {}",
                   db_meta_.graph_name(), index);
      }
      ApplyCreateVertexVectorField(index, std::move(meta));
      return;
    }
    default:
      THROW_CODE(InvalidParameter,
                 "unsupported graph index ddl operation {} for graph [{}] at "
                 "index {}",
                 static_cast<int>(request.operation()), db_meta_.graph_name(),
                 index);
  }
}

void GraphDB::PersistVertexPropertyIndexMeta(
    const std::shared_ptr<VertexPropertyIndex>& index) {
  auto s = db_->Put(
      {}, graph_cf_.meta_info,
      BuildMetaKey(MetaDataType::VertexPropertyIndex, index->meta().name()),
      index->meta().SerializeAsString());
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void GraphDB::PersistVertexFullTextIndexMeta(
    const std::shared_ptr<VertexFullTextIndex>& index) {
  auto s = db_->Put(
      {}, graph_cf_.meta_info,
      BuildMetaKey(MetaDataType::VertexFullTextIndex, index->meta().name()),
      index->meta().SerializeAsString());
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void GraphDB::PersistVertexVectorIndexMeta(
    const std::shared_ptr<VertexVectorIndex>& index) {
  auto s = db_->Put(
      {}, graph_cf_.meta_info,
      BuildMetaKey(MetaDataType::VertexVectorIndex, index->meta().name()),
      index->meta().SerializeAsString());
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
}

void GraphDB::ResumeBackgroundIndexBuilds() {
  for (const auto& index : meta_info_.GetBuildingVertexPropertyIndexes()) {
    ScheduleVertexPropertyIndexBuild(index, true);
  }
  for (const auto& index : meta_info_.GetBuildingVertexFullTextIndexes()) {
    ScheduleVertexFullTextIndexBuild(index, true);
  }
  for (const auto& index : meta_info_.GetBuildingVertexVectorIndexes()) {
    ScheduleVertexVectorIndexBuild(index, true);
  }
}

void GraphDB::DrainAssistant() {
  if (!assistant_strand_) {
    return;
  }
  if (assistant_strand_->running_in_this_thread()) {
    return;
  }
  std::promise<void> drained;
  auto future = drained.get_future();
  assistant_strand_->post([&drained]() mutable { drained.set_value(); });
  future.wait();
}

void GraphDB::ScheduleVertexPropertyIndexBuild(
    const std::shared_ptr<VertexPropertyIndex>& index, bool reset_existing) {
  assistant_strand_->post([this, index, reset_existing]() {
    const rocksdb::Snapshot* snapshot = nullptr;
    try {
      if (reset_existing) {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        DeletePropertyIndexRanges(db_, &graph_cf_, index->index_id());
        index->ResetForBuild();
        index->SetState(meta::IndexBuildState::BUILDING);
        PersistVertexPropertyIndexMeta(index);
      }

      snapshot = db_->GetSnapshot();
      uint64_t snapshot_wal_id =
          LoadVisibleMaxWalId(db_, &graph_cf_, index->index_id(), snapshot);
      index->meta().set_build_start_wal_id(snapshot_wal_id + 1);
      index->Load(snapshot, snapshot_wal_id);
      db_->ReleaseSnapshot(snapshot);
      snapshot = nullptr;

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->SetState(meta::IndexBuildState::CATCHING_UP);
        index->SetBuildError("");
        PersistVertexPropertyIndexMeta(index);
      }

      index->ApplyWAL();

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->ApplyWAL();
        DeletePropertyIndexWalRange(db_, &graph_cf_, index->index_id());
        index->SetState(meta::IndexBuildState::READY);
        index->SetBuildError("");
        PersistVertexPropertyIndexMeta(index);
        meta_info_.PublishVertexPropertyIndex(index->Name());
      }
    } catch (const std::exception& e) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex property index [{}] build failed: {}", index->Name(),
                e.what());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError(e.what());
        PersistVertexPropertyIndexMeta(index);
      }
    } catch (...) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex property index [{}] build failed with unknown error",
                index->Name());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError("unknown error");
        PersistVertexPropertyIndexMeta(index);
      }
    }
  });
}

void GraphDB::ScheduleVertexFullTextIndexBuild(
    const std::shared_ptr<VertexFullTextIndex>& index, bool reset_existing) {
  assistant_strand_->post([this, index, reset_existing]() {
    const rocksdb::Snapshot* snapshot = nullptr;
    try {
      if (reset_existing) {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(fulltext_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        DeleteFullTextIndexRanges(db_, &graph_cf_, index->index_id());
        index->ReleaseResources();
        ResetFullTextIndexPath(index->meta().path());
        index->ResetForClear();
        index->SetState(meta::IndexBuildState::BUILDING);
        PersistVertexFullTextIndexMeta(index);
      }

      snapshot = db_->GetSnapshot();
      uint64_t snapshot_wal_id =
          LoadVisibleMaxWalId(db_, &graph_cf_, index->index_id(), snapshot);
      index->meta().set_build_start_wal_id(snapshot_wal_id + 1);
      index->Load(snapshot, snapshot_wal_id);
      db_->ReleaseSnapshot(snapshot);
      snapshot = nullptr;

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->SetState(meta::IndexBuildState::CATCHING_UP);
        index->SetBuildError("");
        PersistVertexFullTextIndexMeta(index);
      }

      index->ApplyWAL();

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(fulltext_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->ApplyWAL();
        index->SetState(meta::IndexBuildState::READY);
        index->SetBuildError("");
        PersistVertexFullTextIndexMeta(index);
        meta_info_.PublishVertexFullTextIndex(index->Name());
        index->Start();
      }
    } catch (const std::exception& e) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex fulltext index [{}] build failed: {}", index->Name(),
                e.what());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError(e.what());
        PersistVertexFullTextIndexMeta(index);
      }
    } catch (...) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex fulltext index [{}] build failed with unknown error",
                index->Name());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError("unknown error");
        PersistVertexFullTextIndexMeta(index);
      }
    }
  });
}

void GraphDB::ScheduleVertexVectorIndexBuild(
    const std::shared_ptr<VertexVectorIndex>& index, bool reset_existing) {
  assistant_strand_->post([this, index, reset_existing]() {
    const rocksdb::Snapshot* snapshot = nullptr;
    try {
      if (reset_existing) {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(vector_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        DeleteVectorIndexRanges(db_, &graph_cf_, index->index_id());
        index->ReleaseResources();
        ResetIndexPath(index->meta().path(), "vector index");
        index->ResetForClear();
        index->SetState(meta::IndexBuildState::BUILDING);
        PersistVertexVectorIndexMeta(index);
      }

      snapshot = db_->GetSnapshot();
      uint64_t snapshot_wal_id =
          LoadVisibleMaxWalId(db_, &graph_cf_, index->index_id(), snapshot);
      index->meta().set_build_start_wal_id(snapshot_wal_id + 1);
      index->Load(snapshot, snapshot_wal_id);
      db_->ReleaseSnapshot(snapshot);
      snapshot = nullptr;

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->SetState(meta::IndexBuildState::CATCHING_UP);
        index->SetBuildError("");
        PersistVertexVectorIndexMeta(index);
      }

      index->ApplyWAL();

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(vector_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->ApplyWAL();
        index->SetState(meta::IndexBuildState::READY);
        index->SetBuildError("");
        PersistVertexVectorIndexMeta(index);
        meta_info_.PublishVertexVectorIndex(index->meta().name());
        index->Start();
      }
    } catch (const std::exception& e) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex vector index [{}] build failed: {}",
                index->meta().name(), e.what());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError(e.what());
        PersistVertexVectorIndexMeta(index);
      }
    } catch (...) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex vector index [{}] build failed with unknown error",
                index->meta().name());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError("unknown error");
        PersistVertexVectorIndexMeta(index);
      }
    }
  });
}

void GraphDB::ClearData() {
  if (db_meta_.enable_raft()) {
    THROW_CODE(InvalidParameter,
               "ClearData is not supported on raft graph [{}]; use "
               "Galaxy::ClearGraph instead",
               db_meta_.graph_name());
  }
  ClearDataInternal();
}

void GraphDB::ClearDataInternal() {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::vector<std::shared_ptr<VertexFullTextIndex>> ft_indexes;
  std::vector<std::shared_ptr<VertexVectorIndex>> vector_indexes;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    ft_indexes = meta_info_.GetVertexFullTextIndexes();
    vector_indexes = meta_info_.GetVertexVectorIndexes();
  }

  for (const auto& index : ft_indexes) {
    index->Stop();
  }
  for (const auto& index : vector_indexes) {
    index->Stop();
  }
  DrainAssistant();

  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::unique_lock<std::mutex> property_commit_lock(
      property_index_commit_mutex_, std::defer_lock);
  std::unique_lock<std::mutex> fulltext_commit_lock(
      fulltext_index_commit_mutex_, std::defer_lock);
  std::unique_lock<std::mutex> vector_commit_lock(vector_index_commit_mutex_,
                                                  std::defer_lock);
  std::lock(property_commit_lock, fulltext_commit_lock, vector_commit_lock);

  auto property_indexes = meta_info_.GetVertexPropertyIndexes();
  ft_indexes = meta_info_.GetVertexFullTextIndexes();
  vector_indexes = meta_info_.GetVertexVectorIndexes();

  rocksdb::WriteBatch wb;
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.graph_topology, &wb);
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.vertex_property, &wb);
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.vertex_vector_property, &wb);
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.edge_property, &wb);
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.vertex_label_vid, &wb);
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.edge_type_eid, &wb);
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.index, &wb);
  DeleteAllEntriesInColumnFamily(db_, graph_cf_.wal, &wb);

  rocksdb::WriteOptions wo;
  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db_->Write(wo, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());

  for (const auto& index : property_indexes) {
    index->ResetForBuild();
  }
  for (const auto& index : ft_indexes) {
    index->ReleaseResources();
    ResetFullTextIndexPath(index->meta().path());
    index->ResetForClear();
    if (index->IsReady()) {
      index->Start();
    }
  }
  for (const auto& index : vector_indexes) {
    index->ReleaseResources();
    ResetIndexPath(index->meta().path(), "vector index");
    index->ResetForClear();
    if (index->IsReady()) {
      index->Start();
    }
  }
}

void GraphDB::AddVertexPropertyIndex(
    const std::string& index_name, bool unique, const std::string& label,
    const std::vector<std::string>& properties) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (index_name.empty() || label.empty() || properties.empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexPropertyIndex(index_name)) {
    THROW_CODE(VertexIndexAlreadyExist, "Vertex index name {} already exists",
               index_name);
  }
  auto lid = id_generator().GetOrCreateLid(label);
  std::vector<uint32_t> pids;
  pids.reserve(properties.size());
  std::unordered_set<uint32_t> pid_set;
  for (const auto& property : properties) {
    if (property.empty()) {
      THROW_CODE(InvalidParameter);
    }
    auto pid = id_generator().GetOrCreatePid(property);
    if (!pid_set.insert(pid).second) {
      THROW_CODE(InvalidParameter, "Duplicate property [{}] in index [{}]",
                 property, index_name);
    }
    pids.push_back(pid);
  }
  CheckNoVectorFieldForNormalIndex(
      meta_info_, {lid}, std::unordered_set<uint32_t>(pids.begin(), pids.end()),
      index_name);
  if (meta_info_.GetVertexPropertyIndex(lid, pids)) {
    THROW_CODE(VertexIndexAlreadyExist,
               "Vertex index [label:{}, property_count:{}] already exists",
               big_to_native(lid), pids.size());
  }

  auto index_id = id_generator().GetNextIndexId();
  meta::VertexPropertyIndex meta_val;
  meta_val.set_name(index_name);
  meta_val.set_is_unique(unique);
  meta_val.set_label(label);
  meta_val.set_label_id(big_to_native(lid));
  meta_val.set_index_id(big_to_native(index_id));
  meta_val.set_build_start_wal_id(0);
  meta_val.set_applied_wal_id(0);
  meta_val.clear_build_error();
  for (size_t i = 0; i < properties.size(); ++i) {
    meta_val.add_properties(properties[i]);
    meta_val.add_property_ids(big_to_native(pids[i]));
  }

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::CREATE_VERTEX_PROPERTY_INDEX,
        meta_val.SerializeAsString());
    return;
  }
  ApplyCreateVertexPropertyIndex(0, std::move(meta_val));
}

void GraphDB::ApplyCreateVertexPropertyIndex(
    uint64_t apply_index, meta::VertexPropertyIndex meta_val) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta_val.name().empty() || meta_val.label().empty() ||
      meta_val.properties().empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexPropertyIndex(meta_val.name())) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    THROW_CODE(VertexIndexAlreadyExist, "Vertex index name {} already exists",
               meta_val.name());
  }
  if (meta_val.property_ids_size() != meta_val.properties_size()) {
    THROW_CODE(InvalidParameter,
               "vertex property index [{}] property id count {} does not match "
               "property count {}",
               meta_val.name(), meta_val.property_ids_size(),
               meta_val.properties_size());
  }
  auto lid = native_to_big(meta_val.label_id());
  std::vector<uint32_t> pids;
  pids.reserve(meta_val.property_ids_size());
  for (auto pid : meta_val.property_ids()) {
    pids.push_back(native_to_big(pid));
  }
  CheckNoVectorFieldForNormalIndex(
      meta_info_, {lid}, std::unordered_set<uint32_t>(pids.begin(), pids.end()),
      meta_val.name());
  if (meta_info_.GetVertexPropertyIndex(lid, pids)) {
    THROW_CODE(VertexIndexAlreadyExist,
               "Vertex index [label:{}, property_count:{}] already exists",
               meta_val.label_id(), pids.size());
  }
  auto index_id = native_to_big(meta_val.index_id());
  id_generator().ReserveIndexId(meta_val.index_id());

  auto vpi = std::make_shared<VertexPropertyIndex>(db_, &graph_cf_, meta_val,
                                                   graph_cf_.index, index_id,
                                                   lid, std::move(pids));

  vpi->SetState(meta::IndexBuildState::BUILDING);
  rocksdb::WriteBatch wb;
  auto s = wb.Put(
      graph_cf_.meta_info,
      BuildMetaKey(MetaDataType::VertexPropertyIndex, vpi->meta().name()),
      vpi->meta().SerializeAsString());
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  auto ret = meta_info_.AddVertexPropertyIndex(vpi);
  assert(ret);
  LOG_INFO(
      "Begin online build vertex index: [lid:{}, property_count:{}, "
      "is_unique:{}]",
      meta_val.label_id(), meta_val.properties_size(), meta_val.is_unique());
  ScheduleVertexPropertyIndexBuild(vpi, false);
}

void GraphDB::DeleteVertexPropertyIndex(const std::string& index_name) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  meta::VertexPropertyIndex meta;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    auto index = meta_info_.GetVertexPropertyIndex(index_name);
    if (!index) {
      THROW_CODE(VertexUniqueIndexNotFound, "No such vertex index [{}]",
                 index_name);
    }
    meta = index->meta();
  }
  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::DELETE_VERTEX_PROPERTY_INDEX,
        meta.SerializeAsString());
    return;
  }
  ApplyDeleteVertexPropertyIndex(0, meta);
}

void GraphDB::ApplyDeleteVertexPropertyIndex(
    uint64_t apply_index, const meta::VertexPropertyIndex& meta_val) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
  const auto& index_name = meta_val.name();
  auto index = meta_info_.GetVertexPropertyIndex(index_name);
  if (!index) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    THROW_CODE(VertexUniqueIndexNotFound, "No such vertex index [{}]",
               index_name);
  }
  index->MarkDeleted();
  uint32_t index_id = index->index_id();
  meta_info_.DeleteVertexPropertyIndex(index_name);

  rocksdb::WriteBatch wb;
  wb.Delete(graph_cf_.meta_info,
            BuildMetaKey(MetaDataType::VertexPropertyIndex, index_name));
  std::string start_key(AsChars(index_id), sizeof(index_id));
  std::string end_key(AsChars(index_id), sizeof(index_id));
  end_key.append(128, static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.index, start_key, end_key);
  std::string wal_start(AsChars(index_id), sizeof(index_id));
  std::string wal_end(AsChars(index_id), sizeof(index_id));
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.wal, wal_start, wal_end);
  if (apply_index > 0) {
    auto s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db_->Write({}, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  LOG_INFO("Delete vertex index: {}", index_name);
}

void GraphDB::AddVertexFullTextIndex(
    const std::string& index_name, const std::vector<std::string>& labels,
    const std::vector<std::string>& properties) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (index_name.empty() || labels.empty() || properties.empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexFullTextIndex(index_name)) {
    THROW_CODE(VertexFullTextIndexAlreadyExist,
               "Vertex fulltext index [{}] already exists", index_name);
  }
  std::unordered_set<uint32_t> lids, native_lids;
  std::unordered_set<uint32_t> pids, native_pids;
  for (const auto& label : labels) {
    auto lid = id_generator().GetOrCreateLid(label);
    lids.insert(lid);
    native_lids.insert(big_to_native(lid));
  }
  for (const auto& prop : properties) {
    auto pid = id_generator().GetOrCreatePid(prop);
    pids.insert(pid);
    native_pids.insert(big_to_native(pid));
  }
  CheckNoVectorFieldForNormalIndex(meta_info_, lids, pids, index_name);
  uint32_t index_id = id_generator().GetNextIndexId();
  meta::VertexFullTextIndex meta;
  meta.set_index_id(big_to_native(index_id));
  meta.set_name(index_name);
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();
  *meta.mutable_labels() = {labels.begin(), labels.end()};
  *meta.mutable_properties() = {properties.begin(), properties.end()};
  *meta.mutable_label_ids() = {native_lids.begin(), native_lids.end()};
  *meta.mutable_property_ids() = {native_pids.begin(), native_pids.end()};

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::CREATE_VERTEX_FULLTEXT_INDEX,
        meta.SerializeAsString());
    return;
  }
  ApplyCreateVertexFullTextIndex(0, std::move(meta));
}

void GraphDB::ApplyCreateVertexFullTextIndex(uint64_t apply_index,
                                             meta::VertexFullTextIndex meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta.name().empty() || meta.labels().empty() ||
      meta.properties().empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexFullTextIndex(meta.name())) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    THROW_CODE(VertexFullTextIndexAlreadyExist,
               "Vertex fulltext index [{}] already exists", meta.name());
  }
  if (meta.label_ids_size() != meta.labels_size() ||
      meta.property_ids_size() != meta.properties_size()) {
    THROW_CODE(InvalidParameter,
               "vertex fulltext index [{}] id counts do not match label or "
               "property counts",
               meta.name());
  }
  uint32_t index_id = native_to_big(meta.index_id());
  id_generator().ReserveIndexId(meta.index_id());
  std::unordered_set<uint32_t> lids, pids;
  std::unordered_set<uint32_t> native_lids, native_pids;
  for (auto id : meta.label_ids()) {
    native_lids.insert(id);
    lids.insert(native_to_big(id));
  }
  for (auto id : meta.property_ids()) {
    native_pids.insert(id);
    pids.insert(native_to_big(id));
  }
  CheckNoVectorFieldForNormalIndex(meta_info_, lids, pids, meta.name());
  meta.set_path(BuildFullTextIndexPath(path_, meta.name(), index_id));
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();
  DeleteFullTextIndexRanges(db_, &graph_cf_, index_id);
  ResetFullTextIndexPath(meta.path());

  auto v_ft_index = std::make_shared<VertexFullTextIndex>(
      db_, assistant_pool_->Service(), assistant_strand_.get(), &graph_cf_,
      &id_generator(), meta, index_id, options_.ft_writer_threads_,
      options_.ft_writer_memory_budget_, lids, pids,
      options_.ft_apply_interval_);
  rocksdb::WriteBatch wb;
  auto s = wb.Put(graph_cf_.meta_info,
                  BuildMetaKey(MetaDataType::VertexFullTextIndex, meta.name()),
                  meta.SerializeAsString());
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  meta_info_.AddVertexFullTextIndex(v_ft_index);
  LOG_INFO("Begin online build vertex full text index: [lids:{}, pids:{}]",
           native_lids, native_pids);
  ScheduleVertexFullTextIndexBuild(v_ft_index, false);
}

void GraphDB::DeleteVertexFullTextIndex(const std::string& index_name) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  meta::VertexFullTextIndex meta;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    auto ft_index = meta_info_.GetVertexFullTextIndex(index_name);
    if (!ft_index) {
      THROW_CODE(FullTextIndexNotFound, "No such vertex fulltext index [{}]",
                 index_name);
    }
    meta = ft_index->meta();
  }
  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::DELETE_VERTEX_FULLTEXT_INDEX,
        meta.SerializeAsString());
    return;
  }
  ApplyDeleteVertexFullTextIndex(0, meta);
}

void GraphDB::ApplyDeleteVertexFullTextIndex(
    uint64_t apply_index, const meta::VertexFullTextIndex& meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::lock_guard<std::mutex> commit_lock(fulltext_index_commit_mutex_);
  const auto& index_name = meta.name();
  auto ft_index = meta_info_.GetVertexFullTextIndex(index_name);
  if (!ft_index) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    THROW_CODE(FullTextIndexNotFound, "No such vertex fulltext index [{}]",
               index_name);
  }
  std::string path = ft_index->meta().path();
  uint32_t index_id = ft_index->index_id();
  ft_index->MarkDeleted();
  ft_index->Stop();
  ft_index->ReleaseResources();
  meta_info_.DeleteVertexFullTextIndex(index_name);

  rocksdb::WriteBatch wb;
  wb.Delete(graph_cf_.meta_info,
            BuildMetaKey(MetaDataType::VertexFullTextIndex, index_name));

  std::string start_key(AsChars(index_id), sizeof(index_id));
  start_key.append(sizeof(int64_t), static_cast<char>(0x00));
  std::string end_key(AsChars(index_id), sizeof(index_id));
  end_key.append(sizeof(int64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.index, start_key, end_key);
  wb.DeleteRange(graph_cf_.wal, start_key, end_key);
  if (apply_index > 0) {
    auto s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db_->Write({}, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  try {
    std::filesystem::remove_all(path);
    LOG_INFO("remove fulltext index data {}", path);
  } catch (const std::exception& e) {
    LOG_ERROR("[DeleteVertexFullTextIndex] remove_all {} meet error: {}", path,
              e.what());
  }
}

void GraphDB::AddVertexVectorIndex(const std::string& index_name,
                                   const std::string& label,
                                   const std::string& property, int dimension,
                                   std::string distance_type, int hnsw_m,
                                   int hnsw_ef_construction) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (index_name.empty() || label.empty() || property.empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (dimension < 1 || dimension > 4096) {
    THROW_CODE(InvalidParameter,
               "dimension should be an integer in the range [1, 4096]");
  }
  if (hnsw_m < 5 || hnsw_m > 64) {
    THROW_CODE(InvalidParameter,
               "hnsw.m should be an integer in the range [5, 64]");
  }
  if (hnsw_ef_construction < hnsw_m || hnsw_ef_construction > 1000) {
    THROW_CODE(
        InvalidParameter,
        "hnsw.efConstruction should be an integer in the range [hnsw.m,1000]");
  }
  meta::VectorDistanceType dist_type;
  if (distance_type == "l2") {
    dist_type = meta::VectorDistanceType::L2;
  } else if (distance_type == "ip") {
    dist_type = meta::VectorDistanceType::IP;
  } else {
    THROW_CODE(InvalidParameter, "Distance Type {} not supported",
               distance_type);
  }
  if (meta_info_.GetVertexVectorIndex(index_name)) {
    THROW_CODE(VertexVectorIndexAlreadyExist,
               "Vertex vector index [{}] already exists", index_name);
  }
  auto lid_opt = id_generator().GetLid(label);
  auto pid_opt = id_generator().GetPid(property);
  if (!lid_opt || !pid_opt) {
    THROW_CODE(InvalidParameter,
               "Vector field [label:{}, property:{}] is not defined", label,
               property);
  }
  auto lid = lid_opt.value();
  auto pid = pid_opt.value();
  auto vector_field = meta_info_.GetVertexVectorField(lid, pid);
  if (!vector_field) {
    THROW_CODE(InvalidParameter,
               "Vector field [label:{}, property:{}] is not defined", label,
               property);
  }
  if (vector_field->dimensions() != static_cast<uint32_t>(dimension)) {
    THROW_CODE(InvalidParameter,
               "Vector field [label:{}, property:{}] dimension mismatch, "
               "expect {}, actual {}",
               label, property, vector_field->dimensions(), dimension);
  }
  if (meta_info_.GetVertexVectorIndex(lid, pid)) {
    THROW_CODE(VertexVectorIndexAlreadyExist,
               "Vertex vector index [label:{}, property:{}] already exists",
               big_to_native(lid), big_to_native(pid));
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, label, property);
  uint32_t index_id = id_generator().GetNextIndexId();
  meta::VectorIndexType index_type = meta::VectorIndexType::HNSW;
  meta::VertexVectorIndex meta;
  meta.set_index_id(big_to_native(index_id));
  meta.set_name(index_name);
  meta.set_label(label);
  meta.set_property(property);
  meta.set_label_id(big_to_native(lid));
  meta.set_property_id(big_to_native(pid));
  meta.set_dimensions(dimension);
  meta.set_index_type(index_type);
  meta.set_distance_type(dist_type);
  meta.set_hnsw_m(hnsw_m);
  meta.set_hnsw_ef_construction(hnsw_ef_construction);
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_INDEX,
                         meta.SerializeAsString());
    return;
  }
  ApplyCreateVertexVectorIndex(0, std::move(meta));
}

void GraphDB::AddVertexVectorField(const std::string& label,
                                   const std::string& property, int dimension) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (label.empty() || property.empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (dimension < 1 || dimension > 4096) {
    THROW_CODE(InvalidParameter,
               "dimension should be an integer in the range [1, 4096]");
  }

  auto lid = id_generator().GetOrCreateLid(label);
  auto pid = id_generator().GetOrCreatePid(property);
  if (meta_info_.GetVertexVectorField(lid, pid)) {
    THROW_CODE(InvalidParameter,
               "Vector field [label:{}, property:{}] already exists", label,
               property);
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, label, property);

  meta::VertexVectorField meta;
  meta.set_label(label);
  meta.set_label_id(big_to_native(lid));
  meta.set_property(property);
  meta.set_property_id(big_to_native(pid));
  meta.set_dimensions(dimension);

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_FIELD,
                         meta.SerializeAsString());
    return;
  }
  ApplyCreateVertexVectorField(0, std::move(meta));
}

void GraphDB::ApplyCreateVertexVectorField(uint64_t apply_index,
                                           meta::VertexVectorField meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta.label().empty() || meta.property().empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (meta.dimensions() < 1 || meta.dimensions() > 4096) {
    THROW_CODE(InvalidParameter,
               "dimension should be an integer in the range [1, 4096]");
  }

  auto lid = native_to_big(meta.label_id());
  auto pid = native_to_big(meta.property_id());
  auto existing_field = meta_info_.GetVertexVectorField(lid, pid);
  if (existing_field) {
    if (apply_index > 0 && existing_field->dimensions() == meta.dimensions()) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    THROW_CODE(InvalidParameter,
               "Vector field [label:{}, property:{}] already exists",
               meta.label(), meta.property());
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, meta.label(),
                                   meta.property());

  auto vector_field =
      std::make_shared<meta::VertexVectorField>(std::move(meta));
  rocksdb::WriteBatch wb;
  auto s =
      wb.Put(graph_cf_.meta_info,
             BuildMetaKey(MetaDataType::VertexVectorField,
                          BuildVectorFieldMetaKey(vector_field->label(),
                                                  vector_field->property())),
             vector_field->SerializeAsString());
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  meta_info_.AddVertexVectorField(vector_field);
}

void GraphDB::ApplyCreateVertexVectorIndex(uint64_t apply_index,
                                           meta::VertexVectorIndex meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta.name().empty() || meta.label().empty() || meta.property().empty()) {
    THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexVectorIndex(meta.name())) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    THROW_CODE(VertexVectorIndexAlreadyExist,
               "Vertex vector index [{}] already exists", meta.name());
  }
  auto lid = native_to_big(meta.label_id());
  auto pid = native_to_big(meta.property_id());
  if (meta_info_.GetVertexVectorIndex(lid, pid)) {
    THROW_CODE(VertexVectorIndexAlreadyExist,
               "Vertex vector index [label:{}, property:{}] already exists",
               meta.label_id(), meta.property_id());
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, meta.label(),
                                   meta.property());
  auto existing_field = meta_info_.GetVertexVectorField(lid, pid);
  if (existing_field) {
    if (existing_field->dimensions() != meta.dimensions()) {
      THROW_CODE(InvalidParameter,
                 "Vector field [label:{}, property:{}] dimension mismatch, "
                 "expect {}, actual {}",
                 meta.label(), meta.property(), existing_field->dimensions(),
                 meta.dimensions());
    }
  } else {
    THROW_CODE(InvalidParameter,
               "Vector field [label:{}, property:{}] is not defined",
               meta.label(), meta.property());
  }
  uint32_t index_id = native_to_big(meta.index_id());
  id_generator().ReserveIndexId(meta.index_id());
  meta.set_path(path_ + "/vt/" + meta.name());
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();
  DeleteVectorIndexRanges(db_, &graph_cf_, index_id);
  ResetIndexPath(meta.path(), "vector index");

  auto vvi = std::make_shared<VertexVectorIndex>(
      db_, assistant_pool_->Service(), assistant_strand_.get(), &graph_cf_,
      index_id, lid, pid, meta, options_.vt_apply_interval_);
  rocksdb::WriteBatch wb;
  auto s = wb.Put(graph_cf_.meta_info,
                  BuildMetaKey(MetaDataType::VertexVectorIndex, meta.name()),
                  meta.SerializeAsString());
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  meta_info_.AddVertexVectorIndex(vvi);
  LOG_INFO("Begin online build vertex vector index: [lid:{}, pid:{}]",
           meta.label_id(), meta.property_id());
  ScheduleVertexVectorIndexBuild(vvi, false);
}

void GraphDB::DeleteVertexVectorIndex(const std::string& index_name) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  meta::VertexVectorIndex meta;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    auto index = meta_info_.GetVertexVectorIndex(index_name);
    if (!index) {
      THROW_CODE(VectorIndexNotFound, "No such vertex vector index [{}]",
                 index_name);
    }
    meta = index->meta();
  }
  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::DELETE_VERTEX_VECTOR_INDEX,
                         meta.SerializeAsString());
    return;
  }
  ApplyDeleteVertexVectorIndex(0, meta);
}

void GraphDB::ApplyDeleteVertexVectorIndex(
    uint64_t apply_index, const meta::VertexVectorIndex& meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::lock_guard<std::mutex> commit_lock(vector_index_commit_mutex_);
  const auto& index_name = meta.name();
  auto index = meta_info_.GetVertexVectorIndex(index_name);
  if (!index) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    THROW_CODE(VectorIndexNotFound, "No such vertex vector index [{}]",
               index_name);
  }
  auto path = index->meta().path();
  uint32_t index_id = index->index_id();
  index->MarkDeleted();
  index->Stop();
  index->ReleaseResources();
  meta_info_.DeleteVertexVectorIndex(index_name);
  rocksdb::WriteBatch wb;
  wb.Delete(graph_cf_.meta_info,
            BuildMetaKey(MetaDataType::VertexVectorIndex, index_name));

  std::string wal_start(AsChars(index_id), sizeof(index_id));
  std::string wal_end(AsChars(index_id), sizeof(index_id));
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.wal, wal_start, wal_end);
  if (apply_index > 0) {
    auto s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db_->Write({}, two, &wb);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());

  try {
    std::filesystem::remove_all(path);
    LOG_INFO("remove vector index data {}", path);
  } catch (const std::exception& e) {
    LOG_ERROR("[DeleteVertexVectorIndex] remove_all {} meet error: {}", path,
              e.what());
  }
}

}  // namespace graphdb
