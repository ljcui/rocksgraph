#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "ir/query_ir.h"

namespace planner {

struct OrderingKeyItem {
  std::string expression;
  ir::LogicalOrderDirection direction = ir::LogicalOrderDirection::kAscending;
};

[[nodiscard]] bool operator==(const OrderingKeyItem &lhs,
                              const OrderingKeyItem &rhs);
[[nodiscard]] bool operator<(const OrderingKeyItem &lhs,
                             const OrderingKeyItem &rhs);

[[nodiscard]] std::vector<OrderingKeyItem> OrderingKey(
    const std::vector<ir::LogicalSortItem> &ordering);
[[nodiscard]] bool OrderingSatisfies(
    const std::vector<ir::LogicalSortItem> &provided,
    const std::vector<ir::LogicalSortItem> &required);
[[nodiscard]] bool OrderingDependenciesAvailable(
    const std::vector<ir::LogicalSortItem> &ordering,
    const std::unordered_set<std::string> &symbols);
[[nodiscard]] bool OrderingExpressionsDeterministic(
    const std::vector<ir::LogicalSortItem> &ordering);
[[nodiscard]] bool ExpressionIsDeterministic(const ast::Expression &expression);
[[nodiscard]] std::vector<ir::LogicalSortItem> PlanningOrder(
    const ir::InterestingOrder &interesting_order);
[[nodiscard]] std::vector<ir::LogicalSortItem> ProjectOrdering(
    const std::vector<ir::LogicalSortItem> &ordering,
    const ir::ProjectionPlan &projection);

}  // namespace planner
