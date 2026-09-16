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

//
// Created by botu.wzy
//

#pragma once
#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bolt/messages.h"
#include "bolt/pack_stream.h"
#include "value/value.h"

namespace graphdb {
class GraphDB;
class Transaction;
}  // namespace graphdb
namespace rg {
class QueryResultCursor;
}

namespace bolt {

enum class SessionState {
  READY = 0,
  STREAMING,
  TX_READY,
  TX_STREAMING,
  FAILED,
  DEFUNCT
};

struct BoltMsgDetail {
  BoltMsg type;
  std::vector<std::any> fields;
};

struct ActiveBoltQuery {
  ~ActiveBoltQuery();
  void Commit();
  void Rollback() noexcept;

  std::string graph_name;
  std::string cypher;
  std::chrono::steady_clock::time_point start_time;
  std::shared_ptr<graphdb::GraphDB> graph_db;
  std::unique_ptr<graphdb::Transaction> transaction;
  std::unique_ptr<rg::QueryResultCursor> result;
  std::optional<std::vector<rg::Value>> buffered_row;
  bool transaction_closed = false;
};

struct BoltSession {
  void RequestInterrupt() { remaining_interrupts.fetch_add(1); }

  bool HasInterrupt() const { return remaining_interrupts.load() != 0; }

  bool ConsumeInterrupt() {
    size_t current = remaining_interrupts.load();
    while (current != 0) {
      if (remaining_interrupts.compare_exchange_weak(current, current - 1)) {
        return current == 1;
      }
    }
    return true;
  }

  std::unique_ptr<ActiveBoltQuery> active_query;
  PackStream ps;
  std::string user;
  SessionState state;
  std::atomic<size_t> remaining_interrupts = 0;
  bool utc_patch = false;
  bool python_driver = false;
};

}  // namespace bolt
