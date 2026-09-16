#include "server/graph_server.h"

#include "common/logger.h"
#include "server/bolt_handler.h"

namespace server {

bool GraphServer::Start() {
  if (started_.load()) {
    return true;
  }

  if (graph_manager_ != nullptr) {
    LOG_ERROR("rg-server start failed: previous graph manager is still open");
    return false;
  }

  try {
    graph_manager_ =
        GraphManager::Open(options_.data_path, options_.graph_manager_options,
                           options_.local_node_options);
  } catch (const std::exception& e) {
    LOG_ERROR("failed to open graph manager: {}", e.what());
    graph_manager_.reset();
    return false;
  }

  if (!raft_server_.Start(graph_manager_.get(),
                          options_.local_node_options.raft_port)) {
    graph_manager_.reset();
    return false;
  }

  if (!bolt_server_.Start(
          options_.local_node_options.bolt_port, options_.bolt_io_thread_num,
          options_.max_bolt_connections,
          NewBoltHandler(
              graph_manager_.get(),
              {.worker_thread_num = options_.bolt_worker_thread_num}))) {
    raft_server_.Stop();
    graph_manager_.reset();
    return false;
  }

  started_.store(true);
  return true;
}

bool GraphServer::Started() const {
  return started_.load() && bolt_server_.Started() && raft_server_.Started();
}

void GraphServer::Stop() {
  bolt_server_.Stop();
  raft_server_.Stop();
  graph_manager_.reset();
  started_.store(false);
}

}  // namespace server
