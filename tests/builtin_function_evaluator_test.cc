#include "runtime/builtin_function_evaluator.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
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

TEST(BuiltinFunctionEvaluatorTest, EvaluatesNumericFunctions) {
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kAbs, {Value(-7)}),
            Value(7));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kAbs, {Value(-1.5)}),
            Value(1.5));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kCeil, {Value(1.2)}),
            Value(2.0));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kSqrt, {Value(12.96)}),
            Value(3.6));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kSign, {Value(-5)}),
            Value(-1));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kSign, {Value(0.0)}),
            Value(0));
}

TEST(BuiltinFunctionEvaluatorTest, EvaluatesListAndStringFunctions) {
  const Value list(Value::List{Value(1), Value(2), Value(3)});
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kTail, {list}),
            Value(Value::List{Value(2), Value(3)}));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kLast, {list}),
            Value(3));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kReverse, {list}),
            Value(Value::List{Value(3), Value(2), Value(1)}));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kLast,
                                    {Value(Value::List{})}),
            Value::Null());
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kReverse,
                                    {Value("A\xC3\xA9")}),
            Value("\xC3\xA9"
                  "A"));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kSubstring,
                                    {Value("A\xC3\xA9Z"), Value(1), Value(1)}),
            Value("\xC3\xA9"));
}

TEST(BuiltinFunctionEvaluatorTest, RandReturnsUnitIntervalValues) {
  for (int index = 0; index < 32; ++index) {
    const Value value = EvaluateBuiltinFunction(BuiltinFunctionKind::kRand, {});
    ASSERT_TRUE(value.IsDouble());
    EXPECT_GE(value.AsDouble(), 0.0);
    EXPECT_LT(value.AsDouble(), 1.0);
  }
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
  EXPECT_THROW((void)EvaluateBuiltinFunction(
                   BuiltinFunctionKind::kAbs,
                   {Value(std::numeric_limits<std::int64_t>::min())}),
               common::InvalidArgumentError);
  EXPECT_THROW((void)EvaluateBuiltinFunction(BuiltinFunctionKind::kSubstring,
                                             {Value("abc"), Value(-1)}),
               common::InvalidArgumentError);
}

}  // namespace
