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
#include <rocksdb/slice.h>
#include <rocksdb/utilities/transaction_db.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "graph_cf.h"
namespace raft {
class RaftDriver;
}

namespace graphdb {
enum class MetaDataType : char {
  VertexLabel = 0,
  EdgeType = 1,
  Property = 2,
  VertexPropertyIndex = 3,
  VertexFullTextIndex = 4,
  VertexVectorIndex = 5,
  NextVertexId = 6,
  NextEdgeId = 7,
  RaftApplyIndex = 8,
  VertexVectorField = 9
};

class IdGenerator {
 public:
  IdGenerator() = default;
  // No copying allowed
  IdGenerator(const IdGenerator&) = delete;
  void operator=(const IdGenerator&) = delete;

  void Bind(rocksdb::TransactionDB* db, GraphCF* graph_cf);
  void SetRaftDriver(raft::RaftDriver* raft_driver);
  void LoadToken(MetaDataType type, const std::string& name, uint32_t id);
  void ApplyMetaRecord(MetaDataType type, const rocksdb::Slice& key_suffix,
                       const rocksdb::Slice& value);
  void SetMaxIds(uint32_t max_lid, uint32_t max_pid, uint32_t max_tid,
                 uint32_t max_index_id);
  void SetNextEntityIds(int64_t next_vid, int64_t next_eid);
  int64_t GetNextVid();
  int64_t GetNextEid();
  uint32_t GetNextIndexId();
  void ReserveIndexId(uint32_t native_index_id);
  std::optional<uint32_t> GetLid(const std::string& name);
  std::optional<uint32_t> GetPid(const std::string& name);
  std::optional<uint32_t> GetTid(const std::string& name);
  std::optional<std::string> GetPropertyName(uint32_t pid);
  std::optional<std::string> GetVertexLabelName(uint32_t lid);
  std::optional<std::string> GetEdgeTypeName(uint32_t tid);
  uint32_t GetOrCreateLid(const std::string& name);
  uint32_t GetOrCreatePid(const std::string& name);
  uint32_t GetOrCreateTid(const std::string& name);
  std::unordered_set<std::string> GetProperties();
  std::unordered_set<std::string> GetVertexLabels();
  std::unordered_set<std::string> GetEdgeTypes();

 private:
  static constexpr int64_t kIdRangeSize = 1024;

  int64_t GetNextEntityId(std::atomic<int64_t>* next_id,
                          std::atomic<int64_t>* range_end,
                          std::atomic<int64_t>* persisted_next_id,
                          std::mutex* refill_mutex, MetaDataType meta_type);
  void ProposeAndApply(rocksdb::WriteBatch* wb);
  void PersistEntityId(MetaDataType meta_type, int64_t next_id);
  void PersistToken(MetaDataType type, const std::string& name, uint32_t id);

  std::atomic<int64_t> next_vid_{1};
  std::atomic<int64_t> vid_range_end_{1};
  std::atomic<int64_t> persisted_next_vid_{1};
  std::atomic<int64_t> next_eid_{1};
  std::atomic<int64_t> eid_range_end_{1};
  std::atomic<int64_t> persisted_next_eid_{1};
  std::atomic<uint32_t> label_next_lid_{1};
  std::atomic<uint32_t> label_next_pid_{1};
  std::atomic<uint32_t> label_next_tid_{1};
  std::atomic<uint32_t> index_next_id_{1};
  std::unordered_map<std::string, uint32_t> vertex_labels_name_to_id_;
  std::unordered_map<uint32_t, std::string> vertex_labels_id_to_name_;
  std::unordered_map<std::string, uint32_t> edge_types_name_to_id_;
  std::unordered_map<uint32_t, std::string> edge_types_id_to_name_;
  std::unordered_map<std::string, uint32_t> properties_name_to_id_;
  std::unordered_map<uint32_t, std::string> properties_id_to_name_;
  rocksdb::TransactionDB* db_ = nullptr;
  GraphCF* graph_cf_ = nullptr;
  raft::RaftDriver* raft_driver_ = nullptr;
  std::mutex vid_refill_mutex_;
  std::mutex eid_refill_mutex_;
  std::mutex vertex_label_create_mutex_;
  std::mutex edge_type_create_mutex_;
  std::mutex property_create_mutex_;
  std::shared_mutex vertex_labels_mutex_;
  std::shared_mutex edge_types_mutex_;
  std::shared_mutex properties_mutex_;
};
}  // namespace graphdb
