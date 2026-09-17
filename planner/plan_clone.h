#pragma once

#include <memory>

#include "ir/logical_plan.h"
#include "planner/idp.h"

namespace planner {

[[nodiscard]] std::unique_ptr<ir::LogicalPlan> CloneComponentPlan(
    const ir::LogicalPlan &plan);
[[nodiscard]] PlanCandidate CloneCandidate(const PlanCandidate &candidate);

}  // namespace planner
