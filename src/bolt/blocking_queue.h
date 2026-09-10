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
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
namespace bolt {
template <typename T>
class BlockingQueue {
 public:
  explicit BlockingQueue(size_t capacity = 0) : capacity_(capacity) {}

  bool Push(T value) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (capacity_ != 0 && queue_.size() >= capacity_) {
        return false;
      }
      queue_.push_front(std::move(value));
    }
    condition_.notify_one();
    return true;
  }

  T Pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return !queue_.empty(); });
    T ret = std::move(queue_.back());
    queue_.pop_back();
    return ret;
  }

  std::optional<T> Pop(const std::chrono::milliseconds& timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!condition_.wait_for(lock, timeout,
                             [this] { return !queue_.empty(); })) {
      return {};
    }
    T ret = std::move(queue_.back());
    queue_.pop_back();
    return ret;
  }

  std::optional<T> TryPop() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return {};
    }
    T ret = std::move(queue_.back());
    queue_.pop_back();
    return ret;
  }

  bool Empty() {
    std::unique_lock<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  size_t Size() {
    std::unique_lock<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<T> queue_;
  size_t capacity_ = 0;
};
}  // namespace bolt
