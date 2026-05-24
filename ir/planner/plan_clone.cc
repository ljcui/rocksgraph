#include "ir/planner/plan_clone.h"

#include <memory>
#include <string>

#include "common/exception.h"

namespace ir {

std::unique_ptr<LogicalPlan> CloneComponentPlan(const LogicalPlan &plan) {
  switch (plan.Type()) {
    case LogicalPlanNodeType::kArgument:
      return std::make_unique<ArgumentPlan>(plan.OutputColumns());
    case LogicalPlanNodeType::kAllNodeScan: {
      const auto &scan = static_cast<const AllNodeScanPlan &>(plan);
      return std::make_unique<AllNodeScanPlan>(scan.Variable());
    }
    case LogicalPlanNodeType::kNodeByLabelScan: {
      const auto &scan = static_cast<const NodeByLabelScanPlan &>(plan);
      return std::make_unique<NodeByLabelScanPlan>(scan.Variable(),
                                                   scan.Label());
    }
    case LogicalPlanNodeType::kExpand: {
      const auto &expand = static_cast<const ExpandPlan &>(plan);
      return std::make_unique<ExpandPlan>(
          CloneComponentPlan(expand.Child(0)), expand.FromNode(),
          expand.Relationship(), expand.ToNode(), expand.Direction(),
          expand.Types());
    }
    case LogicalPlanNodeType::kExpandInto: {
      const auto &expand = static_cast<const ExpandIntoPlan &>(plan);
      return std::make_unique<ExpandIntoPlan>(
          CloneComponentPlan(expand.Child(0)), expand.FromNode(),
          expand.Relationship(), expand.ToNode(), expand.Direction(),
          expand.Types());
    }
    case LogicalPlanNodeType::kFilter: {
      const auto &filter = static_cast<const FilterPlan &>(plan);
      return std::make_unique<FilterPlan>(CloneComponentPlan(filter.Child(0)),
                                          filter.Predicate());
    }
    default:
      THROW(common::InternalError,
            "unsupported component plan clone: " + std::string(plan.Name()));
  }
}

PlanCandidate CloneCandidate(const PlanCandidate &candidate) {
  CHECK(candidate.plan != nullptr, common::InternalError,
        "candidate plan is null");
  PlanCandidate clone;
  clone.plan = CloneComponentPlan(*candidate.plan);
  clone.relationship_indices = candidate.relationship_indices;
  clone.covered_symbols = candidate.covered_symbols;
  clone.planned_predicates = candidate.planned_predicates;
  clone.estimated_rows = candidate.estimated_rows;
  clone.cost = candidate.cost;
  return clone;
}

}  // namespace ir
