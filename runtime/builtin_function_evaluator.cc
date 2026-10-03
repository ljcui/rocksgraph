#include "runtime/builtin_function_evaluator.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/builtin_function.h"
#include "common/exception.h"
#include "runtime/graphdb_access.h"
#include "value/temporal.h"

namespace runtime {
namespace {

std::string LowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string TrimAscii(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !is_space(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !is_space(ch); })
                  .base(),
              value.end());
  return value;
}

std::optional<std::int64_t> ParseInteger(std::string_view text) {
  std::int64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error == std::errc{} && end == text.data() + text.size()) {
    return value;
  }
  return std::nullopt;
}

std::optional<double> ParseDouble(std::string_view text) {
  double value = 0.0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error == std::errc{} && end == text.data() + text.size()) {
    return value;
  }
  return std::nullopt;
}

bool AddWouldOverflow(std::int64_t left, std::int64_t right) {
  return (right > 0 &&
          left > std::numeric_limits<std::int64_t>::max() - right) ||
         (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right);
}

std::size_t Utf8CodePointSize(std::string_view text, std::size_t offset) {
  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  std::size_t size = 1;
  if ((lead & 0xE0U) == 0xC0U) {
    size = 2;
  } else if ((lead & 0xF0U) == 0xE0U) {
    size = 3;
  } else if ((lead & 0xF8U) == 0xF0U) {
    size = 4;
  }
  if (offset + size > text.size()) {
    return 1;
  }
  for (std::size_t index = 1; index < size; ++index) {
    if ((static_cast<unsigned char>(text[offset + index]) & 0xC0U) != 0x80U) {
      return 1;
    }
  }
  return size;
}

std::vector<std::size_t> Utf8Offsets(std::string_view text) {
  std::vector<std::size_t> offsets;
  for (std::size_t offset = 0; offset < text.size();) {
    offsets.push_back(offset);
    offset += Utf8CodePointSize(text, offset);
  }
  offsets.push_back(text.size());
  return offsets;
}

double RandomUnitDouble() {
  thread_local std::mt19937_64 generator(std::random_device{}());
  thread_local std::uniform_real_distribution<double> distribution(0.0, 1.0);
  return distribution(generator);
}

}  // namespace

rg::Value EvaluateBuiltinFunction(ast::BuiltinFunctionKind kind,
                                  const std::vector<rg::Value> &arguments,
                                  ExecutionClock clock,
                                  graphdb::Transaction *transaction) {
  const ast::BuiltinFunction *builtin = ast::FindBuiltinFunction(kind);
  RG_CHECK(builtin != nullptr, common::ErrorCode::InternalError,
           "unknown built-in function kind");
  RG_CHECK(
      !builtin->aggregate, common::ErrorCode::InvalidParameter,
      "aggregate function requires aggregation execution: " + builtin->name);
  RG_CHECK(ast::BuiltinFunctionAcceptsArgumentCount(*builtin, arguments.size()),
           common::ErrorCode::InvalidParameter,
           ast::BuiltinFunctionArgumentCountError(*builtin));
  switch (builtin->kind) {
    case ast::BuiltinFunctionKind::kAbs:
      if (arguments[0].IsInteger()) {
        const std::int64_t value = arguments[0].AsInteger();
        RG_CHECK(value != std::numeric_limits<std::int64_t>::min(),
                 common::ErrorCode::InvalidParameter, "abs() integer overflow");
        return rg::Value(value < 0 ? -value : value);
      }
      if (arguments[0].IsDouble()) {
        return rg::Value(std::fabs(arguments[0].AsDouble()));
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kCeil:
      if (arguments[0].IsInteger()) {
        return rg::Value(static_cast<double>(arguments[0].AsInteger()));
      }
      return arguments[0].IsDouble()
                 ? rg::Value(std::ceil(arguments[0].AsDouble()))
                 : rg::Value::Null();
    case ast::BuiltinFunctionKind::kFloor:
      if (arguments[0].IsInteger()) {
        return rg::Value(static_cast<double>(arguments[0].AsInteger()));
      }
      return arguments[0].IsDouble()
                 ? rg::Value(std::floor(arguments[0].AsDouble()))
                 : rg::Value::Null();
    case ast::BuiltinFunctionKind::kId:
      if (arguments[0].IsNode()) {
        return rg::Value(arguments[0].AsNode().id);
      }
      if (arguments[0].IsRelationship()) {
        return rg::Value(arguments[0].AsRelationship().id);
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kLabels: {
      if (arguments[0].IsNull()) {
        return rg::Value::Null();
      }
      RG_CHECK(arguments[0].IsNode(), common::ErrorCode::InvalidParameter,
               "labels() argument must be a node");
      rg::Value::List labels;
      for (const auto &label : arguments[0].AsNode().labels) {
        labels.emplace_back(label);
      }
      return rg::Value(std::move(labels));
    }
    case ast::BuiltinFunctionKind::kLast:
      if (!arguments[0].IsList() || arguments[0].AsList().empty()) {
        return rg::Value::Null();
      }
      return arguments[0].AsList().back();
    case ast::BuiltinFunctionKind::kType:
      if (arguments[0].IsNull()) {
        return rg::Value::Null();
      }
      RG_CHECK(arguments[0].IsRelationship(),
               common::ErrorCode::InvalidParameter,
               "type() argument must be a relationship");
      return rg::Value(arguments[0].AsRelationship().type);
    case ast::BuiltinFunctionKind::kSize:
      if (arguments[0].IsList()) {
        return rg::Value(
            static_cast<std::int64_t>(arguments[0].AsList().size()));
      }
      if (arguments[0].IsString()) {
        return rg::Value(
            static_cast<std::int64_t>(arguments[0].AsString().size()));
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kLength:
      return arguments[0].IsPath()
                 ? rg::Value(static_cast<std::int64_t>(
                       arguments[0].AsPath().relationships.size()))
                 : rg::Value::Null();
    case ast::BuiltinFunctionKind::kLocalDateTime:
      return arguments.empty()
                 ? rg::temporal::ConstructLocalDateTime(clock.statement_time)
                 : rg::temporal::ConstructLocalDateTime(arguments[0],
                                                        clock.statement_time);
    case ast::BuiltinFunctionKind::kLocalDateTimeRealtime:
      return arguments.empty()
                 ? rg::temporal::CurrentLocalDateTime(clock.Realtime())
                 : rg::temporal::CurrentLocalDateTime(arguments[0],
                                                      clock.Realtime());
    case ast::BuiltinFunctionKind::kLocalDateTimeStatement:
      return arguments.empty()
                 ? rg::temporal::CurrentLocalDateTime(clock.statement_time)
                 : rg::temporal::CurrentLocalDateTime(arguments[0],
                                                      clock.statement_time);
    case ast::BuiltinFunctionKind::kLocalDateTimeTransaction:
      return arguments.empty()
                 ? rg::temporal::CurrentLocalDateTime(clock.transaction_time)
                 : rg::temporal::CurrentLocalDateTime(arguments[0],
                                                      clock.transaction_time);
    case ast::BuiltinFunctionKind::kLocalDateTimeTruncate:
      return arguments.size() == 3
                 ? rg::temporal::TruncateLocalDateTime(
                       arguments[0], arguments[1], arguments[2])
                 : rg::temporal::TruncateLocalDateTime(arguments[0],
                                                       arguments[1]);
    case ast::BuiltinFunctionKind::kLocalTime:
      return arguments.empty()
                 ? rg::temporal::ConstructLocalTime(clock.statement_time)
                 : rg::temporal::ConstructLocalTime(arguments[0],
                                                    clock.statement_time);
    case ast::BuiltinFunctionKind::kLocalTimeRealtime:
      return arguments.empty()
                 ? rg::temporal::CurrentLocalTime(clock.Realtime())
                 : rg::temporal::CurrentLocalTime(arguments[0],
                                                  clock.Realtime());
    case ast::BuiltinFunctionKind::kLocalTimeStatement:
      return arguments.empty()
                 ? rg::temporal::CurrentLocalTime(clock.statement_time)
                 : rg::temporal::CurrentLocalTime(arguments[0],
                                                  clock.statement_time);
    case ast::BuiltinFunctionKind::kLocalTimeTransaction:
      return arguments.empty()
                 ? rg::temporal::CurrentLocalTime(clock.transaction_time)
                 : rg::temporal::CurrentLocalTime(arguments[0],
                                                  clock.transaction_time);
    case ast::BuiltinFunctionKind::kLocalTimeTruncate:
      return arguments.size() == 3
                 ? rg::temporal::TruncateLocalTime(arguments[0], arguments[1],
                                                   arguments[2])
                 : rg::temporal::TruncateLocalTime(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kCoalesce:
      for (const rg::Value &argument : arguments) {
        if (!argument.IsNull()) {
          return argument;
        }
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kDate:
      return arguments.empty()
                 ? rg::temporal::ConstructDate(clock.statement_time)
                 : rg::temporal::ConstructDate(arguments[0],
                                               clock.statement_time);
    case ast::BuiltinFunctionKind::kDateRealtime:
      return arguments.empty()
                 ? rg::temporal::CurrentDate(clock.Realtime())
                 : rg::temporal::CurrentDate(arguments[0], clock.Realtime());
    case ast::BuiltinFunctionKind::kDateStatement:
      return arguments.empty() ? rg::temporal::CurrentDate(clock.statement_time)
                               : rg::temporal::CurrentDate(
                                     arguments[0], clock.statement_time);
    case ast::BuiltinFunctionKind::kDateTransaction:
      return arguments.empty()
                 ? rg::temporal::CurrentDate(clock.transaction_time)
                 : rg::temporal::CurrentDate(arguments[0],
                                             clock.transaction_time);
    case ast::BuiltinFunctionKind::kDateTruncate:
      return arguments.size() == 3
                 ? rg::temporal::TruncateDate(arguments[0], arguments[1],
                                              arguments[2])
                 : rg::temporal::TruncateDate(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDateTime:
      return arguments.empty()
                 ? rg::temporal::ConstructDateTime(clock.statement_time)
                 : rg::temporal::ConstructDateTime(arguments[0],
                                                   clock.statement_time);
    case ast::BuiltinFunctionKind::kDateTimeFromEpoch:
      return rg::temporal::ConstructDateTimeFromEpoch(arguments[0],
                                                      arguments[1]);
    case ast::BuiltinFunctionKind::kDateTimeFromEpochMillis:
      return rg::temporal::ConstructDateTimeFromEpochMillis(arguments[0]);
    case ast::BuiltinFunctionKind::kDateTimeRealtime:
      return arguments.empty() ? rg::temporal::CurrentDateTime(clock.Realtime())
                               : rg::temporal::CurrentDateTime(
                                     arguments[0], clock.Realtime());
    case ast::BuiltinFunctionKind::kDateTimeStatement:
      return arguments.empty()
                 ? rg::temporal::CurrentDateTime(clock.statement_time)
                 : rg::temporal::CurrentDateTime(arguments[0],
                                                 clock.statement_time);
    case ast::BuiltinFunctionKind::kDateTimeTransaction:
      return arguments.empty()
                 ? rg::temporal::CurrentDateTime(clock.transaction_time)
                 : rg::temporal::CurrentDateTime(arguments[0],
                                                 clock.transaction_time);
    case ast::BuiltinFunctionKind::kDateTimeTruncate:
      return arguments.size() == 3
                 ? rg::temporal::TruncateDateTime(arguments[0], arguments[1],
                                                  arguments[2])
                 : rg::temporal::TruncateDateTime(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDuration:
      return rg::temporal::ConstructDuration(arguments[0]);
    case ast::BuiltinFunctionKind::kDurationBetween:
      return rg::temporal::DurationBetween(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDurationInDays:
      return rg::temporal::DurationInDays(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDurationInMonths:
      return rg::temporal::DurationInMonths(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDurationInSeconds:
      return rg::temporal::DurationInSeconds(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kEndNode:
    case ast::BuiltinFunctionKind::kStartNode: {
      if (!arguments[0].IsRelationship()) {
        return rg::Value::Null();
      }
      const rg::Relationship &relationship = arguments[0].AsRelationship();
      const std::int64_t node_id =
          builtin->kind == ast::BuiltinFunctionKind::kStartNode
              ? relationship.start_node_id
              : relationship.end_node_id;
      if (transaction != nullptr) {
        return rg::Value(MaterializeGraphDBVertex(*transaction, node_id));
      }
      auto node = std::make_shared<rg::Node>();
      node->id = node_id;
      return rg::Value(std::move(node));
    }
    case ast::BuiltinFunctionKind::kHead:
      if (!arguments[0].IsList() || arguments[0].AsList().empty()) {
        return rg::Value::Null();
      }
      return arguments[0].AsList().front();
    case ast::BuiltinFunctionKind::kIsEmpty:
      if (arguments[0].IsString()) {
        return rg::Value(arguments[0].AsString().empty());
      }
      if (arguments[0].IsList()) {
        return rg::Value(arguments[0].AsList().empty());
      }
      if (arguments[0].IsMap()) {
        return rg::Value(arguments[0].AsMap().empty());
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kKeys: {
      const rg::Value::Map *properties = nullptr;
      if (arguments[0].IsMap()) {
        properties = &arguments[0].AsMap();
      } else if (arguments[0].IsNode()) {
        properties = &arguments[0].AsNode().properties;
      } else if (arguments[0].IsRelationship()) {
        properties = &arguments[0].AsRelationship().properties;
      }
      if (properties == nullptr) {
        return rg::Value::Null();
      }
      rg::Value::List keys;
      keys.reserve(properties->size());
      for (const auto &[key, value] : *properties) {
        (void)value;
        keys.emplace_back(key);
      }
      return rg::Value(std::move(keys));
    }
    case ast::BuiltinFunctionKind::kProperties:
      if (arguments[0].IsMap()) {
        return rg::Value(arguments[0].AsMap());
      }
      if (arguments[0].IsNode()) {
        return rg::Value(arguments[0].AsNode().properties);
      }
      if (arguments[0].IsRelationship()) {
        return rg::Value(arguments[0].AsRelationship().properties);
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kRand:
      return rg::Value(RandomUnitDouble());
    case ast::BuiltinFunctionKind::kRange: {
      if (std::any_of(
              arguments.begin(), arguments.end(),
              [](const rg::Value &argument) { return argument.IsNull(); })) {
        return rg::Value::Null();
      }
      RG_CHECK(arguments[0].IsInteger() && arguments[1].IsInteger() &&
                   (arguments.size() != 3 || arguments[2].IsInteger()),
               common::ErrorCode::InvalidParameter,
               "range() arguments must be integers");
      const std::int64_t start = arguments[0].AsInteger();
      const std::int64_t end = arguments[1].AsInteger();
      const std::int64_t step =
          arguments.size() == 3 ? arguments[2].AsInteger() : 1;
      RG_CHECK(step != 0, common::ErrorCode::InvalidParameter,
               "range() step is zero");
      rg::Value::List values;
      for (std::int64_t value = start;
           step > 0 ? value <= end : value >= end;) {
        values.emplace_back(value);
        if (AddWouldOverflow(value, step)) {
          break;
        }
        const std::int64_t next = value + step;
        if (step > 0 ? next > end : next < end) {
          break;
        }
        value = next;
      }
      return rg::Value(std::move(values));
    }
    case ast::BuiltinFunctionKind::kSplit: {
      if (!arguments[0].IsString() || !arguments[1].IsString()) {
        return rg::Value::Null();
      }
      const std::string &input = arguments[0].AsString();
      const std::string &delimiter = arguments[1].AsString();
      rg::Value::List parts;
      if (delimiter.empty()) {
        for (char ch : input) {
          parts.emplace_back(std::string(1, ch));
        }
        return rg::Value(std::move(parts));
      }
      std::size_t start = 0;
      while (true) {
        const std::size_t found = input.find(delimiter, start);
        if (found == std::string::npos) {
          parts.emplace_back(input.substr(start));
          break;
        }
        parts.emplace_back(input.substr(start, found - start));
        start = found + delimiter.size();
      }
      return rg::Value(std::move(parts));
    }
    case ast::BuiltinFunctionKind::kSqrt:
      if (arguments[0].IsInteger()) {
        return rg::Value(
            std::sqrt(static_cast<double>(arguments[0].AsInteger())));
      }
      return arguments[0].IsDouble()
                 ? rg::Value(std::sqrt(arguments[0].AsDouble()))
                 : rg::Value::Null();
    case ast::BuiltinFunctionKind::kSubstring: {
      if (!arguments[0].IsString() || !arguments[1].IsInteger() ||
          (arguments.size() == 3 && !arguments[2].IsInteger())) {
        return rg::Value::Null();
      }
      const std::int64_t start = arguments[1].AsInteger();
      const std::int64_t length =
          arguments.size() == 3 ? arguments[2].AsInteger() : -1;
      RG_CHECK(start >= 0, common::ErrorCode::InvalidParameter,
               "substring() start is negative");
      RG_CHECK(length >= 0 || arguments.size() == 2,
               common::ErrorCode::InvalidParameter,
               "substring() length is negative");

      const std::string &input = arguments[0].AsString();
      const std::vector<std::size_t> offsets = Utf8Offsets(input);
      const std::size_t code_points = offsets.size() - 1;
      if (static_cast<std::uint64_t>(start) >= code_points) {
        return rg::Value("");
      }
      const std::size_t first = static_cast<std::size_t>(start);
      std::size_t last = code_points;
      if (arguments.size() == 3) {
        const std::uint64_t requested_last = static_cast<std::uint64_t>(start) +
                                             static_cast<std::uint64_t>(length);
        last = static_cast<std::size_t>(
            std::min<std::uint64_t>(requested_last, code_points));
      }
      return rg::Value(
          input.substr(offsets[first], offsets[last] - offsets[first]));
    }
    case ast::BuiltinFunctionKind::kTail:
      if (!arguments[0].IsList()) {
        return rg::Value::Null();
      }
      if (arguments[0].AsList().empty()) {
        return rg::Value(rg::Value::List{});
      }
      return rg::Value(rg::Value::List(arguments[0].AsList().begin() + 1,
                                       arguments[0].AsList().end()));
    case ast::BuiltinFunctionKind::kTime:
      return arguments.empty()
                 ? rg::temporal::ConstructTime(clock.statement_time)
                 : rg::temporal::ConstructTime(arguments[0],
                                               clock.statement_time);
    case ast::BuiltinFunctionKind::kTimeRealtime:
      return arguments.empty()
                 ? rg::temporal::CurrentTime(clock.Realtime())
                 : rg::temporal::CurrentTime(arguments[0], clock.Realtime());
    case ast::BuiltinFunctionKind::kTimeStatement:
      return arguments.empty() ? rg::temporal::CurrentTime(clock.statement_time)
                               : rg::temporal::CurrentTime(
                                     arguments[0], clock.statement_time);
    case ast::BuiltinFunctionKind::kTimeTransaction:
      return arguments.empty()
                 ? rg::temporal::CurrentTime(clock.transaction_time)
                 : rg::temporal::CurrentTime(arguments[0],
                                             clock.transaction_time);
    case ast::BuiltinFunctionKind::kTimeTruncate:
      return arguments.size() == 3
                 ? rg::temporal::TruncateTime(arguments[0], arguments[1],
                                              arguments[2])
                 : rg::temporal::TruncateTime(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kNodes:
    case ast::BuiltinFunctionKind::kRelationships: {
      if (!arguments[0].IsPath()) {
        return rg::Value::Null();
      }
      rg::Value::List values;
      if (builtin->kind == ast::BuiltinFunctionKind::kNodes) {
        for (const auto &node : arguments[0].AsPath().nodes) {
          values.emplace_back(node);
        }
      } else {
        for (const auto &relationship : arguments[0].AsPath().relationships) {
          values.emplace_back(relationship);
        }
      }
      return rg::Value(std::move(values));
    }
    case ast::BuiltinFunctionKind::kReverse:
      if (arguments[0].IsList()) {
        rg::Value::List values = arguments[0].AsList();
        std::reverse(values.begin(), values.end());
        return rg::Value(std::move(values));
      }
      if (arguments[0].IsString()) {
        const std::string &input = arguments[0].AsString();
        const std::vector<std::size_t> offsets = Utf8Offsets(input);
        std::string reversed;
        reversed.reserve(input.size());
        for (std::size_t index = offsets.size() - 1; index > 0; --index) {
          reversed.append(input, offsets[index - 1],
                          offsets[index] - offsets[index - 1]);
        }
        return rg::Value(std::move(reversed));
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kSign:
      if (arguments[0].IsInteger()) {
        const std::int64_t value = arguments[0].AsInteger();
        return rg::Value(value > 0 ? 1 : value < 0 ? -1 : 0);
      }
      if (arguments[0].IsDouble()) {
        const double value = arguments[0].AsDouble();
        return rg::Value(value > 0.0 ? 1 : value < 0.0 ? -1 : 0);
      }
      return rg::Value::Null();
    case ast::BuiltinFunctionKind::kToString:
      if (arguments[0].IsNull()) {
        return rg::Value::Null();
      }
      RG_CHECK(!arguments[0].IsList() && !arguments[0].IsMap() &&
                   !arguments[0].IsNode() && !arguments[0].IsRelationship() &&
                   !arguments[0].IsPath(),
               common::ErrorCode::InvalidParameter,
               "toString() argument has an invalid type");
      return arguments[0].IsString() ? arguments[0]
                                     : rg::Value(arguments[0].ToString());
    case ast::BuiltinFunctionKind::kToInteger:
      if (arguments[0].IsNull() || arguments[0].IsInteger()) {
        return arguments[0];
      }
      if (arguments[0].IsDouble()) {
        const double value = arguments[0].AsDouble();
        if (!std::isfinite(value) ||
            value <
                static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            value >=
                static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
          return rg::Value::Null();
        }
        return rg::Value(static_cast<std::int64_t>(value));
      }
      if (arguments[0].IsBool()) {
        return rg::Value(arguments[0].AsBool() ? 1 : 0);
      }
      if (arguments[0].IsString()) {
        const std::string &text = arguments[0].AsString();
        if (const auto value = ParseInteger(text); value.has_value()) {
          return rg::Value(*value);
        }
        if (const auto value = ParseDouble(text);
            value.has_value() && std::isfinite(*value) &&
            *value >=
                static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            *value <
                static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
          return rg::Value(static_cast<std::int64_t>(*value));
        }
        return rg::Value::Null();
      }
      RG_THROW(common::ErrorCode::InvalidParameter,
               "toInteger() argument has an invalid type");
    case ast::BuiltinFunctionKind::kToFloat:
      if (arguments[0].IsNull() || arguments[0].IsDouble()) {
        return arguments[0];
      }
      if (arguments[0].IsInteger()) {
        return rg::Value(static_cast<double>(arguments[0].AsInteger()));
      }
      if (arguments[0].IsString()) {
        const auto value = ParseDouble(arguments[0].AsString());
        return value.has_value() ? rg::Value(*value) : rg::Value::Null();
      }
      RG_THROW(common::ErrorCode::InvalidParameter,
               "toFloat() argument has an invalid type");
    case ast::BuiltinFunctionKind::kToBoolean:
      if (arguments[0].IsNull() || arguments[0].IsBool()) {
        return arguments[0];
      }
      if (arguments[0].IsString()) {
        const std::string value =
            LowerAscii(TrimAscii(arguments[0].AsString()));
        if (value == "true") {
          return rg::Value(true);
        }
        if (value == "false") {
          return rg::Value(false);
        }
        return rg::Value::Null();
      }
      RG_THROW(common::ErrorCode::InvalidParameter,
               "toBoolean() argument has an invalid type");
    case ast::BuiltinFunctionKind::kToLower:
    case ast::BuiltinFunctionKind::kToUpper:
    case ast::BuiltinFunctionKind::kTrim:
      if (!arguments[0].IsString()) {
        return rg::Value::Null();
      }
      {
        std::string value = arguments[0].AsString();
        if (builtin->kind == ast::BuiltinFunctionKind::kToLower) {
          value = LowerAscii(std::move(value));
        } else if (builtin->kind == ast::BuiltinFunctionKind::kToUpper) {
          std::transform(value.begin(), value.end(), value.begin(),
                         [](unsigned char ch) {
                           return static_cast<char>(std::toupper(ch));
                         });
        } else {
          value = TrimAscii(std::move(value));
        }
        return rg::Value(std::move(value));
      }
    case ast::BuiltinFunctionKind::kAverage:
    case ast::BuiltinFunctionKind::kCollect:
    case ast::BuiltinFunctionKind::kCount:
    case ast::BuiltinFunctionKind::kMaximum:
    case ast::BuiltinFunctionKind::kMinimum:
    case ast::BuiltinFunctionKind::kPercentileContinuous:
    case ast::BuiltinFunctionKind::kPercentileDiscrete:
    case ast::BuiltinFunctionKind::kSum:
      RG_THROW(common::ErrorCode::InternalError,
               "aggregate function requires aggregation execution: " +
                   builtin->name);
  }
  RG_THROW(common::ErrorCode::InternalError,
           "built-in function has no scalar implementation: " + builtin->name);
}

rg::Value EvaluateBuiltinFunction(ast::BuiltinFunctionKind kind,
                                  const std::vector<rg::Value> &arguments,
                                  ExecutionContext context) {
  return EvaluateBuiltinFunction(kind, arguments, context.clock,
                                 context.transaction);
}

}  // namespace runtime
