//
// Created by botu.wzy
//

#include <utility>

#include "common/byte_utils.h"
#include "common/exception.h"
#include "graph_db.h"

using common::AsChars;
using common::ReadValue;

namespace graphdb {
namespace {

const std::string kRaftApplyIndexKey(
    1, static_cast<char>(MetadataType::RaftApplyIndex));

class IdGeneratorMetaBatchHandler : public rocksdb::WriteBatch::Handler {
 public:
  explicit IdGeneratorMetaBatchHandler(GraphDB* graph_db)
      : id_generator_(graph_db->id_generator()),
        meta_info_cf_id_(graph_db->graph_cf().meta_info->GetID()) {}

  rocksdb::Status PutCF(uint32_t column_family_id, const rocksdb::Slice& key,
                        const rocksdb::Slice& value) override {
    if (column_family_id != meta_info_cf_id_ || key.empty()) {
      return rocksdb::Status::OK();
    }
    auto type = static_cast<MetadataType>(key.data()[0]);
    rocksdb::Slice key_suffix(key.data() + 1, key.size() - 1);
    id_generator_.ApplyMetaRecord(type, key_suffix, value);
    return rocksdb::Status::OK();
  }

  rocksdb::Status DeleteCF(uint32_t, const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status SingleDeleteCF(uint32_t, const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status DeleteRangeCF(uint32_t, const rocksdb::Slice&,
                                const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

  rocksdb::Status MergeCF(uint32_t, const rocksdb::Slice&,
                          const rocksdb::Slice&) override {
    return rocksdb::Status::OK();
  }

 private:
  IdGenerator& id_generator_;
  uint32_t meta_info_cf_id_;
};

}  // namespace

raft::RaftDriver* GraphDB::raft_driver() const {
  std::shared_lock<std::shared_mutex> lock(raft_mutex_);
  return raft_driver_.get();
}

void GraphDB::SetRaftDriver(std::unique_ptr<raft::RaftDriver> raft_driver) {
  std::unique_lock<std::shared_mutex> lock(raft_mutex_);
  raft_driver_ = std::move(raft_driver);
  meta_info_.id_generator().SetRaftDriver(raft_driver_.get());
}

void GraphDB::StopRaft() {
  std::unique_lock<std::shared_mutex> lock(raft_mutex_);
  if (raft_driver_) {
    raft_driver_->Stop();
    raft_driver_.reset();
  }
  meta_info_.id_generator().SetRaftDriver(nullptr);
}

uint64_t GraphDB::GetRaftApplyIndex() const {
  std::string val;
  auto s = db_->Get({}, graph_cf_.meta_info, kRaftApplyIndexKey, &val);
  if (s.IsNotFound()) {
    return 0;
  }
  if (!s.ok()) {
    RG_THROW_CODE(StorageEngineError, "failed to load raft apply index: {}",
                  s.ToString());
  }
  if (val.size() != sizeof(uint64_t)) {
    RG_THROW_CODE(StorageEngineError,
                  "raft apply index has invalid size, expect {}, actual {}",
                  sizeof(uint64_t), val.size());
  }
  return ReadValue<uint64_t>(val.data());
}

void GraphDB::ApplyRaftRequest(uint64_t index,
                               const meta::RaftRequest& request) {
  switch (request.wb_kind()) {
    case meta::WriteBatchKind::GRAPH_WRITE:
    case meta::WriteBatchKind::ID_GENERATOR:
      ApplyRaftWriteBatch(index, request);
      return;
    case meta::WriteBatchKind::GRAPH_INDEX_DDL: {
      meta::GraphIndexDdlRequest ddl_request;
      if (!ddl_request.ParseFromString(request.wb_data())) {
        RG_THROW_CODE(
            InvalidParameter,
            "failed to parse graph index ddl request for graph [{}] at "
            "index {}",
            db_meta_.graph_name(), index);
      }
      ApplyGraphIndexDdlRequest(index, ddl_request);
      return;
    }
    case meta::WriteBatchKind::UNKNOWN:
      RG_THROW_CODE(
          InvalidParameter,
          "write batch kind must be specified for graph [{}] at index "
          "{}",
          db_meta_.graph_name(), index);
    default:
      RG_THROW_CODE(
          InvalidParameter,
          "unsupported write batch kind {} for graph [{}] at index {}",
          static_cast<int>(request.wb_kind()), db_meta_.graph_name(), index);
  }
}

void GraphDB::ApplyRaftWriteBatch(uint64_t index,
                                  const meta::RaftRequest& request) {
  rocksdb::WriteBatch wb(request.wb_data());

  auto s = SetRaftApplyIndex(index, &wb);
  if (!s.ok()) {
    RG_THROW_CODE(
        StorageEngineError,
        "failed to persist raft apply index for graph [{}] at index {}: "
        "{}",
        db_meta_.graph_name(), index, s.ToString());
  }

  auto* base_db = db_->GetBaseDB();
  if (!base_db) {
    RG_THROW_CODE(StorageEngineError,
                  "failed to access base rocksdb::DB for graph [{}]",
                  db_meta_.graph_name());
  }

  s = base_db->Write({}, &wb);
  if (!s.ok()) {
    RG_THROW_CODE(StorageEngineError,
                  "failed to apply raft request for graph [{}] at index {}: {}",
                  db_meta_.graph_name(), index, s.ToString());
  }

  switch (request.wb_kind()) {
    case meta::WriteBatchKind::GRAPH_WRITE:
      return;
    case meta::WriteBatchKind::ID_GENERATOR:
      SyncIdGeneratorFromRaftBatch(wb);
      return;
    default:
      RG_THROW_CODE(
          InvalidParameter,
          "unsupported write batch kind {} for graph write batch [{}] "
          "at index {}",
          static_cast<int>(request.wb_kind()), db_meta_.graph_name(), index);
  }
}

rocksdb::Status GraphDB::SetRaftApplyIndex(uint64_t apply_index,
                                           rocksdb::WriteBatch* wb) const {
  return wb->Put(graph_cf_.meta_info, kRaftApplyIndexKey,
                 std::string(AsChars(apply_index), sizeof(apply_index)));
}

void GraphDB::SyncIdGeneratorFromRaftBatch(const rocksdb::WriteBatch& wb) {
  IdGeneratorMetaBatchHandler handler(this);
  auto s = wb.Iterate(&handler);
  if (!s.ok()) {
    RG_THROW_CODE(StorageEngineError,
                  "failed to sync id generator cache from raft batch for graph "
                  "[{}]: {}",
                  db_meta_.graph_name(), s.ToString());
  }
}

void GraphDB::ProposeGraphIndexDdl(
    meta::GraphIndexDdlRequest::Operation operation, std::string payload) {
  auto* driver = raft_driver();
  if (driver == nullptr) {
    RG_THROW_CODE(StorageEngineError,
                  "raft driver is required to propose index ddl for graph [{}]",
                  db_meta_.graph_name());
  }
  meta::GraphIndexDdlRequest ddl_request;
  ddl_request.set_operation(operation);
  ddl_request.set_payload(std::move(payload));

  meta::RaftRequest raft_request;
  raft_request.set_wb_kind(meta::WriteBatchKind::GRAPH_INDEX_DDL);
  raft_request.set_wb_data(ddl_request.SerializeAsString());
  auto apply_result =
      driver->ProposeRaftRequestAndWait(std::move(raft_request));
  if (apply_result.err != nullptr) {
    RG_THROW_CODE(StorageEngineError, apply_result.err.String());
  }
}

void GraphDB::ApplyGraphIndexDdlRequest(
    uint64_t index, const meta::GraphIndexDdlRequest& request) {
  switch (request.operation()) {
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_PROPERTY_INDEX: {
      meta::VertexPropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(
            InvalidParameter,
            "failed to parse create vertex property index request for "
            "graph [{}] at index {}",
            db_meta_.graph_name(), index);
      }
      ApplyCreateVertexPropertyIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_VERTEX_PROPERTY_INDEX: {
      meta::VertexPropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(
            InvalidParameter,
            "failed to parse delete vertex property index request for "
            "graph [{}] at index {}",
            db_meta_.graph_name(), index);
      }
      ApplyDeleteVertexPropertyIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_EDGE_PROPERTY_INDEX: {
      meta::EdgePropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(InvalidParameter,
                      "failed to parse create edge property index request for "
                      "graph [{}] at index {}",
                      db_meta_.graph_name(), index);
      }
      ApplyCreateEdgePropertyIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_EDGE_PROPERTY_INDEX: {
      meta::EdgePropertyIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(InvalidParameter,
                      "failed to parse delete edge property index request for "
                      "graph [{}] at index {}",
                      db_meta_.graph_name(), index);
      }
      ApplyDeleteEdgePropertyIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_FULLTEXT_INDEX: {
      meta::VertexFullTextIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(
            InvalidParameter,
            "failed to parse create vertex fulltext index request for "
            "graph [{}] at index {}",
            db_meta_.graph_name(), index);
      }
      ApplyCreateVertexFullTextIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_VERTEX_FULLTEXT_INDEX: {
      meta::VertexFullTextIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(
            InvalidParameter,
            "failed to parse delete vertex fulltext index request for "
            "graph [{}] at index {}",
            db_meta_.graph_name(), index);
      }
      ApplyDeleteVertexFullTextIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_INDEX: {
      meta::VertexVectorIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(InvalidParameter,
                      "failed to parse create vertex vector index request for "
                      "graph [{}] at index {}",
                      db_meta_.graph_name(), index);
      }
      ApplyCreateVertexVectorIndex(index, std::move(meta));
      return;
    }
    case meta::GraphIndexDdlRequest::DELETE_VERTEX_VECTOR_INDEX: {
      meta::VertexVectorIndex meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(InvalidParameter,
                      "failed to parse delete vertex vector index request for "
                      "graph [{}] at index {}",
                      db_meta_.graph_name(), index);
      }
      ApplyDeleteVertexVectorIndex(index, meta);
      return;
    }
    case meta::GraphIndexDdlRequest::CREATE_VERTEX_VECTOR_FIELD: {
      meta::VertexVectorField meta;
      if (!meta.ParseFromString(request.payload())) {
        RG_THROW_CODE(InvalidParameter,
                      "failed to parse create vertex vector field request for "
                      "graph [{}] at index {}",
                      db_meta_.graph_name(), index);
      }
      ApplyCreateVertexVectorField(index, std::move(meta));
      return;
    }
    default:
      RG_THROW_CODE(
          InvalidParameter,
          "unsupported graph index ddl operation {} for graph [{}] at "
          "index {}",
          static_cast<int>(request.operation()), db_meta_.graph_name(), index);
  }
}

}  // namespace graphdb
