#pragma once

#include <cstddef>
#include <memory>

#include "ir/logical_plan.h"
#include "ir/planner_query.h"

namespace ir {

enum class LogicalPlanComponentPlannerKind {
  kRuleBased,
  kIdp,
};

struct LogicalPlanBuilderOptions {
  LogicalPlanComponentPlannerKind component_planner =
      LogicalPlanComponentPlannerKind::kRuleBased;
  std::size_t max_idp_candidates_per_relationship_count = 128;
};

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query);
std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query);
std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query,
    const LogicalPlanBuilderOptions &options);
std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query,
    const LogicalPlanBuilderOptions &options);

}  // namespace ir
