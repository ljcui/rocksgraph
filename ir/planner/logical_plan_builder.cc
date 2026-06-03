#include "ir/planner/logical_plan_builder.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast_const_walker.h"
#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "ast/expression_dependency.h"
#include "common/exception.h"
#include "ir/planner/component_planner.h"
#include "ir/planner/cost_model.h"
#include "ir/planner_query_internal.h"

namespace ir {
namespace {

constexpr std::string_view kLogicalPlanStage = "logical plan";

std::vector<std::string> Sorted(
    const std::unordered_set<std::string> &symbols) {
  std::vector<std::string> out(symbols.begin(), symbols.end());
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> ProjectionAliases(
    const std::vector<ProjectionItem> &items) {
  std::vector<std::string> aliases;
  aliases.reserve(items.size());
  for (const auto &item : items) {
    aliases.push_back(item.alias);
  }
  return aliases;
}

std::vector<std::string> ProjectionAliases(
    const std::vector<ProjectionItem> &lhs,
    const std::vector<ProjectionItem> &rhs) {
  std::vector<std::string> aliases = ProjectionAliases(lhs);
  aliases.reserve(lhs.size() + rhs.size());
  for (const auto &item : rhs) {
    aliases.push_back(item.alias);
  }
  return aliases;
}

std::vector<LogicalPrecomputedExpression> PrecomputedExpressions(
    const std::vector<NestedIRExpression> &nested_expressions) {
  std::vector<LogicalPrecomputedExpression> out;
  out.reserve(nested_expressions.size());
  for (const auto &nested : nested_expressions) {
    if (nested.expression == nullptr) {
      continue;
    }
    switch (nested.kind) {
      case NestedIRExpressionKind::kExists:
        if (!nested.value_variable.empty()) {
          out.push_back({.expression = nested.expression,
                         .variable = nested.value_variable});
        }
        break;
      case NestedIRExpressionKind::kList:
        if (!nested.collection_variable.empty()) {
          out.push_back({.expression = nested.expression,
                         .variable = nested.collection_variable});
        }
        break;
    }
  }
  return out;
}

std::vector<LogicalProjectionItem> LogicalProjectionItems(
    const std::vector<ProjectionItem> &items,
    const std::vector<NestedIRExpression> &nested_expressions = {}) {
  std::vector<LogicalPrecomputedExpression> precomputed_expressions =
      PrecomputedExpressions(nested_expressions);
  std::vector<LogicalProjectionItem> logical_items;
  logical_items.reserve(items.size());
  for (const auto &item : items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "projection expression is null");
    CHECK(!item.alias.empty(), common::InvalidArgumentError,
          "projection alias is empty");
    logical_items.push_back({
        .expression = item.expression,
        .alias = item.alias,
        .precomputed_expressions = precomputed_expressions,
    });
  }
  return logical_items;
}

std::vector<LogicalProjectionItem> PassthroughProjectionItems(
    const std::vector<std::string> &aliases) {
  std::vector<LogicalProjectionItem> logical_items;
  logical_items.reserve(aliases.size());
  for (const auto &alias : aliases) {
    logical_items.push_back({
        .alias = alias,
        .passthrough = true,
    });
  }
  return logical_items;
}

void AppendPassthroughProjectionItem(
    std::string alias, std::vector<LogicalProjectionItem> *items) {
  CHECK(items != nullptr, common::InternalError,
        "logical projection item list is null");
  if (alias.empty()) {
    return;
  }
  for (const auto &item : *items) {
    if (item.alias == alias) {
      return;
    }
  }
  items->push_back({
      .alias = std::move(alias),
      .passthrough = true,
  });
}

bool AddUniqueString(std::vector<std::string> *values,
                     const std::string &value) {
  CHECK(values != nullptr, common::InternalError, "string list is null");
  if (StringVectorContains(*values, value)) {
    return false;
  }
  values->push_back(value);
  return true;
}

std::vector<std::string> ProjectionTailPassthroughVariables(
    const LogicalPlan &input, const QueryProjection &projection,
    const std::vector<std::string> &aliases) {
  std::vector<std::string> variables;
  const auto add_variable = [&](const std::string &variable) {
    if (variable.empty() || StringVectorContains(aliases, variable) ||
        !StringVectorContains(input.OutputColumns(), variable)) {
      return;
    }
    (void)AddUniqueString(&variables, variable);
  };
  for (const auto &item : projection.required_order.items) {
    if (item.expression == nullptr) {
      continue;
    }
    for (const auto &dependency :
         ast::CollectExpressionDependencies(*item.expression)) {
      add_variable(dependency);
    }
  }
  for (const ast::Expression *expression :
       {projection.pagination.skip, projection.pagination.limit}) {
    if (expression == nullptr) {
      continue;
    }
    for (const auto &dependency :
         ast::CollectExpressionDependencies(*expression)) {
      add_variable(dependency);
    }
  }
  for (const auto &precomputed :
       PrecomputedExpressions(projection.nested_expressions)) {
    add_variable(precomputed.variable);
  }
  return variables;
}

void AppendPassthroughProjectionItems(
    const LogicalPlan &input, const QueryProjection &projection,
    const std::vector<std::string> &aliases,
    std::vector<LogicalProjectionItem> *items) {
  CHECK(items != nullptr, common::InternalError,
        "logical projection item list is null");
  for (const auto &variable :
       ProjectionTailPassthroughVariables(input, projection, aliases)) {
    AppendPassthroughProjectionItem(variable, items);
  }
}

bool ContainsExpression(const ast::Expression &haystack,
                        const ast::Expression &needle) {
  class Finder final : public ast::ASTConstWalker {
   public:
    explicit Finder(const ast::Expression &needle) : needle_(needle) {}

    [[nodiscard]] bool found() const noexcept { return found_; }

   protected:
    void Visit(const ast::ExistentialSubquery &node) override {
      found_ = found_ || ast::ASTEqual::Equal(&node, &needle_);
      ast::ASTConstWalker::Visit(node);
    }

    void Visit(const ast::PatternComprehension &node) override {
      found_ = found_ || ast::ASTEqual::Equal(&node, &needle_);
      ast::ASTConstWalker::Visit(node);
    }

   private:
    const ast::Expression &needle_;
    bool found_ = false;
  };

  Finder finder(needle);
  haystack.Accept(finder);
  return finder.found();
}

void ValidateProjectionTailExpressionsAvailable(
    const LogicalPlan &input, const QueryProjection &projection) {
  const std::unordered_set<std::string> available(input.OutputColumns().begin(),
                                                  input.OutputColumns().end());
  const std::vector<LogicalPrecomputedExpression> precomputed_expressions =
      PrecomputedExpressions(projection.nested_expressions);
  const auto validate_dependencies = [&](const ast::Expression *expression,
                                         std::string_view context) {
    if (expression == nullptr) {
      return;
    }
    if (!DependenciesMet(ast::CollectExpressionDependencies(*expression),
                         available)) {
      THROW(
          common::InvalidArgumentError,
          UnsupportedInStage(kLogicalPlanStage,
                             std::string(context) +
                                 " with unmet dependencies after projection"));
    }
    for (const auto &precomputed : precomputed_expressions) {
      if (precomputed.expression == nullptr ||
          !ContainsExpression(*expression, *precomputed.expression)) {
        continue;
      }
      if (precomputed.variable.empty() ||
          !StringVectorContains(input.OutputColumns(), precomputed.variable)) {
        THROW(common::InvalidArgumentError,
              UnsupportedInStage(kLogicalPlanStage,
                                 std::string(context) +
                                     " nested expression after projection"));
      }
    }
  };
  for (const auto &item : projection.required_order.items) {
    validate_dependencies(item.expression, "ORDER BY expression");
  }
  validate_dependencies(projection.pagination.skip, "SKIP expression");
  validate_dependencies(projection.pagination.limit, "LIMIT expression");
}

LogicalOrderDirection ToLogicalOrderDirection(OrderDirection direction) {
  switch (direction) {
    case OrderDirection::kAscending:
      return LogicalOrderDirection::kAscending;
    case OrderDirection::kDescending:
      return LogicalOrderDirection::kDescending;
  }
  THROW(common::InternalError, "unknown order direction");
}

std::vector<LogicalSortItem> SortItems(const RequiredOrder &required_order) {
  std::vector<LogicalSortItem> items;
  items.reserve(required_order.items.size());
  for (const auto &item : required_order.items) {
    CHECK(item.expression != nullptr, common::InvalidArgumentError,
          "sort expression is null");
    items.push_back({.expression = item.expression,
                     .direction = ToLogicalOrderDirection(item.direction)});
  }
  return items;
}

std::vector<LogicalSortItem> SortItems(
    const RequiredOrder &required_order,
    const std::vector<NestedIRExpression> &nested_expressions) {
  std::vector<LogicalSortItem> items = SortItems(required_order);
  std::vector<LogicalPrecomputedExpression> precomputed_expressions =
      PrecomputedExpressions(nested_expressions);
  for (auto &item : items) {
    item.precomputed_expressions = precomputed_expressions;
  }
  return items;
}

std::vector<LogicalUnionMapping> LogicalUnionMappings(
    const std::vector<UnionPlannerQuery::UnionMapping> &mappings) {
  std::vector<LogicalUnionMapping> logical_mappings;
  logical_mappings.reserve(mappings.size());
  for (const auto &mapping : mappings) {
    CHECK(!mapping.output_variable.empty(), common::InvalidArgumentError,
          "UNION output variable is empty");
    logical_mappings.push_back({.output_variable = mapping.output_variable,
                                .lhs_variable = mapping.lhs_variable,
                                .rhs_variable = mapping.rhs_variable});
  }
  return logical_mappings;
}

bool DependenciesWithin(const std::unordered_set<std::string> &dependencies,
                        const std::unordered_set<std::string> &symbols) {
  return DependenciesMet(dependencies, symbols);
}

std::unordered_set<std::string> UnionSymbols(
    const std::unordered_set<std::string> &lhs,
    const std::unordered_set<std::string> &rhs) {
  std::unordered_set<std::string> out = lhs;
  AddSymbols(&out, rhs);
  return out;
}

const ast::ComparisonExpression *AsComparisonExpression(
    const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr ||
      !unwrapped->Is(ast::ASTNodeType::kComparisonExpression)) {
    return nullptr;
  }
  return ast::CastAst<ast::ComparisonExpression>(unwrapped);
}

bool IsCrossComponentPredicate(const Predicate &predicate,
                               const std::unordered_set<std::string> &left,
                               const std::unordered_set<std::string> &right) {
  if (predicate.expression == nullptr || predicate.dependencies.empty()) {
    return false;
  }
  const std::unordered_set<std::string> combined = UnionSymbols(left, right);
  return DependenciesWithin(predicate.dependencies, combined) &&
         !DependenciesWithin(predicate.dependencies, left) &&
         !DependenciesWithin(predicate.dependencies, right);
}

bool IsValueHashJoinPredicate(const Predicate &predicate,
                              const std::unordered_set<std::string> &left,
                              const std::unordered_set<std::string> &right) {
  if (!IsCrossComponentPredicate(predicate, left, right)) {
    return false;
  }
  const ast::ComparisonExpression *comparison =
      AsComparisonExpression(predicate.expression);
  if (comparison == nullptr || comparison->op != "=" ||
      comparison->left == nullptr || comparison->right == nullptr) {
    return false;
  }
  const std::unordered_set<std::string> lhs_dependencies =
      ast::CollectExpressionDependencies(*comparison->left);
  const std::unordered_set<std::string> rhs_dependencies =
      ast::CollectExpressionDependencies(*comparison->right);
  return (DependenciesWithin(lhs_dependencies, left) &&
          DependenciesWithin(rhs_dependencies, right)) ||
         (DependenciesWithin(lhs_dependencies, right) &&
          DependenciesWithin(rhs_dependencies, left));
}

bool CanUsePredicateJoin(const Predicate &predicate) {
  return predicate.kind != PredicateKind::kExistsSubquery &&
         predicate.kind != PredicateKind::kNotExistsSubquery;
}

std::vector<const ast::Expression *> JoinPredicateExpressions(
    const std::vector<const Predicate *> &predicates) {
  std::vector<const ast::Expression *> expressions;
  expressions.reserve(predicates.size());
  for (const Predicate *predicate : predicates) {
    CHECK(predicate != nullptr && predicate->expression != nullptr,
          common::InvalidArgumentError, "join predicate expression is null");
    expressions.push_back(predicate->expression);
  }
  return expressions;
}

std::optional<double> NonNegativeIntegerLiteral(
    const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr ||
      !unwrapped->Is(ast::ASTNodeType::kIntegerLiteral)) {
    return std::nullopt;
  }
  const auto *literal = ast::CastAst<ast::IntegerLiteral>(unwrapped);
  return std::max<double>(0.0, static_cast<double>(literal->value));
}

std::optional<double> LiteralListSize(const ast::Expression *expression) {
  const ast::Expression *unwrapped = UnwrapParenthesized(expression);
  if (unwrapped == nullptr || !unwrapped->Is(ast::ASTNodeType::kListLiteral)) {
    return std::nullopt;
  }
  const auto *literal = ast::CastAst<ast::ListLiteral>(unwrapped);
  return static_cast<double>(literal->elements.size());
}

CostEstimate EstimateLogicalPlanLeaf(const LogicalPlan &plan,
                                     const CostModel &cost_model) {
  switch (plan.Type()) {
    case LogicalPlanNodeType::kArgument:
      return cost_model.EstimateArgument(plan.OutputColumns().size());
    case LogicalPlanNodeType::kAllNodeScan:
      return cost_model.EstimateNodeScan({});
    case LogicalPlanNodeType::kNodeByLabelScan: {
      const auto &scan = static_cast<const NodeByLabelScanPlan &>(plan);
      return cost_model.EstimateNodeScan(std::unordered_set<std::string>(
          scan.Labels().begin(), scan.Labels().end()));
    }
    case LogicalPlanNodeType::kNodeIndexSeek: {
      const auto &seek = static_cast<const NodeIndexSeekPlan &>(plan);
      return cost_model.EstimateNodeIndexSeek(
          std::unordered_set<std::string>(seek.Labels().begin(),
                                          seek.Labels().end()),
          seek.PropertyKey(), seek.Unique());
    }
    case LogicalPlanNodeType::kNodeIndexRangeSeek: {
      const auto &seek = static_cast<const NodeIndexRangeSeekPlan &>(plan);
      return cost_model.EstimateNodeIndexRangeSeek(
          std::unordered_set<std::string>(seek.Labels().begin(),
                                          seek.Labels().end()),
          seek.PropertyKey(), seek.Predicates().size());
    }
    case LogicalPlanNodeType::kRelationshipTypeScan: {
      const auto &scan = static_cast<const RelationshipTypeScanPlan &>(plan);
      return cost_model.EstimateRelationshipTypeScan(scan.Types());
    }
    case LogicalPlanNodeType::kRelationshipIndexSeek: {
      const auto &seek = static_cast<const RelationshipIndexSeekPlan &>(plan);
      return cost_model.EstimateRelationshipIndexSeek(
          seek.Types(), seek.PropertyKey(), seek.Unique());
    }
    case LogicalPlanNodeType::kRelationshipIndexRangeSeek: {
      const auto &seek =
          static_cast<const RelationshipIndexRangeSeekPlan &>(plan);
      return cost_model.EstimateRelationshipIndexRangeSeek(
          seek.Types(), seek.PropertyKey(), seek.Predicates().size());
    }
    default:
      THROW(common::InternalError, "unsupported logical plan leaf estimate: " +
                                       std::string(plan.Name()));
  }
}

const CostEstimate &OnlyChildEstimate(
    const std::vector<CostEstimate> &child_estimates,
    std::string_view node_name) {
  CHECK(child_estimates.size() == 1, common::InternalError,
        std::string(node_name) + " expected one child estimate");
  return child_estimates.front();
}

void InheritTraits(const LogicalPlan &source, LogicalPlan *target) {
  CHECK(target != nullptr, common::InternalError, "logical plan is null");
  target->SetOrderingTrait(source.OrderingTrait());
  target->SetDistinctTrait(source.DistinctTrait());
}

void ClearTraits(LogicalPlan *plan) {
  CHECK(plan != nullptr, common::InternalError, "logical plan is null");
  plan->ClearOrderingTrait();
  plan->SetDistinctTrait(false);
}

bool ChildSolves(const LogicalPlan &plan, std::string_view symbol) {
  return plan.Child(0).SolvedSymbols().contains(std::string(symbol));
}

CostEstimate EstimateLogicalPlanNode(
    const LogicalPlan &plan, const std::vector<CostEstimate> &child_estimates,
    const CostModel &cost_model) {
  switch (plan.Type()) {
    case LogicalPlanNodeType::kArgument:
    case LogicalPlanNodeType::kAllNodeScan:
    case LogicalPlanNodeType::kNodeByLabelScan:
    case LogicalPlanNodeType::kNodeIndexSeek:
    case LogicalPlanNodeType::kNodeIndexRangeSeek:
    case LogicalPlanNodeType::kRelationshipTypeScan:
    case LogicalPlanNodeType::kRelationshipIndexSeek:
    case LogicalPlanNodeType::kRelationshipIndexRangeSeek:
      return EstimateLogicalPlanLeaf(plan, cost_model);
    case LogicalPlanNodeType::kExpand: {
      const auto &expand = static_cast<const ExpandPlan &>(plan);
      return cost_model.EstimateExpand(
          OnlyChildEstimate(child_estimates, plan.Name()), expand.Types());
    }
    case LogicalPlanNodeType::kExpandInto: {
      const auto &expand = static_cast<const ExpandIntoPlan &>(plan);
      return cost_model.EstimateExpandInto(
          OnlyChildEstimate(child_estimates, plan.Name()), expand.Types());
    }
    case LogicalPlanNodeType::kVarExpand: {
      const auto &expand = static_cast<const VarExpandPlan &>(plan);
      const bool endpoints_bound = ChildSolves(plan, expand.FromNode()) &&
                                   ChildSolves(plan, expand.ToNode());
      return endpoints_bound
                 ? cost_model.EstimateExpandInto(
                       OnlyChildEstimate(child_estimates, plan.Name()),
                       expand.Types())
                 : cost_model.EstimateExpand(
                       OnlyChildEstimate(child_estimates, plan.Name()),
                       expand.Types());
    }
    case LogicalPlanNodeType::kPathBuild:
    case LogicalPlanNodeType::kAssertIsNode:
    case LogicalPlanNodeType::kWriteBarrier:
      return cost_model.EstimatePassThrough(
          OnlyChildEstimate(child_estimates, plan.Name()), 0.01);
    case LogicalPlanNodeType::kFilter:
      return cost_model.ApplyFilter(
          OnlyChildEstimate(child_estimates, plan.Name()));
    case LogicalPlanNodeType::kProjection: {
      const auto &projection = static_cast<const ProjectionPlan &>(plan);
      return cost_model.EstimateProjection(
          OnlyChildEstimate(child_estimates, plan.Name()),
          projection.Items().size());
    }
    case LogicalPlanNodeType::kDistinct: {
      const auto &distinct = static_cast<const DistinctPlan &>(plan);
      return cost_model.EstimateDistinct(
          OnlyChildEstimate(child_estimates, plan.Name()),
          distinct.GroupingItems().size());
    }
    case LogicalPlanNodeType::kAggregation: {
      const auto &aggregation = static_cast<const AggregationPlan &>(plan);
      return cost_model.EstimateAggregation(
          OnlyChildEstimate(child_estimates, plan.Name()),
          aggregation.GroupingItems().size(),
          aggregation.AggregationItems().size());
    }
    case LogicalPlanNodeType::kSort: {
      const auto &sort = static_cast<const SortPlan &>(plan);
      return cost_model.EstimateSort(
          OnlyChildEstimate(child_estimates, plan.Name()), sort.Items().size());
    }
    case LogicalPlanNodeType::kSkip: {
      const auto &skip = static_cast<const SkipPlan &>(plan);
      return cost_model.EstimateSkip(
          OnlyChildEstimate(child_estimates, plan.Name()),
          NonNegativeIntegerLiteral(skip.Skip()));
    }
    case LogicalPlanNodeType::kLimit: {
      const auto &limit = static_cast<const LimitPlan &>(plan);
      return cost_model.EstimateLimit(
          OnlyChildEstimate(child_estimates, plan.Name()),
          NonNegativeIntegerLiteral(limit.Limit()));
    }
    case LogicalPlanNodeType::kProduceResults:
      return cost_model.EstimateProduceResults(
          OnlyChildEstimate(child_estimates, plan.Name()),
          plan.OutputColumns().size());
    case LogicalPlanNodeType::kCartesianProduct:
      CHECK(child_estimates.size() == 2, common::InternalError,
            "CartesianProduct expected two child estimates");
      return cost_model.EstimateCartesianProduct(child_estimates[0],
                                                 child_estimates[1]);
    case LogicalPlanNodeType::kNodeHashJoin: {
      CHECK(child_estimates.size() == 2, common::InternalError,
            "NodeHashJoin expected two child estimates");
      const auto &join = static_cast<const NodeHashJoinPlan &>(plan);
      return cost_model.EstimateNodeHashJoin(
          child_estimates[0], child_estimates[1], join.JoinKeys().size());
    }
    case LogicalPlanNodeType::kValueHashJoin: {
      CHECK(child_estimates.size() == 2, common::InternalError,
            "ValueHashJoin expected two child estimates");
      const auto &join = static_cast<const ValueHashJoinPlan &>(plan);
      return cost_model.EstimateValueHashJoin(
          child_estimates[0], child_estimates[1], join.Predicates().size());
    }
    case LogicalPlanNodeType::kPredicateJoin: {
      CHECK(child_estimates.size() == 2, common::InternalError,
            "PredicateJoin expected two child estimates");
      const auto &join = static_cast<const PredicateJoinPlan &>(plan);
      return cost_model.EstimatePredicateJoin(
          child_estimates[0], child_estimates[1], join.Predicates().size());
    }
    case LogicalPlanNodeType::kApply:
      CHECK(child_estimates.size() == 2, common::InternalError,
            "Apply expected two child estimates");
      return cost_model.EstimateApply(child_estimates[0], child_estimates[1]);
    case LogicalPlanNodeType::kSemiApply:
    case LogicalPlanNodeType::kAntiSemiApply:
    case LogicalPlanNodeType::kLetSemiApply:
      CHECK(child_estimates.size() == 2, common::InternalError,
            std::string(plan.Name()) + " expected two child estimates");
      return cost_model.EstimateSemiApply(child_estimates[0],
                                          child_estimates[1]);
    case LogicalPlanNodeType::kRollUpApply:
      CHECK(child_estimates.size() == 2, common::InternalError,
            "RollUpApply expected two child estimates");
      return cost_model.EstimateRollUpApply(child_estimates[0],
                                            child_estimates[1]);
    case LogicalPlanNodeType::kOptionalApply:
      CHECK(child_estimates.size() == 2, common::InternalError,
            "OptionalApply expected two child estimates");
      return cost_model.EstimateOptionalApply(child_estimates[0],
                                              child_estimates[1]);
    case LogicalPlanNodeType::kCreateNode:
    case LogicalPlanNodeType::kCreateRelationship:
    case LogicalPlanNodeType::kSetProperty:
    case LogicalPlanNodeType::kSetProperties:
    case LogicalPlanNodeType::kSetLabels:
    case LogicalPlanNodeType::kRemoveProperty:
    case LogicalPlanNodeType::kRemoveLabels:
    case LogicalPlanNodeType::kDelete:
    case LogicalPlanNodeType::kDetachDelete:
      return cost_model.EstimateWrite(
          OnlyChildEstimate(child_estimates, plan.Name()), 1.0);
    case LogicalPlanNodeType::kMerge:
      CHECK(child_estimates.size() == 2, common::InternalError,
            "Merge expected two child estimates");
      return cost_model.EstimateWrite(
          cost_model.EstimateSemiApply(child_estimates[0], child_estimates[1]),
          1.0);
    case LogicalPlanNodeType::kUnwind: {
      const auto &unwind = static_cast<const UnwindPlan &>(plan);
      return cost_model.EstimateUnwind(
          OnlyChildEstimate(child_estimates, plan.Name()),
          LiteralListSize(unwind.Expression()));
    }
    case LogicalPlanNodeType::kProcedureCall: {
      const auto &procedure_call = static_cast<const ProcedureCallPlan &>(plan);
      const CostEstimate &input =
          OnlyChildEstimate(child_estimates, plan.Name());
      const CostEstimate call = cost_model.EstimateProcedureCall(
          procedure_call.ProcedureName(), procedure_call.Arguments().size(),
          procedure_call.YieldItems().size());
      return {.estimated_rows = input.estimated_rows * call.estimated_rows,
              .cost = input.cost + input.estimated_rows * call.cost};
    }
    case LogicalPlanNodeType::kUnion: {
      CHECK(child_estimates.size() == 2, common::InternalError,
            "Union expected two child estimates");
      const auto &union_plan = static_cast<const UnionPlan &>(plan);
      return cost_model.EstimateUnion(child_estimates[0], child_estimates[1],
                                      union_plan.All());
    }
  }
  THROW(common::InternalError,
        "unknown logical plan estimate: " + std::string(plan.Name()));
}

void ApplyLogicalPlanTraits(LogicalPlan *plan) {
  CHECK(plan != nullptr, common::InternalError, "logical plan is null");
  switch (plan->Type()) {
    case LogicalPlanNodeType::kFilter:
    case LogicalPlanNodeType::kSkip:
    case LogicalPlanNodeType::kLimit:
    case LogicalPlanNodeType::kProduceResults:
    case LogicalPlanNodeType::kAssertIsNode:
    case LogicalPlanNodeType::kWriteBarrier:
    case LogicalPlanNodeType::kSetProperty:
    case LogicalPlanNodeType::kSetProperties:
    case LogicalPlanNodeType::kSetLabels:
    case LogicalPlanNodeType::kRemoveProperty:
    case LogicalPlanNodeType::kRemoveLabels:
    case LogicalPlanNodeType::kDelete:
    case LogicalPlanNodeType::kDetachDelete:
      InheritTraits(plan->Child(0), plan);
      return;
    case LogicalPlanNodeType::kSort: {
      const auto &sort = static_cast<const SortPlan &>(*plan);
      const bool distinct = plan->Child(0).DistinctTrait();
      plan->SetOrderingTrait(sort.Items());
      plan->SetDistinctTrait(distinct);
      return;
    }
    case LogicalPlanNodeType::kDistinct:
    case LogicalPlanNodeType::kAggregation:
      plan->ClearOrderingTrait();
      plan->SetDistinctTrait(true);
      return;
    case LogicalPlanNodeType::kUnion: {
      const auto &union_plan = static_cast<const UnionPlan &>(*plan);
      plan->ClearOrderingTrait();
      plan->SetDistinctTrait(!union_plan.All());
      return;
    }
    default:
      ClearTraits(plan);
      return;
  }
}

CostEstimate AnnotateLogicalPlanMetadata(LogicalPlan *plan,
                                         const CostModel &cost_model) {
  CHECK(plan != nullptr, common::InternalError, "logical plan is null");
  std::vector<CostEstimate> child_estimates;
  child_estimates.reserve(plan->ChildCount());
  for (auto &child : plan->Children()) {
    CHECK(child != nullptr, common::InternalError,
          "logical plan child is null");
    child_estimates.push_back(
        AnnotateLogicalPlanMetadata(child.get(), cost_model));
  }

  CostEstimate estimate =
      EstimateLogicalPlanNode(*plan, child_estimates, cost_model);
  plan->SetCostEstimate(estimate.estimated_rows, estimate.cost);
  ApplyLogicalPlanTraits(plan);
  return estimate;
}

QueryGraph QueryGraphFromMergeMatchGraph(const MergeMatchGraph &match_graph) {
  QueryGraph query_graph;
  query_graph.pattern_nodes = match_graph.pattern_nodes;
  query_graph.pattern_relationships = match_graph.pattern_relationships;
  query_graph.argument_ids = match_graph.argument_ids;
  for (const auto &label : match_graph.node_labels) {
    if (label.variable.empty() || label.labels.empty()) {
      continue;
    }
    CHECK(label.expression != nullptr, common::InternalError,
          "MERGE label predicate expression is null");
    Predicate predicate;
    predicate.expression = label.expression;
    predicate.dependencies.insert(label.variable);
    predicate.kind = PredicateKind::kNodeLabel;
    predicate.variable = label.variable;
    predicate.labels = label.labels;
    query_graph.selections.AddPredicate(std::move(predicate));
  }
  for (const auto &equality : match_graph.property_equalities) {
    if (equality.variable.empty() || equality.property_key.empty()) {
      continue;
    }
    CHECK(equality.value != nullptr, common::InvalidArgumentError,
          "MERGE property equality value is null");
    CHECK(equality.expression != nullptr, common::InternalError,
          "MERGE property equality expression is null");
    Predicate predicate;
    predicate.expression = equality.expression;
    predicate.dependencies.insert(equality.variable);
    AddSymbols(&predicate.dependencies,
               ast::CollectExpressionDependencies(*equality.value));
    predicate.kind = PredicateKind::kPropertyEquality;
    predicate.variable = equality.variable;
    predicate.property_key = equality.property_key;
    predicate.property_value = equality.value;
    predicate.comparison_op = "=";
    query_graph.selections.AddPredicate(std::move(predicate));
  }
  AddAssertIsNodeVariables(&query_graph);
  return query_graph;
}

class LogicalPlanBuilder {
 public:
  explicit LogicalPlanBuilder(const LogicalPlanBuilderOptions &options)
      : options_(options), component_planner_(MakeComponentPlanner(options)) {}

  std::unique_ptr<LogicalPlan> Build(const PlannerQuery &planner_query) {
    switch (planner_query.Kind()) {
      case PlannerQueryKind::kSingle:
        return Build(planner_query.RequireSingle());
      case PlannerQueryKind::kUnion:
        return BuildUnion(planner_query.RequireUnion(),
                          /*produce_results=*/true);
    }
    THROW(common::InternalError, "unknown planner query kind");
  }

  std::unique_ptr<LogicalPlan> Build(const SinglePlannerQuery &planner_query) {
    std::unique_ptr<LogicalPlan> plan =
        BuildQueryGraph(planner_query.query_graph);
    plan = ApplyHorizon(std::move(plan), planner_query.horizon);
    for (const SinglePlannerQuery *tail = planner_query.tail.get();
         tail != nullptr; tail = tail->tail.get()) {
      plan = BuildTailSegment(std::move(plan), *tail);
    }
    return plan;
  }

 private:
  std::unique_ptr<LogicalPlan> BuildUnionInput(
      const PlannerQuery &planner_query) {
    switch (planner_query.Kind()) {
      case PlannerQueryKind::kSingle:
        return Build(planner_query.RequireSingle());
      case PlannerQueryKind::kUnion:
        return BuildUnion(planner_query.RequireUnion(),
                          /*produce_results=*/false);
    }
    THROW(common::InternalError, "unknown planner query kind");
  }

  std::unique_ptr<LogicalPlan> BuildUnion(const UnionPlannerQuery &union_query,
                                          bool produce_results) {
    CHECK(union_query.lhs != nullptr, common::InvalidArgumentError,
          "UNION lhs planner query is null");
    std::vector<LogicalUnionMapping> mappings =
        LogicalUnionMappings(union_query.mappings);
    std::vector<std::string> output_columns;
    output_columns.reserve(mappings.size());
    for (const auto &mapping : mappings) {
      output_columns.push_back(mapping.output_variable);
    }
    std::unique_ptr<LogicalPlan> plan = std::make_unique<UnionPlan>(
        BuildUnionInput(*union_query.lhs), Build(union_query.rhs),
        std::move(mappings), union_query.all);
    if (produce_results) {
      plan = std::make_unique<ProduceResultsPlan>(std::move(plan),
                                                  std::move(output_columns));
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> BuildTailSegment(
      std::unique_ptr<LogicalPlan> input, const SinglePlannerQuery &segment) {
    CHECK(input != nullptr, common::InternalError, "tail input plan is null");
    ValidateTailArgumentsAvailable(*input, segment.query_graph);

    std::unique_ptr<LogicalPlan> plan = std::move(input);
    if (segment.query_graph.HasLocalWork()) {
      std::unique_ptr<LogicalPlan> rhs =
          BuildQueryGraph(segment.query_graph, false);
      plan = std::make_unique<ApplyPlan>(std::move(plan), std::move(rhs));
      QueryGraphPlanningContext context(&planned_predicates_, &options_);
      context.ApplyAvailableFilters(segment.query_graph.selections, &plan);
      context.ValidateAllPredicatesPlanned(segment.query_graph.selections);
    } else {
      planned_predicates_.clear();
      ValidateSupportedQueryGraph(segment.query_graph);
    }
    return ApplyHorizon(std::move(plan), segment.horizon);
  }

  std::unique_ptr<LogicalPlan> BuildQueryGraph(
      const QueryGraph &query_graph, bool validate_all_predicates = true) {
    planned_predicates_.clear();
    ValidateSupportedQueryGraph(query_graph);
    QueryGraphPlanningContext context(&planned_predicates_, &options_);

    std::vector<QueryGraphComponent> components =
        query_graph.ConnectedComponents();
    std::unique_ptr<LogicalPlan> plan;
    if (components.empty()) {
      plan = std::make_unique<ArgumentPlan>(Sorted(query_graph.argument_ids));
      context.ApplyAvailableFilters(query_graph.selections, &plan);
    } else {
      for (const auto &component : components) {
        std::unique_ptr<LogicalPlan> component_plan =
            component_planner_->Plan(query_graph, component, &context);
        context.ApplyAvailableFilters(query_graph.selections, &component_plan);
        if (plan == nullptr) {
          plan = std::move(component_plan);
        } else {
          plan = JoinComponents(std::move(plan), std::move(component_plan),
                                query_graph.selections);
          context.ApplyAvailableFilters(query_graph.selections, &plan);
        }
      }
    }

    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    plan = ApplyPathBuilds(std::move(plan), query_graph.path_patterns);
    plan = ApplyAssertIsNode(std::move(plan),
                             query_graph.assert_is_node_variables);
    context.ApplyAvailableFilters(query_graph.selections, &plan);
    plan = ApplyOptionalMatches(std::move(plan), query_graph, &context);
    plan =
        ApplyMutatingPatterns(std::move(plan), query_graph.mutating_patterns);
    if (validate_all_predicates) {
      context.ValidateAllPredicatesPlanned(query_graph.selections);
    }
    return plan;
  }

  void ValidateTailArgumentsAvailable(const LogicalPlan &input,
                                      const QueryGraph &query_graph) const {
    for (const auto &argument : query_graph.argument_ids) {
      CHECK(StringVectorContains(input.OutputColumns(), argument),
            common::InvalidArgumentError,
            std::string(kLogicalPlanStage) +
                ": tail argument is not available: " + argument);
    }
  }

  void ValidateSupportedQueryGraph(const QueryGraph &query_graph) const {
    CHECK(query_graph.hints.empty(), common::InvalidArgumentError,
          UnsupportedInStage(kLogicalPlanStage, "planner hint"));
    for (const auto &predicate : query_graph.selections.predicates) {
      if (predicate.kind == PredicateKind::kExistsSubquery ||
          predicate.kind == PredicateKind::kNotExistsSubquery) {
        CHECK(predicate.subquery != nullptr, common::InvalidArgumentError,
              "EXISTS predicate subquery is null");
      }
    }
  }

  std::unique_ptr<LogicalPlan> JoinComponents(
      std::unique_ptr<LogicalPlan> left, std::unique_ptr<LogicalPlan> right,
      const Selections &selections) {
    CHECK(left != nullptr && right != nullptr, common::InternalError,
          "component join input is null");
    const std::unordered_set<std::string> left_symbols = left->SolvedSymbols();
    const std::unordered_set<std::string> right_symbols =
        right->SolvedSymbols();

    std::vector<const Predicate *> value_join_predicates =
        JoinPredicates(selections, left_symbols, right_symbols,
                       /*value_hash_join=*/true);
    if (!value_join_predicates.empty()) {
      MarkPredicatesPlanned(value_join_predicates);
      return std::make_unique<ValueHashJoinPlan>(
          std::move(left), std::move(right),
          JoinPredicateExpressions(value_join_predicates));
    }

    std::vector<const Predicate *> predicate_join_predicates =
        JoinPredicates(selections, left_symbols, right_symbols,
                       /*value_hash_join=*/false);
    if (!predicate_join_predicates.empty()) {
      MarkPredicatesPlanned(predicate_join_predicates);
      return std::make_unique<PredicateJoinPlan>(
          std::move(left), std::move(right),
          JoinPredicateExpressions(predicate_join_predicates));
    }

    return std::make_unique<CartesianProductPlan>(std::move(left),
                                                  std::move(right));
  }

  std::vector<const Predicate *> JoinPredicates(
      const Selections &selections, const std::unordered_set<std::string> &left,
      const std::unordered_set<std::string> &right,
      bool value_hash_join) const {
    std::vector<const Predicate *> predicates;
    for (const auto &predicate : selections.predicates) {
      if (planned_predicates_.contains(&predicate)) {
        continue;
      }
      if (value_hash_join) {
        if (IsValueHashJoinPredicate(predicate, left, right)) {
          predicates.push_back(&predicate);
        }
        continue;
      }
      if (CanUsePredicateJoin(predicate) &&
          IsCrossComponentPredicate(predicate, left, right)) {
        predicates.push_back(&predicate);
      }
    }
    return predicates;
  }

  void MarkPredicatesPlanned(const std::vector<const Predicate *> &predicates) {
    for (const Predicate *predicate : predicates) {
      if (predicate != nullptr) {
        planned_predicates_.insert(predicate);
      }
    }
  }

  std::unique_ptr<LogicalPlan> ApplyMutatingPatterns(
      std::unique_ptr<LogicalPlan> plan,
      const std::vector<MutatingPattern> &mutating_patterns) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    if (mutating_patterns.empty()) {
      return plan;
    }

    plan = std::make_unique<WriteBarrierPlan>(std::move(plan));
    for (const auto &mutating_pattern : mutating_patterns) {
      switch (mutating_pattern.kind) {
        case MutatingPatternKind::kCreate:
          plan = ApplyCreatePattern(std::move(plan), mutating_pattern.create);
          break;
        case MutatingPatternKind::kMerge:
          plan = ApplyMergePattern(std::move(plan), mutating_pattern.merge);
          break;
        case MutatingPatternKind::kSet:
          plan =
              ApplySetPatterns(std::move(plan), mutating_pattern.set_patterns);
          break;
        case MutatingPatternKind::kRemove:
          plan = ApplyRemovePatterns(std::move(plan),
                                     mutating_pattern.remove_patterns);
          break;
        case MutatingPatternKind::kDelete:
          plan = ApplyDeletePatterns(std::move(plan),
                                     mutating_pattern.delete_patterns);
          break;
      }
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyCreatePattern(
      std::unique_ptr<LogicalPlan> plan, const CreatePattern &pattern) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    for (const auto &command : pattern.commands) {
      switch (command.kind) {
        case CreateEntityKind::kNode:
          CHECK(command.index < pattern.nodes.size(),
                common::InvalidArgumentError,
                "CREATE node command index is out of range");
          plan = std::make_unique<CreateNodePlan>(std::move(plan),
                                                  pattern.nodes[command.index]);
          break;
        case CreateEntityKind::kRelationship:
          CHECK(command.index < pattern.relationships.size(),
                common::InvalidArgumentError,
                "CREATE relationship command index is out of range");
          plan = std::make_unique<CreateRelationshipPlan>(
              std::move(plan), pattern.relationships[command.index]);
          break;
      }
    }
    return ApplyPathBuilds(std::move(plan), pattern.path_patterns);
  }

  std::unique_ptr<LogicalPlan> ApplyMergePattern(
      std::unique_ptr<LogicalPlan> plan, const MergePattern &merge) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    return std::make_unique<MergePlan>(
        std::move(plan), BuildMergeMatchPlan(merge.match_graph), merge);
  }

  std::unique_ptr<LogicalPlan> BuildMergeMatchPlan(
      const MergeMatchGraph &match_graph) {
    const std::unordered_set<const Predicate *> outer_predicates =
        planned_predicates_;
    std::unique_ptr<LogicalPlan> match_plan =
        BuildQueryGraph(QueryGraphFromMergeMatchGraph(match_graph));
    planned_predicates_ = outer_predicates;
    return match_plan;
  }

  std::unique_ptr<LogicalPlan> ApplySetPatterns(
      std::unique_ptr<LogicalPlan> plan,
      const std::vector<SetMutatingPattern> &patterns) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    for (const auto &pattern : patterns) {
      switch (pattern.kind) {
        case SetMutatingPatternKind::kSetProperty:
          plan = std::make_unique<SetPropertyPlan>(
              std::move(plan), pattern.entity, pattern.property_key,
              pattern.value);
          break;
        case SetMutatingPatternKind::kSetExactPropertiesFromMap:
          plan = std::make_unique<SetPropertiesPlan>(
              std::move(plan), pattern.entity, pattern.value,
              /*include_existing=*/false);
          break;
        case SetMutatingPatternKind::kSetIncludingPropertiesFromMap:
          plan = std::make_unique<SetPropertiesPlan>(
              std::move(plan), pattern.entity, pattern.value,
              /*include_existing=*/true);
          break;
        case SetMutatingPatternKind::kSetLabels:
          plan = std::make_unique<SetLabelsPlan>(
              std::move(plan), pattern.entity, pattern.labels);
          break;
      }
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyRemovePatterns(
      std::unique_ptr<LogicalPlan> plan,
      const std::vector<RemoveMutatingPattern> &patterns) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    for (const auto &pattern : patterns) {
      switch (pattern.kind) {
        case RemoveMutatingPatternKind::kRemoveProperty:
          plan = std::make_unique<RemovePropertyPlan>(
              std::move(plan), pattern.entity, pattern.property_key);
          break;
        case RemoveMutatingPatternKind::kRemoveLabels:
          plan = std::make_unique<RemoveLabelsPlan>(
              std::move(plan), pattern.entity, pattern.labels);
          break;
      }
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyDeletePatterns(
      std::unique_ptr<LogicalPlan> plan,
      const std::vector<DeleteExpressionPattern> &patterns) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    std::vector<const ast::Expression *> expressions;
    expressions.reserve(patterns.size());
    bool detach = false;
    for (const auto &pattern : patterns) {
      expressions.push_back(pattern.expression);
      detach = pattern.detach;
    }
    if (detach) {
      return std::make_unique<DetachDeletePlan>(std::move(plan),
                                                std::move(expressions));
    }
    return std::make_unique<DeletePlan>(std::move(plan),
                                        std::move(expressions));
  }

  std::unique_ptr<LogicalPlan> ApplyHorizon(std::unique_ptr<LogicalPlan> plan,
                                            const QueryHorizon &horizon) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    switch (horizon.kind) {
      case QueryHorizonKind::kRegularProjection:
        return ApplyRegularProjection(std::move(plan),
                                      horizon.RequireRegularProjection());
      case QueryHorizonKind::kDistinctProjection:
        return ApplyDistinctProjection(std::move(plan),
                                       horizon.RequireDistinctProjection());
      case QueryHorizonKind::kAggregatingProjection:
        return ApplyAggregatingProjection(
            std::move(plan), horizon.RequireAggregatingProjection());
      case QueryHorizonKind::kUnwind:
        return ApplyUnwind(std::move(plan), horizon.RequireUnwind());
      case QueryHorizonKind::kProcedureCall:
        return ApplyProcedureCall(std::move(plan),
                                  horizon.RequireProcedureCall());
      case QueryHorizonKind::kPassthrough:
        return plan;
    }
    THROW(common::InternalError, "unknown query horizon kind");
  }

  std::unique_ptr<LogicalPlan> ApplyPathBuilds(
      std::unique_ptr<LogicalPlan> plan,
      const std::vector<PathPattern> &paths) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    for (const auto &path : paths) {
      plan = std::make_unique<PathBuildPlan>(std::move(plan), path);
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyAssertIsNode(
      std::unique_ptr<LogicalPlan> plan,
      const std::unordered_set<LogicalVariable> &variables) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    if (variables.empty()) {
      return plan;
    }
    return std::make_unique<AssertIsNodePlan>(std::move(plan),
                                              Sorted(variables));
  }

  std::unique_ptr<LogicalPlan> ApplyOptionalMatches(
      std::unique_ptr<LogicalPlan> plan, const QueryGraph &query_graph,
      QueryGraphPlanningContext *context) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    CHECK(context != nullptr, common::InternalError,
          "query graph planning context is null");
    for (const auto &optional_match : query_graph.optional_matches) {
      std::unordered_set<const Predicate *> outer_predicates =
          planned_predicates_;
      std::unique_ptr<LogicalPlan> optional_plan =
          BuildQueryGraph(optional_match);
      planned_predicates_ = std::move(outer_predicates);
      plan = std::make_unique<OptionalApplyPlan>(std::move(plan),
                                                 std::move(optional_plan));
      context->ApplyAvailableFilters(query_graph.selections, &plan);
    }
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyRegularProjection(
      std::unique_ptr<LogicalPlan> plan,
      const RegularQueryProjection &projection) {
    plan =
        ApplyNestedExpressions(std::move(plan), projection.nested_expressions);
    std::vector<std::string> aliases = ProjectionAliases(projection.items);
    std::vector<LogicalProjectionItem> items =
        LogicalProjectionItems(projection.items, projection.nested_expressions);
    AppendPassthroughProjectionItems(*plan, projection, aliases, &items);
    plan = std::make_unique<ProjectionPlan>(std::move(plan), std::move(items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyDistinctProjection(
      std::unique_ptr<LogicalPlan> plan,
      const DistinctQueryProjection &projection) {
    plan =
        ApplyNestedExpressions(std::move(plan), projection.nested_expressions);
    std::vector<std::string> aliases =
        ProjectionAliases(projection.grouping_items);
    std::vector<LogicalProjectionItem> grouping_items = LogicalProjectionItems(
        projection.grouping_items, projection.nested_expressions);
    plan = std::make_unique<DistinctPlan>(std::move(plan),
                                          std::move(grouping_items));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyAggregatingProjection(
      std::unique_ptr<LogicalPlan> plan,
      const AggregatingQueryProjection &projection) {
    plan =
        ApplyNestedExpressions(std::move(plan), projection.nested_expressions);
    std::vector<std::string> aliases = ProjectionAliases(
        projection.grouping_items, projection.aggregation_items);
    std::vector<LogicalProjectionItem> grouping_items = LogicalProjectionItems(
        projection.grouping_items, projection.nested_expressions);
    plan = std::make_unique<AggregationPlan>(
        std::move(plan), std::move(grouping_items),
        LogicalProjectionItems(projection.aggregation_items,
                               projection.nested_expressions));
    return ApplyProjectionTail(std::move(plan), projection, std::move(aliases));
  }

  std::unique_ptr<LogicalPlan> ApplyUnwind(std::unique_ptr<LogicalPlan> plan,
                                           const UnwindHorizon &unwind) {
    CHECK(unwind.expression != nullptr, common::InvalidArgumentError,
          "UNWIND expression is null");
    CHECK(!unwind.alias.empty(), common::InvalidArgumentError,
          "UNWIND alias is empty");
    return std::make_unique<UnwindPlan>(std::move(plan), unwind.expression,
                                        unwind.alias);
  }

  std::unique_ptr<LogicalPlan> ApplyProcedureCall(
      std::unique_ptr<LogicalPlan> plan,
      const ProcedureCallHorizon &procedure_call) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    CHECK(!procedure_call.procedure_name.empty(), common::InvalidArgumentError,
          "procedure name is empty");
    ValidateProcedureCallDependencies(*plan, procedure_call);

    if (!procedure_call.read_only) {
      plan = std::make_unique<WriteBarrierPlan>(std::move(plan));
    }
    plan = std::make_unique<ProcedureCallPlan>(
        std::move(plan), procedure_call.procedure_name,
        procedure_call.arguments, procedure_call.yield_items,
        procedure_call.yield_star, procedure_call.read_only);
    return ApplyProjectionSelections(std::move(plan),
                                     procedure_call.yield_selections);
  }

  void ValidateProcedureCallDependencies(
      const LogicalPlan &input,
      const ProcedureCallHorizon &procedure_call) const {
    const std::unordered_set<std::string> &symbols = input.SolvedSymbols();
    for (const ast::Expression *argument : procedure_call.arguments) {
      if (argument == nullptr) {
        continue;
      }
      const std::unordered_set<std::string> dependencies =
          ast::CollectExpressionDependencies(*argument);
      CHECK(DependenciesMet(dependencies, symbols),
            common::InvalidArgumentError,
            UnsupportedInStage(kLogicalPlanStage,
                               "procedure argument with unmet dependencies"));
    }
  }

  std::unique_ptr<LogicalPlan> ApplyNestedExpressions(
      std::unique_ptr<LogicalPlan> plan,
      const std::vector<NestedIRExpression> &nested_expressions) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    std::unordered_set<const Predicate *> planned_predicates;
    QueryGraphPlanningContext context(&planned_predicates, &options_);
    context.ApplyNestedExpressions(nested_expressions, &plan);
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyProjectionSelections(
      std::unique_ptr<LogicalPlan> plan, const Selections &selections) {
    CHECK(plan != nullptr, common::InternalError, "logical plan is null");
    std::unordered_set<const Predicate *> planned_predicates;
    QueryGraphPlanningContext context(&planned_predicates, &options_);
    context.ApplyAvailableFilters(selections, &plan);
    context.ValidateAllPredicatesPlanned(selections);
    return plan;
  }

  std::unique_ptr<LogicalPlan> ApplyProjectionTail(
      std::unique_ptr<LogicalPlan> plan, const QueryProjection &projection,
      std::vector<std::string> aliases) {
    plan = ApplyProjectionSelections(std::move(plan), projection.selections);
    ValidateProjectionTailExpressionsAvailable(*plan, projection);

    if (!projection.required_order.empty()) {
      plan = std::make_unique<SortPlan>(
          std::move(plan),
          SortItems(projection.required_order, projection.nested_expressions));
    }
    if (projection.pagination.skip != nullptr) {
      plan = std::make_unique<SkipPlan>(
          std::move(plan), projection.pagination.skip,
          PrecomputedExpressions(projection.nested_expressions));
    }
    if (projection.pagination.limit != nullptr) {
      plan = std::make_unique<LimitPlan>(
          std::move(plan), projection.pagination.limit,
          PrecomputedExpressions(projection.nested_expressions));
    }
    if (projection.position == ProjectionPosition::kFinal) {
      plan = std::make_unique<ProduceResultsPlan>(std::move(plan),
                                                  std::move(aliases));
    } else if (plan->OutputColumns() != aliases) {
      plan = std::make_unique<ProjectionPlan>(
          std::move(plan), PassthroughProjectionItems(aliases));
    }
    return plan;
  }

  LogicalPlanBuilderOptions options_;
  std::unique_ptr<ComponentPlanner> component_planner_;
  std::unordered_set<const Predicate *> planned_predicates_;
};

}  // namespace

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query) {
  return CreateLogicalPlan(planner_query, LogicalPlanBuilderOptions{});
}

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query) {
  return CreateLogicalPlan(planner_query, LogicalPlanBuilderOptions{});
}

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query,
    const LogicalPlanBuilderOptions &options) {
  LogicalPlanBuilder builder(options);
  std::unique_ptr<LogicalPlan> plan = builder.Build(planner_query);
  AnnotateLogicalPlanMetadata(plan.get(),
                              CostModel(options.planner_statistics));
  return plan;
}

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query,
    const LogicalPlanBuilderOptions &options) {
  LogicalPlanBuilder builder(options);
  std::unique_ptr<LogicalPlan> plan = builder.Build(planner_query);
  AnnotateLogicalPlanMetadata(plan.get(),
                              CostModel(options.planner_statistics));
  return plan;
}

}  // namespace ir
