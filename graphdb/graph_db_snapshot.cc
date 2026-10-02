#include <rocksdb/utilities/checkpoint.h>

#include <filesystem>

#include "common/exception.h"
#include "graphdb/graph_db.h"
#include "graphdb/graph_db_internal.h"
#include "raft_driver/snapshot.h"

namespace graphdb {

void GraphDB::RecoverSnapshotData() {
  namespace fs = std::filesystem;
  const auto record_path = fs::path(path_) / "snapshot_install.pb";
  if (!fs::exists(record_path)) return;
  meta::SnapshotInstall record;
  RG_CHECK(record.ParseFromString(raft::ReadSnapshotRecord(record_path)),
           common::ErrorCode::StorageEngineError,
           "invalid snapshot install record");
  raft::ParseSnapshotReference(record.snapshot());
  fs::path relative(record.directory());
  RG_CHECK(relative.parent_path() == fs::path("snapshots/in"),
           common::ErrorCode::StorageEngineError,
           "invalid snapshot staging path");
  raft::CheckSnapshotFileName(relative.filename().string());
  const auto staged = fs::path(path_) / relative;
  const auto data = fs::path(path_) / "data";
  const auto previous = fs::path(path_) / "data.previous";
  if (fs::exists(staged)) {
    if (fs::exists(data)) {
      fs::remove_all(previous);
      fs::rename(data, previous);
      raft::SyncDirectory(path_);
    }
    fs::rename(staged, data);
    raft::SyncDirectory(staged.parent_path());
    raft::SyncDirectory(path_);
  }
  RG_CHECK(fs::exists(data / "CURRENT"), common::ErrorCode::StorageEngineError,
           "snapshot data directory is missing");
}

std::shared_ptr<raft::SnapshotFiles> GraphDB::CreateRaftSnapshot(
    const std::string& directory) {
  auto snapshot = std::make_shared<raft::SnapshotFiles>();
  snapshot->directory = directory;
  auto index = GetRaftApplyIndex();
  auto state = GetRaftConfState();
  auto infos = GetRaftNodeInfos();
  RG_CHECK(index > 0 && state && infos, common::ErrorCode::StorageEngineError,
           "graph has no applied Raft state to snapshot");
  snapshot->snapshot.mutable_metadata()->set_index(index);
  *snapshot->snapshot.mutable_metadata()->mutable_conf_state() = *state;
  meta::SnapshotReference reference;
  reference.set_version(raft::kSnapshotVersion);
  reference.set_id(snapshot->directory.filename().string());
  *reference.mutable_node_infos() = *infos;
  snapshot->snapshot.set_data(reference.SerializeAsString());
  std::filesystem::create_directories(snapshot->directory.parent_path());
  rocksdb::Checkpoint* raw_checkpoint = nullptr;
  auto status = rocksdb::Checkpoint::Create(db_->GetBaseDB(), &raw_checkpoint);
  RG_CHECK(status.ok(), common::ErrorCode::StorageEngineError,
           status.ToString());
  std::unique_ptr<rocksdb::Checkpoint> checkpoint(raw_checkpoint);
  // Called behind the driver's per-graph apply barrier. No later Raft write
  // can enter the checkpoint, so metadata and database contents describe S.
  status = checkpoint->CreateCheckpoint(directory);
  RG_CHECK(status.ok(), common::ErrorCode::StorageEngineError,
           status.ToString());
  return snapshot;
}

void GraphDB::PrepareSnapshotIndexes() {
  const auto record_path = std::filesystem::path(path_) / "snapshot_install.pb";
  meta::SnapshotInstall record;
  RG_CHECK(record.ParseFromString(raft::ReadSnapshotRecord(record_path)),
           common::ErrorCode::StorageEngineError,
           "invalid snapshot install record");
  auto reference = raft::ParseSnapshotReference(record.snapshot());
  auto state = GetRaftConfStateUnlocked();
  auto infos = GetRaftNodeInfosUnlocked();
  RG_CHECK(
      GetRaftApplyIndexUnlocked() == record.snapshot().metadata().index() &&
          state && infos &&
          raftpb::Equivalent(
              *state, record.snapshot().metadata().conf_state()) == nullptr,
      common::ErrorCode::StorageEngineError,
      "checkpoint does not match snapshot metadata");
  for (const auto& [id, info] : reference.node_infos().nodes()) {
    auto iter = infos->nodes().find(id);
    RG_CHECK(iter != infos->nodes().end() &&
                 iter->second.SerializeAsString() == info.SerializeAsString(),
             common::ErrorCode::StorageEngineError,
             "checkpoint member addresses do not match");
  }
  rocksdb::WriteBatch batch;
  std::unique_ptr<rocksdb::Iterator> iter(
      db_->NewIterator({}, graph_cf_.meta_info));
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    if (iter->key().empty()) continue;
    auto type = static_cast<MetadataType>(iter->key()[0]);
    if (type == MetadataType::VertexFullTextIndex) {
      meta::VertexFullTextIndex meta;
      RG_CHECK(meta.ParseFromString(iter->value().ToString()),
               common::ErrorCode::StorageEngineError,
               "invalid fulltext index metadata");
      meta.set_path(internal::BuildFullTextIndexPath(path_, meta.name(),
                                                     meta.index_id()));
      meta.set_state(meta::BUILDING);
      meta.set_build_start_wal_id(0);
      meta.set_applied_wal_id(0);
      meta.clear_build_error();
      batch.Put(graph_cf_.meta_info, iter->key(), meta.SerializeAsString());
    } else if (type == MetadataType::VertexVectorIndex) {
      meta::VertexVectorIndex meta;
      RG_CHECK(meta.ParseFromString(iter->value().ToString()),
               common::ErrorCode::StorageEngineError,
               "invalid vector index metadata");
      meta.set_path(path_ + "/vt/" + meta.name());
      meta.set_state(meta::BUILDING);
      meta.set_build_start_wal_id(0);
      meta.set_applied_wal_id(0);
      meta.clear_build_error();
      batch.Put(graph_cf_.meta_info, iter->key(), meta.SerializeAsString());
    }
  }
  RG_CHECK(iter->status().ok(), common::ErrorCode::StorageEngineError,
           iter->status().ToString());
  rocksdb::WriteOptions options;
  options.sync = true;
  auto status = db_->Write(options, &batch);
  RG_CHECK(status.ok(), common::ErrorCode::StorageEngineError,
           status.ToString());
}

void GraphDB::InstallRaftSnapshot(
    const std::shared_ptr<raft::SnapshotFiles>& snapshot) {
  recovering_.store(true);
  // Leases are released after cursors and RocksDB transactions are destroyed.
  // New transactions fail while installation waits for existing leases.
  std::unique_lock guard(data_mutex_);
  data_condition_.wait(guard, [this]() { return active_transactions_ == 0; });
  CloseData();
  RecoverSnapshotData();
  OpenData();
}

uint64_t GraphDB::SyncRaftState() {
  auto status = db_->FlushWAL(true);
  RG_CHECK(status.ok(), common::ErrorCode::StorageEngineError,
           status.ToString());
  return GetRaftApplyIndex();
}

void GraphDB::MarkRaftReady(uint64_t target_index, uint64_t local_id) {
  recovering_.store(false);
  if (!joining_.load() || GetRaftApplyIndex() < target_index) return;
  auto state = GetRaftConfState();
  if (!state) return;
  bool member = std::find(state->voters().begin(), state->voters().end(),
                          local_id) != state->voters().end() ||
                std::find(state->learners().begin(), state->learners().end(),
                          local_id) != state->learners().end();
  if (!member) return;
  SyncRaftState();
  raft::RemoveSnapshotRecord(std::filesystem::path(path_) / "raft_join.pb");
  joining_.store(false);
}

}  // namespace graphdb
