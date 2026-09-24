#include "runtime/expression_evaluator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "common/exception.h"
#include "graphdb/edge_iterator.h"
#include "graphdb/graph_entity.h"
#include "graphdb/transaction.h"
#include "graphdb/vertex_iterator.h"
#include "runtime/builtin_function_evaluator.h"
#include "runtime/execution_row.h"
#include "runtime/graphdb_access.h"
#include "value/temporal.h"

namespace rg {

Value ExpressionBindings::Lookup(std::string_view name) const {
  RG_THROW(common::ErrorCode::InvalidParameter,
           "variable is not bound: " + std::string(name));
}

Value ExpressionBindings::LookupVariable(const ast::Variable &variable) const {
  return Lookup(variable.name);
}

bool ExpressionBindings::ReadProperty(std::string_view variable,
                                      std::string_view property_key,
                                      Value *value) const {
  (void)variable;
  (void)property_key;
  (void)value;
  return false;
}

bool ExpressionBindings::ReadVariableProperty(const ast::Variable &variable,
                                              std::string_view property_key,
                                              Value *value) const {
  return ReadProperty(variable.name, property_key, value);
}

bool ExpressionBindings::ReadParameter(const ast::Parameter &parameter,
                                       Value *value) const {
  (void)parameter;
  (void)value;
  return false;
}

namespace {

class ScopedExpressionBindings final : public ExpressionBindings {
 public:
  ScopedExpressionBindings(const ExpressionBindings &parent, std::string name,
                           Value value)
      : parent_(&parent), name_(std::move(name)), value_(std::move(value)) {}

  [[nodiscard]] Value Lookup(std::string_view name) const override {
    return name == name_ ? value_ : parent_->Lookup(name);
  }

  [[nodiscard]] Value LookupVariable(
      const ast::Variable &variable) const override {
    return variable.name == name_ ? value_ : parent_->LookupVariable(variable);
  }

  [[nodiscard]] bool ReadProperty(std::string_view variable,
                                  std::string_view property_key,
                                  Value *value) const override {
    return variable != name_ &&
           parent_->ReadProperty(variable, property_key, value);
  }

  [[nodiscard]] bool ReadVariableProperty(const ast::Variable &variable,
                                          std::string_view property_key,
                                          Value *value) const override {
    return variable.name != name_ &&
           parent_->ReadVariableProperty(variable, property_key, value);
  }

  [[nodiscard]] bool ReadParameter(const ast::Parameter &parameter,
                                   Value *value) const override {
    return parent_->ReadParameter(parameter, value);
  }

 private:
  const ExpressionBindings *parent_ = nullptr;
  std::string name_;
  Value value_;
};

class MapScopedExpressionBindings final : public ExpressionBindings {
 public:
  MapScopedExpressionBindings(const ExpressionBindings &parent,
                              std::unordered_map<std::string, Value> values)
      : parent_(&parent), values_(std::move(values)) {}

  [[nodiscard]] Value Lookup(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? parent_->Lookup(name) : found->second;
  }

  [[nodiscard]] Value LookupVariable(
      const ast::Variable &variable) const override {
    return Lookup(variable.name);
  }

  [[nodiscard]] bool ReadProperty(std::string_view variable,
                                  std::string_view property_key,
                                  Value *value) const override {
    return !values_.contains(std::string(variable)) &&
           parent_->ReadProperty(variable, property_key, value);
  }

  [[nodiscard]] bool ReadVariableProperty(const ast::Variable &variable,
                                          std::string_view property_key,
                                          Value *value) const override {
    return !values_.contains(variable.name) &&
           parent_->ReadVariableProperty(variable, property_key, value);
  }

  [[nodiscard]] bool ReadParameter(const ast::Parameter &parameter,
                                   Value *value) const override {
    return parent_->ReadParameter(parameter, value);
  }

 private:
  const ExpressionBindings *parent_ = nullptr;
  std::unordered_map<std::string, Value> values_;
};

enum class TruthValue { kFalse, kTrue, kNull };
enum class QuantifierMode { kAll, kAny, kNone, kSingle };

TruthValue ToTruthValue(const Value &value) {
  if (value.IsNull()) {
    return TruthValue::kNull;
  }
  RG_CHECK(value.IsBool(), common::ErrorCode::InvalidParameter,
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
  RG_THROW(common::ErrorCode::InternalError, "unknown truth value");
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

std::optional<Value> LookupPrecomputedExpression(
    const ast::Expression &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed) {
  for (const auto &entry : precomputed) {
    if (entry.expression == nullptr ||
        !ast::ASTEqual::Equal(&expression, entry.expression)) {
      continue;
    }
    return row.Lookup(entry.variable);
  }
  return std::nullopt;
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
    const ast::FunctionInvocation &function, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  const ast::BuiltinFunction *builtin =
      ast::FindBuiltinFunction(function.function_name);
  RG_CHECK(builtin != nullptr, common::ErrorCode::InvalidParameter,
           "unknown function: " + function.function_name);
  RG_CHECK(
      !builtin->aggregate, common::ErrorCode::InvalidParameter,
      "aggregate function requires aggregation execution: " + builtin->name);
  RG_CHECK(ast::BuiltinFunctionAcceptsArgumentCount(*builtin,
                                                    function.arguments.size()),
           common::ErrorCode::InvalidParameter,
           ast::BuiltinFunctionArgumentCountError(*builtin));
  RG_CHECK(!function.distinct, common::ErrorCode::InvalidParameter,
           "DISTINCT is only supported for aggregate functions");

  std::vector<Value> arguments;
  arguments.reserve(function.arguments.size());
  for (const auto &argument : function.arguments) {
    RG_CHECK(argument != nullptr, common::ErrorCode::InvalidParameter,
             "function argument is null");
    arguments.push_back(
        EvaluateExpression(*argument, row, precomputed, context));
  }

  return EvaluateBuiltinFunction(builtin->kind, arguments, context);
}

Value EvaluateListIndex(
    const ast::ListIndexExpression &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  RG_CHECK(expression.list != nullptr && expression.index != nullptr,
           common::ErrorCode::InvalidParameter,
           "list index expression is incomplete");
  Value list = EvaluateExpression(*expression.list, row, precomputed, context);
  Value index_value =
      EvaluateExpression(*expression.index, row, precomputed, context);
  if (list.IsNull() || index_value.IsNull()) {
    return Value::Null();
  }
  if (list.IsList()) {
    RG_CHECK(index_value.IsInteger(), common::ErrorCode::InvalidParameter,
             "list index must be an integer");
    const auto &items = list.AsList();
    const std::int64_t normalized =
        NormalizeListIndex(index_value.AsInteger(), items.size());
    if (normalized < 0 ||
        normalized >= static_cast<std::int64_t>(items.size())) {
      return Value::Null();
    }
    return items[static_cast<std::size_t>(normalized)];
  }
  if (list.IsMap() || list.IsNode() || list.IsRelationship()) {
    RG_CHECK(index_value.IsString(), common::ErrorCode::InvalidParameter,
             "map property index must be a string");
    const Value *value = FindProperty(list, index_value.AsString());
    return value != nullptr ? *value : Value::Null();
  }
  RG_THROW(common::ErrorCode::InvalidParameter,
           "indexed expression must be a list, map, node, or relationship");
}

Value EvaluateListSlice(
    const ast::ListSliceExpression &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  RG_CHECK(expression.list != nullptr, common::ErrorCode::InvalidParameter,
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
    const ast::CaseExpression &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  std::optional<Value> test;
  if (expression.test != nullptr) {
    test = EvaluateExpression(*expression.test, row, precomputed, context);
  }
  for (const auto &[when_expression, then_expression] :
       expression.alternatives) {
    RG_CHECK(when_expression != nullptr && then_expression != nullptr,
             common::ErrorCode::InvalidParameter,
             "CASE alternative is incomplete");
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
    const ast::ListComprehension &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  RG_CHECK(!expression.variable.empty() && expression.list_expr != nullptr,
           common::ErrorCode::InvalidParameter,
           "list comprehension is incomplete");
  Value list =
      EvaluateExpression(*expression.list_expr, row, precomputed, context);
  if (!list.IsList()) {
    return Value::Null();
  }
  Value::List output;
  for (const auto &item : list.AsList()) {
    ScopedExpressionBindings scoped(row, expression.variable, item);
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

Value EvaluateReduce(const ast::ReduceExpression &expression,
                     const ExpressionBindings &row,
                     const std::vector<ast::PrecomputedExpression> &precomputed,
                     ExecutionContext context) {
  RG_CHECK(
      !expression.accumulator.empty() && !expression.variable.empty() &&
          expression.initial != nullptr && expression.list_expr != nullptr &&
          expression.eval_expr != nullptr,
      common::ErrorCode::InvalidParameter, "reduce expression is incomplete");
  Value accumulator =
      EvaluateExpression(*expression.initial, row, precomputed, context);
  Value list =
      EvaluateExpression(*expression.list_expr, row, precomputed, context);
  if (list.IsNull()) {
    return Value::Null();
  }
  RG_CHECK(list.IsList(), common::ErrorCode::InvalidParameter,
           "reduce requires a list value");
  for (const auto &item : list.AsList()) {
    context.CheckCancelled();
    ScopedExpressionBindings accumulator_scope(row, expression.accumulator,
                                               accumulator);
    ScopedExpressionBindings item_scope(accumulator_scope, expression.variable,
                                        item);
    accumulator = EvaluateExpression(*expression.eval_expr, item_scope,
                                     precomputed, context);
  }
  return accumulator;
}

using PatternChain =
    std::vector<std::pair<std::unique_ptr<ast::RelationshipPattern>,
                          std::unique_ptr<ast::NodePattern>>>;

bool VisitLocallyCorrelatedPatternChain(
    const ast::NodePattern &start_pattern, const PatternChain &chain,
    const std::string &path_variable, const ast::Expression *where_expression,
    const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context,
    const std::function<bool(const ExpressionBindings &,
                             const std::vector<RelationshipReference> &)>
        &on_match) {
  for (const auto &[relationship, node] : chain) {
    RG_CHECK(relationship != nullptr && node != nullptr,
             common::ErrorCode::InvalidParameter,
             "locally correlated pattern relationship is incomplete");
  }

  using Bindings = std::unordered_map<std::string, Value>;
  const auto lookup_binding =
      [&](const Bindings &bindings,
          const std::string &name) -> std::optional<Value> {
    if (name.empty()) {
      return std::nullopt;
    }
    const auto local = bindings.find(name);
    if (local != bindings.end()) {
      return local->second;
    }
    try {
      return row.Lookup(name);
    } catch (const common::Exception &error) {
      if (error.code() == common::ErrorCode::InvalidParameter) {
        return std::nullopt;
      }
      throw;
    }
  };

  const auto bind = [&](Bindings *bindings, const std::string &name,
                        Value value) {
    if (!name.empty()) {
      bindings->insert_or_assign(name, std::move(value));
    }
  };

  const auto matches_properties = [&](const ast::Properties *properties,
                                      const Value &entity,
                                      const Bindings &bindings) {
    if (properties == nullptr) {
      return true;
    }
    MapScopedExpressionBindings scoped(row, bindings);
    const ast::Expression *expression =
        properties->map != nullptr
            ? static_cast<const ast::Expression *>(properties->map.get())
            : properties->parameter.get();
    RG_CHECK(expression != nullptr, common::ErrorCode::InternalError,
             "pattern properties are incomplete");
    const Value expected =
        EvaluateExpression(*expression, scoped, precomputed, context);
    RG_CHECK(expected.IsMap(), common::ErrorCode::InvalidParameter,
             "pattern properties must be a map");
    for (const auto &[key, value] : expected.AsMap()) {
      const Value *actual = FindProperty(entity, key);
      if (actual == nullptr ||
          EqualityTruth(*actual, value) != TruthValue::kTrue) {
        return false;
      }
    }
    return true;
  };

  std::function<bool(std::size_t, const Value::NodePtr &, Bindings,
                     std::vector<Value::NodePtr>,
                     std::vector<Value::RelationshipPtr>,
                     std::vector<RelationshipReference>)>
      expand;
  expand = [&](std::size_t chain_index, const Value::NodePtr &current,
               Bindings bindings, std::vector<Value::NodePtr> path_nodes,
               std::vector<Value::RelationshipPtr> path_relationships,
               std::vector<RelationshipReference> used_relationships) {
    if (chain_index == chain.size()) {
      if (!path_variable.empty()) {
        auto path = std::make_shared<Path>();
        path->nodes = std::move(path_nodes);
        path->relationships = std::move(path_relationships);
        bind(&bindings, path_variable, Value(std::move(path)));
      }
      MapScopedExpressionBindings scoped(row, std::move(bindings));
      if (where_expression != nullptr &&
          !PredicateIsTrue(EvaluateExpression(*where_expression, scoped,
                                              precomputed, context))) {
        return false;
      }
      return on_match(scoped, used_relationships);
    }

    const auto &[relationship_ptr, next_node_ptr] = chain[chain_index];
    const ast::RelationshipPattern &relationship_pattern = *relationship_ptr;
    const ast::RelationshipDetail *detail = relationship_pattern.detail.get();
    graphdb::EdgeDirection direction = graphdb::EdgeDirection::BOTH;
    if (relationship_pattern.left_arrow && !relationship_pattern.right_arrow) {
      direction = graphdb::EdgeDirection::INCOMING;
    } else if (relationship_pattern.right_arrow &&
               !relationship_pattern.left_arrow) {
      direction = graphdb::EdgeDirection::OUTGOING;
    }
    const std::vector<std::string> types =
        detail == nullptr ? std::vector<std::string>{} : detail->types;
    if (detail != nullptr && detail->range.has_value()) {
      const std::size_t min =
          static_cast<std::size_t>(detail->range->min.value_or(1));
      const std::size_t max =
          detail->range->max.has_value()
              ? static_cast<std::size_t>(*detail->range->max)
              : std::numeric_limits<std::size_t>::max();
      RG_CHECK(detail->range->min.value_or(1) >= 0 &&
                   detail->range->max.value_or(0) >= 0,
               common::ErrorCode::InvalidParameter,
               "negative variable path length");
      std::function<bool(const Value::NodePtr &, std::vector<Value::NodePtr>,
                         std::vector<Value::RelationshipPtr>,
                         std::vector<RelationshipReference>)>
          visit_hops;
      visit_hops = [&](const Value::NodePtr &at,
                       std::vector<Value::NodePtr> segment_nodes,
                       std::vector<Value::RelationshipPtr> segment_edges,
                       std::vector<RelationshipReference> used) {
        if (segment_edges.size() >= min &&
            NodeHasLabels(*at, next_node_ptr->labels) &&
            matches_properties(next_node_ptr->properties.get(), Value(at),
                               bindings)) {
          Value::List relationship_values;
          relationship_values.reserve(segment_edges.size());
          for (const auto &edge : segment_edges) {
            relationship_values.emplace_back(Value(edge));
          }
          Value relationship_list(std::move(relationship_values));
          const std::optional<Value> bound_relationship =
              lookup_binding(bindings, detail->variable);
          const std::optional<Value> bound_node =
              lookup_binding(bindings, next_node_ptr->variable);
          if ((!bound_relationship.has_value() ||
               EqualityTruth(*bound_relationship, relationship_list) ==
                   TruthValue::kTrue) &&
              (!bound_node.has_value() ||
               (bound_node->IsNode() && bound_node->AsNode().id == at->id))) {
            Bindings next_bindings = bindings;
            bind(&next_bindings, detail->variable,
                 std::move(relationship_list));
            bind(&next_bindings, next_node_ptr->variable, Value(at));
            auto next_nodes = path_nodes;
            next_nodes.insert(next_nodes.end(), segment_nodes.begin(),
                              segment_nodes.end());
            auto next_edges = path_relationships;
            next_edges.insert(next_edges.end(), segment_edges.begin(),
                              segment_edges.end());
            if (expand(chain_index + 1, at, std::move(next_bindings),
                       std::move(next_nodes), std::move(next_edges), used)) {
              return true;
            }
          }
        }
        if (segment_edges.size() >= max) {
          return false;
        }
        graphdb::Vertex vertex =
            GraphDBVertexById(context.GraphDBTransaction(), at->id);
        std::unique_ptr<graphdb::EdgeIterator> edges = vertex.NewEdgeIterator(
            direction,
            std::unordered_set<std::string>(types.begin(), types.end()), {});
        while (edges->Valid()) {
          context.CheckCancelled();
          graphdb::Edge edge = edges->GetEdge();
          edges->Next();
          const RelationshipReference reference{.id = edge.GetId(),
                                                .type_id = edge.GetTypeId()};
          if (std::find(used.begin(), used.end(), reference) != used.end()) {
            continue;
          }
          std::int64_t next_id = -1;
          if (direction != graphdb::EdgeDirection::INCOMING &&
              edge.GetStartId() == at->id) {
            next_id = edge.GetEndId();
          } else if (direction != graphdb::EdgeDirection::OUTGOING &&
                     edge.GetEndId() == at->id) {
            next_id = edge.GetStartId();
          } else {
            continue;
          }
          Value::RelationshipPtr relationship = MaterializeGraphDBEdge(edge);
          if (!matches_properties(detail->properties.get(), Value(relationship),
                                  bindings)) {
            continue;
          }
          auto next =
              MaterializeGraphDBVertex(context.GraphDBTransaction(), next_id);
          auto following_nodes = segment_nodes;
          following_nodes.push_back(next);
          auto following_edges = segment_edges;
          following_edges.push_back(relationship);
          auto following_used = used;
          following_used.push_back(reference);
          if (visit_hops(next, std::move(following_nodes),
                         std::move(following_edges),
                         std::move(following_used))) {
            return true;
          }
        }
        return false;
      };
      return visit_hops(current, {}, {}, std::move(used_relationships));
    }
    graphdb::Vertex vertex =
        GraphDBVertexById(context.GraphDBTransaction(), current->id);
    std::unique_ptr<graphdb::EdgeIterator> edges = vertex.NewEdgeIterator(
        direction, std::unordered_set<std::string>(types.begin(), types.end()),
        {});
    while (edges->Valid()) {
      context.CheckCancelled();
      graphdb::Edge edge = edges->GetEdge();
      edges->Next();
      const RelationshipReference reference{.id = edge.GetId(),
                                            .type_id = edge.GetTypeId()};
      if (std::find(used_relationships.begin(), used_relationships.end(),
                    reference) != used_relationships.end()) {
        continue;
      }

      std::int64_t next_id = -1;
      if (direction != graphdb::EdgeDirection::INCOMING &&
          edge.GetStartId() == current->id) {
        next_id = edge.GetEndId();
      } else if (direction != graphdb::EdgeDirection::OUTGOING &&
                 edge.GetEndId() == current->id) {
        next_id = edge.GetStartId();
      } else {
        continue;
      }

      Value::RelationshipPtr relationship = MaterializeGraphDBEdge(edge);
      if (detail != nullptr &&
          !matches_properties(detail->properties.get(), Value(relationship),
                              bindings)) {
        continue;
      }
      if (detail != nullptr && !detail->variable.empty()) {
        const std::optional<Value> bound =
            lookup_binding(bindings, detail->variable);
        if (bound.has_value() &&
            (!bound->IsRelationship() ||
             bound->AsRelationship().id != relationship->id ||
             bound->AsRelationship().type_id != relationship->type_id)) {
          continue;
        }
      }

      Value::NodePtr next =
          MaterializeGraphDBVertex(context.GraphDBTransaction(), next_id);
      if (!NodeHasLabels(*next, next_node_ptr->labels) ||
          !matches_properties(next_node_ptr->properties.get(), Value(next),
                              bindings)) {
        continue;
      }
      if (!next_node_ptr->variable.empty()) {
        const std::optional<Value> bound =
            lookup_binding(bindings, next_node_ptr->variable);
        if (bound.has_value() &&
            (!bound->IsNode() || bound->AsNode().id != next->id)) {
          continue;
        }
      }

      Bindings next_bindings = bindings;
      if (detail != nullptr) {
        bind(&next_bindings, detail->variable, Value(relationship));
      }
      bind(&next_bindings, next_node_ptr->variable, Value(next));
      auto next_nodes = path_nodes;
      next_nodes.push_back(next);
      auto next_relationships = path_relationships;
      next_relationships.push_back(relationship);
      auto next_used = used_relationships;
      next_used.push_back(reference);
      if (expand(chain_index + 1, next, std::move(next_bindings),
                 std::move(next_nodes), std::move(next_relationships),
                 std::move(next_used))) {
        return true;
      }
    }
    return false;
  };

  const std::optional<Value> bound_start =
      lookup_binding({}, start_pattern.variable);
  if (bound_start.has_value()) {
    if (bound_start->IsNull()) {
      return false;
    }
    RG_CHECK(bound_start->IsNode(), common::ErrorCode::InvalidParameter,
             "locally correlated pattern start must be a node");
    Value::NodePtr start = std::make_shared<Node>(bound_start->AsNode());
    if (NodeHasLabels(*start, start_pattern.labels) &&
        matches_properties(start_pattern.properties.get(), Value(start), {})) {
      Bindings bindings;
      bind(&bindings, start_pattern.variable, Value(start));
      return expand(0, start, std::move(bindings), {start}, {}, {});
    }
    return false;
  }

  std::unique_ptr<graphdb::VertexIterator> vertices =
      start_pattern.labels.empty()
          ? context.GraphDBTransaction().NewVertexIterator()
          : context.GraphDBTransaction().NewVertexIterator(
                start_pattern.labels.front());
  while (vertices->Valid()) {
    context.CheckCancelled();
    Value::NodePtr start = MaterializeGraphDBVertex(vertices->GetVertex());
    vertices->Next();
    if (!NodeHasLabels(*start, start_pattern.labels) ||
        !matches_properties(start_pattern.properties.get(), Value(start), {})) {
      continue;
    }
    Bindings bindings;
    bind(&bindings, start_pattern.variable, Value(start));
    if (expand(0, start, std::move(bindings), {start}, {}, {})) {
      return true;
    }
  }
  return false;
}

Value EvaluateLocallyCorrelatedPatternComprehension(
    const ast::PatternComprehension &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  RG_CHECK(expression.relationships_pattern != nullptr &&
               expression.relationships_pattern->node_pattern != nullptr &&
               expression.eval_expr != nullptr,
           common::ErrorCode::InvalidParameter,
           "pattern comprehension is incomplete");
  const ast::RelationshipsPattern &pattern = *expression.relationships_pattern;
  Value::List output;
  (void)VisitLocallyCorrelatedPatternChain(
      *pattern.node_pattern, pattern.chain, expression.variable,
      expression.where_expr.get(), row, precomputed, context,
      [&](const ExpressionBindings &bindings,
          const std::vector<RelationshipReference> &) {
        output.push_back(EvaluateExpression(*expression.eval_expr, bindings,
                                            precomputed, context));
        return false;
      });
  return Value(std::move(output));
}

bool VisitLocallyCorrelatedPattern(
    const ast::Pattern &pattern, std::size_t index,
    const ast::Expression *where_expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context,
    const std::function<bool(const ExpressionBindings &)> &on_match) {
  if (index == pattern.parts.size()) {
    if (where_expression != nullptr &&
        !PredicateIsTrue(
            EvaluateExpression(*where_expression, row, precomputed, context))) {
      return false;
    }
    return on_match(row);
  }
  const ast::PatternPart *part = pattern.parts[index].get();
  RG_CHECK(part != nullptr && part->element != nullptr &&
               part->element->node_pattern != nullptr,
           common::ErrorCode::InternalError,
           "locally correlated EXISTS pattern part is incomplete");
  RG_CHECK(part->shortest_path_kind == ast::ShortestPathKind::kNone,
           common::ErrorCode::InvalidParameter,
           "locally correlated EXISTS requires a fixed pattern");
  return VisitLocallyCorrelatedPatternChain(
      *part->element->node_pattern, part->element->chain, part->variable,
      nullptr, row, precomputed, context,
      [&](const ExpressionBindings &bindings,
          const std::vector<RelationshipReference> &) {
        return VisitLocallyCorrelatedPattern(pattern, index + 1,
                                             where_expression, bindings,
                                             precomputed, context, on_match);
      });
}

std::unordered_map<std::string, Value> OptionalMatchNullBindings(
    const ast::Pattern &pattern, const ExpressionBindings &row) {
  std::unordered_map<std::string, Value> nulls;
  const auto add_unbound = [&](const std::string &name) {
    if (name.empty() || nulls.contains(name)) {
      return;
    }
    try {
      (void)row.Lookup(name);
    } catch (const common::Exception &error) {
      if (error.code() != common::ErrorCode::InvalidParameter) {
        throw;
      }
      nulls.emplace(name, Value::Null());
    }
  };
  for (const auto &part : pattern.parts) {
    RG_CHECK(part != nullptr && part->element != nullptr &&
                 part->element->node_pattern != nullptr,
             common::ErrorCode::InternalError,
             "locally correlated EXISTS pattern part is incomplete");
    add_unbound(part->variable);
    add_unbound(part->element->node_pattern->variable);
    for (const auto &[relationship, node] : part->element->chain) {
      RG_CHECK(relationship != nullptr && node != nullptr,
               common::ErrorCode::InternalError,
               "locally correlated EXISTS pattern chain is incomplete");
      if (relationship->detail != nullptr) {
        add_unbound(relationship->detail->variable);
      }
      add_unbound(node->variable);
    }
  }
  return nulls;
}

bool VisitLocallyCorrelatedReadingClauses(
    const std::vector<std::unique_ptr<ast::ReadingClause>> &clauses,
    std::size_t index, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context,
    const std::function<bool(const ExpressionBindings &)> &on_row) {
  if (index == clauses.size()) {
    return on_row(row);
  }
  const ast::ReadingClause *clause = clauses[index].get();
  RG_CHECK(clause != nullptr, common::ErrorCode::InternalError,
           "locally correlated EXISTS reading clause is null");
  if (clause->Is(ast::ASTNodeType::kMatch)) {
    const auto &match = ast::CastAst<ast::Match>(*clause);
    RG_CHECK(match.pattern != nullptr, common::ErrorCode::InternalError,
             "locally correlated EXISTS MATCH pattern is null");
    bool matched = false;
    const bool stopped = VisitLocallyCorrelatedPattern(
        *match.pattern, 0, match.where.get(), row, precomputed, context,
        [&](const ExpressionBindings &bindings) {
          matched = true;
          return VisitLocallyCorrelatedReadingClauses(
              clauses, index + 1, bindings, precomputed, context, on_row);
        });
    if (stopped || matched || !match.optional_match) {
      return stopped;
    }
    MapScopedExpressionBindings null_row(
        row, OptionalMatchNullBindings(*match.pattern, row));
    return VisitLocallyCorrelatedReadingClauses(clauses, index + 1, null_row,
                                                precomputed, context, on_row);
  }
  if (clause->Is(ast::ASTNodeType::kUnwind)) {
    const auto &unwind = ast::CastAst<ast::Unwind>(*clause);
    RG_CHECK(unwind.expression != nullptr, common::ErrorCode::InternalError,
             "locally correlated EXISTS UNWIND expression is null");
    Value list =
        EvaluateExpression(*unwind.expression, row, precomputed, context);
    if (list.IsNull()) {
      return false;
    }
    RG_CHECK(list.IsList(), common::ErrorCode::InvalidParameter,
             "UNWIND requires a list value");
    for (const Value &item : list.AsList()) {
      context.CheckCancelled();
      ScopedExpressionBindings scoped(row, unwind.variable, item);
      if (VisitLocallyCorrelatedReadingClauses(clauses, index + 1, scoped,
                                               precomputed, context, on_row)) {
        return true;
      }
    }
    return false;
  }
  RG_THROW(common::ErrorCode::InvalidParameter,
           "unsupported reading clause in locally correlated EXISTS");
}

std::uint64_t EvaluateLocallyCorrelatedPagination(
    const ast::Expression *expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context, std::string_view name) {
  Value count = EvaluateExpression(*expression, row, precomputed, context);
  RG_CHECK(count.IsInteger() && count.AsInteger() >= 0,
           common::ErrorCode::InvalidParameter,
           std::string(name) + " requires a non-negative integer");
  return static_cast<std::uint64_t>(count.AsInteger());
}

bool VisitLocallyCorrelatedProjection(
    const std::vector<std::unique_ptr<ast::ReadingClause>> &clauses,
    const ast::ProjectionBody &body, const ast::Expression *where,
    bool preserve_order, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context,
    const std::function<bool(const ExpressionBindings &)> &on_row) {
  std::optional<std::uint64_t> skip;
  std::optional<std::uint64_t> limit;
  std::unordered_set<Value, ValueHash, ValueEqual> distinct_rows;
  std::uint64_t seen = 0;
  const bool sort_before_pagination = preserve_order && !body.order_by.empty();
  struct BufferedProjection {
    std::unordered_map<std::string, Value> aliases;
    std::vector<Value> sort_keys;
  };
  std::vector<BufferedProjection> buffered;
  struct MemoryReservation {
    QueryMemoryTracker *tracker;
    std::size_t bytes = 0;
    ~MemoryReservation() {
      if (tracker != nullptr) {
        tracker->Release(bytes);
      }
    }
    void Reserve(std::size_t amount) {
      if (tracker != nullptr) {
        tracker->Reserve(amount);
        bytes += amount;
      }
    }
  } reservation{context.memory_tracker};
  const bool stopped = VisitLocallyCorrelatedReadingClauses(
      clauses, 0, row, precomputed, context,
      [&](const ExpressionBindings &bindings) {
        Value::List projected;
        std::unordered_map<std::string, Value> aliases;
        projected.reserve(body.items.size());
        for (const auto &item : body.items) {
          RG_CHECK(item != nullptr && item->expression != nullptr,
                   common::ErrorCode::InternalError,
                   "locally correlated EXISTS projection item is null");
          projected.push_back(EvaluateExpression(*item->expression, bindings,
                                                 precomputed, context));
          if (!item->alias.empty()) {
            aliases.insert_or_assign(item->alias, projected.back());
          } else if (item->expression->Is(ast::ASTNodeType::kVariable)) {
            const auto &variable =
                ast::CastAst<ast::Variable>(*item->expression);
            aliases.insert_or_assign(variable.name, projected.back());
          }
        }
        MapScopedExpressionBindings projected_row(bindings, aliases);
        if (where != nullptr &&
            !PredicateIsTrue(EvaluateExpression(*where, projected_row,
                                                precomputed, context))) {
          return false;
        }
        if (body.distinct) {
          if (!distinct_rows.emplace(std::move(projected)).second) {
            return false;
          }
        }
        if (!skip.has_value()) {
          skip = body.skip == nullptr ? 0
                                      : EvaluateLocallyCorrelatedPagination(
                                            body.skip.get(), projected_row,
                                            precomputed, context, "SKIP");
        }
        if (!limit.has_value()) {
          limit = body.limit == nullptr
                      ? std::numeric_limits<std::uint64_t>::max()
                      : EvaluateLocallyCorrelatedPagination(
                            body.limit.get(), projected_row, precomputed,
                            context, "LIMIT");
        }
        if (*limit == 0) {
          return true;
        }
        if (sort_before_pagination) {
          BufferedProjection candidate{.aliases = std::move(aliases)};
          candidate.sort_keys.reserve(body.order_by.size());
          for (const auto &sort_item : body.order_by) {
            RG_CHECK(sort_item != nullptr && sort_item->expression != nullptr,
                     common::ErrorCode::InternalError,
                     "locally correlated EXISTS sort item is null");
            candidate.sort_keys.push_back(EvaluateExpression(
                *sort_item->expression, projected_row, precomputed, context));
          }
          std::size_t bytes = sizeof(BufferedProjection);
          for (const auto &[name, value] : candidate.aliases) {
            bytes += name.capacity() + EstimatedValueHeapUsage(value);
          }
          for (const Value &value : candidate.sort_keys) {
            bytes += sizeof(Value) + EstimatedValueHeapUsage(value);
          }
          reservation.Reserve(bytes);
          buffered.push_back(std::move(candidate));
          return false;
        }
        if (seen < *skip) {
          ++seen;
          return false;
        }
        return on_row(projected_row);
      });
  if (!sort_before_pagination || stopped) {
    return stopped;
  }
  if (buffered.empty()) {
    return false;
  }
  std::stable_sort(
      buffered.begin(), buffered.end(),
      [&](const BufferedProjection &left, const BufferedProjection &right) {
        for (std::size_t index = 0; index < body.order_by.size(); ++index) {
          const bool ascending = body.order_by[index]->ascending;
          const Value &lhs = left.sort_keys[index];
          const Value &rhs = right.sort_keys[index];
          if (ValueLess(lhs, rhs)) {
            return ascending;
          }
          if (ValueLess(rhs, lhs)) {
            return !ascending;
          }
        }
        return false;
      });
  const std::uint64_t begin = std::min<std::uint64_t>(*skip, buffered.size());
  const std::uint64_t count =
      std::min<std::uint64_t>(*limit, buffered.size() - begin);
  for (std::uint64_t index = begin; index < begin + count; ++index) {
    MapScopedExpressionBindings projected_row(row, buffered[index].aliases);
    if (on_row(projected_row)) {
      return true;
    }
  }
  return false;
}

bool VisitLocallyCorrelatedSingleQuery(
    const ast::SingleQuery &query, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context);

bool VisitLocallyCorrelatedMultiPart(
    const ast::MultiPartQuery &query, std::size_t index,
    const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  if (index == query.parts.size()) {
    RG_CHECK(query.final_single_part_query != nullptr,
             common::ErrorCode::InternalError,
             "locally correlated EXISTS final query part is null");
    return VisitLocallyCorrelatedSingleQuery(*query.final_single_part_query,
                                             row, precomputed, context);
  }
  const auto &part = query.parts[index];
  RG_CHECK(part.updating_clauses.empty() && part.with_clause != nullptr &&
               part.with_clause->body != nullptr,
           common::ErrorCode::InternalError,
           "locally correlated EXISTS WITH part is incomplete");
  bool exists = false;
  (void)VisitLocallyCorrelatedProjection(
      part.reading_clauses, *part.with_clause->body,
      part.with_clause->where.get(), true, row, precomputed, context,
      [&](const ExpressionBindings &projected) {
        exists = VisitLocallyCorrelatedMultiPart(query, index + 1, projected,
                                                 precomputed, context);
        return exists;
      });
  return exists;
}

bool VisitLocallyCorrelatedSingleQuery(
    const ast::SingleQuery &query, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  if (query.Is(ast::ASTNodeType::kMultiPartQuery)) {
    return VisitLocallyCorrelatedMultiPart(
        ast::CastAst<ast::MultiPartQuery>(query), 0, row, precomputed, context);
  }
  RG_CHECK(query.Is(ast::ASTNodeType::kSinglePartQuery),
           common::ErrorCode::InternalError,
           "locally correlated EXISTS query has an unknown shape");
  const auto &single = ast::CastAst<ast::SinglePartQuery>(query);
  RG_CHECK(single.updating_clauses.empty() && single.return_clause != nullptr &&
               single.return_clause->body != nullptr,
           common::ErrorCode::InternalError,
           "locally correlated EXISTS query is incomplete");
  bool exists = false;
  (void)VisitLocallyCorrelatedProjection(
      single.reading_clauses, *single.return_clause->body, nullptr, true, row,
      precomputed, context, [&](const ExpressionBindings &) {
        exists = true;
        return true;
      });
  return exists;
}

Value EvaluateLocallyCorrelatedExistentialSubquery(
    const ast::ExistentialSubquery &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  if (expression.pattern != nullptr) {
    return Value(VisitLocallyCorrelatedPattern(
        *expression.pattern, 0, expression.where_expr.get(), row, precomputed,
        context, [](const ExpressionBindings &) { return true; }));
  }
  RG_CHECK(
      expression.query != nullptr && expression.query->single_query != nullptr,
      common::ErrorCode::InternalError,
      "locally correlated EXISTS query is null");
  if (VisitLocallyCorrelatedSingleQuery(*expression.query->single_query, row,
                                        precomputed, context)) {
    return Value(true);
  }
  for (const auto &part : expression.query->unions) {
    RG_CHECK(part != nullptr && part->query != nullptr,
             common::ErrorCode::InternalError,
             "locally correlated EXISTS UNION part is null");
    if (VisitLocallyCorrelatedSingleQuery(*part->query, row, precomputed,
                                          context)) {
      return Value(true);
    }
  }
  return Value(false);
}

Value EvaluateQuantifier(
    const ast::Quantifier &quantifier, QuantifierMode mode,
    const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  RG_CHECK(!quantifier.variable.empty() && quantifier.list_expr != nullptr &&
               quantifier.predicate != nullptr,
           common::ErrorCode::InvalidParameter, "quantifier is incomplete");
  Value list =
      EvaluateExpression(*quantifier.list_expr, row, precomputed, context);
  if (list.IsNull()) {
    return Value::Null();
  }
  RG_CHECK(list.IsList(), common::ErrorCode::InvalidParameter,
           "quantifier requires a list value");
  std::size_t matches = 0;
  bool saw_null = false;
  for (const auto &item : list.AsList()) {
    ScopedExpressionBindings scoped(row, quantifier.variable, item);
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
  RG_THROW(common::ErrorCode::InternalError, "unknown quantifier mode");
}

Value EvaluateArithmetic(
    const ast::Expression &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
  RG_CHECK(binary.left != nullptr && binary.right != nullptr,
           common::ErrorCode::InvalidParameter,
           "arithmetic expression is incomplete");
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
  const bool add = expression.Is(ast::ASTNodeType::kAddExpression);
  const bool subtract = expression.Is(ast::ASTNodeType::kSubtractExpression);
  const bool multiply = expression.Is(ast::ASTNodeType::kMultiplyExpression);
  const bool divide = expression.Is(ast::ASTNodeType::kDivideExpression);
  const auto is_temporal = [](const Value &value) {
    return value.IsDate() || value.IsLocalTime() || value.IsTime() ||
           value.IsLocalDateTime() || value.IsDateTime();
  };
  if ((add || subtract) && left.IsDuration() && right.IsDuration()) {
    return subtract
               ? temporal::SubtractDurations(left.AsDuration(),
                                             right.AsDuration())
               : temporal::AddDurations(left.AsDuration(), right.AsDuration());
  }
  if ((add || subtract) && is_temporal(left) && right.IsDuration()) {
    return subtract ? temporal::SubtractDurationFromTemporal(left,
                                                             right.AsDuration())
                    : temporal::AddDurationToTemporal(left, right.AsDuration());
  }
  if (add && left.IsDuration() && is_temporal(right)) {
    return temporal::AddDurationToTemporal(right, left.AsDuration());
  }
  if (multiply && left.IsDuration() && IsNumeric(right)) {
    return temporal::ScaleDuration(left.AsDuration(), AsDoubleValue(right));
  }
  if (multiply && IsNumeric(left) && right.IsDuration()) {
    return temporal::ScaleDuration(right.AsDuration(), AsDoubleValue(left));
  }
  if (divide && left.IsDuration() && IsNumeric(right)) {
    const double divisor = AsDoubleValue(right);
    RG_CHECK(divisor != 0.0, common::ErrorCode::InvalidParameter,
             "division by zero");
    return temporal::ScaleDuration(left.AsDuration(), 1.0 / divisor);
  }
  if (expression.Is(ast::ASTNodeType::kAddExpression) && left.IsString() &&
      right.IsString()) {
    return Value(left.AsString() + right.AsString());
  }
  RG_CHECK(IsNumeric(left) && IsNumeric(right),
           common::ErrorCode::InvalidParameter,
           "arithmetic expression requires numeric values");
  if (left.IsInteger() && right.IsInteger() &&
      !expression.Is(ast::ASTNodeType::kPowerExpression)) {
    const auto lhs = left.AsInteger();
    const auto rhs = right.AsInteger();
    if (expression.Is(ast::ASTNodeType::kAddExpression)) {
      RG_CHECK(!AddWouldOverflow(lhs, rhs), common::ErrorCode::InvalidParameter,
               "integer addition overflow");
      return Value(lhs + rhs);
    }
    if (expression.Is(ast::ASTNodeType::kSubtractExpression)) {
      RG_CHECK(!SubtractWouldOverflow(lhs, rhs),
               common::ErrorCode::InvalidParameter,
               "integer subtraction overflow");
      return Value(lhs - rhs);
    }
    if (expression.Is(ast::ASTNodeType::kMultiplyExpression)) {
      RG_CHECK(!MultiplyWouldOverflow(lhs, rhs),
               common::ErrorCode::InvalidParameter,
               "integer multiplication overflow");
      return Value(lhs * rhs);
    }
    if (expression.Is(ast::ASTNodeType::kDivideExpression)) {
      RG_CHECK(rhs != 0, common::ErrorCode::InvalidParameter,
               "division by zero");
      RG_CHECK(!(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1),
               common::ErrorCode::InvalidParameter,
               "integer division overflow");
      return Value(lhs / rhs);
    }
    RG_CHECK(rhs != 0, common::ErrorCode::InvalidParameter, "modulo by zero");
    RG_CHECK(!(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1),
             common::ErrorCode::InvalidParameter, "integer modulo overflow");
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
  RG_THROW(common::ErrorCode::InvalidParameter,
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
  RG_CHECK(value.IsDouble(), common::ErrorCode::InvalidParameter,
           "expected numeric value");
  return value.AsDouble();
}

Value EvaluateExpression(
    const ast::Expression &expression, const ExpressionBindings &row,
    const std::vector<ast::PrecomputedExpression> &precomputed,
    ExecutionContext context) {
  if (std::optional<Value> value =
          LookupPrecomputedExpression(expression, row, precomputed);
      value.has_value()) {
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
      return row.LookupVariable(ast::CastAst<ast::Variable>(expression));
    case ast::ASTNodeType::kParameter: {
      const auto &parameter = ast::CastAst<ast::Parameter>(expression);
      Value row_value;
      if (row.ReadParameter(parameter, &row_value)) {
        return row_value;
      }
      const Value *value = context.FindParameter(parameter.name);
      RG_CHECK(value != nullptr, common::ErrorCode::InvalidParameter,
               "missing query parameter: " + parameter.name);
      return *value;
    }
    case ast::ASTNodeType::kPropertyExpression: {
      const auto &property = ast::CastAst<ast::PropertyExpression>(expression);
      RG_CHECK(property.object != nullptr, common::ErrorCode::InvalidParameter,
               "property object is null");
      if (property.object->Is(ast::ASTNodeType::kVariable)) {
        Value value;
        const auto &variable = ast::CastAst<ast::Variable>(*property.object);
        if (row.ReadVariableProperty(variable, property.property_key, &value)) {
          return value;
        }
      }
      Value object =
          EvaluateExpression(*property.object, row, precomputed, context);
      const Value *value = FindProperty(object, property.property_key);
      if (value != nullptr) {
        return *value;
      }
      return temporal::TemporalProperty(object, property.property_key)
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
        RG_CHECK(element != nullptr, common::ErrorCode::InvalidParameter,
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
        RG_CHECK(value != nullptr, common::ErrorCode::InvalidParameter,
                 "map value is null");
        values[key] = EvaluateExpression(*value, row, precomputed, context);
      }
      return Value(std::move(values));
    }
    case ast::ASTNodeType::kAndExpression:
    case ast::ASTNodeType::kOrExpression:
    case ast::ASTNodeType::kXorExpression: {
      const auto &binary = ast::CastAst<ast::BinaryExpression>(expression);
      RG_CHECK(binary.left != nullptr && binary.right != nullptr,
               common::ErrorCode::InvalidParameter,
               "boolean expression is incomplete");
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
      RG_CHECK(unary.operand != nullptr, common::ErrorCode::InvalidParameter,
               "NOT expression operand is null");
      return FromTruthValue(Not(ToTruthValue(
          EvaluateExpression(*unary.operand, row, precomputed, context))));
    }
    case ast::ASTNodeType::kUnaryPlusExpression:
    case ast::ASTNodeType::kUnaryMinusExpression: {
      const auto &unary = ast::CastAst<ast::UnaryExpression>(expression);
      RG_CHECK(unary.operand != nullptr, common::ErrorCode::InvalidParameter,
               "unary expression operand is null");
      Value value =
          EvaluateExpression(*unary.operand, row, precomputed, context);
      if (value.IsNull()) {
        return Value::Null();
      }
      RG_CHECK(IsNumeric(value), common::ErrorCode::InvalidParameter,
               "unary arithmetic requires a numeric value");
      if (expression.Is(ast::ASTNodeType::kUnaryPlusExpression)) {
        return value;
      }
      if (value.IsDouble()) {
        return Value(-value.AsDouble());
      }
      RG_CHECK(value.AsInteger() != std::numeric_limits<std::int64_t>::min(),
               common::ErrorCode::InvalidParameter,
               "integer negation overflow");
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
      RG_CHECK(comparison.left != nullptr && comparison.right != nullptr,
               common::ErrorCode::InvalidParameter,
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
      RG_THROW(common::ErrorCode::InvalidParameter,
               "unsupported comparison operator: " + comparison.op);
    }
    case ast::ASTNodeType::kStringPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::StringPredicateExpression>(expression);
      RG_CHECK(predicate.left != nullptr && predicate.right != nullptr,
               common::ErrorCode::InvalidParameter,
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
      RG_THROW(common::ErrorCode::InvalidParameter,
               "unsupported string predicate: " + predicate.op);
    }
    case ast::ASTNodeType::kListPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::ListPredicateExpression>(expression);
      RG_CHECK(predicate.element != nullptr && predicate.list != nullptr,
               common::ErrorCode::InvalidParameter,
               "list predicate expression is incomplete");
      Value element =
          EvaluateExpression(*predicate.element, row, precomputed, context);
      Value list =
          EvaluateExpression(*predicate.list, row, precomputed, context);
      if (list.IsNull()) {
        return Value::Null();
      }
      RG_CHECK(list.IsList(), common::ErrorCode::InvalidParameter,
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
      RG_CHECK(predicate.expr != nullptr, common::ErrorCode::InvalidParameter,
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
      RG_CHECK(false, common::ErrorCode::InvalidParameter,
               "label predicate requires a graph entity");
    }
    case ast::ASTNodeType::kNullPredicateExpression: {
      const auto &predicate =
          ast::CastAst<ast::NullPredicateExpression>(expression);
      RG_CHECK(predicate.operand != nullptr,
               common::ErrorCode::InvalidParameter,
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
    case ast::ASTNodeType::kReduceExpression:
      return EvaluateReduce(ast::CastAst<ast::ReduceExpression>(expression),
                            row, precomputed, context);
    case ast::ASTNodeType::kPatternComprehension:
      return EvaluateLocallyCorrelatedPatternComprehension(
          ast::CastAst<ast::PatternComprehension>(expression), row, precomputed,
          context);
    case ast::ASTNodeType::kExistentialSubquery:
      return EvaluateLocallyCorrelatedExistentialSubquery(
          ast::CastAst<ast::ExistentialSubquery>(expression), row, precomputed,
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
      RG_CHECK(parenthesized.expr != nullptr,
               common::ErrorCode::InvalidParameter,
               "parenthesized expression is empty");
      return EvaluateExpression(*parenthesized.expr, row, precomputed, context);
    }
    default:
      RG_THROW(common::ErrorCode::InvalidParameter,
               "unsupported expression in executor: " +
                   std::string(ast::ToString(expression.node_type)));
  }
}

Value EvaluateExpression(const ast::Expression &expression,
                         ExecutionContext context) {
  ExpressionBindings bindings;
  return EvaluateExpression(expression, bindings, {}, context);
}

}  // namespace rg
