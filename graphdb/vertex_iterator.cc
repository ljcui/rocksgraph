//
// Created by botu.wzy
//

#include "vertex_iterator.h"

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <cstring>

#include "common/byte_utils.h"
#include "common/exception.h"
#include "common/logger.h"
#include "graph_db.h"
#include "graphdb/transaction.h"
#include "index_error.h"
using common::AsChars;
using common::ReadValue;
namespace graphdb {

namespace {

int CompareKeyWithBoundPrefix(rocksdb::Slice key, const std::string &bound) {
  size_t common_size = std::min(key.size(), bound.size());
  int cmp = std::memcmp(key.data(), bound.data(), common_size);
  if (cmp < 0) {
    return -1;
  }
  if (cmp > 0) {
    return 1;
  }
  if (key.size() < bound.size()) {
    return -1;
  }
  return 0;
}

int64_t ReadPropertyIndexVid(const std::shared_ptr<VertexPropertyIndex> &index,
                             rocksdb::Slice key, rocksdb::Slice value) {
  if (index->is_unique()) {
    if (value.size() != sizeof(int64_t)) {
      RG_THROW_CODE(StorageEngineError,
                    "vertex unique index stores invalid vid size");
    }
    return ReadValue<int64_t>(value.data());
  }
  if (key.size() < sizeof(uint32_t) + sizeof(int64_t)) {
    RG_THROW_CODE(StorageEngineError,
                  "vertex non-unique index stores invalid key size");
  }
  return ReadValue<int64_t>(key.data() + key.size() - sizeof(int64_t));
}

}  // namespace

Vertex &EmptyVertexIterator::GetVertex() {
  RG_THROW(common::InternalError, "empty vertex iterator has no current value");
}

VertexLabelScanIterator::VertexLabelScanIterator(Transaction *txn,
                                                 uint32_t label_id)
    : VertexIterator(txn), label_id_(label_id) {
  rocksdb::ReadOptions ro;
  iterator_.reset(
      txn->dbtxn()->GetIterator(ro, txn->db()->graph_cf().vertex_label_vid));
  iterator_->Seek({AsChars(label_id_), sizeof(label_id_)});
  LoadCurrent();
}

void VertexLabelScanIterator::LoadCurrent() {
  valid_ = false;
  if (!iterator_->Valid()) {
    return;
  }
  auto key = iterator_->key();
  if (key.size() != sizeof(uint32_t) + sizeof(int64_t) ||
      label_id_ != ReadValue<uint32_t>(key.data())) {
    return;
  }
  key.remove_prefix(sizeof(uint32_t));
  vertex_ = std::make_unique<Vertex>(txn_, ReadValue<int64_t>(key.data()));
  valid_ = true;
}

void VertexLabelScanIterator::Next() {
  assert(valid_);
  iterator_->Next();
  LoadCurrent();
}

Vertex &VertexLabelScanIterator::GetVertex() {
  assert(valid_);
  return *vertex_;
}

VertexScanIterator::VertexScanIterator(Transaction *txn) : VertexIterator(txn) {
  rocksdb::ReadOptions ro;
  iterator_.reset(
      txn->dbtxn()->GetIterator(ro, txn->db()->graph_cf().graph_topology));
  iterator_->SeekToFirst();
  SeekToNextVertex();
}

void VertexScanIterator::SeekToNextVertex() {
  valid_ = false;
  while (iterator_->Valid()) {
    if (iterator_->key().size() == sizeof(int64_t)) {
      vertex_ = std::make_unique<Vertex>(
          txn_, ReadValue<int64_t>(iterator_->key().data()));
      valid_ = true;
      return;
    }
    iterator_->Next();
  }
}

void VertexScanIterator::Next() {
  assert(valid_);
  iterator_->Next();
  SeekToNextVertex();
}

Vertex &VertexScanIterator::GetVertex() {
  assert(valid_);
  return *vertex_;
}

VertexPropertyFilterIterator::VertexPropertyFilterIterator(
    std::unique_ptr<VertexIterator> source,
    std::unordered_map<uint32_t, rg::Value> properties)
    : VertexIterator(source->transaction()),
      source_(std::move(source)),
      properties_(std::move(properties)) {
  SeekToNextMatch();
}

bool VertexPropertyFilterIterator::Matches(Vertex &vertex) const {
  for (const auto &[property_id, expected] : properties_) {
    if (vertex.GetProperty(property_id) != expected) {
      return false;
    }
  }
  return true;
}

void VertexPropertyFilterIterator::SeekToNextMatch() {
  valid_ = false;
  while (source_->Valid()) {
    if (Matches(source_->GetVertex())) {
      valid_ = true;
      return;
    }
    source_->Next();
  }
}

void VertexPropertyFilterIterator::Next() {
  assert(valid_);
  source_->Next();
  SeekToNextMatch();
}

Vertex &VertexPropertyFilterIterator::GetVertex() {
  assert(valid_);
  return source_->GetVertex();
}

VertexUniqueIndexIterator::VertexUniqueIndexIterator(
    Transaction *txn, std::shared_ptr<VertexPropertyIndex> index,
    std::vector<rg::Value> values)
    : VertexIterator(txn), index_(std::move(index)) {
  if (!index_ || !index_->is_unique()) {
    return;
  }
  rocksdb::ReadOptions ro;
  std::string index_val;
  std::string index_key = index_->IndexKey(values);
  auto s = txn_->dbtxn()->Get(ro, index_->cf(), index_key, &index_val);
  if (s.ok()) {
    int64_t vid = ReadValue<int64_t>(index_val.data());
    vertex_ = std::make_unique<Vertex>(txn_, vid);
    valid_ = true;
  } else if (!s.IsNotFound()) {
    RG_THROW_CODE(StorageEngineError, s.ToString());
  }
}

Vertex &VertexUniqueIndexIterator::GetVertex() {
  assert(valid_);
  return *vertex_;
}

VertexIndexSeekIterator::VertexIndexSeekIterator(
    Transaction *txn, std::shared_ptr<VertexPropertyIndex> index,
    std::string prefix)
    : VertexIterator(txn),
      index_(std::move(index)),
      prefix_(std::move(prefix)) {
  rocksdb::ReadOptions ro;
  if (index_->is_unique()) {
    std::string index_val;
    auto s = txn_->dbtxn()->Get(ro, index_->cf(), prefix_, &index_val);
    if (s.ok()) {
      vertex_ = std::make_unique<Vertex>(
          txn_, ReadPropertyIndexVid(index_, rocksdb::Slice(prefix_),
                                     rocksdb::Slice(index_val)));
      valid_ = true;
    } else if (!s.IsNotFound()) {
      RG_THROW_CODE(StorageEngineError, s.ToString());
    }
    return;
  }
  iterator_.reset(txn->dbtxn()->GetIterator(ro, index_->cf()));
  iterator_->Seek(prefix_);
  SeekToNextValid();
}

void VertexIndexSeekIterator::SeekToNextValid() {
  valid_ = false;
  while (iterator_ && iterator_->Valid() &&
         iterator_->key().starts_with(prefix_)) {
    vertex_ = std::make_unique<Vertex>(
        txn_,
        ReadPropertyIndexVid(index_, iterator_->key(), iterator_->value()));
    valid_ = true;
    return;
  }
  if (iterator_ && !iterator_->status().ok()) {
    RG_THROW_CODE(StorageEngineError, iterator_->status().ToString());
  }
}

void VertexIndexSeekIterator::Next() {
  assert(valid_);
  valid_ = false;
  if (index_->is_unique()) {
    return;
  }
  iterator_->Next();
  SeekToNextValid();
}

Vertex &VertexIndexSeekIterator::GetVertex() {
  assert(valid_);
  return *vertex_;
}

VertexIndexRangeIterator::VertexIndexRangeIterator(
    Transaction *txn, std::shared_ptr<VertexPropertyIndex> index,
    std::optional<std::string> lower_key, std::optional<std::string> upper_key,
    bool left_closed, bool right_closed)
    : VertexIterator(txn),
      index_(std::move(index)),
      lower_key_(std::move(lower_key)),
      upper_key_(std::move(upper_key)),
      left_closed_(left_closed),
      right_closed_(right_closed) {
  index_prefix_.assign(AsChars(index_->index_id()), sizeof(index_->index_id()));
  rocksdb::ReadOptions ro;
  iterator_.reset(txn->dbtxn()->GetIterator(ro, index_->cf()));
  if (lower_key_.has_value()) {
    iterator_->Seek(*lower_key_);
  } else {
    iterator_->Seek(index_prefix_);
  }
  SeekToNextValid();
}

void VertexIndexRangeIterator::SeekToNextValid() {
  valid_ = false;
  while (iterator_ && iterator_->Valid() &&
         iterator_->key().starts_with(index_prefix_)) {
    if (lower_key_.has_value()) {
      int cmp = CompareKeyWithBoundPrefix(iterator_->key(), *lower_key_);
      if (cmp < 0 || (cmp == 0 && !left_closed_)) {
        iterator_->Next();
        continue;
      }
    }
    if (upper_key_.has_value()) {
      int cmp = CompareKeyWithBoundPrefix(iterator_->key(), *upper_key_);
      if (cmp > 0 || (cmp == 0 && !right_closed_)) {
        break;
      }
    }
    vertex_ = std::make_unique<Vertex>(
        txn_,
        ReadPropertyIndexVid(index_, iterator_->key(), iterator_->value()));
    valid_ = true;
    return;
  }
  if (iterator_ && !iterator_->status().ok()) {
    RG_THROW_CODE(StorageEngineError, iterator_->status().ToString());
  }
}

void VertexIndexRangeIterator::Next() {
  assert(valid_);
  valid_ = false;
  iterator_->Next();
  SeekToNextValid();
}

Vertex &VertexIndexRangeIterator::GetVertex() {
  assert(valid_);
  return *vertex_;
}

VertexFullTextSearchIterator::VertexFullTextSearchIterator(
    Transaction *txn, const std::string &ft_index_name,
    const std::string &query, size_t top_n)
    : VertexScoreIterator(txn) {
  auto ft = txn->db()->meta_info().GetReadyVertexFullTextIndex(ft_index_name);
  if (!ft) {
    if (auto building_index =
            txn->db()->meta_info().GetVertexFullTextIndex(ft_index_name)) {
      ThrowIfIndexUnavailable(building_index, ft_index_name, "Fulltext");
    }
    RG_THROW_CODE(FullTextIndexNotFound, "No such fulltext index: {}",
                  ft_index_name);
  }
  result_ = ft->Query(query, top_n);
  if (!result_.empty()) {
    vertex_score_ = std::make_unique<VertexScore>(
        Vertex(txn_, result_[result_index_].id), result_[result_index_].score);
    valid_ = true;
  }
}

void VertexFullTextSearchIterator::Next() {
  assert(valid_);
  valid_ = false;
  ++result_index_;
  if (result_index_ < result_.size()) {
    vertex_score_ = std::make_unique<VertexScore>(
        Vertex(txn_, result_[result_index_].id), result_[result_index_].score);
    valid_ = true;
  }
}

VertexScore &VertexFullTextSearchIterator::GetVertexScore() {
  assert(valid_);
  return *vertex_score_;
}

VertexKnnSearchIterator::VertexKnnSearchIterator(
    Transaction *txn, const std::string &vector_index,
    const std::vector<float> &query, int top_k, int ef_search)
    : VertexScoreIterator(txn) {
  auto index = txn->db()->meta_info().GetReadyVertexVectorIndex(vector_index);
  if (!index) {
    if (auto building_index =
            txn->db()->meta_info().GetVertexVectorIndex(vector_index)) {
      ThrowIfIndexUnavailable(building_index, vector_index, "Vector");
    }
    RG_THROW_CODE(VectorIndexException, "No such vector index:{}",
                  vector_index);
  }
  result_ = index->KnnSearch(query.data(), top_k, ef_search);
  if (!result_.empty()) {
    vertex_score_ = std::make_unique<VertexScore>(
        Vertex(txn_, result_[result_index_].first),
        result_[result_index_].second);
    valid_ = true;
  }
}

void VertexKnnSearchIterator::Next() {
  assert(valid_);
  valid_ = false;
  ++result_index_;
  if (result_index_ < result_.size()) {
    vertex_score_ = std::make_unique<VertexScore>(
        Vertex(txn_, result_[result_index_].first),
        result_[result_index_].second);
    valid_ = true;
  }
}

VertexScore &VertexKnnSearchIterator::GetVertexScore() {
  assert(valid_);
  return *vertex_score_;
}

}  // namespace graphdb
