#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "bolt/bolt_server.h"
#include "server/graph_manager.h"
#include "server/http_server.h"
#include "server/raft_server.h"

namespace server {

struct GraphServerOptions {
  std::string data_path;
  LocalNodeOptions local_node_options;
  uint32_t bolt_io_thread_num = 1;
  uint32_t bolt_worker_thread_num = 4;
  uint64_t max_bolt_connections = 10000;
  uint64_t bolt_max_message_size = bolt::kDefaultMaxBoltMessageSize;
  // Zero disables HTTP for embedded servers; rg-server defaults to 7689.
  uint32_t http_port = 0;
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
  uint16_t http_port() const { return http_server_.Port(); }

  ~GraphServer() { Stop(); }

 private:
  GraphServerOptions options_;
  std::unique_ptr<GraphManager> graph_manager_;
  bolt::BoltServer bolt_server_;
  RaftServer raft_server_;
  HttpServer http_server_;
  std::atomic<bool> started_{false};
};

}  // namespace server
