#include "runtime/builtin_function_evaluator.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ast/builtin_function.h"
#include "common/exception.h"

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

}  // namespace

Value EvaluateBuiltinFunction(ast::BuiltinFunctionKind kind,
                              const std::vector<Value> &arguments) {
  const ast::BuiltinFunction *builtin = ast::FindBuiltinFunction(kind);
  CHECK(builtin != nullptr, common::InternalError,
        "unknown built-in function kind");
  CHECK(!builtin->aggregate, common::InvalidArgumentError,
        "aggregate function requires aggregation execution: " + builtin->name);
  CHECK(ast::BuiltinFunctionAcceptsArgumentCount(*builtin, arguments.size()),
        common::InvalidArgumentError,
        ast::BuiltinFunctionArgumentCountError(*builtin));
  switch (builtin->kind) {
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
    case ast::BuiltinFunctionKind::kCoalesce:
      for (const Value &argument : arguments) {
        if (!argument.IsNull()) {
          return argument;
        }
      }
      return Value::Null();
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
