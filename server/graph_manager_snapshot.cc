#include <rocksdb/utilities/checkpoint.h>

#include <boost/endian/conversion.hpp>
#include <chrono>
#include <filesystem>
#include <limits>
#include <thread>

#include "common/byte_utils.h"
#include "common/exception.h"
#include "common/logger.h"
#include "graphdb/graph_db_internal.h"
#include "raft_driver/raft_log_store.h"
#include "runtime/plan_cache.h"
#include "server/graph_manager.h"

namespace server {
namespace {
namespace fs = std::filesystem;

void CheckStorage(const rocksdb::Status& status) {
  RG_CHECK(status.ok(), common::ErrorCode::StorageEngineError,
           status.ToString());
}

// Open the checkpoint without constructing indexes or starting background work.
class SnapshotDB {
 public:
  explicit SnapshotDB(const std::string& path) {
    std::vector<std::string> names;
    CheckStorage(rocksdb::DB::ListColumnFamilies({}, path, &names));
    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    for (const auto& name : names)
      descriptors.emplace_back(name, rocksdb::Options{});
    CheckStorage(rocksdb::DB::Open(rocksdb::DBOptions{}, path, descriptors,
                                   &handles_, &db_));
    for (size_t i = 0; i < names.size(); ++i)
      columns_.emplace(names[i], handles_[i]);
  }
  ~SnapshotDB() {
    for (auto* handle : handles_) db_->DestroyColumnFamilyHandle(handle);
  }
  rocksdb::DB* db() { return db_.get(); }
  rocksdb::ColumnFamilyHandle* column(const std::string& name) {
    RG_CHECK(columns_.contains(name), common::ErrorCode::InvalidParameter,
             "snapshot is missing column family [{}]", name);
    return columns_.at(name);
  }
  std::string Get(graphdb::MetadataType type) {
    std::string value;
    CheckStorage(db_->Get({}, column("meta_info"),
                          std::string(1, static_cast<char>(type)), &value));
    return value;
  }

 private:
  std::unique_ptr<rocksdb::DB> db_;
  std::vector<rocksdb::ColumnFamilyHandle*> handles_;
  std::unordered_map<std::string, rocksdb::ColumnFamilyHandle*> columns_;
};

std::string GraphMetaKey(uint64_t id) {
  boost::endian::native_to_big_inplace(id);
  return std::string(1, static_cast<char>(GraphManagerMetadataType::GraphDB)) +
         std::string(reinterpret_cast<const char*>(&id), sizeof(id));
}

void RebuildExternalIndexes(SnapshotDB& checkpoint, const std::string& path) {
  rocksdb::WriteBatch batch;
  std::unique_ptr<rocksdb::Iterator> iter(
      checkpoint.db()->NewIterator({}, checkpoint.column("meta_info")));
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    if (iter->key().empty()) continue;
    auto type = static_cast<graphdb::MetadataType>(iter->key()[0]);
    auto rebuild = [&](auto& meta, const std::string& directory) {
      meta.set_path(directory);
      meta.set_state(meta::IndexBuildState::BUILDING);
      meta.set_build_start_wal_id(0);
      meta.set_applied_wal_id(0);
      meta.clear_build_error();
      batch.Put(checkpoint.column("meta_info"), iter->key(),
                meta.SerializeAsString());
    };
    if (type == graphdb::MetadataType::VertexFullTextIndex) {
      meta::VertexFullTextIndex meta;
      RG_CHECK(meta.ParseFromString(iter->value().ToString()),
               common::ErrorCode::InvalidParameter,
               "invalid fulltext index metadata");
      rebuild(meta, path + "/ft/" + std::to_string(meta.index_id()));
    } else if (type == graphdb::MetadataType::VertexVectorIndex) {
      meta::VertexVectorIndex meta;
      RG_CHECK(meta.ParseFromString(iter->value().ToString()),
               common::ErrorCode::InvalidParameter,
               "invalid vector index metadata");
      rebuild(meta, path + "/vt/" + std::to_string(meta.index_id()));
    }
  }
  CheckStorage(iter->status());
  rocksdb::WriteOptions write_options;
  write_options.sync = true;
  CheckStorage(checkpoint.db()->Write(write_options, &batch));
}

}  // namespace

GraphSnapshot GraphManager::CreateSnapshot(const std::string& name,
                                           const std::string& directory) {
  std::lock_guard guard(create_graph_mutex_);
  auto graph = OpenGraph(name);
  RG_CHECK(graph->db_meta().enable_raft(), common::ErrorCode::InvalidParameter,
           "snapshot requires a raft graph [{}]", name);
  auto* driver = graph->raft_driver();
  RG_CHECK(driver != nullptr, common::ErrorCode::InternalError,
           "raft graph has no driver");
  GraphSnapshot snapshot{.graph = name};
  fs::create_directories(directory);
  auto checkpoint = [&]() {
    rocksdb::Checkpoint* raw = nullptr;
    CheckStorage(rocksdb::Checkpoint::Create(graph->raw_db(), &raw));
    std::unique_ptr<rocksdb::Checkpoint> owner(raw);
    CheckStorage(owner->CreateCheckpoint(directory + "/data"));
    return graph->GetRaftApplyIndex();
  };
  std::tie(snapshot.index, snapshot.term) = driver->CaptureSnapshot(checkpoint);
  RG_CHECK(snapshot.index > 0 && snapshot.term > 0,
           common::ErrorCode::InvalidParameter,
           "raft graph is not initialized");
  return snapshot;
}

void GraphManager::ReplaceGraphFromSnapshot(const std::string& name,
                                            const std::string& directory,
                                            const GraphSnapshot& snapshot) {
  RG_CHECK(snapshot.graph == name, common::ErrorCode::InvalidParameter,
           "snapshot graph does not match [{}]", name);
  std::lock_guard guard(create_graph_mutex_);
  // Keep the original directory intact until the replacement is ready, then
  // publish the new graph ID in one metadata batch.
  std::unique_lock lock(graphs_mutex_);
  auto iter = graphs_.find(name);
  RG_CHECK(iter != graphs_.end(), common::ErrorCode::NoSuchGraph, name);
  RG_CHECK(iter->second->db_meta().enable_raft(),
           common::ErrorCode::InvalidParameter,
           "snapshot update requires a raft graph [{}]", name);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (iter->second.use_count() != 1 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  RG_CHECK(iter->second.use_count() == 1, common::ErrorCode::GraphBusy,
           "graph [{}] has active users; retry after releasing them", name);
  auto old_meta = iter->second->db_meta();
  auto old_path = iter->second->path();
  auto checkpoint = std::make_unique<SnapshotDB>(directory + "/data");
  CheckStorage(checkpoint->db()->VerifyChecksum());
  auto value = checkpoint->Get(graphdb::MetadataType::RaftApplyIndex);
  RG_CHECK(value.size() == sizeof(uint64_t) &&
               common::ReadValue<uint64_t>(value.data()) == snapshot.index &&
               snapshot.index > 0 && snapshot.term > 0,
           common::ErrorCode::InvalidParameter, "invalid snapshot apply index");

  auto new_meta = old_meta;
  auto next = next_graph_id_.load();
  RG_CHECK(next < std::numeric_limits<uint32_t>::max(),
           common::ErrorCode::OutOfRange, "graph ID exhausted");
  new_meta.set_graph_id(next);
  std::string new_path = path_ + "/graph" + std::to_string(next);
  RG_CHECK(!fs::exists(new_path), common::ErrorCode::IOError,
           "replacement graph directory already exists [{}]", new_path);
  // Reserve the ID durably before creating files. A crash before publication
  // can leave an orphan directory, but future graph creation will not reuse it.
  ++next;
  rocksdb::WriteOptions write_options;
  write_options.sync = true;
  CheckStorage(meta_db_->Put(
      write_options,
      std::string(1, static_cast<char>(GraphManagerMetadataType::NextGraphID)),
      std::string(reinterpret_cast<const char*>(&next), sizeof(next))));
  next_graph_id_.store(next);
  RebuildExternalIndexes(*checkpoint, new_path);
  checkpoint.reset();
  auto open = [&](const std::string& path, const meta::GraphDBMetaInfo& meta) {
    auto graph = graphdb::GraphDB::Open(
        path, {.block_cache = block_cache_,
               .assistant_pool = assistant_pool_,
               .ft_apply_interval_ = options_.ft_apply_interval,
               .ft_writer_threads_ = options_.ft_writer_threads,
               .ft_writer_memory_budget_ = options_.ft_writer_memory_budget,
               .vt_apply_interval_ = options_.vt_apply_interval});
    graph->db_meta() = meta;
    StartGraphRaft(graph.get(), nullptr);
    return graph;
  };
  // The checkpoint handle must be closed before moving its RocksDB directory.
  fs::create_directory(new_path);
  try {
    fs::rename(directory + "/data", new_path + "/data");
    raft::CreateRaftLogStorageFromSnapshot(new_path + "/raft", snapshot.index,
                                           snapshot.term);
    iter->second.reset();
    std::shared_ptr<graphdb::GraphDB> replacement = open(new_path, new_meta);
    rocksdb::WriteBatch batch;
    batch.Delete(GraphMetaKey(old_meta.graph_id()));
    batch.Put(GraphMetaKey(new_meta.graph_id()), new_meta.SerializeAsString());
    CheckStorage(meta_db_->Write(write_options, {}, &batch));
    iter->second = std::move(replacement);
    plan_cache_->Clear();
  } catch (...) {
    if (!iter->second) iter->second = open(old_path, old_meta);
    std::error_code ec;
    fs::remove_all(new_path, ec);
    throw;
  }
  std::error_code ec;
  fs::remove_all(old_path, ec);
  if (ec)
    LOG_WARN("failed to remove replaced graph directory: {}", ec.message());
}

}  // namespace server
