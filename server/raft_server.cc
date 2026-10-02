#include "server/raft_server.h"

#include <pthread.h>

#include <future>
#include <stdexcept>

#include "common/logger.h"
#include "raft_driver/connection.h"
#include "raft_driver/io_service.h"
#include "server/graph_manager.h"

namespace server {

bool RaftServer::Start(GraphManager* graph_manager, uint32_t port) {
  if (started_.load()) {
    return true;
  }

  if (!threads_.empty()) {
    LOG_ERROR("raft server start failed: previous threads are still running");
    return false;
  }
  graph_manager_ = graph_manager;
  snapshot_workers_ = std::make_unique<boost::asio::thread_pool>(2);
  listener_.reset();

  std::promise<bool> promise;
  auto future = promise.get_future();
  threads_.emplace_back([this, port, &promise]() {
    bool promise_done = false;
    try {
      protobuf_handler_.message = [this](std::string graph_name,
                                         raftpb::Message msg) {
        if (graph_manager_ == nullptr) {
          LOG_WARN(
              "receive raft message for graph [{}] while graph manager is null",
              graph_name);
          return;
        }
        try {
          auto graph = graph_manager_->OpenGraph(graph_name);
          auto* raft_driver = graph->raft_driver();
          if (raft_driver == nullptr) {
            LOG_WARN("graph [{}] does not enable raft, drop message",
                     graph_name);
            return;
          }
          raft_driver->Step(std::move(msg));
        } catch (const std::exception& e) {
          LOG_WARN("failed to route raft message for graph [{}]: {}",
                   graph_name, e.what());
        }
      };

      protobuf_handler_.snapshot =
          [this](std::string graph_name, meta::SnapshotFrame frame,
                 std::shared_ptr<raft::SnapshotReceiver> receiver,
                 std::function<void(std::string)> reply) {
            boost::asio::post(
                *snapshot_workers_, [this, graph_name = std::move(graph_name),
                                     frame = std::move(frame), receiver,
                                     reply = std::move(reply)]() mutable {
                  try {
                    auto graph = graph_manager_->OpenGraph(graph_name);
                    auto* driver = graph->raft_driver();
                    RG_CHECK(driver != nullptr,
                             common::ErrorCode::InvalidParameter,
                             "snapshot graph does not enable Raft");
                    if (frame.kind() == meta::SnapshotFrame::BEGIN) {
                      RG_CHECK(frame.message().to() ==
                                   driver->GetRaftStatus().s.basicStatus_.id_,
                               common::ErrorCode::InvalidParameter,
                               "snapshot recipient does not match local node");
                      auto reference = raft::ParseSnapshotReference(
                          frame.message().snapshot());
                      for (const auto& [id, info] :
                           reference.node_infos().nodes()) {
                        RG_CHECK(info.graph() == graph_name,
                                 common::ErrorCode::InvalidParameter,
                                 "snapshot member graph does not match");
                      }
                    }
                    receiver->Consume(frame, graph->path());
                    if (frame.kind() == meta::SnapshotFrame::FINISH) {
                      auto files = receiver->Finish();
                      driver->ReceiveSnapshot(
                          std::move(files), receiver->message(),
                          [graph, reply](eraft::Error error) {
                            reply(error == nullptr ? std::string{}
                                                   : error.String());
                          });
                    } else {
                      reply({});
                    }
                  } catch (const std::exception& error) {
                    reply(error.what());
                  }
                });
          };

      raft::IOService<raft::RaftConnection, decltype(protobuf_handler_)>
          raft_service(listener_, port, 1, protobuf_handler_);
      boost::asio::io_service::work holder(listener_);

      started_.store(true);
      promise.set_value(true);
      promise_done = true;

      LOG_INFO("Raft server run");
      pthread_setname_np(pthread_self(), "raft_listener");
      listener_.run();
    } catch (const std::exception& e) {
      LOG_ERROR("raft server exception: {}", e.what());
      if (!promise_done) {
        promise.set_value(false);
      }
    }
    started_.store(false);
    LOG_INFO("Raft server exit");
  });

  if (future.get()) {
    return true;
  }

  Stop();
  return false;
}

void RaftServer::Stop() {
  bool had_threads = !threads_.empty();
  listener_.stop();
  for (auto& t : threads_) {
    t.join();
  }
  threads_.clear();

  if (snapshot_workers_) {
    snapshot_workers_->join();
    snapshot_workers_.reset();
  }

  started_.store(false);
  graph_manager_ = nullptr;
  protobuf_handler_ = {};
  listener_.reset();
  if (had_threads) {
    LOG_INFO("Raft server stopped");
  }
}

}  // namespace server
