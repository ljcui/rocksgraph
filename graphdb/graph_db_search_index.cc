//
// Created by botu.wzy
//

#include <filesystem>

#include "common/exception.h"
#include "common/logger.h"
#include "graph_db.h"
#include "graph_db_internal.h"
#include "graphdb/id_codec.h"

namespace graphdb {
using namespace internal;

void GraphDB::AddVertexFullTextIndex(
    const std::string& index_name, const std::vector<std::string>& labels,
    const std::vector<std::string>& properties) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (index_name.empty() || labels.empty() || properties.empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexFullTextIndex(index_name)) {
    RG_THROW_CODE(VertexFullTextIndexAlreadyExists,
                  "Vertex fulltext index [{}] already exists", index_name);
  }
  std::unordered_set<uint32_t> lids;
  std::unordered_set<uint32_t> pids;
  for (const auto& label : labels) {
    auto lid = id_generator().GetOrCreateLid(label);
    lids.insert(lid);
  }
  for (const auto& prop : properties) {
    auto pid = id_generator().GetOrCreatePid(prop);
    pids.insert(pid);
  }
  CheckNoVectorFieldForNormalIndex(meta_info_, lids, pids, index_name);
  uint32_t index_id = id_generator().GetNextIndexId();
  meta::VertexFullTextIndex meta;
  meta.set_index_id(index_id);
  meta.set_name(index_name);
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();
  *meta.mutable_labels() = {labels.begin(), labels.end()};
  *meta.mutable_properties() = {properties.begin(), properties.end()};
  *meta.mutable_label_ids() = {lids.begin(), lids.end()};
  *meta.mutable_property_ids() = {pids.begin(), pids.end()};

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::CREATE_VERTEX_FULLTEXT_INDEX,
        meta.SerializeAsString());
    return;
  }
  ApplyCreateVertexFullTextIndex(0, std::move(meta));
}

void GraphDB::ApplyCreateVertexFullTextIndex(uint64_t apply_index,
                                             meta::VertexFullTextIndex meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta.name().empty() || meta.labels().empty() ||
      meta.properties().empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexFullTextIndex(meta.name())) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(VertexFullTextIndexAlreadyExists,
                  "Vertex fulltext index [{}] already exists", meta.name());
  }
  if (meta.label_ids_size() != meta.labels_size() ||
      meta.property_ids_size() != meta.properties_size()) {
    RG_THROW_CODE(InvalidParameter,
                  "vertex fulltext index [{}] id counts do not match label or "
                  "property counts",
                  meta.name());
  }
  uint32_t index_id = meta.index_id();
  id_generator().ReserveIndexId(meta.index_id());
  std::unordered_set<uint32_t> lids, pids;
  for (auto id : meta.label_ids()) {
    lids.insert(id);
  }
  for (auto id : meta.property_ids()) {
    pids.insert(id);
  }
  CheckNoVectorFieldForNormalIndex(meta_info_, lids, pids, meta.name());
  meta.set_path(BuildFullTextIndexPath(path_, meta.name(), index_id));
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();
  DeleteFullTextIndexRanges(db_, &graph_cf_, index_id);
  ResetFullTextIndexPath(meta.path());

  auto v_ft_index = std::make_shared<VertexFullTextIndex>(
      db_, assistant_pool_->Service(), assistant_strand_.get(), &graph_cf_,
      &id_generator(), meta, index_id, options_.ft_writer_threads_,
      options_.ft_writer_memory_budget_, lids, pids,
      options_.ft_apply_interval_);
  rocksdb::WriteBatch wb;
  auto s = wb.Put(graph_cf_.meta_info,
                  BuildMetaKey(MetadataType::VertexFullTextIndex, meta.name()),
                  meta.SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  meta_info_.AddVertexFullTextIndex(v_ft_index);
  LOG_INFO("Begin online build vertex full text index: [lids:{}, pids:{}]",
           lids, pids);
  ScheduleVertexFullTextIndexBuild(v_ft_index, false);
}

void GraphDB::DeleteVertexFullTextIndex(const std::string& index_name) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  meta::VertexFullTextIndex meta;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    auto ft_index = meta_info_.GetVertexFullTextIndex(index_name);
    if (!ft_index) {
      RG_THROW_CODE(FullTextIndexNotFound, "No such vertex fulltext index [{}]",
                    index_name);
    }
    meta = ft_index->meta();
  }
  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(
        meta::GraphIndexDdlRequest::DELETE_VERTEX_FULLTEXT_INDEX,
        meta.SerializeAsString());
    return;
  }
  ApplyDeleteVertexFullTextIndex(0, meta);
}

void GraphDB::ApplyDeleteVertexFullTextIndex(
    uint64_t apply_index, const meta::VertexFullTextIndex& meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::lock_guard<std::mutex> commit_lock(fulltext_index_commit_mutex_);
  const auto& index_name = meta.name();
  auto ft_index = meta_info_.GetVertexFullTextIndex(index_name);
  if (!ft_index) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(FullTextIndexNotFound, "No such vertex fulltext index [{}]",
                  index_name);
  }
  std::string path = ft_index->meta().path();
  uint32_t index_id = ft_index->index_id();
  ft_index->MarkDeleted();
  ft_index->Stop();
  ft_index->ReleaseResources();
  meta_info_.DeleteVertexFullTextIndex(index_name);

  rocksdb::WriteBatch wb;
  wb.Delete(graph_cf_.meta_info,
            BuildMetaKey(MetadataType::VertexFullTextIndex, index_name));

  std::string start_key = EncodeBigEndianId(index_id);
  start_key.append(sizeof(int64_t), static_cast<char>(0x00));
  std::string end_key = EncodeBigEndianId(index_id);
  end_key.append(sizeof(int64_t), static_cast<char>(0xFF));
  wb.DeleteRange(graph_cf_.index, start_key, end_key);
  wb.DeleteRange(graph_cf_.wal, start_key, end_key);
  if (apply_index > 0) {
    auto s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }

  rocksdb::TransactionDBWriteOptimizations two;
  two.skip_concurrency_control = true;
  two.skip_duplicate_key_check = true;
  auto s = db_->Write({}, two, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  try {
    std::filesystem::remove_all(path);
    LOG_INFO("remove fulltext index data {}", path);
  } catch (const std::exception& e) {
    LOG_ERROR("[DeleteVertexFullTextIndex] remove_all {} meet error: {}", path,
              e.what());
  }
}

void GraphDB::AddVertexVectorIndex(const std::string& index_name,
                                   const std::string& label,
                                   const std::string& property, int dimension,
                                   std::string distance_type, int hnsw_m,
                                   int hnsw_ef_construction) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (index_name.empty() || label.empty() || property.empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (dimension < 1 || dimension > 4096) {
    RG_THROW_CODE(InvalidParameter,
                  "dimension should be an integer in the range [1, 4096]");
  }
  if (hnsw_m < 5 || hnsw_m > 64) {
    RG_THROW_CODE(InvalidParameter,
                  "hnsw.m should be an integer in the range [5, 64]");
  }
  if (hnsw_ef_construction < hnsw_m || hnsw_ef_construction > 1000) {
    RG_THROW_CODE(
        InvalidParameter,
        "hnsw.efConstruction should be an integer in the range [hnsw.m,1000]");
  }
  meta::VectorDistanceType dist_type;
  if (distance_type == "l2") {
    dist_type = meta::VectorDistanceType::L2;
  } else if (distance_type == "ip") {
    dist_type = meta::VectorDistanceType::IP;
  } else {
    RG_THROW_CODE(InvalidParameter, "Distance Type {} not supported",
                  distance_type);
  }
  if (meta_info_.GetVertexVectorIndex(index_name)) {
    RG_THROW_CODE(VertexVectorIndexAlreadyExists,
                  "Vertex vector index [{}] already exists", index_name);
  }
  auto lid_opt = id_generator().GetLid(label);
  auto pid_opt = id_generator().GetPid(property);
  if (!lid_opt || !pid_opt) {
    RG_THROW_CODE(InvalidParameter,
                  "Vector field [label:{}, property:{}] is not defined", label,
                  property);
  }
  auto lid = lid_opt.value();
  auto pid = pid_opt.value();
  auto vector_field = meta_info_.GetVertexVectorField(lid, pid);
  if (!vector_field) {
    RG_THROW_CODE(InvalidParameter,
                  "Vector field [label:{}, property:{}] is not defined", label,
                  property);
  }
  if (vector_field->dimensions() != static_cast<uint32_t>(dimension)) {
    RG_THROW_CODE(InvalidParameter,
                  "Vector field [label:{}, property:{}] dimension mismatch, "
                  "expect {}, actual {}",
                  label, property, vector_field->dimensions(), dimension);
  }
  if (meta_info_.GetVertexVectorIndex(lid, pid)) {
    RG_THROW_CODE(VertexVectorIndexAlreadyExists,
                  "Vertex vector index [label:{}, property:{}] already exists",
                  lid, pid);
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, label, property);
  uint32_t index_id = id_generator().GetNextIndexId();
  meta::VectorIndexType index_type = meta::VectorIndexType::HNSW;
  meta::VertexVectorIndex meta;
  meta.set_index_id(index_id);
  meta.set_name(index_name);
  meta.set_label(label);
  meta.set_property(property);
  meta.set_label_id(lid);
  meta.set_property_id(pid);
  meta.set_dimensions(dimension);
  meta.set_index_type(index_type);
  meta.set_distance_type(dist_type);
  meta.set_hnsw_m(hnsw_m);
  meta.set_hnsw_ef_construction(hnsw_ef_construction);
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_INDEX,
                         meta.SerializeAsString());
    return;
  }
  ApplyCreateVertexVectorIndex(0, std::move(meta));
}

void GraphDB::AddVertexVectorField(const std::string& label,
                                   const std::string& property, int dimension) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  if (label.empty() || property.empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (dimension < 1 || dimension > 4096) {
    RG_THROW_CODE(InvalidParameter,
                  "dimension should be an integer in the range [1, 4096]");
  }

  auto lid = id_generator().GetOrCreateLid(label);
  auto pid = id_generator().GetOrCreatePid(property);
  if (meta_info_.GetVertexVectorField(lid, pid)) {
    RG_THROW_CODE(InvalidParameter,
                  "Vector field [label:{}, property:{}] already exists", label,
                  property);
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, label, property);

  meta::VertexVectorField meta;
  meta.set_label(label);
  meta.set_label_id(lid);
  meta.set_property(property);
  meta.set_property_id(pid);
  meta.set_dimensions(dimension);

  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_FIELD,
                         meta.SerializeAsString());
    return;
  }
  ApplyCreateVertexVectorField(0, std::move(meta));
}

void GraphDB::ApplyCreateVertexVectorField(uint64_t apply_index,
                                           meta::VertexVectorField meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta.label().empty() || meta.property().empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta.dimensions() < 1 || meta.dimensions() > 4096) {
    RG_THROW_CODE(InvalidParameter,
                  "dimension should be an integer in the range [1, 4096]");
  }

  auto lid = meta.label_id();
  auto pid = meta.property_id();
  auto existing_field = meta_info_.GetVertexVectorField(lid, pid);
  if (existing_field) {
    if (apply_index > 0 && existing_field->dimensions() == meta.dimensions()) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(InvalidParameter,
                  "Vector field [label:{}, property:{}] already exists",
                  meta.label(), meta.property());
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, meta.label(),
                                   meta.property());

  auto vector_field =
      std::make_shared<meta::VertexVectorField>(std::move(meta));
  rocksdb::WriteBatch wb;
  auto s =
      wb.Put(graph_cf_.meta_info,
             BuildMetaKey(MetadataType::VertexVectorField,
                          BuildVectorFieldMetaKey(vector_field->label(),
                                                  vector_field->property())),
             vector_field->SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  meta_info_.AddVertexVectorField(vector_field);
}

void GraphDB::ApplyCreateVertexVectorIndex(uint64_t apply_index,
                                           meta::VertexVectorIndex meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  if (meta.name().empty() || meta.label().empty() || meta.property().empty()) {
    RG_THROW_CODE(InvalidParameter);
  }
  if (meta_info_.GetVertexVectorIndex(meta.name())) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(VertexVectorIndexAlreadyExists,
                  "Vertex vector index [{}] already exists", meta.name());
  }
  auto lid = meta.label_id();
  auto pid = meta.property_id();
  if (meta_info_.GetVertexVectorIndex(lid, pid)) {
    RG_THROW_CODE(VertexVectorIndexAlreadyExists,
                  "Vertex vector index [label:{}, property:{}] already exists",
                  meta.label_id(), meta.property_id());
  }
  CheckNoNormalIndexForVectorField(meta_info_, lid, pid, meta.label(),
                                   meta.property());
  auto existing_field = meta_info_.GetVertexVectorField(lid, pid);
  if (existing_field) {
    if (existing_field->dimensions() != meta.dimensions()) {
      RG_THROW_CODE(InvalidParameter,
                    "Vector field [label:{}, property:{}] dimension mismatch, "
                    "expect {}, actual {}",
                    meta.label(), meta.property(), existing_field->dimensions(),
                    meta.dimensions());
    }
  } else {
    RG_THROW_CODE(InvalidParameter,
                  "Vector field [label:{}, property:{}] is not defined",
                  meta.label(), meta.property());
  }
  uint32_t index_id = meta.index_id();
  id_generator().ReserveIndexId(meta.index_id());
  meta.set_path(path_ + "/vt/" + meta.name());
  meta.set_state(meta::IndexBuildState::BUILDING);
  meta.set_build_start_wal_id(0);
  meta.set_applied_wal_id(0);
  meta.clear_build_error();
  DeleteVectorIndexRanges(db_, &graph_cf_, index_id);
  ResetIndexPath(meta.path(), "vector index");

  auto vvi = std::make_shared<VertexVectorIndex>(
      db_, assistant_pool_->Service(), assistant_strand_.get(), &graph_cf_,
      index_id, lid, pid, meta, options_.vt_apply_interval_);
  rocksdb::WriteBatch wb;
  auto s = wb.Put(graph_cf_.meta_info,
                  BuildMetaKey(MetadataType::VertexVectorIndex, meta.name()),
                  meta.SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  if (apply_index > 0) {
    s = SetRaftApplyIndex(apply_index, &wb);
    if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  }
  s = db_->Write({}, {}, &wb);
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
  meta_info_.AddVertexVectorIndex(vvi);
  LOG_INFO("Begin online build vertex vector index: [lid:{}, pid:{}]",
           meta.label_id(), meta.property_id());
  ScheduleVertexVectorIndexBuild(vvi, false);
}

void GraphDB::DeleteVertexVectorIndex(const std::string& index_name) {
  std::lock_guard<std::mutex> propose_lock(index_ddl_propose_mutex_);
  meta::VertexVectorIndex meta;
  {
    std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
    auto index = meta_info_.GetVertexVectorIndex(index_name);
    if (!index) {
      RG_THROW_CODE(VectorIndexNotFound, "No such vertex vector index [{}]",
                    index_name);
    }
    meta = index->meta();
  }
  auto* driver = raft_driver();
  if (driver != nullptr) {
    ProposeGraphIndexDdl(meta::GraphIndexDdlRequest::DELETE_VERTEX_VECTOR_INDEX,
                         meta.SerializeAsString());
    return;
  }
  ApplyDeleteVertexVectorIndex(0, meta);
}

void GraphDB::ApplyDeleteVertexVectorIndex(
    uint64_t apply_index, const meta::VertexVectorIndex& meta) {
  std::lock_guard<std::mutex> clear_lock(clear_data_mutex_);
  std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
  std::lock_guard<std::mutex> commit_lock(vector_index_commit_mutex_);
  const auto& index_name = meta.name();
  auto index = meta_info_.GetVertexVectorIndex(index_name);
  if (!index) {
    if (apply_index > 0) {
      rocksdb::WriteBatch wb;
      auto s = SetRaftApplyIndex(apply_index, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      s = db_->Write({}, {}, &wb);
      if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
      return;
    }
    RG_THROW_CODE(VectorIndexNotFound, "No such vertex vector index [{}]",
                  index_name);
  }
  auto path = index->meta().path();
  uint32_t index_id = index->index_id();
  index->MarkDeleted();
  index->Stop();
  index->ReleaseResources();
  meta_info_.DeleteVertexVectorIndex(index_name);
  rocksdb::WriteBatch wb;
  wb.Delete(graph_cf_.meta_info,
            BuildMetaKey(MetadataType::VertexVectorIndex, index_name));

  std::string wal_start = EncodeBigEndianId(index_id);
  std::string wal_end = wal_start;
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

  try {
    std::filesystem::remove_all(path);
    LOG_INFO("remove vector index data {}", path);
  } catch (const std::exception& e) {
    LOG_ERROR("[DeleteVertexVectorIndex] remove_all {} meet error: {}", path,
              e.what());
  }
}

}  // namespace graphdb
