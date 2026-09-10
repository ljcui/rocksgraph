/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

/*
 * written by botu.wzy
 */
#include "bolt_server.h"

#include <pthread.h>

#include <future>

#include "common/logger.h"

namespace bolt {
bool BoltServer::Start(
    uint32_t port, uint32_t io_thread_num, size_t max_connections,
    const std::function<void(bolt::BoltConnection& conn, bolt::BoltMsg msg,
                             std::vector<std::any> fields)>& handler) {
  if (started_.load()) {
    return true;
  }

  if (!threads_.empty()) {
    LOG_ERROR("bolt server start failed: previous threads are still running");
    return false;
  }
  listener_.reset();

  std::promise<bool> promise;
  auto future = promise.get_future();
  threads_.emplace_back([this, port, io_thread_num, max_connections, handler,
                         &promise]() {
    bool promise_done = false;
    try {
      bolt::IOService<bolt::BoltConnection,
                      std::function<void(bolt::BoltConnection&, bolt::BoltMsg,
                                         std::vector<std::any> fields)>>
          bolt_service(listener_, port, io_thread_num, max_connections,
                       handler);
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
  started_.store(false);
  listener_.reset();
  if (had_threads) {
    LOG_INFO("Bolt server stopped");
  }
}

}  // namespace bolt
