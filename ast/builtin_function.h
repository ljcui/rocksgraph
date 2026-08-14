#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast/semantic_table.h"

namespace ast {

enum class BuiltinFunctionKind {
  kAverage,
  kCollect,
  kCount,
  kMaximum,
  kMinimum,
  kPercentileContinuous,
  kPercentileDiscrete,
  kSum,
  kAbs,
  kCeil,
  kCoalesce,
  kDate,
  kDateRealtime,
  kDateStatement,
  kDateTransaction,
  kDateTruncate,
  kDateTime,
  kDateTimeFromEpoch,
  kDateTimeFromEpochMillis,
  kDateTimeRealtime,
  kDateTimeStatement,
  kDateTimeTransaction,
  kDateTimeTruncate,
  kDuration,
  kDurationBetween,
  kDurationInDays,
  kDurationInMonths,
  kDurationInSeconds,
  kEndNode,
  kHead,
  kId,
  kIsEmpty,
  kKeys,
  kLabels,
  kLast,
  kLength,
  kLocalDateTime,
  kLocalDateTimeRealtime,
  kLocalDateTimeStatement,
  kLocalDateTimeTransaction,
  kLocalDateTimeTruncate,
  kLocalTime,
  kLocalTimeRealtime,
  kLocalTimeStatement,
  kLocalTimeTransaction,
  kLocalTimeTruncate,
  kNodes,
  kProperties,
  kRand,
  kRange,
  kRelationships,
  kReverse,
  kSign,
  kSize,
  kSplit,
  kSqrt,
  kStartNode,
  kSubstring,
  kTail,
  kTime,
  kTimeRealtime,
  kTimeStatement,
  kTimeTransaction,
  kTimeTruncate,
  kToBoolean,
  kToFloat,
  kToInteger,
  kToLower,
  kToString,
  kToUpper,
  kTrim,
  kType,
};

struct BuiltinFunction {
  BuiltinFunctionKind kind = BuiltinFunctionKind::kCoalesce;
  std::string name;
  SemanticVariableType result_type = SemanticVariableType::kScalar;
  std::optional<SemanticVariableType> list_element_type;
  std::size_t minimum_argument_count = 0;
  std::optional<std::size_t> maximum_argument_count;
  bool aggregate = false;
  bool allows_distinct = false;
  bool deterministic = true;
};

[[nodiscard]] const std::vector<BuiltinFunction> &BuiltinFunctions();
[[nodiscard]] const BuiltinFunction *FindBuiltinFunction(std::string_view name);
[[nodiscard]] const BuiltinFunction *FindBuiltinFunction(
    BuiltinFunctionKind kind);
[[nodiscard]] bool BuiltinFunctionAcceptsArgumentCount(
    const BuiltinFunction &function, std::size_t argument_count);
[[nodiscard]] std::string BuiltinFunctionArgumentCountError(
    const BuiltinFunction &function);

}  // namespace ast
