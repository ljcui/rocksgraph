#include "runtime/expression_evaluator.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "common/exception.h"
#include "runtime/query_row_util.h"

namespace rg {
namespace {

enum class TruthValue { kFalse, kTrue, kNull };
enum class QuantifierMode { kAll, kAny, kNone, kSingle };

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

TruthValue ToTruthValue(const Value &value) {
  if (value.IsNull()) {
    return TruthValue::kNull;
  }
  CHECK(value.IsBool(), common::InvalidArgumentError,
        "predicate requires a boolean value");
  return value.AsBool() ? TruthValue::kTrue : TruthValue::kFalse;
}

Value FromTruthValue(TruthValue value) {
  switch (value) {
    case TruthValue::kFalse:
      return Value(false);
    case TruthValue::kTrue:
      return Value(true);
    case TruthValue::kNull:
      return Value::Null();
  }
  THROW(common::InternalError, "unknown truth value");
}

TruthValue And(TruthValue left, TruthValue right) {
  if (left == TruthValue::kFalse || right == TruthValue::kFalse) {
    return TruthValue::kFalse;
  }
  if (left == TruthValue::kNull || right == TruthValue::kNull) {
    return TruthValue::kNull;
  }
  return TruthValue::kTrue;
}

TruthValue Or(TruthValue left, TruthValue right) {
  if (left == TruthValue::kTrue || right == TruthValue::kTrue) {
    return TruthValue::kTrue;
  }
  if (left == TruthValue::kNull || right == TruthValue::kNull) {
    return TruthValue::kNull;
  }
  return TruthValue::kFalse;
}

TruthValue Not(TruthValue value) {
  if (value == TruthValue::kNull) {
    return TruthValue::kNull;
  }
  return value == TruthValue::kTrue ? TruthValue::kFalse : TruthValue::kTrue;
}

TruthValue EqualityTruth(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) {
    return TruthValue::kNull;
  }
  if (left.IsList() && right.IsList()) {
    if (left.AsList().size() != right.AsList().size()) {
      return TruthValue::kFalse;
    }
    bool saw_null = false;
    for (std::size_t index = 0; index < left.AsList().size(); ++index) {
      const TruthValue item =
          EqualityTruth(left.AsList()[index], right.AsList()[index]);
      if (item == TruthValue::kFalse) {
        return TruthValue::kFalse;
      }
      saw_null = saw_null || item == TruthValue::kNull;
    }
    return saw_null ? TruthValue::kNull : TruthValue::kTrue;
  }
  if (left.IsMap() && right.IsMap()) {
    if (left.AsMap().size() != right.AsMap().size()) {
      return TruthValue::kFalse;
    }
    bool saw_null = false;
    for (const auto &[key, value] : left.AsMap()) {
      const auto found = right.AsMap().find(key);
      if (found == right.AsMap().end()) {
        return TruthValue::kFalse;
      }
      const TruthValue item = EqualityTruth(value, found->second);
      if (item == TruthValue::kFalse) {
        return TruthValue::kFalse;
      }
      saw_null = saw_null || item == TruthValue::kNull;
    }
    return saw_null ? TruthValue::kNull : TruthValue::kTrue;
  }
  return ValuesEqual(left, right) ? TruthValue::kTrue : TruthValue::kFalse;
}

const Value *LookupPrecomputedExpression(
    const ast::Expression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  for (const auto &entry : precomputed) {
    if (entry.expression == nullptr ||
        !ast::ASTEqual::Equal(&expression, entry.expression)) {
      continue;
    }
    const auto found = row.find(entry.variable);
    CHECK(found != row.end(), common::InvalidArgumentError,
          "precomputed expression variable is not bound: " + entry.variable);
    return &found->second;
  }
  return nullptr;
}

const Value *FindProperty(const Value &value, std::string_view property_key) {
  const auto find_in_map =
      [property_key](const Value::Map &properties) -> const Value * {
    const auto found = properties.find(std::string(property_key));
    return found != properties.end() ? &found->second : nullptr;
  };
  if (value.IsMap()) {
    return find_in_map(value.AsMap());
  }
  if (value.IsNode()) {
    return find_in_map(value.AsNode().properties);
  }
  if (value.IsRelationship()) {
    return find_in_map(value.AsRelationship().properties);
  }
  return nullptr;
}

bool NodeHasLabels(const Node &node, const std::vector<std::string> &labels) {
  for (const auto &label : labels) {
    if (std::find(node.labels.begin(), node.labels.end(), label) ==
        node.labels.end()) {
      return false;
    }
  }
  return true;
}

bool RelationshipHasAnyType(const Relationship &relationship,
                            const std::vector<std::string> &types) {
  return types.empty() || std::find(types.begin(), types.end(),
                                    relationship.type) != types.end();
}

std::optional<std::int64_t> IntegerValue(const Value &value) {
  if (!value.IsInteger()) {
    return std::nullopt;
  }
  return value.AsInteger();
}

std::int64_t NormalizeListIndex(std::int64_t index, std::size_t size) {
  if (index < 0) {
    index += static_cast<std::int64_t>(size);
  }
  return index;
}

std::int64_t ClampListSliceIndex(std::int64_t index, std::size_t size) {
  index = NormalizeListIndex(index, size);
  if (index < 0) {
    return 0;
  }
  const auto list_size = static_cast<std::int64_t>(size);
  return index > list_size ? list_size : index;
}

bool AddWouldOverflow(std::int64_t left, std::int64_t right) {
  return (right > 0 &&
          left > std::numeric_limits<std::int64_t>::max() - right) ||
         (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right);
}

bool SubtractWouldOverflow(std::int64_t left, std::int64_t right) {
  return (right < 0 &&
          left > std::numeric_limits<std::int64_t>::max() + right) ||
         (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right);
}

bool MultiplyWouldOverflow(std::int64_t left, std::int64_t right) {
  if (left == 0 || right == 0) {
    return false;
  }
  if ((left == -1 && right == std::numeric_limits<std::int64_t>::min()) ||
      (right == -1 && left == std::numeric_limits<std::int64_t>::min())) {
    return true;
  }
  if (left > 0) {
    return right > 0 ? left > std::numeric_limits<std::int64_t>::max() / right
                     : right < std::numeric_limits<std::int64_t>::min() / left;
  }
  return right > 0 ? left < std::numeric_limits<std::int64_t>::min() / right
                   : left < std::numeric_limits<std::int64_t>::max() / right;
}

Value EvaluateFunction(
    const ast::FunctionInvocation &function, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  const std::string name = LowerAscii(function.function_name);
  std::vector<Value> arguments;
  arguments.reserve(function.arguments.size());
  for (const auto &argument : function.arguments) {
    CHECK(argument != nullptr, common::InvalidArgumentError,
          "function argument is null");
    arguments.push_back(EvaluateExpression(*argument, row, precomputed));
  }

  if (name == "id") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "id() expects one argument");
    if (arguments[0].IsNode()) {
      return Value(arguments[0].AsNode().id);
    }
    if (arguments[0].IsRelationship()) {
      return Value(arguments[0].AsRelationship().id);
    }
    return Value::Null();
  }
  if (name == "labels") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "labels() expects one argument");
    if (!arguments[0].IsNode()) {
      return Value::Null();
    }
    Value::List labels;
    for (const auto &label : arguments[0].AsNode().labels) {
      labels.emplace_back(label);
    }
    return Value(std::move(labels));
  }
  if (name == "type") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "type() expects one argument");
    return arguments[0].IsRelationship()
               ? Value(arguments[0].AsRelationship().type)
               : Value::Null();
  }
  if (name == "size") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "size() expects one argument");
    if (arguments[0].IsList()) {
      return Value(static_cast<std::int64_t>(arguments[0].AsList().size()));
    }
    if (arguments[0].IsString()) {
      return Value(static_cast<std::int64_t>(arguments[0].AsString().size()));
    }
    return Value::Null();
  }
  if (name == "length") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "length() expects one argument");
    return arguments[0].IsPath()
               ? Value(static_cast<std::int64_t>(
                     arguments[0].AsPath().relationships.size()))
               : Value::Null();
  }
  if (name == "coalesce") {
    for (const Value &argument : arguments) {
      if (!argument.IsNull()) {
        return argument;
      }
    }
    return Value::Null();
  }
  if (name == "isempty") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "isEmpty() expects one argument");
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
  }
  if (name == "keys") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "keys() expects one argument");
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
  if (name == "properties") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "properties() expects one argument");
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
  }
  if (name == "range") {
    CHECK(arguments.size() == 2 || arguments.size() == 3,
          common::InvalidArgumentError,
          "range() expects two or three arguments");
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
    for (std::int64_t value = start; step > 0 ? value <= end : value >= end;) {
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
  if (name == "split") {
    CHECK(arguments.size() == 2, common::InvalidArgumentError,
          "split() expects two arguments");
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
  if (name == "nodes" || name == "relationships") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          name + "() expects one argument");
    if (!arguments[0].IsPath()) {
      return Value::Null();
    }
    Value::List values;
    if (name == "nodes") {
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
  if (name == "tostring") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toString() expects one argument");
    if (arguments[0].IsNull()) {
      return Value::Null();
    }
    return arguments[0].IsString() ? arguments[0]
                                   : Value(arguments[0].ToString());
  }
  if (name == "tointeger") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toInteger() expects one argument");
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
  }
  if (name == "tofloat") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toFloat() expects one argument");
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
  }
  if (name == "toboolean") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toBoolean() expects one argument");
    if (arguments[0].IsNull() || arguments[0].IsBool()) {
      return arguments[0];
    }
    if (arguments[0].IsString()) {
      const std::string value = LowerAscii(TrimAscii(arguments[0].AsString()));
      if (value == "true") {
        return Value(true);
      }
      if (value == "false") {
        return Value(false);
      }
    }
    return Value::Null();
  }
  if (name == "tolower" || name == "toupper" || name == "trim") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          function.function_name + "() expects one argument");
    if (!arguments[0].IsString()) {
      return Value::Null();
    }
    std::string value = arguments[0].AsString();
    if (name == "tolower") {
      value = LowerAscii(std::move(value));
    } else if (name == "toupper") {
      std::transform(
          value.begin(), value.end(), value.begin(),
          [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    } else {
      value = TrimAscii(std::move(value));
    }
    return Value(std::move(value));
  }

  THROW(common::InvalidArgumentError,
        "unsupported function in executor: " + function.function_name);
}

Value EvaluateListIndex(
    const ast::ListIndexExpression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(expression.list != nullptr && expression.index != nullptr,
        common::InvalidArgumentError, "list index expression is incomplete");
  Value list = EvaluateExpression(*expression.list, row, precomputed);
  Value index_value = EvaluateExpression(*expression.index, row, precomputed);
  const auto index = IntegerValue(index_value);
  if (!list.IsList() || !index.has_value()) {
    return Value::Null();
  }
  const auto &items = list.AsList();
  const std::int64_t normalized = NormalizeListIndex(*index, items.size());
  if (normalized < 0 || normalized >= static_cast<std::int64_t>(items.size())) {
    return Value::Null();
  }
  return items[static_cast<std::size_t>(normalized)];
}

Value EvaluateListSlice(
    const ast::ListSliceExpression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(expression.list != nullptr, common::InvalidArgumentError,
        "list slice base expression is null");
  Value list = EvaluateExpression(*expression.list, row, precomputed);
  if (!list.IsList()) {
    return Value::Null();
  }
  const auto &items = list.AsList();
  std::int64_t start = 0;
  auto end = static_cast<std::int64_t>(items.size());
  if (expression.start_index != nullptr) {
    const auto value = IntegerValue(
        EvaluateExpression(*expression.start_index, row, precomputed));
    if (!value.has_value()) {
      return Value::Null();
    }
    start = ClampListSliceIndex(*value, items.size());
  }
  if (expression.end_index != nullptr) {
    const auto value = IntegerValue(
        EvaluateExpression(*expression.end_index, row, precomputed));
    if (!value.has_value()) {
      return Value::Null();
    }
    end = ClampListSliceIndex(*value, items.size());
  }
  end = std::max(end, start);
  return Value(Value::List(items.begin() + start, items.begin() + end));
}

Value EvaluateCaseExpression(
    const ast::CaseExpression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  std::optional<Value> test;
  if (expression.test != nullptr) {
    test = EvaluateExpression(*expression.test, row, precomputed);
  }
  for (const auto &[when_expression, then_expression] :
       expression.alternatives) {
    CHECK(when_expression != nullptr && then_expression != nullptr,
          common::InvalidArgumentError, "CASE alternative is incomplete");
    bool matched = false;
    if (test.has_value()) {
      const Value candidate =
          EvaluateExpression(*when_expression, row, precomputed);
      matched = EqualityTruth(*test, candidate) == TruthValue::kTrue;
    } else {
      matched = PredicateIsTrue(
          EvaluateExpression(*when_expression, row, precomputed));
    }
    if (matched) {
      return EvaluateExpression(*then_expression, row, precomputed);
    }
  }
  return expression.else_expr != nullptr
             ? EvaluateExpression(*expression.else_expr, row, precomputed)
             : Value::Null();
}

Value EvaluateListComprehension(
    const ast::ListComprehension &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(!expression.variable.empty() && expression.list_expr != nullptr,
        common::InvalidArgumentError, "list comprehension is incomplete");
  Value list = EvaluateExpression(*expression.list_expr, row, precomputed);
  if (!list.IsList()) {
    return Value::Null();
  }
  Value::List output;
  for (const auto &item : list.AsList()) {
    QueryRow scoped = row;
    scoped[expression.variable] = item;
    if (expression.where_expr != nullptr &&
        !PredicateIsTrue(
            EvaluateExpression(*expression.where_expr, scoped, precomputed))) {
      continue;
    }
    output.push_back(
        expression.eval_expr != nullptr
            ? EvaluateExpression(*expression.eval_expr, scoped, precomputed)
            : item);
  }
  return Value(std::move(output));
}

Value EvaluateQuantifier(
    const ast::Quantifier &quantifier, QuantifierMode mode, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(!quantifier.variable.empty() && quantifier.list_expr != nullptr &&
            quantifier.predicate != nullptr,
        common::InvalidArgumentError, "quantifier is incomplete");
  Value list = EvaluateExpression(*quantifier.list_expr, row, precomputed);
  if (list.IsNull()) {
    return Value::Null();
  }
  CHECK(list.IsList(), common::InvalidArgumentError,
        "quantifier requires a list value");
  std::size_t matches = 0;
  bool saw_null = false;
  for (const auto &item : list.AsList()) {
    QueryRow scoped = row;
    scoped[quantifier.variable] = item;
    const TruthValue truth = ToTruthValue(
        EvaluateExpression(*quantifier.predicate, scoped, precomputed));
    saw_null = saw_null || truth == TruthValue::kNull;
    if (mode == QuantifierMode::kAll && truth == TruthValue::kFalse) {
      return Value(false);
    }
    if (mode == QuantifierMode::kAny && truth == TruthValue::kTrue) {
      return Value(true);
    }
    if (mode == QuantifierMode::kNone && truth == TruthValue::kTrue) {
      return Value(false);
    }
    if (mode == QuantifierMode::kSingle && truth == TruthValue::kTrue &&
        ++matches > 1) {
      return Value(false);
    }
  }
  if (saw_null) {
    return Value::Null();
  }
  switch (mode) {
    case QuantifierMode::kAll:
    case QuantifierMode::kNone:
      return Value(true);
    case QuantifierMode::kAny:
      return Value(false);
    case QuantifierMode::kSingle:
      return Value(matches == 1);
  }
  THROW(common::InternalError, "unknown quantifier mode");
}

Value EvaluateArithmetic(
    const ast::Expression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
  CHECK(binary.left != nullptr && binary.right != nullptr,
        common::InvalidArgumentError, "arithmetic expression is incomplete");
  Value left = EvaluateExpression(*binary.left, row, precomputed);
  Value right = EvaluateExpression(*binary.right, row, precomputed);
  if (left.IsNull() || right.IsNull()) {
    return Value::Null();
  }
  if (expression.Is(ast::ASTNodeType::kAddExpression) && left.IsString() &&
      right.IsString()) {
    return Value(left.AsString() + right.AsString());
  }
  CHECK(IsNumeric(left) && IsNumeric(right), common::InvalidArgumentError,
        "arithmetic expression requires numeric values");
  if (left.IsInteger() && right.IsInteger() &&
      !expression.Is(ast::ASTNodeType::kDivideExpression) &&
      !expression.Is(ast::ASTNodeType::kPowerExpression)) {
    const auto lhs = left.AsInteger();
    const auto rhs = right.AsInteger();
    if (expression.Is(ast::ASTNodeType::kAddExpression)) {
      CHECK(!AddWouldOverflow(lhs, rhs), common::InvalidArgumentError,
            "integer addition overflow");
      return Value(lhs + rhs);
    }
    if (expression.Is(ast::ASTNodeType::kSubtractExpression)) {
      CHECK(!SubtractWouldOverflow(lhs, rhs), common::InvalidArgumentError,
            "integer subtraction overflow");
      return Value(lhs - rhs);
    }
    if (expression.Is(ast::ASTNodeType::kMultiplyExpression)) {
      CHECK(!MultiplyWouldOverflow(lhs, rhs), common::InvalidArgumentError,
            "integer multiplication overflow");
      return Value(lhs * rhs);
    }
    CHECK(rhs != 0, common::InvalidArgumentError, "modulo by zero");
    CHECK(!(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1),
          common::InvalidArgumentError, "integer modulo overflow");
    return Value(lhs % rhs);
  }
  const double lhs = AsDoubleValue(left);
  const double rhs = AsDoubleValue(right);
  if (expression.Is(ast::ASTNodeType::kDivideExpression)) {
    CHECK(rhs != 0.0, common::InvalidArgumentError, "division by zero");
    return Value(lhs / rhs);
  }
  if (expression.Is(ast::ASTNodeType::kPowerExpression)) {
    return Value(std::pow(lhs, rhs));
  }
  if (expression.Is(ast::ASTNodeType::kAddExpression)) {
    return Value(lhs + rhs);
  }
  if (expression.Is(ast::ASTNodeType::kSubtractExpression)) {
    return Value(lhs - rhs);
  }
  if (expression.Is(ast::ASTNodeType::kMultiplyExpression)) {
    return Value(lhs * rhs);
  }
  CHECK(false, common::InvalidArgumentError,
        "modulo requires integer operands");
}

}  // namespace

bool PredicateIsTrue(const Value &value) {
  return ToTruthValue(value) == TruthValue::kTrue;
}

bool IsNumeric(const Value &value) {
  return value.IsInteger() || value.IsDouble();
}

double AsDoubleValue(const Value &value) {
  if (value.IsInteger()) {
    return static_cast<double>(value.AsInteger());
  }
  CHECK(value.IsDouble(), common::InvalidArgumentError,
        "expected numeric value");
  return value.AsDouble();
}

bool ValueLess(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) {
    return !left.IsNull() && right.IsNull();
  }
  if (IsNumeric(left) && IsNumeric(right)) {
    if (left.IsInteger() && right.IsInteger()) {
      return left.AsInteger() < right.AsInteger();
    }
    if (left.IsDouble() && right.IsDouble()) {
      return left.AsDouble() < right.AsDouble();
    }
    const Value &integer = left.IsInteger() ? left : right;
    const Value &floating = left.IsDouble() ? left : right;
    const double number = floating.AsDouble();
    bool integer_less = false;
    if (!std::isnan(number)) {
      if (number >=
          static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        integer_less = true;
      } else if (number >= static_cast<double>(
                               std::numeric_limits<std::int64_t>::min())) {
        const auto truncated = static_cast<std::int64_t>(number);
        integer_less = integer.AsInteger() < truncated ||
                       (integer.AsInteger() == truncated &&
                        static_cast<double>(truncated) < number);
      }
    }
    if (left.IsInteger()) {
      return integer_less;
    }
    return !std::isnan(number) && !ValuesEqual(left, right) && !integer_less;
  }
  if (left.Type() != right.Type()) {
    return static_cast<int>(left.Type()) < static_cast<int>(right.Type());
  }
  if (left.IsString()) {
    return left.AsString() < right.AsString();
  }
  if (left.IsBool()) {
    return !left.AsBool() && right.AsBool();
  }
  if (left.IsNode()) {
    return left.AsNode().id < right.AsNode().id;
  }
  if (left.IsRelationship()) {
    return left.AsRelationship().id < right.AsRelationship().id;
  }
  return left.ToString() < right.ToString();
}

Value EvaluateLogicalProjectionItem(const ir::LogicalProjectionItem &item,
                                    const QueryRow &row) {
  CHECK(item.expression != nullptr, common::InvalidArgumentError,
        "projection expression is null");
  return EvaluateExpression(*item.expression, row,
                            item.precomputed_expressions);
}

Value EvaluateLogicalSortItem(const ir::LogicalSortItem &item,
                              const QueryRow &row) {
  CHECK(item.expression != nullptr, common::InvalidArgumentError,
        "sort expression is null");
  return EvaluateExpression(*item.expression, row,
                            item.precomputed_expressions);
}

Value EvaluateExpression(
    const ast::Expression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  if (const Value *value =
          LookupPrecomputedExpression(expression, row, precomputed);
      value != nullptr) {
    return *value;
  }
  switch (expression.node_type) {
    case ast::ASTNodeType::kBooleanLiteral:
      return Value(ast::CastAst<ast::BooleanLiteral>(expression).value);
    case ast::ASTNodeType::kIntegerLiteral:
      return Value(ast::CastAst<ast::IntegerLiteral>(expression).value);
    case ast::ASTNodeType::kDoubleLiteral:
      return Value(ast::CastAst<ast::DoubleLiteral>(expression).value);
    case ast::ASTNodeType::kStringLiteral:
      return Value(ast::CastAst<ast::StringLiteral>(expression).value);
    case ast::ASTNodeType::kNullLiteral:
      return Value::Null();
    case ast::ASTNodeType::kVariable:
      return LookupQueryVariable(row,
                                 ast::CastAst<ast::Variable>(expression).name);
    case ast::ASTNodeType::kPropertyExpression: {
      const auto &property = ast::CastAst<ast::PropertyExpression>(expression);
      CHECK(property.object != nullptr, common::InvalidArgumentError,
            "property object is null");
      Value object = EvaluateExpression(*property.object, row, precomputed);
      const Value *value = FindProperty(object, property.property_key);
      return value != nullptr ? *value : Value::Null();
    }
    case ast::ASTNodeType::kListIndexExpression:
      return EvaluateListIndex(
          ast::CastAst<ast::ListIndexExpression>(expression), row, precomputed);
    case ast::ASTNodeType::kListSliceExpression:
      return EvaluateListSlice(
          ast::CastAst<ast::ListSliceExpression>(expression), row, precomputed);
    case ast::ASTNodeType::kListLiteral: {
      Value::List values;
      for (const auto &element :
           ast::CastAst<ast::ListLiteral>(expression).elements) {
        CHECK(element != nullptr, common::InvalidArgumentError,
              "list element is null");
        values.push_back(EvaluateExpression(*element, row, precomputed));
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kMapLiteral: {
      Value::Map values;
      for (const auto &[key, value] :
           ast::CastAst<ast::MapLiteral>(expression).entries) {
        CHECK(value != nullptr, common::InvalidArgumentError,
              "map value is null");
        values[key] = EvaluateExpression(*value, row, precomputed);
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kAndExpression:
    case ast::ASTNodeType::kOrExpression:
    case ast::ASTNodeType::kXorExpression: {
      const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "boolean expression is incomplete");
      const TruthValue left =
          ToTruthValue(EvaluateExpression(*binary.left, row, precomputed));
      if (expression.Is(ast::ASTNodeType::kAndExpression) &&
          left == TruthValue::kFalse) {
        return Value(false);
      }
      if (expression.Is(ast::ASTNodeType::kOrExpression) &&
          left == TruthValue::kTrue) {
        return Value(true);
      }
      const TruthValue right =
          ToTruthValue(EvaluateExpression(*binary.right, row, precomputed));
      if (expression.Is(ast::ASTNodeType::kAndExpression)) {
        return FromTruthValue(And(left, right));
      }
      if (expression.Is(ast::ASTNodeType::kOrExpression)) {
        return FromTruthValue(Or(left, right));
      }
      if (left == TruthValue::kNull || right == TruthValue::kNull) {
        return Value::Null();
      }
      return Value(left != right);
    }
    case ast::ASTNodeType::kNotExpression: {
      const auto &unary = ast::CastAst<ast::NotExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "NOT expression operand is null");
      return FromTruthValue(Not(
          ToTruthValue(EvaluateExpression(*unary.operand, row, precomputed))));
    }
    case ast::ASTNodeType::kUnaryPlusExpression:
    case ast::ASTNodeType::kUnaryMinusExpression: {
      const auto &unary = ast::CastAst<ast::UnaryExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "unary expression operand is null");
      Value value = EvaluateExpression(*unary.operand, row, precomputed);
      if (value.IsNull()) {
        return Value::Null();
      }
      CHECK(IsNumeric(value), common::InvalidArgumentError,
            "unary arithmetic requires a numeric value");
      if (expression.Is(ast::ASTNodeType::kUnaryPlusExpression)) {
        return value;
      }
      if (value.IsDouble()) {
        return Value(-value.AsDouble());
      }
      CHECK(value.AsInteger() != std::numeric_limits<std::int64_t>::min(),
            common::InvalidArgumentError, "integer negation overflow");
      return Value(-value.AsInteger());
    }
    case ast::ASTNodeType::kAddExpression:
    case ast::ASTNodeType::kSubtractExpression:
    case ast::ASTNodeType::kMultiplyExpression:
    case ast::ASTNodeType::kDivideExpression:
    case ast::ASTNodeType::kModuloExpression:
    case ast::ASTNodeType::kPowerExpression:
      return EvaluateArithmetic(expression, row, precomputed);
    case ast::ASTNodeType::kComparisonExpression: {
      const auto &comparison =
          ast::CastAst<ast::ComparisonExpression>(expression);
      CHECK(comparison.left != nullptr && comparison.right != nullptr,
            common::InvalidArgumentError,
            "comparison expression is incomplete");
      Value left = EvaluateExpression(*comparison.left, row, precomputed);
      Value right = EvaluateExpression(*comparison.right, row, precomputed);
      if (left.IsNull() || right.IsNull()) {
        return Value::Null();
      }
      if (comparison.op == "=") {
        return FromTruthValue(EqualityTruth(left, right));
      }
      if (comparison.op == "<>") {
        return FromTruthValue(Not(EqualityTruth(left, right)));
      }
      if (comparison.op == "<") {
        return Value(ValueLess(left, right));
      }
      if (comparison.op == ">") {
        return Value(ValueLess(right, left));
      }
      if (comparison.op == "<=") {
        return Value(!ValueLess(right, left));
      }
      if (comparison.op == ">=") {
        return Value(!ValueLess(left, right));
      }
      THROW(common::InvalidArgumentError,
            "unsupported comparison operator: " + comparison.op);
    }
    case ast::ASTNodeType::kStringPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::StringPredicateExpression>(expression);
      CHECK(predicate.left != nullptr && predicate.right != nullptr,
            common::InvalidArgumentError,
            "string predicate expression is incomplete");
      Value left = EvaluateExpression(*predicate.left, row, precomputed);
      Value right = EvaluateExpression(*predicate.right, row, precomputed);
      if (left.IsNull() || right.IsNull()) {
        return Value::Null();
      }
      CHECK(left.IsString() && right.IsString(), common::InvalidArgumentError,
            "string predicate requires string values");
      if (predicate.op == "STARTS WITH") {
        return Value(left.AsString().starts_with(right.AsString()));
      }
      if (predicate.op == "ENDS WITH") {
        return Value(left.AsString().size() >= right.AsString().size() &&
                     left.AsString().compare(
                         left.AsString().size() - right.AsString().size(),
                         right.AsString().size(), right.AsString()) == 0);
      }
      if (predicate.op == "CONTAINS") {
        return Value(left.AsString().find(right.AsString()) !=
                     std::string::npos);
      }
      THROW(common::InvalidArgumentError,
            "unsupported string predicate: " + predicate.op);
    }
    case ast::ASTNodeType::kListPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::ListPredicateExpression>(expression);
      CHECK(predicate.element != nullptr && predicate.list != nullptr,
            common::InvalidArgumentError,
            "list predicate expression is incomplete");
      Value element = EvaluateExpression(*predicate.element, row, precomputed);
      Value list = EvaluateExpression(*predicate.list, row, precomputed);
      if (list.IsNull()) {
        return Value::Null();
      }
      CHECK(list.IsList(), common::InvalidArgumentError,
            "IN requires a list value");
      bool saw_null = false;
      for (const Value &candidate : list.AsList()) {
        const TruthValue equality = EqualityTruth(element, candidate);
        if (equality == TruthValue::kTrue) {
          return Value(true);
        }
        saw_null = saw_null || equality == TruthValue::kNull;
      }
      return saw_null ? Value::Null() : Value(false);
    }
    case ast::ASTNodeType::kLabelPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::LabelPredicateExpression>(expression);
      CHECK(predicate.expr != nullptr, common::InvalidArgumentError,
            "label predicate expression is incomplete");
      Value value = EvaluateExpression(*predicate.expr, row, precomputed);
      if (value.IsNull()) {
        return Value::Null();
      }
      if (value.IsNode()) {
        return Value(NodeHasLabels(value.AsNode(), predicate.labels));
      }
      if (value.IsRelationship()) {
        return Value(
            RelationshipHasAnyType(value.AsRelationship(), predicate.labels));
      }
      CHECK(false, common::InvalidArgumentError,
            "label predicate requires a graph entity");
    }
    case ast::ASTNodeType::kNullPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::NullPredicateExpression>(expression);
      CHECK(predicate.operand != nullptr, common::InvalidArgumentError,
            "null predicate operand is null");
      return Value(
          EvaluateExpression(*predicate.operand, row, precomputed).IsNull() ==
          predicate.is_null);
    }
    case ast::ASTNodeType::kFunctionInvocation:
      return EvaluateFunction(ast::CastAst<ast::FunctionInvocation>(expression),
                              row, precomputed);
    case ast::ASTNodeType::kCaseExpression:
      return EvaluateCaseExpression(
          ast::CastAst<ast::CaseExpression>(expression), row, precomputed);
    case ast::ASTNodeType::kListComprehension:
      return EvaluateListComprehension(
          ast::CastAst<ast::ListComprehension>(expression), row, precomputed);
    case ast::ASTNodeType::kAllQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::AllQuantifier>(expression),
                                QuantifierMode::kAll, row, precomputed);
    case ast::ASTNodeType::kAnyQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::AnyQuantifier>(expression),
                                QuantifierMode::kAny, row, precomputed);
    case ast::ASTNodeType::kNoneQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::NoneQuantifier>(expression),
                                QuantifierMode::kNone, row, precomputed);
    case ast::ASTNodeType::kSingleQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::SingleQuantifier>(expression),
                                QuantifierMode::kSingle, row, precomputed);
    case ast::ASTNodeType::kParenthesizedExpression: {
      const auto &parenthesized =
          ast::CastAst<ast::ParenthesizedExpression>(expression);
      CHECK(parenthesized.expr != nullptr, common::InvalidArgumentError,
            "parenthesized expression is empty");
      return EvaluateExpression(*parenthesized.expr, row, precomputed);
    }
    default:
      THROW(common::InvalidArgumentError,
            "unsupported expression in executor: " +
                std::string(ast::ToString(expression.node_type)));
  }
}

}  // namespace rg
