//
// Created by botu.wzy
//

#include <future>

#include "common/exception.h"
#include "common/logger.h"
#include "graph_db.h"
#include "graph_db_internal.h"

namespace graphdb {
using namespace internal;

void GraphDB::PersistVertexPropertyIndexMeta(
    const std::shared_ptr<VertexPropertyIndex>& index) {
  auto s = db_->Put(
      {}, graph_cf_.meta_info,
      BuildMetaKey(MetadataType::VertexPropertyIndex, index->meta().name()),
      index->meta().SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void GraphDB::PersistEdgePropertyIndexMeta(
    const std::shared_ptr<EdgePropertyIndex>& index) {
  auto s = db_->Put(
      {}, graph_cf_.meta_info,
      BuildMetaKey(MetadataType::EdgePropertyIndex, index->meta().name()),
      index->meta().SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void GraphDB::PersistVertexFullTextIndexMeta(
    const std::shared_ptr<VertexFullTextIndex>& index) {
  auto s = db_->Put(
      {}, graph_cf_.meta_info,
      BuildMetaKey(MetadataType::VertexFullTextIndex, index->meta().name()),
      index->meta().SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void GraphDB::PersistVertexVectorIndexMeta(
    const std::shared_ptr<VertexVectorIndex>& index) {
  auto s = db_->Put(
      {}, graph_cf_.meta_info,
      BuildMetaKey(MetadataType::VertexVectorIndex, index->meta().name()),
      index->meta().SerializeAsString());
  if (!s.ok()) RG_THROW_CODE(StorageEngineError, s.ToString());
}

void GraphDB::ResumeBackgroundIndexBuilds() {
  for (const auto& index : meta_info_.GetBuildingVertexPropertyIndexes()) {
    ScheduleVertexPropertyIndexBuild(index, true);
  }
  for (const auto& index : meta_info_.GetBuildingEdgePropertyIndexes()) {
    ScheduleEdgePropertyIndexBuild(index, true);
  }
  for (const auto& index : meta_info_.GetBuildingVertexFullTextIndexes()) {
    ScheduleVertexFullTextIndexBuild(index, true);
  }
  for (const auto& index : meta_info_.GetBuildingVertexVectorIndexes()) {
    ScheduleVertexVectorIndexBuild(index, true);
  }
}

void GraphDB::DrainAssistant() {
  if (!assistant_strand_) {
    return;
  }
  if (assistant_strand_->running_in_this_thread()) {
    return;
  }
  std::promise<void> drained;
  auto future = drained.get_future();
  assistant_strand_->post([&drained]() mutable { drained.set_value(); });
  future.wait();
}

void GraphDB::ScheduleVertexPropertyIndexBuild(
    const std::shared_ptr<VertexPropertyIndex>& index, bool reset_existing) {
  assistant_strand_->post([this, index, reset_existing]() {
    const rocksdb::Snapshot* snapshot = nullptr;
    try {
      if (reset_existing) {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        DeletePropertyIndexRanges(db_, &graph_cf_, index->index_id());
        index->ResetForBuild();
        index->SetState(meta::IndexBuildState::BUILDING);
        PersistVertexPropertyIndexMeta(index);
      }

      snapshot = db_->GetSnapshot();
      uint64_t snapshot_wal_id =
          LoadVisibleMaxWalId(db_, &graph_cf_, index->index_id(), snapshot);
      index->meta().set_build_start_wal_id(snapshot_wal_id + 1);
      index->Load(snapshot, snapshot_wal_id);
      db_->ReleaseSnapshot(snapshot);
      snapshot = nullptr;

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->SetState(meta::IndexBuildState::CATCHING_UP);
        index->SetBuildError("");
        PersistVertexPropertyIndexMeta(index);
      }

      index->ApplyWAL();

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->ApplyWAL();
        DeletePropertyIndexWalRange(db_, &graph_cf_, index->index_id());
        index->SetState(meta::IndexBuildState::READY);
        index->SetBuildError("");
        PersistVertexPropertyIndexMeta(index);
        meta_info_.PublishVertexPropertyIndex(index->Name());
      }
    } catch (const std::exception& e) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex property index [{}] build failed: {}", index->Name(),
                e.what());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError(e.what());
        PersistVertexPropertyIndexMeta(index);
      }
    } catch (...) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex property index [{}] build failed with unknown error",
                index->Name());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError("unknown error");
        PersistVertexPropertyIndexMeta(index);
      }
    }
  });
}

void GraphDB::ScheduleEdgePropertyIndexBuild(
    const std::shared_ptr<EdgePropertyIndex>& index, bool reset_existing) {
  assistant_strand_->post([this, index, reset_existing]() {
    const rocksdb::Snapshot* snapshot = nullptr;
    try {
      if (reset_existing) {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
        if (index->IsDeleted()) return;
        DeletePropertyIndexRanges(db_, &graph_cf_, index->index_id());
        index->ResetForBuild();
        index->SetState(meta::IndexBuildState::BUILDING);
        PersistEdgePropertyIndexMeta(index);
      }

      snapshot = db_->GetSnapshot();
      uint64_t snapshot_wal_id =
          LoadVisibleMaxWalId(db_, &graph_cf_, index->index_id(), snapshot);
      index->meta().set_build_start_wal_id(snapshot_wal_id + 1);
      index->Load(snapshot, snapshot_wal_id);
      db_->ReleaseSnapshot(snapshot);
      snapshot = nullptr;

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        if (index->IsDeleted()) return;
        index->SetState(meta::IndexBuildState::CATCHING_UP);
        index->SetBuildError("");
        PersistEdgePropertyIndexMeta(index);
      }
      index->ApplyWAL();
      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(property_index_commit_mutex_);
        if (index->IsDeleted()) return;
        index->ApplyWAL();
        DeletePropertyIndexWalRange(db_, &graph_cf_, index->index_id());
        index->SetState(meta::IndexBuildState::READY);
        index->SetBuildError("");
        PersistEdgePropertyIndexMeta(index);
        meta_info_.PublishEdgePropertyIndex(index->Name());
      }
    } catch (const std::exception& e) {
      if (snapshot) db_->ReleaseSnapshot(snapshot);
      LOG_ERROR("edge property index [{}] build failed: {}", index->Name(),
                e.what());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError(e.what());
        PersistEdgePropertyIndexMeta(index);
      }
    } catch (...) {
      if (snapshot) db_->ReleaseSnapshot(snapshot);
      LOG_ERROR("edge property index [{}] build failed with unknown error",
                index->Name());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError("unknown error");
        PersistEdgePropertyIndexMeta(index);
      }
    }
  });
}

void GraphDB::ScheduleVertexFullTextIndexBuild(
    const std::shared_ptr<VertexFullTextIndex>& index, bool reset_existing) {
  assistant_strand_->post([this, index, reset_existing]() {
    const rocksdb::Snapshot* snapshot = nullptr;
    try {
      if (reset_existing) {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(fulltext_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        DeleteFullTextIndexRanges(db_, &graph_cf_, index->index_id());
        index->ReleaseResources();
        ResetFullTextIndexPath(index->meta().path());
        index->ResetForClear();
        index->SetState(meta::IndexBuildState::BUILDING);
        PersistVertexFullTextIndexMeta(index);
      }

      snapshot = db_->GetSnapshot();
      uint64_t snapshot_wal_id =
          LoadVisibleMaxWalId(db_, &graph_cf_, index->index_id(), snapshot);
      index->meta().set_build_start_wal_id(snapshot_wal_id + 1);
      index->Load(snapshot, snapshot_wal_id);
      db_->ReleaseSnapshot(snapshot);
      snapshot = nullptr;

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->SetState(meta::IndexBuildState::CATCHING_UP);
        index->SetBuildError("");
        PersistVertexFullTextIndexMeta(index);
      }

      index->ApplyWAL();

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(fulltext_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->ApplyWAL();
        index->SetState(meta::IndexBuildState::READY);
        index->SetBuildError("");
        PersistVertexFullTextIndexMeta(index);
        meta_info_.PublishVertexFullTextIndex(index->Name());
        index->Start();
      }
    } catch (const std::exception& e) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex fulltext index [{}] build failed: {}", index->Name(),
                e.what());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError(e.what());
        PersistVertexFullTextIndexMeta(index);
      }
    } catch (...) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex fulltext index [{}] build failed with unknown error",
                index->Name());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError("unknown error");
        PersistVertexFullTextIndexMeta(index);
      }
    }
  });
}

void GraphDB::ScheduleVertexVectorIndexBuild(
    const std::shared_ptr<VertexVectorIndex>& index, bool reset_existing) {
  assistant_strand_->post([this, index, reset_existing]() {
    const rocksdb::Snapshot* snapshot = nullptr;
    try {
      if (reset_existing) {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(vector_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        DeleteVectorIndexRanges(db_, &graph_cf_, index->index_id());
        index->ReleaseResources();
        ResetIndexPath(index->meta().path(), "vector index");
        index->ResetForClear();
        index->SetState(meta::IndexBuildState::BUILDING);
        PersistVertexVectorIndexMeta(index);
      }

      snapshot = db_->GetSnapshot();
      uint64_t snapshot_wal_id =
          LoadVisibleMaxWalId(db_, &graph_cf_, index->index_id(), snapshot);
      index->meta().set_build_start_wal_id(snapshot_wal_id + 1);
      index->Load(snapshot, snapshot_wal_id);
      db_->ReleaseSnapshot(snapshot);
      snapshot = nullptr;

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->SetState(meta::IndexBuildState::CATCHING_UP);
        index->SetBuildError("");
        PersistVertexVectorIndexMeta(index);
      }

      index->ApplyWAL();

      {
        std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
        std::lock_guard<std::mutex> commit_lock(vector_index_commit_mutex_);
        if (index->IsDeleted()) {
          return;
        }
        index->ApplyWAL();
        index->SetState(meta::IndexBuildState::READY);
        index->SetBuildError("");
        PersistVertexVectorIndexMeta(index);
        meta_info_.PublishVertexVectorIndex(index->meta().name());
        index->Start();
      }
    } catch (const std::exception& e) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex vector index [{}] build failed: {}",
                index->meta().name(), e.what());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError(e.what());
        PersistVertexVectorIndexMeta(index);
      }
    } catch (...) {
      if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
      }
      LOG_ERROR("vertex vector index [{}] build failed with unknown error",
                index->meta().name());
      std::lock_guard<std::mutex> ddl_lock(index_ddl_mutex_);
      if (!index->IsDeleted()) {
        index->SetState(meta::IndexBuildState::FAILED);
        index->SetBuildError("unknown error");
        PersistVertexVectorIndexMeta(index);
      }
    }
  });
}

}  // namespace graphdb
