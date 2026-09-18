//
// Created by botu.wzy
//

#include <boost/endian/conversion.hpp>

#include "common/byte_utils.h"
#include "common/exception.h"
#include "common/logger.h"
#include "graph_db.h"
#include "graph_db_internal.h"

using namespace boost::endian;
using common::AsChars;

namespace graphdb {
using namespace internal;

void GraphDB::AddVertexPropertyIndex(
    const std::string& index_name, bool unique, const std::string& label,
    const std::vector<std::string>& properties) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (index_name.empty() || label.empty() || properties.empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexPropertyIndex(index_name)) {
    RG_THROW_CODE(VertexIndexAlreadyExists,
                  "Vertex index name {} already exists", index_name);
  }
  auto lid = id_generator().GetOrCreateLid(label);
  std::vector<uint32_t> pids;
  pids.reserve(properties.size());
  std::unordered_set<uint32_t> pid_set;
  for (const auto& property : properties) {
    if (property.empty()) {
      RG_THROW_CODE(InvalidParameter);
    }
    auto pid = id_generator().GetOrCreatePid(property);
    if (!pid_set.insert(pid).second) {
      RG_THROW_CODE(InvalidParameter, "Duplicate property [{}] in index [{}]",
                    property, index_name);
    }
    pids.push_back(pid);
  }
  CheckNoVectorFieldForNormalIndex(
      meta_info_, {lid}, std::unordered_set<uint32_t>(pids.begin(), pids.end()),
      index_name);
  if (meta_info_.GetVertexPropertyIndex(lid, pids)) {
    RG_THROW_CODE(VertexIndexAlreadyExists,
                  "Vertex index [label:{}, property_count:{}] already exists",
                  big_to_native(lid), pids.size());
  }

  auto index_id = id_generator().GetNextIndexId();
  meta::VertexPropertyIndex meta_val;
  meta_val.set_name(index_name);
  meta_val.set_is_unique(unique);
  meta_val.set_label(label);
  meta_val.set_label_id(big_to_native(lid));
  meta_val.set_index_id(big_to_native(index_id));
  meta_val.set_build_start_wal_id(0);
  meta_val.set_applied_wal_id(0);
  meta_val.clear_build_error();
  for (size_t i = 0; i < properties.size(); ++i) {
    meta_val.add_properties(properties[i]);
    meta_val.add_property_ids(big_to_native(pids[i]));
  }

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::CREATE_VERTEX_PROPERTY_INDEX,
        meta_val.SerializeAsString());
    return;
  }
  ApplyCreateVertexPropertyIndex(0, std::move(meta_val));
}

void GraphDB::ApplyCreateVertexPropertyIndex(
    uint64_t apply_index, meta::VertexPropertyIndex meta_val) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta_val.name().empty() || meta_val.label().empty() ||
      meta_val.properties().empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexPropertyIndex(meta_val.name())) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(VertexIndexAlreadyExists,
                  "Vertex index name {} already exists", meta_val.name());
  }
  if (meta_val.property_ids_size() != meta_val.properties_size()) {
    RG_THROW_CODE(
        InvalidParameter,
        "vertex property index [{}] property id count {} does not match "
        "property count {}",
        meta_val.name(), meta_val.property_ids_size(),
        meta_val.properties_size());
  }
  auto lid = native_to_big(meta_val.label_id());
  std::vector<uint32_t> pids;
  pids.reserve(meta_val.property_ids_size());
  for (auto pid : meta_val.property_ids()) {
    pids.push_back(native_to_big(pid));
  }
  CheckNoVectorFieldForNormalIndex(
      meta_info_, {lid}, std::unordered_set<uint32_t>(pids.begin(), pids.end()),
      meta_val.name());
  if (meta_info_.GetVertexPropertyIndex(lid, pids)) {
    RG_THROW_CODE(VertexIndexAlreadyExists,
                  "Vertex index [label:{}, property_count:{}] already exists",
                  meta_val.label_id(), pids.size());
  }
  auto index_id = native_to_big(meta_val.index_id());
  id_generator().ReserveIndexId(meta_val.index_id());

  auto vpi = std::make_shared<VertexPropertyIndex>(db_, &graph_cf_, meta_val,
                                                   graph_cf_.index, index_id,
                                                   lid, std::move(pids));

  vpi->SetState(meta::IndexBuildState::BUILDING);
  rocksdb::WriteBatch wb;
  auto s = wb.Put(
      graph_cf_.meta_info,
      BuildMetaKey(MetadataType::VertexPropertyIndex, vpi->meta().name()),
      vpi->meta().SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  auto ret = meta_info_.AddVertexPropertyIndex(vpi);
  assert(ret);
  LOG_INFO(
      "Begin online build vertex index: [lid:{}, property_count:{}, "
      "is_unique:{}]",
      meta_val.label_id(), meta_val.properties_size(), meta_val.is_unique());
  ScheduleVertexPropertyIndexBuild(vpi, false);
}

void GraphDB::DeleteVertexPropertyIndex(const std::string& index_name) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  meta::VertexPropertyIndex meta;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    auto index = meta_info_.GetVertexPropertyIndex(index_name);
    if (!index) {
      RG_THROW_CODE(VertexUniqueIndexNotFound, "No such vertex index [{}]",
                    index_name);
    }
    meta = index->meta();
  }
  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::DELETE_VERTEX_PROPERTY_INDEX,
        meta.SerializeAsString());
    return;
  }
  ApplyDeleteVertexPropertyIndex(0, meta);
}

void GraphDB::ApplyDeleteVertexPropertyIndex(
    uint64_t apply_index, const meta::VertexPropertyIndex& meta_val) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
  const auto& index_name = meta_val.name();
  auto index = meta_info_.GetVertexPropertyIndex(index_name);
  if (!index) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(VertexUniqueIndexNotFound, "No such vertex index [{}]",
                  index_name);
  }
  index->MarkDeleted();
  uint32_t index_id = index->index_id();
  meta_info_.DeleteVertexPropertyIndex(index_name);

  rocksdb::WriteBatch wb;
  wb.Delete(graph_cf_.meta_info,
            BuildMetaKey(MetadataType::VertexPropertyIndex, index_name));
  std::string start_key(AsChars(index_id), sizeof(index_id));
  std::string end_key(AsChars(index_id), sizeof(index_id));
  end_key.append(128, static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.index, start_key, end_key);
  std::string wal_start(AsChars(index_id), sizeof(index_id));
  std::string wal_end(AsChars(index_id), sizeof(index_id));
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.wal, wal_start, wal_end);
  if (apply_index > 0) {
    auto s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db_->Write({}, two, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  LOG_INFO("Delete vertex index: {}", index_name);
}

void GraphDB::AddEdgePropertyIndex(const std::string& index_name, bool unique,
                                   const std::string& edge_type,
                                   const std::vector<std::string>& properties) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (index_name.empty() || edge_type.empty() || properties.empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetEdgePropertyIndex(index_name)) {
    RG_THROW_CODE(EdgePropertyIndexAlreadyExists,
                  "Edge property index name {} already exists", index_name);
  }
  auto tid = id_generator().GetOrCreateTid(edge_type);
  std::vector<uint32_t> pids;
  pids.reserve(properties.size());
  std::unordered_set<uint32_t> pid_set;
  for (const auto& property : properties) {
    if (property.empty()) RG_THROW_CODE(InvalidParameter);
    auto pid = id_generator().GetOrCreatePid(property);
    if (!pid_set.insert(pid).second) {
      RG_THROW_CODE(InvalidParameter, "Duplicate property [{}] in index [{}]",
                    property, index_name);
    }
    pids.push_back(pid);
  }
  if (meta_info_.GetEdgePropertyIndex(tid, pids)) {
    RG_THROW_CODE(
        EdgePropertyIndexAlreadyExists,
        "Edge property index [type:{}, property_count:{}] already exists",
        big_to_native(tid), pids.size());
  }

  auto index_id = id_generator().GetNextIndexId();
  meta::EdgePropertyIndex meta_val;
  meta_val.set_name(index_name);
  meta_val.set_is_unique(unique);
  meta_val.set_edge_type(edge_type);
  meta_val.set_edge_type_id(big_to_native(tid));
  meta_val.set_index_id(big_to_native(index_id));
  meta_val.set_build_start_wal_id(0);
  meta_val.set_applied_wal_id(0);
  meta_val.clear_build_error();
  for (size_t i = 0; i < properties.size(); ++i) {
    meta_val.add_properties(properties[i]);
    meta_val.add_property_ids(big_to_native(pids[i]));
  }

  if (raft_driver() != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::CREATE_EDGE_PROPERTY_INDEX,
                         meta_val.SerializeAsString());
    return;
  }
  ApplyCreateEdgePropertyIndex(0, std::move(meta_val));
}

void GraphDB::ApplyCreateEdgePropertyIndex(uint64_t apply_index,
                                           meta::EdgePropertyIndex meta_val) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta_val.name().empty() || meta_val.edge_type().empty() ||
      meta_val.properties().empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetEdgePropertyIndex(meta_val.name())) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(EdgePropertyIndexAlreadyExists,
                  "Edge property index name {} already exists",
                  meta_val.name());
  }
  if (meta_val.property_ids_size() != meta_val.properties_size()) {
    RG_THROW_CODE(
        InvalidParameter,
        "edge property index [{}] property id count {} does not match "
        "property count {}",
        meta_val.name(), meta_val.property_ids_size(),
        meta_val.properties_size());
  }
  auto tid = native_to_big(meta_val.edge_type_id());
  std::vector<uint32_t> pids;
  pids.reserve(meta_val.property_ids_size());
  for (auto pid : meta_val.property_ids()) pids.push_back(native_to_big(pid));
  if (meta_info_.GetEdgePropertyIndex(tid, pids)) {
    RG_THROW_CODE(
        EdgePropertyIndexAlreadyExists,
        "Edge property index [type:{}, property_count:{}] already exists",
        meta_val.edge_type_id(), pids.size());
  }
  auto index_id = native_to_big(meta_val.index_id());
  id_generator().ReserveIndexId(meta_val.index_id());
  auto epi = std::make_shared<EdgePropertyIndex>(db_, &graph_cf_, meta_val,
                                                 graph_cf_.index, index_id, tid,
                                                 std::move(pids));
  epi->SetState(meta::IndexBuildState::BUILDING);

  rocksdb::WriteBatch wb;
  auto s =
      wb.Put(graph_cf_.meta_info,
             BuildMetaKey(MetadataType::EdgePropertyIndex, epi->meta().name()),
             epi->meta().SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  auto added = meta_info_.AddEdgePropertyIndex(epi);
  assert(added);
  LOG_INFO(
      "Begin online build edge property index: [tid:{}, property_count:{}, "
      "is_unique:{}]",
      meta_val.edge_type_id(), meta_val.properties_size(),
      meta_val.is_unique());
  ScheduleEdgePropertyIndexBuild(epi, false);
}

void GraphDB::DeleteEdgePropertyIndex(const std::string& index_name) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  meta::EdgePropertyIndex meta_val;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    auto index = meta_info_.GetEdgePropertyIndex(index_name);
    if (!index) {
      RG_THROW_CODE(EdgePropertyIndexNotFound,
                    "No such edge property index [{}]", index_name);
    }
    meta_val = index->meta();
  }
  if (raft_driver() != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::DELETE_EDGE_PROPERTY_INDEX,
                         meta_val.SerializeAsString());
    return;
  }
  ApplyDeleteEdgePropertyIndex(0, meta_val);
}

void GraphDB::ApplyDeleteEdgePropertyIndex(
    uint64_t apply_index, const meta::EdgePropertyIndex& meta_val) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
  const auto& index_name = meta_val.name();
  auto index = meta_info_.GetEdgePropertyIndex(index_name);
  if (!index) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(EdgePropertyIndexNotFound, "No such edge property index [{}]",
                  index_name);
  }
  index->MarkDeleted();
  uint32_t index_id = index->index_id();
  meta_info_.DeleteEdgePropertyIndex(index_name);

  rocksdb::WriteBatch wb;
  wb.Delete(graph_cf_.meta_info,
            BuildMetaKey(MetadataType::EdgePropertyIndex, index_name));
  std::string start_key(AsChars(index_id), sizeof(index_id));
  std::string end_key(AsChars(index_id), sizeof(index_id));
  end_key.append(128, static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.index, start_key, end_key);
  std::string wal_start(AsChars(index_id), sizeof(index_id));
  std::string wal_end(AsChars(index_id), sizeof(index_id));
  wal_end.append(sizeof(uint64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.wal, wal_start, wal_end);
  if (apply_index > 0) {
    auto s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }
  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db_->Write({}, two, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  LOG_INFO("Delete edge property index: {}", index_name);
}

}  // namespace graphdb
