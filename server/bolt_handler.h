#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <vector>

#include "bolt/connection.h"
#include "bolt/messages.h"

namespace server {

class GraphManager;

using BoltHandler =
    std::function<void(bolt::BoltConnection& conn, bolt::BoltMsg msg,
                       std::vector<std::any> fields)>;

struct BoltHandlerOptions {
  uint32_t worker_thread_num = 4;
};

BoltHandler NewBoltHandler(GraphManager* graph_manager,
                           BoltHandlerOptions options = {});

}  // namespace server
