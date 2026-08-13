#pragma once

#include <chrono>
#include <vector>

#include "ast/builtin_function.h"
#include "value/value.h"

namespace rg {

[[nodiscard]] Value EvaluateBuiltinFunction(
    ast::BuiltinFunctionKind kind, const std::vector<Value> &arguments,
    std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now());

}  // namespace rg
