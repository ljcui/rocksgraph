#pragma once

#include "value/value.h"

namespace rg {

using QueryParameters = Value::Map;

struct ExecutionContext {
  const QueryParameters *parameters = nullptr;

  [[nodiscard]] const Value *FindParameter(const std::string &name) const {
    if (parameters == nullptr) {
      return nullptr;
    }
    const auto found = parameters->find(name);
    return found == parameters->end() ? nullptr : &found->second;
  }
};

}  // namespace rg
