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
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_builder.h"
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

Value NumericValue(double value, bool integral) {
  if (integral) {
    return Value(static_cast<std::int64_t>(value));
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
bool RuntimeNodeHasLabels(const Node &node,
                          const std::vector<std::string> &labels);
bool RuntimeRelationshipHasAnyType(const Relationship &relationship,
                                   const std::vector<std::string> &types);

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

  THROW(common::InvalidArgumentError,
        "unsupported function in executor: " + function.function_name);
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
      return Value(IsTruthy(EvaluateExpression(*binary.left, row)) &&
                   IsTruthy(EvaluateExpression(*binary.right, row)));
    }
    case ast::ASTNodeType::kOrExpression: {
      const auto &binary = ast::CastAst<ast::OrExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "OR expression is incomplete");
      return Value(IsTruthy(EvaluateExpression(*binary.left, row)) ||
                   IsTruthy(EvaluateExpression(*binary.right, row)));
    }
    case ast::ASTNodeType::kXorExpression: {
      const auto &binary = ast::CastAst<ast::XorExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError, "XOR expression is incomplete");
      return Value(IsTruthy(EvaluateExpression(*binary.left, row)) !=
                   IsTruthy(EvaluateExpression(*binary.right, row)));
    }
    case ast::ASTNodeType::kNotExpression: {
      const auto &unary = ast::CastAst<ast::NotExpression>(expression);
      CHECK(unary.operand != nullptr, common::InvalidArgumentError,
            "NOT expression operand is null");
      return Value(!IsTruthy(EvaluateExpression(*unary.operand, row)));
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
    case ast::ASTNodeType::kPowerExpression: {
      const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
      CHECK(binary.left != nullptr && binary.right != nullptr,
            common::InvalidArgumentError,
            "arithmetic expression is incomplete");
      Value left = EvaluateExpression(*binary.left, row);
      Value right = EvaluateExpression(*binary.right, row);
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
      Value left = EvaluateExpression(*comparison.left, row);
      Value right = EvaluateExpression(*comparison.right, row);
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

    for (const auto &node_variable : pattern.nodes) {
      const Value &node = LookupVariable(row, node_variable);
      CHECK(node.IsNode(), common::InvalidArgumentError,
            "path node is not a node: " + node_variable);
      path->nodes.push_back(graph_->NodeById(node.AsNode().id));
    }
    for (const auto &relationship_variable : pattern.relationships) {
      const Value &relationship = LookupVariable(row, relationship_variable);
      CHECK(
          relationship.IsRelationship(), common::InvalidArgumentError,
          "path relationship is not a relationship: " + relationship_variable);
      path->relationships.push_back(
          graph_->RelationshipById(relationship.AsRelationship().id));
    }
    return Value(std::move(path));
  }

  Rows ExecuteFilter(const ir::FilterPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    Rows out;
    for (auto &row : rows) {
      if (IsTruthy(EvaluateExpression(*plan.Predicate(), row))) {
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
        CHECK(item.expression != nullptr, common::InvalidArgumentError,
              "projection expression is null");
        projected[item.alias] = EvaluateExpression(*item.expression, row);
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

  Rows ExecuteAggregation(const ir::AggregationPlan &plan, const Rows &input) {
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
        CHECK(item.expression != nullptr, common::InvalidArgumentError,
              "grouping expression is null");
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

  Rows ExecuteSort(const ir::SortPlan &plan, const Rows &input) {
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

  Rows ExecuteSkip(const ir::SkipPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    const std::size_t skip = static_cast<std::size_t>(std::max<double>(
        0.0, EvaluateExpression(*plan.Skip(), QueryRow{}).AsInteger()));
    if (skip >= rows.size()) {
      return {};
    }
    return Rows(rows.begin() + static_cast<std::ptrdiff_t>(skip), rows.end());
  }

  Rows ExecuteLimit(const ir::LimitPlan &plan, const Rows &input) {
    Rows rows = ExecutePlan(plan.Child(0), input);
    const std::size_t limit = static_cast<std::size_t>(std::max<double>(
        0.0, EvaluateExpression(*plan.Limit(), QueryRow{}).AsInteger()));
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
        out.push_back(left);
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
