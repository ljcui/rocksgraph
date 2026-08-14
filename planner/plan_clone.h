#pragma once

#include <memory>

#include "ir/logical_plan.h"
#include "planner/idp.h"

namespace ir {

[[nodiscard]] std::unique_ptr<LogicalPlan> CloneComponentPlan(
    const LogicalPlan &plan);
[[nodiscard]] PlanCandidate CloneCandidate(const PlanCandidate &candidate);

}  // namespace ir
