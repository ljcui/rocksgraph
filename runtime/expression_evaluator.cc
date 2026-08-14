#include "runtime/expression_evaluator.h"

#include <algorithm>
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
#include "runtime/builtin_function_evaluator.h"
#include "runtime/query_row_util.h"
#include "value/temporal.h"

namespace rg {
namespace {

enum class TruthValue { kFalse, kTrue, kNull };
enum class QuantifierMode { kAll, kAny, kNone, kSingle };

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
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
  const ast::BuiltinFunction *builtin =
      ast::FindBuiltinFunction(function.function_name);
  CHECK(builtin != nullptr, common::InvalidArgumentError,
        "unknown function: " + function.function_name);
  CHECK(!builtin->aggregate, common::InvalidArgumentError,
        "aggregate function requires aggregation execution: " + builtin->name);
  CHECK(ast::BuiltinFunctionAcceptsArgumentCount(*builtin,
                                                 function.arguments.size()),
        common::InvalidArgumentError,
        ast::BuiltinFunctionArgumentCountError(*builtin));
  CHECK(!function.distinct, common::InvalidArgumentError,
        "DISTINCT is only supported for aggregate functions");

  std::vector<Value> arguments;
  arguments.reserve(function.arguments.size());
  for (const auto &argument : function.arguments) {
    CHECK(argument != nullptr, common::InvalidArgumentError,
          "function argument is null");
    arguments.push_back(
        EvaluateExpression(*argument, row, precomputed, context));
  }

  return EvaluateBuiltinFunction(builtin->kind, arguments, context.clock,
                                 context.graph_reader);
}

Value EvaluateListIndex(
    const ast::ListIndexExpression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
  CHECK(expression.list != nullptr && expression.index != nullptr,
        common::InvalidArgumentError, "list index expression is incomplete");
  Value list = EvaluateExpression(*expression.list, row, precomputed, context);
  Value index_value =
      EvaluateExpression(*expression.index, row, precomputed, context);
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
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
  CHECK(expression.list != nullptr, common::InvalidArgumentError,
        "list slice base expression is null");
  Value list = EvaluateExpression(*expression.list, row, precomputed, context);
  if (!list.IsList()) {
    return Value::Null();
  }
  const auto &items = list.AsList();
  std::int64_t start = 0;
  auto end = static_cast<std::int64_t>(items.size());
  if (expression.start_index != nullptr) {
    const auto value = IntegerValue(
        EvaluateExpression(*expression.start_index, row, precomputed, context));
    if (!value.has_value()) {
      return Value::Null();
    }
    start = ClampListSliceIndex(*value, items.size());
  }
  if (expression.end_index != nullptr) {
    const auto value = IntegerValue(
        EvaluateExpression(*expression.end_index, row, precomputed, context));
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
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
  std::optional<Value> test;
  if (expression.test != nullptr) {
    test = EvaluateExpression(*expression.test, row, precomputed, context);
  }
  for (const auto &[when_expression, then_expression] :
       expression.alternatives) {
    CHECK(when_expression != nullptr && then_expression != nullptr,
          common::InvalidArgumentError, "CASE alternative is incomplete");
    bool matched = false;
    if (test.has_value()) {
      const Value candidate =
          EvaluateExpression(*when_expression, row, precomputed, context);
      matched = EqualityTruth(*test, candidate) == TruthValue::kTrue;
    } else {
      matched = PredicateIsTrue(
          EvaluateExpression(*when_expression, row, precomputed, context));
    }
    if (matched) {
      return EvaluateExpression(*then_expression, row, precomputed, context);
    }
  }
  return expression.else_expr != nullptr
             ? EvaluateExpression(*expression.else_expr, row, precomputed,
                                  context)
             : Value::Null();
}

Value EvaluateListComprehension(
    const ast::ListComprehension &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
  CHECK(!expression.variable.empty() && expression.list_expr != nullptr,
        common::InvalidArgumentError, "list comprehension is incomplete");
  Value list =
      EvaluateExpression(*expression.list_expr, row, precomputed, context);
  if (!list.IsList()) {
    return Value::Null();
  }
  Value::List output;
  for (const auto &item : list.AsList()) {
    QueryRow scoped = row;
    scoped[expression.variable] = item;
    if (expression.where_expr != nullptr &&
        !PredicateIsTrue(EvaluateExpression(*expression.where_expr, scoped,
                                            precomputed, context))) {
      continue;
    }
    output.push_back(expression.eval_expr != nullptr
                         ? EvaluateExpression(*expression.eval_expr, scoped,
                                              precomputed, context)
                         : item);
  }
  return Value(std::move(output));
}

Value EvaluateQuantifier(
    const ast::Quantifier &quantifier, QuantifierMode mode, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
  CHECK(!quantifier.variable.empty() && quantifier.list_expr != nullptr &&
            quantifier.predicate != nullptr,
        common::InvalidArgumentError, "quantifier is incomplete");
  Value list =
      EvaluateExpression(*quantifier.list_expr, row, precomputed, context);
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
    const TruthValue truth = ToTruthValue(EvaluateExpression(
        *quantifier.predicate, scoped, precomputed, context));
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
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
  const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
  CHECK(binary.left != nullptr && binary.right != nullptr,
        common::InvalidArgumentError, "arithmetic expression is incomplete");
  Value left = EvaluateExpression(*binary.left, row, precomputed, context);
  Value right = EvaluateExpression(*binary.right, row, precomputed, context);
  if (expression.Is(ast::ASTNodeType::kAddExpression) && left.IsList()) {
    Value::List result = left.AsList();
    if (right.IsList()) {
      result.insert(result.end(), right.AsList().begin(), right.AsList().end());
    } else {
      result.push_back(std::move(right));
    }
    return Value(std::move(result));
  }
  if (expression.Is(ast::ASTNodeType::kAddExpression) && right.IsList()) {
    Value::List result;
    result.reserve(right.AsList().size() + 1);
    result.push_back(std::move(left));
    result.insert(result.end(), right.AsList().begin(), right.AsList().end());
    return Value(std::move(result));
  }
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
    if (expression.Is(ast::ASTNodeType::kDivideExpression)) {
      CHECK(rhs != 0, common::InvalidArgumentError, "division by zero");
      CHECK(!(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1),
            common::InvalidArgumentError, "integer division overflow");
      return Value(lhs / rhs);
    }
    CHECK(rhs != 0, common::InvalidArgumentError, "modulo by zero");
    CHECK(!(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1),
          common::InvalidArgumentError, "integer modulo overflow");
    return Value(lhs % rhs);
  }
  const double lhs = AsDoubleValue(left);
  const double rhs = AsDoubleValue(right);
  if (expression.Is(ast::ASTNodeType::kDivideExpression)) {
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
  return Value(std::fmod(lhs, rhs));
}

bool ValuesAreOrderComparable(const Value &left, const Value &right) {
  if (IsNumeric(left) && IsNumeric(right)) {
    return true;
  }
  return left.Type() == right.Type();
}

Value EvaluateOrderingComparison(const Value &left, const Value &right,
                                 std::string_view op) {
  if (!ValuesAreOrderComparable(left, right)) {
    return Value::Null();
  }
  if ((left.IsDouble() && std::isnan(left.AsDouble())) ||
      (right.IsDouble() && std::isnan(right.AsDouble()))) {
    return Value(false);
  }
  const TruthValue equality = EqualityTruth(left, right);
  if (op == "<") {
    return Value(ValueLess(left, right));
  }
  if (op == ">") {
    return Value(ValueLess(right, left));
  }
  if (equality == TruthValue::kNull) {
    return Value::Null();
  }
  const bool equal = equality == TruthValue::kTrue;
  if (op == "<=") {
    return Value(equal || ValueLess(left, right));
  }
  if (op == ">=") {
    return Value(equal || ValueLess(right, left));
  }
  THROW(common::InvalidArgumentError,
        "unsupported comparison operator: " + std::string(op));
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
                                    const QueryRow &row,
                                    ExecutionContext context) {
  CHECK(item.expression != nullptr, common::InvalidArgumentError,
        "projection expression is null");
  return EvaluateExpression(*item.expression, row, item.precomputed_expressions,
                            context);
}

Value EvaluateLogicalSortItem(const ir::LogicalSortItem &item,
                              const QueryRow &row, ExecutionContext context) {
  CHECK(item.expression != nullptr, common::InvalidArgumentError,
        "sort expression is null");
  return EvaluateExpression(*item.expression, row, item.precomputed_expressions,
                            context);
}

Value EvaluateExpression(
    const ast::Expression &expression, const QueryRow &row,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed,
    ExecutionContext context) {
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
    case ast::ASTNodeType::kParameter: {
      const auto &parameter = ast::CastAst<ast::Parameter>(expression);
      const Value *value = context.FindParameter(parameter.name);
      CHECK(value != nullptr, common::InvalidArgumentError,
            "missing query parameter: " + parameter.name);
      return *value;
    }
    case ast::ASTNodeType::kPropertyExpression: {
      const auto &property = ast::CastAst<ast::PropertyExpression>(expression);
      CHECK(property.object != nullptr, common::InvalidArgumentError,
            "property object is null");
      Value object =
          EvaluateExpression(*property.object, row, precomputed, context);
      const Value *value = FindProperty(object, property.property_key);
      if (value != nullptr) {
        return *value;
      }
      return TemporalProperty(object, property.property_key)
          .value_or(Value::Null());
    }
    case ast::ASTNodeType::kListIndexExpression:
      return EvaluateListIndex(
          ast::CastAst<ast::ListIndexExpression>(expression), row, precomputed,
          context);
    case ast::ASTNodeType::kListSliceExpression:
      return EvaluateListSlice(
          ast::CastAst<ast::ListSliceExpression>(expression), row, precomputed,
          context);
    case ast::ASTNodeType::kListLiteral: {
      Value::List values;
      for (const auto &element :
           ast::CastAst<ast::ListLiteral>(expression).elements) {
        CHECK(element != nullptr, common::InvalidArgumentError,
              "list element is null");
        values.push_back(
            EvaluateExpression(*element, row, precomputed, context));
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kMapLiteral: {
      Value::Map values;
      for (const auto &[key, value] :
           ast::CastAst<ast::MapLiteral>(expression).entries) {
        CHECK(value != nullptr, common::InvalidArgumentError,
              "map value is null");
        values[key] = EvaluateExpression(*value, row, precomputed, context);
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kAndExpression:
    case ast::ASTNodeType::kOrExpression:
    case ast::ASTNodeType::kXorExpression: {
      const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "boolean expression is incomplete");
      const TruthValue left = ToTruthValue(
          EvaluateExpression(*binary.left, row, precomputed, context));
      if (expression.Is(ast::ASTNodeType::kAndExpression) &&
          left == TruthValue::kFalse) {
        return Value(false);
      }
      if (expression.Is(ast::ASTNodeType::kOrExpression) &&
          left == TruthValue::kTrue) {
        return Value(true);
      }
      const TruthValue right = ToTruthValue(
          EvaluateExpression(*binary.right, row, precomputed, context));
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
      return FromTruthValue(Not(ToTruthValue(
          EvaluateExpression(*unary.operand, row, precomputed, context))));
    }
    case ast::ASTNodeType::kUnaryPlusExpression:
    case ast::ASTNodeType::kUnaryMinusExpression: {
      const auto &unary = ast::CastAst<ast::UnaryExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "unary expression operand is null");
      Value value =
          EvaluateExpression(*unary.operand, row, precomputed, context);
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
      return EvaluateArithmetic(expression, row, precomputed, context);
    case ast::ASTNodeType::kComparisonExpression: {
      const auto &comparison =
          ast::CastAst<ast::ComparisonExpression>(expression);
      CHECK(comparison.left != nullptr && comparison.right != nullptr,
            common::InvalidArgumentError,
            "comparison expression is incomplete");
      Value left =
          EvaluateExpression(*comparison.left, row, precomputed, context);
      Value right =
          EvaluateExpression(*comparison.right, row, precomputed, context);
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
        return EvaluateOrderingComparison(left, right, comparison.op);
      }
      if (comparison.op == ">") {
        return EvaluateOrderingComparison(left, right, comparison.op);
      }
      if (comparison.op == "<=") {
        return EvaluateOrderingComparison(left, right, comparison.op);
      }
      if (comparison.op == ">=") {
        return EvaluateOrderingComparison(left, right, comparison.op);
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
      Value left =
          EvaluateExpression(*predicate.left, row, precomputed, context);
      Value right =
          EvaluateExpression(*predicate.right, row, precomputed, context);
      if (left.IsNull() || right.IsNull()) {
        return Value::Null();
      }
      if (!left.IsString() || !right.IsString()) {
        return Value::Null();
      }
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
      Value element =
          EvaluateExpression(*predicate.element, row, precomputed, context);
      Value list =
          EvaluateExpression(*predicate.list, row, precomputed, context);
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
      Value value =
          EvaluateExpression(*predicate.expr, row, precomputed, context);
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
          EvaluateExpression(*predicate.operand, row, precomputed, context)
              .IsNull() == predicate.is_null);
    }
    case ast::ASTNodeType::kFunctionInvocation:
      return EvaluateFunction(ast::CastAst<ast::FunctionInvocation>(expression),
                              row, precomputed, context);
    case ast::ASTNodeType::kCaseExpression:
      return EvaluateCaseExpression(
          ast::CastAst<ast::CaseExpression>(expression), row, precomputed,
          context);
    case ast::ASTNodeType::kListComprehension:
      return EvaluateListComprehension(
          ast::CastAst<ast::ListComprehension>(expression), row, precomputed,
          context);
    case ast::ASTNodeType::kAllQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::AllQuantifier>(expression),
                                QuantifierMode::kAll, row, precomputed,
                                context);
    case ast::ASTNodeType::kAnyQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::AnyQuantifier>(expression),
                                QuantifierMode::kAny, row, precomputed,
                                context);
    case ast::ASTNodeType::kNoneQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::NoneQuantifier>(expression),
                                QuantifierMode::kNone, row, precomputed,
                                context);
    case ast::ASTNodeType::kSingleQuantifier:
      return EvaluateQuantifier(ast::CastAst<ast::SingleQuantifier>(expression),
                                QuantifierMode::kSingle, row, precomputed,
                                context);
    case ast::ASTNodeType::kParenthesizedExpression: {
      const auto &parenthesized =
          ast::CastAst<ast::ParenthesizedExpression>(expression);
      CHECK(parenthesized.expr != nullptr, common::InvalidArgumentError,
            "parenthesized expression is empty");
      return EvaluateExpression(*parenthesized.expr, row, precomputed, context);
    }
    default:
      THROW(common::InvalidArgumentError,
            "unsupported expression in executor: " +
                std::string(ast::ToString(expression.node_type)));
  }
}

}  // namespace rg
