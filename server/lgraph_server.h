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
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "bolt/bolt_server.h"
#include "common/type_traits.h"
#include "server/galaxy.h"
#include "server/raft_server.h"

namespace server {

struct LGraphServerOptions {
  std::string data_path;
  LocalNodeOptions local_node_options;
  uint32_t bolt_io_thread_num = 1;
  uint32_t bolt_worker_thread_num = 4;
  uint64_t max_bolt_connections = 10000;
  GalaxyOptions galaxy_options;
};

class LGraphServer final {
 public:
  explicit LGraphServer(LGraphServerOptions options)
      : options_(std::move(options)) {}

  DISABLE_COPY(LGraphServer);
  DISABLE_MOVE(LGraphServer);

  bool Start();
  void Stop();
  bool Started() const;
  Galaxy* galaxy() const { return galaxy_.get(); }
  const LGraphServerOptions& options() const { return options_; }

  ~LGraphServer() { Stop(); }

 private:
  LGraphServerOptions options_;
  std::unique_ptr<Galaxy> galaxy_;
  bolt::BoltServer bolt_server_;
  RaftServer raft_server_;
  std::atomic<bool> started_{false};
};

}  // namespace server
