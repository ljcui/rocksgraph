#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graphdb/hnsw_index.h"
#include "proto/meta.pb.h"

namespace rocksdb {
class DB;
}

namespace graphdb {

class VectorStore {
 public:
  VectorStore(const std::string& path, int64_t dim,
              meta::VectorDistanceType distance_type, int hnsw_m,
              int ef_construction);
  ~VectorStore();

  VectorStore(const VectorStore&) = delete;
  VectorStore& operator=(const VectorStore&) = delete;

  void Add(int64_t vid, const float* vector);
  void Delete(int64_t vid);
  std::vector<std::pair<int64_t, float>> KnnSearch(const float* query,
                                                   int top_k,
                                                   int ef_search) const;
  void Checkpoint(uint64_t applied_wal_id);
  bool has_checkpoint() const { return has_checkpoint_; }
  uint64_t checkpoint_applied_wal_id() const {
    return checkpoint_applied_wal_id_;
  }
  int64_t NumElements() const;
  int64_t MemoryUsage() const;
  int64_t NumDeletedIds() const;

 private:
  static std::string BuildMetaKey(const std::string& name);
  static std::string BuildVidKey(int64_t vid);
  static std::string BuildDeleteMarkKey(int64_t vector_id);

  std::string FaissCheckpointPath(uint64_t applied_wal_id) const;
  void Open();
  void Close();
  void LoadState();

  std::string path_;
  int64_t dim_;
  meta::VectorDistanceType distance_type_;
  int hnsw_m_;
  int ef_construction_;
  std::unique_ptr<rocksdb::DB> db_;
  std::unique_ptr<FaissHnswIndex> hnsw_index_;
  std::atomic<int64_t> next_vector_id_{1};
  bool has_checkpoint_ = false;
  uint64_t checkpoint_applied_wal_id_ = 0;
  std::unordered_map<int64_t, int64_t> vid_vectorid_;
  std::unordered_map<int64_t, int64_t> vectorid_vid_;
  std::unordered_set<int64_t> deleted_vector_ids_;
};

}  // namespace graphdb
