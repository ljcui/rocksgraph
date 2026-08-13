#pragma once

#include <chrono>

#include "value/value.h"

namespace rg {

using QueryParameters = Value::Map;

struct ExecutionContext {
  const QueryParameters *parameters = nullptr;
  std::chrono::system_clock::time_point query_time =
      std::chrono::system_clock::now();

  [[nodiscard]] const Value *FindParameter(const std::string &name) const {
    if (parameters == nullptr) {
      return nullptr;
    }
    const auto found = parameters->find(name);
    return found == parameters->end() ? nullptr : &found->second;
  }
};

}  // namespace rg
