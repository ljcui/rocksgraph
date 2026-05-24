#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "ir/logical_plan_cost_model.h"
#include "ir/planner_query.h"

namespace ir {

struct PlanCandidate {
  std::unique_ptr<LogicalPlan> plan;
  std::vector<std::size_t> relationship_indices;
  std::unordered_set<std::string> covered_symbols;
  std::unordered_set<const Predicate *> planned_predicates;
  double estimated_rows = 1.0;
  double cost = 1.0;
};

[[nodiscard]] std::vector<std::size_t> NormalizedRelationshipKey(
    std::vector<std::size_t> relationship_indices);
[[nodiscard]] bool ContainsRelationshipIndex(
    const std::vector<std::size_t> &indices, std::size_t index);
[[nodiscard]] std::vector<std::size_t> WithRelationshipIndex(
    std::vector<std::size_t> indices, std::size_t index);

[[nodiscard]] CostEstimate CandidateEstimate(const PlanCandidate &candidate);

[[nodiscard]] PlanCandidate MakePlanCandidate(
    std::unique_ptr<LogicalPlan> plan,
    std::vector<std::size_t> relationship_indices, CostEstimate estimate = {},
    std::unordered_set<const Predicate *> planned_predicates = {});

class PlanTable {
 public:
  void PutBest(PlanCandidate candidate);

  [[nodiscard]] PlanCandidate TakeBest(
      std::vector<std::size_t> relationship_indices);
  [[nodiscard]] const PlanCandidate *Best(
      std::vector<std::size_t> relationship_indices) const;
  [[nodiscard]] std::vector<std::vector<std::size_t>> KeysWithSize(
      std::size_t relationship_count) const;

 private:
  std::vector<PlanCandidate>::iterator FindEntry(
      const std::vector<std::size_t> &relationship_indices);
  std::vector<PlanCandidate>::const_iterator FindEntry(
      const std::vector<std::size_t> &relationship_indices) const;

  std::vector<PlanCandidate> entries_;
};

}  // namespace ir
