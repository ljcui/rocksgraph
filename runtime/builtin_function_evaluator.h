#pragma once

#include <vector>

#include "ast/builtin_function.h"
#include "runtime/execution_context.h"
#include "value/value.h"

namespace txn {
class Transaction;
}

namespace rg {

class GraphReader;

[[nodiscard]] Value EvaluateBuiltinFunction(
    ast::BuiltinFunctionKind kind, const std::vector<Value> &arguments,
    ExecutionClock clock = ExecutionClock::Start(),
    const GraphReader *graph_reader = nullptr,
    txn::Transaction *transaction = nullptr);

}  // namespace rg
