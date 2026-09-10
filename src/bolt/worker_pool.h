/**
 * Copyright 2026 AntGroup CO., Ltd.
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

#pragma once

#include <pthread.h>

#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <utility>

#include "common/logger.h"
#include "common/type_traits.h"

namespace bolt {

class BoltWorkerPool {
 public:
  using Executor = boost::asio::thread_pool::executor_type;
  using Strand = boost::asio::strand<Executor>;

  explicit BoltWorkerPool(uint32_t thread_num, std::string worker_name_prefix,
                          std::string log_name)
      : pool_(thread_num == 0 ? 1 : thread_num),
        worker_name_prefix_(std::move(worker_name_prefix)),
        log_name_(std::move(log_name)) {}

  DISABLE_COPY(BoltWorkerPool);
  DISABLE_MOVE(BoltWorkerPool);

  ~BoltWorkerPool() { Stop(); }

  Strand MakeStrand() { return Strand(pool_.get_executor()); }

  bool Post(std::function<void()> task) {
    if (stopped_.load(std::memory_order_acquire)) {
      return false;
    }
    boost::asio::post(pool_, WrapTask(std::move(task)));
    return true;
  }

  bool Post(const Strand& strand, std::function<void()> task) {
    if (stopped_.load(std::memory_order_acquire)) {
      return false;
    }
    boost::asio::post(strand, WrapTask(std::move(task)));
    return true;
  }

  void Stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
      return;
    }
    pool_.stop();
    pool_.join();
  }

 private:
  std::function<void()> WrapTask(std::function<void()> task) {
    return [this, task = std::move(task)]() mutable {
      InitWorkerThread();
      try {
        task();
      } catch (const std::exception& e) {
        LOG_ERROR("{} worker task failed: {}", log_name_, e.what());
      } catch (...) {
        LOG_ERROR("{} worker task failed with unknown exception", log_name_);
      }
    };
  }

  void InitWorkerThread() {
    static thread_local bool initialized = false;
    if (initialized) {
      return;
    }
    initialized = true;

    auto worker_id = next_worker_id_.fetch_add(1, std::memory_order_relaxed);
    std::string name = worker_name_prefix_ + std::to_string(worker_id);
    pthread_setname_np(pthread_self(), name.c_str());
  }

  boost::asio::thread_pool pool_;
  std::string worker_name_prefix_;
  std::string log_name_;
  std::atomic<bool> stopped_{false};
  std::atomic<uint32_t> next_worker_id_{0};
};

}  // namespace bolt
