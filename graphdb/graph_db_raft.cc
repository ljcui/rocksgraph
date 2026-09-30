//
// Created by botu.wzy
//

#include <rocksdb/utilities/checkpoint.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>

#include "common/byte_utils.h"
#include "common/exception.h"
#include "common/logger.h"
#include "graph_db.h"
#include "graph_db_internal.h"

using common::AsChars;
using common::ReadValue;

namespace graphdb {
namespace {

constexpr uint32_t kSnapshotFormatVersion = 1;

std::pair<uint32_t, rocksdb::Status> SnapshotFileChecksum(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {0, rocksdb::Status::IOError("failed to open " + path.string())};
  }
  uint32_t crc = 0xffffffffU;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const auto count = input.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      crc ^= static_cast<unsigned char>(buffer[static_cast<size_t>(i)]);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
      }
    }
  }
  if (!input.eof()) {
    return {0, rocksdb::Status::IOError("failed to read " + path.string())};
  }
  return {~crc, rocksdb::Status::OK()};
}

const std::string kRaftApplyIndexKey(
    1, static_cast<char>(MetadataType::RaftApplyIndex));
const std::string kRaftConfStateKey(
    1, static_cast<char>(MetadataType::RaftConfState));
const std::string kRaftNodeInfosKey(
    1, static_cast<char>(MetadataType::RaftNodeInfos));

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
    auto type = static_cast<MetadataType>(key.data()[0]);
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

}  // namespace

raft::SnapshotBuildResult GraphDB::CreateRaftSnapshot() {
  std::lock_guard<std::mutex> snapshot_lock(snapshot_restore_mutex_);
  raft::SnapshotBuildResult result;
  const auto index = GetRaftApplyIndex();
  auto conf_state = GetRaftConfState();
  auto node_infos = GetRaftNodeInfos();
  if (index == 0 || !conf_state.has_value() || !node_infos.has_value()) {
    result.err = eraft::Error(
        "graph has no applied Raft state available for a snapshot");
    return result;
  }

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string snapshot_id =
      std::to_string(index) + "-" + std::to_string(nonce);
  const auto root =
      std::filesystem::path(path_) / "snapshots" / "outgoing" / snapshot_id;
  const auto checkpoint_path = root / "data";
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    result.err = eraft::Error("failed to create graph snapshot directory: " +
                              ec.message());
    return result;
  }

  rocksdb::Checkpoint* raw_checkpoint = nullptr;
  auto status = rocksdb::Checkpoint::Create(db_->GetBaseDB(), &raw_checkpoint);
  if (!status.ok()) {
    result.err = eraft::Error("failed to initialize RocksDB checkpoint: " +
                              status.ToString());
    return result;
  }
  std::unique_ptr<rocksdb::Checkpoint> checkpoint(raw_checkpoint);
  status = checkpoint->CreateCheckpoint(checkpoint_path.string());
  if (!status.ok()) {
    result.err = eraft::Error("failed to create RocksDB checkpoint: " +
                              status.ToString());
    return result;
  }

  auto* descriptor = &result.descriptor;
  descriptor->set_format_version(kSnapshotFormatVersion);
  descriptor->set_snapshot_id(snapshot_id);
  descriptor->set_graph(db_meta_.graph_name());
  descriptor->set_index(index);
  *descriptor->mutable_node_infos() = *node_infos;
  uint64_t total_size = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(checkpoint_path)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto relative =
        std::filesystem::relative(entry.path(), checkpoint_path)
            .generic_string();
    auto [checksum, checksum_status] = SnapshotFileChecksum(entry.path());
    if (!checksum_status.ok()) {
      result.err = eraft::Error(checksum_status.ToString());
      return result;
    }
    auto* file = descriptor->add_files();
    file->set_name(relative);
    file->set_size(entry.file_size());
    file->set_checksum(checksum);
    total_size += entry.file_size();
  }
  descriptor->set_total_size(total_size);
  result.conf_state = std::move(*conf_state);
  return result;
}

void GraphDB::ApplyRaftSnapshot(
    const meta::GraphSnapshotDescriptor& descriptor) {
  RG_CHECK(descriptor.format_version() == kSnapshotFormatVersion,
           common::ErrorCode::InvalidParameter,
           "unsupported graph snapshot format {}", descriptor.format_version());
  RG_CHECK(descriptor.graph() == db_meta_.graph_name(),
           common::ErrorCode::InvalidParameter,
           "snapshot graph [{}] does not match graph [{}]", descriptor.graph(),
           db_meta_.graph_name());

  std::lock_guard<std::mutex> snapshot_lock(snapshot_restore_mutex_);
  std::unique_lock<std::shared_mutex> storage_lock(storage_mutex_);
  const auto snapshot_root = std::filesystem::path(path_) / "snapshots" /
                             "incoming" / descriptor.snapshot_id();
  const auto incoming_data = snapshot_root / "data";
  RG_CHECK(std::filesystem::exists(incoming_data),
           common::ErrorCode::StorageEngineError,
           "incoming graph snapshot [{}] is missing", descriptor.snapshot_id());

  std::vector<std::shared_ptr<VertexFullTextIndex>> fulltext_indexes;
  std::vector<std::shared_ptr<VertexVectorIndex>> vector_indexes;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    fulltext_indexes = meta_info_.GetVertexFullTextIndexes(true);
    vector_indexes = meta_info_.GetVertexVectorIndexes(true);
  }
  for (const auto& index : fulltext_indexes) {
    index->Stop();
  }
  for (const auto& index : vector_indexes) {
    index->Stop();
  }
  DrainAssistant();
  meta_info_.Reset();

  const auto data_path = std::filesystem::path(path_) / "data";
  const auto backup_path =
      std::filesystem::path(path_) / "data.before_snapshot";
  std::error_code ec;
  std::filesystem::remove_all(backup_path, ec);
  RG_CHECK(!ec, common::ErrorCode::StorageEngineError,
           "failed to clear graph snapshot backup: {}", ec.message());
  auto close_status = CloseStorage();
  RG_CHECK(close_status.ok(), common::ErrorCode::StorageEngineError,
           "failed to close graph before installing snapshot: {}",
           close_status.ToString());
  std::filesystem::rename(data_path, backup_path, ec);
  if (ec) {
    OpenStorage();
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to preserve current graph data: {}", ec.message());
  }
  std::filesystem::rename(incoming_data, data_path, ec);
  if (ec) {
    std::error_code rollback_ec;
    std::filesystem::rename(backup_path, data_path, rollback_ec);
    if (!rollback_ec) {
      OpenStorage();
    }
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to install graph snapshot: {}", ec.message());
  }

  try {
    OpenStorage(false);
    rocksdb::WriteBatch wb;
    std::unique_ptr<rocksdb::Iterator> iter(
        db_->NewIterator({}, graph_cf_.meta_info));
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      if (iter->key().empty()) {
        continue;
      }
      const auto type = static_cast<MetadataType>(iter->key().data()[0]);
      if (type == MetadataType::VertexFullTextIndex) {
        meta::VertexFullTextIndex meta;
        RG_CHECK(
            meta.ParseFromArray(iter->value().data(), iter->value().size()),
            common::ErrorCode::StorageEngineError,
            "failed to parse fulltext index metadata from snapshot");
        meta.set_path(internal::BuildFullTextIndexPath(path_, meta.name(),
                                                       meta.index_id()));
        meta.set_state(meta::IndexBuildState::BUILDING);
        meta.set_build_start_wal_id(0);
        meta.set_applied_wal_id(0);
        meta.clear_build_error();
        wb.Put(graph_cf_.meta_info, iter->key(), meta.SerializeAsString());
      } else if (type == MetadataType::VertexVectorIndex) {
        meta::VertexVectorIndex meta;
        RG_CHECK(
            meta.ParseFromArray(iter->value().data(), iter->value().size()),
            common::ErrorCode::StorageEngineError,
            "failed to parse vector index metadata from snapshot");
        meta.set_path(path_ + "/vt/" + meta.name());
        meta.set_state(meta::IndexBuildState::BUILDING);
        meta.set_build_start_wal_id(0);
        meta.set_applied_wal_id(0);
        meta.clear_build_error();
        wb.Put(graph_cf_.meta_info, iter->key(), meta.SerializeAsString());
      }
    }
    RG_CHECK(iter->status().ok(), common::ErrorCode::StorageEngineError,
             "failed to scan snapshot metadata: {}", iter->status().ToString());
    auto status = db_->Write({}, &wb);
    RG_CHECK(status.ok(), common::ErrorCode::StorageEngineError,
             "failed to normalize snapshot index metadata: {}",
             status.ToString());
    std::filesystem::remove_all(std::filesystem::path(path_) / "ft", ec);
    ec.clear();
    std::filesystem::remove_all(std::filesystem::path(path_) / "vt", ec);
    InitializeMetaInfo();
    meta_info_.id_generator().SetRaftDriver(raft_driver());
    RefreshPlanCacheIdentity();
    std::filesystem::remove_all(backup_path, ec);
    std::filesystem::remove_all(snapshot_root, ec);
  } catch (...) {
    if (db_ != nullptr) {
      meta_info_.Reset();
      auto status = CloseStorage();
      if (!status.ok()) {
        LOG_WARN("failed to close graph after snapshot restore error: {}",
                 status.ToString());
      }
    }
    std::filesystem::remove_all(data_path, ec);
    ec.clear();
    std::filesystem::rename(backup_path, data_path, ec);
    if (!ec) {
      OpenStorage();
      meta_info_.id_generator().SetRaftDriver(raft_driver());
    }
    throw;
  }
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
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to load raft apply index: {}", s.ToString());
  }
  if (val.size() != sizeof(uint64_t)) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "raft apply index has invalid size, expect {}, actual {}",
             sizeof(uint64_t), val.size());
  }
  return ReadValue<uint64_t>(val.data());
}

std::optional<raftpb::ConfState> GraphDB::GetRaftConfState() const {
  std::string val;
  auto s = db_->Get({}, graph_cf_.meta_info, kRaftConfStateKey, &val);
  if (s.IsNotFound()) {
    return std::nullopt;
  }
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to load raft conf state: {}", s.ToString());
  }
  raftpb::ConfState conf_state;
  if (!conf_state.ParseFromString(val)) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to parse raft conf state for graph [{}]",
             db_meta_.graph_name());
  }
  return conf_state;
}

std::optional<meta::RaftNodeInfos> GraphDB::GetRaftNodeInfos() const {
  std::string val;
  auto s = db_->Get({}, graph_cf_.meta_info, kRaftNodeInfosKey, &val);
  if (s.IsNotFound()) {
    return std::nullopt;
  }
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to load raft node infos: {}", s.ToString());
  }
  meta::RaftNodeInfos node_infos;
  if (!node_infos.ParseFromString(val)) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to parse raft node infos for graph [{}]",
             db_meta_.graph_name());
  }
  return node_infos;
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
        RG_THROW(common::ErrorCode::InvalidParameter,
                 "failed to parse graph index ddl request for graph [{}] at "
                 "index {}",
                 db_meta_.graph_name(), index);
      }
      ApplyGraphIndexDdlRequest(index, ddl_request);
      return;
    }
    case meta::WriteBatchKind::UNKNOWN:
      if (request.wb_data().empty()) {
        rocksdb::WriteBatch wb;
        auto s = SetRaftApplyIndex(index, &wb);
        if (!s.ok()) {
          RG_THROW(common::ErrorCode::StorageEngineError,
                   "failed to persist raft apply index for graph [{}] at "
                   "index {}: {}",
                   db_meta_.graph_name(), index, s.ToString());
        }
        s = db_->Write({}, &wb);
        if (!s.ok()) {
          RG_THROW(common::ErrorCode::StorageEngineError,
                   "failed to persist raft noop for graph [{}] at index {}: "
                   "{}",
                   db_meta_.graph_name(), index, s.ToString());
        }
        return;
      }
      RG_THROW(common::ErrorCode::InvalidParameter,
               "write batch kind must be specified for graph [{}] at index "
               "{}",
               db_meta_.graph_name(), index);
    default:
      RG_THROW(common::ErrorCode::InvalidParameter,
               "unsupported write batch kind {} for graph [{}] at index {}",
               static_cast<int>(request.wb_kind()), db_meta_.graph_name(),
               index);
  }
}

void GraphDB::ApplyRaftConfChange(uint64_t index,
                                  const raftpb::ConfState& conf_state,
                                  const meta::RaftNodeInfos& node_infos) {
  std::string conf_state_data;
  if (!conf_state.SerializeToString(&conf_state_data)) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to serialize raft conf state for graph [{}]",
             db_meta_.graph_name());
  }
  std::string node_infos_data;
  if (!node_infos.SerializeToString(&node_infos_data)) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to serialize raft node infos for graph [{}]",
             db_meta_.graph_name());
  }
  rocksdb::WriteBatch wb;
  auto s = wb.Put(graph_cf_.meta_info, kRaftConfStateKey, conf_state_data);
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to prepare raft conf state for graph [{}]: {}",
             db_meta_.graph_name(), s.ToString());
  }
  s = wb.Put(graph_cf_.meta_info, kRaftNodeInfosKey, node_infos_data);
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to prepare raft node infos for graph [{}]: {}",
             db_meta_.graph_name(), s.ToString());
  }
  s = SetRaftApplyIndex(index, &wb);
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to prepare raft apply index for graph [{}] at index {}: "
             "{}",
             db_meta_.graph_name(), index, s.ToString());
  }
  s = db_->Write({}, &wb);
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to apply raft conf change for graph [{}] at index {}: "
             "{}",
             db_meta_.graph_name(), index, s.ToString());
  }
}

void GraphDB::ApplyRaftWriteBatch(uint64_t index,
                                  const meta::RaftRequest& request) {
  rocksdb::WriteBatch wb(request.wb_data());

  auto s = SetRaftApplyIndex(index, &wb);
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to persist raft apply index for graph [{}] at index {}: "
             "{}",
             db_meta_.graph_name(), index, s.ToString());
  }

  auto* base_db = db_->GetBaseDB();
  if (!base_db) {
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to access base rocksdb::DB for graph [{}]",
             db_meta_.graph_name());
  }

  s = base_db->Write({}, &wb);
  if (!s.ok()) {
    RG_THROW(common::ErrorCode::StorageEngineError,
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
      RG_THROW(common::ErrorCode::InvalidParameter,
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
    RG_THROW(common::ErrorCode::StorageEngineError,
             "failed to sync id generator cache from raft batch for graph "
             "[{}]: {}",
             db_meta_.graph_name(), s.ToString());
  }
}

void GraphDB::ProposeGraphIndexDdl(
    meta::GraphIndexDdlRequest::Operation operation, std::string payload) {
  auto* driver = raft_driver();
  if (driver == nullptr) {
    RG_THROW(common::ErrorCode::StorageEngineError,
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
    RG_THROW(common::ErrorCode::StorageEngineError, apply_result.err.String());
  }
}

void GraphDB::ApplyGraphIndexDdlRequest(
    uint64_t index, const meta::GraphIndexDdlRequest& request) {
  switch (request.operation()) {
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_PROPERTY_INDEX: {
      meta::VertexPropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW(common::ErrorCode::InvalidParameter,
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
        RG_THROW(common::ErrorCode::InvalidParameter,
                 "failed to parse delete vertex property index request for "
                 "graph [{}] at index {}",
                 db_meta_.graph_name(), index);
      }
      ApplyDeleteVertexPropertyIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_EDGE_PROPERTY_INDEX: {
      meta::EdgePropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW(common::ErrorCode::InvalidParameter,
                 "failed to parse create edge property index request for "
                 "graph [{}] at index {}",
                 db_meta_.graph_name(), index);
      }
      ApplyCreateEdgePropertyIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_EDGE_PROPERTY_INDEX: {
      meta::EdgePropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW(common::ErrorCode::InvalidParameter,
                 "failed to parse delete edge property index request for "
                 "graph [{}] at index {}",
                 db_meta_.graph_name(), index);
      }
      ApplyDeleteEdgePropertyIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_FULLTEXT_INDEX: {
      meta::VertexFullTextIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW(common::ErrorCode::InvalidParameter,
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
        RG_THROW(common::ErrorCode::InvalidParameter,
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
        RG_THROW(common::ErrorCode::InvalidParameter,
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
        RG_THROW(common::ErrorCode::InvalidParameter,
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
        RG_THROW(common::ErrorCode::InvalidParameter,
                 "failed to parse create vertex vector field request for "
                 "graph [{}] at index {}",
                 db_meta_.graph_name(), index);
      }
      ApplyCreateVertexVectorField(index, std::move(meta));
      return;
    }
    default:
      RG_THROW(common::ErrorCode::InvalidParameter,
               "unsupported graph index ddl operation {} for graph [{}] at "
               "index {}",
               static_cast<int>(request.operation()), db_meta_.graph_name(),
               index);
  }
}

}  // namespace graphdb
