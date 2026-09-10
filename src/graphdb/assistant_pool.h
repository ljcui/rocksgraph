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

#include <boost/asio.hpp>
#include <memory>
#include <thread>
#include <vector>

#include "common/type_traits.h"

namespace graphdb {

class AssistantPool {
 public:
  explicit AssistantPool(size_t thread_num);
  ~AssistantPool();

  DISABLE_COPY(AssistantPool);
  DISABLE_MOVE(AssistantPool);

  boost::asio::io_service& Service() { return service_; }
  boost::asio::io_service::strand NewStrand() {
    return boost::asio::io_service::strand(service_);
  }

 private:
  boost::asio::io_service service_;
  std::unique_ptr<boost::asio::io_service::work> work_;
  std::vector<std::thread> threads_;
};

}  // namespace graphdb
