#include "ir/planner/cost_model.h"

#include <algorithm>

namespace ir {

CostEstimate CostModel::EstimateArgument(std::size_t symbol_count) const {
  return {.estimated_rows = 1.0, .cost = 0.1 + 0.01 * symbol_count};
}

CostEstimate CostModel::EstimateNodeScan(
    const std::unordered_set<std::string> &labels) const {
  const double rows = labels.empty() ? 1000.0 : 100.0;
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateExpand(
    CostEstimate input,
    const std::vector<std::string> &relationship_types) const {
  const double fanout = relationship_types.empty() ? 10.0 : 3.0;
  const double rows = input.estimated_rows * fanout;
  return {.estimated_rows = rows, .cost = input.cost + rows};
}

CostEstimate CostModel::EstimateExpandInto(
    CostEstimate input,
    const std::vector<std::string> &relationship_types) const {
  const double selectivity = relationship_types.empty() ? 0.5 : 0.2;
  const double rows = std::max(1.0, input.estimated_rows * selectivity);
  return {.estimated_rows = rows,
          .cost = input.cost + input.estimated_rows * selectivity};
}

CostEstimate CostModel::ApplyFilter(CostEstimate input) const {
  return {.estimated_rows = std::max(1.0, input.estimated_rows * 0.1),
          .cost = input.cost + input.estimated_rows * 0.1};
}

CostEstimate ApplyFilterEstimates(CostEstimate estimate,
                                  std::size_t filter_count,
                                  const CostModel &cost_model) {
  for (std::size_t i = 0; i < filter_count; ++i) {
    estimate = cost_model.ApplyFilter(estimate);
  }
  return estimate;
}

}  // namespace ir
