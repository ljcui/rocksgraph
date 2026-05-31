#include "runtime/query_executor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>

#include "ast/ast_builder.h"
#include "ast/ast_node.h"
#include "ast/expression_to_string.h"
#include "common/exception.h"
#include "ir/logical_plan_builder.h"
#include "ir/planner_query.h"

namespace rg {
namespace {

using Rows = std::vector<QueryRow>;

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

Value NumericValue(double value, bool integral) {
  if (integral) {
    return Value(static_cast<int64_t>(value));
  }
  return Value(value);
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

bool Truthy(const Value &value) { return value.IsBool() && value.AsBool(); }

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

Value EvaluateExpression(const ast::Expression &expression,
                         const QueryRow &row);

Value EvaluateBinaryArithmetic(const ast::BinaryExpression &expression,
                               const QueryRow &row,
                               ast::ASTNodeType node_type) {
  CHECK(expression.left != nullptr && expression.right != nullptr,
        common::InvalidArgumentError, "binary expression is incomplete");
  Value left = EvaluateExpression(*expression.left, row);
  Value right = EvaluateExpression(*expression.right, row);

  if (node_type == ast::ASTNodeType::kAddExpression && left.IsString() &&
      right.IsString()) {
    return Value(left.AsString() + right.AsString());
  }

  CHECK(IsNumeric(left) && IsNumeric(right), common::InvalidArgumentError,
        "arithmetic expression requires numeric values");
  const bool integral = left.IsInteger() && right.IsInteger() &&
                        node_type != ast::ASTNodeType::kDivideExpression &&
                        node_type != ast::ASTNodeType::kPowerExpression;
  const double lhs = AsDoubleValue(left);
  const double rhs = AsDoubleValue(right);
  switch (node_type) {
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

bool CompareValues(const Value &left, const Value &right,
                   const std::string &op) {
  if (op == "=") {
    return left == right;
  }
  if (op == "<>") {
    return left != right;
  }
  if (op == "<") {
    return ValueLess(left, right);
  }
  if (op == ">") {
    return ValueLess(right, left);
  }
  if (op == "<=") {
    return !ValueLess(right, left);
  }
  if (op == ">=") {
    return !ValueLess(left, right);
  }
  THROW(common::InvalidArgumentError, "unsupported comparison operator: " + op);
}

Value EvaluateFunction(const ast::FunctionInvocation &function,
                       const QueryRow &row) {
  const std::string name = LowerAscii(function.function_name);
  std::vector<Value> arguments;
  arguments.reserve(function.arguments.size());
  for (const auto &argument : function.arguments) {
    CHECK(argument != nullptr, common::InvalidArgumentError,
          "function argument is null");
    arguments.push_back(EvaluateExpression(*argument, row));
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
      return Value(static_cast<int64_t>(arguments[0].AsList().size()));
    }
    if (arguments[0].IsString()) {
      return Value(static_cast<int64_t>(arguments[0].AsString().size()));
    }
    return Value::Null();
  }
  if (name == "length") {
    CHECK(arguments.size() == 1, common::InvalidArgumentError,
          "length() expects one argument");
    if (arguments[0].IsPath()) {
      return Value(
          static_cast<int64_t>(arguments[0].AsPath().relationships.size()));
    }
    return Value::Null();
  }

  THROW(common::InvalidArgumentError,
        "unsupported function in read executor: " + function.function_name);
}

Value EvaluateExpression(const ast::Expression &expression,
                         const QueryRow &row) {
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
      Value object = EvaluateExpression(*property.object, row);
      const Value *value = FindProperty(object, property.property_key);
      return value != nullptr ? *value : Value::Null();
    }
    case ast::ASTNodeType::kListLiteral: {
      const auto &list = ast::CastAst<ast::ListLiteral>(expression);
      Value::List values;
      values.reserve(list.elements.size());
      for (const auto &element : list.elements) {
        CHECK(element != nullptr, common::InvalidArgumentError,
              "list element is null");
        values.push_back(EvaluateExpression(*element, row));
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kMapLiteral: {
      const auto &map = ast::CastAst<ast::MapLiteral>(expression);
      Value::Map values;
      for (const auto &[key, value] : map.entries) {
        CHECK(value != nullptr, common::InvalidArgumentError,
              "map value is null");
        values[key] = EvaluateExpression(*value, row);
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kAndExpression: {
      const auto &binary = ast::CastAst<ast::AndExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "AND expression is incomplete");
      return Value(Truthy(EvaluateExpression(*binary.left, row)) &&
                   Truthy(EvaluateExpression(*binary.right, row)));
    }
    case ast::ASTNodeType::kOrExpression: {
      const auto &binary = ast::CastAst<ast::OrExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "OR expression is incomplete");
      return Value(Truthy(EvaluateExpression(*binary.left, row)) ||
                   Truthy(EvaluateExpression(*binary.right, row)));
    }
    case ast::ASTNodeType::kXorExpression: {
      const auto &binary = ast::CastAst<ast::XorExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "XOR expression is incomplete");
      return Value(Truthy(EvaluateExpression(*binary.left, row)) !=
                   Truthy(EvaluateExpression(*binary.right, row)));
    }
    case ast::ASTNodeType::kNotExpression: {
      const auto &unary = ast::CastAst<ast::NotExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "NOT expression operand is null");
      return Value(!Truthy(EvaluateExpression(*unary.operand, row)));
    }
    case ast::ASTNodeType::kUnaryPlusExpression: {
      const auto &unary = ast::CastAst<ast::UnaryPlusExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "unary plus operand is null");
      return EvaluateExpression(*unary.operand, row);
    }
    case ast::ASTNodeType::kUnaryMinusExpression: {
      const auto &unary = ast::CastAst<ast::UnaryMinusExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "unary minus operand is null");
      Value value = EvaluateExpression(*unary.operand, row);
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
    case ast::ASTNodeType::kPowerExpression:
      return EvaluateBinaryArithmetic(
          ast::CastAst<ast::BinaryExpression>(expression), row,
          expression.node_type);
    case ast::ASTNodeType::kComparisonExpression: {
      const auto &comparison =
          ast::CastAst<ast::ComparisonExpression>(expression);
      CHECK(comparison.left != nullptr && comparison.right != nullptr,
            common::InvalidArgumentError,
            "comparison expression is incomplete");
      return Value(CompareValues(EvaluateExpression(*comparison.left, row),
                                 EvaluateExpression(*comparison.right, row),
                                 comparison.op));
    }
    case ast::ASTNodeType::kStringPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::StringPredicateExpression>(expression);
      CHECK(predicate.left != nullptr && predicate.right != nullptr,
            common::InvalidArgumentError,
            "string predicate expression is incomplete");
      Value left = EvaluateExpression(*predicate.left, row);
      Value right = EvaluateExpression(*predicate.right, row);
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
      Value element = EvaluateExpression(*predicate.element, row);
      Value list = EvaluateExpression(*predicate.list, row);
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
      Value value = EvaluateExpression(*predicate.expr, row);
      if (value.IsNode()) {
        return Value(NodeHasLabels(value.AsNode(), predicate.labels));
      }
      if (value.IsRelationship()) {
        return Value(
            RelationshipHasAnyType(value.AsRelationship(), predicate.labels));
      }
      return Value(false);
    }
    case ast::ASTNodeType::kNullPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::NullPredicateExpression>(expression);
      CHECK(predicate.operand != nullptr, common::InvalidArgumentError,
            "null predicate operand is null");
      return Value(EvaluateExpression(*predicate.operand, row).IsNull() ==
                   predicate.is_null);
    }
    case ast::ASTNodeType::kFunctionInvocation:
      return EvaluateFunction(ast::CastAst<ast::FunctionInvocation>(expression),
                              row);
    case ast::ASTNodeType::kParenthesizedExpression: {
      const auto &parenthesized =
          ast::CastAst<ast::ParenthesizedExpression>(expression);
      CHECK(parenthesized.expr != nullptr, common::InvalidArgumentError,
            "parenthesized expression is empty");
      return EvaluateExpression(*parenthesized.expr, row);
    }
    default:
      THROW(common::InvalidArgumentError,
            "unsupported expression in read executor: " +
                std::string(ast::ToString(expression.node_type)));
  }
}

bool RelationshipMatchesDirection(const Relationship &relationship,
                                  int64_t from_node_id,
                                  ir::ExpandDirection direction) {
  switch (direction) {
    case ir::ExpandDirection::kOutgoing:
      return relationship.start_node_id == from_node_id;
    case ir::ExpandDirection::kIncoming:
      return relationship.end_node_id == from_node_id;
    case ir::ExpandDirection::kBoth:
      return relationship.start_node_id == from_node_id ||
             relationship.end_node_id == from_node_id;
  }
  return false;
}

int64_t OtherEndpoint(const Relationship &relationship, int64_t from_node_id,
                      ir::ExpandDirection direction) {
  switch (direction) {
    case ir::ExpandDirection::kOutgoing:
      return relationship.end_node_id;
    case ir::ExpandDirection::kIncoming:
      return relationship.start_node_id;
    case ir::ExpandDirection::kBoth:
      return relationship.start_node_id == from_node_id
                 ? relationship.end_node_id
                 : relationship.start_node_id;
  }
  THROW(common::InternalError, "unknown expand direction");
}

bool RelationshipConnects(const Relationship &relationship,
                          int64_t from_node_id, int64_t to_node_id,
                          ir::ExpandDirection direction) {
  switch (direction) {
    case ir::ExpandDirection::kOutgoing:
      return relationship.start_node_id == from_node_id &&
             relationship.end_node_id == to_node_id;
    case ir::ExpandDirection::kIncoming:
      return relationship.end_node_id == from_node_id &&
             relationship.start_node_id == to_node_id;
    case ir::ExpandDirection::kBoth:
      return (relationship.start_node_id == from_node_id &&
              relationship.end_node_id == to_node_id) ||
             (relationship.end_node_id == from_node_id &&
              relationship.start_node_id == to_node_id);
  }
  return false;
}

std::size_t OffsetValue(const ast::Expression *expression) {
  CHECK(expression != nullptr, common::InvalidArgumentError,
        "pagination expression is null");
  QueryRow row;
  Value value = EvaluateExpression(*expression, row);
  CHECK(value.IsInteger() && value.AsInteger() >= 0,
        common::InvalidArgumentError,
        "pagination expression must be a non-negative integer");
  return static_cast<std::size_t>(value.AsInteger());
}

class ExecutorImpl {
 public:
  explicit ExecutorImpl(const InMemoryGraph &graph) : graph_(graph) {}

  QueryResult Execute(const ir::LogicalPlan &plan) const {
    Rows rows = ExecutePlan(plan, Rows{QueryRow{}});
    QueryResult result;
    result.columns = plan.OutputColumns();
    result.rows.reserve(rows.size());
    for (const auto &row : rows) {
      std::vector<Value> values;
      values.reserve(result.columns.size());
      for (const auto &column : result.columns) {
        const auto found = row.find(column);
        values.push_back(found == row.end() ? Value::Null() : found->second);
      }
      result.rows.push_back(std::move(values));
    }
    return result;
  }

 private:
  Rows ExecutePlan(const ir::LogicalPlan &plan, const Rows &input) const {
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
      case ir::LogicalPlanNodeType::kFilter:
        return ExecuteFilter(static_cast<const ir::FilterPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kProjection:
        return ExecuteProjection(static_cast<const ir::ProjectionPlan &>(plan),
                                 input);
      case ir::LogicalPlanNodeType::kDistinct:
        return ExecuteDistinct(static_cast<const ir::DistinctPlan &>(plan),
                               input);
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
      case ir::LogicalPlanNodeType::kUnwind:
        return ExecuteUnwind(static_cast<const ir::UnwindPlan &>(plan), input);
      case ir::LogicalPlanNodeType::kAggregation:
        return ExecuteAggregation(
            static_cast<const ir::AggregationPlan &>(plan), input);
      default:
        THROW(common::InvalidArgumentError,
              "unsupported logical plan in read executor: " +
                  std::string(plan.Name()));
    }
  }

  Rows ExecuteArgument(const ir::ArgumentPlan &plan, const Rows &input) const {
    Rows out;
    out.reserve(input.size());
    for (const auto &row : input) {
      QueryRow projected;
      for (const auto &column : plan.OutputColumns()) {
        const auto found = row.find(column);
        CHECK(found != row.end(), common::InvalidArgumentError,
              "argument variable is not bound: " + column);
        projected.emplace(column, found->second);
      }
      if (plan.OutputColumns().empty()) {
        projected = row;
      }
      out.push_back(std::move(projected));
    }
    return out;
  }

  Rows ExecuteAllNodeScan(const ir::AllNodeScanPlan &plan,
                          const Rows &input) const {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : graph_.Nodes()) {
        QueryRow next = row;
        if (TryBind(&next, plan.Variable(), Value(node))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteNodeByLabelScan(const ir::NodeByLabelScanPlan &plan,
                              const Rows &input) const {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : graph_.Nodes()) {
        if (!NodeHasLabels(*node, plan.Labels())) {
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
                            const Rows &input) const {
    Rows out;
    for (const auto &row : input) {
      Value expected = EvaluateExpression(*plan.ValueExpression(), row);
      for (const auto &node : graph_.Nodes()) {
        if (!NodeHasLabels(*node, plan.Labels())) {
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
                                 const Rows &input) const {
    Rows out;
    for (const auto &row : input) {
      for (const auto &node : graph_.Nodes()) {
        if (!NodeHasLabels(*node, plan.Labels())) {
          continue;
        }
        QueryRow next = row;
        if (!TryBind(&next, plan.Variable(), Value(node))) {
          continue;
        }
        bool keep = true;
        for (const ast::Expression *predicate : plan.Predicates()) {
          keep = keep && Truthy(EvaluateExpression(*predicate, next));
        }
        if (keep) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteRelationshipTypeScan(const ir::RelationshipTypeScanPlan &plan,
                                   const Rows &input) const {
    Rows out;
    for (const auto &row : input) {
      for (const auto &relationship : graph_.Relationships()) {
        if (!RelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &out);
      }
    }
    return out;
  }

  Rows ExecuteRelationshipIndexSeek(const ir::RelationshipIndexSeekPlan &plan,
                                    const Rows &input) const {
    Rows out;
    for (const auto &row : input) {
      Value expected = EvaluateExpression(*plan.ValueExpression(), row);
      for (const auto &relationship : graph_.Relationships()) {
        if (!RelationshipHasAnyType(*relationship, plan.Types())) {
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
      const ir::RelationshipIndexRangeSeekPlan &plan, const Rows &input) const {
    Rows out;
    for (const auto &row : input) {
      for (const auto &relationship : graph_.Relationships()) {
        if (!RelationshipHasAnyType(*relationship, plan.Types())) {
          continue;
        }
        Rows candidate_rows;
        AddRelationshipRow(row, *relationship, plan.FromNode(),
                           plan.Relationship(), plan.ToNode(), plan.Direction(),
                           &candidate_rows);
        for (QueryRow &candidate : candidate_rows) {
          bool keep = true;
          for (const ast::Expression *predicate : plan.Predicates()) {
            keep = keep && Truthy(EvaluateExpression(*predicate, candidate));
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
                          ir::ExpandDirection direction, Rows *out) const {
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
    const int64_t from_id = direction == ir::ExpandDirection::kOutgoing
                                ? relationship.start_node_id
                                : relationship.end_node_id;
    const int64_t to_id = direction == ir::ExpandDirection::kOutgoing
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
                                  const std::string &to_node, int64_t from_id,
                                  int64_t to_id, Rows *out) const {
    QueryRow next = row;
    if (!TryBind(&next, from_node, Value(graph_.NodeById(from_id)))) {
      return;
    }
    if (!TryBind(&next, relationship_variable,
                 Value(std::make_shared<Relationship>(relationship)))) {
      return;
    }
    if (!TryBind(&next, to_node, Value(graph_.NodeById(to_id)))) {
      return;
    }
    out->push_back(std::move(next));
  }

  Rows ExecuteExpand(const ir::ExpandPlan &plan, const Rows &input) const {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      CHECK(from.IsNode(), common::InvalidArgumentError,
            "expand source is not a node: " + plan.FromNode());
      const int64_t from_id = from.AsNode().id;
      for (const auto &relationship : graph_.Relationships()) {
        if (!RelationshipHasAnyType(*relationship, plan.Types()) ||
            !RelationshipMatchesDirection(*relationship, from_id,
                                          plan.Direction())) {
          continue;
        }
        const int64_t to_id =
            OtherEndpoint(*relationship, from_id, plan.Direction());
        QueryRow next = row;
        if (!TryBind(&next, plan.Relationship(), Value(relationship))) {
          continue;
        }
        if (!TryBind(&next, plan.ToNode(), Value(graph_.NodeById(to_id)))) {
          continue;
        }
        out.push_back(std::move(next));
      }
    }
    return out;
  }

  Rows ExecuteExpandInto(const ir::ExpandIntoPlan &plan,
                         const Rows &input) const {
    Rows child_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &row : child_rows) {
      const Value &from = LookupVariable(row, plan.FromNode());
      const Value &to = LookupVariable(row, plan.ToNode());
      CHECK(from.IsNode() && to.IsNode(), common::InvalidArgumentError,
            "expand-into endpoints must be nodes");
      for (const auto &relationship : graph_.Relationships()) {
        if (!RelationshipHasAnyType(*relationship, plan.Types()) ||
            !RelationshipConnects(*relationship, from.AsNode().id,
                                  to.AsNode().id, plan.Direction())) {
          continue;
        }
        QueryRow next = row;
        if (TryBind(&next, plan.Relationship(), Value(relationship))) {
          out.push_back(std::move(next));
        }
      }
    }
    return out;
  }

  Rows ExecuteFilter(const ir::FilterPlan &plan, const Rows &input) const {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (auto &row : rows) {
      if (Truthy(EvaluateExpression(*plan.Predicate(), row))) {
        out.push_back(std::move(row));
      }
    }
    return out;
  }

  Rows ExecuteProjection(const ir::ProjectionPlan &plan,
                         const Rows &input) const {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    out.reserve(rows.size());
    for (const auto &row : rows) {
      QueryRow projected;
      for (const auto &item : plan.Items()) {
        CHECK(item.expression != nullptr, common::InvalidArgumentError,
              "projection expression is null");
        projected[item.alias] = EvaluateExpression(*item.expression, row);
      }
      out.push_back(std::move(projected));
    }
    return out;
  }

  Rows ExecuteDistinct(const ir::DistinctPlan &plan, const Rows &input) const {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    std::set<std::string> seen;
    for (const auto &row : rows) {
      QueryRow projected;
      std::string key;
      for (const auto &item : plan.GroupingItems()) {
        CHECK(item.expression != nullptr, common::InvalidArgumentError,
              "distinct expression is null");
        Value value = EvaluateExpression(*item.expression, row);
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

  Rows ExecuteAggregation(const ir::AggregationPlan &plan,
                          const Rows &input) const {
    Rows rows = ExecutePlan(plan.Child(0), input);
    struct GroupState {
      QueryRow row;
      std::int64_t count = 0;
    };
    std::map<std::string, GroupState> groups;

    if (plan.GroupingItems().empty()) {
      groups.emplace("", GroupState{});
    }

    for (const auto &row : rows) {
      QueryRow group_row;
      std::string key;
      for (const auto &item : plan.GroupingItems()) {
        Value value = EvaluateExpression(*item.expression, row);
        key += item.alias;
        key += '=';
        key += ValueKey(value);
        key += '\n';
        group_row[item.alias] = std::move(value);
      }
      auto [it, inserted] = groups.emplace(key, GroupState{group_row, 0});
      (void)inserted;
      ++it->second.count;
    }

    Rows out;
    for (auto &[key, state] : groups) {
      (void)key;
      QueryRow row = std::move(state.row);
      for (const auto &item : plan.AggregationItems()) {
        CHECK(item.expression != nullptr, common::InvalidArgumentError,
              "aggregation expression is null");
        if (item.expression->Is(ast::ASTNodeType::kCountStarExpression)) {
          row[item.alias] = Value(state.count);
          continue;
        }
        if (item.expression->Is(ast::ASTNodeType::kFunctionInvocation)) {
          const auto &function =
              ast::CastAst<ast::FunctionInvocation>(*item.expression);
          if (LowerAscii(function.function_name) == "count") {
            row[item.alias] = Value(state.count);
            continue;
          }
        }
        THROW(common::InvalidArgumentError,
              "unsupported aggregation expression: " +
                  ast::ExpressionToString(*item.expression));
      }
      out.push_back(std::move(row));
    }
    return out;
  }

  Rows ExecuteSort(const ir::SortPlan &plan, const Rows &input) const {
    Rows rows = ExecutePlan(plan.Child(0), input);
    std::stable_sort(
        rows.begin(), rows.end(),
        [&plan](const QueryRow &left, const QueryRow &right) {
          for (const auto &item : plan.Items()) {
            Value lhs = EvaluateExpression(*item.expression, left);
            Value rhs = EvaluateExpression(*item.expression, right);
            if (lhs == rhs) {
              continue;
            }
            const bool less = ValueLess(lhs, rhs);
            return item.direction == ir::LogicalOrderDirection::kAscending
                       ? less
                       : !less;
          }
          return false;
        });
    return rows;
  }

  Rows ExecuteSkip(const ir::SkipPlan &plan, const Rows &input) const {
    Rows rows = ExecutePlan(plan.Child(0), input);
    const std::size_t skip = OffsetValue(plan.Skip());
    if (skip >= rows.size()) {
      return {};
    }
    return Rows(rows.begin() + static_cast<std::ptrdiff_t>(skip), rows.end());
  }

  Rows ExecuteLimit(const ir::LimitPlan &plan, const Rows &input) const {
    Rows rows = ExecutePlan(plan.Child(0), input);
    const std::size_t limit = OffsetValue(plan.Limit());
    if (limit < rows.size()) {
      rows.resize(limit);
    }
    return rows;
  }

  Rows ExecuteJoin(const ir::LogicalPlan &plan, const Rows &input) const {
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

  bool JoinPredicateMatches(const ir::LogicalPlan &plan,
                            const QueryRow &row) const {
    switch (plan.Type()) {
      case ir::LogicalPlanNodeType::kCartesianProduct:
        return true;
      case ir::LogicalPlanNodeType::kNodeHashJoin: {
        const auto &join = static_cast<const ir::NodeHashJoinPlan &>(plan);
        for (const auto &key : join.JoinKeys()) {
          (void)LookupVariable(row, key);
        }
        return true;
      }
      case ir::LogicalPlanNodeType::kValueHashJoin: {
        const auto &join = static_cast<const ir::ValueHashJoinPlan &>(plan);
        for (const ast::Expression *predicate : join.Predicates()) {
          if (!Truthy(EvaluateExpression(*predicate, row))) {
            return false;
          }
        }
        return true;
      }
      case ir::LogicalPlanNodeType::kPredicateJoin: {
        const auto &join = static_cast<const ir::PredicateJoinPlan &>(plan);
        for (const ast::Expression *predicate : join.Predicates()) {
          if (!Truthy(EvaluateExpression(*predicate, row))) {
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

  Rows ExecuteApply(const ir::LogicalPlan &plan, const Rows &input) const {
    Rows left_rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (const auto &left : left_rows) {
      Rows right_rows = ExecutePlan(plan.Child(1), Rows{left});
      for (auto &row : right_rows) {
        QueryRow merged;
        if (MergeRows(left, row, &merged)) {
          out.push_back(std::move(merged));
        }
      }
    }
    return out;
  }

  Rows ExecuteUnwind(const ir::UnwindPlan &plan, const Rows &input) const {
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

  const InMemoryGraph &graph_;
};

}  // namespace

QueryResult QueryExecutor::Execute(const ir::LogicalPlan &plan) const {
  CHECK(graph_ != nullptr, common::InternalError, "graph is null");
  return ExecutorImpl(*graph_).Execute(plan);
}

QueryResult ExecuteReadQuery(const InMemoryGraph &graph,
                             std::string_view cypher) {
  std::unique_ptr<ast::Statement> statement =
      ast::ParseCypherAndRewrite(std::string(cypher));
  std::unique_ptr<ir::PlannerQuery> planner_query =
      ir::CreatePlannerQuery(*statement);
  std::unique_ptr<ir::LogicalPlan> logical_plan = ir::CreateLogicalPlan(
      *planner_query, ir::LogicalPlanBuilderOptions{.planner_catalog = &graph});
  return QueryExecutor(graph).Execute(*logical_plan);
}

}  // namespace rg
