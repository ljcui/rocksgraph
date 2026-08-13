#pragma once

#include <vector>

#include "ast/builtin_function.h"
#include "value/value.h"

namespace rg {

[[nodiscard]] Value EvaluateBuiltinFunction(
    ast::BuiltinFunctionKind kind, const std::vector<Value> &arguments);

}  // namespace rg
