#include "semantic_validator.h"

#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast_equal.h"
#include "ast_exception.h"
#include "ast_walker.h"
#include "builtin_function.h"
#include "builtin_procedure.h"
#include "common/exception.h"
#include "expression_dependency.h"
#include "expression_to_string.h"

namespace ast {
namespace {

bool IsAggregateFunction(std::string_view function_name) {
  const BuiltinFunction *function = FindBuiltinFunction(function_name);
  return function != nullptr && function->aggregate;
}

Expression *UnwrapParenthesized(Expression *expression) {
  Expression *unwrapped = expression;
  while (unwrapped != nullptr &&
         unwrapped->Is(ASTNodeType::kParenthesizedExpression)) {
    auto *parenthesized = CastAst<ParenthesizedExpression>(unwrapped);
    CHECK(parenthesized->expr != nullptr, common::InternalError,
          "parenthesized expression is null");
    unwrapped = parenthesized->expr.get();
  }
  return unwrapped;
}

class AggregationScanner final : public ASTWalker {
 public:
  bool Scan(Expression &expression) {
    contains_ = false;
    expression.Accept(*this);
    return contains_;
  }

 protected:
  void Visit(FunctionInvocation &node) override {
    if (IsAggregateFunction(node.function_name)) {
      contains_ = true;
    }
    ASTWalker::Visit(node);
  }

  void Visit(CountStarExpression &node) override {
    (void)node;
    contains_ = true;
  }

 private:
  bool contains_ = false;
};

bool ContainsAggregation(Expression *expression) {
  if (expression == nullptr) {
    return false;
  }
  AggregationScanner scanner;
  return scanner.Scan(*expression);
}

bool IsTopLevelAggregation(Expression *expression) {
  Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr) {
    return false;
  }
  if (unwrapped->Is(ASTNodeType::kCountStarExpression)) {
    return true;
  }
  if (!unwrapped->Is(ASTNodeType::kFunctionInvocation)) {
    return false;
  }
  const auto *function = CastAst<FunctionInvocation>(unwrapped);
  return IsAggregateFunction(function->function_name);
}

const Expression *UnwrapParenthesized(const Expression *expression) {
  return UnwrapParenthesized(const_cast<Expression *>(expression));
}

bool IsSimpleGroupingExpression(const Expression *expression) {
  const Expression *unwrapped = UnwrapParenthesized(expression);
  return unwrapped != nullptr &&
         (unwrapped->Is(ASTNodeType::kVariable) ||
          unwrapped->Is(ASTNodeType::kPropertyExpression));
}

class MixedAggregationScanner final : public ASTWalker {
 public:
  explicit MixedAggregationScanner(
      std::vector<const Expression *> grouping_expressions,
      std::unordered_set<std::string> grouping_variables = {})
      : grouping_expressions_(std::move(grouping_expressions)),
        grouping_variables_(std::move(grouping_variables)) {}

  void Scan(Expression &expression) {
    ambiguous_ = false;
    invalid_comprehension_aggregation_ = false;
    local_scopes_.clear();
    expression.Accept(*this);
  }

  [[nodiscard]] bool Ambiguous() const noexcept { return ambiguous_; }
  [[nodiscard]] bool InvalidComprehensionAggregation() const noexcept {
    return invalid_comprehension_aggregation_;
  }

 protected:
  void Visit(FunctionInvocation &node) override {
    if (IsAggregateFunction(node.function_name)) {
      return;
    }
    ASTWalker::Visit(node);
  }

  void Visit(CountStarExpression &node) override { (void)node; }

  void Visit(Variable &node) override {
    if (!IsLocal(node.name) && !IsGroupingExpression(node) &&
        !grouping_variables_.contains(node.name)) {
      ambiguous_ = true;
    }
  }

  void Visit(PropertyExpression &node) override {
    if (!ReferencesLocal(node) && IsGroupingExpression(node)) {
      return;
    }
    ASTWalker::Visit(node);
  }

  void Visit(ListComprehension &node) override {
    WalkMaybe(node.list_expr);
    PushLocal(node.variable);
    if (ContainsAggregation(node.where_expr.get()) ||
        ContainsAggregation(node.eval_expr.get())) {
      invalid_comprehension_aggregation_ = true;
    }
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopLocal();
  }

  void Visit(AllQuantifier &node) override { VisitQuantifier(node); }
  void Visit(AnyQuantifier &node) override { VisitQuantifier(node); }
  void Visit(NoneQuantifier &node) override { VisitQuantifier(node); }
  void Visit(SingleQuantifier &node) override { VisitQuantifier(node); }

 private:
  [[nodiscard]] bool IsGroupingExpression(const Expression &expression) const {
    for (const Expression *grouping : grouping_expressions_) {
      if (grouping != nullptr && ASTEqual::Equal(&expression, grouping)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool IsLocal(const std::string &name) const {
    for (auto scope = local_scopes_.rbegin(); scope != local_scopes_.rend();
         ++scope) {
      if (scope->contains(name)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool ReferencesLocal(const Expression &expression) const {
    const std::unordered_set<std::string> dependencies =
        CollectExpressionDependencies(expression);
    for (const auto &dependency : dependencies) {
      if (IsLocal(dependency)) {
        return true;
      }
    }
    return false;
  }

  void VisitQuantifier(Quantifier &node) {
    WalkMaybe(node.list_expr);
    PushLocal(node.variable);
    if (ContainsAggregation(node.predicate.get())) {
      invalid_comprehension_aggregation_ = true;
    }
    WalkMaybe(node.predicate);
    PopLocal();
  }

  void PushLocal(const std::string &name) {
    local_scopes_.emplace_back();
    if (!name.empty()) {
      local_scopes_.back().insert(name);
    }
  }

  void PopLocal() { local_scopes_.pop_back(); }

  std::vector<const Expression *> grouping_expressions_;
  std::unordered_set<std::string> grouping_variables_;
  std::vector<std::unordered_set<std::string>> local_scopes_;
  bool ambiguous_ = false;
  bool invalid_comprehension_aggregation_ = false;
};

bool ProjectionContainsAggregation(const ProjectionBody &body) {
  for (const auto &item : body.items) {
    if (item != nullptr && ContainsAggregation(item->expression.get())) {
      return true;
    }
  }
  return false;
}

class PatternPredicateScanner final : public ASTWalker {
 public:
  bool Scan(Expression &expression) {
    contains_ = false;
    expression.Accept(*this);
    return contains_;
  }

 protected:
  void Visit(PatternPredicateExpression &node) override {
    (void)node;
    contains_ = true;
  }

 private:
  bool contains_ = false;
};

bool ContainsPatternPredicate(Expression *expression) {
  if (expression == nullptr) {
    return false;
  }
  PatternPredicateScanner scanner;
  return scanner.Scan(*expression);
}

class UpdatingClauseScanner final : public ASTWalker {
 public:
  bool Scan(ASTNode &node) {
    contains_ = false;
    node.Accept(*this);
    return contains_;
  }

 protected:
  void Visit(Create &node) override {
    (void)node;
    contains_ = true;
  }
  void Visit(Merge &node) override {
    (void)node;
    contains_ = true;
  }
  void Visit(Delete &node) override {
    (void)node;
    contains_ = true;
  }
  void Visit(Set &node) override {
    (void)node;
    contains_ = true;
  }
  void Visit(Remove &node) override {
    (void)node;
    contains_ = true;
  }

 private:
  bool contains_ = false;
};

const ProjectionBody *TerminalProjectionBody(const SingleQuery &query) {
  switch (query.node_type) {
    case ASTNodeType::kSinglePartQuery: {
      const auto &single = CastAst<SinglePartQuery>(query);
      CHECK(single.return_clause != nullptr, common::InternalError,
            "single part query return clause is null");
      CHECK(single.return_clause->body != nullptr, common::InternalError,
            "single part query return body is null");
      return single.return_clause->body.get();
    }
    case ASTNodeType::kMultiPartQuery: {
      const auto &multi = CastAst<MultiPartQuery>(query);
      CHECK(multi.final_single_part_query != nullptr, common::InternalError,
            "multi part query final single part is null");
      CHECK(multi.final_single_part_query->return_clause != nullptr,
            common::InternalError, "multi part query return clause is null");
      CHECK(multi.final_single_part_query->return_clause->body != nullptr,
            common::InternalError, "multi part query return body is null");
      return multi.final_single_part_query->return_clause->body.get();
    }
    default: {
      THROW(common::InternalError, "unsupported single query type");
    }
  }
}

std::optional<std::vector<std::string>> CollectTerminalColumns(
    const SingleQuery &query) {
  const ProjectionBody *body = TerminalProjectionBody(query);
  CHECK(body != nullptr, common::InternalError,
        "terminal projection body must not be null");
  if (body->star) {
    return std::nullopt;
  }

  std::vector<std::string> columns;
  columns.reserve(body->items.size());
  for (const auto &item : body->items) {
    CHECK(item != nullptr, common::InternalError, "projection item is null");
    if (!item->alias.empty()) {
      columns.push_back(item->alias);
      continue;
    }

    CHECK(item->expression != nullptr, common::InternalError,
          "projection item expression is null");
    const std::string name = ExpressionToString(*item->expression);
    CHECK(!name.empty(), common::InternalError,
          "projection item alias stringify failed");
    columns.push_back(name);
  }
  return columns;
}

class SemanticValidator : public ASTWalker {
 public:
  explicit SemanticValidator(std::vector<std::string> &errors)
      : errors_(errors) {}

  void Validate(ASTNode &node) {
    scope_stack_.clear();
    scope_stack_.emplace_back();
    reported_.clear();
    reported_semantic_errors_.clear();
    allow_any_depth_ = 0;
    aggregation_depth_ = 0;
    Walk(node);
    scope_stack_.clear();
  }

 protected:
  void Visit(RegularQuery &node) override {
    if (!node.unions.empty()) {
      const bool union_all = node.unions.front()->all;
      for (const auto &part : node.unions) {
        if (part != nullptr && part->all != union_all) {
          ReportSemantic("cannot mix UNION and UNION ALL");
          break;
        }
      }
    }
    const Scope base = CurrentScope();
    if (node.single_query) {
      PushScope(base);
      WalkMaybe(node.single_query);
      PopScope();
    }
    for (auto &part : node.unions) {
      if (!part || !part->query) {
        continue;
      }
      PushScope(base);
      WalkMaybe(part->query);
      PopScope();
    }

    ValidateUnionColumns(node);
  }

  void Visit(StandaloneCall &node) override {
    ValidateProcedureCall(node.procedure_name, node.arguments.size(),
                          node.yield_items);
    WalkList(node.arguments);

    Scope yield_scope = CurrentScope();
    if (!node.yield_star) {
      for (const auto &item : node.yield_items) {
        yield_scope.Add(item.variable);
      }
    }

    if (node.yield_where) {
      PushScope(yield_scope);
      if (node.yield_star) {
        allow_any_depth_++;
      }
      ValidateNoAggregation(node.yield_where.get(), "YIELD WHERE");
      WalkMaybe(node.yield_where);
      if (node.yield_star) {
        allow_any_depth_--;
      }
      PopScope();
    }

    ReplaceCurrentScope(yield_scope);
  }

  void Visit(Match &node) override {
    if (node.pattern) {
      ValidateRelationshipUniqueness(*node.pattern);
      BindPattern(*node.pattern);
    }
    WalkMaybe(node.pattern);
    ValidateNoAggregation(node.where.get(), "WHERE");
    ValidateBooleanExpression(node.where.get(), "WHERE");
    WalkMaybe(node.where);
  }

  void Visit(Unwind &node) override {
    WalkMaybe(node.expression);
    Define(node.variable, ListElementType(node.expression.get()));
  }

  void Visit(InQueryCall &node) override {
    ValidateProcedureCall(node.procedure_name, node.arguments.size(),
                          node.yield_items);
    WalkList(node.arguments);

    Scope yield_scope = CurrentScope();
    for (const auto &item : node.yield_items) {
      yield_scope.Add(item.variable);
    }

    if (node.yield_where) {
      PushScope(yield_scope);
      ValidateNoAggregation(node.yield_where.get(), "YIELD WHERE");
      WalkMaybe(node.yield_where);
      PopScope();
    }

    ReplaceCurrentScope(yield_scope);
  }

  void Visit(Create &node) override {
    if (node.pattern) {
      BindUpdatingPattern(*node.pattern, "CREATE");
    }
    ASTWalker::Visit(node);
  }

  void Visit(Merge &node) override {
    if (node.pattern_part) {
      BindUpdatingPatternPart(*node.pattern_part, "MERGE");
    }
    ASTWalker::Visit(node);
  }

  void Visit(Delete &node) override {
    for (const auto &expression : node.expressions) {
      const StaticValue type = InferType(expression.get());
      if (IsKnown(type) && type.type != StaticType::kNull &&
          type.type != StaticType::kNode &&
          type.type != StaticType::kRelationship &&
          type.type != StaticType::kPath) {
        ReportInvalidArgument("DELETE requires a node, relationship, or path");
      }
    }
    ASTWalker::Visit(node);
  }

  void Visit(ProjectionBody &node) override {
    ValidateProjectionAggregations(node);
    ValidateProjectionColumns(node);
    const Scope pre = CurrentScope();
    for (auto &item : node.items) {
      WalkMaybe(item);
    }

    const Scope projected = ScopeFromProjection(node, pre);
    const Scope order_scope = MergeScopes(pre, projected);
    ValidateRestrictedProjectionOrderBy(node, pre, projected);
    ValidateProjectionOrderByAggregations(node, projected);

    PushScope(order_scope);
    WalkList(node.order_by);
    ValidateNoAggregation(node.skip.get(), "SKIP");
    ValidatePagination(node.skip.get(), "SKIP");
    WalkMaybe(node.skip);
    ValidateNoAggregation(node.limit.get(), "LIMIT");
    ValidatePagination(node.limit.get(), "LIMIT");
    WalkMaybe(node.limit);
    PopScope();
  }

  void Visit(With &node) override {
    CHECK(node.body != nullptr, common::InternalError, "WITH body is null");
    ValidateWithAliases(*node.body);
    const Scope pre = CurrentScope();
    node.body->Accept(*this);

    const Scope projected = ScopeFromProjection(*node.body, pre);
    ReplaceCurrentScope(MergeScopes(pre, projected));
    ValidateNoAggregation(node.where.get(), "WHERE");
    ValidateBooleanExpression(node.where.get(), "WHERE");
    WalkMaybe(node.where);
    ReplaceCurrentScope(projected);
  }

  void Visit(ListComprehension &node) override {
    WalkMaybe(node.list_expr);

    PushScope(CurrentScope());
    Define(node.variable, ListElementType(node.list_expr.get()));
    ValidateBooleanExpression(node.where_expr.get(), "list comprehension");
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopScope();
  }

  void Visit(PatternComprehension &node) override {
    PushScope(CurrentScope());
    Define(node.variable, {StaticType::kPath});
    if (node.relationships_pattern) {
      BindRelationshipsPattern(*node.relationships_pattern);
    }
    ASTWalker::Visit(node);
    PopScope();
  }

  void Visit(PatternPredicateExpression &node) override {
    if (node.relationships_pattern) {
      ValidatePatternPredicateVariables(*node.relationships_pattern);
    }
    ASTWalker::Visit(node);
  }

  void Visit(AllQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(AnyQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(NoneQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(SingleQuantifier &node) override { ValidateQuantifier(node); }

  void Visit(ExistentialSubquery &node) override {
    if (node.query) {
      UpdatingClauseScanner scanner;
      if (scanner.Scan(*node.query)) {
        ReportSemantic("existential subquery cannot contain updates");
      }
      PushScope(CurrentScope());
      WalkMaybe(node.query);
      PopScope();
      return;
    }
    if (node.pattern) {
      PushScope(CurrentScope());
      BindPattern(*node.pattern);
      WalkMaybe(node.pattern);
      WalkMaybe(node.where_expr);
      PopScope();
    }
  }

  void Visit(Variable &node) override {
    if (allow_any_depth_ > 0) {
      return;
    }
    if (node.name.empty()) {
      return;
    }
    if (!IsDefined(node.name)) {
      ReportUndefined(node.name);
    }
  }

  void Visit(OrExpression &node) override {
    ValidateBooleanOperands(node, "OR");
  }

  void Visit(XorExpression &node) override {
    ValidateBooleanOperands(node, "XOR");
  }

  void Visit(AndExpression &node) override {
    ValidateBooleanOperands(node, "AND");
  }

  void Visit(NotExpression &node) override {
    ValidateBooleanExpression(node.operand.get(), "NOT");
    ASTWalker::Visit(node);
  }

  void Visit(SubtractExpression &node) override {
    ValidateNumericOperands(node, "subtraction");
  }

  void Visit(MultiplyExpression &node) override {
    ValidateNumericOperands(node, "multiplication");
  }

  void Visit(DivideExpression &node) override {
    ValidateNumericOperands(node, "division");
  }

  void Visit(ModuloExpression &node) override {
    ValidateNumericOperands(node, "modulo");
  }

  void Visit(PowerExpression &node) override {
    ValidateNumericOperands(node, "power");
  }

  void Visit(ListPredicateExpression &node) override {
    const StaticValue list_type = InferType(node.list.get());
    if (IsKnown(list_type) && list_type.type != StaticType::kNull &&
        list_type.type != StaticType::kList) {
      ReportInvalidArgument("IN requires a list on the right-hand side");
    }
    ASTWalker::Visit(node);
  }

  void Visit(PropertyExpression &node) override {
    const StaticValue object_type = InferType(node.object.get());
    if (IsKnown(object_type) && object_type.type != StaticType::kNull &&
        object_type.type != StaticType::kNode &&
        object_type.type != StaticType::kRelationship &&
        object_type.type != StaticType::kMap) {
      ReportInvalidArgument(
          "property access requires a node, relationship, or map");
    }
    ASTWalker::Visit(node);
  }

  void Visit(FunctionInvocation &node) override {
    const BuiltinFunction *function = FindBuiltinFunction(node.function_name);
    if (function == nullptr) {
      ReportSemantic("unknown function: " + node.function_name);
      ASTWalker::Visit(node);
      return;
    }
    if (!BuiltinFunctionAcceptsArgumentCount(*function,
                                             node.arguments.size())) {
      ReportSemantic(BuiltinFunctionArgumentCountError(*function));
    }
    if (node.distinct && !function->allows_distinct) {
      ReportSemantic("DISTINCT is only supported for aggregate functions");
    }
    if (!function->deterministic && aggregation_depth_ > 0) {
      ReportSemantic(
          "non-deterministic function is not allowed inside "
          "aggregation: " +
          function->name);
    }
    ValidateFunctionArguments(*function, node);

    const bool aggregate = function->aggregate;
    if (aggregate && aggregation_depth_ > 0) {
      ReportSemantic("nested aggregation is not allowed");
    }
    if (aggregate) {
      ++aggregation_depth_;
    }
    ASTWalker::Visit(node);
    if (aggregate) {
      --aggregation_depth_;
    }
  }

  void Visit(CountStarExpression &node) override {
    (void)node;
    if (aggregation_depth_ > 0) {
      ReportSemantic("nested aggregation is not allowed");
    }
  }

 private:
  enum class StaticType {
    kUnknown,
    kNull,
    kBoolean,
    kInteger,
    kFloat,
    kString,
    kList,
    kMap,
    kNode,
    kRelationship,
    kPath,
  };

  struct StaticValue {
    StaticType type = StaticType::kUnknown;
    StaticType element_type = StaticType::kUnknown;
  };

  void ValidateProcedureCall(
      std::string_view procedure_name, std::size_t argument_count,
      const std::vector<StandaloneCall::YieldItem> &yield_items) {
    const BuiltinProcedure *procedure = FindBuiltinProcedure(procedure_name);
    if (procedure == nullptr) {
      ReportSemantic("unknown procedure: " + std::string(procedure_name));
      return;
    }
    if (argument_count != procedure->argument_count) {
      ReportSemantic(procedure->name + " expects " +
                     std::to_string(procedure->argument_count) + " arguments");
    }
    for (const auto &item : yield_items) {
      const std::string &field =
          item.result_field.has_value() ? *item.result_field : item.variable;
      if (FindBuiltinProcedureYield(*procedure, field) == nullptr) {
        ReportSemantic("unknown yield field for " + procedure->name + ": " +
                       field);
      }
    }
  }

  void ValidateUnionColumns(const RegularQuery &query) {
    if (!query.single_query || query.unions.empty()) {
      return;
    }

    const auto main_columns = CollectTerminalColumns(*query.single_query);
    for (const auto &part : query.unions) {
      if (!part || !part->query) {
        continue;
      }
      const auto branch_columns = CollectTerminalColumns(*part->query);
      if (!main_columns.has_value() || !branch_columns.has_value()) {
        continue;
      }
      const auto &main_column_names = main_columns.value();
      const auto &branch_column_names = branch_columns.value();
      if (main_column_names.size() != branch_column_names.size()) {
        errors_.emplace_back(
            "UNION branches must return the same number of columns");
        continue;
      }
      for (size_t index = 0; index < main_column_names.size(); ++index) {
        if (main_column_names[index] != branch_column_names[index]) {
          errors_.emplace_back(
              "UNION branches must return the same column names by position");
          break;
        }
      }
    }
  }

  struct Scope {
    std::unordered_map<std::string, StaticValue> bindings;

    void Add(const std::string &name) { Add(name, StaticValue{}); }

    void Add(const std::string &name, StaticValue value) {
      if (!name.empty()) {
        bindings[name] = value;
      }
    }

    [[nodiscard]] bool Contains(const std::string &name) const {
      return bindings.contains(name);
    }

    [[nodiscard]] std::optional<StaticValue> Find(
        const std::string &name) const {
      const auto it = bindings.find(name);
      if (it == bindings.end()) {
        return std::nullopt;
      }
      return it->second;
    }
  };

  class RestrictedProjectionOrderByScanner final : public ASTWalker {
   public:
    RestrictedProjectionOrderByScanner(
        const Scope &pre_projection_scope, const Scope &projected_scope,
        const std::vector<const Expression *> &projected_expressions)
        : pre_projection_scope_(pre_projection_scope),
          projected_scope_(projected_scope),
          projected_expressions_(projected_expressions) {}

    [[nodiscard]] std::vector<std::string> Scan(Expression &expression) {
      restricted_variables_.clear();
      reported_restricted_variables_.clear();
      if (!IsProjectedExpression(expression)) {
        expression.Accept(*this);
      }
      return restricted_variables_;
    }

   protected:
    void WalkExpression(std::unique_ptr<Expression> &expr) override {
      if (!expr) {
        return;
      }
      if (IsProjectedExpression(*expr)) {
        return;
      }
      ASTWalker::WalkExpression(expr);
    }

    void Visit(Variable &node) override {
      if (node.name.empty() || projected_scope_.Contains(node.name) ||
          !pre_projection_scope_.Contains(node.name)) {
        return;
      }
      if (reported_restricted_variables_.insert(node.name).second) {
        restricted_variables_.push_back(node.name);
      }
    }

   private:
    [[nodiscard]] bool IsProjectedExpression(
        const Expression &expression) const {
      for (const Expression *projected_expression : projected_expressions_) {
        if (projected_expression != nullptr &&
            ASTEqual::Equal(&expression, projected_expression)) {
          return true;
        }
      }
      return false;
    }

    const Scope &pre_projection_scope_;
    const Scope &projected_scope_;
    const std::vector<const Expression *> &projected_expressions_;
    std::vector<std::string> restricted_variables_;
    std::unordered_set<std::string> reported_restricted_variables_;
  };

  Scope &CurrentScope() { return scope_stack_.back(); }
  [[nodiscard]] const Scope &CurrentScope() const {
    return scope_stack_.back();
  }

  void PushScope(const Scope &scope) { scope_stack_.push_back(scope); }
  void PushEmptyScope() { scope_stack_.emplace_back(); }
  void PopScope() { scope_stack_.pop_back(); }

  void Define(const std::string &name) { Define(name, StaticValue{}); }

  void Define(const std::string &name, StaticValue value) {
    CurrentScope().Add(name, value);
  }

  void ReplaceCurrentScope(const Scope &scope) { scope_stack_.back() = scope; }

  [[nodiscard]] bool IsDefined(const std::string &name) const {
    return Lookup(name).has_value();
  }

  [[nodiscard]] std::optional<StaticValue> Lookup(
      const std::string &name) const {
    for (const auto &it : std::ranges::reverse_view(scope_stack_)) {
      const auto value = it.Find(name);
      if (value.has_value()) {
        return value;
      }
    }
    return std::nullopt;
  }

  static Scope MergeScopes(const Scope &lhs, const Scope &rhs) {
    Scope out = lhs;
    out.bindings.insert(rhs.bindings.begin(), rhs.bindings.end());
    return out;
  }

  void ReportUndefined(const std::string &name) {
    if (reported_.insert(name).second) {
      errors_.push_back("undefined variable: " + name);
    }
  }

  void ReportSemantic(std::string message) {
    if (reported_semantic_errors_.insert(message).second) {
      errors_.push_back(std::move(message));
    }
  }

  void ValidateNoAggregation(Expression *expression, std::string_view context) {
    if (ContainsAggregation(expression)) {
      ReportSemantic(std::string(context) + " cannot contain aggregation");
    }
  }

  void ValidateNoAggregation(
      const std::vector<std::unique_ptr<SortItem>> &items,
      std::string_view context) {
    for (const auto &item : items) {
      if (item) {
        ValidateNoAggregation(item->expression.get(), context);
      }
    }
  }

  void ValidateProjectionAggregations(ProjectionBody &body) {
    std::vector<const Expression *> grouping_expressions;
    grouping_expressions.reserve(body.items.size());
    for (const auto &item : body.items) {
      if (!item || !item->expression ||
          ContainsAggregation(item->expression.get()) ||
          !IsSimpleGroupingExpression(item->expression.get())) {
        continue;
      }
      grouping_expressions.push_back(
          UnwrapParenthesized(item->expression.get()));
    }

    for (const auto &item : body.items) {
      if (!item || !item->expression) {
        continue;
      }
      Expression *expression = item->expression.get();
      if (!ContainsAggregation(expression) ||
          IsTopLevelAggregation(expression)) {
        continue;
      }
      MixedAggregationScanner scanner(grouping_expressions);
      scanner.Scan(*expression);
      if (scanner.InvalidComprehensionAggregation()) {
        ReportSemantic(
            "aggregation is not allowed in a comprehension predicate or "
            "mapping expression");
      }
      if (scanner.Ambiguous()) {
        ReportSemantic("ambiguous aggregation expression");
      }
    }
  }

  void ValidateProjectionOrderByAggregations(const ProjectionBody &body,
                                             const Scope &projected_scope) {
    bool contains_order_aggregation = false;
    for (const auto &item : body.order_by) {
      if (item != nullptr && ContainsAggregation(item->expression.get())) {
        contains_order_aggregation = true;
        break;
      }
    }
    if (!contains_order_aggregation) {
      return;
    }

    if (!ProjectionContainsAggregation(body)) {
      ReportSemantic("ORDER BY cannot contain aggregation");
      return;
    }

    std::vector<const Expression *> grouping_expressions;
    grouping_expressions.reserve(body.items.size());
    for (const auto &item : body.items) {
      if (!item || !item->expression ||
          ContainsAggregation(item->expression.get()) ||
          !IsSimpleGroupingExpression(item->expression.get())) {
        continue;
      }
      grouping_expressions.push_back(
          UnwrapParenthesized(item->expression.get()));
    }

    std::unordered_set<std::string> grouping_variables;
    grouping_variables.reserve(projected_scope.bindings.size());
    for (const auto &[name, value] : projected_scope.bindings) {
      (void)value;
      grouping_variables.insert(name);
    }

    for (const auto &item : body.order_by) {
      if (item == nullptr || item->expression == nullptr ||
          !ContainsAggregation(item->expression.get())) {
        continue;
      }
      MixedAggregationScanner scanner(grouping_expressions, grouping_variables);
      scanner.Scan(*item->expression);
      if (scanner.InvalidComprehensionAggregation()) {
        ReportSemantic(
            "aggregation is not allowed in a comprehension predicate or "
            "mapping expression");
      }
      if (scanner.Ambiguous()) {
        ReportSemantic("ambiguous aggregation expression");
      }
    }
  }

  void ValidateProjectionColumns(ProjectionBody &body) {
    std::unordered_set<std::string> names;
    for (const auto &item : body.items) {
      if (!item || !item->expression) {
        continue;
      }
      const std::string name = !item->alias.empty()
                                   ? item->alias
                                   : ExpressionToString(*item->expression);
      if (!name.empty() && !names.insert(name).second) {
        ReportSemantic("duplicate projection column: " + name);
      }
      if (ContainsPatternPredicate(item->expression.get())) {
        ReportSemantic("pattern expressions are not allowed in projections");
      }
    }
  }

  void ValidateWithAliases(const ProjectionBody &body) {
    for (const auto &item : body.items) {
      if (!item || !item->expression || !item->alias.empty() ||
          item->expression->Is(ASTNodeType::kVariable)) {
        continue;
      }
      ReportSemantic("WITH expressions must be aliased");
    }
  }

  void ValidateRestrictedProjectionOrderBy(const ProjectionBody &body,
                                           const Scope &pre_projection_scope,
                                           const Scope &projected_scope) {
    if (!body.distinct && !ProjectionContainsAggregation(body)) {
      return;
    }

    std::vector<const Expression *> projected_expressions;
    projected_expressions.reserve(body.items.size());
    for (const auto &item : body.items) {
      if (item != nullptr) {
        projected_expressions.push_back(item->expression.get());
      }
    }

    RestrictedProjectionOrderByScanner scanner(
        pre_projection_scope, projected_scope, projected_expressions);
    for (const auto &item : body.order_by) {
      if (item == nullptr || item->expression == nullptr) {
        continue;
      }
      for (const std::string &name : scanner.Scan(*item->expression)) {
        ReportRestrictedProjectionOrderBy(name);
      }
    }
  }

  void ReportRestrictedProjectionOrderBy(const std::string &name) {
    ReportSemantic(
        "In a WITH/RETURN with DISTINCT or an aggregation, it is "
        "not possible to access variables declared before the "
        "WITH/RETURN: " +
        name);
  }

  static bool IsKnown(StaticValue value) {
    return value.type != StaticType::kUnknown;
  }

  static bool IsBooleanCompatible(StaticValue value) {
    return value.type == StaticType::kUnknown ||
           value.type == StaticType::kNull ||
           value.type == StaticType::kBoolean;
  }

  static bool IsNumericCompatible(StaticValue value) {
    return value.type == StaticType::kUnknown ||
           value.type == StaticType::kNull ||
           value.type == StaticType::kInteger ||
           value.type == StaticType::kFloat;
  }

  void ReportInvalidArgument(std::string_view detail) {
    ReportSemantic("invalid argument type: " + std::string(detail));
  }

  void ReportVariableTypeConflict(const std::string &name) {
    ReportSemantic("variable type conflict: " + name);
  }

  void ReportVariableAlreadyBound(const std::string &name) {
    ReportSemantic("variable already bound: " + name);
  }

  StaticValue InferType(const Expression *expression) const {
    if (expression == nullptr) {
      return {};
    }
    switch (expression->node_type) {
      case ASTNodeType::kVariable: {
        const auto &variable = CastAst<Variable>(*expression);
        return Lookup(variable.name).value_or(StaticValue{});
      }
      case ASTNodeType::kBooleanLiteral:
      case ASTNodeType::kOrExpression:
      case ASTNodeType::kXorExpression:
      case ASTNodeType::kAndExpression:
      case ASTNodeType::kComparisonExpression:
      case ASTNodeType::kComparisonChainExpression:
      case ASTNodeType::kNotExpression:
      case ASTNodeType::kStringPredicateExpression:
      case ASTNodeType::kListPredicateExpression:
      case ASTNodeType::kLabelPredicateExpression:
      case ASTNodeType::kNullPredicateExpression:
      case ASTNodeType::kPatternPredicateExpression:
      case ASTNodeType::kAllQuantifier:
      case ASTNodeType::kAnyQuantifier:
      case ASTNodeType::kNoneQuantifier:
      case ASTNodeType::kSingleQuantifier:
      case ASTNodeType::kExistentialSubquery:
        return {StaticType::kBoolean};
      case ASTNodeType::kIntegerLiteral:
      case ASTNodeType::kCountStarExpression:
        return {StaticType::kInteger};
      case ASTNodeType::kDoubleLiteral:
        return {StaticType::kFloat};
      case ASTNodeType::kStringLiteral:
        return {StaticType::kString};
      case ASTNodeType::kNullLiteral:
        return {StaticType::kNull};
      case ASTNodeType::kListLiteral: {
        const auto &list = CastAst<ListLiteral>(*expression);
        StaticType element_type = StaticType::kUnknown;
        for (const auto &element : list.elements) {
          const StaticValue current = InferType(element.get());
          if (current.type == StaticType::kUnknown ||
              current.type == StaticType::kNull) {
            continue;
          }
          if (element_type == StaticType::kUnknown) {
            element_type = current.type;
          } else if (element_type != current.type) {
            element_type = StaticType::kUnknown;
            break;
          }
        }
        return {StaticType::kList, element_type};
      }
      case ASTNodeType::kMapLiteral:
        return {StaticType::kMap};
      case ASTNodeType::kParenthesizedExpression: {
        const auto &parenthesized =
            CastAst<ParenthesizedExpression>(*expression);
        return InferType(parenthesized.expr.get());
      }
      case ASTNodeType::kUnaryPlusExpression:
      case ASTNodeType::kUnaryMinusExpression: {
        const auto &unary = CastAst<UnaryExpression>(*expression);
        return InferType(unary.operand.get());
      }
      case ASTNodeType::kSubtractExpression:
      case ASTNodeType::kMultiplyExpression:
      case ASTNodeType::kDivideExpression:
      case ASTNodeType::kModuloExpression:
      case ASTNodeType::kPowerExpression: {
        const auto &binary = CastAst<BinaryExpression>(*expression);
        const StaticValue left = InferType(binary.left.get());
        const StaticValue right = InferType(binary.right.get());
        if (left.type == StaticType::kFloat ||
            right.type == StaticType::kFloat) {
          return {StaticType::kFloat};
        }
        if (left.type == StaticType::kInteger &&
            right.type == StaticType::kInteger) {
          return {StaticType::kInteger};
        }
        return {};
      }
      case ASTNodeType::kAddExpression: {
        const auto &binary = CastAst<BinaryExpression>(*expression);
        const StaticValue left = InferType(binary.left.get());
        const StaticValue right = InferType(binary.right.get());
        if (left.type == StaticType::kList || right.type == StaticType::kList) {
          return {StaticType::kList};
        }
        if (left.type == StaticType::kFloat ||
            right.type == StaticType::kFloat) {
          return {StaticType::kFloat};
        }
        if (left.type == StaticType::kInteger &&
            right.type == StaticType::kInteger) {
          return {StaticType::kInteger};
        }
        if (left.type == StaticType::kString &&
            right.type == StaticType::kString) {
          return {StaticType::kString};
        }
        return {};
      }
      case ASTNodeType::kListComprehension:
      case ASTNodeType::kPatternComprehension:
      case ASTNodeType::kListSliceExpression:
        return {StaticType::kList};
      case ASTNodeType::kListIndexExpression: {
        const auto &index = CastAst<ListIndexExpression>(*expression);
        return ListElementType(index.list.get());
      }
      case ASTNodeType::kFunctionInvocation:
        return InferFunctionType(CastAst<FunctionInvocation>(*expression));
      default:
        return {};
    }
  }

  StaticValue InferFunctionType(const FunctionInvocation &function) const {
    const BuiltinFunction *builtin =
        FindBuiltinFunction(function.function_name);
    if (builtin == nullptr) {
      return {};
    }
    switch (builtin->kind) {
      case BuiltinFunctionKind::kCount:
      case BuiltinFunctionKind::kId:
      case BuiltinFunctionKind::kLength:
      case BuiltinFunctionKind::kSign:
      case BuiltinFunctionKind::kSize:
      case BuiltinFunctionKind::kToInteger:
        return {StaticType::kInteger};
      case BuiltinFunctionKind::kAverage:
      case BuiltinFunctionKind::kCeil:
      case BuiltinFunctionKind::kRand:
      case BuiltinFunctionKind::kSqrt:
      case BuiltinFunctionKind::kToFloat:
        return {StaticType::kFloat};
      case BuiltinFunctionKind::kToBoolean:
      case BuiltinFunctionKind::kIsEmpty:
        return {StaticType::kBoolean};
      case BuiltinFunctionKind::kSplit:
        return {StaticType::kList, StaticType::kString};
      case BuiltinFunctionKind::kKeys:
      case BuiltinFunctionKind::kLabels:
        return {StaticType::kList, StaticType::kString};
      case BuiltinFunctionKind::kNodes:
        return {StaticType::kList, StaticType::kNode};
      case BuiltinFunctionKind::kRelationships:
        return {StaticType::kList, StaticType::kRelationship};
      case BuiltinFunctionKind::kRange:
        return {StaticType::kList, StaticType::kInteger};
      case BuiltinFunctionKind::kTail: {
        const StaticValue input =
            function.arguments.empty()
                ? StaticValue{}
                : InferType(function.arguments.front().get());
        return {StaticType::kList, input.element_type};
      }
      case BuiltinFunctionKind::kProperties:
        return {StaticType::kMap};
      case BuiltinFunctionKind::kSubstring:
      case BuiltinFunctionKind::kToLower:
      case BuiltinFunctionKind::kToString:
      case BuiltinFunctionKind::kToUpper:
      case BuiltinFunctionKind::kTrim:
      case BuiltinFunctionKind::kType:
        return {StaticType::kString};
      case BuiltinFunctionKind::kCollect:
        return {StaticType::kList,
                function.arguments.empty()
                    ? StaticType::kUnknown
                    : InferType(function.arguments.front().get()).type};
      case BuiltinFunctionKind::kCoalesce:
        for (const auto &argument : function.arguments) {
          const StaticValue type = InferType(argument.get());
          if (type.type != StaticType::kNull && IsKnown(type)) {
            return type;
          }
        }
        return {};
      case BuiltinFunctionKind::kDate:
      case BuiltinFunctionKind::kDateRealtime:
      case BuiltinFunctionKind::kDateStatement:
      case BuiltinFunctionKind::kDateTransaction:
      case BuiltinFunctionKind::kDateTruncate:
      case BuiltinFunctionKind::kDateTime:
      case BuiltinFunctionKind::kDateTimeFromEpoch:
      case BuiltinFunctionKind::kDateTimeFromEpochMillis:
      case BuiltinFunctionKind::kDateTimeRealtime:
      case BuiltinFunctionKind::kDateTimeStatement:
      case BuiltinFunctionKind::kDateTimeTransaction:
      case BuiltinFunctionKind::kDateTimeTruncate:
      case BuiltinFunctionKind::kDuration:
      case BuiltinFunctionKind::kDurationBetween:
      case BuiltinFunctionKind::kDurationInDays:
      case BuiltinFunctionKind::kDurationInMonths:
      case BuiltinFunctionKind::kDurationInSeconds:
      case BuiltinFunctionKind::kLocalDateTime:
      case BuiltinFunctionKind::kLocalDateTimeRealtime:
      case BuiltinFunctionKind::kLocalDateTimeStatement:
      case BuiltinFunctionKind::kLocalDateTimeTransaction:
      case BuiltinFunctionKind::kLocalDateTimeTruncate:
      case BuiltinFunctionKind::kLocalTime:
      case BuiltinFunctionKind::kLocalTimeRealtime:
      case BuiltinFunctionKind::kLocalTimeStatement:
      case BuiltinFunctionKind::kLocalTimeTransaction:
      case BuiltinFunctionKind::kLocalTimeTruncate:
      case BuiltinFunctionKind::kTime:
      case BuiltinFunctionKind::kTimeRealtime:
      case BuiltinFunctionKind::kTimeStatement:
      case BuiltinFunctionKind::kTimeTransaction:
      case BuiltinFunctionKind::kTimeTruncate:
        return {};
      case BuiltinFunctionKind::kAbs:
      case BuiltinFunctionKind::kLast:
      case BuiltinFunctionKind::kReverse:
        return function.arguments.empty()
                   ? StaticValue{}
                   : (builtin->kind == BuiltinFunctionKind::kLast
                          ? ListElementType(function.arguments[0].get())
                          : InferType(function.arguments[0].get()));
      case BuiltinFunctionKind::kMaximum:
      case BuiltinFunctionKind::kMinimum:
      case BuiltinFunctionKind::kSum:
        return function.arguments.empty()
                   ? StaticValue{}
                   : InferType(function.arguments[0].get());
    }
    return {};
  }

  StaticValue ListElementType(const Expression *expression) const {
    const StaticValue list_type = InferType(expression);
    if (list_type.type != StaticType::kList) {
      return {};
    }
    return {list_type.element_type};
  }

  void ValidateBooleanExpression(const Expression *expression,
                                 std::string_view context) {
    if (expression == nullptr) {
      return;
    }
    if (!IsBooleanCompatible(InferType(expression))) {
      ReportInvalidArgument(std::string(context) +
                            " requires a boolean expression");
    }
  }

  void ValidateBooleanOperands(BinaryExpression &node,
                               std::string_view operator_name) {
    if (!IsBooleanCompatible(InferType(node.left.get())) ||
        !IsBooleanCompatible(InferType(node.right.get()))) {
      ReportInvalidArgument(std::string(operator_name) +
                            " requires boolean operands");
    }
    ASTWalker::Visit(node);
  }

  void ValidateNumericOperands(BinaryExpression &node,
                               std::string_view operator_name) {
    if (!IsNumericCompatible(InferType(node.left.get())) ||
        !IsNumericCompatible(InferType(node.right.get()))) {
      ReportInvalidArgument(std::string(operator_name) +
                            " requires numeric operands");
    }
    ASTWalker::Visit(node);
  }

  void ValidatePagination(const Expression *expression,
                          std::string_view clause) {
    if (expression == nullptr) {
      return;
    }
    const StaticValue type = InferType(expression);
    if (IsKnown(type) && type.type != StaticType::kNull &&
        type.type != StaticType::kInteger) {
      ReportInvalidArgument(std::string(clause) +
                            " requires an integer expression");
    }
    std::unordered_set<std::string> scope_names;
    for (const auto &entry : CurrentScope().bindings) {
      scope_names.insert(entry.first);
    }
    if (!CollectExpressionDependencies(*expression, std::move(scope_names))
             .empty()) {
      ReportSemantic(std::string(clause) + " expression must be constant");
    }
    const auto constant = ConstantInteger(expression);
    if (constant.has_value() && *constant < 0) {
      ReportSemantic(std::string(clause) + " expression must be non-negative");
    }
  }

  static std::optional<int64_t> ConstantInteger(const Expression *expression) {
    const Expression *unwrapped =
        UnwrapParenthesized(const_cast<Expression *>(expression));
    if (unwrapped == nullptr) {
      return std::nullopt;
    }
    if (unwrapped->Is(ASTNodeType::kIntegerLiteral)) {
      return CastAst<IntegerLiteral>(*unwrapped).value;
    }
    if (!unwrapped->Is(ASTNodeType::kUnaryMinusExpression)) {
      return std::nullopt;
    }
    const auto &unary = CastAst<UnaryMinusExpression>(*unwrapped);
    const auto magnitude = ConstantInteger(unary.operand.get());
    if (!magnitude.has_value() || *magnitude < 0) {
      return std::nullopt;
    }
    return -*magnitude;
  }

  static bool AcceptsType(StaticValue value,
                          std::initializer_list<StaticType> accepted) {
    if (!IsKnown(value) || value.type == StaticType::kNull) {
      return true;
    }
    return std::ranges::find(accepted, value.type) != accepted.end();
  }

  void ValidateFunctionArguments(const BuiltinFunction &function,
                                 const FunctionInvocation &invocation) {
    if (invocation.arguments.empty()) {
      return;
    }
    const StaticValue argument = InferType(invocation.arguments[0].get());
    bool accepted = true;
    switch (function.kind) {
      case BuiltinFunctionKind::kAbs:
      case BuiltinFunctionKind::kCeil:
      case BuiltinFunctionKind::kSign:
      case BuiltinFunctionKind::kSqrt:
        accepted = IsNumericCompatible(argument);
        break;
      case BuiltinFunctionKind::kId:
        accepted = AcceptsType(argument,
                               {StaticType::kNode, StaticType::kRelationship});
        break;
      case BuiltinFunctionKind::kKeys:
      case BuiltinFunctionKind::kProperties:
        accepted = AcceptsType(
            argument,
            {StaticType::kNode, StaticType::kRelationship, StaticType::kMap});
        break;
      case BuiltinFunctionKind::kLabels:
        accepted = AcceptsType(argument, {StaticType::kNode});
        break;
      case BuiltinFunctionKind::kType:
        accepted = AcceptsType(argument, {StaticType::kRelationship});
        break;
      case BuiltinFunctionKind::kNodes:
      case BuiltinFunctionKind::kRelationships:
        accepted = AcceptsType(argument, {StaticType::kPath});
        break;
      case BuiltinFunctionKind::kLength:
        accepted = AcceptsType(argument, {StaticType::kPath, StaticType::kList,
                                          StaticType::kString});
        break;
      case BuiltinFunctionKind::kSize:
      case BuiltinFunctionKind::kIsEmpty:
        accepted =
            AcceptsType(argument, {StaticType::kList, StaticType::kString});
        break;
      case BuiltinFunctionKind::kLast:
      case BuiltinFunctionKind::kTail:
        accepted = AcceptsType(argument, {StaticType::kList});
        break;
      case BuiltinFunctionKind::kReverse:
        accepted =
            AcceptsType(argument, {StaticType::kList, StaticType::kString});
        break;
      case BuiltinFunctionKind::kSubstring:
        accepted = AcceptsType(argument, {StaticType::kString});
        for (std::size_t index = 1;
             accepted && index < invocation.arguments.size(); ++index) {
          accepted = AcceptsType(InferType(invocation.arguments[index].get()),
                                 {StaticType::kInteger});
        }
        break;
      case BuiltinFunctionKind::kSplit:
      case BuiltinFunctionKind::kToLower:
      case BuiltinFunctionKind::kToUpper:
      case BuiltinFunctionKind::kTrim:
        accepted = AcceptsType(argument, {StaticType::kString});
        break;
      case BuiltinFunctionKind::kDate:
      case BuiltinFunctionKind::kDateTime:
      case BuiltinFunctionKind::kDuration:
      case BuiltinFunctionKind::kLocalDateTime:
      case BuiltinFunctionKind::kLocalTime:
      case BuiltinFunctionKind::kTime:
        accepted =
            AcceptsType(argument, {StaticType::kString, StaticType::kMap});
        break;
      case BuiltinFunctionKind::kDateRealtime:
      case BuiltinFunctionKind::kDateStatement:
      case BuiltinFunctionKind::kDateTransaction:
      case BuiltinFunctionKind::kDateTimeRealtime:
      case BuiltinFunctionKind::kDateTimeStatement:
      case BuiltinFunctionKind::kDateTimeTransaction:
      case BuiltinFunctionKind::kLocalDateTimeRealtime:
      case BuiltinFunctionKind::kLocalDateTimeStatement:
      case BuiltinFunctionKind::kLocalDateTimeTransaction:
      case BuiltinFunctionKind::kLocalTimeRealtime:
      case BuiltinFunctionKind::kLocalTimeStatement:
      case BuiltinFunctionKind::kLocalTimeTransaction:
      case BuiltinFunctionKind::kTimeRealtime:
      case BuiltinFunctionKind::kTimeStatement:
      case BuiltinFunctionKind::kTimeTransaction:
        accepted = AcceptsType(argument, {StaticType::kString});
        break;
      case BuiltinFunctionKind::kDateTimeFromEpoch:
        accepted = AcceptsType(argument, {StaticType::kInteger});
        if (accepted && invocation.arguments.size() > 1) {
          accepted = AcceptsType(InferType(invocation.arguments[1].get()),
                                 {StaticType::kInteger});
        }
        break;
      case BuiltinFunctionKind::kDateTimeFromEpochMillis:
        accepted = AcceptsType(argument, {StaticType::kInteger});
        break;
      case BuiltinFunctionKind::kDateTruncate:
      case BuiltinFunctionKind::kDateTimeTruncate:
      case BuiltinFunctionKind::kLocalDateTimeTruncate:
      case BuiltinFunctionKind::kLocalTimeTruncate:
      case BuiltinFunctionKind::kTimeTruncate:
        accepted = AcceptsType(argument, {StaticType::kString});
        if (accepted && invocation.arguments.size() == 3) {
          accepted = AcceptsType(InferType(invocation.arguments[2].get()),
                                 {StaticType::kMap});
        }
        break;
      default:
        break;
    }
    if (!accepted) {
      ReportInvalidArgument(function.name + "() argument has invalid type");
    }
  }

  void BindPattern(const Pattern &pattern) {
    for (const auto &part : pattern.parts) {
      if (part) {
        BindPatternPart(*part);
      }
    }
  }

  void BindPatternPart(const PatternPart &part) {
    if (!part.variable.empty()) {
      if (IsDefined(part.variable)) {
        ReportVariableAlreadyBound(part.variable);
      } else {
        Define(part.variable, {StaticType::kPath});
      }
    }
    if (part.element) {
      BindPatternElement(*part.element);
    }
  }

  void BindPatternElement(const PatternElement &element) {
    if (element.node_pattern) {
      BindPatternVariable(element.node_pattern->variable, StaticType::kNode);
    }
    for (const auto &link : element.chain) {
      if (link.first && link.first->detail) {
        BindRelationshipVariable(*link.first->detail);
      }
      if (link.second) {
        BindPatternVariable(link.second->variable, StaticType::kNode);
      }
    }
  }

  void BindRelationshipsPattern(const RelationshipsPattern &pattern) {
    if (pattern.node_pattern) {
      BindPatternVariable(pattern.node_pattern->variable, StaticType::kNode);
    }
    for (const auto &link : pattern.chain) {
      if (link.first && link.first->detail) {
        BindRelationshipVariable(*link.first->detail);
      }
      if (link.second) {
        BindPatternVariable(link.second->variable, StaticType::kNode);
      }
    }
  }

  void BindPatternVariable(const std::string &name, StaticType type) {
    if (name.empty()) {
      return;
    }
    const auto existing = Lookup(name);
    if (existing.has_value() && existing->type != StaticType::kUnknown &&
        existing->type != StaticType::kNull && existing->type != type) {
      ReportVariableTypeConflict(name);
      return;
    }
    if (!existing.has_value()) {
      Define(name, {type});
    }
  }

  void BindRelationshipVariable(const RelationshipDetail &detail) {
    const StaticValue type =
        detail.range.has_value()
            ? StaticValue{StaticType::kList, StaticType::kRelationship}
            : StaticValue{StaticType::kRelationship};
    if (detail.variable.empty()) {
      return;
    }
    const auto existing = Lookup(detail.variable);
    if (existing.has_value() && existing->type != StaticType::kUnknown &&
        existing->type != StaticType::kNull && existing->type != type.type) {
      ReportVariableTypeConflict(detail.variable);
      return;
    }
    if (!existing.has_value()) {
      Define(detail.variable, type);
    }
  }

  void BindUpdatingPattern(const Pattern &pattern, std::string_view clause) {
    for (const auto &part : pattern.parts) {
      if (part) {
        BindUpdatingPatternPart(*part, clause);
      }
    }
  }

  void BindUpdatingPatternPart(const PatternPart &part,
                               std::string_view clause) {
    ValidateUpdatingPatternPart(part, clause);
    if (!part.variable.empty()) {
      if (IsDefined(part.variable)) {
        ReportVariableAlreadyBound(part.variable);
      } else {
        Define(part.variable, {StaticType::kPath});
      }
    }
    if (!part.element) {
      return;
    }

    const bool standalone_node = part.element->chain.empty();
    BindUpdatingNode(part.element->node_pattern.get(), clause, standalone_node);
    for (const auto &link : part.element->chain) {
      if (link.first && link.first->detail) {
        const std::string &name = link.first->detail->variable;
        if (!name.empty()) {
          if (IsDefined(name)) {
            ReportVariableAlreadyBound(name);
          } else {
            Define(name, {StaticType::kRelationship});
          }
        }
      }
      BindUpdatingNode(link.second.get(), clause, false);
    }
  }

  void BindUpdatingNode(const NodePattern *node, std::string_view clause,
                        bool standalone) {
    if (node == nullptr || node->variable.empty()) {
      return;
    }
    const auto existing = Lookup(node->variable);
    if (!existing.has_value()) {
      Define(node->variable, {StaticType::kNode});
      return;
    }
    if (existing->type != StaticType::kUnknown &&
        existing->type != StaticType::kNode) {
      ReportVariableTypeConflict(node->variable);
      return;
    }
    if (standalone || !node->labels.empty() || node->properties != nullptr) {
      (void)clause;
      ReportVariableAlreadyBound(node->variable);
    }
  }

  void ValidateUpdatingPatternPart(const PatternPart &part,
                                   std::string_view clause) {
    if (!part.element) {
      return;
    }
    ValidateUpdatingNodeProperties(part.element->node_pattern.get(), clause);
    for (const auto &link : part.element->chain) {
      if (link.first) {
        const bool directed = link.first->left_arrow != link.first->right_arrow;
        if (clause == "CREATE" && !directed) {
          ReportSemantic(std::string(clause) +
                         " relationships must have one direction");
        }
        const RelationshipDetail *detail = link.first->detail.get();
        if (detail == nullptr || detail->types.size() != 1) {
          ReportSemantic(std::string(clause) +
                         " relationships require exactly one type");
        }
        if (detail != nullptr && detail->range.has_value()) {
          ReportSemantic(std::string(clause) +
                         " cannot create variable-length relationships");
        }
        if (clause == "MERGE" && detail != nullptr && detail->properties &&
            detail->properties->parameter) {
          ReportSemantic("MERGE relationship properties cannot be a parameter");
        }
      }
      ValidateUpdatingNodeProperties(link.second.get(), clause);
    }
  }

  void ValidateUpdatingNodeProperties(const NodePattern *node,
                                      std::string_view clause) {
    if (clause == "MERGE" && node != nullptr && node->properties &&
        node->properties->parameter) {
      ReportSemantic("MERGE node properties cannot be a parameter");
    }
  }

  void ValidateRelationshipUniqueness(const Pattern &pattern) {
    std::unordered_set<std::string> relationships;
    for (const auto &part : pattern.parts) {
      if (!part || !part->element) {
        continue;
      }
      for (const auto &link : part->element->chain) {
        if (!link.first || !link.first->detail ||
            link.first->detail->variable.empty()) {
          continue;
        }
        const std::string &name = link.first->detail->variable;
        if (!relationships.insert(name).second) {
          ReportSemantic("relationship variable is reused in one pattern: " +
                         name);
        }
      }
    }
  }

  void ValidatePatternPredicateVariables(const RelationshipsPattern &pattern) {
    ValidatePatternPredicateVariable(pattern.node_pattern.get());
    for (const auto &link : pattern.chain) {
      if (link.first && link.first->detail) {
        ValidatePatternPredicateVariable(link.first->detail->variable);
      }
      ValidatePatternPredicateVariable(link.second.get());
    }
  }

  void ValidatePatternPredicateVariable(const NodePattern *node) {
    if (node != nullptr) {
      ValidatePatternPredicateVariable(node->variable);
    }
  }

  void ValidatePatternPredicateVariable(const std::string &name) {
    if (!name.empty() && !IsDefined(name)) {
      ReportUndefined(name);
    }
  }

  void CollectFromProjectionItem(const ProjectionItem &item, Scope &scope) {
    if (!item.alias.empty()) {
      scope.Add(item.alias, InferType(item.expression.get()));
      return;
    }
    if (item.expression && item.expression->Is(ASTNodeType::kVariable)) {
      const auto *var = CastAst<Variable>(item.expression.get());
      scope.Add(var->name, Lookup(var->name).value_or(StaticValue{}));
    }
  }

  [[nodiscard]] Scope ScopeFromProjection(const ProjectionBody &body,
                                          const Scope &fallback) {
    Scope scope = body.star ? fallback : Scope{};
    for (const auto &item : body.items) {
      if (item) {
        CollectFromProjectionItem(*item, scope);
      }
    }
    return scope;
  }

  void ValidateQuantifier(Quantifier &node) {
    WalkMaybe(node.list_expr);
    const StaticValue list_type = InferType(node.list_expr.get());
    if (IsKnown(list_type) && list_type.type != StaticType::kNull &&
        list_type.type != StaticType::kList) {
      ReportInvalidArgument("quantifier requires a list expression");
    }
    PushScope(CurrentScope());
    Define(node.variable, ListElementType(node.list_expr.get()));
    ValidateBooleanExpression(node.predicate.get(), "quantifier predicate");
    WalkMaybe(node.predicate);
    PopScope();
  }

  std::vector<Scope> scope_stack_;
  std::unordered_set<std::string> reported_;
  std::unordered_set<std::string> reported_semantic_errors_;
  std::vector<std::string> &errors_;
  int allow_any_depth_ = 0;
  int aggregation_depth_ = 0;
};

}  // namespace

void ValidateStatement(ASTNode &node) {
  std::vector<std::string> errors;
  SemanticValidator validator(errors);
  validator.Validate(node);
  if (!errors.empty()) {
    THROW(SemanticError, std::move(errors));
  }
}

}  // namespace ast
