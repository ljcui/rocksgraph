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
  listener_.reset();

  std::promise<bool> promise;
  auto future = promise.get_future();
  threads_.emplace_back([this, port, &promise]() {
    bool promise_done = false;
    try {
      protobuf_handler_ = [this](std::string graph_name, raftpb::Message msg) {
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

  started_.store(false);
  graph_manager_ = nullptr;
  protobuf_handler_ = {};
  listener_.reset();
  if (had_threads) {
    LOG_INFO("Raft server stopped");
  }
}

}  // namespace server
