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

class PlannerStatistics {
 public:
  PlannerStatistics() = default;
  PlannerStatistics(const PlannerStatistics &) = delete;
  PlannerStatistics &operator=(const PlannerStatistics &) = delete;
  virtual ~PlannerStatistics() = default;

  [[nodiscard]] virtual double EstimateNodeCount(
      const std::unordered_set<std::string> &labels) const = 0;
  [[nodiscard]] virtual double EstimateExpandFanout(
      const std::vector<std::string> &relationship_types) const = 0;
  [[nodiscard]] virtual double EstimateExpandIntoSelectivity(
      const std::vector<std::string> &relationship_types) const = 0;
  [[nodiscard]] virtual double EstimateFilterSelectivity() const = 0;
  [[nodiscard]] virtual double EstimateNodeHashJoinSelectivity(
      std::size_t key_count) const = 0;
};

class HeuristicPlannerStatistics final : public PlannerStatistics {
 public:
  [[nodiscard]] double EstimateNodeCount(
      const std::unordered_set<std::string> &labels) const override;
  [[nodiscard]] double EstimateExpandFanout(
      const std::vector<std::string> &relationship_types) const override;
  [[nodiscard]] double EstimateExpandIntoSelectivity(
      const std::vector<std::string> &relationship_types) const override;
  [[nodiscard]] double EstimateFilterSelectivity() const override;
  [[nodiscard]] double EstimateNodeHashJoinSelectivity(
      std::size_t key_count) const override;
};

class CostModel {
 public:
  explicit CostModel(const PlannerStatistics *statistics = nullptr);

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

 private:
  const PlannerStatistics &Statistics() const;

  const PlannerStatistics *statistics_ = nullptr;
};

[[nodiscard]] CostEstimate ApplyFilterEstimates(CostEstimate estimate,
                                                std::size_t filter_count,
                                                const CostModel &cost_model);

}  // namespace ir
