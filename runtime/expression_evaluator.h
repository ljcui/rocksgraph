#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ir/logical_plan.h"
#include "runtime/execution_context.h"
#include "runtime/query_row.h"
#include "value/value.h"

namespace ast {

class Expression;
class Parameter;
class Variable;

}  // namespace ast

namespace rg {

class ExpressionBindings {
 public:
  ExpressionBindings() = default;
  ExpressionBindings(const ExpressionBindings &) = delete;
  ExpressionBindings &operator=(const ExpressionBindings &) = delete;
  virtual ~ExpressionBindings() = default;

  [[nodiscard]] virtual Value Lookup(std::string_view name) const = 0;
  [[nodiscard]] virtual Value LookupVariable(
      const ast::Variable &variable) const;
  [[nodiscard]] virtual bool ReadProperty(std::string_view variable,
                                          std::string_view property_key,
                                          Value *value) const;
  [[nodiscard]] virtual bool ReadVariableProperty(const ast::Variable &variable,
                                                  std::string_view property_key,
                                                  Value *value) const;
  [[nodiscard]] virtual bool ReadParameter(const ast::Parameter &parameter,
                                           Value *value) const;
};

[[nodiscard]] Value EvaluateExpression(
    const ast::Expression &expression, const ExpressionBindings &bindings,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed = {},
    ExecutionContext context = {});

[[nodiscard]] Value EvaluateExpression(
    const ast::Expression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed = {},
    ExecutionContext context = {});
[[nodiscard]] Value EvaluateLogicalProjectionItem(
    const ir::LogicalProjectionItem &item, const ExpressionBindings &bindings,
    ExecutionContext context = {});
[[nodiscard]] Value EvaluateLogicalProjectionItem(
    const ir::LogicalProjectionItem &item, const QueryRow &row,
    ExecutionContext context = {});
[[nodiscard]] Value EvaluateLogicalSortItem(const ir::LogicalSortItem &item,
                                            const ExpressionBindings &bindings,
                                            ExecutionContext context = {});
[[nodiscard]] Value EvaluateLogicalSortItem(const ir::LogicalSortItem &item,
                                            const QueryRow &row,
                                            ExecutionContext context = {});

[[nodiscard]] bool PredicateIsTrue(const Value &value);
[[nodiscard]] bool IsNumeric(const Value &value);
[[nodiscard]] double AsDoubleValue(const Value &value);
[[nodiscard]] bool ValueLess(const Value &left, const Value &right);

}  // namespace rg
