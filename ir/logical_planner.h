#pragma once

#include <memory>

#include "ast/ast_node.h"
#include "ir/idp_planner.h"
#include "ir/logical_plan.h"
#include "ir/query_ir.h"

namespace ir {

struct LogicalPlannerConfig {
  IDPPlannerConfig idp;
};

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const ast::Statement &statement, const LogicalPlannerConfig &config = {});

std::unique_ptr<LogicalPlan> BuildLogicalPlan(
    const QueryIR &query_ir, const LogicalPlannerConfig &config = {});

}  // namespace ir
