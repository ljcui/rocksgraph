#include "aggregation_expression_rewriter.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../ast_clone.h"
#include "../ast_const_walker.h"
#include "../ast_equal.h"
#include "../ast_walker.h"
#include "../builtin_function.h"
#include "../expression_dependency.h"

namespace ast {
namespace {

bool IsAggregateFunction(std::string_view function_name) {
  const BuiltinFunction *function = FindBuiltinFunction(function_name);
  return function != nullptr && function->aggregate;
}

const Expression *UnwrapParenthesized(const Expression *expression) {
  const Expression *unwrapped = expression;
  while (unwrapped != nullptr &&
         unwrapped->Is(ASTNodeType::kParenthesizedExpression)) {
    unwrapped = CastAst<ParenthesizedExpression>(*unwrapped).expr.get();
  }
  return unwrapped;
}

class AggregationScanner final : public ASTConstWalker {
 public:
  bool Scan(const Expression &expression) {
    contains_ = false;
    expression.Accept(*this);
    return contains_;
  }

 protected:
  void Visit(const ExistentialSubquery &node) override { (void)node; }

  void Visit(const FunctionInvocation &node) override {
    if (IsAggregateFunction(node.function_name)) {
      contains_ = true;
      return;
    }
    ASTConstWalker::Visit(node);
  }

  void Visit(const CountStarExpression &node) override {
    (void)node;
    contains_ = true;
  }

 private:
  bool contains_ = false;
};

class AggregateExpressionCollector final : public ASTConstWalker {
 public:
  std::vector<const Expression *> Collect(const Expression &expression) {
    expressions_.clear();
    expression.Accept(*this);
    return expressions_;
  }

 protected:
  void Visit(const ExistentialSubquery &node) override { (void)node; }

  void Visit(const FunctionInvocation &node) override {
    if (IsAggregateFunction(node.function_name)) {
      expressions_.push_back(&node);
      return;
    }
    ASTConstWalker::Visit(node);
  }

  void Visit(const CountStarExpression &node) override {
    expressions_.push_back(&node);
  }

 private:
  std::vector<const Expression *> expressions_;
};

class NonDeterministicFunctionScanner final : public ASTConstWalker {
 public:
  bool Scan(const Expression &expression) {
    contains_ = false;
    expression.Accept(*this);
    return contains_;
  }

 protected:
  void Visit(const FunctionInvocation &node) override {
    const BuiltinFunction *function = FindBuiltinFunction(node.function_name);
    if (function != nullptr && !function->deterministic) {
      contains_ = true;
      return;
    }
    ASTConstWalker::Visit(node);
  }

 private:
  bool contains_ = false;
};

class NameCollector final : public ASTWalker {
 public:
  std::unordered_set<std::string> Collect(ASTNode &node) {
    names_.clear();
    node.Accept(*this);
    return std::move(names_);
  }

 protected:
  void Visit(Variable &node) override { Add(node.name); }

  void Visit(ProjectionItem &node) override {
    Add(node.alias);
    ASTWalker::Visit(node);
  }

  void Visit(NodePattern &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(RelationshipDetail &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(PatternPart &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(StandaloneCall &node) override {
    for (const auto &item : node.yield_items) {
      Add(item.variable);
    }
    ASTWalker::Visit(node);
  }

  void Visit(InQueryCall &node) override {
    for (const auto &item : node.yield_items) {
      Add(item.variable);
    }
    ASTWalker::Visit(node);
  }

  void Visit(Unwind &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(ListComprehension &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(PatternComprehension &node) override {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Visit(AllQuantifier &node) override { VisitQuantifier(node); }
  void Visit(AnyQuantifier &node) override { VisitQuantifier(node); }
  void Visit(NoneQuantifier &node) override { VisitQuantifier(node); }
  void Visit(SingleQuantifier &node) override { VisitQuantifier(node); }

 private:
  void VisitQuantifier(Quantifier &node) {
    Add(node.variable);
    ASTWalker::Visit(node);
  }

  void Add(const std::string &name) {
    if (!name.empty()) {
      names_.insert(name);
    }
  }

  std::unordered_set<std::string> names_;
};

}  // namespace

class AggregationExpressionRewriter::ExpressionAliasRewriter final
    : public ASTRewriter {
 public:
  explicit ExpressionAliasRewriter(
      const std::vector<AggregationExpressionRewriter::NamedExpression>
          &expressions)
      : expressions_(expressions) {}

  void Rewrite(std::unique_ptr<Expression> &expression) {
    RewriteExpression(expression);
  }

 protected:
  void RewriteExpression(std::unique_ptr<Expression> &expression) override {
    if (!expression) {
      return;
    }
    if (!ReferencesLocal(*expression)) {
      for (const auto &entry : expressions_) {
        if (entry.expression != nullptr &&
            ASTEqual::Equal(expression.get(), entry.expression.get())) {
          expression = MakeVariable(entry.alias);
          return;
        }
      }
    }
    ASTRewriter::RewriteExpression(expression);
  }

  void Visit(ExistentialSubquery &node) override { (void)node; }

  void Visit(ListComprehension &node) override {
    RewriteMaybe(node.list_expr);
    PushLocal(node.variable);
    RewriteMaybe(node.where_expr);
    RewriteMaybe(node.eval_expr);
    PopLocal();
  }

  void Visit(AllQuantifier &node) override { VisitQuantifier(node); }
  void Visit(AnyQuantifier &node) override { VisitQuantifier(node); }
  void Visit(NoneQuantifier &node) override { VisitQuantifier(node); }
  void Visit(SingleQuantifier &node) override { VisitQuantifier(node); }

 private:
  static std::unique_ptr<Expression> MakeVariable(const std::string &name) {
    auto variable = std::make_unique<Variable>();
    variable->name = name;
    return variable;
  }

  [[nodiscard]] bool ReferencesLocal(const Expression &expression) const {
    const auto dependencies = CollectExpressionDependencies(expression);
    for (const auto &dependency : dependencies) {
      for (auto scope = local_scopes_.rbegin(); scope != local_scopes_.rend();
           ++scope) {
        if (scope->contains(dependency)) {
          return true;
        }
      }
    }
    return false;
  }

  void VisitQuantifier(Quantifier &node) {
    RewriteMaybe(node.list_expr);
    PushLocal(node.variable);
    RewriteMaybe(node.predicate);
    PopLocal();
  }

  void PushLocal(const std::string &name) {
    local_scopes_.emplace_back();
    if (!name.empty()) {
      local_scopes_.back().insert(name);
    }
  }

  void PopLocal() { local_scopes_.pop_back(); }

  const std::vector<AggregationExpressionRewriter::NamedExpression>
      &expressions_;
  std::vector<std::unordered_set<std::string>> local_scopes_;
};

void AggregationExpressionRewriter::Visit(RegularQuery &node) {
  Prepare(node);
  RewriteSingleQuery(node.single_query);
  RewriteList(node.unions);
}

void AggregationExpressionRewriter::Visit(UnionPart &node) {
  RewriteSingleQuery(node.query);
}

void AggregationExpressionRewriter::Prepare(ASTNode &node) {
  if (prepared_) {
    return;
  }
  NameCollector collector;
  used_names_ = collector.Collect(node);
  prepared_ = true;
}

void AggregationExpressionRewriter::RewriteSingleQuery(
    std::unique_ptr<SingleQuery> &query) {
  if (!query) {
    return;
  }
  switch (query->node_type) {
    case ASTNodeType::kSinglePartQuery:
      query = RewriteSinglePart(std::unique_ptr<SinglePartQuery>(
          CastAst<SinglePartQuery>(query.release())));
      return;
    case ASTNodeType::kMultiPartQuery:
      RewriteMultiPart(CastAst<MultiPartQuery>(*query));
      return;
    default:
      return;
  }
}

std::unique_ptr<SingleQuery> AggregationExpressionRewriter::RewriteSinglePart(
    std::unique_ptr<SinglePartQuery> query) {
  RewriteList(query->reading_clauses);
  RewriteList(query->updating_clauses);
  RewriteMaybe(query->return_clause);
  if (!query->return_clause || !query->return_clause->body) {
    return query;
  }

  std::unique_ptr<With> prefix;
  if (!RewriteProjection(*query->return_clause->body, prefix)) {
    return query;
  }

  auto multi = std::make_unique<MultiPartQuery>();
  MultiPartQuery::WithPart part;
  part.reading_clauses = std::move(query->reading_clauses);
  part.updating_clauses = std::move(query->updating_clauses);
  part.with_clause = std::move(prefix);
  multi->parts.push_back(std::move(part));

  auto final = std::make_unique<SinglePartQuery>();
  final->return_clause = std::move(query->return_clause);
  multi->final_single_part_query = std::move(final);
  return multi;
}

void AggregationExpressionRewriter::RewriteMultiPart(MultiPartQuery &node) {
  std::vector<MultiPartQuery::WithPart> rewritten_parts;
  rewritten_parts.reserve(node.parts.size());
  for (auto &part : node.parts) {
    RewriteList(part.reading_clauses);
    RewriteList(part.updating_clauses);
    RewriteMaybe(part.with_clause);

    std::unique_ptr<With> prefix;
    const bool split = part.with_clause && part.with_clause->body &&
                       RewriteProjection(*part.with_clause->body, prefix);
    if (!split) {
      rewritten_parts.push_back(std::move(part));
      continue;
    }

    MultiPartQuery::WithPart aggregation_part;
    aggregation_part.reading_clauses = std::move(part.reading_clauses);
    aggregation_part.updating_clauses = std::move(part.updating_clauses);
    aggregation_part.with_clause = std::move(prefix);
    rewritten_parts.push_back(std::move(aggregation_part));

    MultiPartQuery::WithPart projection_part;
    projection_part.with_clause = std::move(part.with_clause);
    rewritten_parts.push_back(std::move(projection_part));
  }
  node.parts = std::move(rewritten_parts);

  if (!node.final_single_part_query) {
    return;
  }
  std::unique_ptr<SingleQuery> rewritten_final =
      RewriteSinglePart(std::move(node.final_single_part_query));
  if (rewritten_final->Is(ASTNodeType::kSinglePartQuery)) {
    node.final_single_part_query = std::unique_ptr<SinglePartQuery>(
        CastAst<SinglePartQuery>(rewritten_final.release()));
    return;
  }

  auto tail = std::unique_ptr<MultiPartQuery>(
      CastAst<MultiPartQuery>(rewritten_final.release()));
  for (auto &part : tail->parts) {
    node.parts.push_back(std::move(part));
  }
  node.final_single_part_query = std::move(tail->final_single_part_query);
}

bool AggregationExpressionRewriter::RewriteProjection(
    ProjectionBody &body, std::unique_ptr<With> &prefix) {
  if (body.star) {
    return false;
  }

  bool needs_split = false;
  for (const auto &item : body.items) {
    if (item && item->expression &&
        ContainsAggregation(item->expression.get()) &&
        !IsTopLevelAggregation(item->expression.get())) {
      needs_split = true;
      break;
    }
  }
  if (!needs_split) {
    for (const auto &item : body.order_by) {
      if (item && ContainsAggregation(item->expression.get())) {
        needs_split = true;
        break;
      }
    }
  }
  if (!needs_split) {
    return false;
  }

  std::vector<NamedExpression> expressions;
  for (const auto &item : body.items) {
    if (!item || !item->expression ||
        ContainsAggregation(item->expression.get()) ||
        !IsNotConstantExpression(item->expression.get())) {
      continue;
    }
    AddExpression(*item->expression, item->alias, expressions);
  }
  for (const auto &item : body.items) {
    if (!item || !IsTopLevelAggregation(item->expression.get())) {
      continue;
    }
    AddExpression(*item->expression, item->alias, expressions);
  }
  for (const auto &item : body.items) {
    if (!item || !item->expression ||
        !ContainsAggregation(item->expression.get()) ||
        IsTopLevelAggregation(item->expression.get())) {
      continue;
    }
    AddAggregateExpressions(*item->expression, expressions);
  }
  for (const auto &item : body.order_by) {
    if (item && item->expression) {
      AddAggregateExpressions(*item->expression, expressions);
    }
  }
  if (expressions.empty()) {
    return false;
  }

  prefix = BuildPrefix(expressions);
  ExpressionAliasRewriter rewriter(expressions);
  for (auto &item : body.items) {
    if (item && item->expression) {
      rewriter.Rewrite(item->expression);
    }
  }
  for (auto &item : body.order_by) {
    if (item && item->expression) {
      rewriter.Rewrite(item->expression);
    }
  }
  // Aggregation already produces at most one row for each projected grouping
  // key, so DISTINCT is redundant. Removing it also keeps hidden ORDER BY
  // aggregate aliases accessible from the post-aggregation projection.
  body.distinct = false;
  return true;
}

void AggregationExpressionRewriter::AddExpression(
    const Expression &expression, const std::string &preferred_alias,
    std::vector<NamedExpression> &out) {
  for (const auto &entry : out) {
    if (entry.expression &&
        ASTEqual::Equal(&expression, entry.expression.get())) {
      return;
    }
  }
  out.push_back(
      NamedExpression{CloneExpression(expression),
                      preferred_alias.empty() ? NextAlias() : preferred_alias});
}

void AggregationExpressionRewriter::AddAggregateExpressions(
    const Expression &expression, std::vector<NamedExpression> &out) {
  AggregateExpressionCollector collector;
  for (const Expression *aggregate : collector.Collect(expression)) {
    AddExpression(*aggregate, "", out);
  }
}

std::unique_ptr<With> AggregationExpressionRewriter::BuildPrefix(
    const std::vector<NamedExpression> &expressions) const {
  auto with = std::make_unique<With>();
  with->body = std::make_unique<ProjectionBody>();
  with->body->items.reserve(expressions.size());
  for (const auto &entry : expressions) {
    auto item = std::make_unique<ProjectionItem>();
    item->expression = CloneExpression(*entry.expression);
    item->alias = entry.alias;
    with->body->items.push_back(std::move(item));
  }
  return with;
}

std::string AggregationExpressionRewriter::NextAlias() {
  for (;;) {
    std::string alias = "__agg_" + std::to_string(next_alias_id_++);
    if (used_names_.insert(alias).second) {
      return alias;
    }
  }
}

bool AggregationExpressionRewriter::ContainsAggregation(
    const Expression *expression) const {
  return expression != nullptr && AggregationScanner{}.Scan(*expression);
}

bool AggregationExpressionRewriter::IsTopLevelAggregation(
    const Expression *expression) const {
  const Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr) {
    return false;
  }
  if (unwrapped->Is(ASTNodeType::kCountStarExpression)) {
    return true;
  }
  return unwrapped->Is(ASTNodeType::kFunctionInvocation) &&
         IsAggregateFunction(
             CastAst<FunctionInvocation>(*unwrapped).function_name);
}

bool AggregationExpressionRewriter::IsNotConstantExpression(
    const Expression *expression) const {
  return expression != nullptr &&
         (!CollectExpressionDependencies(*expression).empty() ||
          NonDeterministicFunctionScanner{}.Scan(*expression));
}

}  // namespace ast
