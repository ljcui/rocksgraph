#pragma once

#include <cstddef>
#include <memory>
#include <unordered_set>

#include "ir/logical_plan.h"
#include "ir/planner_query.h"

namespace ir {

struct IDPPlannerConfig {
  std::size_t max_relationships = 20;
  std::size_t expand_cost = 10;
  std::size_t expand_into_cost = 8;
  std::size_t cartesian_product_cost = 100;
};

std::unique_ptr<LogicalPlan> BuildIDPLogicalPlan(
    const QueryGraph &query_graph, const IDPPlannerConfig &config = {});

std::unique_ptr<LogicalPlan> BuildIDPLogicalPlan(
    const QueryGraph &query_graph,
    const std::unordered_set<std::string> &argument_symbols,
    const IDPPlannerConfig &config = {});

}  // namespace ir
