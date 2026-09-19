//
// Created by botu.wzy
//

#include "edge_iterator.h"

#include <algorithm>
#include <cstring>

#include "common/byte_utils.h"
#include "common/exception.h"
#include "common/logger.h"
#include "graph_db.h"
#include "graphdb/id_codec.h"
#include "graphdb/transaction.h"
namespace graphdb {
namespace {

int CompareEdgeIndexKeyWithBoundPrefix(rocksdb::Slice key,
                                       const std::string &bound) {
  size_t common_size = std::min(key.size(), bound.size());
  int cmp = std::memcmp(key.data(), bound.data(), common_size);
  if (cmp != 0) return cmp < 0 ? -1 : 1;
  return key.size() < bound.size() ? -1 : 0;
}

int64_t ReadEdgePropertyIndexEid(
    const std::shared_ptr<EdgePropertyIndex> &index, rocksdb::Slice key,
    rocksdb::Slice value) {
  if (index->is_unique()) {
    if (value.size() != sizeof(int64_t)) {
      RG_THROW_CODE(StorageEngineError,
                    "edge unique index stores invalid eid size");
    }
    return ReadBigEndianId<int64_t>(value.data());
  }
  if (key.size() < sizeof(uint32_t) + sizeof(int64_t)) {
    RG_THROW_CODE(StorageEngineError,
                  "edge non-unique index stores invalid key size");
  }
  return ReadBigEndianId<int64_t>(key.data() + key.size() - sizeof(int64_t));
}

std::unique_ptr<Edge> LoadIndexedEdge(Transaction *txn,
                                      const EdgePropertyIndex &index,
                                      int64_t eid) {
  return std::make_unique<Edge>(txn->GetEdgeById(index.tid(), eid));
}

}  // namespace

Edge &EmptyEdgeIterator::GetEdge() {
  RG_THROW(common::InternalError, "empty edge iterator has no current value");
}

EdgeTypeScanIterator::EdgeTypeScanIterator(
    Transaction *txn, std::unordered_set<uint32_t> type_ids)
    : EdgeIterator(txn), scan_all_(type_ids.empty()) {
  rocksdb::ReadOptions ro;
  iterator_.reset(
      txn_->dbtxn()->GetIterator(ro, txn_->db()->graph_cf().edge_type_eid));
  if (scan_all_) {
    iterator_->SeekToFirst();
    if (iterator_->Valid()) {
      valid_ = true;
      LoadCurrent();
    } else {
      CheckIteratorStatus();
    }
    return;
  }
  for (uint32_t type_id : type_ids) {
    prefixes_.emplace(EncodeBigEndianId(type_id));
  }
  SeekToNextPrefix();
}

void EdgeTypeScanIterator::LoadCurrent() {
  const rocksdb::Slice key = iterator_->key();
  const rocksdb::Slice value = iterator_->value();
  if (key.size() != sizeof(uint32_t) + sizeof(int64_t)) {
    RG_THROW_CODE(StorageEngineError,
                  "edge type/eid key has invalid size, expect {}, actual {}",
                  sizeof(uint32_t) + sizeof(int64_t), key.size());
  }
  if (value.size() != 2 * sizeof(int64_t)) {
    RG_THROW_CODE(StorageEngineError,
                  "edge type/eid value has invalid size, expect {}, actual {}",
                  2 * sizeof(int64_t), value.size());
  }

  const char *key_data = key.data();
  const uint32_t type = ReadBigEndianId<uint32_t>(key_data);
  const int64_t eid = ReadBigEndianId<int64_t>(key_data + sizeof(type));
  const char *value_data = value.data();
  const int64_t start_id = ReadBigEndianId<int64_t>(value_data);
  const int64_t end_id =
      ReadBigEndianId<int64_t>(value_data + sizeof(start_id));
  edge_ = std::make_unique<Edge>(txn_, eid, start_id, end_id, type);
}

void EdgeTypeScanIterator::SeekToNextPrefix() {
  valid_ = false;
  while (!prefixes_.empty()) {
    prefix_ = std::move(prefixes_.front());
    prefixes_.pop();
    iterator_->Seek(prefix_);
    if (iterator_->Valid() && iterator_->key().starts_with(prefix_)) {
      valid_ = true;
      LoadCurrent();
      return;
    }
    CheckIteratorStatus();
  }
}

void EdgeTypeScanIterator::CheckIteratorStatus() const {
  if (!iterator_->status().ok()) {
    RG_THROW_CODE(StorageEngineError, "edge scan iterator failed: {}",
                  iterator_->status().ToString());
  }
}

void EdgeTypeScanIterator::Next() {
  assert(valid_);
  valid_ = false;
  iterator_->Next();
  if (scan_all_) {
    if (iterator_->Valid()) {
      valid_ = true;
      LoadCurrent();
    } else {
      CheckIteratorStatus();
    }
    return;
  }
  if (iterator_->Valid() && iterator_->key().starts_with(prefix_)) {
    valid_ = true;
    LoadCurrent();
    return;
  }
  CheckIteratorStatus();
  SeekToNextPrefix();
}

Edge &EdgeTypeScanIterator::GetEdge() {
  assert(valid_);
  return *edge_;
}

IncidentEdgeIterator::IncidentEdgeIterator(
    Transaction *txn, int64_t vertex_id, EdgeDirection direction,
    std::unordered_set<uint32_t> type_ids)
    : EdgeIterator(txn),
      vertex_id_(vertex_id),
      direction_(direction),
      type_ids_(std::move(type_ids)) {
  rocksdb::ReadOptions ro;
  iterator_.reset(
      txn_->dbtxn()->GetIterator(ro, txn->db()->graph_cf().graph_topology));
  if (direction_ == EdgeDirection::OUTGOING ||
      direction_ == EdgeDirection::BOTH) {
    std::string prefix;
    AppendBigEndianId(prefix, vertex_id_);
    prefix.append(1, 0);
    if (type_ids_.empty()) {
      prefixes_.push(prefix);
    } else {
      for (auto type : type_ids_) {
        std::string tmp = prefix;
        AppendBigEndianId(tmp, type);
        prefixes_.push(std::move(tmp));
      }
    }
  }
  if (direction_ == EdgeDirection::INCOMING ||
      direction_ == EdgeDirection::BOTH) {
    std::string prefix;
    AppendBigEndianId(prefix, vertex_id_);
    prefix.append(1, 1);
    if (type_ids_.empty()) {
      prefixes_.push(prefix);
    } else {
      for (auto type : type_ids_) {
        std::string tmp = prefix;
        AppendBigEndianId(tmp, type);
        prefixes_.push(std::move(tmp));
      }
    }
  }
  SeekToNextPrefix();
}

bool IncidentEdgeIterator::LoadCurrent() {
  auto p = iterator_->key().data();
  int64_t vid1 = ReadBigEndianId<int64_t>(p);
  p += sizeof(int64_t);
  auto dir = static_cast<EdgeDirection>(*(p));
  p += sizeof(char);
  uint32_t etid = ReadBigEndianId<uint32_t>(p);
  p += sizeof(uint32_t);
  int64_t vid2 = ReadBigEndianId<int64_t>(p);
  p += sizeof(int64_t);
  int64_t eid = ReadBigEndianId<int64_t>(p);
  if (direction_ == EdgeDirection::BOTH && dir == EdgeDirection::INCOMING &&
      vid1 == vid2) {
    return false;
  }
  if (dir == EdgeDirection::OUTGOING) {
    edge_ = std::make_unique<Edge>(txn_, eid, vid1, vid2, etid);
  } else {
    edge_ = std::make_unique<Edge>(txn_, eid, vid2, vid1, etid);
  }
  return true;
}

void IncidentEdgeIterator::SeekToNextPrefix() {
  while (!prefixes_.empty()) {
    prefix_ = prefixes_.front();
    prefixes_.pop();
    iterator_->Seek(prefix_);
    while (iterator_->Valid() && iterator_->key().starts_with(prefix_)) {
      if (LoadCurrent()) {
        valid_ = true;
        return;
      }
      iterator_->Next();
    }
  }
}

void IncidentEdgeIterator::Next() {
  assert(valid_);
  valid_ = false;
  for (iterator_->Next();
       iterator_->Valid() && iterator_->key().starts_with(prefix_);
       iterator_->Next()) {
    if (LoadCurrent()) {
      valid_ = true;
      return;
    }
  }
  SeekToNextPrefix();
}

Edge &IncidentEdgeIterator::GetEdge() {
  assert(valid_);
  return *edge_;
}

EdgePropertyFilterIterator::EdgePropertyFilterIterator(
    std::unique_ptr<EdgeIterator> source,
    std::unordered_map<uint32_t, rg::Value> properties)
    : EdgeIterator(source->transaction()),
      source_(std::move(source)),
      properties_(std::move(properties)) {
  SeekToNextMatch();
}

bool EdgePropertyFilterIterator::Matches(Edge &edge) const {
  for (const auto &[property_id, expected] : properties_) {
    if (edge.GetProperty(property_id) != expected) {
      return false;
    }
  }
  return true;
}

void EdgePropertyFilterIterator::SeekToNextMatch() {
  valid_ = false;
  while (source_->Valid()) {
    if (Matches(source_->GetEdge())) {
      valid_ = true;
      return;
    }
    source_->Next();
  }
}

void EdgePropertyFilterIterator::Next() {
  assert(valid_);
  source_->Next();
  SeekToNextMatch();
}

Edge &EdgePropertyFilterIterator::GetEdge() {
  assert(valid_);
  return source_->GetEdge();
}

EdgeOtherVertexFilterIterator::EdgeOtherVertexFilterIterator(
    std::unique_ptr<EdgeIterator> source, int64_t origin_vertex_id,
    std::optional<int64_t> other_vertex_id,
    std::unordered_set<uint32_t> other_vertex_labels,
    std::unordered_map<uint32_t, rg::Value> other_vertex_properties)
    : EdgeIterator(source->transaction()),
      source_(std::move(source)),
      origin_vertex_id_(origin_vertex_id),
      other_vertex_id_(other_vertex_id),
      other_vertex_labels_(std::move(other_vertex_labels)),
      other_vertex_properties_(std::move(other_vertex_properties)) {
  SeekToNextMatch();
}

bool EdgeOtherVertexFilterIterator::Matches(Edge &edge) const {
  Vertex other_vertex = edge.GetOtherEnd(origin_vertex_id_);
  if (other_vertex_id_.has_value() &&
      other_vertex.GetId() != *other_vertex_id_) {
    return false;
  }
  if (!other_vertex_labels_.empty()) {
    auto labels = other_vertex.GetLabelIds();
    bool found = std::any_of(labels.begin(), labels.end(), [this](uint32_t id) {
      return other_vertex_labels_.count(id) > 0;
    });
    if (!found) {
      return false;
    }
  }
  for (const auto &[property_id, expected] : other_vertex_properties_) {
    if (other_vertex.GetProperty(property_id) != expected) {
      return false;
    }
  }
  return true;
}

void EdgeOtherVertexFilterIterator::SeekToNextMatch() {
  valid_ = false;
  while (source_->Valid()) {
    if (Matches(source_->GetEdge())) {
      valid_ = true;
      return;
    }
    source_->Next();
  }
}

void EdgeOtherVertexFilterIterator::Next() {
  assert(valid_);
  source_->Next();
  SeekToNextMatch();
}

Edge &EdgeOtherVertexFilterIterator::GetEdge() {
  assert(valid_);
  return source_->GetEdge();
}

EdgeIndexSeekIterator::EdgeIndexSeekIterator(
    Transaction *txn, std::shared_ptr<EdgePropertyIndex> index,
    std::string prefix)
    : EdgeIterator(txn), index_(std::move(index)), prefix_(std::move(prefix)) {
  rocksdb::ReadOptions ro;
  if (index_->is_unique()) {
    std::string index_val;
    auto s = txn_->dbtxn()->Get(ro, index_->cf(), prefix_, &index_val);
    if (s.ok()) {
      edge_ = LoadIndexedEdge(
          txn_, *index_,
          ReadEdgePropertyIndexEid(index_, rocksdb::Slice(prefix_), index_val));
      valid_ = true;
    } else if (!s.IsNotFound()) {
      RG_THROW_CODE(StorageEngineError, s.ToString());
    }
    return;
  }
  iterator_.reset(txn_->dbtxn()->GetIterator(ro, index_->cf()));
  iterator_->Seek(prefix_);
  SeekToNextValid();
}

void EdgeIndexSeekIterator::SeekToNextValid() {
  valid_ = false;
  if (iterator_ && iterator_->Valid() &&
      iterator_->key().starts_with(prefix_)) {
    edge_ = LoadIndexedEdge(
        txn_, *index_,
        ReadEdgePropertyIndexEid(index_, iterator_->key(), iterator_->value()));
    valid_ = true;
  }
  if (iterator_ && !iterator_->status().ok()) {
    RG_THROW_CODE(StorageEngineError, iterator_->status().ToString());
  }
}

void EdgeIndexSeekIterator::Next() {
  assert(valid_);
  valid_ = false;
  if (index_->is_unique()) return;
  iterator_->Next();
  SeekToNextValid();
}

Edge &EdgeIndexSeekIterator::GetEdge() {
  assert(valid_);
  return *edge_;
}

EdgeIndexRangeIterator::EdgeIndexRangeIterator(
    Transaction *txn, std::shared_ptr<EdgePropertyIndex> index,
    std::optional<std::string> lower_key, std::optional<std::string> upper_key,
    bool left_closed, bool right_closed)
    : EdgeIterator(txn),
      index_(std::move(index)),
      lower_key_(std::move(lower_key)),
      upper_key_(std::move(upper_key)),
      left_closed_(left_closed),
      right_closed_(right_closed) {
  index_prefix_ = EncodeBigEndianId(index_->index_id());
  rocksdb::ReadOptions ro;
  iterator_.reset(txn_->dbtxn()->GetIterator(ro, index_->cf()));
  if (lower_key_) {
    iterator_->Seek(*lower_key_);
  } else {
    iterator_->Seek(index_prefix_);
  }
  SeekToNextValid();
}

void EdgeIndexRangeIterator::SeekToNextValid() {
  valid_ = false;
  while (iterator_ && iterator_->Valid() &&
         iterator_->key().starts_with(index_prefix_)) {
    if (lower_key_) {
      int cmp =
          CompareEdgeIndexKeyWithBoundPrefix(iterator_->key(), *lower_key_);
      if (cmp < 0 || (cmp == 0 && !left_closed_)) {
        iterator_->Next();
        continue;
      }
    }
    if (upper_key_) {
      int cmp =
          CompareEdgeIndexKeyWithBoundPrefix(iterator_->key(), *upper_key_);
      if (cmp > 0 || (cmp == 0 && !right_closed_)) break;
    }
    edge_ = LoadIndexedEdge(
        txn_, *index_,
        ReadEdgePropertyIndexEid(index_, iterator_->key(), iterator_->value()));
    valid_ = true;
    return;
  }
  if (iterator_ && !iterator_->status().ok()) {
    RG_THROW_CODE(StorageEngineError, iterator_->status().ToString());
  }
}

void EdgeIndexRangeIterator::Next() {
  assert(valid_);
  valid_ = false;
  iterator_->Next();
  SeekToNextValid();
}

Edge &EdgeIndexRangeIterator::GetEdge() {
  assert(valid_);
  return *edge_;
}
}  // namespace graphdb
