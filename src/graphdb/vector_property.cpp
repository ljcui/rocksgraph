#include "graphdb/vector_property.h"

#include "common/byte_utils.h"
#include "common/exceptions.h"

using common::AsChars;
using common::ReadValue;

namespace graphdb {

uint64_t VectorFieldKey(uint32_t lid, uint32_t pid) {
  return (static_cast<uint64_t>(lid) << 32) | static_cast<uint64_t>(pid);
}

std::string VertexVectorPropertyKey(uint32_t lid, uint32_t pid, int64_t vid) {
  std::string key;
  key.reserve(sizeof(lid) + sizeof(pid) + sizeof(vid));
  key.append(AsChars(lid), sizeof(lid));
  key.append(AsChars(pid), sizeof(pid));
  key.append(AsChars(vid), sizeof(vid));
  return key;
}

std::string VertexVectorPropertyPrefix(uint32_t lid, uint32_t pid) {
  std::string prefix;
  prefix.reserve(sizeof(lid) + sizeof(pid));
  prefix.append(AsChars(lid), sizeof(lid));
  prefix.append(AsChars(pid), sizeof(pid));
  return prefix;
}

bool TryParseVectorValue(const Value& value, size_t dimensions,
                         std::vector<float>* out) {
  if (!value.IsArray()) {
    return false;
  }
  const auto& array = value.AsArray();
  if (array.size() != dimensions) {
    return false;
  }
  out->clear();
  out->reserve(array.size());
  for (const auto& item : array) {
    if (item.IsFloat()) {
      out->push_back(item.AsFloat());
    } else if (item.IsDouble()) {
      out->push_back(static_cast<float>(item.AsDouble()));
    } else if (item.IsInteger()) {
      out->push_back(static_cast<float>(item.AsInteger()));
    } else {
      out->clear();
      return false;
    }
  }
  return true;
}

std::vector<float> ParseVectorValue(const Value& value, size_t dimensions) {
  std::vector<float> vector;
  if (!TryParseVectorValue(value, dimensions, &vector)) {
    THROW_CODE(InvalidParameter,
               "vector field value should be a numeric array with dimension {}",
               dimensions);
  }
  return vector;
}

std::string SerializeVector(const std::vector<float>& vector) {
  std::string value;
  value.reserve(vector.size() * sizeof(float));
  for (float item : vector) {
    value.append(AsChars(item), sizeof(item));
  }
  return value;
}

std::vector<float> DeserializeVector(rocksdb::Slice value, size_t dimensions) {
  if (value.size() != dimensions * sizeof(float)) {
    THROW_CODE(StorageEngineError,
               "vector field value has invalid size, expect {}, actual {}",
               dimensions * sizeof(float), value.size());
  }
  std::vector<float> vector;
  vector.reserve(dimensions);
  const char* data = value.data();
  for (size_t i = 0; i < dimensions; ++i) {
    vector.push_back(ReadValue<float>(data + i * sizeof(float)));
  }
  return vector;
}

}  // namespace graphdb
