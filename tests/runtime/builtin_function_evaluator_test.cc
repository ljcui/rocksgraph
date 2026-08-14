#include "runtime/builtin_function_evaluator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "common/exception.h"

namespace {

using ast::BuiltinFunctionKind;
using rg::EvaluateBuiltinFunction;
using rg::ExecutionClock;
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
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kHead, {list}),
            Value(1));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kTail, {list}),
            Value(Value::List{Value(2), Value(3)}));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kLast, {list}),
            Value(3));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kReverse, {list}),
            Value(Value::List{Value(3), Value(2), Value(1)}));
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kLast,
                                    {Value(Value::List{})}),
            Value::Null());
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kHead,
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

TEST(BuiltinFunctionEvaluatorTest, EvaluatesRelationshipEndpointFunctions) {
  auto relationship = std::make_shared<rg::Relationship>();
  relationship->start_node_id = 7;
  relationship->end_node_id = 11;
  const Value value(relationship);

  const Value start =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kStartNode, {value});
  const Value end =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kEndNode, {value});
  ASSERT_TRUE(start.IsNode());
  ASSERT_TRUE(end.IsNode());
  EXPECT_EQ(start.AsNode().id, 7);
  EXPECT_EQ(end.AsNode().id, 11);
  EXPECT_EQ(
      EvaluateBuiltinFunction(BuiltinFunctionKind::kStartNode, {Value::Null()}),
      Value::Null());
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

TEST(BuiltinFunctionEvaluatorTest, ConstructsTemporalValuesFromStrings) {
  const Value date = EvaluateBuiltinFunction(BuiltinFunctionKind::kDate,
                                             {Value("2015-W30-2")});
  const Value local_time = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kLocalTime, {Value("214032.142")});
  const Value time = EvaluateBuiltinFunction(BuiltinFunctionKind::kTime,
                                             {Value("21:40-01:30")});
  const Value local_date_time = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kLocalDateTime, {Value("2015-202T21:40:32")});
  const Value date_time = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kDateTime,
      {Value("2015-07-21T21:40:32.142[Europe/London]")});
  const Value duration = EvaluateBuiltinFunction(BuiltinFunctionKind::kDuration,
                                                 {Value("P0.75M")});

  EXPECT_EQ(date.ToString(), "2015-07-21");
  EXPECT_EQ(local_time.ToString(), "21:40:32.142");
  EXPECT_EQ(time.ToString(), "21:40-01:30");
  EXPECT_EQ(local_date_time.ToString(), "2015-07-21T21:40:32");
  EXPECT_EQ(date_time.ToString(),
            "2015-07-21T21:40:32.142+01:00[Europe/London]");
  EXPECT_EQ(duration.ToString(), "P22DT19H51M49.5S");
}

TEST(BuiltinFunctionEvaluatorTest, ConstructsTemporalValuesFromMaps) {
  const Value date =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kDate,
                              {Value(Value::Map{{"year", Value(1984)},
                                                {"quarter", Value(3)},
                                                {"dayOfQuarter", Value(45)}})});
  const Value time = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kTime,
      {Value(Value::Map{{"hour", Value(12)},
                        {"minute", Value(31)},
                        {"second", Value(14)},
                        {"millisecond", Value(123)},
                        {"microsecond", Value(456)},
                        {"nanosecond", Value(789)},
                        {"timezone", Value("+01:00")}})});
  const Value date_time = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kDateTime,
      {Value(Value::Map{{"year", Value(2017)},
                        {"month", Value(8)},
                        {"day", Value(8)},
                        {"hour", Value(12)},
                        {"timezone", Value("Europe/Stockholm")}})});

  EXPECT_EQ(date.ToString(), "1984-08-14");
  EXPECT_EQ(time.ToString(), "12:31:14.123456789+01:00");
  EXPECT_EQ(date_time.ToString(), "2017-08-08T12:00+02:00[Europe/Stockholm]");
}

TEST(BuiltinFunctionEvaluatorTest, RejectsInvalidTemporalValues) {
  EXPECT_THROW((void)EvaluateBuiltinFunction(BuiltinFunctionKind::kDate,
                                             {Value("2023-02-29")}),
               common::InvalidArgumentError);
  EXPECT_THROW((void)EvaluateBuiltinFunction(BuiltinFunctionKind::kTime,
                                             {Value("12:00+18:01")}),
               common::InvalidArgumentError);
}

TEST(BuiltinFunctionEvaluatorTest, ComputesTemporalDifferences) {
  const Value left = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kLocalDateTime, {Value("2018-01-02T10:00:00.1")});
  const Value right = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kLocalDateTime, {Value("2018-01-01T10:00:00.2")});

  const Value between = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kDurationBetween, {left, right});
  const Value seconds = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kDurationInSeconds, {left, right});

  EXPECT_EQ(between.ToString(), "PT-23H-59M-59.9S");
  EXPECT_EQ(between.AsDuration().seconds, -86'400);
  EXPECT_EQ(between.AsDuration().nanoseconds, 100'000'000);
  EXPECT_EQ(seconds.ToString(), "PT-23H-59M-59.9S");
}

TEST(BuiltinFunctionEvaluatorTest, UsesOneTimestampForCurrentTemporals) {
  const auto now =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::May / 6} +
      std::chrono::hours{7} + std::chrono::minutes{8} + std::chrono::seconds{9};
  ExecutionClock clock;
  clock.transaction_time = now;
  clock.statement_time = now;
  clock.realtime_now = [now] { return now; };

  const Value date =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kDate, {}, clock);
  const Value time =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kTime, {}, clock);
  const Value date_time =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kDateTime, {}, clock);

  EXPECT_EQ(date.ToString(), "2024-05-06");
  EXPECT_EQ(time.ToString(), "07:08:09Z");
  EXPECT_EQ(date_time.ToString(), "2024-05-06T07:08:09Z");
}

TEST(BuiltinFunctionEvaluatorTest, EvaluatesTemporalClockFunctions) {
  const auto transaction =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::May / 6} +
      std::chrono::hours{7} + std::chrono::minutes{8} + std::chrono::seconds{9};
  const auto statement = std::chrono::sys_days{std::chrono::year{2025} /
                                               std::chrono::January / 2} +
                         std::chrono::hours{3} + std::chrono::minutes{4} +
                         std::chrono::seconds{5};
  const auto realtime = std::chrono::sys_days{std::chrono::year{2026} /
                                              std::chrono::February / 3} +
                        std::chrono::hours{11} + std::chrono::minutes{12} +
                        std::chrono::seconds{13};
  ExecutionClock clock;
  clock.transaction_time = transaction;
  clock.statement_time = statement;
  clock.realtime_now = [realtime] { return realtime; };

  EXPECT_EQ(
      EvaluateBuiltinFunction(BuiltinFunctionKind::kDateTransaction, {}, clock)
          .ToString(),
      "2024-05-06");
  EXPECT_EQ(EvaluateBuiltinFunction(
                BuiltinFunctionKind::kLocalDateTimeStatement, {}, clock)
                .ToString(),
            "2025-01-02T03:04:05");
  EXPECT_EQ(
      EvaluateBuiltinFunction(BuiltinFunctionKind::kDateTimeRealtime, {}, clock)
          .ToString(),
      "2026-02-03T11:12:13Z");
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kTimeTransaction,
                                    {Value("+01:00")}, clock)
                .ToString(),
            "08:08:09+01:00");
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kLocalTimeStatement,
                                    {Value("-02:00")}, clock)
                .ToString(),
            "01:04:05");

  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kDateRealtime,
                                    {Value::Null()}, clock),
            Value::Null());
}

TEST(BuiltinFunctionEvaluatorTest, ConstructsDateTimeFromEpoch) {
  EXPECT_EQ(EvaluateBuiltinFunction(BuiltinFunctionKind::kDateTimeFromEpoch,
                                    {Value(416779), Value(999'999'999)})
                .ToString(),
            "1970-01-05T19:46:19.999999999Z");
  EXPECT_EQ(
      EvaluateBuiltinFunction(BuiltinFunctionKind::kDateTimeFromEpochMillis,
                              {Value(237'821'673'987)})
          .ToString(),
      "1977-07-15T13:34:33.987Z");
  EXPECT_EQ(EvaluateBuiltinFunction(
                BuiltinFunctionKind::kDateTimeFromEpochMillis, {Value(-1)})
                .ToString(),
            "1969-12-31T23:59:59.999Z");
  EXPECT_THROW(
      (void)EvaluateBuiltinFunction(BuiltinFunctionKind::kDateTimeFromEpoch,
                                    {Value(1), Value(1'000'000'000)}),
      common::InvalidArgumentError);
}

TEST(BuiltinFunctionEvaluatorTest, TruncatesTemporalValues) {
  const Value date = EvaluateBuiltinFunction(BuiltinFunctionKind::kDate,
                                             {Value("1984-10-11")});
  const Value date_time =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kDateTime,
                              {Value("1984-10-11T12:31:14.645876123-01:00")});

  const Value week = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kDateTruncate,
      {Value("week"), date, Value(Value::Map{{"dayOfWeek", Value(2)}})});
  const Value milliseconds =
      EvaluateBuiltinFunction(BuiltinFunctionKind::kLocalDateTimeTruncate,
                              {Value("millisecond"), date_time,
                               Value(Value::Map{{"nanosecond", Value(2)}})});
  const Value zoned = EvaluateBuiltinFunction(
      BuiltinFunctionKind::kDateTimeTruncate,
      {Value("hour"), date_time,
       Value(Value::Map{{"timezone", Value("Europe/Stockholm")}})});

  EXPECT_EQ(week.ToString(), "1984-10-09");
  EXPECT_EQ(milliseconds.ToString(), "1984-10-11T12:31:14.645000002");
  EXPECT_EQ(zoned.ToString(), "1984-10-11T12:00+01:00[Europe/Stockholm]");
  EXPECT_THROW((void)EvaluateBuiltinFunction(BuiltinFunctionKind::kTimeTruncate,
                                             {Value("year"), date_time}),
               common::InvalidArgumentError);
}

}  // namespace
