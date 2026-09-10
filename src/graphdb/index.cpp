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

#include "index.h"

#include <rocksdb/utilities/write_batch_with_index.h>

#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "common/byte_utils.h"
#include "common/flags.h"
#include "common/logger.h"
#include "ftindex/include/lib.rs.h"
#include "graphdb/graph_db.h"
#include "graphdb/vector_property.h"
#include "spdlog/stopwatch.h"
#include "transaction/transaction.h"

using namespace txn;
using namespace boost::endian;
using common::AsChars;
using common::ReadValue;
namespace graphdb {

namespace {

enum class FTBatchOp : uint8_t {
  Delete = 0,
  Add = 1,
};

struct FTUpdateBatch {
  ::rust::Vec<int64_t> ids;
  ::rust::Vec<uint8_t> ops;
  ::rust::Vec<uint64_t> field_counts;
  ::rust::Vec<::rust::String> fields;
  ::rust::Vec<uint64_t> value_counts;
  ::rust::Vec<::rust::String> values;

  template <typename FieldContainer, typename ValueContainer>
  void AddDocument(int64_t id, FieldContainer* field_items,
                   ValueContainer* value_items) {
    ids.push_back(id);
    ops.push_back(static_cast<uint8_t>(FTBatchOp::Add));
    field_counts.push_back(static_cast<uint64_t>(field_items->size()));
    value_counts.push_back(static_cast<uint64_t>(value_items->size()));
    for (auto& item : *field_items) {
      fields.emplace_back(std::move(item));
    }
    for (auto& item : *value_items) {
      values.emplace_back(std::move(item));
    }
  }

  void AddDelete(int64_t id) {
    ids.push_back(id);
    ops.push_back(static_cast<uint8_t>(FTBatchOp::Delete));
    field_counts.push_back(0);
    value_counts.push_back(0);
  }

  bool Empty() const { return ids.size() == 0; }

  void Clear() {
    ids.clear();
    ops.clear();
    field_counts.clear();
    fields.clear();
    value_counts.clear();
    values.clear();
  }
};

void ThrowIfIteratorError(rocksdb::Iterator* iter, std::string_view action) {
  auto status = iter->status();
  if (!status.ok()) {
    THROW_CODE(StorageEngineError, "{}: {}", action, status.ToString());
  }
}

void AppendEscapedPropertyIndexByte(std::string& encoded, unsigned char ch) {
  if (ch == 0) {
    encoded.push_back(0);
    encoded.push_back(static_cast<char>(0xFF));
  } else {
    encoded.push_back(static_cast<char>(ch));
  }
}

void AppendEscapedPropertyIndexBytes(std::string& encoded,
                                     std::string_view bytes) {
  for (unsigned char ch : bytes) {
    AppendEscapedPropertyIndexByte(encoded, ch);
  }
}

template <typename T>
void AppendEscapedPropertyIndexRaw(std::string& encoded, const T& value) {
  AppendEscapedPropertyIndexBytes(encoded, common::AsStringView(value));
}

void AppendPropertyIndexValue(std::string& encoded, const Value& value) {
  encoded.push_back(static_cast<char>(value.type));
  switch (value.type) {
    case ValueType::Null: {
      break;
    }
    case ValueType::BOOL: {
      encoded.push_back(value.AsBool() ? 1 : 0);
      break;
    }
    case ValueType::INTEGER: {
      uint64_t sortable =
          static_cast<uint64_t>(value.AsInteger()) ^ (1ULL << 63);
      sortable = native_to_big(sortable);
      encoded.append(AsChars(sortable), sizeof(sortable));
      break;
    }
    case ValueType::DOUBLE: {
      uint64_t bits = 0;
      auto number = value.AsDouble();
      std::memcpy(&bits, &number, sizeof(bits));
      bits = (bits & (1ULL << 63)) ? ~bits : (bits ^ (1ULL << 63));
      bits = native_to_big(bits);
      encoded.append(AsChars(bits), sizeof(bits));
      break;
    }
    case ValueType::FLOAT: {
      uint32_t bits = 0;
      auto number = value.AsFloat();
      std::memcpy(&bits, &number, sizeof(bits));
      bits = (bits & (1U << 31)) ? ~bits : (bits ^ (1U << 31));
      bits = native_to_big(bits);
      encoded.append(AsChars(bits), sizeof(bits));
      break;
    }
    case ValueType::STRING: {
      AppendEscapedPropertyIndexBytes(encoded, value.AsString());
      encoded.push_back(0);
      encoded.push_back(0);
      break;
    }
    case ValueType::ARRAY: {
      const auto& array = value.AsArray();
      if (!array.empty()) {
        auto t = array[0].type;
        AppendEscapedPropertyIndexByte(encoded, static_cast<unsigned char>(t));
        for (const auto& item : array) {
          if (item.type != t) {
            THROW_CODE(
                ValueException,
                "Array elements must have the same type for serializing, "
                "error type: " +
                    ::ToString(item.type));
          }
          switch (item.type) {
            case ValueType::BOOL: {
              AppendEscapedPropertyIndexByte(
                  encoded, static_cast<unsigned char>(item.AsBool()));
              break;
            }
            case ValueType::INTEGER: {
              AppendEscapedPropertyIndexRaw(encoded, item.AsInteger());
              break;
            }
            case ValueType::DOUBLE: {
              AppendEscapedPropertyIndexRaw(encoded, item.AsDouble());
              break;
            }
            case ValueType::FLOAT: {
              AppendEscapedPropertyIndexRaw(encoded, item.AsFloat());
              break;
            }
            case ValueType::STRING: {
              const auto& str = item.AsString();
              size_t len = str.size();
              AppendEscapedPropertyIndexRaw(encoded, len);
              AppendEscapedPropertyIndexBytes(encoded, str);
              break;
            }
            default: {
              THROW_CODE(ValueException,
                         "Unsupported data type for serializing array, type: " +
                             ::ToString(item.type));
            }
          }
        }
      }
      encoded.push_back(0);
      encoded.push_back(0);
      break;
    }
    case ValueType::DATE: {
      uint64_t sortable =
          static_cast<uint64_t>(value.AsDate().GetStorage()) ^ (1ULL << 63);
      sortable = native_to_big(sortable);
      encoded.append(AsChars(sortable), sizeof(sortable));
      break;
    }
    case ValueType::LOCALDATETIME: {
      uint64_t sortable =
          static_cast<uint64_t>(value.AsLocalDateTime().GetStorage()) ^
          (1ULL << 63);
      sortable = native_to_big(sortable);
      encoded.append(AsChars(sortable), sizeof(sortable));
      break;
    }
    case ValueType::LOCALTIME: {
      uint64_t sortable =
          static_cast<uint64_t>(value.AsLocalTime().GetStorage()) ^
          (1ULL << 63);
      sortable = native_to_big(sortable);
      encoded.append(AsChars(sortable), sizeof(sortable));
      break;
    }
    case ValueType::TIME: {
      auto storage = value.AsTime().GetStorage();
      uint64_t sortable =
          static_cast<uint64_t>(std::get<0>(storage) -
                                std::get<1>(storage) * NANOS_PER_SECOND) ^
          (1ULL << 63);
      sortable = native_to_big(sortable);
      encoded.append(AsChars(sortable), sizeof(sortable));
      break;
    }
    case ValueType::DATETIME: {
      auto storage = value.AsDateTime().GetStorage();
      uint64_t sortable =
          static_cast<uint64_t>(std::get<0>(storage)) ^ (1ULL << 63);
      sortable = native_to_big(sortable);
      encoded.append(AsChars(sortable), sizeof(sortable));
      break;
    }
    case ValueType::DURATION: {
      auto duration = value.AsDuration();
      uint64_t months =
          native_to_big(static_cast<uint64_t>(duration.months) ^ (1ULL << 63));
      uint64_t days =
          native_to_big(static_cast<uint64_t>(duration.days) ^ (1ULL << 63));
      uint64_t seconds =
          native_to_big(static_cast<uint64_t>(duration.seconds) ^ (1ULL << 63));
      uint64_t nanos =
          native_to_big(static_cast<uint64_t>(duration.nanos) ^ (1ULL << 63));
      encoded.append(AsChars(months), sizeof(months));
      encoded.append(AsChars(days), sizeof(days));
      encoded.append(AsChars(seconds), sizeof(seconds));
      encoded.append(AsChars(nanos), sizeof(nanos));
      break;
    }
    case ValueType::MAP:
    default: {
      THROW_CODE(ValueException,
                 "Unsupported data type for property index, type: {}",
                 ::ToString(value.type));
    }
  }
}

std::string EncodePropertyIndexValues(const std::vector<Value>& values) {
  std::string encoded;
  for (const auto& value : values) {
    AppendPropertyIndexValue(encoded, value);
  }
  return encoded;
}

Value DeserializeStoredPropertyValue(const std::string& value) {
  if (value.empty()) {
    THROW_CODE(InvalidParameter, "Indexed property value is invalid");
  }
  Value decoded;
  decoded.Deserialize(value.data(), value.size());
  return decoded;
}

uint64_t LoadVisibleMaxWalId(rocksdb::TransactionDB* db, GraphCF* graph_cf,
                             uint32_t index_id,
                             const rocksdb::Snapshot* snapshot) {
  std::string prefix(AsChars(index_id), sizeof(index_id));
  std::string seek_key(prefix);
  seek_key.append(sizeof(uint64_t), static_cast<char>(0xFF));
  rocksdb::ReadOptions ro;
  ro.snapshot = snapshot;
  std::unique_ptr<rocksdb::Iterator> iter(db->NewIterator(ro, graph_cf->wal));
  iter->SeekForPrev(seek_key);
  if (!iter->Valid()) {
    return 0;
  }
  auto key = iter->key();
  if (!key.starts_with(prefix)) {
    return 0;
  }
  key.remove_prefix(sizeof(index_id));
  if (key.size() != sizeof(uint64_t)) {
    THROW_CODE(StorageEngineError,
               "index wal key has invalid size while loading max wal id, "
               "expect {}, actual {}",
               sizeof(uint64_t), key.size());
  }
  return big_to_native(ReadValue<uint64_t>(key.data()));
}

}  // namespace

void VertexPropertyIndex::AddIndex(Transaction* txn, int64_t vid,
                                   const std::vector<Value>& values) {
  UpdateIndex(txn, vid, values, std::nullopt);
}

void VertexPropertyIndex::UpdateIndexDirect(
    Transaction* txn, int64_t vid,
    const std::optional<std::vector<Value>>& new_values,
    const std::optional<std::vector<Value>>& old_values) {
  if (!new_values && !old_values) {
    return;
  }
  std::string new_key;
  std::string old_key;
  if (meta_.is_unique()) {
    rocksdb::ReadOptions ro;
    bool keep_existing_entry = false;
    if (new_values) {
      std::string tmp;
      new_key = IndexKey(*new_values);
      auto s = txn->dbtxn()->GetForUpdate(ro, cf_, new_key, &tmp);
      if (s.ok()) {
        if (tmp.size() != sizeof(int64_t)) {
          THROW_CODE(StorageEngineError,
                     "vertex unique index stores invalid vid size");
        }
        if (ReadValue<int64_t>(tmp.data()) != vid) {
          THROW_CODE(IndexValueAlreadyExist);
        }
        keep_existing_entry = true;
      } else if (!s.IsNotFound()) {
        THROW_CODE(StorageEngineError, s.ToString());
      }
    }
    if (old_values) {
      old_key = IndexKey(*old_values);
    }
    if (old_values && (!new_values || old_key != new_key)) {
      auto s = txn->dbtxn()->GetForUpdate(ro, cf_, old_key,
                                          static_cast<std::string*>(nullptr));
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      s = txn->dbtxn()->GetWriteBatch()->SingleDelete(cf_, old_key);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      keep_existing_entry = false;
    }
    if (new_values && !keep_existing_entry) {
      auto s = txn->dbtxn()->GetWriteBatch()->Put(
          cf_, new_key, rocksdb::Slice(AsChars(vid), sizeof(vid)));
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
  } else {
    if (new_values) {
      new_key = EntryKey(*new_values, vid);
    }
    if (old_values) {
      old_key = EntryKey(*old_values, vid);
    }
    if (old_values && (!new_values || old_key != new_key)) {
      auto s = txn->dbtxn()->GetWriteBatch()->Delete(cf_, old_key);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
    if (new_values && (!old_values || old_key != new_key)) {
      auto s = txn->dbtxn()->GetWriteBatch()->Put(cf_, new_key, {});
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
  }
}

void VertexPropertyIndex::AppendBuildUpdate(txn::Transaction* txn,
                                            meta::UpdateType type, int64_t vid,
                                            const std::vector<Value>& values) {
  meta::PropertyIndexUpdate update;
  update.set_type(type);
  update.set_vid(vid);
  for (const auto& value : values) {
    update.add_values(value.Serialize());
  }
  txn->AppendPropertyIndexWAL(shared_from_this(), update);
}

void VertexPropertyIndex::UpdateIndex(
    Transaction* txn, int64_t vid,
    const std::optional<std::vector<Value>>& new_values,
    const std::optional<std::vector<Value>>& old_values) {
  if (!new_values && !old_values) {
    return;
  }
  if (IsReady()) {
    UpdateIndexDirect(txn, vid, new_values, old_values);
    return;
  }
  std::string new_key;
  std::string old_key;
  if (new_values) {
    new_key =
        meta_.is_unique() ? IndexKey(*new_values) : EntryKey(*new_values, vid);
  }
  if (old_values) {
    old_key =
        meta_.is_unique() ? IndexKey(*old_values) : EntryKey(*old_values, vid);
  }
  if (old_values && (!new_values || old_key != new_key)) {
    AppendBuildUpdate(txn, meta::UpdateType::Delete, vid, *old_values);
  }
  if (new_values && (!old_values || old_key != new_key)) {
    AppendBuildUpdate(txn, meta::UpdateType::Add, vid, *new_values);
  }
}

void VertexPropertyIndex::ApplyBuildUpdate(
    const meta::PropertyIndexUpdate& update) {
  std::vector<Value> values;
  values.reserve(update.values_size());
  for (const auto& item : update.values()) {
    values.push_back(DeserializeStoredPropertyValue(item));
  }
  rocksdb::WriteOptions wo;
  rocksdb::Status s;
  if (meta_.is_unique()) {
    rocksdb::ReadOptions ro;
    auto index_key = IndexKey(values);
    std::string current_vid;
    s = db_->Get(ro, cf_, index_key, &current_vid);
    if (update.type() == meta::UpdateType::Add) {
      if (s.ok()) {
        if (current_vid.size() != sizeof(int64_t)) {
          THROW_CODE(StorageEngineError,
                     "vertex unique index stores invalid vid size");
        }
        if (ReadValue<int64_t>(current_vid.data()) != update.vid()) {
          THROW_CODE(IndexValueAlreadyExist);
        }
        return;
      }
      if (!s.IsNotFound()) {
        THROW_CODE(StorageEngineError, s.ToString());
      }
      s = db_->Put(wo, cf_, index_key,
                   rocksdb::Slice(AsChars(update.vid()), sizeof(update.vid())));
    } else if (update.type() == meta::UpdateType::Delete) {
      if (s.IsNotFound()) {
        return;
      }
      if (!s.ok()) {
        THROW_CODE(StorageEngineError, s.ToString());
      }
      if (current_vid.size() != sizeof(int64_t)) {
        THROW_CODE(StorageEngineError,
                   "vertex unique index stores invalid vid size");
      }
      if (ReadValue<int64_t>(current_vid.data()) != update.vid()) {
        return;
      }
      s = db_->Delete(wo, cf_, index_key);
    } else {
      THROW_CODE(StorageEngineError,
                 "property index wal has invalid update type: {}",
                 static_cast<int>(update.type()));
    }
  } else {
    auto entry_key = EntryKey(values, update.vid());
    if (update.type() == meta::UpdateType::Add) {
      s = db_->Put(wo, cf_, entry_key, {});
    } else if (update.type() == meta::UpdateType::Delete) {
      s = db_->Delete(wo, cf_, entry_key);
    } else {
      THROW_CODE(StorageEngineError,
                 "property index wal has invalid update type: {}",
                 static_cast<int>(update.type()));
    }
  }
  if (!s.ok()) {
    THROW_CODE(StorageEngineError, s.ToString());
  }
}

void VertexPropertyIndex::ApplyCommittedBuildUpdate(
    txn::Transaction* txn, const meta::PropertyIndexUpdate& update) {
  std::vector<Value> values;
  values.reserve(update.values_size());
  for (const auto& item : update.values()) {
    values.push_back(DeserializeStoredPropertyValue(item));
  }
  if (update.type() == meta::UpdateType::Add) {
    UpdateIndexDirect(txn, update.vid(), values, std::nullopt);
  } else if (update.type() == meta::UpdateType::Delete) {
    UpdateIndexDirect(txn, update.vid(), std::nullopt, values);
  } else {
    THROW_CODE(StorageEngineError,
               "property index wal has invalid update type: {}",
               static_cast<int>(update.type()));
  }
}

void VertexPropertyIndex::Load(const rocksdb::Snapshot* snapshot,
                               uint64_t snapshot_wal_id) {
  rocksdb::ReadOptions ro;
  ro.snapshot = snapshot;
  std::unique_ptr<rocksdb::Iterator> iter(
      db_->NewIterator(ro, graph_cf_->vertex_label_vid));
  rocksdb::Slice prefix(AsChars(lid_), sizeof(lid_));
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key();
    key.remove_prefix(sizeof(uint32_t));
    int64_t vid = ReadValue<int64_t>(key.data());
    std::vector<Value> values;
    values.reserve(pids_.size());
    bool complete = true;
    for (auto pid : pids_) {
      std::string property_key(AsChars(vid), sizeof(vid));
      property_key.append(AsChars(pid), sizeof(pid));
      std::string property_val;
      auto s =
          db_->Get(ro, graph_cf_->vertex_property, property_key, &property_val);
      if (s.IsNotFound()) {
        complete = false;
        break;
      }
      if (!s.ok()) {
        THROW_CODE(StorageEngineError, s.ToString());
      }
      values.push_back(DeserializeStoredPropertyValue(property_val));
    }
    if (!complete) {
      continue;
    }
    meta::PropertyIndexUpdate add;
    add.set_type(meta::UpdateType::Add);
    add.set_vid(vid);
    for (const auto& value : values) {
      add.add_values(value.Serialize());
    }
    ApplyBuildUpdate(add);
  }
  ThrowIfIteratorError(iter.get(),
                       "vertex property index load iterator failed");
  apply_id_ = native_to_big(snapshot_wal_id);
  meta_.set_applied_wal_id(snapshot_wal_id);
}

void VertexPropertyIndex::ApplyWAL() {
  std::string prefix(AsChars(index_id_), sizeof(index_id_));
  std::string start_key(prefix);
  uint64_t next = big_to_native(apply_id_) + 1;
  native_to_big_inplace(next);
  start_key.append(AsChars(next), sizeof(next));
  uint64_t consumed_wal_id = 0;
  rocksdb::WriteBatch delete_batch;
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> iter(db_->NewIterator(ro, graph_cf_->wal));
  for (iter->Seek(start_key); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key();
    delete_batch.Delete(graph_cf_->wal, key.ToString());

    key.remove_prefix(sizeof(index_id_));
    if (key.size() != sizeof(apply_id_)) {
      THROW_CODE(
          StorageEngineError,
          "property index wal key has invalid size, expect {}, actual {}",
          sizeof(apply_id_), key.size());
    }
    consumed_wal_id = ReadValue<uint64_t>(key.data());
    meta::PropertyIndexUpdate update;
    auto val = iter->value();
    if (!update.ParseFromArray(val.data(), val.size())) {
      THROW_CODE(StorageEngineError,
                 "failed to parse property index wal payload");
    }
    ApplyBuildUpdate(update);
  }
  ThrowIfIteratorError(iter.get(),
                       "vertex property index wal iteration failed");
  if (consumed_wal_id != 0) {
    rocksdb::WriteOptions wo;
    rocksdb::TransactionDBWriteOptimizations two;
    two.skip_concurrency_control = true;
    two.skip_duplicate_key_check = true;
    auto s = db_->Write(wo, two, &delete_batch);
    if (!s.ok()) {
      THROW_CODE(StorageEngineError, s.ToString());
    }
    apply_id_ = consumed_wal_id;
    meta_.set_applied_wal_id(big_to_native(consumed_wal_id));
  }
}

std::string VertexPropertyIndex::NextWALKey() {
  std::string ret(AsChars(index_id_), sizeof(index_id_));
  uint64_t wal_id = native_to_big(next_wal_id_++);
  ret.append(AsChars(wal_id), sizeof(wal_id));
  return ret;
}

std::string VertexPropertyIndex::IndexKey(
    const std::vector<Value>& values) const {
  std::string index_key(AsChars(index_id_), sizeof(index_id_));
  index_key.append(EncodePropertyIndexValues(values));
  return index_key;
}

std::string VertexPropertyIndex::EntryKey(const std::vector<Value>& values,
                                          int64_t vid) const {
  std::string index_key = IndexKey(values);
  index_key.append(AsChars(vid), sizeof(vid));
  return index_key;
}

std::optional<std::vector<Value>>
VertexPropertyIndex::LoadIndexedPropertyValues(
    txn::Transaction* txn, int64_t vid,
    const std::unordered_map<uint32_t, std::string>* overrides,
    const std::unordered_set<uint32_t>* removed) const {
  std::vector<Value> values;
  values.reserve(pids_.size());
  rocksdb::ReadOptions ro;
  for (auto pid : pids_) {
    if (removed && removed->count(pid)) {
      return std::nullopt;
    }
    if (overrides) {
      auto iter = overrides->find(pid);
      if (iter != overrides->end()) {
        values.push_back(DeserializeStoredPropertyValue(iter->second));
        continue;
      }
    }
    std::string property_key(AsChars(vid), sizeof(vid));
    property_key.append(AsChars(pid), sizeof(pid));
    std::string property_val;
    auto s = txn->dbtxn()->Get(ro, txn->db()->graph_cf().vertex_property,
                               property_key, &property_val);
    if (s.IsNotFound()) {
      return std::nullopt;
    }
    if (!s.ok()) {
      THROW_CODE(StorageEngineError, s.ToString());
    }
    values.push_back(DeserializeStoredPropertyValue(property_val));
  }
  return values;
}

bool VertexPropertyIndex::TouchesAnyProperty(
    const std::unordered_set<uint32_t>& pids) const {
  for (auto pid : pids) {
    if (pid_set_.count(pid)) {
      return true;
    }
  }
  return false;
}

bool VertexPropertyIndex::AllPropertiesPresent(
    const std::unordered_set<uint32_t>& pids) const {
  for (auto pid : pids_) {
    if (!pids.count(pid)) {
      return false;
    }
  }
  return true;
}

void VertexPropertyIndex::DeleteIndex(Transaction* txn, int64_t vid,
                                      const std::vector<Value>& values) {
  UpdateIndex(txn, vid, std::nullopt, values);
}

void VertexPropertyIndex::ResetForBuild() {
  deleted_.store(false);
  next_wal_id_ = LoadVisibleMaxWalId(db_, graph_cf_, index_id_, nullptr) + 1;
  apply_id_ = 0;
  meta_.set_applied_wal_id(0);
  meta_.clear_build_error();
}

void VertexFullTextIndex::StartTimer() {
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (stopped_) {
      return;
    }
  }
  timer_.expires_after(std::chrono::seconds(interval_));
  timer_.async_wait(strand_->wrap([this](const boost::system::error_code& e) {
    if (e) {
      if (e != boost::asio::error::operation_aborted) {
        LOG_ERROR("timer async_wait error: {}", e.message());
      }
      return;
    }
    {
      std::lock_guard<std::mutex> lock(timer_mutex_);
      if (stopped_) {
        timer_cv_.notify_all();
        return;
      }
      active_callbacks_++;
    }
    bool restart = false;
    try {
      ApplyWAL();
    } catch (const std::exception& ex) {
      LOG_ERROR("fulltext index [{}] apply WAL failed: {}", meta_.name(),
                ex.what());
    } catch (...) {
      LOG_ERROR("fulltext index [{}] apply WAL failed with unknown error",
                meta_.name());
    }
    {
      std::lock_guard<std::mutex> lock(timer_mutex_);
      active_callbacks_--;
      timer_cv_.notify_all();
      restart = !stopped_;
    }
    if (restart) {
      StartTimer();
    }
  }));
}

void VertexFullTextIndex::Start() {
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (started_ || stopped_) {
      return;
    }
    started_ = true;
  }
  StartTimer();
}

void VertexFullTextIndex::Stop() {
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    if (!started_) {
      return;
    }
  }

  if (strand_->running_in_this_thread()) {
    boost::system::error_code ec;
    timer_.cancel(ec);
    return;
  }

  std::promise<void> cancelled;
  auto future = cancelled.get_future();
  strand_->post([this, &cancelled]() mutable {
    boost::system::error_code ec;
    timer_.cancel(ec);
    cancelled.set_value();
  });
  future.wait();

  std::unique_lock<std::mutex> lock(timer_mutex_);
  timer_cv_.wait(lock, [this] { return active_callbacks_ == 0; });
}

void VertexFullTextIndex::ReleaseResources() {
  std::lock_guard<std::mutex> lock(mutex_);
  ft_index_ = nullptr;
  instance_.reset();
}

void VertexFullTextIndex::ResetForClear() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    deleted_.store(false);
    apply_id_ = 0;
    next_wal_id_ = 1;
    ft_index_ = nullptr;
    instance_.reset();
    ::rust::Vec<::rust::String> fields;
    for (const auto& prop : meta_.properties()) {
      fields.push_back(prop);
    }
    instance_ = std::make_unique<::rust::Box<::FTIndex>>(new_ftindex(
        meta_.path(), fields, writer_threads_, writer_memory_budget_));
    ft_index_ = instance_->operator->();
  }
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    active_callbacks_ = 0;
    started_ = false;
    stopped_ = false;
  }
}

VertexFullTextIndex::VertexFullTextIndex(
    rocksdb::TransactionDB* db, boost::asio::io_service& service,
    boost::asio::io_service::strand* strand, GraphCF* graph_cf,
    IdGenerator* id_generator, meta::VertexFullTextIndex meta,
    uint32_t index_id, size_t writer_threads, size_t writer_memory_budget,
    const std::unordered_set<uint32_t>& lids,
    const std::unordered_set<uint32_t>& pids, size_t commit_interval)
    : db_(db),
      graph_cf_(graph_cf),
      id_generator_(id_generator),
      meta_(std::move(meta)),
      index_id_(index_id),
      lids_(lids),
      pids_(pids),
      interval_(commit_interval),
      writer_threads_(writer_threads),
      writer_memory_budget_(writer_memory_budget),
      strand_(strand),
      timer_(service) {
  ::rust::Vec<::rust::String> fields;
  for (auto& prop : meta_.properties()) {
    fields.push_back(prop);
  }
  instance_ = std::make_unique<::rust::Box<::FTIndex>>(
      new_ftindex(meta_.path(), fields, writer_threads, writer_memory_budget));
  ft_index_ = instance_->operator->();
  if (meta_.state() == meta::IndexBuildState::READY) {
    auto payload = ft_get_payload(*ft_index_);
    if (!payload.empty()) {
      apply_id_ =
          native_to_big(static_cast<uint64_t>(std::stoull(payload.c_str())));
    }
  } else {
    apply_id_ = native_to_big(meta_.applied_wal_id());
  }
  meta_.set_applied_wal_id(big_to_native(apply_id_));

  std::string prefix(AsChars(index_id_), sizeof(index_id_));
  prefix.append(8, 0xFF);
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> iter(db_->NewIterator(ro, graph_cf_->wal));
  iter->SeekForPrev(prefix);
  if (iter->Valid()) {
    auto key = iter->key();
    if (key.starts_with({AsChars(index_id_), sizeof(index_id_)})) {
      key.remove_prefix(sizeof(index_id_));
      if (key.size() != sizeof(uint64_t)) {
        THROW_CODE(StorageEngineError,
                   "fulltext index wal key has invalid size while loading next "
                   "wal id, expect {}, actual {}",
                   sizeof(uint64_t), key.size());
      }
      uint64_t wal_id = ReadValue<uint64_t>(key.data());
      next_wal_id_ = big_to_native(wal_id) + 1;
    }
  }
  ThrowIfIteratorError(iter.get(),
                       "fulltext index next wal id iterator failed");
  next_wal_id_ = std::max(next_wal_id_.load(), big_to_native(apply_id_) + 1);
}

void VertexFullTextIndex::AddIndex(txn::Transaction* txn, int64_t vid,
                                   const meta::FullTextIndexUpdate& wal) {
  (void)vid;
  txn->AppendFullTextIndexWAL(shared_from_this(), wal);
}

void VertexFullTextIndex::DeleteIndex(txn::Transaction* txn, int64_t vid,
                                      const meta::FullTextIndexUpdate& wal) {
  (void)vid;
  txn->AppendFullTextIndexWAL(shared_from_this(), wal);
}

std::string VertexFullTextIndex::NextWALKey() {
  std::string ret(AsChars(index_id_), sizeof(index_id_));
  uint64_t wal_id = native_to_big(next_wal_id_++);
  ret.append(AsChars(wal_id), sizeof(wal_id));
  return ret;
}

void VertexFullTextIndex::Load(const rocksdb::Snapshot* snapshot,
                               uint64_t snapshot_wal_id) {
  int count = 0;
  bool has_documents = false;
  FTUpdateBatch batch;
  std::vector<std::pair<uint32_t, std::string>> indexed_properties;
  indexed_properties.reserve(pids_.size());
  for (auto pid : pids_) {
    indexed_properties.emplace_back(
        pid, id_generator_->GetPropertyName(pid).value());
  }
  std::optional<std::unordered_set<int64_t>> loaded_vids;
  if (lids_.size() > 1) {
    loaded_vids.emplace();
  }
  for (auto lid : lids_) {
    rocksdb::ReadOptions ro;
    ro.snapshot = snapshot;
    std::unique_ptr<rocksdb::Iterator> iter(
        db_->NewIterator(ro, graph_cf_->vertex_label_vid));
    rocksdb::Slice prefix(AsChars(lid), sizeof(lid));
    for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
         iter->Next()) {
      auto key = iter->key();
      key.remove_prefix(sizeof(uint32_t));
      int64_t id = ReadValue<int64_t>(key.data());
      if (loaded_vids) {
        // A vertex may appear in multiple label scans; skip it before
        // property IO.
        if (!loaded_vids->emplace(id).second) {
          continue;
        }
      }
      std::vector<std::string> fields;
      std::vector<std::string> values;
      fields.reserve(indexed_properties.size());
      values.reserve(indexed_properties.size());
      std::string property_key(key.data(), key.size());
      size_t vertex_key_size = property_key.size();
      for (const auto& [pid, prop_name] : indexed_properties) {
        std::string property_val;
        property_key.resize(vertex_key_size);
        property_key.append(AsChars(pid), sizeof(pid));
        auto s = db_->Get(ro, graph_cf_->vertex_property, property_key,
                          &property_val);
        if (s.IsNotFound()) {
          continue;
        } else if (!s.ok()) {
          THROW_CODE(StorageEngineError, s.ToString());
        }
        Value pv;
        pv.Deserialize(property_val.data(), property_val.size());
        if (!pv.IsString()) {
          continue;
        }
        fields.push_back(prop_name);
        values.push_back(pv.AsString());
      }
      if (!fields.empty()) {
        has_documents = true;
        batch.AddDocument(id, &fields, &values);
        count++;
        if (count == 10000) {
          ApplyUpdatesBatch(batch.ids, batch.ops, batch.field_counts,
                            batch.fields, batch.value_counts, batch.values);
          count = 0;
          batch.Clear();
        }
      }
    }
    ThrowIfIteratorError(iter.get(),
                         "vertex fulltext index load iterator failed");
  }
  if (count > 0) {
    ApplyUpdatesBatch(batch.ids, batch.ops, batch.field_counts, batch.fields,
                      batch.value_counts, batch.values);
  }
  if (has_documents) {
    Commit(std::to_string(snapshot_wal_id));
  }
  apply_id_ = native_to_big(snapshot_wal_id);
  meta_.set_applied_wal_id(snapshot_wal_id);
}

void VertexFullTextIndex::AddVertex(int64_t id, std::vector<std::string> fields,
                                    std::vector<std::string> values) {
  FTUpdateBatch batch;
  batch.AddDocument(id, &fields, &values);
  ApplyUpdatesBatch(batch.ids, batch.ops, batch.field_counts, batch.fields,
                    batch.value_counts, batch.values);
}

bool VertexFullTextIndex::MatchLabelIds(
    const std::unordered_set<uint32_t>& lids) const {
  return std::any_of(lids.begin(), lids.end(), [this](uint32_t lid) {
    return lids_.find(lid) != lids_.end();
  });
}

bool VertexFullTextIndex::MatchPropertyIds(
    const std::unordered_set<uint32_t>& pids) const {
  return std::any_of(pids.begin(), pids.end(), [this](uint32_t lid) {
    return pids_.find(lid) != pids_.end();
  });
}

void VertexFullTextIndex::DeleteVertex(int64_t id) {
  FTUpdateBatch batch;
  batch.AddDelete(id);
  ApplyUpdatesBatch(batch.ids, batch.ops, batch.field_counts, batch.fields,
                    batch.value_counts, batch.values);
}

void VertexFullTextIndex::ApplyUpdatesBatch(
    const ::rust::Vec<int64_t>& ids, const ::rust::Vec<uint8_t>& ops,
    const ::rust::Vec<uint64_t>& field_counts,
    const ::rust::Vec<::rust::String>& fields,
    const ::rust::Vec<uint64_t>& value_counts,
    const ::rust::Vec<::rust::String>& values) {
  ft_apply_updates(*ft_index_, ids, ops, field_counts, fields, value_counts,
                   values);
}

void VertexFullTextIndex::Commit(const std::string& payload) {
  ft_commit(*ft_index_, payload);
}

void VertexFullTextIndex::ApplyWAL() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string prefix(AsChars(index_id_), sizeof(index_id_));
  std::string start_key(prefix);
  uint64_t next = big_to_native(apply_id_) + 1;
  native_to_big_inplace(next);
  start_key.append(AsChars(next), sizeof(next));
  int count = 0;
  uint64_t consumed_wal_id = 0;
  FTUpdateBatch batch;
  rocksdb::WriteBatch write_batch;
  rocksdb::ReadOptions ro;
  rocksdb::WriteOptions wo;
  std::unique_ptr<rocksdb::Iterator> iter(db_->NewIterator(ro, graph_cf_->wal));
  for (iter->Seek(start_key); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key();
    write_batch.Delete(graph_cf_->wal, key.ToString());

    key.remove_prefix(sizeof(index_id_));
    if (key.size() != sizeof(apply_id_)) {
      THROW_CODE(
          StorageEngineError,
          "fulltext index wal key has invalid size, expect {}, actual {}",
          sizeof(apply_id_), key.size());
    }
    consumed_wal_id = ReadValue<uint64_t>(key.data());
    meta::FullTextIndexUpdate update;
    auto val = iter->value();
    auto ret = update.ParseFromArray(val.data(), val.size());
    if (!ret) {
      THROW_CODE(StorageEngineError,
                 "failed to parse fulltext index wal payload");
    }
    if (update.type() == meta::UpdateType::Add) {
      batch.AddDocument(update.vid(), update.mutable_fields(),
                        update.mutable_values());
    } else if (update.type() == meta::UpdateType::Delete) {
      batch.AddDelete(update.vid());
    } else {
      THROW_CODE(StorageEngineError,
                 "fulltext index wal has invalid update type: {}",
                 static_cast<int>(update.type()));
    }
    if (++count == 1000) {
      ApplyUpdatesBatch(batch.ids, batch.ops, batch.field_counts, batch.fields,
                        batch.value_counts, batch.values);
      auto payload = std::to_string(big_to_native(consumed_wal_id));
      Commit(payload);
      LOG_DEBUG("apply {} wal, payload: {}", count, payload);
      count = 0;
      rocksdb::TransactionDBWriteOptimizations two;
      two.skip_concurrency_control = true;
      two.skip_duplicate_key_check = true;
      auto s = db_->Write(wo, two, &write_batch);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      write_batch.Clear();
      batch.Clear();
    }
  }
  ThrowIfIteratorError(iter.get(),
                       "vertex fulltext index wal iteration failed");
  if (count > 0) {
    ApplyUpdatesBatch(batch.ids, batch.ops, batch.field_counts, batch.fields,
                      batch.value_counts, batch.values);
    auto payload = std::to_string(big_to_native(consumed_wal_id));
    Commit(payload);
    LOG_DEBUG("apply {} wal, payload: {}", count, payload);
    count = 0;
    rocksdb::TransactionDBWriteOptimizations two;
    two.skip_concurrency_control = true;
    two.skip_duplicate_key_check = true;
    auto s = db_->Write(wo, two, &write_batch);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    write_batch.Clear();
  }
  if (consumed_wal_id != 0) {
    apply_id_ = consumed_wal_id;
    meta_.set_applied_wal_id(big_to_native(consumed_wal_id));
  }
}

::rust::Vec<::IdScore> VertexFullTextIndex::Query(const std::string& query,
                                                  size_t top_n) {
  return ft_query(*ft_index_, query, QueryOptions{top_n});
}

VertexVectorIndex::VertexVectorIndex(rocksdb::TransactionDB* db,
                                     boost::asio::io_service& service,
                                     boost::asio::io_service::strand* strand,
                                     graphdb::GraphCF* graph_cf,
                                     uint32_t index_id, uint32_t lid,
                                     uint32_t pid, meta::VertexVectorIndex meta,
                                     size_t commit_interval)
    : db_(db),
      graph_cf_(graph_cf),
      index_id_(index_id),
      lid_(lid),
      pid_(pid),
      meta_(std::move(meta)),
      interval_(commit_interval),
      strand_(strand),
      timer_(service) {
  if (meta_.distance_type() != meta::VectorDistanceType::L2 &&
      meta_.distance_type() != meta::VectorDistanceType::IP) {
    THROW_CODE(VectorIndexException, "invalid metric_type: {}",
               meta::VectorDistanceType_Name(meta_.distance_type()));
  }
  vector_store_ = std::make_unique<VectorStore>(
      meta_.path(), meta_.dimensions(), meta_.distance_type(), meta_.hnsw_m(),
      meta_.hnsw_ef_construction());

  if (meta_.state() == meta::IndexBuildState::READY) {
    auto applied_wal_id = meta_.applied_wal_id();
    if (vector_store_->has_checkpoint()) {
      applied_wal_id = vector_store_->checkpoint_applied_wal_id();
    }
    apply_id_ = native_to_big(applied_wal_id);
  } else {
    apply_id_ = native_to_big(meta_.applied_wal_id());
  }
  LOG_INFO("vector index {}, apply_id:{}", meta_.name(),
           big_to_native(apply_id_));
  meta_.set_applied_wal_id(big_to_native(apply_id_));
  {
    std::string prefix(AsChars(index_id_), sizeof(index_id_));
    prefix.append(8, 0xFF);
    rocksdb::ReadOptions ro;
    std::unique_ptr<rocksdb::Iterator> iter(
        db_->NewIterator(ro, graph_cf_->wal));
    iter->SeekForPrev(prefix);
    if (iter->Valid()) {
      auto key = iter->key();
      if (key.starts_with({AsChars(index_id_), sizeof(index_id_)})) {
        key.remove_prefix(sizeof(index_id_));
        if (key.size() != sizeof(uint64_t)) {
          THROW_CODE(
              VectorIndexException,
              "vector index wal key has invalid size while loading next wal "
              "id, expect {}, actual {}",
              sizeof(uint64_t), key.size());
        }
        uint64_t wal_id = ReadValue<uint64_t>(key.data());
        next_wal_id_ = big_to_native(wal_id) + 1;
      }
    }
    ThrowIfIteratorError(iter.get(),
                         "vector index next wal id iterator failed");
    next_wal_id_ = std::max(next_wal_id_.load(), big_to_native(apply_id_) + 1);
    LOG_INFO("vector index {}, next_wal_id: {}", meta_.name(),
             next_wal_id_.load());
  }
}

void VertexVectorIndex::StartTimer() {
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (stopped_) {
      return;
    }
  }
  timer_.expires_after(std::chrono::seconds(interval_));
  timer_.async_wait(strand_->wrap([this](const boost::system::error_code& e) {
    if (e) {
      if (e != boost::asio::error::operation_aborted) {
        LOG_ERROR("timer async_wait error: {}", e.message());
      }
      return;
    }
    {
      std::lock_guard<std::mutex> lock(timer_mutex_);
      if (stopped_) {
        timer_cv_.notify_all();
        return;
      }
      active_callbacks_++;
    }
    try {
      ApplyWAL();
    } catch (const std::exception& ex) {
      LOG_ERROR("vector index [{}] apply WAL failed: {}", meta_.name(),
                ex.what());
    } catch (...) {
      LOG_ERROR("vector index [{}] apply WAL failed with unknown error",
                meta_.name());
    }
    bool restart = false;
    {
      std::lock_guard<std::mutex> lock(timer_mutex_);
      active_callbacks_--;
      timer_cv_.notify_all();
      restart = !stopped_;
    }
    if (restart) {
      StartTimer();
    }
  }));
}

void VertexVectorIndex::Start() {
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (started_ || stopped_) {
      return;
    }
    started_ = true;
  }
  StartTimer();
}

void VertexVectorIndex::Stop() {
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
    if (!started_) {
      return;
    }
  }

  if (strand_->running_in_this_thread()) {
    boost::system::error_code ec;
    timer_.cancel(ec);
    return;
  }

  std::promise<void> cancelled;
  auto future = cancelled.get_future();
  strand_->post([this, &cancelled]() mutable {
    boost::system::error_code ec;
    timer_.cancel(ec);
    cancelled.set_value();
  });
  future.wait();

  std::unique_lock<std::mutex> lock(timer_mutex_);
  timer_cv_.wait(lock, [this] { return active_callbacks_ == 0; });
}

void VertexVectorIndex::ReleaseResources() {
  std::lock_guard<std::mutex> apply_lock(apply_mutex_);
  std::unique_lock<std::shared_mutex> write(mutex_);
  vector_store_.reset();
}

void VertexVectorIndex::ResetForClear() {
  std::lock_guard<std::mutex> apply_lock(apply_mutex_);
  {
    std::unique_lock<std::shared_mutex> write(mutex_);
    vector_store_.reset();
    vector_store_ = std::make_unique<VectorStore>(
        meta_.path(), meta_.dimensions(), meta_.distance_type(), meta_.hnsw_m(),
        meta_.hnsw_ef_construction());
    next_wal_id_ = 1;
    apply_id_ = 0;
    deleted_.store(false);
  }
  {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    active_callbacks_ = 0;
    started_ = false;
    stopped_ = false;
  }
  meta_.set_applied_wal_id(0);
  meta_.clear_build_error();
}

int64_t VertexVectorIndex::NumElements() {
  std::shared_lock read(mutex_);
  return vector_store_->NumElements();
}

int64_t VertexVectorIndex::MemoryUsage() {
  std::shared_lock read(mutex_);
  return vector_store_->MemoryUsage();
}

int64_t VertexVectorIndex::NumDeletedIds() {
  std::shared_lock read(mutex_);
  return vector_store_->NumDeletedIds();
}

std::vector<std::pair<int64_t, float>> VertexVectorIndex::KnnSearch(
    const float* query, int top_k, int ef_search) {
  std::shared_lock read(mutex_);
  return vector_store_->KnnSearch(query, top_k, ef_search);
}

void VertexVectorIndex::DeleteIfPresent(txn::Transaction* txn, int64_t vid) {
  meta::VectorIndexUpdate wal;
  wal.set_type(meta::UpdateType::Delete);
  wal.set_vid(vid);
  txn->AppendVectorIndexWAL(shared_from_this(), wal);
}

std::string VertexVectorIndex::NextWALKey() {
  std::string ret(AsChars(index_id_), sizeof(index_id_));
  uint64_t wal_id = native_to_big(next_wal_id_++);
  ret.append(AsChars(wal_id), sizeof(wal_id));
  return ret;
}

void VertexVectorIndex::ApplyWAL() {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  std::string prefix(AsChars(index_id_), sizeof(index_id_));
  std::string start_key(prefix);
  uint64_t next = big_to_native(apply_id_) + 1;
  native_to_big_inplace(next);
  start_key.append(AsChars(next), sizeof(next));
  uint64_t consumed_wal_id = 0;
  uint64_t checkpoint_interval =
      std::max<uint64_t>(1, FLAGS_vt_serialize_interval);
  rocksdb::ReadOptions ro;
  std::unique_ptr<rocksdb::Iterator> iter(db_->NewIterator(ro, graph_cf_->wal));
  auto maybe_checkpoint = [&](const rocksdb::Slice& key) {
    if (consumed_wal_id == 0) {
      return;
    }
    uint64_t applied_wal_id = big_to_native(consumed_wal_id);
    uint64_t checkpoint_applied_wal_id = 0;
    {
      std::shared_lock read(mutex_);
      if (vector_store_->has_checkpoint()) {
        checkpoint_applied_wal_id = vector_store_->checkpoint_applied_wal_id();
      }
    }
    if (applied_wal_id - checkpoint_applied_wal_id < checkpoint_interval) {
      return;
    }

    int64_t num_elements = 0;
    LOG_INFO("Vector Index {} begin serialization", meta_.name());
    {
      std::unique_lock write(mutex_);
      try {
        vector_store_->Checkpoint(applied_wal_id);
        num_elements = vector_store_->NumElements();
      } catch (...) {
        apply_id_ = consumed_wal_id;
        meta_.set_applied_wal_id(applied_wal_id);
        throw;
      }
    }
    LOG_INFO("Vector Index {} finish serialization, num:{}, apply_id: {}",
             meta_.name(), num_elements, applied_wal_id);

    rocksdb::WriteOptions wo;
    rocksdb::TransactionDBWriteOptimizations two;
    two.skip_concurrency_control = true;
    two.skip_duplicate_key_check = true;
    rocksdb::WriteBatch batch;
    std::string wal_end = key.ToString();
    wal_end.push_back('\0');
    batch.DeleteRange(graph_cf_->wal, prefix, wal_end);
    auto s = db_->Write(wo, two, &batch);
    if (!s.ok()) {
      apply_id_ = consumed_wal_id;
      meta_.set_applied_wal_id(applied_wal_id);
      THROW_CODE(StorageEngineError,
                 "VertexVectorIndex db DeleteRange error: {}", s.ToString());
    }
  };
  for (iter->Seek(start_key); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key();
    rocksdb::Slice tmp = key;
    tmp.remove_prefix(sizeof(index_id_));
    if (tmp.size() != sizeof(apply_id_)) {
      THROW_CODE(VectorIndexException,
                 "vector index wal key has invalid size, expect {}, actual {}",
                 sizeof(apply_id_), tmp.size());
    }
    consumed_wal_id = ReadValue<uint64_t>(tmp.data());
    meta::VectorIndexUpdate update;
    auto val = iter->value();
    auto ret = update.ParseFromArray(val.data(), val.size());
    if (!ret) {
      THROW_CODE(VectorIndexException,
                 "failed to parse vector index wal payload");
    }
    if (update.type() == meta::UpdateType::Delete) {
      std::unique_lock write(mutex_);
      vector_store_->Delete(update.vid());
    } else if (update.type() == meta::UpdateType::Add) {
      std::unique_ptr<float[]> embedding(new float[update.vector_size()]);
      for (int i = 0; i < update.vector_size(); i++) {
        embedding[i] = update.vector(i);
      }
      {
        std::unique_lock write(mutex_);
        vector_store_->Add(update.vid(), embedding.get());
      }
    } else {
      THROW_CODE(VectorIndexException,
                 "vector index wal has invalid update type: {}",
                 static_cast<int>(update.type()));
    }
    maybe_checkpoint(key);
  }
  ThrowIfIteratorError(iter.get(), "vertex vector index wal iteration failed");
  if (consumed_wal_id != 0) {
    apply_id_ = consumed_wal_id;
    meta_.set_applied_wal_id(big_to_native(consumed_wal_id));
  }
}

void VertexVectorIndex::AddIndex(txn::Transaction* txn, int64_t vid,
                                 meta::VectorIndexUpdate& wal) {
  wal.set_vid(vid);
  txn->AppendVectorIndexWAL(shared_from_this(), wal);
}

void VertexVectorIndex::Load(const rocksdb::Snapshot* snapshot,
                             uint64_t snapshot_wal_id) {
  rocksdb::ReadOptions ro;
  ro.snapshot = snapshot;
  std::unique_ptr<rocksdb::Iterator> iter(
      db_->NewIterator(ro, graph_cf_->vertex_vector_property));
  SPDLOG_INFO("Begin to load vector index: {}", meta_.name());
  int count = 0;
  std::string prefix = VertexVectorPropertyPrefix(lid_, pid_);
  for (iter->Seek(prefix); iter->Valid() && iter->key().starts_with(prefix);
       iter->Next()) {
    auto key = iter->key();
    key.remove_prefix(prefix.size());
    if (key.size() != sizeof(int64_t)) {
      THROW_CODE(StorageEngineError,
                 "vertex vector property key has invalid vid size");
    }
    int64_t vid = ReadValue<int64_t>(key.data());
    auto embedding = DeserializeVector(iter->value(), meta_.dimensions());
    {
      std::unique_lock write(mutex_);
      vector_store_->Add(vid, embedding.data());
    }
    count++;
    if (count % 10000 == 0) {
      SPDLOG_INFO("{} vector indexes have been load", count);
    }
  }
  ThrowIfIteratorError(iter.get(), "vertex vector index load iterator failed");
  SPDLOG_INFO("End to load vector index: {}, index num: {}", meta_.name(),
              count);
  if (count == 0) {
    apply_id_ = native_to_big(snapshot_wal_id);
    meta_.set_applied_wal_id(snapshot_wal_id);
    return;
  }
  LOG_INFO("Vector Index {} begin serialization", meta_.name());
  {
    std::unique_lock write(mutex_);
    vector_store_->Checkpoint(snapshot_wal_id);
    SPDLOG_INFO("Vector Index {} Serialize, num:{}", meta_.name(),
                vector_store_->NumElements());
  }
  apply_id_ = native_to_big(snapshot_wal_id);
  meta_.set_applied_wal_id(snapshot_wal_id);
}

}  // namespace graphdb
