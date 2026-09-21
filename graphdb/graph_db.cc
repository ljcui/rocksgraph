//
// Created by botu.wzy
//
#include "graph_db.h"

#include <filesystem>

#include "common/exception.h"
#include "common/logger.h"
#include "graph_db_internal.h"
#include "graphdb/transaction.h"
namespace graphdb {
using namespace internal;

std::unique_ptr<GraphDB> GraphDB::Open(const std::string& path,
                                       const GraphDBOptions& graph_options) {
  if (!graph_options.assistant_pool) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "GraphDB assistant_pool must be provided");
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
  if (!s.ok()) RG_THROW(common::ErrorCode::StorageEngineError, s.ToString());
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

std::unique_ptr<Transaction> GraphDB::BeginTransaction() {
  rocksdb::WriteOptions wo;
  rocksdb::TransactionOptions to;
  rocksdb::Transaction* txn = db_->BeginTransaction(wo, to);
  return std::make_unique<Transaction>(txn, this);
}

void GraphDB::ClearData() {
  if (db_meta_.enable_raft()) {
    RG_THROW(common::ErrorCode::InvalidParameter,
             "ClearData is not supported on raft graph [{}]; use "
             "GraphManager::ClearGraph instead",
             db_meta_.graph_name());
  }
  ClearDataInternal();
}

void GraphDB::ClearDataInternal() {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::vector<std::shared_ptr<VertexFullTextIndex>> ft_indexes;
  std::vector<std::shared_ptr<VertexVectorIndex>> vector_indexes;
  std::vector<std::shared_ptr<EdgePropertyIndex>> edge_property_indexes;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    ft_indexes = meta_info_.GetVertexFullTextIndexes();
    vector_indexes = meta_info_.GetVertexVectorIndexes();
    edge_property_indexes = meta_info_.GetEdgePropertyIndexes();
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
  if (!s.ok()) RG_THROW(common::ErrorCode::StorageEngineError, s.ToString());

  for (const auto& index : property_indexes) {
    index->ResetForBuild();
  }
  for (const auto& index : edge_property_indexes) {
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

}  // namespace graphdb
