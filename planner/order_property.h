#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "ir/query_ir.h"

namespace ir {

struct OrderingKeyItem {
  std::string expression;
  LogicalOrderDirection direction = LogicalOrderDirection::kAscending;
};

[[nodiscard]] bool operator==(const OrderingKeyItem &lhs,
                              const OrderingKeyItem &rhs);
[[nodiscard]] bool operator<(const OrderingKeyItem &lhs,
                             const OrderingKeyItem &rhs);

[[nodiscard]] std::vector<OrderingKeyItem> OrderingKey(
    const std::vector<LogicalSortItem> &ordering);
[[nodiscard]] bool OrderingSatisfies(
    const std::vector<LogicalSortItem> &provided,
    const std::vector<LogicalSortItem> &required);
[[nodiscard]] bool OrderingDependenciesAvailable(
    const std::vector<LogicalSortItem> &ordering,
    const std::unordered_set<std::string> &symbols);
[[nodiscard]] bool OrderingExpressionsDeterministic(
    const std::vector<LogicalSortItem> &ordering);
[[nodiscard]] bool ExpressionIsDeterministic(const ast::Expression &expression);
[[nodiscard]] std::vector<LogicalSortItem> PlanningOrder(
    const InterestingOrder &interesting_order);
[[nodiscard]] std::vector<LogicalSortItem> ProjectOrdering(
    const std::vector<LogicalSortItem> &ordering,
    const ProjectionPlan &projection);

}  // namespace ir
