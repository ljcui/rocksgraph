#include "ir/planner/cost_model.h"

#include <algorithm>

namespace ir {
namespace {

const HeuristicPlannerStatistics &DefaultStatistics() {
  static const HeuristicPlannerStatistics statistics;
  return statistics;
}

}  // namespace

double PlannerStatistics::EstimateNodeIndexSeekSelectivity(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key) const {
  (void)labels;
  (void)property_key;
  return 0.01;
}

double PlannerStatistics::EstimateNodeIndexRangeSeekSelectivity(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key, std::size_t bound_count) const {
  (void)labels;
  (void)property_key;
  return bound_count > 1 ? 0.05 : 0.1;
}

double PlannerStatistics::EstimateRelationshipCount(
    const std::vector<std::string> &relationship_types) const {
  return relationship_types.empty() ? 10000.0 : 5000.0;
}

double PlannerStatistics::EstimateRelationshipIndexSeekSelectivity(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  (void)relationship_types;
  (void)property_key;
  return 0.01;
}

double PlannerStatistics::EstimateRelationshipIndexRangeSeekSelectivity(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, std::size_t bound_count) const {
  (void)relationship_types;
  (void)property_key;
  return bound_count > 1 ? 0.05 : 0.1;
}

double PlannerStatistics::EstimateValueHashJoinSelectivity(
    std::size_t predicate_count) const {
  return predicate_count == 0 ? 1.0 : 1.0 / predicate_count;
}

double PlannerStatistics::EstimatePredicateJoinSelectivity(
    std::size_t predicate_count) const {
  if (predicate_count == 0) {
    return 1.0;
  }
  double selectivity = 1.0;
  for (std::size_t i = 0; i < predicate_count; ++i) {
    selectivity *= EstimateFilterSelectivity();
  }
  return selectivity;
}

double HeuristicPlannerStatistics::EstimateNodeCount(
    const std::unordered_set<std::string> &labels) const {
  return labels.empty() ? 1000.0 : 100.0;
}

double HeuristicPlannerStatistics::EstimateExpandFanout(
    const std::vector<std::string> &relationship_types) const {
  return relationship_types.empty() ? 10.0 : 3.0;
}

double HeuristicPlannerStatistics::EstimateExpandIntoSelectivity(
    const std::vector<std::string> &relationship_types) const {
  return relationship_types.empty() ? 0.5 : 0.2;
}

double HeuristicPlannerStatistics::EstimateFilterSelectivity() const {
  return 0.1;
}

double HeuristicPlannerStatistics::EstimateNodeHashJoinSelectivity(
    std::size_t key_count) const {
  return key_count == 0 ? 1.0 : 1.0 / key_count;
}

CostModel::CostModel(const PlannerStatistics *statistics)
    : statistics_(statistics) {}

CostEstimate CostModel::EstimateArgument(std::size_t symbol_count) const {
  return {.estimated_rows = 1.0, .cost = 0.1 + 0.01 * symbol_count};
}

CostEstimate CostModel::EstimateNodeScan(
    const std::unordered_set<std::string> &labels) const {
  const double rows = Statistics().EstimateNodeCount(labels);
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateNodeIndexSeek(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key) const {
  const double base_rows = Statistics().EstimateNodeCount(labels);
  const double rows =
      std::max(1.0, base_rows * Statistics().EstimateNodeIndexSeekSelectivity(
                                    labels, property_key));
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateNodeIndexRangeSeek(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key, std::size_t bound_count) const {
  const double base_rows = Statistics().EstimateNodeCount(labels);
  const double rows = std::max(
      1.0, base_rows * Statistics().EstimateNodeIndexRangeSeekSelectivity(
                           labels, property_key, bound_count));
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateRelationshipTypeScan(
    const std::vector<std::string> &relationship_types) const {
  const double rows =
      Statistics().EstimateRelationshipCount(relationship_types);
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateRelationshipIndexSeek(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  const double base_rows =
      Statistics().EstimateRelationshipCount(relationship_types);
  const double rows = std::max(
      1.0, base_rows * Statistics().EstimateRelationshipIndexSeekSelectivity(
                           relationship_types, property_key));
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateRelationshipIndexRangeSeek(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key, std::size_t bound_count) const {
  const double base_rows =
      Statistics().EstimateRelationshipCount(relationship_types);
  const double rows = std::max(
      1.0,
      base_rows * Statistics().EstimateRelationshipIndexRangeSeekSelectivity(
                      relationship_types, property_key, bound_count));
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateExpand(
    CostEstimate input,
    const std::vector<std::string> &relationship_types) const {
  const double fanout = Statistics().EstimateExpandFanout(relationship_types);
  const double rows = input.estimated_rows * fanout;
  return {.estimated_rows = rows, .cost = input.cost + rows};
}

CostEstimate CostModel::EstimateExpandInto(
    CostEstimate input,
    const std::vector<std::string> &relationship_types) const {
  const double selectivity =
      Statistics().EstimateExpandIntoSelectivity(relationship_types);
  const double rows = std::max(1.0, input.estimated_rows * selectivity);
  return {.estimated_rows = rows,
          .cost = input.cost + input.estimated_rows * selectivity};
}

CostEstimate CostModel::EstimateNodeHashJoin(CostEstimate left,
                                             CostEstimate right,
                                             std::size_t key_count) const {
  const double key_selectivity =
      Statistics().EstimateNodeHashJoinSelectivity(key_count);
  const double rows =
      std::max(1.0, std::min(left.estimated_rows, right.estimated_rows) *
                        key_selectivity);
  return {.estimated_rows = rows,
          .cost = left.cost + right.cost + left.estimated_rows +
                  right.estimated_rows + rows};
}

CostEstimate CostModel::EstimateValueHashJoin(
    CostEstimate left, CostEstimate right, std::size_t predicate_count) const {
  const double selectivity =
      Statistics().EstimateValueHashJoinSelectivity(predicate_count);
  const double rows = std::max(
      1.0, std::min(left.estimated_rows, right.estimated_rows) * selectivity);
  return {.estimated_rows = rows,
          .cost = left.cost + right.cost + left.estimated_rows +
                  right.estimated_rows + rows};
}

CostEstimate CostModel::EstimatePredicateJoin(
    CostEstimate left, CostEstimate right, std::size_t predicate_count) const {
  const double selectivity =
      Statistics().EstimatePredicateJoinSelectivity(predicate_count);
  const double rows =
      std::max(1.0, left.estimated_rows * right.estimated_rows * selectivity);
  return {.estimated_rows = rows,
          .cost = left.cost + right.cost + left.estimated_rows +
                  right.estimated_rows + rows};
}

CostEstimate CostModel::ApplyFilter(CostEstimate input) const {
  const double selectivity = Statistics().EstimateFilterSelectivity();
  return {.estimated_rows = std::max(1.0, input.estimated_rows * selectivity),
          .cost = input.cost + input.estimated_rows * selectivity};
}

const PlannerStatistics &CostModel::Statistics() const {
  if (statistics_ != nullptr) {
    return *statistics_;
  }
  return DefaultStatistics();
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
