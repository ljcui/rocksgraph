#include "runtime/builtin_function_evaluator.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/builtin_function.h"
#include "common/exception.h"
#include "value/temporal.h"

namespace rg {
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

Value EvaluateBuiltinFunction(ast::BuiltinFunctionKind kind,
                              const std::vector<Value> &arguments,
                              std::chrono::system_clock::time_point now) {
  const ast::BuiltinFunction *builtin = ast::FindBuiltinFunction(kind);
  CHECK(builtin != nullptr, common::InternalError,
        "unknown built-in function kind");
  CHECK(!builtin->aggregate, common::InvalidArgumentError,
        "aggregate function requires aggregation execution: " + builtin->name);
  CHECK(ast::BuiltinFunctionAcceptsArgumentCount(*builtin, arguments.size()),
        common::InvalidArgumentError,
        ast::BuiltinFunctionArgumentCountError(*builtin));
  switch (builtin->kind) {
    case ast::BuiltinFunctionKind::kAbs:
      if (arguments[0].IsInteger()) {
        const std::int64_t value = arguments[0].AsInteger();
        CHECK(value != std::numeric_limits<std::int64_t>::min(),
              common::InvalidArgumentError, "abs() integer overflow");
        return Value(value < 0 ? -value : value);
      }
      if (arguments[0].IsDouble()) {
        return Value(std::fabs(arguments[0].AsDouble()));
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kCeil:
      if (arguments[0].IsInteger()) {
        return Value(static_cast<double>(arguments[0].AsInteger()));
      }
      return arguments[0].IsDouble() ? Value(std::ceil(arguments[0].AsDouble()))
                                     : Value::Null();
    case ast::BuiltinFunctionKind::kId:
      if (arguments[0].IsNode()) {
        return Value(arguments[0].AsNode().id);
      }
      if (arguments[0].IsRelationship()) {
        return Value(arguments[0].AsRelationship().id);
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kLabels: {
      if (!arguments[0].IsNode()) {
        return Value::Null();
      }
      Value::List labels;
      for (const auto &label : arguments[0].AsNode().labels) {
        labels.emplace_back(label);
      }
      return Value(std::move(labels));
    }
    case ast::BuiltinFunctionKind::kLast:
      if (!arguments[0].IsList() || arguments[0].AsList().empty()) {
        return Value::Null();
      }
      return arguments[0].AsList().back();
    case ast::BuiltinFunctionKind::kType:
      return arguments[0].IsRelationship()
                 ? Value(arguments[0].AsRelationship().type)
                 : Value::Null();
    case ast::BuiltinFunctionKind::kSize:
      if (arguments[0].IsList()) {
        return Value(static_cast<std::int64_t>(arguments[0].AsList().size()));
      }
      if (arguments[0].IsString()) {
        return Value(static_cast<std::int64_t>(arguments[0].AsString().size()));
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kLength:
      return arguments[0].IsPath()
                 ? Value(static_cast<std::int64_t>(
                       arguments[0].AsPath().relationships.size()))
                 : Value::Null();
    case ast::BuiltinFunctionKind::kLocalDateTime:
      return ConstructLocalDateTime(arguments.empty() ? nullptr : &arguments[0],
                                    now);
    case ast::BuiltinFunctionKind::kLocalDateTimeTruncate:
      return TruncateLocalDateTime(
          arguments[0], arguments[1],
          arguments.size() == 3 ? &arguments[2] : nullptr);
    case ast::BuiltinFunctionKind::kLocalTime:
      return ConstructLocalTime(arguments.empty() ? nullptr : &arguments[0],
                                now);
    case ast::BuiltinFunctionKind::kLocalTimeTruncate:
      return TruncateLocalTime(arguments[0], arguments[1],
                               arguments.size() == 3 ? &arguments[2] : nullptr);
    case ast::BuiltinFunctionKind::kCoalesce:
      for (const Value &argument : arguments) {
        if (!argument.IsNull()) {
          return argument;
        }
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kDate:
      return ConstructDate(arguments.empty() ? nullptr : &arguments[0], now);
    case ast::BuiltinFunctionKind::kDateTruncate:
      return TruncateDate(arguments[0], arguments[1],
                          arguments.size() == 3 ? &arguments[2] : nullptr);
    case ast::BuiltinFunctionKind::kDateTime:
      return ConstructDateTime(arguments.empty() ? nullptr : &arguments[0],
                               now);
    case ast::BuiltinFunctionKind::kDateTimeTruncate:
      return TruncateDateTime(arguments[0], arguments[1],
                              arguments.size() == 3 ? &arguments[2] : nullptr);
    case ast::BuiltinFunctionKind::kDuration:
      return ConstructDuration(&arguments[0]);
    case ast::BuiltinFunctionKind::kDurationBetween:
      return DurationBetween(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDurationInDays:
      return DurationInDays(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDurationInMonths:
      return DurationInMonths(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kDurationInSeconds:
      return DurationInSeconds(arguments[0], arguments[1]);
    case ast::BuiltinFunctionKind::kIsEmpty:
      if (arguments[0].IsString()) {
        return Value(arguments[0].AsString().empty());
      }
      if (arguments[0].IsList()) {
        return Value(arguments[0].AsList().empty());
      }
      if (arguments[0].IsMap()) {
        return Value(arguments[0].AsMap().empty());
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kKeys: {
      const Value::Map *properties = nullptr;
      if (arguments[0].IsMap()) {
        properties = &arguments[0].AsMap();
      } else if (arguments[0].IsNode()) {
        properties = &arguments[0].AsNode().properties;
      } else if (arguments[0].IsRelationship()) {
        properties = &arguments[0].AsRelationship().properties;
      }
      if (properties == nullptr) {
        return Value::Null();
      }
      Value::List keys;
      keys.reserve(properties->size());
      for (const auto &[key, value] : *properties) {
        (void)value;
        keys.emplace_back(key);
      }
      return Value(std::move(keys));
    }
    case ast::BuiltinFunctionKind::kProperties:
      if (arguments[0].IsMap()) {
        return Value(arguments[0].AsMap());
      }
      if (arguments[0].IsNode()) {
        return Value(arguments[0].AsNode().properties);
      }
      if (arguments[0].IsRelationship()) {
        return Value(arguments[0].AsRelationship().properties);
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kRand:
      return Value(RandomUnitDouble());
    case ast::BuiltinFunctionKind::kRange: {
      if (!arguments[0].IsInteger() || !arguments[1].IsInteger() ||
          (arguments.size() == 3 && !arguments[2].IsInteger())) {
        return Value::Null();
      }
      const std::int64_t start = arguments[0].AsInteger();
      const std::int64_t end = arguments[1].AsInteger();
      const std::int64_t step =
          arguments.size() == 3 ? arguments[2].AsInteger() : 1;
      CHECK(step != 0, common::InvalidArgumentError, "range() step is zero");
      Value::List values;
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
      return Value(std::move(values));
    }
    case ast::BuiltinFunctionKind::kSplit: {
      if (!arguments[0].IsString() || !arguments[1].IsString()) {
        return Value::Null();
      }
      const std::string &input = arguments[0].AsString();
      const std::string &delimiter = arguments[1].AsString();
      Value::List parts;
      if (delimiter.empty()) {
        for (char ch : input) {
          parts.emplace_back(std::string(1, ch));
        }
        return Value(std::move(parts));
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
      return Value(std::move(parts));
    }
    case ast::BuiltinFunctionKind::kSqrt:
      if (arguments[0].IsInteger()) {
        return Value(std::sqrt(static_cast<double>(arguments[0].AsInteger())));
      }
      return arguments[0].IsDouble() ? Value(std::sqrt(arguments[0].AsDouble()))
                                     : Value::Null();
    case ast::BuiltinFunctionKind::kSubstring: {
      if (!arguments[0].IsString() || !arguments[1].IsInteger() ||
          (arguments.size() == 3 && !arguments[2].IsInteger())) {
        return Value::Null();
      }
      const std::int64_t start = arguments[1].AsInteger();
      const std::int64_t length =
          arguments.size() == 3 ? arguments[2].AsInteger() : -1;
      CHECK(start >= 0, common::InvalidArgumentError,
            "substring() start is negative");
      CHECK(length >= 0 || arguments.size() == 2, common::InvalidArgumentError,
            "substring() length is negative");

      const std::string &input = arguments[0].AsString();
      const std::vector<std::size_t> offsets = Utf8Offsets(input);
      const std::size_t code_points = offsets.size() - 1;
      if (static_cast<std::uint64_t>(start) >= code_points) {
        return Value("");
      }
      const std::size_t first = static_cast<std::size_t>(start);
      std::size_t last = code_points;
      if (arguments.size() == 3) {
        const std::uint64_t requested_last = static_cast<std::uint64_t>(start) +
                                             static_cast<std::uint64_t>(length);
        last = static_cast<std::size_t>(
            std::min<std::uint64_t>(requested_last, code_points));
      }
      return Value(
          input.substr(offsets[first], offsets[last] - offsets[first]));
    }
    case ast::BuiltinFunctionKind::kTail:
      if (!arguments[0].IsList()) {
        return Value::Null();
      }
      if (arguments[0].AsList().empty()) {
        return Value(Value::List{});
      }
      return Value(Value::List(arguments[0].AsList().begin() + 1,
                               arguments[0].AsList().end()));
    case ast::BuiltinFunctionKind::kTime:
      return ConstructTime(arguments.empty() ? nullptr : &arguments[0], now);
    case ast::BuiltinFunctionKind::kTimeTruncate:
      return TruncateTime(arguments[0], arguments[1],
                          arguments.size() == 3 ? &arguments[2] : nullptr);
    case ast::BuiltinFunctionKind::kNodes:
    case ast::BuiltinFunctionKind::kRelationships: {
      if (!arguments[0].IsPath()) {
        return Value::Null();
      }
      Value::List values;
      if (builtin->kind == ast::BuiltinFunctionKind::kNodes) {
        for (const auto &node : arguments[0].AsPath().nodes) {
          values.emplace_back(node);
        }
      } else {
        for (const auto &relationship : arguments[0].AsPath().relationships) {
          values.emplace_back(relationship);
        }
      }
      return Value(std::move(values));
    }
    case ast::BuiltinFunctionKind::kReverse:
      if (arguments[0].IsList()) {
        Value::List values = arguments[0].AsList();
        std::reverse(values.begin(), values.end());
        return Value(std::move(values));
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
        return Value(std::move(reversed));
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kSign:
      if (arguments[0].IsInteger()) {
        const std::int64_t value = arguments[0].AsInteger();
        return Value(value > 0 ? 1 : value < 0 ? -1 : 0);
      }
      if (arguments[0].IsDouble()) {
        const double value = arguments[0].AsDouble();
        return Value(value > 0.0 ? 1 : value < 0.0 ? -1 : 0);
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kToString:
      if (arguments[0].IsNull()) {
        return Value::Null();
      }
      return arguments[0].IsString() ? arguments[0]
                                     : Value(arguments[0].ToString());
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
          return Value::Null();
        }
        return Value(static_cast<std::int64_t>(value));
      }
      if (arguments[0].IsBool()) {
        return Value(arguments[0].AsBool() ? 1 : 0);
      }
      if (arguments[0].IsString()) {
        const auto value = ParseInteger(arguments[0].AsString());
        return value.has_value() ? Value(*value) : Value::Null();
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kToFloat:
      if (arguments[0].IsNull() || arguments[0].IsDouble()) {
        return arguments[0];
      }
      if (arguments[0].IsInteger()) {
        return Value(static_cast<double>(arguments[0].AsInteger()));
      }
      if (arguments[0].IsBool()) {
        return Value(arguments[0].AsBool() ? 1.0 : 0.0);
      }
      if (arguments[0].IsString()) {
        const auto value = ParseDouble(arguments[0].AsString());
        return value.has_value() ? Value(*value) : Value::Null();
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kToBoolean:
      if (arguments[0].IsNull() || arguments[0].IsBool()) {
        return arguments[0];
      }
      if (arguments[0].IsString()) {
        const std::string value =
            LowerAscii(TrimAscii(arguments[0].AsString()));
        if (value == "true") {
          return Value(true);
        }
        if (value == "false") {
          return Value(false);
        }
      }
      return Value::Null();
    case ast::BuiltinFunctionKind::kToLower:
    case ast::BuiltinFunctionKind::kToUpper:
    case ast::BuiltinFunctionKind::kTrim:
      if (!arguments[0].IsString()) {
        return Value::Null();
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
        return Value(std::move(value));
      }
    case ast::BuiltinFunctionKind::kAverage:
    case ast::BuiltinFunctionKind::kCollect:
    case ast::BuiltinFunctionKind::kCount:
    case ast::BuiltinFunctionKind::kMaximum:
    case ast::BuiltinFunctionKind::kMinimum:
    case ast::BuiltinFunctionKind::kSum:
      THROW(common::InternalError,
            "aggregate function requires aggregation execution: " +
                builtin->name);
  }
  THROW(common::InternalError,
        "built-in function has no scalar implementation: " + builtin->name);
}

}  // namespace rg
