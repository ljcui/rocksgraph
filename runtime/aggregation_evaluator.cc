#include "runtime/aggregation_evaluator.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "ast/expression_to_string.h"
#include "common/exception.h"
#include "runtime/expression_evaluator.h"
#include "value/value.h"

namespace rg {
namespace {

struct GroupingProjection {
  QueryRow row;
  std::string key;
};

struct GroupState {
  QueryRow row;
  std::vector<QueryRow> rows;
};

std::string LowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool AddWouldOverflow(std::int64_t left, std::int64_t right) {
  return (right > 0 &&
          left > std::numeric_limits<std::int64_t>::max() - right) ||
         (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right);
}

std::int64_t CountValue(std::size_t size) {
  CHECK(size <=
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()),
        common::InvalidArgumentError, "aggregation count overflow");
  return static_cast<std::int64_t>(size);
}

void AppendKeyPart(const Value &value, std::string *key) {
  CHECK(key != nullptr, common::InternalError, "grouping key is null");
  const std::string value_key = ValueKey(value);
  key->append(std::to_string(value_key.size()));
  key->push_back(':');
  key->append(value_key);
}

GroupingProjection EvaluateGrouping(
    const std::vector<ir::LogicalProjectionItem> &grouping_items,
    const QueryRow &input) {
  GroupingProjection projection;
  for (const auto &item : grouping_items) {
    Value value = EvaluateLogicalProjectionItem(item, input);
    AppendKeyPart(value, &projection.key);
    projection.row[item.alias] = std::move(value);
  }
  return projection;
}

std::vector<Value> EvaluateAggregationValues(
    const ast::FunctionInvocation &function, const std::vector<QueryRow> &rows,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  CHECK(function.arguments.size() == 1, common::InvalidArgumentError,
        function.function_name + "() expects one argument");
  CHECK(function.arguments[0] != nullptr, common::InvalidArgumentError,
        function.function_name + "() argument is null");

  std::vector<Value> values;
  values.reserve(rows.size());
  std::set<std::string> seen;
  for (const auto &row : rows) {
    Value value = EvaluateExpression(*function.arguments[0], row, precomputed);
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

Value EvaluateSum(const ast::FunctionInvocation &function,
                  const std::vector<Value> &values) {
  for (const Value &value : values) {
    CHECK(IsNumeric(value), common::InvalidArgumentError,
          function.function_name + "() expects numeric values");
  }
  const bool integral =
      std::all_of(values.begin(), values.end(),
                  [](const Value &value) { return value.IsInteger(); });
  if (integral) {
    std::int64_t sum = 0;
    for (const Value &value : values) {
      CHECK(!AddWouldOverflow(sum, value.AsInteger()),
            common::InvalidArgumentError, "integer sum overflow");
      sum += value.AsInteger();
    }
    return Value(sum);
  }

  double sum = 0.0;
  for (const Value &value : values) {
    sum += AsDoubleValue(value);
  }
  return Value(sum);
}

Value EvaluateAverage(const ast::FunctionInvocation &function,
                      const std::vector<Value> &values) {
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

Value EvaluateMinMax(const std::vector<Value> &values, bool min) {
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

Value EvaluateAggregationExpression(
    const ast::Expression &expression, const std::vector<QueryRow> &rows,
    const std::vector<ir::LogicalPrecomputedExpression> &precomputed) {
  if (expression.Is(ast::ASTNodeType::kCountStarExpression)) {
    return Value(CountValue(rows.size()));
  }
  if (!expression.Is(ast::ASTNodeType::kFunctionInvocation)) {
    THROW(common::InvalidArgumentError,
          "unsupported aggregation expression: " +
              ast::ExpressionToString(expression));
  }

  const auto &function = ast::CastAst<ast::FunctionInvocation>(expression);
  const std::string name = LowerAscii(function.function_name);
  if (name != "count" && name != "collect" && name != "sum" && name != "avg" &&
      name != "min" && name != "max") {
    THROW(common::InvalidArgumentError,
          "unsupported aggregation expression: " +
              ast::ExpressionToString(expression));
  }
  std::vector<Value> values =
      EvaluateAggregationValues(function, rows, precomputed);
  if (name == "count") {
    return Value(CountValue(values.size()));
  }
  if (name == "collect") {
    return Value(Value::List(std::move(values)));
  }
  if (name == "sum") {
    return EvaluateSum(function, values);
  }
  if (name == "avg") {
    return EvaluateAverage(function, values);
  }
  if (name == "min") {
    return EvaluateMinMax(values, true);
  }
  if (name == "max") {
    return EvaluateMinMax(values, false);
  }

  THROW(common::InternalError, "unreachable aggregation function");
}

}  // namespace

std::vector<QueryRow> ProjectDistinctRows(
    const std::vector<ir::LogicalProjectionItem> &grouping_items,
    const std::vector<QueryRow> &rows) {
  std::vector<QueryRow> result;
  std::set<std::string> seen;
  for (const auto &row : rows) {
    GroupingProjection projection = EvaluateGrouping(grouping_items, row);
    if (seen.insert(std::move(projection.key)).second) {
      result.push_back(std::move(projection.row));
    }
  }
  return result;
}

std::vector<QueryRow> AggregateRows(
    const std::vector<ir::LogicalProjectionItem> &grouping_items,
    const std::vector<ir::LogicalProjectionItem> &aggregation_items,
    const std::vector<QueryRow> &rows) {
  std::map<std::string, GroupState> groups;
  if (grouping_items.empty()) {
    groups.emplace("", GroupState{});
  }

  for (const auto &row : rows) {
    GroupingProjection projection = EvaluateGrouping(grouping_items, row);
    auto [group, inserted] = groups.emplace(
        std::move(projection.key), GroupState{std::move(projection.row), {}});
    (void)inserted;
    group->second.rows.push_back(row);
  }

  std::vector<QueryRow> result;
  result.reserve(groups.size());
  for (auto &[key, state] : groups) {
    (void)key;
    QueryRow row = std::move(state.row);
    for (const auto &item : aggregation_items) {
      CHECK(item.expression != nullptr, common::InvalidArgumentError,
            "aggregation expression is null");
      row[item.alias] = EvaluateAggregationExpression(
          *item.expression, state.rows, item.precomputed_expressions);
    }
    result.push_back(std::move(row));
  }
  return result;
}

}  // namespace rg
