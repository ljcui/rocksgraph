#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "ir/planner/logical_plan_builder.h"
#include "ir/planner_query.h"

namespace ir {

class QueryGraphPlanningContext {
 public:
  explicit QueryGraphPlanningContext(
      std::unordered_set<const Predicate *> *planned_predicates,
      const LogicalPlanBuilderOptions *options);

  [[nodiscard]] std::unique_ptr<LogicalPlan> BuildNodeLeaf(
      const QueryGraph &query_graph, std::string_view variable);
  std::size_t ApplyAvailableFilters(const Selections &selections,
                                    std::unique_ptr<LogicalPlan> *plan);
  void ApplyNestedExpressions(
      const std::vector<NestedIRExpression> &nested_expressions,
      std::unique_ptr<LogicalPlan> *plan) const;
  void ValidateAllPredicatesPlanned(const Selections &selections) const;

  [[nodiscard]] std::unordered_set<const Predicate *> Snapshot() const;
  void Restore(std::unordered_set<const Predicate *> planned_predicates);

 private:
  [[nodiscard]] const Predicate *FirstConsumableNodeLabelPredicate(
      const Selections &selections, std::string_view variable) const;
  [[nodiscard]] std::unique_ptr<LogicalPlan> BuildNestedPlan(
      const PlannerQuery &query) const;

  std::unordered_set<const Predicate *> *planned_predicates_;
  const LogicalPlanBuilderOptions *options_ = nullptr;
};

class ComponentPlanner {
 public:
  ComponentPlanner() = default;
  ComponentPlanner(const ComponentPlanner &) = delete;
  ComponentPlanner &operator=(const ComponentPlanner &) = delete;
  virtual ~ComponentPlanner() = default;

  [[nodiscard]] virtual std::unique_ptr<LogicalPlan> Plan(
      const QueryGraph &query_graph, const QueryGraphComponent &component,
      QueryGraphPlanningContext *context) const = 0;
};

[[nodiscard]] std::unique_ptr<ComponentPlanner> MakeComponentPlanner(
    const LogicalPlanBuilderOptions &options);

}  // namespace ir
