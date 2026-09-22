#pragma once

#include <cstddef>
#include <vector>

#include "value/value.h"

namespace rg::execution {

struct CompositeValueKey {
  std::vector<Value> values;

  bool operator==(const CompositeValueKey &other) const noexcept;
};

struct CompositeValueKeyHash {
  [[nodiscard]] std::size_t operator()(
      const CompositeValueKey &key) const noexcept;
};

struct CompositeValueKeyEqual {
  [[nodiscard]] bool operator()(const CompositeValueKey &left,
                                const CompositeValueKey &right) const noexcept;
};

}  // namespace rg::execution
