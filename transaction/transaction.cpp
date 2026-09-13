//
// Created by botu.wzy
//

#include "transaction/transaction.h"

#include <rocksdb/utilities/write_batch_with_index.h>

#include <algorithm>
#include <boost/endian/conversion.hpp>
#include <cstring>

#include "common/byte_utils.h"
#include "common/exceptions.h"
#include "common/logger.h"
#include "graphdb/edge_index_updater.h"
#include "graphdb/graph_db.h"
#include "graphdb/index_error.h"
#include "graphdb/value_codec.h"
#include "graphdb/vector_property.h"
#include "graphdb/vertex_index_updater.h"
using namespace graphdb;
using namespace boost::endian;
using common::AsChars;

namespace {

bool IsRangeComparableValue(const rg::Value& value) {
  return !value.IsList() && !value.IsMap();
}

std::shared_ptr<VertexPropertyIndex> ResolveVertexPropertyIndexOrThrow(
    txn::Transaction* txn, const std::string& index_name) {
  auto index = txn->db()->meta_info().GetReadyVertexPropertyIndex(index_name);
  if (!index) {
    if (auto building_index =
            txn->db()->meta_info().GetVertexPropertyIndex(index_name)) {
      ThrowIfIndexUnavailable(building_index, index_name, "Vertex");
    }
    THROW_CODE(VertexUniqueIndexNotFound, "No such vertex index [{}]",
               index_name);
  }
  return index;
}

std::shared_ptr<EdgePropertyIndex> ResolveEdgePropertyIndexOrThrow(
    txn::Transaction* txn, const std::string& index_name) {
  auto index = txn->db()->meta_info().GetReadyEdgePropertyIndex(index_name);
  if (!index) {
    if (auto building_index =
            txn->db()->meta_info().GetEdgePropertyIndex(index_name)) {
      ThrowIfIndexUnavailable(building_index, index_name, "Edge");
    }
    THROW_CODE(EdgePropertyIndexNotFound, "No such edge property index [{}]",
               index_name);
  }
  return index;
}

template <typename Index>
std::vector<rg::Value> BuildPropertyIndexQueryValues(
    const std::shared_ptr<Index>& index, const rg::Value& query,
    const std::string& arg_name) {
  std::vector<rg::Value> values;
  if (index->PropertyCount() == 1) {
    values.emplace_back(query);
    return values;
  }

  if (!query.IsList()) {
    THROW_CODE(ReminderException,
               "{} type should be List for composite index {}", arg_name,
               index->meta().name());
  }
  const auto& items = query.AsList();
  if (items.size() != index->PropertyCount()) {
    THROW_CODE(ReminderException,
               "{} element count should be {}, but {} are given", arg_name,
               index->PropertyCount(), items.size());
  }
  return {items.begin(), items.end()};
}

template <typename Index>
std::optional<std::string> BuildPropertyIndexRangeKey(
    const std::shared_ptr<Index>& index, const std::optional<rg::Value>& bound,
    const std::string& arg_name) {
  if (!bound.has_value()) {
    return std::nullopt;
  }
  auto values = BuildPropertyIndexQueryValues(index, *bound, arg_name);
  for (const auto& value : values) {
    if (!IsRangeComparableValue(value)) {
      THROW_CODE(ReminderException, "{} does not support LIST or MAP component",
                 arg_name);
    }
  }
  return index->IndexKey(values);
}

bool IsEmptyPropertyIndexRange(const std::optional<std::string>& lower_key,
                               const std::optional<std::string>& upper_key,
                               bool left_closed, bool right_closed) {
  if (!lower_key.has_value() || !upper_key.has_value()) {
    return false;
  }
  size_t common_size = std::min(lower_key->size(), upper_key->size());
  int cmp = std::memcmp(lower_key->data(), upper_key->data(), common_size);
  if (cmp == 0) {
    if (lower_key->size() < upper_key->size()) {
      cmp = -1;
    } else if (lower_key->size() > upper_key->size()) {
      cmp = 1;
    }
  }
  return cmp > 0 || (cmp == 0 && (!left_closed || !right_closed));
}

}  // namespace

namespace txn {
Vertex Transaction::CreateVertex(
    const std::unordered_set<std::string>& labels,
    const std::unordered_map<std::string, rg::Value>& values) {
  rocksdb::Status s;
  int64_t vid = db_->id_generator().GetNextVid();
  std::unordered_set<uint32_t> lids;
  std::string buffer;
  for (const auto& label : labels) {
    auto lid = db_->id_generator().GetOrCreateLid(label);
    lids.emplace(lid);
    buffer.clear();
    buffer.append(AsChars(lid), sizeof(lid));
    buffer.append(AsChars(vid), sizeof(vid));
    s = txn_->GetWriteBatch()->Put(db_->graph_cf().vertex_label_vid, buffer,
                                   {});
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  buffer.clear();
  for (auto lid : lids) {
    buffer.append(AsChars(lid), sizeof(lid));
  }
  s = txn_->GetWriteBatch()->Put(db_->graph_cf().graph_topology,
                                 rocksdb::Slice(AsChars(vid), sizeof(vid)),
                                 buffer);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  VertexSerializedProperties serialized_values;
  VertexVectorProperties vector_values;
  std::unordered_set<uint32_t> pids;
  for (const auto& [name, value] : values) {
    uint32_t pid = db_->id_generator().GetOrCreatePid(name);
    pids.insert(pid);
    bool is_vector_property = false;
    for (auto lid : lids) {
      auto field = db_->meta_info().GetVertexVectorField(lid, pid);
      if (!field) {
        continue;
      }
      is_vector_property = true;
      vector_values[pid] = ParseVectorValue(value, field->dimensions());
      std::string key = VertexVectorPropertyKey(lid, pid, vid);
      auto val = SerializeVector(vector_values.at(pid));
      s = txn_->GetWriteBatch()->Put(db_->graph_cf().vertex_vector_property,
                                     key, val);
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    }
    if (!is_vector_property) {
      serialized_values.emplace(pid, SerializeValue(value));
    }
  }
  for (const auto& [pid, val] : serialized_values) {
    buffer.clear();
    buffer.append(AsChars(vid), sizeof(vid));
    buffer.append(AsChars(pid), sizeof(pid));
    s = txn_->GetWriteBatch()->Put(db_->graph_cf().vertex_property, buffer,
                                   val);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  std::unordered_set<uint32_t> empty_lids;
  VertexSerializedProperties empty_properties;
  VertexVectorProperties empty_vector_properties;
  UpdateVertexIndexes(this, vid, empty_lids, lids, empty_properties,
                      serialized_values, empty_vector_properties, vector_values,
                      pids);
  return {this, vid};
}

Edge Transaction::CreateEdge(
    const Vertex& start, const Vertex& end, const std::string& type,
    const std::unordered_map<std::string, rg::Value>& values) {
  rocksdb::Status s;
  {
    rocksdb::ReadOptions ro;
    s = txn_->GetForUpdate(ro, db_->graph_cf().graph_topology,
                           start.GetIdView(), (std::string*)nullptr);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    s = txn_->GetForUpdate(ro, db_->graph_cf().graph_topology, end.GetIdView(),
                           (std::string*)nullptr);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }

  int64_t eid = db_->id_generator().GetNextEid();
  uint32_t tid = db_->id_generator().GetOrCreateTid(type);
  std::string key, val;
  // out key
  key.append(start.GetIdView());
  key.append(1, 0);
  key.append(AsChars(tid), sizeof(tid));
  key.append(end.GetIdView());
  key.append(AsChars(eid), sizeof(eid));
  s = txn_->GetWriteBatch()->Put(db_->graph_cf().graph_topology, key, {});
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  // in key
  key.clear();
  key.append(end.GetIdView());
  key.append(1, 1);
  key.append(AsChars(tid), sizeof(tid));
  key.append(start.GetIdView());
  key.append(AsChars(eid), sizeof(eid));
  s = txn_->GetWriteBatch()->Put(db_->graph_cf().graph_topology, key, {});
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  // type
  key.clear();
  key.append(AsChars(tid), sizeof(tid));
  key.append(AsChars(eid), sizeof(eid));
  val.append(start.GetIdView());
  val.append(end.GetIdView());
  s = txn_->GetWriteBatch()->Put(db_->graph_cf().edge_type_eid, key, val);
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  // properties
  EdgeSerializedProperties serialized_properties;
  for (const auto& [name, value] : values) {
    uint32_t pid = db_->id_generator().GetOrCreatePid(name);
    key.clear();
    key.append(AsChars(eid), sizeof(eid));
    key.append(AsChars(pid), sizeof(pid));
    val = SerializeValue(value);
    serialized_properties.emplace(pid, val);
    s = txn_->GetWriteBatch()->Put(db_->graph_cf().edge_property, key, val);
    if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  }
  UpdateEdgeIndexes(this, eid, tid, {}, serialized_properties);
  return {this, eid, start.GetId(), end.GetId(), tid};
}

Vertex Transaction::GetVertexById(int64_t vid) {
  rocksdb::ReadOptions ro;
  std::string val;
  auto s = txn_->Get(ro, db_->graph_cf().graph_topology,
                     rocksdb::Slice(AsChars(vid), sizeof(vid)), &val);
  if (s.ok()) {
    return {this, vid};
  } else if (s.IsNotFound()) {
    THROW_CODE(VertexIdNotFound, "Vertex id {} not found", big_to_native(vid));
  } else {
    THROW_CODE(StorageEngineError, s.ToString());
  }
}

Edge Transaction::GetEdgeById(uint32_t etid, int64_t eid) {
  std::string key;
  key.append(AsChars(etid), sizeof(etid));
  key.append(AsChars(eid), sizeof(eid));
  rocksdb::ReadOptions ro;
  std::string val;
  auto s = txn_->Get(ro, db_->graph_cf().edge_type_eid, key, &val);
  if (s.ok()) {
    auto p = val.data();
    int64_t startId = common::ReadValue<int64_t>(p);
    p += sizeof(int64_t);
    int64_t endId = common::ReadValue<int64_t>(p);
    return {this, eid, startId, endId, etid};
  } else if (s.IsNotFound()) {
    THROW_CODE(EdgeIdNotFound, "Edge [etid:{},eid:{}] not found",
               big_to_native(etid), big_to_native(eid));
  } else {
    THROW_CODE(StorageEngineError, s.ToString());
  }
}

std::unique_ptr<VertexIterator> Transaction::NewVertexIterator() {
  return std::make_unique<ScanAllVertex>(this);
}

std::unique_ptr<VertexIterator> Transaction::NewVertexIterator(
    const std::string& label) {
  auto lid = db_->id_generator().GetLid(label);
  if (lid.has_value()) {
    return std::make_unique<ScanVertexBylabel>(this, lid.value());
  } else {
    return std::make_unique<NoVertexFound>(this);
  }
}

std::unique_ptr<EdgeIterator> Transaction::NewEdgeIterator() {
  return std::make_unique<ScanEdgeByTypes>(this,
                                           std::unordered_set<uint32_t>{});
}

std::unique_ptr<EdgeIterator> Transaction::NewEdgeIterator(
    const std::unordered_set<std::string>& types) {
  std::unordered_set<uint32_t> type_ids;
  for (const auto& type : types) {
    if (auto type_id = db_->id_generator().GetTid(type); type_id.has_value()) {
      type_ids.insert(*type_id);
    }
  }
  if (!types.empty() && type_ids.empty()) {
    return std::make_unique<NoEdgeFound>(this);
  }
  return std::make_unique<ScanEdgeByTypes>(this, std::move(type_ids));
}

std::string Transaction::GetVertexIteratorInfo(
    const std::optional<std::string>& label,
    const std::optional<std::unordered_set<std::string>>& props) {
  if (!label && !props) {
    return "ScanAllVertex";
  } else if (label && !props) {
    auto lid = db_->id_generator().GetLid(label.value());
    if (!lid.has_value()) {
      return "NoVertexFound";
    } else {
      return "ScanVertexBylabel";
    }
  } else if (!label && props) {
    std::unordered_map<uint32_t, rg::Value> map;
    for (auto& name : props.value()) {
      auto pid = db_->id_generator().GetPid(name);
      if (!pid.has_value()) {
        return "NoVertexFound";
      }
    }
    return "ScanVertexByProperties";
  } else {
    auto lid = db_->id_generator().GetLid(label.value());
    if (!lid.has_value()) {
      return "NoVertexFound";
    }
    std::unordered_set<uint32_t> pids;
    for (auto& name : props.value()) {
      auto pid = db_->id_generator().GetPid(name);
      if (!pid.has_value()) {
        return "NoVertexFound";
      }
      pids.insert(pid.value());
    }
    if (db_->meta_info().GetBestVertexPropertyUniqueIndex(lid.value(), pids)) {
      return "GetVertexByUniqueIndex";
    }
    return "ScanVertexBylabelProperties";
  }
}

std::unique_ptr<VertexIterator> Transaction::NewVertexIterator(
    const std::optional<std::string>& label,
    const std::optional<std::unordered_map<std::string, rg::Value>>& props) {
  if (!label && !props) {
    return std::make_unique<ScanAllVertex>(this);
  } else if (label && !props) {
    auto lid = db_->id_generator().GetLid(label.value());
    if (!lid.has_value()) {
      return std::make_unique<NoVertexFound>(this);
    } else {
      return std::make_unique<ScanVertexBylabel>(this, lid.value());
    }
  } else if (!label && props) {
    std::unordered_map<uint32_t, rg::Value> map;
    for (auto& [name, val] : props.value()) {
      auto pid = db_->id_generator().GetPid(name);
      if (!pid.has_value()) {
        return std::make_unique<NoVertexFound>(this);
      } else {
        map.emplace(pid.value(), val);
      }
    }
    return std::make_unique<ScanVertexByProperties>(this, std::move(map));
  } else {
    auto lid = db_->id_generator().GetLid(label.value());
    if (!lid.has_value()) {
      return std::make_unique<NoVertexFound>(this);
    }
    std::unordered_map<uint32_t, rg::Value> map;
    for (auto& [name, val] : props.value()) {
      auto pid = db_->id_generator().GetPid(name);
      if (!pid.has_value()) {
        return std::make_unique<NoVertexFound>(this);
      }
      map.emplace(pid.value(), val);
    }
    std::unordered_set<uint32_t> pids;
    for (const auto& [pid, _] : map) {
      pids.insert(pid);
    }
    auto unique_index =
        db_->meta_info().GetBestVertexPropertyUniqueIndex(lid.value(), pids);
    if (!unique_index) {
      return std::make_unique<ScanVertexBylabelProperties>(this, lid.value(),
                                                           std::move(map));
    }
    std::vector<rg::Value> indexed_values;
    indexed_values.reserve(unique_index->PropertyCount());
    for (auto pid : unique_index->pids()) {
      indexed_values.push_back(map.at(pid));
      map.erase(pid);
    }
    return std::make_unique<GetVertexByUniqueIndex>(
        this, std::move(unique_index), std::move(indexed_values),
        std::move(map));
  }
}

void Transaction::AppendFullTextIndexWAL(
    std::shared_ptr<graphdb::VertexFullTextIndex> index,
    const meta::FullTextIndexUpdate& update) {
  pending_fulltext_wals_.push_back({std::move(index), update});
}

void Transaction::AppendPropertyIndexWAL(
    std::shared_ptr<graphdb::VertexPropertyIndex> index,
    const meta::PropertyIndexUpdate& update) {
  pending_property_wals_.push_back({std::move(index), update});
}

void Transaction::AppendEdgePropertyIndexWAL(
    std::shared_ptr<graphdb::EdgePropertyIndex> index,
    const meta::PropertyIndexUpdate& update) {
  pending_edge_property_wals_.push_back({std::move(index), update});
}

void Transaction::Commit() {
  if (state_ != State::kActive) {
    THROW_CODE(InvalidParameter, "transaction is not active");
  }
  {
    std::unique_lock<std::mutex> property_commit_lock(
        db_->property_index_commit_mutex(), std::defer_lock);
    std::unique_lock<std::mutex> fulltext_commit_lock(
        db_->fulltext_index_commit_mutex(), std::defer_lock);
    std::unique_lock<std::mutex> vector_commit_lock(
        db_->vector_index_commit_mutex(), std::defer_lock);
    bool has_property_wals =
        !pending_property_wals_.empty() || !pending_edge_property_wals_.empty();
    bool has_fulltext_wals = !pending_fulltext_wals_.empty();
    bool has_vector_wals = !pending_vector_wals_.empty();
    if (has_property_wals && has_fulltext_wals && has_vector_wals) {
      std::lock(property_commit_lock, fulltext_commit_lock, vector_commit_lock);
    } else if (has_property_wals && has_fulltext_wals) {
      std::lock(property_commit_lock, fulltext_commit_lock);
    } else if (has_property_wals && has_vector_wals) {
      std::lock(property_commit_lock, vector_commit_lock);
    } else if (has_fulltext_wals && has_vector_wals) {
      std::lock(fulltext_commit_lock, vector_commit_lock);
    } else if (has_property_wals) {
      property_commit_lock.lock();
    } else if (has_fulltext_wals) {
      fulltext_commit_lock.lock();
    } else if (has_vector_wals) {
      vector_commit_lock.lock();
    }
    if (has_property_wals || has_fulltext_wals || has_vector_wals) {
      auto* write_batch = txn_->GetWriteBatch();
      for (const auto& wal : pending_property_wals_) {
        if (wal.index->IsDeleted()) {
          THROW_CODE(VertexUniqueIndexNotFound,
                     "Vertex index [{}] was deleted during transaction",
                     wal.index->Name());
        }
        if (wal.index->IsReady()) {
          wal.index->ApplyCommittedBuildUpdate(this, wal.update);
          continue;
        }
        auto s = write_batch->Put(db_->graph_cf().wal, wal.index->NextWALKey(),
                                  wal.update.SerializeAsString());
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      for (const auto& wal : pending_edge_property_wals_) {
        if (wal.index->IsDeleted()) {
          THROW_CODE(EdgePropertyIndexNotFound,
                     "Edge property index [{}] was deleted during transaction",
                     wal.index->Name());
        }
        if (wal.index->IsReady()) {
          wal.index->ApplyCommittedBuildUpdate(this, wal.update);
          continue;
        }
        auto s = write_batch->Put(db_->graph_cf().wal, wal.index->NextWALKey(),
                                  wal.update.SerializeAsString());
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      for (const auto& wal : pending_fulltext_wals_) {
        if (wal.index->IsDeleted()) {
          THROW_CODE(FullTextIndexNotFound,
                     "Fulltext index [{}] was deleted during transaction",
                     wal.index->Name());
        }
        auto s = write_batch->Put(db_->graph_cf().wal, wal.index->NextWALKey(),
                                  wal.update.SerializeAsString());
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
      for (const auto& wal : pending_vector_wals_) {
        if (wal.index->IsDeleted()) {
          THROW_CODE(VectorIndexNotFound,
                     "Vector index [{}] was deleted during transaction",
                     wal.index->meta().name());
        }
        auto s = write_batch->Put(db_->graph_cf().wal, wal.index->NextWALKey(),
                                  wal.update.SerializeAsString());
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      }
    }
    auto* raft_driver = db_->raft_driver();
    if (raft_driver == nullptr) {
      auto s = txn_->Commit();
      if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
    } else {
      auto* write_batch_with_index = txn_->GetWriteBatch();
      auto* write_batch = write_batch_with_index->GetWriteBatch();
      if (write_batch->Count() == 0) {
        auto s = txn_->Commit();
        if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
      } else {
        auto apply_result = raft_driver->ProposeWriteBatch(
            meta::WriteBatchKind::GRAPH_WRITE, *write_batch);
        if (apply_result.err != nullptr) {
          auto rollback_status = txn_->Rollback();
          if (!rollback_status.ok()) {
            LOG_WARN(
                "transaction rollback after raft commit failure failed: {}",
                rollback_status.ToString());
          }
          state_ = State::kRolledBack;
          THROW_CODE(StorageEngineError, apply_result.err.String());
        }
        write_batch_with_index->Clear();
        auto s = txn_->Commit();
        if (!s.ok()) {
          LOG_FATAL("raft commit succeeded but transaction cleanup failed: {}",
                    s.ToString());
        }
      }
    }
    pending_property_wals_.clear();
    pending_edge_property_wals_.clear();
    pending_fulltext_wals_.clear();
    pending_vector_wals_.clear();
  }
  state_ = State::kCommitted;
}

void Transaction::Rollback() {
  if (state_ != State::kActive) {
    THROW_CODE(InvalidParameter, "transaction is not active");
  }
  auto s = txn_->Rollback();
  if (!s.ok()) THROW_CODE(StorageEngineError, s.ToString());
  pending_property_wals_.clear();
  pending_edge_property_wals_.clear();
  pending_fulltext_wals_.clear();
  pending_vector_wals_.clear();
  state_ = State::kRolledBack;
}

std::unique_ptr<VertexScoreIterator> Transaction::QueryVertexByFTIndex(
    const std::string& index_name, const std::string& query, size_t top_n) {
  return std::make_unique<GetVertexByFullTextIndex>(this, index_name, query,
                                                    top_n);
}

std::unique_ptr<graphdb::VertexIterator>
Transaction::QueryVertexByPropertyIndex(const std::string& index_name,
                                        const rg::Value& query) {
  auto index = ResolveVertexPropertyIndexOrThrow(this, index_name);
  auto values = BuildPropertyIndexQueryValues(index, query, "query");
  auto key = index->IndexKey(values);
  return std::make_unique<GetVertexByPropertyIndex>(this, std::move(index),
                                                    std::move(key));
}

std::unique_ptr<graphdb::VertexIterator>
Transaction::QueryVertexByPropertyIndex(const std::vector<std::string>& labels,
                                        const std::string& property_key,
                                        const rg::Value& query) {
  if (labels.size() != 1) {
    THROW_CODE(InvalidParameter,
               "GraphDB vertex property indexes require exactly one label");
  }
  auto lid = db_->id_generator().GetLid(labels.front());
  auto pid = db_->id_generator().GetPid(property_key);
  if (!lid.has_value() || !pid.has_value()) {
    return std::make_unique<NoVertexFound>(this);
  }
  auto index = db_->meta_info().GetReadyVertexPropertyIndex(*lid, *pid);
  if (!index) {
    if (auto unavailable =
            db_->meta_info().GetVertexPropertyIndex(*lid, *pid)) {
      ThrowIfIndexUnavailable(unavailable, property_key, "Vertex");
    }
    THROW_CODE(
        VertexUniqueIndexNotFound,
        "No ready vertex property index for label [{}] and property [{}]",
        labels.front(), property_key);
  }
  return QueryVertexByPropertyIndex(index->Name(), query);
}

std::unique_ptr<graphdb::EdgeIterator> Transaction::QueryEdgeByPropertyIndex(
    const std::string& index_name, const rg::Value& query) {
  auto index = ResolveEdgePropertyIndexOrThrow(this, index_name);
  auto values = BuildPropertyIndexQueryValues(index, query, "query");
  auto key = index->IndexKey(values);
  return std::make_unique<GetEdgeByPropertyIndex>(this, std::move(index),
                                                  std::move(key));
}

std::unique_ptr<graphdb::EdgeIterator> Transaction::QueryEdgeByPropertyIndex(
    const std::vector<std::string>& types, const std::string& property_key,
    const rg::Value& query) {
  if (types.size() != 1) {
    THROW_CODE(InvalidParameter,
               "GraphDB edge property indexes require exactly one type");
  }
  auto tid = db_->id_generator().GetTid(types.front());
  auto pid = db_->id_generator().GetPid(property_key);
  if (!tid.has_value() || !pid.has_value()) {
    return std::make_unique<NoEdgeFound>(this);
  }
  auto index = db_->meta_info().GetReadyEdgePropertyIndex(*tid, *pid);
  if (!index) {
    if (auto unavailable = db_->meta_info().GetEdgePropertyIndex(*tid, *pid)) {
      ThrowIfIndexUnavailable(unavailable, property_key, "Edge");
    }
    THROW_CODE(EdgePropertyIndexNotFound,
               "No ready edge property index for type [{}] and property [{}]",
               types.front(), property_key);
  }
  return QueryEdgeByPropertyIndex(index->Name(), query);
}

std::unique_ptr<graphdb::EdgeIterator> Transaction::QueryEdgeByPropertyRange(
    const std::string& index_name, const std::optional<rg::Value>& lower,
    const std::optional<rg::Value>& upper, bool left_closed,
    bool right_closed) {
  auto index = ResolveEdgePropertyIndexOrThrow(this, index_name);
  auto lower_key = BuildPropertyIndexRangeKey(index, lower, "lower");
  auto upper_key = BuildPropertyIndexRangeKey(index, upper, "upper");
  if (IsEmptyPropertyIndexRange(lower_key, upper_key, left_closed,
                                right_closed)) {
    return std::make_unique<NoEdgeFound>(this);
  }
  return std::make_unique<GetEdgeByPropertyRange>(
      this, std::move(index), std::move(lower_key), std::move(upper_key),
      left_closed, right_closed);
}

std::unique_ptr<graphdb::EdgeIterator> Transaction::QueryEdgeByPropertyRange(
    const std::vector<std::string>& types, const std::string& property_key,
    const std::optional<rg::Value>& lower,
    const std::optional<rg::Value>& upper, bool left_closed,
    bool right_closed) {
  if (types.size() != 1) {
    THROW_CODE(InvalidParameter,
               "GraphDB edge property indexes require exactly one type");
  }
  auto tid = db_->id_generator().GetTid(types.front());
  auto pid = db_->id_generator().GetPid(property_key);
  if (!tid.has_value() || !pid.has_value()) {
    return std::make_unique<NoEdgeFound>(this);
  }
  auto index = db_->meta_info().GetReadyEdgePropertyIndex(*tid, *pid);
  if (!index) {
    if (auto unavailable = db_->meta_info().GetEdgePropertyIndex(*tid, *pid)) {
      ThrowIfIndexUnavailable(unavailable, property_key, "Edge");
    }
    THROW_CODE(EdgePropertyIndexNotFound,
               "No ready edge property index for type [{}] and property [{}]",
               types.front(), property_key);
  }
  return QueryEdgeByPropertyRange(index->Name(), lower, upper, left_closed,
                                  right_closed);
}

std::unique_ptr<graphdb::VertexIterator>
Transaction::QueryVertexByPropertyRange(const std::string& index_name,
                                        const std::optional<rg::Value>& lower,
                                        const std::optional<rg::Value>& upper,
                                        bool left_closed, bool right_closed) {
  auto index = ResolveVertexPropertyIndexOrThrow(this, index_name);
  auto lower_key = BuildPropertyIndexRangeKey(index, lower, "lower");
  auto upper_key = BuildPropertyIndexRangeKey(index, upper, "upper");
  if (IsEmptyPropertyIndexRange(lower_key, upper_key, left_closed,
                                right_closed)) {
    return std::make_unique<NoVertexFound>(this);
  }
  return std::make_unique<GetVertexByPropertyRange>(
      this, std::move(index), std::move(lower_key), std::move(upper_key),
      left_closed, right_closed);
}

std::unique_ptr<graphdb::VertexIterator>
Transaction::QueryVertexByPropertyRange(const std::vector<std::string>& labels,
                                        const std::string& property_key,
                                        const std::optional<rg::Value>& lower,
                                        const std::optional<rg::Value>& upper,
                                        bool left_closed, bool right_closed) {
  if (labels.size() != 1) {
    THROW_CODE(InvalidParameter,
               "GraphDB vertex property indexes require exactly one label");
  }
  auto lid = db_->id_generator().GetLid(labels.front());
  auto pid = db_->id_generator().GetPid(property_key);
  if (!lid.has_value() || !pid.has_value()) {
    return std::make_unique<NoVertexFound>(this);
  }
  auto index = db_->meta_info().GetReadyVertexPropertyIndex(*lid, *pid);
  if (!index) {
    if (auto unavailable =
            db_->meta_info().GetVertexPropertyIndex(*lid, *pid)) {
      ThrowIfIndexUnavailable(unavailable, property_key, "Vertex");
    }
    THROW_CODE(
        VertexUniqueIndexNotFound,
        "No ready vertex property index for label [{}] and property [{}]",
        labels.front(), property_key);
  }
  return QueryVertexByPropertyRange(index->Name(), lower, upper, left_closed,
                                    right_closed);
}

std::unique_ptr<graphdb::VertexScoreIterator>
Transaction::QueryVertexByKnnSearch(const std::string& index_name,
                                    const std::vector<float>& query, int top_k,
                                    int ef_search) {
  return std::make_unique<GetVertexByKnnSearch>(this, index_name, query, top_k,
                                                ef_search);
}

}  // namespace txn
