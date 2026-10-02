#include <filesystem>

#include "common/exception.h"
#include "common/logger.h"
#include "raft_driver/raft_driver.h"

namespace raft {
namespace {

eraft::Error RunWork(const std::function<void()>& work) {
  try {
    work();
    return nullptr;
  } catch (const std::exception& error) {
    return eraft::Error(error.what());
  } catch (...) {
    return eraft::Error("unknown snapshot worker failure");
  }
}

}  // namespace

void RaftDriver::FinishWork() {
  std::lock_guard lock(work_mutex_);
  --pending_work_;
  work_condition_.notify_all();
}

void RaftDriver::PostSnapshotWork(std::function<void()> work,
                                  std::function<void(eraft::Error)> completion,
                                  bool transfer) {
  {
    std::lock_guard lock(work_mutex_);
    ++pending_work_;
  }
  auto& service = transfer ? manager_->snapshot_transfer_service()
                           : manager_->snapshot_service();
  service.post([this, work = std::move(work),
                completion = std::move(completion)]() mutable {
    auto error = RunWork(work);
    manager_->raft_service(shard_id_).post(
        [this, completion = std::move(completion), error]() mutable {
          completion(error);
          FinishWork();
        });
  });
}

void RaftDriver::QueueApply(std::function<void()> work,
                            std::function<void(eraft::Error)> completion,
                            bool snapshot_worker) {
  {
    std::lock_guard lock(work_mutex_);
    ++pending_work_;
  }
  apply_jobs_.push_back(
      {std::move(work), std::move(completion), snapshot_worker});
  StartApplyJob();
}

void RaftDriver::StartApplyJob() {
  if (apply_busy_ || apply_jobs_.empty()) return;
  apply_busy_ = true;
  auto job = std::move(apply_jobs_.front());
  apply_jobs_.pop_front();
  auto& service = job.snapshot_worker ? manager_->snapshot_service()
                                      : manager_->apply_service(shard_id_);
  service.post([this, job = std::move(job)]() mutable {
    auto error = RunWork(job.work);
    manager_->raft_service(shard_id_).post(
        [this, job = std::move(job), error]() mutable {
          job.completion(error);
          apply_busy_ = false;
          StartApplyJob();
          FinishWork();
          CheckReady();
        });
  });
}

void RaftDriver::DeliverResponses(
    const google::protobuf::RepeatedPtrField<raftpb::Message>& responses) {
  for (const auto& response : responses) {
    if (response.to() == node_id_) {
      auto error = rn_->Step(response);
      if (error != nullptr)
        LOG_FATAL("failed to acknowledge Raft storage: {}", error.String());
    } else {
      SendMessage(response);
    }
  }
}

void RaftDriver::SendMessage(raftpb::Message message) {
  if (stopped_.load()) return;
  if (message.type() == raftpb::MsgSnap) {
    auto snapshot = outgoing_snapshot_;
    auto iter = node_infos_.nodes().find(message.to());
    if (!snapshot || iter == node_infos_.nodes().end() ||
        snapshot->snapshot.data() != message.snapshot().data()) {
      rn_->ReportSnapshot(message.to(), eraft::SnapshotFailure);
      return;
    }
    auto peer = iter->second;
    auto index = snapshot->snapshot.metadata().index();
    snapshot_pins_.insert(index);
    ++snapshot_sends_;
    PostSnapshotWork(
        [this, snapshot, message, peer]() {
          SendSnapshotFiles(
              local_node_.graph, peer.ip(), peer.raft_poft(), message,
              *snapshot,
              [this, term = message.term(),
               index = message.snapshot().metadata().index()]() {
                return stopped_.load() || transport_term_.load() != term ||
                       (apply_id_.load() > index &&
                        apply_id_.load() - index >
                            std::max<uint64_t>(store_config_.keep_logs,
                                               1000000));
              });
        },
        [this, snapshot, message, index](eraft::Error error) {
          --snapshot_sends_;
          auto pin = snapshot_pins_.find(index);
          if (pin != snapshot_pins_.end()) snapshot_pins_.erase(pin);
          if (!stopped_.load()) {
            auto status = rn_->GetStatus();
            auto progress = status.progress_.find(message.to());
            if (status.basicStatus_.hardState_.term() == message.term() &&
                status.basicStatus_.softState_.raftState_ ==
                    eraft::StateLeader &&
                progress != status.progress_.end() &&
                progress->second.pendingSnapshot_ == index) {
              rn_->ReportSnapshot(message.to(), error == nullptr
                                                    ? eraft::SnapshotFinish
                                                    : eraft::SnapshotFailure);
            }
            if (error != nullptr)
              LOG_WARN("snapshot to node {} failed: {}", message.to(),
                       error.String());
          }
          if (snapshot_sends_ == 0 && outgoing_snapshot_ == snapshot) {
            storage_->PublishSnapshot({});
            outgoing_snapshot_.reset();
          }
          CheckReady();
        },
        true);
    return;
  }
  std::shared_lock lock(nodes_mutex_);
  auto iter = node_clients_.find(message.to());
  if (iter != node_clients_.end() && iter->second->connected()) {
    iter->second->Send(MessageToNetString(local_node_.graph, message));
    mark_unreachable_.erase(message.to());
  } else if (mark_unreachable_.insert(message.to()).second) {
    rn_->ReportUnreachable(message.to());
  }
}

void RaftDriver::RequestSnapshot() {
  if (building_snapshot_ || stopped_.load() || !snapshots_.create) return;
  building_snapshot_ = true;
  auto result = std::make_shared<std::shared_ptr<SnapshotFiles>>();
  auto directory =
      std::filesystem::path(store_config_.path).parent_path() / "snapshots" /
      "out" /
      (std::to_string(node_id_) + "-" + std::to_string(id_generator_.Next()));
  QueueApply(
      [this, result, directory]() {
        *result = snapshots_.create(directory.string());
      },
      [this, result](eraft::Error error) {
        if (error != nullptr || stopped_.load()) {
          building_snapshot_ = false;
          if (error != nullptr)
            LOG_WARN("cannot create snapshot: {}", error.String());
          return;
        }
        auto snapshot = *result;
        auto term = storage_->Term(snapshot->snapshot.metadata().index());
        if (term.second != nullptr) {
          building_snapshot_ = false;
          return;
        }
        snapshot->snapshot.mutable_metadata()->set_term(term.first);
        // The checkpoint is now fixed. Hash its files without holding the apply
        // barrier; ordinary graph writes can continue throughout this disk
        // scan.
        PostSnapshotWork(
            [this, snapshot, term = rn_->GetBasicStatus().hardState_.term()]() {
              DescribeSnapshotFiles(*snapshot, [this, snapshot, term]() {
                const auto index = snapshot->snapshot.metadata().index();
                return stopped_.load() || transport_term_.load() != term ||
                       (apply_id_.load() > index &&
                        apply_id_.load() - index >
                            std::max<uint64_t>(store_config_.keep_logs,
                                               1000000));
              });
            },
            [this, snapshot](eraft::Error scan_error) {
              building_snapshot_ = false;
              if (scan_error != nullptr) {
                LOG_WARN("cannot describe snapshot: {}", scan_error.String());
                return;
              }
              if (stopped_.load() ||
                  rn_->GetBasicStatus().softState_.raftState_ !=
                      eraft::StateLeader)
                return;
              auto reference = ParseSnapshotReference(snapshot->snapshot);
              // A concurrent membership change requires a newer snapshot.
              if (reference.node_infos().nodes_size() !=
                  node_infos_.nodes_size())
                return;
              for (const auto& [id, info] : node_infos_.nodes()) {
                auto iter = reference.node_infos().nodes().find(id);
                if (iter == reference.node_infos().nodes().end() ||
                    iter->second.SerializeAsString() !=
                        info.SerializeAsString())
                  return;
              }
              outgoing_snapshot_ = snapshot;
              snapshot_created_ = std::chrono::steady_clock::now();
              storage_->PublishSnapshot(snapshot->snapshot);
              CheckReady();
            });
      },
      true);
}

void RaftDriver::ScheduleCompaction(
    uint64_t index, std::function<void(eraft::Error)> completion) {
  if (!snapshots_.sync) {
    completion(eraft::Error(
        "state machine durability callback is required for compaction"));
    return;
  }
  auto durable = std::make_shared<uint64_t>(0);
  QueueApply(
      [this, durable]() { *durable = snapshots_.sync(); },
      [this, durable, index,
       completion = std::move(completion)](eraft::Error error) {
        if (error != nullptr) {
          completion(error);
          return;
        }
        if (index > *durable) {
          completion(eraft::Error("cannot compact unapplied log entries"));
          return;
        }
        if (building_snapshot_ || installing_snapshot_) {
          completion(
              eraft::Error("snapshot creation or installation is in progress"));
          return;
        }
        uint64_t boundary = index;
        if (!snapshot_pins_.empty())
          boundary = std::min(boundary, *snapshot_pins_.begin());
        if (outgoing_snapshot_)
          boundary = std::min(boundary,
                              outgoing_snapshot_->snapshot.metadata().index());
        if (boundary >= storage_->FirstIndex().first &&
            boundary <= storage_->LastIndex().first) {
          storage_->Compact(boundary);
        }
        completion(nullptr);
      });
}

eraft::Error RaftDriver::CompactLog(uint64_t index) {
  auto promise = std::make_shared<std::promise<eraft::Error>>();
  auto future = promise->get_future();
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post([this, alive, index, promise]() {
    if (!alive->load() || stopped_.load()) {
      promise->set_value(eraft::Error("raft driver stopped"));
      return;
    }
    ScheduleCompaction(
        index, [promise](eraft::Error error) { promise->set_value(error); });
  });
  return future.get();
}

void RaftDriver::ReceiveSnapshot(std::shared_ptr<SnapshotFiles> snapshot,
                                 raftpb::Message message,
                                 std::function<void(eraft::Error)> completion) {
  auto alive = callback_alive_;
  manager_->raft_service(shard_id_).post(
      [this, alive, snapshot = std::move(snapshot),
       message = std::move(message),
       completion = std::move(completion)]() mutable {
        if (!alive->load() || stopped_.load() || installing_snapshot_ ||
            incoming_snapshot_) {
          completion(eraft::Error("snapshot receiver is unavailable"));
          return;
        }
        incoming_snapshot_ = snapshot;
        incoming_completion_ = std::move(completion);
        if (join_target_index_.load() == 0)
          join_target_index_.store(message.snapshot().metadata().index());
        auto error = rn_->Step(std::move(message));
        CheckReady();
        if (error != nullptr || !installing_snapshot_) {
          auto done = std::move(incoming_completion_);
          incoming_snapshot_.reset();
          done(error);
        }
      });
}

void RaftDriver::StartSnapshotInstall(raftpb::Message append) {
  RG_CHECK(incoming_snapshot_ && snapshots_.install,
           common::ErrorCode::InternalError,
           "snapshot file transfer must precede Raft snapshot installation");
  installing_snapshot_ = true;
  auto snapshot = incoming_snapshot_;
  auto reference = ParseSnapshotReference(append.snapshot());
  RG_CHECK(snapshot->snapshot.data() == append.snapshot().data(),
           common::ErrorCode::InternalError,
           "received snapshot files do not match Ready snapshot");
  const auto root = std::filesystem::path(store_config_.path).parent_path();
  const auto record_path = root / "snapshot_install.pb";
  auto record = std::make_shared<meta::SnapshotInstall>();
  *record->mutable_snapshot() = append.snapshot();
  record->mutable_hard_state()->set_term(append.term());
  record->mutable_hard_state()->set_vote(append.vote());
  record->mutable_hard_state()->set_commit(append.commit());
  *record->mutable_entries() = append.entries();
  record->set_directory(
      std::filesystem::relative(snapshot->directory, root).string());
  QueueApply(
      [this, snapshot, record, record_path]() {
        WriteSnapshotRecord(record_path, record->SerializeAsString());
        snapshot->keep = true;
        snapshots_.install(snapshot);
        apply_id_.store(snapshot->snapshot.metadata().index());
      },
      [this, snapshot, record, record_path, reference,
       append = std::move(append)](eraft::Error error) {
        if (error != nullptr) {
          // Keep the durable installation record and refuse further Ready work.
          // Startup will finish the installation before opening Raft again.
          LOG_ERROR("snapshot installation requires recovery: {}",
                    error.String());
          auto done = std::move(incoming_completion_);
          incoming_snapshot_.reset();
          if (done) done(error);
          return;
        }
        rocksdb::WriteBatch batch;
        storage_->InstallSnapshot(record->snapshot(), batch);
        storage_->Append({record->entries().begin(), record->entries().end()},
                         batch);
        if (!eraft::IsEmptyHardState(record->hard_state()))
          storage_->SetHardState(record->hard_state(), batch);
        storage_->WriteBatch(batch, true);
        {
          std::unique_lock lock(nodes_mutex_);
          node_infos_ = reference.node_infos();
          node_clients_.clear();
          for (const auto& [id, info] : node_infos_.nodes()) {
            node_clients_[id] =
                manager_->AcquireClient(info.ip(), info.raft_poft());
          }
        }
        QueueApply(
            [this, record_path]() {
              RemoveSnapshotRecord(record_path);
              std::filesystem::remove_all(record_path.parent_path() /
                                          "data.previous");
              if (snapshots_.ready)
                snapshots_.ready(join_target_index_.load(), node_id_);
            },
            [this, append](eraft::Error finish_error) {
              if (finish_error != nullptr)
                LOG_FATAL("cannot finish snapshot installation: {}",
                          finish_error.String());
              installing_snapshot_ = false;
              DeliverResponses(append.responses());
              auto done = std::move(incoming_completion_);
              incoming_snapshot_.reset();
              if (done) done(nullptr);
              CheckReady();
            },
            true);
      },
      true);
}

}  // namespace raft
