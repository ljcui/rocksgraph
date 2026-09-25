#include "bolt_server.h"

#include <pthread.h>

#include <future>

#include "common/logger.h"

namespace bolt {
bool BoltServer::Start(
    uint32_t port, uint32_t io_thread_num, size_t max_connections,
    const std::function<void(bolt::BoltConnection& conn, bolt::BoltMsg msg,
                             std::vector<std::any> fields)>& handler,
    std::shared_ptr<BoltWorkerPool> worker_pool,
    size_t max_message_size) {
  if (started_.load()) {
    return true;
  }

  if (!threads_.empty()) {
    LOG_ERROR("bolt server start failed: previous threads are still running");
    return false;
  }
  if (!worker_pool) {
    LOG_ERROR("bolt server start failed: worker pool is null");
    return false;
  }
  listener_.reset();
  worker_pool_ = std::move(worker_pool);

  std::promise<bool> promise;
  auto future = promise.get_future();
  threads_.emplace_back([this, port, io_thread_num, max_connections,
                         max_message_size, handler, &promise]() {
    bool promise_done = false;
    try {
      bolt::IOService<bolt::BoltConnection,
                      std::function<void(bolt::BoltConnection&, bolt::BoltMsg,
                                         std::vector<std::any> fields)>>
          bolt_service(listener_, port, io_thread_num, max_connections,
                       handler, max_message_size);
      boost::asio::io_service::work holder(listener_);

      started_.store(true);
      promise.set_value(true);
      promise_done = true;

      LOG_INFO("Bolt server run");
      pthread_setname_np(pthread_self(), "bolt_listener");
      listener_.run();
    } catch (const std::exception& e) {
      LOG_ERROR("bolt server exception: {}", e.what());
      if (!promise_done) {
        promise.set_value(false);
      }
    }
    started_.store(false);
    LOG_INFO("Bolt server exit");
  });

  if (future.get()) {
    return true;
  }

  Stop();
  return false;
}

void BoltServer::Stop() {
  bool had_threads = !threads_.empty();
  listener_.stop();
  for (auto& t : threads_) {
    t.join();
  }
  threads_.clear();
  if (worker_pool_) {
    worker_pool_->Stop();
    worker_pool_.reset();
  }
  started_.store(false);
  listener_.reset();
  if (had_threads) {
    LOG_INFO("Bolt server stopped");
  }
}

}  // namespace bolt
