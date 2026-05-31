#include "ir/planner/plan_clone.h"

#include <memory>
#include <string>

#include "common/exception.h"

namespace ir {
namespace {

std::unique_ptr<LogicalPlan> CloneComponentPlanWithoutMetadata(
    const LogicalPlan &plan) {
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
                                                   scan.Labels());
    }
    case LogicalPlanNodeType::kNodeIndexSeek: {
      const auto &seek = static_cast<const NodeIndexSeekPlan &>(plan);
      return std::make_unique<NodeIndexSeekPlan>(
          seek.Variable(), seek.Labels(), seek.PropertyKey(),
          seek.ValueExpression(), seek.Unique());
    }
    case LogicalPlanNodeType::kNodeIndexRangeSeek: {
      const auto &seek = static_cast<const NodeIndexRangeSeekPlan &>(plan);
      return std::make_unique<NodeIndexRangeSeekPlan>(
          seek.Variable(), seek.Labels(), seek.PropertyKey(),
          seek.Predicates());
    }
    case LogicalPlanNodeType::kRelationshipTypeScan: {
      const auto &scan = static_cast<const RelationshipTypeScanPlan &>(plan);
      return std::make_unique<RelationshipTypeScanPlan>(
          scan.FromNode(), scan.Relationship(), scan.ToNode(), scan.Direction(),
          scan.Types());
    }
    case LogicalPlanNodeType::kRelationshipIndexSeek: {
      const auto &seek = static_cast<const RelationshipIndexSeekPlan &>(plan);
      return std::make_unique<RelationshipIndexSeekPlan>(
          seek.FromNode(), seek.Relationship(), seek.ToNode(), seek.Direction(),
          seek.Types(), seek.PropertyKey(), seek.ValueExpression(),
          seek.Unique());
    }
    case LogicalPlanNodeType::kRelationshipIndexRangeSeek: {
      const auto &seek =
          static_cast<const RelationshipIndexRangeSeekPlan &>(plan);
      return std::make_unique<RelationshipIndexRangeSeekPlan>(
          seek.FromNode(), seek.Relationship(), seek.ToNode(), seek.Direction(),
          seek.Types(), seek.PropertyKey(), seek.Predicates());
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
    case LogicalPlanNodeType::kVarExpand: {
      const auto &expand = static_cast<const VarExpandPlan &>(plan);
      return std::make_unique<VarExpandPlan>(
          CloneComponentPlan(expand.Child(0)), expand.FromNode(),
          expand.Relationship(), expand.ToNode(), expand.Direction(),
          expand.Types(), expand.Length());
    }
    case LogicalPlanNodeType::kPathBuild: {
      const auto &path = static_cast<const PathBuildPlan &>(plan);
      return std::make_unique<PathBuildPlan>(CloneComponentPlan(path.Child(0)),
                                             path.Path());
    }
    case LogicalPlanNodeType::kFilter: {
      const auto &filter = static_cast<const FilterPlan &>(plan);
      return std::make_unique<FilterPlan>(CloneComponentPlan(filter.Child(0)),
                                          filter.Predicate());
    }
    case LogicalPlanNodeType::kProjection: {
      const auto &projection = static_cast<const ProjectionPlan &>(plan);
      return std::make_unique<ProjectionPlan>(
          CloneComponentPlan(projection.Child(0)), projection.Items());
    }
    case LogicalPlanNodeType::kDistinct: {
      const auto &distinct = static_cast<const DistinctPlan &>(plan);
      return std::make_unique<DistinctPlan>(
          CloneComponentPlan(distinct.Child(0)), distinct.GroupingItems());
    }
    case LogicalPlanNodeType::kAggregation: {
      const auto &aggregation = static_cast<const AggregationPlan &>(plan);
      return std::make_unique<AggregationPlan>(
          CloneComponentPlan(aggregation.Child(0)), aggregation.GroupingItems(),
          aggregation.AggregationItems());
    }
    case LogicalPlanNodeType::kSort: {
      const auto &sort = static_cast<const SortPlan &>(plan);
      return std::make_unique<SortPlan>(CloneComponentPlan(sort.Child(0)),
                                        sort.Items());
    }
    case LogicalPlanNodeType::kSkip: {
      const auto &skip = static_cast<const SkipPlan &>(plan);
      return std::make_unique<SkipPlan>(CloneComponentPlan(skip.Child(0)),
                                        skip.Skip());
    }
    case LogicalPlanNodeType::kLimit: {
      const auto &limit = static_cast<const LimitPlan &>(plan);
      return std::make_unique<LimitPlan>(CloneComponentPlan(limit.Child(0)),
                                         limit.Limit());
    }
    case LogicalPlanNodeType::kProduceResults:
      return std::make_unique<ProduceResultsPlan>(
          CloneComponentPlan(plan.Child(0)), plan.OutputColumns());
    case LogicalPlanNodeType::kCartesianProduct:
      return std::make_unique<CartesianProductPlan>(
          CloneComponentPlan(plan.Child(0)), CloneComponentPlan(plan.Child(1)));
    case LogicalPlanNodeType::kNodeHashJoin: {
      const auto &join = static_cast<const NodeHashJoinPlan &>(plan);
      return std::make_unique<NodeHashJoinPlan>(
          CloneComponentPlan(join.Child(0)), CloneComponentPlan(join.Child(1)),
          join.JoinKeys());
    }
    case LogicalPlanNodeType::kValueHashJoin: {
      const auto &join = static_cast<const ValueHashJoinPlan &>(plan);
      return std::make_unique<ValueHashJoinPlan>(
          CloneComponentPlan(join.Child(0)), CloneComponentPlan(join.Child(1)),
          join.Predicates());
    }
    case LogicalPlanNodeType::kPredicateJoin: {
      const auto &join = static_cast<const PredicateJoinPlan &>(plan);
      return std::make_unique<PredicateJoinPlan>(
          CloneComponentPlan(join.Child(0)), CloneComponentPlan(join.Child(1)),
          join.Predicates());
    }
    case LogicalPlanNodeType::kApply:
      return std::make_unique<ApplyPlan>(CloneComponentPlan(plan.Child(0)),
                                         CloneComponentPlan(plan.Child(1)));
    case LogicalPlanNodeType::kSemiApply:
      return std::make_unique<SemiApplyPlan>(CloneComponentPlan(plan.Child(0)),
                                             CloneComponentPlan(plan.Child(1)));
    case LogicalPlanNodeType::kAntiSemiApply:
      return std::make_unique<AntiSemiApplyPlan>(
          CloneComponentPlan(plan.Child(0)), CloneComponentPlan(plan.Child(1)));
    case LogicalPlanNodeType::kLetSemiApply: {
      const auto &apply = static_cast<const LetSemiApplyPlan &>(plan);
      return std::make_unique<LetSemiApplyPlan>(
          CloneComponentPlan(apply.Child(0)),
          CloneComponentPlan(apply.Child(1)), apply.ValueVariable());
    }
    case LogicalPlanNodeType::kRollUpApply: {
      const auto &apply = static_cast<const RollUpApplyPlan &>(plan);
      return std::make_unique<RollUpApplyPlan>(
          CloneComponentPlan(apply.Child(0)),
          CloneComponentPlan(apply.Child(1)), apply.CollectionVariable(),
          apply.ValueVariable());
    }
    case LogicalPlanNodeType::kOptionalApply:
      return std::make_unique<OptionalApplyPlan>(
          CloneComponentPlan(plan.Child(0)), CloneComponentPlan(plan.Child(1)));
    case LogicalPlanNodeType::kAssertIsNode: {
      const auto &assert_is_node = static_cast<const AssertIsNodePlan &>(plan);
      return std::make_unique<AssertIsNodePlan>(
          CloneComponentPlan(assert_is_node.Child(0)),
          assert_is_node.Variables());
    }
    case LogicalPlanNodeType::kWriteBarrier:
      return std::make_unique<WriteBarrierPlan>(
          CloneComponentPlan(plan.Child(0)));
    case LogicalPlanNodeType::kCreateNode: {
      const auto &create = static_cast<const CreateNodePlan &>(plan);
      return std::make_unique<CreateNodePlan>(
          CloneComponentPlan(create.Child(0)), create.Node());
    }
    case LogicalPlanNodeType::kCreateRelationship: {
      const auto &create = static_cast<const CreateRelationshipPlan &>(plan);
      return std::make_unique<CreateRelationshipPlan>(
          CloneComponentPlan(create.Child(0)), create.Relationship());
    }
    case LogicalPlanNodeType::kMerge: {
      const auto &merge = static_cast<const MergePlan &>(plan);
      return std::make_unique<MergePlan>(CloneComponentPlan(merge.Child(0)),
                                         CloneComponentPlan(merge.Child(1)),
                                         merge.Merge());
    }
    case LogicalPlanNodeType::kSetProperty: {
      const auto &set = static_cast<const SetPropertyPlan &>(plan);
      return std::make_unique<SetPropertyPlan>(CloneComponentPlan(set.Child(0)),
                                               set.Entity(), set.PropertyKey(),
                                               set.Value());
    }
    case LogicalPlanNodeType::kSetProperties: {
      const auto &set = static_cast<const SetPropertiesPlan &>(plan);
      return std::make_unique<SetPropertiesPlan>(
          CloneComponentPlan(set.Child(0)), set.Entity(), set.Value(),
          set.IncludeExisting());
    }
    case LogicalPlanNodeType::kSetLabels: {
      const auto &set = static_cast<const SetLabelsPlan &>(plan);
      return std::make_unique<SetLabelsPlan>(CloneComponentPlan(set.Child(0)),
                                             set.Entity(), set.Labels());
    }
    case LogicalPlanNodeType::kRemoveProperty: {
      const auto &remove = static_cast<const RemovePropertyPlan &>(plan);
      return std::make_unique<RemovePropertyPlan>(
          CloneComponentPlan(remove.Child(0)), remove.Entity(),
          remove.PropertyKey());
    }
    case LogicalPlanNodeType::kRemoveLabels: {
      const auto &remove = static_cast<const RemoveLabelsPlan &>(plan);
      return std::make_unique<RemoveLabelsPlan>(
          CloneComponentPlan(remove.Child(0)), remove.Entity(),
          remove.Labels());
    }
    case LogicalPlanNodeType::kDelete: {
      const auto &del = static_cast<const DeletePlan &>(plan);
      return std::make_unique<DeletePlan>(CloneComponentPlan(del.Child(0)),
                                          del.Expressions());
    }
    case LogicalPlanNodeType::kDetachDelete: {
      const auto &del = static_cast<const DetachDeletePlan &>(plan);
      return std::make_unique<DetachDeletePlan>(
          CloneComponentPlan(del.Child(0)), del.Expressions());
    }
    case LogicalPlanNodeType::kUnwind: {
      const auto &unwind = static_cast<const UnwindPlan &>(plan);
      return std::make_unique<UnwindPlan>(CloneComponentPlan(unwind.Child(0)),
                                          unwind.Expression(), unwind.Alias());
    }
    case LogicalPlanNodeType::kProcedureCall: {
      const auto &procedure_call = static_cast<const ProcedureCallPlan &>(plan);
      return std::make_unique<ProcedureCallPlan>(
          CloneComponentPlan(procedure_call.Child(0)),
          procedure_call.ProcedureName(), procedure_call.Arguments(),
          procedure_call.YieldItems(), procedure_call.YieldStar(),
          procedure_call.ReadOnly());
    }
    case LogicalPlanNodeType::kUnion: {
      const auto &union_plan = static_cast<const UnionPlan &>(plan);
      return std::make_unique<UnionPlan>(
          CloneComponentPlan(union_plan.Child(0)),
          CloneComponentPlan(union_plan.Child(1)), union_plan.Mappings(),
          union_plan.All());
    }
    default:
      THROW(common::InternalError,
            "unsupported component plan clone: " + std::string(plan.Name()));
  }
}

}  // namespace

std::unique_ptr<LogicalPlan> CloneComponentPlan(const LogicalPlan &plan) {
  std::unique_ptr<LogicalPlan> clone = CloneComponentPlanWithoutMetadata(plan);
  clone->CopyMetadataFrom(plan);
  return clone;
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
