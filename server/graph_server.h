#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "bolt/bolt_server.h"
#include "server/graph_manager.h"
#include "server/raft_server.h"

namespace server {

struct GraphServerOptions {
  std::string data_path;
  LocalNodeOptions local_node_options;
  uint32_t bolt_io_thread_num = 1;
  uint32_t bolt_worker_thread_num = 4;
  uint64_t max_bolt_connections = 10000;
  GraphManagerOptions graph_manager_options;
};

class GraphServer final {
 public:
  explicit GraphServer(GraphServerOptions options)
      : options_(std::move(options)) {}

  GraphServer(const GraphServer&) = delete;
  GraphServer& operator=(const GraphServer&) = delete;
  GraphServer(GraphServer&&) = delete;
  GraphServer& operator=(GraphServer&&) = delete;

  bool Start();
  void Stop();
  bool Started() const;
  GraphManager* graph_manager() const { return graph_manager_.get(); }
  const GraphServerOptions& options() const { return options_; }

  ~GraphServer() { Stop(); }

 private:
  GraphServerOptions options_;
  std::unique_ptr<GraphManager> graph_manager_;
  bolt::BoltServer bolt_server_;
  RaftServer raft_server_;
  std::atomic<bool> started_{false};
};

}  // namespace server
