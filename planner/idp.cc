#include "planner/idp.h"

#include <algorithm>
#include <utility>

#include "common/exception.h"

namespace ir {
namespace {

std::vector<std::string> NormalizedSymbolKey(
    const std::unordered_set<std::string> &symbols) {
  std::vector<std::string> key(symbols.begin(), symbols.end());
  std::sort(key.begin(), key.end());
  key.erase(std::unique(key.begin(), key.end()), key.end());
  return key;
}

}  // namespace

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

bool operator==(const PlanKey &lhs, const PlanKey &rhs) {
  return lhs.relationship_indices == rhs.relationship_indices &&
         lhs.covered_symbols == rhs.covered_symbols;
}

bool operator<(const PlanKey &lhs, const PlanKey &rhs) {
  if (lhs.relationship_indices != rhs.relationship_indices) {
    return lhs.relationship_indices < rhs.relationship_indices;
  }
  return lhs.covered_symbols < rhs.covered_symbols;
}

PlanKey CandidateKey(const PlanCandidate &candidate) {
  return {.relationship_indices =
              NormalizedRelationshipKey(candidate.relationship_indices),
          .covered_symbols = NormalizedSymbolKey(candidate.covered_symbols)};
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

bool CandidateCostLess(const PlanCandidate &lhs, const PlanCandidate &rhs) {
  if (lhs.cost != rhs.cost) {
    return lhs.cost < rhs.cost;
  }
  return CandidateKey(lhs) < CandidateKey(rhs);
}

void PlanTable::PutBest(PlanCandidate candidate) {
  CHECK(candidate.plan != nullptr, common::InternalError,
        "candidate plan is null");
  candidate.relationship_indices =
      NormalizedRelationshipKey(std::move(candidate.relationship_indices));
  const PlanKey key = CandidateKey(candidate);

  auto found = FindEntry(key);
  if (found == entries_.end()) {
    entries_.push_back(std::move(candidate));
    return;
  }
  if (candidate.cost < found->cost) {
    *found = std::move(candidate);
  }
}

void PlanTable::PruneRelationshipCount(std::size_t relationship_count,
                                       std::size_t max_candidates) {
  CHECK(max_candidates > 0, common::InvalidArgumentError,
        "max candidates must be positive");
  std::vector<PlanKey> keys = KeysWithRelationshipCount(relationship_count);
  if (keys.size() <= max_candidates) {
    return;
  }

  std::sort(
      keys.begin(), keys.end(), [&](const PlanKey &lhs, const PlanKey &rhs) {
        const PlanCandidate *lhs_candidate = Best(lhs);
        const PlanCandidate *rhs_candidate = Best(rhs);
        CHECK(lhs_candidate != nullptr && rhs_candidate != nullptr,
              common::InternalError, "missing plan candidate during pruning");
        return CandidateCostLess(*lhs_candidate, *rhs_candidate);
      });
  keys.resize(max_candidates);
  std::sort(keys.begin(), keys.end());

  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const PlanCandidate &candidate) {
                                  if (candidate.relationship_indices.size() !=
                                      relationship_count) {
                                    return false;
                                  }
                                  const PlanKey key = CandidateKey(candidate);
                                  return !std::binary_search(keys.begin(),
                                                             keys.end(), key);
                                }),
                 entries_.end());
}

PlanCandidate PlanTable::TakeBest(const PlanKey &key) {
  auto found = FindEntry(key);
  CHECK(found != entries_.end(), common::InternalError,
        "missing plan candidate");
  PlanCandidate candidate = std::move(*found);
  entries_.erase(found);
  return candidate;
}

const PlanCandidate *PlanTable::Best(const PlanKey &key) const {
  auto found = FindEntry(key);
  if (found == entries_.end()) {
    return nullptr;
  }
  return &*found;
}

std::vector<PlanKey> PlanTable::KeysWithRelationshipCount(
    std::size_t relationship_count) const {
  std::vector<PlanKey> keys;
  for (const auto &candidate : entries_) {
    if (candidate.relationship_indices.size() == relationship_count) {
      keys.push_back(CandidateKey(candidate));
    }
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<PlanCandidate>::iterator PlanTable::FindEntry(const PlanKey &key) {
  return std::find_if(entries_.begin(), entries_.end(),
                      [&](const PlanCandidate &candidate) {
                        return CandidateKey(candidate) == key;
                      });
}

std::vector<PlanCandidate>::const_iterator PlanTable::FindEntry(
    const PlanKey &key) const {
  return std::find_if(entries_.begin(), entries_.end(),
                      [&](const PlanCandidate &candidate) {
                        return CandidateKey(candidate) == key;
                      });
}

}  // namespace ir
