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

#pragma once
#include <rocksdb/utilities/transaction_db.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/type_traits.h"
#include "common/value.h"
#include "ftindex/include/lib.rs.h"
#include "graphdb/graph_cf.h"
#include "graphdb/id_generator.h"
#include "graphdb/vector_store.h"
#include "proto/meta.pb.h"

namespace txn {
class Transaction;
}
namespace graphdb {
struct VertexPropertyIndex
    : public std::enable_shared_from_this<VertexPropertyIndex> {
 public:
  VertexPropertyIndex(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                      meta::VertexPropertyIndex meta,
                      rocksdb::ColumnFamilyHandle* cf, uint32_t index_id,
                      uint32_t lid, std::vector<uint32_t> pids)
      : db_(db),
        graph_cf_(graph_cf),
        meta_(std::move(meta)),
        cf_(cf),
        index_id_(index_id),
        lid_(lid),
        pids_(std::move(pids)),
        pid_set_(pids_.begin(), pids_.end()) {}
  void AddIndex(txn::Transaction* txn, int64_t vid,
                const std::vector<Value>& values);
  void UpdateIndex(txn::Transaction* txn, int64_t vid,
                   const std::optional<std::vector<Value>>& new_values,
                   const std::optional<std::vector<Value>>& old_values);
  void DeleteIndex(txn::Transaction* txn, int64_t vid,
                   const std::vector<Value>& values);
  void ApplyCommittedBuildUpdate(txn::Transaction* txn,
                                 const meta::PropertyIndexUpdate& update);
  void Load(const rocksdb::Snapshot* snapshot, uint64_t snapshot_wal_id);
  void ApplyWAL();
  std::string NextWALKey();
  std::string IndexKey(const std::vector<Value>& values) const;
  std::string EntryKey(const std::vector<Value>& values, int64_t vid) const;
  std::optional<std::vector<Value>> LoadIndexedPropertyValues(
      txn::Transaction* txn, int64_t vid,
      const std::unordered_map<uint32_t, std::string>* overrides = nullptr,
      const std::unordered_set<uint32_t>* removed = nullptr) const;
  bool ContainsProperty(uint32_t pid) const { return pid_set_.count(pid) > 0; }
  bool TouchesAnyProperty(const std::unordered_set<uint32_t>& pids) const;
  bool AllPropertiesPresent(const std::unordered_set<uint32_t>& pids) const;
  bool is_unique() const { return meta_.is_unique(); }
  bool IsReady() const { return meta_.state() == meta::IndexBuildState::READY; }
  meta::IndexBuildState state() const { return meta_.state(); }
  void SetState(meta::IndexBuildState state) { meta_.set_state(state); }
  void SetBuildError(std::string msg) { meta_.set_build_error(std::move(msg)); }
  const std::string& Name() const { return meta_.name(); }
  bool IsDeleted() const { return deleted_.load(); }
  void MarkDeleted() { deleted_.store(true); }
  meta::VertexPropertyIndex& meta() { return meta_; }
  const meta::VertexPropertyIndex& meta() const { return meta_; }
  rocksdb::ColumnFamilyHandle* cf() { return cf_; }
  uint32_t lid() const { return lid_; }
  const std::vector<uint32_t>& pids() const { return pids_; }
  size_t PropertyCount() const { return pids_.size(); }
  uint32_t index_id() const { return index_id_; }
  void ResetForBuild();

 private:
  void UpdateIndexDirect(txn::Transaction* txn, int64_t vid,
                         const std::optional<std::vector<Value>>& new_values,
                         const std::optional<std::vector<Value>>& old_values);
  void AppendBuildUpdate(txn::Transaction* txn, meta::UpdateType type,
                         int64_t vid, const std::vector<Value>& values);
  void ApplyBuildUpdate(const meta::PropertyIndexUpdate& update);

  rocksdb::TransactionDB* db_;
  GraphCF* graph_cf_;
  meta::VertexPropertyIndex meta_;
  rocksdb::ColumnFamilyHandle* cf_;
  uint32_t index_id_;
  uint32_t lid_;
  std::vector<uint32_t> pids_;
  std::unordered_set<uint32_t> pid_set_;
  std::atomic<uint64_t> next_wal_id_ = 1;
  uint64_t apply_id_ = 0;
  std::atomic<bool> deleted_{false};
};

class VertexFullTextIndex
    : public std::enable_shared_from_this<VertexFullTextIndex> {
 public:
  VertexFullTextIndex(rocksdb::TransactionDB* db,
                      boost::asio::io_service& service,
                      boost::asio::io_service::strand* strand,
                      GraphCF* graph_cf, IdGenerator* id_generator,
                      meta::VertexFullTextIndex meta, uint32_t index_id,
                      size_t writer_threads, size_t writer_memory_budget,
                      const std::unordered_set<uint32_t>& lids,
                      const std::unordered_set<uint32_t>& pids,
                      size_t commit_interval);
  void AddVertex(int64_t id, std::vector<std::string> fields,
                 std::vector<std::string> values);
  void DeleteVertex(int64_t id);
  void ApplyWAL();
  void Start();
  void Stop();
  void ReleaseResources();
  [[nodiscard]] bool MatchLabelIds(
      const std::unordered_set<uint32_t>& lids) const;
  [[nodiscard]] bool MatchPropertyIds(
      const std::unordered_set<uint32_t>& pids) const;
  ::rust::Vec<::IdScore> Query(const std::string& query, size_t top_n);
  [[nodiscard]] const std::unordered_set<uint32_t>& LabelIds() const {
    return lids_;
  }
  [[nodiscard]] const std::unordered_set<uint32_t>& PropertyIds() const {
    return pids_;
  }
  [[nodiscard]] const std::string& Name() const { return meta_.name(); }
  meta::VertexFullTextIndex& meta() { return meta_; }
  const meta::VertexFullTextIndex& meta() const { return meta_; }
  uint32_t index_id() const { return index_id_; }
  [[nodiscard]] bool IsDeleted() const { return deleted_.load(); }
  void MarkDeleted() { deleted_.store(true); }
  [[nodiscard]] bool IsReady() const {
    return meta_.state() == meta::IndexBuildState::READY;
  }
  [[nodiscard]] meta::IndexBuildState state() const { return meta_.state(); }
  void SetState(meta::IndexBuildState state) { meta_.set_state(state); }
  void SetBuildError(std::string msg) { meta_.set_build_error(std::move(msg)); }
  void Load(const rocksdb::Snapshot* snapshot = nullptr,
            uint64_t snapshot_wal_id = 0);
  std::string NextWALKey();
  void AddIndex(txn::Transaction* txn, int64_t vid,
                const meta::FullTextIndexUpdate& wal);
  void DeleteIndex(txn::Transaction* txn, int64_t vid,
                   const meta::FullTextIndexUpdate& wal);
  void ResetForClear();

 private:
  void StartTimer();
  void ApplyUpdatesBatch(const ::rust::Vec<int64_t>& ids,
                         const ::rust::Vec<uint8_t>& ops,
                         const ::rust::Vec<uint64_t>& field_counts,
                         const ::rust::Vec<::rust::String>& fields,
                         const ::rust::Vec<uint64_t>& value_counts,
                         const ::rust::Vec<::rust::String>& values);
  void Commit(const std::string& payload);

  rocksdb::TransactionDB* db_ = nullptr;
  GraphCF* graph_cf_ = nullptr;
  IdGenerator* id_generator_ = nullptr;
  meta::VertexFullTextIndex meta_;
  uint32_t index_id_;
  std::atomic<uint64_t> next_wal_id_ = 1;
  uint64_t apply_id_ = 0;
  std::unordered_set<uint32_t> lids_;
  std::unordered_set<uint32_t> pids_;
  ::FTIndex* ft_index_ = nullptr;
  std::unique_ptr<::rust::Box<::FTIndex>> instance_;
  std::mutex mutex_;
  std::mutex timer_mutex_;
  std::condition_variable timer_cv_;
  size_t active_callbacks_ = 0;
  bool started_ = false;
  bool stopped_ = false;
  std::atomic<bool> deleted_{false};
  size_t interval_ = 1;
  size_t writer_threads_ = 1;
  size_t writer_memory_budget_ = 0;
  boost::asio::io_service::strand* strand_;
  boost::asio::steady_timer timer_;
};

class VertexVectorIndex
    : public std::enable_shared_from_this<VertexVectorIndex> {
 public:
  VertexVectorIndex(rocksdb::TransactionDB* db,
                    boost::asio::io_service& service,
                    boost::asio::io_service::strand* strand, GraphCF* graph_cf,
                    uint32_t index_id, uint32_t lid, uint32_t pid,
                    meta::VertexVectorIndex meta, size_t commit_interval);
  std::vector<std::pair<int64_t, float>> KnnSearch(const float* query,
                                                   int top_k, int ef_search);
  int64_t NumElements();
  int64_t MemoryUsage();
  int64_t NumDeletedIds();
  meta::VertexVectorIndex& meta() { return meta_; }
  const meta::VertexVectorIndex& meta() const { return meta_; }
  uint32_t lid() const { return lid_; }
  uint32_t pid() const { return pid_; }
  uint32_t index_id() const { return index_id_; }
  bool IsReady() const { return meta_.state() == meta::IndexBuildState::READY; }
  meta::IndexBuildState state() const { return meta_.state(); }
  void SetState(meta::IndexBuildState state) { meta_.set_state(state); }
  void SetBuildError(std::string msg) { meta_.set_build_error(std::move(msg)); }
  bool IsDeleted() const { return deleted_.load(); }
  void MarkDeleted() { deleted_.store(true); }
  void Start();
  void Stop();
  void ReleaseResources();
  void Load(const rocksdb::Snapshot* snapshot = nullptr,
            uint64_t snapshot_wal_id = 0);
  void DeleteIfPresent(txn::Transaction* txn, int64_t vid);
  std::string NextWALKey();
  void AddIndex(txn::Transaction* txn, int64_t vid,
                meta::VectorIndexUpdate& wal);
  void ApplyWAL();
  void ResetForClear();

 private:
  void StartTimer();
  rocksdb::TransactionDB* db_ = nullptr;
  GraphCF* graph_cf_ = nullptr;
  uint32_t index_id_;
  uint32_t lid_;
  uint32_t pid_;
  meta::VertexVectorIndex meta_;
  std::unique_ptr<VectorStore> vector_store_;
  std::atomic<uint64_t> next_wal_id_ = 1;
  uint64_t apply_id_ = 0;
  std::shared_mutex mutex_;
  std::mutex apply_mutex_;
  std::mutex timer_mutex_;
  std::condition_variable timer_cv_;
  size_t active_callbacks_ = 0;
  bool started_ = false;
  bool stopped_ = false;
  size_t interval_ = 5;
  boost::asio::io_service::strand* strand_;
  boost::asio::steady_timer timer_;
  std::atomic<bool> deleted_{false};
};

}  // namespace graphdb
