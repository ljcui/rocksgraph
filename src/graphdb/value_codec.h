#pragma once

#include <string>
#include <string_view>

#include "value/value.h"

namespace graphdb {

[[nodiscard]] std::string SerializeValue(const rg::Value& value);
[[nodiscard]] rg::Value DeserializeValue(std::string_view data);

}  // namespace graphdb
