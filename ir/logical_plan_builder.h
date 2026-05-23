#pragma once

#include <memory>

#include "ir/logical_plan.h"
#include "ir/planner_query.h"

namespace ir {

std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const PlannerQuery &planner_query);
std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SinglePlannerQuery &planner_query);

}  // namespace ir
