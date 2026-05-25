#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ir {

struct CostEstimate {
  double estimated_rows = 1.0;
  double cost = 1.0;
};

struct PropertyHistogramBucket {
  double lower_bound = 0.0;
  double upper_bound = 0.0;
  double row_count = 0.0;
};

struct PropertyHistogram {
  double total_rows = 0.0;
  double distinct_values = 0.0;
  std::vector<PropertyHistogramBucket> buckets;
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
  [[nodiscard]] virtual double EstimateCombinedFilterSelectivity(
      std::size_t predicate_count) const;
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
  [[nodiscard]] virtual std::optional<PropertyHistogram> NodePropertyHistogram(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key) const;
  [[nodiscard]] virtual std::optional<PropertyHistogram>
  RelationshipPropertyHistogram(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const;
  [[nodiscard]] virtual double EstimateValueHashJoinSelectivity(
      std::size_t predicate_count) const;
  [[nodiscard]] virtual double EstimatePredicateJoinSelectivity(
      std::size_t predicate_count) const;
  [[nodiscard]] virtual double EstimateDistinctSelectivity(
      std::size_t key_count) const;
  [[nodiscard]] virtual double EstimateAggregationGroupSelectivity(
      std::size_t grouping_key_count) const;
  [[nodiscard]] virtual double EstimateUnwindRowsPerInput() const;
  [[nodiscard]] virtual double EstimateLimitRows(
      double input_rows, std::optional<double> literal_limit) const;
  [[nodiscard]] virtual double EstimateProcedureRows(
      std::string_view procedure_name, std::size_t yield_count) const;
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
  [[nodiscard]] CostEstimate ApplyFilters(CostEstimate input,
                                          std::size_t filter_count) const;
  [[nodiscard]] CostEstimate EstimateProjection(CostEstimate input,
                                                std::size_t item_count) const;
  [[nodiscard]] CostEstimate EstimateDistinct(CostEstimate input,
                                              std::size_t key_count) const;
  [[nodiscard]] CostEstimate EstimateAggregation(
      CostEstimate input, std::size_t grouping_key_count,
      std::size_t aggregation_count) const;
  [[nodiscard]] CostEstimate EstimateSort(CostEstimate input,
                                          std::size_t item_count) const;
  [[nodiscard]] CostEstimate EstimateSkip(
      CostEstimate input, std::optional<double> literal_skip) const;
  [[nodiscard]] CostEstimate EstimateLimit(
      CostEstimate input, std::optional<double> literal_limit) const;
  [[nodiscard]] CostEstimate EstimateProduceResults(
      CostEstimate input, std::size_t column_count) const;
  [[nodiscard]] CostEstimate EstimateCartesianProduct(CostEstimate left,
                                                      CostEstimate right) const;
  [[nodiscard]] CostEstimate EstimateApply(CostEstimate left,
                                           CostEstimate right) const;
  [[nodiscard]] CostEstimate EstimateSemiApply(CostEstimate left,
                                               CostEstimate right) const;
  [[nodiscard]] CostEstimate EstimateRollUpApply(CostEstimate left,
                                                 CostEstimate right) const;
  [[nodiscard]] CostEstimate EstimateOptionalApply(CostEstimate left,
                                                   CostEstimate right) const;
  [[nodiscard]] CostEstimate EstimateUnwind(
      CostEstimate input, std::optional<double> literal_list_size) const;
  [[nodiscard]] CostEstimate EstimateUnion(CostEstimate left,
                                           CostEstimate right, bool all) const;
  [[nodiscard]] CostEstimate EstimateProcedureCall(
      std::string_view procedure_name, std::size_t argument_count,
      std::size_t yield_count) const;
  [[nodiscard]] CostEstimate EstimatePassThrough(CostEstimate input,
                                                 double per_row_cost) const;
  [[nodiscard]] CostEstimate EstimateWrite(CostEstimate input,
                                           double per_row_cost) const;

 private:
  const PlannerStatistics &Statistics() const;

  const PlannerStatistics *statistics_ = nullptr;
};

[[nodiscard]] CostEstimate ApplyFilterEstimates(CostEstimate estimate,
                                                std::size_t filter_count,
                                                const CostModel &cost_model);

}  // namespace ir
