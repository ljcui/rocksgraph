//
// Created by botu.wzy
//

#pragma once
#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/utilities/transaction_db.h>

#include <boost/asio.hpp>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "assistant_pool.h"
#include "graph_cf.h"
#include "id_generator.h"
#include "meta_info.h"
#include "proto/meta.pb.h"
#include "raft_driver/raft_driver.h"
#include "value/value.h"

namespace txn {
class Transaction;
}
namespace server {
class Galaxy;
}
namespace graphdb {

struct GraphDBOptions {
  std::shared_ptr<rocksdb::Cache> block_cache;
  std::shared_ptr<AssistantPool> assistant_pool;
  size_t ft_apply_interval_ = 1;
  size_t ft_writer_threads_ = 1;
  size_t ft_writer_memory_budget_ = 50 * 1000 * 1000;
  size_t vt_apply_interval_ = 1;
};

class GraphDB {
  friend class server::Galaxy;

 public:
  GraphDB() = default;
  ~GraphDB();
  // No copying allowed
  GraphDB(const GraphDB&) = delete;
  void operator=(const GraphDB&) = delete;

  static std::unique_ptr<GraphDB> Open(const std::string& path,
                                       const GraphDBOptions& options);
  std::unique_ptr<txn::Transaction> BeginTransaction();
  void ClearData();
  void AddVertexPropertyIndex(const std::string& index_name, bool,
                              const std::string& label,
                              const std::vector<std::string>& properties);
  void DeleteVertexPropertyIndex(const std::string& index_name);
  void AddEdgePropertyIndex(const std::string& index_name, bool unique,
                            const std::string& edge_type,
                            const std::vector<std::string>& properties);
  void DeleteEdgePropertyIndex(const std::string& index_name);
  void AddVertexFullTextIndex(const std::string& index_name,
                              const std::vector<std::string>& labels,
                              const std::vector<std::string>& properties);
  void DeleteVertexFullTextIndex(const std::string& index_name);
  void AddVertexVectorIndex(const std::string& index_name,
                            const std::string& label,
                            const std::string& property, int dimension,
                            std::string distance_type, int hnsw_m,
                            int hnsw_ef_construction);
  void AddVertexVectorField(const std::string& label,
                            const std::string& property, int dimension);
  void DeleteVertexVectorIndex(const std::string& index_name);
  std::vector<rocksdb::ColumnFamilyHandle*>& cf_handles() {
    return cf_handles_;
  }
  rocksdb::TransactionDB* raw_db() { return db_; }
  GraphCF& graph_cf() { return graph_cf_; }
  IdGenerator& id_generator() { return meta_info_.id_generator(); }
  MetaInfo& meta_info() { return meta_info_; }
  meta::GraphDBMetaInfo& db_meta() { return db_meta_; }
  const std::string& path() { return path_; }
  raft::RaftDriver* raft_driver() const;
  void SetRaftDriver(std::unique_ptr<raft::RaftDriver> raft_driver);
  void StopRaft();
  uint64_t GetRaftApplyIndex() const;
  void ApplyRaftRequest(uint64_t index, const meta::RaftRequest& request);
  rocksdb::Status SetRaftApplyIndex(uint64_t apply_index,
                                    rocksdb::WriteBatch* wb) const;
  bool& drop_on_close() { return drop_on_close_; }
  std::mutex& property_index_commit_mutex() {
    return property_index_commit_mutex_;
  }
  std::mutex& fulltext_index_commit_mutex() {
    return fulltext_index_commit_mutex_;
  }
  std::mutex& vector_index_commit_mutex() { return vector_index_commit_mutex_; }

 private:
  void ClearDataInternal();
  void DrainAssistant();
  void ResumeBackgroundIndexBuilds();
  void PersistVertexPropertyIndexMeta(
      const std::shared_ptr<VertexPropertyIndex>& index);
  void PersistEdgePropertyIndexMeta(
      const std::shared_ptr<EdgePropertyIndex>& index);
  void PersistVertexFullTextIndexMeta(
      const std::shared_ptr<VertexFullTextIndex>& index);
  void PersistVertexVectorIndexMeta(
      const std::shared_ptr<VertexVectorIndex>& index);
  void SyncIdGeneratorFromRaftBatch(const rocksdb::WriteBatch& wb);
  void ApplyRaftWriteBatch(uint64_t index, const meta::RaftRequest& request);
  void ApplyGraphIndexDdlRequest(uint64_t index,
                                 const meta::GraphIndexDdlRequest& request);
  void ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::Operation operation,
                            std::string payload);
  void ApplyCreateVertexPropertyIndex(uint64_t apply_index,
                                      meta::VertexPropertyIndex meta);
  void ApplyDeleteVertexPropertyIndex(uint64_t apply_index,
                                      const meta::VertexPropertyIndex& meta);
  void ApplyCreateEdgePropertyIndex(uint64_t apply_index,
                                    meta::EdgePropertyIndex meta);
  void ApplyDeleteEdgePropertyIndex(uint64_t apply_index,
                                    const meta::EdgePropertyIndex& meta);
  void ApplyCreateVertexFullTextIndex(uint64_t apply_index,
                                      meta::VertexFullTextIndex meta);
  void ApplyDeleteVertexFullTextIndex(uint64_t apply_index,
                                      const meta::VertexFullTextIndex& meta);
  void ApplyCreateVertexVectorIndex(uint64_t apply_index,
                                    meta::VertexVectorIndex meta);
  void ApplyDeleteVertexVectorIndex(uint64_t apply_index,
                                    const meta::VertexVectorIndex& meta);
  void ApplyCreateVertexVectorField(uint64_t apply_index,
                                    meta::VertexVectorField meta);
  void ScheduleVertexPropertyIndexBuild(
      const std::shared_ptr<VertexPropertyIndex>& index, bool reset_existing);
  void ScheduleEdgePropertyIndexBuild(
      const std::shared_ptr<EdgePropertyIndex>& index, bool reset_existing);
  void ScheduleVertexFullTextIndexBuild(
      const std::shared_ptr<VertexFullTextIndex>& index, bool reset_existing);
  void ScheduleVertexVectorIndexBuild(
      const std::shared_ptr<VertexVectorIndex>& index, bool reset_existing);

  std::string path_;
  rocksdb::TransactionDB* db_ = nullptr;
  std::vector<rocksdb::ColumnFamilyHandle*> cf_handles_;
  std::shared_ptr<AssistantPool> assistant_pool_;
  std::unique_ptr<boost::asio::io_service::strand> assistant_strand_;
  GraphCF graph_cf_;
  MetaInfo meta_info_;
  meta::GraphDBMetaInfo db_meta_;
  GraphDBOptions options_;
  mutable std::shared_mutex raft_mutex_;
  std::unique_ptr<raft::RaftDriver> raft_driver_;
  bool drop_on_close_ = false;
  std::mutex clear_data_mutex_;
  std::mutex index_ddl_propose_mutex_;
  std::mutex index_ddl_mutex_;
  std::mutex property_index_commit_mutex_;
  std::mutex fulltext_index_commit_mutex_;
  std::mutex vector_index_commit_mutex_;
};
}  // namespace graphdb
