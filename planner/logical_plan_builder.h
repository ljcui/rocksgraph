#pragma once

#include <cstddef>
#include <memory>

#include "ir/logical_plan.h"
#include "ir/query_ir.h"
#include "planner/catalog.h"

namespace ir {

class PlannerStatistics;

struct LogicalPlanBuilderOptions {
  std::size_t max_idp_candidates_per_relationship_count = 128;
  const PlannerStatistics *planner_statistics = nullptr;
  const PlannerCatalog *planner_catalog = nullptr;
};

// These low-level builders borrow AST expressions reachable through query_ir.
// Keep the source Statement and QueryIR alive for the resulting plan's
// lifetime. Prefer PlanCypher when compiling query text.
std::unique_ptr<LogicalPlan> CreateLogicalPlan(const QueryIR &query_ir);
std::unique_ptr<LogicalPlan> CreateLogicalPlan(const SingleQueryIR &query_ir);
std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const QueryIR &query_ir, const LogicalPlanBuilderOptions &options);
std::unique_ptr<LogicalPlan> CreateLogicalPlan(
    const SingleQueryIR &query_ir, const LogicalPlanBuilderOptions &options);

}  // namespace ir
