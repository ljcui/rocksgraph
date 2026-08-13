#include "ir/planner/cost_model.h"

#include <algorithm>
#include <cmath>

namespace ir {
namespace {

const HeuristicPlannerStatistics &DefaultStatistics() {
  static const HeuristicPlannerStatistics kStatistics;
  return kStatistics;
}

double AtLeastOne(double value) { return std::max(1.0, value); }

double NonNegative(double value) { return std::max(0.0, value); }

}  // namespace

double PlannerStatistics::EstimateCombinedFilterSelectivity(
    std::size_t predicate_count) const {
  double selectivity = 1.0;
  for (std::size_t i = 0; i < predicate_count; ++i) {
    selectivity *= EstimateFilterSelectivity();
  }
  return selectivity;
}

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

std::optional<PropertyHistogram> PlannerStatistics::NodePropertyHistogram(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key) const {
  (void)labels;
  (void)property_key;
  return std::nullopt;
}

std::optional<PropertyHistogram>
PlannerStatistics::RelationshipPropertyHistogram(
    const std::vector<std::string> &relationship_types,
    std::string_view property_key) const {
  (void)relationship_types;
  (void)property_key;
  return std::nullopt;
}

double PlannerStatistics::EstimateValueHashJoinSelectivity(
    std::size_t predicate_count) const {
  return predicate_count == 0 ? 1.0
                              : 1.0 / static_cast<double>(predicate_count);
}

double PlannerStatistics::EstimatePredicateJoinSelectivity(
    std::size_t predicate_count) const {
  if (predicate_count == 0) {
    return 1.0;
  }
  return EstimateCombinedFilterSelectivity(predicate_count);
}

double PlannerStatistics::EstimateDistinctSelectivity(
    std::size_t key_count) const {
  return key_count == 0 ? 1.0 : 0.5;
}

double PlannerStatistics::EstimateAggregationGroupSelectivity(
    std::size_t grouping_key_count) const {
  return grouping_key_count == 0 ? 0.0 : 0.1;
}

double PlannerStatistics::EstimateUnwindRowsPerInput() const { return 10.0; }

double PlannerStatistics::EstimateLimitRows(
    double input_rows, std::optional<double> literal_limit) const {
  if (literal_limit.has_value()) {
    return std::min(input_rows, NonNegative(*literal_limit));
  }
  return std::min(input_rows, AtLeastOne(input_rows * 0.1));
}

double PlannerStatistics::EstimateProcedureRows(std::string_view procedure_name,
                                                std::size_t yield_count) const {
  (void)procedure_name;
  (void)yield_count;
  return 100.0;
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
  return key_count == 0 ? 1.0 : 1.0 / static_cast<double>(key_count);
}

CostModel::CostModel(const PlannerStatistics *statistics)
    : statistics_(statistics) {}

CostEstimate CostModel::EstimateArgument(std::size_t symbol_count) const {
  return {.estimated_rows = 1.0,
          .cost = 0.1 + 0.01 * static_cast<double>(symbol_count)};
}

CostEstimate CostModel::EstimateNodeScan(
    const std::unordered_set<std::string> &labels) const {
  const double rows = Statistics().EstimateNodeCount(labels);
  return {.estimated_rows = rows, .cost = rows};
}

CostEstimate CostModel::EstimateNodeIndexSeek(
    const std::unordered_set<std::string> &labels,
    std::string_view property_key, bool unique) const {
  if (unique) {
    return {.estimated_rows = 1.0, .cost = 1.0};
  }
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
    std::string_view property_key, bool unique) const {
  if (unique) {
    return {.estimated_rows = 1.0, .cost = 1.0};
  }
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
  return ApplyFilters(input, 1);
}

CostEstimate CostModel::ApplyFilters(CostEstimate input,
                                     std::size_t filter_count) const {
  if (filter_count == 0) {
    return input;
  }

  double cost = input.cost;
  double rows_for_cost = input.estimated_rows;
  const double single_filter_selectivity =
      Statistics().EstimateFilterSelectivity();
  for (std::size_t i = 0; i < filter_count; ++i) {
    rows_for_cost = AtLeastOne(rows_for_cost * single_filter_selectivity);
    cost += rows_for_cost;
  }

  const double combined_selectivity =
      Statistics().EstimateCombinedFilterSelectivity(filter_count);
  return {
      .estimated_rows = AtLeastOne(input.estimated_rows * combined_selectivity),
      .cost = cost};
}

CostEstimate CostModel::EstimateProjection(CostEstimate input,
                                           std::size_t item_count) const {
  return EstimatePassThrough(
      input, 0.01 * static_cast<double>(std::max<std::size_t>(1, item_count)));
}

CostEstimate CostModel::EstimateDistinct(CostEstimate input,
                                         std::size_t key_count) const {
  const double rows =
      AtLeastOne(input.estimated_rows *
                 Statistics().EstimateDistinctSelectivity(key_count));
  return {.estimated_rows = rows,
          .cost = input.cost + input.estimated_rows + rows};
}

CostEstimate CostModel::EstimateAggregation(
    CostEstimate input, std::size_t grouping_key_count,
    std::size_t aggregation_count) const {
  const double rows =
      grouping_key_count == 0
          ? 1.0
          : AtLeastOne(input.estimated_rows *
                       Statistics().EstimateAggregationGroupSelectivity(
                           grouping_key_count));
  const double per_row_cost =
      1.0 + 0.1 * static_cast<double>(aggregation_count);
  return {.estimated_rows = rows,
          .cost = input.cost + input.estimated_rows * per_row_cost + rows};
}

CostEstimate CostModel::EstimateSort(CostEstimate input,
                                     std::size_t item_count) const {
  const double comparison_factor =
      static_cast<double>(std::max<std::size_t>(1, item_count));
  const double sort_work = input.estimated_rows *
                           std::log2(input.estimated_rows + 1.0) *
                           comparison_factor;
  return {.estimated_rows = input.estimated_rows,
          .cost = input.cost + sort_work};
}

CostEstimate CostModel::EstimateSkip(CostEstimate input,
                                     std::optional<double> literal_skip) const {
  const double rows =
      literal_skip.has_value()
          ? NonNegative(input.estimated_rows - NonNegative(*literal_skip))
          : input.estimated_rows;
  return {.estimated_rows = rows, .cost = input.cost + input.estimated_rows};
}

CostEstimate CostModel::EstimateLimit(
    CostEstimate input, std::optional<double> literal_limit) const {
  const double rows = NonNegative(
      Statistics().EstimateLimitRows(input.estimated_rows, literal_limit));
  return {.estimated_rows = rows, .cost = input.cost + rows};
}

CostEstimate CostModel::EstimateProduceResults(CostEstimate input,
                                               std::size_t column_count) const {
  return EstimatePassThrough(
      input,
      0.01 * static_cast<double>(std::max<std::size_t>(1, column_count)));
}

CostEstimate CostModel::EstimateCartesianProduct(CostEstimate left,
                                                 CostEstimate right) const {
  const double rows = left.estimated_rows * right.estimated_rows;
  return {.estimated_rows = rows,
          .cost = left.cost + right.cost + left.estimated_rows +
                  right.estimated_rows + rows};
}

CostEstimate CostModel::EstimateApply(CostEstimate left,
                                      CostEstimate right) const {
  const double rows = left.estimated_rows * right.estimated_rows;
  return {.estimated_rows = rows,
          .cost = left.cost + left.estimated_rows * right.cost + rows};
}

CostEstimate CostModel::EstimateSemiApply(CostEstimate left,
                                          CostEstimate right) const {
  return {.estimated_rows = left.estimated_rows,
          .cost = left.cost + left.estimated_rows * right.cost};
}

CostEstimate CostModel::EstimateRollUpApply(CostEstimate left,
                                            CostEstimate right) const {
  return {.estimated_rows = left.estimated_rows,
          .cost = left.cost + left.estimated_rows * right.cost +
                  left.estimated_rows};
}

CostEstimate CostModel::EstimateOptionalApply(CostEstimate left,
                                              CostEstimate right) const {
  const double rows =
      std::max(left.estimated_rows, left.estimated_rows * right.estimated_rows);
  return {.estimated_rows = rows,
          .cost = left.cost + left.estimated_rows * right.cost + rows};
}

CostEstimate CostModel::EstimateUnwind(
    CostEstimate input, std::optional<double> literal_list_size) const {
  const double rows_per_input =
      literal_list_size.has_value()
          ? NonNegative(*literal_list_size)
          : NonNegative(Statistics().EstimateUnwindRowsPerInput());
  const double rows = input.estimated_rows * rows_per_input;
  return {.estimated_rows = rows, .cost = input.cost + rows};
}

CostEstimate CostModel::EstimateUnion(CostEstimate left, CostEstimate right,
                                      bool all) const {
  const double input_rows = left.estimated_rows + right.estimated_rows;
  const double rows =
      all ? input_rows
          : AtLeastOne(input_rows *
                       Statistics().EstimateDistinctSelectivity(1));
  return {.estimated_rows = rows,
          .cost = left.cost + right.cost + input_rows + rows};
}

CostEstimate CostModel::EstimateProcedureCall(std::string_view procedure_name,
                                              std::size_t argument_count,
                                              std::size_t yield_count) const {
  const double rows = NonNegative(
      Statistics().EstimateProcedureRows(procedure_name, yield_count));
  return {.estimated_rows = rows,
          .cost = rows * (1.0 + 0.1 * static_cast<double>(argument_count) +
                          0.05 * static_cast<double>(yield_count))};
}

CostEstimate CostModel::EstimatePassThrough(CostEstimate input,
                                            double per_row_cost) const {
  return {.estimated_rows = input.estimated_rows,
          .cost = input.cost + input.estimated_rows * per_row_cost};
}

CostEstimate CostModel::EstimateWrite(CostEstimate input,
                                      double per_row_cost) const {
  return EstimatePassThrough(input, per_row_cost);
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
  return cost_model.ApplyFilters(estimate, filter_count);
}

}  // namespace ir
