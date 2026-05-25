#pragma once

#include <cstddef>
#include <string>
#include <string_view>
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
  [[nodiscard]] virtual double EstimateNodeIndexSeekSelectivity(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key) const;
  [[nodiscard]] virtual double EstimateNodeIndexRangeSeekSelectivity(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key, std::size_t bound_count) const;
  [[nodiscard]] virtual double EstimateRelationshipCount(
      const std::vector<std::string> &relationship_types) const;
  [[nodiscard]] virtual double EstimateRelationshipIndexSeekSelectivity(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const;
  [[nodiscard]] virtual double EstimateRelationshipIndexRangeSeekSelectivity(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, std::size_t bound_count) const;
  [[nodiscard]] virtual double EstimateValueHashJoinSelectivity(
      std::size_t predicate_count) const;
  [[nodiscard]] virtual double EstimatePredicateJoinSelectivity(
      std::size_t predicate_count) const;
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
  [[nodiscard]] CostEstimate EstimateNodeIndexSeek(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key) const;
  [[nodiscard]] CostEstimate EstimateNodeIndexRangeSeek(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key, std::size_t bound_count) const;
  [[nodiscard]] CostEstimate EstimateRelationshipTypeScan(
      const std::vector<std::string> &relationship_types) const;
  [[nodiscard]] CostEstimate EstimateRelationshipIndexSeek(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const;
  [[nodiscard]] CostEstimate EstimateRelationshipIndexRangeSeek(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, std::size_t bound_count) const;
  [[nodiscard]] CostEstimate EstimateExpand(
      CostEstimate input,
      const std::vector<std::string> &relationship_types) const;
  [[nodiscard]] CostEstimate EstimateExpandInto(
      CostEstimate input,
      const std::vector<std::string> &relationship_types) const;
  [[nodiscard]] CostEstimate EstimateNodeHashJoin(CostEstimate left,
                                                  CostEstimate right,
                                                  std::size_t key_count) const;
  [[nodiscard]] CostEstimate EstimateValueHashJoin(
      CostEstimate left, CostEstimate right, std::size_t predicate_count) const;
  [[nodiscard]] CostEstimate EstimatePredicateJoin(
      CostEstimate left, CostEstimate right, std::size_t predicate_count) const;
  [[nodiscard]] CostEstimate ApplyFilter(CostEstimate input) const;

 private:
  const PlannerStatistics &Statistics() const;

  const PlannerStatistics *statistics_ = nullptr;
};

[[nodiscard]] CostEstimate ApplyFilterEstimates(CostEstimate estimate,
                                                std::size_t filter_count,
                                                const CostModel &cost_model);

}  // namespace ir
