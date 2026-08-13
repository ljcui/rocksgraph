#pragma once

#include <string>
#include <vector>

#include "ir/logical_plan.h"
#include "runtime/execution_context.h"
#include "runtime/query_row.h"
#include "value/value.h"

namespace ast {

class Expression;

}  // namespace ast

namespace rg {

[[nodiscard]] Value EvaluateExpression(
    const ast::Expression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed = {},
    ExecutionContext context = {});
[[nodiscard]] Value EvaluateLogicalProjectionItem(
    const ir::LogicalProjectionItem &item, const QueryRow &row,
    ExecutionContext context = {});
[[nodiscard]] Value EvaluateLogicalSortItem(const ir::LogicalSortItem &item,
                                            const QueryRow &row,
                                            ExecutionContext context = {});

[[nodiscard]] bool PredicateIsTrue(const Value &value);
[[nodiscard]] bool IsNumeric(const Value &value);
[[nodiscard]] double AsDoubleValue(const Value &value);
[[nodiscard]] bool ValueLess(const Value &left, const Value &right);

}  // namespace rg
