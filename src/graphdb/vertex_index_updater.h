/**
 * Copyright 2024 AntGroup CO., Ltd.
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

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace txn {
class Transaction;
}

namespace graphdb {

using VertexSerializedProperties = std::unordered_map<uint32_t, std::string>;
using VertexVectorProperties = std::unordered_map<uint32_t, std::vector<float>>;

void UpdateVertexIndexes(txn::Transaction* txn, int64_t vid,
                         const std::unordered_set<uint32_t>& old_lids,
                         const std::unordered_set<uint32_t>& new_lids,
                         const VertexSerializedProperties& old_properties,
                         const VertexSerializedProperties& new_properties,
                         const VertexVectorProperties& old_vector_properties,
                         const VertexVectorProperties& new_vector_properties,
                         const std::unordered_set<uint32_t>& touched_pids);

}  // namespace graphdb
