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

#include "graphdb/assistant_pool.h"

#include <pthread.h>

#include "common/exceptions.h"

namespace graphdb {

AssistantPool::AssistantPool(size_t thread_num)
    : work_(std::make_unique<boost::asio::io_service::work>(service_)) {
  if (thread_num == 0) {
    THROW_CODE(InvalidParameter, "assistant thread num must be greater than 0");
  }
  threads_.reserve(thread_num);
  for (size_t i = 0; i < thread_num; ++i) {
    threads_.emplace_back([this]() {
      pthread_setname_np(pthread_self(), "assistant");
      service_.run();
    });
  }
}

AssistantPool::~AssistantPool() {
  work_.reset();
  service_.stop();
  for (auto& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

}  // namespace graphdb
