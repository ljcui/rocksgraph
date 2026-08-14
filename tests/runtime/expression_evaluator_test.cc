#include "runtime/expression_evaluator.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "ast/ast_node.h"
#include "common/exception.h"

namespace {

std::unique_ptr<ast::StringLiteral> String(std::string value) {
  auto literal = std::make_unique<ast::StringLiteral>();
  literal->value = std::move(value);
  return literal;
}

ast::FunctionInvocation Function(std::string name) {
  ast::FunctionInvocation function;
  function.function_name = std::move(name);
  return function;
}

}  // namespace

TEST(ExpressionEvaluatorTest, ExecutesRegisteredFunctionCaseInsensitively) {
  ast::FunctionInvocation function = Function("ToLoWeR");
  function.arguments.push_back(String("ADA"));

  EXPECT_EQ(rg::EvaluateExpression(function, {}), rg::Value("ada"));
}

TEST(ExpressionEvaluatorTest, RejectsInvalidFunctionContracts) {
  ast::FunctionInvocation unknown = Function("unknown");
  unknown.arguments.push_back(String("value"));
  EXPECT_THROW((void)rg::EvaluateExpression(unknown, {}),
               common::InvalidArgumentError);

  ast::FunctionInvocation invalid_arity = Function("size");
  EXPECT_THROW((void)rg::EvaluateExpression(invalid_arity, {}),
               common::InvalidArgumentError);

  ast::FunctionInvocation aggregate = Function("count");
  aggregate.arguments.push_back(String("value"));
  EXPECT_THROW((void)rg::EvaluateExpression(aggregate, {}),
               common::InvalidArgumentError);

  ast::FunctionInvocation distinct = Function("size");
  distinct.arguments.push_back(String("value"));
  distinct.distinct = true;
  EXPECT_THROW((void)rg::EvaluateExpression(distinct, {}),
               common::InvalidArgumentError);
}
