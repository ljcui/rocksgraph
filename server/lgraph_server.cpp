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

#include "server/lgraph_server.h"

#include "common/logger.h"
#include "server/bolt_handler.h"

namespace server {

bool LGraphServer::Start() {
  if (started_.load()) {
    return true;
  }

  if (galaxy_ != nullptr) {
    LOG_ERROR("lgraph server start failed: previous galaxy is still open");
    return false;
  }

  try {
    galaxy_ = Galaxy::Open(options_.data_path, options_.galaxy_options,
                           options_.local_node_options);
  } catch (const std::exception& e) {
    LOG_ERROR("failed to open galaxy: {}", e.what());
    galaxy_.reset();
    return false;
  }

  if (!raft_server_.Start(galaxy_.get(),
                          options_.local_node_options.raft_port)) {
    galaxy_.reset();
    return false;
  }

  if (!bolt_server_.Start(
          options_.local_node_options.bolt_port, options_.bolt_io_thread_num,
          options_.max_bolt_connections,
          NewBoltHandler(
              galaxy_.get(),
              {.worker_thread_num = options_.bolt_worker_thread_num}))) {
    raft_server_.Stop();
    galaxy_.reset();
    return false;
  }

  started_.store(true);
  return true;
}

bool LGraphServer::Started() const {
  return started_.load() && bolt_server_.Started() && raft_server_.Started();
}

void LGraphServer::Stop() {
  bolt_server_.Stop();
  raft_server_.Stop();
  galaxy_.reset();
  started_.store(false);
}

}  // namespace server
