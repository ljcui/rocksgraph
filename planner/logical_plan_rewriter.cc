#include "planner/logical_plan_rewriter.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_node.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"
#include "ir/query_ir_internal.h"
#include "planner/order_property.h"

namespace ir {
namespace {

constexpr std::size_t kMaxRuleIterations = 64;

bool ApplyRuleBottomUp(LogicalPlanPtr *plan,
                       const LogicalPlanRewriteRule &rule) {
  CHECK(plan != nullptr && *plan != nullptr, common::InternalError,
        "logical plan rewrite input is null");
  bool changed = false;
  for (std::size_t i = 0; i < (*plan)->ChildCount(); ++i) {
    LogicalPlanPtr child = (*plan)->TakeChild(i);
    changed |= ApplyRuleBottomUp(&child, rule);
    (*plan)->ReplaceChild(i, std::move(child));
  }
  const std::vector<std::string> output_columns = (*plan)->OutputColumns();
  const std::unordered_set<std::string> solved_symbols =
      (*plan)->SolvedSymbols();
  const bool node_changed = rule.Apply(plan);
  CHECK(*plan != nullptr, common::InternalError,
        "logical plan rewrite produced a null plan");
  CHECK((*plan)->OutputColumns() == output_columns &&
            (*plan)->SolvedSymbols() == solved_symbols,
        common::InternalError, "logical plan rewrite changed the plan schema");
  changed |= node_changed;
  return changed;
}

bool IsVariable(const ast::Expression *expression, std::string_view name) {
  expression = UnwrapParenthesized(expression);
  return expression != nullptr && expression->Is(ast::ASTNodeType::kVariable) &&
         ast::CastAst<ast::Variable>(expression)->name == name;
}

bool IsRelationshipUniqueness(const ast::Expression *expression,
                              std::string_view relationship) {
  expression = UnwrapParenthesized(expression);
  if (expression == nullptr ||
      !expression->Is(ast::ASTNodeType::kAllQuantifier)) {
    return false;
  }
  const auto &all = *ast::CastAst<ast::AllQuantifier>(expression);
  const auto *predicate = UnwrapParenthesized(all.predicate.get());
  if (!IsVariable(all.list_expr.get(), relationship) || predicate == nullptr ||
      !predicate->Is(ast::ASTNodeType::kSingleQuantifier)) {
    return false;
  }
  const auto &single = *ast::CastAst<ast::SingleQuantifier>(predicate);
  predicate = UnwrapParenthesized(single.predicate.get());
  if (all.variable == relationship || single.variable == relationship ||
      !IsVariable(single.list_expr.get(), relationship) ||
      predicate == nullptr ||
      !predicate->Is(ast::ASTNodeType::kComparisonExpression) ||
      all.variable == single.variable) {
    return false;
  }
  const auto &comparison = *ast::CastAst<ast::ComparisonExpression>(predicate);
  return comparison.op == "=" &&
         ((IsVariable(comparison.left.get(), all.variable) &&
           IsVariable(comparison.right.get(), single.variable)) ||
          (IsVariable(comparison.right.get(), all.variable) &&
           IsVariable(comparison.left.get(), single.variable)));
}

LogicalPlanPtr PruneDistinctExpand(
    LogicalPlanPtr plan, const std::vector<LogicalProjectionItem> &items,
    bool *changed) {
  CHECK(plan != nullptr, common::InternalError, "distinct input plan is null");
  CHECK(changed != nullptr, common::InternalError,
        "logical plan rewrite change flag is null");
  const LogicalPlan *candidate = plan.get();
  std::vector<const FilterPlan *> filters;
  while (candidate->Type() == LogicalPlanNodeType::kFilter) {
    filters.push_back(&static_cast<const FilterPlan &>(*candidate));
    candidate = &candidate->Child(0);
  }
  if (candidate->Type() != LogicalPlanNodeType::kVarExpand) {
    return plan;
  }
  const auto &expand = static_cast<const VarExpandPlan &>(*candidate);
  // Directed reachability with a lower bound of zero or one preserves
  // endpoint sets. Higher lower bounds and undirected trails need more state.
  if (expand.Direction() == ExpandDirection::kBoth ||
      expand.Length().min.value_or(1) > 1 ||
      expand.Child(0).SolvedSymbols().contains(expand.Relationship())) {
    return plan;
  }
  for (const auto &item : items) {
    if (item.passthrough
            ? item.alias == expand.Relationship()
            : item.expression == nullptr ||
                  !item.precomputed_expressions.empty() ||
                  !ExpressionIsDeterministic(*item.expression) ||
                  ast::CollectExpressionDependencies(*item.expression)
                      .contains(expand.Relationship())) {
      return plan;
    }
  }

  std::vector<const ast::Expression *> retained;
  for (const auto *filter : filters) {
    if (filter->Predicate() == nullptr ||
        !filter->PrecomputedExpressions().empty()) {
      return plan;
    }
    if (IsRelationshipUniqueness(filter->Predicate(), expand.Relationship())) {
      continue;
    }
    if (!ExpressionIsDeterministic(*filter->Predicate()) ||
        ast::CollectExpressionDependencies(*filter->Predicate())
            .contains(expand.Relationship())) {
      return plan;
    }
    retained.push_back(filter->Predicate());
  }

  PatternRelationship pattern{
      .variable = expand.Relationship(),
      .left_node = expand.FromNode(),
      .right_node = expand.ToNode(),
      .direction = expand.Direction() == ExpandDirection::kOutgoing
                       ? Direction::kOutgoing
                       : Direction::kIncoming,
      .types = expand.Types(),
      .length = {.variable = true,
                 .min = expand.Length().min,
                 .max = expand.Length().max}};
  for (std::size_t i = 0; i < filters.size(); ++i) {
    plan = plan->TakeChild(0);
  }
  plan = std::make_unique<PruningVarExpandPlan>(plan->TakeChild(0),
                                                std::move(pattern));
  for (auto it = retained.rbegin(); it != retained.rend(); ++it) {
    plan = std::make_unique<FilterPlan>(std::move(plan), *it);
  }
  *changed = true;
  return plan;
}

class PruneDistinctVarExpandRule final : public LogicalPlanRewriteRule {
 public:
  [[nodiscard]] bool Apply(LogicalPlanPtr *plan) const override {
    CHECK(plan != nullptr && *plan != nullptr, common::InternalError,
          "logical plan rewrite input is null");
    if ((*plan)->Type() != LogicalPlanNodeType::kDistinct) {
      return false;
    }
    auto &distinct = static_cast<DistinctPlan &>(**plan);
    bool changed = false;
    LogicalPlanPtr child = PruneDistinctExpand(
        distinct.TakeChild(0), distinct.GroupingItems(), &changed);
    distinct.ReplaceChild(0, std::move(child));
    return changed;
  }
};

}  // namespace

LogicalPlanRewritePipeline::LogicalPlanRewritePipeline(
    std::vector<std::unique_ptr<LogicalPlanRewriteRule>> rules)
    : rules_(std::move(rules)) {
  for (const auto &rule : rules_) {
    CHECK(rule != nullptr, common::InvalidArgumentError,
          "logical plan rewrite rule is null");
  }
}

void LogicalPlanRewritePipeline::Add(
    std::unique_ptr<LogicalPlanRewriteRule> rule) {
  CHECK(rule != nullptr, common::InvalidArgumentError,
        "logical plan rewrite rule is null");
  rules_.push_back(std::move(rule));
}

LogicalPlanPtr LogicalPlanRewritePipeline::Run(LogicalPlanPtr plan) const {
  CHECK(plan != nullptr, common::InvalidArgumentError,
        "logical plan rewrite input is null");
  for (const auto &rule : rules_) {
    std::size_t iteration = 0;
    while (ApplyRuleBottomUp(&plan, *rule)) {
      ++iteration;
      CHECK(iteration < kMaxRuleIterations, common::InternalError,
            "logical plan rewrite rule did not converge");
    }
  }
  return plan;
}

LogicalPlanRewritePipeline MakeDefaultLogicalPlanRewritePipeline() {
  LogicalPlanRewritePipeline pipeline;
  pipeline.Add(std::make_unique<PruneDistinctVarExpandRule>());
  return pipeline;
}

LogicalPlanPtr RewriteLogicalPlan(LogicalPlanPtr plan) {
  return MakeDefaultLogicalPlanRewritePipeline().Run(std::move(plan));
}

}  // namespace ir
