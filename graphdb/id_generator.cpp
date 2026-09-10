//
// Created by botu.wzy
//

#include "id_generator.h"

#include <algorithm>
#include <boost/endian/conversion.hpp>

#include "common/byte_utils.h"
#include "common/exceptions.h"
#include "common/logger.h"
#include "raft_driver/raft_driver.h"
using namespace boost::endian;
using common::AsChars;
using common::ReadValue;
namespace graphdb {
namespace {

std::string TokenKey(MetaDataType type, const std::string &name) {
  std::string key;
  key.append(1, static_cast<char>(type));
  key.append(name);
  return key;
}

std::string EntityIdKey(MetaDataType type) {
  return std::string(1, static_cast<char>(type));
}

template <typename T>
void StoreMax(std::atomic<T> *target, T value) {
  T current = target->load();
  while (current < value && !target->compare_exchange_weak(current, value)) {
  }
}

}  // namespace

void IdGenerator::Bind(rocksdb::TransactionDB *db, GraphCF *graph_cf) {
  db_ = db;
  graph_cf_ = graph_cf;
}

void IdGenerator::SetRaftDriver(raft::RaftDriver *raft_driver) {
  raft_driver_ = raft_driver;
}

void IdGenerator::LoadToken(MetaDataType type, const std::string &name,
                            uint32_t id) {
  if (type == MetaDataType::VertexLabel) {
    vertex_labels_name_to_id_[name] = id;
    vertex_labels_id_to_name_[id] = name;
  } else if (type == MetaDataType::EdgeType) {
    edge_types_name_to_id_[name] = id;
    edge_types_id_to_name_[id] = name;
  } else if (type == MetaDataType::Property) {
    properties_name_to_id_[name] = id;
    properties_id_to_name_[id] = name;
  } else {
    THROW_CODE(InvalidParameter, "unsupported token metadata type {}",
               static_cast<int>(type));
  }
}

void IdGenerator::ApplyMetaRecord(MetaDataType type,
                                  const rocksdb::Slice &key_suffix,
                                  const rocksdb::Slice &value) {
  switch (type) {
    case MetaDataType::VertexLabel: {
      if (value.size() != sizeof(uint32_t)) {
        THROW_CODE(StorageEngineError,
                   "vertex label metadata has invalid size, expect {}, actual "
                   "{}",
                   sizeof(uint32_t), value.size());
      }
      uint32_t id = ReadValue<uint32_t>(value.data());
      uint32_t native_id = big_to_native(id);
      std::string name(key_suffix.data(), key_suffix.size());
      std::unique_lock write_lock(vertex_labels_mutex_);
      vertex_labels_name_to_id_[name] = id;
      vertex_labels_id_to_name_[id] = name;
      StoreMax(&label_next_lid_, native_id + 1);
      return;
    }
    case MetaDataType::EdgeType: {
      if (value.size() != sizeof(uint32_t)) {
        THROW_CODE(StorageEngineError,
                   "edge type metadata has invalid size, expect {}, actual {}",
                   sizeof(uint32_t), value.size());
      }
      uint32_t id = ReadValue<uint32_t>(value.data());
      uint32_t native_id = big_to_native(id);
      std::string name(key_suffix.data(), key_suffix.size());
      std::unique_lock write_lock(edge_types_mutex_);
      edge_types_name_to_id_[name] = id;
      edge_types_id_to_name_[id] = name;
      StoreMax(&label_next_tid_, native_id + 1);
      return;
    }
    case MetaDataType::Property: {
      if (value.size() != sizeof(uint32_t)) {
        THROW_CODE(StorageEngineError,
                   "property metadata has invalid size, expect {}, actual {}",
                   sizeof(uint32_t), value.size());
      }
      uint32_t id = ReadValue<uint32_t>(value.data());
      uint32_t native_id = big_to_native(id);
      std::string name(key_suffix.data(), key_suffix.size());
      std::unique_lock write_lock(properties_mutex_);
      properties_name_to_id_[name] = id;
      properties_id_to_name_[id] = name;
      StoreMax(&label_next_pid_, native_id + 1);
      return;
    }
    case MetaDataType::NextVertexId: {
      if (value.size() != sizeof(int64_t)) {
        THROW_CODE(StorageEngineError,
                   "next vertex id metadata has invalid size, expect {}, "
                   "actual {}",
                   sizeof(int64_t), value.size());
      }
      int64_t next_vid = big_to_native(ReadValue<int64_t>(value.data()));
      if (next_vid < 1) {
        THROW_CODE(StorageEngineError,
                   "next vertex id metadata must be positive");
      }
      StoreMax(&persisted_next_vid_, next_vid);
      StoreMax(&next_vid_, next_vid);
      StoreMax(&vid_range_end_, next_vid);
      return;
    }
    case MetaDataType::NextEdgeId: {
      if (value.size() != sizeof(int64_t)) {
        THROW_CODE(StorageEngineError,
                   "next edge id metadata has invalid size, expect {}, actual "
                   "{}",
                   sizeof(int64_t), value.size());
      }
      int64_t next_eid = big_to_native(ReadValue<int64_t>(value.data()));
      if (next_eid < 1) {
        THROW_CODE(StorageEngineError,
                   "next edge id metadata must be positive");
      }
      StoreMax(&persisted_next_eid_, next_eid);
      StoreMax(&next_eid_, next_eid);
      StoreMax(&eid_range_end_, next_eid);
      return;
    }
    default:
      return;
  }
}

void IdGenerator::SetMaxIds(uint32_t max_lid, uint32_t max_pid,
                            uint32_t max_tid, uint32_t max_index_id) {
  LOG_INFO("max_lid:{}, max_pid:{}, max_tid:{}, max_index_id:{}", max_lid,
           max_pid, max_tid, max_index_id);
  label_next_lid_ = max_lid + 1;
  label_next_pid_ = max_pid + 1;
  label_next_tid_ = max_tid + 1;
  index_next_id_ = max_index_id + 1;
}

void IdGenerator::SetNextEntityIds(int64_t next_vid, int64_t next_eid) {
  if (next_vid < 1 || next_eid < 1) {
    THROW_CODE(InvalidParameter, "entity id range must start from positive id");
  }
  persisted_next_vid_ = next_vid;
  next_vid_ = next_vid;
  vid_range_end_ = next_vid;
  persisted_next_eid_ = next_eid;
  next_eid_ = next_eid;
  eid_range_end_ = next_eid;
}

int64_t IdGenerator::GetNextEntityId(std::atomic<int64_t> *next_id,
                                     std::atomic<int64_t> *range_end,
                                     std::atomic<int64_t> *persisted_next_id,
                                     std::mutex *refill_mutex,
                                     MetaDataType meta_type) {
  for (;;) {
    int64_t candidate = next_id->load();
    int64_t limit = range_end->load();
    while (candidate < limit) {
      if (next_id->compare_exchange_weak(candidate, candidate + 1)) {
        return native_to_big(candidate);
      }
    }

    std::unique_lock refill_lock(*refill_mutex);
    candidate = next_id->load();
    limit = range_end->load();
    if (candidate < limit) {
      continue;
    }

    int64_t start = persisted_next_id->load();
    if (start > std::numeric_limits<int64_t>::max() - kIdRangeSize) {
      THROW_CODE(StorageEngineError, "entity id range exhausted");
    }
    int64_t end = start + kIdRangeSize;
    PersistEntityId(meta_type, end);
    persisted_next_id->store(end);
    next_id->store(start + 1);
    range_end->store(end);
    return native_to_big(start);
  }
}

void IdGenerator::ProposeAndApply(rocksdb::WriteBatch *wb) {
  if (raft_driver_ == nullptr) {
    rocksdb::TransactionDBWriteOptimizations two;
    auto s = db_->Write({}, two, wb);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    return;
  }

  auto apply_result =
      raft_driver_->ProposeWriteBatch(meta::WriteBatchKind::ID_GENERATOR, *wb);
  if (apply_result.err != nullptr) {
    THROW_CODE(StorageEngineError, apply_result.err.String());
  }
}

void IdGenerator::PersistEntityId(MetaDataType meta_type, int64_t next_id) {
  int64_t bigendian_next_id = native_to_big(next_id);
  rocksdb::WriteBatch wb;
  auto s = wb.Put(
      graph_cf_->meta_info, EntityIdKey(meta_type),
      std::string(AsChars(bigendian_next_id), sizeof(bigendian_next_id)));
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  ProposeAndApply(&wb);
}

void IdGenerator::PersistToken(MetaDataType type, const std::string &name,
                               uint32_t id) {
  std::string key = TokenKey(type, name);
  std::string val;
  val.append(AsChars(id), sizeof(id));
  rocksdb::WriteBatch wb;
  auto s = wb.Put(graph_cf_->meta_info, key, val);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  ProposeAndApply(&wb);
}

int64_t IdGenerator::GetNextVid() {
  return GetNextEntityId(&next_vid_, &vid_range_end_, &persisted_next_vid_,
                         &vid_refill_mutex_, MetaDataType::NextVertexId);
}

int64_t IdGenerator::GetNextEid() {
  return GetNextEntityId(&next_eid_, &eid_range_end_, &persisted_next_eid_,
                         &eid_refill_mutex_, MetaDataType::NextEdgeId);
}

uint32_t IdGenerator::GetNextIndexId() {
  return native_to_big(index_next_id_++);
}

void IdGenerator::ReserveIndexId(uint32_t native_index_id) {
  StoreMax(&index_next_id_, native_index_id + 1);
}

std::optional<uint32_t> IdGenerator::GetLid(const std::string &name) {
  if (name.empty()) {
    THROW_CODE(InvalidParameter, "label name is empty");
  }
  std::shared_lock read_lock(vertex_labels_mutex_);
  auto iter = vertex_labels_name_to_id_.find(name);
  if (iter != vertex_labels_name_to_id_.end()) {
    return iter->second;
  } else {
    return {};
  }
}

std::optional<uint32_t> IdGenerator::GetPid(const std::string &name) {
  if (name.empty()) {
    THROW_CODE(InvalidParameter, "property name is empty");
  }
  std::shared_lock read_lock(properties_mutex_);
  auto iter = properties_name_to_id_.find(name);
  if (iter != properties_name_to_id_.end()) {
    return iter->second;
  } else {
    return {};
  }
}

std::optional<uint32_t> IdGenerator::GetTid(const std::string &name) {
  if (name.empty()) {
    THROW_CODE(InvalidParameter, "edge type is empty");
  }
  std::shared_lock read_lock(edge_types_mutex_);
  auto iter = edge_types_name_to_id_.find(name);
  if (iter != edge_types_name_to_id_.end()) {
    return iter->second;
  } else {
    return {};
  }
}

std::optional<std::string> IdGenerator::GetPropertyName(uint32_t pid) {
  std::shared_lock read_lock(properties_mutex_);
  auto iter = properties_id_to_name_.find(pid);
  if (iter != properties_id_to_name_.end()) {
    return iter->second;
  } else {
    return {};
  }
}

std::optional<std::string> IdGenerator::GetVertexLabelName(uint32_t lid) {
  std::shared_lock read_lock(vertex_labels_mutex_);
  auto iter = vertex_labels_id_to_name_.find(lid);
  if (iter != vertex_labels_id_to_name_.end()) {
    return iter->second;
  } else {
    return {};
  }
}

std::optional<std::string> IdGenerator::GetEdgeTypeName(uint32_t tid) {
  std::shared_lock read_lock(edge_types_mutex_);
  auto iter = edge_types_id_to_name_.find(tid);
  if (iter != edge_types_id_to_name_.end()) {
    return iter->second;
  } else {
    return {};
  }
}

uint32_t IdGenerator::GetOrCreateLid(const std::string &name) {
  if (name.empty()) {
    THROW_CODE(InvalidParameter, "label name is empty");
  }
  {
    std::shared_lock read_lock(vertex_labels_mutex_);
    auto iter = vertex_labels_name_to_id_.find(name);
    if (iter != vertex_labels_name_to_id_.end()) {
      return iter->second;
    }
  }
  std::lock_guard create_lock(vertex_label_create_mutex_);
  {
    std::shared_lock read_lock(vertex_labels_mutex_);
    auto iter = vertex_labels_name_to_id_.find(name);
    if (iter != vertex_labels_name_to_id_.end()) {
      return iter->second;
    }
  }
  uint32_t bigendian_lid = native_to_big(label_next_lid_++);
  PersistToken(MetaDataType::VertexLabel, name, bigendian_lid);
  {
    std::unique_lock write_lock(vertex_labels_mutex_);
    vertex_labels_name_to_id_[name] = bigendian_lid;
    vertex_labels_id_to_name_[bigendian_lid] = name;
  }
  return bigendian_lid;
}

uint32_t IdGenerator::GetOrCreateTid(const std::string &name) {
  if (name.empty()) {
    THROW_CODE(InvalidParameter, "edge type is empty");
  }
  {
    std::shared_lock read_lock(edge_types_mutex_);
    auto iter = edge_types_name_to_id_.find(name);
    if (iter != edge_types_name_to_id_.end()) {
      return iter->second;
    }
  }
  std::lock_guard create_lock(edge_type_create_mutex_);
  {
    std::shared_lock read_lock(edge_types_mutex_);
    auto iter = edge_types_name_to_id_.find(name);
    if (iter != edge_types_name_to_id_.end()) {
      return iter->second;
    }
  }
  uint32_t bigendian_tid = native_to_big(label_next_tid_++);
  PersistToken(MetaDataType::EdgeType, name, bigendian_tid);
  {
    std::unique_lock write_lock(edge_types_mutex_);
    edge_types_name_to_id_[name] = bigendian_tid;
    edge_types_id_to_name_[bigendian_tid] = name;
  }
  return bigendian_tid;
}

uint32_t IdGenerator::GetOrCreatePid(const std::string &name) {
  if (name.empty()) {
    THROW_CODE(InvalidParameter, "property name is empty");
  }
  {
    std::shared_lock read_lock(properties_mutex_);
    auto iter = properties_name_to_id_.find(name);
    if (iter != properties_name_to_id_.end()) {
      return iter->second;
    }
  }
  std::lock_guard create_lock(property_create_mutex_);
  {
    std::shared_lock read_lock(properties_mutex_);
    auto iter = properties_name_to_id_.find(name);
    if (iter != properties_name_to_id_.end()) {
      return iter->second;
    }
  }
  uint32_t bigendian_pid = native_to_big(label_next_pid_++);
  PersistToken(MetaDataType::Property, name, bigendian_pid);
  {
    std::unique_lock write_lock(properties_mutex_);
    properties_name_to_id_[name] = bigendian_pid;
    properties_id_to_name_[bigendian_pid] = name;
  }
  return bigendian_pid;
}

std::unordered_set<std::string> IdGenerator::GetProperties() {
  std::unordered_set<std::string> ret;
  std::shared_lock read_lock(properties_mutex_);
  for (const auto &[key, val] : properties_name_to_id_) {
    ret.insert(key);
  }
  return ret;
}

std::unordered_set<std::string> IdGenerator::GetVertexLabels() {
  std::unordered_set<std::string> ret;
  std::shared_lock read_lock(vertex_labels_mutex_);
  for (const auto &[key, val] : vertex_labels_name_to_id_) {
    ret.insert(key);
  }
  return ret;
}

std::unordered_set<std::string> IdGenerator::GetEdgeTypes() {
  std::unordered_set<std::string> ret;
  std::shared_lock read_lock(edge_types_mutex_);
  for (const auto &[key, val] : edge_types_name_to_id_) {
    ret.insert(key);
  }
  return ret;
}
}  // namespace graphdb
