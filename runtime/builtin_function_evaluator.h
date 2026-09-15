#pragma once

#include <vector>

#include "ast/builtin_function.h"
#include "runtime/execution_context.h"
#include "value/value.h"

namespace graphdb {
class Transaction;
}

namespace rg {

[[nodiscard]] Value EvaluateBuiltinFunction(
    ast::BuiltinFunctionKind kind, const std::vector<Value> &arguments,
    ExecutionClock clock = ExecutionClock::Start(),
    graphdb::Transaction *transaction = nullptr);

// Native query execution passes its complete context so GraphDB transaction
// access remains available to built-in functions.
[[nodiscard]] Value EvaluateBuiltinFunction(ast::BuiltinFunctionKind kind,
                                            const std::vector<Value> &arguments,
                                            ExecutionContext context);

}  // namespace rg
