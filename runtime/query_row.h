#pragma once

#include <map>
#include <string>
#include <vector>

#include "value/value.h"

namespace rg {

using QueryRow = std::map<std::string, Value>;
using QueryRows = std::vector<QueryRow>;

}  // namespace rg
