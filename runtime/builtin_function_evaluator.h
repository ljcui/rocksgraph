#pragma once

#include <vector>

#include "ast/builtin_function.h"
#include "runtime/execution_context.h"
#include "value/value.h"

namespace rg {

class AccessPath;

[[nodiscard]] Value EvaluateBuiltinFunction(
    ast::BuiltinFunctionKind kind, const std::vector<Value> &arguments,
    ExecutionClock clock = ExecutionClock::Start(),
    const AccessPath *access_path = nullptr);

}  // namespace rg
