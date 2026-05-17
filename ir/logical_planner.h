#pragma once

#include <memory>

#include "ast/ast_node.h"
#include "ir/idp_planner.h"
#include "ir/logical_plan.h"
#include "ir/planner_query.h"

namespace ir {

struct LogicalPlannerConfig {
  IDPPlannerConfig idp;
};

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const ast::Statement &statement, const LogicalPlannerConfig &config = {});

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const PlannerQuery &planner_query, const LogicalPlannerConfig &config = {});

}  // namespace ir
