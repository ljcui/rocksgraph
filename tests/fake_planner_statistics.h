#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/planner/cost_model.h"

namespace test_support {

class FakePlannerStatistics final : public ir::PlannerStatistics {
 public:
  double all_node_count = 1000.0;
  double labeled_node_count = 100.0;
  std::unordered_map<std::string, double> node_count_by_label;
  double node_index_seek_selectivity = 0.01;
  double node_index_range_seek_selectivity = 0.1;
  std::unordered_map<std::string, double> node_index_seek_selectivity_by_key;
  std::unordered_map<std::string, double> node_index_range_selectivity_by_key;

  double untyped_relationship_count = 10000.0;
  double typed_relationship_count = 5000.0;
  std::unordered_map<std::string, double> relationship_count_by_type;
  double relationship_index_seek_selectivity = 0.01;
  double relationship_index_range_seek_selectivity = 0.1;
  std::unordered_map<std::string, double>
      relationship_index_seek_selectivity_by_key;
  std::unordered_map<std::string, double>
      relationship_index_range_selectivity_by_key;
  double untyped_expand_fanout = 10.0;
  double typed_expand_fanout = 3.0;
  std::unordered_map<std::string, double> expand_fanout_by_type;
  double untyped_expand_into_selectivity = 0.5;
  double typed_expand_into_selectivity = 0.2;
  std::unordered_map<std::string, double> expand_into_selectivity_by_type;
  double filter_selectivity = 0.1;
  double node_hash_join_selectivity = 1.0;
  double value_hash_join_selectivity = 1.0;
  double predicate_join_selectivity = 0.1;

  [[nodiscard]] double EstimateNodeCount(
      const std::unordered_set<std::string> &labels) const override {
    if (labels.empty()) {
      return all_node_count;
    }

    double best_count = 0.0;
    bool found_label_count = false;
    for (const auto &label : labels) {
      const auto found = node_count_by_label.find(label);
      if (found == node_count_by_label.end()) {
        continue;
      }
      best_count = found_label_count ? std::min(best_count, found->second)
                                     : found->second;
      found_label_count = true;
    }
    return found_label_count ? best_count : labeled_node_count;
  }

  [[nodiscard]] double EstimateNodeIndexSeekSelectivity(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key) const override {
    (void)labels;
    const auto found =
        node_index_seek_selectivity_by_key.find(std::string(property_key));
    return found == node_index_seek_selectivity_by_key.end()
               ? node_index_seek_selectivity
               : found->second;
  }

  [[nodiscard]] double EstimateNodeIndexRangeSeekSelectivity(
      const std::unordered_set<std::string> &labels,
      std::string_view property_key, std::size_t bound_count) const override {
    (void)labels;
    (void)bound_count;
    const auto found =
        node_index_range_selectivity_by_key.find(std::string(property_key));
    return found == node_index_range_selectivity_by_key.end()
               ? node_index_range_seek_selectivity
               : found->second;
  }

  [[nodiscard]] double EstimateRelationshipCount(
      const std::vector<std::string> &relationship_types) const override {
    if (relationship_types.empty()) {
      return untyped_relationship_count;
    }

    double best_count = 0.0;
    bool found_type_count = false;
    for (const auto &type : relationship_types) {
      const auto found = relationship_count_by_type.find(type);
      if (found == relationship_count_by_type.end()) {
        continue;
      }
      best_count = found_type_count ? std::min(best_count, found->second)
                                    : found->second;
      found_type_count = true;
    }
    return found_type_count ? best_count : typed_relationship_count;
  }

  [[nodiscard]] double EstimateRelationshipIndexSeekSelectivity(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key) const override {
    (void)relationship_types;
    const auto found = relationship_index_seek_selectivity_by_key.find(
        std::string(property_key));
    return found == relationship_index_seek_selectivity_by_key.end()
               ? relationship_index_seek_selectivity
               : found->second;
  }

  [[nodiscard]] double EstimateRelationshipIndexRangeSeekSelectivity(
      const std::vector<std::string> &relationship_types,
      std::string_view property_key, std::size_t bound_count) const override {
    (void)relationship_types;
    (void)bound_count;
    const auto found = relationship_index_range_selectivity_by_key.find(
        std::string(property_key));
    return found == relationship_index_range_selectivity_by_key.end()
               ? relationship_index_range_seek_selectivity
               : found->second;
  }

  [[nodiscard]] double EstimateExpandFanout(
      const std::vector<std::string> &relationship_types) const override {
    if (relationship_types.empty()) {
      return untyped_expand_fanout;
    }

    double best_fanout = 0.0;
    bool found_type_fanout = false;
    for (const auto &type : relationship_types) {
      const auto found = expand_fanout_by_type.find(type);
      if (found == expand_fanout_by_type.end()) {
        continue;
      }
      best_fanout = found_type_fanout ? std::min(best_fanout, found->second)
                                      : found->second;
      found_type_fanout = true;
    }
    return found_type_fanout ? best_fanout : typed_expand_fanout;
  }

  [[nodiscard]] double EstimateExpandIntoSelectivity(
      const std::vector<std::string> &relationship_types) const override {
    if (relationship_types.empty()) {
      return untyped_expand_into_selectivity;
    }

    double best_selectivity = 0.0;
    bool found_type_selectivity = false;
    for (const auto &type : relationship_types) {
      const auto found = expand_into_selectivity_by_type.find(type);
      if (found == expand_into_selectivity_by_type.end()) {
        continue;
      }
      best_selectivity = found_type_selectivity
                             ? std::min(best_selectivity, found->second)
                             : found->second;
      found_type_selectivity = true;
    }
    return found_type_selectivity ? best_selectivity
                                  : typed_expand_into_selectivity;
  }

  [[nodiscard]] double EstimateFilterSelectivity() const override {
    return filter_selectivity;
  }

  [[nodiscard]] double EstimateNodeHashJoinSelectivity(
      std::size_t key_count) const override {
    return key_count == 0 ? 1.0 : node_hash_join_selectivity;
  }

  [[nodiscard]] double EstimateValueHashJoinSelectivity(
      std::size_t predicate_count) const override {
    return predicate_count == 0 ? 1.0 : value_hash_join_selectivity;
  }

  [[nodiscard]] double EstimatePredicateJoinSelectivity(
      std::size_t predicate_count) const override {
    return predicate_count == 0 ? 1.0 : predicate_join_selectivity;
  }
};

}  // namespace test_support
