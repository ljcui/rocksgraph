#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ast/precomputed_expression.h"
#include "runtime/execution_context.h"
#include "value/value.h"

namespace ast {

class Expression;
class Parameter;
class Variable;

}  // namespace ast

namespace runtime {

class ExpressionBindings {
 public:
  ExpressionBindings() = default;
  ExpressionBindings(const ExpressionBindings &) = delete;
  ExpressionBindings &operator=(const ExpressionBindings &) = delete;
  virtual ~ExpressionBindings() = default;

  [[nodiscard]] virtual rg::Value Lookup(std::string_view name) const;
  [[nodiscard]] virtual rg::Value LookupVariable(
      const ast::Variable &variable) const;
  [[nodiscard]] virtual bool ReadProperty(std::string_view variable,
                                          std::string_view property_key,
                                          rg::Value *value) const;
  [[nodiscard]] virtual bool ReadVariableProperty(const ast::Variable &variable,
                                                  std::string_view property_key,
                                                  rg::Value *value) const;
  [[nodiscard]] virtual bool ReadParameter(const ast::Parameter &parameter,
                                           rg::Value *value) const;
};

[[nodiscard]] rg::Value EvaluateExpression(
    const ast::Expression &expression, const ExpressionBindings &bindings,
    const std::vector<ast::PrecomputedExpression> &precomputed = {},
    ExecutionContext context = {});

[[nodiscard]] rg::Value EvaluateExpression(const ast::Expression &expression,
                                           ExecutionContext context = {});

[[nodiscard]] bool PredicateIsTrue(const rg::Value &value);
[[nodiscard]] bool IsNumeric(const rg::Value &value);
[[nodiscard]] double AsDoubleValue(const rg::Value &value);

}  // namespace runtime
