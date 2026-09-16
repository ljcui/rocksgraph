#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <functional>
#include <thread>
#include <vector>

#include "common/type_traits.h"
#include "etcd_raft/raftpb/raft.pb.h"

namespace server {

class GraphManager;

class RaftServer final {
 public:
  RaftServer() = default;
  DISABLE_COPY(RaftServer);
  DISABLE_MOVE(RaftServer);

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
