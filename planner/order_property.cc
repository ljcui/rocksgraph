#include "planner/order_property.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "ast/ast_const_walker.h"
#include "ast/ast_equal.h"
#include "ast/ast_node.h"
#include "ast/builtin_function.h"
#include "ast/expression_dependency.h"
#include "ast/expression_to_string.h"
#include "common/exception.h"
#include "ir/query_ir_internal.h"

namespace ir {
namespace {

class DeterminismChecker final : public ast::ASTConstWalker {
 public:
  [[nodiscard]] bool Deterministic() const noexcept { return deterministic_; }

 protected:
  void Visit(const ast::FunctionInvocation &node) override {
    const ast::BuiltinFunction *function =
        ast::FindBuiltinFunction(node.function_name);
    if (function == nullptr || !function->deterministic) {
      deterministic_ = false;
    }
    ast::ASTConstWalker::Visit(node);
  }

 private:
  bool deterministic_ = true;
};

LogicalOrderDirection ToLogicalDirection(OrderDirection direction) {
  switch (direction) {
    case OrderDirection::kAscending:
      return LogicalOrderDirection::kAscending;
    case OrderDirection::kDescending:
      return LogicalOrderDirection::kDescending;
  }
  THROW(common::InternalError, "unknown order direction");
}

const ast::Expression *ResolveProjectionAlias(
    const ast::Expression *expression,
    const std::vector<OrderProjectionMapping> &mappings) {
  const ast::Variable *variable = AsVariableExpression(expression);
  if (variable == nullptr) {
    return expression;
  }
  for (const auto &mapping : mappings) {
    if (mapping.projected_alias == variable->name &&
        mapping.source_expression != nullptr) {
      return mapping.source_expression;
    }
  }
  return expression;
}

LogicalSortItem AliasSortItem(std::string alias,
                              LogicalOrderDirection direction) {
  auto variable = std::make_shared<ast::Variable>();
  variable->name = std::move(alias);
  const ast::Expression *expression = variable.get();
  return {.expression = expression,
          .owned_expression = std::move(variable),
          .direction = direction};
}

}  // namespace

bool operator==(const OrderingKeyItem &lhs, const OrderingKeyItem &rhs) {
  return lhs.expression == rhs.expression && lhs.direction == rhs.direction;
}

bool operator<(const OrderingKeyItem &lhs, const OrderingKeyItem &rhs) {
  if (lhs.expression != rhs.expression) {
    return lhs.expression < rhs.expression;
  }
  return lhs.direction < rhs.direction;
}

std::vector<OrderingKeyItem> OrderingKey(
    const std::vector<LogicalSortItem> &ordering) {
  std::vector<OrderingKeyItem> key;
  key.reserve(ordering.size());
  for (const auto &item : ordering) {
    CHECK(item.expression != nullptr, common::InternalError,
          "ordering expression is null");
    key.push_back({.expression = ast::ExpressionToString(*item.expression),
                   .direction = item.direction});
  }
  return key;
}

bool OrderingSatisfies(const std::vector<LogicalSortItem> &provided,
                       const std::vector<LogicalSortItem> &required) {
  if (provided.size() < required.size()) {
    return false;
  }
  for (std::size_t i = 0; i < required.size(); ++i) {
    if (provided[i].direction != required[i].direction ||
        !ast::ASTEqual::Equal(provided[i].expression, required[i].expression)) {
      return false;
    }
  }
  return true;
}

bool OrderingDependenciesAvailable(
    const std::vector<LogicalSortItem> &ordering,
    const std::unordered_set<std::string> &symbols) {
  for (const auto &item : ordering) {
    if (item.expression == nullptr ||
        !DependenciesMet(ast::CollectExpressionDependencies(*item.expression),
                         symbols)) {
      return false;
    }
  }
  return true;
}

bool OrderingExpressionsDeterministic(
    const std::vector<LogicalSortItem> &ordering) {
  for (const auto &item : ordering) {
    if (item.expression == nullptr) {
      return false;
    }
    if (!ExpressionIsDeterministic(*item.expression)) {
      return false;
    }
  }
  return true;
}

bool ExpressionIsDeterministic(const ast::Expression &expression) {
  DeterminismChecker checker;
  expression.Accept(checker);
  return checker.Deterministic();
}

std::vector<LogicalSortItem> PlanningOrder(
    const InterestingOrder &interesting_order) {
  std::vector<LogicalSortItem> ordering;
  ordering.reserve(interesting_order.candidates.size());
  for (const auto &item : interesting_order.candidates) {
    const ast::Expression *expression = ResolveProjectionAlias(
        item.expression, interesting_order.reverse_projection);
    CHECK(expression != nullptr, common::InvalidArgumentError,
          "interesting order expression is null");
    ordering.push_back({.expression = expression,
                        .direction = ToLogicalDirection(item.direction)});
  }
  return ordering;
}

std::vector<LogicalSortItem> ProjectOrdering(
    const std::vector<LogicalSortItem> &ordering,
    const ProjectionPlan &projection) {
  std::vector<LogicalSortItem> projected;
  projected.reserve(ordering.size());
  const std::unordered_set<std::string> output_symbols(
      projection.OutputColumns().begin(), projection.OutputColumns().end());

  for (const auto &order_item : ordering) {
    if (order_item.expression == nullptr) {
      break;
    }
    const auto matching_item = std::find_if(
        projection.Items().begin(), projection.Items().end(),
        [&](const LogicalProjectionItem &item) {
          return item.expression != nullptr && !item.alias.empty() &&
                 ast::ASTEqual::Equal(item.expression, order_item.expression);
        });
    if (matching_item != projection.Items().end()) {
      projected.push_back(
          AliasSortItem(matching_item->alias, order_item.direction));
      continue;
    }
    if (DependenciesMet(
            ast::CollectExpressionDependencies(*order_item.expression),
            output_symbols)) {
      projected.push_back(order_item);
      continue;
    }
    break;
  }
  return projected;
}

}  // namespace ir
