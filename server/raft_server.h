#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <functional>
#include <thread>
#include <vector>

#include "etcd_raft/raftpb/raft.pb.h"

namespace server {

class GraphManager;

class RaftServer final {
 public:
  RaftServer() = default;
  RaftServer(const RaftServer&) = delete;
  RaftServer& operator=(const RaftServer&) = delete;
  RaftServer(RaftServer&&) = delete;
  RaftServer& operator=(RaftServer&&) = delete;

  bool Start(GraphManager* graph_manager, uint32_t port);
  void Stop();
  bool Started() const { return started_.load(); }

  ~RaftServer() { Stop(); }

  std::vector<std::thread> threads_;
  std::atomic<bool> started_{false};
  GraphManager* graph_manager_ = nullptr;
  boost::asio::io_service listener_{BOOST_ASIO_CONCURRENCY_HINT_UNSAFE};
  std::function<void(std::string, raftpb::Message)> protobuf_handler_{};
};

}  // namespace server
