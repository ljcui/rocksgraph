#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "ir/logical_plan.h"
#include "ir/query_ir.h"
#include "planner/cost_model.h"

namespace ir {

struct PlanKey {
  std::vector<std::size_t> relationship_indices;
  std::vector<std::string> covered_symbols;
};

[[nodiscard]] bool operator==(const PlanKey &lhs, const PlanKey &rhs);
[[nodiscard]] bool operator<(const PlanKey &lhs, const PlanKey &rhs);

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
[[nodiscard]] PlanKey CandidateKey(const PlanCandidate &candidate);

[[nodiscard]] PlanCandidate MakePlanCandidate(
    std::unique_ptr<LogicalPlan> plan,
    std::vector<std::size_t> relationship_indices, CostEstimate estimate = {},
    std::unordered_set<const Predicate *> planned_predicates = {});

class PlanTable {
 public:
  void PutBest(PlanCandidate candidate);
  void PruneRelationshipCount(std::size_t relationship_count,
                              std::size_t max_candidates);

  [[nodiscard]] PlanCandidate TakeBest(const PlanKey &key);
  [[nodiscard]] const PlanCandidate *Best(const PlanKey &key) const;
  [[nodiscard]] std::vector<PlanKey> KeysWithRelationshipCount(
      std::size_t relationship_count) const;

 private:
  std::vector<PlanCandidate>::iterator FindEntry(const PlanKey &key);
  std::vector<PlanCandidate>::const_iterator FindEntry(
      const PlanKey &key) const;

  std::vector<PlanCandidate> entries_;
};

}  // namespace ir
