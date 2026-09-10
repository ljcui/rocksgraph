#pragma once

#include <rocksdb/slice.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "value/value.h"

namespace graphdb {

uint64_t VectorFieldKey(uint32_t lid, uint32_t pid);
std::string VertexVectorPropertyKey(uint32_t lid, uint32_t pid, int64_t vid);
std::string VertexVectorPropertyPrefix(uint32_t lid, uint32_t pid);

bool TryParseVectorValue(const rg::Value& value, size_t dimensions,
                         std::vector<float>* out);
std::vector<float> ParseVectorValue(const rg::Value& value, size_t dimensions);
rg::Value VectorToValue(const std::vector<float>& vector);

std::string SerializeVector(const std::vector<float>& vector);
std::vector<float> DeserializeVector(rocksdb::Slice value, size_t dimensions);

}  // namespace graphdb
