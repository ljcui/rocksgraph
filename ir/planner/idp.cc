#include "ir/planner/idp.h"

#include <algorithm>
#include <utility>

#include "common/exception.h"

namespace ir {

std::vector<std::size_t> NormalizedRelationshipKey(
    std::vector<std::size_t> relationship_indices) {
  std::sort(relationship_indices.begin(), relationship_indices.end());
  relationship_indices.erase(
      std::unique(relationship_indices.begin(), relationship_indices.end()),
      relationship_indices.end());
  return relationship_indices;
}

bool ContainsRelationshipIndex(const std::vector<std::size_t> &indices,
                               std::size_t index) {
  return std::find(indices.begin(), indices.end(), index) != indices.end();
}

std::vector<std::size_t> WithRelationshipIndex(std::vector<std::size_t> indices,
                                               std::size_t index) {
  indices.push_back(index);
  return NormalizedRelationshipKey(std::move(indices));
}

CostEstimate CandidateEstimate(const PlanCandidate &candidate) {
  return {.estimated_rows = candidate.estimated_rows, .cost = candidate.cost};
}

PlanCandidate MakePlanCandidate(
    std::unique_ptr<LogicalPlan> plan,
    std::vector<std::size_t> relationship_indices, CostEstimate estimate,
    std::unordered_set<const Predicate *> planned_predicates) {
  CHECK(plan != nullptr, common::InternalError, "candidate plan is null");

  PlanCandidate candidate;
  candidate.covered_symbols = plan->SolvedSymbols();
  candidate.plan = std::move(plan);
  candidate.relationship_indices =
      NormalizedRelationshipKey(std::move(relationship_indices));
  candidate.planned_predicates = std::move(planned_predicates);
  candidate.estimated_rows = estimate.estimated_rows;
  candidate.cost = estimate.cost;
  return candidate;
}

void PlanTable::PutBest(PlanCandidate candidate) {
  CHECK(candidate.plan != nullptr, common::InternalError,
        "candidate plan is null");
  candidate.relationship_indices =
      NormalizedRelationshipKey(std::move(candidate.relationship_indices));

  auto found = FindEntry(candidate.relationship_indices);
  if (found == entries_.end()) {
    entries_.push_back(std::move(candidate));
    return;
  }
  if (candidate.cost < found->cost) {
    *found = std::move(candidate);
  }
}

PlanCandidate PlanTable::TakeBest(
    std::vector<std::size_t> relationship_indices) {
  relationship_indices =
      NormalizedRelationshipKey(std::move(relationship_indices));
  auto found = FindEntry(relationship_indices);
  CHECK(found != entries_.end(), common::InternalError,
        "missing plan candidate");
  PlanCandidate candidate = std::move(*found);
  entries_.erase(found);
  return candidate;
}

const PlanCandidate *PlanTable::Best(
    std::vector<std::size_t> relationship_indices) const {
  relationship_indices =
      NormalizedRelationshipKey(std::move(relationship_indices));
  auto found = FindEntry(relationship_indices);
  if (found == entries_.end()) {
    return nullptr;
  }
  return &*found;
}

std::vector<std::vector<std::size_t>> PlanTable::KeysWithSize(
    std::size_t relationship_count) const {
  std::vector<std::vector<std::size_t>> keys;
  for (const auto &candidate : entries_) {
    if (candidate.relationship_indices.size() == relationship_count) {
      keys.push_back(candidate.relationship_indices);
    }
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<PlanCandidate>::iterator PlanTable::FindEntry(
    const std::vector<std::size_t> &relationship_indices) {
  return std::find_if(
      entries_.begin(), entries_.end(), [&](const PlanCandidate &candidate) {
        return candidate.relationship_indices == relationship_indices;
      });
}

std::vector<PlanCandidate>::const_iterator PlanTable::FindEntry(
    const std::vector<std::size_t> &relationship_indices) const {
  return std::find_if(
      entries_.begin(), entries_.end(), [&](const PlanCandidate &candidate) {
        return candidate.relationship_indices == relationship_indices;
      });
}

}  // namespace ir
