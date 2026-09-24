#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "bolt/connection.h"
#include "bolt/messages.h"

namespace bolt {
class BoltWorkerPool;
}

namespace server {

class GraphManager;

using BoltHandler =
    std::function<void(bolt::BoltConnection& conn, bolt::BoltMsg msg,
                       std::vector<std::any> fields)>;

BoltHandler NewBoltHandler(GraphManager* graph_manager,
                           std::shared_ptr<bolt::BoltWorkerPool> worker_pool);

}  // namespace server
