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

#include <any>
#include <cstdint>
#include <functional>
#include <vector>

#include "bolt/connection.h"
#include "bolt/messages.h"

namespace server {

class Galaxy;

using BoltHandler =
    std::function<void(bolt::BoltConnection& conn, bolt::BoltMsg msg,
                       std::vector<std::any> fields)>;

struct BoltHandlerOptions {
  uint32_t worker_thread_num = 4;
};

BoltHandler NewBoltHandler(Galaxy* galaxy, BoltHandlerOptions options = {});

}  // namespace server
