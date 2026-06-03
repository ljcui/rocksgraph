#include "runtime/query_executor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "ast/expression_to_string.h"
#include "common/exception.h"
#include "ir/logical_plan_builder.h"
#include "ir/planner_query.h"

namespace rg {
namespace {

using Rows = std::vector<QueryRow>;

struct EntityRef {
  enum class Kind { kNode, kRelationship };

  Kind kind = Kind::kNode;
  std::int64_t id = -1;
};

enum class QuantifierMode { kAll, kAny, kNone, kSingle };

std::string LowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string ValueKey(const Value &value) {
  return std::to_string(static_cast<int>(value.Type())) + ":" +
         value.ToString();
}

bool IsNumeric(const Value &value) {
  return value.IsInteger() || value.IsDouble();
}

double AsDoubleValue(const Value &value) {
  if (value.IsInteger()) {
    return static_cast<double>(value.AsInteger());
  }
  if (value.IsDouble()) {
    return value.AsDouble();
  }
  THROW(common::InvalidArgumentError, "expected numeric value");
}

std::optional<std::int64_t> ParseInteger(std::string_view text) {
  try {
    std::size_t parsed = 0;
    std::int64_t value = std::stoll(std::string(text), &parsed);
    if (parsed == text.size()) {
      return value;
    }
  } catch (const std::invalid_argument &) {
  } catch (const std::out_of_range &) {
  }
  return std::nullopt;
}

std::optional<double> ParseDouble(std::string_view text) {
  try {
    std::size_t parsed = 0;
    double value = std::stod(std::string(text), &parsed);
    if (parsed == text.size()) {
      return value;
    }
  } catch (const std::invalid_argument &) {
  } catch (const std::out_of_range &) {
  }
  return std::nullopt;
}

bool AddWouldOverflow(std::int64_t lhs, std::int64_t rhs) {
  if (rhs > 0) {
    return lhs > std::numeric_limits<std::int64_t>::max() - rhs;
  }
  if (rhs < 0) {
    if (rhs == std::numeric_limits<std::int64_t>::min()) {
      return lhs < 0;
    }
    return lhs < std::numeric_limits<std::int64_t>::min() - rhs;
  }
  return false;
}

bool IsTruthy(const Value &value) {
  if (value.IsNull()) {
    return false;
  }
  if (value.IsBool()) {
    return value.AsBool();
  }
  if (value.IsInteger()) {
    return value.AsInteger() != 0;
  }
  if (value.IsDouble()) {
    return value.AsDouble() != 0.0;
  }
  if (value.IsString()) {
    return !value.AsString().empty();
  }
  if (value.IsList()) {
    return !value.AsList().empty();
  }
  return true;
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

Value NumericValue(double value, bool integral) {
  if (integral) {
    return Value(static_cast<std::int64_t>(value));
  }
  return Value(value);
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
  const std::int64_t list_size = static_cast<std::int64_t>(size);
  return index > list_size ? list_size : index;
}

bool ValueLess(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) {
    return !left.IsNull() && right.IsNull();
  }
  if (IsNumeric(left) && IsNumeric(right)) {
    return AsDoubleValue(left) < AsDoubleValue(right);
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

const Value &LookupVariable(const QueryRow &row, const std::string &name) {
  const auto found = row.find(name);
  CHECK(found != row.end(), common::InvalidArgumentError,
        "variable is not bound: " + name);
  return found->second;
}

bool TryBind(QueryRow *row, const std::string &variable, Value value) {
  CHECK(row != nullptr, common::InternalError, "query row is null");
  if (variable.empty()) {
    return true;
  }
  const auto found = row->find(variable);
  if (found == row->end()) {
    row->emplace(variable, std::move(value));
    return true;
  }
  return found->second == value;
}

bool MergeRows(const QueryRow &left, const QueryRow &right, QueryRow *out) {
  CHECK(out != nullptr, common::InternalError, "query row is null");
  *out = left;
  for (const auto &[key, value] : right) {
    if (!TryBind(out, key, value)) {
      return false;
    }
  }
  return true;
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

Value EvaluateExpression(
    const ast::Expression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed = {});

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

bool RuntimeNodeHasLabels(const Node &node,
                          const std::vector<std::string> &labels);
bool RuntimeRelationshipHasAnyType(const Relationship &relationship,
                                   const std::vector<std::string> &types);

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
    Value::List labels;
    if (arguments[0].IsNode()) {
      for (const auto &label : arguments[0].AsNode().labels) {
        labels.emplace_back(label);
      }
    }
    return Value(std::move(labels));
  }
  if (name == "type") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "type() expects one argument");
    if (arguments[0].IsRelationship()) {
      return Value(arguments[0].AsRelationship().type);
    }
    return Value::Null();
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
    if (arguments[0].IsPath()) {
      return Value(static_cast<std::int64_t>(
          arguments[0].AsPath().relationships.size()));
    }
    return Value::Null();
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
    if (arguments[0].IsNull()) {
      return Value::Null();
    }
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
    Value::List keys;
    if (arguments[0].IsMap()) {
      keys.reserve(arguments[0].AsMap().size());
      for (const auto &[key, value] : arguments[0].AsMap()) {
        (void)value;
        keys.emplace_back(key);
      }
      return Value(std::move(keys));
    }
    if (arguments[0].IsNode()) {
      keys.reserve(arguments[0].AsNode().properties.size());
      for (const auto &[key, value] : arguments[0].AsNode().properties) {
        (void)value;
        keys.emplace_back(key);
      }
      return Value(std::move(keys));
    }
    if (arguments[0].IsRelationship()) {
      keys.reserve(arguments[0].AsRelationship().properties.size());
      for (const auto &[key, value] :
           arguments[0].AsRelationship().properties) {
        (void)value;
        keys.emplace_back(key);
      }
      return Value(std::move(keys));
    }
    return Value::Null();
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
    if (step > 0) {
      for (std::int64_t value = start; value <= end;) {
        values.emplace_back(value);
        if (AddWouldOverflow(value, step)) {
          break;
        }
        const std::int64_t next = value + step;
        if (next > end) {
          break;
        }
        value = next;
      }
    } else {
      for (std::int64_t value = start; value >= end;) {
        values.emplace_back(value);
        if (AddWouldOverflow(value, step)) {
          break;
        }
        const std::int64_t next = value + step;
        if (next < end) {
          break;
        }
        value = next;
      }
    }
    return Value(std::move(values));
  }
  if (name == "split") {
    CHECK(arguments.size() == 2, common::InvalidArgumentError,
          "split() expects two arguments");
    if (!arguments[0].IsString() || !arguments[1].IsString()) {
      return Value::Null();
    }
    const std::string &text = arguments[0].AsString();
    const std::string &delimiter = arguments[1].AsString();
    Value::List parts;
    if (delimiter.empty()) {
      parts.reserve(text.size());
      for (char ch : text) {
        parts.emplace_back(std::string(1, ch));
      }
      return Value(std::move(parts));
    }
    std::size_t start = 0;
    while (true) {
      const std::size_t found = text.find(delimiter, start);
      if (found == std::string::npos) {
        parts.emplace_back(text.substr(start));
        break;
      }
      parts.emplace_back(text.substr(start, found - start));
      start = found + delimiter.size();
    }
    return Value(std::move(parts));
  }
  if (name == "nodes") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "nodes() expects one argument");
    if (!arguments[0].IsPath()) {
      return Value::Null();
    }
    Value::List nodes;
    nodes.reserve(arguments[0].AsPath().nodes.size());
    for (const auto &node : arguments[0].AsPath().nodes) {
      nodes.emplace_back(node);
    }
    return Value(std::move(nodes));
  }
  if (name == "relationships") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "relationships() expects one argument");
    if (!arguments[0].IsPath()) {
      return Value::Null();
    }
    Value::List relationships;
    relationships.reserve(arguments[0].AsPath().relationships.size());
    for (const auto &relationship : arguments[0].AsPath().relationships) {
      relationships.emplace_back(relationship);
    }
    return Value(std::move(relationships));
  }
  if (name == "tostring") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toString() expects one argument");
    if (arguments[0].IsNull()) {
      return Value::Null();
    }
    if (arguments[0].IsString()) {
      return arguments[0];
    }
    return Value(arguments[0].ToString());
  }
  if (name == "tointeger") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toInteger() expects one argument");
    if (arguments[0].IsNull()) {
      return Value::Null();
    }
    if (arguments[0].IsInteger()) {
      return arguments[0];
    }
    if (arguments[0].IsDouble()) {
      return Value(static_cast<std::int64_t>(arguments[0].AsDouble()));
    }
    if (arguments[0].IsBool()) {
      return Value(arguments[0].AsBool() ? 1 : 0);
    }
    if (arguments[0].IsString()) {
      std::optional<std::int64_t> value = ParseInteger(arguments[0].AsString());
      return value.has_value() ? Value(*value) : Value::Null();
    }
    return Value::Null();
  }
  if (name == "tofloat") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toFloat() expects one argument");
    if (arguments[0].IsNull()) {
      return Value::Null();
    }
    if (arguments[0].IsDouble()) {
      return arguments[0];
    }
    if (arguments[0].IsInteger()) {
      return Value(static_cast<double>(arguments[0].AsInteger()));
    }
    if (arguments[0].IsBool()) {
      return Value(arguments[0].AsBool() ? 1.0 : 0.0);
    }
    if (arguments[0].IsString()) {
      std::optional<double> value = ParseDouble(arguments[0].AsString());
      return value.has_value() ? Value(*value) : Value::Null();
    }
    return Value::Null();
  }
  if (name == "toboolean") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toBoolean() expects one argument");
    if (arguments[0].IsNull()) {
      return Value::Null();
    }
    if (arguments[0].IsBool()) {
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
  if (name == "tolower") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toLower() expects one argument");
    return arguments[0].IsString() ? Value(LowerAscii(arguments[0].AsString()))
                                   : Value::Null();
  }
  if (name == "toupper") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "toUpper() expects one argument");
    if (!arguments[0].IsString()) {
      return Value::Null();
    }
    std::string value = arguments[0].AsString();
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return Value(std::move(value));
  }
  if (name == "trim") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "trim() expects one argument");
    return arguments[0].IsString() ? Value(TrimAscii(arguments[0].AsString()))
                                   : Value::Null();
  }

  THROW(common::InvalidArgumentError,
        "unsupported function in executor: " + function.function_name);
}

Value EvaluateListIndex(
    const ast::ListIndexExpression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(expression.list != nullptr, common::InvalidArgumentError,
        "list index base expression is null");
  CHECK(expression.index != nullptr, common::InvalidArgumentError,
        "list index expression is null");

  Value list = EvaluateExpression(*expression.list, row, precomputed);
  Value index_value = EvaluateExpression(*expression.index, row, precomputed);
  std::optional<std::int64_t> index = IntegerValue(index_value);
  if (!list.IsList() || !index.has_value()) {
    return Value::Null();
  }

  const Value::List &items = list.AsList();
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

  const Value::List &items = list.AsList();
  std::int64_t start = 0;
  std::int64_t end = static_cast<std::int64_t>(items.size());
  if (expression.start_index != nullptr) {
    Value start_value =
        EvaluateExpression(*expression.start_index, row, precomputed);
    std::optional<std::int64_t> maybe_start = IntegerValue(start_value);
    if (!maybe_start.has_value()) {
      return Value::Null();
    }
    start = ClampListSliceIndex(*maybe_start, items.size());
  }
  if (expression.end_index != nullptr) {
    Value end_value =
        EvaluateExpression(*expression.end_index, row, precomputed);
    std::optional<std::int64_t> maybe_end = IntegerValue(end_value);
    if (!maybe_end.has_value()) {
      return Value::Null();
    }
    end = ClampListSliceIndex(*maybe_end, items.size());
  }
  if (end < start) {
    end = start;
  }

  Value::List sliced;
  sliced.reserve(static_cast<std::size_t>(end - start));
  for (std::int64_t index = start; index < end; ++index) {
    sliced.push_back(items[static_cast<std::size_t>(index)]);
  }
  return Value(std::move(sliced));
}

Value EvaluateCaseExpression(
    const ast::CaseExpression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  std::optional<Value> test_value;
  if (expression.test != nullptr) {
    test_value = EvaluateExpression(*expression.test, row, precomputed);
  }

  for (const auto &[when_expression, then_expression] :
       expression.alternatives) {
    CHECK(when_expression != nullptr, common::InvalidArgumentError,
          "CASE WHEN expression is null");
    CHECK(then_expression != nullptr, common::InvalidArgumentError,
          "CASE THEN expression is null");

    bool matched = false;
    if (test_value.has_value()) {
      matched =
          *test_value == EvaluateExpression(*when_expression, row, precomputed);
    } else {
      matched =
          IsTruthy(EvaluateExpression(*when_expression, row, precomputed));
    }
    if (matched) {
      return EvaluateExpression(*then_expression, row, precomputed);
    }
  }

  if (expression.else_expr != nullptr) {
    return EvaluateExpression(*expression.else_expr, row, precomputed);
  }
  return Value::Null();
}

Value EvaluateListComprehension(
    const ast::ListComprehension &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(!expression.variable.empty(), common::InvalidArgumentError,
        "list comprehension variable is empty");
  CHECK(expression.list_expr != nullptr, common::InvalidArgumentError,
        "list comprehension list expression is null");

  Value list = EvaluateExpression(*expression.list_expr, row, precomputed);
  if (!list.IsList()) {
    return Value::Null();
  }

  Value::List output;
  for (const auto &item : list.AsList()) {
    QueryRow scoped = row;
    scoped[expression.variable] = item;
    if (expression.where_expr != nullptr &&
        !IsTruthy(
            EvaluateExpression(*expression.where_expr, scoped, precomputed))) {
      continue;
    }
    if (expression.eval_expr != nullptr) {
      output.push_back(
          EvaluateExpression(*expression.eval_expr, scoped, precomputed));
    } else {
      output.push_back(item);
    }
  }
  return Value(std::move(output));
}

Value EvaluateQuantifier(
    const ast::Quantifier &quantifier, QuantifierMode mode, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(!quantifier.variable.empty(), common::InvalidArgumentError,
        "quantifier variable is empty");
  CHECK(quantifier.list_expr != nullptr, common::InvalidArgumentError,
        "quantifier list expression is null");
  CHECK(quantifier.predicate != nullptr, common::InvalidArgumentError,
        "quantifier predicate is null");

  Value list = EvaluateExpression(*quantifier.list_expr, row, precomputed);
  if (!list.IsList()) {
    return Value(false);
  }

  std::int64_t matches = 0;
  for (const auto &item : list.AsList()) {
    QueryRow scoped = row;
    scoped[quantifier.variable] = item;
    const bool matched = IsTruthy(
        EvaluateExpression(*quantifier.predicate, scoped, precomputed));
    if (mode == QuantifierMode::kAll && !matched) {
      return Value(false);
    }
    if (mode == QuantifierMode::kAny && matched) {
      return Value(true);
    }
    if (mode == QuantifierMode::kNone && matched) {
      return Value(false);
    }
    if (mode == QuantifierMode::kSingle && matched) {
      ++matches;
      if (matches > 1) {
        return Value(false);
      }
    }
  }

  switch (mode) {
    case QuantifierMode::kAll:
      return Value(true);
    case QuantifierMode::kAny:
      return Value(false);
    case QuantifierMode::kNone:
      return Value(true);
    case QuantifierMode::kSingle:
      return Value(matches == 1);
  }
  THROW(common::InternalError, "unknown quantifier mode");
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
    case ast::ASTNodeType::kVariable: {
      const auto &variable = ast::CastAst<ast::Variable>(expression);
      return LookupVariable(row, variable.name);
    }
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
      const auto &list = ast::CastAst<ast::ListLiteral>(expression);
      Value::List values;
      values.reserve(list.elements.size());
      for (const auto &element : list.elements) {
        CHECK(element != nullptr, common::InvalidArgumentError,
              "list element is null");
        values.push_back(EvaluateExpression(*element, row, precomputed));
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kMapLiteral: {
      const auto &map = ast::CastAst<ast::MapLiteral>(expression);
      Value::Map values;
      for (const auto &[key, value] : map.entries) {
        CHECK(value != nullptr, common::InvalidArgumentError,
              "map value is null");
        values[key] = EvaluateExpression(*value, row, precomputed);
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kAndExpression: {
      const auto &binary = ast::CastAst<ast::AndExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "AND expression is incomplete");
      return Value(
          IsTruthy(EvaluateExpression(*binary.left, row, precomputed)) &&
          IsTruthy(EvaluateExpression(*binary.right, row, precomputed)));
    }
    case ast::ASTNodeType::kOrExpression: {
      const auto &binary = ast::CastAst<ast::OrExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "OR expression is incomplete");
      return Value(
          IsTruthy(EvaluateExpression(*binary.left, row, precomputed)) ||
          IsTruthy(EvaluateExpression(*binary.right, row, precomputed)));
    }
    case ast::ASTNodeType::kXorExpression: {
      const auto &binary = ast::CastAst<ast::XorExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "XOR expression is incomplete");
      return Value(
          IsTruthy(EvaluateExpression(*binary.left, row, precomputed)) !=
          IsTruthy(EvaluateExpression(*binary.right, row, precomputed)));
    }
    case ast::ASTNodeType::kNotExpression: {
      const auto &unary = ast::CastAst<ast::NotExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "NOT expression operand is null");
      return Value(
          !IsTruthy(EvaluateExpression(*unary.operand, row, precomputed)));
    }
    case ast::ASTNodeType::kUnaryPlusExpression: {
      const auto &unary = ast::CastAst<ast::UnaryPlusExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "unary plus operand is null");
      return EvaluateExpression(*unary.operand, row, precomputed);
    }
    case ast::ASTNodeType::kUnaryMinusExpression: {
      const auto &unary = ast::CastAst<ast::UnaryMinusExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "unary minus operand is null");
      Value value = EvaluateExpression(*unary.operand, row, precomputed);
      CHECK(IsNumeric(value), common::InvalidArgumentError,
            "unary minus requires numeric value");
      return value.IsInteger() ? Value(-value.AsInteger())
                               : Value(-value.AsDouble());
    }
    case ast::ASTNodeType::kAddExpression:
    case ast::ASTNodeType::kSubtractExpression:
    case ast::ASTNodeType::kMultiplyExpression:
    case ast::ASTNodeType::kDivideExpression:
    case ast::ASTNodeType::kModuloExpression:
    case ast::ASTNodeType::kPowerExpression: {
      const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError,
            "arithmetic expression is incomplete");
      Value left = EvaluateExpression(*binary.left, row, precomputed);
      Value right = EvaluateExpression(*binary.right, row, precomputed);
      if (expression.node_type == ast::ASTNodeType::kAddExpression &&
          left.IsString() && right.IsString()) {
        return Value(left.AsString() + right.AsString());
      }
      CHECK(IsNumeric(left) && IsNumeric(right), common::InvalidArgumentError,
            "arithmetic expression requires numeric values");
      const bool integral =
          left.IsInteger() && right.IsInteger() &&
          expression.node_type != ast::ASTNodeType::kDivideExpression &&
          expression.node_type != ast::ASTNodeType::kPowerExpression;
      const double lhs = AsDoubleValue(left);
      const double rhs = AsDoubleValue(right);
      switch (expression.node_type) {
        case ast::ASTNodeType::kAddExpression:
          return NumericValue(lhs + rhs, integral);
        case ast::ASTNodeType::kSubtractExpression:
          return NumericValue(lhs - rhs, integral);
        case ast::ASTNodeType::kMultiplyExpression:
          return NumericValue(lhs * rhs, integral);
        case ast::ASTNodeType::kDivideExpression:
          return Value(lhs / rhs);
        case ast::ASTNodeType::kModuloExpression:
          CHECK(right.AsInteger() != 0, common::InvalidArgumentError,
                "modulo by zero");
          return Value(left.AsInteger() % right.AsInteger());
        case ast::ASTNodeType::kPowerExpression:
          return Value(std::pow(lhs, rhs));
        default:
          break;
      }
      THROW(common::InternalError, "unknown arithmetic expression");
    }
    case ast::ASTNodeType::kComparisonExpression: {
      const auto &comparison =
          ast::CastAst<ast::ComparisonExpression>(expression);
      CHECK(comparison.left != nullptr && comparison.right != nullptr,
            common::InvalidArgumentError,
            "comparison expression is incomplete");
      Value left = EvaluateExpression(*comparison.left, row, precomputed);
      Value right = EvaluateExpression(*comparison.right, row, precomputed);
      if (comparison.op == "=") {
        return Value(left == right);
      }
      if (comparison.op == "<>") {
        return Value(left != right);
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
      if (!left.IsString() || !right.IsString()) {
        return Value(false);
      }
      const std::string &text = left.AsString();
      const std::string &needle = right.AsString();
      if (predicate.op == "STARTS WITH") {
        return Value(text.rfind(needle, 0) == 0);
      }
      if (predicate.op == "ENDS WITH") {
        return Value(text.size() >= needle.size() &&
                     text.compare(text.size() - needle.size(), needle.size(),
                                  needle) == 0);
      }
      if (predicate.op == "CONTAINS") {
        return Value(text.find(needle) != std::string::npos);
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
      if (!list.IsList()) {
        return Value(false);
      }
      return Value(std::find(list.AsList().begin(), list.AsList().end(),
                             element) != list.AsList().end());
    }
    case ast::ASTNodeType::kLabelPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::LabelPredicateExpression>(expression);
      CHECK(predicate.expr != nullptr, common::InvalidArgumentError,
            "label predicate expression is incomplete");
      Value value = EvaluateExpression(*predicate.expr, row, precomputed);
      if (value.IsNode()) {
        return Value(RuntimeNodeHasLabels(value.AsNode(), predicate.labels));
      }
      if (value.IsRelationship()) {
        return Value(RuntimeRelationshipHasAnyType(value.AsRelationship(),
                                                   predicate.labels));
      }
      return Value(false);
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

std::optional<double> EvaluateNumericOptional(const ast::Expression *expression,
                                              const QueryRow &row) {
  if (expression == nullptr) {
    return std::nullopt;
  }
  Value value = EvaluateExpression(*expression, row);
  if (value.IsInteger()) {
    return static_cast<double>(value.AsInteger());
  }
  if (value.IsDouble()) {
    return value.AsDouble();
  }
  return std::nullopt;
}

Value MakeValueFromLiteralMap(
    const std::vector<std::pair<std::string, const ast::Expression *>> &entries,
    const QueryRow &row) {
  Value::Map map;
  for (const auto &[key, expression] : entries) {
    CHECK(expression != nullptr, common::InvalidArgumentError,
          "map value is null");
    map[key] = EvaluateExpression(*expression, row);
  }
  return Value(std::move(map));
}

bool RuntimeNodeHasLabels(const Node &node,
                          const std::vector<std::string> &labels) {
  for (const auto &label : labels) {
    if (std::find(node.labels.begin(), node.labels.end(), label) ==
        node.labels.end()) {
      return false;
    }
  }
  return true;
}

bool RuntimeRelationshipHasAnyType(const Relationship &relationship,
                                   const std::vector<std::string> &types) {
  return types.empty() || std::find(types.begin(), types.end(),
                                    relationship.type) != types.end();
}

void PopulateLabels(Node *node, const std::vector<std::string> &labels) {
  CHECK(node != nullptr, common::InternalError, "node is null");
  for (const auto &label : labels) {
    if (std::find(node->labels.begin(), node->labels.end(), label) ==
        node->labels.end()) {
      node->labels.push_back(label);
    }
  }
  std::sort(node->labels.begin(), node->labels.end());
}

void RemoveLabels(Node *node, const std::vector<std::string> &labels) {
  CHECK(node != nullptr, common::InternalError, "node is null");
  node->labels.erase(std::remove_if(node->labels.begin(), node->labels.end(),
                                    [&labels](const std::string &label) {
                                      return std::find(labels.begin(),
                                                       labels.end(),
                                                       label) != labels.end();
                                    }),
                     node->labels.end());
}

std::vector<EntityRef> RelationshipsToDelete(const InMemoryGraph &graph,
                                             std::int64_t node_id) {
  std::vector<EntityRef> refs;
  for (const auto &relationship : graph.Relationships()) {
    if (relationship->start_node_id == node_id ||
        relationship->end_node_id == node_id) {
      refs.push_back(
          EntityRef{EntityRef::Kind::kRelationship, relationship->id});
    }
  }
  return refs;
}

Value CopyOrNull(const Value *value) {
  return value != nullptr ? *value : Value::Null();
}

EntityRef MakeEntityRef(const Value &value) {
  CHECK(value.IsNode() || value.IsRelationship(), common::InvalidArgumentError,
        "expected node or relationship value");
  if (value.IsNode()) {
    return EntityRef{EntityRef::Kind::kNode, value.AsNode().id};
  }
  return EntityRef{EntityRef::Kind::kRelationship, value.AsRelationship().id};
}

Value::Map ParseMapLiteral(const ast::Expression &expression,
                           const QueryRow &row) {
  CHECK(expression.Is(ast::ASTNodeType::kMapLiteral),
        common::InvalidArgumentError, "expected map literal");
  const auto &map = ast::CastAst<ast::MapLiteral>(expression);
  Value::Map values;
  for (const auto &[key, value] : map.entries) {
    CHECK(value != nullptr, common::InvalidArgumentError, "map value is null");
    values[key] = EvaluateExpression(*value, row);
  }
  return values;
}

Value EnsureMapValue(const Value &value) {
  CHECK(value.IsMap(), common::InvalidArgumentError, "expected map value");
  return value;
}

class QueryExecutorImpl {
 public:
  explicit QueryExecutorImpl(InMemoryGraph *graph) : graph_(graph) {
    CHECK(graph_ != nullptr, common::InvalidArgumentError, "graph is null");
  }

  QueryResult Execute(const ir::LogicalPlan &plan) {
    Rows rows = ExecutePlan(plan, Rows{QueryRow{}});
    return Materialize(plan.OutputColumns(), rows);
  }

  void ExecuteWrite(const ir::LogicalPlan &plan) {
    (void)ExecutePlan(plan, Rows{QueryRow{}});
  }

 private:
  QueryResult Materialize(const std::vector<std::string> &columns,
                          const Rows &rows) const {
    QueryResult result;
    result.columns = columns;
    result.rows.reserve(rows.size());
    for (const auto &row : rows) {
      std::vector<Value> values;
      values.reserve(columns.size());
      for (const auto &column : columns) {
        const auto found = row.find(column);
        values.push_back(found == row.end() ? Value::Null() : found->second);
      }
      result.rows.push_back(std::move(values));
    }
    return result;
  }

  Rows ExecutePlan(const ir::LogicalPlan &plan, const Rows &input) {
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kArgument:
        return ExecuteArgument(static_cast<const ir::ArgumentPlan &>(plan),
                               input);
      case ir::LogicalPlanNodeType::kAllNodeScan:
        return ExecuteAllNodeScan(
            static_cast<const ir::AllNodeScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeByLabelScan:
        return ExecuteNodeByLabelScan(
            static_cast<const ir::NodeByLabelScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeIndexSeek:
        return ExecuteNodeIndexSeek(
            static_cast<const ir::NodeIndexSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kNodeIndexRangeSeek:
        return ExecuteNodeIndexRangeSeek(
            static_cast<const ir::NodeIndexRangeSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipTypeScan:
        return ExecuteRelationshipTypeScan(
            static_cast<const ir::RelationshipTypeScanPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipIndexSeek:
        return ExecuteRelationshipIndexSeek(
            static_cast<const ir::RelationshipIndexSeekPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRelationshipIndexRangeSeek:
        return ExecuteRelationshipIndexRangeSeek(
            static_cast<const ir::RelationshipIndexRangeSeekPlan &>(plan),
            input);
      case ir::LogicalPlanNodeType::kExpand:
        return ExecuteExpand(static_cast<const ir::ExpandPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kExpandInto:
        return ExecuteExpandInto(static_cast<const ir::ExpandIntoPlan &>(plan),
                                 input);
      case ir::LogicalPlanNodeType::kVarExpand:
        return ExecuteVarExpand(static_cast<const ir::VarExpandPlan &>(plan),
                                input);
      case ir::LogicalPlanNodeType::kPathBuild:
        return ExecutePathBuild(static_cast<const ir::PathBuildPlan &>(plan),
                                input);
      case ir::LogicalPlanNodeType::kFilter:
        return ExecuteFilter(static_cast<const ir::FilterPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kProjection:
        return ExecuteProjection(static_cast<const ir::ProjectionPlan &>(plan),
                                 input);
      case ir::LogicalPlanNodeType::kDistinct:
        return ExecuteDistinct(static_cast<const ir::DistinctPlan &>(plan),
                               input);
      case ir::LogicalPlanNodeType::kAggregation:
        return ExecuteAggregation(
            static_cast<const ir::AggregationPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSort:
        return ExecuteSort(static_cast<const ir::SortPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSkip:
        return ExecuteSkip(static_cast<const ir::SkipPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kLimit:
        return ExecuteLimit(static_cast<const ir::LimitPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kProduceResults:
        return ExecutePlan(plan.Child(0), input);
      case ir::LogicalPlanNodeType::kCartesianProduct:
      case ir::LogicalPlanNodeType::kNodeHashJoin:
      case ir::LogicalPlanNodeType::kValueHashJoin:
      case ir::LogicalPlanNodeType::kPredicateJoin:
        return ExecuteJoin(plan, input);
      case ir::LogicalPlanNodeType::kApply:
        return ExecuteApply(plan, input);
      case ir::LogicalPlanNodeType::kSemiApply:
        return ExecuteSemiApply(plan, input);
      case ir::LogicalPlanNodeType::kAntiSemiApply:
        return ExecuteAntiSemiApply(plan, input);
      case ir::LogicalPlanNodeType::kLetSemiApply:
        return ExecuteLetSemiApply(
            static_cast<const ir::LetSemiApplyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRollUpApply:
        return ExecuteRollUpApply(
            static_cast<const ir::RollUpApplyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kOptionalApply:
        return ExecuteOptionalApply(plan, input);
      case ir::LogicalPlanNodeType::kUnwind:
        return ExecuteUnwind(static_cast<const ir::UnwindPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kProcedureCall:
        return ExecuteProcedureCall(
            static_cast<const ir::ProcedureCallPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kUnion:
        return ExecuteUnion(static_cast<const ir::UnionPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kWriteBarrier:
        return ExecutePlan(plan.Child(0), input);
      case ir::LogicalPlanNodeType::kCreateNode:
        return ExecuteCreateNode(static_cast<const ir::CreateNodePlan &>(plan),
                                 input);
      case ir::LogicalPlanNodeType::kCreateRelationship:
        return ExecuteCreateRelationship(
            static_cast<const ir::CreateRelationshipPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kMerge:
        return ExecuteMerge(static_cast<const ir::MergePlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSetProperty:
        return ExecuteSetProperty(
            static_cast<const ir::SetPropertyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSetProperties:
        return ExecuteSetProperties(
            static_cast<const ir::SetPropertiesPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kSetLabels:
        return ExecuteSetLabels(static_cast<const ir::SetLabelsPlan &>(plan),
                                input);
      case ir::LogicalPlanNodeType::kRemoveProperty:
        return ExecuteRemoveProperty(
            static_cast<const ir::RemovePropertyPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kRemoveLabels:
        return ExecuteRemoveLabels(
            static_cast<const ir::RemoveLabelsPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kDelete:
        return ExecuteDelete(static_cast<const ir::DeletePlan &>(plan), input,
                             false);
      case ir::LogicalPlanNodeType::kDetachDelete:
        return ExecuteDelete(static_cast<const ir::DetachDeletePlan &>(plan),
                             input, true);
      case ir::LogicalPlanNodeType::kAssertIsNode:
        return ExecutePlan(plan.Child(0), input);
      default:
        THROW(common::InvalidArgumentError,
              "unsupported logical plan in executor: " +
                  std::string(plan.Name()));
    }
  }

  Rows ExecuteArgument(const ir::ArgumentPlan &plan, const Rows &input) {
    Rows out;
    out.reserve(input.size());
    for (const auto &row : input) {
      QueryRow projected;
      if (plan.OutputColumns().empty()) {
        projected = row;
      } else {
        for (const auto &column : plan.OutputColumns()) {
          const auto found = row.find(column);
          CHECK(found != row.end(), common::InvalidArgumentError,
                "argument variable is not bound: " + column);
          projected.emplace(column, found->second);
        }
      }
      out.push_back(std::move(projected));
    }
    return out;
  }

  Rows ExecuteAllNodeScan(const ir::AllNodeScanPlan &plan, const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : graph_->Nodes()) {
        QueryRow next = row;
        if (TryBind(&next, plan.Variable(), Value(node))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteNodeByLabelScan(const ir::NodeByLabelScanPlan &plan,
                              const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : graph_->Nodes()) {
        if (!RuntimeNodeHasLabels(*node, plan.Labels())) {
          continue;
        }
        QueryRow next = row;
        if (TryBind(&next, plan.Variable(), Value(node))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteNodeIndexSeek(const ir::NodeIndexSeekPlan &plan,
                            const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      Value expected = EvaluateExpression(*plan.ValueExpression(), row);
      for (const auto &node : graph_->Nodes()) {
        if (!RuntimeNodeHasLabels(*node, plan.Labels())) {
          continue;
        }
        const auto found = node->properties.find(plan.PropertyKey());
        if (found == node->properties.end() || found->second != expected) {
          continue;
        }
        QueryRow next = row;
        if (TryBind(&next, plan.Variable(), Value(node))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteNodeIndexRangeSeek(const ir::NodeIndexRangeSeekPlan &plan,
                                 const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : graph_->Nodes()) {
        if (!RuntimeNodeHasLabels(*node, plan.Labels())) {
          continue;
        }
        QueryRow next = row;
        if (!TryBind(&next, plan.Variable(), Value(node))) {
          continue;
        }
        bool keep = true;
        for (const ast::Expression *predicate : plan.Predicates()) {
          CHECK(predicate != nullptr, common::InvalidArgumentError,
                "index range predicate is null");
          keep = keep && IsTruthy(EvaluateExpression(*predicate, next));
        }
        if (keep) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteRelationshipTypeScan(const ir::RelationshipTypeScanPlan &plan,
                                   const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &relationship : graph_->Relationships()) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        if (const auto *from =
                graph_->NodeById(relationship->start_node_id).get();
            from != nullptr) {
          (void)from;
        }
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &out);
      }
    }
    return out;
  }

  Rows ExecuteRelationshipIndexSeek(const ir::RelationshipIndexSeekPlan &plan,
                                    const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      Value expected = EvaluateExpression(*plan.ValueExpression(), row);
      for (const auto &relationship : graph_->Relationships()) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        const auto found = relationship->properties.find(plan.PropertyKey());
        if (found == relationship->properties.end() ||
            found->second != expected) {
          continue;
        }
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &out);
      }
    }
    return out;
  }

  Rows ExecuteRelationshipIndexRangeSeek(
      const ir::RelationshipIndexRangeSeekPlan &plan, const Rows &input) {
    Rows out;
    for (const auto &row : input) {
      for (const auto &relationship : graph_->Relationships()) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        Rows candidate_rows;
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &candidate_rows);
        for (QueryRow &candidate : candidate_rows) {
          bool keep = true;
          for (const ast::Expression *predicate : plan.Predicates()) {
            CHECK(predicate != nullptr, common::InvalidArgumentError,
                  "index range predicate is null");
            keep = keep && IsTruthy(EvaluateExpression(*predicate, candidate));
          }
          if (keep) {
            out.push_back(std::move(candidate));
          }
        }
      }
    }
    return out;
  }

  void AddRelationshipRow(const QueryRow &row, const Relationship &relationship,
                          const std::string &from_node,
                          const std::string &relationship_variable,
                          const std::string &to_node,
                          ir::ExpandDirection direction, Rows *out) {
    CHECK(out != nullptr, common::InternalError, "row output is null");
    if (direction == ir::ExpandDirection::kBoth) {
      AddDirectedRelationshipRow(
          row, relationship, from_node, relationship_variable, to_node,
          relationship.start_node_id, relationship.end_node_id, out);
      if (relationship.start_node_id != relationship.end_node_id) {
        AddDirectedRelationshipRow(
            row, relationship, from_node, relationship_variable, to_node,
            relationship.end_node_id, relationship.start_node_id, out);
      }
      return;
    }
    const std::int64_t from_id = direction == ir::ExpandDirection::kOutgoing
                                     ? relationship.start_node_id
                                     : relationship.end_node_id;
    const std::int64_t to_id = direction == ir::ExpandDirection::kOutgoing
                                   ? relationship.end_node_id
                                   : relationship.start_node_id;
    AddDirectedRelationshipRow(row, relationship, from_node,
                               relationship_variable, to_node, from_id, to_id,
                               out);
  }

  void AddDirectedRelationshipRow(const QueryRow &row,
                                  const Relationship &relationship,
                                  const std::string &from_node,
                                  const std::string &relationship_variable,
                                  const std::string &to_node,
                                  std::int64_t from_id, std::int64_t to_id,
                                  Rows *out) {
    QueryRow next = row;
    if (!TryBind(&next, from_node, Value(graph_->NodeById(from_id)))) {
      return;
    }
    if (!TryBind(&next, relationship_variable,
                 Value(graph_->RelationshipById(relationship.id)))) {
      return;
    }
    if (!TryBind(&next, to_node, Value(graph_->NodeById(to_id)))) {
      return;
    }
    out->push_back(std::move(next));
  }

  Rows ExecuteExpand(const ir::ExpandPlan &plan, const Rows &input) {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      CHECK(from.IsNode(), common::InvalidArgumentError,
            "expand source is not a node: " + plan.FromNode());
      const std::int64_t from_id = from.AsNode().id;
      for (const auto &relationship : graph_->Relationships()) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        if (plan.Direction() == ir::ExpandDirection::kOutgoing &&
            relationship->start_node_id != from_id) {
          continue;
        }
        if (plan.Direction() == ir::ExpandDirection::kIncoming &&
            relationship->end_node_id != from_id) {
          continue;
        }
        if (plan.Direction() == ir::ExpandDirection::kBoth &&
            relationship->start_node_id != from_id &&
            relationship->end_node_id != from_id) {
          continue;
        }
        const std::int64_t to_id = relationship->start_node_id == from_id
                                       ? relationship->end_node_id
                                       : relationship->start_node_id;
        QueryRow next = row;
        if (!TryBind(&next, plan.Relationship(),
                     Value(graph_->RelationshipById(relationship->id)))) {
          continue;
        }
        if (!TryBind(&next, plan.ToNode(), Value(graph_->NodeById(to_id)))) {
          continue;
        }
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Rows ExecuteExpandInto(const ir::ExpandIntoPlan &plan, const Rows &input) {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      const Value &to = LookupVariable(row, plan.ToNode());
      CHECK(from.IsNode() && to.IsNode(), common::InvalidArgumentError,
            "expand-into endpoints must be nodes");
      for (const auto &relationship : graph_->Relationships()) {
        if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        bool match = false;
        if (plan.Direction() == ir::ExpandDirection::kOutgoing) {
          match = relationship->start_node_id == from.AsNode().id &&
                  relationship->end_node_id == to.AsNode().id;
        } else if (plan.Direction() == ir::ExpandDirection::kIncoming) {
          match = relationship->end_node_id == from.AsNode().id &&
                  relationship->start_node_id == to.AsNode().id;
        } else {
          match = (relationship->start_node_id == from.AsNode().id &&
                   relationship->end_node_id == to.AsNode().id) ||
                  (relationship->end_node_id == from.AsNode().id &&
                   relationship->start_node_id == to.AsNode().id);
        }
        if (!match) {
          continue;
        }
        QueryRow next = row;
        if (TryBind(&next, plan.Relationship(),
                    Value(graph_->RelationshipById(relationship->id)))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteVarExpand(const ir::VarExpandPlan &plan, const Rows &input) {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      CHECK(from.IsNode(), common::InvalidArgumentError,
            "variable expand source is not a node: " + plan.FromNode());

      std::optional<std::int64_t> bound_to_id;
      const auto to_found = row.find(plan.ToNode());
      if (to_found != row.end()) {
        CHECK(to_found->second.IsNode(), common::InvalidArgumentError,
              "variable expand target is not a node: " + plan.ToNode());
        bound_to_id = to_found->second.AsNode().id;
      }

      const std::size_t min_length = VarExpandMinLength(plan.Length());
      const std::size_t max_length = VarExpandMaxLength(plan.Length());
      if (max_length < min_length) {
        continue;
      }

      std::vector<InMemoryGraph::RelationshipPtr> path;
      std::unordered_set<std::int64_t> used_relationships;
      ExpandVariableLengthPath(plan, row, from.AsNode().id, bound_to_id,
                               min_length, max_length, &path,
                               &used_relationships, &out);
    }
    return out;
  }

  std::size_t VarExpandMinLength(
      const ir::LogicalVariableLength &length) const {
    if (!length.min.has_value()) {
      return 1;
    }
    CHECK(*length.min >= 0, common::InvalidArgumentError,
          "variable expand minimum length is negative");
    return static_cast<std::size_t>(*length.min);
  }

  std::size_t VarExpandMaxLength(
      const ir::LogicalVariableLength &length) const {
    if (!length.max.has_value()) {
      return graph_->Relationships().size();
    }
    CHECK(*length.max >= 0, common::InvalidArgumentError,
          "variable expand maximum length is negative");
    return static_cast<std::size_t>(*length.max);
  }

  std::optional<std::int64_t> NextVarExpandNode(
      const Relationship &relationship, std::int64_t current_node_id,
      ir::ExpandDirection direction) const {
    if (direction == ir::ExpandDirection::kOutgoing) {
      if (relationship.start_node_id != current_node_id) {
        return std::nullopt;
      }
      return relationship.end_node_id;
    }
    if (direction == ir::ExpandDirection::kIncoming) {
      if (relationship.end_node_id != current_node_id) {
        return std::nullopt;
      }
      return relationship.start_node_id;
    }
    if (relationship.start_node_id == current_node_id) {
      return relationship.end_node_id;
    }
    if (relationship.end_node_id == current_node_id) {
      return relationship.start_node_id;
    }
    return std::nullopt;
  }

  void ExpandVariableLengthPath(
      const ir::VarExpandPlan &plan, const QueryRow &row,
      std::int64_t current_node_id, std::optional<std::int64_t> bound_to_id,
      std::size_t min_length, std::size_t max_length,
      std::vector<InMemoryGraph::RelationshipPtr> *path,
      std::unordered_set<std::int64_t> *used_relationships, Rows *out) {
    CHECK(path != nullptr, common::InternalError,
          "variable expand path is null");
    CHECK(used_relationships != nullptr, common::InternalError,
          "variable expand used relationship set is null");
    CHECK(out != nullptr, common::InternalError,
          "variable expand output is null");

    if (path->size() >= min_length &&
        (!bound_to_id.has_value() || *bound_to_id == current_node_id)) {
      EmitVarExpandRow(plan, row, current_node_id, *path, out);
    }
    if (path->size() == max_length) {
      return;
    }

    for (const auto &relationship : graph_->Relationships()) {
      if (used_relationships->contains(relationship->id)) {
        continue;
      }
      if (!RuntimeRelationshipHasAnyType(*relationship, plan.Types())) {
        continue;
      }
      std::optional<std::int64_t> next_node_id =
          NextVarExpandNode(*relationship, current_node_id, plan.Direction());
      if (!next_node_id.has_value()) {
        continue;
      }

      used_relationships->insert(relationship->id);
      path->push_back(relationship);
      ExpandVariableLengthPath(plan, row, *next_node_id, bound_to_id,
                               min_length, max_length, path, used_relationships,
                               out);
      path->pop_back();
      used_relationships->erase(relationship->id);
    }
  }

  void EmitVarExpandRow(const ir::VarExpandPlan &plan, const QueryRow &row,
                        std::int64_t current_node_id,
                        const std::vector<InMemoryGraph::RelationshipPtr> &path,
                        Rows *out) {
    CHECK(out != nullptr, common::InternalError,
          "variable expand output is null");
    Value::List relationships;
    relationships.reserve(path.size());
    for (const auto &relationship : path) {
      CHECK(relationship != nullptr, common::InternalError,
            "variable expand relationship is null");
      relationships.emplace_back(relationship);
    }

    QueryRow next = row;
    if (!TryBind(&next, plan.Relationship(), Value(std::move(relationships)))) {
      return;
    }
    if (!TryBind(&next, plan.ToNode(),
                 Value(graph_->NodeById(current_node_id)))) {
      return;
    }
    out->push_back(std::move(next));
  }

  Rows ExecutePathBuild(const ir::PathBuildPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    out.reserve(rows.size());
    for (const auto &row : rows) {
      QueryRow next = row;
      if (TryBind(&next, plan.PathVariable(),
                  BuildPathValue(plan.Path(), row))) {
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Value BuildPathValue(const ir::PathPattern &pattern, const QueryRow &row) {
    CHECK(!pattern.nodes.empty(), common::InvalidArgumentError,
          "path has no nodes: " + pattern.variable);
    CHECK(
        pattern.nodes.size() == pattern.relationships.size() + 1,
        common::InvalidArgumentError,
        "path node and relationship counts do not match: " + pattern.variable);

    auto path = std::make_shared<Path>();
    path->nodes.reserve(pattern.nodes.size());
    path->relationships.reserve(pattern.relationships.size());

    const Value &start_node = LookupVariable(row, pattern.nodes.front());
    CHECK(start_node.IsNode(), common::InvalidArgumentError,
          "path node is not a node: " + pattern.nodes.front());
    std::int64_t current_node_id = start_node.AsNode().id;
    path->nodes.push_back(graph_->NodeById(current_node_id));

    for (std::size_t index = 0; index < pattern.relationships.size(); ++index) {
      const std::string &relationship_variable = pattern.relationships[index];
      const Value &relationship = LookupVariable(row, relationship_variable);
      std::vector<InMemoryGraph::RelationshipPtr> relationships;
      if (relationship.IsList()) {
        relationships.reserve(relationship.AsList().size());
        for (const auto &item : relationship.AsList()) {
          CHECK(item.IsRelationship(), common::InvalidArgumentError,
                "path relationship list item is not a relationship: " +
                    relationship_variable);
          relationships.push_back(
              graph_->RelationshipById(item.AsRelationship().id));
        }
      } else {
        CHECK(relationship.IsRelationship(), common::InvalidArgumentError,
              "path relationship is not a relationship: " +
                  relationship_variable);
        relationships.push_back(
            graph_->RelationshipById(relationship.AsRelationship().id));
      }

      const Value &target_node = LookupVariable(row, pattern.nodes[index + 1]);
      CHECK(target_node.IsNode(), common::InvalidArgumentError,
            "path node is not a node: " + pattern.nodes[index + 1]);
      const std::int64_t target_node_id = target_node.AsNode().id;

      if (!CanTraverseRelationshipSequence(relationships, current_node_id,
                                           target_node_id)) {
        std::vector<InMemoryGraph::RelationshipPtr> reversed(
            relationships.rbegin(), relationships.rend());
        CHECK(CanTraverseRelationshipSequence(reversed, current_node_id,
                                              target_node_id),
              common::InvalidArgumentError,
              "path relationship sequence does not connect nodes: " +
                  relationship_variable);
        relationships = std::move(reversed);
      }

      for (const auto &item : relationships) {
        AppendPathRelationship(*item, path.get(), &current_node_id);
      }
      CHECK(current_node_id == target_node_id, common::InvalidArgumentError,
            "path node does not match traversed relationship endpoint: " +
                pattern.nodes[index + 1]);
    }
    return Value(std::move(path));
  }

  bool CanTraverseRelationshipSequence(
      const std::vector<InMemoryGraph::RelationshipPtr> &relationships,
      std::int64_t start_node_id, std::int64_t target_node_id) const {
    std::int64_t current_node_id = start_node_id;
    for (const auto &relationship : relationships) {
      CHECK(relationship != nullptr, common::InternalError,
            "path relationship is null");
      if (relationship->start_node_id == current_node_id) {
        current_node_id = relationship->end_node_id;
      } else if (relationship->end_node_id == current_node_id) {
        current_node_id = relationship->start_node_id;
      } else {
        return false;
      }
    }
    return current_node_id == target_node_id;
  }

  void AppendPathRelationship(const Relationship &relationship, Path *path,
                              std::int64_t *current_node_id) {
    CHECK(path != nullptr, common::InternalError, "path is null");
    CHECK(current_node_id != nullptr, common::InternalError,
          "path current node id is null");
    path->relationships.push_back(graph_->RelationshipById(relationship.id));
    if (relationship.start_node_id == *current_node_id) {
      *current_node_id = relationship.end_node_id;
    } else if (relationship.end_node_id == *current_node_id) {
      *current_node_id = relationship.start_node_id;
    } else {
      THROW(common::InvalidArgumentError,
            "path relationship is not connected to current node");
    }
    path->nodes.push_back(graph_->NodeById(*current_node_id));
  }

  Rows ExecuteFilter(const ir::FilterPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (auto &row : rows) {
      if (IsTruthy(EvaluateExpression(*plan.Predicate(), row,
                                      plan.PrecomputedExpressions()))) {
        out.push_back(std::move(row));
      }
    }
    return out;
  }

  Rows ExecuteProjection(const ir::ProjectionPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    out.reserve(rows.size());
    for (const auto &row : rows) {
      QueryRow projected;
      for (const auto &item : plan.Items()) {
        if (item.passthrough) {
          const auto found = row.find(item.alias);
          CHECK(found != row.end(), common::InvalidArgumentError,
                "passthrough projection variable is not bound: " + item.alias);
          projected[item.alias] = found->second;
        } else {
          projected[item.alias] = EvaluateLogicalProjectionItem(item, row);
        }
      }
      out.push_back(std::move(projected));
    }
    return out;
  }

  Rows ExecuteDistinct(const ir::DistinctPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    std::set<std::string> seen;
    for (const auto &row : rows) {
      QueryRow projected;
      std::string key;
      for (const auto &item : plan.GroupingItems()) {
        Value value = EvaluateLogicalProjectionItem(item, row);
        key += item.alias;
        key += '=';
        key += ValueKey(value);
        key += '\n';
        projected[item.alias] = std::move(value);
      }
      if (seen.insert(std::move(key)).second) {
        out.push_back(std::move(projected));
      }
    }
    return out;
  }

  Rows ExecuteAggregation(const ir::AggregationPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    struct GroupState {
      QueryRow row;
      Rows rows;
    };

    std::map<std::string, GroupState> groups;
    if (plan.GroupingItems().empty()) {
      groups.emplace("", GroupState{});
    }

    for (const auto &row : rows) {
      QueryRow group_row;
      std::string key;
      for (const auto &item : plan.GroupingItems()) {
        Value value = EvaluateLogicalProjectionItem(item, row);
        key += item.alias;
        key += '=';
        key += ValueKey(value);
        key += '\n';
        group_row[item.alias] = std::move(value);
      }
      auto [it, inserted] = groups.emplace(key, GroupState{group_row, {}});
      (void)inserted;
      it->second.rows.push_back(row);
    }

    Rows out;
    for (auto &[key, state] : groups) {
      (void)key;
      QueryRow row = std::move(state.row);
      for (const auto &item : plan.AggregationItems()) {
        CHECK(item.expression != nullptr, common::InvalidArgumentError,
              "aggregation expression is null");
        row[item.alias] = EvaluateAggregationExpression(
            *item.expression, state.rows, item.precomputed_expressions);
      }
      out.push_back(std::move(row));
    }
    return out;
  }

  Value EvaluateAggregationExpression(
      const ast::Expression &expression, const Rows &rows,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed) const {
    if (expression.Is(ast::ASTNodeType::kCountStarExpression)) {
      return Value(static_cast<std::int64_t>(rows.size()));
    }
    if (!expression.Is(ast::ASTNodeType::kFunctionInvocation)) {
      THROW(common::InvalidArgumentError,
            "unsupported aggregation expression: " +
                ast::ExpressionToString(expression));
    }

    const auto &function = ast::CastAst<ast::FunctionInvocation>(expression);
    const std::string name = LowerAscii(function.function_name);
    if (name == "count") {
      return EvaluateCountAggregation(function, rows, precomputed);
    }
    if (name == "collect") {
      return EvaluateCollectAggregation(function, rows, precomputed);
    }
    if (name == "sum") {
      return EvaluateSumAggregation(function, rows, precomputed);
    }
    if (name == "avg") {
      return EvaluateAvgAggregation(function, rows, precomputed);
    }
    if (name == "min") {
      return EvaluateMinMaxAggregation(function, rows, precomputed, true);
    }
    if (name == "max") {
      return EvaluateMinMaxAggregation(function, rows, precomputed, false);
    }

    THROW(common::InvalidArgumentError,
          "unsupported aggregation expression: " +
              ast::ExpressionToString(expression));
  }

  std::vector<Value> EvaluateAggregationValues(
      const ast::FunctionInvocation &function, const Rows &rows,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed) const {
    CHECK(function.arguments.size() == 1, common::InvalidArgumentError,
          function.function_name + "() expects one argument");
    CHECK(function.arguments[0] != nullptr, common::InvalidArgumentError,
          function.function_name + "() argument is null");

    std::vector<Value> values;
    values.reserve(rows.size());
    std::set<std::string> seen;
    for (const auto &row : rows) {
      Value value =
          EvaluateExpression(*function.arguments[0], row, precomputed);
      if (value.IsNull()) {
        continue;
      }
      if (function.distinct && !seen.insert(ValueKey(value)).second) {
        continue;
      }
      values.push_back(std::move(value));
    }
    return values;
  }

  Value EvaluateCountAggregation(
      const ast::FunctionInvocation &function, const Rows &rows,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed) const {
    return Value(static_cast<std::int64_t>(
        EvaluateAggregationValues(function, rows, precomputed).size()));
  }

  Value EvaluateCollectAggregation(
      const ast::FunctionInvocation &function, const Rows &rows,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed) const {
    return Value(
        Value::List(EvaluateAggregationValues(function, rows, precomputed)));
  }

  Value EvaluateSumAggregation(
      const ast::FunctionInvocation &function, const Rows &rows,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed) const {
    std::vector<Value> values =
        EvaluateAggregationValues(function, rows, precomputed);
    bool integral = true;
    std::int64_t integer_sum = 0;
    double double_sum = 0.0;
    for (const Value &value : values) {
      CHECK(IsNumeric(value), common::InvalidArgumentError,
            function.function_name + "() expects numeric values");
      if (value.IsInteger()) {
        integer_sum += value.AsInteger();
        double_sum += static_cast<double>(value.AsInteger());
      } else {
        integral = false;
        double_sum += value.AsDouble();
      }
    }
    return integral ? Value(integer_sum) : Value(double_sum);
  }

  Value EvaluateAvgAggregation(
      const ast::FunctionInvocation &function, const Rows &rows,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed) const {
    std::vector<Value> values =
        EvaluateAggregationValues(function, rows, precomputed);
    if (values.empty()) {
      return Value::Null();
    }
    double sum = 0.0;
    for (const Value &value : values) {
      CHECK(IsNumeric(value), common::InvalidArgumentError,
            function.function_name + "() expects numeric values");
      sum += AsDoubleValue(value);
    }
    return Value(sum / static_cast<double>(values.size()));
  }

  Value EvaluateMinMaxAggregation(
      const ast::FunctionInvocation &function, const Rows &rows,
      const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
      bool min) const {
    std::vector<Value> values =
        EvaluateAggregationValues(function, rows, precomputed);
    if (values.empty()) {
      return Value::Null();
    }
    Value best = values.front();
    for (std::size_t index = 1; index < values.size(); ++index) {
      const Value &candidate = values[index];
      if ((min && ValueLess(candidate, best)) ||
          (!min && ValueLess(best, candidate))) {
        best = candidate;
      }
    }
    return best;
  }

  Rows ExecuteSort(const ir::SortPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    std::stable_sort(rows.begin(), rows.end(),
                     [&plan](const QueryRow &left, const QueryRow &right) {
                       for (const auto &item : plan.Items()) {
                         Value lhs = EvaluateLogicalSortItem(item, left);
                         Value rhs = EvaluateLogicalSortItem(item, right);
                         if (lhs == rhs) {
                           continue;
                         }
                         const bool less = ValueLess(lhs, rhs);
                         return item.direction ==
                                        ir::LogicalOrderDirection::kAscending
                                    ? less
                                    : !less;
                       }
                       return false;
                     });
    return rows;
  }

  Rows ExecuteSkip(const ir::SkipPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    if (rows.empty()) {
      return rows;
    }
    const std::size_t skip = static_cast<std::size_t>(std::max<double>(
        0.0,
        EvaluateExpression(*plan.Skip(), rows[0], plan.PrecomputedExpressions())
            .AsInteger()));
    if (skip >= rows.size()) {
      return {};
    }
    return Rows(rows.begin() + static_cast<std::ptrdiff_t>(skip), rows.end());
  }

  Rows ExecuteLimit(const ir::LimitPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    if (rows.empty()) {
      return rows;
    }
    const std::size_t limit = static_cast<std::size_t>(
        std::max<double>(0.0, EvaluateExpression(*plan.Limit(), rows[0],
                                                 plan.PrecomputedExpressions())
                                  .AsInteger()));
    if (limit < rows.size()) {
      rows.resize(limit);
    }
    return rows;
  }

  Rows ExecuteJoin(const ir::LogicalPlan &plan, const Rows &input) {
    Rows out;
    for (const auto &base : input) {
      Rows left_rows = ExecutePlan(plan.Child(0), Rows{base});
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{base});
      for (const auto &left : left_rows) {
        for (const auto &right : right_rows) {
          QueryRow merged;
          if (!MergeRows(left, right, &merged)) {
            continue;
          }
          if (!JoinPredicateMatches(plan, merged)) {
            continue;
          }
          out.push_back(std::move(merged));
        }
      }
    }
    return out;
  }

  bool JoinPredicateMatches(const ir::LogicalPlan &plan, const QueryRow &row) {
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kCartesianProduct:
        return true;
      case ir::LogicalPlanNodeType::kNodeHashJoin:
        return true;
      case ir::LogicalPlanNodeType::kValueHashJoin: {
        const auto &join = static_cast<const ir::ValueHashJoinPlan &>(plan);
        for (const ast::Expression *predicate : join.Predicates()) {
          if (!IsTruthy(EvaluateExpression(*predicate, row))) {
            return false;
          }
        }
        return true;
      }
      case ir::LogicalPlanNodeType::kPredicateJoin: {
        const auto &join = static_cast<const ir::PredicateJoinPlan &>(plan);
        for (const ast::Expression *predicate : join.Predicates()) {
          if (!IsTruthy(EvaluateExpression(*predicate, row))) {
            return false;
          }
        }
        return true;
      }
      default:
        break;
    }
    THROW(common::InternalError, "unknown join plan");
  }

  Rows ExecuteApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      for (const auto &row : right_rows) {
        QueryRow merged;
        if (MergeRows(left, row, &merged)) {
          out.push_back(std::move(merged));
        }
      }
    }
    return out;
  }

  Rows ExecuteSemiApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      if (!right_rows.empty()) {
        out.push_back(left);
      }
    }
    return out;
  }

  Rows ExecuteAntiSemiApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      if (right_rows.empty()) {
        out.push_back(left);
      }
    }
    return out;
  }

  Rows ExecuteLetSemiApply(const ir::LetSemiApplyPlan &plan,
                           const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      QueryRow merged = left;
      merged[plan.ValueVariable()] = Value(!right_rows.empty());
      out.push_back(std::move(merged));
    }
    return out;
  }

  Rows ExecuteRollUpApply(const ir::RollUpApplyPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      QueryRow merged = left;
      Value::List list;
      for (const auto &row : right_rows) {
        const auto found = row.find(plan.ValueVariable());
        if (found != row.end()) {
          list.push_back(found->second);
        }
      }
      merged[plan.CollectionVariable()] = Value(std::move(list));
      out.push_back(std::move(merged));
    }
    return out;
  }

  Rows ExecuteOptionalApply(const ir::LogicalPlan &plan, const Rows &input) {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      if (right_rows.empty()) {
        QueryRow null_extended = left;
        for (const auto &column : plan.Child(1).OutputColumns()) {
          if (null_extended.find(column) == null_extended.end()) {
            null_extended[column] = Value::Null();
          }
        }
        out.push_back(std::move(null_extended));
        continue;
      }
      for (const auto &row : right_rows) {
        QueryRow merged;
        if (MergeRows(left, row, &merged)) {
          out.push_back(std::move(merged));
        }
      }
    }
    return out;
  }

  Rows ExecuteUnwind(const ir::UnwindPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      Value list = EvaluateExpression(*plan.Expression(), row);
      if (!list.IsList()) {
        continue;
      }
      for (const auto &item : list.AsList()) {
        QueryRow next = row;
        next[plan.Alias()] = item;
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Rows ExecuteProcedureCall(const ir::ProcedureCallPlan &plan,
                            const Rows &input) {
    CHECK(plan.ReadOnly(), common::InvalidArgumentError,
          "write procedure calls are not supported");
    const std::string procedure_name = LowerAscii(plan.ProcedureName());
    if (procedure_name == "db.labels") {
      return ExecuteMetadataProcedure(plan, input, "label", CollectLabels());
    }
    if (procedure_name == "db.relationshiptypes") {
      return ExecuteMetadataProcedure(plan, input, "relationshipType",
                                      CollectRelationshipTypes());
    }
    if (procedure_name == "db.propertykeys") {
      return ExecuteMetadataProcedure(plan, input, "propertyKey",
                                      CollectPropertyKeys());
    }

    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      QueryRow next = row;
      for (const auto &item : plan.YieldItems()) {
        if (item.variable.empty()) {
          continue;
        }
        next[item.variable] = Value::Null();
      }
      out.push_back(std::move(next));
    }
    return out;
  }

  std::set<std::string> CollectLabels() const {
    std::set<std::string> labels;
    for (const auto &node : graph_->Nodes()) {
      for (const auto &label : node->labels) {
        labels.insert(label);
      }
    }
    return labels;
  }

  std::set<std::string> CollectRelationshipTypes() const {
    std::set<std::string> types;
    for (const auto &relationship : graph_->Relationships()) {
      if (!relationship->type.empty()) {
        types.insert(relationship->type);
      }
    }
    return types;
  }

  std::set<std::string> CollectPropertyKeys() const {
    std::set<std::string> keys;
    for (const auto &node : graph_->Nodes()) {
      for (const auto &[key, value] : node->properties) {
        (void)value;
        keys.insert(key);
      }
    }
    for (const auto &relationship : graph_->Relationships()) {
      for (const auto &[key, value] : relationship->properties) {
        (void)value;
        keys.insert(key);
      }
    }
    return keys;
  }

  Rows ExecuteMetadataProcedure(const ir::ProcedureCallPlan &plan,
                                const Rows &input, std::string_view field_name,
                                const std::set<std::string> &values) {
    CHECK(plan.Arguments().empty(), common::InvalidArgumentError,
          plan.ProcedureName() + "() expects no arguments");

    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      for (const auto &value : values) {
        QueryRow next = row;
        for (const auto &item : plan.YieldItems()) {
          if (item.variable.empty()) {
            continue;
          }
          const std::string field = item.result_field.value_or(item.variable);
          CHECK(
              field == field_name, common::InvalidArgumentError,
              "unsupported " + plan.ProcedureName() + " yield field: " + field);
          next[item.variable] = Value(value);
        }
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Rows ExecuteUnion(const ir::UnionPlan &plan, const Rows &input) {
    Rows out;
    std::set<std::string> seen;

    auto append_rows = [&](const Rows &rows, bool left_side) {
      for (const auto &row : rows) {
        QueryRow mapped;
        std::string key;
        for (const auto &mapping : plan.Mappings()) {
          const std::string &source =
              left_side ? mapping.lhs_variable : mapping.rhs_variable;
          const auto found = row.find(source);
          CHECK(found != row.end(), common::InvalidArgumentError,
                "UNION source variable is not bound: " + source);
          mapped[mapping.output_variable] = found->second;
          if (!plan.All()) {
            key += mapping.output_variable;
            key += '=';
            key += ValueKey(found->second);
            key += '\n';
          }
        }
        if (plan.All() || seen.insert(std::move(key)).second) {
          out.push_back(std::move(mapped));
        }
      }
    };

    append_rows(ExecutePlan(plan.Child(0), input), true);
    append_rows(ExecutePlan(plan.Child(1), input), false);
    return out;
  }

  Rows ExecuteCreateNode(const ir::CreateNodePlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      Value::Map properties;
      for (const auto &entry : plan.Node().properties.entries) {
        CHECK(entry.value != nullptr, common::InvalidArgumentError,
              "CREATE node property value is null");
        properties[entry.key] = EvaluateExpression(*entry.value, row);
      }
      auto node = graph_->CreateNode(plan.Node().labels, std::move(properties));
      QueryRow next = row;
      next[plan.Node().variable] = Value(node);
      out.push_back(std::move(next));
    }
    return out;
  }

  Rows ExecuteCreateRelationship(const ir::CreateRelationshipPlan &plan,
                                 const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      const Value &left = LookupVariable(row, plan.Relationship().left_node);
      const Value &right = LookupVariable(row, plan.Relationship().right_node);
      CHECK(left.IsNode() && right.IsNode(), common::InvalidArgumentError,
            "CREATE relationship endpoints must be nodes");
      Value::Map properties;
      for (const auto &entry : plan.Relationship().properties.entries) {
        CHECK(entry.value != nullptr, common::InvalidArgumentError,
              "CREATE relationship property value is null");
        properties[entry.key] = EvaluateExpression(*entry.value, row);
      }
      auto relationship = graph_->CreateRelationship(
          left.AsNode().id, right.AsNode().id,
          plan.Relationship().types.empty() ? std::string()
                                            : plan.Relationship().types[0],
          std::move(properties));
      QueryRow next = row;
      next[plan.Relationship().variable] = Value(relationship);
      out.push_back(std::move(next));
    }
    return out;
  }

  Rows ExecuteMerge(const ir::MergePlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : rows) {
      Rows matches = ExecutePlan(plan.Child(1), Rows{row});
      if (!matches.empty()) {
        for (auto &match : matches) {
          QueryRow next;
          if (!MergeRows(row, match, &next)) {
            continue;
          }
          for (const auto &action : plan.Merge().actions) {
            if (action.on_match) {
              ExecuteSetPatterns(action.set_patterns, &next);
            }
          }
          out.push_back(std::move(next));
        }
        continue;
      }

      QueryRow next = row;
      { next = ExecuteMergeCreate(plan.Merge().create_pattern, next); }
      for (const auto &action : plan.Merge().actions) {
        if (!action.on_match) {
          ExecuteSetPatterns(action.set_patterns, &next);
        }
      }
      out.push_back(std::move(next));
    }
    return out;
  }

  QueryRow ExecuteMergeCreate(const ir::CreatePattern &pattern, QueryRow row) {
    for (const auto &command : pattern.commands) {
      if (command.kind == ir::CreateEntityKind::kNode) {
        const auto &node_pattern = pattern.nodes.at(command.index);
        Value::Map properties;
        for (const auto &entry : node_pattern.properties.entries) {
          CHECK(entry.value != nullptr, common::InvalidArgumentError,
                "MERGE node property value is null");
          properties[entry.key] = EvaluateExpression(*entry.value, row);
        }
        auto node =
            graph_->CreateNode(node_pattern.labels, std::move(properties));
        row[node_pattern.variable] = Value(node);
      } else {
        const auto &relationship_pattern =
            pattern.relationships.at(command.index);
        const Value &left = LookupVariable(row, relationship_pattern.left_node);
        const Value &right =
            LookupVariable(row, relationship_pattern.right_node);
        CHECK(left.IsNode() && right.IsNode(), common::InvalidArgumentError,
              "MERGE relationship endpoints must be nodes");
        Value::Map properties;
        for (const auto &entry : relationship_pattern.properties.entries) {
          CHECK(entry.value != nullptr, common::InvalidArgumentError,
                "MERGE relationship property value is null");
          properties[entry.key] = EvaluateExpression(*entry.value, row);
        }
        auto relationship = graph_->CreateRelationship(
            left.AsNode().id, right.AsNode().id,
            relationship_pattern.types.empty() ? std::string()
                                               : relationship_pattern.types[0],
            std::move(properties));
        row[relationship_pattern.variable] = Value(relationship);
      }
    }
    return row;
  }

  void ExecuteSetPatterns(const std::vector<ir::SetMutatingPattern> &patterns,
                          QueryRow *row) {
    CHECK(row != nullptr, common::InternalError, "query row is null");
    for (const auto &pattern : patterns) {
      switch (pattern.kind) {
        case ir::SetMutatingPatternKind::kSetProperty:
          ApplySetProperty(pattern, row);
          break;
        case ir::SetMutatingPatternKind::kSetExactPropertiesFromMap:
          ApplySetProperties(pattern, row, false);
          break;
        case ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap:
          ApplySetProperties(pattern, row, true);
          break;
        case ir::SetMutatingPatternKind::kSetLabels:
          ApplySetLabels(pattern, row);
          break;
      }
    }
  }

  void ApplySetProperty(const ir::SetMutatingPattern &pattern, QueryRow *row) {
    const Value &entity = EvaluateExpression(*pattern.entity, *row);
    Value value = EvaluateExpression(*pattern.value, *row);
    if (entity.IsNode()) {
      graph_->SetNodeProperty(graph_->NodeById(entity.AsNode().id),
                              pattern.property_key, std::move(value));
      return;
    }
    if (entity.IsRelationship()) {
      graph_->SetRelationshipProperty(
          graph_->RelationshipById(entity.AsRelationship().id),
          pattern.property_key, std::move(value));
      return;
    }
    THROW(common::InvalidArgumentError, "SET property target is not an entity");
  }

  void ApplySetProperties(const ir::SetMutatingPattern &pattern, QueryRow *row,
                          bool include_existing) {
    const Value &entity = EvaluateExpression(*pattern.entity, *row);
    Value map_value = EvaluateExpression(*pattern.value, *row);
    CHECK(map_value.IsMap(), common::InvalidArgumentError,
          "SET properties requires a map value");
    if (entity.IsNode()) {
      graph_->SetNodeProperties(graph_->NodeById(entity.AsNode().id),
                                std::move(map_value.AsMap()), include_existing);
      return;
    }
    THROW(common::InvalidArgumentError, "SET properties target is not a node");
  }

  void ApplySetLabels(const ir::SetMutatingPattern &pattern, QueryRow *row) {
    const Value &entity = EvaluateExpression(*pattern.entity, *row);
    if (!entity.IsNode()) {
      THROW(common::InvalidArgumentError, "SET labels target is not a node");
    }
    graph_->SetLabels(graph_->NodeById(entity.AsNode().id), pattern.labels);
  }

  Rows ExecuteSetProperty(const ir::SetPropertyPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      ir::SetMutatingPattern pattern;
      pattern.kind = ir::SetMutatingPatternKind::kSetProperty;
      pattern.entity = plan.Entity();
      pattern.property_key = plan.PropertyKey();
      pattern.value = plan.Value();
      ExecuteSetPatterns({pattern}, &row);
    }
    return rows;
  }

  Rows ExecuteSetProperties(const ir::SetPropertiesPlan &plan,
                            const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      ir::SetMutatingPattern pattern;
      pattern.kind =
          plan.IncludeExisting()
              ? ir::SetMutatingPatternKind::kSetIncludingPropertiesFromMap
              : ir::SetMutatingPatternKind::kSetExactPropertiesFromMap;
      pattern.entity = plan.Entity();
      pattern.value = plan.Value();
      ExecuteSetPatterns({pattern}, &row);
    }
    return rows;
  }

  Rows ExecuteSetLabels(const ir::SetLabelsPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      ir::SetMutatingPattern pattern;
      pattern.kind = ir::SetMutatingPatternKind::kSetLabels;
      pattern.entity = plan.Entity();
      pattern.labels = plan.Labels();
      ExecuteSetPatterns({pattern}, &row);
    }
    return rows;
  }

  Rows ExecuteRemoveProperty(const ir::RemovePropertyPlan &plan,
                             const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      const Value &entity = EvaluateExpression(*plan.Entity(), row);
      if (entity.IsNode()) {
        graph_->RemoveNodeProperty(graph_->NodeById(entity.AsNode().id),
                                   plan.PropertyKey());
      } else if (entity.IsRelationship()) {
        graph_->RemoveRelationshipProperty(
            graph_->RelationshipById(entity.AsRelationship().id),
            plan.PropertyKey());
      } else {
        THROW(common::InvalidArgumentError,
              "REMOVE property target is not an entity");
      }
    }
    return rows;
  }

  Rows ExecuteRemoveLabels(const ir::RemoveLabelsPlan &plan,
                           const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (auto &row : rows) {
      const Value &entity = EvaluateExpression(*plan.Entity(), row);
      if (!entity.IsNode()) {
        THROW(common::InvalidArgumentError,
              "REMOVE labels target is not a node");
      }
      graph_->RemoveLabels(graph_->NodeById(entity.AsNode().id), plan.Labels());
    }
    return rows;
  }

  Rows ExecuteDelete(const ir::LogicalPlan &plan, const Rows &input,
                     bool detach) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    for (const auto &row : rows) {
      std::vector<EntityRef> entities;
      if (plan.Type() == ir::LogicalPlanNodeType::kDelete) {
        const auto &del = static_cast<const ir::DeletePlan &>(plan);
        for (const auto *expression : del.Expressions()) {
          CHECK(expression != nullptr, common::InvalidArgumentError,
                "DELETE expression is null");
          Value value = EvaluateExpression(*expression, row);
          entities.push_back(MakeEntityRef(value));
        }
      } else {
        const auto &del = static_cast<const ir::DetachDeletePlan &>(plan);
        for (const auto *expression : del.Expressions()) {
          CHECK(expression != nullptr, common::InvalidArgumentError,
                "DETACH DELETE expression is null");
          Value value = EvaluateExpression(*expression, row);
          entities.push_back(MakeEntityRef(value));
        }
        detach = true;
      }
      for (const auto &entity : entities) {
        if (entity.kind == EntityRef::Kind::kRelationship) {
          graph_->DeleteRelationship(graph_->RelationshipById(entity.id));
          continue;
        }
        if (detach) {
          for (const auto &relationship :
               graph_->RelationshipsConnectedTo(entity.id)) {
            graph_->DeleteRelationship(relationship);
          }
        } else {
          CHECK(graph_->RelationshipsConnectedTo(entity.id).empty(),
                common::InvalidArgumentError,
                "DELETE node still has relationships");
        }
        graph_->DeleteNode(graph_->NodeById(entity.id));
      }
    }
    return rows;
  }

  InMemoryGraph *graph_ = nullptr;
};

}  // namespace

QueryResult QueryExecutor::Execute(const ir::LogicalPlan &plan) const {
  CHECK(graph_ != nullptr, common::InternalError, "graph is null");
  return QueryExecutorImpl(graph_).Execute(plan);
}

void QueryExecutor::ExecuteWrite(const ir::LogicalPlan &plan) {
  CHECK(graph_ != nullptr, common::InternalError, "graph is null");
  QueryExecutorImpl(graph_).ExecuteWrite(plan);
}

QueryResult ExecuteReadQuery(const InMemoryGraph &graph,
                             std::string_view cypher) {
  return ExecuteQuery(const_cast<InMemoryGraph &>(graph), cypher);
}

QueryResult ExecuteQuery(InMemoryGraph &graph, std::string_view cypher) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan = ir::CreateLogicalPlan(
      *planner_query, ir::LogicalPlanBuilderOptions{.planner_catalog = &graph});
  return QueryExecutor(graph).Execute(*logical_plan);
}

void ExecuteWriteQuery(InMemoryGraph &graph, std::string_view cypher) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan = ir::CreateLogicalPlan(
      *planner_query, ir::LogicalPlanBuilderOptions{.planner_catalog = &graph});
  QueryExecutor(graph).ExecuteWrite(*logical_plan);
}

}  // namespace rg
