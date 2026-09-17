#pragma once
#include <atomic>
#include <thread>
#include <vector>

#include "connection.h"
#include "io_service.h"

namespace bolt {
class BoltServer final {
 public:
  BoltServer() = default;
  BoltServer(const BoltServer&) = delete;
  BoltServer& operator=(const BoltServer&) = delete;
  BoltServer(BoltServer&&) = delete;
  BoltServer& operator=(BoltServer&&) = delete;
  bool Start(
      uint32_t port, uint32_t io_thread_num, size_t max_connections,
      const std::function<void(bolt::BoltConnection& conn, bolt::BoltMsg msg,
                               std::vector<std::any> fields)>& handler);
  void Stop();
  bool Started() const { return started_.load(); }
  ~BoltServer() { Stop(); }

  std::vector<std::thread> threads_;
  std::atomic<bool> started_{false};
  boost::asio::io_service listener_{BOOST_ASIO_CONCURRENCY_HINT_UNSAFE};
};
}  // namespace bolt
