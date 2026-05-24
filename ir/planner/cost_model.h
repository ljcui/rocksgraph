#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace ir {

struct CostEstimate {
  double estimated_rows = 1.0;
  double cost = 1.0;
};

class CostModel {
 public:
  [[nodiscard]] CostEstimate EstimateArgument(std::size_t symbol_count) const;
  [[nodiscard]] CostEstimate EstimateNodeScan(
      const std::unordered_set<std::string> &labels) const;
  [[nodiscard]] CostEstimate EstimateExpand(
      CostEstimate input,
      const std::vector<std::string> &relationship_types) const;
  [[nodiscard]] CostEstimate EstimateExpandInto(
      CostEstimate input,
      const std::vector<std::string> &relationship_types) const;
  [[nodiscard]] CostEstimate EstimateNodeHashJoin(CostEstimate left,
                                                  CostEstimate right,
                                                  std::size_t key_count) const;
  [[nodiscard]] CostEstimate ApplyFilter(CostEstimate input) const;
};

[[nodiscard]] CostEstimate ApplyFilterEstimates(CostEstimate estimate,
                                                std::size_t filter_count,
                                                const CostModel &cost_model);

}  // namespace ir
