#include "semantic_validator.h"

#include <cctype>
#include <optional>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast_equal.h"
#include "ast_exception.h"
#include "ast_walker.h"
#include "builtin_procedure.h"
#include "common/exception.h"
#include "expression_to_string.h"

namespace ast {
namespace {

std::string LowerAscii(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool IsAggregateFunction(std::string_view function_name) {
  const std::string name = LowerAscii(function_name);
  static const std::unordered_set<std::string> kAggregates = {
      "avg",
      "collect",
      "count",
      "max",
      "min",
      "percentilecont",
      "percentiledisc",
      "stdev",
      "stdevp",
      "sum",
  };
  return kAggregates.contains(name);
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

bool ProjectionContainsAggregation(const ProjectionBody &body) {
  for (const auto &item : body.items) {
    if (item != nullptr && ContainsAggregation(item->expression.get())) {
      return true;
    }
  }
  return false;
}

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
      CollectFromPattern(*node.pattern, CurrentScope());
    }
    WalkMaybe(node.pattern);
    ValidateNoAggregation(node.where.get(), "WHERE");
    WalkMaybe(node.where);
  }

  void Visit(Unwind &node) override {
    ASTWalker::Visit(node);
    Define(node.variable);
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
      CollectFromPattern(*node.pattern, CurrentScope());
    }
    ASTWalker::Visit(node);
  }

  void Visit(Merge &node) override {
    if (node.pattern_part) {
      CollectFromPatternPart(*node.pattern_part, CurrentScope());
    }
    ASTWalker::Visit(node);
  }

  void Visit(ProjectionBody &node) override {
    ValidateProjectionAggregations(node);
    const Scope pre = CurrentScope();
    for (auto &item : node.items) {
      WalkMaybe(item);
    }

    const Scope projected = ScopeFromProjection(node, pre);
    const Scope order_scope = MergeScopes(pre, projected);
    ValidateRestrictedProjectionOrderBy(node, pre, projected);

    PushScope(order_scope);
    ValidateNoAggregation(node.order_by, "ORDER BY");
    WalkList(node.order_by);
    ValidateNoAggregation(node.skip.get(), "SKIP");
    WalkMaybe(node.skip);
    ValidateNoAggregation(node.limit.get(), "LIMIT");
    WalkMaybe(node.limit);
    PopScope();
  }

  void Visit(With &node) override {
    CHECK(node.body != nullptr, common::InternalError, "WITH body is null");
    const Scope pre = CurrentScope();
    node.body->Accept(*this);

    const Scope projected = ScopeFromProjection(*node.body, pre);
    ReplaceCurrentScope(projected);
    ValidateNoAggregation(node.where.get(), "WHERE");
    WalkMaybe(node.where);
  }

  void Visit(ListComprehension &node) override {
    WalkMaybe(node.list_expr);

    PushScope(CurrentScope());
    Define(node.variable);
    WalkMaybe(node.where_expr);
    WalkMaybe(node.eval_expr);
    PopScope();
  }

  void Visit(PatternComprehension &node) override {
    PushScope(CurrentScope());
    Define(node.variable);
    if (node.relationships_pattern) {
      CollectFromRelationshipsPattern(*node.relationships_pattern,
                                      CurrentScope());
    }
    ASTWalker::Visit(node);
    PopScope();
  }

  void Visit(PatternPredicateExpression &node) override {
    PushScope(CurrentScope());
    if (node.relationships_pattern) {
      CollectFromRelationshipsPattern(*node.relationships_pattern,
                                      CurrentScope());
    }
    ASTWalker::Visit(node);
    PopScope();
  }

  void Visit(AllQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(AnyQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(NoneQuantifier &node) override { ValidateQuantifier(node); }
  void Visit(SingleQuantifier &node) override { ValidateQuantifier(node); }

  void Visit(ExistentialSubquery &node) override {
    if (node.query) {
      PushScope(CurrentScope());
      WalkMaybe(node.query);
      PopScope();
      return;
    }
    if (node.pattern) {
      PushScope(CurrentScope());
      CollectFromPattern(*node.pattern, CurrentScope());
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

  void Visit(FunctionInvocation &node) override {
    const bool aggregate = IsAggregateFunction(node.function_name);
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
    std::unordered_set<std::string> names;

    void Add(const std::string &name) {
      if (!name.empty()) {
        names.insert(name);
      }
    }

    [[nodiscard]] bool Contains(const std::string &name) const {
      return names.find(name) != names.end();
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

  void Define(const std::string &name) { CurrentScope().Add(name); }

  void ReplaceCurrentScope(const Scope &scope) { scope_stack_.back() = scope; }

  [[nodiscard]] bool IsDefined(const std::string &name) const {
    for (const auto &it : std::ranges::reverse_view(scope_stack_)) {
      if (it.Contains(name)) {
        return true;
      }
    }
    return false;
  }

  static Scope MergeScopes(const Scope &lhs, const Scope &rhs) {
    Scope out = lhs;
    out.names.insert(rhs.names.begin(), rhs.names.end());
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
    for (const auto &item : body.items) {
      if (!item || !item->expression) {
        continue;
      }
      Expression *expression = item->expression.get();
      if (ContainsAggregation(expression) &&
          !IsTopLevelAggregation(expression)) {
        ReportSemantic("aggregation must be a top-level projection item");
      }
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

  void CollectFromPattern(const Pattern &pattern, Scope &scope) const {
    for (const auto &part : pattern.parts) {
      if (part) {
        CollectFromPatternPart(*part, scope);
      }
    }
  }

  void CollectFromPatternPart(const PatternPart &part, Scope &scope) const {
    scope.Add(part.variable);
    if (part.element) {
      CollectFromPatternElement(*part.element, scope);
    }
  }

  void CollectFromPatternElement(const PatternElement &element,
                                 Scope &scope) const {
    if (element.node_pattern) {
      CollectFromNodePattern(*element.node_pattern, scope);
    }
    for (const auto &link : element.chain) {
      if (link.first && link.first->detail) {
        CollectFromRelationshipDetail(*link.first->detail, scope);
      }
      if (link.second) {
        CollectFromNodePattern(*link.second, scope);
      }
    }
  }

  void CollectFromRelationshipsPattern(const RelationshipsPattern &pattern,
                                       Scope &scope) const {
    if (pattern.node_pattern) {
      CollectFromNodePattern(*pattern.node_pattern, scope);
    }
    for (const auto &link : pattern.chain) {
      if (link.first && link.first->detail) {
        CollectFromRelationshipDetail(*link.first->detail, scope);
      }
      if (link.second) {
        CollectFromNodePattern(*link.second, scope);
      }
    }
  }

  static void CollectFromNodePattern(const NodePattern &node, Scope &scope) {
    scope.Add(node.variable);
  }

  static void CollectFromRelationshipDetail(const RelationshipDetail &detail,
                                            Scope &scope) {
    scope.Add(detail.variable);
  }

  static void CollectFromProjectionItem(const ProjectionItem &item,
                                        Scope &scope) {
    if (!item.alias.empty()) {
      scope.Add(item.alias);
      return;
    }
    if (item.expression && item.expression->Is(ASTNodeType::kVariable)) {
      const auto *var = CastAst<Variable>(item.expression.get());
      scope.Add(var->name);
    }
  }

  [[nodiscard]] Scope ScopeFromProjection(const ProjectionBody &body,
                                          const Scope &fallback) const {
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
    PushScope(CurrentScope());
    Define(node.variable);
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
