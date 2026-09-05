#pragma once

#include <string>
#include <vector>

#include "runtime/execution_context.h"
#include "runtime/physical_plan.h"
#include "storage/storage.h"
#include "value/value.h"

namespace rg {

struct PhysicalExecutionResult {
  std::vector<std::vector<Value>> rows;
};

[[nodiscard]] PhysicalExecutionResult ExecutePhysicalPlan(
    const PhysicalPlan &plan, const GraphReader &graph_reader, Storage *storage,
    const QueryParameters &parameters,
    const std::vector<std::string> &result_columns, bool collect_results);

}  // namespace rg
