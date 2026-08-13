#include "runtime/builtin_function_evaluator.h"

#include <gtest/gtest.h>

#include <vector>

#include "common/exception.h"

namespace {

using ast::BuiltinFunctionKind;
using rg::EvaluateBuiltinFunction;
using rg::Value;

TEST(BuiltinFunctionEvaluatorTest, EvaluatesScalarFunctionFamilies) {
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kCoalesce,
                                    {Value::Null(), Value("fallback")}),
            Value("fallback"));
  EXPECT_EQ(
      EvaluateBuiltinFunction(BuiltinFunctionKind::kToLower, {Value("ADA")}),
      Value("ada"));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kRange,
                                    {Value(5), Value(1), Value(-2)}),
            Value(Value::List{Value(5), Value(3), Value(1)}));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kSplit,
                                    {Value("a,b"), Value(",")}),
            Value(Value::List{Value("a"), Value("b")}));
}

TEST(BuiltinFunctionEvaluatorTest, ReturnsNullForUnsupportedArgumentTypes) {
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kSize, {Value(1)}),
            Value::Null());
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kToInteger,
                                    {Value("not an integer")}),
            Value::Null());
}

TEST(BuiltinFunctionEvaluatorTest, EnforcesFunctionContracts) {
  EXPECT_THROW((void)EvaluateBuiltinFunction(BuiltinFunctionKind::kSize, {}),
               common::InvalidArgumentError);
  EXPECT_THROW((void)EvaluateBuiltinFunction(BuiltinFunctionKind::kRange,
                                             {Value(1), Value(3), Value(0)}),
               common::InvalidArgumentError);
  EXPECT_THROW(
      (void)EvaluateBuiltinFunction(BuiltinFunctionKind::kCount, {Value(1)}),
      common::InvalidArgumentError);
}

}  // namespace
