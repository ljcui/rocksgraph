#include "graphdb/vector_store.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string_view>

#include "common/byte_utils.h"
#include "common/exceptions.h"
#include "common/logger.h"

using common::AsChars;
using common::ReadValue;
namespace fs = std::filesystem;

namespace graphdb {

namespace {

constexpr char kMetaPrefix = 0;
constexpr char kVidPrefix = 1;
constexpr char kDeletePrefix = 2;

const char* kCheckpointAppliedWalIdKey = "checkpoint_applied_wal_id";
const char* kVectorStoreStateDbDir = "state_db";
const char* kVectorStoreCheckpointFilePrefix = "checkpoint.";

std::string BuildKey(char prefix, std::string_view suffix) {
  std::string key(1, prefix);
  key.append(suffix.data(), suffix.size());
  return key;
}

}  // namespace

VectorStore::VectorStore(const std::string& path, int64_t dim,
                         meta::VectorDistanceType distance_type, int hnsw_m,
                         int ef_construction)
    : path_(path),
      dim_(dim),
      distance_type_(distance_type),
      hnsw_m_(hnsw_m),
      ef_construction_(ef_construction) {
  Open();
  LoadState();
}

VectorStore::~VectorStore() { Close(); }

std::string VectorStore::BuildMetaKey(const std::string& name) {
  return BuildKey(kMetaPrefix, name);
}

std::string VectorStore::BuildVidKey(int64_t vid) {
  return BuildKey(kVidPrefix, {AsChars(vid), sizeof(vid)});
}

std::string VectorStore::BuildDeleteMarkKey(int64_t vector_id) {
  return BuildKey(kDeletePrefix, {AsChars(vector_id), sizeof(vector_id)});
}

std::string VectorStore::FaissCheckpointPath(uint64_t applied_wal_id) const {
  return path_ + "/" + kVectorStoreCheckpointFilePrefix +
         std::to_string(applied_wal_id);
}

void VectorStore::Open() {
  std::error_code ec;
  fs::create_directories(path_, ec);
  if (ec) {
    THROW_CODE(IOException, "failed to create vector index directory {}: {}",
               path_, ec.message());
  }
  auto db_path = path_ + "/" + kVectorStoreStateDbDir;
  rocksdb::Options options;
  options.create_if_missing = true;
  options.IncreaseParallelism();
  options.OptimizeLevelStyleCompaction();
  auto s = rocksdb::DB::Open(options, db_path, &db_);
  if (!s.ok()) {
    THROW_CODE(StorageEngineError, "failed to open vector store {}: {}",
               db_path, s.ToString());
  }
}

void VectorStore::Close() {
  hnsw_index_.reset();
  if (!db_) {
    return;
  }
  auto s = db_->Close();
  if (!s.ok()) {
    LOG_WARN("vector store close error: {}", s.ToString());
  }
  db_.reset();
}

void VectorStore::LoadState() {
  hnsw_index_ = std::make_unique<FaissHnswIndex>(dim_, distance_type_, hnsw_m_,
                                                 ef_construction_);
  std::string checkpoint_value;
  auto s =
      db_->Get({}, BuildMetaKey(kCheckpointAppliedWalIdKey), &checkpoint_value);
  if (s.ok()) {
    if (checkpoint_value.size() != sizeof(uint64_t)) {
      THROW_CODE(VectorIndexException,
                 "vector store checkpoint wal id has invalid size, expect {}, "
                 "actual {}",
                 sizeof(uint64_t), checkpoint_value.size());
    }
    has_checkpoint_ = true;
    checkpoint_applied_wal_id_ = ReadValue<uint64_t>(checkpoint_value.data());
  } else if (!s.IsNotFound()) {
    THROW_CODE(StorageEngineError, s.ToString());
  }

  int64_t max_vector_id = 0;
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> iter(db_->NewIterator(ro));
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    auto key = iter->key();
    if (key.empty()) {
      continue;
    }
    char prefix = key[0];
    key.remove_prefix(1);
    auto value = iter->value();
    if (prefix == kMetaPrefix) {
      continue;
    }
    if (key.size() != sizeof(int64_t)) {
      THROW_CODE(VectorIndexException,
                 "vector store key has invalid size, expect {}, actual {}",
                 sizeof(int64_t), key.size());
    }
    int64_t id = ReadValue<int64_t>(key.data());
    if (prefix == kVidPrefix) {
      if (value.size() != sizeof(int64_t)) {
        THROW_CODE(VectorIndexException,
                   "vector store vid mapping has invalid value size, expect "
                   "{}, actual {}",
                   sizeof(int64_t), value.size());
      }
      auto vector_id = ReadValue<int64_t>(value.data());
      max_vector_id = std::max(max_vector_id, vector_id);
      vid_vectorid_.emplace(id, vector_id);
      vectorid_vid_.emplace(vector_id, id);
      continue;
    }
    if (prefix == kDeletePrefix) {
      if (!value.empty()) {
        THROW_CODE(VectorIndexException,
                   "vector store delete marker has invalid value size, expect "
                   "0, actual {}",
                   value.size());
      }
      max_vector_id = std::max(max_vector_id, id);
      deleted_vector_ids_.emplace(id);
      continue;
    }
    THROW_CODE(VectorIndexException,
               "vector store entry has invalid prefix: {}",
               static_cast<int>(prefix));
  }
  next_vector_id_ = max_vector_id + 1;

  if (!has_checkpoint_) {
    return;
  }
  auto checkpoint_path = FaissCheckpointPath(checkpoint_applied_wal_id_);
  if (!fs::exists(checkpoint_path)) {
    THROW_CODE(IOException, "vector store checkpoint file does not exist: {}",
               checkpoint_path);
  }
  hnsw_index_ = FaissHnswIndex::Load(checkpoint_path, dim_, distance_type_,
                                     hnsw_m_, ef_construction_);
}

void VectorStore::Add(int64_t vid, const float* vector) {
  auto vid_iter = vid_vectorid_.find(vid);
  if (vid_iter != vid_vectorid_.end()) {
    auto old_vector_id = vid_iter->second;
    vectorid_vid_.erase(old_vector_id);
    deleted_vector_ids_.emplace(old_vector_id);
  }
  int64_t vector_id = next_vector_id_++;
  assert(vector_id > 0);
  vid_vectorid_[vid] = vector_id;
  vectorid_vid_[vector_id] = vid;
  deleted_vector_ids_.erase(vector_id);
  next_vector_id_ = std::max(next_vector_id_.load(), vector_id + 1);
  hnsw_index_->Add(vector, 1, &vector_id);
}

void VectorStore::Delete(int64_t vid) {
  auto iter = vid_vectorid_.find(vid);
  if (iter == vid_vectorid_.end()) {
    return;
  }
  int64_t vector_id = iter->second;
  vid_vectorid_.erase(iter);
  vectorid_vid_.erase(vector_id);
  deleted_vector_ids_.emplace(vector_id);
}

std::vector<std::pair<int64_t, float>> VectorStore::KnnSearch(
    const float* query, int top_k, int ef_search) const {
  std::vector<std::pair<int64_t, float>> result_ids;
  auto result = hnsw_index_->KnnSearch(
      query, top_k, ef_search, [this](int64_t vector_id) {
        return deleted_vector_ids_.count(vector_id) > 0;
      });
  result_ids.reserve(result.ids.size());
  for (size_t i = 0; i < result.ids.size(); ++i) {
    if (result.ids[i] < 0) {
      continue;
    }
    auto iter = vectorid_vid_.find(result.ids[i]);
    if (iter == vectorid_vid_.end()) {
      THROW_CODE(VectorIndexException,
                 "vector id {} returned by faiss is missing in vid mapping",
                 result.ids[i]);
    }
    result_ids.emplace_back(iter->second, result.distances[i]);
  }
  return result_ids;
}

void VectorStore::Checkpoint(uint64_t applied_wal_id) {
  auto next_checkpoint_path = FaissCheckpointPath(applied_wal_id);
  hnsw_index_->WriteToFile(next_checkpoint_path);

  rocksdb::WriteBatch batch;
  batch.DeleteRange(std::string(1, kVidPrefix),
                    std::string(1, static_cast<char>(kDeletePrefix + 1)));
  for (const auto& [vid, vector_id] : vid_vectorid_) {
    batch.Put(BuildVidKey(vid),
              rocksdb::Slice(AsChars(vector_id), sizeof(vector_id)));
  }
  for (int64_t vector_id : deleted_vector_ids_) {
    batch.Put(BuildDeleteMarkKey(vector_id), {});
  }
  batch.Put(BuildMetaKey(kCheckpointAppliedWalIdKey),
            rocksdb::Slice(AsChars(applied_wal_id), sizeof(applied_wal_id)));
  auto s = db_->Write({}, &batch);
  if (!s.ok()) {
    THROW_CODE(StorageEngineError,
               "failed to persist vector checkpoint wal "
               "id: {}",
               s.ToString());
  }

  auto old_checkpoint_id = checkpoint_applied_wal_id_;
  auto had_checkpoint = has_checkpoint_;
  has_checkpoint_ = true;
  checkpoint_applied_wal_id_ = applied_wal_id;
  if (!had_checkpoint || old_checkpoint_id == checkpoint_applied_wal_id_) {
    return;
  }
  std::error_code ec;
  fs::remove(FaissCheckpointPath(old_checkpoint_id), ec);
  if (ec) {
    LOG_WARN("failed to remove obsolete vector checkpoint file {}: {}",
             FaissCheckpointPath(old_checkpoint_id), ec.message());
  }
}

int64_t VectorStore::NumElements() const {
  return hnsw_index_->GetNumElements();
}

int64_t VectorStore::MemoryUsage() const {
  return hnsw_index_->GetMemoryUsage();
}

int64_t VectorStore::NumDeletedIds() const {
  return static_cast<int64_t>(deleted_vector_ids_.size());
}

}  // namespace graphdb
