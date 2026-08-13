#pragma once

#include <vector>

#include "ir/logical_plan.h"
#include "runtime/execution_context.h"
#include "runtime/query_row.h"

namespace rg {

[[nodiscard]] std::vector<QueryRow> ProjectDistinctRows(
    const std::vector<ir::LogicalProjectionItem> &grouping_items,
    const std::vector<QueryRow> &rows, ExecutionContext context = {});

[[nodiscard]] std::vector<QueryRow> AggregateRows(
    const std::vector<ir::LogicalProjectionItem> &grouping_items,
    const std::vector<ir::LogicalProjectionItem> &aggregation_items,
    const std::vector<QueryRow> &rows, ExecutionContext context = {});

}  // namespace rg
