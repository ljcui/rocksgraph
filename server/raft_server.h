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

#include <atomic>
#include <boost/asio.hpp>
#include <functional>
#include <thread>
#include <vector>

#include "common/type_traits.h"
#include "etcd_raft/raftpb/raft.pb.h"

namespace server {

class Galaxy;

class RaftServer final {
 public:
  RaftServer() = default;
  DISABLE_COPY(RaftServer);
  DISABLE_MOVE(RaftServer);

  bool Start(Galaxy* galaxy, uint32_t port);
  void Stop();
  bool Started() const { return started_.load(); }

  ~RaftServer() { Stop(); }

  std::vector<std::thread> threads_;
  std::atomic<bool> started_{false};
  Galaxy* galaxy_ = nullptr;
  boost::asio::io_service listener_{BOOST_ASIO_CONCURRENCY_HINT_UNSAFE};
  std::function<void(std::string, raftpb::Message)> protobuf_handler_{};
};

}  // namespace server
