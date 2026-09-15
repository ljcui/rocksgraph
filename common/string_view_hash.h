#pragma once

#include <functional>
#include <string_view>

namespace common {

struct StringViewHash {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};

}  // namespace common
